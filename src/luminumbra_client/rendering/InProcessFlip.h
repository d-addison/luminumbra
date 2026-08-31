#pragma once
// In-process FLIP-style perceptual image comparison (;   /
// ). A GL-free, header-only C++ port of tools/flip_diff.py's `backend_luma`
// -- the SAME Rec.709 luma + central-difference gradient + per-channel colour
// blend -- operating on two in-memory RGBA8 framebuffers instead of PPM/PNG
// files.
//
// Why it exists: the offline file-based path (tools/flip_diff.py) is run-to-run
// noisy (~0.057 on the same tree), so it cannot gate a per-pass backend port --
// a real regression smaller than the noise floor is invisible. Comparing two
// framebuffers produced in ONE process / GL context is bit-exact (identical
// inputs -> score 0.0), so it gates backend parity with zero cross-run variance.
//
// Two consumers, one metric:
//   -: the DualBackendFlipInProcessGpu ctest calibrates the
//     zero-noise floor (the same pass rendered twice -> exactly 0.0) and the
//     score-vs-perturbation-magnitude discrimination curve. That curve is what
//     gives  an empirical basis for its per-pass threshold.
//   -  +: the RHI pilot's RhiPilotParityGpu reuses this
//     exact function to FLIP GL-via-Diligent / native-Vulkan framebuffers
//     against raw-GL goldens, with the thresholds recorded by.
//
// Faithfulness to backend_luma is load-bearing: the "beats the 0.057 offline
// floor" claim is only valid if the in-process number is the SAME metric. Any
// change here must mirror tools/flip_diff.py::backend_luma (and vice versa).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Luminumbra::Rendering::InProcessFlip {

struct FlipResult {
    double score = 0.0; // mean error in [0,1]; exactly 0.0 iff the two buffers are bit-identical.
    double max_error = 0.0; // peak per-pixel error in [0,1].
};

// The metric name, matching the flip_diff.py backend it ports. Recorded in the
// calibration artifact so a threshold is never compared across metrics.
inline const char* BackendName() {
    return "luma";
}

// backend_luma over two RGBA8 buffers (row-major, width*height*4 bytes each;
// alpha is ignored, matching flip_diff._to_array, which loads RGB only). The
// buffers must share dims and be non-empty. Deterministic by construction:
// identical inputs make every error term bitwise zero -> score 0.0, even through
// the sqrt in the gradient path.
inline FlipResult
ComputeLumaFlip(const std::uint8_t* a, const std::uint8_t* b, int width, int height) {
    FlipResult result;
    if (!a || !b || width <= 0 || height <= 0) {
        return result;
    }
    const std::size_t pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    // Rec.709 luma per pixel in [0,1] (uint8/255), matching flip_diff._luma.
    std::vector<float> la(pixel_count);
    std::vector<float> lb(pixel_count);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const float ar = a[i * 4u + 0u] / 255.0f;
        const float ag = a[i * 4u + 1u] / 255.0f;
        const float ab = a[i * 4u + 2u] / 255.0f;
        const float br = b[i * 4u + 0u] / 255.0f;
        const float bg = b[i * 4u + 1u] / 255.0f;
        const float bb = b[i * 4u + 2u] / 255.0f;
        la[i] = ar * 0.2126f + ag * 0.7152f + ab * 0.0722f;
        lb[i] = br * 0.2126f + bg * 0.7152f + bb * 0.0722f;
    }

    const auto sample = [width, height](const std::vector<float>& img, int x, int y) -> float {
        const int cx = std::clamp(x, 0, width - 1); // edge-clamp == numpy pad(mode="edge")
        const int cy = std::clamp(y, 0, height - 1);
        return img[static_cast<std::size_t>(cy) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(cx)];
    };
    // Central-difference gradient magnitude (right-left, down-up, edge-clamped),
    // exactly flip_diff.sobel: sqrt(gx^2 + gy^2).
    const auto sobel = [&](const std::vector<float>& img, int x, int y) -> float {
        const float gx = sample(img, x + 1, y) - sample(img, x - 1, y);
        const float gy = sample(img, x, y + 1) - sample(img, x, y - 1);
        return std::sqrt(gx * gx + gy * gy);
    };

    double sum = 0.0;
    double peak = 0.0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(x);
            const float luma_err = std::fabs(la[i] - lb[i]);
            float grad_err = std::fabs(sobel(la, x, y) - sobel(lb, x, y));
            grad_err = std::clamp(grad_err, 0.0f, 1.0f);
            const float cr = std::fabs(a[i * 4u + 0u] / 255.0f - b[i * 4u + 0u] / 255.0f);
            const float cg = std::fabs(a[i * 4u + 1u] / 255.0f - b[i * 4u + 1u] / 255.0f);
            const float cb = std::fabs(a[i * 4u + 2u] / 255.0f - b[i * 4u + 2u] / 255.0f);
            const float color_err = (cr + cg + cb) / 3.0f;
            float err = 0.5f * luma_err + 0.3f * grad_err + 0.2f * color_err;
            err = std::clamp(err, 0.0f, 1.0f);
            sum += static_cast<double>(err);
            peak = std::max(peak, static_cast<double>(err));
        }
    }
    result.score = pixel_count > 0 ? sum / static_cast<double>(pixel_count) : 0.0;
    result.max_error = peak;
    return result;
}

} // namespace Luminumbra::Rendering::InProcessFlip
