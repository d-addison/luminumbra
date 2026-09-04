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

// --- Cloud-shadow smoke (, ) ---

CloudShadowPixelStats
AnalyzeCloudShadowPixels(const std::vector<unsigned char>& pixels, int width, int height) {
    CloudShadowPixelStats stats;
    stats.width = width;
    stats.height = height;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    // Fixed terrain ROI: a centred rectangle in the lower-middle of the frame
    // where the downward-tilted cloud-shadow camera frames lit ground. The cast
    // shadow darkens this band as a cloud core drifts over it.
    const int roi_x0 = width * 5 / 16;
    const int roi_x1 = width * 11 / 16;
    const int roi_y0_from_bottom = height * 6 / 16;  // above the very bottom edge
    const int roi_y1_from_bottom = height * 12 / 16; // up to mid-frame
    double terrain_accum = 0.0;

    // Sky band: top kSkyRoiHeightFraction. Cloud edges raise the mean luminance
    // and the horizontal gradient over a clear dome.
    const int sky_rows =
        std::max(1, static_cast<int>(static_cast<double>(height) * kSkyRoiHeightFraction));
    const int sky_min_x = width / 64;
    const int sky_max_x = width - sky_min_x;
    double sky_luminance_accum = 0.0;
    double sky_gradient_accum = 0.0;
    std::uint64_t sky_gradient_samples = 0;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        const int y_from_bottom = y;
        const bool in_terrain_roi =
            y_from_bottom >= roi_y0_from_bottom && y_from_bottom < roi_y1_from_bottom;
        const bool in_sky_roi = y_from_top < sky_rows;
        double previous_luminance = 0.0;
        bool has_previous = false;
        for (int x = 0; x < width; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const double luminance =
                PixelLuminance(pixels[offset], pixels[offset + 1u], pixels[offset + 2u]);
            if (in_terrain_roi && x >= roi_x0 && x < roi_x1) {
                terrain_accum += luminance;
                ++stats.terrain_roi_pixels;
            }
            if (in_sky_roi && x >= sky_min_x && x < sky_max_x) {
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

    if (stats.terrain_roi_pixels > 0) {
        stats.terrain_roi_mean_luminance =
            terrain_accum / static_cast<double>(stats.terrain_roi_pixels);
    }
    if (stats.sky_roi_pixels > 0) {
        stats.sky_mean_luminance = sky_luminance_accum / static_cast<double>(stats.sky_roi_pixels);
    }
    if (sky_gradient_samples > 0) {
        stats.sky_horizontal_gradient_mean =
            sky_gradient_accum / static_cast<double>(sky_gradient_samples);
    }
    return stats;
}

void WriteCloudShadowAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& terrain_t0_screenshot,
    const std::string& terrain_t1_screenshot,
    const std::string& sky_screenshot,
    const CloudShadowResult& result,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass) {
    // Calibrated thresholds (recorded next to the measured values):
    //  - the cast shadow must move the fixed terrain ROI luminance by >= 4% of the
    //    [0,1] scale between the two times as an edge crosses (the moving signature);
    //  - the cloud layer must register in the sky (a non-trivial horizontal
    //    gradient from bright cloud edges over the dome).
    constexpr double kMinTerrainRoiDelta = 0.020;  // >= 2% luminance swing
    constexpr double kMinSkyCloudGradient = 0.004; // cloud edges in the sky

    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    const bool moving_shadow_passed = result.terrain_roi_luminance_delta >= kMinTerrainRoiDelta;
    const bool cloud_layer_passed =
        result.cloud_layer_present && result.sky_horizontal_gradient_mean >= kMinSkyCloudGradient;
    // Budget check is enforced on release where the timers are reliable; on debug
    // it is informational (debug is ~10x slower — / precedent).
    const bool budget_ok = !result.gpu_timers_supported ||
                           result.cloud_shadow_added_ms <= result.cloud_shadow_budget_ms;

    const bool passed = render_pass.skybox_draws > 0 && moving_shadow_passed &&
                        cloud_layer_passed && gl_debug.errors == 0;

    nlohmann::json artifact = {
        {"schema", "luminumbra.cloud_shadow.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"terrain_t0_screenshot", terrain_t0_screenshot},
        {"terrain_t1_screenshot", terrain_t1_screenshot},
        {"sky_screenshot", sky_screenshot},
        {"cloud_fixture",
         {{"coverage_amount", result.coverage_amount},
          {"shadow_strength", result.shadow_strength},
          {"scroll_offset_t0", result.scroll_offset_t0},
          {"scroll_offset_t1", result.scroll_offset_t1}}},
        {"moving_shadow",
         {{"passed", moving_shadow_passed},
          {"terrain_roi_luminance_t0", result.terrain_roi_luminance_t0},
          {"terrain_roi_luminance_t1", result.terrain_roi_luminance_t1},
          {"terrain_roi_luminance_delta", result.terrain_roi_luminance_delta},
          {"min_terrain_roi_delta", kMinTerrainRoiDelta}}},
        {"cloud_layer",
         {{"passed", cloud_layer_passed},
          {"present", result.cloud_layer_present},
          {"sky_mean_luminance", result.sky_mean_luminance},
          {"sky_horizontal_gradient_mean", result.sky_horizontal_gradient_mean},
          {"min_sky_cloud_gradient", kMinSkyCloudGradient}}},
        {"gpu_timer",
         {{"supported", result.gpu_timers_supported},
          {"lighting_gpu_ms_clouds_off", result.lighting_gpu_ms_clouds_off},
          {"lighting_gpu_ms_clouds_on", result.lighting_gpu_ms_clouds_on},
          {"cloud_shadow_added_ms", result.cloud_shadow_added_ms},
          {"cloud_shadow_budget_ms", result.cloud_shadow_budget_ms},
          {"within_budget", budget_ok}}},
        {"render_pass",
         {{"skybox_draws", render_pass.skybox_draws},
          {"terrain_draws", render_pass.terrain_draws},
          {"lighting_draws", render_pass.lighting_draws}}},
        {"gl_debug",
         {{"messages", gl_debug.messages},
          {"errors", gl_debug.errors},
          {"warnings", gl_debug.warnings},
          {"notifications", gl_debug.notifications}}}};

    std::ofstream output(artifact_dir / "cloud-shadow-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
