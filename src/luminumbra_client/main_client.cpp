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
#include "rendering/FarLodSystem.h"
#include "rendering/RenderPipeline.h"
#include "rendering/passes/WaterPass.h"
#include "rendering/passes/ParticlePass.h" // T-I5a-1: EmitterDescriptor + accessor type
#include "rendering/WorldLoadingVisualizer.h"
#include "ui/Rml_UIManager.h"
#include "audio/AudioManagerFactory.h"
#include "audio/IAudioManager.h"
#include "audio/NullAudioManager.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
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
#include <cstring>
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

// --- Window-mode runtime state (T-I4-DR-window-modes) ---
// Tracks the active window arrangement plus the saved windowed geometry so the
// Alt+Enter windowed<->borderless toggle (and F11 exclusive-fullscreen toggle)
// can restore it. The framebuffer-size callback writes pending sizes here; the
// main loop debounces them to a single RenderPipeline::on_resize per settle
// window so dragging the window edge does not reallocate targets every event.
struct WindowState {
    Luminumbra::Client::ScenarioHarness::WindowMode mode =
        Luminumbra::Client::ScenarioHarness::WindowMode::Borderless;
    // Geometry to restore when leaving borderless/fullscreen back to windowed.
    int windowedX = 100, windowedY = 100;
    int windowedWidth = 1280, windowedHeight = 720;
    // Capture-pin lock: when true the window is held at the pinned capture size
    // and the runtime mode toggles are suppressed (scenario/capture runs).
    bool capture_pinned = false;

    // Debounced framebuffer resize (driven by the GLFW framebuffer-size cb).
    bool resize_pending = false;
    int pending_width = 0;
    int pending_height = 0;
    double pending_since_seconds = 0.0;
};
WindowState g_windowState;

// Debounce window for framebuffer resizes (seconds). A drag emits a burst of
// framebuffer-size events; we coalesce them into one realloc once the size has
// been stable for this long.
constexpr double kResizeDebounceSeconds = 0.12;

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
            // Total across both priority lanes (validators read this field).
            {"queue_depth", stats.queue_depth},
            {"high_priority_queue_depth", stats.high_priority_queue_depth},
            {"normal_priority_queue_depth", stats.normal_priority_queue_depth},
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
            {"sdf_payload_bytes", stats.sdf_payload_bytes},
            {"heightmap_payload_bytes", stats.heightmap_payload_bytes},
            {"sdf_skipped_chunks", stats.sdf_skipped_chunks},
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
            {"far_region_draws", stats.far_region_draws},
            {"far_indices_drawn", stats.far_indices_drawn},
            {"gpu_timers_supported", stats.gpu_timers_supported},
            {"gbuffer_gpu_ms", stats.gbuffer_gpu_ms},
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
            }},
            // Texture-array residency telemetry (T-I4-6). texture_resident_bytes
            // is gated against the 96 MB iteration budget by the
            // TextureResidency validator mode.
            {"texture_residency", {
                {"texture_resident_bytes", stats.texture_resident_bytes},
                {"texture_resident_budget_bytes", stats.texture_resident_budget_bytes},
                {"within_budget", stats.texture_resident_within_budget},
                {"array_count", stats.texture_residency_array_count},
                {"layer_count", stats.texture_residency_layer_count}
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
            // Capture-pin protection (T-I4-DR-window-modes): record the active
            // window mode + the live render-target size so the offline gate can
            // hard-fail if a capture-mode run ever drifted off the pinned size.
            state["capture_pin"] = Luminumbra::Client::ScenarioHarness::CapturePinMetadata(
                m_config.window_mode,
                static_cast<int>(render_pipeline->screen_width()),
                static_cast<int>(render_pipeline->screen_height()));
            state["resize_generation"] = render_pipeline->resize_generation();
            state["upload_queue"] = UploadStatsToJson(render_pipeline->get_last_mesh_upload_stats());
            state["render_pass"] = RenderPassStatsToJson(render_pipeline->get_last_render_pass_stats());
            // Far-LOD scheduler telemetry (T-I3-9, FarLodHorizon gate inputs).
            if (const auto* farlod = render_pipeline->farlod()) {
                const auto& farlod_stats = farlod->stats();
                state["farlod"] = {
                    {"enabled", farlod_stats.enabled},
                    {"farlod_regions_wanted", farlod_stats.regions_wanted},
                    {"farlod_regions_resident", farlod_stats.regions_resident},
                    {"farlod_regions_missing", farlod_stats.regions_missing},
                    {"farlod_regions_building", farlod_stats.regions_building},
                    {"farlod_resident_bytes", farlod_stats.resident_bytes},
                    {"farlod_region_draws", farlod_stats.region_draws},
                    {"farlod_indices_drawn", farlod_stats.indices_drawn},
                    {"farlod_builds_completed_total", farlod_stats.builds_completed_total},
                    {"farlod_evictions_total", farlod_stats.evictions_total}
                };
            }
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
    // Takes an ALREADY-SORTED vector by const reference. The previous
    // by-value-copy-then-sort signature tripped a GCC 15 -O3
    // -Wfree-nonheap-object false positive when the inlined copy's
    // deallocation was folded (release lane, T-I3-20); sorting once at the
    // call site also avoids three copies/sorts of the frame-time vector.
    static double percentile_sorted(const std::vector<double>& sorted_values, double pct) {
        if (sorted_values.empty()) {
            return 0.0;
        }
        const double position = pct * static_cast<double>(sorted_values.size() - 1);
        const auto index = static_cast<size_t>(std::round(position));
        return sorted_values[std::min(index, sorted_values.size() - 1)];
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

        std::vector<double> deltas = frame_times_ms();
        std::sort(deltas.begin(), deltas.end());
        const auto elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_started_at).count();
        const double p50_ms = percentile_sorted(deltas, 0.50);
        const double p95_ms = percentile_sorted(deltas, 0.95);
        const double p99_ms = percentile_sorted(deltas, 0.99);
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

namespace {

using Luminumbra::Client::ScenarioHarness::WindowMode;

// Monitor under the window's center (falls back to the primary monitor). Used
// so borderless/fullscreen target the display the window currently lives on.
GLFWmonitor* MonitorForWindow(GLFWwindow* window) {
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(window, &wx, &wy);
    glfwGetWindowSize(window, &ww, &wh);
    const int cx = wx + ww / 2;
    const int cy = wy + wh / 2;

    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh) {
            return monitors[i];
        }
    }
    return glfwGetPrimaryMonitor();
}

// Saves the current windowed geometry so a later return to windowed restores it.
void SaveWindowedGeometry(GLFWwindow* window, WindowState& state) {
    glfwGetWindowPos(window, &state.windowedX, &state.windowedY);
    glfwGetWindowSize(window, &state.windowedWidth, &state.windowedHeight);
}

// Applies a window mode to an existing window. capture_pinned runs (scenario
// captures) are never reconfigured: they stay at the pinned size in a hidden /
// stable window so every pixel-ROI gate sees exactly 1280x720.
void ApplyWindowMode(GLFWwindow* window, WindowState& state, WindowMode mode) {
    if (state.capture_pinned) {
        state.mode = mode; // record intent, but do not touch the pinned window
        return;
    }
    if (mode == state.mode) return;

    // Leaving windowed: remember where it was so we can come back to it.
    if (state.mode == WindowMode::Windowed) {
        SaveWindowedGeometry(window, state);
    }

    GLFWmonitor* monitor = MonitorForWindow(window);
    switch (mode) {
        case WindowMode::Windowed: {
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowAttrib(window, GLFW_RESIZABLE, GLFW_TRUE);
            glfwSetWindowMonitor(window, nullptr, state.windowedX, state.windowedY,
                                 state.windowedWidth, state.windowedHeight, 0);
            break;
        }
        case WindowMode::Borderless: {
            // Borderless window covering the monitor work-area (no exclusive
            // video-mode switch, no decorations).
            int mx = 0, my = 0, mw = 0, mh = 0;
            glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
            glfwSetWindowMonitor(window, nullptr, mx, my, mw, mh, 0);
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowAttrib(window, GLFW_RESIZABLE, GLFW_FALSE);
            break;
        }
        case WindowMode::Fullscreen: {
            // Exclusive fullscreen at the monitor's native video mode.
            const GLFWvidmode* vmode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(window, monitor, 0, 0, vmode->width, vmode->height, vmode->refreshRate);
            break;
        }
        case WindowMode::Headless:
            glfwHideWindow(window);
            break;
    }
    state.mode = mode;
    LUMINUMBRA_CORE_INFO("Window mode -> {}", Luminumbra::Client::ScenarioHarness::WindowModeName(mode));
}

} // namespace

using Luminumbra::Client::ScenarioHarness::WindowMode;

// Alt+Enter runtime toggle: windowed <-> borderless. Suppressed on
// capture-pinned (scenario) runs.
void ToggleWindowedBorderless(GLFWwindow* window, WindowState& state) {
    if (state.capture_pinned) return;
    const WindowMode next = (state.mode == WindowMode::Windowed)
        ? WindowMode::Borderless
        : WindowMode::Windowed;
    ApplyWindowMode(window, state, next);
}

// F11 toggle: exclusive fullscreen <-> windowed (kept for back-compat with the
// previous F11 binding).
void ToggleFullscreen(GLFWwindow* window, WindowState& state) {
    if (state.capture_pinned) return;
    const WindowMode next = (state.mode == WindowMode::Fullscreen)
        ? WindowMode::Windowed
        : WindowMode::Fullscreen;
    ApplyWindowMode(window, state, next);
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

    // --- Window-mode resolution (T-I4-DR-window-modes) ---
    // GATE PROTECTION: any scenario/capture run PINS the window to 1280x720 so
    // every pixel-ROI gate sees the same framebuffer regardless of CLI flags.
    // Outside scenarios, --window-mode / --resolution select the arrangement.
    using Luminumbra::Client::ScenarioHarness::kCapturePinnedWidth;
    using Luminumbra::Client::ScenarioHarness::kCapturePinnedHeight;
    const bool capture_pinned = scenario_config.requires_pinned_capture();
    g_windowState.capture_pinned = capture_pinned;
    g_windowState.mode = scenario_config.window_mode;

    int create_width = kCapturePinnedWidth;
    int create_height = kCapturePinnedHeight;
    if (!capture_pinned && scenario_config.window_mode == WindowMode::Windowed) {
        create_width = scenario_config.windowed_width;
        create_height = scenario_config.windowed_height;
    }
    g_windowState.windowedWidth = create_width;
    g_windowState.windowedHeight = create_height;

    // Headless (and runtime-boot metrics) keep a hidden window. Capture-pinned
    // runs also create the window hidden/decorated at the pinned size; the mode
    // application below is suppressed for them.
    const bool hidden_window =
        runtime_boot_recorder.enabled() || scenario_config.hidden_window ||
        scenario_config.window_mode == WindowMode::Headless;
    if (hidden_window) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }
    if (scenario_config.active()) {
        // Automated gate runs need a visible window (Endurance300 asserts it)
        // but must not steal focus from whatever the developer is doing.
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        glfwWindowHint(GLFW_FLOATING, GLFW_FALSE);
    }
    #ifdef LUMINUMBRA_DEBUG
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
    #endif

    GLFWwindow* window = glfwCreateWindow(create_width, create_height, "Luminumbra", nullptr, nullptr);
    LUMINUMBRA_ASSERT(window, "Failed to create GLFW window!");
    glfwMakeContextCurrent(window);

    // [[maybe_unused]]: LUMINUMBRA_ASSERT compiles out in release builds
    // (T-I3-20 release perf lane builds with -Werror).
    [[maybe_unused]] const int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
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
    // T-I3-6 asset-manifest split: the engine validates simulation
    // requirements only; the CLIENT declares the renderer/UI assets it needs
    // before any world create/load. This list matches the pre-split
    // engine-side manifest byte for byte.
    gameSession->SetRequiredClientAssets({
        std::filesystem::path("res") / "shaders" / "basic.vert",
        std::filesystem::path("res") / "shaders" / "g_buffer.frag",
        std::filesystem::path("res") / "shaders" / "sdf_generation.compute",
        std::filesystem::path("data") / "ui" / "main_menu.rml",
        std::filesystem::path("data") / "fonts" / "Lora" / "static" / "Lora-Regular.ttf",
    });

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
    // T-I3-9: far-LOD tile builds ride the JobSystem Normal lane.
    renderPipeline.attach_farlod_job_system(&jobSystem);
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
    // T-I4-DR-split-lint: data-driven skinned-mesh texture set. The scenario
    // config resolved the .ltex paths (from the game archetype JSON or a generic
    // test texture); hand them to the generic RenderPipeline loader so no
    // creature/content name lives in engine source.
    if (!scenario_config.skinned_albedo_texture.empty()) {
        int skinned_albedo_layer = -1;
        int skinned_normal_layer = -1;
        renderPipeline.load_skinned_texture_set(
            root_dir / scenario_config.skinned_albedo_texture,
            scenario_config.skinned_normal_texture.empty()
                ? std::filesystem::path{}
                : (root_dir / scenario_config.skinned_normal_texture),
            skinned_albedo_layer, skinned_normal_layer);
    }
    glfwSetWindowUserPointer(window, &renderPipeline);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    // Apply the requested interactive window mode now that the GL context and
    // render pipeline exist: the framebuffer-size callback this triggers drives
    // RenderPipeline::on_resize so all targets are reallocated to the real
    // framebuffer size. Capture-pinned (scenario) runs are intentionally left at
    // the pinned 1280x720 window (ApplyWindowMode is a no-op for them).
    if (!capture_pinned && scenario_config.window_mode != WindowMode::Headless) {
        ApplyWindowMode(window, g_windowState, scenario_config.window_mode);
        // The framebuffer-size callback only fires on a real change; for the
        // borderless/fullscreen path it does, but resync the pipeline directly
        // in case GLFW coalesced the event so targets always match the window.
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (fbw > 0 && fbh > 0) {
            renderPipeline.on_resize(static_cast<unsigned int>(fbw), static_cast<unsigned int>(fbh));
            framebufferWidth = fbw;
            framebufferHeight = fbh;
        }
    }

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
        // T-I3-9: drain in-flight far-LOD tile builds before CreateWorld
        // replaces the world system they sample.
        renderPipeline.prepare_world_swap();
        // 1. Synchronously create the world systems and metadata. This is fast.
        if (gameSession->CreateWorld(name, seed, worldType)) {
            if (auto* world_system = gameSession->GetWorldSystem()) {
                renderPipeline.SetupGPUSDFIntegration(*world_system);
            }

            // T-I2-12: restore persisted chunk state AFTER the world systems
            // initialize but BEFORE any chunk generation runs, so saved voxel
            // edits cannot be clobbered by regeneration (generation skips
            // chunks that already carry voxel data). A world without a
            // snapshot is a clean miss and proceeds on the byte-for-byte
            // unchanged fresh-world path.
            if (scenario_config.persistence_roundtrip_smoke() &&
                scenario_config.persistence_phase == "load" &&
                !scenario_config.persistence_session_dir.empty()) {
                gameSession->LoadWorldStateFrom(scenario_config.persistence_session_dir);
            } else {
                gameSession->LoadWorldState();
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

    // The endurance and water gates assert visible water; the default
    // preset (height_offset 20, sea level 0) generates none near spawn,
    // so every water-asserting scenario runs in the archipelago world.
    // lod_ground_smoke keeps the default world its thresholds were tuned on.
    // player_view_smoke (T-I3-3) takes its preset from --world-preset
    // (default mountains, the worst case for surface-span coverage) and runs
    // once per preset from the PlayerView validator mode.
    const std::string scenario_world_type =
        (scenario_config.player_view_smoke() || scenario_config.farlod_horizon_smoke())
            ? (scenario_config.world_preset.empty() ? std::string("mountains") : scenario_config.world_preset)
            : ((scenario_config.water_visual_smoke() || scenario_config.material_visual_smoke() ||
                scenario_config.auto_world_smoke() || scenario_config.persistence_roundtrip_smoke() ||
                scenario_config.creature_slice_smoke()) ? "archipelago" : "default");
    if (scenario_config.auto_create_world || HasCommandLineFlag(argc, argv, "--auto-create-world") || runtime_boot_recorder.enabled()) {
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
    std::vector<WaterCausticsSample> water_caustics_samples;
    std::vector<unsigned char> water_caustics_previous_texels;
    double water_caustics_next_sample_seconds = 0.0;
    ScreenshotPixelStats water_visual_pixel_stats;
    WaterRegionPatch water_shallow_patch;
    WaterRegionPatch water_deep_patch;
    WaterRegionPatch water_foam_patch;
    bool water_reflection_capture_written = false;
    // Projects a world point into capture pixel coordinates using the same
    // projection the render pipeline builds. Returns false when the point is
    // behind the camera or too close to the frame edge for a full patch.
    const auto project_world_to_capture = [](const Luminumbra::Rendering::Camera& camera,
                                             const Luminumbra::Vec3& world,
                                             int width,
                                             int height,
                                             int margin,
                                             int& out_x,
                                             int& out_y_from_top) -> bool {
        if (width <= 0 || height <= 0) {
            return false;
        }
        const glm::mat4 projection = glm::perspective(
            glm::radians(camera.Zoom),
            static_cast<float>(width) / static_cast<float>(height),
            camera.GetNearPlane(),
            camera.GetFarPlane());
        const glm::vec4 clip = projection * camera.GetViewMatrix() * glm::vec4(world, 1.0f);
        if (clip.w <= 0.0f) {
            return false;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        const int x = static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(width));
        const int y_from_bottom = static_cast<int>((ndc.y * 0.5f + 0.5f) * static_cast<float>(height));
        const int y_from_top = height - 1 - y_from_bottom;
        if (x < margin || x >= width - margin || y_from_top < margin || y_from_top >= height - margin) {
            return false;
        }
        out_x = x;
        out_y_from_top = y_from_top;
        return true;
    };
    WaterVisualCameraTarget material_visual_target;
    bool material_visual_target_initialized = false;
    bool material_visual_capture_written = false;
    bool skybox_visual_capture_written = false;
    bool weather_baseline_capture_written = false;
    bool weather_visual_capture_written = false;
    WeatherPixelStats weather_baseline_stats;
    // T-I5a-8 cloud-shadow scenario state. Two terrain ROI captures (t0/t1) as the
    // cloud-shadow edge drifts, a sky capture for cloud presence, and a clouds-off
    // lighting-pass GPU timing captured before enabling the shadow (budget check).
    bool cloud_shadow_t0_written = false;
    bool cloud_shadow_done = false;
    double cloud_shadow_terrain_luma_t0 = 0.0;
    double cloud_shadow_scroll_t0 = 0.0;
    double cloud_shadow_lighting_ms_off = 0.0;
    bool cloud_shadow_lighting_off_sampled = false;
    // T-I5a-1 particle determinism scenario state.
    bool particle_emitter_spawned = false;
    bool particle_determinism_capture_written = false;
    // T-I5a-4 (B2): precipitation scenario state. The run spawns the rain emitter
    // (driven by the replicated weather state) and captures TWO frames -- a CALM
    // phase (no wind) and a WINDY phase (wind-advected slant) -- so the gate can
    // assert precip particles are present AND that they slant with wind.
    bool precip_emitter_spawned = false;
    bool precip_calm_capture_written = false;
    bool precip_windy_capture_written = false;
    PrecipPixelStats precip_calm_stats;
    Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats precip_calm_render_pass;
    // T-I5a-7 (C2): 6 season-sweep windows (summer noon/dusk/night, winter
    // noon/dusk/night).
    std::array<bool, 6> timeofday_season_captures_written{false, false, false, false, false, false};
    std::vector<TimeOfDayPhaseCapture> timeofday_phase_captures;
    EmissiveMaterialTarget timeofday_emissive_target;
    bool timeofday_emissive_target_initialized = false;
    bool timeofday_emissive_capture_written = false;
    TimeOfDayPixelStats timeofday_emissive_stats;
    bool timeofday_analysis_final = false;
    LodBoundaryTransitionRecorder lod_boundary_transition_recorder;
    LodSeamArrivalRecorder lod_seam_arrival_recorder;
    std::array<bool, 4> lod_seam_screenshots_written{false, false, false, false};
    std::vector<LodGroundVisualCapture> lod_seam_visual_captures;
    bool persistence_phase_attempted = false;
    std::vector<PlayerViewStation> player_view_stations;
    std::vector<bool> player_view_captures_written;
    std::vector<PlayerViewStationCapture> player_view_station_captures;
    bool player_view_sky_enforced = true;
    // farlod_horizon_smoke (T-I3-9): phase A (far-LOD disabled) measures the
    // honest in-run gbuffer GPU baseline, phase B enables far-LOD and sweeps
    // the stations across the live/far boundary.
    constexpr double kFarLodHorizonPhaseSplit = 0.30;
    std::vector<FarLodHorizonStation> farlod_horizon_stations;
    std::vector<bool> farlod_horizon_captures_written;
    std::vector<FarLodHorizonStationCapture> farlod_horizon_station_captures;
    bool farlod_horizon_sky_enforced = true;
    std::vector<double> farlod_baseline_gbuffer_samples;
    std::vector<double> farlod_far_gbuffer_samples;
    // T-I4-DR-sliver-baseline-diff: each station's far-OFF above-horizon sliver
    // baseline, captured PAIRED with its far-ON capture in phase B (an extra
    // far-disabled render at the same camera, same frame). Thin LIVE
    // mountain/island peak/ridge silhouettes (and diagonal live-geometry slivers)
    // are legitimate geometry that classify as slivers in BOTH renders; the gated
    // far-ATTRIBUTABLE sliver re-analyzes the far-ON frame with the far-OFF frame's
    // intrusion pixels cancelled per-pixel in a 3x3 neighborhood, so they cancel
    // pixel-for-pixel and only a genuine far-render streak (present only when far
    // is on) survives. This vector retains the raw far-OFF measurement as
    // telemetry only. Indexed by station; -1 = not yet captured.
    std::vector<int> farlod_horizon_far_off_sliver_px;
    // T-I4-DR-sliver-baseline-diff: station the camera was applied to this frame;
    // the post-render capture targets exactly this station so the analyzed back
    // buffer always matches the camera that rendered it.
    std::size_t farlod_horizon_applied_station = 0;
    // skinned_mesh_visual_smoke (T-I3-16): rig spawned once after readiness;
    // captures at two clip times prove the skinned stage renders and animates.
    SkinnedMeshVisualTarget skinned_mesh_visual_target;
    bool skinned_mesh_spawn_attempted = false;
    bool skinned_mesh_capture_a_written = false;
    bool skinned_mesh_analysis_written = false;
    SkinnedMeshVisualCapture skinned_mesh_capture_a;
    std::vector<unsigned char> skinned_mesh_pixels_a;
    // creature_slice_smoke (T-I3-18): data-driven creature game slice. The
    // stimulus appears at 55% progress; captures at 45% and 85%.
    CreatureSliceScene creature_slice_scene;
    bool creature_slice_spawn_attempted = false;
    bool creature_slice_before_written = false;
    bool creature_slice_analysis_written = false;
    CreatureSliceCapture creature_slice_before;
    // window_mode_stress_smoke (T-I4-DR-window-modes): scripted resize cycle
    // exercised once after readiness. Each step drives RenderPipeline::on_resize
    // (windowed->borderless->resolutions->fullscreen->restore-pinned) and
    // records the resize-generation delta + GL error count; the final pinned
    // step is captured as a Smoke-equivalent screenshot.
    std::vector<WindowModeStressStep> window_mode_stress_steps;
    std::size_t window_mode_stress_step_index = 0;
    bool window_mode_stress_complete = false;
    bool window_mode_stress_analysis_written = false;
    WindowModeStressCapture window_mode_stress_capture;
    // networked_session_smoke (T-I4-14): the client renders a SERVER-OWNED world
    // streamed over the lockstep transport. The driver owns the host authority +
    // both LockstepSession ends (LoopbackTransport, no sockets); per agreed tick
    // both worlds step one fixed sim tick from the SAME spawn anchor and exchange
    // hashes (the desync oracle). Camera LOOK is applied render-side each frame
    // and is NEVER sent through the session, so look latency is zero (research
    // worldgen-lockstep-sdfrt.md Area 2 takeaway 2).
    NetworkedSessionDriver networked_session_driver;
    bool networked_session_begun = false;
    bool networked_session_done = false;
    const auto median_of = [](std::vector<double> samples) -> double {
        if (samples.empty()) {
            return 0.0;
        }
        std::sort(samples.begin(), samples.end());
        return samples[(samples.size() - 1) / 2];
    };
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

        // Debounced framebuffer resize (T-I4-DR-window-modes): coalesce a burst
        // of drag events into one RenderPipeline::on_resize once the size has
        // settled. RmlUi (Update/Render) and ImGui (GLFW backend NewFrame)
        // re-query the framebuffer size every frame, so their viewports track
        // the new size automatically once the GL targets are reallocated here.
        if (g_windowState.resize_pending && !g_windowState.capture_pinned) {
            const double now = glfwGetTime();
            if (now - g_windowState.pending_since_seconds >= kResizeDebounceSeconds) {
                renderPipeline.on_resize(
                    static_cast<unsigned int>(g_windowState.pending_width),
                    static_cast<unsigned int>(g_windowState.pending_height));
                framebufferWidth = g_windowState.pending_width;
                framebufferHeight = g_windowState.pending_height;
                g_windowState.resize_pending = false;
            }
        }

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
                // T-I2-13: persistence runtime roundtrip phases run once on
                // the first ready frame and exit cleanly; the streaming
                // update is skipped so the hashed/saved chunk set is exactly
                // the deterministic post-readiness world.
                if (scenario_config.persistence_roundtrip_smoke() && scenario_ready && !persistence_phase_attempted) {
                    persistence_phase_attempted = true;
                    PersistenceRoundtripPhaseResult phase_result;
                    if (scenario_config.persistence_phase == "load") {
                        phase_result = RunPersistenceRoundtripLoadPhase(scenario_config, gameSession.get());
                    } else {
                        phase_result = RunPersistenceRoundtripSavePhase(scenario_config, gameSession.get());
                    }
                    if (!phase_result.passed) {
                        scenario_failed = true;
                        scenario_failure_reason = "persistence_phase_" + phase_result.failure_reason;
                        runtime_state_recorder.capture(scenario_failure_reason, &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, last_readiness_report);
                    }
                    glfwSetWindowShouldClose(window, true);
                    break;
                }
                if (scenario_config.lod_ground_smoke() && scenario_ready && g_camera) {
                    const double elapsed_play_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - scenario_play_started_at).count();
                    ApplyLodGroundCameraPath(scenario_config, gameSession.get(), g_camera.get(), elapsed_play_seconds);
                    // T-I4-DR-churn-perf: the engine no longer synchronously
                    // catches the near field up on a camera discontinuity (that
                    // hook caused an 11x chunk_churn PerfRegression). The
                    // LodGround camera sweeps in wall-clock-driven jumps that can
                    // outrun the async, throttled activation/meshing path, so the
                    // exact frame the coverage gate samples could show a near-field
                    // dip. Pull the destination near surface band ready
                    // synchronously right after moving the camera and before this
                    // frame renders, so every captured frame is fully renderable
                    // (49/49). EnsureSurfaceReadyNear only (re)builds chunks not
                    // already Ready at the required LOD, so steady-state frames
                    // (camera already settled) pay nothing.
                    if (gameSession->GetWorldSystem() && gameSession->GetPhysicsSystem()) {
                        gameSession->GetWorldSystem()->EnsureSurfaceReadyNear(
                            g_camera->Position,
                            gameSession->GetPhysicsSystem(),
                            4,
                            1);
                    }
                } else if (scenario_config.water_visual_smoke() && scenario_ready && g_camera) {
                    if (!water_visual_target_initialized || !water_visual_target.found) {
                        water_visual_target = FindWaterVisualCameraTarget(gameSession.get());
                        water_visual_target_initialized = water_visual_target.found;
                    }
                    // T-I2-16b: top-down framing for the main capture and the
                    // caustics samples (first 60% of the run), then the
                    // grazing open-water framing for the reflection capture.
                    const double water_elapsed_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - scenario_play_started_at).count();
                    const double water_duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    if (water_elapsed_seconds / water_duration < 0.60) {
                        ApplyWaterVisualCamera(g_camera.get(), water_visual_target);
                    } else {
                        ApplyWaterReflectionCamera(g_camera.get(), water_visual_target);
                    }
                } else if (scenario_config.material_visual_smoke() && scenario_ready && g_camera) {
                    if (!material_visual_target_initialized || !material_visual_target.found) {
                        material_visual_target = FindMaterialVisualCameraTarget(gameSession.get());
                        material_visual_target_initialized = material_visual_target.found;
                    }
                    ApplyWaterVisualCamera(g_camera.get(), material_visual_target);
                } else if (scenario_config.skybox_visual_smoke() && scenario_ready && g_camera) {
                    ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.04f);
                } else if (scenario_config.weather_visual_smoke() && scenario_ready && g_camera) {
                    ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.04f);
                    // T-I5a-3 (B1): SIM-DRIVEN weather overlay (one-way, F2). First
                    // half of the run captures the CLEAR-SKY control (premise guard
                    // F4: this is the dedicated weather scenario with a clear-sky
                    // control phase); the weather phase at the midpoint pushes a
                    // render state derived from the REPLICATED WeatherSystem state
                    // sampled at the camera -- the overlay uniforms come from sim
                    // precipitation / storm / advected wind, not the debug mapping.
                    const double elapsed_play_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - scenario_play_started_at).count();
                    const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    const bool weather_phase = (elapsed_play_seconds / duration) >= 0.5;
                    const auto* weather = gameSession->GetWeatherSystem();
                    Luminumbra::Rendering::WeatherRenderState wstate;
                    if (weather_phase && weather) {
                        const Luminumbra::Vec3 cam(
                            g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
                        const auto sample = weather->SampleAt(cam);
                        // Sim precipitation drives the overlay. In this dedicated
                        // scenario we floor rain to a strong, deterministic value
                        // at capture so the gate's clear-vs-weather luma drop +
                        // streak gradient measure a stable overlay (premise guard:
                        // storms run ONLY here). The wind direction/strength + the
                        // storm intensity are taken straight from sim state.
                        const float precip = std::max(sample.precip_intensity, 1.0f);
                        wstate.rain_intensity = precip;
                        wstate.snow_intensity =
                            (sample.category == Luminumbra::Systems::WeatherCategory::Snow)
                                ? sample.precip_intensity : 0.0f;
                        wstate.fog_density =
                            (sample.category == Luminumbra::Systems::WeatherCategory::Fog)
                                ? 0.4f : 0.1f;
                        wstate.storm_intensity = std::max(sample.storm_intensity, 0.4f);
                        wstate.wetness = precip;
                        const float wlen = std::sqrt(
                            sample.wind.x * sample.wind.x + sample.wind.y * sample.wind.y);
                        if (wlen > 1e-4f) {
                            wstate.wind_direction =
                                glm::vec3(sample.wind.x / wlen, 0.0f, sample.wind.y / wlen);
                            wstate.wind_strength = std::clamp(wlen / 13.0f, 0.0f, 1.0f);
                        }
                        renderPipeline.set_weather_state(wstate);
                    } else {
                        // Clear-sky control: a driven CLEAR state (overlay off).
                        renderPipeline.set_weather_state(wstate);
                    }
                } else if (scenario_config.cloud_shadow_smoke() && scenario_ready && g_camera) {
                    // T-I5a-8 (C3): partly-cloudy cast-shadow scenario. Fixed noon
                    // camera framing lit terrain in the lower frame (strong sun ->
                    // strong cast shadow). Enable the wind-advected cloud layer +
                    // its projected cast shadow at a PARTLY-CLOUDY coverage (NOT
                    // overcast -- premise guard F4). A strong, fixed wind drifts the
                    // coverage field across the run so a shadow edge crawls over the
                    // fixed terrain ROI between the two captures. Render-only (F2):
                    // the cloud state never feeds back into the sim.
                    ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.04f);
                    const double elapsed_play_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - scenario_play_started_at).count();
                    const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                    Luminumbra::Rendering::WeatherRenderState wstate;
                    // Strong, deterministic wind so the cloud sheet drifts visibly
                    // across the ROI in the run window (one-way: this is the render
                    // wind the cloud scroll consumes; it is not written to the sim).
                    wstate.wind_direction = glm::vec3(1.0f, 0.0f, 0.0f);
                    wstate.wind_strength = 1.0f;
                    renderPipeline.set_weather_state(wstate);
                    Luminumbra::Rendering::CloudRenderState cstate;
                    cstate.enabled = true;
                    // Cast shadow is OFF for the first ~15% so a clouds-off lighting
                    // GPU baseline can be sampled, then ON for the rest (the added
                    // per-fragment sample cost = on - off, bounded by the budget).
                    cstate.shadow_enabled = progress >= 0.15;
                    cstate.coverage_amount = 0.5f;   // partly cloudy (not overcast)
                    cstate.biome_variation = 0.0f;
                    cstate.plane_height = 900.0f;
                    cstate.shadow_strength = 0.8f;
                    renderPipeline.set_cloud_state(cstate);
                } else if (scenario_config.particle_emitter_determinism_smoke() && scenario_ready && g_camera) {
                    // T-I5a-1: fixed skybox-style camera; spawn the fixture
                    // emitter ONCE in front of the camera so particles render.
                    ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.30f);
                    if (!particle_emitter_spawned) {
                        if (auto* particles = renderPipeline.particles()) {
                            const glm::vec3 spawn_origin =
                                g_camera->Position + g_camera->Front * 8.0f;
                            particles->add_emitter(
                                root_dir / "data/common/particles/fixture_sparkle.json",
                                spawn_origin);
                            particle_emitter_spawned = true;
                        }
                    }
                } else if (scenario_config.precipitation_smoke() && scenario_ready && g_camera) {
                    // T-I5a-4 (B2): RAIN through the A1 particle framework, driven
                    // by the REPLICATED weather state at the camera and WIND-ADVECTED
                    // by the A2 wind field. Two phases at the SAME framing: a CALM
                    // phase (zero wind -> vertical fall) then a WINDY phase (strong
                    // horizontal wind -> diagonal slant). ONE-WAY (F2): we READ
                    // weather/wind and write nothing back to sim/world_hash.
                    ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.30f);
                    const double elapsed_play_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - scenario_play_started_at).count();
                    const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                    const bool windy_phase = progress >= 0.5;

                    auto* particles = renderPipeline.particles();
                    if (particles != nullptr && !precip_emitter_spawned) {
                        // Spawn the rain field above + around the camera so the
                        // falling column fills the frame, plus the splash template.
                        const glm::vec3 field_origin(
                            g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
                        particles->add_emitter(
                            root_dir / "data/common/particles/precip_rain.json", field_origin);
                        particles->add_splash_emitter(
                            root_dir / "data/common/particles/precip_splash.json");
                        precip_emitter_spawned = true;
                    }

                    // Overcast/wet backdrop from the replicated weather state (the
                    // same one-way overlay the WeatherVisual gate exercises) so the
                    // rain reads against a darkened sky.
                    const auto* weather = gameSession->GetWeatherSystem();
                    Luminumbra::Rendering::WeatherRenderState wstate;
                    const Luminumbra::Vec3 cam(
                        g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
                    float sampled_wind_len = 0.0f;
                    glm::vec3 sampled_wind_dir(1.0f, 0.0f, 0.0f);
                    if (weather != nullptr) {
                        const auto sample = weather->SampleAt(cam);
                        wstate.rain_intensity = std::max(sample.precip_intensity, 1.0f);
                        wstate.storm_intensity = std::max(sample.storm_intensity, 0.4f);
                        wstate.wetness = wstate.rain_intensity;
                        wstate.fog_density = 0.1f;
                        sampled_wind_len = std::sqrt(
                            sample.wind.x * sample.wind.x + sample.wind.y * sample.wind.y);
                        if (sampled_wind_len > 1e-4f) {
                            sampled_wind_dir = glm::vec3(
                                sample.wind.x / sampled_wind_len, 0.0f, sample.wind.y / sampled_wind_len);
                            wstate.wind_direction = sampled_wind_dir;
                            wstate.wind_strength = std::clamp(sampled_wind_len / 13.0f, 0.0f, 1.0f);
                        }
                    } else {
                        wstate.rain_intensity = 1.0f;
                        wstate.wetness = 1.0f;
                    }
                    renderPipeline.set_weather_state(wstate);

                    // WIND-ADVECTION push (render-only). Calm phase: zero wind so
                    // rain falls straight down. Windy phase: a strong horizontal
                    // wind aligned with the camera-right axis so the slant is
                    // unambiguous in screen space and clearly diagonal. The wind
                    // DIRECTION comes from the replicated A2 field when available;
                    // its MAGNITUDE is floored to a strong, deterministic value in
                    // this dedicated scenario (premise guard: storms run only here).
                    if (particles != nullptr) {
                        if (windy_phase) {
                            glm::vec3 wind_dir = sampled_wind_dir;
                            // Bias the slant onto the camera-right axis so the
                            // 2D analyzer measures a clean horizontal lean.
                            const glm::vec3 right = glm::normalize(g_camera->Right);
                            if (glm::length(wind_dir) < 1e-3f) {
                                wind_dir = right;
                            } else {
                                wind_dir = glm::normalize(wind_dir + right);
                            }
                            const float wind_speed = 16.0f; // strong storm gust
                            particles->set_wind(wind_dir * wind_speed);
                        } else {
                            particles->set_wind(glm::vec3(0.0f));
                        }
                    }
                } else if (scenario_config.timeofday_sweep_smoke() && scenario_ready && g_camera) {
                    const double elapsed_play_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - scenario_play_started_at).count();
                    const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                    // Discovery reruns while meshes stream in (the first
                    // frames only carry a fraction of the surface meshes);
                    // it freezes once found or once the SUMMER night phase nears
                    // so the emissive camera target stays stable. T-I5a-7: the
                    // summer half ends at progress 0.5, so freeze discovery before
                    // its night/emissive window (~0.45-0.5).
                    if (!timeofday_emissive_target.found && progress < 0.43 &&
                        (!timeofday_emissive_target_initialized || (scenario_frame_count % 120) == 0)) {
                        timeofday_emissive_target_initialized = true;
                        timeofday_emissive_target = FindEmissiveMaterialTarget(gameSession.get(), root_dir);
                    }
                    if (timeofday_emissive_target.found && progress >= 0.44 && progress < 0.5) {
                        // End of the SUMMER half: aim at the discovered surface
                        // emissive material for the dedicated night-emissive
                        // capture (the existing emissive night check, unchanged).
                        g_camera->Position = timeofday_emissive_target.position + Luminumbra::Vec3(8.0f, 6.0f, 8.0f);
                        g_camera->Zoom = 60.0f;
                        AimCameraAt(g_camera.get(), timeofday_emissive_target.position);
                    } else {
                        // Fixed framing across all phases (both seasons) so the
                        // luminance/palette comparison measures lighting, not
                        // framing.
                        ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.04f);
                    }
                } else if (scenario_config.lod_boundary_oscillation_smoke() && scenario_ready && g_camera) {
                    const double elapsed_play_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - scenario_play_started_at).count();
                    ApplyLodBoundaryOscillationCamera(gameSession.get(), g_camera.get(), elapsed_play_seconds);
                } else if (scenario_config.lod_seam_arrival_smoke() && scenario_ready && g_camera) {
                    const double elapsed_play_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - scenario_play_started_at).count();
                    ApplyLodSeamArrivalCamera(scenario_config, gameSession.get(), g_camera.get(), elapsed_play_seconds);
                } else if (scenario_config.player_view_smoke() && scenario_ready && g_camera) {
                    // T-I3-3: eye-level 360-degree sweep. Stations are built
                    // once after readiness (the peak station scans the loaded
                    // span field); each station holds its window so streaming
                    // and uploads settle before the capture at 70% progress.
                    if (player_view_stations.empty()) {
                        player_view_stations = BuildPlayerViewStations(gameSession.get(), scenario_world_type);
                        player_view_captures_written.assign(player_view_stations.size(), false);
                        player_view_sky_enforced = !PlayerViewSeaWaterInNearField(gameSession.get());
                    }
                    const double elapsed_play_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - scenario_play_started_at).count();
                    const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    // Warmup lead before station 0: the post-readiness LOD0
                    // promotion of the near ring is still draining during the
                    // first seconds; stations divide the remaining time.
                    const double warmup_seconds = std::min(8.0, duration * 0.2);
                    const double effective_seconds = std::max(0.0, elapsed_play_seconds - warmup_seconds);
                    const double progress = std::clamp(
                        effective_seconds / std::max(1.0, duration - warmup_seconds), 0.0, 0.999);
                    const std::size_t wall_clock_index = std::min(
                        player_view_stations.size() - 1u,
                        static_cast<std::size_t>(progress * static_cast<double>(player_view_stations.size())));
                    // Hitch tolerance (mirrors the capture branch): hold the
                    // camera on the first un-captured station so a passed-over
                    // station is framed when its catch-up capture fires.
                    std::size_t first_unwritten = 0;
                    while (first_unwritten < player_view_captures_written.size() &&
                           player_view_captures_written[first_unwritten]) {
                        ++first_unwritten;
                    }
                    const std::size_t station_index =
                        first_unwritten >= player_view_stations.size()
                            ? wall_clock_index
                            : std::min(wall_clock_index, first_unwritten);
                    ApplyPlayerViewCamera(gameSession.get(), g_camera.get(), player_view_stations[station_index]);
                } else if (scenario_config.farlod_horizon_smoke() && scenario_ready && g_camera) {
                    // T-I3-9 / T-I4-DR-sliver-baseline-diff: phase A holds station 0
                    // with far-LOD DISABLED (the in-run gbuffer GPU baseline); phase
                    // B enables far-LOD and sweeps the stations. The per-station
                    // far-OFF sliver baseline is captured PAIRED with the far-ON
                    // capture in phase B (an extra far-disabled render at the exact
                    // same camera the same frame), so the diagonal live-geometry
                    // streaks are pixel-aligned and the far-attributable analysis
                    // cancels them per-pixel (3x3 neighborhood mask) cleanly.
                    if (farlod_horizon_stations.empty()) {
                        farlod_horizon_stations = BuildFarLodHorizonStations();
                        farlod_horizon_captures_written.assign(farlod_horizon_stations.size(), false);
                        farlod_horizon_far_off_sliver_px.assign(farlod_horizon_stations.size(), -1);
                        farlod_horizon_sky_enforced = !PlayerViewSeaWaterInNearField(gameSession.get());
                    }
                    const double elapsed_play_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - scenario_play_started_at).count();
                    const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 0.999);
                    if (auto* farlod = renderPipeline.farlod()) {
                        farlod->set_enabled(progress >= kFarLodHorizonPhaseSplit);
                    }
                    const std::size_t station_count = farlod_horizon_stations.size();
                    std::size_t station_index = 0;
                    if (progress >= kFarLodHorizonPhaseSplit) {
                        const double sweep =
                            (progress - kFarLodHorizonPhaseSplit) / (1.0 - kFarLodHorizonPhaseSplit);
                        const std::size_t time_based_index = std::min(
                            station_count - 1u,
                            static_cast<std::size_t>(sweep * static_cast<double>(station_count)));
                        // T-I4-DR-sliver-baseline-diff: on slow debug runs the
                        // time-based sweep outruns the expensive captures (each
                        // capture renders a paired far-OFF frame and analyzes two
                        // back buffers, ~1 fps), so wall-clock stations get skipped.
                        // Hold the camera on the first un-captured station so the
                        // sweep cannot advance past it - mirrors the player_view
                        // catch-up clamp above.
                        std::size_t first_unwritten = 0;
                        while (first_unwritten < farlod_horizon_captures_written.size() &&
                               farlod_horizon_captures_written[first_unwritten]) {
                            ++first_unwritten;
                        }
                        station_index = first_unwritten >= station_count
                                            ? time_based_index
                                            : std::min(time_based_index, first_unwritten);
                    }
                    farlod_horizon_applied_station = station_index;
                    ApplyFarLodHorizonCamera(gameSession.get(), g_camera.get(), farlod_horizon_stations[station_index]);
                } else if (scenario_config.skinned_mesh_visual_smoke() && scenario_ready && g_camera) {
                    // T-I3-16: spawn the rigged test mesh once, then hold the
                    // fixed framing for both captures.
                    if (!skinned_mesh_spawn_attempted) {
                        skinned_mesh_spawn_attempted = true;
                        skinned_mesh_visual_target = SpawnSkinnedMeshVisualEntity(
                            gameSession.get(), scenario_config.artifact_dir);
                    }
                    ApplySkinnedMeshVisualCamera(g_camera.get(), skinned_mesh_visual_target);
                } else if (scenario_config.creature_slice_smoke() && scenario_ready && g_camera) {
                    // T-I3-18: spawn the creature scene once, hold the fixed
                    // photographic framing, run the game glue every frame and
                    // bring in the light stimulus at 55% progress.
                    if (!creature_slice_spawn_attempted) {
                        creature_slice_spawn_attempted = true;
                        creature_slice_scene = SpawnCreatureSliceScene(
                            gameSession.get(), root_dir, scenario_config.creature_archetype);
                    }
                    const double elapsed_play_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - scenario_play_started_at).count();
                    const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                    if (progress >= 0.55 && creature_slice_scene.spawned && !creature_slice_scene.stimulus_spawned) {
                        SpawnCreatureSliceStimulus(gameSession.get(), creature_slice_scene);
                    }
                    UpdateCreatureSliceScene(gameSession.get(), creature_slice_scene, static_cast<double>(deltaTime));
                    ApplyCreatureSliceCamera(gameSession.get(), g_camera.get(), creature_slice_scene);
                } else if (scenario_config.networked_session_smoke() && scenario_ready && g_camera) {
                    // T-I4-14: the client renders a SERVER-OWNED world over the
                    // lockstep transport. The driver owns the host authority world
                    // + both LockstepSession ends; per agreed tick it steps BOTH
                    // worlds (the client world is THIS gameSession, stepped via the
                    // driver's apply_and_step hook from the spawn anchor) and
                    // exchanges hashes. The client's WORLD-AFFECTING input set
                    // (empty today) round-trips through LockstepSession::*Input.
                    if (!networked_session_begun) {
                        networked_session_begun = true;
                        NetworkedSessionDriver::Config net_cfg;
                        net_cfg.seed = 424242;
                        net_cfg.preset = scenario_world_type;
                        net_cfg.budget_ticks = 90;
                        net_cfg.hash_cadence_ticks = 30;
                        net_cfg.root_path = root_path_str;
                        net_cfg.surface_radius = scenario_config.horizon_radius;
                        net_cfg.collision_radius = scenario_config.collision_radius;
                        if (!networked_session_driver.Begin(gameSession.get(), net_cfg)) {
                            scenario_failed = true;
                            scenario_failure_reason =
                                "networked_session_begin_failed_" + networked_session_driver.failure_reason();
                        }
                    }
                    if (networked_session_begun && !networked_session_done && !scenario_failed) {
                        // Drive the lockstep session to COMPLETION here (bounded by
                        // the budget): each agreed tick quiesces both worlds' streaming
                        // jobs, which is expensive in a debug build, so spreading it
                        // across rendered frames would blow the run window. The world
                        // is server-owned and stepped through the driver's
                        // apply_and_step hook; the render frame BELOW then draws the
                        // settled server-owned world (proving the client is
                        // render-capable, unlike the headless server). One agreed tick
                        // is stepped before the first render so the loop is observable.
                        bool live = networked_session_driver.StepAgreedTick();
                        while (live) {
                            live = networked_session_driver.StepAgreedTick();
                        }
                        if (!live) {
                            networked_session_done = true;
                            networked_session_driver.Disconnect();
                            const double net_seconds = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - scenario_play_started_at).count();
                            const bool net_passed = networked_session_driver.WriteArtifact(
                                scenario_config.artifact_dir, net_seconds);
                            if (!net_passed) {
                                scenario_failed = true;
                                scenario_failure_reason =
                                    networked_session_driver.failure_reason().empty()
                                        ? std::string("networked_session_not_in_sync")
                                        : ("networked_session_" + networked_session_driver.failure_reason());
                            }
                            scenario_timed_run_complete = true;
                            glfwSetWindowShouldClose(window, true);
                        }
                    }
                    // Camera LOOK is RENDER-SIDE: a fixed eye-level framing applied
                    // locally each frame, NEVER round-tripped through the session, so
                    // look latency is zero (research worldgen-lockstep-sdfrt.md Area 2
                    // takeaway 2). It reads the spawn anchor the driver streams the
                    // world around, but does NOT influence the hashed world step.
                    {
                        const Luminumbra::Vec3 anchor = networked_session_driver.ClientStreamingAnchor();
                        g_camera->Position = glm::vec3(anchor.x, anchor.y + 1.8f, anchor.z);
                        g_camera->Yaw = 0.0f;
                        g_camera->Pitch = 0.0f;
                        g_camera->updateCameraVectors();
                    }
                } else if (g_playerController) {
                    g_playerController->Update(deltaTime);
                }
                if (auto* physics = gameSession->GetPhysicsSystem()) physics->update(deltaTime);
                // T-I3-4: fixed 30 Hz simulation tick (SimulationClock +
                // OrderedEventBus drain) hosted by GameSession. Render,
                // physics, and scenario paths above remain variable-dt.
                // T-I4-14: the networked-session scenario steps its client world
                // through the lockstep driver's apply_and_step hook (in lockstep
                // with the host), so the default per-frame tick + camera-anchored
                // streaming are SKIPPED here -- ticking twice would desync from the
                // host, and camera-anchored streaming would diverge the hashed world.
                if (!scenario_config.networked_session_smoke()) {
                gameSession->TickSimulation(static_cast<double>(deltaTime));
                if (gameSession->GetWorldSystem() && (g_playerController || g_camera)) {
                    const Luminumbra::Vec3 streaming_position =
                        ((scenario_config.lod_ground_smoke() || scenario_config.water_visual_smoke() || scenario_config.material_visual_smoke() || scenario_config.skybox_visual_smoke() || scenario_config.weather_visual_smoke() || scenario_config.cloud_shadow_smoke() || scenario_config.precipitation_smoke() || scenario_config.timeofday_sweep_smoke() || scenario_config.lod_boundary_oscillation_smoke() || scenario_config.lod_seam_arrival_smoke() || scenario_config.player_view_smoke() || scenario_config.farlod_horizon_smoke() || scenario_config.skinned_mesh_visual_smoke() || scenario_config.creature_slice_smoke()) && scenario_ready && g_camera)
                            ? Luminumbra::Vec3(g_camera->Position)
                            : (g_playerController ? Luminumbra::Vec3(g_playerController->GetPosition()) : Luminumbra::Vec3(g_camera->Position));
                    gameSession->GetWorldSystem()->update(
                        gameSession->GetRegistry(),
                        streaming_position,
                        gameSession->GetPhysicsSystem()
                    );
                }
                } // T-I4-14: end !networked_session_smoke default-tick guard
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
                    if (scenario_config.lod_ground_smoke() || scenario_config.water_visual_smoke() || scenario_config.material_visual_smoke() || scenario_config.skybox_visual_smoke() || scenario_config.weather_visual_smoke() || scenario_config.cloud_shadow_smoke() || scenario_config.precipitation_smoke() || scenario_config.lod_seam_arrival_smoke() || scenario_config.player_view_smoke() || scenario_config.farlod_horizon_smoke() || scenario_config.skinned_mesh_visual_smoke() || scenario_config.creature_slice_smoke()) {
                        // time_of_day 0 is noon (sun elevation = cos(2*pi*t));
                        // 0.04 keeps the sun near its zenith for stable captures.
                        renderPipeline.set_time_of_day(0.04f);
                    } else if (scenario_config.timeofday_sweep_smoke() && scenario_ready) {
                        // T-I5a-7 (C2): SEASON SWEEP. The run is split into two
                        // season halves (summer then winter); each half replays
                        // the noon/dusk/night phase windows. Both the time-of-day
                        // AND the tick-derived season are re-pinned every frame so
                        // update_time_of_day cannot drift either between settle
                        // frames. set_season_tick feeds the authoritative-tick-
                        // style integer the season is a PURE FUNCTION of (no
                        // wall-clock for the season itself).
                        const double elapsed_play_seconds = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - scenario_play_started_at).count();
                        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                        const double sweep_progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                        const SeasonSweepPoint season_point = SeasonSweepAt(sweep_progress);
                        renderPipeline.set_season_tick(season_point.season_tick);
                        renderPipeline.set_time_of_day(season_point.time_of_day);
                    }
                    renderPipeline.render_frame(gameSession->GetRegistry(), *gameSession->GetWorldSystem(), *g_camera, deltaTime, wireframe_mode);
                    if (scenario_config.active() && currentState == GameState::IN_GAME) {
                        ++scenario_frame_count;
                        const auto now = std::chrono::steady_clock::now();
                        // window_mode_stress_smoke (T-I4-DR-window-modes): drive
                        // one resize-chain step per frame after readiness. The
                        // window itself stays pinned/hidden (capture protection),
                        // but on_resize reallocates the non-pinned targets through
                        // exactly the runtime resize path. We measure the resize
                        // generation + GL errors AFTER this frame's render so the
                        // PREVIOUS step's new targets have been drawn into once.
                        if (scenario_config.window_mode_stress_smoke() && scenario_ready &&
                            !window_mode_stress_complete) {
                            if (window_mode_stress_steps.empty()) {
                                window_mode_stress_steps = BuildWindowModeStressSequence();
                            }
                            // Finalize the step applied on the previous frame
                            // (its targets were just rendered into this frame).
                            if (window_mode_stress_step_index > 0) {
                                WindowModeStressStep& done =
                                    window_mode_stress_steps[window_mode_stress_step_index - 1];
                                done.resize_generation_after = renderPipeline.resize_generation();
                                done.gl_errors_after = CurrentGLDebugRuntimeStats().errors;
                                done.targets_width_after = static_cast<int>(renderPipeline.screen_width());
                                done.targets_height_after = static_cast<int>(renderPipeline.screen_height());
                            }
                            if (window_mode_stress_step_index < window_mode_stress_steps.size()) {
                                WindowModeStressStep& step =
                                    window_mode_stress_steps[window_mode_stress_step_index];
                                step.resize_generation_before = renderPipeline.resize_generation();
                                step.size_changed =
                                    static_cast<int>(renderPipeline.screen_width()) != step.width ||
                                    static_cast<int>(renderPipeline.screen_height()) != step.height;
                                renderPipeline.on_resize(
                                    static_cast<unsigned int>(step.width),
                                    static_cast<unsigned int>(step.height));
                                ++window_mode_stress_step_index;
                            } else {
                                // All steps applied + finalized: capture the
                                // restored pinned-size frame (Smoke-equivalent).
                                // Targets are 1280x720; render one clean frame
                                // into the default framebuffer for the readback.
                                window_mode_stress_capture.width =
                                    static_cast<int>(renderPipeline.screen_width());
                                window_mode_stress_capture.height =
                                    static_cast<int>(renderPipeline.screen_height());
                                window_mode_stress_capture.file = "window-mode-stress-final.ppm";
                                ScreenshotPixelStats final_stats;
                                if (WriteBackbufferPpm(
                                        scenario_config.artifact_dir / window_mode_stress_capture.file,
                                        window_mode_stress_capture.width,
                                        window_mode_stress_capture.height,
                                        &final_stats)) {
                                    window_mode_stress_capture.pixels = final_stats;
                                }
                                window_mode_stress_complete = true;
                            }
                        }
                        if (lod_ground_frame_recorder.enabled()) {
                            lod_ground_frame_recorder.record_frame(deltaTime, gameSession.get(), renderPipeline, scenario_frame_count);
                        }
                        if (scenario_config.lod_boundary_oscillation_smoke() && scenario_ready) {
                            lod_boundary_transition_recorder.record_frame(gameSession->GetWorldSystem());
                        }
                        if (scenario_config.lod_seam_arrival_smoke() && scenario_ready) {
                            lod_seam_arrival_recorder.record_frame(gameSession->GetWorldSystem());
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const std::array<double, 4> thresholds{0.25, 0.50, 0.75, 0.95};
                            const std::array<const char*, 4> names{"p25", "p50", "p75", "p95"};
                            for (std::size_t i = 0; i < thresholds.size(); ++i) {
                                if (lod_seam_screenshots_written[i] || progress < thresholds[i]) {
                                    continue;
                                }
                                lod_seam_screenshots_written[i] = true;
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                const std::string relative_path = std::string("screenshots/lod-seam-") + names[i] + ".ppm";
                                LodHolePixelStats hole_stats;
                                if (WriteBackbufferPpm(scenario_config.artifact_dir / relative_path, screenshot_width, screenshot_height, nullptr, &hole_stats)) {
                                    lod_seam_visual_captures.push_back({
                                        names[i],
                                        relative_path,
                                        hole_stats
                                    });
                                    WriteLodSeamArrivalAnalysis(
                                        scenario_config.artifact_dir,
                                        elapsed_play_seconds,
                                        lod_seam_visual_captures,
                                        lod_seam_arrival_recorder);
                                }
                            }
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
                        if (scenario_config.water_visual_smoke() && scenario_ready && !water_reflection_capture_written) {
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            // T-I2-16a: sample the ROI water luminance roughly once a
                            // second while the top-down framing is active; the analysis
                            // requires temporal variance across these samples (animated
                            // caustics, not a static tint).
                            if (!water_visual_capture_written &&
                                render_pass_stats.water_draws > 0 && water_visual_target.found &&
                                elapsed_play_seconds >= water_caustics_next_sample_seconds) {
                                int sample_width = 0;
                                int sample_height = 0;
                                glfwGetFramebufferSize(window, &sample_width, &sample_height);
                                WaterCausticsSample caustics_sample =
                                    SampleBackbufferWaterLuminance(sample_width, sample_height, elapsed_play_seconds);
                                caustics_sample.texture_mean_abs_delta = SampleCausticsTextureDelta(
                                    renderPipeline.water_caustics_texture(), water_caustics_previous_texels);
                                water_caustics_samples.push_back(caustics_sample);
                                water_caustics_next_sample_seconds = elapsed_play_seconds + 1.0;
                            }
                            // Main capture (top-down framing) at 50% progress.
                            if (!water_visual_capture_written &&
                                progress >= 0.50 && progress < 0.60 &&
                                render_pass_stats.water_draws > 0 && water_visual_target.found &&
                                water_caustics_samples.size() >= 2) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());

                                    if (WritePixelBufferPpm(scenario_config.artifact_dir / "screenshots/water-visual.ppm", screenshot_width, screenshot_height, frame_pixels)) {
                                        water_visual_capture_written = true;
                                        water_visual_pixel_stats =
                                            AnalyzeScreenshotPixels(frame_pixels, screenshot_width, screenshot_height);

                                        // T-I2-16c: depth tint gradient + shoreline foam
                                        // probes around the projected shallow/deep points.
                                        constexpr int kWaterPatchRadius = 10;
                                        int patch_x = 0;
                                        int patch_y = 0;
                                        if (water_visual_target.shallow_point_found && g_camera &&
                                            project_world_to_capture(*g_camera, water_visual_target.shallow_point,
                                                                     screenshot_width, screenshot_height,
                                                                     kWaterPatchRadius, patch_x, patch_y)) {
                                            water_shallow_patch = AnalyzeWaterRegionPatch(
                                                frame_pixels, screenshot_width, screenshot_height,
                                                patch_x, patch_y, kWaterPatchRadius);
                                        }
                                        if (water_visual_target.deep_point_found && g_camera &&
                                            project_world_to_capture(*g_camera, water_visual_target.deep_point,
                                                                     screenshot_width, screenshot_height,
                                                                     kWaterPatchRadius, patch_x, patch_y)) {
                                            water_deep_patch = AnalyzeWaterRegionPatch(
                                                frame_pixels, screenshot_width, screenshot_height,
                                                patch_x, patch_y, kWaterPatchRadius);
                                        }
                                        // Wider patch for the foam band: the projected
                                        // point sits mid-band, the extra radius tolerates
                                        // the heightfield-vs-mesh shoreline offset.
                                        constexpr int kFoamPatchRadius = 16;
                                        if (water_visual_target.foam_point_found && g_camera &&
                                            project_world_to_capture(*g_camera, water_visual_target.foam_point,
                                                                     screenshot_width, screenshot_height,
                                                                     kFoamPatchRadius, patch_x, patch_y)) {
                                            water_foam_patch = AnalyzeWaterRegionPatch(
                                                frame_pixels, screenshot_width, screenshot_height,
                                                patch_x, patch_y, kFoamPatchRadius);
                                        }
                                    }
                                }
                            }
                            // Reflection capture (grazing open-water framing, T-I2-16b)
                            // at 85% progress; writes the combined analysis artifact.
                            if (water_visual_capture_written && progress >= 0.85 &&
                                render_pass_stats.water_draws > 0 && water_visual_target.found) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());

                                    // Correlate the reflective upper water band against the
                                    // same sky reference the water shader uses for SSR
                                    // misses. The scenario pins time_of_day at 0.04 (sun
                                    // near zenith), so the sun intensity is 1.0.
                                    const WaterReflectionStats reflection_stats = AnalyzeWaterReflection(
                                        frame_pixels,
                                        screenshot_width,
                                        screenshot_height,
                                        Luminumbra::Rendering::WaterPass::approximate_sky_reflection_color(1.0f));
                                    const std::string reflection_path = "screenshots/water-reflection.ppm";
                                    if (WritePixelBufferPpm(scenario_config.artifact_dir / reflection_path, screenshot_width, screenshot_height, frame_pixels)) {
                                        water_reflection_capture_written = true;
                                        WriteWaterVisualAnalysis(
                                            scenario_config.artifact_dir,
                                            "screenshots/water-visual.ppm",
                                            reflection_path,
                                            water_visual_target,
                                            water_visual_pixel_stats,
                                            render_pass_stats,
                                            renderPipeline.get_last_mesh_upload_stats(),
                                            water_caustics_samples,
                                            reflection_stats,
                                            water_shallow_patch,
                                            water_deep_patch,
                                            water_foam_patch
                                        );
                                    }
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
                        if (scenario_config.skybox_visual_smoke() && scenario_ready && !skybox_visual_capture_written) {
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            if (progress >= 0.50 && render_pass_stats.skybox_draws > 0) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());

                                    double sun_screen_x = 0.0;
                                    double sun_screen_y = 0.0;
                                    const bool sun_on_screen = ProjectDirectionToScreen(
                                        *g_camera, screenshot_width, screenshot_height,
                                        TowardSunDirection(0.04f), sun_screen_x, sun_screen_y);
                                    const SkyboxPixelStats skybox_stats = AnalyzeSkyboxPixels(
                                        frame_pixels, screenshot_width, screenshot_height,
                                        sun_screen_x, sun_screen_y, sun_on_screen);
                                    const std::string screenshot_path = "screenshots/skybox-visual.ppm";
                                    if (WritePixelBufferPpm(
                                            scenario_config.artifact_dir / screenshot_path,
                                            screenshot_width, screenshot_height, frame_pixels)) {
                                        skybox_visual_capture_written = true;
                                        WriteSkyboxVisualAnalysis(
                                            scenario_config.artifact_dir,
                                            screenshot_path,
                                            skybox_stats,
                                            sun_screen_x,
                                            sun_screen_y,
                                            sun_on_screen,
                                            render_pass_stats);
                                    }
                                }
                            }
                        }
                        if (scenario_config.weather_visual_smoke() && scenario_ready && !weather_visual_capture_written) {
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            const bool capture_baseline = !weather_baseline_capture_written && progress >= 0.35 && progress < 0.5;
                            const bool capture_weather = weather_baseline_capture_written && progress >= 0.85;
                            if ((capture_baseline || capture_weather) && render_pass_stats.skybox_draws > 0) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());
                                    const WeatherPixelStats stats = AnalyzeWeatherPixels(frame_pixels, screenshot_width, screenshot_height);
                                    if (capture_baseline) {
                                        const std::string baseline_path = "screenshots/weather-baseline.ppm";
                                        if (WritePixelBufferPpm(
                                                scenario_config.artifact_dir / baseline_path,
                                                screenshot_width, screenshot_height, frame_pixels)) {
                                            weather_baseline_capture_written = true;
                                            weather_baseline_stats = stats;
                                        }
                                    } else {
                                        const std::string weather_path = "screenshots/weather-visual.ppm";
                                        if (WritePixelBufferPpm(
                                                scenario_config.artifact_dir / weather_path,
                                                screenshot_width, screenshot_height, frame_pixels)) {
                                            weather_visual_capture_written = true;
                                            WriteWeatherVisualAnalysis(
                                                scenario_config.artifact_dir,
                                                "screenshots/weather-baseline.ppm",
                                                weather_path,
                                                weather_baseline_stats,
                                                stats,
                                                "rain",
                                                1.0f,
                                                render_pass_stats);
                                        }
                                    }
                                }
                            }
                        }
                        if (scenario_config.cloud_shadow_smoke() && scenario_ready && !cloud_shadow_done) {
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            const auto& cloud_state = renderPipeline.get_cloud_state();
                            // Sample the clouds-OFF lighting GPU baseline during the
                            // shadow-off warmup window (the camera branch keeps the
                            // cast shadow off until progress >= 0.15).
                            if (!cloud_shadow_lighting_off_sampled &&
                                progress >= 0.10 && progress < 0.15 &&
                                render_pass_stats.lighting_draws > 0 &&
                                render_pass_stats.lighting_gpu_ms > 0.0) {
                                cloud_shadow_lighting_ms_off = render_pass_stats.lighting_gpu_ms;
                                cloud_shadow_lighting_off_sampled = true;
                            }
                            // Two terrain-ROI captures with the cast shadow ON, far
                            // enough apart that the wind has drifted a shadow edge
                            // across the fixed ROI (t0 ~45%, t1 ~92%).
                            const bool capture_t0 = !cloud_shadow_t0_written && progress >= 0.45 && progress < 0.55;
                            const bool capture_t1 = cloud_shadow_t0_written && progress >= 0.90;
                            if ((capture_t0 || capture_t1) && render_pass_stats.skybox_draws > 0) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());
                                    const Luminumbra::Client::ScenarioHarness::CloudShadowPixelStats stats =
                                        Luminumbra::Client::ScenarioHarness::AnalyzeCloudShadowPixels(
                                            frame_pixels, screenshot_width, screenshot_height);
                                    if (capture_t0) {
                                        const std::string t0_path = "screenshots/cloud-shadow-t0.ppm";
                                        if (WritePixelBufferPpm(
                                                scenario_config.artifact_dir / t0_path,
                                                screenshot_width, screenshot_height, frame_pixels)) {
                                            cloud_shadow_t0_written = true;
                                            cloud_shadow_terrain_luma_t0 = stats.terrain_roi_mean_luminance;
                                            cloud_shadow_scroll_t0 = static_cast<double>(cloud_state.scroll_offset.x);
                                        }
                                    } else {
                                        const std::string t1_path = "screenshots/cloud-shadow-t1.ppm";
                                        if (WritePixelBufferPpm(
                                                scenario_config.artifact_dir / t1_path,
                                                screenshot_width, screenshot_height, frame_pixels)) {
                                            Luminumbra::Client::ScenarioHarness::CloudShadowResult result;
                                            result.terrain_roi_luminance_t0 = cloud_shadow_terrain_luma_t0;
                                            result.terrain_roi_luminance_t1 = stats.terrain_roi_mean_luminance;
                                            result.terrain_roi_luminance_delta =
                                                std::abs(result.terrain_roi_luminance_t1 - result.terrain_roi_luminance_t0);
                                            result.sky_mean_luminance = stats.sky_mean_luminance;
                                            result.sky_horizontal_gradient_mean = stats.sky_horizontal_gradient_mean;
                                            result.cloud_layer_present = stats.sky_horizontal_gradient_mean > 0.0;
                                            result.lighting_gpu_ms_clouds_off = cloud_shadow_lighting_ms_off;
                                            result.lighting_gpu_ms_clouds_on = render_pass_stats.cloud_shadow_gpu_ms;
                                            result.cloud_shadow_added_ms = std::max(
                                                0.0, result.lighting_gpu_ms_clouds_on - result.lighting_gpu_ms_clouds_off);
                                            result.gpu_timers_supported =
                                                render_pass_stats.gpu_timers_supported &&
                                                cloud_shadow_lighting_off_sampled &&
                                                render_pass_stats.cloud_shadow_gpu_ms > 0.0;
                                            result.coverage_amount = cloud_state.coverage_amount;
                                            result.shadow_strength = cloud_state.shadow_strength;
                                            result.scroll_offset_t0 = cloud_shadow_scroll_t0;
                                            result.scroll_offset_t1 = static_cast<double>(cloud_state.scroll_offset.x);
                                            Luminumbra::Client::ScenarioHarness::WriteCloudShadowAnalysis(
                                                scenario_config.artifact_dir,
                                                "screenshots/cloud-shadow-t0.ppm",
                                                t1_path,
                                                t1_path,
                                                result,
                                                render_pass_stats);
                                            cloud_shadow_done = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (scenario_config.particle_emitter_determinism_smoke() && scenario_ready && !particle_determinism_capture_written) {
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            // Capture once particles are visibly rendering (the
                            // pass reports draws) and late enough that the GPU
                            // timer ring has resolved a real sample.
                            if (progress >= 0.5 && render_pass_stats.particle_draws > 0) {
                                auto* particles = renderPipeline.particles();
                                if (particles != nullptr) {
                                    // --- DETERMINISM ASSERTION (critique F2) ---
                                    // Rebuild the sim-deterministic emitter
                                    // DESCRIPTOR SET from identical world state
                                    // twice; the descriptor bytes must be
                                    // byte-equal across runs. Per-particle motion
                                    // is render-only and is NOT snapshotted here.
                                    std::uint64_t world_seed = 0;
                                    if (auto* ws = gameSession->GetWorldSystem()) {
                                        world_seed = static_cast<std::uint64_t>(
                                            static_cast<std::uint32_t>(ws->get_seed()));
                                    }
                                    const std::uint64_t world_tick =
                                        gameSession->GetSimulationTickCount();

                                    particles->rebuild_emitter_descriptors(world_seed, world_tick);
                                    const auto descriptors_a = particles->emitter_descriptors();
                                    const std::uint64_t hash_a = particles->emitter_descriptor_hash();

                                    particles->rebuild_emitter_descriptors(world_seed, world_tick);
                                    const auto descriptors_b = particles->emitter_descriptors();
                                    const std::uint64_t hash_b = particles->emitter_descriptor_hash();

                                    const bool byte_equal =
                                        descriptors_a.size() == descriptors_b.size() &&
                                        std::memcmp(descriptors_a.data(), descriptors_b.data(),
                                                    descriptors_a.size() * sizeof(Luminumbra::Rendering::ParticlePass::EmitterDescriptor)) == 0;

                                    Luminumbra::Client::ScenarioHarness::ParticleDeterminismResult result;
                                    result.world_seed = world_seed;
                                    result.world_tick = world_tick;
                                    result.descriptor_hash_run_a = hash_a;
                                    result.descriptor_hash_run_b = hash_b;
                                    result.descriptor_count = descriptors_a.size();
                                    result.byte_equal = byte_equal && hash_a == hash_b;
                                    result.particle_pass_gpu_ms = render_pass_stats.particle_gpu_ms;
                                    result.particles_drawn = render_pass_stats.particles_drawn;

                                    int screenshot_width = 0;
                                    int screenshot_height = 0;
                                    glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                    if (screenshot_width > 0 && screenshot_height > 0) {
                                        std::vector<unsigned char> frame_pixels(
                                            static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                        glReadBuffer(GL_BACK);
                                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                        glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());
                                        const std::string screenshot_path = "screenshots/particle-determinism.ppm";
                                        if (WritePixelBufferPpm(
                                                scenario_config.artifact_dir / screenshot_path,
                                                screenshot_width, screenshot_height, frame_pixels)) {
                                            particle_determinism_capture_written = true;
                                            WriteParticleEmitterDeterminismAnalysis(
                                                scenario_config.artifact_dir,
                                                screenshot_path,
                                                result,
                                                render_pass_stats);
                                        }
                                    }
                                }
                            }
                        }
                        if (scenario_config.precipitation_smoke() && scenario_ready && !precip_windy_capture_written) {
                            // T-I5a-4 (B2): capture a CALM rain frame (first half,
                            // zero wind -> vertical fall) and a WINDY rain frame
                            // (second half, wind-advected slant). The analysis on
                            // the windy capture asserts precip particles are present
                            // in both AND that the streaks slant with wind.
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            // Capture once rain is visibly rendering (the pass
                            // reports draws). Calm: late in the first half so the
                            // field has filled and settled to a steady vertical
                            // fall. Windy: late in the second half so the slant has
                            // fully developed after the wind switch at progress 0.5.
                            const bool capture_calm = !precip_calm_capture_written &&
                                progress >= 0.35 && progress < 0.5 && render_pass_stats.particle_draws > 0;
                            const bool capture_windy = precip_calm_capture_written &&
                                progress >= 0.9 && render_pass_stats.particle_draws > 0;
                            if (capture_calm || capture_windy) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());
                                    const PrecipPixelStats stats = AnalyzePrecipPixels(frame_pixels, screenshot_width, screenshot_height);
                                    if (capture_calm) {
                                        const std::string calm_path = "screenshots/precip-calm.ppm";
                                        if (WritePixelBufferPpm(
                                                scenario_config.artifact_dir / calm_path,
                                                screenshot_width, screenshot_height, frame_pixels)) {
                                            precip_calm_capture_written = true;
                                            precip_calm_stats = stats;
                                            precip_calm_render_pass = render_pass_stats;
                                        }
                                    } else {
                                        const std::string windy_path = "screenshots/precip-windy.ppm";
                                        if (WritePixelBufferPpm(
                                                scenario_config.artifact_dir / windy_path,
                                                screenshot_width, screenshot_height, frame_pixels)) {
                                            precip_windy_capture_written = true;
                                            WritePrecipitationAnalysis(
                                                scenario_config.artifact_dir,
                                                "screenshots/precip-calm.ppm",
                                                windy_path,
                                                precip_calm_stats,
                                                stats,
                                                "rain",
                                                0.0,
                                                16.0,
                                                precip_calm_render_pass,
                                                render_pass_stats);
                                        }
                                    }
                                }
                            }
                        }
                        if (scenario_config.timeofday_sweep_smoke() && scenario_ready && !timeofday_analysis_final) {
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            // T-I5a-7 (C2): SEASON SWEEP capture. SIX windows = two
                            // season halves (summer then winter), each replaying
                            // noon/dusk/night. Captures land late in each window so
                            // the pinned time-of-day + tick-derived season have
                            // settle frames. Summer (season 0) OWNS the canonical
                            // timeofday-{noon,dusk,night}.ppm files + the existing
                            // ordering/hue-band/emissive assertions (unchanged);
                            // winter (season 1) adds the per-season comparison set.
                            struct SeasonCapturePlan {
                                double threshold;       // progress at which to grab it
                                const char* phase_name; // noon/dusk/night
                                int phase_index;        // 0/1/2
                                double phase_time;      // pinned time-of-day
                                const char* season_label;
                                int season_index;       // 0 summer, 1 winter
                                const char* file;       // relative screenshot path
                            };
                            static const std::array<SeasonCapturePlan, 6> kPlans{{
                                {0.13, "noon",  0, 0.04, "summer", 0, "screenshots/timeofday-noon.ppm"},
                                {0.28, "dusk",  1, 0.22, "summer", 0, "screenshots/timeofday-dusk.ppm"},
                                {0.42, "night", 2, 0.45, "summer", 0, "screenshots/timeofday-night.ppm"},
                                {0.63, "noon",  0, 0.04, "winter", 1, "screenshots/timeofday-winter-noon.ppm"},
                                {0.78, "dusk",  1, 0.22, "winter", 1, "screenshots/timeofday-winter-dusk.ppm"},
                                {0.92, "night", 2, 0.45, "winter", 1, "screenshots/timeofday-winter-night.ppm"},
                            }};
                            int capture_index = -1;
                            for (int i = 0; i < 6; ++i) {
                                if (!timeofday_season_captures_written[static_cast<std::size_t>(i)] &&
                                    progress >= kPlans[static_cast<std::size_t>(i)].threshold) {
                                    capture_index = i;
                                    break;
                                }
                            }
                            // The emissive capture follows the SUMMER night (season 0,
                            // index 2) so the existing emissive night check is unchanged;
                            // it fires once that night is in and the camera has settled.
                            const bool capture_emissive =
                                timeofday_season_captures_written[2] &&
                                timeofday_emissive_target.found &&
                                !timeofday_emissive_capture_written &&
                                progress >= 0.45 && progress < 0.5;
                            if ((capture_index >= 0 || capture_emissive) && render_pass_stats.skybox_draws > 0) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());
                                    const TimeOfDayPixelStats stats = AnalyzeTimeOfDayPixels(frame_pixels, screenshot_width, screenshot_height);
                                    if (capture_index >= 0) {
                                        const SeasonCapturePlan& plan = kPlans[static_cast<std::size_t>(capture_index)];
                                        if (WritePixelBufferPpm(
                                                scenario_config.artifact_dir / plan.file,
                                                screenshot_width, screenshot_height, frame_pixels)) {
                                            timeofday_season_captures_written[static_cast<std::size_t>(capture_index)] = true;
                                            TimeOfDayPhaseCapture cap;
                                            cap.name = plan.phase_name;
                                            cap.time_of_day = plan.phase_time;
                                            cap.file = plan.file;
                                            cap.stats = stats;
                                            cap.season_label = plan.season_label;
                                            cap.season_index = plan.season_index;
                                            cap.season_phase = renderPipeline.get_season_phase();
                                            cap.sun_elevation_rad = renderPipeline.get_sun_elevation_rad();
                                            cap.season_sun_declination_rad = renderPipeline.get_season_sun_declination();
                                            cap.season_tick = renderPipeline.get_season_tick();
                                            timeofday_phase_captures.push_back(cap);
                                        }
                                    } else {
                                        const std::string emissive_path = "screenshots/timeofday-night-emissive.ppm";
                                        if (WritePixelBufferPpm(
                                                scenario_config.artifact_dir / emissive_path,
                                                screenshot_width, screenshot_height, frame_pixels)) {
                                            timeofday_emissive_capture_written = true;
                                            timeofday_emissive_stats = stats;
                                        }
                                    }
                                    const bool all_six =
                                        timeofday_season_captures_written[0] && timeofday_season_captures_written[1] &&
                                        timeofday_season_captures_written[2] && timeofday_season_captures_written[3] &&
                                        timeofday_season_captures_written[4] && timeofday_season_captures_written[5];
                                    if (all_six) {
                                        // Final once the optional emissive capture is in
                                        // (or no surface emissive target exists).
                                        timeofday_analysis_final =
                                            !timeofday_emissive_target.found || timeofday_emissive_capture_written;
                                        WriteTimeOfDaySweepAnalysis(
                                            scenario_config.artifact_dir,
                                            timeofday_phase_captures,
                                            timeofday_emissive_target,
                                            timeofday_emissive_capture_written,
                                            "screenshots/timeofday-night-emissive.ppm",
                                            timeofday_emissive_stats,
                                            render_pass_stats);
                                    }
                                }
                            }
                        }
                        if (scenario_config.player_view_smoke() && scenario_ready && !player_view_stations.empty() && g_camera) {
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            // Mirror the camera branch's warmup-adjusted schedule.
                            const double warmup_seconds = std::min(8.0, duration * 0.2);
                            const double effective_seconds = std::max(0.0, elapsed_play_seconds - warmup_seconds);
                            const double progress = std::clamp(
                                effective_seconds / std::max(1.0, duration - warmup_seconds), 0.0, 0.999);
                            const std::size_t station_count = player_view_stations.size();
                            const std::size_t wall_clock_index = std::min(
                                station_count - 1u,
                                static_cast<std::size_t>(progress * static_cast<double>(station_count)));
                            // Hitch tolerance: the sweep may not advance past the
                            // first un-captured station — a frame hitch that jumps
                            // a whole window otherwise orphans that station's
                            // capture (observed: yaw_030 skipped on heavier
                            // generation). A passed-over station captures
                            // immediately (progress treated as 1.0 = max settle).
                            std::size_t first_unwritten = 0;
                            while (first_unwritten < player_view_captures_written.size() &&
                                   player_view_captures_written[first_unwritten]) {
                                ++first_unwritten;
                            }
                            const std::size_t station_index =
                                first_unwritten >= station_count
                                    ? wall_clock_index
                                    : std::min(wall_clock_index, first_unwritten);
                            const double station_progress =
                                wall_clock_index > station_index
                                    ? 1.0
                                    : progress * static_cast<double>(station_count) - static_cast<double>(station_index);
                            if (!player_view_captures_written[station_index] && station_progress >= 0.7) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());

                                    // Horizon row: project the camera's horizontal
                                    // forward direction (the eye-level horizon) into
                                    // the frame; everything below it must be geometry.
                                    glm::vec3 horizontal_forward = g_camera->Front;
                                    horizontal_forward.y = 0.0f;
                                    int horizon_row_from_top = screenshot_height / 2;
                                    if (glm::dot(horizontal_forward, horizontal_forward) > 1.0e-6f) {
                                        horizontal_forward = glm::normalize(horizontal_forward);
                                        double horizon_x_norm = 0.0;
                                        double horizon_y_norm = 0.0;
                                        if (ProjectDirectionToScreen(*g_camera, screenshot_width, screenshot_height,
                                                                     horizontal_forward, horizon_x_norm, horizon_y_norm)) {
                                            horizon_row_from_top = static_cast<int>(horizon_y_norm * screenshot_height);
                                        } else {
                                            // Horizon outside the frame: pitched far up
                                            // (all sky legitimate, ROI empty) or far down
                                            // (all terrain, full-frame ROI).
                                            horizon_row_from_top = g_camera->Pitch > 0.0f ? screenshot_height : 0;
                                        }
                                    }

                                    PlayerViewStationCapture capture;
                                    capture.station = player_view_stations[station_index];
                                    capture.station.yaw_degrees = g_camera->Yaw;
                                    capture.station.pitch_degrees = g_camera->Pitch;
                                    capture.sky = AnalyzePlayerViewPixels(
                                        frame_pixels, screenshot_width, screenshot_height, horizon_row_from_top);
                                    capture.holes = AnalyzeLodHolePixels(frame_pixels, screenshot_width, screenshot_height);
                                    capture.coverage = gameSession->GetWorldSystem()->get_frustum_surface_coverage_stats(
                                        g_camera->Position,
                                        ExtractCameraFrustumPlanes(*g_camera, screenshot_width, screenshot_height),
                                        192.0f);

                                    const std::string relative_path =
                                        "screenshots/player-view-" + player_view_stations[station_index].name + ".ppm";
                                    if (WritePixelBufferPpm(scenario_config.artifact_dir / relative_path,
                                                            screenshot_width, screenshot_height, frame_pixels)) {
                                        capture.file = relative_path;
                                        player_view_station_captures.push_back(capture);
                                        player_view_captures_written[station_index] = true;
                                        WritePlayerViewAnalysis(
                                            scenario_config.artifact_dir,
                                            scenario_world_type,
                                            elapsed_play_seconds,
                                            player_view_station_captures,
                                            station_count,
                                            gameSession->GetWorldSystem()->get_runtime_chunk_stats(),
                                            player_view_sky_enforced);
                                    }
                                }
                            }
                        }
                        if (scenario_config.farlod_horizon_smoke() && scenario_ready && !farlod_horizon_stations.empty() && g_camera) {
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 0.999);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();

                            // gbuffer GPU samples: phase-A tail = far-disabled
                            // baseline; run tail = far-enabled comparison.
                            if (render_pass_stats.gpu_timers_supported && render_pass_stats.gbuffer_gpu_ms > 0.0) {
                                if (progress >= 0.15 && progress < kFarLodHorizonPhaseSplit) {
                                    farlod_baseline_gbuffer_samples.push_back(render_pass_stats.gbuffer_gpu_ms);
                                } else if (progress >= 0.85) {
                                    farlod_far_gbuffer_samples.push_back(render_pass_stats.gbuffer_gpu_ms);
                                }
                            }

                            if (progress >= kFarLodHorizonPhaseSplit) {
                                const double sweep =
                                    (progress - kFarLodHorizonPhaseSplit) / (1.0 - kFarLodHorizonPhaseSplit);
                                const std::size_t station_count = farlod_horizon_stations.size();
                                const std::size_t time_station_index = std::min(
                                    station_count - 1u,
                                    static_cast<std::size_t>(sweep * static_cast<double>(station_count)));
                                // T-I4-DR-sliver-baseline-diff: capture the station the
                                // rendered back buffer actually shows (the camera-apply
                                // clamp), not the bare time index. station_progress is
                                // only meaningful when station_index == time_station_index.
                                const std::size_t station_index = farlod_horizon_applied_station;
                                const double station_progress =
                                    sweep * static_cast<double>(station_count) - static_cast<double>(station_index);
                                // T-I4-DR-sliver-baseline-diff: when behind schedule the
                                // settle wait is skipped - the stations are yaw rotations
                                // of an already-settled world (and at ~1 fps a full second
                                // of simulation precedes each frame), so capture
                                // immediately to catch up; when on schedule, keep the 0.7
                                // settle gate.
                                const bool behind_schedule = station_index < time_station_index;
                                if (station_index < farlod_horizon_captures_written.size() &&
                                    !farlod_horizon_captures_written[station_index] &&
                                    (behind_schedule || station_progress >= 0.7)) {
                                    int screenshot_width = 0;
                                    int screenshot_height = 0;
                                    glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                    if (screenshot_width > 0 && screenshot_height > 0) {
                                        std::vector<unsigned char> frame_pixels(
                                            static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                        glReadBuffer(GL_BACK);
                                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                        glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());

                                        // Eye-level horizon row (same construction
                                        // as the player-view gate).
                                        glm::vec3 horizontal_forward = g_camera->Front;
                                        horizontal_forward.y = 0.0f;
                                        int horizon_row_from_top = screenshot_height / 2;
                                        if (glm::dot(horizontal_forward, horizontal_forward) > 1.0e-6f) {
                                            horizontal_forward = glm::normalize(horizontal_forward);
                                            double horizon_x_norm = 0.0;
                                            double horizon_y_norm = 0.0;
                                            if (ProjectDirectionToScreen(*g_camera, screenshot_width, screenshot_height,
                                                                         horizontal_forward, horizon_x_norm, horizon_y_norm)) {
                                                horizon_row_from_top = static_cast<int>(horizon_y_norm * screenshot_height);
                                            } else {
                                                horizon_row_from_top = g_camera->Pitch > 0.0f ? screenshot_height : 0;
                                            }
                                        }

                                        FarLodHorizonStationCapture capture;
                                        capture.station = farlod_horizon_stations[station_index];
                                        capture.sky = AnalyzePlayerViewPixels(
                                            frame_pixels, screenshot_width, screenshot_height, horizon_row_from_top);
                                        int band_top = 0;
                                        int band_bottom = 0;
                                        if (ComputeFarLodBoundaryBandRows(
                                                gameSession.get(), *g_camera, screenshot_width, screenshot_height,
                                                128.0f, 384.0f, horizon_row_from_top, band_top, band_bottom)) {
                                            capture.boundary = AnalyzeFarLodBoundaryBand(
                                                frame_pixels, screenshot_width, screenshot_height, band_top, band_bottom);
                                            // T-I4-DR-far-water-sheet: far water coverage in the band.
                                            std::uint64_t band_water_px = 0;
                                            std::uint64_t band_total_px = 0;
                                            AnalyzeFarLodBoundaryBandWater(
                                                frame_pixels, screenshot_width, screenshot_height, band_top, band_bottom,
                                                band_water_px, band_total_px);
                                            capture.boundary_band_water_pixels = band_water_px;
                                            capture.boundary_band_water_ratio =
                                                band_total_px > 0 ? static_cast<double>(band_water_px) /
                                                                        static_cast<double>(band_total_px)
                                                                  : 0.0;
                                        }
                                        // T-I4-DR-river-seam-sliver: above-horizon thin-sliver scan (far ON).
                                        capture.sky_sliver = AnalyzeFarLodHorizonSkySliver(
                                            frame_pixels, screenshot_width, screenshot_height, horizon_row_from_top);
                                        // T-I4-DR-sliver-baseline-diff: snapshot the far-LOD
                                        // scheduler stats for the ON frame BEFORE the paired
                                        // far-OFF render below - that render updates
                                        // farlod->stats() to the disabled frame (no wanted/
                                        // resident regions, zero draws), which is not the
                                        // state this capture asserts on.
                                        if (const auto* farlod = renderPipeline.farlod()) {
                                            const auto& farlod_stats = farlod->stats();
                                            capture.regions_wanted = farlod_stats.regions_wanted;
                                            capture.regions_resident = farlod_stats.regions_resident;
                                            capture.regions_missing = farlod_stats.regions_missing;
                                            capture.resident_bytes = farlod_stats.resident_bytes;
                                            capture.region_draws = farlod_stats.region_draws;
                                            capture.far_indices_drawn = farlod_stats.indices_drawn;
                                            // T-I4-DR-far-water-sheet: far water sheet draw counts.
                                            capture.water_sheet_draws = farlod_stats.water_sheet_draws;
                                            capture.water_sheet_indices = farlod_stats.water_sheet_indices;
                                        }
                                        // T-I4-DR-sliver-baseline-diff: PAIRED far-OFF
                                        // baseline at the EXACT same camera/frame. The
                                        // current back buffer was rendered far-ON; render
                                        // one more frame with far-LOD disabled, read its
                                        // pixels, then restore far-ON. Both buffers are
                                        // pixel-aligned (identical view, identical live
                                        // geometry), so the far-attributable sliver is the
                                        // far-ON frame re-analyzed with the far-OFF frame's
                                        // intrusion pixels cancelled PER PIXEL within a 3x3
                                        // neighborhood: pixel-aligned LIVE peak/ridge
                                        // silhouettes and diagonal live-geometry slivers
                                        // drop out and only a genuine far-render streak
                                        // (present only with far on) survives. (A scalar
                                        // on-minus-off max diff cannot do this: a detached
                                        // live streak and the legitimate far-LOD horizon
                                        // silhouette fuse into one far-ON-only span.)
                                        int far_off_sliver_px = -1;
                                        if (auto* farlod = renderPipeline.farlod()) {
                                            const bool was_enabled = farlod->enabled();
                                            farlod->set_enabled(false);
                                            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                                            renderPipeline.render_frame(
                                                gameSession->GetRegistry(),
                                                *gameSession->GetWorldSystem(), *g_camera, deltaTime, wireframe_mode);
                                            std::vector<unsigned char> off_pixels(
                                                static_cast<std::size_t>(screenshot_width) *
                                                static_cast<std::size_t>(screenshot_height) * 3u);
                                            glReadBuffer(GL_BACK);
                                            glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                            glReadPixels(0, 0, screenshot_width, screenshot_height,
                                                         GL_RGB, GL_UNSIGNED_BYTE, off_pixels.data());
                                            const auto off_sliver = AnalyzeFarLodHorizonSkySliver(
                                                off_pixels, screenshot_width, screenshot_height, horizon_row_from_top);
                                            far_off_sliver_px = off_sliver.tallest_sliver_px;
                                            farlod->set_enabled(was_enabled);
                                            // Masked far-attributable analysis of the ON
                                            // frame, cancelling the paired far-OFF intrusion
                                            // pixels in a 3x3 neighborhood.
                                            const auto attributable_sliver = AnalyzeFarLodHorizonSkySliver(
                                                frame_pixels, screenshot_width, screenshot_height,
                                                horizon_row_from_top, &off_pixels);
                                            capture.far_attributable_sliver_px =
                                                attributable_sliver.tallest_sliver_px;
                                        } else {
                                            // No far-OFF sample available: fall back to the
                                            // raw far-ON sliver (conservative - never under-
                                            // reports an attributable streak).
                                            capture.far_attributable_sliver_px =
                                                capture.sky_sliver.tallest_sliver_px;
                                        }
                                        if (station_index < farlod_horizon_far_off_sliver_px.size()) {
                                            farlod_horizon_far_off_sliver_px[station_index] = far_off_sliver_px;
                                        }
                                        capture.far_off_sliver_px = far_off_sliver_px;

                                        const std::string relative_path =
                                            "screenshots/farlod-horizon-" + farlod_horizon_stations[station_index].name + ".ppm";
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir / relative_path,
                                                                screenshot_width, screenshot_height, frame_pixels)) {
                                            capture.file = relative_path;
                                            farlod_horizon_station_captures.push_back(capture);
                                            farlod_horizon_captures_written[station_index] = true;
                                            WriteFarLodHorizonAnalysis(
                                                scenario_config.artifact_dir,
                                                scenario_world_type,
                                                elapsed_play_seconds,
                                                farlod_horizon_station_captures,
                                                farlod_horizon_stations.size(),
                                                median_of(farlod_baseline_gbuffer_samples),
                                                median_of(farlod_far_gbuffer_samples),
                                                render_pass_stats.gpu_timers_supported,
                                                farlod_horizon_sky_enforced);
                                        }
                                    }
                                }
                            }
                        }
                        if (scenario_config.skinned_mesh_visual_smoke() && scenario_ready &&
                            !skinned_mesh_analysis_written && skinned_mesh_visual_target.spawned) {
                            // T-I3-16: capture A at 50% progress, capture B at
                            // 85%; the two clip times must differ visibly in
                            // the rig ROI.
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            const bool want_capture_a = !skinned_mesh_capture_a_written && progress >= 0.50;
                            const bool want_capture_b = skinned_mesh_capture_a_written && progress >= 0.85;
                            if (want_capture_a || want_capture_b) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());

                                    SkinnedMeshVisualCapture capture;
                                    capture.elapsed_seconds = elapsed_play_seconds;
                                    capture.animation_time_seconds =
                                        SkinnedMeshVisualAnimationTime(gameSession.get(), skinned_mesh_visual_target);
                                    capture.skinned_draws = render_pass_stats.skinned_draws;
                                    capture.skinned_indices_drawn = render_pass_stats.skinned_indices_drawn;
                                    const std::string relative_path = want_capture_a
                                        ? "screenshots/skinned-mesh-a.ppm"
                                        : "screenshots/skinned-mesh-b.ppm";
                                    if (WritePixelBufferPpm(scenario_config.artifact_dir / relative_path,
                                                            screenshot_width, screenshot_height, frame_pixels)) {
                                        capture.file = relative_path;
                                        if (want_capture_a) {
                                            skinned_mesh_capture_a = capture;
                                            skinned_mesh_pixels_a = std::move(frame_pixels);
                                            skinned_mesh_capture_a_written = true;
                                        } else {
                                            const SkinnedMeshDiffStats diff = AnalyzeSkinnedMeshCaptures(
                                                skinned_mesh_pixels_a, frame_pixels,
                                                screenshot_width, screenshot_height);
                                            WriteSkinnedMeshVisualAnalysis(
                                                scenario_config.artifact_dir,
                                                skinned_mesh_visual_target,
                                                skinned_mesh_capture_a,
                                                capture,
                                                diff);
                                            skinned_mesh_analysis_written = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (scenario_config.creature_slice_smoke() && scenario_ready &&
                            !creature_slice_analysis_written && creature_slice_scene.spawned) {
                            // T-I3-18: planner state + screenshot before the
                            // stimulus (45%) and after it (85%).
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            const bool want_before = !creature_slice_before_written && progress >= 0.45 && progress < 0.55;
                            const bool want_after = creature_slice_before_written &&
                                creature_slice_scene.stimulus_spawned && progress >= 0.85;
                            if (want_before || want_after) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    CreatureSliceCapture capture;
                                    capture.elapsed_seconds = elapsed_play_seconds;
                                    capture.plan = ProbeCreatureSlicePlan(gameSession.get(), creature_slice_scene);
                                    capture.skinned_draws = render_pass_stats.skinned_draws;
                                    capture.skinned_indices_drawn = render_pass_stats.skinned_indices_drawn;
                                    // T-I3-22 composition: read the backbuffer,
                                    // project the (live) creature position to
                                    // screen, and measure sky_ratio + the
                                    // creature-vs-terrain color delta so a
                                    // visually broken-but-functional frame fails.
                                    {
                                        std::vector<unsigned char> comp_pixels(
                                            static_cast<std::size_t>(screenshot_width) *
                                            static_cast<std::size_t>(screenshot_height) * 3u);
                                        glReadBuffer(GL_BACK);
                                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                        glReadPixels(0, 0, screenshot_width, screenshot_height,
                                                     GL_RGB, GL_UNSIGNED_BYTE, comp_pixels.data());
                                        Luminumbra::Vec3 creature_world = creature_slice_scene.creature_position;
                                        if (gameSession->GetRegistry().valid(creature_slice_scene.creature)) {
                                            if (const auto* tf = gameSession->GetRegistry().try_get<
                                                    const Luminumbra::Components::TransformComponent>(
                                                        creature_slice_scene.creature)) {
                                                creature_world = tf->position;
                                            }
                                        }
                                        const glm::mat4 view = g_camera->GetViewMatrix();
                                        const glm::mat4 proj = g_camera->GetProjectionMatrix(
                                            screenshot_width, screenshot_height);
                                        const glm::vec4 clip = proj * view *
                                            glm::vec4(creature_world.x,
                                                      creature_world.y + 1.0f, // body center, above feet
                                                      creature_world.z, 1.0f);
                                        int sx = -1, sy = -1;
                                        if (clip.w > 0.0f) {
                                            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                                            sx = static_cast<int>((ndc.x * 0.5f + 0.5f) *
                                                                  static_cast<float>(screenshot_width));
                                            sy = static_cast<int>((1.0f - (ndc.y * 0.5f + 0.5f)) *
                                                                  static_cast<float>(screenshot_height));
                                        }
                                        // T-I4-9: project the emissive glow_bloom stimulus prop too so
                                        // the glow-halo (bright core -> falloff ring) can be measured.
                                        int stim_x = -1, stim_y = -1;
                                        if (creature_slice_scene.stimulus_spawned) {
                                            const Luminumbra::Vec3 sp = creature_slice_scene.stimulus_position;
                                            const glm::vec4 sclip = proj * view *
                                                glm::vec4(sp.x, sp.y + 0.5f, sp.z, 1.0f);
                                            if (sclip.w > 0.0f) {
                                                const glm::vec3 sndc = glm::vec3(sclip) / sclip.w;
                                                stim_x = static_cast<int>((sndc.x * 0.5f + 0.5f) *
                                                                          static_cast<float>(screenshot_width));
                                                stim_y = static_cast<int>((1.0f - (sndc.y * 0.5f + 0.5f)) *
                                                                          static_cast<float>(screenshot_height));
                                            }
                                        }
                                        capture.composition = AnalyzeCreatureSliceComposition(
                                            comp_pixels, screenshot_width, screenshot_height, sx, sy,
                                            stim_x, stim_y);
                                    }
                                    const std::string relative_path = want_before
                                        ? "screenshots/creature-slice-before.ppm"
                                        : "screenshots/creature-slice-after.ppm";
                                    if (WriteBackbufferPpm(scenario_config.artifact_dir / relative_path,
                                                           screenshot_width, screenshot_height)) {
                                        capture.file = relative_path;
                                        if (want_before) {
                                            creature_slice_before = capture;
                                            creature_slice_before_written = true;
                                        } else {
                                            WriteCreatureSliceAnalysis(
                                                scenario_config.artifact_dir,
                                                creature_slice_scene,
                                                creature_slice_before,
                                                capture);
                                            creature_slice_analysis_written = true;
                                        }
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
    if (scenario_config.active()) {
        if (auto* world_system = gameSession->GetWorldSystem()) {
            const double scenario_play_seconds = scenario_ready
                ? std::chrono::duration<double>(std::chrono::steady_clock::now() - scenario_play_started_at).count()
                : 0.0;
            WriteStreamingTelemetry(
                scenario_config.artifact_dir,
                scenario_config.scenario,
                scenario_play_seconds,
                world_system->get_streaming_telemetry_stats());
            if (scenario_config.lod_boundary_oscillation_smoke()) {
                WriteLodBoundaryOscillationAnalysis(
                    scenario_config.artifact_dir,
                    scenario_play_seconds,
                    LodBoundaryDistance(world_system),
                    lod_boundary_transition_recorder);
            }
            if (scenario_config.lod_seam_arrival_smoke()) {
                WriteLodSeamArrivalAnalysis(
                    scenario_config.artifact_dir,
                    scenario_play_seconds,
                    lod_seam_visual_captures,
                    lod_seam_arrival_recorder);
            }
        }
        if (scenario_config.window_mode_stress_smoke() && !window_mode_stress_analysis_written) {
            const double scenario_play_seconds = scenario_ready
                ? std::chrono::duration<double>(std::chrono::steady_clock::now() - scenario_play_started_at).count()
                : 0.0;
            WriteWindowModeStressAnalysis(
                scenario_config.artifact_dir,
                scenario_play_seconds,
                scenario_config.window_mode,
                window_mode_stress_steps,
                window_mode_stress_capture);
            window_mode_stress_analysis_written = true;
        }
    }

    std::vector<std::string> shutdown_milestones;
    auto mark_shutdown = [&](const std::string& milestone) {
        shutdown_milestones.push_back(milestone);
    };

    // T-I2-12: persist unsaved voxel edits on the world-exit/shutdown path,
    // before the streamed chunks are torn down. No-op when no world session
    // is active or when no chunk carries unsaved edits.
    if (gameSession && gameSession->SaveWorldState()) {
        mark_shutdown("world_state_saved");
    }

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
    // Alt+Enter: runtime windowed <-> borderless toggle (T-I4-DR-window-modes).
    if (key == GLFW_KEY_ENTER && action == GLFW_PRESS && (mods & GLFW_MOD_ALT)) {
        ToggleWindowedBorderless(window, g_windowState);
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
    (void)window;
    if (width <= 0 || height <= 0) return; // minimized window: ignore
    // Capture-pinned (scenario) runs must never resize their targets; the gate
    // depends on a fixed 1280x720 framebuffer.
    if (g_windowState.capture_pinned) return;
    // Debounce: record the pending size and let the main loop coalesce a burst
    // of drag events into one RenderPipeline::on_resize after the size settles.
    g_windowState.pending_width = width;
    g_windowState.pending_height = height;
    g_windowState.pending_since_seconds = glfwGetTime();
    g_windowState.resize_pending = true;
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
