#pragma once

// Resolution-relative visual-gate threshold scaling ( capture-native update the baseline).
//
// Pixel-ROI gate thresholds in RuntimeScenarioHarness were tuned at a fixed base
// resolution. When the pinned capture size is raised to a native display
// resolution (owner directive 2026-06-16: capture at the 3840x1600 ultrawide),
// the gates must analyze at the new size while their thresholds keep their
// original meaning. These helpers scale a base threshold from the tuning base to
// the actual capture size.
//
// Dependency-free on purpose (only <cmath>) so it is unit-testable without the
// GL-heavy harness/render headers.

#include <cmath>

namespace Luminumbra::Client::ScenarioHarness {

// The fixed resolution every pixel-ROI gate threshold was tuned at. Decoupled
// from kCapturePinnedWidth/Height: raising the pinned capture size must NOT change
// this base, or the scaling would silently no-op and leave thresholds mis-tuned.
inline constexpr int kThresholdTuningWidth = 1280;
inline constexpr int kThresholdTuningHeight = 720;

// Scale a pixel-AREA threshold (pixel counts, cluster sizes) from the tuning base
// to an actual capture (w x h).
inline long long ScalePinnedArea(long long base, int w, int h) {
    return std::llround(static_cast<double>(base) * (static_cast<double>(w) * h) /
                        (static_cast<double>(kThresholdTuningWidth) * kThresholdTuningHeight));
}

// Scale a horizontal LINEAR threshold (ROI width, sliver width) by the width.
inline long long ScalePinnedWidth(long long base, int w) {
    return std::llround(static_cast<double>(base) * static_cast<double>(w) /
                        static_cast<double>(kThresholdTuningWidth));
}

// Scale a vertical LINEAR threshold (sliver span, guard rows) by the height.
inline long long ScalePinnedHeight(long long base, int h) {
    return std::llround(static_cast<double>(base) * static_cast<double>(h) /
                        static_cast<double>(kThresholdTuningHeight));
}

} // namespace Luminumbra::Client::ScenarioHarness
