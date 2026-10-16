#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <string>

#include "../fixtures/TerrainFixtureParams.h"
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

// Translated copies of the portal, spaced 16 m apart on X and Z. Exactly the
// anchors congruent to base on X/Z, at base Y, pass both portal and pose checks.
StationDiscoverySource PortalGrid(const WorldSystem::SurfaceBreakInfo& feature,
                                  const std::array<int, 3>& base) {
    auto source = Portal();
    source.cell_size_m = 65536;
    source.max_radius_m = 128;
    source.cell = [feature](int x, int z) {
        return x == 0 && z == 0 ? feature : WorldSystem::SurfaceBreakInfo{};
    };
    source.height = [base](float, float) {
        return static_cast<float>(base[1]);
    };
    source.density = [base](const Vec3& point) {
        const float x = std::remainder(point.x - static_cast<float>(base[0]), 16.0f);
        const float y = point.y - static_cast<float>(base[1]);
        const float z = std::remainder(point.z - static_cast<float>(base[2]), 16.0f);
        const bool roof = std::abs(x) < 2 && std::abs(z) <= 4 && y >= 3 && y <= 5;
        const bool support = std::abs(x) < 2 && std::abs(z) >= 3 && std::abs(z) <= 5 && y <= 5;
        return roof || support ? -1.0f : 1.0f;
    };
    return source;
}

// Independent full Z/X/Y traversal with the original float box and footprint
// arithmetic. The known grid topology supplies the portal/pose acceptance rule;
// no clipped discovery call is used to decide which anchors should survive.
std::vector<StationCandidate> FullScanPortalGrid(const WorldSystem::SurfaceBreakInfo& feature,
                                                 const StationQuery& query,
                                                 const std::array<int, 3>& base) {
    const int x0 = static_cast<int>(std::floor((feature.x - feature.radius) / 4));
    const int x1 = static_cast<int>(std::floor((feature.x + feature.radius) / 4));
    const int z0 = static_cast<int>(std::floor((feature.z - feature.radius) / 4));
    const int z1 = static_cast<int>(std::floor((feature.z + feature.radius) / 4));
    const int y0 = static_cast<int>(std::floor(static_cast<double>(query.y_range_m[0]) / 4));
    const int y1 = static_cast<int>(std::floor(static_cast<double>(query.y_range_m[1]) / 4));
    const auto radius_mm = static_cast<std::int64_t>(query.radius_m) * 1000;
    std::vector<StationCandidate> candidates;
    std::uint64_t index = 0;
    for (int gz = z0; gz <= z1; ++gz) {
        for (int gx = x0; gx <= x1; ++gx) {
            for (int gy = y0; gy <= y1; ++gy, ++index) {
                const int x = gx * 4, y = gy * 4, z = gz * 4;
                if (y < query.y_range_m[0] || y + 12 > query.y_range_m[1]) {
                    continue;
                }
                const double fx = static_cast<double>(x) - feature.x;
                const double fz = static_cast<double>(z) - feature.z;
                if (fx * fx + fz * fz > feature.radius * feature.radius) {
                    continue;
                }
                const auto dx = static_cast<std::int64_t>(x) * 1000 - query.origin_mm[0];
                const auto dz = static_cast<std::int64_t>(z) * 1000 - query.origin_mm[1];
                if (std::abs(dx) > radius_mm || std::abs(dz) > radius_mm ||
                    dx * dx + dz * dz > radius_mm * radius_mm) {
                    continue;
                }
                if (y != base[1] || (x - base[0]) % 16 != 0 || (z - base[2]) % 16 != 0) {
                    continue;
                }
                StationCandidate candidate;
                candidate.anchor_mm = {static_cast<std::int64_t>(x) * 1000,
                                       static_cast<std::int64_t>(y) * 1000,
                                       static_cast<std::int64_t>(z) * 1000};
                candidate.local_index = index;
                candidate.id = std::string(query.kind == StationKind::Arch ? "arch" : "mouth") +
                               "-z-p0-x-p0-i-" + std::to_string(index);
                candidates.push_back(candidate);
            }
        }
    }
    RankStationCandidates(candidates, query);
    return candidates;
}

void ExpectSameStations(const std::vector<StationCandidate>& actual,
                        const std::vector<StationCandidate>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].anchor_mm, expected[i].anchor_mm);
        EXPECT_EQ(actual[i].id, expected[i].id);
        EXPECT_EQ(actual[i].cell_x, expected[i].cell_x);
        EXPECT_EQ(actual[i].cell_z, expected[i].cell_z);
        EXPECT_EQ(actual[i].local_index, expected[i].local_index);
    }
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
        EXPECT_EQ(candidate.local_index, 15u); // full Z/X/Y box, including skipped anchors
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

TEST(StationDiscovery, ClippingPreservesBinary32FeatureBoxIndices) {
    auto source = Portal();
    source.cell = [](int x, int z) {
        // Binary32 subtraction rounds the lower X bound to exactly 4 m.
        return x == 0 && z == 0
                   ? WorldSystem::SurfaceBreakInfo{true, 5, 4, 1.00000011920928955078125f, 4, false}
                   : WorldSystem::SurfaceBreakInfo{};
    };
    const auto density = source.density;
    source.density = [density](const Vec3& point) {
        return density(Vec3(point.x - 8, point.y, point.z - 8));
    };
    auto query = Query();
    query.origin_mm = {4000, 4000};
    query.radius_m = 1;
    const auto result = DiscoverStations(source, query);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates.front().local_index, 5u);
}

TEST(StationDiscovery, ClippingPreservesBinary32RadiusBoundaryStations) {
    for (const bool transpose : {false, true}) {
        WorldSystem::SurfaceBreakInfo feature{
            true, 1.5530474185943604f, 0, 41.55304718017578f, 4, false};
        std::array<int, 3> base{-40, 0, 0};
        if (transpose) {
            std::swap(feature.x, feature.z);
            std::swap(base[0], base[2]);
        }
        for (auto kind : {StationKind::Mouth, StationKind::Arch}) {
            SCOPED_TRACE(transpose);
            SCOPED_TRACE(static_cast<int>(kind));
            auto query = Query(kind);
            query.origin_mm = {base[0] * 1000, base[2] * 1000};
            query.radius_m = 1;
            const auto source = PortalGrid(feature, base);
            const auto expected = FullScanPortalGrid(feature, query, base);
            ASSERT_EQ(expected.size(), 1u);
            EXPECT_EQ(expected.front().anchor_mm,
                      (std::array<std::int64_t, 3>{base[0] * 1000, 0, base[2] * 1000}));
            EXPECT_EQ(expected.front().local_index, transpose ? 55u : 1155u);
            const auto result = DiscoverStations(source, query);
            ASSERT_TRUE(result.ok) << result.error;
            ExpectSameStations(result.candidates, expected);
        }
    }
}

TEST(StationDiscovery, RandomizedClippingMatchesFullScanStationsAndIds) {
    std::mt19937 random(0x51a710u);
    // Use engine integers directly so the cases do not depend on a library's
    // uniform-distribution implementation. Keep full scans small and bounded.
    const auto draw = [&random](int count) {
        return static_cast<int>(random() % static_cast<std::uint32_t>(count));
    };
    std::size_t positive_cases = 0, multiple_station_cases = 0;
    for (int iteration = 0; iteration < 256; ++iteration) {
        const std::array<int, 3> base{4 * (draw(33) - 16), 4 * (draw(9) - 4), 4 * (draw(33) - 16)};
        WorldSystem::SurfaceBreakInfo feature{
            true,
            static_cast<float>(base[0]) + static_cast<float>(draw(98305) - 49152) / 1024.0f,
            static_cast<float>(base[2]) + static_cast<float>(draw(98305) - 49152) / 1024.0f,
            static_cast<float>(1 + draw(96)),
            4,
            false};
        if (iteration % 2 == 0) {
            // Straddle the binary32 radius comparison at a known portal anchor.
            const double dx = static_cast<double>(base[0]) - feature.x;
            const double dz = static_cast<double>(base[2]) - feature.z;
            feature.radius = static_cast<float>(std::sqrt(dx * dx + dz * dz));
            feature.radius = std::nextafter(feature.radius, iteration % 4 == 0 ? 128.0f : 0.0f);
        }
        auto query = Query();
        query.origin_mm = {static_cast<std::int64_t>(base[0]) * 1000 + draw(48001) - 24000,
                           static_cast<std::int64_t>(base[2]) * 1000 + draw(48001) - 24000};
        query.radius_m = 1 + draw(64);
        query.y_range_m = {base[1] - draw(8), base[1] + 12 + draw(9)};
        query.view_distance_m = 0;
        query.limit = 1 + draw(64);
        const auto source = PortalGrid(feature, base);
        for (auto kind : {StationKind::Mouth, StationKind::Arch}) {
            SCOPED_TRACE(iteration);
            SCOPED_TRACE(static_cast<int>(kind));
            query.kind = kind;
            const auto expected = FullScanPortalGrid(feature, query, base);
            positive_cases += !expected.empty();
            multiple_station_cases += expected.size() > 1;
            const auto result = DiscoverStations(source, query);
            ASSERT_TRUE(result.ok) << result.error;
            ExpectSameStations(result.candidates, expected);
        }
    }
    EXPECT_GT(positive_cases, 0u);
    EXPECT_GT(multiple_station_cases, 0u);
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

TEST(StationDiscovery, SeededWorldControlsMatchFlatTerrainGolden) {
    auto params = Test::SyntheticTerrainParams();
    params.base_amplitude = 0;
    params.peaks_amplitude = 0;
    params.continental_spline = {{-1.0f, 0.0f}, {1.0f, 0.0f}};
    params.height_offset = 4.25f;
    params.caves_enabled = false;
    // The existing synthetic fixture and null services require no GPU or assets.
    const WorldSystem world(nullptr, nullptr, params, 17);
    constexpr std::array<StationPose, 5> expected{{{-4.25, 6.05, 8.75, 0, 0},
                                                   {-4.25, 6.05, 8.75, 90, 0},
                                                   {-4.25, 6.05, 8.75, -180, 0},
                                                   {-4.25, 6.05, 8.75, -90, 0},
                                                   {-4.25, 84.25, 8.75, 45, -20}}};
    for (const auto kind : {StationKind::Horizon, StationKind::Coverage}) {
        auto query = Query(kind);
        query.origin_mm = {-4250, 8750};
        query.y_range_m = {0, 100};
        const auto result = DiscoverStations(world, query);
        ASSERT_TRUE(result.ok) << result.error;
        ASSERT_EQ(result.candidates.size(), kind == StationKind::Horizon ? 4u : 5u);
        for (std::size_t i = 0; i < result.candidates.size(); ++i) {
            SCOPED_TRACE(i);
            const auto& actual = result.candidates[i];
            const std::string prefix = kind == StationKind::Horizon ? "horizon" : "coverage";
            EXPECT_EQ(actual.id, prefix + "-control-z-p0-x-p0-i-" + std::to_string(i));
            EXPECT_EQ(actual.anchor_mm, (std::array<std::int64_t, 3>{-4250, 4250, 8750}));
            EXPECT_EQ(actual.cell_x, 0);
            EXPECT_EQ(actual.cell_z, 0);
            EXPECT_EQ(actual.local_index, i);
            EXPECT_DOUBLE_EQ(actual.pose.x, expected[i].x);
            EXPECT_DOUBLE_EQ(actual.pose.y, expected[i].y);
            EXPECT_DOUBLE_EQ(actual.pose.z, expected[i].z);
            EXPECT_FLOAT_EQ(actual.pose.yaw, expected[i].yaw);
            EXPECT_FLOAT_EQ(actual.pose.pitch, expected[i].pitch);
        }
    }
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
    auto query = Query();
    query.radius_m = 250;
    query.y_range_m = {0, 16};
    query.view_distance_m = 256;
    // Each cell visits eight anchors: six miss the footprint, one passes the
    // portal and pose (7 + 7 + 65 samples), and one fails in solid (1 sample).
    // Prove that the topology yields a candidate before repeating it to refusal.
    const auto single = DiscoverStations(source, query);
    ASSERT_TRUE(single.ok) << single.error;
    ASSERT_EQ(single.candidates.size(), 1u);
    EXPECT_EQ(single.lattice_visits, 8u);
    EXPECT_EQ(single.density_samples, 80u);
    source.cell = [](int, int) {
        return WorldSystem::SurfaceBreakInfo{true, -4, -4, 3, 4, false};
    };
    const auto result = DiscoverStations(source, query);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "density sample budget exhausted");
    // 209715 complete cells, then 16 samples in the seventh visit of the next.
    EXPECT_EQ(result.enumerated_cells, 209716u);
    EXPECT_EQ(result.lattice_visits, 1677727u);
    EXPECT_EQ(result.density_samples, kStationSampleBudget);
    EXPECT_TRUE(result.candidates.empty());
}

TEST(StationDiscovery, LatticeBudgetCountsWorkAndDiscardsEarlierCandidates) {
    auto source = Portal();
    source.cell_size_m = 1;
    source.max_radius_m = 3;
    auto query = Query();
    query.radius_m = 250;
    query.y_range_m = {0, 512};
    // Each cell visits 4 * 126 anchors. Only the final X/Z column samples:
    // 18 samples for the accepted portal/pose, 1 in solid, then 124 * 2 in air.
    const auto single = DiscoverStations(source, query);
    ASSERT_TRUE(single.ok) << single.error;
    ASSERT_EQ(single.candidates.size(), 1u);
    EXPECT_EQ(single.lattice_visits, 504u);
    EXPECT_EQ(single.density_samples, 267u);
    source.cell = [](int, int) {
        return WorldSystem::SurfaceBreakInfo{true, -4, -4, 3, 4, false};
    };
    const auto result = DiscoverStations(source, query);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "lattice visit budget exhausted");
    // 33288 complete cells, then 64 footprint misses before the next refusal.
    EXPECT_EQ(result.enumerated_cells, 33289u);
    EXPECT_EQ(result.lattice_visits, kStationLatticeVisitBudget);
    EXPECT_EQ(result.density_samples, 8887896u);
    EXPECT_TRUE(result.candidates.empty());
}

TEST(StationDiscovery, ShortOrUnalignedPortalSpanDoesNoCatalogueOrLatticeWork) {
    for (auto kind : {StationKind::Mouth, StationKind::Arch}) {
        for (const auto span :
             {std::array<int, 2>{0, 0}, std::array<int, 2>{-11, 0}, std::array<int, 2>{1, 13}}) {
            auto source = Portal();
            source.cell_size_m = 65536;
            source.max_radius_m = 16384;
            std::uint64_t cell_calls = 0, density_calls = 0;
            source.cell = [&cell_calls](int, int) {
                ++cell_calls;
                return WorldSystem::SurfaceBreakInfo{true, 0, 0, 16384, 4, false};
            };
            source.density = [&density_calls](const Vec3&) {
                ++density_calls;
                return -1.0f;
            };
            auto query = Query(kind);
            query.radius_m = 16384;
            query.y_range_m = span;
            const auto result = DiscoverStations(source, query);
            ASSERT_TRUE(result.ok) << result.error;
            EXPECT_TRUE(result.candidates.empty());
            EXPECT_EQ(result.enumerated_cells, 0u);
            EXPECT_EQ(result.lattice_visits, 0u);
            EXPECT_EQ(result.density_samples, 0u);
            EXPECT_EQ(cell_calls, 0u);
            EXPECT_EQ(density_calls, 0u);
        }
    }
}

TEST(StationDiscovery, DiskIntersectionBoundsLatticeVisitsAndIncludesBoundaryAnchors) {
    for (auto kind : {StationKind::Mouth, StationKind::Arch}) {
        auto source = Portal();
        source.cell_size_m = 65536;
        source.max_radius_m = 4096;
        source.cell = [](int x, int z) {
            return x == 0 && z == 0 ? WorldSystem::SurfaceBreakInfo{true, 0, 0, 4096, 4, false}
                                    : WorldSystem::SurfaceBreakInfo{};
        };
        source.density = [](const Vec3&) {
            return -1.0f;
        };
        auto query = Query(kind);
        query.origin_mm = {0, 0};
        query.y_range_m = {0, 12};
        for (const auto radius : {1, 4}) {
            query.radius_m = radius;
            const auto result = DiscoverStations(source, query);
            ASSERT_TRUE(result.ok) << result.error;
            EXPECT_EQ(result.lattice_visits, radius == 1 ? 1u : 5u);
            EXPECT_EQ(result.density_samples, result.lattice_visits);
            EXPECT_TRUE(result.candidates.empty());
        }
        query.origin_mm = {-3999, 4001};
        const auto shifted = DiscoverStations(source, query);
        ASSERT_TRUE(shifted.ok) << shifted.error;
        EXPECT_EQ(shifted.lattice_visits, 3u);
        EXPECT_EQ(shifted.density_samples, 3u);
    }
}

TEST(StationDiscovery, WideDiskRefusesAtVisitBudgetEvenWithoutDensitySamples) {
    for (auto kind : {StationKind::Mouth, StationKind::Arch}) {
        auto source = Portal();
        source.cell_size_m = 65536;
        source.max_radius_m = 16384;
        // The bounding boxes overlap the query disk, but the feature disk does
        // not. All visited anchors fail before density sampling.
        source.cell = [](int x, int z) {
            return x == 0 && z == 0
                       ? WorldSystem::SurfaceBreakInfo{true, 25000, 25000, 16384, 4, false}
                       : WorldSystem::SurfaceBreakInfo{};
        };
        auto query = Query(kind);
        query.origin_mm = {0, 0};
        query.radius_m = 16384;
        query.y_range_m = {0, 512};
        const auto result = DiscoverStations(source, query);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error, "lattice visit budget exhausted");
        EXPECT_EQ(result.enumerated_cells, 4u);
        EXPECT_EQ(result.lattice_visits, kStationLatticeVisitBudget);
        EXPECT_EQ(result.density_samples, 0u);
        EXPECT_TRUE(result.candidates.empty());
    }
}

TEST(StationDiscovery, UnrepresentableFullLatticeIndexRefusesBeforeVisits) {
    auto source = Portal();
    source.cell_size_m = 16000000000.0f;
    source.max_radius_m = 8000000000.0f;
    source.cell = [](int, int) {
        return WorldSystem::SurfaceBreakInfo{true, 0, 0, 8000000000.0f, 4, false};
    };
    const auto result = DiscoverStations(source, Query());
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "local lattice index outside integer domain");
    EXPECT_EQ(result.lattice_visits, 0u);
    EXPECT_EQ(result.density_samples, 0u);
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
