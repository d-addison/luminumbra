#pragma once

#include "core/RuntimeScenarioConfig.h"
#include "luminumbra_common/net/ReplicationEndpoint.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "nlohmann/json.hpp"
#include "rendering/FoliageSurface.h"
#include "rendering/PixelIo.h"
#include "rendering/RenderPipeline.h"
#include "rendering/passes/FoliagePass.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Luminumbra::world {
class GameSession;
}
namespace Luminumbra::Rendering {
class Camera;
}

namespace Luminumbra::Client::ScenarioHarness {

// Helpers that moved DOWN into luminumbra_client (the shipping client uses
// them too, so they cannot live in the QA library): the GL debug counters +
// GLDebugRuntimeStats now come from core/RuntimeScenarioConfig.h; the pixel
// writer and the foliage surface query are re-exported here so the many
// existing ScenarioHarness call sites keep compiling.
using Luminumbra::Rendering::FoliageScatterContext;
using Luminumbra::Rendering::FoliageSurfaceQuery;
using Luminumbra::Rendering::WritePixelBufferPpm;

void ApplyLodGroundCameraPath(const RuntimeScenarioConfig& config,
                              Luminumbra::world::GameSession* game_session,
                              Luminumbra::Rendering::Camera* camera,
                              double elapsed_seconds);

struct WaterVisualCameraTarget {
    bool found = false;
    Luminumbra::Vec3 focus{0.0f};
    Luminumbra::Vec3 camera_position{0.0f};
    // grazing-angle framing toward the most open water, used for
    // the late-run reflection capture (the top-down camera_position view has
    // no usable fresnel reflection signal).
    Luminumbra::Vec3 reflection_camera_position{0.0f};
    // water-surface points (y = sea level) near the focus,
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
WaterVisualCameraTarget
FindMaterialVisualCameraTarget(Luminumbra::world::GameSession* game_session);
void AimCameraAt(Luminumbra::Rendering::Camera* camera, const Luminumbra::Vec3& focus);
void ApplyWaterVisualCamera(Luminumbra::Rendering::Camera* camera,
                            const WaterVisualCameraTarget& target);
void ApplyWaterReflectionCamera(Luminumbra::Rendering::Camera* camera,
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

// temporal caustics-animation probe for the water visual scenario.
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
double SampleCausticsTextureDelta(unsigned int texture_id,
                                  std::vector<unsigned char>& previous_texels);

// SSR sky-correlation probe. Measures the mean color of the
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

WaterReflectionStats AnalyzeWaterReflection(const std::vector<unsigned char>& pixels,
                                            int width,
                                            int height,
                                            const Luminumbra::Vec3& sky_reference);

// mean color of a square pixel patch around a projected water
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

WaterRegionPatch AnalyzeWaterRegionPatch(const std::vector<unsigned char>& pixels,
                                         int width,
                                         int height,
                                         int center_x,
                                         int center_y_from_top,
                                         int radius);

// --- Skybox visual smoke ---
// Camera sits over open terrain near spawn, tilted up 30 degrees with a wide
// (90 degree) vertical FOV aimed at the sun azimuth so the noon sun disc is
// inside the frame. The analysis measures the atmospheric gradient and the
// sun disc directly from backbuffer pixels.
struct SkyboxVisualBandStats {
    double mean_luminance = 0.0;
    std::uint64_t pixels = 0;
    //  per-band color accumulators for the low-sun palette-emergence
    // assertion (a warm horizon band reads R>B from Rayleigh/Mie scattering at
    // long optical paths). mean_r/mean_b are finalized as band means.
    double mean_r = 0.0;
    double mean_b = 0.0;
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
    double sun_disc_centroid_x = 0.0; // normalized [0,1], 0 = left
    double sun_disc_centroid_y = 0.0; // normalized [0,1], 0 = top
    // Disc pixels within the expected-sun-position cluster radius; the
    // localization metric (a half/quadrant split breaks when the sun sits on
    // the frame centerline).
    std::uint64_t sun_disc_pixels_near_expected = 0;
    //  low-sun scattering palette emergence. horizon_band_r_b_ratio is
    // the R/B ratio of the warm horizon band; zenith_band_r_b_ratio the cool
    // zenith band. At a low sun the horizon band warms (R>B) measurably above
    // the zenith band, which the SkyboxVisual gate asserts on top of the
    // existing monotonic-brighten + sun-disc-localize premises (clear sky).
    double horizon_band_r_b_ratio = 0.0;
    double zenith_band_r_b_ratio = 0.0;
};

// Toward-sun unit vector for a normalized time of day, mirroring
// RenderPipeline::update_time_of_day (t=0 is noon, elevation = cos(2*pi*t)).
Luminumbra::Vec3 TowardSunDirection(float time_of_day);

void ApplySkyboxVisualCamera(Luminumbra::world::GameSession* game_session,
                             Luminumbra::Rendering::Camera* camera,
                             float pinned_time_of_day);

// Projects a world-space direction (point at infinity) to normalized screen
// coordinates; returns true when the direction lands inside the frame.
bool ProjectDirectionToScreen(const Luminumbra::Rendering::Camera& camera,
                              int width,
                              int height,
                              const Luminumbra::Vec3& direction,
                              double& x_norm,
                              double& y_norm_from_top);

SkyboxPixelStats AnalyzeSkyboxPixels(const std::vector<unsigned char>& pixels,
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

// --- Weather visual smoke ---
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

WeatherPixelStats
AnalyzeWeatherPixels(const std::vector<unsigned char>& pixels, int width, int height);

void WriteWeatherVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& baseline_screenshot,
    const std::string& weather_screenshot,
    const WeatherPixelStats& baseline_stats,
    const WeatherPixelStats& weather_stats,
    const std::string& weather_type,
    float weather_intensity,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass);

// --- Atmosphere audio telemetry (, AU1) ---
// Client-side dressing: the replicated weather/wind state drives wind/rain
// AMBIENCE layers on the AudioPropagationSystem ambience bed + a weather-modulated
// reverb shift through the EnvironmentalAudioSystem. This sweeps a fixed set of
// weather conditions (clear / rain / storm) through the REAL audio systems (no
// audio backend needed -- the model is a pure function) and writes the
// AtmosphereAudio artifact (schema luminumbra.audio.atmosphere.v1) asserting an
// ambience layer is present and SCALES with weather intensity and the reverb param
// SHIFTS with weather. No world_hash, no visual-gate dependency; the null-audio
// path is unaffected. Returns true when the telemetry asserts pass.
bool WriteAtmosphereAudioTelemetry(const std::filesystem::path& artifact_dir);

// --- Lightning strike-frame smoke (, ) ---
// The strike capture is the photography TIMING shot: a deterministically scheduled
// strike fires during the weather phase, and the gate asserts the captured STRIKE
// FRAME shows (a) a full-scene luminance PULSE (frame-mean luminance spike vs the
// neighbour pre-strike frame) and (b) BOLT pixels (a bright thin high-gradient
// structure). The bolt-pixel detector counts pixels that are both bright AND a
// local high-gradient (the thin channel against the darkened storm sky). Render-
// only: the bolt/pulse are a one-way response to the SIM strike event .
struct StrikePixelStats {
    int width = 0;
    int height = 0;
    double frame_mean_luminance = 0.0;    // whole-frame mean (the pulse signal)
    std::uint64_t bright_thin_pixels = 0; // bright + high local gradient (bolt body)
    double max_luminance = 0.0;
    //  bolt SHAPE so the gate catches the "fat lumpy
    // white blob" failure that the raw bright-pixel count alone passed. The bolt's
    // bright-core pixels are collected into a bounding box; a real bolt is a THIN,
    // mostly-VERTICAL, sparse structure.
    std::uint64_t bolt_core_pixels = 0; // strictly-bright bolt-core pixels
    int bolt_bbox_width = 0;            // bbox width in px (narrow for a bolt)
    int bolt_bbox_height = 0;           // bbox height in px (tall for a bolt)
    double bolt_aspect_ratio = 0.0;     // height / width (>1 = vertical bolt)
    double bolt_fill_fraction = 0.0;    // core pixels / bbox area (low = thin)
};

StrikePixelStats
AnalyzeStrikePixels(const std::vector<unsigned char>& pixels, int width, int height);

void WriteStrikeVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& neighbor_screenshot,
    const std::string& strike_screenshot,
    const StrikePixelStats& neighbor_stats,
    const StrikePixelStats& strike_stats,
    int sim_strikes_scheduled,
    double lightning_pulse_gpu_ms,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass);

// --- Cloud-shadow smoke (, ) ---
// PARTLY-CLOUDY fixture (NOT overcast — regression review). The skybox dome renders the
// wind-advected cloud layer and the lighting pass projects the SAME coverage field
// to cast crawling terrain shadows. The camera holds a fixed downward-tilted view
// of terrain; the cloud field scrolls with the wind across the run. Two captures
// (t0, t1) are taken as a cloud-shadow edge drifts across a FIXED terrain ROI: the
// gate asserts a luminance delta in that ROI between the two times (the moving
// cast-shadow signature) AND that the cloud layer is present in the sky band. A
// clouds-OFF lighting GPU timing is captured alongside the clouds-ON timing so the
// added per-fragment cloud-shadow sample cost is bounded against the ≤ 0.4 ms
// budget (documented design, ). Render-only: the cloud field never feeds the sim .
struct CloudShadowPixelStats {
    int width = 0;
    int height = 0;
    // Fixed terrain ROI (a centred rectangle in the lower frame, on lit ground)
    // mean luminance — the cast shadow darkens this band as a cloud core crosses.
    std::uint64_t terrain_roi_pixels = 0;
    double terrain_roi_mean_luminance = 0.0;
    // Sky band cloud presence: clouds raise the mean luminance + the horizontal
    // gradient of the upper sky band over a clear dome (bright cloud edges).
    std::uint64_t sky_roi_pixels = 0;
    double sky_mean_luminance = 0.0;
    double sky_horizontal_gradient_mean = 0.0;
};

CloudShadowPixelStats
AnalyzeCloudShadowPixels(const std::vector<unsigned char>& pixels, int width, int height);

struct CloudShadowResult {
    // Two terrain-ROI captures as the shadow edge crosses.
    double terrain_roi_luminance_t0 = 0.0;
    double terrain_roi_luminance_t1 = 0.0;
    double terrain_roi_luminance_delta = 0.0; // |t1 - t0|
    // Cloud presence in the sky (from the t1, fully-clouded capture).
    double sky_mean_luminance = 0.0;
    double sky_horizontal_gradient_mean = 0.0;
    bool cloud_layer_present = false;
    // GPU timing: lighting pass with the cloud-shadow sample OFF vs ON; the added
    // cost is on - off, bounded against the budget on release.
    double lighting_gpu_ms_clouds_off = 0.0;
    double lighting_gpu_ms_clouds_on = 0.0;
    double cloud_shadow_added_ms = 0.0;
    double cloud_shadow_budget_ms = 0.4;
    bool gpu_timers_supported = false;
    // Cloud fixture parameters echoed for the artifact.
    double coverage_amount = 0.0;
    double shadow_strength = 0.0;
    double scroll_offset_t0 = 0.0;
    double scroll_offset_t1 = 0.0;
};

void WriteCloudShadowAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& terrain_t0_screenshot,
    const std::string& terrain_t1_screenshot,
    const std::string& sky_screenshot,
    const CloudShadowResult& result,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass);

// --- Particle emitter determinism smoke ---
// Spawns the fixture particle emitter, rebuilds the sim-deterministic emitter
// DESCRIPTOR SET at a fixed tick twice (from identical world state), and asserts
// the descriptor bytes are byte-equal across the two rebuilds. The emitter
// schedule is a pure function of world state; per-particle MOTION is render-only
// and is NEVER snapshotted (regression review). Also records the ParticlePass GPU-timer
// (budget ≤ 0.8 ms) and a particle render capture. The two descriptor-set hashes
// + the per-descriptor fields are written to the analysis JSON.
struct ParticleDeterminismResult {
    std::uint64_t world_seed = 0;
    std::uint64_t world_tick = 0;
    std::uint64_t descriptor_hash_run_a = 0;
    std::uint64_t descriptor_hash_run_b = 0;
    std::size_t descriptor_count = 0;
    bool byte_equal = false;
    double particle_pass_gpu_ms = 0.0;
    double particle_pass_budget_ms = 0.8;
    std::size_t particles_drawn = 0;
};

void WriteParticleEmitterDeterminismAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& particle_screenshot,
    const ParticleDeterminismResult& result,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass);

// --- Foliage instancing smoke ( / ) ---
// Drives the instanced foliage scatter over the visible live ring and snapshots
// the scatter instance set so the FoliageInstancing gate can assert, from the
// DATA (not pixels), that: (a) coverage density tracks the biome table within a
// band at fixed seeds; (b) the distance-fade is present (no instances beyond the
// live ring / fade end); (c) the wind-sway responds (calm vs windy max-tip
// displacement differs, and only swaying archetypes move); (d) the FoliagePass
// GPU-timer is within the pinned release budget. The placement hash is asserted
// reproducible (run==run) — the determinism surface.: nothing here
// writes world_hash (one-way, regression review).
struct FoliageInstancingResult {
    // Determinism: the instance-set hash from two identical rebuilds.
    std::uint64_t instance_hash_run_a = 0;
    std::uint64_t instance_hash_run_b = 0;
    bool hash_byte_equal = false;
    std::uint64_t world_seed = 0;
    // Coverage density: live instances within the live-ring radius and the
    // measured biome density they were generated against (band-checked).
    std::size_t instances_within_ring = 0;
    std::size_t instances_total = 0;
    double measured_density = 0.0;   // instances / candidate budget, normalized
    double biome_density = 0.0;      // the biome table density the scatter used
    double biome_density_band = 0.5; // |measured - biome| tolerance band
    // Distance fade: instances beyond the fade end (must be 0) + the fade band.
    std::size_t instances_beyond_fade = 0;
    double fade_start_m = 0.0;
    double fade_end_m = 0.0;
    double live_ring_radius_m = 0.0;
    // Wind sway: calm vs windy max tip displacement (must differ) + that pebbles
    // / clutter (non-swaying archetypes) carry zero displacement.
    double calm_max_sway = 0.0;
    double windy_max_sway = 0.0;
    bool sway_responds = false;
    // GPU timing.
    double foliage_gpu_ms = 0.0;
    double foliage_budget_ms = 0.6;
    bool gpu_timers_supported = false;
    std::size_t foliage_draws = 0;
    std::size_t foliage_instances_drawn = 0;
};

void WriteFoliageInstancingAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& foliage_screenshot,
    const FoliageInstancingResult& result,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass);

// --- Waterfall visual (, ) ---
// Render-only dressing on a WORLD-DETERMINISTIC site (WaterfallDetect). The
// detection is a pure function of the generated world (same seed -> same sites,
// regression review) computed render-side and CACHED, never hashed; the sheet/spray/
// foam are render-time. This result carries the determinism proof (the site
// hash from two detections + whether they match) and the dressing-capture pixel
// signatures (sheet body + spray plume + plunge foam present). The WaterfallVisual
// gate asserts both. The gtest waterfall_visual_test is the primary gate; this
// writer lets the live-engine scenario emit the same artifact schema.
struct WaterfallVisualResult {
    std::uint64_t world_seed = 0;
    std::size_t site_count = 0;
    std::uint64_t site_hash_run_a = 0;
    std::uint64_t site_hash_run_b = 0;
    bool determinism_byte_equal = false; // same world, two detections byte-equal
    bool same_seed_same_sites = false;   // separate world same seed -> same sites
    double best_drop_height = 0.0;
    double best_steepness = 0.0;
    // Dressing capture signatures (0 when no capture taken / GL unavailable).
    std::uint64_t cascade_pixels = 0;
    std::uint64_t foam_pixels = 0;
    std::uint64_t spray_pixels = 0;
    bool capture_written = false;
};

void WriteWaterfallVisualAnalysis(const std::filesystem::path& artifact_dir,
                                  const std::string& waterfall_screenshot,
                                  const WaterfallVisualResult& result);

// (The FoliagePass surface-query context/callback moved to
// rendering/FoliageSurface.h — re-exported at the top of this header.)

// --- Precipitation visual + wind-slant smoke ( / ) ---
// Rain is rendered through the  particle framework, spawned by the REPLICATED
// weather state at the camera and WIND-ADVECTED by the  wind field (rain slants
// in storms). The scenario captures two frames at the SAME camera: a CALM phase
// (zero wind -> particles fall vertically) and a WINDY phase (strong horizontal
// wind -> the falling field slants diagonally). The analysis measures, over the
// near-field precipitation band, the orientation of the bright-particle mass: a
// "slant ratio" = mean |dL/dx| / mean |dL/dy|. Calm rain is dominated by vertical
// streak columns (low horizontal gradient relative to vertical); wind-advected
// rain leans, raising the horizontal gradient component. The gate asserts
// precip particles are PRESENT in both frames (ParticlePass draws + a bright
// pixel count floor) AND that the windy slant ratio exceeds the calm one by a
// margin (the streaks demonstrably slant with wind). Particle MOTION is
// render-only and is never snapshotted/hashed (regression review, one-way).
struct PrecipPixelStats {
    int width = 0;
    int height = 0;
    std::uint64_t precip_band_pixels = 0;
    // Pixels in the precip band brighter than the bright floor (precip presence).
    std::uint64_t bright_particle_pixels = 0;
    double bright_particle_fraction = 0.0;
    // Mean |L(x+1,y)-L(x,y)| and |L(x,y+1)-L(x,y)| over bright neighborhoods.
    double horizontal_gradient_mean = 0.0;
    double vertical_gradient_mean = 0.0;
    // horizontal_gradient_mean / vertical_gradient_mean. Rises as rain slants.
    double slant_ratio = 0.0;
    //  SHAPE/QUALITY metrics so the gate catches the
    // "dark speckled dots" failure that thresholds alone passed.
    //  - streak_anisotropy: max(h,v)/min(h,v) gradient energy around bright precip.
    //    A round DOT is gradient-isotropic (~1); an elongated STREAK has one axis
    //    of sharp cross-gradient and one of smooth along-gradient (>> 1).
    double streak_anisotropy = 0.0;
    // Mean luminance of the precip BAND (the backdrop the particles sit over).
    double band_mean_luminance = 0.0;
    // Mean luminance of the BRIGHT precip pixels (must sit ABOVE the band: rain is
    // a LIGHT streak over the sky, not a DARK speck).
    double bright_particle_mean_luminance = 0.0;
    // Pixels notably DARKER than the band (dark specks: the failure signature).
    std::uint64_t dark_speck_pixels = 0;
    double dark_speck_fraction = 0.0;
};

PrecipPixelStats
AnalyzePrecipPixels(const std::vector<unsigned char>& pixels, int width, int height);

void WritePrecipitationAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& calm_screenshot,
    const std::string& windy_screenshot,
    const PrecipPixelStats& calm_stats,
    const PrecipPixelStats& windy_stats,
    const std::string& precip_type,
    double calm_wind_speed,
    double windy_wind_speed,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& calm_render_pass,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& windy_render_pass);

// --- Time-of-day sweep smoke ---
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
    double sky_mean_luminance = 0.0;     // top kSkyRoiHeightFraction of the frame
    double terrain_mean_luminance = 0.0; // bottom 25% of the frame
    double frame_mean_r = 0.0;
    double frame_mean_b = 0.0;
    double frame_r_b_ratio = 0.0;
    double terrain_r_b_ratio = 0.0;
    // sky-band color balance. sky_r_b_ratio is the
    // whole sky band; sky_warm_half_r_b_ratio is the warmer of the left/right
    // sky halves (the sun sits on one side at dusk, so the dusk sunset glow
    // raises R>B on its side without the analyzer needing the sun azimuth).
    // These let the gate require a real twilight warm-shift IN THE SKY, not
    // just in the terrain lighting, so a midday-looking dusk dome fails.
    double sky_mean_r = 0.0;
    double sky_mean_b = 0.0;
    double sky_r_b_ratio = 0.0;
    double sky_warm_half_r_b_ratio = 0.0;
    double max_luminance = 0.0;
    double max_luminance_y_from_top_norm = 0.0; // 0 = top of frame
    double sky_max_luminance = 0.0;             // max within the sky band
    //  AURORA chroma detector. The aurora paints
    // SATURATED green (g strongly exceeds BOTH r and b) curtains across the sky
    // band -- a warm low-sun sky (r>=g, yellow) does NOT produce this. We measure
    // the per-pixel GREEN EXCESS (g - max(r,b), >=0) and, crucially, COUNT pixels
    // with a STRONG green excess (the aurora curtain core). At night the strong-
    // green count is high; at day/dusk it is ~0. This catches the "aurora bleeds
    // into dusk" failure that the luminance/warm-shift thresholds passed, while
    // staying robust to the warm sky's own faint green/yellow gradient.
    double sky_green_excess_mean = 0.0;     // mean green-excess over the sky band
    double sky_green_excess_max = 0.0;      // peak green-excess (the smear core)
    double sky_strong_green_fraction = 0.0; // fraction of sky pixels with g-max(r,b) strong
    // Pixels above the emissive glow floor inside the central third of the
    // frame; only consumed by the optional night-emissive capture.
    std::uint64_t center_glow_pixels = 0;
};

TimeOfDayPixelStats
AnalyzeTimeOfDayPixels(const std::vector<unsigned char>& pixels, int width, int height);

// Phase time for a normalized sweep progress: noon / dusk / night thirds.
float TimeOfDaySweepPhaseTime(double progress);

// ---  : season sweep -------------------------------------------
// The sweep is split into TWO season halves; each half replays the
// noon/dusk/night thirds. progress<0.5 == season 0 (summer), >=0.5 == season 1
// (winter). These helpers map a normalized sweep progress to the season index,
// the within-season phase time, the season tick to push to set_season_tick, and
// the season label -- all PURE FUNCTIONS of the (tick-derived) progress, so the
// sweep stays reproducible and never consults wall-clock for the SEASON itself.
struct SeasonSweepPoint {
    int season_index = 0;          // 0 summer, 1 winter
    const char* season_label = ""; // "summer" / "winter"
    std::uint64_t season_tick = 0; // tick to feed RenderPipeline::set_season_tick
    float time_of_day = 0.0f;      // within-season noon/dusk/night phase time
    int phase_index = 0;           // 0 noon, 1 dusk, 2 night
};
SeasonSweepPoint SeasonSweepAt(double progress);
// The two season ticks the sweep pins (summer / winter solstice), pure tick math.
std::uint64_t SeasonSweepTick(int season_index);

//  the SIX season-sweep CAPTURE windows, the single
// source of truth shared by the per-frame sun PIN and the capture WRITER. Each
// entry is one screenshot the gate consumes (summer/winter x noon/dusk/night).
// The per-frame pin selects the FIRST not-yet-written plan whose progress
// threshold has been reached and pins the sun to THAT plan's phase_time/season,
// so the rendered sun the capture grabs is ALWAYS the labelled phase -- even if
// the sim hitches and several thresholds pass between frames (the old code pinned
// from SeasonSweepAt(progress), whose phase window could already have advanced to
// night by the time the throttled one-per-frame dusk capture actually wrote,
// recording a night-lit frame as "dusk" and collapsing the dusk>night ordering).
struct TimeOfDaySweepCapturePlan {
    double threshold;       // sweep progress at which to grab this capture
    const char* phase_name; // "noon" / "dusk" / "night"
    int phase_index;        // 0 noon, 1 dusk, 2 night
    float phase_time;       // pinned time-of-day (sun position)
    const char* season_label;
    int season_index; // 0 summer, 1 winter
    const char* file; // relative screenshot path
};
// Count of capture windows (two seasons x three phases).
constexpr int kTimeOfDaySweepCaptureCount = 6;
// Accessor for capture plan [0,kTimeOfDaySweepCaptureCount): single definition.
const TimeOfDaySweepCapturePlan& TimeOfDaySweepCapturePlanAt(int index);

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

EmissiveMaterialTarget FindEmissiveMaterialTarget(Luminumbra::world::GameSession* game_session,
                                                  const std::filesystem::path& root_dir);

struct TimeOfDayPhaseCapture {
    std::string name;
    double time_of_day = 0.0;
    std::string file;
    TimeOfDayPixelStats stats;
    // season dimension. The sweep captures the noon/dusk/night
    // phases under >=2 SEASONS; these record the tick-derived season state read
    // from the RenderPipeline at capture time so the analysis can assert a real
    // per-season sun-path band + palette band difference. season_label is e.g.
    // "summer"/"winter"; season_phase is the [0,1) tick-derived phase; the sun
    // elevation/declination are the sun-path metrics the season modulates.
    std::string season_label = "neutral";
    int season_index = 0; // 0 summer, 1 winter
    double season_phase = 0.0;
    double sun_elevation_rad = 0.0;
    double season_sun_declination_rad = 0.0;
    std::uint64_t season_tick = 0;
};

void WriteTimeOfDaySweepAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::vector<TimeOfDayPhaseCapture>& phases,
    const EmissiveMaterialTarget& emissive_target,
    bool emissive_capture_written,
    const std::string& emissive_screenshot,
    const TimeOfDayPixelStats& emissive_stats,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass);

ScreenshotPixelStats
AnalyzeScreenshotPixels(const std::vector<unsigned char>& pixels, int width, int height);
LodHolePixelStats
AnalyzeLodHolePixels(const std::vector<unsigned char>& pixels, int width, int height);
MaterialPixelStats
AnalyzeMaterialPixels(const std::vector<unsigned char>& pixels, int width, int height);

bool WriteBackbufferPpm(const std::filesystem::path& path,
                        int width,
                        int height,
                        ScreenshotPixelStats* out_stats = nullptr,
                        LodHolePixelStats* out_lod_hole_stats = nullptr);

std::vector<unsigned char>
BuildMaterialHeatmap(const std::vector<unsigned char>& pixels, int width, int height);

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

void WriteLodGroundScreenshotIndex(const std::filesystem::path& artifact_dir,
                                   const std::vector<std::string>& screenshots);

void WriteLodGroundVisualAnalysis(const std::filesystem::path& artifact_dir,
                                  const std::vector<LodGroundVisualCapture>& captures);

void WriteStreamingTelemetry(
    const std::filesystem::path& artifact_dir,
    const std::string& scenario,
    double duration_seconds,
    const Luminumbra::Systems::SHIELD_WorldSystem::StreamingTelemetryStats& stats);

float LodBoundaryDistance(Luminumbra::Systems::SHIELD_WorldSystem* world_system);

void ApplyLodBoundaryOscillationCamera(Luminumbra::world::GameSession* game_session,
                                       Luminumbra::Rendering::Camera* camera,
                                       double elapsed_seconds);

class LodBoundaryTransitionRecorder {
public:
    void record_frame(Luminumbra::Systems::SHIELD_WorldSystem* world_system);

    uint64_t frames_observed() const {
        return m_frames_observed;
    }
    std::size_t chunks_observed() const {
        return m_last_lod.size();
    }
    const std::unordered_map<Luminumbra::ChunkID, std::uint64_t>& transitions() const {
        return m_transitions;
    }

private:
    std::unordered_map<Luminumbra::ChunkID, int> m_last_lod;
    std::unordered_map<Luminumbra::ChunkID, std::uint64_t> m_transitions;
    uint64_t m_frames_observed = 0;
};

void WriteLodBoundaryOscillationAnalysis(const std::filesystem::path& artifact_dir,
                                         double duration_seconds,
                                         float boundary_distance,
                                         const LodBoundaryTransitionRecorder& recorder);

void ApplyLodSeamArrivalCamera(const RuntimeScenarioConfig& config,
                               Luminumbra::world::GameSession* game_session,
                               Luminumbra::Rendering::Camera* camera,
                               double elapsed_seconds);

// --- Persistence runtime roundtrip ---
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

PersistenceRoundtripPhaseResult
RunPersistenceRoundtripSavePhase(const RuntimeScenarioConfig& config,
                                 Luminumbra::world::GameSession* game_session);

PersistenceRoundtripPhaseResult
RunPersistenceRoundtripLoadPhase(const RuntimeScenarioConfig& config,
                                 Luminumbra::world::GameSession* game_session);

class LodSeamArrivalRecorder {
public:
    void record_frame(Luminumbra::Systems::SHIELD_WorldSystem* world_system);

    uint64_t frames_observed() const {
        return m_frames_observed;
    }
    std::size_t pending_lod_high_water() const {
        return m_pending_lod_high_water;
    }
    std::size_t last_pending_lod() const {
        return m_last_pending_lod;
    }

private:
    std::size_t m_pending_lod_high_water = 0;
    std::size_t m_last_pending_lod = 0;
    uint64_t m_frames_observed = 0;
};

void WriteLodSeamArrivalAnalysis(const std::filesystem::path& artifact_dir,
                                 double duration_seconds,
                                 const std::vector<LodGroundVisualCapture>& captures,
                                 const LodSeamArrivalRecorder& recorder);

// --- player_view_smoke: eye-level 360-degree coverage gate ---
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

std::vector<PlayerViewStation> BuildPlayerViewStations(Luminumbra::world::GameSession* game_session,
                                                       const std::string& world_preset);

void ApplyPlayerViewCamera(Luminumbra::world::GameSession* game_session,
                           Luminumbra::Rendering::Camera* camera,
                           const PlayerViewStation& station);

// Inward-facing frustum planes (ax+by+cz+d >= 0 inside) extracted from the
// camera's projection*view matrix (Gribb-Hartmann).
std::array<Luminumbra::Vec4, 6>
ExtractCameraFrustumPlanes(const Luminumbra::Rendering::Camera& camera, int width, int height);

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

PlayerViewPixelStats AnalyzePlayerViewPixels(const std::vector<unsigned char>& pixels,
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

// --- farlod_horizon_smoke: far-LOD horizon + live/far seam gate ---
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

void ApplyFarLodHorizonCamera(Luminumbra::world::GameSession* game_session,
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
bool ComputeFarLodBoundaryBandRows(Luminumbra::world::GameSession* game_session,
                                   const Luminumbra::Rendering::Camera& camera,
                                   int width,
                                   int height,
                                   float inner_distance_m,
                                   float outer_distance_m,
                                   int horizon_row_from_top,
                                   int& out_top_row_from_top,
                                   int& out_bottom_row_from_top);

FarLodBoundaryBandStats AnalyzeFarLodBoundaryBand(const std::vector<unsigned char>& pixels,
                                                  int width,
                                                  int height,
                                                  int band_top_row_from_top,
                                                  int band_bottom_row_from_top);

// above-horizon sky-sliver detector. The
// below-horizon sky-ratio gate is area-based and a 1-2 px near-vertical sliver
// triangle streaking up into the sky passes it (negligible area, and it sits
// ABOVE the horizon row the sky gate ignores). This pass scans the sky region
// (rows above the eye-level horizon) for connected non-sky components that are
// tall and thin and cross a large vertical fraction of the sky - the signature
// of a degenerate far-mesh sliver seen edge-on - and gates their height.
struct FarLodHorizonSkySliverStats {
    int sky_top_row_from_top = 0;    // top of the scanned sky band (0)
    int sky_bottom_row_from_top = 0; // = horizon row
    std::uint64_t sky_pixels = 0;
    // Tallest thin (width-bounded) non-sky component found in the sky band, in
    // pixels of vertical extent. 0 when the sky is clean.
    int tallest_sliver_px = 0;
    int tallest_sliver_width_px = 0;
    int tallest_sliver_col = 0;
};

// cancel_baseline (optional) is the paired far-OFF
// render of the identical camera/frame; far-ON terrain-intrusion pixels with a
// far-OFF intrusion pixel in their 3x3 neighborhood are cancelled (pixel-aligned
// live geometry), so the result is far-attributable only. nullptr = raw far-ON.
FarLodHorizonSkySliverStats
AnalyzeFarLodHorizonSkySliver(const std::vector<unsigned char>& pixels,
                              int width,
                              int height,
                              int horizon_row_from_top,
                              const std::vector<unsigned char>* cancel_baseline = nullptr);

struct FarLodHorizonStationCapture {
    FarLodHorizonStation station;
    std::string file;
    PlayerViewPixelStats sky;               // full below-horizon machinery
    FarLodBoundaryBandStats boundary;       // live/far boundary band ROI
    FarLodHorizonSkySliverStats sky_sliver; // above-horizon sliver detector (far ON)
    // far_attributable_sliver_px is the tallest
    // sliver from the MASKED analysis - the far-ON frame re-analyzed with the
    // paired far-OFF frame's intrusion pixels cancelled per-pixel within a 3x3
    // neighborhood, so pixel-aligned LIVE peak/ridge silhouettes and diagonal
    // live-geometry slivers drop out and only a genuine far-render streak (present
    // only with far-LOD on) survives. This is the ratcheted gate's metric.
    // far_off_sliver_px is the raw tallest sliver of the far-OFF measurement,
    // retained as telemetry only; -1 = no far-OFF sample.
    int far_off_sliver_px = -1;
    int far_attributable_sliver_px = 0;
    // Far-LOD scheduler state at capture time.
    std::size_t regions_wanted = 0;
    std::size_t regions_resident = 0;
    std::size_t regions_missing = 0;
    std::size_t resident_bytes = 0;
    std::size_t region_draws = 0;
    std::size_t far_indices_drawn = 0;
    // far water sheet draw/index counts at capture
    // time (proves the far water continues past the live water ring), plus the
    // boundary-band water-pixel coverage (deep-water-tinted pixels in the
    // live/far boundary ROI — non-zero where the ring ends over water).
    std::size_t water_sheet_draws = 0;
    std::size_t water_sheet_indices = 0;
    std::uint64_t boundary_band_water_pixels = 0;
    double boundary_band_water_ratio = 0.0;
    //  sun-bright warm sand-flat coverage in the same
    // boundary band ROI (the sand-flat-brightness band). The albedo_scale LUT
    // calibration keeps real dry sand below the ACES hard clip; this fraction is
    // the regression guard that the band is not a fully white-washed sand sheet.
    std::uint64_t boundary_band_sand_flat_pixels = 0;
    double boundary_band_sand_flat_ratio = 0.0;
    //   diagnostics: far-LOD scheduler activity at capture time. The
    // per-frame counters snapshot this tick's dispatch/integrate/evict; the cumulative
    // totals separate the two hypotheses for persistent mountains-preset missing regions
    // builds_failed_total > 0 means BuildPristineFarLodTile returned empty meshes
    // (hypothesis b), while a large evictions_total relative to builds_completed_total
    // means evict/rebuild thrash the 8/frame dispatch cap can't keep up with (hypothesis a).
    std::size_t builds_dispatched = 0;
    std::size_t builds_integrated_ok = 0;
    std::size_t builds_integrated_failed = 0;
    std::size_t builds_failed_total = 0;
    std::size_t builds_completed_total = 0;
    std::size_t evictions_this_frame = 0;
    std::size_t evictions_total = 0;
    std::size_t pending_depth = 0;
    //  resolved eye world position at capture time. All stations should share
    // one XZ (only yaw/pitch/height sweep), so the wanted ring is identical — this empirically
    // confirms it rather than relying on reading ApplyFarLodHorizonCamera.
    float camera_world_x = 0.0f;
    float camera_world_y = 0.0f;
    float camera_world_z = 0.0f;
};

// classifies the live/far
// boundary band ROI. out_water_pixels uses the RE-DERIVED post-aerial-
// perspective far-water classifier (proves far water continues where the live
// ring ends); out_sand_flat_pixels (optional) counts sun-bright warm sand-flat
// pixels (the sand-flat-brightness band). Pass nullptr to skip the sand count.
void AnalyzeFarLodBoundaryBandWater(const std::vector<unsigned char>& pixels,
                                    int width,
                                    int height,
                                    int band_top_row_from_top,
                                    int band_bottom_row_from_top,
                                    std::uint64_t& out_water_pixels,
                                    std::uint64_t& out_band_pixels,
                                    std::uint64_t* out_sand_flat_pixels = nullptr);

void WriteFarLodHorizonAnalysis(const std::filesystem::path& artifact_dir,
                                const std::string& world_preset,
                                double duration_seconds,
                                const std::vector<FarLodHorizonStationCapture>& captures,
                                std::size_t expected_station_count,
                                double baseline_gbuffer_gpu_ms,
                                double far_gbuffer_gpu_ms,
                                bool gpu_timers_supported,
                                bool enforce_sky_ratio);

// --- skinned_mesh_visual_smoke: skinned G-Buffer stage gate ---
// Spawns a procedurally generated rigged test mesh (LMS2 +.lanim written
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
    //   integration: ALL spawned avatar entities (ascending player_id) so a
    // network-driven driver can update each row member's transform from snapshots.
    std::vector<Luminumbra::EntityID> all_entities;
    std::vector<Luminumbra::Vec3> spawn_positions; // initial world positions (by id)
    Luminumbra::Vec3 mesh_position{0.0f};          // base of the post
    Luminumbra::Vec3 focus{0.0f};                  // arm hinge (camera aim point)
    Luminumbra::Vec3 camera_position{0.0f};
    std::string mesh_path; // absolute LMS2 path
    std::string clip_path; // absolute.lanim path
    std::string failure_reason;
};

//   integration: an in-process REPLICATION-DRIVEN avatar demo. Hosts a
// ReplicationServer + ReplicationClient over a LoopbackTransport pair inside the
// client process: the "server" walks a set of avatars (kinematic + terrain-grounded)
// and broadcasts SnapshotMsgs at a fixed snapshot rate; the client applies them
// most-recent-wins, buffers them in a SnapshotInterpolator, and Update returns the
//  INTERPOLATED positions for the renderer. So the on-screen avatars are
// driven through the real wire pipeline (server -> snapshot -> transport -> client ->
// interpolate), not by direct transforms. The Jolt-physics avatar path is proven
// headless (--replicate); this demo proves the pipeline DRIVES THE RENDER. Owns its
// transports/endpoints; engine-generic, world_hash-neutral.
class ReplicatedAvatarDemo {
public:
    // Seed the server-side avatars at these world positions (id = index). Builds the
    // loopback server/client + registers one client. snapshot_hz = broadcast rate.
    void Setup(const std::vector<Luminumbra::Vec3>& spawn_positions, double snapshot_hz = 15.0);
    // Advance dt: walk the server avatars (+Z, terrain-grounded via `world`), broadcast
    // on the snapshot cadence, pump the client, and return the render-behind interpolated
    // positions (by id). Returns the last good positions between snapshots.
    std::vector<Luminumbra::Vec3> Update(double dt_seconds,
                                         Luminumbra::Systems::SHIELD_WorldSystem* world);
    [[nodiscard]] bool ready() const {
        return m_ready;
    }

private:
    std::unique_ptr<Luminumbra::Net::LoopbackTransport> m_server_tp;
    std::unique_ptr<Luminumbra::Net::LoopbackTransport> m_client_tp;
    std::unique_ptr<Luminumbra::Net::ReplicationServer> m_server;
    std::unique_ptr<Luminumbra::Net::ReplicationClient> m_client;
    std::unique_ptr<Luminumbra::Net::SnapshotInterpolator> m_interp;
    std::vector<Luminumbra::Vec3> m_server_pos; // server-side authoritative positions
    double m_accum = 0.0;
    double m_period = 1.0 / 15.0;
    std::uint64_t m_tick = 0;
    bool m_ready = false;
};

SkinnedMeshVisualTarget SpawnSkinnedMeshVisualEntity(Luminumbra::world::GameSession* game_session,
                                                     const std::filesystem::path& artifact_dir,
                                                     const std::filesystem::path& root_dir = {},
                                                     int avatar_count = 1);

void ApplySkinnedMeshVisualCamera(Luminumbra::Rendering::Camera* camera,
                                  const SkinnedMeshVisualTarget& target);

// Animation clock of the spawned entity's player component (seconds), -1.0
// when the entity is gone.
double SkinnedMeshVisualAnimationTime(Luminumbra::world::GameSession* game_session,
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
    //  textured-response: spatial color variation across the mesh-like
    // pixels in capture A (mean per-channel std-dev, 0..255). A flat-colored
    // (untextured) creature reads near-uniform; the authored creature
    // texture drives this well above the flat bound.
    double mesh_color_stddev_a = 0.0;
};

SkinnedMeshDiffStats AnalyzeSkinnedMeshCaptures(const std::vector<unsigned char>& pixels_a,
                                                const std::vector<unsigned char>& pixels_b,
                                                int width,
                                                int height);

void WriteSkinnedMeshVisualAnalysis(const std::filesystem::path& artifact_dir,
                                    const SkinnedMeshVisualTarget& target,
                                    const SkinnedMeshVisualCapture& capture_a,
                                    const SkinnedMeshVisualCapture& capture_b,
                                    const SkinnedMeshDiffStats& diff);

// --- creature_slice_smoke: Project Capture game slice ---
// One MVP creature (pure game data: the archetype JSON named by
// --creature-archetype plus its rigged assets under data/models/) is
// spawned near the archipelago spawn: rigged LMS2 mesh + idle/walk clips on
// the skinned G-Buffer stage, needs/opportunities planned by the
// fixed-tick InstinctSystem. Mid-run a light stimulus appears (a
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
    bool ecology_locomotion = false;
    bool ecology_scent_emitter = false;
    bool ecology_scent_sense = false;
    bool ecology_perception = false;
    std::string ecology_scent_hash;
    // Loaded archetype JSON (slice/creature blocks consumed at runtime).
    nlohmann::json archetype;
};

CreatureSliceScene SpawnCreatureSliceScene(Luminumbra::world::GameSession* game_session,
                                           const std::filesystem::path& root_dir,
                                           const std::string& archetype_relative_path);

// Spawns the light stimulus: an emissive-material prop plus the curiosity
// opportunity from the archetype's slice block.
bool SpawnCreatureSliceStimulus(Luminumbra::world::GameSession* game_session,
                                CreatureSliceScene& scene);

// Per-frame game glue: planner-action -> clip selection (graze=idle,
// approach=walk, from the archetype's clip_by_action map) and approach
// locomotion (walk toward the plan target, terrain-following, facing the
// movement direction).
void UpdateCreatureSliceScene(Luminumbra::world::GameSession* game_session,
                              CreatureSliceScene& scene,
                              double dt);

// Live photographic framing: follows the creature, keeps the active target
// (graze spot before the stimulus, the glow after) in frame, and lifts the
// camera over intervening terrain ridges.
void ApplyCreatureSliceCamera(Luminumbra::world::GameSession* game_session,
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

CreatureSlicePlanProbe ProbeCreatureSlicePlan(Luminumbra::world::GameSession* game_session,
                                              const CreatureSliceScene& scene);

//  composition check: a "functionally green, visually broken" capture
// (creature rendered, planner correct, but the camera stares at the ground or
// the sky, or the creature is camouflaged against its own terrain) must not
// pass. sky_ratio proves a horizon is in frame; creature_terrain_color_delta
// proves the creature reads against the surrounding terrain.
struct CreatureSliceComposition {
    bool valid = false;     // the creature projected into the frame
    double sky_ratio = 0.0; // fraction of frame pixels classified as sky
    double creature_roi_mean[3] = {0.0, 0.0, 0.0};
    double terrain_ref_mean[3] = {0.0, 0.0, 0.0};
    double creature_terrain_color_delta = 0.0; // L1 distance between the means
    std::size_t creature_roi_pixels = 0;
    std::size_t terrain_ref_pixels = 0;
    int creature_screen_x = 0; // from left
    int creature_screen_y = 0; // from top
    //  emissive glow halo around the glow_bloom stimulus prop. A real
    // bloom/glow reads as a bright core with luminance that FALLS OFF into a
    // surrounding ring still brighter than the far background (a halo). These
    // are 0 when the stimulus is absent/off-frame.
    bool glow_measured = false;
    double glow_core_luminance = 0.0;       // mean luminance of the inner disc
    double glow_ring_luminance = 0.0;       // mean luminance of the falloff annulus
    double glow_background_luminance = 0.0; // mean luminance of the far background
    int stimulus_screen_x = -1;
    int stimulus_screen_y = -1;
};

// Analyzes a captured RGB framebuffer (bottom-up glReadPixels layout) for the
// creature-slice composition metrics. creature_screen_x/y are in top-left
// pixel coordinates (the projected creature position); pass valid=false-making
// out-of-frame coordinates and the ROI metrics stay zero.
CreatureSliceComposition AnalyzeCreatureSliceComposition(const std::vector<unsigned char>& pixels,
                                                         int width,
                                                         int height,
                                                         int creature_screen_x_from_left,
                                                         int creature_screen_y_from_top,
                                                         int stimulus_screen_x_from_left = -1,
                                                         int stimulus_screen_y_from_top = -1);

struct CreatureSliceCapture {
    std::string file;
    double elapsed_seconds = 0.0;
    CreatureSlicePlanProbe plan;
    std::size_t skinned_draws = 0;
    std::size_t skinned_indices_drawn = 0;
    CreatureSliceComposition composition;
};

void WriteCreatureSliceAnalysis(const std::filesystem::path& artifact_dir,
                                const CreatureSliceScene& scene,
                                const CreatureSliceCapture& before,
                                const CreatureSliceCapture& after);

// --- window_mode_stress_smoke: resize-stress gate ---
// A scripted run that exercises the render-target resize chain mid-run: it
// drives RenderPipeline::on_resize through a sequence of framebuffer sizes
// (simulating windowed->borderless->windowed plus several resolutions) and
// records, per step, the resize-generation delta and the GL error count. After
// the resize cycle the framebuffer is restored to the pinned capture size and a
// Smoke-equivalent screenshot is captured. The gate asserts:
//   - every step that changed the size reallocated targets (generation bumped),
//   - zero GL errors across the whole cycle,
//   - the final state is restored to the pinned 1280x720 targets, and
//   - the pinned capture meets the visual-Smoke water/sky expectations
//     (ScreenshotPixelStats, same predicates the auto-world smoke uses).
struct WindowModeStressStep {
    std::string label; // e.g. "borderless_1920x1080"
    int width = 0;
    int height = 0;
    bool size_changed = false; // did this step change the framebuffer size?
    std::uint64_t resize_generation_before = 0;
    std::uint64_t resize_generation_after = 0;
    std::uint64_t gl_errors_after = 0;
    int targets_width_after = 0; // RenderPipeline::screen_width() after
    int targets_height_after = 0;
};

// The scripted resize sequence (label/width/height). The pinned capture size is
// the final entry. Intermediate sizes intentionally stress non-pinned targets.
std::vector<WindowModeStressStep> BuildWindowModeStressSequence();

struct WindowModeStressCapture {
    std::string file;
    int width = 0;
    int height = 0;
    ScreenshotPixelStats pixels;
};

void WriteWindowModeStressAnalysis(const std::filesystem::path& artifact_dir,
                                   double duration_seconds,
                                   WindowMode requested_window_mode,
                                   const std::vector<WindowModeStressStep>& steps,
                                   const WindowModeStressCapture& final_capture);

// --- networked_session_smoke: client renders a server-owned world ---
// over the lockstep transport. A LockstepSession pair runs over an in-process
// LoopbackTransport (no sockets/ports -- gate stability). One peer is a HOST
// world authority (a headless GameSession stepped in the same process); the
// other peer is the CLIENT's rendering GameSession. Per agreed tick:
//   * the client's WORLD-AFFECTING input set (empty today -- no gameplay inputs
//     yet) round-trips through LockstepSession::*Input and is APPLIED, so the
//     input blob travels the real path even while it is empty;
//   * both worlds advance EXACTLY one fixed sim tick from the SAME spawn anchor
//     (the server-owned streaming position), so they stay byte-identical;
//   * camera LOOK (yaw/pitch) is NOT carried here -- it stays render-side and is
//     applied locally each frame, so look latency is zero (research
//     worldgen-lockstep-sdfrt.md Area 2 takeaway 2: "Camera look must remain
//     client-local... only quantized movement/interaction intents enter the
//     lockstep input stream");
//   * the peers exchange world_hash + sub-hashes at the cadence (the desync
//     oracle), proving the client world == the host world.
// The driver OWNS the host authority + both session ends; the client's render
// GameSession is supplied by the caller. The hashed world step uses the spawn
// anchor (NOT the camera) so render-side look can never perturb the hash.
class NetworkedSessionDriver {
public:
    struct Config {
        std::uint64_t seed = 424242;
        std::string preset = "default";
        std::uint64_t budget_ticks = 90;
        std::uint64_t hash_cadence_ticks = 30;
        std::string root_path;
        int surface_radius = 12;
        int collision_radius = 4;
    };

    NetworkedSessionDriver();
    ~NetworkedSessionDriver();

    NetworkedSessionDriver(const NetworkedSessionDriver&) = delete;
    NetworkedSessionDriver& operator=(const NetworkedSessionDriver&) = delete;

    // Boots the host authority world, builds the loopback pair + both session
    // ends, and completes the handshake. The client GameSession must already be
    // world-ready (its world streamed to the spawn anchor). Returns false on any
    // boot/handshake failure (failure_reason carries the cause).
    bool Begin(Luminumbra::world::GameSession* client_session, const Config& config);

    // Advances the lockstep session by AT MOST one agreed tick on BOTH peers.
    // Each agreed tick steps both worlds one fixed sim tick from the spawn
    // anchor and, at the cadence, exchanges + compares hashes. Returns true while
    // the session is still live (more ticks to run); false once it has finished,
    // disconnected, or desynced (terminal). Idempotent after termination.
    bool StepAgreedTick();

    // The server-owned streaming anchor the CLIENT world must stream around
    // (the spawn point). Render-side camera look is independent of this.
    Luminumbra::Vec3 ClientStreamingAnchor() const {
        return m_spawn_anchor;
    }

    [[nodiscard]] bool finished() const {
        return m_finished;
    }
    [[nodiscard]] bool desynced() const {
        return m_desynced;
    }
    [[nodiscard]] std::uint64_t agreed_ticks() const {
        return m_agreed_ticks;
    }
    [[nodiscard]] const std::string& failure_reason() const {
        return m_failure_reason;
    }

    // Sends a clean Bye on both ends and tears down the host world. Idempotent.
    void Disconnect();

    // Writes the runtime artifact (schema luminumbra.networked_session.v1) and
    // returns whether the session met its in-sync + clean-disconnect contract.
    bool WriteArtifact(const std::filesystem::path& artifact_dir, double duration_seconds);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    Luminumbra::Vec3 m_spawn_anchor{0.0f};
    std::uint64_t m_agreed_ticks = 0;
    bool m_finished = false;
    bool m_desynced = false;
    bool m_disconnected = false;
    std::string m_failure_reason;
};

// --- world_visual_sweep ---------------------------------
// A self-contained, DETERMINISTIC capture matrix for orchestrator visual review.
// Once the world is ready the driver renders a fixed matrix of
//   times-of-day x camera angles x weather (x optional season)
// from a single feature-rich anchor (open sky + foliage ground + water/shore),
// driving the EXISTING render systems through their one-way bridges
// (set_time_of_day, set_weather_state/set_cloud_state/set_lightning_state, the
// FoliagePass scatter, ParticlePass rain). Each cell is written as
//   <artifact_dir>/sweep/<tod>__<angle>__<weather>[__<season>].ppm
// plus a sweep-manifest.json carrying, per cell, the active feature signals
// (foliage_draws / particle_draws / lightning active / cloud coverage / water
// pixels) and a non-black check, so the WorldVisualSweep gate can assert
// PRODUCTION + PRESENCE offline.: nothing here writes world_hash.
//
// `render_one_frame` is supplied by the caller (main_client owns the GL context,
// the RenderPipeline, the camera, and the swap); the harness calls it to advance
// + present a settled frame and hands back the captured backbuffer. Returns true
// when every expected cell PPM was produced and non-black.
struct WorldVisualSweepDeps {
    Luminumbra::world::GameSession* game_session = nullptr;
    Luminumbra::Rendering::RenderPipeline* pipeline = nullptr;
    Luminumbra::Rendering::Camera* camera = nullptr;
    std::filesystem::path root_dir;
    std::filesystem::path artifact_dir;
    // Renders ONE frame with the current camera/pipeline state and presents it,
    // then reads the backbuffer into `out_pixels` (RGB, bottom-up glReadPixels
    // layout) and reports the framebuffer size. Returns false on a GL/size error.
    std::function<bool(std::vector<unsigned char>& out_pixels, int& width, int& height)>
        render_and_read;
    // Whether to also capture the winter season pass (summer is always captured).
    bool include_winter = true;
};

bool RunWorldVisualSweep(const WorldVisualSweepDeps& deps);

} // namespace Luminumbra::Client::ScenarioHarness
