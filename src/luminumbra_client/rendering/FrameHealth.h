#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {

// ===========================================================================
// framehealth — render-only frame-health diagnostic + anomaly VERDICT.
//
// Given a CPU-side readback of the FINAL color buffer (the same back-buffer
// RGB8 readback that --frame-scan / --timelapse already perform) and an OPTIONAL
// readback of the G-buffer position attachment (RGB16F view-space), this module
// computes whole-frame health metrics and a heuristic verdict that distinguishes
// a "correctly dark night" from a real "rendered black / unlit bug".
//
// PURE ANALYZER. This issues NO GL calls and NO draws — it is a pure function of
// buffers the caller already read back. It therefore NEVER touches sim state and
// NEVER feeds world_hash. (Contrast with the new-pass pattern: this is not a
// render pass, it has no shader/VAO/FBO; it is the sibling of FrameScan.)
//
// COLOR SOURCE: the back buffer is the final tonemapped sRGB image, so byte
// luminance == "how bright did the frame render". Fully-black / fully-white
// fractions are exact-byte tests (catches a cleared/blown buffer).
//
// FLOAT SOURCE (optional): if the caller supplies a float color readback (e.g.
// the pre-tonemap HDR lighting target, GL_RGB + GL_FLOAT), NaN/inf are counted
// there — an 8-bit back buffer can never carry NaN, so a non-zero nan_inf_count
// requires the float input. With only the RGB8 input, nan_inf_count stays 0.
//
// GBUFFER COVERAGE: the position attachment is VIEW-SPACE position at
// COLOR_ATTACHMENT0 (RGB16F). The G-buffer is cleared so sky / no-geometry pixels
// read exactly (0,0,0); any pixel with a non-zero position component is "a solid
// surface was rasterized here". coverage = fraction of non-empty position pixels
// = fraction of the frame that is NOT sky. This is the load-bearing signal that
// separates "dark but populated scene" (real geometry, just unlit by night) from
// "nothing was drawn" (zero coverage => pipeline/clear bug).
// ===========================================================================

// One bucket of the coarse luminance histogram. Buckets are uniform over [0,1].
struct LumaHistogramBucket {
    double lo = 0.0;   // inclusive lower luma bound
    double hi = 0.0;   // exclusive upper luma bound (the last bucket is inclusive)
    std::uint64_t pixels = 0;
    double fraction = 0.0; // pixels / total_pixels
};

// Heuristic anomaly verdict. `anomalous` is the CI-gateable rollup: true if any
// specific failure fired. `reason` is a human-readable one-liner for the log/JSON.
struct FrameHealthVerdict {
    bool likely_black_frame = false; // near-zero luma AND ~no coverage -> nothing rendered
    bool likely_unlit = false;       // coverage present (geometry drawn) but ~no light reached it
    bool likely_blown = false;       // a large fraction of pixels are fully white / clipped
    bool anomalous = false;          // rollup: any of the above, or NaN/inf present
    std::string reason;              // why (e.g. "zero gbuffer coverage at near-zero luminance")
};

struct FrameHealthReport {
    int width = 0;
    int height = 0;
    std::uint64_t total_pixels = 0;

    double mean_luminance = 0.0;     // whole-frame mean back-buffer luma [0,1]
    double max_luminance = 0.0;      // brightest pixel luma [0,1]
    double black_fraction = 0.0;     // fraction of fully-black (r==g==b==0) pixels
    double white_fraction = 0.0;     // fraction of fully-white/blown (r==g==b==255) pixels
    std::uint64_t nan_inf_count = 0; // count of NaN/inf channels (0 unless a float source given)

    bool have_coverage = false;      // a G-buffer position readback was supplied
    double gbuffer_coverage = 0.0;   // fraction of non-empty position pixels (not-sky)

    bool have_albedo = false;        // a G-buffer albedo readback was supplied
    double albedo_mean_luminance = 0.0; // mean albedo luma over COVERED pixels [0,1]

    std::vector<LumaHistogramBucket> histogram; // coarse luma histogram (default 8 buckets)

    FrameHealthVerdict verdict;
    bool ok = false;
};

// Tunable heuristic thresholds. Defaults are deliberately conservative so a real
// (if dark) night frame does NOT trip the gate. All values are in luma/fraction
// units [0,1] unless noted. Exposed so the gate / scene can override per-context.
struct FrameHealthThresholds {
    int histogram_buckets = 8;

    // "near-zero luminance" — below this the frame is suspiciously dark.
    double near_black_mean_luma = 0.012;
    // A genuinely dark-but-real frame still has SOME highlight (moon, sky band,
    // emissive). If even the brightest pixel is under this, the frame is dead.
    double dead_max_luma = 0.02;

    // Coverage below this == "essentially nothing rasterized" (all sky).
    double min_expected_coverage = 0.01;

    // unlit: geometry IS present (coverage above this) yet the lit result is
    // near-black while the ALBEDO carries real color (so it is a LIGHTING failure,
    // not a legitimately black surface). Requires the albedo readback.
    double unlit_min_coverage = 0.20;
    double unlit_min_albedo_luma = 0.05; // surfaces have real base color...
    double unlit_max_result_luma = 0.012; // ...but the lit frame is black

    // blown: too much of the frame is fully clipped to white.
    double blown_white_fraction = 0.50;
};

// Analyze a final-color readback. `color_rgb8` MUST be width*height*3 bytes
// (row-major, bottom-up as glReadPixels delivers — orientation is irrelevant to
// the statistics). `gbuffer_position_rgb16f` is OPTIONAL: pass an empty vector to
// skip coverage (have_coverage=false); when present it MUST be width*height*3
// floats. `gbuffer_albedo_rgb8` is OPTIONAL (width*height*3 bytes) and enables the
// unlit-vs-correctly-dark discrimination. `color_rgbf` is OPTIONAL float color
// (width*height*3 floats) used ONLY for NaN/inf counting; pass empty to skip.
FrameHealthReport AnalyzeFrameHealth(int width,
                                     int height,
                                     const std::vector<unsigned char>& color_rgb8,
                                     const std::vector<float>& gbuffer_position_rgb16f = {},
                                     const std::vector<unsigned char>& gbuffer_albedo_rgb8 = {},
                                     const std::vector<float>& color_rgbf = {},
                                     const FrameHealthThresholds& thresholds = {});

// Serialize the report (metrics + verdict) to a JSON string in FrameScan's style
// (schema-tagged, libm doubles ok — this is a client diagnostic). Caller writes
// the string, or folds it into render-health-analysis.json (see integration spec).
std::string FrameHealthToJson(const FrameHealthReport& report);

} // namespace Luminumbra::Rendering
