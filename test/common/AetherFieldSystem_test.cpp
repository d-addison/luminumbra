// deterministic Aetheric scalar-field determinism + behavior test.
//
// Proves the field is bit-deterministic (the property the world_hash `aether`
// sub-hash and the AetherFieldDeterminism gate depend on): two independent
// instances advanced through the same ticks with the same seed/anchor produce
// the IDENTICAL aether sub-hash; a different seed differs; the field EVOLVES
// over ticks and is non-trivial (not a flat constant); samples are non-negative;
// out-of-region reads 0; and the wind advection measurably changes the field.

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "luminumbra/core/Types.h"
#include "luminumbra_common/systems/AmbientNoiseDispatch.h"

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

// Compare with terrain's explicit ceiling, independently of the ambient helper.
// On non-x86 builds the existing native dispatch remains the reference.
TEST(AetherFieldSystem, NoiseDispatchMatchesTerrainCeiling) {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    const auto reference = FastNoise::New<FastNoise::FractalFBm>(FastSIMD::Level_AVX2);
#else
    const auto reference = FastNoise::New<FastNoise::FractalFBm>();
#endif
    AetherFieldSystem field(kSeed);
    RecordProperty("cpu_max_simd", static_cast<int>(FastSIMD::CPUMaxSIMDLevel()));
    RecordProperty("reference_simd", static_cast<int>(reference->GetSIMDLevel()));
    RecordProperty("noise_simd_level", static_cast<int>(field.noise_simd_level()));
    EXPECT_EQ(field.noise_simd_level(), reference->GetSIMDLevel());
    // Keep constructor and evolved hashes in the test receipt. The control and
    // repaired binaries run identical ticks within each build configuration.
    RecordProperty("constructor_hash", field.ComputeAetherSubHash());
    for (std::uint64_t tick = 1; tick <= kTicks; ++tick) {
        field.Update(tick, kAnchor);
    }
    RecordProperty("evolved_hash", field.ComputeAetherSubHash());
}

TEST(AetherFieldSystem, GeometryMatchesPinnedShape) {
    AetherFieldSystem aether(kSeed);
    EXPECT_EQ(aether.extent_cells(), kAetherExtentCells);
    EXPECT_FLOAT_EQ(aether.cell_size_m(), kAetherCellSizeM);
    EXPECT_EQ(aether.diffuse_iterations(), kAetherDiffuseIterations);
}

TEST(AetherFieldSystem, RegionalCallbacksPopulateRenderGridAndDirectSamples) {
    AetherFieldSystem aether(kSeed);
    WindFieldSystem wind(kSeed);
    int wind_calls = 0;
    wind.BindRegionalBackend(
        [&wind_calls](const Vec3&, Luminumbra::Systems::WindLayer) {
            ++wind_calls;
            return Luminumbra::Vec2(100.0f);
        },
        {});
    std::size_t sample_calls = 0;
    aether.BindRegionalBackend(
        [&sample_calls](const Vec3& position) {
            ++sample_calls;
            return 4096.0f + position.x + 2.0f * position.z + position.y;
        },
        [] { return std::string("regional aether"); });
    aether.Update(17, Vec3(25.0f, 100.0f, -1.0f), &wind);
    EXPECT_EQ(aether.last_tick(), 17u);
    EXPECT_EQ(sample_calls, 4096u);
    EXPECT_EQ(wind_calls, 0);
    EXPECT_EQ(aether.ComputeAetherSubHash(), "regional aether");
    // floor(25/24)-32 = -31; floor(-1/24)-32 = -33.
    const auto& field_grid = aether.grid();
    EXPECT_EQ(field_grid.origin_cell_x(), -31);
    EXPECT_EQ(field_grid.origin_cell_z(), -33);
    for (int row = 0; row < 64; ++row) {
        for (int column = 0; column < 64; ++column) {
            // First cell centre (-732,-780), then +24 m per column/row, y = 0.
            const float expected =
                1804.0f + 24.0f * static_cast<float>(column) + 48.0f * static_cast<float>(row);
            EXPECT_EQ(field_grid.at(column, row), expected);
        }
    }
    // Direct sampling preserves the full position and is not clamped to the texture extent.
    EXPECT_EQ(aether.SampleAether(Vec3(24.0f, 7.0f, -12.0f)), 4103.0f);
    EXPECT_EQ(aether.SampleAether(Vec3(2400.0f, 7.0f, -12.0f)), 6479.0f);
    EXPECT_EQ(sample_calls, 4098u);
}

TEST(AetherFieldSystem, EmptyRegionalCallbacksPreserveLegacySamplesAndHash) {
    AetherFieldSystem baseline(kSeed);
    AetherFieldSystem aether(kSeed);
    aether.BindRegionalBackend({}, {});
    EXPECT_EQ(aether.ComputeAetherSubHash(), baseline.ComputeAetherSubHash());

    aether.BindRegionalBackend(
        [](const Vec3& position) { return 4096.0f + position.x + 2.0f * position.z; },
        [] { return std::string("regional aether"); });
    aether.Update(1, Vec3(25.0f, 100.0f, -1.0f));
    aether.BindRegionalBackend({}, {});
    // Legacy sampling uses floor(24/24)=1, floor(-12/24)=-1: local cell (32,32).
    // Its centre is (36,-12), so the stored value is 4096+36-24=4108, ignoring y.
    EXPECT_EQ(aether.SampleAether(Vec3(24.0f, 100.0f, -12.0f)), 4108.0f);
    // x=792 is global cell 33, local column 64: outside the 64-cell footprint.
    EXPECT_EQ(aether.SampleAether(Vec3(792.0f, 0.0f, -12.0f)), 0.0f);
    EXPECT_EQ(aether.SampleAether(Vec3(-745.0f, 0.0f, -12.0f)), 0.0f);

    WindFieldSystem wind(kSeed);
    for (const WindFieldSystem* advection : {static_cast<const WindFieldSystem*>(nullptr),
                                             static_cast<const WindFieldSystem*>(&wind)}) {
        for (const std::uint64_t tick : {1u, 45u, 90u}) {
            wind.Update(tick, kAnchor);
            baseline.Update(tick, kAnchor, advection);
            aether.Update(tick, kAnchor, advection);
            EXPECT_EQ(aether.last_tick(), tick);
            EXPECT_EQ(aether.ComputeAetherSubHash(), baseline.ComputeAetherSubHash());
            const auto& actual_cells = aether.grid().cells();
            const auto& original_cells = baseline.grid().cells();
            ASSERT_EQ(actual_cells.size(), original_cells.size());
            for (std::size_t index = 0; index < actual_cells.size(); ++index) {
                EXPECT_EQ(std::bit_cast<std::uint32_t>(actual_cells[index]),
                          std::bit_cast<std::uint32_t>(original_cells[index]));
            }
            EXPECT_EQ(aether.SampleAether(Vec3(1000000.0f)), 0.0f);
        }
    }
}

TEST(AetherFieldSystem, RegionalRenderGridRejectsUnrepresentableAnchors) {
    AetherFieldSystem aether(kSeed);
    std::size_t sample_calls = 0;
    aether.BindRegionalBackend(
        [&sample_calls](const Vec3&) {
            ++sample_calls;
            return 0.75f;
        },
        [] { return std::string("regional aether"); });
    const float extreme = (std::numeric_limits<float>::max)();
    const float infinity = std::numeric_limits<float>::infinity();
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    // 24 * 2^63 metres reaches the integer conversion boundary exactly.
    for (const float coordinate :
         {extreme, -extreme, 0x1.8p67f, -0x1.8p67f, infinity, -infinity, invalid}) {
        for (const auto& anchor : {Vec3(coordinate, 0.0f, 0.0f), Vec3(0.0f, 0.0f, coordinate)}) {
            aether.Update(1, kAnchor);
            EXPECT_EQ(aether.grid().at(0, 0), 0.75f);
            sample_calls = 0;
            aether.Update(2, anchor);
            EXPECT_EQ(sample_calls, 0u);
            EXPECT_EQ(aether.last_tick(), 2u);
            EXPECT_EQ(aether.grid().origin_cell_x(), 0);
            EXPECT_EQ(aether.grid().origin_cell_z(), 0);
            for (const float value : aether.grid().cells()) {
                EXPECT_EQ(value, 0.0f);
            }
            EXPECT_EQ(aether.SampleAether(anchor), 0.75f);
            EXPECT_EQ(aether.ComputeAetherSubHash(), "regional aether");
        }
    }
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
