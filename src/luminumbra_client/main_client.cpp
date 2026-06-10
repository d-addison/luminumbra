#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "core/Log.h"
#include "core/Debug.h"
#include "core/GameState.h"
#include "core/RuntimeScenarioHarness.h"
#include "player/PlayerController.h"
#include "rendering/Camera.h"
#include "rendering/RenderPipeline.h"
#include "rendering/WorldLoadingVisualizer.h"
#include "ui/Rml_UIManager.h"
#include "audio/AudioManagerFactory.h"
#include "audio/IAudioManager.h"
#include "audio/NullAudioManager.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/core/JobSystem.h"
#include "debug/WorldGenViewer.h"
#include "nlohmann/json.hpp"
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <memory>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include "luminumbra_common/components/CoreComponents.h"

#if defined(_WIN32)
#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>
#endif

using namespace Luminumbra::Client::ScenarioHarness;

// --- Global Pointers ---
std::unique_ptr<Luminumbra::Rendering::Camera> g_camera;
std::unique_ptr<Luminumbra::Client::PlayerController> g_playerController;
std::unique_ptr<Luminumbra::Client::Rml_UIManager> g_uiManager;
std::unique_ptr<Luminumbra::Client::WorldLoadingVisualizer> g_loading_visualizer;

// --- Forward Declarations ---
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void GLAPIENTRY GLDebugMessageCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam);
void GLFWErrorCallback(int error, const char* description);
void SetGameState(GLFWwindow* window, GameStateManager& gameStateManager, GameState newState);

// --- Global State ---
float lastX = 1280 / 2.0f;
float lastY = 720 / 2.0f;
bool firstMouse = true;
bool show_worldgen_viewer = false;
bool wireframe_mode = false;
bool g_world_render_data_initialized = false;
bool g_imgui_enabled = true;

std::vector<Luminumbra::IVec3> g_initial_chunks_to_load;
int g_generation_dispatch_index = 0;
Luminumbra::JobHandle g_world_gen_handle;

struct WindowState {
    bool isFullscreen = false;
    int windowedX = 100, windowedY = 100;
    int windowedWidth = 1280, windowedHeight = 720;
};
WindowState g_windowState;

namespace {

bool HasRuntimeAssets(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path / "res/shaders/g_buffer.vert", ec) &&
           std::filesystem::exists(path / "data/audio/music.bank.json", ec) &&
           std::filesystem::exists(path / "worlds/atlas/presets/default.json", ec);
}

void AddAncestorCandidates(std::vector<std::filesystem::path>& candidates, std::filesystem::path path) {
    std::error_code ec;
    path = std::filesystem::absolute(path, ec);
    if (path.empty()) {
        return;
    }

    while (!path.empty()) {
        candidates.push_back(path);
        const std::filesystem::path parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
}

std::filesystem::path ResolveRuntimeRoot(const char* argv0) {
    std::vector<std::filesystem::path> candidates;

    std::error_code ec;
    AddAncestorCandidates(candidates, std::filesystem::current_path(ec));
    if (argv0 && argv0[0] != '\0') {
        AddAncestorCandidates(candidates, std::filesystem::path(argv0).parent_path());
    }

    for (const std::filesystem::path& candidate : candidates) {
        if (HasRuntimeAssets(candidate)) {
            std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
            return ec ? candidate : canonical;
        }
    }

    return std::filesystem::current_path(ec);
}

std::string RuntimeRootString(const std::filesystem::path& root_dir) {
    std::string root_path = root_dir.generic_string();
    if (!root_path.empty() && root_path.back() != '/') {
        root_path.push_back('/');
    }
    return root_path;
}

struct ProcessMemoryStats {
    uint64_t working_set_bytes = 0;
    uint64_t peak_working_set_bytes = 0;
    uint64_t private_bytes = 0;
    uint64_t pagefile_bytes = 0;
    uint64_t total_physical_bytes = 0;
    uint64_t available_physical_bytes = 0;
};

ProcessMemoryStats QueryProcessMemoryStats() {
    ProcessMemoryStats stats;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        stats.working_set_bytes = static_cast<uint64_t>(counters.WorkingSetSize);
        stats.peak_working_set_bytes = static_cast<uint64_t>(counters.PeakWorkingSetSize);
        stats.private_bytes = static_cast<uint64_t>(counters.PrivateUsage);
        stats.pagefile_bytes = static_cast<uint64_t>(counters.PagefileUsage);
    }

    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = sizeof(memory_status);
    if (GlobalMemoryStatusEx(&memory_status)) {
        stats.total_physical_bytes = static_cast<uint64_t>(memory_status.ullTotalPhys);
        stats.available_physical_bytes = static_cast<uint64_t>(memory_status.ullAvailPhys);
    }
#endif
    return stats;
}

struct RuntimeReadinessReport {
    bool ready = false;
    std::vector<std::string> reasons;
};

RuntimeReadinessReport EvaluateReadiness(
    const RuntimeScenarioConfig& config,
    Luminumbra::world::GameSession* game_session)
{
    RuntimeReadinessReport report;

    if (!game_session) {
        report.reasons.push_back("game_session_missing");
        return report;
    }

    auto* world_system = game_session->GetWorldSystem();
    if (!world_system) {
        report.reasons.push_back("world_system_missing");
        return report;
    }

    (void)world_system->get_renderable_chunks();
    const auto chunk_stats = world_system->get_runtime_chunk_stats();
    if (chunk_stats.renderable_chunks < config.min_renderable_chunks) {
        report.reasons.push_back("renderable_chunks_below_minimum");
    }
    if (chunk_stats.collision_chunks < config.min_collision_chunks) {
        report.reasons.push_back("collision_chunks_below_minimum");
    }
    if (chunk_stats.loading_chunks > 0) {
        report.reasons.push_back("chunks_still_loading");
    }
    if (chunk_stats.meshing_chunks > 0) {
        report.reasons.push_back("chunks_still_meshing");
    }
    if (chunk_stats.generation_job_active) {
        report.reasons.push_back("generation_job_active");
    }
    if (chunk_stats.meshing_job_active) {
        report.reasons.push_back("meshing_job_active");
    }

    report.ready = report.reasons.empty();
    return report;
}

class RuntimeStateRecorder {
public:
    explicit RuntimeStateRecorder(RuntimeScenarioConfig config)
        : m_config(std::move(config)),
          m_started_at(std::chrono::steady_clock::now()) {
        std::error_code ec;
        std::filesystem::create_directories(m_config.artifact_dir, ec);
    }

    const std::filesystem::path& artifact_dir() const { return m_config.artifact_dir; }
    const std::filesystem::path& crash_dir() const { return m_config.crash_dir; }

    void capture(
        const std::string& phase,
        const Luminumbra::JobSystem* job_system,
        Luminumbra::world::GameSession* game_session,
        const Luminumbra::Rendering::RenderPipeline* render_pipeline,
        uint64_t frame_count,
        const RuntimeReadinessReport& readiness)
    {
        m_last_known = build_state_json(phase, job_system, game_session, render_pipeline, frame_count, readiness);
        write_last_known();
    }

    void write_memory_watermark(
        const std::string& phase,
        const ProcessMemoryStats& memory,
        uint64_t measured_bytes)
    {
        std::error_code ec;
        std::filesystem::create_directories(m_config.artifact_dir, ec);
        nlohmann::json artifact = {
            {"schema", "luminumbra.memory_watermark.v1"},
            {"timestamp_utc", TimestampUtc()},
            {"phase", phase},
            {"watermark_mb", m_config.memory_watermark_mb},
            {"measured_bytes", measured_bytes},
            {"memory", MemoryToJson(memory)}
        };
        std::ofstream output(m_config.artifact_dir / "memory-watermark.json");
        output << std::setw(2) << artifact << '\n';
    }

    void write_shutdown(const std::vector<std::string>& milestones, const Luminumbra::JobSystem::RuntimeStats& job_stats) {
        std::error_code ec;
        std::filesystem::create_directories(m_config.artifact_dir, ec);
        nlohmann::json artifact = {
            {"schema", "luminumbra.shutdown.v1"},
            {"timestamp_utc", TimestampUtc()},
            {"scenario", m_config.scenario},
            {"milestones", milestones},
            {"job_queue", JobStatsToJson(job_stats)},
            {"jobs_drained", job_stats.queue_depth == 0 && job_stats.worker_count == 0 && !job_stats.accepting_jobs}
        };
        std::ofstream output(m_config.artifact_dir / "shutdown.json");
        output << std::setw(2) << artifact << '\n';
    }

    void mark_unhandled_exception(uint32_t exception_code) {
        m_last_known["phase"] = "unhandled_exception";
        m_last_known["exception_code"] = exception_code;
        m_last_known["timestamp_utc"] = TimestampUtc();
        write_last_known();
    }

private:
    static nlohmann::json MemoryToJson(const ProcessMemoryStats& memory) {
        return {
            {"working_set_bytes", memory.working_set_bytes},
            {"peak_working_set_bytes", memory.peak_working_set_bytes},
            {"private_bytes", memory.private_bytes},
            {"pagefile_bytes", memory.pagefile_bytes},
            {"total_physical_bytes", memory.total_physical_bytes},
            {"available_physical_bytes", memory.available_physical_bytes}
        };
    }

    static nlohmann::json JobStatsToJson(const Luminumbra::JobSystem::RuntimeStats& stats) {
        return {
            {"worker_count", stats.worker_count},
            {"queue_depth", stats.queue_depth},
            {"accepting_jobs", stats.accepting_jobs},
            {"stop_requested", stats.stop_requested}
        };
    }

    static nlohmann::json ChunkStatsToJson(const Luminumbra::Systems::SHIELD_WorldSystem::RuntimeChunkStats& stats) {
        return {
            {"total_chunks", stats.total_chunks},
            {"unloaded", stats.unloaded_chunks},
            {"loading", stats.loading_chunks},
            {"idle", stats.idle_chunks},
            {"meshing", stats.meshing_chunks},
            {"ready", stats.ready_chunks},
            {"unloading", stats.unloading_chunks},
            {"renderable", stats.renderable_chunks},
            {"collision", stats.collision_chunks},
            {"terrain_vertex_count", stats.terrain_vertex_count},
            {"terrain_index_count", stats.terrain_index_count},
            {"water_vertex_count", stats.water_vertex_count},
            {"water_index_count", stats.water_index_count},
            {"terrain_payload_bytes", stats.terrain_payload_bytes},
            {"generation_job_active", stats.generation_job_active},
            {"meshing_job_active", stats.meshing_job_active}
        };
    }

    static nlohmann::json UploadStatsToJson(const Luminumbra::Rendering::RenderPipeline::MeshUploadFrameStats& stats) {
        return {
            {"snapshot_count", stats.snapshot_count},
            {"terrain_upload_candidates", stats.terrain_upload_candidates},
            {"terrain_uploads", stats.terrain_uploads},
            {"terrain_uploads_deferred", stats.terrain_uploads_deferred},
            {"terrain_payload_bytes", stats.terrain_payload_bytes},
            {"terrain_upload_failures", stats.terrain_upload_failures},
            {"terrain_new_upload_candidates", stats.terrain_new_upload_candidates},
            {"terrain_stale_upload_candidates", stats.terrain_stale_upload_candidates},
            {"terrain_new_uploads_selected", stats.terrain_new_uploads_selected},
            {"terrain_stale_uploads_selected", stats.terrain_stale_uploads_selected},
            {"terrain_new_uploads_deferred", stats.terrain_new_uploads_deferred},
            {"terrain_stale_uploads_deferred", stats.terrain_stale_uploads_deferred},
            {"terrain_deferred_nearer_than_selected", stats.terrain_deferred_nearer_than_selected},
            {"terrain_nearest_candidate_distance_sq", stats.terrain_nearest_candidate_distance_sq},
            {"terrain_farthest_selected_distance_sq", stats.terrain_farthest_selected_distance_sq},
            {"terrain_nearest_deferred_distance_sq", stats.terrain_nearest_deferred_distance_sq},
            {"water_upload_candidates", stats.water_upload_candidates},
            {"water_uploads", stats.water_uploads},
            {"water_uploads_deferred", stats.water_uploads_deferred},
            {"water_payload_bytes", stats.water_payload_bytes},
            {"water_upload_failures", stats.water_upload_failures},
            {"water_new_upload_candidates", stats.water_new_upload_candidates},
            {"water_stale_upload_candidates", stats.water_stale_upload_candidates},
            {"water_new_uploads_selected", stats.water_new_uploads_selected},
            {"water_stale_uploads_selected", stats.water_stale_uploads_selected},
            {"water_new_uploads_deferred", stats.water_new_uploads_deferred},
            {"water_stale_uploads_deferred", stats.water_stale_uploads_deferred},
            {"water_deferred_nearer_than_selected", stats.water_deferred_nearer_than_selected},
            {"water_nearest_candidate_distance_sq", stats.water_nearest_candidate_distance_sq},
            {"water_farthest_selected_distance_sq", stats.water_farthest_selected_distance_sq},
            {"water_nearest_deferred_distance_sq", stats.water_nearest_deferred_distance_sq}
        };
    }

    static nlohmann::json RenderPassStatsToJson(const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& stats) {
        return {
            {"snapshot_count", stats.snapshot_count},
            {"culling_hierarchy_rebuilds", stats.culling_hierarchy_rebuilds},
            {"culling_hierarchy_chunks", stats.culling_hierarchy_chunks},
            {"terrain_visible_chunks", stats.terrain_visible_chunks},
            {"terrain_draws", stats.terrain_draws},
            {"terrain_indices_drawn", stats.terrain_indices_drawn},
            {"water_draws", stats.water_draws},
            {"water_indices_drawn", stats.water_indices_drawn},
            {"shadow_draws", stats.shadow_draws},
            {"shadow_indices_drawn", stats.shadow_indices_drawn}
        };
    }

    static nlohmann::json RenderRuntimeStatsToJson(const Luminumbra::Rendering::RenderPipeline::RuntimeRenderStats& stats) {
        return {
            {"started", stats.started},
            {"terrain_gpu_chunks", stats.terrain_gpu_chunks},
            {"water_gpu_chunks", stats.water_gpu_chunks},
            {"free_terrain_slots", stats.free_terrain_slots},
            {"free_water_slots", stats.free_water_slots},
            {"terrain_vertex_capacity", stats.terrain_vertex_capacity},
            {"terrain_index_capacity", stats.terrain_index_capacity},
            {"water_vertex_capacity", stats.water_vertex_capacity},
            {"water_index_capacity", stats.water_index_capacity},
            {"estimated_vram_bytes", stats.estimated_vram_bytes},
            {"shader_health", {
                {"geometry", stats.geometry_shader_ok},
                {"lighting", stats.lighting_shader_ok},
                {"skybox", stats.skybox_shader_ok},
                {"shadow", stats.shadow_shader_ok},
                {"ssao", stats.ssao_shader_ok},
                {"ssao_blur", stats.ssao_blur_shader_ok},
                {"water", stats.water_shader_ok},
                {"instanced_static_mesh", stats.instanced_static_mesh_shader_ok},
                {"gpu_sdf_initialized", stats.gpu_sdf_initialized}
            }},
            {"gpu_sdf_runtime", {
                {"compile_time_enabled", stats.gpu_sdf_compile_time_enabled},
                {"runtime_requested", stats.gpu_sdf_runtime_requested},
                {"runtime_allowed", stats.gpu_sdf_runtime_allowed},
                {"callback_registered", stats.gpu_sdf_callback_registered},
                {"cpu_fallback_active", stats.gpu_sdf_cpu_fallback_active}
            }}
        };
    }

    static nlohmann::json CoverageStatsToJson(const Luminumbra::Systems::SHIELD_WorldSystem::CameraLocalCoverageStats& stats) {
        return {
            {"camera_position", Vec3ToJson(stats.camera_position)},
            {"camera_chunk", IVec3ToJson(stats.camera_chunk)},
            {"surface_chunk_under_camera", IVec3ToJson(stats.surface_chunk_under_camera)},
            {"horizontal_radius", stats.horizontal_radius},
            {"terrain_height_under_camera", stats.terrain_height_under_camera},
            {"camera_height_above_terrain", stats.camera_height_above_terrain},
            {"expected_surface_chunks", stats.expected_surface_chunks},
            {"present_surface_chunks", stats.present_surface_chunks},
            {"missing_surface_chunks", stats.missing_surface_chunks},
            {"unloaded_surface_chunks", stats.unloaded_surface_chunks},
            {"loading_surface_chunks", stats.loading_surface_chunks},
            {"idle_surface_chunks", stats.idle_surface_chunks},
            {"meshing_surface_chunks", stats.meshing_surface_chunks},
            {"ready_surface_chunks", stats.ready_surface_chunks},
            {"renderable_surface_chunks", stats.renderable_surface_chunks},
            {"collision_surface_chunks", stats.collision_surface_chunks},
            {"pending_lod_chunks", stats.pending_lod_chunks},
            {"lod_counts", {
                {"lod0", stats.lod_counts[0]},
                {"lod1", stats.lod_counts[1]},
                {"lod2", stats.lod_counts[2]},
                {"unknown", stats.lod_unknown_chunks}
            }},
            {"center_chunk_present", stats.center_chunk_present},
            {"center_chunk_renderable", stats.center_chunk_renderable},
            {"near_field_renderable", stats.near_field_renderable}
        };
    }

    static nlohmann::json GLDebugStatsToJson(const GLDebugRuntimeStats& stats) {
        return {
            {"messages", stats.messages},
            {"errors", stats.errors},
            {"warnings", stats.warnings},
            {"notifications", stats.notifications}
        };
    }

    nlohmann::json build_state_json(
        const std::string& phase,
        const Luminumbra::JobSystem* job_system,
        Luminumbra::world::GameSession* game_session,
        const Luminumbra::Rendering::RenderPipeline* render_pipeline,
        uint64_t frame_count,
        const RuntimeReadinessReport& readiness) const
    {
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started_at).count();
        const ProcessMemoryStats memory = QueryProcessMemoryStats();

        nlohmann::json state = {
            {"schema", "luminumbra.runtime_state.v1"},
            {"timestamp_utc", TimestampUtc()},
            {"phase", phase},
            {"scenario", m_config.scenario},
            {"elapsed_seconds", elapsed},
            {"frame_count", frame_count},
            {"launch_flags", {
                {"auto_create_world", m_config.auto_create_world},
                {"auto_enter_world", m_config.auto_enter_world},
                {"timed_run_seconds", m_config.timed_run_seconds},
                {"coverage_radius", m_config.coverage_radius},
                {"no_audio", m_config.no_audio},
                {"audio_telemetry_path", m_config.audio_telemetry_path.generic_string()},
                {"no_ui", m_config.no_ui},
                {"hidden_window", m_config.hidden_window},
                {"enable_gpu_sdf_runtime", m_config.enable_gpu_sdf_runtime},
                {"memory_watermark_mb", m_config.memory_watermark_mb}
            }},
            {"memory", MemoryToJson(memory)},
            {"estimated_vram_bytes", 0},
            {"chunk_states", nlohmann::json::object()},
            {"job_queue", nlohmann::json::object()},
            {"upload_queue", nlohmann::json::object()},
            {"render_pass", nlohmann::json::object()},
            {"shader_health", nlohmann::json::object()},
            {"camera", nlohmann::json::object()},
            {"camera_local_coverage", nlohmann::json::object()},
            {"gl_debug", GLDebugStatsToJson(CurrentGLDebugRuntimeStats())},
            {"readiness", {
                {"ready", readiness.ready},
                {"reasons", readiness.reasons},
                {"timeout_seconds", m_config.readiness_timeout_seconds},
                {"min_renderable_chunks", m_config.min_renderable_chunks},
                {"min_collision_chunks", m_config.min_collision_chunks}
            }}
        };

        if (job_system) {
            state["job_queue"] = JobStatsToJson(job_system->get_runtime_stats());
        }

        if (game_session) {
            const auto& metadata = game_session->GetMetadata();
            state["world"] = {
                {"name", metadata.name},
                {"seed", metadata.seed},
                {"world_type", metadata.worldType},
                {"world_id", metadata.worldId},
                {"spawn_point", Vec3ToJson(metadata.spawnPoint)}
            };
            if (auto* world_system = game_session->GetWorldSystem()) {
                (void)world_system->get_renderable_chunks();
                state["chunk_states"] = ChunkStatsToJson(world_system->get_runtime_chunk_stats());
                if (g_camera) {
                    const auto coverage = world_system->get_camera_local_coverage_stats(
                        g_camera->Position,
                        m_config.coverage_radius
                    );
                    state["camera"] = {
                        {"position", Vec3ToJson(g_camera->Position)},
                        {"chunk", IVec3ToJson(coverage.camera_chunk)},
                        {"terrain_height", coverage.terrain_height_under_camera},
                        {"height_above_terrain", coverage.camera_height_above_terrain}
                    };
                    state["camera_local_coverage"] = CoverageStatsToJson(coverage);
                }
                const auto& streaming = world_system->get_last_streaming_budget_stats();
                state["streaming"] = {
                    {"target_render_radius", streaming.target_render_radius},
                    {"generation_budget", streaming.generation_budget},
                    {"meshing_budget", streaming.meshing_budget},
                    {"scheduled_generation", streaming.scheduled_generation},
                    {"deferred_generation", streaming.deferred_generation},
                    {"scheduled_meshing", streaming.scheduled_meshing},
                    {"deferred_meshing", streaming.deferred_meshing},
                    {"unloaded_chunks", streaming.unloaded_chunks},
                    {"generation_job_active", streaming.generation_job_active},
                    {"meshing_job_active", streaming.meshing_job_active}
                };
            }
        }

        if (render_pipeline) {
            const auto runtime_render = render_pipeline->get_runtime_render_stats();
            const auto render_json = RenderRuntimeStatsToJson(runtime_render);
            state["render_runtime"] = render_json;
            state["estimated_vram_bytes"] = runtime_render.estimated_vram_bytes;
            state["shader_health"] = render_json["shader_health"];
            state["upload_queue"] = UploadStatsToJson(render_pipeline->get_last_mesh_upload_stats());
            state["render_pass"] = RenderPassStatsToJson(render_pipeline->get_last_render_pass_stats());
        }

        return state;
    }

    void write_last_known() const {
        std::error_code ec;
        std::filesystem::create_directories(m_config.artifact_dir, ec);
        std::ofstream output(m_config.artifact_dir / "last-known-runtime.json");
        output << std::setw(2) << m_last_known << '\n';
    }

    RuntimeScenarioConfig m_config;
    std::chrono::steady_clock::time_point m_started_at{};
    nlohmann::json m_last_known = {
        {"schema", "luminumbra.runtime_state.v1"},
        {"phase", "not_started"},
        {"timestamp_utc", TimestampUtc()}
    };
};

struct RuntimeScenarioFrameSample {
    uint64_t frame = 0;
    double elapsed_seconds = 0.0;
    double delta_ms = 0.0;
    Luminumbra::Vec3 camera_position{0.0f};
    float terrain_height = 0.0f;
    float height_above_terrain = 0.0f;
    std::size_t expected_surface_chunks = 0;
    std::size_t missing_surface_chunks = 0;
    std::size_t renderable_surface_chunks = 0;
    std::size_t pending_lod_chunks = 0;
    bool near_field_renderable = false;
    std::size_t terrain_visible_chunks = 0;
    std::size_t terrain_upload_candidates = 0;
    std::size_t terrain_uploads = 0;
    std::size_t terrain_uploads_deferred = 0;
    std::size_t terrain_stale_upload_candidates = 0;
    std::size_t terrain_stale_uploads_deferred = 0;
    std::size_t terrain_deferred_nearer_than_selected = 0;
    std::size_t water_upload_candidates = 0;
    std::size_t water_uploads = 0;
    std::size_t water_uploads_deferred = 0;
    std::size_t water_stale_upload_candidates = 0;
    std::size_t water_stale_uploads_deferred = 0;
    std::size_t water_deferred_nearer_than_selected = 0;
    uint64_t gl_debug_errors = 0;
};

class RuntimeScenarioFrameRecorder {
public:
    RuntimeScenarioFrameRecorder(bool enabled, int coverage_radius, std::filesystem::path output_dir)
        : m_enabled(enabled),
          m_coverage_radius(std::max(0, coverage_radius)),
          m_output_dir(std::move(output_dir)),
          m_started_at(std::chrono::steady_clock::now()) {}

    bool enabled() const { return m_enabled; }

    void record_frame(
        float delta_time,
        Luminumbra::world::GameSession* game_session,
        const Luminumbra::Rendering::RenderPipeline& render_pipeline,
        uint64_t frame_count)
    {
        if (!m_enabled || !game_session || !g_camera) {
            return;
        }

        auto* world_system = game_session->GetWorldSystem();
        if (!world_system) {
            return;
        }

        const auto coverage = world_system->get_camera_local_coverage_stats(g_camera->Position, m_coverage_radius);
        const auto& upload = render_pipeline.get_last_mesh_upload_stats();
        const auto& passes = render_pipeline.get_last_render_pass_stats();
        const auto gl_debug = CurrentGLDebugRuntimeStats();

        RuntimeScenarioFrameSample sample;
        sample.frame = frame_count;
        sample.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started_at).count();
        sample.delta_ms = static_cast<double>(delta_time) * 1000.0;
        sample.camera_position = g_camera->Position;
        sample.terrain_height = coverage.terrain_height_under_camera;
        sample.height_above_terrain = coverage.camera_height_above_terrain;
        sample.expected_surface_chunks = coverage.expected_surface_chunks;
        sample.missing_surface_chunks = coverage.missing_surface_chunks;
        sample.renderable_surface_chunks = coverage.renderable_surface_chunks;
        sample.pending_lod_chunks = coverage.pending_lod_chunks;
        sample.near_field_renderable = coverage.near_field_renderable;
        sample.terrain_visible_chunks = passes.terrain_visible_chunks;
        sample.terrain_upload_candidates = upload.terrain_upload_candidates;
        sample.terrain_uploads = upload.terrain_uploads;
        sample.terrain_uploads_deferred = upload.terrain_uploads_deferred;
        sample.terrain_stale_upload_candidates = upload.terrain_stale_upload_candidates;
        sample.terrain_stale_uploads_deferred = upload.terrain_stale_uploads_deferred;
        sample.terrain_deferred_nearer_than_selected = upload.terrain_deferred_nearer_than_selected;
        sample.water_upload_candidates = upload.water_upload_candidates;
        sample.water_uploads = upload.water_uploads;
        sample.water_uploads_deferred = upload.water_uploads_deferred;
        sample.water_stale_upload_candidates = upload.water_stale_upload_candidates;
        sample.water_stale_uploads_deferred = upload.water_stale_uploads_deferred;
        sample.water_deferred_nearer_than_selected = upload.water_deferred_nearer_than_selected;
        sample.gl_debug_errors = gl_debug.errors;
        m_samples.push_back(sample);
    }

    bool write_artifacts() const {
        if (!m_enabled) {
            return true;
        }

        std::error_code ec;
        std::filesystem::create_directories(m_output_dir, ec);
        if (ec) {
            LUMINUMBRA_CORE_ERROR("Failed to create runtime scenario frame directory '{}': {}", m_output_dir.string(), ec.message());
            return false;
        }

        return write_json(m_output_dir / "runtime-frames.json") && write_csv(m_output_dir / "runtime-frames.csv");
    }

private:
    bool write_json(const std::filesystem::path& path) const {
        nlohmann::json frames = nlohmann::json::array();
        for (const RuntimeScenarioFrameSample& sample : m_samples) {
            frames.push_back({
                {"frame", sample.frame},
                {"elapsed_seconds", sample.elapsed_seconds},
                {"delta_ms", sample.delta_ms},
                {"camera_position", Vec3ToJson(sample.camera_position)},
                {"terrain_height", sample.terrain_height},
                {"height_above_terrain", sample.height_above_terrain},
                {"coverage", {
                    {"expected_surface_chunks", sample.expected_surface_chunks},
                    {"missing_surface_chunks", sample.missing_surface_chunks},
                    {"renderable_surface_chunks", sample.renderable_surface_chunks},
                    {"pending_lod_chunks", sample.pending_lod_chunks},
                    {"near_field_renderable", sample.near_field_renderable}
                }},
                {"render_pass", {
                    {"terrain_visible_chunks", sample.terrain_visible_chunks}
                }},
                {"upload_queue", {
                    {"terrain_upload_candidates", sample.terrain_upload_candidates},
                    {"terrain_uploads", sample.terrain_uploads},
                    {"terrain_uploads_deferred", sample.terrain_uploads_deferred},
                    {"terrain_stale_upload_candidates", sample.terrain_stale_upload_candidates},
                    {"terrain_stale_uploads_deferred", sample.terrain_stale_uploads_deferred},
                    {"terrain_deferred_nearer_than_selected", sample.terrain_deferred_nearer_than_selected},
                    {"water_upload_candidates", sample.water_upload_candidates},
                    {"water_uploads", sample.water_uploads},
                    {"water_uploads_deferred", sample.water_uploads_deferred},
                    {"water_stale_upload_candidates", sample.water_stale_upload_candidates},
                    {"water_stale_uploads_deferred", sample.water_stale_uploads_deferred},
                    {"water_deferred_nearer_than_selected", sample.water_deferred_nearer_than_selected}
                }},
                {"gl_debug_errors", sample.gl_debug_errors}
            });
        }

        nlohmann::json artifact = {
            {"schema", "luminumbra.runtime_frames.v1"},
            {"timestamp_utc", TimestampUtc()},
            {"coverage_radius", m_coverage_radius},
            {"frames_recorded", m_samples.size()},
            {"frames", frames}
        };

        std::ofstream output(path);
        if (!output) {
            LUMINUMBRA_CORE_ERROR("Failed to write runtime frame JSON: {}", path.string());
            return false;
        }
        output << std::setw(2) << artifact << '\n';
        return true;
    }

    bool write_csv(const std::filesystem::path& path) const {
        std::ofstream output(path);
        if (!output) {
            LUMINUMBRA_CORE_ERROR("Failed to write runtime frame CSV: {}", path.string());
            return false;
        }

        output << "frame,elapsed_seconds,delta_ms,camera_x,camera_y,camera_z,terrain_height,height_above_terrain,expected_surface_chunks,missing_surface_chunks,renderable_surface_chunks,pending_lod_chunks,near_field_renderable,terrain_visible_chunks,terrain_upload_candidates,terrain_uploads,terrain_uploads_deferred,terrain_stale_upload_candidates,terrain_stale_uploads_deferred,terrain_deferred_nearer_than_selected,water_upload_candidates,water_uploads,water_uploads_deferred,water_stale_upload_candidates,water_stale_uploads_deferred,water_deferred_nearer_than_selected,gl_debug_errors\n";
        for (const RuntimeScenarioFrameSample& sample : m_samples) {
            output << sample.frame << ','
                   << sample.elapsed_seconds << ','
                   << sample.delta_ms << ','
                   << sample.camera_position.x << ','
                   << sample.camera_position.y << ','
                   << sample.camera_position.z << ','
                   << sample.terrain_height << ','
                   << sample.height_above_terrain << ','
                   << sample.expected_surface_chunks << ','
                   << sample.missing_surface_chunks << ','
                   << sample.renderable_surface_chunks << ','
                   << sample.pending_lod_chunks << ','
                   << (sample.near_field_renderable ? 1 : 0) << ','
                   << sample.terrain_visible_chunks << ','
                   << sample.terrain_upload_candidates << ','
                   << sample.terrain_uploads << ','
                   << sample.terrain_uploads_deferred << ','
                   << sample.terrain_stale_upload_candidates << ','
                   << sample.terrain_stale_uploads_deferred << ','
                   << sample.terrain_deferred_nearer_than_selected << ','
                   << sample.water_upload_candidates << ','
                   << sample.water_uploads << ','
                   << sample.water_uploads_deferred << ','
                   << sample.water_stale_upload_candidates << ','
                   << sample.water_stale_uploads_deferred << ','
                   << sample.water_deferred_nearer_than_selected << ','
                   << sample.gl_debug_errors << '\n';
        }
        return true;
    }

    bool m_enabled = false;
    int m_coverage_radius = 0;
    std::filesystem::path m_output_dir;
    std::chrono::steady_clock::time_point m_started_at{};
    std::vector<RuntimeScenarioFrameSample> m_samples;
};

RuntimeStateRecorder* g_runtime_state_recorder = nullptr;

#if defined(_WIN32)
bool WriteMiniDump(EXCEPTION_POINTERS* exception_info, const std::filesystem::path& crash_dir) {
    std::error_code ec;
    std::filesystem::create_directories(crash_dir, ec);
    const std::filesystem::path dump_path = crash_dir / ("luminumbra-" + TimestampForFile() + ".dmp");

    HANDLE file = CreateFileW(
        dump_path.wstring().c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception_information{};
    exception_information.ThreadId = GetCurrentThreadId();
    exception_information.ExceptionPointers = exception_info;
    exception_information.ClientPointers = FALSE;

    const BOOL wrote_dump = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        file,
        MiniDumpNormal,
        exception_info ? &exception_information : nullptr,
        nullptr,
        nullptr);
    CloseHandle(file);
    return wrote_dump == TRUE;
}

LONG WINAPI RuntimeUnhandledExceptionFilter(EXCEPTION_POINTERS* exception_info) {
    const uint32_t exception_code =
        exception_info && exception_info->ExceptionRecord
            ? static_cast<uint32_t>(exception_info->ExceptionRecord->ExceptionCode)
            : 0;
    if (g_runtime_state_recorder) {
        g_runtime_state_recorder->mark_unhandled_exception(exception_code);
        WriteMiniDump(exception_info, g_runtime_state_recorder->crash_dir());
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void InstallRuntimeCrashHandler(RuntimeStateRecorder& recorder) {
    g_runtime_state_recorder = &recorder;
    SetUnhandledExceptionFilter(RuntimeUnhandledExceptionFilter);
}
#else
void InstallRuntimeCrashHandler(RuntimeStateRecorder& recorder) {
    g_runtime_state_recorder = &recorder;
}
#endif

[[noreturn]] void TriggerForcedCrash() {
#if defined(_WIN32)
    RaiseException(0xE0000001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
    std::raise(SIGABRT);
#endif
    std::abort();
}

bool MemoryWatermarkExceeded(const RuntimeScenarioConfig& config, ProcessMemoryStats* out_memory, uint64_t* out_measured_bytes) {
    if (config.memory_watermark_mb == 0) {
        return false;
    }

    ProcessMemoryStats memory = QueryProcessMemoryStats();
    const uint64_t measured_bytes = std::max(memory.working_set_bytes, memory.private_bytes);
    if (out_memory) {
        *out_memory = memory;
    }
    if (out_measured_bytes) {
        *out_measured_bytes = measured_bytes;
    }
    return measured_bytes > config.memory_watermark_mb * 1024ull * 1024ull;
}

struct RuntimeBootFrameMetrics {
    int frame = 0;
    double delta_ms = 0.0;
    size_t snapshots = 0;
    size_t terrain_visible_chunks = 0;
    size_t terrain_draws = 0;
    size_t terrain_indices_drawn = 0;
    size_t shadow_draws = 0;
    size_t shadow_indices_drawn = 0;
    size_t culling_hierarchy_rebuilds = 0;
    size_t terrain_upload_candidates = 0;
    size_t terrain_uploads = 0;
    size_t terrain_uploads_deferred = 0;
    size_t terrain_payload_bytes = 0;
    size_t terrain_slots_created = 0;
    size_t terrain_slots_reused = 0;
    size_t terrain_slots_grown = 0;
    size_t terrain_upload_failures = 0;
    size_t water_upload_candidates = 0;
    size_t water_uploads = 0;
    size_t water_uploads_deferred = 0;
    size_t water_payload_bytes = 0;
    size_t water_slots_created = 0;
    size_t water_slots_reused = 0;
    size_t water_slots_grown = 0;
    size_t water_upload_failures = 0;
};

class RuntimeBootMetricsRecorder {
public:
    RuntimeBootMetricsRecorder(bool enabled, int target_frames, std::filesystem::path output_dir)
        : m_enabled(enabled),
          m_target_frames(std::max(1, target_frames)),
          m_output_dir(std::move(output_dir)) {
        if (m_enabled) {
            m_frames.reserve(static_cast<size_t>(m_target_frames));
            m_started_at = std::chrono::steady_clock::now();
        }
    }

    bool enabled() const { return m_enabled; }
    bool complete() const { return m_enabled && static_cast<int>(m_frames.size()) >= m_target_frames; }

    void record_frame(float delta_time, const Luminumbra::Rendering::RenderPipeline& render_pipeline) {
        if (!m_enabled || complete()) {
            return;
        }

        const auto& upload = render_pipeline.get_last_mesh_upload_stats();
        const auto& passes = render_pipeline.get_last_render_pass_stats();

        RuntimeBootFrameMetrics frame;
        frame.frame = static_cast<int>(m_frames.size()) + 1;
        frame.delta_ms = static_cast<double>(delta_time) * 1000.0;
        frame.snapshots = passes.snapshot_count;
        frame.terrain_visible_chunks = passes.terrain_visible_chunks;
        frame.terrain_draws = passes.terrain_draws;
        frame.terrain_indices_drawn = passes.terrain_indices_drawn;
        frame.shadow_draws = passes.shadow_draws;
        frame.shadow_indices_drawn = passes.shadow_indices_drawn;
        frame.culling_hierarchy_rebuilds = passes.culling_hierarchy_rebuilds;
        frame.terrain_upload_candidates = upload.terrain_upload_candidates;
        frame.terrain_uploads = upload.terrain_uploads;
        frame.terrain_uploads_deferred = upload.terrain_uploads_deferred;
        frame.terrain_payload_bytes = upload.terrain_payload_bytes;
        frame.terrain_slots_created = upload.terrain_slots_created;
        frame.terrain_slots_reused = upload.terrain_slots_reused;
        frame.terrain_slots_grown = upload.terrain_slots_grown;
        frame.terrain_upload_failures = upload.terrain_upload_failures;
        frame.water_upload_candidates = upload.water_upload_candidates;
        frame.water_uploads = upload.water_uploads;
        frame.water_uploads_deferred = upload.water_uploads_deferred;
        frame.water_payload_bytes = upload.water_payload_bytes;
        frame.water_slots_created = upload.water_slots_created;
        frame.water_slots_reused = upload.water_slots_reused;
        frame.water_slots_grown = upload.water_slots_grown;
        frame.water_upload_failures = upload.water_upload_failures;
        m_frames.push_back(frame);
    }

    bool write_artifacts() const {
        if (!m_enabled) {
            return true;
        }

        std::error_code ec;
        std::filesystem::create_directories(m_output_dir, ec);
        if (ec) {
            LUMINUMBRA_CORE_ERROR("Failed to create runtime boot metrics directory '{}': {}", m_output_dir.string(), ec.message());
            return false;
        }

        const std::filesystem::path json_path = m_output_dir / "runtime_boot.json";
        const std::filesystem::path csv_path = m_output_dir / "runtime_boot.csv";
        return write_json(json_path) && write_csv(csv_path);
    }

private:
    static double percentile(std::vector<double> values, double pct) {
        if (values.empty()) {
            return 0.0;
        }
        std::sort(values.begin(), values.end());
        const double position = pct * static_cast<double>(values.size() - 1);
        const auto index = static_cast<size_t>(std::round(position));
        return values[std::min(index, values.size() - 1)];
    }

    std::vector<double> frame_times_ms() const {
        std::vector<double> values;
        values.reserve(m_frames.size());
        for (const auto& frame : m_frames) {
            values.push_back(frame.delta_ms);
        }
        return values;
    }

    bool write_json(const std::filesystem::path& path) const {
        std::ofstream output(path);
        if (!output) {
            LUMINUMBRA_CORE_ERROR("Failed to write runtime boot metrics JSON: {}", path.string());
            return false;
        }

        const std::vector<double> deltas = frame_times_ms();
        const auto elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_started_at).count();
        const double p50_ms = percentile(deltas, 0.50);
        const double p95_ms = percentile(deltas, 0.95);
        const double p99_ms = percentile(deltas, 0.99);
        size_t max_snapshots = 0;
        size_t max_terrain_visible_chunks = 0;
        size_t max_terrain_draws = 0;
        size_t max_shadow_draws = 0;
        size_t total_culling_hierarchy_rebuilds = 0;
        size_t frames_with_culling_hierarchy_rebuilds = 0;
        size_t total_terrain_upload_candidates = 0;
        size_t total_terrain_uploads = 0;
        size_t total_terrain_uploads_deferred = 0;
        size_t total_terrain_payload_bytes = 0;
        size_t total_terrain_slots_created = 0;
        size_t total_terrain_slots_reused = 0;
        size_t total_terrain_slots_grown = 0;
        size_t total_terrain_upload_failures = 0;
        size_t total_water_upload_candidates = 0;
        size_t total_water_uploads = 0;
        size_t total_water_uploads_deferred = 0;
        size_t total_water_payload_bytes = 0;
        size_t total_water_slots_created = 0;
        size_t total_water_slots_reused = 0;
        size_t total_water_slots_grown = 0;
        size_t total_water_upload_failures = 0;
        size_t peak_terrain_uploads_deferred = 0;
        size_t peak_water_uploads_deferred = 0;
        size_t final_window_terrain_uploads_deferred = 0;
        size_t final_window_water_uploads_deferred = 0;
        constexpr size_t kFinalWindowFrames = 10;
        const size_t final_window_start = m_frames.size() > kFinalWindowFrames ? m_frames.size() - kFinalWindowFrames : 0;

        for (size_t i = 0; i < m_frames.size(); ++i) {
            const auto& frame = m_frames[i];
            max_snapshots = std::max(max_snapshots, frame.snapshots);
            max_terrain_visible_chunks = std::max(max_terrain_visible_chunks, frame.terrain_visible_chunks);
            max_terrain_draws = std::max(max_terrain_draws, frame.terrain_draws);
            max_shadow_draws = std::max(max_shadow_draws, frame.shadow_draws);
            total_culling_hierarchy_rebuilds += frame.culling_hierarchy_rebuilds;
            if (frame.culling_hierarchy_rebuilds > 0) {
                frames_with_culling_hierarchy_rebuilds++;
            }
            total_terrain_upload_candidates += frame.terrain_upload_candidates;
            total_terrain_uploads += frame.terrain_uploads;
            total_terrain_uploads_deferred += frame.terrain_uploads_deferred;
            total_terrain_payload_bytes += frame.terrain_payload_bytes;
            total_terrain_slots_created += frame.terrain_slots_created;
            total_terrain_slots_reused += frame.terrain_slots_reused;
            total_terrain_slots_grown += frame.terrain_slots_grown;
            total_terrain_upload_failures += frame.terrain_upload_failures;
            total_water_upload_candidates += frame.water_upload_candidates;
            total_water_uploads += frame.water_uploads;
            total_water_uploads_deferred += frame.water_uploads_deferred;
            total_water_payload_bytes += frame.water_payload_bytes;
            total_water_slots_created += frame.water_slots_created;
            total_water_slots_reused += frame.water_slots_reused;
            total_water_slots_grown += frame.water_slots_grown;
            total_water_upload_failures += frame.water_upload_failures;
            peak_terrain_uploads_deferred = std::max(peak_terrain_uploads_deferred, frame.terrain_uploads_deferred);
            peak_water_uploads_deferred = std::max(peak_water_uploads_deferred, frame.water_uploads_deferred);
            if (i >= final_window_start) {
                final_window_terrain_uploads_deferred += frame.terrain_uploads_deferred;
                final_window_water_uploads_deferred += frame.water_uploads_deferred;
            }
        }

        output << "{\n";
        output << "  \"schema\": \"luminumbra.runtime_boot.v1\",\n";
        output << "  \"frames_requested\": " << m_target_frames << ",\n";
        output << "  \"frames_recorded\": " << m_frames.size() << ",\n";
        output << "  \"elapsed_ms\": " << elapsed_ms << ",\n";
        output << "  \"frame_time_ms\": {\n";
        output << "    \"p50\": " << p50_ms << ",\n";
        output << "    \"p95\": " << p95_ms << ",\n";
        output << "    \"p99\": " << p99_ms << "\n";
        output << "  },\n";
        output << "  \"summary\": {\n";
        output << "    \"max_snapshots\": " << max_snapshots << ",\n";
        output << "    \"max_terrain_visible_chunks\": " << max_terrain_visible_chunks << ",\n";
        output << "    \"max_terrain_draws\": " << max_terrain_draws << ",\n";
        output << "    \"max_shadow_draws\": " << max_shadow_draws << ",\n";
        output << "    \"total_culling_hierarchy_rebuilds\": " << total_culling_hierarchy_rebuilds << ",\n";
        output << "    \"frames_with_culling_hierarchy_rebuilds\": " << frames_with_culling_hierarchy_rebuilds << ",\n";
        output << "    \"total_terrain_upload_candidates\": " << total_terrain_upload_candidates << ",\n";
        output << "    \"total_terrain_uploads\": " << total_terrain_uploads << ",\n";
        output << "    \"total_terrain_uploads_deferred\": " << total_terrain_uploads_deferred << ",\n";
        output << "    \"total_terrain_payload_bytes\": " << total_terrain_payload_bytes << ",\n";
        output << "    \"total_terrain_slots_created\": " << total_terrain_slots_created << ",\n";
        output << "    \"total_terrain_slots_reused\": " << total_terrain_slots_reused << ",\n";
        output << "    \"total_terrain_slots_grown\": " << total_terrain_slots_grown << ",\n";
        output << "    \"total_terrain_upload_failures\": " << total_terrain_upload_failures << ",\n";
        output << "    \"total_water_upload_candidates\": " << total_water_upload_candidates << ",\n";
        output << "    \"total_water_uploads\": " << total_water_uploads << ",\n";
        output << "    \"total_water_uploads_deferred\": " << total_water_uploads_deferred << ",\n";
        output << "    \"total_water_payload_bytes\": " << total_water_payload_bytes << ",\n";
        output << "    \"total_water_slots_created\": " << total_water_slots_created << ",\n";
        output << "    \"total_water_slots_reused\": " << total_water_slots_reused << ",\n";
        output << "    \"total_water_slots_grown\": " << total_water_slots_grown << ",\n";
        output << "    \"total_water_upload_failures\": " << total_water_upload_failures << ",\n";
        output << "    \"peak_terrain_uploads_deferred\": " << peak_terrain_uploads_deferred << ",\n";
        output << "    \"peak_water_uploads_deferred\": " << peak_water_uploads_deferred << ",\n";
        output << "    \"final_window_frames\": " << std::min(kFinalWindowFrames, m_frames.size()) << ",\n";
        output << "    \"final_window_terrain_uploads_deferred\": " << final_window_terrain_uploads_deferred << ",\n";
        output << "    \"final_window_water_uploads_deferred\": " << final_window_water_uploads_deferred << "\n";
        output << "  },\n";
        output << "  \"last_frame\": ";
        if (m_frames.empty()) {
            output << "null\n";
        } else {
            const auto& frame = m_frames.back();
            output << "{";
            output << "\"snapshots\": " << frame.snapshots << ", ";
            output << "\"terrain_visible_chunks\": " << frame.terrain_visible_chunks << ", ";
            output << "\"terrain_draws\": " << frame.terrain_draws << ", ";
            output << "\"shadow_draws\": " << frame.shadow_draws << ", ";
            output << "\"terrain_uploads_deferred\": " << frame.terrain_uploads_deferred << ", ";
            output << "\"water_upload_candidates\": " << frame.water_upload_candidates << ", ";
            output << "\"water_uploads_deferred\": " << frame.water_uploads_deferred;
            output << "}\n";
        }
        output << "}\n";
        return true;
    }

    bool write_csv(const std::filesystem::path& path) const {
        std::ofstream output(path);
        if (!output) {
            LUMINUMBRA_CORE_ERROR("Failed to write runtime boot metrics CSV: {}", path.string());
            return false;
        }

        output << "frame,delta_ms,snapshots,terrain_visible_chunks,terrain_draws,terrain_indices_drawn,shadow_draws,shadow_indices_drawn,culling_hierarchy_rebuilds,terrain_upload_candidates,terrain_uploads,terrain_uploads_deferred,terrain_payload_bytes,terrain_slots_created,terrain_slots_reused,terrain_slots_grown,terrain_upload_failures,water_upload_candidates,water_uploads,water_uploads_deferred,water_payload_bytes,water_slots_created,water_slots_reused,water_slots_grown,water_upload_failures\n";
        for (const auto& frame : m_frames) {
            output << frame.frame << ','
                   << frame.delta_ms << ','
                   << frame.snapshots << ','
                   << frame.terrain_visible_chunks << ','
                   << frame.terrain_draws << ','
                   << frame.terrain_indices_drawn << ','
                   << frame.shadow_draws << ','
                   << frame.shadow_indices_drawn << ','
                   << frame.culling_hierarchy_rebuilds << ','
                   << frame.terrain_upload_candidates << ','
                   << frame.terrain_uploads << ','
                   << frame.terrain_uploads_deferred << ','
                   << frame.terrain_payload_bytes << ','
                   << frame.terrain_slots_created << ','
                   << frame.terrain_slots_reused << ','
                   << frame.terrain_slots_grown << ','
                   << frame.terrain_upload_failures << ','
                   << frame.water_upload_candidates << ','
                   << frame.water_uploads << ','
                   << frame.water_uploads_deferred << ','
                   << frame.water_payload_bytes << ','
                   << frame.water_slots_created << ','
                   << frame.water_slots_reused << ','
                   << frame.water_slots_grown << ','
                   << frame.water_upload_failures << '\n';
        }
        return true;
    }

    bool m_enabled = false;
    int m_target_frames = 300;
    std::filesystem::path m_output_dir;
    std::chrono::steady_clock::time_point m_started_at{};
    std::vector<RuntimeBootFrameMetrics> m_frames;
};

} // namespace

void ToggleFullscreen(GLFWwindow* window, WindowState& state) {
    if (state.isFullscreen) {
        glfwSetWindowMonitor(window, nullptr, state.windowedX, state.windowedY, state.windowedWidth, state.windowedHeight, 0);
    } else {
        glfwGetWindowPos(window, &state.windowedX, &state.windowedY);
        glfwGetWindowSize(window, &state.windowedWidth, &state.windowedHeight);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    }
    state.isFullscreen = !state.isFullscreen;
}

int main(int argc, char* argv[]) {
    Log::Init();
    std::filesystem::path root_dir = ResolveRuntimeRoot(argc > 0 ? argv[0] : nullptr);
    std::string root_path_str = RuntimeRootString(root_dir);
    LUMINUMBRA_CORE_INFO("Runtime root: {}", root_path_str);

    RuntimeScenarioConfig scenario_config = ParseRuntimeScenarioConfig(argc, argv, root_dir);
    RuntimeStateRecorder runtime_state_recorder(scenario_config);
    InstallRuntimeCrashHandler(runtime_state_recorder);
    g_imgui_enabled = !scenario_config.no_ui;
    runtime_state_recorder.capture("startup_requested", nullptr, nullptr, nullptr, 0, {});

    if (scenario_config.forced_crash()) {
        runtime_state_recorder.capture("forced_crash_requested", nullptr, nullptr, nullptr, 0, {});
        TriggerForcedCrash();
    }

    const bool runtime_boot_metrics_enabled = HasCommandLineFlag(argc, argv, "--runtime-boot-metrics");
    const int runtime_boot_frames = GetCommandLineIntOption(argc, argv, "--runtime-boot-frames", 300);
    const std::filesystem::path default_runtime_boot_output = root_dir / "build/debug/test-artifacts/performance";
    const std::filesystem::path runtime_boot_output = GetCommandLineOption(
        argc,
        argv,
        "--runtime-boot-output",
        default_runtime_boot_output.string());
    RuntimeBootMetricsRecorder runtime_boot_recorder(
        runtime_boot_metrics_enabled,
        runtime_boot_frames,
        runtime_boot_output);
    RuntimeScenarioFrameRecorder lod_ground_frame_recorder(
        scenario_config.lod_ground_smoke(),
        scenario_config.coverage_radius,
        scenario_config.artifact_dir);

    glfwSetErrorCallback(GLFWErrorCallback);
    if (!glfwInit()) { LUMINUMBRA_CORE_ERROR("FATAL: Failed to initialize GLFW!"); return -1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    if (runtime_boot_recorder.enabled() || scenario_config.hidden_window) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }
    #ifdef LUMINUMBRA_DEBUG
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
    #endif

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Luminumbra", nullptr, nullptr);
    LUMINUMBRA_ASSERT(window, "Failed to create GLFW window!");
    glfwMakeContextCurrent(window);
    // g_windowState.isFullscreen = false;
    // ToggleFullscreen(window, g_windowState);

    int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    LUMINUMBRA_ASSERT(status, "Failed to initialize GLAD!");
    
    if (g_imgui_enabled) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 450");
    }

    #ifdef LUMINUMBRA_DEBUG
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(GLDebugMessageCallback, nullptr);
    #endif

    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    GameStateManager gameStateManager;
    Luminumbra::JobSystem jobSystem;
    jobSystem.startup();

    auto gameSession = std::make_unique<Luminumbra::world::GameSession>();
    gameSession->SetJobSystem(&jobSystem);
    gameSession->SetRootPath(root_path_str);

    std::unique_ptr<Luminumbra::Client::IAudioManager> audioManager;
    if (scenario_config.no_audio) {
        audioManager = std::make_unique<Luminumbra::Client::NullAudioManager>(scenario_config.audio_telemetry_path);
    } else {
        audioManager = Luminumbra::Client::CreateAudioManager(root_path_str);
    }
    audioManager->Init();
    audioManager->LoadBank("data/audio/sfx_main.bank.json");
    audioManager->LoadBank("data/audio/music.bank.json");

    if (!scenario_config.no_ui) {
        g_uiManager = std::make_unique<Luminumbra::Client::Rml_UIManager>(root_path_str);
        g_uiManager->Init(window, audioManager.get());
    }

    Luminumbra::Rendering::RenderPipeline renderPipeline;
    renderPipeline.set_gpu_sdf_runtime_enabled(scenario_config.enable_gpu_sdf_runtime);
    if (!renderPipeline.startup(framebufferWidth, framebufferHeight, root_dir)) {
        LUMINUMBRA_CORE_ERROR("FATAL: Render pipeline startup failed.");
        runtime_state_recorder.capture("render_pipeline_startup_failed", &jobSystem, gameSession.get(), &renderPipeline, 0, {});
        if (g_uiManager) {
            g_uiManager->Shutdown();
        }
        audioManager->Shutdown();
        jobSystem.shutdown();
        if (g_imgui_enabled) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    glfwSetWindowUserPointer(window, &renderPipeline);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    g_loading_visualizer = std::make_unique<Luminumbra::Client::WorldLoadingVisualizer>();
    g_loading_visualizer->Startup(root_dir, framebufferWidth, framebufferHeight);

    bool scenario_failed = false;
    std::string scenario_failure_reason;
    bool scenario_ready = false;
    bool scenario_timed_run_complete = false;
    uint64_t scenario_frame_count = 0;
    std::chrono::steady_clock::time_point scenario_play_started_at{};
    RuntimeReadinessReport last_readiness_report;

    auto start_world_creation = [&](const std::string& name, const std::string& seed, const std::string& worldType) {
        // 1. Synchronously create the world systems and metadata. This is fast.
        if (gameSession->CreateWorld(name, seed, worldType)) {
            if (auto* world_system = gameSession->GetWorldSystem()) {
                renderPipeline.SetupGPUSDFIntegration(*world_system);
            }
            const bool bypass_loading_ui = runtime_boot_recorder.enabled() || (scenario_config.active() && scenario_config.auto_enter_world);
            if (bypass_loading_ui) {
                LUMINUMBRA_CORE_INFO("Runtime scenario mode: created world without loading UI.");
                bool horizon_ready = true;
                if (gameSession->GetWorldSystem() && gameSession->GetPhysicsSystem()) {
                    horizon_ready = gameSession->GetWorldSystem()->EnsureSurfaceReadyNear(
                        gameSession->GetMetadata().spawnPoint,
                        gameSession->GetPhysicsSystem(),
                        scenario_config.horizon_radius,
                        scenario_config.collision_radius
                    );
                }
                g_camera = std::make_unique<Luminumbra::Rendering::Camera>(gameSession->GetMetadata().spawnPoint);
                g_playerController = std::make_unique<Luminumbra::Client::PlayerController>(window, g_camera.get(), gameSession->GetPhysicsSystem());
                if (g_world_render_data_initialized) {
                    renderPipeline.clear_all_chunk_data();
                }
                g_world_render_data_initialized = true;
                SetGameState(window, gameStateManager, GameState::IN_GAME);
                last_readiness_report = EvaluateReadiness(scenario_config, gameSession.get());
                if (!horizon_ready || !last_readiness_report.ready) {
                    scenario_failed = true;
                    scenario_failure_reason = "world_readiness_failed";
                    runtime_state_recorder.capture("world_readiness_failed", &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, last_readiness_report);
                } else {
                    scenario_ready = true;
                    scenario_play_started_at = std::chrono::steady_clock::now();
                    runtime_state_recorder.capture("world_entered", &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, last_readiness_report);
                }
                return;
            }

            // Hide the main menu UI
            if (g_uiManager && g_uiManager->GetContext()) {
                for (int i = 0; i < g_uiManager->GetContext()->GetNumDocuments(); ++i) {
                    if (auto* doc = g_uiManager->GetContext()->GetDocument(i)) { doc->Hide(); }
                }
                if (Rml::Element* focused_element = g_uiManager->GetContext()->GetFocusElement()) {
                    focused_element->Blur();
                }
            }

            // 2. Switch to the loading state
            SetGameState(window, gameStateManager, GameState::WORLD_LOADING);
            
            // 3. Get the list of chunks to generate and start the visualizer
            auto* world_system = gameSession->GetWorldSystem();
            // Use the actual spawn position for initial chunk loading
            Luminumbra::Vec3 spawn_pos = gameSession->GetMetadata().spawnPoint;
            if (runtime_boot_recorder.enabled()) {
                g_initial_chunks_to_load.clear();
            } else {
                g_initial_chunks_to_load = world_system->GetInitialChunkLoadList(spawn_pos);
            }

            g_generation_dispatch_index = 0;
            if (g_loading_visualizer) {
                g_loading_visualizer->BeginVisualization(g_initial_chunks_to_load);
            }

        } else {
            LUMINUMBRA_CORE_ERROR("Failed to create world!");
            scenario_failed = scenario_config.active();
            scenario_failure_reason = "create_world_failed";
            runtime_state_recorder.capture("create_world_failed", &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, {});
        }
    };

    if (g_uiManager) {
        g_uiManager->SetWorldCreationCallback(start_world_creation);
    }

    glfwSetKeyCallback(window, key_callback);
    SetGameState(window, gameStateManager, GameState::MAIN_MENU);
    audioManager->PlayMusic("music_main_menu");
    if (g_uiManager) {
        g_uiManager->RequestLoadDocument("main_menu.rml");
    }

    if (scenario_config.auto_create_world || HasCommandLineFlag(argc, argv, "--auto-create-world") || runtime_boot_recorder.enabled()) {
        // The endurance and water gates assert visible water; the default
        // preset (height_offset 20, sea level 0) generates none near spawn,
        // so every water-asserting scenario runs in the archipelago world.
        // lod_ground_smoke keeps the default world its thresholds were tuned on.
        const std::string scenario_world_type =
            (scenario_config.water_visual_smoke() || scenario_config.material_visual_smoke() ||
             scenario_config.auto_world_smoke()) ? "archipelago" : "default";
        start_world_creation("Automated Test World", "424242", scenario_world_type);
    }

    std::unique_ptr<Luminumbra::Client::WorldGenViewer> worldGenViewer;
    if (g_imgui_enabled) {
        worldGenViewer = std::make_unique<Luminumbra::Client::WorldGenViewer>();
    }

    if (runtime_boot_recorder.enabled() && gameStateManager.GetCurrentState() == GameState::IN_GAME) {
        LUMINUMBRA_CORE_INFO("Runtime boot metrics mode: capturing fixed frames through streaming scheduler.");
        if (gameSession->GetWorldSystem() && gameSession->GetPhysicsSystem()) {
            gameSession->GetWorldSystem()->EnsureSurfaceReadyNear(
                gameSession->GetMetadata().spawnPoint,
                gameSession->GetPhysicsSystem(),
                4,
                2
            );
        }

        constexpr float capture_delta_time = 1.0f / 60.0f;
        const Luminumbra::Vec3 capture_position = gameSession->GetMetadata().spawnPoint;
        const int max_capture_attempts = runtime_boot_frames * 6;
        for (int attempt = 0; attempt < max_capture_attempts && !runtime_boot_recorder.complete(); ++attempt) {
            if (gameSession->GetWorldSystem()) {
                gameSession->GetWorldSystem()->update(
                    gameSession->GetRegistry(),
                    capture_position,
                    nullptr
                );
            }
            if (gameSession->GetWorldSystem() && g_camera) {
                if (gameSession->GetWorldSystem()->get_renderable_chunks().empty()) {
                    continue;
                }
                if (runtime_boot_recorder.complete()) {
                    break;
                }
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                renderPipeline.render_frame(
                    gameSession->GetRegistry(),
                    *gameSession->GetWorldSystem(),
                    *g_camera,
                    capture_delta_time,
                    wireframe_mode
                );
                runtime_boot_recorder.record_frame(capture_delta_time, renderPipeline);
                glfwSwapBuffers(window);
            }
        }

        const bool wrote_metrics = runtime_boot_recorder.write_artifacts();
        LUMINUMBRA_CORE_INFO(
            "Runtime boot metrics {}: {} frames -> {}",
            wrote_metrics ? "written" : "failed",
            runtime_boot_frames,
            runtime_boot_output.string());
        if (auto* world_system = gameSession->GetWorldSystem()) {
            world_system->clear_world(nullptr);
        }
        glfwSetWindowShouldClose(window, true);
    }

    int exit_code = 0;
    float lastFrame = 0.0f;
    auto scenario_started_at = std::chrono::steady_clock::now();
    auto last_runtime_state_write = std::chrono::steady_clock::now();
    std::array<bool, 3> lod_ground_screenshots_written{false, false, false};
    std::vector<std::string> lod_ground_screenshot_files;
    std::vector<LodGroundVisualCapture> lod_ground_visual_captures;
    WaterVisualCameraTarget water_visual_target;
    bool water_visual_target_initialized = false;
    bool water_visual_capture_written = false;
    WaterVisualCameraTarget material_visual_target;
    bool material_visual_target_initialized = false;
    bool material_visual_capture_written = false;
    while (!glfwWindowShouldClose(window)) {
        float currentFrame = (float)glfwGetTime();
        float deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;
        deltaTime = std::min(deltaTime, 1.0f / 20.0f);

        if (scenario_failed) {
            exit_code = 2;
            glfwSetWindowShouldClose(window, true);
        }

        ProcessMemoryStats watermark_memory;
        uint64_t watermark_measured_bytes = 0;
        if (MemoryWatermarkExceeded(scenario_config, &watermark_memory, &watermark_measured_bytes)) {
            scenario_failed = true;
            scenario_failure_reason = "memory_watermark_exceeded";
            exit_code = 3;
            runtime_state_recorder.write_memory_watermark("main_loop", watermark_memory, watermark_measured_bytes);
            runtime_state_recorder.capture("memory_watermark_exceeded", &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, last_readiness_report);
            glfwSetWindowShouldClose(window, true);
        }

        if (scenario_config.active() && !scenario_ready && !scenario_failed) {
            const auto readiness_wait_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - scenario_started_at).count();
            if (readiness_wait_seconds >= scenario_config.readiness_timeout_seconds) {
                last_readiness_report = EvaluateReadiness(scenario_config, gameSession.get());
                scenario_failed = true;
                scenario_failure_reason = "world_readiness_timeout";
                exit_code = 4;
                runtime_state_recorder.capture("world_readiness_timeout", &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, last_readiness_report);
                glfwSetWindowShouldClose(window, true);
            }
        }

        glfwPollEvents();
        audioManager->Update();

        if (g_uiManager) {
            g_uiManager->Update();
        }
        
        GameState currentState = gameStateManager.GetCurrentState();
        
        if (g_imgui_enabled) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
        }
        
        switch (currentState) {
            case GameState::WORLD_LOADING:
            {
                const int JOBS_PER_FRAME = 512;
                if (runtime_boot_recorder.enabled()) {
                    g_generation_dispatch_index = static_cast<int>(g_initial_chunks_to_load.size());
                }

                if (static_cast<size_t>(g_generation_dispatch_index) < g_initial_chunks_to_load.size()) {
                    std::vector<Luminumbra::IVec3> batch_to_generate;
                    const int batch_start_index = g_generation_dispatch_index;
                    while (static_cast<size_t>(batch_start_index + static_cast<int>(batch_to_generate.size())) < g_initial_chunks_to_load.size() &&
                           static_cast<int>(batch_to_generate.size()) < JOBS_PER_FRAME) {
                        batch_to_generate.push_back(g_initial_chunks_to_load[batch_start_index + static_cast<int>(batch_to_generate.size())]);
                    }
                    if(!batch_to_generate.empty()) {
                        Luminumbra::JobHandle handle = gameSession->GetWorldSystem()->dispatch_generation_jobs(batch_to_generate);
                        if (handle.counter) {
                            for (const auto& coords : batch_to_generate) {
                                g_loading_visualizer->UpdateChunkState(coords, Luminumbra::Client::ChunkLoadVisualState::DISPATCHED);
                            }
                            g_generation_dispatch_index += static_cast<int>(batch_to_generate.size());
                        }
                    }
                }

                float progress = g_initial_chunks_to_load.empty() ? 1.0f : static_cast<float>(g_generation_dispatch_index) / g_initial_chunks_to_load.size();
                
                glClearColor(0.01f, 0.02f, 0.05f, 1.0f); // Dark blue background
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                if (g_loading_visualizer) {
                    g_loading_visualizer->UpdateAndRender(deltaTime, "CONSTRUCTING WORLD GEOMETRY...", progress);
                }

                if (static_cast<size_t>(g_generation_dispatch_index) >= g_initial_chunks_to_load.size()) {
                    if (gameSession->GetWorldSystem() && gameSession->GetPhysicsSystem()) {
                        gameSession->GetWorldSystem()->EnsureSurfaceReadyNear(
                            gameSession->GetMetadata().spawnPoint,
                            gameSession->GetPhysicsSystem(),
                            12,
                            4
                        );
                    }
                    LUMINUMBRA_CORE_INFO("World generation phase complete. Entering world.");
                    audioManager->StopMusic();
                    audioManager->PlayOneShot2D("ui_world_loaded"); // Play a sound on completion
                    if (g_loading_visualizer) {
                        g_loading_visualizer->EndVisualization();
                    }
                    g_camera = std::make_unique<Luminumbra::Rendering::Camera>(gameSession->GetMetadata().spawnPoint);
                    g_playerController = std::make_unique<Luminumbra::Client::PlayerController>(window, g_camera.get(), gameSession->GetPhysicsSystem());
                    if (g_world_render_data_initialized) {
                        renderPipeline.clear_all_chunk_data();
                    }
                    g_world_render_data_initialized = true;
                    SetGameState(window, gameStateManager, GameState::IN_GAME);
                    if (scenario_config.active()) {
                        last_readiness_report = EvaluateReadiness(scenario_config, gameSession.get());
                        if (!last_readiness_report.ready) {
                            scenario_failed = true;
                            scenario_failure_reason = "world_readiness_failed";
                            runtime_state_recorder.capture("world_readiness_failed", &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, last_readiness_report);
                        } else {
                            scenario_ready = true;
                            scenario_play_started_at = std::chrono::steady_clock::now();
                            runtime_state_recorder.capture("world_entered", &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, last_readiness_report);
                        }
                    }
                    lastFrame = static_cast<float>(glfwGetTime());
                }
                break;
            }

            case GameState::IN_GAME:
                if (scenario_config.lod_ground_smoke() && scenario_ready && g_camera) {
                    const double elapsed_play_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - scenario_play_started_at).count();
                    ApplyLodGroundCameraPath(scenario_config, gameSession.get(), g_camera.get(), elapsed_play_seconds);
                } else if (scenario_config.water_visual_smoke() && scenario_ready && g_camera) {
                    if (!water_visual_target_initialized || !water_visual_target.found) {
                        water_visual_target = FindWaterVisualCameraTarget(gameSession.get());
                        water_visual_target_initialized = water_visual_target.found;
                    }
                    ApplyWaterVisualCamera(g_camera.get(), water_visual_target);
                } else if (scenario_config.material_visual_smoke() && scenario_ready && g_camera) {
                    if (!material_visual_target_initialized || !material_visual_target.found) {
                        material_visual_target = FindMaterialVisualCameraTarget(gameSession.get());
                        material_visual_target_initialized = material_visual_target.found;
                    }
                    ApplyWaterVisualCamera(g_camera.get(), material_visual_target);
                } else if (g_playerController) {
                    g_playerController->Update(deltaTime);
                }
                if (auto* physics = gameSession->GetPhysicsSystem()) physics->update(deltaTime);
                if (gameSession->GetWorldSystem() && (g_playerController || g_camera)) {
                    const Luminumbra::Vec3 streaming_position =
                        ((scenario_config.lod_ground_smoke() || scenario_config.water_visual_smoke() || scenario_config.material_visual_smoke()) && scenario_ready && g_camera)
                            ? Luminumbra::Vec3(g_camera->Position)
                            : (g_playerController ? Luminumbra::Vec3(g_playerController->GetPosition()) : Luminumbra::Vec3(g_camera->Position));
                    gameSession->GetWorldSystem()->update(
                        gameSession->GetRegistry(),
                        streaming_position,
                        gameSession->GetPhysicsSystem()
                    );
                }
                break;
            case GameState::MAIN_MENU:
                break;
            case GameState::EXITING:
                glfwSetWindowShouldClose(window, true);
                break;
            default:
                break;
        }

        if (currentState != GameState::WORLD_LOADING) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (currentState == GameState::IN_GAME) {
                if (gameSession->GetWorldSystem() && g_camera) {
                    if (scenario_config.lod_ground_smoke() || scenario_config.water_visual_smoke() || scenario_config.material_visual_smoke()) {
                        // time_of_day 0 is noon (sun elevation = cos(2*pi*t));
                        // 0.04 keeps the sun near its zenith for stable captures.
                        renderPipeline.set_time_of_day(0.04f);
                    }
                    renderPipeline.render_frame(gameSession->GetRegistry(), *gameSession->GetWorldSystem(), *g_camera, deltaTime, wireframe_mode);
                    if (scenario_config.active() && currentState == GameState::IN_GAME) {
                        ++scenario_frame_count;
                        const auto now = std::chrono::steady_clock::now();
                        if (lod_ground_frame_recorder.enabled()) {
                            lod_ground_frame_recorder.record_frame(deltaTime, gameSession.get(), renderPipeline, scenario_frame_count);
                        }
                        if (scenario_config.lod_ground_smoke() && scenario_ready) {
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const std::array<double, 3> thresholds{0.15, 0.55, 0.95};
                            const std::array<const char*, 3> names{"start", "mid", "end"};
                            for (std::size_t i = 0; i < thresholds.size(); ++i) {
                                if (lod_ground_screenshots_written[i] || progress < thresholds[i]) {
                                    continue;
                                }
                                lod_ground_screenshots_written[i] = true;
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                const std::string relative_path = std::string("screenshots/lod-ground-") + names[i] + ".ppm";
                                LodHolePixelStats hole_stats;
                                if (WriteBackbufferPpm(scenario_config.artifact_dir / relative_path, screenshot_width, screenshot_height, nullptr, &hole_stats)) {
                                    lod_ground_screenshot_files.push_back(relative_path);
                                    lod_ground_visual_captures.push_back({
                                        names[i],
                                        relative_path,
                                        hole_stats
                                    });
                                    WriteLodGroundScreenshotIndex(scenario_config.artifact_dir, lod_ground_screenshot_files);
                                    WriteLodGroundVisualAnalysis(scenario_config.artifact_dir, lod_ground_visual_captures);
                                }
                            }
                        }
                        if (scenario_config.water_visual_smoke() && scenario_ready && !water_visual_capture_written) {
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            if (progress >= 0.50 && render_pass_stats.water_draws > 0 && water_visual_target.found) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                const std::string relative_path = "screenshots/water-visual.ppm";
                                ScreenshotPixelStats pixel_stats;
                                if (WriteBackbufferPpm(scenario_config.artifact_dir / relative_path, screenshot_width, screenshot_height, &pixel_stats)) {
                                    water_visual_capture_written = true;
                                    WriteWaterVisualAnalysis(
                                        scenario_config.artifact_dir,
                                        relative_path,
                                        water_visual_target,
                                        pixel_stats,
                                        render_pass_stats,
                                        renderPipeline.get_last_mesh_upload_stats()
                                    );
                                }
                            }
                        }
                        if (scenario_config.material_visual_smoke() && scenario_ready && !material_visual_capture_written) {
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            if (progress >= 0.50 && render_pass_stats.terrain_draws > 0 && material_visual_target.found) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());

                                    const std::string screenshot_path = "screenshots/material-visual.ppm";
                                    const std::string heatmap_path = "screenshots/material-id-heatmap.ppm";
                                    const MaterialPixelStats material_stats =
                                        AnalyzeMaterialPixels(frame_pixels, screenshot_width, screenshot_height);
                                    const std::vector<unsigned char> heatmap_pixels =
                                        BuildMaterialHeatmap(frame_pixels, screenshot_width, screenshot_height);
                                    const bool wrote_screenshot = WritePixelBufferPpm(
                                        scenario_config.artifact_dir / screenshot_path,
                                        screenshot_width, screenshot_height, frame_pixels);
                                    const bool wrote_heatmap = WritePixelBufferPpm(
                                        scenario_config.artifact_dir / heatmap_path,
                                        screenshot_width, screenshot_height, heatmap_pixels);
                                    if (wrote_screenshot && wrote_heatmap) {
                                        material_visual_capture_written = true;
                                        WriteMaterialVisualAnalysis(
                                            scenario_config.artifact_dir,
                                            screenshot_path,
                                            heatmap_path,
                                            material_visual_target,
                                            material_stats,
                                            render_pass_stats
                                        );
                                    }
                                }
                            }
                        }
                        if (now - last_runtime_state_write >= std::chrono::seconds(1)) {
                            last_readiness_report = EvaluateReadiness(scenario_config, gameSession.get());
                            runtime_state_recorder.capture("running", &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, last_readiness_report);
                            last_runtime_state_write = now;
                        }
                        if (scenario_ready && scenario_config.timed_run_seconds > 0) {
                            const auto elapsed_play_seconds =
                                std::chrono::duration_cast<std::chrono::seconds>(now - scenario_play_started_at).count();
                            if (elapsed_play_seconds >= scenario_config.timed_run_seconds && scenario_frame_count > 0) {
                                last_readiness_report = EvaluateReadiness(scenario_config, gameSession.get());
                                runtime_state_recorder.capture("timed_run_complete", &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, last_readiness_report);
                                scenario_timed_run_complete = true;
                                glfwSetWindowShouldClose(window, true);
                            }
                        }
                    }
                    if (runtime_boot_recorder.enabled()) {
                        runtime_boot_recorder.record_frame(deltaTime, renderPipeline);
                        if (runtime_boot_recorder.complete()) {
                            const bool wrote_metrics = runtime_boot_recorder.write_artifacts();
                            LUMINUMBRA_CORE_INFO(
                                "Runtime boot metrics {}: {} frames -> {}",
                                wrote_metrics ? "written" : "failed",
                                runtime_boot_frames,
                                runtime_boot_output.string());
                            glfwSetWindowShouldClose(window, true);
                        }
                    }
                }
            } else { // Main Menu, etc.
                if (g_uiManager) {
                    g_uiManager->Render();
                }
            }
        }
        
        if (currentState == GameState::IN_GAME) {
            if (g_imgui_enabled && g_playerController) {
                g_playerController->RenderDebugUI();
            }
            if (g_imgui_enabled && show_worldgen_viewer && worldGenViewer) {
                worldGenViewer->UpdateAndRender(show_worldgen_viewer, gameSession->GetWorldSystem());
            }
        }
        
        if (g_imgui_enabled) {
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        glfwSwapBuffers(window);
    }

    if (scenario_config.active() &&
        scenario_config.timed_run_seconds > 0 &&
        scenario_ready &&
        !scenario_timed_run_complete &&
        !scenario_failed)
    {
        const auto elapsed_play_seconds =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - scenario_play_started_at).count();
        if (elapsed_play_seconds < scenario_config.timed_run_seconds) {
            scenario_failed = true;
            scenario_failure_reason = "window_closed_before_timed_run_complete";
            exit_code = 5;
            last_readiness_report = EvaluateReadiness(scenario_config, gameSession.get());
            runtime_state_recorder.capture("window_closed_before_timed_run_complete", &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, last_readiness_report);
        }
    }

    if (scenario_failed && exit_code == 0) {
        exit_code = 2;
    }
    if (scenario_config.active()) {
        const std::string shutdown_phase = scenario_failed ? ("scenario_failed_" + scenario_failure_reason) : "shutdown_begin";
        runtime_state_recorder.capture(shutdown_phase, &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, last_readiness_report);
    }
    if (lod_ground_frame_recorder.enabled()) {
        lod_ground_frame_recorder.write_artifacts();
        if (!lod_ground_screenshot_files.empty()) {
            WriteLodGroundScreenshotIndex(scenario_config.artifact_dir, lod_ground_screenshot_files);
            WriteLodGroundVisualAnalysis(scenario_config.artifact_dir, lod_ground_visual_captures);
        }
    }

    std::vector<std::string> shutdown_milestones;
    auto mark_shutdown = [&](const std::string& milestone) {
        shutdown_milestones.push_back(milestone);
    };

    if (auto* world_system = gameSession->GetWorldSystem()) {
        world_system->clear_world(gameSession->GetPhysicsSystem());
        mark_shutdown("world_cleared");
    }
    g_playerController.reset();
    mark_shutdown("player_controller_reset");
    g_camera.reset();
    mark_shutdown("camera_reset");
    if (g_uiManager) {
        g_uiManager->Shutdown();
        g_uiManager.reset();
        mark_shutdown("ui_shutdown");
    } else {
        mark_shutdown("ui_not_started");
    }
    if (g_loading_visualizer) {
        g_loading_visualizer->Shutdown();
        g_loading_visualizer.reset();
        mark_shutdown("loading_visualizer_shutdown");
    }
    audioManager->Shutdown();
    mark_shutdown("audio_shutdown");
    renderPipeline.shutdown();
    mark_shutdown("renderer_shutdown");
    if (g_imgui_enabled) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        mark_shutdown("imgui_shutdown");
    } else {
        mark_shutdown("imgui_not_started");
    }
    gameSession.reset();
    mark_shutdown("game_session_reset");
    jobSystem.shutdown();
    mark_shutdown("job_system_shutdown");
    const auto shutdown_job_stats = jobSystem.get_runtime_stats();
    runtime_state_recorder.write_shutdown(shutdown_milestones, shutdown_job_stats);
    glfwDestroyWindow(window);
    glfwTerminate();
    return exit_code;
}

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (key == GLFW_KEY_F7 && action == GLFW_PRESS) {
        show_worldgen_viewer = !show_worldgen_viewer;
        if (show_worldgen_viewer) {
             glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        return;
    }
    if (key == GLFW_KEY_F11 && action == GLFW_PRESS) {
        ToggleFullscreen(window, g_windowState);
        return;
    }
    if (key == GLFW_KEY_F9 && action == GLFW_PRESS) {
        wireframe_mode = !wireframe_mode;
        return;
    }

    if (g_playerController) {
        g_playerController->ProcessKeyInput(key, action);
    }
    
    if (g_imgui_enabled && ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureKeyboard) {
            return;
        }
    }

    // Always give RmlUi a chance to process key events.
    if (g_uiManager) {
        g_uiManager->KeyCallback(window, key, scancode, action, mods);
        // If RmlUi has a focused input element, it should consume the event.
        if (g_uiManager->GetContext() && g_uiManager->GetContext()->GetFocusElement()) {
             return;
        }
    }
}

void SetGameState(GLFWwindow* window, GameStateManager& gameStateManager, GameState newState) {
    gameStateManager.SetState(newState);
    bool cursorDisabled = (newState == GameState::IN_GAME);

    if (cursorDisabled) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        // Set game-related callbacks
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetScrollCallback(window, scroll_callback);
        // Mouse buttons could be set here for game actions if needed
        glfwSetMouseButtonCallback(window, nullptr);
        firstMouse = true;
    } else {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (g_uiManager) {
            // Set UI-related callbacks
            glfwSetCursorPosCallback(window, Luminumbra::Client::Rml_UIManager::CursorPosCallback);
            glfwSetScrollCallback(window, Luminumbra::Client::Rml_UIManager::ScrollCallback);
            glfwSetMouseButtonCallback(window, Luminumbra::Client::Rml_UIManager::MouseButtonCallback);
        } else {
            glfwSetCursorPosCallback(window, nullptr);
            glfwSetScrollCallback(window, nullptr);
            glfwSetMouseButtonCallback(window, nullptr);
        }
    }
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    (void)window;
    if (firstMouse) {
        lastX = (float)xpos;
        lastY = (float)ypos;
        firstMouse = false;
    }
    float xoffset = (float)xpos - lastX;
    float yoffset = lastY - (float)ypos;
    lastX = (float)xpos;
    lastY = (float)ypos;
    if (g_camera) g_camera->ProcessMouseMovement(xoffset, yoffset);
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)window;
    (void)xoffset;
    const bool imgui_wants_mouse = g_imgui_enabled && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
    if (imgui_wants_mouse || (g_uiManager && g_uiManager->GetContext()->GetHoverElement() != nullptr)) {
        return;
    }
    if (g_camera) g_camera->ProcessMouseScroll((float)yoffset);
    if (g_playerController) g_playerController->ProcessMouseScroll(yoffset);
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    auto* pipeline = static_cast<Luminumbra::Rendering::RenderPipeline*>(glfwGetWindowUserPointer(window));
    if (pipeline) pipeline->on_resize(width, height);
}

void GLFWErrorCallback(int error, const char* description) {
    LUMINUMBRA_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
}

void GLAPIENTRY GLDebugMessageCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam) {
    (void)source;
    (void)length;
    (void)userParam;
    if(id == 131169 || id == 131185 || id == 131218 || id == 131204) return; 
    g_gl_debug_message_count.fetch_add(1, std::memory_order_relaxed);
    const bool is_error = type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH;
    if (is_error) {
        g_gl_debug_error_count.fetch_add(1, std::memory_order_relaxed);
        LUMINUMBRA_CORE_ERROR("OpenGL: {0}", message);
        return;
    }

    switch (severity) {
        case GL_DEBUG_SEVERITY_NOTIFICATION:
            g_gl_debug_notification_count.fetch_add(1, std::memory_order_relaxed);
            LUMINUMBRA_CORE_TRACE("OpenGL: {0}", message);
            break;
        default:
            g_gl_debug_warning_count.fetch_add(1, std::memory_order_relaxed);
            LUMINUMBRA_CORE_WARN("OpenGL: {0}", message);
            break;
    }
}
