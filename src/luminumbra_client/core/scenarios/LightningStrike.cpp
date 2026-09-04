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

// --- Lightning strike-frame smoke (, ) ---

StrikePixelStats
AnalyzeStrikePixels(const std::vector<unsigned char>& pixels, int width, int height) {
    StrikePixelStats stats;
    stats.width = width;
    stats.height = height;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    // Frame-mean luminance is the PULSE signal (the whole frame brightens on the
    // strike frame). The bolt detector counts BRIGHT pixels that are also a local
    // high-gradient -- the thin bright channel of the bolt standing out from its
    // surroundings. The gradient is checked in BOTH axes (the bolt descends, so a
    // near-vertical segment has bright horizontal neighbours but a sharp VERTICAL
    // step at its ends/kinks; a near-horizontal branch is the reverse) so the thin
    // structure is caught regardless of its local orientation. "Bright" is relative
    // to the frame mean (the bolt sits well above the scene average) so it works
    // against either a dark or a bright storm sky after tonemapping.
    constexpr double kAbsBright = 0.74;    // near the post-tonemap bolt core ribbon
    constexpr double kGradientLuma = 0.06; // sharp local luminance step (either axis)
    double frame_accum = 0.0;
    std::uint64_t frame_pixels = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t off =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const double l =
                PixelLuminance(pixels[off], pixels[off + 1u], pixels[off + 2u]) / 255.0;
            frame_accum += l;
            ++frame_pixels;
            stats.max_luminance = std::max(stats.max_luminance, l);
        }
    }
    if (frame_pixels > 0) {
        stats.frame_mean_luminance = frame_accum / static_cast<double>(frame_pixels);
    }
    // The bolt pixel is bright in absolute terms AND well above the frame mean AND
    // a sharp local step on at least one axis.
    const double rel_bright = stats.frame_mean_luminance + 0.12;
    // Bolt-core bounding box accumulation: a real
    // bolt's bright core is THIN + mostly VERTICAL, so its bright-core pixels span
    // a tall, narrow box and fill only a small fraction of it. A fat white blob
    // fills a near-square box densely. We collect the bbox over the SAME bright-
    // thin pixels the count above tracks (the bolt body, excluding the broad pulse).
    int bbox_min_x = width;
    int bbox_min_y = height;
    int bbox_max_x = -1;
    int bbox_max_y = -1;
    std::uint64_t core_pixels = 0;
    // The sharp-step test compares a bolt pixel to a neighbour a fixed VISUAL
    // distance away. Sample that neighbour at a RESOLUTION-SCALED pixel offset so
    // an anti-aliased/bloomed bolt edge — which ramps over more pixels at higher
    // resolution — reads the same per-step gradient at any capture size. At the
    // 1280x720 tuning base both offsets are 1 px (byte-identical to the original
    // off +/- 3 bytes / +/- one row);  capture-native update the baseline.
    const int grad_dx = std::max(1, static_cast<int>(ScalePinnedWidth(1, width)));
    const int grad_dy = std::max(1, static_cast<int>(ScalePinnedHeight(1, height)));
    for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
            const std::size_t off =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const double l =
                PixelLuminance(pixels[off], pixels[off + 1u], pixels[off + 2u]) / 255.0;
            if (l < kAbsBright || l < rel_bright) {
                continue;
            }
            const std::size_t offL = static_cast<std::size_t>(y) * row_stride +
                                     static_cast<std::size_t>(std::max(0, x - grad_dx)) * 3u;
            const std::size_t offR =
                static_cast<std::size_t>(y) * row_stride +
                static_cast<std::size_t>(std::min(width - 1, x + grad_dx)) * 3u;
            const std::size_t offU =
                static_cast<std::size_t>(std::max(0, y - grad_dy)) * row_stride +
                static_cast<std::size_t>(x) * 3u;
            const std::size_t offD =
                static_cast<std::size_t>(std::min(height - 1, y + grad_dy)) * row_stride +
                static_cast<std::size_t>(x) * 3u;
            const double lL =
                PixelLuminance(pixels[offL], pixels[offL + 1u], pixels[offL + 2u]) / 255.0;
            const double lR =
                PixelLuminance(pixels[offR], pixels[offR + 1u], pixels[offR + 2u]) / 255.0;
            const double lU =
                PixelLuminance(pixels[offU], pixels[offU + 1u], pixels[offU + 2u]) / 255.0;
            const double lD =
                PixelLuminance(pixels[offD], pixels[offD + 1u], pixels[offD + 2u]) / 255.0;
            if ((l - lL) >= kGradientLuma || (l - lR) >= kGradientLuma ||
                (l - lU) >= kGradientLuma || (l - lD) >= kGradientLuma) {
                ++stats.bright_thin_pixels;
                ++core_pixels;
                bbox_min_x = std::min(bbox_min_x, x);
                bbox_min_y = std::min(bbox_min_y, y);
                bbox_max_x = std::max(bbox_max_x, x);
                bbox_max_y = std::max(bbox_max_y, y);
            }
        }
    }
    stats.bolt_core_pixels = core_pixels;
    if (bbox_max_x >= bbox_min_x && bbox_max_y >= bbox_min_y) {
        stats.bolt_bbox_width = bbox_max_x - bbox_min_x + 1;
        stats.bolt_bbox_height = bbox_max_y - bbox_min_y + 1;
        if (stats.bolt_bbox_width > 0) {
            stats.bolt_aspect_ratio = static_cast<double>(stats.bolt_bbox_height) /
                                      static_cast<double>(stats.bolt_bbox_width);
        }
        const double bbox_area = static_cast<double>(stats.bolt_bbox_width) *
                                 static_cast<double>(stats.bolt_bbox_height);
        if (bbox_area > 0.0) {
            stats.bolt_fill_fraction = static_cast<double>(core_pixels) / bbox_area;
        }
    }
    return stats;
}

void WriteStrikeVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& neighbor_screenshot,
    const std::string& strike_screenshot,
    const StrikePixelStats& neighbor_stats,
    const StrikePixelStats& strike_stats,
    int sim_strikes_scheduled,
    double lightning_pulse_gpu_ms,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass) {
    // The strike frame must (a) show a full-scene luminance PULSE -- frame-mean
    // luminance markedly higher than the neighbour pre-strike frame -- and (b)
    // contain BOLT pixels (a bright thin high-gradient structure). Thresholds
    // calibrated to the first strike capture; measured values recorded alongside.
    constexpr double kMinPulseDelta = 0.04; // >= 4% absolute frame-mean luma rise
    const std::uint64_t kMinBoltPixels =    // a visible thin bolt structure (area-scaled)
        static_cast<std::uint64_t>(ScalePinnedArea(40, kCapturePinnedWidth, kCapturePinnedHeight));
    //  BOLT-SHAPE gate (catch the "fat lumpy blob"):
    //  - the bolt's bright core must form a TALL, NARROW structure (aspect >= 2:1),
    //  - and it must be THIN -- its bright-core pixels fill only a small fraction
    //    of its bounding box (a solid white worm fills a near-square box densely),
    //  - and the pre-strike STORM scene must be dark enough that the flash reads.
    // height/width: a vertical bolt. The bbox aspect is measured in PIXELS, so it
    // scales with the capture's pixel aspect ratio; correct the threshold by
    // (H/W) / (tuningH/tuningW) so it tests the same TRUE bolt shape at any capture
    // aspect. Identity at the 1280x720 tuning base; at 3840x1600 (24:10) it relaxes
    // to ~1.48 because horizontal pixels stretch 3x vs vertical 2.22x (
    // capture-native update the baseline). fill_fraction below is a ratio — aspect-invariant.
    const double kMinBoltAspect =
        2.0 * (static_cast<double>(kCapturePinnedHeight) * kThresholdTuningWidth) /
        (static_cast<double>(kCapturePinnedWidth) * kThresholdTuningHeight);
    constexpr double kMaxBoltFillFraction = 0.34; // sparse/thin, not a filled blob
    constexpr double kMaxNeighborLuma = 0.62;     // storm sky dark enough for contrast

    const double pulse_delta =
        strike_stats.frame_mean_luminance - neighbor_stats.frame_mean_luminance;
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();

    const bool pulse_passed = pulse_delta >= kMinPulseDelta;
    const bool bolt_passed = strike_stats.bright_thin_pixels >= kMinBoltPixels;
    // Shape: a thin, mostly-vertical, connected-looking structure -- NOT a blob.
    const bool bolt_shape_passed = strike_stats.bolt_aspect_ratio >= kMinBoltAspect &&
                                   strike_stats.bolt_fill_fraction > 0.0 &&
                                   strike_stats.bolt_fill_fraction <= kMaxBoltFillFraction;
    // Contrast: the strike fires against a dark enough storm/overcast sky that the
    // pulse + bolt read (the neighbour pre-strike frame is the storm backdrop).
    const bool strike_contrast_passed = neighbor_stats.frame_mean_luminance <= kMaxNeighborLuma;
    // The strike's SIM determinism (the schedule is a replayable WORLD EVENT folded
    // into the `weather` world_hash sub-hash) is proven by the weather-bench arm of
    // the WeatherVisual gate (strikes_scheduled there is asserted > 0). Here in the
    // VISUAL arm the strike count is reported as telemetry; the visual PASS is the
    // pulse + bolt, and it does NOT depend on audio (regression review).
    const bool sim_scheduled = sim_strikes_scheduled > 0; // telemetry (not a pass gate here)
    const bool passed = render_pass.lighting_draws > 0 && pulse_passed && bolt_passed &&
                        bolt_shape_passed && strike_contrast_passed && gl_debug.errors == 0;

    nlohmann::json artifact = {
        {"schema", "luminumbra.lightning_strike_visual.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"neighbor_screenshot", neighbor_screenshot},
        {"strike_screenshot", strike_screenshot},
        {"neighbor",
         {{"frame_mean_luminance", neighbor_stats.frame_mean_luminance},
          {"bright_thin_pixels", neighbor_stats.bright_thin_pixels},
          {"max_luminance", neighbor_stats.max_luminance}}},
        {"strike",
         {{"frame_mean_luminance", strike_stats.frame_mean_luminance},
          {"bright_thin_pixels", strike_stats.bright_thin_pixels},
          {"max_luminance", strike_stats.max_luminance}}},
        {"pulse", {{"passed", pulse_passed}, {"frame_mean_luminance_delta", pulse_delta}}},
        {"bolt",
         {{"passed", bolt_passed}, {"bright_thin_pixels", strike_stats.bright_thin_pixels}}},
        {"bolt_shape",
         {{"passed", bolt_shape_passed},
          {"bolt_core_pixels", strike_stats.bolt_core_pixels},
          {"bbox_width", strike_stats.bolt_bbox_width},
          {"bbox_height", strike_stats.bolt_bbox_height},
          {"aspect_ratio", strike_stats.bolt_aspect_ratio},
          {"fill_fraction", strike_stats.bolt_fill_fraction}}},
        {"strike_contrast",
         {{"passed", strike_contrast_passed},
          {"neighbor_frame_mean_luminance", neighbor_stats.frame_mean_luminance}}},
        {"sim", {{"strikes_scheduled", sim_strikes_scheduled}, {"sim_scheduled", sim_scheduled}}},
        {"thresholds",
         {{"min_pulse_delta", kMinPulseDelta},
          {"min_bolt_pixels", kMinBoltPixels},
          {"min_bolt_aspect", kMinBoltAspect},
          {"max_bolt_fill_fraction", kMaxBoltFillFraction},
          {"max_neighbor_luma", kMaxNeighborLuma}}},
        {"render_pass",
         {{"lighting_draws", render_pass.lighting_draws},
          {"lighting_pulse_gpu_ms", lightning_pulse_gpu_ms}}},
        {"gl_debug",
         {{"messages", gl_debug.messages},
          {"errors", gl_debug.errors},
          {"warnings", gl_debug.warnings},
          {"notifications", gl_debug.notifications}}}};

    std::ofstream output(artifact_dir / "lightning-strike-visual-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
