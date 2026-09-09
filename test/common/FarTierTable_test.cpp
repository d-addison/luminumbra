#include "luminumbra_common/world/FarTierTable.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>

namespace {

using namespace Luminumbra::World;

TEST(FarTierTable, PublishedValues) {
    constexpr std::array<FarTierDimensions, 5> expected = {{
        {4, 16, 512, 1024},
        {8, 32, 1024, 2048},
        {16, 64, 2048, 4096},
        {32, 128, 4096, 8192},
        {64, 256, 8192, 16384},
    }};

    ASSERT_EQ(kFarTierCount, expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        SCOPED_TRACE(index + 1);
        const auto actual = FarTierAt(index + 1);
        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(actual->sample_spacing_meters, expected[index].sample_spacing_meters);
        EXPECT_EQ(actual->brick_edge_meters, expected[index].brick_edge_meters);
        EXPECT_EQ(actual->tile_edge_meters, expected[index].tile_edge_meters);
        EXPECT_EQ(actual->outer_radius_meters, expected[index].outer_radius_meters);
    }
    EXPECT_EQ(kFarOuterRadiusMeters, 16384u);
}

TEST(FarTierTable, NestedDimensions) {
    for (std::size_t index = 0; index < kFarTierCount; ++index) {
        SCOPED_TRACE(index + 1);
        const auto& tier = kFarTierTable[index];
        ASSERT_GT(tier.brick_edge_meters, 0u);
        EXPECT_EQ(tier.tile_edge_meters % tier.brick_edge_meters, 0u);
        EXPECT_EQ(tier.brick_edge_meters, 4u * tier.sample_spacing_meters);
        if (index > 0) {
            const auto& previous = kFarTierTable[index - 1];
            EXPECT_EQ(tier.sample_spacing_meters, 2u * previous.sample_spacing_meters);
            EXPECT_EQ(tier.brick_edge_meters, 2u * previous.brick_edge_meters);
            EXPECT_EQ(tier.tile_edge_meters, 2u * previous.tile_edge_meters);
            EXPECT_EQ(tier.outer_radius_meters, 2u * previous.outer_radius_meters);
        }
    }
}

TEST(FarTierTable, UniformAngularSampleDensity) {
    for (const auto& tier : kFarTierTable) {
        EXPECT_EQ(tier.sample_spacing_meters * 256u, tier.outer_radius_meters);
        EXPECT_EQ(static_cast<double>(tier.sample_spacing_meters) / tier.outer_radius_meters,
                  1.0 / 256.0);
    }
}

TEST(FarTierTable, HorizontalDistanceBoundaries) {
    static_assert(FarTierAt(1)->sample_spacing_meters == 4u);
    static_assert(FarTierForHorizontalDistance(1024.0) == 1u);
    static_assert(FarTierForHorizontalDistance(16384.0) == kFarTierCount);

    EXPECT_EQ(FarTierForHorizontalDistance(0.0), 1u);
    EXPECT_EQ(FarTierForHorizontalDistance(-0.0), 1u);
    EXPECT_EQ(FarTierForHorizontalDistance(std::nextafter(0.0, 1.0)), 1u);
    for (std::size_t index = 0; index < kFarTierCount; ++index) {
        SCOPED_TRACE(index + 1);
        const double radius = kFarTierTable[index].outer_radius_meters;
        EXPECT_EQ(FarTierForHorizontalDistance(std::nextafter(radius, 0.0)), index + 1);
        EXPECT_EQ(FarTierForHorizontalDistance(radius), index + 1);
        const auto beyond = FarTierForHorizontalDistance(
            std::nextafter(radius, std::numeric_limits<double>::infinity()));
        if (index + 1 < kFarTierCount) {
            EXPECT_EQ(beyond, index + 2);
        } else {
            EXPECT_EQ(beyond, std::nullopt);
        }
    }
}

TEST(FarTierTable, RejectsInvalidInputs) {
    EXPECT_EQ(FarTierAt(0), std::nullopt);
    EXPECT_EQ(FarTierAt(kFarTierCount + 1), std::nullopt);
    EXPECT_EQ(FarTierAt(std::numeric_limits<std::size_t>::max()), std::nullopt);
    EXPECT_EQ(FarTierForHorizontalDistance(std::nextafter(0.0, -1.0)), std::nullopt);
    EXPECT_EQ(FarTierForHorizontalDistance(-1.0), std::nullopt);
    EXPECT_EQ(FarTierForHorizontalDistance(kFarOuterRadiusMeters + 1.0), std::nullopt);
    EXPECT_EQ(FarTierForHorizontalDistance(std::numeric_limits<double>::infinity()), std::nullopt);
    EXPECT_EQ(FarTierForHorizontalDistance(-std::numeric_limits<double>::infinity()), std::nullopt);
    EXPECT_EQ(FarTierForHorizontalDistance(std::numeric_limits<double>::quiet_NaN()), std::nullopt);
}

} // namespace
