#include "../fixtures/TerrainFixtureParams.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/FarVolume.h"

#include <gtest/gtest.h>

#include <optional>

namespace {
using namespace Luminumbra::World;

FarVolumeCompilationLimits PristineLimits() {
    // Explicit test limits, not approved runtime residency caps.
    return {{131 * 131, 20'000'000}, {131'072, 16'384}, {1'048'576, 3'000'000, 300'000, 1'000'000}};
}
FarVolumeCoverageIntent PristineIntent() {
    return {{1, 0, 0}, {}};
}
FarVolumeRequest PristineHeld() {
    return {{3, 6, 7}, {-64, 64}, 91};
}
Luminumbra::Systems::TerrainGenParams PristinePlaneParams() {
    Luminumbra::Systems::TerrainGenParams params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.peaks_amplitude = 0.0f;
    params.continental_spline = {{-1.0f, 0.0f}, {1.0f, 0.0f}};
    params.height_offset = 0.25f;
    params.caves_enabled = false;
    return params;
}
FarVolumeCoverageIntent PristinePlaneIntent() {
    // V5 keeps the 256 m interior band to two layers of 256 m bricks.
    return {{5, 0, 0}, {}};
}
FarVolumeCompilationLimits PristinePlaneLimits() {
    // One mixed brick per 32x32 column, 128x128 surface cells, two triangles
    // per surface cell and one vertex per 129x129 horizontal lattice point.
    return {{131 * 131, 1'000'000},
            {2 * 32 * 32, 32 * 32},
            {32 * 32 * 64, 300'000, 129 * 129, 128 * 128 * 6}};
}
} // namespace

TEST(FarVolumeWorld, RejectsMissingRetainedBinding) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    FarVolumeMemoryPool pool(1024);
    FarVolumeCompilation output;
    output.tile.request = PristineHeld();
    const auto previous = output;
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompilePristineFarVolumeCharged(world,
                                                 PristineIntent(),
                                                 42,
                                                 FarCaveMode::BandLimited,
                                                 FarVolumeNumericProfile::RawFloat,
                                                 PristineLimits(),
                                                 {&pool, nullptr, nullptr},
                                                 output,
                                                 &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::InvalidMemory);
    EXPECT_EQ(failure.work, FarVolumeAuthorityWork{});
    EXPECT_EQ(pool.attempts(), 0u);
    EXPECT_EQ(pool.used(), 0u);
    EXPECT_EQ(output, previous);
}

TEST(FarVolumeWorld, RefusalPreservesPriorOutputAndCharge) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    FarVolumeMemoryPool pool(1024);
    FarVolumeMemoryLease retained(pool);
    FarVolumeCompilation output;
    output.mesh.vertices.emplace_back();
    output.tile.request = PristineHeld();
    ASSERT_TRUE(retained.TryCharge(FarVolumeCompilationRetainedBytes(output)));
    const auto previous = output;
    const auto held = retained.bytes();
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompilePristineFarVolumeCharged(world,
                                                 PristineIntent(),
                                                 42,
                                                 FarCaveMode::BandLimited,
                                                 FarVolumeNumericProfile::LegacySigned16,
                                                 PristineLimits(),
                                                 {&pool, &retained, nullptr},
                                                 output,
                                                 &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::AllocationFailure);
    // The first height-cache allocation is refused before any world sampling.
    EXPECT_EQ(failure.work, FarVolumeAuthorityWork{});
    EXPECT_EQ(pool.refusals(), 1u);
    EXPECT_EQ(retained.bytes(), held);
    EXPECT_EQ(pool.used(), held);
    EXPECT_EQ(output, previous);
}

TEST(FarVolumeWorld, NullPoolKeepsLegacyRefusal) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    FarVolumeCompilation expected, actual;
    expected.tile.request = actual.tile.request = PristineHeld();
    FarVolumeFacadeFailure uncharged, charged;
    auto limits = PristineLimits();
    limits.authority.max_height_samples = 0;
    EXPECT_FALSE(CompilePristineFarVolume(world,
                                          PristineIntent(),
                                          42,
                                          FarCaveMode::BandLimited,
                                          FarVolumeNumericProfile::RawFloat,
                                          limits,
                                          expected,
                                          &uncharged));
    EXPECT_FALSE(CompilePristineFarVolumeCharged(world,
                                                 PristineIntent(),
                                                 42,
                                                 FarCaveMode::BandLimited,
                                                 FarVolumeNumericProfile::RawFloat,
                                                 limits,
                                                 {},
                                                 actual,
                                                 &charged));
    EXPECT_EQ(uncharged.code, FarVolumeFacadeError::HeightLimit);
    EXPECT_EQ(charged.code, uncharged.code);
    EXPECT_EQ(charged.work, uncharged.work);
    EXPECT_EQ(actual, expected);
}

TEST(FarVolumeWorld, SuccessfulChargedCompileMatchesUnchargedAndNullPool) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, PristinePlaneParams(), 1337);
    const auto intent = PristinePlaneIntent();
    const auto limits = PristinePlaneLimits();
    FarVolumeMemoryPool pool(64u * 1024u * 1024u);
    FarVolumeMemoryLease retained(pool);
    FarVolumeCompilation output;
    output.tile.request = PristineHeld();
    output.mesh.vertices.emplace_back();
    ASSERT_TRUE(retained.TryCharge(FarVolumeCompilationRetainedBytes(output)));
    FarVolumeFacadeFailure failure;
    ASSERT_TRUE(CompilePristineFarVolumeCharged(world,
                                                intent,
                                                42,
                                                FarCaveMode::BandLimited,
                                                FarVolumeNumericProfile::RawFloat,
                                                limits,
                                                {&pool, &retained, nullptr},
                                                output,
                                                &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::None);
    EXPECT_EQ(failure.generation, FarVolumeBuildError::None);
    EXPECT_EQ(failure.mesh, FarVolumeMeshError::None);
    EXPECT_EQ(output.intent, intent);
    EXPECT_EQ(output.caves, FarCaveMode::BandLimited);
    EXPECT_EQ(output.numeric, FarVolumeNumericProfile::RawFloat);
    EXPECT_EQ(output.tile.request, (FarVolumeRequest{intent.key, {-256, 256}, 42}));
    EXPECT_EQ(output.tile.min_meters, (FarVolumePosition{0, -256, 0}));
    EXPECT_EQ(output.tile.max_meters, (FarVolumePosition{8192, 256, 8192}));
    EXPECT_EQ(output.tile.content, FarVolumeContent::Mixed);
    ASSERT_EQ(output.tile.bricks.size(), 32u * 32u);
    EXPECT_EQ(output.tile.candidates_visited, 2u * 32u * 32u);
    EXPECT_EQ(output.tile.samples_evaluated,
              output.tile.candidates_visited * kFarVolumeBrickSamples);
    EXPECT_EQ(output.mesh.request, output.tile.request);
    EXPECT_EQ(output.mesh.origin_meters, output.tile.min_meters);
    EXPECT_EQ(output.mesh.sampled_max_meters, output.tile.max_meters);
    EXPECT_EQ(output.mesh.content, FarVolumeContent::Mixed);
    EXPECT_EQ(output.mesh.cells_visited, output.tile.bricks.size() * 64u);
    ASSERT_EQ(output.mesh.vertices.size(), 129u * 129u);
    ASSERT_EQ(output.mesh.indices.size(), 128u * 128u * 6u);
    ASSERT_TRUE(output.mesh.bounds.has_value());
    if (output.mesh.bounds) {
        EXPECT_EQ(output.mesh.bounds->min, (FarVolumeVector{0.0, 256.25, 0.0}));
        EXPECT_EQ(output.mesh.bounds->max, (FarVolumeVector{8192.0, 256.25, 8192.0}));
    }
    for (const auto index : output.mesh.indices)
        ASSERT_LT(index, output.mesh.vertices.size());
    EXPECT_EQ(output.work, failure.work);
    EXPECT_GT(output.work.height_samples, 129u * 129u); // Includes the normal halo.
    EXPECT_EQ(output.work.density_samples,
              output.tile.samples_evaluated + output.mesh.samples_evaluated);
    EXPECT_GT(retained.bytes(), 0u);
    EXPECT_EQ(retained.bytes(), FarVolumeCompilationRetainedBytes(output));
    EXPECT_EQ(pool.used(), retained.bytes());
    EXPECT_EQ(pool.refusals(), 0u);

    FarVolumeCompilation uncharged, null_pool;
    FarVolumeFacadeFailure uncharged_failure, null_pool_failure;
    ASSERT_TRUE(CompilePristineFarVolume(world,
                                         intent,
                                         42,
                                         FarCaveMode::BandLimited,
                                         FarVolumeNumericProfile::RawFloat,
                                         limits,
                                         uncharged,
                                         &uncharged_failure));
    ASSERT_TRUE(CompilePristineFarVolumeCharged(world,
                                                intent,
                                                42,
                                                FarCaveMode::BandLimited,
                                                FarVolumeNumericProfile::RawFloat,
                                                limits,
                                                {},
                                                null_pool,
                                                &null_pool_failure));
    EXPECT_EQ(uncharged_failure.code, FarVolumeFacadeError::None);
    EXPECT_EQ(null_pool_failure.code, FarVolumeFacadeError::None);
    EXPECT_EQ(uncharged_failure.work, failure.work);
    EXPECT_EQ(null_pool_failure.work, failure.work);
    EXPECT_EQ(output, uncharged);
    EXPECT_EQ(output, null_pool);
    EXPECT_EQ(pool.used(), retained.bytes());
}

TEST(FarVolumeWorld, BrickAllocationRefusalAfterSamplingPreservesPriorOutputAndCharge) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, PristinePlaneParams(), 1337);
    FarVolumeCompilation output;
    output.mesh.vertices.emplace_back();
    output.tile.request = PristineHeld();
    const auto held = FarVolumeCompilationRetainedBytes(output);
    // Measure discovery's height cache, including allocator bookkeeping.
    FarVolumeMemoryPool discovery_pool(64u * 1024u * 1024u);
    FarVolumeAuthority authority;
    authority.field_identity = 42;
    authority.caves = FarCaveMode::BandLimited;
    authority.numeric = FarVolumeNumericProfile::LegacySigned16;
    authority.height = [&world](std::int64_t x, std::int64_t z) {
        return world.GetTerrainHeightAt(static_cast<float>(x), static_cast<float>(z));
    };
    // Discovery requires a density callback but never invokes it.
    authority.density = [](const FarVolumePosition&, float, std::uint32_t, FarCaveMode) {
        return FarVolumeSample{};
    };
    FarVolumeRequest discovered;
    {
        const auto scope = world.acquire_worldgen_sample_scope();
        ASSERT_TRUE(ResolveFarVolumeCoverageCharged(PristinePlaneIntent(),
                                                    authority,
                                                    PristinePlaneLimits().authority,
                                                    {&discovery_pool, nullptr, nullptr},
                                                    discovered));
    }
    const auto measured_discovery_peak = discovery_pool.peak();
    // No bytes remain for the first mixed-brick buffer, after two bricks' samples.
    FarVolumeMemoryPool pool(held + measured_discovery_peak);
    FarVolumeMemoryLease retained(pool);
    ASSERT_TRUE(retained.TryCharge(held));
    const auto previous = output;
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompilePristineFarVolumeCharged(world,
                                                 PristinePlaneIntent(),
                                                 42,
                                                 FarCaveMode::BandLimited,
                                                 FarVolumeNumericProfile::LegacySigned16,
                                                 PristinePlaneLimits(),
                                                 {&pool, &retained, nullptr},
                                                 output,
                                                 &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::AllocationFailure);
    EXPECT_EQ(failure.generation, FarVolumeBuildError::AllocationFailure);
    EXPECT_EQ(failure.mesh, FarVolumeMeshError::None);
    EXPECT_EQ(failure.work.height_samples, 129u * 129u);
    EXPECT_EQ(failure.work.density_samples, 2u * kFarVolumeBrickSamples);
    EXPECT_EQ(pool.refusals(), 1u);
    EXPECT_EQ(pool.peak(), pool.limit());
    EXPECT_EQ(output, previous);
    EXPECT_EQ(retained.bytes(), held);
    EXPECT_EQ(retained.bytes(), FarVolumeCompilationRetainedBytes(output));
    EXPECT_EQ(pool.used(), held);
}
