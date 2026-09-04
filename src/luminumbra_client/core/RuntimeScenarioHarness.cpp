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

double PixelLuminance(unsigned char r, unsigned char g, unsigned char b) {
    return 0.2126 * static_cast<double>(r) + 0.7152 * static_cast<double>(g) +
           0.0722 * static_cast<double>(b);
}

bool IsBelowHorizonSkyPixel(unsigned char r, unsigned char g, unsigned char b) {
    return b >= 70 && static_cast<int>(b) >= static_cast<int>(r) + 35 &&
           static_cast<int>(g) >= static_cast<int>(r) + 18 && g <= b;
}

bool IsWaterLikePixel(unsigned char r, unsigned char g, unsigned char b) {
    const bool blue_green_dominant =
        b >= static_cast<unsigned char>(std::min(255, static_cast<int>(r) + 10)) &&
        g >= static_cast<unsigned char>(std::min(255, static_cast<int>(r) + 4));
    const bool plausible_water_luma = b >= 45 && g >= 42 && r <= 135;
    const bool plausible_dark_water_luma = b >= 24 && g >= 18 && r <= 70;
    const int gb_delta = std::abs(static_cast<int>(g) - static_cast<int>(b));
    return blue_green_dominant && (plausible_water_luma || plausible_dark_water_luma) &&
           gb_delta <= 95;
}

bool IsDarkVoidPixel(unsigned char r, unsigned char g, unsigned char b) {
    return r < 24 && g < 34 && b < 54 &&
           b >= static_cast<unsigned char>(std::min(255, static_cast<int>(r) + 8)) &&
           b >= static_cast<unsigned char>(std::min(255, static_cast<int>(g) + 4));
}

bool IsNearBlackPixel(unsigned char r, unsigned char g, unsigned char b) {
    return r < 24 && g < 24 && b < 24;
}

// Sliver-cluster predicate for LOD seam crack detection. True seam cracks are
// holes through the terrain into the unrendered void, so they capture at
// RGB <= 10 (pure black, at most slightly lifted by bloom/tonemap). The
// legitimately dark scene content nearby (shaded crevices, steep trench
// walls) measures RGB 17-28 in the seam-arrival captures, so the tight bound
// keeps the gate exact: crack pixels are counted, dark-but-lit geometry is
// not.
bool IsSeamSliverPixel(unsigned char r, unsigned char g, unsigned char b) {
    return r <= 10 && g <= 10 && b <= 10;
}

// Minimum connected-component size (in pixels) for a near-black run to count
// as a seam crack sliver instead of legitimate point shadow/noise.
const std::uint64_t kMinNearBlackClusterPx =
    static_cast<std::uint64_t>(ScalePinnedArea(12, kCapturePinnedWidth, kCapturePinnedHeight));

bool IsBackgroundBluePixel(unsigned char r, unsigned char g, unsigned char b) {
    return r >= 38 && r <= 110 && g >= 50 && g <= 130 && b >= 70 && b <= 150 &&
           b >= static_cast<unsigned char>(std::min(255, static_cast<int>(r) + 8)) &&
           std::abs(static_cast<int>(b) - static_cast<int>(g)) <= 55;
}

ScreenshotPixelStats
AnalyzeScreenshotPixels(const std::vector<unsigned char>& pixels, int width, int height) {
    ScreenshotPixelStats stats;
    stats.width = width;
    stats.height = height;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    const int min_x = width / 5;
    const int max_x = width - min_x;
    const int min_top_y = height / 4;
    const int max_top_y = (height * 9) / 10;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        if (y_from_top < min_top_y || y_from_top >= max_top_y) {
            continue;
        }

        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            ++stats.roi_pixels;
            if (IsWaterLikePixel(r, g, b)) {
                ++stats.water_like_pixels;
            }
            if (r < 16 && g < 20 && b < 28) {
                ++stats.dark_pixels;
            }
            if (r > 80 && g > 100 && b > 120 &&
                std::abs(static_cast<int>(b) - static_cast<int>(g)) < 40) {
                ++stats.bright_sky_like_pixels;
            }
        }
    }

    if (stats.roi_pixels > 0) {
        stats.water_like_ratio =
            static_cast<double>(stats.water_like_pixels) / static_cast<double>(stats.roi_pixels);
    }
    return stats;
}

WaterReflectionStats AnalyzeWaterReflection(const std::vector<unsigned char>& pixels,
                                            int width,
                                            int height,
                                            const Luminumbra::Vec3& sky_reference) {
    WaterReflectionStats stats;
    stats.sky_reference = sky_reference;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    // Upper third of the AnalyzeScreenshotPixels ROI: the shallowest view
    // angles, where the fresnel term makes the SSR/sky reflection dominate.
    const int min_x = width / 5;
    const int max_x = width - min_x;
    const int min_top_y = height / 4;
    const int max_top_y = (height * 9) / 10;
    const int upper_end_y = min_top_y + (max_top_y - min_top_y) / 3;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    double sum_r = 0.0;
    double sum_g = 0.0;
    double sum_b = 0.0;
    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        if (y_from_top < min_top_y || y_from_top >= upper_end_y) {
            continue;
        }
        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            ++stats.upper_roi_pixels;
            if (IsWaterLikePixel(r, g, b)) {
                ++stats.upper_roi_water_pixels;
                sum_r += static_cast<double>(r);
                sum_g += static_cast<double>(g);
                sum_b += static_cast<double>(b);
            }
        }
    }
    if (stats.upper_roi_water_pixels == 0) {
        return stats;
    }

    stats.mean_r = sum_r / static_cast<double>(stats.upper_roi_water_pixels);
    stats.mean_g = sum_g / static_cast<double>(stats.upper_roi_water_pixels);
    stats.mean_b = sum_b / static_cast<double>(stats.upper_roi_water_pixels);

    const double mean_len = std::sqrt(stats.mean_r * stats.mean_r + stats.mean_g * stats.mean_g +
                                      stats.mean_b * stats.mean_b);
    const double sky_len = std::sqrt(static_cast<double>(sky_reference.x) * sky_reference.x +
                                     static_cast<double>(sky_reference.y) * sky_reference.y +
                                     static_cast<double>(sky_reference.z) * sky_reference.z);
    if (mean_len > 0.0 && sky_len > 0.0) {
        stats.sky_correlation = (stats.mean_r * sky_reference.x + stats.mean_g * sky_reference.y +
                                 stats.mean_b * sky_reference.z) /
                                (mean_len * sky_len);
    }
    return stats;
}

// Foam pixels are bright and nearly achromatic: white-ish froth over any of
// the water tints. Calibrated against the procedural shoreline band: full
// foam captures at min channel >= 140; lit sand measures min ~32 and bright
// shallow water min ~96 with a wider channel spread, so neither aliases in.
bool IsFoamLikePixel(unsigned char r, unsigned char g, unsigned char b) {
    const int max_channel =
        std::max({static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)});
    const int min_channel =
        std::min({static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)});
    return min_channel >= 140 && (max_channel - min_channel) <= 60;
}

WaterRegionPatch AnalyzeWaterRegionPatch(const std::vector<unsigned char>& pixels,
                                         int width,
                                         int height,
                                         int center_x,
                                         int center_y_from_top,
                                         int radius) {
    WaterRegionPatch patch;
    patch.center_x = center_x;
    patch.center_y_from_top = center_y_from_top;
    if (width <= 0 || height <= 0 || radius <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return patch;
    }

    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;
    double sum_r = 0.0;
    double sum_g = 0.0;
    double sum_b = 0.0;
    for (int oy = -radius; oy <= radius; ++oy) {
        const int y_from_top = center_y_from_top + oy;
        if (y_from_top < 0 || y_from_top >= height) {
            continue;
        }
        const int y = height - 1 - y_from_top; // glReadPixels rows start at the bottom
        for (int ox = -radius; ox <= radius; ++ox) {
            const int x = center_x + ox;
            if (x < 0 || x >= width) {
                continue;
            }
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            ++patch.pixels;
            sum_r += static_cast<double>(r);
            sum_g += static_cast<double>(g);
            sum_b += static_cast<double>(b);
            if (IsFoamLikePixel(r, g, b)) {
                ++patch.foam_pixels;
            }
        }
    }
    if (patch.pixels == 0) {
        return patch;
    }

    patch.sampled = true;
    patch.mean_r = sum_r / static_cast<double>(patch.pixels);
    patch.mean_g = sum_g / static_cast<double>(patch.pixels);
    patch.mean_b = sum_b / static_cast<double>(patch.pixels);
    const double gb_sum = patch.mean_g + patch.mean_b;
    patch.gb_balance = gb_sum > 0.0 ? (patch.mean_g - patch.mean_b) / gb_sum : 0.0;
    patch.foam_ratio = static_cast<double>(patch.foam_pixels) / static_cast<double>(patch.pixels);
    return patch;
}

// Mean luminance (Rec.601, 0-255) of the water-like pixels inside the same
// ROI AnalyzeScreenshotPixels gates on. Read back from the back buffer so the
// caustics-animation probe samples exactly what the screenshot capture sees.
WaterCausticsSample SampleBackbufferWaterLuminance(int width, int height, double elapsed_seconds) {
    WaterCausticsSample sample;
    sample.elapsed_seconds = elapsed_seconds;
    if (width <= 0 || height <= 0) {
        return sample;
    }

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) *
                                      static_cast<std::size_t>(height) * 3u);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    const int min_x = width / 5;
    const int max_x = width - min_x;
    const int min_top_y = height / 4;
    const int max_top_y = (height * 9) / 10;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    double luminance_sum = 0.0;
    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        if (y_from_top < min_top_y || y_from_top >= max_top_y) {
            continue;
        }
        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            if (IsWaterLikePixel(r, g, b)) {
                ++sample.water_pixels;
                luminance_sum += 0.299 * static_cast<double>(r) + 0.587 * static_cast<double>(g) +
                                 0.114 * static_cast<double>(b);
            }
        }
    }
    if (sample.water_pixels > 0) {
        sample.water_mean_luminance = luminance_sum / static_cast<double>(sample.water_pixels);
    }
    return sample;
}

double SampleCausticsTextureDelta(unsigned int texture_id,
                                  std::vector<unsigned char>& previous_texels) {
    if (texture_id == 0) {
        return -1.0;
    }

    GLint previous_binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_binding);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    GLint width = 0;
    GLint height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    if (width <= 0 || height <= 0) {
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_binding));
        return -1.0;
    }

    std::vector<unsigned char> texels(static_cast<std::size_t>(width) *
                                      static_cast<std::size_t>(height) * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_binding));

    double delta = -1.0;
    if (previous_texels.size() == texels.size()) {
        double sum = 0.0;
        for (std::size_t i = 0; i < texels.size(); ++i) {
            sum += std::abs(static_cast<int>(texels[i]) - static_cast<int>(previous_texels[i]));
        }
        delta = sum / static_cast<double>(texels.size());
    }
    previous_texels = std::move(texels);
    return delta;
}

LodHolePixelStats
AnalyzeLodHolePixels(const std::vector<unsigned char>& pixels, int width, int height) {
    LodHolePixelStats stats;
    stats.width = width;
    stats.height = height;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    const int min_x = width / 64;
    const int max_x = width - min_x;
    const int min_top_y = height / 4;
    const int max_top_y = height;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        if (y_from_top < min_top_y || y_from_top >= max_top_y) {
            continue;
        }

        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            ++stats.roi_pixels;
            if (IsDarkVoidPixel(r, g, b)) {
                ++stats.dark_void_pixels;
            }
            if (IsNearBlackPixel(r, g, b)) {
                ++stats.near_black_pixels;
            }
            if (IsBackgroundBluePixel(r, g, b)) {
                ++stats.background_blue_pixels;
            }
        }
    }

    if (stats.roi_pixels > 0) {
        stats.dark_void_ratio =
            static_cast<double>(stats.dark_void_pixels) / static_cast<double>(stats.roi_pixels);
        stats.near_black_ratio =
            static_cast<double>(stats.near_black_pixels) / static_cast<double>(stats.roi_pixels);
        stats.background_blue_ratio = static_cast<double>(stats.background_blue_pixels) /
                                      static_cast<double>(stats.roi_pixels);
    }

    // Sliver-cluster pass: connected components (8-connectivity) of void
    // (RGB <= 10) pixels inside the enforced ROI. Persistent LOD seam cracks
    // show up as narrow runs of tens of connected pixels while the overall
    // near-black ratio stays below the area threshold.
    std::vector<std::uint8_t> sliver_mask(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0u);
    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        if (y_from_top < min_top_y || y_from_top >= max_top_y) {
            continue;
        }
        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            if (IsSeamSliverPixel(pixels[offset + 0u], pixels[offset + 1u], pixels[offset + 2u])) {
                sliver_mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                            static_cast<std::size_t>(x)] = 1u;
            }
        }
    }

    std::vector<std::size_t> flood_stack;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t seed = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                     static_cast<std::size_t>(x);
            if (sliver_mask[seed] != 1u) {
                continue;
            }

            std::uint64_t cluster_px = 0;
            flood_stack.clear();
            flood_stack.push_back(seed);
            sliver_mask[seed] = 2u;
            while (!flood_stack.empty()) {
                const std::size_t current = flood_stack.back();
                flood_stack.pop_back();
                ++cluster_px;
                const int cx = static_cast<int>(current % static_cast<std::size_t>(width));
                const int cy = static_cast<int>(current / static_cast<std::size_t>(width));
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = cx + dx;
                        const int ny = cy + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                            continue;
                        }
                        const std::size_t neighbor =
                            static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) +
                            static_cast<std::size_t>(nx);
                        if (sliver_mask[neighbor] == 1u) {
                            sliver_mask[neighbor] = 2u;
                            flood_stack.push_back(neighbor);
                        }
                    }
                }
            }

            stats.largest_near_black_cluster_px =
                std::max(stats.largest_near_black_cluster_px, cluster_px);
            if (cluster_px >= kMinNearBlackClusterPx) {
                ++stats.near_black_cluster_count;
            }
        }
    }

    return stats;
}

bool WriteBackbufferPpm(const std::filesystem::path& path,
                        int width,
                        int height,
                        ScreenshotPixelStats* out_stats,
                        LodHolePixelStats* out_lod_hole_stats) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        LUMINUMBRA_CORE_ERROR("Failed to create screenshot directory '{}': {}",
                              path.parent_path().string(),
                              ec.message());
        return false;
    }

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) *
                                      static_cast<std::size_t>(height) * 3u);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    if (out_stats) {
        *out_stats = AnalyzeScreenshotPixels(pixels, width, height);
    }
    if (out_lod_hole_stats) {
        *out_lod_hole_stats = AnalyzeLodHolePixels(pixels, width, height);
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        LUMINUMBRA_CORE_ERROR("Failed to write screenshot artifact: {}", path.string());
        return false;
    }

    output << "P6\n" << width << ' ' << height << "\n255\n";
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;
    for (int row = height - 1; row >= 0; --row) {
        const std::size_t offset = static_cast<std::size_t>(row) * row_stride;
        output.write(reinterpret_cast<const char*>(pixels.data() + offset),
                     static_cast<std::streamsize>(row_stride));
    }
    return true;
}

nlohmann::json LodHolePixelStatsToJson(const LodHolePixelStats& stats) {
    return {{"width", stats.width},
            {"height", stats.height},
            {"roi_pixels", stats.roi_pixels},
            {"dark_void_pixels", stats.dark_void_pixels},
            {"near_black_pixels", stats.near_black_pixels},
            {"background_blue_pixels", stats.background_blue_pixels},
            {"dark_void_ratio", stats.dark_void_ratio},
            {"near_black_ratio", stats.near_black_ratio},
            {"background_blue_ratio", stats.background_blue_ratio},
            {"near_black_cluster_count", stats.near_black_cluster_count},
            {"largest_near_black_cluster_px", stats.largest_near_black_cluster_px}};
}

nlohmann::json ScreenshotPixelStatsToJson(const ScreenshotPixelStats& stats) {
    return {{"width", stats.width},
            {"height", stats.height},
            {"roi_pixels", stats.roi_pixels},
            {"water_like_pixels", stats.water_like_pixels},
            {"dark_pixels", stats.dark_pixels},
            {"bright_sky_like_pixels", stats.bright_sky_like_pixels},
            {"water_like_ratio", stats.water_like_ratio}};
}

nlohmann::json WaterVisualTargetToJson(const WaterVisualCameraTarget& target) {
    return {{"found", target.found},
            {"focus", Vec3ToJson(target.focus)},
            {"camera_position", Vec3ToJson(target.camera_position)},
            {"reflection_camera_position", Vec3ToJson(target.reflection_camera_position)},
            {"terrain_height", target.terrain_height},
            {"camera_terrain_height", target.camera_terrain_height},
            {"supporting_water_samples", target.supporting_water_samples}};
}

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
    const WaterRegionPatch& foam_patch) {
    const std::uint64_t kMinWaterLikePixels = static_cast<std::uint64_t>(
        ScalePinnedArea(2500, kCapturePinnedWidth, kCapturePinnedHeight));
    constexpr double kMinWaterLikeRatio = 0.02;
    //  depth gradient + shoreline foam gates, calibrated against the
    // top-down noon capture:
    // - depth gradient: the shallow (0.8-1.6 m) patch measured gb_balance
    //   +0.012 (bright teal, green/blue balanced) vs the deep patch -0.264
    //   (blue-led): separation measured 0.276. This is a property gate (the
    //   pre-change linear ramp already had distinct endpoint hues at 0.276;
    //   the new curve reshapes the falloff) protecting the shallow-teal vs
    //   deep-blue contrast against regressions. Floor 0.12 keeps >2x margin.
    // - shoreline foam: the foam-band patch measured a foam-like pixel
    //   ratio of 0.084-0.108 with the procedural band; the pre-change
    //   shader (foam multiplied by the black fallback texture, i.e. never
    //   rendered) measured 0.0. Floor 0.04 sits ~2x under the weakest
    //   measured band.
    constexpr double kMinDepthGradientSeparation = 0.12;
    constexpr double kMinShorelineFoamRatio = 0.04;
    //  reflection gate, calibrated against the grazing open-water
    // reflection capture (noon): with the sky-aware SSR miss color the
    // upper-band water hue correlates with the sky reference at 0.9895; the
    // pre-change deep-tint miss color measured 0.9806. The 0.985 floor sits
    // between the two with comparable margin on both sides.
    const std::uint64_t kMinReflectionWaterPixels =
        static_cast<std::uint64_t>(ScalePinnedArea(500, kCapturePinnedWidth, kCapturePinnedHeight));
    constexpr double kMinSkyCorrelation = 0.985;
    //  caustics animation gate (recalibrated in  after the
    // water normal fix): the enforced signal is the mean absolute texel delta
    // of the generated caustics texture between per-second readbacks. A
    // static tint re-renders identical texels (delta measured exactly 0.0)
    // while the generated pattern measured a mean delta of 3.5 (0-255
    // scale); the 1.0 floor keeps ~3.5x margin. The screen-side ROI
    // luminance series is recorded as supporting evidence but not gated:
    // chunk streaming inside the capture ROI dominates its variance
    // (measured up to 135 with caustics fully disabled), so it cannot
    // honestly prove caustics animation on its own.
    constexpr std::size_t kMinCausticsSamples = 2;
    constexpr double kMinCausticsSampleSpacingSeconds = 0.75;
    constexpr double kMinCausticsTextureDelta = 1.0;

    std::size_t caustics_valid_samples = 0;
    double caustics_min = 0.0;
    double caustics_max = 0.0;
    double caustics_mean = 0.0;
    for (const WaterCausticsSample& sample : caustics_samples) {
        if (sample.water_pixels == 0) {
            continue;
        }
        if (caustics_valid_samples == 0) {
            caustics_min = sample.water_mean_luminance;
            caustics_max = sample.water_mean_luminance;
        } else {
            caustics_min = std::min(caustics_min, sample.water_mean_luminance);
            caustics_max = std::max(caustics_max, sample.water_mean_luminance);
        }
        caustics_mean += sample.water_mean_luminance;
        ++caustics_valid_samples;
    }
    if (caustics_valid_samples > 0) {
        caustics_mean /= static_cast<double>(caustics_valid_samples);
    }
    double caustics_variance = 0.0;
    for (const WaterCausticsSample& sample : caustics_samples) {
        if (sample.water_pixels == 0) {
            continue;
        }
        const double delta = sample.water_mean_luminance - caustics_mean;
        caustics_variance += delta * delta;
    }
    if (caustics_valid_samples > 0) {
        caustics_variance /= static_cast<double>(caustics_valid_samples);
    }
    double caustics_min_spacing = 0.0;
    for (std::size_t i = 1; i < caustics_samples.size(); ++i) {
        const double spacing =
            caustics_samples[i].elapsed_seconds - caustics_samples[i - 1u].elapsed_seconds;
        caustics_min_spacing = (i == 1u) ? spacing : std::min(caustics_min_spacing, spacing);
    }
    const double caustics_peak_to_peak = caustics_max - caustics_min;

    std::size_t caustics_texture_delta_count = 0;
    double caustics_texture_mean_delta = 0.0;
    for (const WaterCausticsSample& sample : caustics_samples) {
        if (sample.texture_mean_abs_delta >= 0.0) {
            caustics_texture_mean_delta += sample.texture_mean_abs_delta;
            ++caustics_texture_delta_count;
        }
    }
    if (caustics_texture_delta_count > 0) {
        caustics_texture_mean_delta /= static_cast<double>(caustics_texture_delta_count);
    }

    const bool caustics_animated = caustics_valid_samples >= kMinCausticsSamples &&
                                   caustics_min_spacing >= kMinCausticsSampleSpacingSeconds &&
                                   caustics_texture_delta_count >= 1 &&
                                   caustics_texture_mean_delta >= kMinCausticsTextureDelta;

    const bool reflection_sky_correlated =
        reflection_stats.upper_roi_water_pixels >= kMinReflectionWaterPixels &&
        reflection_stats.sky_correlation >= kMinSkyCorrelation;

    const double depth_gradient_separation = (shallow_patch.sampled && deep_patch.sampled)
                                                 ? shallow_patch.gb_balance - deep_patch.gb_balance
                                                 : 0.0;
    const bool depth_gradient_present = shallow_patch.sampled && deep_patch.sampled &&
                                        depth_gradient_separation >= kMinDepthGradientSeparation;
    const bool foam_present = foam_patch.sampled && foam_patch.foam_ratio >= kMinShorelineFoamRatio;

    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    const bool passed =
        target.found && render_pass.water_draws > 0 && render_pass.water_indices_drawn > 0 &&
        pixel_stats.water_like_pixels >= kMinWaterLikePixels &&
        pixel_stats.water_like_ratio >= kMinWaterLikeRatio && caustics_animated &&
        reflection_sky_correlated && depth_gradient_present && foam_present && gl_debug.errors == 0;

    nlohmann::json caustics_sample_json = nlohmann::json::array();
    for (const WaterCausticsSample& sample : caustics_samples) {
        caustics_sample_json.push_back({{"elapsed_seconds", sample.elapsed_seconds},
                                        {"water_mean_luminance", sample.water_mean_luminance},
                                        {"water_pixels", sample.water_pixels},
                                        {"texture_mean_abs_delta", sample.texture_mean_abs_delta}});
    }

    nlohmann::json artifact = {
        {"schema", "luminumbra.water_visual_analysis.v2"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"screenshot", screenshot},
        {"target", WaterVisualTargetToJson(target)},
        {"pixels", ScreenshotPixelStatsToJson(pixel_stats)},
        {"thresholds",
         {{"min_water_like_pixels", kMinWaterLikePixels},
          {"min_water_like_ratio", kMinWaterLikeRatio}}},
        {"render_pass",
         {{"water_draws", render_pass.water_draws},
          {"water_indices_drawn", render_pass.water_indices_drawn},
          {"terrain_draws", render_pass.terrain_draws},
          {"terrain_indices_drawn", render_pass.terrain_indices_drawn},
          {"water_gpu_ms", render_pass.water_gpu_ms}}},
        {"upload_queue",
         {{"water_upload_candidates", upload_queue.water_upload_candidates},
          {"water_uploads_deferred", upload_queue.water_uploads_deferred},
          {"terrain_upload_candidates", upload_queue.terrain_upload_candidates},
          {"terrain_uploads_deferred", upload_queue.terrain_uploads_deferred}}},
        {"depth_gradient",
         {{"shallow_point_found", target.shallow_point_found},
          {"deep_point_found", target.deep_point_found},
          {"shallow",
           {{"sampled", shallow_patch.sampled},
            {"center_x", shallow_patch.center_x},
            {"center_y_from_top", shallow_patch.center_y_from_top},
            {"pixels", shallow_patch.pixels},
            {"mean_rgb", {shallow_patch.mean_r, shallow_patch.mean_g, shallow_patch.mean_b}},
            {"gb_balance", shallow_patch.gb_balance}}},
          {"deep",
           {{"sampled", deep_patch.sampled},
            {"center_x", deep_patch.center_x},
            {"center_y_from_top", deep_patch.center_y_from_top},
            {"pixels", deep_patch.pixels},
            {"mean_rgb", {deep_patch.mean_r, deep_patch.mean_g, deep_patch.mean_b}},
            {"gb_balance", deep_patch.gb_balance}}},
          {"hue_separation", depth_gradient_separation},
          {"present", depth_gradient_present},
          {"thresholds", {{"min_hue_separation", kMinDepthGradientSeparation}}}}},
        {"foam_presence",
         {{"foam_point_found", target.foam_point_found},
          {"sampled", foam_patch.sampled},
          {"center_x", foam_patch.center_x},
          {"center_y_from_top", foam_patch.center_y_from_top},
          {"patch_pixels", foam_patch.pixels},
          {"foam_pixels", foam_patch.foam_pixels},
          {"foam_ratio", foam_patch.foam_ratio},
          {"mean_rgb", {foam_patch.mean_r, foam_patch.mean_g, foam_patch.mean_b}},
          {"present", foam_present},
          {"thresholds", {{"min_foam_ratio", kMinShorelineFoamRatio}}}}},
        {"reflection",
         {{"screenshot", reflection_screenshot},
          {"upper_roi_pixels", reflection_stats.upper_roi_pixels},
          {"upper_roi_water_pixels", reflection_stats.upper_roi_water_pixels},
          {"upper_roi_mean_rgb",
           {reflection_stats.mean_r, reflection_stats.mean_g, reflection_stats.mean_b}},
          {"sky_reference_rgb",
           {reflection_stats.sky_reference.x,
            reflection_stats.sky_reference.y,
            reflection_stats.sky_reference.z}},
          {"sky_correlation", reflection_stats.sky_correlation},
          {"sky_correlated", reflection_sky_correlated},
          {"thresholds",
           {{"min_upper_roi_water_pixels", kMinReflectionWaterPixels},
            {"min_sky_correlation", kMinSkyCorrelation}}}}},
        {"caustics_animation",
         {{"sample_count", caustics_samples.size()},
          {"valid_sample_count", caustics_valid_samples},
          {"min_sample_spacing_seconds", caustics_min_spacing},
          {"luminance_min", caustics_min},
          {"luminance_max", caustics_max},
          {"luminance_mean", caustics_mean},
          {"luminance_variance", caustics_variance},
          {"luminance_peak_to_peak", caustics_peak_to_peak},
          {"texture_delta_count", caustics_texture_delta_count},
          {"texture_mean_abs_delta", caustics_texture_mean_delta},
          {"animated", caustics_animated},
          {"thresholds",
           {{"min_samples", kMinCausticsSamples},
            {"min_sample_spacing_seconds", kMinCausticsSampleSpacingSeconds},
            {"min_texture_mean_abs_delta", kMinCausticsTextureDelta}}},
          {"samples", caustics_sample_json}}},
        {"gl_debug",
         {{"messages", gl_debug.messages},
          {"errors", gl_debug.errors},
          {"warnings", gl_debug.warnings},
          {"notifications", gl_debug.notifications}}}};

    std::ofstream output(artifact_dir / "water-visual-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
