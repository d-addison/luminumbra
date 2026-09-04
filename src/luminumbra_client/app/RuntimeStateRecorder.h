#pragma once

// Runtime scenario state/telemetry recorders for the client app, extracted
// verbatim from main_client.cpp: the last-known-runtime / shutdown / memory
// watermark artifact writer (RuntimeStateRecorder), the readiness evaluation
// the timed scenario gates poll, and the per-frame / boot-window metrics
// recorders that feed the runtime-frames and runtime-boot artifacts.

#include "core/RuntimeScenarioConfig.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h" // nested RuntimeChunkStats / coverage stats
#include "rendering/RenderPipeline.h" // nested MeshUpload/RenderPass/RuntimeRender stats

#include "nlohmann/json.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {
class Camera;
}

namespace Luminumbra::world {
class GameSession;
}

namespace Luminumbra::Client::App {

// The moved code names the scenario-harness vocabulary unqualified, exactly as
// it did inside main_client.cpp (which has a file-level using-directive).
using ScenarioHarness::GLDebugRuntimeStats;
using ScenarioHarness::RuntimeScenarioConfig;

struct ProcessMemoryStats {
    uint64_t working_set_bytes = 0;
    uint64_t peak_working_set_bytes = 0;
    uint64_t private_bytes = 0;
    uint64_t pagefile_bytes = 0;
    uint64_t total_physical_bytes = 0;
    uint64_t available_physical_bytes = 0;
};

ProcessMemoryStats QueryProcessMemoryStats();

struct RuntimeReadinessReport {
    bool ready = false;
    std::vector<std::string> reasons;
};

RuntimeReadinessReport EvaluateReadiness(const RuntimeScenarioConfig& config,
                                         Luminumbra::world::GameSession* game_session);

bool MemoryWatermarkExceeded(const RuntimeScenarioConfig& config,
                             ProcessMemoryStats* out_memory,
                             uint64_t* out_measured_bytes);

class RuntimeStateRecorder {
public:
    RuntimeStateRecorder(RuntimeScenarioConfig config,
                         std::unique_ptr<Luminumbra::Rendering::Camera>& g_camera);

    const std::filesystem::path& artifact_dir() const;
    const std::filesystem::path& crash_dir() const;

    void capture(const std::string& phase,
                 const Luminumbra::JobSystem* job_system,
                 Luminumbra::world::GameSession* game_session,
                 const Luminumbra::Rendering::RenderPipeline* render_pipeline,
                 uint64_t frame_count,
                 const RuntimeReadinessReport& readiness);

    void write_memory_watermark(const std::string& phase,
                                const ProcessMemoryStats& memory,
                                uint64_t measured_bytes);

    void write_shutdown(const std::vector<std::string>& milestones,
                        const Luminumbra::JobSystem::RuntimeStats& job_stats);

    void mark_unhandled_exception(uint32_t exception_code);

private:
    static nlohmann::json MemoryToJson(const ProcessMemoryStats& memory);
    static nlohmann::json JobStatsToJson(const Luminumbra::JobSystem::RuntimeStats& stats);
    static nlohmann::json
    ChunkStatsToJson(const Luminumbra::Systems::SHIELD_WorldSystem::RuntimeChunkStats& stats);
    static nlohmann::json
    UploadStatsToJson(const Luminumbra::Rendering::RenderPipeline::MeshUploadFrameStats& stats);
    static nlohmann::json
    RenderPassStatsToJson(const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& stats);
    static nlohmann::json RenderRuntimeStatsToJson(
        const Luminumbra::Rendering::RenderPipeline::RuntimeRenderStats& stats);
    static nlohmann::json CoverageStatsToJson(
        const Luminumbra::Systems::SHIELD_WorldSystem::CameraLocalCoverageStats& stats);
    static nlohmann::json GLDebugStatsToJson(const GLDebugRuntimeStats& stats);

    nlohmann::json build_state_json(const std::string& phase,
                                    const Luminumbra::JobSystem* job_system,
                                    Luminumbra::world::GameSession* game_session,
                                    const Luminumbra::Rendering::RenderPipeline* render_pipeline,
                                    uint64_t frame_count,
                                    const RuntimeReadinessReport& readiness) const;

    void write_last_known() const;

    RuntimeScenarioConfig m_config;
    std::unique_ptr<Luminumbra::Rendering::Camera>& g_camera;
    std::chrono::steady_clock::time_point m_started_at{};
    nlohmann::json m_last_known = {{"schema", "luminumbra.runtime_state.v1"},
                                   {"phase", "not_started"},
                                   {"timestamp_utc", ScenarioHarness::TimestampUtc()}};
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
    RuntimeScenarioFrameRecorder(bool enabled,
                                 int coverage_radius,
                                 std::filesystem::path output_dir,
                                 std::unique_ptr<Luminumbra::Rendering::Camera>& g_camera);

    bool enabled() const;

    void record_frame(float delta_time,
                      Luminumbra::world::GameSession* game_session,
                      const Luminumbra::Rendering::RenderPipeline& render_pipeline,
                      uint64_t frame_count);

    bool write_artifacts() const;

private:
    bool write_json(const std::filesystem::path& path) const;
    bool write_csv(const std::filesystem::path& path) const;

    bool m_enabled = false;
    int m_coverage_radius = 0;
    std::filesystem::path m_output_dir;
    std::unique_ptr<Luminumbra::Rendering::Camera>& g_camera;
    std::chrono::steady_clock::time_point m_started_at{};
    std::vector<RuntimeScenarioFrameSample> m_samples;
};

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
    RuntimeBootMetricsRecorder(bool enabled, int target_frames, std::filesystem::path output_dir);

    bool enabled() const;
    bool complete() const;

    void record_frame(float delta_time,
                      const Luminumbra::Rendering::RenderPipeline& render_pipeline);

    bool write_artifacts() const;

private:
    static double percentile_sorted(const std::vector<double>& sorted_values, double pct);

    std::vector<double> frame_times_ms() const;

    bool write_json(const std::filesystem::path& path) const;
    bool write_csv(const std::filesystem::path& path) const;

    bool m_enabled = false;
    int m_target_frames = 300;
    std::filesystem::path m_output_dir;
    std::chrono::steady_clock::time_point m_started_at{};
    std::vector<RuntimeBootFrameMetrics> m_frames;
};

} // namespace Luminumbra::Client::App
