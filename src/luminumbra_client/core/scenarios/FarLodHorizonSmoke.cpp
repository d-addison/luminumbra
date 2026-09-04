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

// --- farlod_horizon_smoke ---

namespace {

// FarLodHorizon thresholds (luminumbra.farlod_horizon.v1, pinned numbers in
// the deterministic runtime contract section 4).
constexpr std::size_t kFarLodHorizonMaxMissingRegions = 0;
// Raised 64 -> 128 MB for: hydraulic relief adds far-LOD geometry to the
// gameplay presets (eroded archipelago ~67 MB). Matches FarLodSystem::
// kResidentBudgetBytes (the runtime LRU cap); 128 MB is trivial for the 16 GB
// RTX 5070 Ti target (owner "up the caps, make use of this PC").
constexpr std::size_t kFarLodHorizonResidentBudgetBytes = 128ull * 1024ull * 1024ull;
constexpr double kFarLodHorizonMaxGbufferDeltaMs = 1.5;
// Horizon screenshots must show terrain to the horizon: with the far field
// resident there is no legitimate sky below the eye-level horizon away from
// open sea, so the full below-horizon sky ratio gates at this bound (looser
// than the 192 m player-view bound because the far heightfield approximates
// silhouettes at kilometer range).
constexpr double kFarLodHorizonMaxSkyRatio = 0.02;
// Boundary band: ground distances spanning the live-ring boundary at the
// smoke radii (radius 12 = 192 m).
constexpr float kFarLodBoundaryBandInnerMeters = 128.0f;
constexpr float kFarLodBoundaryBandOuterMeters = 384.0f;
constexpr double kFarLodBoundaryMaxSkyRatio = 0.02;
constexpr std::uint64_t kFarLodBoundaryMaxVoidClusters = 0;
//  re-derived far-water band floor. With the post-aerial-
// perspective far-water classifier the open-water preset's boundary band must
// register at least this water fraction at its best station (the live/far sea
// reads as water past the live ring). Measured ~0.36 (eye) / ~0.71 (elevated)
// on archipelago with the re-derived bands; 0.05 leaves generous margin while
// still hard-failing a dry/degenerate band (the pre-re-derivation 0.013 reading
// that silently passed the old > 0 check).
constexpr double kFarLodBoundaryMinWaterRatioOpenSea = 0.05;
//  sand-flat-brightness band ceiling. The sand-flat metric
// counts WHITE-CLIPPED warm sand (every channel driven to the ACES hard ceiling
// - the blown-out sun-bright dry sand the visual contract identifies). With the albedo_scale
// LUT calibration the real near-sea-level dry sand stays off that hard clip:
// measured boundary-band clipped-sand fraction <= ~0.03 across presets. The gate
// holds the fraction under 0.20 - generous headroom over the calibrated reading
// (the band ROI also grazes the bright hazy near-horizon, which is legitimately
// bright but NOT hard-clipped), while still hard-failing a regression that blows
// the band fully white (the uncalibrated sand was the original defect).
constexpr double kFarLodBoundaryMaxSandFlatRatio = 0.20;
// max vertical extent (px) of a thin near-
// vertical non-sky streak permitted in the sky band above the eye-level horizon.
// The FAR-render sky-sliver (a far-region triangle straddling the camera /
// far-plane corner, rasterized as a ~360 px thick streak crossing into the sky)
// is fixed here by the far-region geometry clip + camera-region skip
// (FarLodSystem). The detector now HARD-FAILS (validate-engine-frontier.ps1) to
// gate that class. The threshold sits at 256 px: above the residual ~150-200 px
// thin streaks from sharp LIVE mountain-peak silhouettes (proven independent of
// the far path - they reproduce with far-LOD disabled, see the WA2 + this-task
// far-OFF classification), and well below the 360 px far-render defect so a
// regression of it fails. Lowering toward 24 px requires a separate live-terrain
// peak-silhouette fix (deferred; outside the far-LOD render path).
//
// the raw above-horizon sliver mixes the genuine
// far-render streak with legitimate thin LIVE mountain/island peak silhouettes
// (which classify TALLER after the 6a16048 ambient brightening: archipelago
// 201 -> 345 px, mountains ~107 px). The gated FAR-ATTRIBUTABLE metric is the
// sliver analysis of the far-ON frame run with each far-OFF-intrusion pixel
// cancelled PER PIXEL: an ON pixel counts as a terrain intrusion only when the
// paired far-OFF render (identical camera/frame) has no intrusion pixel within
// its 3x3 neighborhood (the dilation absorbs sub-pixel rasterization jitter).
// A per-image scalar max-difference cannot do this: in one column a detached
// live-geometry streak and the legitimate far-LOD horizon silhouette grazing
// just above the estimated horizon row fuse into a single tall span that exists
// only in the far-ON frame, so the scalar diff fails to cancel. The pixel-level
// mask cancels the pixel-aligned live geometry exactly; the surviving far-LOD
// horizon silhouette is then excluded by the existing bottom-anchor check
// (first < sky_band_rows*3/4). With the far streak eliminated the metric is ~0,
// so the hard-fail budget ratchets from 256 px (raw) down to 64 px
// (far-attributable). The raw kFarLodHorizonMaxSkySliverPx is retained as
// informational telemetry only (it is no longer the gated metric).
constexpr int kFarLodHorizonMaxSkySliverPx = 256;
constexpr int kFarLodHorizonMaxFarAttributableSliverPx = 64;
// Rows immediately above the estimated horizon row excluded from the sliver
// span scan: the horizon row is a projection estimate, and legitimate far-LOD
// terrain silhouettes graze within a few px of it (far-ON only), which would
// otherwise fuse with a detached streak into one span and defeat the far-OFF
// cancellation. ~2% of the observed 360-row sky band.
constexpr int kFarLodHorizonSliverHorizonGuardPx = 8;
// -#45: a far-ON dark pixel counts as a far-ATTRIBUTABLE terrain streak only
// when the paired far-OFF pixel was substantially BRIGHTER — i.e. far-LOD drew
// solid terrain where the baseline showed sky. Far-LOD's aerial perspective
// nudges distant CLOUD pixels a few luma darker (measured 1-19, median ~4, off
// luma ~92), which crosses the hard luma<90 terrain threshold and — fused with a
// legitimate near-horizon far peak in the same column — manufactures a phantom
// ~700px span. A genuine far-render streak is dark terrain (luma ~70) over bright
// sky/cloud (>=130), a delta of 60-180; requiring a minimum sky-to-terrain delta
// kills the benign atmospheric shifts while keeping full sensitivity to a real
// streak. (Render is unchanged — this is an analysis-only robustness guard, like
// the 3x3 jitter dilation and the horizon guard band above.)
constexpr int kFarLodHorizonFarAttribMinSkyDeltaLuma = 32;

bool ProjectWorldPointToScreenRow(const Luminumbra::Rendering::Camera& camera,
                                  int width,
                                  int height,
                                  const Luminumbra::Vec3& world,
                                  int& out_row_from_top) {
    const glm::mat4 clip_matrix =
        camera.GetProjectionMatrix(std::max(1, width), std::max(1, height)) *
        camera.GetViewMatrix();
    const glm::vec4 clip = clip_matrix * glm::vec4(world, 1.0f);
    if (clip.w <= 0.0f) {
        return false;
    }
    const float ndc_y = clip.y / clip.w;
    const int y_from_bottom = static_cast<int>((ndc_y * 0.5f + 0.5f) * static_cast<float>(height));
    out_row_from_top = std::clamp(height - 1 - y_from_bottom, -height, 2 * height);
    return true;
}

} // namespace

std::vector<FarLodHorizonStation> BuildFarLodHorizonStations() {
    // Four eye-level yaw stations spanning the boundary ring in every
    // direction, plus an elevated station looking down across the boundary
    // (the band ROI is widest there).
    return {
        {"eye_yaw_000", 0.0f, 0.0f, 1.8f},
        {"eye_yaw_090", 90.0f, 0.0f, 1.8f},
        {"eye_yaw_180", 180.0f, 0.0f, 1.8f},
        {"eye_yaw_270", 270.0f, 0.0f, 1.8f},
        {"elevated", 45.0f, -20.0f, 80.0f},
    };
}

void ApplyFarLodHorizonCamera(Luminumbra::world::GameSession* game_session,
                              Luminumbra::Rendering::Camera* camera,
                              const FarLodHorizonStation& station) {
    if (!camera || !game_session || !game_session->GetWorldSystem()) {
        return;
    }
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;
    const float terrain_height =
        game_session->GetWorldSystem()->GetTerrainHeightAt(spawn.x, spawn.z);
    camera->Position =
        Luminumbra::Vec3(spawn.x, terrain_height + station.eye_height_meters, spawn.z);
    camera->Zoom = 60.0f;
    camera->Yaw = station.yaw_degrees;
    camera->Pitch = station.pitch_degrees;
    camera->updateCameraVectors();
}

bool ComputeFarLodBoundaryBandRows(Luminumbra::world::GameSession* game_session,
                                   const Luminumbra::Rendering::Camera& camera,
                                   int width,
                                   int height,
                                   float inner_distance_m,
                                   float outer_distance_m,
                                   int horizon_row_from_top,
                                   int& out_top_row_from_top,
                                   int& out_bottom_row_from_top) {
    if (!game_session || !game_session->GetWorldSystem() || width <= 0 || height <= 0) {
        return false;
    }
    auto* world_system = game_session->GetWorldSystem();

    glm::vec3 forward = camera.Front;
    forward.y = 0.0f;
    if (glm::dot(forward, forward) <= 1.0e-6f) {
        return false;
    }
    forward = glm::normalize(forward);

    // Ground points at the band distances along the forward azimuth, at the
    // sampled terrain height (the band follows the terrain, not a flat
    // ground-plane assumption).
    const auto ground_row = [&](float distance, int& out_row) -> bool {
        const glm::vec3 ground_xz = glm::vec3(camera.Position) + forward * distance;
        const float ground_height = world_system->GetTerrainHeightAt(ground_xz.x, ground_xz.z);
        return ProjectWorldPointToScreenRow(
            camera,
            width,
            height,
            Luminumbra::Vec3(ground_xz.x, ground_height, ground_xz.z),
            out_row);
    };

    int outer_row = 0; // farther ground projects higher in the frame
    int inner_row = 0;
    if (!ground_row(outer_distance_m, outer_row) || !ground_row(inner_distance_m, inner_row)) {
        return false;
    }

    // Clamp below the horizon row (terrain rising above eye level occludes
    // the boundary there; only the visible below-horizon part is gateable).
    const int top = std::max(std::min(outer_row, inner_row), horizon_row_from_top);
    const int bottom = std::min(std::max(outer_row, inner_row), height - 1);
    if (bottom - top < 2) {
        return false; // band fully occluded or degenerate
    }
    out_top_row_from_top = top;
    out_bottom_row_from_top = bottom;
    return true;
}

FarLodBoundaryBandStats AnalyzeFarLodBoundaryBand(const std::vector<unsigned char>& pixels,
                                                  int width,
                                                  int height,
                                                  int band_top_row_from_top,
                                                  int band_bottom_row_from_top) {
    FarLodBoundaryBandStats stats;
    stats.band_top_row_from_top = band_top_row_from_top;
    stats.band_bottom_row_from_top = band_bottom_row_from_top;
    if (width <= 0 || height <= 0 || band_bottom_row_from_top <= band_top_row_from_top ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }
    stats.band_resolved = true;

    const int min_x = width / 64;
    const int max_x = width - min_x;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    // Sky-leak pass over the band (same predicate as the player-view gate).
    std::vector<std::uint8_t> void_mask(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0u);
    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y; // glReadPixels rows are bottom-up
        if (y_from_top < band_top_row_from_top || y_from_top > band_bottom_row_from_top) {
            continue;
        }
        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            ++stats.band_pixels;
            if (IsBelowHorizonSkyPixel(pixels[offset], pixels[offset + 1u], pixels[offset + 2u])) {
                ++stats.band_sky_pixels;
            }
            if (pixels[offset] <= 2u && pixels[offset + 1u] <= 2u && pixels[offset + 2u] <= 2u) {
                void_mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                          static_cast<std::size_t>(x)] = 1u;
            }
        }
    }
    if (stats.band_pixels > 0) {
        stats.band_sky_ratio =
            static_cast<double>(stats.band_sky_pixels) / static_cast<double>(stats.band_pixels);
    }

    // Strict-void cluster pass (max(r,g,b) <= 2, 8-connectivity, >= 12 px)
    // restricted to the boundary band.
    const std::uint64_t kMinVoidClusterPx =
        static_cast<std::uint64_t>(ScalePinnedArea(12, kCapturePinnedWidth, kCapturePinnedHeight));
    std::vector<std::size_t> flood_stack;
    for (std::size_t seed = 0; seed < void_mask.size(); ++seed) {
        if (void_mask[seed] != 1u) {
            continue;
        }
        std::uint64_t cluster_px = 0;
        flood_stack.clear();
        flood_stack.push_back(seed);
        void_mask[seed] = 2u;
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
                    if (void_mask[neighbor] == 1u) {
                        void_mask[neighbor] = 2u;
                        flood_stack.push_back(neighbor);
                    }
                }
            }
        }
        stats.largest_void_cluster_px = std::max(stats.largest_void_cluster_px, cluster_px);
        if (cluster_px >= kMinVoidClusterPx) {
            ++stats.void_cluster_count;
        }
    }
    return stats;
}

// cancel_baseline, when non-null, is the PAIRED
// far-OFF render of the IDENTICAL camera/frame as `pixels` (the far-ON frame).
// Both buffers are pixel-aligned, so the legitimate LIVE mountain/island peak
// silhouettes and diagonal live-geometry slivers rasterize to the same pixels
// in both; an ON-frame terrain-intrusion pixel is therefore counted only when
// the baseline has NO intrusion pixel in its 3x3 neighborhood (the dilation
// absorbs sub-pixel rasterization jitter between the two renders). What
// survives is far-ATTRIBUTABLE only - a streak present solely with far-LOD on.
// Passing nullptr disables cancellation (raw far-ON measurement).
FarLodHorizonSkySliverStats
AnalyzeFarLodHorizonSkySliver(const std::vector<unsigned char>& pixels,
                              int width,
                              int height,
                              int horizon_row_from_top,
                              const std::vector<unsigned char>* cancel_baseline) {
    FarLodHorizonSkySliverStats stats;
    stats.sky_bottom_row_from_top = std::clamp(horizon_row_from_top, 0, std::max(0, height - 1));
    if (width <= 0 || height <= 0 || horizon_row_from_top <= 1 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;
    // glReadPixels rows are bottom-up; sky rows are those ABOVE (numerically
    // smaller from-top than) the horizon row. The UPPER sky here is the bright
    // blue-grey skybox, not the hazy near-horizon band IsBelowHorizonSkyPixel
    // classifies. A degenerate far-mesh sliver seen edge-on draws as a DARK,
    // narrow terrain-colored streak against that bright sky. So in the sky band
    // a "terrain intrusion" pixel is one that is markedly darker than skybox
    // brightness (a terrain/unlit fragment), detected by a luminance floor.
    // Edge columns are ignored (the frame border legitimately shows near terrain
    // rising above the horizon).
    const int min_x = width / 64;
    const int max_x = width - min_x;
    const int sky_band_rows = horizon_row_from_top; // rows [0, horizon)
    // Skybox at the pinned noon time is bright (luma > ~120); terrain/unlit
    // sliver fragments are dark (the observed defect measured ~(10,14,8)).
    const auto is_terrain_intrusion = [](unsigned char r, unsigned char g, unsigned char b) {
        const int luma =
            (static_cast<int>(r) * 30 + static_cast<int>(g) * 59 + static_cast<int>(b) * 11) / 100;
        return luma < 90;
    };

    // only honor the cancellation baseline when it
    // is the paired full-resolution far-OFF buffer; a wrong-sized or absent
    // buffer leaves the raw far-ON measurement untouched.
    const bool use_cancel_baseline =
        cancel_baseline != nullptr &&
        cancel_baseline->size() ==
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u;
    // True when the far-OFF baseline has an intrusion pixel anywhere in the 3x3
    // neighborhood (in the same bottom-up buffer coordinates, clamped to bounds)
    // of (x, y) - i.e. the same live geometry is present in the far-OFF frame, so
    // the matching far-ON pixel is not far-attributable and must be treated as
    // sky. The 3x3 dilation absorbs sub-pixel rasterization jitter.
    const auto baseline_cancels = [&](int x, int y) {
        if (!use_cancel_baseline) {
            return false;
        }
        const std::vector<unsigned char>& base = *cancel_baseline;
        for (int dy = -1; dy <= 1; ++dy) {
            const int ny = std::clamp(y + dy, 0, height - 1);
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = std::clamp(x + dx, 0, width - 1);
                const std::size_t boffset =
                    static_cast<std::size_t>(ny) * row_stride + static_cast<std::size_t>(nx) * 3u;
                if (is_terrain_intrusion(base[boffset], base[boffset + 1u], base[boffset + 2u])) {
                    return true;
                }
            }
        }
        return false;
    };

    // -#45: a far-ON dark pixel is far-ATTRIBUTABLE only when the paired
    // far-OFF pixel at (x, y) was substantially BRIGHTER — far-LOD drew solid
    // terrain where the baseline showed sky/cloud. A small ON/OFF delta is the
    // aerial-perspective shift on a distant cloud crossing the luma<90 threshold,
    // not a terrain streak. Without a baseline (raw measurement) every dark pixel
    // qualifies (conservative). See kFarLodHorizonFarAttribMinSkyDeltaLuma.
    const auto far_attributable_terrain = [&](int x, int y, int on_luma) {
        if (!use_cancel_baseline) {
            return true;
        }
        const std::vector<unsigned char>& base = *cancel_baseline;
        const std::size_t boffset =
            static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
        const int off_luma =
            (static_cast<int>(base[boffset]) * 30 + static_cast<int>(base[boffset + 1u]) * 59 +
             static_cast<int>(base[boffset + 2u]) * 11) /
            100;
        return (off_luma - on_luma) >= kFarLodHorizonFarAttribMinSkyDeltaLuma;
    };

    // Per-column vertical SPAN of terrain-intrusion pixels within the sky band
    // (highest-minus-lowest intrusion row). A sliver is diagonal and dotted
    // after rasterization, so span captures its reach better than the longest
    // contiguous run. The topmost band of rows near the very top edge is part of
    // the scan (a sliver streaks to the frame top). Columns with no intrusion
    // have span 0.
    //
    // the horizon row is a projection ESTIMATE;
    // legitimate far-LOD terrain silhouettes sit within a few px above it. A
    // single such grazing pixel must not fuse with a detached streak higher in
    // the column into one giant span (observed: an 8 px live streak + one far
    // silhouette pixel 1 px above the horizon row read as a 107 px span in the
    // far-ON phase only, defeating the far-OFF cancellation). The scan therefore
    // stops a guard band above the horizon row; a genuine far-render streak (the
    // ~360 px defect class) towers far above the guard, so sensitivity holds.
    const int sliver_scan_rows = std::max(
        0,
        sky_band_rows -
            static_cast<int>(ScalePinnedHeight(kFarLodHorizonSliverHorizonGuardPx, height)));
    std::vector<int> column_span(static_cast<std::size_t>(width), 0);
    for (int x = min_x; x < max_x; ++x) {
        int first = -1;
        int last = -1;
        for (int y_from_top = 0; y_from_top < sliver_scan_rows; ++y_from_top) {
            const int y = height - 1 - y_from_top; // to bottom-up buffer row
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const int on_luma = (static_cast<int>(pixels[offset]) * 30 +
                                 static_cast<int>(pixels[offset + 1u]) * 59 +
                                 static_cast<int>(pixels[offset + 2u]) * 11) /
                                100;
            if (on_luma < 90 && !baseline_cancels(x, y) &&
                far_attributable_terrain(x, y, on_luma)) {
                if (first < 0)
                    first = y_from_top;
                last = y_from_top;
            }
            ++stats.sky_pixels;
        }
        // Exclude intrusion that only touches the bottom rows just above the
        // horizon: that is the legitimate near-horizon terrain silhouette, not a
        // sliver streaking up. Require the intrusion to start well above the
        // horizon (first row from top must be in the upper part of the sky band).
        if (first >= 0 && first < sky_band_rows * 3 / 4) {
            column_span[static_cast<std::size_t>(x)] = last - first + 1;
        }
    }
    std::vector<int>& column_run = column_span;

    // A sliver is a narrow cluster of columns (width-bounded) whose tallest
    // non-sky run reaches well up into the sky. Slide a width window: a true
    // tall thin sliver lights up only a few adjacent columns; a real mountain
    // ridge intruding above the horizon spans a wide column range, so requiring
    // the run on BOTH flanks of the window to fall off keeps ridges out.
    // px; slivers are 1-2 px, walls < 16 (tuning base, scaled to capture width).
    constexpr int kMaxSliverWidthBase = 16;
    const int kMaxSliverWidth = static_cast<int>(ScalePinnedWidth(kMaxSliverWidthBase, width));
    for (int x = min_x; x < max_x; ++x) {
        const int run = column_run[static_cast<std::size_t>(x)];
        if (run <= stats.tallest_sliver_px) {
            continue;
        }
        // Measure the contiguous width of columns whose run is at least half of
        // this column's run, centered on x.
        int left = x;
        while (left > min_x && column_run[static_cast<std::size_t>(left - 1)] * 2 >= run) {
            --left;
        }
        int right = x;
        while (right + 1 < max_x && column_run[static_cast<std::size_t>(right + 1)] * 2 >= run) {
            ++right;
        }
        const int wwidth = right - left + 1;
        if (wwidth <= kMaxSliverWidth) {
            stats.tallest_sliver_px = run;
            stats.tallest_sliver_width_px = wwidth;
            stats.tallest_sliver_col = x;
        }
    }
    return stats;
}

// classify deep-water-tinted
// and sun-bright sand-flat pixels in the live/far boundary band ROI.
//
//  RE-DERIVATION: the original blue-dominance classifier
//   (b > r + 25 && g >= r && b > 150 && r < 205)
// was tuned PRE-aerial-perspective. 5a scattering/aerial fog re-tinted the far
// field: the far-water sheet now renders at a deep, MID-LOW brightness blue
// (the matte deep-water albedo run through the calibrated irradiance chain;
// measured boundary-band median ~rgb(52,75,66): B above R, mid-low value), and
// the near-horizon band is dominated by bright warm aerial haze
// (median ~rgb(239,234,211): R above B, value > 200). The old "b > 150" floor
// excluded the now-darker far water entirely (measured 0.013 water-pixel ratio
// at the open-water elevated station, where the sheet visibly fills the frame),
// so the gate was passing on a degenerate reading. The bands are re-derived
// here against the post-5a look:
//   far water:  B clearly above R, NOT warm-bright; mid brightness band.
//   sand-flat:  warm (R >= B), all channels bright (the sun-bright dry sand /
//               hazy near-horizon the visual contract identifies at ~234).
// The two predicates are mutually exclusive (the R-vs-B sign and the brightness
// band separate them), so a single band ROI yields independent water + sand
// fractions. Render-only analysis; no world_hash / tile-byte input.
void AnalyzeFarLodBoundaryBandWater(const std::vector<unsigned char>& pixels,
                                    int width,
                                    int height,
                                    int band_top_row_from_top,
                                    int band_bottom_row_from_top,
                                    std::uint64_t& out_water_pixels,
                                    std::uint64_t& out_band_pixels,
                                    std::uint64_t* out_sand_flat_pixels) {
    out_water_pixels = 0;
    out_band_pixels = 0;
    if (out_sand_flat_pixels) {
        *out_sand_flat_pixels = 0;
    }
    if (width <= 0 || height <= 0 || band_bottom_row_from_top <= band_top_row_from_top ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return;
    }
    const int min_x = width / 64;
    const int max_x = width - min_x;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;
    //  re-derived far-water classifier. Blue clearly above red, blue not
    // far below green (so the deep-blue sheet qualifies but warm terrain does
    // not), MID brightness (above the dark shadowed-cliff floor, below the bright
    // hazy sky), and red bounded so the warm bright haze is excluded.
    const auto is_far_water = [](int r, int g, int b) {
        return b > r + 4 && b >= g - 12 && b > 35 && b < 165 && r < 175;
    };
    //  WHITE-CLIPPED warm sand-flat. Warm (red at least as strong as
    // blue) AND every channel driven near the ACES hard ceiling - the blown-out
    // sun-bright dry sand the visual contract identifies (~234 and brighter, washing toward
    // white). The albedo_scale LUT calibration keeps real near-sea-level dry sand
    // OFF this hard clip; the assertion guards against a regression that blows the
    // boundary band fully white. The legitimately-bright-but-unclipped hazy
    // near-horizon (the band ROI grazes it post-5a) does NOT trip this floor.
    const auto is_sand_flat_bright = [](int r, int g, int b) {
        return r >= 234 && g >= 226 && b >= 210 && r >= b - 12;
    };
    for (int y_from_top = band_top_row_from_top; y_from_top <= band_bottom_row_from_top;
         ++y_from_top) {
        if (y_from_top < 0 || y_from_top >= height) {
            continue;
        }
        const int y = height - 1 - y_from_top; // bottom-up buffer row
        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const int r = pixels[offset];
            const int g = pixels[offset + 1u];
            const int b = pixels[offset + 2u];
            ++out_band_pixels;
            if (is_far_water(r, g, b)) {
                ++out_water_pixels;
            }
            if (out_sand_flat_pixels && is_sand_flat_bright(r, g, b)) {
                ++*out_sand_flat_pixels;
            }
        }
    }
}

void WriteFarLodHorizonAnalysis(const std::filesystem::path& artifact_dir,
                                const std::string& world_preset,
                                double duration_seconds,
                                const std::vector<FarLodHorizonStationCapture>& captures,
                                std::size_t expected_station_count,
                                double baseline_gbuffer_gpu_ms,
                                double far_gbuffer_gpu_ms,
                                bool gpu_timers_supported,
                                bool enforce_sky_ratio) {
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    const double gbuffer_delta_ms = far_gbuffer_gpu_ms - baseline_gbuffer_gpu_ms;

    std::size_t final_missing = 0;
    std::size_t final_resident_bytes = 0;
    std::size_t final_wanted = 0;
    std::size_t final_resident = 0;
    std::size_t final_draws = 0;
    std::size_t final_indices = 0;
    //  true once the most-converged (min-missing) station has been recorded.
    bool had_converged_capture = false;
    double max_sky_ratio = 0.0;
    double max_band_sky_ratio = 0.0;
    std::uint64_t max_band_void_clusters = 0;
    std::size_t bands_resolved = 0;
    int max_sky_sliver_px = 0;
    // gated far-attributable aggregate.
    int max_far_attributable_sliver_px = 0;
    //  aggregates.
    std::size_t total_water_sheet_draws = 0;
    std::size_t max_water_sheet_draws = 0;
    double max_boundary_band_water_ratio = 0.0;
    std::uint64_t total_boundary_band_water_pixels = 0;
    //  sand-flat-brightness band aggregates. max over ALL
    // stations is telemetry (the eye-level bands legitimately graze the bright
    // post-5a hazy near-horizon); the GATED metric is the elevated (downward)
    // station's band, which frames real near-shore ground - white-clipped sand
    // there is the calibration regression the gate watches.
    double max_boundary_band_sand_flat_ratio = 0.0;
    double elevated_boundary_band_sand_flat_ratio = 0.0;
    bool elevated_band_resolved = false;
    bool all_stations_passed = captures.size() == expected_station_count;

    nlohmann::json station_rows = nlohmann::json::array();
    for (const FarLodHorizonStationCapture& capture : captures) {
        const bool boundary_passed =
            !capture.boundary.band_resolved ||
            ((!enforce_sky_ratio || capture.boundary.band_sky_ratio < kFarLodBoundaryMaxSkyRatio) &&
             capture.boundary.void_cluster_count <= kFarLodBoundaryMaxVoidClusters);
        const bool horizon_passed = (!enforce_sky_ratio || capture.sky.below_horizon_sky_ratio <
                                                               kFarLodHorizonMaxSkyRatio) &&
                                    capture.sky.void_cluster_count == 0;
        const bool station_passed = boundary_passed && horizon_passed;
        all_stations_passed = all_stations_passed && station_passed;
        max_sky_sliver_px = std::max(max_sky_sliver_px, capture.sky_sliver.tallest_sliver_px);
        max_far_attributable_sliver_px =
            std::max(max_far_attributable_sliver_px, capture.far_attributable_sliver_px);

        if (capture.boundary.band_resolved) {
            ++bands_resolved;
            max_band_sky_ratio = std::max(max_band_sky_ratio, capture.boundary.band_sky_ratio);
            max_band_void_clusters =
                std::max(max_band_void_clusters, capture.boundary.void_cluster_count);
        }
        max_sky_ratio = std::max(max_sky_ratio, capture.sky.below_horizon_sky_ratio);
        max_water_sheet_draws = std::max(max_water_sheet_draws, capture.water_sheet_draws);
        total_water_sheet_draws += capture.water_sheet_draws;
        max_boundary_band_water_ratio =
            std::max(max_boundary_band_water_ratio, capture.boundary_band_water_ratio);
        total_boundary_band_water_pixels += capture.boundary_band_water_pixels;
        //  sand-flat band telemetry + the gated elevated
        // (downward) station reading.
        max_boundary_band_sand_flat_ratio =
            std::max(max_boundary_band_sand_flat_ratio, capture.boundary_band_sand_flat_ratio);
        if (capture.station.name == std::string("elevated") && capture.boundary.band_resolved) {
            elevated_band_resolved = true;
            elevated_boundary_band_sand_flat_ratio = capture.boundary_band_sand_flat_ratio;
        }
        //  the residency coverage measure must reflect the CONVERGED ring,
        // not whichever station happened to be captured last. All stations share the same
        // XZ eye position, so the wanted ring is identical and residency monotonically fills
        // in; one station captured mid-build (e.g. the `elevated` outlier added last) would
        // otherwise drive the gate with a transient missing count. Take the values from the
        // MOST-CONVERGED station (minimum regions_missing) so coverage + budget read a single
        // coherent, settled capture. Only consider stations whose wanted ring has populated
        // (regions_wanted > 0): an early station captured before the far system seeded its
        // ring reports wanted=0/missing=0 trivially and must NOT win the min (it would make
        // the gate read "no resident/wanted regions"). `had_converged_capture` guards the
        // all-stations-empty case (far disabled).
        if (capture.regions_wanted > 0 &&
            (!had_converged_capture || capture.regions_missing < final_missing)) {
            had_converged_capture = true;
            final_missing = capture.regions_missing;
            final_resident_bytes = capture.resident_bytes;
            final_wanted = capture.regions_wanted;
            final_resident = capture.regions_resident;
            final_draws = capture.region_draws;
            final_indices = capture.far_indices_drawn;
        }

        station_rows.push_back({
            {"name", capture.station.name},
            {"yaw_degrees", capture.station.yaw_degrees},
            {"pitch_degrees", capture.station.pitch_degrees},
            {"eye_height_meters", capture.station.eye_height_meters},
            {"file", capture.file},
            {"horizon",
             {
                 {"horizon_row_from_top", capture.sky.horizon_row_from_top},
                 {"below_horizon_pixels", capture.sky.below_horizon_pixels},
                 {"below_horizon_sky_pixels", capture.sky.below_horizon_sky_pixels},
                 {"below_horizon_sky_ratio", capture.sky.below_horizon_sky_ratio},
                 {"void_cluster_count", capture.sky.void_cluster_count},
                 {"largest_void_cluster_px", capture.sky.largest_void_cluster_px},
             }},
            {"boundary_band",
             {
                 {"resolved", capture.boundary.band_resolved},
                 {"inner_distance_m", kFarLodBoundaryBandInnerMeters},
                 {"outer_distance_m", kFarLodBoundaryBandOuterMeters},
                 {"top_row_from_top", capture.boundary.band_top_row_from_top},
                 {"bottom_row_from_top", capture.boundary.band_bottom_row_from_top},
                 {"band_pixels", capture.boundary.band_pixels},
                 {"band_sky_pixels", capture.boundary.band_sky_pixels},
                 {"band_sky_ratio", capture.boundary.band_sky_ratio},
                 {"void_cluster_count", capture.boundary.void_cluster_count},
                 {"largest_void_cluster_px", capture.boundary.largest_void_cluster_px},
             }},
            {"sky_sliver",
             {
                 {"sky_bottom_row_from_top", capture.sky_sliver.sky_bottom_row_from_top},
                 {"sky_pixels", capture.sky_sliver.sky_pixels},
                 {"tallest_sliver_px", capture.sky_sliver.tallest_sliver_px},
                 {"tallest_sliver_width_px", capture.sky_sliver.tallest_sliver_width_px},
                 {"tallest_sliver_col", capture.sky_sliver.tallest_sliver_col},
                 // far-OFF (phase A) baseline at the
                 // same station and the far-attributable diff (the gated metric).
                 {"far_off_sliver_px", capture.far_off_sliver_px},
                 {"far_attributable_sliver_px", capture.far_attributable_sliver_px},
             }},
            {"farlod",
             {
                 {"regions_wanted", capture.regions_wanted},
                 {"regions_resident", capture.regions_resident},
                 {"regions_missing", capture.regions_missing},
                 {"resident_bytes", capture.resident_bytes},
                 {"region_draws", capture.region_draws},
                 {"far_indices_drawn", capture.far_indices_drawn},
                 {"water_sheet_draws", capture.water_sheet_draws},
                 {"water_sheet_indices", capture.water_sheet_indices},
                 //  scheduler diagnostics to root-cause persistent missing regions.
                 {"builds_dispatched", capture.builds_dispatched},
                 {"builds_integrated_ok", capture.builds_integrated_ok},
                 {"builds_integrated_failed", capture.builds_integrated_failed},
                 {"builds_failed_total", capture.builds_failed_total},
                 {"builds_completed_total", capture.builds_completed_total},
                 {"evictions_this_frame", capture.evictions_this_frame},
                 {"evictions_total", capture.evictions_total},
                 {"pending_depth", capture.pending_depth},
             }},
            //  resolved eye WORLD position at capture (confirms all stations share
            // one XZ so the wanted ring is identical; only yaw/pitch/height differ).
            {"camera",
             {
                 {"world_x", capture.camera_world_x},
                 {"world_y", capture.camera_world_y},
                 {"world_z", capture.camera_world_z},
                 {"yaw_degrees", capture.station.yaw_degrees},
                 {"pitch_degrees", capture.station.pitch_degrees},
                 {"eye_height_meters", capture.station.eye_height_meters},
             }},
            {"far_water",
             {
                 {"boundary_band_water_pixels", capture.boundary_band_water_pixels},
                 {"boundary_band_water_ratio", capture.boundary_band_water_ratio},
                 //  per-station sand-flat-brightness band.
                 {"boundary_band_sand_flat_pixels", capture.boundary_band_sand_flat_pixels},
                 {"boundary_band_sand_flat_ratio", capture.boundary_band_sand_flat_ratio},
             }},
            {"passed", station_passed},
        });
    }

    //  after-settle gates evaluate the MOST-CONVERGED station's scheduler
    // state (minimum regions_missing across stations). Every station holds the same XZ eye
    // position so the wanted ring is identical and residency fills in monotonically; reading
    // the converged station avoids failing the gate on a single station captured mid-build.
    const bool coverage_passed =
        !captures.empty() && final_missing <= kFarLodHorizonMaxMissingRegions;
    const bool budget_passed =
        !captures.empty() && final_resident_bytes < kFarLodHorizonResidentBudgetBytes;
    // gpu timer support is hardware-dependent; without timers the delta gate
    // records zeros and passes (the honest comparison needs the timers).
    const bool gbuffer_passed =
        !gpu_timers_supported || gbuffer_delta_ms < kFarLodHorizonMaxGbufferDeltaMs;
    // the gated sliver metric is the far-attributable
    // diff against the per-station far-OFF baseline (<= 64 px), not the raw sliver.
    // Sliver spans are vertical pixel extents; scale the budget to the actual
    // capture height (captures are pinned to kCapturePinnedHeight by contract).
    const bool sliver_passed =
        max_far_attributable_sliver_px <=
        ScalePinnedHeight(kFarLodHorizonMaxFarAttributableSliverPx, kCapturePinnedHeight);
    //  sand-flat-brightness gate. The elevated (downward)
    // station frames real near-shore ground; with the albedo_scale calibration
    // its band must not be a white-clipped sun-bright sand sheet. When the
    // elevated band is unresolved (fully occluded) the assertion is vacuously
    // satisfied (no ground to over-brighten). Eye-level bands are NOT gated here
    // (they graze the bright post-5a hazy near-horizon); their reading is
    // recorded as telemetry only.
    const bool sand_flat_passed =
        !elevated_band_resolved ||
        elevated_boundary_band_sand_flat_ratio < kFarLodBoundaryMaxSandFlatRatio;
    const bool passed = all_stations_passed && coverage_passed && budget_passed && gbuffer_passed &&
                        sliver_passed && sand_flat_passed && gl_debug.errors == 0;

    const nlohmann::json artifact = {
        {"schema", "luminumbra.farlod_horizon.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"scenario", "farlod_horizon_smoke"},
        {"world_preset", world_preset},
        {"duration_seconds", duration_seconds},
        {"thresholds",
         {
             {"max_missing_wanted_regions", kFarLodHorizonMaxMissingRegions},
             {"resident_budget_bytes", kFarLodHorizonResidentBudgetBytes},
             {"max_gbuffer_delta_ms", kFarLodHorizonMaxGbufferDeltaMs},
             {"max_below_horizon_sky_ratio", kFarLodHorizonMaxSkyRatio},
             {"max_boundary_band_sky_ratio", kFarLodBoundaryMaxSkyRatio},
             {"max_boundary_band_void_clusters", kFarLodBoundaryMaxVoidClusters},
             {"max_sky_sliver_px",
              ScalePinnedHeight(kFarLodHorizonMaxSkySliverPx, kCapturePinnedHeight)},
             // the GATED sliver budget is now the
             // far-attributable diff (max(0, on - off)); the raw max_sky_sliver_px
             // above is informational telemetry only. Both are vertical pixel spans
             // scaled to the pinned capture height ( capture-native update the baseline).
             {"max_far_attributable_sliver_px",
              ScalePinnedHeight(kFarLodHorizonMaxFarAttributableSliverPx, kCapturePinnedHeight)},
             //  re-derived far-water band floor (open-sea)
             // + sand-flat-brightness band ceiling (elevated station).
             {"min_boundary_band_water_ratio_open_sea", kFarLodBoundaryMinWaterRatioOpenSea},
             {"max_boundary_band_sand_flat_ratio", kFarLodBoundaryMaxSandFlatRatio},
             {"boundary_band_inner_m", kFarLodBoundaryBandInnerMeters},
             {"boundary_band_outer_m", kFarLodBoundaryBandOuterMeters},
             {"f2_outer_range_m", 1536.0},
             {"sky_ratio_enforced", enforce_sky_ratio},
         }},
        {"stations", station_rows},
        {"farlod",
         {
             {"regions_wanted", final_wanted},
             {"regions_resident", final_resident},
             {"regions_missing", final_missing},
             {"farlod_resident_bytes", final_resident_bytes},
             {"far_region_draws", final_draws},
             {"far_indices_drawn", final_indices},
         }},
        {"far_water",
         {
             //  continuity telemetry. On a water-bearing
             // preset max_water_sheet_draws > 0 (the far water continues past the
             // live ring) and the boundary band shows far-water pixels.
             {"total_water_sheet_draws", total_water_sheet_draws},
             {"max_water_sheet_draws", max_water_sheet_draws},
             {"total_boundary_band_water_pixels", total_boundary_band_water_pixels},
             {"max_boundary_band_water_ratio", max_boundary_band_water_ratio},
             //  sand-flat-brightness band. max over all
             // stations is telemetry; the elevated reading is the gated metric.
             {"max_boundary_band_sand_flat_ratio", max_boundary_band_sand_flat_ratio},
             {"elevated_boundary_band_sand_flat_ratio", elevated_boundary_band_sand_flat_ratio},
             {"elevated_band_resolved", elevated_band_resolved},
             {"sand_flat_passed", sand_flat_passed},
         }},
        {"gbuffer",
         {
             // Honest in-run A/B: the committed perf baseline records frame
             // times, not per-pass GPU times, so the reference gbuffer time is
             // measured in this run's far-LOD-disabled phase A.
             {"baseline_source", "in_run_far_lod_disabled_phase"},
             {"gpu_timers_supported", gpu_timers_supported},
             {"baseline_gbuffer_gpu_ms", baseline_gbuffer_gpu_ms},
             {"far_gbuffer_gpu_ms", far_gbuffer_gpu_ms},
             {"gbuffer_delta_ms", gbuffer_delta_ms},
         }},
        {"aggregates",
         {
             {"expected_stations", expected_station_count},
             {"captured_stations", captures.size()},
             {"bands_resolved", bands_resolved},
             {"max_below_horizon_sky_ratio", max_sky_ratio},
             {"max_boundary_band_sky_ratio", max_band_sky_ratio},
             {"max_boundary_band_void_clusters", max_band_void_clusters},
             {"max_sky_sliver_px", max_sky_sliver_px},
             {"max_far_attributable_sliver_px", max_far_attributable_sliver_px},
             {"max_water_sheet_draws", max_water_sheet_draws},
             {"max_boundary_band_water_ratio", max_boundary_band_water_ratio},
             {"max_boundary_band_sand_flat_ratio", max_boundary_band_sand_flat_ratio},
             {"elevated_boundary_band_sand_flat_ratio", elevated_boundary_band_sand_flat_ratio},
         }},
        {"gl_debug",
         {
             {"messages", gl_debug.messages},
             {"errors", gl_debug.errors},
             {"warnings", gl_debug.warnings},
             {"notifications", gl_debug.notifications},
         }},
        {"passed", passed},
    };

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream output(artifact_dir / "farlod-horizon-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
