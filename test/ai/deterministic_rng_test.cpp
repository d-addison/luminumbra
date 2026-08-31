// shared deterministic seeded PRNG coverage (sim-path mutation noise).
// Validates reproducibility, uniform bounds, Gaussian shape, and stream separation.

#include "gtest/gtest.h"

#include "luminumbra_common/core/DeterministicRng.h"

#include <cmath>

namespace {
using luminumbra::core::DeterministicRng;
} // namespace

TEST(DeterministicRng, SameSeedSameSequence) {
    DeterministicRng a(12345), b(12345);
    for (int i = 0; i < 64; ++i)
        EXPECT_EQ(a.next_u64(), b.next_u64());
}

TEST(DeterministicRng, DifferentSeedsDiverge) {
    DeterministicRng a(1), b(2);
    int same = 0;
    for (int i = 0; i < 64; ++i)
        if (a.next_u64() == b.next_u64())
            ++same;
    EXPECT_EQ(same, 0); // independent streams should not collide over 64 draws
}

TEST(DeterministicRng, UnitIsInHalfOpenZeroOne) {
    DeterministicRng r(99);
    for (int i = 0; i < 10000; ++i) {
        const float u = r.next_unit();
        EXPECT_GE(u, 0.0f);
        EXPECT_LT(u, 1.0f);
    }
}

TEST(DeterministicRng, RangeStaysWithinBounds) {
    DeterministicRng r(7);
    for (int i = 0; i < 10000; ++i) {
        const float v = r.next_range(-3.0f, 5.0f);
        EXPECT_GE(v, -3.0f);
        EXPECT_LT(v, 5.0f);
    }
}

TEST(DeterministicRng, GaussianIsZeroMeanBounded) {
    DeterministicRng r(2024);
    double sum = 0.0;
    const int n = 50000;
    for (int i = 0; i < n; ++i) {
        const float g = r.next_gaussian();
        EXPECT_GE(g, -6.0f); // Irwin-Hall(12) support is [-6, 6]
        EXPECT_LE(g, 6.0f);
        sum += g;
    }
    const double mean = sum / n;
    EXPECT_NEAR(mean, 0.0, 0.05); // ~zero mean over many samples
}

TEST(DeterministicRng, SeededComposesInputsDeterministicallyAndDistinctly) {
    // Same (a,b,c) -> same stream; different inputs -> different streams.
    auto first = [](std::uint64_t a, std::uint64_t b, std::uint64_t c) {
        return DeterministicRng::seeded(a, b, c).next_u64();
    };
    EXPECT_EQ(first(16, 100, 5), first(16, 100, 5));
    EXPECT_NE(first(16, 100, 5), first(16, 100, 6)); // generation differs
    EXPECT_NE(first(16, 100, 5), first(16, 101, 5)); // entity id differs
    EXPECT_NE(first(16, 100, 5), first(17, 100, 5)); // subsystem offset differs
}
