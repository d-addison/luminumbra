// deterministic wind-grid snapshot + determinism test.
//
// Proves the wind field is bit-deterministic (the property the world_hash `wind`
// sub-hash and the WindFieldDeterminism gate depend on): two independent
// instances advanced through the same ticks with the same seed/anchor produce
// the IDENTICAL wind sub-hash; a different seed produces a different hash; the
// public SampleWind API returns the in-region cell value inside the streamed
// extent and clamps to the base direction outside it; and the field geometry
// matches the PINNED 24 m / 3-layer / 64-cell shape.

#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "luminumbra_common/systems/WindFieldSystem.h"

namespace {

using Luminumbra::Vec2;
using Luminumbra::Vec3;
using Luminumbra::Systems::kWindCellSizeM;
using Luminumbra::Systems::kWindExtentCells;
using Luminumbra::Systems::kWindLayerCount;
using Luminumbra::Systems::WindFieldSystem;
using Luminumbra::Systems::WindLayer;

constexpr int kSeed = 424242;
constexpr std::uint64_t kTicks = 90;
const Vec3 kAnchor(8.0f, 100.0f, 8.0f);

std::string RunWind(int seed, std::uint64_t ticks, const Vec3& anchor) {
    WindFieldSystem wind(seed);
    for (std::uint64_t t = 1; t <= ticks; ++t) {
        wind.Update(t, anchor);
    }
    return wind.ComputeWindSubHash();
}

TEST(WindFieldSystem, GeometryMatchesPinnedShape) {
    WindFieldSystem wind(kSeed);
    EXPECT_EQ(wind.extent_cells(), kWindExtentCells);
    EXPECT_FLOAT_EQ(wind.cell_size_m(), kWindCellSizeM);
    EXPECT_EQ(kWindLayerCount, 3);
}

TEST(WindFieldSystem, SubHashIsDeterministicAcrossRuns) {
    // The load-bearing property: two fresh instances, same inputs, equal hash.
    const std::string a = RunWind(kSeed, kTicks, kAnchor);
    const std::string b = RunWind(kSeed, kTicks, kAnchor);
    EXPECT_FALSE(a.empty());
    EXPECT_EQ(a, b) << "wind sub-hash diverged across identical runs (non-deterministic)";
}

TEST(WindFieldSystem, SubHashEvolvesWithTick) {
    // The field must actually change over time (a frozen field would make the
    // gate vacuous). Different tick counts => different hashes.
    const std::string h30 = RunWind(kSeed, 30, kAnchor);
    const std::string h90 = RunWind(kSeed, 90, kAnchor);
    EXPECT_NE(h30, h90);
}

TEST(WindFieldSystem, SubHashDependsOnSeed) {
    const std::string a = RunWind(kSeed, kTicks, kAnchor);
    const std::string b = RunWind(kSeed + 1, kTicks, kAnchor);
    EXPECT_NE(a, b) << "wind field ignored the seed";
}

TEST(WindFieldSystem, SampleInRegionMatchesCell) {
    WindFieldSystem wind(kSeed);
    wind.Update(kTicks, kAnchor);
    // A sample at the anchor is inside the centred streamed extent; it should
    // return a non-zero wind vector (the cells are populated every Update).
    const Vec2 ground = wind.SampleWind(Vec3(kAnchor.x, 5.0f, kAnchor.z), WindLayer::Ground);
    const float mag = std::sqrt(ground.x * ground.x + ground.y * ground.y);
    EXPECT_GT(mag, 0.0f);
    // Higher layers blow harder than the surface-sheared ground layer.
    const Vec2 high = wind.SampleWind(Vec3(kAnchor.x, 5.0f, kAnchor.z), WindLayer::High);
    const float high_mag = std::sqrt(high.x * high.x + high.y * high.y);
    EXPECT_GT(high_mag, mag);
}

TEST(WindFieldSystem, OutOfRegionClampsToBaseDirection) {
    WindFieldSystem wind(kSeed);
    wind.Update(kTicks, kAnchor);
    const Vec2 base = wind.BaseDirection();
    // A position far outside the centred 64-cell (1536 m) extent clamps to the
    // base direction at the layer speed.
    const float far = 1.0e6f;
    const Vec2 out = wind.SampleWind(Vec3(far, 5.0f, far), WindLayer::Ground);
    const float out_mag = std::sqrt(out.x * out.x + out.y * out.y);
    ASSERT_GT(out_mag, 0.0f);
    // Direction equals the base direction (cross product ~0, dot > 0).
    const float cross = base.x * out.y - base.y * out.x;
    const float dot = base.x * out.x + base.y * out.y;
    EXPECT_NEAR(cross, 0.0f, 1.0e-3f * out_mag);
    EXPECT_GT(dot, 0.0f);
}

TEST(WindFieldSystem, LayerSelectionByHeight) {
    WindFieldSystem wind(kSeed);
    wind.Update(kTicks, kAnchor);
    // The AGL-variant SampleWind selects the layer from world_pos.y. Ground band
    // (< 32 m) and high band (>= 128 m) at the same x/z must match the explicit
    // layer samples.
    const Vec3 col(kAnchor.x, 0.0f, kAnchor.z);
    const Vec2 by_height_ground = wind.SampleWind(Vec3(col.x, 10.0f, col.z));
    const Vec2 explicit_ground = wind.SampleWind(col, WindLayer::Ground);
    EXPECT_FLOAT_EQ(by_height_ground.x, explicit_ground.x);
    EXPECT_FLOAT_EQ(by_height_ground.y, explicit_ground.y);

    const Vec2 by_height_high = wind.SampleWind(Vec3(col.x, 300.0f, col.z));
    const Vec2 explicit_high = wind.SampleWind(col, WindLayer::High);
    EXPECT_FLOAT_EQ(by_height_high.x, explicit_high.x);
    EXPECT_FLOAT_EQ(by_height_high.y, explicit_high.y);
}

TEST(WindFieldSystem, StormPerturbationsAreLocalizedAndOrderIndependent) {
    const WindFieldSystem::StormPerturbation first{
        Vec2(kAnchor.x, kAnchor.z), 120.0f, Vec2(5.0f, 1.0f), 0.8f};
    const WindFieldSystem::StormPerturbation second{
        Vec2(kAnchor.x + 20.0f, kAnchor.z), 80.0f, Vec2(-1.0f, 3.0f), 0.5f};

    WindFieldSystem a(kSeed);
    a.InjectStormPerturbation(first);
    a.InjectStormPerturbation(second);
    a.Update(kTicks, kAnchor);

    WindFieldSystem b(kSeed);
    b.InjectStormPerturbation(second);
    b.InjectStormPerturbation(first);
    b.Update(kTicks, kAnchor);

    EXPECT_EQ(a.ComputeWindSubHash(), b.ComputeWindSubHash());

    WindFieldSystem calm(kSeed);
    calm.Update(kTicks, kAnchor);
    const Vec2 gust = a.SampleWind(Vec3(kAnchor.x, 5.0f, kAnchor.z));
    const Vec2 baseline = calm.SampleWind(Vec3(kAnchor.x, 5.0f, kAnchor.z));
    EXPECT_TRUE(gust.x != baseline.x || gust.y != baseline.y);

    const Vec3 far_inside(kAnchor.x + 500.0f, 5.0f, kAnchor.z + 500.0f);
    const Vec2 far_gust = a.SampleWind(far_inside);
    const Vec2 far_calm = calm.SampleWind(far_inside);
    EXPECT_FLOAT_EQ(far_gust.x, far_calm.x);
    EXPECT_FLOAT_EQ(far_gust.y, far_calm.y);

    a.ClearStormPerturbations();
    a.Update(kTicks, kAnchor);
    EXPECT_EQ(a.ComputeWindSubHash(), calm.ComputeWindSubHash());
}

} // namespace
