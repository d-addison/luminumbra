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

// Calibrated against noon captures: lit sand measures around RGB(67,67,39) -
// red and green track together while blue trails by a wide margin. Grass is
// green-led (g far above r), soil and shadows fall below the brightness
// floor, so neither aliases into this bucket.
bool IsSandLikePixel(unsigned char r, unsigned char g, unsigned char b) {
    return r >= 45 && static_cast<int>(r) + 5 >= static_cast<int>(g) &&
           static_cast<int>(g) - static_cast<int>(b) >= 12 &&
           static_cast<int>(r) - static_cast<int>(b) >= 18;
}

// The grey fallback failure renders as a flat grey: all channels within a
// narrow spread, above shadow black and below sky white.
bool IsGreyFallbackPixel(unsigned char r, unsigned char g, unsigned char b) {
    const int max_channel =
        std::max({static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)});
    const int min_channel =
        std::min({static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)});
    return (max_channel - min_channel) <= 12 && max_channel >= 30 && max_channel <= 215;
}

// Calibrated against noon captures at the composite beach+highland vantage:
// rendered grass averages RGB(16,24,11) - green leads both other channels by
// a small but consistent margin (g-r p5..p95 = 3..12, g-b p5..p95 = 7..20) and
// stays dim (g p95 = 33), so the brightness ceiling excludes sky/haze (r 155+)
// while the floor excludes the near-black void. Classification keeps grey
// fallback primacy: AnalyzeMaterialPixels tests IsGreyFallbackPixel before
// this predicate so a flat-grey fallback can never be absorbed into the grass
// bucket (measured collision on real grass: 385 of 188034 ROI pixels, 0.2%).
bool IsGrassLikePixel(unsigned char r, unsigned char g, unsigned char b) {
    return g >= 12 && static_cast<int>(g) - static_cast<int>(r) >= 2 &&
           static_cast<int>(g) - static_cast<int>(b) >= 5 && r <= 90;
}

// Stone vs grey-fallback resolution (measured, documented): stone's base
// colour (0.5, 0.5, 0.52) is itself neutral, and the steep faces where
// classify_material exposes stone (depth >= 5) stay dim at noon, so rendered
// stone measures avg RGB(26, 22, 20) with a per-pixel channel spread of
// 3..11 - inside the grey-fallback detector's <= 12 spread window. They are
// genuinely inseparable by colour alone. Resolution: stone is gated on
// presence in the rim sub-ROI (top quarter of the frame), where high
// altitude excludes sand (y < 34 band) and the only neutral warm-ordered
// (r >= g >= b, sun-tinted) pixels are the stone/soil rim bands; the grey
// fallback detector keeps exclusive ownership of the main beach/flank ROI.
bool IsStoneLikePixel(unsigned char r, unsigned char g, unsigned char b) {
    const int max_channel =
        std::max({static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)});
    const int min_channel =
        std::min({static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)});
    return r >= g && g >= b && (max_channel - min_channel) <= 12 && max_channel >= 20 &&
           max_channel <= 215;
}

// Calibrated against noon rim-band captures (seed 424242): rendered soil
// (base colour (0.3, 0.15, 0.05), depth 1-5 exposure along cliff rims)
// measures avg RGB(41, 30, 25) - strongly red-led and warm. The r-b >= 13
// floor keeps it disjoint from the grey-fallback detector (spread <= 12) and
// from the stone bucket (same spread window); g-b <= 11 excludes sand, whose
// green channel rides far above blue (measured sand g-b ~ 28). Like stone,
// soil is counted in the rim sub-ROI, where its population is ~5x the main
// ROI's (interpolation-error exposure concentrates on the rims).
bool IsSoilLikePixel(unsigned char r, unsigned char g, unsigned char b) {
    return r >= 20 && r <= 120 && static_cast<int>(r) - static_cast<int>(g) >= 7 && g >= b &&
           static_cast<int>(g) - static_cast<int>(b) <= 11 &&
           static_cast<int>(r) - static_cast<int>(b) >= 13;
}

void MaterialRoiBounds(
    int width, int height, int& min_x, int& max_x, int& min_top_y, int& max_top_y) {
    min_x = width / 6;
    max_x = width - width / 6;
    min_top_y = (height * 2) / 5;
    max_top_y = height;
}

// Rim sub-ROI: same horizontal band, top quarter of the frame. The composite
// vantage places the grass-topped cliff rims (the only natural soil/stone
// exposure) against the sky in this band.
void MaterialRimRoiBounds(
    int width, int height, int& min_x, int& max_x, int& min_top_y, int& max_top_y) {
    min_x = width / 6;
    max_x = width - width / 6;
    min_top_y = 0;
    max_top_y = height / 4;
}

MaterialPixelStats
AnalyzeMaterialPixels(const std::vector<unsigned char>& pixels, int width, int height) {
    MaterialPixelStats stats;
    stats.width = width;
    stats.height = height;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    int min_x = 0;
    int max_x = 0;
    int min_top_y = 0;
    int max_top_y = 0;
    MaterialRoiBounds(width, height, min_x, max_x, min_top_y, max_top_y);
    int rim_min_x = 0;
    int rim_max_x = 0;
    int rim_min_top_y = 0;
    int rim_max_top_y = 0;
    MaterialRimRoiBounds(width, height, rim_min_x, rim_max_x, rim_min_top_y, rim_max_top_y);
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        const bool in_main_band = y_from_top >= min_top_y && y_from_top < max_top_y;
        const bool in_rim_band = y_from_top >= rim_min_top_y && y_from_top < rim_max_top_y;
        if (!in_main_band && !in_rim_band) {
            continue;
        }
        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            if (in_main_band) {
                ++stats.roi_pixels;
                if (IsSandLikePixel(r, g, b)) {
                    ++stats.sand_pixels;
                } else if (IsWaterLikePixel(r, g, b)) {
                    ++stats.water_like_pixels;
                } else if (IsGreyFallbackPixel(r, g, b)) {
                    ++stats.grey_fallback_pixels;
                } else if (IsGrassLikePixel(r, g, b)) {
                    ++stats.grass_pixels;
                } else {
                    ++stats.other_pixels;
                }
            } else {
                ++stats.rim_roi_pixels;
                if (IsStoneLikePixel(r, g, b)) {
                    ++stats.stone_pixels;
                } else if (IsSoilLikePixel(r, g, b)) {
                    ++stats.soil_pixels;
                }
            }
        }
    }

    if (stats.roi_pixels > 0) {
        stats.sand_ratio =
            static_cast<double>(stats.sand_pixels) / static_cast<double>(stats.roi_pixels);
        stats.grass_ratio =
            static_cast<double>(stats.grass_pixels) / static_cast<double>(stats.roi_pixels);
        stats.grey_fallback_ratio =
            static_cast<double>(stats.grey_fallback_pixels) / static_cast<double>(stats.roi_pixels);
    }
    if (stats.rim_roi_pixels > 0) {
        stats.stone_ratio =
            static_cast<double>(stats.stone_pixels) / static_cast<double>(stats.rim_roi_pixels);
        stats.soil_ratio =
            static_cast<double>(stats.soil_pixels) / static_cast<double>(stats.rim_roi_pixels);
    }
    return stats;
}

// (WritePixelBufferPpm moved to rendering/PixelIo.cpp — every capture path in
// the shipping client shares it, not just the harness.)

// Heatmap legend: sand -> gold, grass -> green, grey fallback -> magenta (the
// failure being gated must be unmissable), water -> blue, stone (rim sub-ROI
// only) -> slate, soil (rim sub-ROI only) -> brown, other ROI -> dimmed
// luminance, outside ROI -> heavily dimmed luminance.
std::vector<unsigned char>
BuildMaterialHeatmap(const std::vector<unsigned char>& pixels, int width, int height) {
    std::vector<unsigned char> heatmap(pixels.size());
    int min_x = 0;
    int max_x = 0;
    int min_top_y = 0;
    int max_top_y = 0;
    MaterialRoiBounds(width, height, min_x, max_x, min_top_y, max_top_y);
    int rim_min_x = 0;
    int rim_max_x = 0;
    int rim_min_top_y = 0;
    int rim_max_top_y = 0;
    MaterialRimRoiBounds(width, height, rim_min_x, rim_max_x, rim_min_top_y, rim_max_top_y);
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        const bool row_in_roi = y_from_top >= min_top_y && y_from_top < max_top_y;
        const bool row_in_rim = y_from_top >= rim_min_top_y && y_from_top < rim_max_top_y;
        for (int x = 0; x < width; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            const unsigned char luminance =
                static_cast<unsigned char>((static_cast<int>(r) + g + b) / 3);
            const bool in_roi = row_in_roi && x >= min_x && x < max_x;
            const bool in_rim = row_in_rim && x >= rim_min_x && x < rim_max_x;

            unsigned char out_r = static_cast<unsigned char>(luminance / 4);
            unsigned char out_g = out_r;
            unsigned char out_b = out_r;
            if (in_roi) {
                if (IsSandLikePixel(r, g, b)) {
                    out_r = 240;
                    out_g = 200;
                    out_b = 40;
                } else if (IsWaterLikePixel(r, g, b)) {
                    out_r = 40;
                    out_g = 80;
                    out_b = 220;
                } else if (IsGreyFallbackPixel(r, g, b)) {
                    out_r = 255;
                    out_g = 0;
                    out_b = 255;
                } else if (IsGrassLikePixel(r, g, b)) {
                    out_r = 60;
                    out_g = 220;
                    out_b = 60;
                } else {
                    out_r = static_cast<unsigned char>(luminance / 2);
                    out_g = out_r;
                    out_b = out_r;
                }
            } else if (in_rim) {
                if (IsStoneLikePixel(r, g, b)) {
                    out_r = 150;
                    out_g = 150;
                    out_b = 170;
                } else if (IsSoilLikePixel(r, g, b)) {
                    out_r = 150;
                    out_g = 90;
                    out_b = 40;
                } else {
                    out_r = static_cast<unsigned char>(luminance / 2);
                    out_g = out_r;
                    out_b = out_r;
                }
            }
            heatmap[offset + 0u] = out_r;
            heatmap[offset + 1u] = out_g;
            heatmap[offset + 2u] = out_b;
        }
    }
    return heatmap;
}

void WriteMaterialVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& screenshot,
    const std::string& heatmap_screenshot,
    const WaterVisualCameraTarget& target,
    const MaterialPixelStats& pixel_stats,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass) {
    // Pixel-count floors scale with the capture area ( capture-native update the baseline;
    // identity at the 1280x720 tuning base). The companion ratios are already
    // resolution-independent.
    const std::uint64_t kMinSandPixels = static_cast<std::uint64_t>(
        ScalePinnedArea(2000, kCapturePinnedWidth, kCapturePinnedHeight));
    constexpr double kMinSandRatio = 0.02;
    // Grass calibration (composite beach+highland vantage, seed 424242, noon):
    // measured grass_ratio 0.50 across repeated runs; the gate takes half the
    // observed ratio as the floor.
    const std::uint64_t kMinGrassPixels = static_cast<std::uint64_t>(
        ScalePinnedArea(2000, kCapturePinnedWidth, kCapturePinnedHeight));
    constexpr double kMinGrassRatio = 0.25;
    // Stone calibration (rim sub-ROI, seed 424242, noon): measured
    // stone_ratio 0.132-0.134 across repeated runs; the gate takes half the
    // observed ratio as the floor.
    const std::uint64_t kMinStonePixels = static_cast<std::uint64_t>(
        ScalePinnedArea(5000, kCapturePinnedWidth, kCapturePinnedHeight));
    constexpr double kMinStoneRatio = 0.066;
    // Soil calibration (rim sub-ROI, seed 424242, noon): measured soil_ratio
    // 0.0107-0.0109 across repeated runs; the gate takes half the observed
    // ratio as the floor.
    const std::uint64_t kMinSoilPixels =
        static_cast<std::uint64_t>(ScalePinnedArea(800, kCapturePinnedWidth, kCapturePinnedHeight));
    constexpr double kMinSoilRatio = 0.0054;
    constexpr double kMaxGreyFallbackRatio = 0.125;
    const std::uint64_t max_grey_fallback_pixels = static_cast<std::uint64_t>(
        static_cast<double>(pixel_stats.roi_pixels) * kMaxGreyFallbackRatio);
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    const bool passed =
        target.found && render_pass.terrain_draws > 0 && render_pass.terrain_indices_drawn > 0 &&
        pixel_stats.sand_pixels >= kMinSandPixels && pixel_stats.sand_ratio >= kMinSandRatio &&
        pixel_stats.grass_pixels >= kMinGrassPixels && pixel_stats.grass_ratio >= kMinGrassRatio &&
        pixel_stats.stone_pixels >= kMinStonePixels && pixel_stats.stone_ratio >= kMinStoneRatio &&
        pixel_stats.soil_pixels >= kMinSoilPixels && pixel_stats.soil_ratio >= kMinSoilRatio &&
        pixel_stats.grey_fallback_pixels <= max_grey_fallback_pixels && gl_debug.errors == 0;

    nlohmann::json artifact = {
        {"schema", "luminumbra.material_visual_analysis.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"screenshot", screenshot},
        {"heatmap_screenshot", heatmap_screenshot},
        {"target", WaterVisualTargetToJson(target)},
        {"roi",
         {{"width", pixel_stats.width},
          {"height", pixel_stats.height},
          {"roi_pixels", pixel_stats.roi_pixels},
          {"rim_roi_pixels", pixel_stats.rim_roi_pixels},
          {"water_like_pixels", pixel_stats.water_like_pixels},
          {"other_pixels", pixel_stats.other_pixels}}},
        {"materials",
         nlohmann::json::array({{{"material_id", 4},
                                 {"name", "Sand"},
                                 {"pixels",
                                  {{"classified_pixels", pixel_stats.sand_pixels},
                                   {"classified_ratio", pixel_stats.sand_ratio},
                                   {"grey_fallback_pixels", pixel_stats.grey_fallback_pixels},
                                   {"grey_fallback_ratio", pixel_stats.grey_fallback_ratio}}},
                                 {"thresholds",
                                  {{"min_classified_pixels", kMinSandPixels},
                                   {"min_classified_ratio", kMinSandRatio},
                                   {"max_grey_fallback_pixels", max_grey_fallback_pixels},
                                   {"max_grey_fallback_ratio", kMaxGreyFallbackRatio}}}},
                                {{"material_id", 3},
                                 {"name", "Grass"},
                                 {"pixels",
                                  {{"classified_pixels", pixel_stats.grass_pixels},
                                   {"classified_ratio", pixel_stats.grass_ratio},
                                   {"grey_fallback_pixels", pixel_stats.grey_fallback_pixels},
                                   {"grey_fallback_ratio", pixel_stats.grey_fallback_ratio}}},
                                 {"thresholds",
                                  {{"min_classified_pixels", kMinGrassPixels},
                                   {"min_classified_ratio", kMinGrassRatio},
                                   {"max_grey_fallback_pixels", max_grey_fallback_pixels},
                                   {"max_grey_fallback_ratio", kMaxGreyFallbackRatio}}}},
                                {{"material_id", 1},
                                 {"name", "Stone"},
                                 // Presence-in-expected-ROI gate: legitimate dim stone shares
                                 // the grey-fallback colour shape (neutral, channel spread
                                 // <= 12), so stone is counted only inside the rim sub-ROI
                                 // (top quarter of the frame, where high altitude excludes
                                 // sand and the cliff rims are the only neutral warm-ordered
                                 // surfaces), while grey-fallback enforcement stays scoped to
                                 // the main beach/flank ROI reported below.
                                 {"roi_scope", "rim_band"},
                                 {"grey_fallback_scope", "main_roi"},
                                 {"pixels",
                                  {{"classified_pixels", pixel_stats.stone_pixels},
                                   {"classified_ratio", pixel_stats.stone_ratio},
                                   {"grey_fallback_pixels", pixel_stats.grey_fallback_pixels},
                                   {"grey_fallback_ratio", pixel_stats.grey_fallback_ratio}}},
                                 {"thresholds",
                                  {{"min_classified_pixels", kMinStonePixels},
                                   {"min_classified_ratio", kMinStoneRatio},
                                   {"max_grey_fallback_pixels", max_grey_fallback_pixels},
                                   {"max_grey_fallback_ratio", kMaxGreyFallbackRatio}}}},
                                {{"material_id", 2},
                                 {"name", "Soil"},
                                 // Soil's depth 1-5 band surfaces along the same cliff rims
                                 // as stone (5x the main-ROI pixel density), so it is counted
                                 // in the rim sub-ROI as well. Unlike stone, its warm hue
                                 // (r-b >= 13) keeps it colour-separable from the grey
                                 // fallback, whose enforcement remains scoped to the main ROI.
                                 {"roi_scope", "rim_band"},
                                 {"grey_fallback_scope", "main_roi"},
                                 {"pixels",
                                  {{"classified_pixels", pixel_stats.soil_pixels},
                                   {"classified_ratio", pixel_stats.soil_ratio},
                                   {"grey_fallback_pixels", pixel_stats.grey_fallback_pixels},
                                   {"grey_fallback_ratio", pixel_stats.grey_fallback_ratio}}},
                                 {"thresholds",
                                  {{"min_classified_pixels", kMinSoilPixels},
                                   {"min_classified_ratio", kMinSoilRatio},
                                   {"max_grey_fallback_pixels", max_grey_fallback_pixels},
                                   {"max_grey_fallback_ratio", kMaxGreyFallbackRatio}}}}})},
        {"render_pass",
         {{"terrain_draws", render_pass.terrain_draws},
          {"terrain_indices_drawn", render_pass.terrain_indices_drawn},
          {"water_draws", render_pass.water_draws}}},
        {"gl_debug",
         {{"messages", gl_debug.messages},
          {"errors", gl_debug.errors},
          {"warnings", gl_debug.warnings},
          {"notifications", gl_debug.notifications}}}};

    std::ofstream output(artifact_dir / "material-visual-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

void WriteLodGroundScreenshotIndex(const std::filesystem::path& artifact_dir,
                                   const std::vector<std::string>& screenshots) {
    nlohmann::json captures = nlohmann::json::array();
    for (const std::string& screenshot : screenshots) {
        captures.push_back({{"file", screenshot}});
    }

    nlohmann::json artifact = {{"schema", "luminumbra.lod_ground_screenshots.v1"},
                               {"timestamp_utc", TimestampUtc()},
                               {"captures", captures}};
    std::ofstream output(artifact_dir / "lod-ground-screenshots.json");
    output << std::setw(2) << artifact << '\n';
}

void WriteLodGroundVisualAnalysis(const std::filesystem::path& artifact_dir,
                                  const std::vector<LodGroundVisualCapture>& captures) {
    // Pixel-count ceilings scale with the capture area ( capture-native
    // update the baseline; identity at the 1280x720 tuning base). Critical: without scaling
    // these would false-fail at native res, where a benign frame has ~6.67x more
    // pixels. The companion ratios are resolution-independent.
    const std::uint64_t kMaxDarkVoidPixels = static_cast<std::uint64_t>(
        ScalePinnedArea(18000, kCapturePinnedWidth, kCapturePinnedHeight));
    const std::uint64_t kMaxNearBlackPixels = static_cast<std::uint64_t>(
        ScalePinnedArea(4000, kCapturePinnedWidth, kCapturePinnedHeight));
    const std::uint64_t kMaxBackgroundBluePixels = static_cast<std::uint64_t>(
        ScalePinnedArea(22000, kCapturePinnedWidth, kCapturePinnedHeight));
    constexpr double kMaxDarkVoidRatio = 0.020;
    constexpr double kMaxNearBlackRatio = 0.0065;
    constexpr double kMaxBackgroundBlueRatio = 0.025;

    bool passed = captures.size() >= 3;
    nlohmann::json captures_json = nlohmann::json::array();
    for (const LodGroundVisualCapture& capture : captures) {
        const bool enforced = capture.role == "mid" || capture.role == "end";
        const bool capture_passed =
            !enforced || (capture.pixels.dark_void_pixels <= kMaxDarkVoidPixels &&
                          capture.pixels.dark_void_ratio <= kMaxDarkVoidRatio &&
                          capture.pixels.near_black_pixels <= kMaxNearBlackPixels &&
                          capture.pixels.near_black_ratio <= kMaxNearBlackRatio &&
                          capture.pixels.background_blue_pixels <= kMaxBackgroundBluePixels &&
                          capture.pixels.background_blue_ratio <= kMaxBackgroundBlueRatio);
        if (!capture_passed) {
            passed = false;
        }
        captures_json.push_back({{"role", capture.role},
                                 {"file", capture.file},
                                 {"enforced", enforced},
                                 {"passed", capture_passed},
                                 {"pixels", LodHolePixelStatsToJson(capture.pixels)}});
    }

    nlohmann::json artifact = {{"schema", "luminumbra.lod_ground_visual_analysis.v1"},
                               {"timestamp_utc", TimestampUtc()},
                               {"passed", passed},
                               {"thresholds",
                                {{"max_dark_void_pixels", kMaxDarkVoidPixels},
                                 {"max_dark_void_ratio", kMaxDarkVoidRatio},
                                 {"max_near_black_pixels", kMaxNearBlackPixels},
                                 {"max_near_black_ratio", kMaxNearBlackRatio},
                                 {"max_background_blue_pixels", kMaxBackgroundBluePixels},
                                 {"max_background_blue_ratio", kMaxBackgroundBlueRatio}}},
                               {"captures", captures_json}};

    std::ofstream output(artifact_dir / "lod-ground-visual-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
