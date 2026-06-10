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
    bool skybox_visual_smoke() const { return scenario == "skybox_visual_smoke"; }
    bool weather_visual_smoke() const { return scenario == "weather_visual_smoke"; }
    bool timeofday_sweep_smoke() const { return scenario == "timeofday_sweep_smoke"; }
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

// --- Skybox visual smoke (T-I2-17a) ---
// Camera sits over open terrain near spawn, tilted up 30 degrees with a wide
// (90 degree) vertical FOV aimed at the sun azimuth so the noon sun disc is
// inside the frame. The analysis measures the atmospheric gradient and the
// sun disc directly from backbuffer pixels.
struct SkyboxVisualBandStats {
    double mean_luminance = 0.0;
    std::uint64_t pixels = 0;
};

struct SkyboxPixelStats {
    int width = 0;
    int height = 0;
    std::uint64_t sky_roi_pixels = 0;
    // Bands run from the horizon end of the sky ROI (index 0) to the zenith
    // end (last index). Sun-disc pixels are excluded from the band means so
    // the gradient check measures atmosphere, not the disc.
    std::vector<SkyboxVisualBandStats> bands;
    double horizon_band_mean = 0.0;
    double zenith_band_mean = 0.0;
    int monotonic_violations = 0;
    double max_luminance = 0.0;
    std::uint64_t sun_disc_pixels = 0;
    double sun_disc_centroid_x = 0.0;   // normalized [0,1], 0 = left
    double sun_disc_centroid_y = 0.0;   // normalized [0,1], 0 = top
    // Disc pixels within the expected-sun-position cluster radius; the
    // localization metric (a half/quadrant split breaks when the sun sits on
    // the frame centerline).
    std::uint64_t sun_disc_pixels_near_expected = 0;
};

// Toward-sun unit vector for a normalized time of day, mirroring
// RenderPipeline::update_time_of_day (t=0 is noon, elevation = cos(2*pi*t)).
Luminumbra::Vec3 TowardSunDirection(float time_of_day);

void ApplySkyboxVisualCamera(
    Luminumbra::world::GameSession* game_session,
    Luminumbra::Rendering::Camera* camera,
    float pinned_time_of_day);

// Projects a world-space direction (point at infinity) to normalized screen
// coordinates; returns true when the direction lands inside the frame.
bool ProjectDirectionToScreen(
    const Luminumbra::Rendering::Camera& camera,
    int width,
    int height,
    const Luminumbra::Vec3& direction,
    double& x_norm,
    double& y_norm_from_top);

SkyboxPixelStats AnalyzeSkyboxPixels(
    const std::vector<unsigned char>& pixels,
    int width,
    int height,
    double sun_screen_x_norm,
    double sun_screen_y_norm,
    bool sun_on_screen);

void WriteSkyboxVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& screenshot,
    const SkyboxPixelStats& pixel_stats,
    double sun_screen_x_norm,
    double sun_screen_y_norm,
    bool sun_on_screen,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass);

// --- Weather visual smoke (T-I2-17b) ---
// Same camera as the skybox scenario. A clear-sky baseline frame is captured
// in the first half of the run; weather (Rain at intensity 1.0) is enabled at
// the midpoint and the weather frame captured near the end. The analysis
// compares the two captures: overcast luminance drop in the sky ROI and rain
// streak structure (horizontal luminance gradient energy, since vertical
// streaks create high-frequency variation across columns).
struct WeatherPixelStats {
    int width = 0;
    int height = 0;
    std::uint64_t sky_roi_pixels = 0;
    double sky_mean_luminance = 0.0;
    // Mean |L(x+1,y) - L(x,y)| over the sky ROI: vertical rain streaks
    // produce horizontal high-frequency luminance transitions.
    double sky_horizontal_gradient_mean = 0.0;
    double frame_mean_luminance = 0.0;
};

WeatherPixelStats AnalyzeWeatherPixels(const std::vector<unsigned char>& pixels, int width, int height);

void WriteWeatherVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& baseline_screenshot,
    const std::string& weather_screenshot,
    const WeatherPixelStats& baseline_stats,
    const WeatherPixelStats& weather_stats,
    const std::string& weather_type,
    float weather_intensity,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass);

// --- Time-of-day sweep smoke (T-I2-17c) ---
// Fixed skybox camera; the run is split into three equal phases pinned at
// t=0.04 (noon), t=0.22 (dusk, sun elevation ~10.8 degrees), t=0.45 (night),
// each captured near the end of its phase window so settle frames separate
// the transitions. The analysis checks per-phase mean luminance ordering
// (noon > dusk > night), the dusk warm shift (r/b rises vs noon), and a
// generic emissive-material night check with an honest fallback when no
// emissive registry material is discoverable in a surface capture.
struct TimeOfDayPixelStats {
    int width = 0;
    int height = 0;
    double frame_mean_luminance = 0.0;
    double sky_mean_luminance = 0.0;       // top kSkyRoiHeightFraction of the frame
    double terrain_mean_luminance = 0.0;   // bottom 25% of the frame
    double frame_mean_r = 0.0;
    double frame_mean_b = 0.0;
    double frame_r_b_ratio = 0.0;
    double terrain_r_b_ratio = 0.0;
    double max_luminance = 0.0;
    double max_luminance_y_from_top_norm = 0.0;  // 0 = top of frame
    double sky_max_luminance = 0.0;              // max within the sky band
    // Pixels above the emissive glow floor inside the central third of the
    // frame; only consumed by the optional night-emissive capture.
    std::uint64_t center_glow_pixels = 0;
};

TimeOfDayPixelStats AnalyzeTimeOfDayPixels(const std::vector<unsigned char>& pixels, int width, int height);

// Phase time for a normalized sweep progress: noon / dusk / night thirds.
float TimeOfDaySweepPhaseTime(double progress);

// Generic emissive-material discovery: emissive material ids come from the
// engine material registry (data/common/materials.json entries with a
// non-zero "emission"); the streamed terrain meshes are scanned for a
// near-surface vertex carrying one of those ids. Game content decides which
// materials are emissive; the engine check stays generic.
struct EmissiveMaterialTarget {
    bool found = false;
    std::vector<std::uint32_t> emissive_material_ids;
    Luminumbra::Vec3 position{0.0f};
    std::uint32_t material_id = 0;
    float distance_from_spawn = 0.0f;
    float depth_below_surface = 0.0f;
    std::size_t vertices_scanned = 0;
    std::size_t emissive_vertices_total = 0;
    std::size_t emissive_vertices_in_range = 0;
};

EmissiveMaterialTarget FindEmissiveMaterialTarget(
    Luminumbra::world::GameSession* game_session,
    const std::filesystem::path& root_dir);

struct TimeOfDayPhaseCapture {
    std::string name;
    double time_of_day = 0.0;
    std::string file;
    TimeOfDayPixelStats stats;
};

void WriteTimeOfDaySweepAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::vector<TimeOfDayPhaseCapture>& phases,
    const EmissiveMaterialTarget& emissive_target,
    bool emissive_capture_written,
    const std::string& emissive_screenshot,
    const TimeOfDayPixelStats& emissive_stats,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass);

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
