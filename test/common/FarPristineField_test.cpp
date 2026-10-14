#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/FarVolume.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace {
using Luminumbra::Vec3;
using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::Systems::TerrainGenParams;
using Luminumbra::World::FarCaveMode;

TerrainGenParams Caverns() {
    const auto preset = Luminumbra::world::LoadTerrainPreset(
        std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/caverns.json");
    if (!preset.ok)
        throw std::runtime_error("Could not load caverns preset");
    return preset.params;
}

// Lane A #176, 9b04e1a6:test/common/FarVolume_test.cpp:327.
TEST(FarPristineField, FineTiersMatchLiveCaveBitsAndNestedSamples) {
    const auto params = Caverns();
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    for (int z = -4; z <= 4; ++z)
        for (int x = -4; x <= 4; ++x)
            for (int y = 0; y < 8; ++y) {
                const Vec3 p(static_cast<float>(x * 8),
                             static_cast<float>(-y * 8),
                             static_cast<float>(z * 8));
                const float height = world.GetTerrainHeightAt(p.x, p.z);
                // The existing live router is the independent operation-order
                // reference, rather than another invocation of the far sampler.
                const auto expected = std::bit_cast<std::uint32_t>(
                    world.get_density_at_from_precalculated(p, height));
                for (const auto mode : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
                    SCOPED_TRACE(::testing::Message() << "position=" << p.x << "," << p.y << ","
                                                      << p.z << " mode=" << static_cast<int>(mode));
                    for (const std::uint32_t spacing : {4u, 8u})
                        EXPECT_EQ(std::bit_cast<std::uint32_t>(
                                      world.SamplePristineFarDensity(p, height, spacing, mode)),
                                  expected)
                            << "spacing=" << spacing;
                }
            }
}

// Lane A #176, 9b04e1a6:test/common/FarVolume_test.cpp:350.
TEST(FarPristineField, CoarseAnalyticOpeningSurvivesNoiseCutoffAndOldSignControlFails) {
    auto params = Caverns();
    params.carve_smoothness = 0.0f;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    const auto feature = world.FindLargestSurfaceBreak(-12288.0f, -4096.0f, 512.0f);
    ASSERT_TRUE(feature.found);
    EXPECT_GT(std::hypot(feature.x, feature.z), 8192.0f);
    const float height = world.GetTerrainHeightAt(feature.x, feature.z);
    const Vec3 inside(feature.x, height - feature.depth * 0.5f, feature.z);
    const float signed_opening = world.AnalyticSurfaceOpeningDensity(inside, height);
    ASSERT_GT(signed_opening, 0.0f);
    // Exercise the historical composition itself, with noise suppressed by its
    // cap branch and smoothing disabled. It fails the intended positive-air sign.
    const float historical =
        world.EvaluateCaveDensity(inside, inside.y - height, 1.0e6f, signed_opening);
    EXPECT_LT(historical, 0.0f);
    const Vec3 below(feature.x, height - feature.depth - feature.radius * 2.0f, feature.z);
    EXPECT_LT(world.AnalyticSurfaceOpeningDensity(below, height), 0.0f);
    for (const std::uint32_t spacing : {32u, 64u}) {
        SCOPED_TRACE(spacing);
        for (const auto mode : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered})
            EXPECT_GT(world.SamplePristineFarDensity(inside, height, spacing, mode), 0.0f);
        // BandLimited suppresses noise here, isolating solid below the opening.
        // BoxFiltered intentionally retains independent interior noise cavities.
        EXPECT_LT(world.SamplePristineFarDensity(below, height, spacing, FarCaveMode::BandLimited),
                  0.0f);
    }
}

// Lane A #176, 9b04e1a6:test/common/FarVolume_test.cpp:535.
TEST(FarPristineField, PristineSamplerRejectsInvalidInputsAndHonorsDisabledCaves) {
    auto params = Caverns();
    params.caves_enabled = false;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    for (const std::uint32_t spacing : {4u, 8u, 16u, 32u, 64u})
        for (const auto mode : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
            SCOPED_TRACE(::testing::Message()
                         << "spacing=" << spacing << " mode=" << static_cast<int>(mode));
            // Analytic terrain subtraction, independent of the cave sampler.
            EXPECT_EQ(
                world.SamplePristineFarDensity(Vec3(-32.0f, -50.0f, -64.0f), 10.0f, spacing, mode),
                -60.0f);
            EXPECT_THROW(world.SamplePristineFarDensity(
                             Vec3(std::numeric_limits<float>::max()), 0.0f, spacing, mode),
                         std::invalid_argument);
            EXPECT_THROW(world.SamplePristineFarDensity(
                             Vec3(0.0f), std::numeric_limits<float>::quiet_NaN(), spacing, mode),
                         std::invalid_argument);
        }
    EXPECT_THROW(world.SamplePristineFarDensity(Vec3(0.0f), 0.0f, 12, FarCaveMode::BandLimited),
                 std::invalid_argument);
    EXPECT_THROW(
        world.SamplePristineFarDensity(Vec3(0.0f), 0.0f, 16, static_cast<FarCaveMode>(255)),
        std::invalid_argument);
}
} // namespace
