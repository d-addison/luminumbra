#include "FrameHealth.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Luminumbra::Rendering {

namespace {

// Rec. 709 luminance of an sRGB byte triple, normalized to [0,1]. Matches
// FrameScan::LumaOf so the two diagnostics speak the same units.
inline double LumaOf(unsigned char r, unsigned char g, unsigned char b) {
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
}

inline double LumaOfF(float r, float g, float b) {
    return 0.2126 * static_cast<double>(r) + 0.7152 * static_cast<double>(g) +
           0.0722 * static_cast<double>(b);
}

} // namespace

FrameHealthReport AnalyzeFrameHealth(int width,
                                     int height,
                                     const std::vector<unsigned char>& color_rgb8,
                                     const std::vector<float>& gbuffer_position_rgb16f,
                                     const std::vector<unsigned char>& gbuffer_albedo_rgb8,
                                     const std::vector<float>& color_rgbf,
                                     const FrameHealthThresholds& thresholds) {
    FrameHealthReport report;
    if (width <= 0 || height <= 0) {
        LUMINUMBRA_CORE_ERROR("FrameHealth: invalid frame size {}x{}", width, height);
        return report;
    }
    const std::size_t pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (color_rgb8.size() < pixel_count * 3u) {
        LUMINUMBRA_CORE_ERROR("FrameHealth: color buffer too small ({} < {} bytes for {}x{})",
                              color_rgb8.size(),
                              pixel_count * 3u,
                              width,
                              height);
        return report;
    }
    report.width = width;
    report.height = height;
    report.total_pixels = pixel_count;

    const int buckets = std::max(1, thresholds.histogram_buckets);
    report.histogram.resize(static_cast<std::size_t>(buckets));
    for (int i = 0; i < buckets; ++i) {
        auto& h = report.histogram[static_cast<std::size_t>(i)];
        h.lo = static_cast<double>(i) / static_cast<double>(buckets);
        h.hi = static_cast<double>(i + 1) / static_cast<double>(buckets);
    }

    // --- 1. Whole-frame color stats (exact-byte black/white, luma histogram) ---
    double luma_sum = 0.0;
    double luma_max = 0.0;
    std::uint64_t black_px = 0;
    std::uint64_t white_px = 0;
    for (std::size_t p = 0; p < pixel_count; ++p) {
        const unsigned char r = color_rgb8[p * 3u + 0u];
        const unsigned char g = color_rgb8[p * 3u + 1u];
        const unsigned char b = color_rgb8[p * 3u + 2u];
        if (r == 0 && g == 0 && b == 0)
            ++black_px;
        if (r == 255 && g == 255 && b == 255)
            ++white_px;
        const double luma = LumaOf(r, g, b);
        luma_sum += luma;
        if (luma > luma_max)
            luma_max = luma;
        int bi = static_cast<int>(luma * static_cast<double>(buckets));
        if (bi >= buckets)
            bi = buckets - 1; // luma==1.0 lands in the last (inclusive) bucket
        if (bi < 0)
            bi = 0;
        report.histogram[static_cast<std::size_t>(bi)].pixels++;
    }
    const double inv_total = 1.0 / static_cast<double>(pixel_count);
    report.mean_luminance = luma_sum * inv_total;
    report.max_luminance = luma_max;
    report.black_fraction = static_cast<double>(black_px) * inv_total;
    report.white_fraction = static_cast<double>(white_px) * inv_total;
    for (auto& h : report.histogram) {
        h.fraction = static_cast<double>(h.pixels) * inv_total;
    }

    // --- 2. NaN/inf scan (float source only) ----------------------------------
    // An 8-bit back buffer can never carry NaN; this needs the float HDR readback.
    if (color_rgbf.size() >= pixel_count * 3u) {
        std::uint64_t bad = 0;
        const std::size_t n = pixel_count * 3u;
        for (std::size_t i = 0; i < n; ++i) {
            if (!std::isfinite(color_rgbf[i]))
                ++bad;
        }
        report.nan_inf_count = bad;
    }

    // --- 3. G-buffer coverage (not-sky fraction) ------------------------------
    // Position attachment is view-space RGB16F; cleared sky reads (0,0,0). Any
    // non-zero component => geometry rasterized at that pixel. Also accumulate the
    // mean ALBEDO luma over covered pixels (needs both readbacks aligned 1:1).
    const bool have_pos = gbuffer_position_rgb16f.size() >= pixel_count * 3u;
    const bool have_alb = gbuffer_albedo_rgb8.size() >= pixel_count * 3u;
    if (have_pos) {
        report.have_coverage = true;
        std::uint64_t covered = 0;
        double alb_luma_sum = 0.0;
        std::uint64_t alb_covered = 0;
        for (std::size_t p = 0; p < pixel_count; ++p) {
            const float px = gbuffer_position_rgb16f[p * 3u + 0u];
            const float py = gbuffer_position_rgb16f[p * 3u + 1u];
            const float pz = gbuffer_position_rgb16f[p * 3u + 2u];
            const bool is_solid = (px != 0.0f) || (py != 0.0f) || (pz != 0.0f);
            if (is_solid) {
                ++covered;
                if (have_alb) {
                    alb_luma_sum += LumaOf(gbuffer_albedo_rgb8[p * 3u + 0u],
                                           gbuffer_albedo_rgb8[p * 3u + 1u],
                                           gbuffer_albedo_rgb8[p * 3u + 2u]);
                    ++alb_covered;
                }
            }
        }
        report.gbuffer_coverage = static_cast<double>(covered) * inv_total;
        if (have_alb) {
            report.have_albedo = true;
            report.albedo_mean_luminance =
                alb_covered ? alb_luma_sum / static_cast<double>(alb_covered) : 0.0;
        }
    }

    // --- 4. Verdict -----------------------------------------------------------
    // The crux: low luminance is NOT itself a fault — a correct night is dark. We
    // only flag when the EVIDENCE points to a rendering failure rather than a dark
    // scene:
    //   * black_frame: near-zero luma AND (no coverage data says nothing drawn, OR
    //     coverage exists but even the brightest pixel is dead). Pure-black + zero
    //     coverage is the unambiguous "nothing rendered" case.
    //   * unlit: real geometry IS present (coverage high) and its ALBEDO carries
    //     real color, yet the lit frame is black -> light never reached it.
    //   * blown: a large fraction clipped to pure white.
    //   * NaN/inf anywhere is always anomalous.
    FrameHealthVerdict& v = report.verdict;
    const double mean = report.mean_luminance;
    const double maxl = report.max_luminance;
    const bool near_black = mean < thresholds.near_black_mean_luma;
    const bool dead = maxl < thresholds.dead_max_luma;

    if (report.have_coverage) {
        // We KNOW whether anything was drawn.
        const bool nothing_drawn = report.gbuffer_coverage < thresholds.min_expected_coverage;
        if (near_black && (nothing_drawn || dead)) {
            v.likely_black_frame = true;
            v.reason =
                nothing_drawn
                    ? "near-zero luminance with near-zero gbuffer coverage (nothing rendered)"
                    : "near-zero luminance and dead max luminance despite gbuffer coverage";
        }
        // Unlit: plenty of geometry, real albedo, but black result.
        if (!v.likely_black_frame && report.gbuffer_coverage >= thresholds.unlit_min_coverage &&
            report.have_albedo &&
            report.albedo_mean_luminance >= thresholds.unlit_min_albedo_luma &&
            mean < thresholds.unlit_max_result_luma) {
            v.likely_unlit = true;
            v.reason = "geometry + albedo present but lit result is near-black (lighting failed)";
        }
    } else {
        // No coverage info: fall back to color-only. Pure-black buffer (high black
        // fraction) at near-zero luma is the only thing we can confidently flag;
        // a merely-dim frame is left alone to avoid false-positives on real night.
        if (near_black && dead && report.black_fraction > 0.98) {
            v.likely_black_frame = true;
            v.reason = "near-fully-black frame with dead max luminance (no gbuffer to confirm)";
        }
    }

    if (report.white_fraction >= thresholds.blown_white_fraction) {
        v.likely_blown = true;
        if (v.reason.empty())
            v.reason = "majority of frame clipped to pure white (blown/overbright)";
    }

    if (report.nan_inf_count > 0) {
        if (v.reason.empty())
            v.reason = "NaN/inf present in color buffer";
        else
            v.reason += "; NaN/inf present in color buffer";
    }

    v.anomalous =
        v.likely_black_frame || v.likely_unlit || v.likely_blown || (report.nan_inf_count > 0);
    if (!v.anomalous && v.reason.empty())
        v.reason = "nominal";

    report.ok = true;
    return report;
}

std::string FrameHealthToJson(const FrameHealthReport& report) {
    nlohmann::json j;
    j["schema"] = "luminumbra.frame_health.v1";
    j["width"] = report.width;
    j["height"] = report.height;
    j["total_pixels"] = report.total_pixels;
    j["mean_luminance"] = report.mean_luminance;
    j["max_luminance"] = report.max_luminance;
    j["black_fraction"] = report.black_fraction;
    j["white_fraction"] = report.white_fraction;
    j["nan_inf_count"] = report.nan_inf_count;

    j["have_coverage"] = report.have_coverage;
    j["gbuffer_coverage"] = report.gbuffer_coverage;
    j["have_albedo"] = report.have_albedo;
    j["albedo_mean_luminance"] = report.albedo_mean_luminance;

    nlohmann::json hist = nlohmann::json::array();
    for (const auto& h : report.histogram) {
        nlohmann::json hj;
        hj["lo"] = h.lo;
        hj["hi"] = h.hi;
        hj["pixels"] = h.pixels;
        hj["fraction"] = h.fraction;
        hist.push_back(std::move(hj));
    }
    j["luma_histogram"] = std::move(hist);

    nlohmann::json verdict;
    verdict["likely_black_frame"] = report.verdict.likely_black_frame;
    verdict["likely_unlit"] = report.verdict.likely_unlit;
    verdict["likely_blown"] = report.verdict.likely_blown;
    verdict["anomalous"] = report.verdict.anomalous;
    verdict["reason"] = report.verdict.reason;
    j["verdict"] = std::move(verdict);

    return j.dump(2);
}

} // namespace Luminumbra::Rendering
