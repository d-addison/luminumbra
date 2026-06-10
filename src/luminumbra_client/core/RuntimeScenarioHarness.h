#pragma once

#include "rendering/RenderPipeline.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "nlohmann/json.hpp"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace Luminumbra::world { class GameSession; }
namespace Luminumbra::Rendering { class Camera; }

namespace Luminumbra::Client::ScenarioHarness {

extern std::atomic<uint64_t> g_gl_debug_message_count;
extern std::atomic<uint64_t> g_gl_debug_error_count;
extern std::atomic<uint64_t> g_gl_debug_warning_count;
extern std::atomic<uint64_t> g_gl_debug_notification_count;

bool HasCommandLineFlag(int argc, char* argv[], const std::string& flag);
std::string GetCommandLineOption(int argc, char* argv[], const std::string& flag, const std::string& fallback);
int GetCommandLineIntOption(int argc, char* argv[], const std::string& flag, int fallback);
uint64_t GetCommandLineUInt64Option(int argc, char* argv[], const std::string& flag, uint64_t fallback);

struct RuntimeScenarioConfig {
    std::string scenario;
    bool auto_create_world = false;
    bool auto_enter_world = false;
    bool no_audio = false;
    bool no_ui = false;
    bool hidden_window = false;
    bool enable_gpu_sdf_runtime = false;
    int timed_run_seconds = 0;
    int readiness_timeout_seconds = 120;
    int horizon_radius = 12;
    int collision_radius = 4;
    int coverage_radius = 3;
    size_t min_renderable_chunks = 64;
    size_t min_collision_chunks = 9;
    uint64_t memory_watermark_mb = 0;
    std::filesystem::path artifact_dir;
    std::filesystem::path audio_telemetry_path;
    std::filesystem::path crash_dir;
    // Persistence runtime roundtrip (T-I2-13): which half of the roundtrip
    // this process runs ("save" or "load") and the shared session directory
    // the world snapshot travels through.
    std::string persistence_phase;
    std::filesystem::path persistence_session_dir;

    bool active() const { return !scenario.empty(); }
    bool auto_world_smoke() const { return scenario == "auto_world_smoke"; }
    bool lod_ground_smoke() const { return scenario == "lod_ground_smoke"; }
    bool water_visual_smoke() const { return scenario == "water_visual_smoke"; }
    bool material_visual_smoke() const { return scenario == "material_visual_smoke"; }
    bool lod_boundary_oscillation_smoke() const { return scenario == "lod_boundary_oscillation_smoke"; }
    bool lod_seam_arrival_smoke() const { return scenario == "lod_seam_arrival_smoke"; }
    bool persistence_roundtrip_smoke() const { return scenario == "persistence_roundtrip_smoke"; }
    bool forced_crash() const { return scenario == "forced_crash"; }
};

RuntimeScenarioConfig ParseRuntimeScenarioConfig(int argc, char* argv[], const std::filesystem::path& root_dir);

std::string TimestampUtc();
std::string TimestampForFile();

nlohmann::json Vec3ToJson(const Luminumbra::Vec3& value);
nlohmann::json IVec3ToJson(const Luminumbra::IVec3& value);

struct GLDebugRuntimeStats {
    uint64_t messages = 0;
    uint64_t errors = 0;
    uint64_t warnings = 0;
    uint64_t notifications = 0;
};

GLDebugRuntimeStats CurrentGLDebugRuntimeStats();

void ApplyLodGroundCameraPath(
    const RuntimeScenarioConfig& config,
    Luminumbra::world::GameSession* game_session,
    Luminumbra::Rendering::Camera* camera,
    double elapsed_seconds);

struct WaterVisualCameraTarget {
    bool found = false;
    Luminumbra::Vec3 focus{0.0f};
    Luminumbra::Vec3 camera_position{0.0f};
    float terrain_height = 0.0f;
    float camera_terrain_height = 0.0f;
    int supporting_water_samples = 0;
};

WaterVisualCameraTarget FindWaterVisualCameraTarget(Luminumbra::world::GameSession* game_session);
WaterVisualCameraTarget FindMaterialVisualCameraTarget(Luminumbra::world::GameSession* game_session);
void AimCameraAt(Luminumbra::Rendering::Camera* camera, const Luminumbra::Vec3& focus);
void ApplyWaterVisualCamera(
    Luminumbra::Rendering::Camera* camera,
    const WaterVisualCameraTarget& target);

struct ScreenshotPixelStats {
    int width = 0;
    int height = 0;
    std::uint64_t roi_pixels = 0;
    std::uint64_t water_like_pixels = 0;
    std::uint64_t dark_pixels = 0;
    std::uint64_t bright_sky_like_pixels = 0;
    double water_like_ratio = 0.0;
};

struct LodHolePixelStats {
    int width = 0;
    int height = 0;
    std::uint64_t roi_pixels = 0;
    std::uint64_t dark_void_pixels = 0;
    std::uint64_t near_black_pixels = 0;
    std::uint64_t background_blue_pixels = 0;
    double dark_void_ratio = 0.0;
    double near_black_ratio = 0.0;
    double background_blue_ratio = 0.0;
    // Connected components (8-connectivity) of void (RGB <= 10) ROI pixels
    // with at least kMinNearBlackClusterPx pixels. Narrow LOD seam crack
    // slivers form connected runs of pure-black hole pixels even when the
    // total near-black ratio stays below the area threshold, so the seam
    // gate enforces the cluster count directly.
    std::uint64_t near_black_cluster_count = 0;
    std::uint64_t largest_near_black_cluster_px = 0;
};

struct LodGroundVisualCapture {
    std::string role;
    std::string file;
    LodHolePixelStats pixels;
};

struct MaterialPixelStats {
    int width = 0;
    int height = 0;
    std::uint64_t roi_pixels = 0;
    std::uint64_t sand_pixels = 0;
    std::uint64_t grey_fallback_pixels = 0;
    std::uint64_t water_like_pixels = 0;
    std::uint64_t other_pixels = 0;
    double sand_ratio = 0.0;
    double grey_fallback_ratio = 0.0;
};

ScreenshotPixelStats AnalyzeScreenshotPixels(const std::vector<unsigned char>& pixels, int width, int height);
LodHolePixelStats AnalyzeLodHolePixels(const std::vector<unsigned char>& pixels, int width, int height);
MaterialPixelStats AnalyzeMaterialPixels(const std::vector<unsigned char>& pixels, int width, int height);

bool WriteBackbufferPpm(
    const std::filesystem::path& path,
    int width,
    int height,
    ScreenshotPixelStats* out_stats = nullptr,
    LodHolePixelStats* out_lod_hole_stats = nullptr);

bool WritePixelBufferPpm(
    const std::filesystem::path& path,
    int width,
    int height,
    const std::vector<unsigned char>& pixels);

std::vector<unsigned char> BuildMaterialHeatmap(const std::vector<unsigned char>& pixels, int width, int height);

nlohmann::json LodHolePixelStatsToJson(const LodHolePixelStats& stats);
nlohmann::json ScreenshotPixelStatsToJson(const ScreenshotPixelStats& stats);

void WriteWaterVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& screenshot,
    const WaterVisualCameraTarget& target,
    const ScreenshotPixelStats& pixel_stats,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass,
    const Luminumbra::Rendering::RenderPipeline::MeshUploadFrameStats& upload_queue);

void WriteMaterialVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& screenshot,
    const std::string& heatmap_screenshot,
    const WaterVisualCameraTarget& target,
    const MaterialPixelStats& pixel_stats,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass);

void WriteLodGroundScreenshotIndex(
    const std::filesystem::path& artifact_dir,
    const std::vector<std::string>& screenshots);

void WriteLodGroundVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::vector<LodGroundVisualCapture>& captures);

void WriteStreamingTelemetry(
    const std::filesystem::path& artifact_dir,
    const std::string& scenario,
    double duration_seconds,
    const Luminumbra::Systems::SHIELD_WorldSystem::StreamingTelemetryStats& stats);

float LodBoundaryDistance(Luminumbra::Systems::SHIELD_WorldSystem* world_system);

void ApplyLodBoundaryOscillationCamera(
    Luminumbra::world::GameSession* game_session,
    Luminumbra::Rendering::Camera* camera,
    double elapsed_seconds);

class LodBoundaryTransitionRecorder {
public:
    void record_frame(Luminumbra::Systems::SHIELD_WorldSystem* world_system);

    uint64_t frames_observed() const { return m_frames_observed; }
    std::size_t chunks_observed() const { return m_last_lod.size(); }
    const std::unordered_map<Luminumbra::ChunkID, std::uint64_t>& transitions() const { return m_transitions; }

private:
    std::unordered_map<Luminumbra::ChunkID, int> m_last_lod;
    std::unordered_map<Luminumbra::ChunkID, std::uint64_t> m_transitions;
    uint64_t m_frames_observed = 0;
};

void WriteLodBoundaryOscillationAnalysis(
    const std::filesystem::path& artifact_dir,
    double duration_seconds,
    float boundary_distance,
    const LodBoundaryTransitionRecorder& recorder);

void ApplyLodSeamArrivalCamera(
    const RuntimeScenarioConfig& config,
    Luminumbra::world::GameSession* game_session,
    Luminumbra::Rendering::Camera* camera,
    double elapsed_seconds);

// --- Persistence runtime roundtrip (T-I2-13) ---
// save phase: applies deterministic scripted voxel edits near spawn, remeshes
// them through the existing surface path, hashes the edited chunk set, saves
// the world snapshot to the session dir, and writes the save-phase artifact.
// load phase: verifies the runtime adopted the snapshot at world enter, then
// re-loads the snapshot through WorldSaveService::load_world, hashes the same
// chunk ids recorded by the save phase, and writes the load-phase artifact.
struct PersistenceRoundtripPhaseResult {
    bool passed = false;
    std::string failure_reason;
};

PersistenceRoundtripPhaseResult RunPersistenceRoundtripSavePhase(
    const RuntimeScenarioConfig& config,
    Luminumbra::world::GameSession* game_session);

PersistenceRoundtripPhaseResult RunPersistenceRoundtripLoadPhase(
    const RuntimeScenarioConfig& config,
    Luminumbra::world::GameSession* game_session);

class LodSeamArrivalRecorder {
public:
    void record_frame(Luminumbra::Systems::SHIELD_WorldSystem* world_system);

    uint64_t frames_observed() const { return m_frames_observed; }
    std::size_t pending_lod_high_water() const { return m_pending_lod_high_water; }
    std::size_t last_pending_lod() const { return m_last_pending_lod; }

private:
    std::size_t m_pending_lod_high_water = 0;
    std::size_t m_last_pending_lod = 0;
    uint64_t m_frames_observed = 0;
};

void WriteLodSeamArrivalAnalysis(
    const std::filesystem::path& artifact_dir,
    double duration_seconds,
    const std::vector<LodGroundVisualCapture>& captures,
    const LodSeamArrivalRecorder& recorder);

} // namespace Luminumbra::Client::ScenarioHarness
