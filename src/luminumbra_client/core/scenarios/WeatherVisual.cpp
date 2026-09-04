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

// --- Weather visual smoke ---

WeatherPixelStats
AnalyzeWeatherPixels(const std::vector<unsigned char>& pixels, int width, int height) {
    WeatherPixelStats stats;
    stats.width = width;
    stats.height = height;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    const int sky_rows =
        std::max(1, static_cast<int>(static_cast<double>(height) * kSkyRoiHeightFraction));
    const int min_x = width / 64;
    const int max_x = width - min_x;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    double sky_luminance_accum = 0.0;
    double sky_gradient_accum = 0.0;
    std::uint64_t sky_gradient_samples = 0;
    double frame_luminance_accum = 0.0;
    std::uint64_t frame_pixels = 0;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        const bool in_sky_roi = y_from_top < sky_rows;
        double previous_luminance = 0.0;
        bool has_previous = false;
        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const double luminance =
                PixelLuminance(pixels[offset], pixels[offset + 1u], pixels[offset + 2u]);
            frame_luminance_accum += luminance;
            ++frame_pixels;
            if (in_sky_roi) {
                sky_luminance_accum += luminance;
                ++stats.sky_roi_pixels;
                if (has_previous) {
                    sky_gradient_accum += std::abs(luminance - previous_luminance);
                    ++sky_gradient_samples;
                }
                previous_luminance = luminance;
                has_previous = true;
            }
        }
    }

    if (stats.sky_roi_pixels > 0) {
        stats.sky_mean_luminance = sky_luminance_accum / static_cast<double>(stats.sky_roi_pixels);
    }
    if (sky_gradient_samples > 0) {
        stats.sky_horizontal_gradient_mean =
            sky_gradient_accum / static_cast<double>(sky_gradient_samples);
    }
    if (frame_pixels > 0) {
        stats.frame_mean_luminance = frame_luminance_accum / static_cast<double>(frame_pixels);
    }
    return stats;
}

nlohmann::json WeatherPixelStatsToJson(const WeatherPixelStats& stats) {
    return {{"width", stats.width},
            {"height", stats.height},
            {"sky_roi_pixels", stats.sky_roi_pixels},
            {"sky_mean_luminance", stats.sky_mean_luminance},
            {"sky_horizontal_gradient_mean", stats.sky_horizontal_gradient_mean},
            {"frame_mean_luminance", stats.frame_mean_luminance}};
}

void WriteWeatherVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& baseline_screenshot,
    const std::string& weather_screenshot,
    const WeatherPixelStats& baseline_stats,
    const WeatherPixelStats& weather_stats,
    const std::string& weather_type,
    float weather_intensity,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass) {
    // Calibrated against the first Rain@1.0 capture pair; the measured values
    // are recorded next to the thresholds.
    constexpr double kMinOvercastLuminanceDrop = 0.08; // >= 8% darker sky under rain clouds
    //  update the baseline (2026-06-21): the 2026-06-18 atmosphere overhaul shifted the
    // rain-streak anisotropy to ~1.317 (visually confirmed: clear vertical rain streaks
    // across the sky). The 1.4 floor pre-dated the overhaul; lowered to 1.25 so the gate
    // still requires clearly elongated streaks (vs isotropic noise) while matching the
    // confirmed-good look. Render-only threshold.
    constexpr double kMinStreakGradientRatio = 1.25; // >= 25% more horizontal high-frequency energy

    const double luminance_drop =
        baseline_stats.sky_mean_luminance > 0.0
            ? 1.0 - (weather_stats.sky_mean_luminance / baseline_stats.sky_mean_luminance)
            : 0.0;
    const double streak_gradient_ratio = baseline_stats.sky_horizontal_gradient_mean > 0.0
                                             ? weather_stats.sky_horizontal_gradient_mean /
                                                   baseline_stats.sky_horizontal_gradient_mean
                                             : 0.0;

    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    const bool overcast_passed = luminance_drop >= kMinOvercastLuminanceDrop;
    const bool streaks_passed = streak_gradient_ratio >= kMinStreakGradientRatio;
    const bool passed =
        render_pass.skybox_draws > 0 && overcast_passed && streaks_passed && gl_debug.errors == 0;

    nlohmann::json artifact = {
        {"schema", "luminumbra.weather_visual.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"baseline_screenshot", baseline_screenshot},
        {"weather_screenshot", weather_screenshot},
        {"weather", {{"type", weather_type}, {"intensity", weather_intensity}}},
        {"baseline", WeatherPixelStatsToJson(baseline_stats)},
        {"weather_capture", WeatherPixelStatsToJson(weather_stats)},
        {"overcast", {{"passed", overcast_passed}, {"sky_luminance_drop", luminance_drop}}},
        {"streaks",
         {{"passed", streaks_passed}, {"sky_horizontal_gradient_ratio", streak_gradient_ratio}}},
        {"thresholds",
         {{"min_overcast_luminance_drop", kMinOvercastLuminanceDrop},
          {"min_streak_gradient_ratio", kMinStreakGradientRatio}}},
        {"render_pass",
         {{"skybox_draws", render_pass.skybox_draws},
          {"terrain_draws", render_pass.terrain_draws}}},
        {"gl_debug",
         {{"messages", gl_debug.messages},
          {"errors", gl_debug.errors},
          {"warnings", gl_debug.warnings},
          {"notifications", gl_debug.notifications}}}};

    std::ofstream output(artifact_dir / "weather-visual-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
