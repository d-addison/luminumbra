#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <glad/glad.h>

#include "core/Log.h"
#include "core/RuntimeScenarioHarness.h"
#include "core/scenarios/ScenarioCommon.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h" //  species base_color -> creature tint
#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/CreatureProcgen.h" //  genome -> body-proportion build
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include "rendering/Camera.h"
#include "rendering/LightningBolt.h"
#include "rendering/passes/FoliagePass.h"
#include "rendering/passes/ParticlePass.h"
// lockstep transport seam (engine-generic; ILockstepTransport +
// LoopbackTransport + LockstepSession). Named SendFrame/TryReceiveFrame to dodge
// the <windows.h> SendMessage macro (see LockstepSession.h note).
#include "luminumbra_common/net/LockstepSession.h"
//  (AU1): atmosphere audio telemetry. The harness sweeps the replicated
// weather/wind state through the REAL EnvironmentalAudioSystem atmosphere model +
// the AudioPropagationSystem ambience bed and emits the AtmosphereAudio artifact.
// Client-side dressing only -- no world_hash, no visual-gate dependency.
#include "audio/AudioPropagationSystem.h"
#include "audio/EnvironmentalAudioSystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace Luminumbra::Client::ScenarioHarness {

// --- Skybox visual smoke ---

Luminumbra::Vec3 TowardSunDirection(float time_of_day) {
    // Mirrors RenderPipeline::update_time_of_day: the pipeline stores the
    // light-travel direction; the toward-sun direction is its negation.
    const float sun_angle_rad = time_of_day * 2.0f * glm::pi<float>();
    const glm::vec3 light_direction =
        glm::normalize(glm::vec3(std::sin(sun_angle_rad), -std::cos(sun_angle_rad), -0.2f));
    return -light_direction;
}

void ApplySkyboxVisualCamera(Luminumbra::world::GameSession* game_session,
                             Luminumbra::Rendering::Camera* camera,
                             float pinned_time_of_day) {
    if (!camera || !game_session) {
        return;
    }

    auto* world_system = game_session->GetWorldSystem();
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;
    const float terrain_height =
        world_system ? world_system->GetTerrainHeightAt(spawn.x, spawn.z) : spawn.y;
    camera->Position =
        Luminumbra::Vec3(spawn.x, std::max(spawn.y, terrain_height) + 24.0f, spawn.z);

    // Aim the yaw at the sun azimuth so the noon sun disc (elevation ~71.6
    // degrees at t=0.04) lands inside the widened frame; the pitch stays at
    // the scenario's fixed 30-degree upward tilt.
    const Luminumbra::Vec3 toward_sun = TowardSunDirection(pinned_time_of_day);
    camera->Yaw = glm::degrees(std::atan2(toward_sun.z, toward_sun.x));
    camera->Pitch = 30.0f;
    camera->Zoom = 90.0f; // wide vertical FOV: horizon in frame at the bottom, sun near the top
    camera->updateCameraVectors();
}

bool ProjectDirectionToScreen(const Luminumbra::Rendering::Camera& camera,
                              int width,
                              int height,
                              const Luminumbra::Vec3& direction,
                              double& x_norm,
                              double& y_norm_from_top) {
    x_norm = 0.0;
    y_norm_from_top = 0.0;
    if (width <= 0 || height <= 0) {
        return false;
    }
    const glm::mat3 view_rotation = glm::mat3(camera.GetViewMatrix());
    const glm::vec3 view_dir = view_rotation * glm::vec3(direction);
    if (view_dir.z >= -1.0e-4f) {
        return false; // behind or parallel to the camera plane
    }
    const glm::mat4 projection =
        glm::perspective(glm::radians(camera.Zoom),
                         static_cast<float>(width) / static_cast<float>(height),
                         camera.GetNearPlane(),
                         camera.GetFarPlane());
    const glm::vec4 clip = projection * glm::vec4(view_dir, 0.0f); // point at infinity
    if (clip.w <= 0.0f) {
        return false;
    }
    const float ndc_x = clip.x / clip.w;
    const float ndc_y = clip.y / clip.w;
    x_norm = (static_cast<double>(ndc_x) + 1.0) * 0.5;
    y_norm_from_top = 1.0 - (static_cast<double>(ndc_y) + 1.0) * 0.5;
    return ndc_x >= -1.0f && ndc_x <= 1.0f && ndc_y >= -1.0f && ndc_y <= 1.0f;
}

namespace {

// Sun-disc classification ( part B). At NOON the open pale dome ALSO rides
// the ACES tonemap ceiling (~250-253 luminance), so the old 243 threshold tagged
// the whole bright dome as "sun" and the cluster could never localize. The shader
// now forces a tight disc core to PURE WHITE (255) post-tonemap -- above the dome
// ceiling -- so a 254 threshold isolates the genuine disc from the saturated dome.
// (Dusk/dawn discs are warm/low and are gated by TimeOfDaySweep, not this noon
// smoke.) Calibrated: measured noon dome max ~253.5, forced disc core = 255.
constexpr double kSunDiscMinLuminance = 254.0;

// Sky ROI: with pitch +30 and a 90-degree vertical FOV the horizon projects
// ~79% down the frame; the top 55% is guaranteed sky.
constexpr int kSkyboxGradientBands = 6;

// Sun-disc cluster radius around the expected screen position, as a fraction
// of the frame height. The disc spans ~11 degrees (smoothstep 0.995..0.9999)
// which projects to roughly a 100 px radius near the top of a 720 px frame
// at 90-degree vertical FOV; 0.3 * height leaves room for the corona halo.
constexpr double kSunClusterRadiusFraction = 0.30;

// Gradient-band exclusion radius around the sun: the corona glow
// (pow(cosTheta, 32)) brightens the sky for tens of degrees around the disc
// and would mask the horizon->zenith atmosphere gradient, so band means are
// computed from sky pixels outside this radius.
constexpr double kSunGradientExclusionRadiusFraction = 0.45;

} // namespace

SkyboxPixelStats AnalyzeSkyboxPixels(const std::vector<unsigned char>& pixels,
                                     int width,
                                     int height,
                                     double sun_screen_x_norm,
                                     double sun_screen_y_norm,
                                     bool sun_on_screen) {
    SkyboxPixelStats stats;
    stats.width = width;
    stats.height = height;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    stats.bands.assign(static_cast<std::size_t>(kSkyboxGradientBands), {});
    const int sky_rows =
        std::max(1, static_cast<int>(static_cast<double>(height) * kSkyRoiHeightFraction));
    const int min_x = width / 64;
    const int max_x = width - min_x;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    double sun_centroid_x_accum = 0.0;
    double sun_centroid_y_accum = 0.0;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        if (y_from_top >= sky_rows) {
            continue;
        }
        const int band_from_zenith =
            std::min(kSkyboxGradientBands - 1, (y_from_top * kSkyboxGradientBands) / sky_rows);
        const int band_from_horizon = kSkyboxGradientBands - 1 - band_from_zenith;

        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            const double luminance = PixelLuminance(r, g, b);
            ++stats.sky_roi_pixels;
            stats.max_luminance = std::max(stats.max_luminance, luminance);

            if (luminance >= kSunDiscMinLuminance) {
                ++stats.sun_disc_pixels;
                sun_centroid_x_accum += static_cast<double>(x) / static_cast<double>(width);
                sun_centroid_y_accum +=
                    static_cast<double>(y_from_top) / static_cast<double>(height);
                if (sun_on_screen) {
                    const double dx =
                        static_cast<double>(x) - sun_screen_x_norm * static_cast<double>(width);
                    const double dy = static_cast<double>(y_from_top) -
                                      sun_screen_y_norm * static_cast<double>(height);
                    const double cluster_radius_px =
                        kSunClusterRadiusFraction * static_cast<double>(height);
                    if (dx * dx + dy * dy <= cluster_radius_px * cluster_radius_px) {
                        ++stats.sun_disc_pixels_near_expected;
                    }
                }
                // Sun-disc pixels are excluded from the gradient bands so the
                // gradient check measures atmosphere, not the disc.
                continue;
            }

            if (sun_on_screen) {
                const double dx =
                    static_cast<double>(x) - sun_screen_x_norm * static_cast<double>(width);
                const double dy = static_cast<double>(y_from_top) -
                                  sun_screen_y_norm * static_cast<double>(height);
                const double exclusion_radius_px =
                    kSunGradientExclusionRadiusFraction * static_cast<double>(height);
                if (dx * dx + dy * dy <= exclusion_radius_px * exclusion_radius_px) {
                    continue; // corona glow region: keep it out of the gradient bands
                }
            }

            SkyboxVisualBandStats& band = stats.bands[static_cast<std::size_t>(band_from_horizon)];
            band.mean_luminance += luminance;
            band.mean_r += static_cast<double>(r); //  palette-emergence
            band.mean_b += static_cast<double>(b);
            ++band.pixels;
        }
    }

    for (SkyboxVisualBandStats& band : stats.bands) {
        if (band.pixels > 0) {
            band.mean_luminance /= static_cast<double>(band.pixels);
            band.mean_r /= static_cast<double>(band.pixels);
            band.mean_b /= static_cast<double>(band.pixels);
        }
    }
    stats.horizon_band_mean = stats.bands.front().mean_luminance;
    stats.zenith_band_mean = stats.bands.back().mean_luminance;
    //  warm/cool band R/B ratios for the low-sun scattering palette.
    stats.horizon_band_r_b_ratio = stats.bands.front().mean_b > 1.0
                                       ? stats.bands.front().mean_r / stats.bands.front().mean_b
                                       : 0.0;
    stats.zenith_band_r_b_ratio = stats.bands.back().mean_b > 1.0
                                      ? stats.bands.back().mean_r / stats.bands.back().mean_b
                                      : 0.0;

    // "Monotonic-ish": count adjacent horizon->zenith transitions where the
    // luminance rises by more than a small tolerance (clouds add noise).
    constexpr double kBandRiseTolerance = 2.0;
    for (int i = 0; i + 1 < kSkyboxGradientBands; ++i) {
        if (stats.bands[static_cast<std::size_t>(i + 1)].mean_luminance >
            stats.bands[static_cast<std::size_t>(i)].mean_luminance + kBandRiseTolerance) {
            ++stats.monotonic_violations;
        }
    }

    if (stats.sun_disc_pixels > 0) {
        stats.sun_disc_centroid_x =
            sun_centroid_x_accum / static_cast<double>(stats.sun_disc_pixels);
        stats.sun_disc_centroid_y =
            sun_centroid_y_accum / static_cast<double>(stats.sun_disc_pixels);
    }
    return stats;
}

void WriteSkyboxVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& screenshot,
    const SkyboxPixelStats& pixel_stats,
    double sun_screen_x_norm,
    double sun_screen_y_norm,
    bool sun_on_screen,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass) {
    // part A: the SkyboxVisual smoke is pinned at t=0.04, which
    // is NOON (sun elevation ~71.6 deg, near zenith) -- not a low/sunset sun. The
    // original gradient + palette_emergence premises were SUNSET physics (horizon
    // brighter than the zenith; a warm R>B horizon band warmer than the cool
    // zenith) and are INVALID at noon: at a high sun the dome is brightest near the
    // overhead sun, so horizon <= zenith, and the warm low-sun palette has not
    // emerged. Those sunset checks are owned by TimeOfDaySweep (dusk/dawn), which
    // is NOT weakened here. The noon gate instead asserts a NOON-appropriate dome:
    //   * SMOOTH  -- no harsh inter-band luminance banding (kMaxAdjacentBandStep),
    //   * BRIGHT  -- both the horizon and zenith bands sit above a daylight floor
    //                (kMinDaylightBandLuminance), i.e. the dome is genuinely lit,
    //   * NOT INVERTED-DARK -- the horizon->zenith spread stays within a sane band
    //                (|drop| <= kMaxNoonHorizonZenithSpread), so neither a freak
    //                dark-overhead inversion nor a runaway blow-out passes.
    // The sun-disc localization (part B tightens the shader) stays a REAL check.
    constexpr double kMaxAdjacentBandStep = 60.0;      // max |Lum step| between adjacent bands
    constexpr double kMinDaylightBandLuminance = 60.0; // both bands lit (noon daylight floor)
    constexpr double kMaxNoonHorizonZenithSpread =
        90.0; // |horizon-zenith| bound (no inversion/blowout)
    const std::uint64_t kMinSunDiscPixels =
        static_cast<std::uint64_t>(ScalePinnedArea(50, kCapturePinnedWidth, kCapturePinnedHeight));
    constexpr double kMinSunClusterFraction = 0.6;
    // GPU-timer budgets (PINNED documented design). Enforced on RELEASE by the PS1 gate
    // (debug is ~10x slower --  wind precedent); the harness emits the raw
    // measurements + within-budget flags either way.
    constexpr double kAerialBudgetMs = 0.3;
    constexpr double kSkyViewRefreshBudgetMs = 0.2;
    // sky_full_precompute is the ONE-TIME startup LUT build (SkyAtmosphereLut::initialize,
    // wall-clock, seeded once at world init and carried per-frame for reporting) — NOT a
    // per-frame GPU cost. The old 8.0 ms budget mis-applied a per-frame-style ceiling to a
    // one-shot startup metric (stale, same class as the noon-premise staleness fixed in );
    // the real per-frame sky cost is gated by kAerial/kSkyViewRefresh above (both well under).
    // Re-budgeted to a one-time-startup bound: tolerates the legitimate ~32 ms cold build with
    // headroom while still failing a gross startup regression.
    constexpr double kSkyPrecomputeBudgetMs = 64.0;

    const double sun_cluster_fraction =
        pixel_stats.sun_disc_pixels > 0
            ? static_cast<double>(pixel_stats.sun_disc_pixels_near_expected) /
                  static_cast<double>(pixel_stats.sun_disc_pixels)
            : 0.0;
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    //  part A: NOON-appropriate dome check (smooth + bright + not inverted).
    double max_adjacent_band_step = 0.0;
    for (std::size_t i = 0; i + 1 < pixel_stats.bands.size(); ++i) {
        max_adjacent_band_step = std::max(max_adjacent_band_step,
                                          std::abs(pixel_stats.bands[i + 1].mean_luminance -
                                                   pixel_stats.bands[i].mean_luminance));
    }
    const double horizon_zenith_spread =
        std::abs(pixel_stats.horizon_band_mean - pixel_stats.zenith_band_mean);
    const bool gradient_passed = max_adjacent_band_step <= kMaxAdjacentBandStep &&
                                 pixel_stats.horizon_band_mean >= kMinDaylightBandLuminance &&
                                 pixel_stats.zenith_band_mean >= kMinDaylightBandLuminance &&
                                 horizon_zenith_spread <= kMaxNoonHorizonZenithSpread;
    const bool sun_disc_passed = sun_on_screen &&
                                 pixel_stats.sun_disc_pixels >= kMinSunDiscPixels &&
                                 sun_cluster_fraction >= kMinSunClusterFraction;
    //  part A: palette_emergence (warm low-sun horizon band) is SUNSET physics
    // and does not hold at noon -- the values are still EMITTED for diagnostics but
    // are no longer a pass gate here (dusk/dawn warmth is gated by TimeOfDaySweep).
    const double horizon_over_zenith_warm_gap =
        pixel_stats.horizon_band_r_b_ratio - pixel_stats.zenith_band_r_b_ratio;
    // GPU-timer correctness (non-negative, supported). Budget enforcement is the
    // PS1 gate's job on release.
    const bool aerial_within_budget = render_pass.aerial_gpu_ms <= kAerialBudgetMs;
    const bool sky_view_refresh_within_budget =
        render_pass.sky_view_refresh_ms <= kSkyViewRefreshBudgetMs;
    const bool sky_precompute_within_budget =
        render_pass.sky_full_precompute_ms <= kSkyPrecomputeBudgetMs;
    const bool passed =
        render_pass.skybox_draws > 0 && gradient_passed && sun_disc_passed && gl_debug.errors == 0;

    nlohmann::json bands = nlohmann::json::array();
    for (std::size_t i = 0; i < pixel_stats.bands.size(); ++i) {
        bands.push_back({{"band_from_horizon", i},
                         {"mean_luminance", pixel_stats.bands[i].mean_luminance},
                         {"mean_r", pixel_stats.bands[i].mean_r},
                         {"mean_b", pixel_stats.bands[i].mean_b},
                         {"pixels", pixel_stats.bands[i].pixels}});
    }

    nlohmann::json artifact = {
        {"schema", "luminumbra.skybox_visual.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"screenshot", screenshot},
        {"pinned_time_of_day", 0.04},
        {"gradient",
         {{"passed", gradient_passed},
          {"bands", bands},
          {"horizon_band_mean", pixel_stats.horizon_band_mean},
          {"zenith_band_mean", pixel_stats.zenith_band_mean},
          {"horizon_zenith_drop", pixel_stats.horizon_band_mean - pixel_stats.zenith_band_mean},
          //  part A: NOON-appropriate dome metrics (smooth + bright + not
          // inverted). |horizon-zenith| spread + max adjacent-band luminance step
          // replace the old sunset "horizon brighter by >=8" + monotonic-fall.
          {"horizon_zenith_spread", horizon_zenith_spread},
          {"max_adjacent_band_step", max_adjacent_band_step},
          {"monotonic_violations", pixel_stats.monotonic_violations}}},
        {"sun_disc",
         {{"passed", sun_disc_passed},
          {"on_screen", sun_on_screen},
          {"expected_screen_x", sun_screen_x_norm},
          {"expected_screen_y_from_top", sun_screen_y_norm},
          {"pixels", pixel_stats.sun_disc_pixels},
          {"pixels_near_expected", pixel_stats.sun_disc_pixels_near_expected},
          {"sun_cluster_fraction", sun_cluster_fraction},
          {"centroid_x", pixel_stats.sun_disc_centroid_x},
          {"centroid_y_from_top", pixel_stats.sun_disc_centroid_y},
          {"max_luminance", pixel_stats.max_luminance}}},
        {"palette_emergence",
         {//  part A: DIAGNOSTIC ONLY at noon. The warm low-sun horizon band
          // is sunset physics (gated by TimeOfDaySweep dusk/dawn); it is NOT a
          // pass gate for the noon SkyboxVisual smoke. Values kept for telemetry.
          {"gated", false},
          {"diagnostic_only", true},
          {"horizon_band_r_b_ratio", pixel_stats.horizon_band_r_b_ratio},
          {"zenith_band_r_b_ratio", pixel_stats.zenith_band_r_b_ratio},
          {"horizon_over_zenith_warm_gap", horizon_over_zenith_warm_gap}}},
        {"gpu_timer",
         {//  per-pass GPU timers for the aerial term + sky precompute.
          // Budgets enforced on RELEASE by the PS1 gate (debug ~10x slower).
          {"supported", render_pass.gpu_timers_supported},
          {"aerial_gpu_ms", render_pass.aerial_gpu_ms},
          {"aerial_budget_ms", kAerialBudgetMs},
          {"aerial_within_budget", aerial_within_budget},
          {"sky_view_refresh_ms", render_pass.sky_view_refresh_ms},
          {"sky_view_refresh_budget_ms", kSkyViewRefreshBudgetMs},
          {"sky_view_refresh_within_budget", sky_view_refresh_within_budget},
          {"sky_full_precompute_ms", render_pass.sky_full_precompute_ms},
          {"sky_precompute_budget_ms", kSkyPrecomputeBudgetMs},
          {"sky_precompute_within_budget", sky_precompute_within_budget}}},
        {"roi",
         {{"width", pixel_stats.width},
          {"height", pixel_stats.height},
          {"sky_roi_pixels", pixel_stats.sky_roi_pixels},
          {"sky_roi_height_fraction", kSkyRoiHeightFraction}}},
        {"thresholds",
         {//  part A: NOON dome thresholds (smooth + bright + not inverted).
          {"max_adjacent_band_step", kMaxAdjacentBandStep},
          {"min_daylight_band_luminance", kMinDaylightBandLuminance},
          {"max_noon_horizon_zenith_spread", kMaxNoonHorizonZenithSpread},
          {"min_sun_disc_pixels", kMinSunDiscPixels},
          {"min_sun_cluster_fraction", kMinSunClusterFraction},
          {"sun_cluster_radius_fraction", kSunClusterRadiusFraction},
          {"sun_gradient_exclusion_radius_fraction", kSunGradientExclusionRadiusFraction},
          {"sun_disc_min_luminance", kSunDiscMinLuminance}}},
        {"render_pass",
         {{"skybox_draws", render_pass.skybox_draws},
          {"terrain_draws", render_pass.terrain_draws}}},
        {"gl_debug",
         {{"messages", gl_debug.messages},
          {"errors", gl_debug.errors},
          {"warnings", gl_debug.warnings},
          {"notifications", gl_debug.notifications}}}};

    std::ofstream output(artifact_dir / "skybox-visual-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
