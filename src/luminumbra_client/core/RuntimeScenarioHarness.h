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
    // player_view_smoke (T-I3-3): world preset to create the automated test
    // world from (--world-preset; empty falls back to "mountains", the
    // worst-case preset for surface-span coverage).
    std::string world_preset;
    // creature_slice_smoke (T-I3-18): root-relative path of the game
    // archetype JSON to spawn (--creature-archetype). The engine harness
    // carries no game nouns; the validator supplies the content path.
    std::string creature_archetype;

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
    bool player_view_smoke() const { return scenario == "player_view_smoke"; }
    bool farlod_horizon_smoke() const { return scenario == "farlod_horizon_smoke"; }
    bool skinned_mesh_visual_smoke() const { return scenario == "skinned_mesh_visual_smoke"; }
    bool creature_slice_smoke() const { return scenario == "creature_slice_smoke"; }
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
    // T-I2-16b: grazing-angle framing toward the most open water, used for
    // the late-run reflection capture (the top-down camera_position view has
    // no usable fresnel reflection signal).
    Luminumbra::Vec3 reflection_camera_position{0.0f};
    // T-I2-16c: water-surface points (y = sea level) near the focus,
    // projected into the main capture to gate the depth tint gradient and
    // the shoreline foam band:
    // - shallow_point: 0.8-1.6 m of water (bright teal tint, clear of sand)
    // - foam_point: 0.25-0.6 m of water (middle of the foam band)
    // - deep_point: >= 3 m of water (dark deep tint)
    bool shallow_point_found = false;
    bool deep_point_found = false;
    bool foam_point_found = false;
    Luminumbra::Vec3 shallow_point{0.0f};
    Luminumbra::Vec3 deep_point{0.0f};
    Luminumbra::Vec3 foam_point{0.0f};
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
void ApplyWaterReflectionCamera(
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
    std::uint64_t grass_pixels = 0;
    std::uint64_t grey_fallback_pixels = 0;
    std::uint64_t water_like_pixels = 0;
    std::uint64_t other_pixels = 0;
    double sand_ratio = 0.0;
    double grass_ratio = 0.0;
    double grey_fallback_ratio = 0.0;
    // Stone presence is measured in a separate rim sub-ROI (top quarter of
    // the frame, same horizontal band): the high-altitude cliff rims are the
    // only natural stone exposure, and legitimate dim stone is colour-shaped
    // like the grey fallback, so it is counted there instead of competing
    // with the fallback detector inside the main beach/flank ROI.
    std::uint64_t rim_roi_pixels = 0;
    std::uint64_t stone_pixels = 0;
    double stone_ratio = 0.0;
    // Soil is also measured in the rim sub-ROI: the depth 1-5 band surfaces
    // along the same cliff rims (5x the pixel density of the main ROI). Its
    // warm hue (r-b >= 13) keeps it separable from both the grey fallback and
    // the stone bucket.
    std::uint64_t soil_pixels = 0;
    double soil_ratio = 0.0;
};

// T-I2-16a: temporal caustics-animation probe for the water visual scenario.
// Each sample records:
// - the mean luminance (0-255) of the water-like pixels in the screenshot
//   ROI at a given elapsed time (scene-side supporting evidence), and
// - the mean absolute texel delta of the generated caustics texture against
//   the previous sample's readback (texture_mean_abs_delta, -1 when there is
//   no previous readback). The texture delta is the enforced animation gate:
//   a static tint reproduces the same texels every second (delta exactly 0)
//   while generated caustics keep moving, and unlike the screen luminance it
//   is immune to chunk-streaming noise in the capture ROI.
struct WaterCausticsSample {
    double elapsed_seconds = 0.0;
    double water_mean_luminance = 0.0;
    std::uint64_t water_pixels = 0;
    double texture_mean_abs_delta = -1.0;
};

WaterCausticsSample SampleBackbufferWaterLuminance(int width, int height, double elapsed_seconds);

// Reads back the generated caustics texture (RGBA8) and computes the mean
// absolute per-channel delta against `previous_texels` (when non-empty),
// then replaces `previous_texels` with the fresh readback. Returns -1.0 when
// the texture is unavailable or there is no previous readback to compare.
double SampleCausticsTextureDelta(unsigned int texture_id, std::vector<unsigned char>& previous_texels);

// T-I2-16b: SSR sky-correlation probe. Measures the mean color of the
// water-like pixels in the upper third of the screenshot ROI (where the view
// angle is shallowest, so the fresnel-weighted reflection dominates) and
// correlates its hue (normalized RGB cosine similarity) against the sky
// reflection reference color the water shader uses for SSR misses.
struct WaterReflectionStats {
    std::uint64_t upper_roi_pixels = 0;
    std::uint64_t upper_roi_water_pixels = 0;
    double mean_r = 0.0;
    double mean_g = 0.0;
    double mean_b = 0.0;
    Luminumbra::Vec3 sky_reference{0.0f};
    double sky_correlation = 0.0;
};

WaterReflectionStats AnalyzeWaterReflection(
    const std::vector<unsigned char>& pixels,
    int width,
    int height,
    const Luminumbra::Vec3& sky_reference);

// T-I2-16c: mean color of a square pixel patch around a projected water
// point, plus the fraction of foam-like (bright, low-saturation) pixels in
// it. gb_balance ((g-b)/(g+b)) separates the bright-teal shallow tint
// (green-led) from the deep blue tint (blue-led).
struct WaterRegionPatch {
    bool sampled = false;
    int center_x = 0;          // pixels from the left edge
    int center_y_from_top = 0; // pixels from the top edge
    std::uint64_t pixels = 0;
    double mean_r = 0.0;
    double mean_g = 0.0;
    double mean_b = 0.0;
    double gb_balance = 0.0;
    std::uint64_t foam_pixels = 0;
    double foam_ratio = 0.0;
};

WaterRegionPatch AnalyzeWaterRegionPatch(
    const std::vector<unsigned char>& pixels,
    int width,
    int height,
    int center_x,
    int center_y_from_top,
    int radius);

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
    const std::string& reflection_screenshot,
    const WaterVisualCameraTarget& target,
    const ScreenshotPixelStats& pixel_stats,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass,
    const Luminumbra::Rendering::RenderPipeline::MeshUploadFrameStats& upload_queue,
    const std::vector<WaterCausticsSample>& caustics_samples,
    const WaterReflectionStats& reflection_stats,
    const WaterRegionPatch& shallow_patch,
    const WaterRegionPatch& deep_patch,
    const WaterRegionPatch& foam_patch);

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

// --- player_view_smoke (T-I3-3): eye-level 360-degree coverage gate ---
// Camera stands at spawn at eye level (terrain + 1.8 m) and sweeps 12 yaw
// stations 30 degrees apart at pitch 0, plus one station aimed at the highest
// visible peak within the near field (and, on the archipelago preset, one
// station framing the seed-424242 degenerate-geometry investigation region).
// Per station after a settle window the gate records (a) the sim-side frustum
// surface coverage (SHIELD_WorldSystem::get_frustum_surface_coverage_stats)
// and (b) a screenshot analyzed for sky-colored pixels below the projected
// horizon line plus the existing near-black seam-cluster detection.
struct PlayerViewStation {
    std::string name;
    float yaw_degrees = 0.0f;
    float pitch_degrees = 0.0f;
    // When set, the camera is aimed at `target` instead of using the fixed
    // yaw/pitch (peak + archipelago degenerate-region stations).
    bool aim_at_target = false;
    Luminumbra::Vec3 target{0.0f};
};

// Eye-level camera position at the world spawn: (spawn.x, terrain + 1.8 m,
// spawn.z).
Luminumbra::Vec3 PlayerViewEyePosition(Luminumbra::world::GameSession* game_session);

std::vector<PlayerViewStation> BuildPlayerViewStations(
    Luminumbra::world::GameSession* game_session,
    const std::string& world_preset);

void ApplyPlayerViewCamera(
    Luminumbra::world::GameSession* game_session,
    Luminumbra::Rendering::Camera* camera,
    const PlayerViewStation& station);

// Inward-facing frustum planes (ax+by+cz+d >= 0 inside) extracted from the
// camera's projection*view matrix (Gribb-Hartmann).
std::array<Luminumbra::Vec4, 6> ExtractCameraFrustumPlanes(
    const Luminumbra::Rendering::Camera& camera,
    int width,
    int height);

struct PlayerViewPixelStats {
    int width = 0;
    int height = 0;
    // Projected horizon row (pixels from the top); pixels below this row at
    // eye level over loaded terrain must be geometry, never sky.
    int horizon_row_from_top = 0;
    std::uint64_t below_horizon_pixels = 0;
    std::uint64_t below_horizon_sky_pixels = 0;
    double below_horizon_sky_ratio = 0.0;
    // Degenerate-void clusters (8-connectivity, >= 12 px) under the STRICT
    // void predicate max(r,g,b) <= 2. The LOD-seam gate's RGB <= 10 sliver
    // predicate was tuned on the default preset; on the mountains preset
    // legitimately shadowed cliff faces have a continuous dark tail (~5% of
    // frame pixels <= 10 at noon, measured) while true voids - backface
    // peeks through missing geometry - stay at RGB 0-2. Missing chunks that
    // open to the skybox are caught by the sky classifier instead.
    std::uint64_t void_cluster_count = 0;
    std::uint64_t largest_void_cluster_px = 0;
};

PlayerViewPixelStats AnalyzePlayerViewPixels(
    const std::vector<unsigned char>& pixels,
    int width,
    int height,
    int horizon_row_from_top);

struct PlayerViewStationCapture {
    PlayerViewStation station;
    std::string file;
    PlayerViewPixelStats sky;
    LodHolePixelStats holes;
    Luminumbra::Systems::SHIELD_WorldSystem::FrustumSurfaceCoverageStats coverage;
};

// True when any column within the player-view coverage range holds open
// sea-level water. The skybox and the water surface share hue at the pinned
// time of day, so the below-horizon sky-leak classifier cannot distinguish a
// leak from legitimate sea; the sky-ratio threshold is only enforced when no
// sea water is visible in the near field (coverage + void clusters always
// are).
bool PlayerViewSeaWaterInNearField(Luminumbra::world::GameSession* game_session);

void WritePlayerViewAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& world_preset,
    double duration_seconds,
    const std::vector<PlayerViewStationCapture>& captures,
    std::size_t expected_station_count,
    const Luminumbra::Systems::SHIELD_WorldSystem::RuntimeChunkStats& chunk_stats,
    bool enforce_sky_ratio);

// --- farlod_horizon_smoke (T-I3-9): far-LOD horizon + live/far seam gate ---
// Two-phase run: phase A holds the eye-level camera with far-LOD DISABLED
// and samples the gbuffer GPU time (the honest in-run baseline - the
// committed perf baseline carries frame times, not per-pass GPU times);
// phase B enables far-LOD and sweeps the stations, capturing each one after
// a settle window plus the far gbuffer GPU time. The seam gate (the
// Distant-Horizons failure mode): a boundary-band ROI spanning the live-ring
// boundary (~192 m at the smoke radii) is analyzed with the below-horizon
// sky-leak predicate and the strict void-cluster machinery - any sky/void
// band at the live/far boundary fails the station.
struct FarLodHorizonStation {
    std::string name;
    float yaw_degrees = 0.0f;
    float pitch_degrees = 0.0f;
    // Camera height above the spawn-column terrain (eye level or elevated).
    float eye_height_meters = 1.8f;
};

std::vector<FarLodHorizonStation> BuildFarLodHorizonStations();

void ApplyFarLodHorizonCamera(
    Luminumbra::world::GameSession* game_session,
    Luminumbra::Rendering::Camera* camera,
    const FarLodHorizonStation& station);

struct FarLodBoundaryBandStats {
    // False when terrain along the forward azimuth occludes the boundary
    // ring (projected band collapses above the horizon) - nothing to gate.
    bool band_resolved = false;
    int band_top_row_from_top = 0;
    int band_bottom_row_from_top = 0;
    std::uint64_t band_pixels = 0;
    std::uint64_t band_sky_pixels = 0;
    double band_sky_ratio = 0.0;
    std::uint64_t void_cluster_count = 0;
    std::uint64_t largest_void_cluster_px = 0;
};

// Projects ground points at the inner/outer band distances along the camera
// forward azimuth (terrain height sampled per point) to screen rows; the
// rows are clamped below the projected eye-level horizon row.
bool ComputeFarLodBoundaryBandRows(
    Luminumbra::world::GameSession* game_session,
    const Luminumbra::Rendering::Camera& camera,
    int width,
    int height,
    float inner_distance_m,
    float outer_distance_m,
    int horizon_row_from_top,
    int& out_top_row_from_top,
    int& out_bottom_row_from_top);

FarLodBoundaryBandStats AnalyzeFarLodBoundaryBand(
    const std::vector<unsigned char>& pixels,
    int width,
    int height,
    int band_top_row_from_top,
    int band_bottom_row_from_top);

struct FarLodHorizonStationCapture {
    FarLodHorizonStation station;
    std::string file;
    PlayerViewPixelStats sky;          // full below-horizon machinery
    FarLodBoundaryBandStats boundary;  // live/far boundary band ROI
    // Far-LOD scheduler state at capture time.
    std::size_t regions_wanted = 0;
    std::size_t regions_resident = 0;
    std::size_t regions_missing = 0;
    std::size_t resident_bytes = 0;
    std::size_t region_draws = 0;
    std::size_t far_indices_drawn = 0;
};

void WriteFarLodHorizonAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& world_preset,
    double duration_seconds,
    const std::vector<FarLodHorizonStationCapture>& captures,
    std::size_t expected_station_count,
    double baseline_gbuffer_gpu_ms,
    double far_gbuffer_gpu_ms,
    bool gpu_timers_supported,
    bool enforce_sky_ratio);

// --- skinned_mesh_visual_smoke (T-I3-16): skinned G-Buffer stage gate ---
// Spawns a procedurally generated rigged test mesh (LMS2 + .lanim written
// into the artifact dir at scenario start: a static post with an arm hinged
// at the top, the arm joint rotating slowly about Z over a 60 s clip) near
// the world spawn, frames it with a fixed camera, and captures the frame at
// two different clip times. The gate asserts the skinned draw stage ran
// (skinned_draws > 0 at both captures) and that the two captures differ in
// the mesh ROI (the deformation is visible), excluding sky-colored pixels so
// drifting clouds cannot pass the gate by themselves.
struct SkinnedMeshVisualTarget {
    bool spawned = false;
    Luminumbra::EntityID entity{entt::null};
    Luminumbra::Vec3 mesh_position{0.0f};   // base of the post
    Luminumbra::Vec3 focus{0.0f};           // arm hinge (camera aim point)
    Luminumbra::Vec3 camera_position{0.0f};
    std::string mesh_path;                  // absolute LMS2 path
    std::string clip_path;                  // absolute .lanim path
    std::string failure_reason;
};

SkinnedMeshVisualTarget SpawnSkinnedMeshVisualEntity(
    Luminumbra::world::GameSession* game_session,
    const std::filesystem::path& artifact_dir);

void ApplySkinnedMeshVisualCamera(
    Luminumbra::Rendering::Camera* camera,
    const SkinnedMeshVisualTarget& target);

// Animation clock of the spawned entity's player component (seconds), -1.0
// when the entity is gone.
double SkinnedMeshVisualAnimationTime(
    Luminumbra::world::GameSession* game_session,
    const SkinnedMeshVisualTarget& target);

struct SkinnedMeshVisualCapture {
    std::string file;
    double elapsed_seconds = 0.0;
    double animation_time_seconds = -1.0;
    std::size_t skinned_draws = 0;
    std::size_t skinned_indices_drawn = 0;
};

struct SkinnedMeshDiffStats {
    int width = 0;
    int height = 0;
    int roi_x0 = 0;
    int roi_y0 = 0; // from top
    int roi_x1 = 0;
    int roi_y1 = 0;
    std::uint64_t roi_pixels = 0;
    // Pixels whose max channel delta >= threshold AND that are not
    // sky-colored in both captures.
    std::uint64_t changed_pixels = 0;
    double changed_ratio = 0.0;
    // Warm-toned opaque-geometry pixels (rig + terrain band) inside the ROI;
    // recorded as supporting evidence only — the enforced visibility signal
    // is skinned_draws > 0 plus the non-sky temporal diff.
    std::uint64_t mesh_like_pixels_a = 0;
    std::uint64_t mesh_like_pixels_b = 0;
    // T-I4-8 textured-response: spatial color variation across the mesh-like
    // pixels in capture A (mean per-channel std-dev, 0..255). A flat-colored
    // (untextured) creature reads near-uniform; the authored grovestrider
    // texture drives this well above the flat bound.
    double mesh_color_stddev_a = 0.0;
};

SkinnedMeshDiffStats AnalyzeSkinnedMeshCaptures(
    const std::vector<unsigned char>& pixels_a,
    const std::vector<unsigned char>& pixels_b,
    int width,
    int height);

void WriteSkinnedMeshVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const SkinnedMeshVisualTarget& target,
    const SkinnedMeshVisualCapture& capture_a,
    const SkinnedMeshVisualCapture& capture_b,
    const SkinnedMeshDiffStats& diff);

// --- creature_slice_smoke (T-I3-18): Project Capture game slice ---
// One MVP creature (pure game data: the archetype JSON named by
// --creature-archetype plus its rigged assets under data/models/) is
// spawned near the archipelago spawn: rigged LMS2 mesh + idle/walk clips on
// the skinned G-Buffer stage (T-I3-16), needs/opportunities planned by the
// fixed-tick InstinctSystem (T-I3-17). Mid-run a light stimulus appears (a
// prop rendered with the emissive LUT material from
// data/common/materials.json plus a high-urgency curiosity opportunity, both
// declared in the archetype's `slice` block) and the planner switches
// behavior (graze -> approach); the creature turns and walks toward the
// glow. The gate records the planner state before/after the stimulus plus
// two screenshots — the photographable moment.
struct CreatureSliceScene {
    bool spawned = false;
    std::string failure_reason;
    Luminumbra::EntityID creature{entt::null};
    Luminumbra::EntityID graze_opportunity{entt::null};
    Luminumbra::EntityID stimulus{entt::null};
    bool stimulus_spawned = false;
    Luminumbra::Vec3 creature_position{0.0f};
    Luminumbra::Vec3 graze_position{0.0f};
    Luminumbra::Vec3 stimulus_position{0.0f};
    Luminumbra::Vec3 camera_position{0.0f};
    Luminumbra::Vec3 camera_focus{0.0f};
    std::string archetype_name;
    std::string expected_before_action;
    std::string expected_after_action;
    std::string active_clip;
    // Loaded archetype JSON (slice/creature blocks consumed at runtime).
    nlohmann::json archetype;
};

CreatureSliceScene SpawnCreatureSliceScene(
    Luminumbra::world::GameSession* game_session,
    const std::filesystem::path& root_dir,
    const std::string& archetype_relative_path);

// Spawns the light stimulus: an emissive-material prop plus the curiosity
// opportunity from the archetype's slice block.
bool SpawnCreatureSliceStimulus(
    Luminumbra::world::GameSession* game_session,
    CreatureSliceScene& scene);

// Per-frame game glue: planner-action -> clip selection (graze=idle,
// approach=walk, from the archetype's clip_by_action map) and approach
// locomotion (walk toward the plan target, terrain-following, facing the
// movement direction).
void UpdateCreatureSliceScene(
    Luminumbra::world::GameSession* game_session,
    CreatureSliceScene& scene,
    double dt);

// Live photographic framing: follows the creature, keeps the active target
// (graze spot before the stimulus, the glow after) in frame, and lifts the
// camera over intervening terrain ridges.
void ApplyCreatureSliceCamera(
    Luminumbra::world::GameSession* game_session,
    Luminumbra::Rendering::Camera* camera,
    CreatureSliceScene& scene);

struct CreatureSlicePlanProbe {
    bool valid = false;
    std::string action;
    std::string target;
    std::string need;
    double score = 0.0;
    std::string checksum;
    std::uint64_t plans_executed = 0;
    std::string active_clip;
    // Live world state at probe time (the creature moves on approach).
    Luminumbra::Vec3 creature_position{0.0f};
    Luminumbra::Vec3 camera_position{0.0f};
};

CreatureSlicePlanProbe ProbeCreatureSlicePlan(
    Luminumbra::world::GameSession* game_session,
    const CreatureSliceScene& scene);

// T-I3-22 composition check: a "functionally green, visually broken" capture
// (creature rendered, planner correct, but the camera stares at the ground or
// the sky, or the creature is camouflaged against its own terrain) must not
// pass. sky_ratio proves a horizon is in frame; creature_terrain_color_delta
// proves the creature reads against the surrounding terrain.
struct CreatureSliceComposition {
    bool valid = false;            // the creature projected into the frame
    double sky_ratio = 0.0;        // fraction of frame pixels classified as sky
    double creature_roi_mean[3] = {0.0, 0.0, 0.0};
    double terrain_ref_mean[3] = {0.0, 0.0, 0.0};
    double creature_terrain_color_delta = 0.0; // L1 distance between the means
    std::size_t creature_roi_pixels = 0;
    std::size_t terrain_ref_pixels = 0;
    int creature_screen_x = 0;     // from left
    int creature_screen_y = 0;     // from top
};

// Analyzes a captured RGB framebuffer (bottom-up glReadPixels layout) for the
// creature-slice composition metrics. creature_screen_x/y are in top-left
// pixel coordinates (the projected creature position); pass valid=false-making
// out-of-frame coordinates and the ROI metrics stay zero.
CreatureSliceComposition AnalyzeCreatureSliceComposition(
    const std::vector<unsigned char>& pixels,
    int width,
    int height,
    int creature_screen_x_from_left,
    int creature_screen_y_from_top);

struct CreatureSliceCapture {
    std::string file;
    double elapsed_seconds = 0.0;
    CreatureSlicePlanProbe plan;
    std::size_t skinned_draws = 0;
    std::size_t skinned_indices_drawn = 0;
    CreatureSliceComposition composition;
};

void WriteCreatureSliceAnalysis(
    const std::filesystem::path& artifact_dir,
    const CreatureSliceScene& scene,
    const CreatureSliceCapture& before,
    const CreatureSliceCapture& after);

} // namespace Luminumbra::Client::ScenarioHarness
