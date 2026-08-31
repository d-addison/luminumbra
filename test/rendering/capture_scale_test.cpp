//  capture-native update the baseline: unit tests for the resolution-relative gate
// threshold scaling helpers (core/CaptureScale.h). These guarantee the core risk
// of the update the baseline — that converting a gate's thresholds to resolution-relative
// form is BYTE-IDENTICAL at the 1280x720 tuning base — so per-gate conversions
// cannot silently change gate behavior at the current pinned size.

#include "gtest/gtest.h"

#include "core/CaptureScale.h"

using namespace Luminumbra::Client::ScenarioHarness;

namespace {
constexpr int kTuneW = kThresholdTuningWidth;  // 1280
constexpr int kTuneH = kThresholdTuningHeight; // 720
constexpr int kNativeW = 3840;                 // owner ultrawide
constexpr int kNativeH = 1600;
} // namespace

TEST(CaptureScale, IdentityAtTuningBase) {
    // At the tuning base every helper returns the base value EXACTLY (no drift),
    // so a converted gate is byte-identical to its pre-conversion behavior.
    for (long long base : {1LL, 8LL, 12LL, 16LL, 64LL, 256LL, 2000LL, 5000LL, 18000LL, 22000LL}) {
        EXPECT_EQ(ScalePinnedArea(base, kTuneW, kTuneH), base) << "area base=" << base;
        EXPECT_EQ(ScalePinnedWidth(base, kTuneW), base) << "width base=" << base;
        EXPECT_EQ(ScalePinnedHeight(base, kTuneH), base) << "height base=" << base;
    }
}

TEST(CaptureScale, AreaScalesWithTotalPixels) {
    // 3840x1600 has 6,144,000 px vs 921,600 -> 6.6666... x.
    const double ratio =
        (static_cast<double>(kNativeW) * kNativeH) / (static_cast<double>(kTuneW) * kTuneH);
    EXPECT_NEAR(ratio, 6.6667, 0.001);
    EXPECT_EQ(ScalePinnedArea(2000, kNativeW, kNativeH), std::llround(2000.0 * ratio));   // 13333
    EXPECT_EQ(ScalePinnedArea(18000, kNativeW, kNativeH), std::llround(18000.0 * ratio)); // 120000
}

TEST(CaptureScale, LinearScalesWithSingleDimension) {
    // Width threshold scales 3840/1280 = 3x; height 1600/720 = 2.2222x.
    EXPECT_EQ(ScalePinnedWidth(16, kNativeW), 48);
    EXPECT_EQ(ScalePinnedHeight(64, kNativeH), std::llround(64.0 * 1600.0 / 720.0));   // 142
    EXPECT_EQ(ScalePinnedHeight(8, kNativeH), std::llround(8.0 * 1600.0 / 720.0));     // 18
    EXPECT_EQ(ScalePinnedHeight(256, kNativeH), std::llround(256.0 * 1600.0 / 720.0)); // 569
}

TEST(CaptureScale, MonotonicAndPositive) {
    // Sanity: scaling never collapses a positive threshold to zero or inverts it.
    EXPECT_GT(ScalePinnedArea(1, kNativeW, kNativeH), 0);
    EXPECT_GE(ScalePinnedArea(2000, kNativeW, kNativeH), ScalePinnedArea(2000, kTuneW, kTuneH));
    EXPECT_GE(ScalePinnedWidth(16, kNativeW), ScalePinnedWidth(16, kTuneW));
}
