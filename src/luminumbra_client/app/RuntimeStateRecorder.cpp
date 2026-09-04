#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "app/RuntimeStateRecorder.h"

#include "core/Log.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "rendering/Camera.h"
#include "rendering/FarLodSystem.h" // farlod()->stats() in build_state_json
#include "rendering/RenderPipeline.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <memory>
#include <system_error>
#include <utility>

#if defined(_WIN32)
// Windows.h must precede Psapi.h (Psapi declares against windef types and does
// not include them itself). The blank line keeps clang-format from re-sorting
// the two into alphabetical — and broken — order.
#include <Windows.h>

#include <Psapi.h>
#endif

using namespace Luminumbra::Client::ScenarioHarness;

namespace Luminumbra::Client::App {

ProcessMemoryStats QueryProcessMemoryStats() {
    ProcessMemoryStats stats;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
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

RuntimeReadinessReport EvaluateReadiness(const RuntimeScenarioConfig& config,
                                         Luminumbra::world::GameSession* game_session) {
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

bool MemoryWatermarkExceeded(const RuntimeScenarioConfig& config,
                             ProcessMemoryStats* out_memory,
                             uint64_t* out_measured_bytes) {
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

RuntimeStateRecorder::RuntimeStateRecorder(RuntimeScenarioConfig config,
                                           std::unique_ptr<Luminumbra::Rendering::Camera>& g_camera)
    : m_config(std::move(config))
    , g_camera(g_camera)
    , m_started_at(std::chrono::steady_clock::now()) {
    std::error_code ec;
    std::filesystem::create_directories(m_config.artifact_dir, ec);
}

const std::filesystem::path& RuntimeStateRecorder::artifact_dir() const {
    return m_config.artifact_dir;
}

const std::filesystem::path& RuntimeStateRecorder::crash_dir() const {
    return m_config.crash_dir;
}

void RuntimeStateRecorder::capture(const std::string& phase,
                                   const Luminumbra::JobSystem* job_system,
                                   Luminumbra::world::GameSession* game_session,
                                   const Luminumbra::Rendering::RenderPipeline* render_pipeline,
                                   uint64_t frame_count,
                                   const RuntimeReadinessReport& readiness) {
    m_last_known =
        build_state_json(phase, job_system, game_session, render_pipeline, frame_count, readiness);
    write_last_known();
}

void RuntimeStateRecorder::write_memory_watermark(const std::string& phase,
                                                  const ProcessMemoryStats& memory,
                                                  uint64_t measured_bytes) {
    std::error_code ec;
    std::filesystem::create_directories(m_config.artifact_dir, ec);
    nlohmann::json artifact = {{"schema", "luminumbra.memory_watermark.v1"},
                               {"timestamp_utc", TimestampUtc()},
                               {"phase", phase},
                               {"watermark_mb", m_config.memory_watermark_mb},
                               {"measured_bytes", measured_bytes},
                               {"memory", MemoryToJson(memory)}};
    std::ofstream output(m_config.artifact_dir / "memory-watermark.json");
    output << std::setw(2) << artifact << '\n';
}

void RuntimeStateRecorder::write_shutdown(const std::vector<std::string>& milestones,
                                          const Luminumbra::JobSystem::RuntimeStats& job_stats) {
    std::error_code ec;
    std::filesystem::create_directories(m_config.artifact_dir, ec);
    nlohmann::json artifact = {
        {"schema", "luminumbra.shutdown.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"scenario", m_config.scenario},
        {"milestones", milestones},
        {"job_queue", JobStatsToJson(job_stats)},
        {"jobs_drained",
         job_stats.queue_depth == 0 && job_stats.worker_count == 0 && !job_stats.accepting_jobs}};
    std::ofstream output(m_config.artifact_dir / "shutdown.json");
    output << std::setw(2) << artifact << '\n';
}

void RuntimeStateRecorder::mark_unhandled_exception(uint32_t exception_code) {
    m_last_known["phase"] = "unhandled_exception";
    m_last_known["exception_code"] = exception_code;
    m_last_known["timestamp_utc"] = TimestampUtc();
    write_last_known();
}

nlohmann::json RuntimeStateRecorder::MemoryToJson(const ProcessMemoryStats& memory) {
    return {{"working_set_bytes", memory.working_set_bytes},
            {"peak_working_set_bytes", memory.peak_working_set_bytes},
            {"private_bytes", memory.private_bytes},
            {"pagefile_bytes", memory.pagefile_bytes},
            {"total_physical_bytes", memory.total_physical_bytes},
            {"available_physical_bytes", memory.available_physical_bytes}};
}

nlohmann::json
RuntimeStateRecorder::JobStatsToJson(const Luminumbra::JobSystem::RuntimeStats& stats) {
    return {{"worker_count", stats.worker_count},
            // Total across both priority lanes (validators read this field).
            {"queue_depth", stats.queue_depth},
            {"high_priority_queue_depth", stats.high_priority_queue_depth},
            {"normal_priority_queue_depth", stats.normal_priority_queue_depth},
            {"accepting_jobs", stats.accepting_jobs},
            {"stop_requested", stats.stop_requested}};
}

nlohmann::json RuntimeStateRecorder::ChunkStatsToJson(
    const Luminumbra::Systems::SHIELD_WorldSystem::RuntimeChunkStats& stats) {
    return {{"total_chunks", stats.total_chunks},
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
            {"meshing_job_active", stats.meshing_job_active}};
}

nlohmann::json RuntimeStateRecorder::UploadStatsToJson(
    const Luminumbra::Rendering::RenderPipeline::MeshUploadFrameStats& stats) {
    return {{"snapshot_count", stats.snapshot_count},
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
            {"water_nearest_deferred_distance_sq", stats.water_nearest_deferred_distance_sq}};
}

nlohmann::json RuntimeStateRecorder::RenderPassStatsToJson(
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& stats) {
    return {{"snapshot_count", stats.snapshot_count},
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
            {"shadow_indices_drawn", stats.shadow_indices_drawn}};
}

nlohmann::json RuntimeStateRecorder::RenderRuntimeStatsToJson(
    const Luminumbra::Rendering::RenderPipeline::RuntimeRenderStats& stats) {
    return {{"started", stats.started},
            {"terrain_gpu_chunks", stats.terrain_gpu_chunks},
            {"water_gpu_chunks", stats.water_gpu_chunks},
            {"free_terrain_slots", stats.free_terrain_slots},
            {"free_water_slots", stats.free_water_slots},
            {"terrain_vertex_capacity", stats.terrain_vertex_capacity},
            {"terrain_index_capacity", stats.terrain_index_capacity},
            {"water_vertex_capacity", stats.water_vertex_capacity},
            {"water_index_capacity", stats.water_index_capacity},
            {"estimated_vram_bytes", stats.estimated_vram_bytes},
            {"shader_health",
             {{"geometry", stats.geometry_shader_ok},
              {"lighting", stats.lighting_shader_ok},
              {"skybox", stats.skybox_shader_ok},
              {"shadow", stats.shadow_shader_ok},
              {"ssao", stats.ssao_shader_ok},
              {"ssao_blur", stats.ssao_blur_shader_ok},
              {"water", stats.water_shader_ok},
              {"instanced_static_mesh", stats.instanced_static_mesh_shader_ok}}}};
}

nlohmann::json RuntimeStateRecorder::CoverageStatsToJson(
    const Luminumbra::Systems::SHIELD_WorldSystem::CameraLocalCoverageStats& stats) {
    return {{"camera_position", Vec3ToJson(stats.camera_position)},
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
            {"lod_counts",
             {{"lod0", stats.lod_counts[0]},
              {"lod1", stats.lod_counts[1]},
              {"lod2", stats.lod_counts[2]},
              {"unknown", stats.lod_unknown_chunks}}},
            {"center_chunk_present", stats.center_chunk_present},
            {"center_chunk_renderable", stats.center_chunk_renderable},
            {"near_field_renderable", stats.near_field_renderable}};
}

nlohmann::json RuntimeStateRecorder::GLDebugStatsToJson(const GLDebugRuntimeStats& stats) {
    return {{"messages", stats.messages},
            {"errors", stats.errors},
            {"warnings", stats.warnings},
            {"notifications", stats.notifications}};
}

nlohmann::json
RuntimeStateRecorder::build_state_json(const std::string& phase,
                                       const Luminumbra::JobSystem* job_system,
                                       Luminumbra::world::GameSession* game_session,
                                       const Luminumbra::Rendering::RenderPipeline* render_pipeline,
                                       uint64_t frame_count,
                                       const RuntimeReadinessReport& readiness) const {
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started_at).count();
    const ProcessMemoryStats memory = QueryProcessMemoryStats();

    nlohmann::json state = {
        {"schema", "luminumbra.runtime_state.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"phase", phase},
        {"scenario", m_config.scenario},
        {"elapsed_seconds", elapsed},
        {"frame_count", frame_count},
        {"launch_flags",
         {{"auto_create_world", m_config.auto_create_world},
          {"auto_enter_world", m_config.auto_enter_world},
          {"timed_run_seconds", m_config.timed_run_seconds},
          {"coverage_radius", m_config.coverage_radius},
          {"no_audio", m_config.no_audio},
          {"audio_telemetry_path", m_config.audio_telemetry_path.generic_string()},
          {"no_ui", m_config.no_ui},
          {"hidden_window", m_config.hidden_window},
          {"memory_watermark_mb", m_config.memory_watermark_mb}}},
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
        {"readiness",
         {{"ready", readiness.ready},
          {"reasons", readiness.reasons},
          {"timeout_seconds", m_config.readiness_timeout_seconds},
          {"min_renderable_chunks", m_config.min_renderable_chunks},
          {"min_collision_chunks", m_config.min_collision_chunks}}}};

    if (job_system) {
        state["job_queue"] = JobStatsToJson(job_system->get_runtime_stats());
    }

    if (game_session) {
        const auto& metadata = game_session->GetMetadata();
        state["world"] = {{"name", metadata.name},
                          {"seed", metadata.seed},
                          {"world_type", metadata.worldType},
                          {"world_id", metadata.worldId},
                          {"spawn_point", Vec3ToJson(metadata.spawnPoint)}};
        if (auto* world_system = game_session->GetWorldSystem()) {
            (void)world_system->get_renderable_chunks();
            state["chunk_states"] = ChunkStatsToJson(world_system->get_runtime_chunk_stats());
            if (g_camera) {
                const auto coverage = world_system->get_camera_local_coverage_stats(
                    g_camera->Position, m_config.coverage_radius);
                state["camera"] = {{"position", Vec3ToJson(g_camera->Position)},
                                   {"chunk", IVec3ToJson(coverage.camera_chunk)},
                                   {"terrain_height", coverage.terrain_height_under_camera},
                                   {"height_above_terrain", coverage.camera_height_above_terrain}};
                state["camera_local_coverage"] = CoverageStatsToJson(coverage);
            }
            const auto& streaming = world_system->get_last_streaming_budget_stats();
            state["streaming"] = {{"target_render_radius", streaming.target_render_radius},
                                  {"generation_budget", streaming.generation_budget},
                                  {"meshing_budget", streaming.meshing_budget},
                                  {"scheduled_generation", streaming.scheduled_generation},
                                  {"deferred_generation", streaming.deferred_generation},
                                  {"scheduled_meshing", streaming.scheduled_meshing},
                                  {"deferred_meshing", streaming.deferred_meshing},
                                  {"unloaded_chunks", streaming.unloaded_chunks},
                                  {"generation_job_active", streaming.generation_job_active},
                                  {"meshing_job_active", streaming.meshing_job_active}};
        }
    }

    if (render_pipeline) {
        const auto runtime_render = render_pipeline->get_runtime_render_stats();
        const auto render_json = RenderRuntimeStatsToJson(runtime_render);
        state["render_runtime"] = render_json;
        state["estimated_vram_bytes"] = runtime_render.estimated_vram_bytes;
        state["shader_health"] = render_json["shader_health"];
        // Capture-pin protection: record the active
        // window mode + the live render-target size so the offline gate can
        // hard-fail if a capture-mode run ever drifted off the pinned size.
        state["capture_pin"] = Luminumbra::Client::ScenarioHarness::CapturePinMetadata(
            m_config.window_mode,
            static_cast<int>(render_pipeline->screen_width()),
            static_cast<int>(render_pipeline->screen_height()));
        state["resize_generation"] = render_pipeline->resize_generation();
        state["upload_queue"] = UploadStatsToJson(render_pipeline->get_last_mesh_upload_stats());
        state["render_pass"] = RenderPassStatsToJson(render_pipeline->get_last_render_pass_stats());
        // Far-LOD scheduler telemetry (, FarLodHorizon gate inputs).
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
                {"farlod_evictions_total", farlod_stats.evictions_total},
                //  per-frame scheduler diagnostics for the mountains residency trace.
                {"farlod_builds_dispatched", farlod_stats.builds_dispatched},
                {"farlod_builds_integrated_ok", farlod_stats.builds_integrated_ok},
                {"farlod_builds_integrated_failed", farlod_stats.builds_integrated_failed},
                {"farlod_builds_failed_total", farlod_stats.builds_failed_total},
                {"farlod_evictions_this_frame", farlod_stats.evictions_this_frame},
                {"farlod_pending_depth", farlod_stats.pending_depth}};
        }
    }

    return state;
}

void RuntimeStateRecorder::write_last_known() const {
    std::error_code ec;
    std::filesystem::create_directories(m_config.artifact_dir, ec);
    std::ofstream output(m_config.artifact_dir / "last-known-runtime.json");
    output << std::setw(2) << m_last_known << '\n';
}

RuntimeScenarioFrameRecorder::RuntimeScenarioFrameRecorder(
    bool enabled,
    int coverage_radius,
    std::filesystem::path output_dir,
    std::unique_ptr<Luminumbra::Rendering::Camera>& g_camera)
    : m_enabled(enabled)
    , m_coverage_radius(std::max(0, coverage_radius))
    , m_output_dir(std::move(output_dir))
    , g_camera(g_camera)
    , m_started_at(std::chrono::steady_clock::now()) {}

bool RuntimeScenarioFrameRecorder::enabled() const {
    return m_enabled;
}

void RuntimeScenarioFrameRecorder::record_frame(
    float delta_time,
    Luminumbra::world::GameSession* game_session,
    const Luminumbra::Rendering::RenderPipeline& render_pipeline,
    uint64_t frame_count) {
    if (!m_enabled || !game_session || !g_camera) {
        return;
    }

    auto* world_system = game_session->GetWorldSystem();
    if (!world_system) {
        return;
    }

    const auto coverage =
        world_system->get_camera_local_coverage_stats(g_camera->Position, m_coverage_radius);
    const auto& upload = render_pipeline.get_last_mesh_upload_stats();
    const auto& passes = render_pipeline.get_last_render_pass_stats();
    const auto gl_debug = CurrentGLDebugRuntimeStats();

    RuntimeScenarioFrameSample sample;
    sample.frame = frame_count;
    sample.elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started_at).count();
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

bool RuntimeScenarioFrameRecorder::write_artifacts() const {
    if (!m_enabled) {
        return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(m_output_dir, ec);
    if (ec) {
        LUMINUMBRA_CORE_ERROR("Failed to create runtime scenario frame directory '{}': {}",
                              m_output_dir.string(),
                              ec.message());
        return false;
    }

    return write_json(m_output_dir / "runtime-frames.json") &&
           write_csv(m_output_dir / "runtime-frames.csv");
}

bool RuntimeScenarioFrameRecorder::write_json(const std::filesystem::path& path) const {
    nlohmann::json frames = nlohmann::json::array();
    for (const RuntimeScenarioFrameSample& sample : m_samples) {
        frames.push_back(
            {{"frame", sample.frame},
             {"elapsed_seconds", sample.elapsed_seconds},
             {"delta_ms", sample.delta_ms},
             {"camera_position", Vec3ToJson(sample.camera_position)},
             {"terrain_height", sample.terrain_height},
             {"height_above_terrain", sample.height_above_terrain},
             {"coverage",
              {{"expected_surface_chunks", sample.expected_surface_chunks},
               {"missing_surface_chunks", sample.missing_surface_chunks},
               {"renderable_surface_chunks", sample.renderable_surface_chunks},
               {"pending_lod_chunks", sample.pending_lod_chunks},
               {"near_field_renderable", sample.near_field_renderable}}},
             {"render_pass", {{"terrain_visible_chunks", sample.terrain_visible_chunks}}},
             {"upload_queue",
              {{"terrain_upload_candidates", sample.terrain_upload_candidates},
               {"terrain_uploads", sample.terrain_uploads},
               {"terrain_uploads_deferred", sample.terrain_uploads_deferred},
               {"terrain_stale_upload_candidates", sample.terrain_stale_upload_candidates},
               {"terrain_stale_uploads_deferred", sample.terrain_stale_uploads_deferred},
               {"terrain_deferred_nearer_than_selected",
                sample.terrain_deferred_nearer_than_selected},
               {"water_upload_candidates", sample.water_upload_candidates},
               {"water_uploads", sample.water_uploads},
               {"water_uploads_deferred", sample.water_uploads_deferred},
               {"water_stale_upload_candidates", sample.water_stale_upload_candidates},
               {"water_stale_uploads_deferred", sample.water_stale_uploads_deferred},
               {"water_deferred_nearer_than_selected",
                sample.water_deferred_nearer_than_selected}}},
             {"gl_debug_errors", sample.gl_debug_errors}});
    }

    nlohmann::json artifact = {{"schema", "luminumbra.runtime_frames.v1"},
                               {"timestamp_utc", TimestampUtc()},
                               {"coverage_radius", m_coverage_radius},
                               {"frames_recorded", m_samples.size()},
                               {"frames", frames}};

    std::ofstream output(path);
    if (!output) {
        LUMINUMBRA_CORE_ERROR("Failed to write runtime frame JSON: {}", path.string());
        return false;
    }
    output << std::setw(2) << artifact << '\n';
    return true;
}

bool RuntimeScenarioFrameRecorder::write_csv(const std::filesystem::path& path) const {
    std::ofstream output(path);
    if (!output) {
        LUMINUMBRA_CORE_ERROR("Failed to write runtime frame CSV: {}", path.string());
        return false;
    }

    output << "frame,elapsed_seconds,delta_ms,camera_x,camera_y,camera_z,terrain_height,height_"
              "above_terrain,expected_surface_chunks,missing_surface_chunks,renderable_surface_"
              "chunks,pending_lod_chunks,near_field_renderable,terrain_visible_chunks,terrain_"
              "upload_candidates,terrain_uploads,terrain_uploads_deferred,terrain_stale_upload_"
              "candidates,terrain_stale_uploads_deferred,terrain_deferred_nearer_than_selected,"
              "water_upload_candidates,water_uploads,water_uploads_deferred,water_stale_upload_"
              "candidates,water_stale_uploads_deferred,water_deferred_nearer_than_selected,gl_"
              "debug_errors\n";
    for (const RuntimeScenarioFrameSample& sample : m_samples) {
        output << sample.frame << ',' << sample.elapsed_seconds << ',' << sample.delta_ms << ','
               << sample.camera_position.x << ',' << sample.camera_position.y << ','
               << sample.camera_position.z << ',' << sample.terrain_height << ','
               << sample.height_above_terrain << ',' << sample.expected_surface_chunks << ','
               << sample.missing_surface_chunks << ',' << sample.renderable_surface_chunks << ','
               << sample.pending_lod_chunks << ',' << (sample.near_field_renderable ? 1 : 0) << ','
               << sample.terrain_visible_chunks << ',' << sample.terrain_upload_candidates << ','
               << sample.terrain_uploads << ',' << sample.terrain_uploads_deferred << ','
               << sample.terrain_stale_upload_candidates << ','
               << sample.terrain_stale_uploads_deferred << ','
               << sample.terrain_deferred_nearer_than_selected << ','
               << sample.water_upload_candidates << ',' << sample.water_uploads << ','
               << sample.water_uploads_deferred << ',' << sample.water_stale_upload_candidates
               << ',' << sample.water_stale_uploads_deferred << ','
               << sample.water_deferred_nearer_than_selected << ',' << sample.gl_debug_errors
               << '\n';
    }
    return true;
}

RuntimeBootMetricsRecorder::RuntimeBootMetricsRecorder(bool enabled,
                                                       int target_frames,
                                                       std::filesystem::path output_dir)
    : m_enabled(enabled)
    , m_target_frames(std::max(1, target_frames))
    , m_output_dir(std::move(output_dir)) {
    if (m_enabled) {
        m_frames.reserve(static_cast<size_t>(m_target_frames));
        m_started_at = std::chrono::steady_clock::now();
    }
}

bool RuntimeBootMetricsRecorder::enabled() const {
    return m_enabled;
}

bool RuntimeBootMetricsRecorder::complete() const {
    return m_enabled && static_cast<int>(m_frames.size()) >= m_target_frames;
}

void RuntimeBootMetricsRecorder::record_frame(
    float delta_time, const Luminumbra::Rendering::RenderPipeline& render_pipeline) {
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

bool RuntimeBootMetricsRecorder::write_artifacts() const {
    if (!m_enabled) {
        return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(m_output_dir, ec);
    if (ec) {
        LUMINUMBRA_CORE_ERROR("Failed to create runtime boot metrics directory '{}': {}",
                              m_output_dir.string(),
                              ec.message());
        return false;
    }

    const std::filesystem::path json_path = m_output_dir / "runtime_boot.json";
    const std::filesystem::path csv_path = m_output_dir / "runtime_boot.csv";
    return write_json(json_path) && write_csv(csv_path);
}

// Takes an ALREADY-SORTED vector by const reference. The previous
// by-value-copy-then-sort signature tripped a GCC 15 -O3
// -Wfree-nonheap-object false positive when the inlined copy's
// deallocation was folded (release lane, ); sorting once at the
// call site also avoids three copies/sorts of the frame-time vector.
double RuntimeBootMetricsRecorder::percentile_sorted(const std::vector<double>& sorted_values,
                                                     double pct) {
    if (sorted_values.empty()) {
        return 0.0;
    }
    const double position = pct * static_cast<double>(sorted_values.size() - 1);
    const auto index = static_cast<size_t>(std::round(position));
    return sorted_values[std::min(index, sorted_values.size() - 1)];
}

std::vector<double> RuntimeBootMetricsRecorder::frame_times_ms() const {
    std::vector<double> values;
    values.reserve(m_frames.size());
    for (const auto& frame : m_frames) {
        values.push_back(frame.delta_ms);
    }
    return values;
}

bool RuntimeBootMetricsRecorder::write_json(const std::filesystem::path& path) const {
    std::ofstream output(path);
    if (!output) {
        LUMINUMBRA_CORE_ERROR("Failed to write runtime boot metrics JSON: {}", path.string());
        return false;
    }

    std::vector<double> deltas = frame_times_ms();
    std::sort(deltas.begin(), deltas.end());
    const auto elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_started_at)
            .count();
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
    const size_t final_window_start =
        m_frames.size() > kFinalWindowFrames ? m_frames.size() - kFinalWindowFrames : 0;

    for (size_t i = 0; i < m_frames.size(); ++i) {
        const auto& frame = m_frames[i];
        max_snapshots = std::max(max_snapshots, frame.snapshots);
        max_terrain_visible_chunks =
            std::max(max_terrain_visible_chunks, frame.terrain_visible_chunks);
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
        peak_terrain_uploads_deferred =
            std::max(peak_terrain_uploads_deferred, frame.terrain_uploads_deferred);
        peak_water_uploads_deferred =
            std::max(peak_water_uploads_deferred, frame.water_uploads_deferred);
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
    output << "    \"total_culling_hierarchy_rebuilds\": " << total_culling_hierarchy_rebuilds
           << ",\n";
    output << "    \"frames_with_culling_hierarchy_rebuilds\": "
           << frames_with_culling_hierarchy_rebuilds << ",\n";
    output << "    \"total_terrain_upload_candidates\": " << total_terrain_upload_candidates
           << ",\n";
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
    output << "    \"final_window_frames\": " << std::min(kFinalWindowFrames, m_frames.size())
           << ",\n";
    output << "    \"final_window_terrain_uploads_deferred\": "
           << final_window_terrain_uploads_deferred << ",\n";
    output << "    \"final_window_water_uploads_deferred\": " << final_window_water_uploads_deferred
           << "\n";
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

bool RuntimeBootMetricsRecorder::write_csv(const std::filesystem::path& path) const {
    std::ofstream output(path);
    if (!output) {
        LUMINUMBRA_CORE_ERROR("Failed to write runtime boot metrics CSV: {}", path.string());
        return false;
    }

    output << "frame,delta_ms,snapshots,terrain_visible_chunks,terrain_draws,terrain_indices_"
              "drawn,shadow_draws,shadow_indices_drawn,culling_hierarchy_rebuilds,terrain_upload_"
              "candidates,terrain_uploads,terrain_uploads_deferred,terrain_payload_bytes,terrain_"
              "slots_created,terrain_slots_reused,terrain_slots_grown,terrain_upload_failures,"
              "water_upload_candidates,water_uploads,water_uploads_deferred,water_payload_bytes,"
              "water_slots_created,water_slots_reused,water_slots_grown,water_upload_failures\n";
    for (const auto& frame : m_frames) {
        output << frame.frame << ',' << frame.delta_ms << ',' << frame.snapshots << ','
               << frame.terrain_visible_chunks << ',' << frame.terrain_draws << ','
               << frame.terrain_indices_drawn << ',' << frame.shadow_draws << ','
               << frame.shadow_indices_drawn << ',' << frame.culling_hierarchy_rebuilds << ','
               << frame.terrain_upload_candidates << ',' << frame.terrain_uploads << ','
               << frame.terrain_uploads_deferred << ',' << frame.terrain_payload_bytes << ','
               << frame.terrain_slots_created << ',' << frame.terrain_slots_reused << ','
               << frame.terrain_slots_grown << ',' << frame.terrain_upload_failures << ','
               << frame.water_upload_candidates << ',' << frame.water_uploads << ','
               << frame.water_uploads_deferred << ',' << frame.water_payload_bytes << ','
               << frame.water_slots_created << ',' << frame.water_slots_reused << ','
               << frame.water_slots_grown << ',' << frame.water_upload_failures << '\n';
    }
    return true;
}

} // namespace Luminumbra::Client::App
