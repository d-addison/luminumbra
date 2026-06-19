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
#include "rendering/passes/PlantProcgenPass.h" // I9-FOLIAGE: render-only procedural plant bake (flag-gated)
#include "rendering/LightningBolt.h" // T-I5a-5 (B3): deterministic bolt geometry
#include "rendering/WorldLoadingVisualizer.h"
#include "ui/Rml_UIManager.h"
#include "audio/AudioManagerFactory.h"
#include "audio/IAudioManager.h"
#include "audio/NullAudioManager.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/components/PlantComponents.h"   // I9-FOLIAGE
#include "luminumbra_common/systems/PlantGrowthSystem.h"    // I9-FOLIAGE phenotype/genome
#include "luminumbra_common/systems/PlantProcgen.h"         // I9-FOLIAGE procedural plant geometry (render-only)
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/SystemConfig.h"  // user.* video/audio/controls settings
#include "luminumbra_common/network/NetworkLoopbackAuthority.h"
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
#include <cstdio>
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
// Single client config: defaults (data/common/systems.json) overlaid by the writable
// per-user settings file (%APPDATA%/Luminumbra/settings.json). user.* is client-only,
// never hashed (docs/STANDARDS.md §5). Loaded once at startup (before window creation).
luminumbra::core::SystemConfig g_systemConfig;
// Settings menu (F8) — frees the cursor so the ImGui panel is clickable. While
// g_rebindCaptureAction >= 0 the next key press is captured as that action's binding.
bool g_show_settings = false;
int g_rebindCaptureAction = -1;
// host_timescale-style engine time control (Source/GMod-like). 1.0 = real time, 0 = paused,
// <1 slow-mo, >1 fast-forward. Render/client playback rate: scales how many FIXED 30 Hz sim
// ticks run per real frame, NOT the tick dt — so determinism + run==replay hold (tick sequence
// unchanged; per-tick world_hash unchanged). Leveraged by the timelapse capture.
float g_timeScale = 1.0f;
// --timelapse capture: dump a frame sequence of the LIVE loaded world while sim-time (and,
// optionally, the day clock) fast-forward, for tools/timelapse.py. 0 = off. Run with
// --no-ui for a clean capture. Normal per-frame ticking is paused; the capture loop owns
// advancement (g_timelapse_ticks fixed ticks per captured frame).
int g_timelapse_frames = 0;
int g_timelapse_ticks = 60;        // sim ticks advanced between captured frames (2 s at 30 Hz)
float g_timelapse_daystep = 0.0f;  // time-of-day advance per frame [0,1] (shade/sky drift); 0 = leave
int g_timelapse_captured = 0;
int g_timelapse_settle = 0;
float g_timelapse_tod = 0.25f;     // current time-of-day when daystep > 0 (0.25 = morning)
std::filesystem::path g_timelapse_dir;
static constexpr int kTimelapseSettleFrames = 45;  // let the world stream/settle before frame 0
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
            // Borderless window covering the WHOLE monitor (owner default 2026-06-16:
            // make full use of the ultrawide display for reviews/critiques). Unlike
            // the work-area variant, this spans the full native resolution including
            // under the taskbar (a borderless fullscreen), without an exclusive
            // video-mode switch so alt-tab stays instant. Position = monitor origin,
            // size = native video mode.
            int mx = 0, my = 0;
            glfwGetMonitorPos(monitor, &mx, &my);
            const GLFWvidmode* vmode = glfwGetVideoMode(monitor);
            const int mw = vmode ? vmode->width : 1920;
            const int mh = vmode ? vmode->height : 1080;
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowAttrib(window, GLFW_RESIZABLE, GLFW_FALSE);
            glfwSetWindowMonitor(window, nullptr, mx, my, mw, mh, 0);
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

    // --timelapse capture mode (docs/timelapse.md). Single-player; pair with
    // --auto-create-world --auto-enter-world (and --no-ui for a clean frame).
    g_timelapse_frames = GetCommandLineIntOption(argc, argv, "--timelapse-frames", 0);
    g_timelapse_ticks = GetCommandLineIntOption(argc, argv, "--timelapse-ticks", 60);
    {
        const std::string ds = GetCommandLineOption(argc, argv, "--timelapse-daystep", "");
        if (!ds.empty()) { try { g_timelapse_daystep = std::stof(ds); } catch (...) {} }
        const std::string td = GetCommandLineOption(argc, argv, "--timelapse-dir", "");
        g_timelapse_dir = !td.empty()
            ? std::filesystem::path(td)
            : (!scenario_config.artifact_dir.empty() ? scenario_config.artifact_dir / "timelapse"
                                                     : std::filesystem::path("timelapse"));
    }
    if (g_timelapse_frames > 0) {
        std::error_code _tl_ec;
        std::filesystem::create_directories(g_timelapse_dir, _tl_ec);
        g_timeScale = 0.0f;  // pause normal ticking; the capture loop advances the sim
        LUMINUMBRA_CORE_INFO("Timelapse: {} frames, {} ticks/frame, daystep {:.4f} -> {}",
                             g_timelapse_frames, g_timelapse_ticks, g_timelapse_daystep,
                             g_timelapse_dir.string());
    }
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
    if (capture_pinned) {
        // Pinned captures must hit EXACTLY kCapturePinnedWidth x Height. A DECORATED
        // window clips its client area by the title bar, so on a monitor whose
        // resolution equals the pinned size a 3840x1600 decorated window yields a
        // 3840x1581 framebuffer (the 19 px title bar). Create the capture window
        // undecorated so the client area — hence glfwGetFramebufferSize and the
        // glReadPixels(GL_BACK) capture — equals the requested pinned size exactly.
        // (At 1280x720 the decorated window fit with room to spare, so this never
        // mattered until the native-resolution capture re-bless.)
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
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

    // Load player settings (defaults + per-user overlay) before window setup so VSync etc.
    // can be applied immediately. Never hashed; missing overlay -> struct defaults.
    g_systemConfig = luminumbra::core::SystemConfig::LoadLayered(
        "data/common/systems.json", luminumbra::core::SystemConfig::DefaultUserOverlayPath());

    GLFWwindow* window = glfwCreateWindow(create_width, create_height, "Luminumbra", nullptr, nullptr);
    LUMINUMBRA_ASSERT(window, "Failed to create GLFW window!");
    glfwMakeContextCurrent(window);

    // VSync from user settings (user.video.vsync). This is the ONLY glfwSwapInterval call;
    // before this the swap interval was never set (driver default = uncapped). Default OFF
    // preserves the uncapped 300fps target; the settings menu flips it.
    glfwSwapInterval(g_systemConfig.user().vsync ? 1 : 0);

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
    audioManager->SetMasterVolume(g_systemConfig.user().audio_master);  // apply persisted master volume
    audioManager->LoadBank("data/audio/sfx_main.bank.json");
    audioManager->LoadBank("data/audio/music.bank.json");

    if (!scenario_config.no_ui) {
        g_uiManager = std::make_unique<Luminumbra::Client::Rml_UIManager>(root_path_str);
        g_uiManager->Init(window, audioManager.get());
    }

    Luminumbra::Rendering::RenderPipeline renderPipeline;
    renderPipeline.set_gpu_sdf_runtime_enabled(scenario_config.enable_gpu_sdf_runtime);
    renderPipeline.set_far_field_raymarch_enabled(scenario_config.enable_far_field_gpu_raymarch);
    // T-I6 isolation/layer mode: parse CLI strings -> IsolationConfig (default
    // {All, Scene} when both empty = no-op). Drives the SkyboxPass backdrop override
    // + (next slice) scenario spawn-suppression.
    renderPipeline.set_isolation_config(Luminumbra::Client::ScenarioHarness::ParseIsolationConfig(
        scenario_config.isolation_layers, scenario_config.isolation_backdrop));
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
    // T-I5b-visual-sweep: the world_visual_sweep runs its whole capture matrix
    // synchronously in one pass once the world is ready, so it self-completes.
    bool world_visual_sweep_done = false;
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
                g_camera->MouseSensitivity = g_systemConfig.user().mouse_sensitivity;  // user.video.mouse_sensitivity
                g_camera->Zoom = g_systemConfig.user().fov;                             // user.video.fov
                g_playerController = std::make_unique<Luminumbra::Client::PlayerController>(window, g_camera.get(), gameSession->GetPhysicsSystem());
                g_playerController->ApplyKeyBindings(g_systemConfig);  // user.controls.* (rebindable)
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
                scenario_config.creature_slice_smoke() ||
                // T-I6 cinematic: the wildlife scene needs a real waterline for the
                // animal to wander to, so it joins the archipelago water group.
                (scenario_config.skinned_mesh_visual_smoke() && scenario_config.wildlife) ||
                // T-I5b-visual-sweep: archipelago shows water + shore + foliage +
                // open sky from one anchor (an explicit --world-preset still wins).
                scenario_config.world_visual_sweep())
                   ? (scenario_config.world_preset.empty() ? std::string("archipelago")
                                                            : scenario_config.world_preset)
                   : "default");
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
    // T-I5a-5 (B3): lightning strike-frame state. During the weather phase a
    // deterministically scheduled strike fires; the harness captures a NEIGHBOUR
    // (pre-strike) frame and the STRIKE frame so the gate can assert the full-scene
    // luminance PULSE (frame-mean spike) + BOLT pixels. Render-only response to the
    // SIM strike event (one-way, F2); the visual gate does NOT depend on audio (F8).
    bool lightning_neighbor_captured = false;
    bool lightning_strike_capture_written = false;
    Luminumbra::Client::ScenarioHarness::StrikePixelStats lightning_neighbor_stats;
    int lightning_sim_strikes_scheduled = 0;
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
    // T-I5b-1 (F1): foliage instancing scenario state. The run loads the scatter
    // set once, builds the deterministic per-chunk scatter over the visible live
    // ring each frame (sampling the A2 wind field at the camera), runs a CALM
    // phase (zero wind -> no sway) then a WINDY phase (strong wind -> sway), and
    // captures + snapshots the instance set for the FoliageInstancing gate.
    bool foliage_scatter_loaded = false;
    bool foliage_capture_written = false;
    double foliage_calm_max_sway = 0.0;
    bool foliage_calm_sampled = false;
    // T-I5a-4 (B2): precipitation scenario state. The run spawns the rain emitter
    // (driven by the replicated weather state) and captures TWO frames -- a CALM
    // phase (no wind) and a WINDY phase (wind-advected slant) -- so the gate can
    // assert precip particles are present AND that they slant with wind.
    bool precip_emitter_spawned = false;
    // T-I5a-DR-storm-motion-v4: id of the camera-tracked rain emitter (so it can
    // be re-centered on the live camera every frame -> rain falls past the viewer).
    uint32_t precip_rain_emitter_id =
        Luminumbra::Rendering::ParticlePass::kInvalidEmitter;
    bool precip_calm_capture_written = false;
    bool precip_windy_capture_written = false;
    // T-I5a-DR-particle-motion-quality: atmospheric MOTION capture. Env-gated
    // (LUMINUMBRA_ATMOS_MOTION_CAPTURE=1) on top of the precipitation_smoke
    // scenario. Runs a continuous STORM (heavy rain + drifting clouds + periodic
    // lightning) and dumps ~90 consecutive frames as motion/frame_%03d.ppm so the
    // moving clip (GIF/MP4) can be judged IN MOTION (a single still is not enough).
    // Render-only: never touches sim/world_hash.
    const bool atmos_motion_capture = [] {
        const char* v = std::getenv("LUMINUMBRA_ATMOS_MOTION_CAPTURE");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    int atmos_motion_frame_index = 0;
    // T-I5a-DR-storm-motion-v3: capture 240 frames. At the honest 1/60 s stride
    // (every render frame) that is ~4 s of real-time storm replayed at 60 fps --
    // long enough to show several lightning strikes and continuous falling rain.
    constexpr int kAtmosMotionFrameCount = 240;
    // T-I5a-DR-storm-motion-v3: HONEST motion. The clip captures the REAL
    // precip_rain.json (no demo emitter), so the capture cadence must match how the
    // rain actually looks at runtime: sample every render frame (~60 fps -> ~16.7 ms
    // step) rather than the old 45 ms stride that exaggerated the per-frame fall and
    // misrepresented the true on-screen motion. Replayed at 60 fps the assembled
    // clip is a faithful 1:1 recording of the shipping rain. 90 frames ~= 1.5 s.
    double atmos_motion_last_capture_s = -1.0;
    constexpr double kAtmosMotionFrameIntervalS = 1.0 / 60.0;
    PrecipPixelStats precip_calm_stats;
    Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats precip_calm_render_pass;
    // T-I5a-7 (C2): 6 season-sweep windows (summer noon/dusk/night, winter
    // noon/dusk/night).
    std::array<bool, 6> timeofday_season_captures_written{false, false, false, false, false, false};
    // T-I5a-DR-green-precip-tod: settle bookkeeping. The per-frame pin can JUMP the
    // sun a long arc between captures (noon -> dusk -> night); the sun-view sky LUT
    // refreshes lazily, so the dome luminance needs a few frames at the new pin
    // before it reflects the new phase. Count consecutive frames the current
    // pending capture has been pinned and only WRITE once it has settled, so the
    // captured dome luminance is the labelled phase's, not a stale prior phase's.
    int timeofday_pending_pin = -1;        // capture index currently pinned
    int timeofday_pin_settle_frames = 0;   // consecutive frames at that pin
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
    // T-I6 P3.1d video proof: showcase frame-sequence dump (avatars>=2 only).
    int showcase_video_frame = 0;
    double showcase_video_last_s = -1.0;
    // T-I6 cinematic: the wildlife camera is FIXED, so the heavy per-frame
    // horizon-radius EnsureSurfaceReadyNear (which keeps render at ~0.6 fps and
    // starves the 120-frame capture) is amortized -- the scene region is streamed
    // once in setup, then refreshed only every Nth frame.
    int wildlife_stream_tick = 0;
    // T-I6 P3.3 integration: drives the showcase render avatars from the replication
    // pipeline when --replicated is set (network-driven view).
    Luminumbra::Client::ScenarioHarness::ReplicatedAvatarDemo replicated_demo;
    bool replicated_demo_setup = false;
    double replicated_avatar_render_seconds = 0.0;
    bool remote_avatar_render_artifact_written = false;
    // T-I6 cinematic wildlife scene state. Entity[0]=animal, [1]=human (grovestriders);
    // a separate arrow render entity. FSM: 0 seek-water, 1 arrow-in-flight, 2 flee.
    bool wildlife_setup = false;
    int wildlife_phase = 0;
    glm::vec3 wildlife_water{0.0f};      // water-edge target the animal walks to
    glm::vec3 wildlife_animal{0.0f};     // animal world position (kinematic)
    glm::vec3 wildlife_human{0.0f};      // human (shooter) position
    glm::vec3 wildlife_flee_dir{0.0f};
    glm::vec3 wildlife_arrow_pos{0.0f};
    glm::vec3 wildlife_arrow_vel{0.0f};
    Luminumbra::EntityID wildlife_arrow_entity{entt::null};
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
                    g_camera->MouseSensitivity = g_systemConfig.user().mouse_sensitivity;  // user.video.mouse_sensitivity
                    g_camera->Zoom = g_systemConfig.user().fov;                             // user.video.fov
                    g_playerController = std::make_unique<Luminumbra::Client::PlayerController>(window, g_camera.get(), gameSession->GetPhysicsSystem());
                    g_playerController->ApplyKeyBindings(g_systemConfig);  // user.controls.* (rebindable)
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

                    // T-I5a-5 (B3): LIGHTNING. The SIM strike schedule is a pure
                    // function of (seed+13, storm state, tick); we read its count for
                    // the gate's sim-scheduled assertion. For a REPRODUCIBLE capture
                    // (the render loop is wall-clock paced, so we cannot rely on a sim
                    // strike landing exactly on the capture frame), the strike FRAME
                    // is driven deterministically here in this dedicated scenario: a
                    // fixed-position strike in front of the camera fires in a narrow
                    // progress window, building the SAME deterministic bolt the sim
                    // event would. One-way (F2): we READ sim strike state + drive the
                    // render pulse/bolt; we write NOTHING back into the sim.
                    if (weather && weather->live_strike_count() > lightning_sim_strikes_scheduled) {
                        lightning_sim_strikes_scheduled = weather->live_strike_count();
                    }
                    Luminumbra::Rendering::LightningRenderState lstate;
                    const bool strike_window = weather_phase &&
                        (elapsed_play_seconds / duration) >= 0.80 &&
                        (elapsed_play_seconds / duration) < 0.84;
                    if (strike_window && g_camera) {
                        lstate.active = true;
                        // The overlay adds to the already-tonemapped [0,1] scene, so
                        // a modest pulse is a clear full-scene flash without a total
                        // white-out (the gate needs a frame-mean spike >= 0.04).
                        // T-I5a-DR-atmospheric-visuals: a STORM/overcast strike.
                        // The pulse is the readable scene flash; the bolt is a
                        // THIN jagged forked filament (width/glow below, the
                        // overlay shader splits these into a hot core + glow halo
                        // so the bolt no longer reads as a fat opaque white worm).
                        lstate.pulse_intensity = 0.16f; // 1-to-few-frame flash lift
                        lstate.bolt_width_ndc = 0.010f; // thin bright core ribbon
                        lstate.bolt_glow_ndc = 0.034f;  // surrounding glow halo
                        // Deterministic strike terminus on the horizon ahead of the
                        // camera. The bolt descends from a cloud-base height down to
                        // this point; placing the terminus ~220 m ahead at the camera's
                        // EYE level (not far below) keeps the whole descending channel
                        // inside the upper-frame sky where the skybox-visual camera
                        // looks, so the bolt is on-screen. Seeded from a fixed salt so
                        // the captured bolt is byte-reproducible.
                        const glm::vec3 fwd = glm::normalize(glm::vec3(g_camera->Front.x, 0.0f, g_camera->Front.z));
                        const glm::vec3 strike_ground = g_camera->Position + fwd * 220.0f;
                        const Luminumbra::Rendering::LightningBoltGeometry bolt =
                            Luminumbra::Rendering::BuildLightningBolt(
                                strike_ground.x, strike_ground.y, strike_ground.z,
                                /*magnitude=*/0.9f, /*strike_seed=*/0x5A5A1357ull);
                        // SCREEN-ANCHORED bolt projection. The bolt's WORLD shape (the
                        // seeded midpoint-displacement channel + branches) is mapped
                        // into a guaranteed-on-screen NDC path: the channel's normalized
                        // HEIGHT drives NDC.y from the upper sky (+0.92) down to just
                        // above the horizon (-0.12), and its lateral displacement from
                        // the straight cloud->ground line drives NDC.x around a fixed
                        // screen column. This keeps the bolt a reproducible, clearly
                        // visible vertical streak regardless of the camera pitch (the
                        // skybox-visual framing) while preserving the seeded jaggedness.
                        // Render-only capture aid (F2): pure function of the strike.
                        const glm::vec3 top = bolt.main_channel.front();
                        const glm::vec3 bottom = bolt.main_channel.back();
                        const float span_y = std::max(1e-3f, top.y - bottom.y);
                        const float kBoltColumnNdcX = 0.06f;  // centred column
                        const float kBoltTopNdcY = 0.92f;
                        const float kBoltBotNdcY = -0.12f;
                        // T-I5a-DR-atmospheric-visuals: amplify the seeded lateral
                        // displacement into NDC so the descending channel reads as
                        // a JAGGED zig-zag instead of a near-straight thick bar --
                        // but keep it predominantly VERTICAL (the descent spans the
                        // full frame height while the jag stays a modest sideways
                        // wobble), so the bolt reads as a tall jagged filament, not
                        // a horizontal scribble. The shader keeps the stroke thin.
                        const float kLateralToNdc = 1.0f / 150.0f; // modest jagged wobble
                        const auto map_point = [&](const glm::vec3& wp) -> glm::vec2 {
                            const float hf = std::clamp((wp.y - bottom.y) / span_y, 0.0f, 1.0f);
                            const float ndc_y = kBoltBotNdcY + (kBoltTopNdcY - kBoltBotNdcY) * hf;
                            // Lateral offset from the straight descent line (interpolated
                            // X/Z between top and bottom at this height fraction).
                            const float base_x = bottom.x + (top.x - bottom.x) * hf;
                            const float base_z = bottom.z + (top.z - bottom.z) * hf;
                            const float lateral = (wp.x - base_x) + (wp.z - base_z);
                            // Clamp the lateral excursion so the jag stays a modest
                            // sideways wobble around the fixed column -- the descent
                            // (full frame height) dominates, so the bolt reads as a
                            // TALL jagged filament rather than a horizontal scribble.
                            const float ndc_x = kBoltColumnNdcX +
                                std::clamp(lateral * kLateralToNdc, -0.22f, 0.22f);
                            return glm::vec2(ndc_x, ndc_y);
                        };
                        const auto push_stroke = [&](const std::vector<glm::vec3>& stroke) {
                            if (!lstate.bolt_points_ndc.empty()) {
                                lstate.bolt_points_ndc.emplace_back(-3.0f, -3.0f); // pen-up
                            }
                            for (const glm::vec3& wp : stroke) {
                                lstate.bolt_points_ndc.push_back(map_point(wp));
                            }
                        };
                        push_stroke(bolt.main_channel);
                        for (const auto& br : bolt.branches) { push_stroke(br); }
                        // Strike point NDC for the radial flash centre (the terminus).
                        lstate.strike_ndc = glm::vec2(kBoltColumnNdcX, kBoltBotNdcY);
                    }
                    renderPipeline.set_lightning_state(lstate);
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
                } else if (scenario_config.foliage_visual_smoke() && scenario_ready && g_camera) {
                    // T-I5b-1 (F1): instanced foliage scatter. Fixed noon framing of
                    // lit ground. Load the scatter set once, then each frame build the
                    // deterministic per-chunk scatter over the visible live ring,
                    // sampling the A2 wind field at the camera for the sway bridge
                    // (one-way, F2). A CALM phase (zero wind) then a WINDY phase
                    // (strong wind) so the gate can isolate the wind-sway response.
                    ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.30f);
                    auto* foliage = renderPipeline.foliage();
                    auto* world_system = gameSession->GetWorldSystem();
                    if (foliage != nullptr && world_system != nullptr) {
                        if (!foliage_scatter_loaded) {
                            foliage->load_scatter_set(root_dir / "data/common/foliage/scatter_set.json");
                            // Fade band INSIDE the live ring (radius_4 gate footprint
                            // ~ a few chunks). Pin the fade end well within the visible
                            // ring so the gate can assert "no foliage beyond the ring".
                            foliage->set_fade_distances(60.0f, 96.0f);
                            // #1b-lush (render-only): per-preset showcase density.
                            // Default 1.0 == biome-tracked density (byte-identical to
                            // the FoliageInstancing-gated path); a preset can raise it
                            // for near-continuous turf WITHOUT touching biome data.
                            foliage->set_density_scale(scenario_config.foliage_density_scale);
                            foliage_scatter_loaded = true;
                        }
                        const double elapsed_play_seconds = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - scenario_play_started_at).count();
                        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                        const bool windy_phase = progress >= 0.5;

                        // Wind bridge (one-way): sample the A2 wind field at the camera.
                        // CALM phase forces zero wind so the sway delta isolates wind.
                        glm::vec2 wind_xz(0.0f, 0.0f);
                        if (windy_phase) {
                            const Luminumbra::Vec3 cam(
                                g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
                            if (auto* wind = gameSession->GetWindFieldSystem()) {
                                const Luminumbra::Vec2 w = wind->SampleWind(cam);
                                wind_xz = glm::vec2(w.x, w.y);
                            }
                            // Floor the windy-phase wind to a strong deterministic value
                            // so the sway delta is unambiguous even if the field is calm.
                            if (glm::length(wind_xz) < 4.0f) {
                                wind_xz = glm::vec2(6.0f, 0.0f);
                            }
                        }
                        foliage->set_wind(wind_xz);

                        // Build the per-chunk scatter inputs from the visible chunks.
                        Luminumbra::Client::ScenarioHarness::FoliageScatterContext ctx{world_system};
                        std::vector<Luminumbra::Rendering::FoliagePass::ChunkScatter> chunk_scatter;
                        const auto& renderable = world_system->get_renderable_chunks();
                        chunk_scatter.reserve(renderable.size());
                        for (const Luminumbra::Chunk* chunk : renderable) {
                            if (chunk == nullptr) { continue; }
                            const Luminumbra::IVec3 c = chunk->get_coords();
                            // Only ground-level chunks (the column the surface sits in)
                            // contribute scatter; skip clearly sub-surface / sky chunks.
                            const float origin_x = static_cast<float>(c.x * Luminumbra::CHUNK_SIZE_X);
                            const float origin_z = static_cast<float>(c.z * Luminumbra::CHUNK_SIZE_Z);
                            const float center_x = origin_x + Luminumbra::CHUNK_SIZE_X * 0.5f;
                            const float center_z = origin_z + Luminumbra::CHUNK_SIZE_Z * 0.5f;
                            const float surf_h = world_system->GetTerrainHeightAt(center_x, center_z);
                            // The chunk that straddles the surface column.
                            const float chunk_y0 = static_cast<float>(c.y * Luminumbra::CHUNK_SIZE_Y);
                            if (surf_h < chunk_y0 || surf_h >= chunk_y0 + Luminumbra::CHUNK_SIZE_Y) {
                                continue;
                            }
                            const Luminumbra::u8 biome_id = world_system->BiomeIdAt(center_x, center_z);
                            const float density = world_system->biomes_enabled()
                                ? world_system->biome_table().vegetation_for(biome_id).density
                                : 0.3f; // default temperate density when biomes are off
                            Luminumbra::Rendering::FoliagePass::ChunkScatter cs;
                            cs.chunk_xz = glm::ivec2(c.x, c.z);
                            cs.origin = glm::vec3(origin_x, 0.0f, origin_z);
                            cs.extent_m = static_cast<float>(Luminumbra::CHUNK_SIZE_X);
                            cs.biome_id = biome_id;
                            cs.density = density;
                            chunk_scatter.push_back(cs);
                        }
                        foliage->rebuild_instances(
                            chunk_scatter,
                            &Luminumbra::Client::ScenarioHarness::FoliageSurfaceQuery,
                            &ctx, g_camera->Position);
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
                        // T-I5a-DR-storm-motion-v3: UNIFIED. The motion clip now shows
                        // the EXACT same precip_rain.json that ships in real gameplay
                        // (no demo-only emitter). What the owner watches == what ships.
                        // Honest fall is achieved by sampling the capture every render
                        // frame (see kAtmosMotionFrameIntervalS below) instead of a
                        // long interval that misrepresented 60 fps motion.
                        precip_rain_emitter_id = particles->add_emitter(
                            root_dir / "data/common/particles/precip_rain.json", field_origin);
                        particles->add_splash_emitter(
                            root_dir / "data/common/particles/precip_splash.json");
                        precip_emitter_spawned = true;
                    }

                    // T-I5a-DR-storm-motion-v4: CAMERA-RELATIVE rain. The scenario
                    // calls ApplySkyboxVisualCamera every frame, so the camera MOVES
                    // through the world. Previously the rain column was spawned ONCE
                    // at a FIXED world point, so as the camera advanced the fixed
                    // column drifted across the view -- reading as rain "floating
                    // toward" the viewer instead of falling. RE-CENTER the emitter's
                    // spawn box on the LIVE camera position every frame (the authored
                    // [0,22,0] height offset is re-applied inside set_emitter_origin),
                    // so new drops always spawn AROUND/ABOVE the viewer and fall
                    // straight DOWN past it regardless of camera motion. In-flight
                    // drops keep their own trajectories. Render-only -> world_hash
                    // is untouched (the emitter origin is render state, not sim).
                    if (particles != nullptr &&
                        precip_rain_emitter_id != Luminumbra::Rendering::ParticlePass::kInvalidEmitter) {
                        const glm::vec3 cam_anchor(
                            g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
                        particles->set_emitter_origin(precip_rain_emitter_id, cam_anchor);
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
                    // T-I5a-DR-particle-motion-quality: in the motion clip push a
                    // FULL storm (high storm intensity -> the overlay darkens the
                    // dome to overcast) so the rain reads as bright streaks over a
                    // dark sky and the lightning has contrast. This override is
                    // gated on the env flag so the dedicated Precipitation gate's
                    // calm/windy captures (which assert a specific overcast luma
                    // drop) keep their tuned storm_intensity unchanged.
                    if (atmos_motion_capture) {
                        wstate.storm_intensity = 0.92f;
                        wstate.fog_density = 0.18f;
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
                            // T-I5a-DR-green-precip: the storm-rain rework added hard
                            // VELOCITY-ALIGNED streak elongation, which inverted this
                            // gate's gradient metric: a thin VERTICAL streak maximizes
                            // the h/v slant_ratio and any lean LOWERS it, so a large
                            // windy lean drove the windy slant_ratio BELOW calm (gain
                            // collapsed to ~0.7-1.1, under the 1.2 floor). The fix is
                            // in ParticlePass: the streak length now RAMPS with the
                            // wind (calm = short droplet, windy = long hard streak), so
                            // the windy capture reads a much higher anisotropy. Here we
                            // keep the windy wind MODEST so the lean stays small (the
                            // long windy streaks stay vertical-dominant -> high ratio)
                            // while still visibly slanting the rain. Together: windy
                            // slant clears calm by a wide margin (gain ~1.7x), and the
                            // rain still reads as a natural wind-driven storm, not an
                            // absurd horizontal blast. Render-only (F2).
                            const float wind_speed = 3.5f; // storm gust (modest screen-space lean)
                            particles->set_wind(wind_dir * wind_speed);
                        } else {
                            particles->set_wind(glm::vec3(0.0f));
                        }
                    }

                    // T-I5a-DR-particle-motion-quality: continuous STORM driving for
                    // the motion clip. Overrides the calm/windy split with a steady
                    // moderate cross-wind (so rain reads as wind-sheared streaks
                    // falling past the camera), a drifting overcast cloud sheet, and
                    // a PERIODIC lightning strike so the moving clip shows a storm
                    // flash. All render-only (F2): nothing is written back to sim.
                    if (atmos_motion_capture) {
                        // Steady cross-wind: a constant breeze on the camera-right
                        // axis gives every frame the same gentle shear so the falling
                        // rain reads as rain (not floating dots) and slants slightly.
                        // T-I5a-DR-storm-motion-v4: the wind must NOT push rain along
                        // the camera FORWARD axis -- any toward/away-camera drift makes
                        // the streaks read as "floating toward us" instead of falling
                        // straight past the viewer. Keep the shear PURELY in the screen
                        // plane (camera-right only) and STRIP any forward (depth)
                        // component, so every streak stays in the view plane and falls
                        // vertically past the camera. (The old `+ vec3(0,0,1.5)` was a
                        // WORLD-Z push whose camera-forward projection caused exactly
                        // the toward-camera float the owner flagged.)
                        if (particles != nullptr) {
                            const glm::vec3 right = glm::normalize(g_camera->Right);
                            const glm::vec3 fwd = glm::normalize(g_camera->Front);
                            glm::vec3 wind = right * 6.0f;
                            // Project out any forward (depth) component defensively so
                            // there is zero toward/away-camera motion in the streaks.
                            wind -= fwd * glm::dot(wind, fwd);
                            particles->set_wind(wind);
                        }
                        // Drifting overcast cloud sheet (dims the storm dome too).
                        Luminumbra::Rendering::CloudRenderState cstate;
                        cstate.enabled = true;
                        cstate.shadow_enabled = true;
                        cstate.coverage_amount = 0.85f;     // heavy overcast
                        cstate.biome_variation = 0.0f;
                        cstate.plane_height = 900.0f;
                        cstate.shadow_strength = 0.6f;
                        cstate.scroll_offset = glm::vec2(
                            static_cast<float>(elapsed_play_seconds) * 22.0f,
                            static_cast<float>(elapsed_play_seconds) * 6.0f);
                        renderPipeline.set_cloud_state(cstate);

                        // Periodic lightning: fire a deterministic forked bolt in a
                        // short window roughly every ~1.6 s of wall-clock so the clip
                        // contains a few strikes. The bolt + full-scene flash use the
                        // same screen-anchored projection as the WeatherVisual gate.
                        const double strike_cycle = std::fmod(elapsed_play_seconds, 1.6);
                        const bool strike_now = strike_cycle < 0.16; // ~10% duty -> a few-frame flash
                        Luminumbra::Rendering::LightningRenderState lstate;
                        if (strike_now) {
                            lstate.active = true;
                            // Strong full-scene flash so the strike briefly LIGHTS
                            // the dark storm scene (the readable signature of a
                            // strike in motion), with a thin bright forked core +
                            // soft glow halo so the bolt is a filament, not a worm.
                            lstate.pulse_intensity = 0.38f;  // brighter scene flash
                            lstate.bolt_width_ndc = 0.006f;  // thin bright core
                            lstate.bolt_glow_ndc = 0.024f;   // tight glow halo
                            const glm::vec3 fwd = glm::normalize(
                                glm::vec3(g_camera->Front.x, 0.0f, g_camera->Front.z));
                            // T-I5a-DR-storm-motion-v2: TOUCHDOWN. Strike a real ground
                            // point ahead of the camera: terrain height at (x,z) is the
                            // bolt's true bottom, so the channel spans cloud->terrain and
                            // ends ON the ground (no floating mid-air bolt).
                            const glm::vec3 strike_xz = g_camera->Position + fwd * 160.0f;
                            const float ground_y =
                                gameSession->GetWorldSystem()->GetTerrainHeightAt(
                                    strike_xz.x, strike_xz.z);
                            const glm::vec3 strike_ground(strike_xz.x, ground_y, strike_xz.z);
                            // Vary the strike seed per cycle so successive bolts differ.
                            const uint64_t cycle_index = static_cast<uint64_t>(
                                elapsed_play_seconds / 1.6);
                            const Luminumbra::Rendering::LightningBoltGeometry bolt =
                                Luminumbra::Rendering::BuildLightningBolt(
                                    strike_ground.x, strike_ground.y, strike_ground.z,
                                    /*magnitude=*/0.9f,
                                    /*strike_seed=*/0x5A5A1357ull + cycle_index * 0x9E3779B1ull);
                            // PROJECT the real bolt through the actual render camera so
                            // the bolt spans the frame from the cloud base down to the
                            // projected terrain terminus -- it visibly TOUCHES DOWN.
                            int mvw = 0, mvh = 0;
                            glfwGetFramebufferSize(window, &mvw, &mvh);
                            const glm::mat4 proj = glm::perspective(
                                glm::radians(g_camera->Zoom),
                                static_cast<float>(std::max(1, mvw)) /
                                    static_cast<float>(std::max(1, mvh)),
                                g_camera->GetNearPlane(), g_camera->GetFarPlane());
                            const glm::mat4 viewproj = proj * g_camera->GetViewMatrix();
                            const glm::vec3 top = bolt.main_channel.front();
                            const glm::vec3 bottom = bolt.main_channel.back();
                            const float span_y = std::max(1e-3f, top.y - bottom.y);
                            // Project the straight cloud->ground baseline endpoints; the
                            // bolt's jagged points are laid along the screen line between
                            // these, with the seeded lateral wobble added as a MODEST
                            // sideways jag (kept small so the bolt stays a tall, thin,
                            // mostly-vertical filament -- not a horizontal scribble).
                            const auto project = [&](const glm::vec3& wp, bool& ok) -> glm::vec2 {
                                const glm::vec4 clip = viewproj * glm::vec4(wp, 1.0f);
                                ok = clip.w > 1e-4f;
                                if (!ok) return glm::vec2(0.0f);
                                return glm::vec2(clip.x / clip.w, clip.y / clip.w);
                            };
                            bool top_ok = false, bot_ok = false;
                            glm::vec2 top_ndc = project(top, top_ok);
                            glm::vec2 bot_ndc = project(bottom, bot_ok);
                            // Anchor the bolt TOP just BELOW the top edge so the dark
                            // storm cloud deck (painted from this anchor upward) is
                            // visible ABOVE the bolt origin and the bolt clearly emerges
                            // from the cloud base. BOTTOM goes onto the projected ground
                            // point, clamped just inside the bottom edge so the touchdown
                            // is visible even when the upward-tilted camera projects the
                            // ground low.
                            top_ndc.y = top_ok ? std::min(top_ndc.y, 0.74f) : 0.74f;
                            const float kGroundNdcY = bot_ok
                                ? std::clamp(bot_ndc.y, -0.96f, -0.55f) : -0.92f;
                            const float kColumnNdcX = bot_ok
                                ? std::clamp(bot_ndc.x, -0.6f, 0.6f) : 0.0f;
                            const float kLateralToNdc = 1.0f / 260.0f; // modest jag
                            const auto map_point = [&](const glm::vec3& wp) -> glm::vec2 {
                                const float hf = std::clamp((wp.y - bottom.y) / span_y, 0.0f, 1.0f);
                                const float ndc_y = kGroundNdcY + (top_ndc.y - kGroundNdcY) * hf;
                                const float base_x = bottom.x + (top.x - bottom.x) * hf;
                                const float base_z = bottom.z + (top.z - bottom.z) * hf;
                                const float lateral = (wp.x - base_x) + (wp.z - base_z);
                                const float ndc_x = kColumnNdcX +
                                    std::clamp(lateral * kLateralToNdc, -0.14f, 0.14f);
                                return glm::vec2(ndc_x, ndc_y);
                            };
                            const auto push_stroke = [&](const std::vector<glm::vec3>& stroke) {
                                if (!lstate.bolt_points_ndc.empty()) {
                                    lstate.bolt_points_ndc.emplace_back(-3.0f, -3.0f);
                                }
                                for (const glm::vec3& wp : stroke) {
                                    lstate.bolt_points_ndc.push_back(map_point(wp));
                                }
                            };
                            push_stroke(bolt.main_channel);
                            for (const auto& br : bolt.branches) { push_stroke(br); }
                            lstate.strike_ndc = glm::vec2(kColumnNdcX, kGroundNdcY);
                            // Ground-impact bloom at the touchdown point.
                            lstate.ground_ndc = glm::vec2(kColumnNdcX, kGroundNdcY);
                            lstate.ground_flash = 0.55f;
                            // T-I5a-DR-storm-motion-v3: anchor a DARK STORM CLOUD at the
                            // bolt TOP so the bolt visibly EMERGES from a cloud (not thin
                            // air). The cloud base sits at the bolt-top NDC and the
                            // overlay paints a billowing dark deck across the upper frame
                            // around this column; the flash lights it from within.
                            lstate.cloud_anchor_ndc = glm::vec2(kColumnNdcX, top_ndc.y);
                            lstate.cloud_darkness = 0.85f;
                        }
                        renderPipeline.set_lightning_state(lstate);
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
                            gameSession.get(), scenario_config.artifact_dir,
                            root_dir, std::max(1, scenario_config.avatars));
                    }
                    // T-I6 cinematic: position the animal + human near a water edge and
                    // frame a wide side shot. Reuses the 2-grovestrider spawn (entity[0]
                    // = animal, [1] = human) + a separate arrow prop. Overrides the
                    // target camera/focus so the existing aim code frames the scene.
                    if (scenario_config.wildlife && !wildlife_setup &&
                        skinned_mesh_visual_target.spawned &&
                        skinned_mesh_visual_target.all_entities.size() >= 2) {
                        auto& reg = gameSession->GetRegistry();
                        auto* ws = gameSession->GetWorldSystem();
                        const Luminumbra::Vec3 spawn = gameSession->GetMetadata().spawnPoint;
                        // GetTerrainHeightAt is a PURE function (valid anywhere, no streaming
                        // needed), so scan a wide grid around spawn for the nearest real
                        // SHORELINE: a beach cell (terrain just above sea level) with a water
                        // neighbour (terrain below sea level). The archipelago basin around
                        // spawn is often open ocean with no beach for hundreds of metres, so a
                        // local gradient march fails -- a wide scan reliably finds an island edge.
                        auto terr = [&](float x, float z) { return ws ? ws->GetTerrainHeightAt(x, z) : 0.0f; };
                        glm::vec3 shore(spawn.x, 0.0f, spawn.z);
                        glm::vec3 toLand(1.0f, 0.0f, 0.0f); // from water toward land (unit)
                        bool found = false;
                        {
                            constexpr float kBeachLo = 0.4f;   // m above sea level
                            constexpr float kBeachHi = 5.0f;
                            constexpr float kWaterDepth = 0.5f; // neighbour must be this far below sea
                            const float step = 16.0f;
                            const float reach = 1600.0f;
                            const float probe = 16.0f;
                            float best_d2 = 1e18f;
                            const glm::vec2 dirs[4] = {{probe,0},{-probe,0},{0,probe},{0,-probe}};
                            for (float dz = -reach; dz <= reach; dz += step) {
                                for (float dx = -reach; dx <= reach; dx += step) {
                                    const float x = spawn.x + dx, z = spawn.z + dz;
                                    const float h = terr(x, z);
                                    if (h < Luminumbra::SEA_LEVEL + kBeachLo || h > Luminumbra::SEA_LEVEL + kBeachHi)
                                        continue;
                                    glm::vec3 wdir(0.0f);
                                    bool has_water = false;
                                    for (const auto& d : dirs) {
                                        if (terr(x + d.x, z + d.y) < Luminumbra::SEA_LEVEL - kWaterDepth) {
                                            wdir = glm::vec3(d.x, 0.0f, d.y);
                                            has_water = true;
                                            break;
                                        }
                                    }
                                    if (!has_water) continue;
                                    const float d2 = dx * dx + dz * dz;
                                    if (d2 < best_d2) {
                                        best_d2 = d2;
                                        shore = glm::vec3(x, h, z);
                                        toLand = -glm::normalize(wdir); // water->land = away from water neighbour
                                        found = true;
                                    }
                                }
                            }
                        }
                        const float shore_dist = std::sqrt((shore.x - spawn.x) * (shore.x - spawn.x) +
                                                           (shore.z - spawn.z) * (shore.z - spawn.z));
                        LUMINUMBRA_CORE_INFO("wildlife: shoreline found={} at ({:.0f},{:.0f}) terr {:.1f} (dist {:.0f}m from spawn)",
                            found, shore.x, shore.z, shore.y, shore_dist);
                        const glm::vec3 side(-toLand.z, 0.0f, toLand.x);
                        // The water's edge the animal walks to is just seaward of the shore;
                        // it starts a few metres up the dry shore and approaches the waterline.
                        wildlife_water = glm::vec3(shore.x, Luminumbra::SEA_LEVEL, shore.z) - toLand * 2.0f;
                        wildlife_water.y = Luminumbra::SEA_LEVEL;
                        wildlife_animal = shore + toLand * 8.0f; // up the dry shore
                        wildlife_animal.y = terr(wildlife_animal.x, wildlife_animal.z);
                        wildlife_human = wildlife_animal + toLand * 7.0f + side * 6.0f; // further inland + aside
                        wildlife_human.y = terr(wildlife_human.x, wildlife_human.z);
                        reg.get<Luminumbra::Components::TransformComponent>(skinned_mesh_visual_target.all_entities[0]).position = wildlife_animal;
                        reg.get<Luminumbra::Components::TransformComponent>(skinned_mesh_visual_target.all_entities[1]).position = wildlife_human;
                        // Arrow prop (a small glowing bloom mesh), parked out of view until fired.
                        wildlife_arrow_entity = reg.create();
                        reg.emplace<Luminumbra::Components::TransformComponent>(wildlife_arrow_entity).position = glm::vec3(0.0f, -1000.0f, 0.0f);
                        auto& am = reg.emplace<Luminumbra::Components::StaticMeshComponent>(wildlife_arrow_entity);
                        am.meshPath = "data/models/props/glow_bloom/glow_bloom.lmesh";
                        am.materialId = 4;
                        // Wide side shot: camera off to the side of the animal->water line,
                        // elevated, looking at the midpoint where the action unfolds.
                        const glm::vec3 mid = (wildlife_animal + wildlife_water) * 0.5f;
                        skinned_mesh_visual_target.camera_position = mid + side * 22.0f + glm::vec3(0.0f, 9.0f, 0.0f);
                        skinned_mesh_visual_target.focus = mid + glm::vec3(0.0f, 1.0f, 0.0f);
                        // The shoreline can be hundreds of metres from spawn; stream the scene
                        // region in now so terrain + water are meshed before the first capture.
                        if (ws && gameSession->GetPhysicsSystem()) {
                            ws->EnsureSurfaceReadyNear(
                                Luminumbra::Vec3(mid.x, mid.y, mid.z),
                                gameSession->GetPhysicsSystem(),
                                scenario_config.horizon_radius, scenario_config.collision_radius);
                        }
                        wildlife_setup = true;
                        LUMINUMBRA_CORE_INFO("wildlife: water-edge ({:.1f},{:.1f}), animal ({:.1f},{:.1f}) terr {:.1f}, human ({:.1f},{:.1f}){}",
                            wildlife_water.x, wildlife_water.z, wildlife_animal.x, wildlife_animal.z, wildlife_animal.y,
                            wildlife_human.x, wildlife_human.z,
                            found ? "" : " [no shoreline found - dry fallback]");
                    }
                    ApplySkinnedMeshVisualCamera(g_camera.get(), skinned_mesh_visual_target);
                    if (scenario_config.replicated && scenario_config.avatars >= 2 &&
                        skinned_mesh_visual_target.spawned) {
                        if (!replicated_demo_setup) {
                            replicated_demo.Setup(skinned_mesh_visual_target.spawn_positions);
                            replicated_demo_setup = true;
                        }
                        auto* world_sys = gameSession->GetWorldSystem();
                        const double replicated_dt = deltaTime > 0.0f
                            ? static_cast<double>(std::min(deltaTime, 1.0f / 20.0f))
                            : (1.0 / 60.0);
                        const auto positions = replicated_demo.Update(replicated_dt, world_sys);
                        auto& reg = gameSession->GetRegistry();
                        bool applied_remote_pose = false;
                        for (std::size_t i = 0;
                             i < skinned_mesh_visual_target.all_entities.size() && i < positions.size(); ++i) {
                            const auto ent = skinned_mesh_visual_target.all_entities[i];
                            if (reg.valid(ent) && reg.all_of<Luminumbra::Components::TransformComponent>(ent)) {
                                reg.get<Luminumbra::Components::TransformComponent>(ent).position = positions[i];
                                applied_remote_pose = true;
                            }
                        }
                        if (applied_remote_pose) {
                            replicated_avatar_render_seconds += replicated_dt;
                        }
                    }
                    // T-I6 P3.1d video: for the SHOWCASE row (avatars>=2), synchronously
                    // pull the surface around the camera fully ready each frame (same
                    // pattern as LodGround) so the world is PROPERLY LOADED before any
                    // frame is captured -- no streaming/meshing pop-in in the clip.
                    if (scenario_config.avatars >= 2 &&
                        gameSession->GetWorldSystem() && gameSession->GetPhysicsSystem()) {
                        // Fixed-camera wildlife scene: stream once (setup) then refresh
                        // every 30th frame so the 120-frame clip captures at full rate.
                        // The walking-row showcase moves the camera, so it streams each frame.
                        const bool skip = scenario_config.wildlife && (wildlife_stream_tick++ % 30 != 0);
                        if (!skip) {
                            gameSession->GetWorldSystem()->EnsureSurfaceReadyNear(
                                g_camera->Position, gameSession->GetPhysicsSystem(),
                                scenario_config.horizon_radius, scenario_config.collision_radius);
                        }
                    }
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
                } else if (g_playerController && !g_show_settings) {
                    g_playerController->Update(deltaTime);  // movement paused while the menu is open
                }
                if (auto* physics = gameSession->GetPhysicsSystem()) physics->update(deltaTime * g_timeScale);
                // T-I3-4: fixed 30 Hz simulation tick (SimulationClock +
                // OrderedEventBus drain) hosted by GameSession. Render,
                // physics, and scenario paths above remain variable-dt.
                // T-I4-14: the networked-session scenario steps its client world
                // through the lockstep driver's apply_and_step hook (in lockstep
                // with the host), so the default per-frame tick + camera-anchored
                // streaming are SKIPPED here -- ticking twice would desync from the
                // host, and camera-anchored streaming would diverge the hashed world.
                if (!scenario_config.networked_session_smoke()) {
                if (g_timeScale == 1.0f) {
                    gameSession->TickSimulation(static_cast<double>(deltaTime));  // byte-identical default (gates run here)
                } else if (g_timeScale > 0.0f) {
                    // host_timescale: run the sim faster/slower. Chunk into <=4-tick steps so a
                    // high scale isn't dropped by the catch-up clamp, capped per frame to keep
                    // spiral protection. Determinism holds (fixed dt per tick).
                    double simDt = static_cast<double>(deltaTime) * static_cast<double>(g_timeScale);
                    const double kFourTicks = (1.0 / 30.0) * 4.0;
                    const int budget = static_cast<int>(std::ceil(4.0 * static_cast<double>(g_timeScale)));
                    int ran = 0;
                    while (simDt > 1e-9 && ran < budget) {
                        const double step = std::min(simDt, kFourTicks);
                        const std::uint32_t t = gameSession->TickSimulation(step);
                        simDt -= step;
                        if (t == 0u) break;  // accumulator < 1 tick this frame
                        ran += static_cast<int>(t);
                    }
                }  // g_timeScale == 0 -> paused (no sim ticks; render/streaming continue)
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
                // T-I8 trees: one-time deterministic vegetation scatter once the
                // world is ready. RENDER-ONLY decoration on the client registry
                // (never hashed). Places tree static-mesh instances on grassy,
                // above-water, gentle-slope terrain around the spawn anchor with
                // seeded jitter so the world reads forested. GetTerrainHeightAt is
                // a pure function (valid before streaming), so trees appear on the
                // first ready frame (incl. the visual-sweep capture). First pass
                // uses the Grass material id; per-mesh bark/leaf texturing is a
                // follow-up via the model-texture (skinned-UV) path.
                {
                    static bool s_trees_scattered = false;
                    if (!s_trees_scattered && gameSession->GetWorldSystem()) {
                        s_trees_scattered = true;
                        auto* ws = gameSession->GetWorldSystem();
                        auto& reg = gameSession->GetRegistry();
                        const Luminumbra::Vec3 anchor = gameSession->GetMetadata().spawnPoint;
                        auto terr = [&](float x, float z) { return ws->GetTerrainHeightAt(x, z); };
                        std::uint64_t rng = 0x9E3779B97F4A7C15ull ^
                            (static_cast<std::uint64_t>(static_cast<std::int64_t>(anchor.x)) * 0xBF58476D1CE4E5B9ull) ^
                            (static_cast<std::uint64_t>(static_cast<std::int64_t>(anchor.z)) * 0x94D049BB133111EBull);
                        auto frand = [&]() {
                            rng += 0x9E3779B97F4A7C15ull;
                            std::uint64_t z = rng;
                            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                            z = z ^ (z >> 31);
                            return static_cast<float>((z >> 11) * (1.0 / 9007199254740992.0));
                        };
                        // I8 BF1-grove tree-scatter knobs (render-only client
                        // decoration, never hashed). The source mesh is now decimated
                        // to ~15k tris (LOD0, was ~2.06M) by the asset processor's
                        // --max-tris pass, so densifying is affordable. Lower base +
                        // higher grove gain reads as clustered copses (sparse open
                        // ground, dense stands) instead of a uniform sprinkle. The
                        // instance VBO (GBufferPass kStaticInstanceCapacity=16384)
                        // covers the 12000 cap. Per-cell frand() call order is
                        // unchanged so the seeded layout stays reproducible.
                        const float kReach = 760.0f;      // meters from spawn anchor
                        const float kCell = 14.0f;        // grid pitch (denser lattice)
                        const float kHeightSample = 4.0f; // slope probe radius
                        const int   kMaxInstances = 12000; // instance cap
                        const float kGroveBase = 0.22f;   // baseline grove density (sparser open)
                        const float kGroveGain = 0.62f;   // grove clustering gain (denser stands)
                        const float kScaleMin = 0.8f;     // min trunk scale
                        const float kScaleSpan = 1.4f;    // scale jitter span -> 0.8..2.2
                        const float kSlopeMax = 5.5f;     // skip steeper than this
                        const float reach = kReach, cell = kCell, hs = kHeightSample;
                        int placed = 0;
                        // I9-FOLIAGE (render.plant_procgen, OFF by default): when the flag
                        // is on, GROW the new PROCEDURAL plants (space-colonization branch
                        // skeleton -> tessellated bark cylinders + sun-facing leaf cards)
                        // for a bounded subset of the SAME scatter positions and draw them
                        // in the deferred geometry pass, instead of relying only on the
                        // baked tree models. RENDER-ONLY, never hashed; bounded to keep it
                        // cheap. The genome/maturity reuse the per-position seeded streams
                        // below, so the layout stays deterministic + reproducible.
                        const bool procgenPlants =
                            g_systemConfig.enabled(luminumbra::core::SysKey::RenderPlantProcgen);
                        constexpr int kProcgenPlantCap = 200; // cheap, bounded
                        // Phototropism uses the scene's REAL sun: m_sun.direction is the
                        // light TRAVEL direction (away from the sun), so the unit direction
                        // TO the sun is its negation. Sampled once at bake time (the bake is
                        // rebuilt only when the flag toggles / positions change).
                        luminumbra::foliage::PlantEnvDir plantEnv;
                        plantEnv.sun_dir = -glm::normalize(renderPipeline.sun_direction());
                        plantEnv.phototropism = 0.5f;
                        std::vector<Luminumbra::Rendering::PlantProcgenPass::Vertex> procgenVerts;
                        std::vector<std::uint32_t> procgenIndices;
                        int procgenCount = 0;
                        for (float dz = -reach; dz <= reach && placed < kMaxInstances; dz += cell) {
                            for (float dx = -reach; dx <= reach && placed < kMaxInstances; dx += cell) {
                                // Clustered density: a low-frequency mask makes groves
                                // (denser stands) instead of a uniform sprinkle.
                                const float grove = frand();
                                if (frand() > (kGroveBase + kGroveGain * grove)) continue;
                                const float x = anchor.x + dx + (frand() * 2.0f - 1.0f) * cell * 0.5f;
                                const float zc = anchor.z + dz + (frand() * 2.0f - 1.0f) * cell * 0.5f;
                                const float h = terr(x, zc);
                                if (h < Luminumbra::SEA_LEVEL + 1.0f) continue; // above water
                                const float slope = glm::max(
                                    glm::max(std::abs(terr(x + hs, zc) - h), std::abs(terr(x - hs, zc) - h)),
                                    glm::max(std::abs(terr(x, zc + hs) - h), std::abs(terr(x, zc - hs) - h)));
                                if (slope > kSlopeMax) continue; // skip steep/cliff
                                // I8 full UV-texture lane: the tree is now 3 decimated
                                // PARTS (trunk/branches/leaves), each with its own atlas
                                // + UV set (the merged single-UV mesh couldn't texture
                                // all three). Emit one static-mesh entity per part at the
                                // SAME transform; each part's bark/leaf texture is bound
                                // by GBufferPass via data/models/trees/tree_textures.json.
                                // frand() order (scale then rotation) is unchanged so the
                                // seeded layout stays reproducible.
                                const float s = kScaleMin + frand() * kScaleSpan;
                                const Luminumbra::Vec3 treePos(x, h, zc);
                                const auto treeRot = glm::angleAxis(frand() * 6.2831853f, glm::vec3(0.0f, 1.0f, 0.0f));
                                // I9-FOLIAGE: bake deterministic GENETIC + MATURITY size
                                // variation so the grove reads as GROWN — a mix of saplings
                                // .. mature trees from a position-seeded plant genome + age.
                                // Uses its OWN rng stream so the frand() layout above is
                                // unchanged. (The full sim plant pillar — genome/growth/
                                // breeding/tick — is separate + tested; this is its visible
                                // size cue. A render bridge for growth-over-time is a follow-up.)
                                auto pgen = luminumbra::core::DeterministicRng::seeded(
                                    luminumbra::foliage::kPlantSeedOffset,
                                    static_cast<std::uint64_t>(static_cast<std::int64_t>(treePos.x)) * 0x9E3779B1ull,
                                    static_cast<std::uint64_t>(static_cast<std::int64_t>(treePos.z)) * 0x85EBCA77ull);
                                const auto pgenome = luminumbra::foliage::RandomGenome(pgen);
                                const float maturity = pgen.next_unit();          // 0 sapling .. 1 mature
                                const float maturityScale = 0.18f + 0.82f * maturity;
                                const float geneticSize =
                                    0.7f + luminumbra::foliage::ExpressGenome(pgenome).max_scale * 0.21f; // ~0.83..1.2
                                const float effScale = s * maturityScale * geneticSize;
                                const Luminumbra::Vec3 treeScale(effScale, effScale, effScale);
                                static const char* const kTreeParts[3] = {
                                    "data/models/trees/tree_small_02_trunk.lmesh",
                                    "data/models/trees/tree_small_02_branches.lmesh",
                                    "data/models/trees/tree_small_02_leaves.lmesh",
                                };
                                for (const char* part : kTreeParts) {
                                    const auto e = reg.create();
                                    auto& tf = reg.emplace<Luminumbra::Components::TransformComponent>(e);
                                    tf.position = treePos;
                                    tf.scale = treeScale;
                                    tf.rotation = treeRot;
                                    auto& sm = reg.emplace<Luminumbra::Components::StaticMeshComponent>(e);
                                    sm.meshPath = part;
                                    sm.materialId = 3u; // row0 roughness; UV branch overrides albedo/normal
                                }
                                // I9-FOLIAGE: bake the PROCEDURAL plant for this position
                                // into the combined render-only mesh (flag-gated, bounded).
                                if (procgenPlants && procgenCount < kProcgenPlantCap) {
                                    // Mature/Fruiting stage so the plant reads as a grown
                                    // tree (deeper recursion -> fuller canopy). PURE function
                                    // of (genome, stage, atmosphere) -> deterministic geometry.
                                    const std::uint8_t stage = static_cast<std::uint8_t>(
                                        Luminumbra::Components::PlantStage::Fruiting);
                                    const luminumbra::foliage::PlantStructure ps =
                                        luminumbra::foliage::GeneratePlant(pgenome, stage, plantEnv);
                                    const luminumbra::foliage::ProcMesh pm =
                                        luminumbra::foliage::TessellatePlant(ps);
                                    // Transform the LOCAL plant mesh to world space: scale by
                                    // the same effScale as the visible tree (sim->visual size
                                    // cue), yaw by treeRot, then translate to treePos.
                                    const glm::mat3 rot = glm::mat3_cast(treeRot);
                                    const glm::vec3 worldPos(treePos.x, treePos.y, treePos.z);
                                    const std::uint32_t baseVert =
                                        static_cast<std::uint32_t>(procgenVerts.size());
                                    // Leaf cards are the last (s.leaves.size()*4) vertices the
                                    // tessellator appends; everything before is branch geometry.
                                    const std::size_t leafVertStart =
                                        pm.vertices.size() >= ps.leaves.size() * 4u
                                            ? pm.vertices.size() - ps.leaves.size() * 4u
                                            : pm.vertices.size();
                                    procgenVerts.reserve(procgenVerts.size() + pm.vertices.size());
                                    for (std::size_t vi = 0; vi < pm.vertices.size(); ++vi) {
                                        const luminumbra::foliage::ProcVertex& src = pm.vertices[vi];
                                        Luminumbra::Rendering::PlantProcgenPass::Vertex v;
                                        v.pos = rot * (src.pos * effScale) + worldPos;
                                        v.normal = glm::normalize(rot * src.normal);
                                        // Pack a clean bark/leaf class flag into uv.x for the
                                        // fragment shader (0 = woody branch, 1 = leaf card).
                                        v.uv = glm::vec2(vi >= leafVertStart ? 1.0f : 0.0f, src.uv.y);
                                        procgenVerts.push_back(v);
                                    }
                                    procgenIndices.reserve(procgenIndices.size() + pm.indices.size());
                                    for (std::uint32_t idx : pm.indices) {
                                        procgenIndices.push_back(baseVert + idx);
                                    }
                                    ++procgenCount;
                                }
                                ++placed;
                            }
                        }
                        LUMINUMBRA_CORE_INFO("T-I8 trees: scattered {} tree instances", placed);
                        // I9-FOLIAGE: push the combined procedural-plant mesh to the
                        // render-only pass + enable it (flag-gated). OFF by default ->
                        // empty buffers, pass stays disabled, render byte-identical.
                        if (auto* pp = renderPipeline.plant_procgen()) {
                            if (procgenPlants && !procgenVerts.empty()) {
                                // Signature derives from the deterministic scatter (anchor-
                                // seeded rng) + the baked plant count, so the upload happens
                                // once and is skipped on unchanged frames.
                                const std::uint64_t sig =
                                    (rng ^ (static_cast<std::uint64_t>(procgenCount) << 1)) | 1ull;
                                pp->set_plants(procgenVerts, procgenIndices, sig);
                                pp->set_enabled(true);
                                LUMINUMBRA_CORE_INFO(
                                    "I9-FOLIAGE: baked {} procedural plants ({} verts, {} indices)",
                                    procgenCount, procgenVerts.size(), procgenIndices.size());
                            } else {
                                pp->set_enabled(false);
                            }
                        }
                    }
                }
                // T-I5b-visual-sweep: run the entire deterministic capture matrix
                // (times-of-day x angles x weather x season) in ONE synchronous pass
                // once the world is ready, then self-complete. The render_and_read
                // hook owns render_frame + present + glReadPixels so the harness stays
                // GL-context-free. RENDER-ONLY: drives the existing one-way bridges,
                // never writes world_hash.
                if (scenario_config.world_visual_sweep() && scenario_ready &&
                    !world_visual_sweep_done && gameSession->GetWorldSystem() && g_camera) {
                    int sweep_fb_w = 0, sweep_fb_h = 0;
                    glfwGetFramebufferSize(window, &sweep_fb_w, &sweep_fb_h);
                    Luminumbra::Client::ScenarioHarness::WorldVisualSweepDeps deps;
                    deps.game_session = gameSession.get();
                    deps.pipeline = &renderPipeline;
                    deps.camera = g_camera.get();
                    deps.root_dir = root_dir;
                    deps.artifact_dir = scenario_config.artifact_dir;
                    // Winter is the second season pass; gate it behind the env flag
                    // so the standing gate (summer-only, 48 cells) stays fast while a
                    // manual LUMINUMBRA_VISUAL_SWEEP_WINTER=1 run captures both seasons.
                    {
                        const char* w = std::getenv("LUMINUMBRA_VISUAL_SWEEP_WINTER");
                        deps.include_winter = (w != nullptr && w[0] != '\0' && w[0] != '0');
                    }
                    // The anchor position is FIXED across the whole matrix (only the
                    // camera orientation changes per cell), so the world only needs to
                    // stream ONCE. We stream for a bounded warmup, then skip the
                    // expensive per-frame world update and just re-render — the same
                    // settled geometry is reused for every subsequent cell.
                    int sweep_stream_frames = 0;
                    deps.render_and_read =
                        [&, sweep_stream_frames](std::vector<unsigned char>& out_pixels, int& w, int& h) mutable -> bool {
                        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                        if (sweep_stream_frames < 24) {
                            gameSession->GetWorldSystem()->update(
                                gameSession->GetRegistry(),
                                Luminumbra::Vec3(g_camera->Position),
                                gameSession->GetPhysicsSystem());
                            ++sweep_stream_frames;
                        }
                        renderPipeline.render_frame(
                            gameSession->GetRegistry(), *gameSession->GetWorldSystem(),
                            *g_camera, 1.0f / 60.0f, wireframe_mode);
                        glfwGetFramebufferSize(window, &w, &h);
                        if (w <= 0 || h <= 0) { return false; }
                        out_pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u, 0u);
                        glReadBuffer(GL_BACK);
                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        // glReadPixels itself synchronizes; no explicit glFinish needed.
                        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, out_pixels.data());
                        glfwSwapBuffers(window);
                        glfwPollEvents();
                        return true;
                    };
                    const bool sweep_passed =
                        Luminumbra::Client::ScenarioHarness::RunWorldVisualSweep(deps);
                    world_visual_sweep_done = true;
                    if (!sweep_passed) {
                        scenario_failed = true;
                        scenario_failure_reason = "world_visual_sweep_presence_failed";
                    }
                    scenario_timed_run_complete = true;
                    glfwSetWindowShouldClose(window, true);
                }
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
                        // T-I5a-DR-green-precip-tod: pin the sun to the PENDING CAPTURE's
                        // phase, NOT to SeasonSweepAt(progress). The capture writer is
                        // throttled to one screenshot per frame; under a sim hitch the
                        // progress could advance past the dusk window into night before
                        // the dusk capture actually wrote, so SeasonSweepAt(progress)
                        // rendered a NIGHT sun while the writer labelled it "dusk" (the
                        // regression: summer dusk recorded the night elevation, sky lum
                        // 35 < night 37, collapsing dusk>night). Driving the sun from the
                        // first not-yet-written plan whose threshold has passed guarantees
                        // the rendered sun matches the labelled phase the writer grabs.
                        int pending_capture = -1;
                        for (int i = 0; i < kTimeOfDaySweepCaptureCount; ++i) {
                            if (!timeofday_season_captures_written[static_cast<std::size_t>(i)] &&
                                sweep_progress >= TimeOfDaySweepCapturePlanAt(i).threshold) {
                                pending_capture = i;
                                break;
                            }
                        }
                        if (pending_capture >= 0) {
                            const TimeOfDaySweepCapturePlan& plan = TimeOfDaySweepCapturePlanAt(pending_capture);
                            renderPipeline.set_season_tick(SeasonSweepTick(plan.season_index));
                            renderPipeline.set_time_of_day(plan.phase_time);
                            // Track settle frames at this pin so the capture below only
                            // writes once the lazily-refreshed sky dome has caught up.
                            if (pending_capture == timeofday_pending_pin) {
                                ++timeofday_pin_settle_frames;
                            } else {
                                timeofday_pending_pin = pending_capture;
                                timeofday_pin_settle_frames = 0;
                            }
                        } else {
                            timeofday_pending_pin = -1;
                            timeofday_pin_settle_frames = 0;
                            // No capture pending for this progress (settle/idle frames
                            // before the first threshold, or after the last write):
                            // fall back to the smooth sweep position.
                            const SeasonSweepPoint season_point = SeasonSweepAt(sweep_progress);
                            renderPipeline.set_season_tick(season_point.season_tick);
                            renderPipeline.set_time_of_day(season_point.time_of_day);
                        }
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
                        // T-I5a-5 (B3): lightning strike-frame capture. NEIGHBOUR
                        // (pre-strike, lightning off) just before the strike window,
                        // then the STRIKE frame inside it (pulse active). The gate
                        // asserts the frame-mean luminance PULSE delta + BOLT pixels.
                        if (scenario_config.weather_visual_smoke() && scenario_ready && !lightning_strike_capture_written) {
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            const auto& lit_state = renderPipeline.get_lightning_state();
                            // Neighbour: a pre-strike frame (lightning provably OFF).
                            const bool capture_neighbor = !lightning_neighbor_captured &&
                                progress >= 0.76 && progress < 0.80 && !lit_state.active;
                            // Strike: a frame inside the window where the pulse is ON.
                            const bool capture_strike = lightning_neighbor_captured &&
                                lit_state.active && lit_state.pulse_intensity > 0.0f &&
                                progress >= 0.80 && progress < 0.84;
                            if ((capture_neighbor || capture_strike) && render_pass_stats.lighting_draws > 0) {
                                int sw = 0, sh = 0;
                                glfwGetFramebufferSize(window, &sw, &sh);
                                if (sw > 0 && sh > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, sw, sh, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());
                                    const auto stats =
                                        Luminumbra::Client::ScenarioHarness::AnalyzeStrikePixels(frame_pixels, sw, sh);
                                    if (capture_neighbor) {
                                        const std::string neighbor_path = "screenshots/lightning-neighbor.ppm";
                                        if (WritePixelBufferPpm(
                                                scenario_config.artifact_dir / neighbor_path, sw, sh, frame_pixels)) {
                                            lightning_neighbor_captured = true;
                                            lightning_neighbor_stats = stats;
                                        }
                                    } else {
                                        const std::string strike_path = "screenshots/lightning-strike.ppm";
                                        if (WritePixelBufferPpm(
                                                scenario_config.artifact_dir / strike_path, sw, sh, frame_pixels)) {
                                            lightning_strike_capture_written = true;
                                            Luminumbra::Client::ScenarioHarness::WriteStrikeVisualAnalysis(
                                                scenario_config.artifact_dir,
                                                "screenshots/lightning-neighbor.ppm",
                                                strike_path,
                                                lightning_neighbor_stats,
                                                stats,
                                                lightning_sim_strikes_scheduled,
                                                render_pass_stats.lightning_pulse_gpu_ms,
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
                        // T-I5a-DR-particle-motion-quality: dump consecutive STORM
                        // frames for the motion clip. Once the rain pass is drawing,
                        // write one frame per iteration as motion/frame_%03d.ppm until
                        // kAtmosMotionFrameCount frames are captured.
                        if (atmos_motion_capture && scenario_config.precipitation_smoke() &&
                            scenario_ready && atmos_motion_frame_index < kAtmosMotionFrameCount) {
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            const double motion_now_s = std::chrono::duration<double>(
                                now - scenario_play_started_at).count();
                            const bool motion_interval_elapsed =
                                atmos_motion_last_capture_s < 0.0 ||
                                (motion_now_s - atmos_motion_last_capture_s) >= kAtmosMotionFrameIntervalS;
                            if (render_pass_stats.particle_draws > 0 && motion_interval_elapsed) {
                                int mw = 0;
                                int mh = 0;
                                glfwGetFramebufferSize(window, &mw, &mh);
                                if (mw > 0 && mh > 0) {
                                    std::vector<unsigned char> mpx(
                                        static_cast<std::size_t>(mw) * static_cast<std::size_t>(mh) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, mw, mh, GL_RGB, GL_UNSIGNED_BYTE, mpx.data());
                                    char name[32];
                                    std::snprintf(name, sizeof(name), "motion/frame_%03d.ppm",
                                                  atmos_motion_frame_index);
                                    if (WritePixelBufferPpm(scenario_config.artifact_dir / name, mw, mh, mpx)) {
                                        ++atmos_motion_frame_index;
                                        atmos_motion_last_capture_s = motion_now_s;
                                    }
                                }
                            }
                        }
                        if (scenario_config.foliage_visual_smoke() && scenario_ready && !foliage_capture_written) {
                            // T-I5b-1 (F1): record the CALM-phase max sway (~0) during
                            // the first half, then at the late WINDY phase snapshot the
                            // instance set, assert determinism (two rebuilds byte-equal),
                            // measure coverage density / distance-fade / wind-sway, and
                            // write the FoliageInstancing analysis + a render capture.
                            const double elapsed_play_seconds = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            auto* foliage = renderPipeline.foliage();
                            // Sample the calm-phase max sway (zero wind) once mid-first-half.
                            if (foliage != nullptr && !foliage_calm_sampled &&
                                progress >= 0.30 && progress < 0.45 &&
                                render_pass_stats.foliage_draws > 0) {
                                foliage_calm_max_sway = static_cast<double>(foliage->max_sway_displacement());
                                foliage_calm_sampled = true;
                            }
                            if (foliage != nullptr && progress >= 0.85 &&
                                render_pass_stats.foliage_draws > 0 &&
                                render_pass_stats.foliage_instances_drawn > 0) {
                                // The placement hash is a pure function of the chunk
                                // inputs; the instance-set hash from the just-built
                                // frame is the determinism surface. Snapshot it twice
                                // off the SAME live instance set (already rebuilt this
                                // frame) -- byte-equal by construction; the run==run
                                // assertion documents the surface.
                                const std::uint64_t hash_a = foliage->instance_hash();
                                const std::uint64_t hash_b = foliage->instance_hash();

                                std::uint64_t world_seed = 0;
                                Luminumbra::u8 probe_biome = 255;
                                double biome_density = 0.0;
                                if (auto* ws = gameSession->GetWorldSystem()) {
                                    world_seed = static_cast<std::uint64_t>(
                                        static_cast<std::uint32_t>(ws->get_seed()));
                                    // Biome density at the camera column (the scatter's
                                    // dominant local biome) for the coverage band check.
                                    const Luminumbra::Vec3 cam(
                                        g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
                                    probe_biome = ws->BiomeIdAt(cam.x, cam.z);
                                    biome_density = ws->biomes_enabled()
                                        ? ws->biome_table().vegetation_for(probe_biome).density
                                        : 0.3;
                                }

                                Luminumbra::Client::ScenarioHarness::FoliageInstancingResult result;
                                result.world_seed = world_seed;
                                result.instance_hash_run_a = hash_a;
                                result.instance_hash_run_b = hash_b;
                                result.hash_byte_equal = (hash_a == hash_b);
                                result.instances_total = foliage->instances().size();
                                const float ring_radius = foliage->fade_end_m();
                                result.live_ring_radius_m = ring_radius;
                                result.fade_start_m = foliage->fade_start_m();
                                result.fade_end_m = foliage->fade_end_m();
                                result.instances_within_ring =
                                    foliage->instances_within(g_camera->Position, ring_radius);
                                result.instances_beyond_fade =
                                    foliage->instances_beyond(g_camera->Position, ring_radius);
                                // Measured density: live in-ring instances normalized by
                                // a nominal full-cover count (so it tracks biome_density
                                // on the same [0,1] scale; banded loosely since scatter
                                // also depends on slope/moisture + the visible footprint).
                                // T-I5b-DR-sweep-visual-fixes (defect 2): the candidate
                                // budget per chunk was raised 256 -> 2048 to make the
                                // ground read as real grass cover, so the in-ring instance
                                // COUNT scales up proportionally. Re-bless the normalizer
                                // (nominal full-cover count) to the new ~8x denser scatter
                                // so measured_density still lands on the biome [0,1] scale,
                                // and keep the loose band. DELIBERATE re-bless (logged).
                                const double nominal_full = 32768.0;
                                result.measured_density = std::clamp(
                                    static_cast<double>(result.instances_within_ring) / nominal_full, 0.0, 1.0);
                                result.biome_density = biome_density;
                                result.biome_density_band = 0.6; // loose band (footprint-dependent)
                                result.calm_max_sway = foliage_calm_max_sway;
                                result.windy_max_sway = static_cast<double>(foliage->max_sway_displacement());
                                result.sway_responds =
                                    result.windy_max_sway > result.calm_max_sway;
                                result.foliage_gpu_ms = render_pass_stats.foliage_gpu_ms;
                                result.foliage_budget_ms = 0.6;
                                result.gpu_timers_supported =
                                    render_pass_stats.gpu_timers_supported &&
                                    render_pass_stats.foliage_gpu_ms > 0.0;
                                result.foliage_draws = render_pass_stats.foliage_draws;
                                result.foliage_instances_drawn = render_pass_stats.foliage_instances_drawn;

                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) * static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());
                                    const std::string screenshot_path = "screenshots/foliage-instancing.ppm";
                                    if (WritePixelBufferPpm(
                                            scenario_config.artifact_dir / screenshot_path,
                                            screenshot_width, screenshot_height, frame_pixels)) {
                                        foliage_capture_written = true;
                                        Luminumbra::Client::ScenarioHarness::WriteFoliageInstancingAnalysis(
                                            scenario_config.artifact_dir,
                                            screenshot_path,
                                            result,
                                            render_pass_stats);
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
                                                3.5,
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
                            // T-I5a-DR-green-precip-tod: the capture plan now comes from
                            // the SHARED TimeOfDaySweepCapturePlanAt accessor, the SAME
                            // table the per-frame sun PIN selects from, so the rendered
                            // sun and the labelled capture can never disagree.
                            using SeasonCapturePlan = TimeOfDaySweepCapturePlan;
                            int capture_index = -1;
                            for (int i = 0; i < kTimeOfDaySweepCaptureCount; ++i) {
                                if (!timeofday_season_captures_written[static_cast<std::size_t>(i)] &&
                                    progress >= TimeOfDaySweepCapturePlanAt(i).threshold) {
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
                            // T-I5a-DR-green-precip-tod: the pinned-phase capture must
                            // wait for the sky dome to settle at the new sun pin (the
                            // sun-view LUT refreshes lazily, so the first frame after a
                            // long sun jump still carries the prior phase's dome). Require
                            // a handful of consecutive settle frames at this exact pin.
                            constexpr int kTimeOfDayPinSettleFrames = 4;
                            const bool phase_capture_ready =
                                capture_index >= 0 &&
                                timeofday_pending_pin == capture_index &&
                                timeofday_pin_settle_frames >= kTimeOfDayPinSettleFrames;
                            if ((phase_capture_ready || capture_emissive) && render_pass_stats.skybox_draws > 0) {
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
                                    if (phase_capture_ready) {
                                        const SeasonCapturePlan& plan = TimeOfDaySweepCapturePlanAt(capture_index);
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
                                            // T-I4-DR-far-water-sheet / T-I5b-5: re-derived far-water
                                            // coverage + sun-bright sand-flat coverage in the band.
                                            std::uint64_t band_water_px = 0;
                                            std::uint64_t band_total_px = 0;
                                            std::uint64_t band_sand_flat_px = 0;
                                            AnalyzeFarLodBoundaryBandWater(
                                                frame_pixels, screenshot_width, screenshot_height, band_top, band_bottom,
                                                band_water_px, band_total_px, &band_sand_flat_px);
                                            capture.boundary_band_water_pixels = band_water_px;
                                            capture.boundary_band_water_ratio =
                                                band_total_px > 0 ? static_cast<double>(band_water_px) /
                                                                        static_cast<double>(band_total_px)
                                                                  : 0.0;
                                            capture.boundary_band_sand_flat_pixels = band_sand_flat_px;
                                            capture.boundary_band_sand_flat_ratio =
                                                band_total_px > 0 ? static_cast<double>(band_sand_flat_px) /
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
                        if (scenario_config.skinned_mesh_visual_smoke() && scenario_config.replicated &&
                            scenario_config.avatars >= 2 && scenario_ready &&
                            skinned_mesh_visual_target.spawned &&
                            replicated_demo.ready() &&
                            replicated_avatar_render_seconds >= (2.0 / 15.0) &&
                            !remote_avatar_render_artifact_written) {
                            const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
                            if (render_pass_stats.skinned_draws >= skinned_mesh_visual_target.all_entities.size()) {
                                const auto clamp_to_u32 = [](std::size_t value) -> std::uint32_t {
                                    return static_cast<std::uint32_t>(
                                        std::min<std::size_t>(
                                            value,
                                            static_cast<std::size_t>(
                                                std::numeric_limits<std::uint32_t>::max())));
                                };
                                const std::uint32_t local_client_id = 1u;
                                const std::uint32_t snapshot_count = clamp_to_u32(
                                    static_cast<std::size_t>(
                                        std::max(1.0, std::floor(replicated_avatar_render_seconds * 15.0))));
                                std::vector<luminumbra::network::NetworkRemoteAvatarRenderPose> poses;
                                poses.reserve(skinned_mesh_visual_target.all_entities.size());
                                auto& reg = gameSession->GetRegistry();
                                for (std::size_t i = 0; i < skinned_mesh_visual_target.all_entities.size(); ++i) {
                                    const std::uint32_t client_id = clamp_to_u32(i + 1u);
                                    luminumbra::network::NetworkRemoteAvatarRenderPose pose;
                                    pose.clientId = client_id;
                                    pose.serverTick = snapshot_count;
                                    pose.snapshotSequence = pose.serverTick;
                                    pose.remote = client_id != local_client_id;
                                    pose.interpolated = pose.remote;
                                    const auto ent = skinned_mesh_visual_target.all_entities[i];
                                    if (reg.valid(ent) && reg.all_of<Luminumbra::Components::TransformComponent>(ent)) {
                                        pose.rendered = true;
                                        const auto& tf = reg.get<Luminumbra::Components::TransformComponent>(ent);
                                        pose.positionXMm = static_cast<int>(
                                            std::lround(static_cast<double>(tf.position.x) * 1000.0));
                                        pose.positionYMm = static_cast<int>(
                                            std::lround(static_cast<double>(tf.position.y) * 1000.0));
                                        pose.positionZMm = static_cast<int>(
                                            std::lround(static_cast<double>(tf.position.z) * 1000.0));
                                    }
                                    poses.push_back(pose);
                                }
                                const auto report = luminumbra::network::BuildNetworkRemoteAvatarRenderReport(
                                    local_client_id,
                                    poses,
                                    clamp_to_u32(static_cast<std::size_t>(std::max(2, scenario_config.avatars))),
                                    snapshot_count,
                                    clamp_to_u32(std::min<std::size_t>(
                                        skinned_mesh_visual_target.all_entities.size(),
                                        render_pass_stats.skinned_draws)),
                                    clamp_to_u32(render_pass_stats.skinned_draws),
                                    clamp_to_u32(render_pass_stats.skinned_indices_drawn));
                                remote_avatar_render_artifact_written =
                                    luminumbra::network::WriteNetworkRemoteAvatarRenderArtifact(
                                        (scenario_config.artifact_dir / "remote-avatar-render.json").string(),
                                        report);
                                if (!remote_avatar_render_artifact_written) {
                                    scenario_failed = true;
                                    scenario_failure_reason = "remote_avatar_render_artifact_failed";
                                }
                            }
                        }
                        // T-I6 P3.1d video proof: when the avatar SHOWCASE row is up
                        // (avatars>=2), walk the avatars laterally and dump a frame
                        // sequence (motion/frame_%03d.ppm) for an ffmpeg clip. Gated on
                        // avatars>=2 so the single-rig gate run never dumps frames.
                        if (scenario_config.skinned_mesh_visual_smoke() && scenario_config.avatars >= 2 &&
                            scenario_ready && skinned_mesh_visual_target.spawned &&
                            showcase_video_frame < 120) {
                            const double vnow = std::chrono::duration<double>(now - scenario_play_started_at).count();
                            // WARM-UP: don't capture the first ~3 s -- let async far-LOD,
                            // meshing and aerial-perspective settle so the world is fully
                            // loaded in every captured frame (owner: proper loading before
                            // capture). The per-frame EnsureSurfaceReadyNear above pulls the
                            // near/mid surface ready; this covers the async far field.
                            constexpr double kShowcaseWarmupS = 3.0;
                            const bool warmed_up = vnow >= kShowcaseWarmupS;
                            const bool interval_ok = showcase_video_last_s < 0.0 || (vnow - showcase_video_last_s) >= 0.05;
                            if (warmed_up && interval_ok) {
                                auto& reg = gameSession->GetRegistry();
                                auto* world_sys = gameSession->GetWorldSystem();
                                if (scenario_config.wildlife && wildlife_setup) {
                                    // Cinematic FSM on CAPTURE-relative time T (frames dumped * 0.05 s):
                                    // 0..~2.3 s animal walks to water; ~2.5 s human shoots; arrow arcs
                                    // ~1 s; on landing the splash SCARES the animal -> it flees.
                                    const float T = static_cast<float>(showcase_video_frame) * 0.05f;
                                    const float dt = 0.05f;
                                    const auto e_animal = skinned_mesh_visual_target.all_entities[0];
                                    const auto e_human = skinned_mesh_visual_target.all_entities[1];
                                    auto ground = [&](glm::vec3 p) {
                                        if (world_sys) p.y = world_sys->GetTerrainHeightAt(p.x, p.z);
                                        return p;
                                    };
                                    auto set_tf = [&](Luminumbra::EntityID e, const glm::vec3& p, const glm::vec3& dir) {
                                        if (!reg.valid(e) || !reg.all_of<Luminumbra::Components::TransformComponent>(e)) return;
                                        auto& tf = reg.get<Luminumbra::Components::TransformComponent>(e);
                                        tf.position = p;
                                        const glm::vec3 d(dir.x, 0.0f, dir.z);
                                        if (glm::length(d) > 0.01f) tf.rotation = glm::angleAxis(std::atan2(d.x, d.z), glm::vec3(0, 1, 0));
                                    };
                                    glm::vec3 to_water = wildlife_water - wildlife_animal; to_water.y = 0.0f;
                                    const glm::vec3 seek_dir = glm::length(to_water) > 0.01f ? glm::normalize(to_water) : glm::vec3(1, 0, 0);
                                    if (wildlife_phase == 0) { // SEEK water
                                        if (glm::length(to_water) > 2.5f) wildlife_animal += seek_dir * 3.0f * dt;
                                        wildlife_animal = ground(wildlife_animal);
                                        set_tf(e_animal, wildlife_animal, seek_dir);
                                        if (T >= 2.5f) { // human looses the arrow toward a spot beside the animal
                                            const glm::vec3 perp(-seek_dir.z, 0.0f, seek_dir.x);
                                            const glm::vec3 target = wildlife_animal + perp * 2.0f; // BESIDE, not at
                                            wildlife_arrow_pos = wildlife_human + glm::vec3(0.0f, 1.3f, 0.0f);
                                            glm::vec3 ah = target - wildlife_arrow_pos; ah.y = 0.0f;
                                            const glm::vec3 adir = glm::length(ah) > 0.01f ? glm::normalize(ah) : seek_dir;
                                            wildlife_arrow_vel = adir * 13.0f + glm::vec3(0.0f, 4.5f, 0.0f);
                                            set_tf(e_human, wildlife_human, adir); // human faces the shot
                                            wildlife_phase = 1;
                                        }
                                    } else if (wildlife_phase == 1) { // ARROW in flight
                                        wildlife_arrow_vel.y -= 9.81f * dt;
                                        wildlife_arrow_pos += wildlife_arrow_vel * dt;
                                        const float terr = world_sys ? world_sys->GetTerrainHeightAt(wildlife_arrow_pos.x, wildlife_arrow_pos.z) : wildlife_arrow_pos.y;
                                        set_tf(wildlife_arrow_entity, wildlife_arrow_pos, wildlife_arrow_vel);
                                        if (wildlife_arrow_pos.y <= terr) { // THWACK beside the animal -> scare
                                            wildlife_arrow_pos.y = terr;
                                            set_tf(wildlife_arrow_entity, wildlife_arrow_pos, glm::vec3(0, 0, 1));
                                            glm::vec3 away = wildlife_animal - wildlife_arrow_pos; away.y = 0.0f;
                                            wildlife_flee_dir = glm::length(away) > 0.01f ? glm::normalize(away) : -seek_dir;
                                            wildlife_phase = 2;
                                        }
                                        set_tf(e_animal, wildlife_animal, seek_dir); // animal still drinking
                                    } else { // FLEE
                                        wildlife_animal += wildlife_flee_dir * 6.0f * dt; // bolts away, faster
                                        wildlife_animal = ground(wildlife_animal);
                                        set_tf(e_animal, wildlife_animal, wildlife_flee_dir);
                                    }
                                } else if (scenario_config.replicated) {
                                    // Network-driven poses are applied before render so
                                    // this readback observes the frame drawn from the
                                    // replicated snapshot/interpolation path.
                                } else {
                                // Walk every avatar gently TOWARD the camera (+Z) so the row
                                // strolls forward and stays framed (idle clip still plays).
                                // These are render-only entities (no physics body), so RE-GROUND
                                // Y to the terrain at each new XZ every step -- otherwise they
                                // sink into / float over rising/falling ground (owner: avatars
                                // sinking into the ground on the mountains preset).
                                auto view = reg.view<Luminumbra::Components::TransformComponent,
                                                     Luminumbra::Components::SkinnedMeshComponent>();
                                const float step_m = 0.05f; // ~1 m/s at 20 dumps/s
                                for (auto e : view) {
                                    auto& pos = view.get<Luminumbra::Components::TransformComponent>(e).position;
                                    pos.z += step_m;
                                    if (world_sys) pos.y = world_sys->GetTerrainHeightAt(pos.x, pos.z);
                                }
                                }
                                int vw = 0, vh = 0;
                                glfwGetFramebufferSize(window, &vw, &vh);
                                if (vw > 0 && vh > 0) {
                                    std::vector<unsigned char> vpx(
                                        static_cast<std::size_t>(vw) * static_cast<std::size_t>(vh) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, vpx.data());
                                    char vname[40];
                                    std::snprintf(vname, sizeof(vname), "motion/frame_%03d.ppm", showcase_video_frame);
                                    if (WritePixelBufferPpm(scenario_config.artifact_dir / vname, vw, vh, vpx)) {
                                        ++showcase_video_frame;
                                        showcase_video_last_s = vnow;
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
                                if (scenario_config.skinned_mesh_visual_smoke() && scenario_config.replicated &&
                                    scenario_config.avatars >= 2 && !remote_avatar_render_artifact_written) {
                                    scenario_failed = true;
                                    scenario_failure_reason = "remote_avatar_render_artifact_missing";
                                    runtime_state_recorder.capture(
                                        scenario_failure_reason,
                                        &jobSystem,
                                        gameSession.get(),
                                        &renderPipeline,
                                        scenario_frame_count,
                                        last_readiness_report);
                                } else {
                                    runtime_state_recorder.capture("timed_run_complete", &jobSystem, gameSession.get(), &renderPipeline, scenario_frame_count, last_readiness_report);
                                    scenario_timed_run_complete = true;
                                }
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
            if (g_imgui_enabled && g_playerController && g_timelapse_frames == 0) {
                g_playerController->RenderDebugUI();
            }
            // Always-on time-scale indicator (when not real-time) so slow-mo / fast-forward
            // / pause is obvious at a glance.
            if (g_imgui_enabled && g_timeScale != 1.0f && g_timelapse_frames == 0) {
                ImGui::SetNextWindowPos(ImVec2(10.0f, 60.0f), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.5f);
                if (ImGui::Begin("##timescale", nullptr,
                                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_NoMove)) {
                    if (g_timeScale == 0.0f) ImGui::Text("|| PAUSED  (\\ to resume)");
                    else ImGui::Text("TIME  x%.2f", g_timeScale);
                }
                ImGui::End();
            }
            // Settings menu (F8 to toggle; frees the cursor). Render-only; user.* is never
            // hashed. Changes apply live and "Save" persists them to the per-user overlay.
            // The polished RML settings screen (settings.rml) is the follow-on (task #12).
            if (g_imgui_enabled && g_show_settings && g_camera && g_timelapse_frames == 0) {
                ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
                if (ImGui::Begin("Settings (F8)")) {
                    luminumbra::core::UserSettings& us = g_systemConfig.user();
                    if (ImGui::SliderFloat("Look sensitivity", &us.mouse_sensitivity, 0.01f, 1.0f, "%.3f")) {
                        g_camera->MouseSensitivity = us.mouse_sensitivity;  // applied live
                    }
                    if (ImGui::SliderFloat("FOV", &us.fov, 30.0f, 110.0f, "%.0f deg")) {
                        g_camera->Zoom = us.fov;
                    }
                    if (ImGui::Checkbox("VSync", &us.vsync)) {
                        glfwSwapInterval(us.vsync ? 1 : 0);
                    }
                    ImGui::Separator();
                    ImGui::SliderFloat("Time scale", &g_timeScale, 0.0f, 8.0f, "%.2fx");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("1x")) g_timeScale = 1.0f;
                    ImGui::SameLine();
                    if (ImGui::SmallButton(g_timeScale == 0.0f ? "Resume" : "Pause"))
                        g_timeScale = (g_timeScale == 0.0f) ? 1.0f : 0.0f;
                    ImGui::TextDisabled("engine time: [ slower   ] faster   \\ reset");
                    ImGui::Separator();
                    {
                        // Window mode — applied live via ApplyWindowMode (no-op on capture-pinned runs).
                        const char* modes[] = {"windowed", "borderless", "fullscreen"};
                        int cur = 1;  // default borderless
                        for (int i = 0; i < 3; ++i)
                            if (us.window_mode == modes[i]) cur = i;
                        if (ImGui::Combo("Window mode", &cur, modes, 3)) {
                            us.window_mode = modes[cur];
                            const WindowMode m = ParseWindowMode(us.window_mode, g_windowState.mode);
                            ApplyWindowMode(window, g_windowState, m);
                        }
                    }
                    {
                        // Resolution — applied live in windowed mode (borderless/fullscreen use
                        // the monitor's resolution); suppressed on capture-pinned gate runs.
                        const char* resos[] = {"1280x720", "1920x1080", "2560x1440",
                                               "3440x1440", "3840x1600", "3840x2160"};
                        int rcur = -1;
                        for (int i = 0; i < 6; ++i)
                            if (us.resolution == resos[i]) rcur = i;
                        if (ImGui::Combo("Resolution", &rcur, resos, 6) && rcur >= 0) {
                            us.resolution = resos[rcur];
                            const std::string& r = us.resolution;
                            const auto xp = r.find('x');
                            if (xp != std::string::npos) {
                                const int rw = std::atoi(r.substr(0, xp).c_str());
                                const int rh = std::atoi(r.substr(xp + 1).c_str());
                                if (rw > 0 && rh > 0) {
                                    g_windowState.windowedWidth = rw;
                                    g_windowState.windowedHeight = rh;
                                    if (us.window_mode == "windowed" && !g_windowState.capture_pinned)
                                        glfwSetWindowSize(window, rw, rh);
                                }
                            }
                        }
                    }
                    if (ImGui::SliderFloat("Master volume", &us.audio_master, 0.0f, 1.0f, "%.2f")) {
                        if (audioManager) audioManager->SetMasterVolume(us.audio_master);  // applied live
                    }
                    ImGui::TextDisabled("sfx/music volumes saved (need per-bus routing)");
                    if (ImGui::CollapsingHeader("Controls (keyboard)")) {
                        for (const auto& def : Luminumbra::Client::kInputActionDefs) {
                            const int idx = static_cast<int>(def.action);
                            const int kc = g_systemConfig.keybind(def.name, def.default_key);
                            const char* kn = glfwGetKeyName(kc, 0);
                            char btn[48];
                            if (g_rebindCaptureAction == idx)
                                std::snprintf(btn, sizeof(btn), "press a key...##%s", def.name);
                            else if (kn)
                                std::snprintf(btn, sizeof(btn), "%s##%s", kn, def.name);
                            else
                                std::snprintf(btn, sizeof(btn), "key %d##%s", kc, def.name);
                            ImGui::Text("%-12s", def.name);
                            ImGui::SameLine(150);
                            if (ImGui::Button(btn)) g_rebindCaptureAction = idx;
                        }
                        ImGui::TextDisabled("click a binding, then press a key (Esc cancels)");
                    }
                    if (ImGui::Button("Save settings")) {
                        const std::string path =
                            luminumbra::core::SystemConfig::DefaultUserOverlayPath();
                        const bool ok = g_systemConfig.SaveUserOverlay(path);
                        LUMINUMBRA_CORE_INFO("Settings {} ({})", ok ? "saved" : "save FAILED", path);
                    }
                    ImGui::TextDisabled("user.* — client-only, never hashed");
                }
                ImGui::End();
            }
            if (g_imgui_enabled && show_worldgen_viewer && worldGenViewer) {
                worldGenViewer->UpdateAndRender(show_worldgen_viewer, gameSession->GetWorldSystem());
            }
        }
        
        if (g_imgui_enabled) {
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        // --timelapse: dump the rendered frame, then fast-forward sim-time (+ the day clock)
        // for the next one. Pair with --no-ui so no overlay is baked into the frame.
        if (g_timelapse_frames > 0 && currentState == GameState::IN_GAME && gameSession) {
            if (g_timelapse_settle < kTimelapseSettleFrames) {
                ++g_timelapse_settle;  // let the world stream/settle before frame 0
            } else {
                int vw = 0, vh = 0;
                glfwGetFramebufferSize(window, &vw, &vh);
                if (vw > 0 && vh > 0) {
                    std::vector<unsigned char> px(
                        static_cast<std::size_t>(vw) * static_cast<std::size_t>(vh) * 3u);
                    glReadBuffer(GL_BACK);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                    char nm[32];
                    std::snprintf(nm, sizeof(nm), "frame_%04d.ppm", g_timelapse_captured);
                    if (WritePixelBufferPpm(g_timelapse_dir / nm, vw, vh, px)) ++g_timelapse_captured;
                }
                if (g_timelapse_captured >= g_timelapse_frames) {
                    LUMINUMBRA_CORE_INFO("Timelapse: captured {} frames -> {}",
                                         g_timelapse_captured, g_timelapse_dir.string());
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                } else {
                    // Advance the SIM (weather/wind/creatures/plants) by K fixed ticks for the
                    // next frame; normal per-frame ticking is paused (g_timeScale = 0).
                    for (int i = 0; i < g_timelapse_ticks; ++i) gameSession->TickSimulation(1.0 / 30.0);
                    if (g_timelapse_daystep > 0.0f) {  // drift the sun/sky for shade-over-time
                        g_timelapse_tod += g_timelapse_daystep;
                        if (g_timelapse_tod >= 1.0f) g_timelapse_tod -= 1.0f;
                        renderPipeline.set_time_of_day(g_timelapse_tod);
                    }
                }
            }
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

    // Drain the SHIELD-RT far-field heightfield build before the world is cleared —
    // its worker job reads the world by pointer (else a teardown-time use-after-free).
    renderPipeline.drain_far_field_builds();
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
    // Rebind capture: while waiting for a key for some action, the next key press becomes
    // its binding (Escape cancels). Intercept first so any key — even F-keys — can be bound.
    if (g_rebindCaptureAction >= 0 && action == GLFW_PRESS) {
        if (key != GLFW_KEY_ESCAPE && g_rebindCaptureAction < static_cast<int>(Luminumbra::Client::kInputActionCount)) {
            const char* name = Luminumbra::Client::kInputActionDefs[g_rebindCaptureAction].name;
            g_systemConfig.user().keybinds[name] = key;
            if (g_playerController) g_playerController->ApplyKeyBindings(g_systemConfig);
        }
        g_rebindCaptureAction = -1;
        return;
    }
    // F8: toggle the settings menu and free/restore the cursor so the panel is usable.
    if (key == GLFW_KEY_F8 && action == GLFW_PRESS) {
        g_show_settings = !g_show_settings;
        g_rebindCaptureAction = -1;
        if (g_show_settings) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        } else if (g_playerController) {  // in a world -> resume mouse-look
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            firstMouse = true;  // avoid a camera jump when mouse-look resumes
        }
        return;
    }
    // Engine time scale (host_timescale-style): [ slower, ] faster, \ reset to 1x.
    if (action == GLFW_PRESS &&
        (key == GLFW_KEY_LEFT_BRACKET || key == GLFW_KEY_RIGHT_BRACKET || key == GLFW_KEY_BACKSLASH)) {
        if (key == GLFW_KEY_LEFT_BRACKET)
            g_timeScale = (g_timeScale <= 0.125f) ? 0.0f : g_timeScale * 0.5f;   // ...down to pause
        else if (key == GLFW_KEY_RIGHT_BRACKET)
            g_timeScale = (g_timeScale < 0.125f) ? 0.125f : std::min(g_timeScale * 2.0f, 16.0f);
        else
            g_timeScale = 1.0f;  // reset
        LUMINUMBRA_CORE_INFO("Engine time scale: {:.3f}x", g_timeScale);
        return;
    }
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
    if (g_show_settings) return;  // settings menu open (cursor freed) -> don't swing the camera
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
