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

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "luminumbra/core/Types.h"
#include "luminumbra_common/systems/AmbientNoiseDispatch.h"

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

// Compare with terrain's explicit ceiling, independently of the ambient helper.
// On non-x86 builds the existing native dispatch remains the reference.
TEST(WindFieldSystem, NoiseDispatchMatchesTerrainCeiling) {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    const auto reference = FastNoise::New<FastNoise::FractalFBm>(FastSIMD::Level_AVX2);
#else
    const auto reference = FastNoise::New<FastNoise::FractalFBm>();
#endif
    WindFieldSystem field(kSeed);
    RecordProperty("cpu_max_simd", static_cast<int>(FastSIMD::CPUMaxSIMDLevel()));
    RecordProperty("reference_simd", static_cast<int>(reference->GetSIMDLevel()));
    RecordProperty("noise_simd_level", static_cast<int>(field.noise_simd_level()));
    EXPECT_EQ(field.noise_simd_level(), reference->GetSIMDLevel());
    // Keep constructor and evolved hashes in the test receipt. The control and
    // repaired binaries run identical ticks within each build configuration.
    RecordProperty("constructor_hash", field.ComputeWindSubHash());
    for (std::uint64_t tick = 1; tick <= kTicks; ++tick) {
        field.Update(tick, kAnchor);
    }
    RecordProperty("evolved_hash", field.ComputeWindSubHash());
}

TEST(AmbientNoiseDispatch, FractalAndSimplexMatchTerrainCeiling) {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    constexpr auto reference_ceiling = FastSIMD::Level_AVX2;
#else
    constexpr auto reference_ceiling = FastSIMD::Level_Null;
#endif
    auto fractal = Luminumbra::Systems::NewAmbientNoise<FastNoise::FractalFBm>();
    auto simplex = Luminumbra::Systems::NewAmbientNoise<FastNoise::Simplex>();
    auto reference = FastNoise::New<FastNoise::FractalFBm>(reference_ceiling);
    auto reference_simplex = FastNoise::New<FastNoise::Simplex>(reference_ceiling);
    ASSERT_TRUE(fractal && simplex && reference && reference_simplex);
    RecordProperty("fractal_simd", static_cast<int>(fractal->GetSIMDLevel()));
    RecordProperty("simplex_simd", static_cast<int>(simplex->GetSIMDLevel()));
    ASSERT_EQ(fractal->GetSIMDLevel(), reference->GetSIMDLevel());
    ASSERT_EQ(simplex->GetSIMDLevel(), reference_simplex->GetSIMDLevel());
    fractal->SetSource(simplex);
    reference->SetSource(reference_simplex);
    fractal->SetOctaveCount(2);
    reference->SetOctaveCount(2);

    // Full AVX-512-width arrays keep this dispatch test independent of tail padding; the
    // padding of real call sites is covered by AmbientInputBounds.
    std::array<float, 16> x{}, z{}, actual{}, expected{};
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = (static_cast<float>(i) - 7.5f) * 0.375f;
        z[i] = (static_cast<float>(i) + 0.5f) * -0.215f;
    }
    for (int seed : {kSeed + 11, kSeed + 12, kSeed + 113, kSeed + 223, kSeed + 14}) {
        fractal->GenPositionArray2D(actual.data(), 16, x.data(), z.data(), 0.0f, 0.0f, seed);
        reference->GenPositionArray2D(expected.data(), 16, x.data(), z.data(), 0.0f, 0.0f, seed);
        for (std::size_t i = 0; i < x.size(); ++i) {
            EXPECT_EQ(std::bit_cast<std::uint32_t>(actual[i]),
                      std::bit_cast<std::uint32_t>(expected[i]))
                << "seed=" << seed << " lane=" << i;
        }
    }
}

TEST(WindFieldSystem, GeometryMatchesPinnedShape) {
    WindFieldSystem wind(kSeed);
    EXPECT_EQ(wind.extent_cells(), kWindExtentCells);
    EXPECT_FLOAT_EQ(wind.cell_size_m(), kWindCellSizeM);
    EXPECT_EQ(kWindLayerCount, 3);
}

TEST(WindFieldSystem, RegionalCallbacksReceivePositionAndSelectedLayer) {
    WindFieldSystem wind(kSeed);
    int sample_calls = 0;
    wind.BindRegionalBackend(
        [&sample_calls](const Vec3& position, WindLayer selected_layer) {
            ++sample_calls;
            const auto layer_offset = static_cast<float>(selected_layer);
            return Vec2(position.x + 10.0f * layer_offset, position.z - 5.0f * layer_offset);
        },
        [] { return std::string("regional wind"); });
    wind.Update(17, Vec3(3.0f, 100.0f, 4.0f));
    EXPECT_EQ(sample_calls, 1);
    EXPECT_EQ(wind.last_tick(), 17u);
    // Update explicitly samples ground: (3,4) / sqrt(9+16) = (0.6,0.8).
    EXPECT_FLOAT_EQ(wind.BaseDirection().x, 0.6f);
    EXPECT_FLOAT_EQ(wind.BaseDirection().y, 0.8f);
    EXPECT_EQ(wind.ComputeWindSubHash(), "regional wind");

    constexpr std::array layers{WindLayer::Ground, WindLayer::Mid, WindLayer::High};
    const std::array expected{Vec2(2.0f, 7.0f), Vec2(12.0f, 2.0f), Vec2(22.0f, -3.0f)};
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const auto sampled = wind.SampleWind(Vec3(2.0f, 1000.0f, 7.0f), layers[index]);
        EXPECT_EQ(sampled.x, expected[index].x);
        EXPECT_EQ(sampled.y, expected[index].y);
    }
    constexpr std::array heights{-1.0f, 31.0f, 32.0f, 127.0f, 128.0f, 512.0f};
    for (std::size_t index = 0; index < heights.size(); ++index) {
        const auto sampled = wind.SampleWind(Vec3(2.0f, heights[index], 7.0f));
        EXPECT_EQ(sampled.x, expected[index / 2].x);
        EXPECT_EQ(sampled.y, expected[index / 2].y);
    }
    EXPECT_EQ(sample_calls, 10);

    wind.BindRegionalBackend([](const Vec3&, WindLayer) { return Vec2(0.0f); }, {});
    wind.Update(18, kAnchor);
    EXPECT_EQ(wind.BaseDirection().x, 0.0f);
    EXPECT_EQ(wind.BaseDirection().y, 0.0f);
}

TEST(WindFieldSystem, EmptyRegionalCallbacksPreserveLegacySamplesAndHash) {
    WindFieldSystem baseline(kSeed);
    WindFieldSystem wind(kSeed);
    wind.BindRegionalBackend({}, {});
    EXPECT_EQ(wind.ComputeWindSubHash(), baseline.ComputeWindSubHash());

    // Seed a known base direction through the public callback, then remove it.
    // The regional update leaves the constructor's cell grid untouched.
    wind.BindRegionalBackend([](const Vec3&, WindLayer) { return Vec2(0.0f, 5.0f); },
                             [] { return std::string("regional wind"); });
    wind.Update(1, kAnchor);
    wind.BindRegionalBackend({}, {});
    // Out-of-region legacy wind = normalized (0,5) times the layer speeds 6,9,13.
    constexpr std::array heights{0.0f, 32.0f, 128.0f};
    constexpr std::array speeds{6.0f, 9.0f, 13.0f};
    for (std::size_t index = 0; index < heights.size(); ++index) {
        const auto sampled = wind.SampleWind(Vec3(1000000.0f, heights[index], 1000000.0f));
        EXPECT_EQ(sampled.x, 0.0f);
        EXPECT_EQ(sampled.y, speeds[index]);
        const auto inside = wind.SampleWind(Vec3(0.0f, heights[index], 0.0f));
        const auto original = baseline.SampleWind(Vec3(0.0f, heights[index], 0.0f));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(inside.x), std::bit_cast<std::uint32_t>(original.x));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(inside.y), std::bit_cast<std::uint32_t>(original.y));
    }

    const WindFieldSystem::StormPerturbation perturbation{
        Vec2(-12.0f, 12.0f), 120.0f, Vec2(5.0f, 1.0f), 0.8f};
    baseline.InjectStormPerturbation(perturbation);
    wind.InjectStormPerturbation(perturbation);
    for (const std::uint64_t tick : {1u, 45u, 90u}) {
        baseline.Update(tick, Vec3(-1.0f, 100.0f, 25.0f));
        wind.Update(tick, Vec3(-1.0f, 100.0f, 25.0f));
        EXPECT_EQ(wind.last_tick(), tick);
        EXPECT_EQ(wind.ComputeWindSubHash(), baseline.ComputeWindSubHash());
        for (const auto height : heights) {
            for (const auto coordinate : {-768.0f, -0.1f, 0.0f, 767.0f, 768.0f}) {
                const Vec3 position(coordinate, height, coordinate);
                const auto sampled = wind.SampleWind(position);
                const auto original = baseline.SampleWind(position);
                EXPECT_EQ(std::bit_cast<std::uint32_t>(sampled.x),
                          std::bit_cast<std::uint32_t>(original.x));
                EXPECT_EQ(std::bit_cast<std::uint32_t>(sampled.y),
                          std::bit_cast<std::uint32_t>(original.y));
            }
        }
    }
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
