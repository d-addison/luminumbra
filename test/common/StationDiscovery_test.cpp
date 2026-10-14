#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "luminumbra_common/world/StationDiscovery.h"

namespace {
using namespace Luminumbra;
using namespace Luminumbra::world;
using WorldSystem = Systems::SHIELD_WorldSystem;

StationQuery Query(StationKind kind = StationKind::Mouth) {
    StationQuery query;
    query.kind = kind;
    query.origin_mm = {-4000, -4000};
    query.radius_m = 16;
    query.y_range_m = {0, 16};
    query.view_distance_m = 12;
    query.limit = 64;
    return query;
}

// One lattice anchor at (-4,0,-4), a transverse roof and two piers.
// A straight X tunnel reaches two open exterior columns, all by construction.
StationDiscoverySource Portal(bool two_sides = true, bool roof = true, bool supports = true) {
    StationDiscoverySource source;
    source.cell_size_m = 16;
    source.max_radius_m = 3;
    source.cell = [](int x, int z) {
        return x == -1 && z == -1 ? WorldSystem::SurfaceBreakInfo{true, -4, -4, 3, 4, false}
                                  : WorldSystem::SurfaceBreakInfo{};
    };
    source.height = [](float, float) {
        return 0.0f;
    };
    source.density = [=](const Vec3& point) {
        const float x = point.x + 4, z = point.z + 4;
        if (!two_sides && x < -2) {
            return -1.0f;
        }
        if (roof && std::abs(x) < 2 && std::abs(z) <= 4 && point.y >= 3 && point.y <= 5) {
            return -1.0f;
        }
        if (supports && std::abs(x) < 2 && std::abs(z) >= 3 && std::abs(z) <= 5 && point.y <= 5) {
            return -1.0f;
        }
        return 1.0f;
    };
    return source;
}

TEST(StationDiscovery, SampledMouthAndArchPositiveControls) {
    for (auto kind : {StationKind::Mouth, StationKind::Arch}) {
        const auto result = DiscoverStations(Portal(), Query(kind));
        ASSERT_TRUE(result.ok) << result.error;
        ASSERT_EQ(result.candidates.size(), 1u);
        const auto& candidate = result.candidates.front();
        EXPECT_EQ(candidate.anchor_mm, (std::array<std::int64_t, 3>{-4000, 0, -4000}));
        EXPECT_EQ(candidate.cell_x, -1);
        EXPECT_EQ(candidate.cell_z, -1);
    }
}

TEST(StationDiscovery, OneSidedMouthIsNotAnArch) {
    const auto mouth = DiscoverStations(Portal(false), Query());
    ASSERT_TRUE(mouth.ok) << mouth.error;
    ASSERT_EQ(mouth.candidates.size(), 1u);
    const auto arch = DiscoverStations(Portal(false), Query(StationKind::Arch));
    ASSERT_TRUE(arch.ok) << arch.error;
    EXPECT_TRUE(arch.candidates.empty());
}

TEST(StationDiscovery, ArchRequiresRoofAndLateralSupports) {
    for (const auto& source : {Portal(true, false), Portal(true, true, false)}) {
        const auto result = DiscoverStations(source, Query(StationKind::Arch));
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_TRUE(result.candidates.empty());
    }
    const auto open = DiscoverStations(Portal(true, false, false), Query());
    ASSERT_TRUE(open.ok);
    EXPECT_TRUE(open.candidates.empty());
}

TEST(StationDiscovery, MouthRequiresConnectedExteriorAir) {
    auto source = Portal();
    const auto density = source.density;
    source.density = [density](const Vec3& point) {
        if (point.y < 3 && std::abs(point.x + 4) >= 3) {
            return -1.0f;
        }
        return density(point);
    };
    const auto result = DiscoverStations(source, Query());
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.candidates.empty());
}

TEST(StationDiscovery, DistanceThenSignedCellsThenLocalIndexBeforeLimit) {
    auto query = Query();
    query.origin_mm = {0, 0};
    query.limit = 4;
    std::vector<StationCandidate> candidates(5);
    candidates[0].anchor_mm = {8000, 0, 0};
    candidates[1].anchor_mm = {-4000, 0, 0};
    candidates[1].cell_z = 1;
    candidates[2].anchor_mm = {4000, 0, 0};
    candidates[2].cell_z = -1;
    candidates[2].cell_x = -2;
    candidates[2].local_index = 9;
    candidates[3] = candidates[2];
    candidates[3].cell_x = -3;
    candidates[4] = candidates[3];
    candidates[4].local_index = 1;
    RankStationCandidates(candidates, query);
    ASSERT_EQ(candidates.size(), 4u);
    EXPECT_EQ(candidates[0].local_index, 1u);
    EXPECT_EQ(candidates[1].cell_x, -3);
    EXPECT_EQ(candidates[2].cell_x, -2);
    EXPECT_EQ(candidates[3].cell_z, 1);
}

TEST(StationDiscovery, EyeAndSightlineClearanceAndDirectionConventions) {
    auto query = Query();
    const auto positive_x = DiscoverStations(Portal(), query);
    ASSERT_TRUE(positive_x.ok);
    ASSERT_EQ(positive_x.candidates.size(), 1u);
    EXPECT_DOUBLE_EQ(positive_x.candidates[0].pose.x, -16);
    EXPECT_DOUBLE_EQ(positive_x.candidates[0].pose.y, 1.8);
    EXPECT_FLOAT_EQ(positive_x.candidates[0].pose.yaw, 0);
    EXPECT_LT(positive_x.candidates[0].pose.pitch, 0);

    auto source = Portal(true, true, false);
    const auto density = source.density;
    // Block the first direction halfway to the anchor, leaving the +Z view clear.
    source.density = [density](const Vec3& point) {
        return std::abs(point.x + 10) < 1 && std::abs(point.z + 4) < 1 ? -1.0f : density(point);
    };
    const auto positive_z = DiscoverStations(source, query);
    ASSERT_TRUE(positive_z.ok);
    ASSERT_EQ(positive_z.candidates.size(), 1u);
    EXPECT_FLOAT_EQ(positive_z.candidates[0].pose.yaw, 90);

    source = Portal(true, true, false);
    source.density = [density](const Vec3& point) {
        // Block each eye footprint while leaving the portal stencil intact.
        return std::abs(point.x + 4) > 10 || std::abs(point.z + 4) > 10 ? -1.0f : density(point);
    };
    const auto blocked = DiscoverStations(source, query);
    ASSERT_TRUE(blocked.ok);
    EXPECT_TRUE(blocked.candidates.empty());
}

TEST(StationDiscovery, NearestMouthWinsOverLargerFartherFootprint) {
    auto source = Portal();
    source.max_radius_m = 7;
    source.cell = [](int x, int z) {
        if (z == -1 && x == -1) {
            return WorldSystem::SurfaceBreakInfo{true, -4, -4, 3, 4, false};
        }
        if (z == -1 && x == 0) {
            return WorldSystem::SurfaceBreakInfo{true, 12, -4, 7, 4, false};
        }
        return WorldSystem::SurfaceBreakInfo{};
    };
    const auto density = source.density;
    source.density = [density](const Vec3& point) {
        return point.x > 4 ? density(Vec3(point.x - 16, point.y, point.z)) : density(point);
    };
    auto query = Query();
    query.radius_m = 32;
    query.limit = 1;
    const auto result = DiscoverStations(source, query);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].anchor_mm[0], -4000);
    EXPECT_EQ(result.candidates[0].cell_x, -1);
}

TEST(StationDiscovery, CardinalYawWrapAndOrderedDirections) {
    constexpr std::array<float, 4> yaws{{0, 90, -180, -90}};
    constexpr std::array<std::array<float, 2>, 4> eyes{{{-16, -4}, {-4, -16}, {8, -4}, {-4, 8}}};
    for (std::size_t direction = 0; direction < eyes.size(); ++direction) {
        auto source = Portal(true, true, false);
        const auto density = source.density;
        source.density = [density, direction, eyes](const Vec3& point) {
            for (std::size_t blocked = 0; blocked < direction; ++blocked) {
                if (std::abs(point.x - eyes[blocked][0]) < 1 &&
                    std::abs(point.z - eyes[blocked][1]) < 1) {
                    return -1.0f;
                }
            }
            return density(point);
        };
        const auto result = DiscoverStations(source, Query());
        ASSERT_TRUE(result.ok) << result.error;
        ASSERT_EQ(result.candidates.size(), 1u);
        EXPECT_NEAR(result.candidates[0].pose.yaw, yaws[direction], 0.001f);
        EXPECT_LT(result.candidates[0].pose.pitch, 0);
    }
}

TEST(StationDiscovery, EmptyAndDisabledCataloguesAreSuccessful) {
    auto source = Portal();
    source.cell = [](int, int) {
        return WorldSystem::SurfaceBreakInfo{};
    };
    const auto empty = DiscoverStations(source, Query());
    ASSERT_TRUE(empty.ok);
    EXPECT_TRUE(empty.candidates.empty());
    EXPECT_GT(empty.enumerated_cells, 0u);
    source.cell_size_m = 0;
    const auto disabled = DiscoverStations(source, Query());
    ASSERT_TRUE(disabled.ok);
    EXPECT_EQ(disabled.enumerated_cells, 0u);
    EXPECT_EQ(disabled.density_samples, 0u);
}

TEST(StationDiscovery, ControlsUseEyeAndElevatedConventionsAndDistinctIds) {
    auto source = Portal();
    source.density = [](const Vec3&) {
        return 1.0f;
    };
    auto query = Query(StationKind::Coverage);
    query.y_range_m = {0, 100};
    const auto result = DiscoverStations(source, query);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.candidates.size(), 5u);
    EXPECT_EQ(result.enumerated_cells, 0u);
    EXPECT_TRUE(result.candidates[0].id.starts_with("coverage-control-"));
    EXPECT_DOUBLE_EQ(result.candidates[0].pose.y, 1.8);
    EXPECT_FLOAT_EQ(result.candidates[2].pose.yaw, -180);
    EXPECT_DOUBLE_EQ(result.candidates[4].pose.y, 80);
    EXPECT_FLOAT_EQ(result.candidates[4].pose.yaw, 45);
    EXPECT_FLOAT_EQ(result.candidates[4].pose.pitch, -20);
    query.kind = StationKind::Horizon;
    EXPECT_EQ(DiscoverStations(source, query).candidates.size(), 4u);
}

TEST(StationDiscovery, BoundsAndCellBudgetRefuseWithoutPartialResults) {
    auto query = Query();
    query.y_range_m = {16, 0};
    EXPECT_FALSE(DiscoverStations(Portal(), query).ok);
    query.y_range_m = {0, 513};
    EXPECT_FALSE(DiscoverStations(Portal(), query).ok);
    query = Query();
    query.origin_mm[0] = kStationHorizontalBoundMm;
    EXPECT_FALSE(DiscoverStations(Portal(), query).ok);
    query = Query();
    query.radius_m = 16384;
    auto source = Portal();
    source.cell_size_m = 1;
    const auto budget = DiscoverStations(source, query);
    EXPECT_FALSE(budget.ok);
    EXPECT_TRUE(budget.candidates.empty());
    EXPECT_EQ(budget.enumerated_cells, 0u);
}

TEST(StationDiscovery, DensityBudgetCountsWorkAndDiscardsEarlierCandidates) {
    auto source = Portal();
    source.cell_size_m = 1;
    source.max_radius_m = 3;
    source.cell = [](int, int) {
        return WorldSystem::SurfaceBreakInfo{true, -4, -4, 3, 4, false};
    };
    auto query = Query();
    query.radius_m = 250;
    query.y_range_m = {0, 512};
    const auto result = DiscoverStations(source, query);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "density sample budget exhausted");
    EXPECT_EQ(result.density_samples, kStationSampleBudget);
    EXPECT_TRUE(result.candidates.empty());
}

TEST(StationDiscovery, NonfiniteSamplesRefuse) {
    auto source = Portal();
    source.density = [](const Vec3&) {
        return std::numeric_limits<float>::quiet_NaN();
    };
    const auto result = DiscoverStations(source, Query());
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.candidates.empty());
}

TEST(StationDiscovery, SharedPlacementParityWithLargestQuery) {
    Systems::TerrainGenParams params;
    params.surface_breaks_enabled = true;
    params.surface_break_density = 1;
    params.feature_cell_size = 32;
    params.max_feature_radius = 12;
    WorldSystem world(nullptr, nullptr, params, 17);
    WorldSystem::SurfaceBreakInfo expected;
    constexpr float radius = 64;
    for (int z = -3; z <= 3; ++z) {
        for (int x = -3; x <= 3; ++x) {
            const auto feature = world.SurfaceBreakInCell(x, z);
            if (feature.found && feature.x * feature.x + feature.z * feature.z <= radius * radius &&
                feature.radius > expected.radius) {
                expected = feature;
            }
        }
    }
    const auto largest = world.FindLargestSurfaceBreak(0, 0, radius);
    ASSERT_EQ(largest.found, expected.found);
    EXPECT_FLOAT_EQ(largest.x, expected.x);
    EXPECT_FLOAT_EQ(largest.z, expected.z);
    EXPECT_FLOAT_EQ(largest.radius, expected.radius);
    EXPECT_FLOAT_EQ(largest.depth, expected.depth);
    EXPECT_EQ(largest.shaft, expected.shaft);
    params.surface_breaks_enabled = false;
    WorldSystem disabled(nullptr, nullptr, params, 17);
    EXPECT_FALSE(disabled.SurfaceBreakInCell(-1, -1).found);
    EXPECT_EQ(disabled.SurfaceBreakCellSize(), 0);
}
} // namespace
