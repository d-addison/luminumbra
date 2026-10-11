#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/FarVolume.h"

#include <gtest/gtest.h>

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
} // namespace

TEST(FarVolumeWorld, RejectsMissingRetainedBinding) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, Luminumbra::Systems::TerrainGenParams{}, 1337);
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
        nullptr, nullptr, Luminumbra::Systems::TerrainGenParams{}, 1337);
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
        nullptr, nullptr, Luminumbra::Systems::TerrainGenParams{}, 1337);
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
