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

// --- Precipitation visual + wind-slant smoke ( / ) ---

PrecipPixelStats
AnalyzePrecipPixels(const std::vector<unsigned char>& pixels, int width, int height) {
    PrecipPixelStats stats;
    stats.width = width;
    stats.height = height;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    // Precip band: the middle vertical span of the frame where the falling rain
    // field renders (skip the extreme top sky and the bottom HUD/terrain glare so
    // the orientation measure isolates the particle streaks). Horizontal margins
    // trim the frame edges.
    const int band_top = height / 8;             // skip top 1/8
    const int band_bottom = height - height / 6; // skip bottom ~1/6
    const int min_x = width / 16;
    const int max_x = width - min_x;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    // Bright floor: rain/snow particles are the brightest moving structure over
    // the (overcast, darkened) backdrop. Measured relative to the band mean so the
    // analysis is robust to overall exposure.
    double band_luminance_accum = 0.0;
    std::uint64_t band_pixels = 0;
    for (int y = band_top; y < band_bottom; ++y) {
        for (int x = min_x; x < max_x; ++x) {
            const std::size_t off =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            band_luminance_accum += PixelLuminance(pixels[off], pixels[off + 1u], pixels[off + 2u]);
            ++band_pixels;
        }
    }
    stats.precip_band_pixels = band_pixels;
    const double band_mean =
        band_pixels > 0 ? band_luminance_accum / static_cast<double>(band_pixels) : 0.0;
    stats.band_mean_luminance = band_mean;
    // Bright = clearly above the local backdrop (particles read as light streaks).
    // NOTE: PixelLuminance is on the 0-255 scale, so the floor uses 0-255 units.
    const double bright_floor = band_mean + 0.06;
    // Dark speck = MEANINGFULLY below the backdrop (the "dirt on the sky" failure).
    // A real margin (0-255 units) so ordinary cloud/terrain noise near the band
    // mean is NOT counted; only pixels clearly darker than the sky are "specks".
    constexpr double kDarkSpeckMargin = 14.0; // ~0.055 on the [0,1] scale
    const double dark_floor = band_mean - kDarkSpeckMargin;

    double h_grad_accum = 0.0;
    double v_grad_accum = 0.0;
    std::uint64_t grad_samples = 0;
    std::uint64_t bright_pixels = 0;
    std::uint64_t dark_pixels = 0;
    double bright_luminance_accum = 0.0;

    for (int y = band_top; y < band_bottom - 1; ++y) {
        for (int x = min_x; x < max_x - 1; ++x) {
            const std::size_t off =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const double l = PixelLuminance(pixels[off], pixels[off + 1u], pixels[off + 2u]);
            const bool bright = l >= bright_floor;
            const bool dark = l <= dark_floor;
            if (bright) {
                ++bright_pixels;
                bright_luminance_accum += l;
            }
            if (dark) {
                ++dark_pixels;
            }
            // Only accumulate gradient orientation around bright structure so the
            // measure tracks the particle streaks, not the smooth backdrop.
            if (bright) {
                const std::size_t off_x = off + 3u;         // (x+1, y)
                const std::size_t off_y = off + row_stride; // (x, y+1)
                const double lx =
                    PixelLuminance(pixels[off_x], pixels[off_x + 1u], pixels[off_x + 2u]);
                const double ly =
                    PixelLuminance(pixels[off_y], pixels[off_y + 1u], pixels[off_y + 2u]);
                h_grad_accum += std::abs(lx - l);
                v_grad_accum += std::abs(ly - l);
                ++grad_samples;
            }
        }
    }

    stats.bright_particle_pixels = bright_pixels;
    stats.dark_speck_pixels = dark_pixels;
    if (band_pixels > 0) {
        stats.bright_particle_fraction =
            static_cast<double>(bright_pixels) / static_cast<double>(band_pixels);
        stats.dark_speck_fraction =
            static_cast<double>(dark_pixels) / static_cast<double>(band_pixels);
    }
    if (bright_pixels > 0) {
        stats.bright_particle_mean_luminance =
            bright_luminance_accum / static_cast<double>(bright_pixels);
    }
    if (grad_samples > 0) {
        stats.horizontal_gradient_mean = h_grad_accum / static_cast<double>(grad_samples);
        stats.vertical_gradient_mean = v_grad_accum / static_cast<double>(grad_samples);
    }
    // Streak ANISOTROPY: the ratio of the dominant gradient axis to the weaker.
    // A round dot is ~isotropic (~1); an elongated streak is strongly anisotropic.
    {
        const double hi = std::max(stats.horizontal_gradient_mean, stats.vertical_gradient_mean);
        const double lo = std::min(stats.horizontal_gradient_mean, stats.vertical_gradient_mean);
        stats.streak_anisotropy = (lo > 1e-6) ? (hi / lo) : 0.0;
    }
    // Slant ratio: horizontal vs vertical gradient energy around bright streaks.
    // Vertical (calm) rain produces near-vertical streaks -> strong vertical
    // gradient, weak horizontal -> LOW ratio. Wind-slanted rain leans -> the
    // horizontal gradient component rises -> HIGHER ratio.
    if (stats.vertical_gradient_mean > 1e-6) {
        stats.slant_ratio = stats.horizontal_gradient_mean / stats.vertical_gradient_mean;
    }
    return stats;
}

namespace {
nlohmann::json PrecipPixelStatsToJson(const PrecipPixelStats& s) {
    return {{"width", s.width},
            {"height", s.height},
            {"precip_band_pixels", s.precip_band_pixels},
            {"bright_particle_pixels", s.bright_particle_pixels},
            {"bright_particle_fraction", s.bright_particle_fraction},
            {"horizontal_gradient_mean", s.horizontal_gradient_mean},
            {"vertical_gradient_mean", s.vertical_gradient_mean},
            {"slant_ratio", s.slant_ratio},
            {"streak_anisotropy", s.streak_anisotropy},
            {"band_mean_luminance", s.band_mean_luminance},
            {"bright_particle_mean_luminance", s.bright_particle_mean_luminance},
            {"dark_speck_pixels", s.dark_speck_pixels},
            {"dark_speck_fraction", s.dark_speck_fraction}};
}
} // namespace

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
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& windy_render_pass) {
    // Calibrated against the first calm/windy rain capture pair.
    constexpr double kMinBrightFraction = 0.0008; // precip particles visibly present
    constexpr double kMinSlantRatioGain = 1.20;   // windy slant >= 20% over calm
    // Active-storm + precipitation ParticlePass budget (documented design).
    constexpr double kParticleStormBudgetMs = 1.2;
    //  SHAPE/QUALITY gates (catch "dark speckled dots"):
    //  - the precip must read as ELONGATED streaks (anisotropic gradient), not
    //    round dots (isotropic). A round dot is ~1.0; a vertical streak is well
    //    above 1. We assert this on the CALM frame (vertical fall -> the cleanest
    //    anisotropy signal); the WINDY frame's streaks run diagonally so their h/v
    //    gradient energy is balanced (anisotropy ~1 even though they ARE streaks),
    //    so the windy STREAK proof is the slant-gain check above, not anisotropy.
    constexpr double kMinStreakAnisotropy = 1.50; // calm vertical streaks
    //  - rain must be LIGHT over the sky: the bright precip pixels' mean luminance
    //    must sit ABOVE the band backdrop by a real margin (0-255 units) -- not
    //    dark specks. (dark_speck_fraction is reported as telemetry; the band
    //    includes dark horizon terrain so it is not a clean pass gate.)
    constexpr double kMinBrightOverBandMargin = 6.0; // bright precip clearly lighter than sky

    const bool precip_present_calm = calm_render_pass.particle_draws > 0 &&
                                     calm_stats.bright_particle_fraction >= kMinBrightFraction;
    const bool precip_present_windy = windy_render_pass.particle_draws > 0 &&
                                      windy_stats.bright_particle_fraction >= kMinBrightFraction;

    const double slant_gain =
        calm_stats.slant_ratio > 1e-6 ? windy_stats.slant_ratio / calm_stats.slant_ratio : 0.0;
    const bool slants_with_wind = slant_gain >= kMinSlantRatioGain;

    // Streaks-not-dots: the bright precip structure must be ELONGATED (anisotropic
    // gradient) in the CALM frame (vertical streaks). The windy frame is covered by
    // the slant-gain check (diagonal streaks are gradient-isotropic).
    const bool streaks_not_dots = calm_stats.streak_anisotropy >= kMinStreakAnisotropy;
    // Light-not-dark: bright precip pixels sit clearly ABOVE the band backdrop in
    // BOTH frames (rain is a light streak over the sky, not a dark speck).
    const bool light_not_dark = calm_stats.bright_particle_mean_luminance >=
                                    calm_stats.band_mean_luminance + kMinBrightOverBandMargin &&
                                windy_stats.bright_particle_mean_luminance >=
                                    windy_stats.band_mean_luminance + kMinBrightOverBandMargin;

    // Active-storm precip GPU-timer budget: assert the higher of the two captures'
    // ParticlePass timer against the storm budget (informational on debug where
    // the timer may report 0.0; enforced on the gate where supported).
    const double particle_storm_gpu_ms =
        std::max(calm_render_pass.particle_gpu_ms, windy_render_pass.particle_gpu_ms);
    const bool within_storm_budget = particle_storm_gpu_ms <= kParticleStormBudgetMs;

    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    const bool passed = precip_present_calm && precip_present_windy && slants_with_wind &&
                        streaks_not_dots && light_not_dark && within_storm_budget &&
                        gl_debug.errors == 0;

    nlohmann::json artifact = {
        {"schema", "luminumbra.precipitation.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"calm_screenshot", calm_screenshot},
        {"windy_screenshot", windy_screenshot},
        {"precip",
         {{"type", precip_type},
          {"calm_wind_speed", calm_wind_speed},
          {"windy_wind_speed", windy_wind_speed}}},
        {"calm_capture", PrecipPixelStatsToJson(calm_stats)},
        {"windy_capture", PrecipPixelStatsToJson(windy_stats)},
        {"presence",
         {{"calm_passed", precip_present_calm},
          {"windy_passed", precip_present_windy},
          {"calm_bright_fraction", calm_stats.bright_particle_fraction},
          {"windy_bright_fraction", windy_stats.bright_particle_fraction}}},
        {"wind_slant",
         {{"passed", slants_with_wind},
          {"calm_slant_ratio", calm_stats.slant_ratio},
          {"windy_slant_ratio", windy_stats.slant_ratio},
          {"slant_ratio_gain", slant_gain}}},
        {"streak_shape",
         {{"passed", streaks_not_dots},
          {"calm_anisotropy", calm_stats.streak_anisotropy},
          {"windy_anisotropy", windy_stats.streak_anisotropy}}},
        {"light_streaks",
         {{"passed", light_not_dark},
          {"calm_band_mean_luminance", calm_stats.band_mean_luminance},
          {"calm_bright_mean_luminance", calm_stats.bright_particle_mean_luminance},
          {"windy_band_mean_luminance", windy_stats.band_mean_luminance},
          {"windy_bright_mean_luminance", windy_stats.bright_particle_mean_luminance},
          {"calm_dark_speck_fraction", calm_stats.dark_speck_fraction},
          {"windy_dark_speck_fraction", windy_stats.dark_speck_fraction}}},
        {"gpu_timer",
         {{"particle_pass_gpu_ms", particle_storm_gpu_ms},
          {"storm_budget_ms", kParticleStormBudgetMs},
          {"within_budget", within_storm_budget},
          {"supported", windy_render_pass.gpu_timers_supported}}},
        {"thresholds",
         {{"min_bright_fraction", kMinBrightFraction},
          {"min_slant_ratio_gain", kMinSlantRatioGain},
          {"particle_storm_budget_ms", kParticleStormBudgetMs},
          {"min_streak_anisotropy", kMinStreakAnisotropy},
          {"min_bright_over_band_margin", kMinBrightOverBandMargin}}},
        {"render_pass",
         {{"calm_particle_draws", calm_render_pass.particle_draws},
          {"calm_particles_drawn", calm_render_pass.particles_drawn},
          {"windy_particle_draws", windy_render_pass.particle_draws},
          {"windy_particles_drawn", windy_render_pass.particles_drawn}}},
        {"gl_debug",
         {{"messages", gl_debug.messages},
          {"errors", gl_debug.errors},
          {"warnings", gl_debug.warnings},
          {"notifications", gl_debug.notifications}}}};

    std::ofstream output(artifact_dir / "precipitation-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
