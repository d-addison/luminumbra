// deterministic Aetheric scalar-field determinism + behavior test.
//
// Proves the field is bit-deterministic (the property the world_hash `aether`
// sub-hash and the AetherFieldDeterminism gate depend on): two independent
// instances advanced through the same ticks with the same seed/anchor produce
// the IDENTICAL aether sub-hash; a different seed differs; the field EVOLVES
// over ticks and is non-trivial (not a flat constant); samples are non-negative;
// out-of-region reads 0; and the wind advection measurably changes the field.

#include <gtest/gtest.h>

#include <string>

#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"

namespace {

using Luminumbra::Vec3;
using Luminumbra::Systems::AetherFieldSystem;
using Luminumbra::Systems::kAetherCellSizeM;
using Luminumbra::Systems::kAetherDiffuseIterations;
using Luminumbra::Systems::kAetherExtentCells;
using Luminumbra::Systems::WindFieldSystem;

constexpr int kSeed = 424242;
constexpr std::uint64_t kTicks = 90;
const Vec3 kAnchor(8.0f, 100.0f, 8.0f);

std::string RunAether(int seed, std::uint64_t ticks, const Vec3& anchor) {
    AetherFieldSystem aether(seed);
    for (std::uint64_t t = 1; t <= ticks; ++t) {
        aether.Update(t, anchor);
    }
    return aether.ComputeAetherSubHash();
}

TEST(AetherFieldSystem, GeometryMatchesPinnedShape) {
    AetherFieldSystem aether(kSeed);
    EXPECT_EQ(aether.extent_cells(), kAetherExtentCells);
    EXPECT_FLOAT_EQ(aether.cell_size_m(), kAetherCellSizeM);
    EXPECT_EQ(aether.diffuse_iterations(), kAetherDiffuseIterations);
}

TEST(AetherFieldSystem, SubHashIsDeterministicAcrossRuns) {
    const std::string a = RunAether(kSeed, kTicks, kAnchor);
    const std::string b = RunAether(kSeed, kTicks, kAnchor);
    EXPECT_FALSE(a.empty());
    EXPECT_EQ(a, b) << "aether sub-hash diverged across identical runs (non-deterministic)";
}

TEST(AetherFieldSystem, SubHashEvolvesWithTick) {
    const std::string h30 = RunAether(kSeed, 30, kAnchor);
    const std::string h90 = RunAether(kSeed, 90, kAnchor);
    EXPECT_NE(h30, h90) << "aether field did not evolve over ticks (gate would be vacuous)";
}

TEST(AetherFieldSystem, SubHashDependsOnSeed) {
    const std::string a = RunAether(kSeed, kTicks, kAnchor);
    const std::string b = RunAether(kSeed + 1, kTicks, kAnchor);
    EXPECT_NE(a, b) << "aether field ignored the seed";
}

TEST(AetherFieldSystem, FieldIsNonNegativeAndNonTrivial) {
    AetherFieldSystem aether(kSeed);
    aether.Update(kTicks, kAnchor);
    // Scan a grid of in-region samples: all >= 0, and the field has spatial
    // variation (a flat-zero or flat-constant field would be a broken solver).
    float min_v = 1.0e30f;
    float max_v = -1.0e30f;
    const float cell = aether.cell_size_m();
    for (int lz = 0; lz < aether.extent_cells(); lz += 4) {
        for (int lx = 0; lx < aether.extent_cells(); lx += 4) {
            const float wx =
                kAnchor.x + (static_cast<float>(lx - aether.extent_cells() / 2)) * cell;
            const float wz =
                kAnchor.z + (static_cast<float>(lz - aether.extent_cells() / 2)) * cell;
            const float v = aether.SampleAether(Vec3(wx, 0.0f, wz));
            EXPECT_GE(v, 0.0f) << "aether sample is negative at (" << lx << "," << lz << ")";
            min_v = (v < min_v) ? v : min_v;
            max_v = (v > max_v) ? v : max_v;
        }
    }
    EXPECT_GT(max_v, 0.0f) << "aether field is entirely zero (solver inert)";
    EXPECT_GT(max_v - min_v, 0.0f) << "aether field is a flat constant (no spatial structure)";
}

TEST(AetherFieldSystem, OutOfRegionReturnsZero) {
    AetherFieldSystem aether(kSeed);
    aether.Update(kTicks, kAnchor);
    const float far = 1.0e6f;
    EXPECT_FLOAT_EQ(aether.SampleAether(Vec3(far, 0.0f, far)), 0.0f);
}

TEST(AetherFieldSystem, WindAdvectionChangesTheField) {
    // Same seed/anchor/tick, but one run advects by a live wind field. The
    // advection backtrace shifts the emission source, so the diffused field --
    // and thus the sub-hash -- must differ from the no-wind run.
    AetherFieldSystem no_wind(kSeed);
    for (std::uint64_t t = 1; t <= kTicks; ++t) {
        no_wind.Update(t, kAnchor, nullptr);
    }

    WindFieldSystem wind(kSeed);
    AetherFieldSystem with_wind(kSeed);
    for (std::uint64_t t = 1; t <= kTicks; ++t) {
        wind.Update(t, kAnchor);
        with_wind.Update(t, kAnchor, &wind);
    }

    EXPECT_NE(no_wind.ComputeAetherSubHash(), with_wind.ComputeAetherSubHash())
        << "wind advection had no effect on the aether field";
}

} // namespace
