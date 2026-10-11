#include "luminumbra_common/world/FarWantedSet.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace {

using namespace Luminumbra::World;

FarTileKey Key(std::size_t tier, std::int64_t tx, std::int64_t tz) {
    return FarTileKey{static_cast<std::uint8_t>(tier), tx, tz};
}

bool Contains(const std::vector<FarTileKey>& keys, const FarTileKey& key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

FarResidentTile MakeResident(const FarTileKey& key,
                             FarTileState state,
                             std::uint64_t cpu_bytes = 0,
                             std::uint64_t gpu_bytes = 0,
                             bool current = true,
                             bool span_ok = true) {
    FarResidentTile entry;
    entry.key = key;
    entry.status.state = state;
    entry.status.current = current;
    entry.status.span_ok = span_ok;
    entry.cpu_bytes = cpu_bytes;
    entry.gpu_bytes = gpu_bytes;
    return entry;
}

std::vector<FarTileKey> BruteForceWantedSet(double camera_x, double camera_z) {
    std::vector<FarTileKey> expected;
    for (std::size_t tier = 1; tier <= kFarTierCount; ++tier) {
        const double edge = static_cast<double>(FarTileEdgeMeters(tier));
        const double radius = FarWantedRadiusMeters(tier);
        const auto x_lo = static_cast<std::int64_t>(std::floor((camera_x - radius) / edge)) - 3;
        const auto x_hi = static_cast<std::int64_t>(std::floor((camera_x + radius) / edge)) + 3;
        const auto z_lo = static_cast<std::int64_t>(std::floor((camera_z - radius) / edge)) - 3;
        const auto z_hi = static_cast<std::int64_t>(std::floor((camera_z + radius) / edge)) + 3;
        for (std::int64_t tz = z_lo; tz <= z_hi; ++tz) {
            for (std::int64_t tx = x_lo; tx <= x_hi; ++tx) {
                const double lo_x = static_cast<double>(tx) * edge;
                const double hi_x = lo_x + edge;
                const double lo_z = static_cast<double>(tz) * edge;
                const double hi_z = lo_z + edge;
                const double dx = std::max({lo_x - camera_x, 0.0, camera_x - hi_x});
                const double dz = std::max({lo_z - camera_z, 0.0, camera_z - hi_z});
                if (std::sqrt(dx * dx + dz * dz) <= radius) {
                    expected.push_back(Key(tier, tx, tz));
                }
            }
        }
    }
    std::sort(expected.begin(), expected.end());
    return expected;
}

std::vector<FarTileKey> FourChildren() {
    return {Key(1, 0, 0), Key(1, 1, 0), Key(1, 0, 1), Key(1, 1, 1)};
}

std::vector<FarResidentTile> ResidentFor(const std::vector<FarTileKey>& keys, FarTileState state) {
    std::vector<FarResidentTile> resident;
    for (const FarTileKey& key : keys) {
        resident.push_back(MakeResident(key, state));
    }
    return resident;
}

std::vector<FarTileKey> FinestWanted() {
    return {Key(1, 0, 0), Key(1, 1, 0), Key(1, 0, 1), Key(1, 1, 1), Key(2, 0, 0)};
}

std::vector<FarResidentTile> FinestResident(FarTileState parent_state) {
    return {
        MakeResident(Key(1, 0, 0), FarTileState::Ready, 100),
        MakeResident(Key(1, 1, 0), FarTileState::Ready, 100),
        MakeResident(Key(1, 0, 1), FarTileState::Ready, 100),
        MakeResident(Key(1, 1, 1), FarTileState::Ready, 100),
        MakeResident(Key(2, 0, 0), parent_state),
    };
}

FarEvictionPlan PlanWith(const std::vector<FarResidentTile>& resident,
                         const std::vector<FarTileKey>& wanted,
                         const FarTierBudgets& budgets) {
    return PlanFarEviction(0.0, 0.0, resident, wanted, budgets);
}

} // namespace

TEST(FarWantedSet, RadiusAndEdgePerTier) {
    const std::array<double, kFarTierCount> radii = {1040.0, 2080.0, 4160.0, 8320.0, 16384.0};
    const std::array<std::int64_t, kFarTierCount> edges = {512, 1024, 2048, 4096, 8192};
    for (std::size_t tier = 1; tier <= kFarTierCount; ++tier) {
        SCOPED_TRACE(tier);
        EXPECT_DOUBLE_EQ(FarWantedRadiusMeters(tier), radii[tier - 1]);
        EXPECT_EQ(FarTileEdgeMeters(tier), edges[tier - 1]);
    }
    EXPECT_EQ(FarWantedRadiusMeters(0), 0.0);
    EXPECT_EQ(FarWantedRadiusMeters(6), 0.0);
    EXPECT_EQ(FarTileEdgeMeters(0), 0);
    EXPECT_EQ(FarTileEdgeMeters(6), 0);
}

TEST(FarWantedSet, OriginTierOneHasTwentyFourTilesIncludingTangent) {
    std::vector<FarTileKey> out;
    ASSERT_EQ(ComputeFarWantedSet(0.0, 0.0, out), FarWantedError::None);

    const auto tier_one =
        std::count_if(out.begin(), out.end(), [](const FarTileKey& key) { return key.tier == 1; });
    EXPECT_EQ(tier_one, 24);

    EXPECT_TRUE(Contains(out, Key(1, -3, -1)));
    EXPECT_TRUE(Contains(out, Key(1, -3, 0)));
    EXPECT_TRUE(Contains(out, Key(1, -1, -3)));
    EXPECT_TRUE(Contains(out, Key(1, 0, -3)));

    EXPECT_TRUE(std::is_sorted(out.begin(), out.end()));
    EXPECT_EQ(std::adjacent_find(out.begin(),
                                 out.end(),
                                 [](const FarTileKey& a, const FarTileKey& b) { return !(a < b); }),
              out.end());
}

TEST(FarWantedSet, MatchesBruteForceAtSeveralCameras) {
    const std::array<std::pair<double, double>, 5> cameras = {{
        {0.0, 0.0},
        {100.5, -37.25},
        {-513.0, 2047.75},
        {8191.0, 8192.0},
        {123456.5, -98765.25},
    }};
    for (std::size_t index = 0; index < cameras.size(); ++index) {
        SCOPED_TRACE(index);
        const auto [camera_x, camera_z] = cameras[index];
        const std::vector<FarTileKey> expected = BruteForceWantedSet(camera_x, camera_z);
        std::vector<FarTileKey> actual;
        ASSERT_EQ(ComputeFarWantedSet(camera_x, camera_z, actual), FarWantedError::None);
        EXPECT_EQ(actual, expected);
    }
}

TEST(FarWantedSet, BoundaryIsInclusivePerTier) {
    for (std::size_t tier = 1; tier <= kFarTierCount; ++tier) {
        SCOPED_TRACE(tier);
        const double radius = FarWantedRadiusMeters(tier);
        const double edge = static_cast<double>(FarTileEdgeMeters(tier));
        const FarTileKey tile = Key(tier, 0, 0);

        std::vector<FarTileKey> out;
        ASSERT_EQ(ComputeFarWantedSet(-radius, edge / 2.0, out), FarWantedError::None);
        EXPECT_TRUE(Contains(out, tile));

        ASSERT_EQ(ComputeFarWantedSet(-radius - 0.25, edge / 2.0, out), FarWantedError::None);
        EXPECT_FALSE(Contains(out, tile));
    }
}

TEST(FarWantedSet, RefusesBadCameras) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<FarTileKey> out;

    out = {Key(1, 0, 0)};
    EXPECT_EQ(ComputeFarWantedSet(nan, 0.0, out), FarWantedError::NonFiniteCamera);
    EXPECT_TRUE(out.empty());

    out = {Key(1, 0, 0)};
    EXPECT_EQ(ComputeFarWantedSet(0.0, inf, out), FarWantedError::NonFiniteCamera);
    EXPECT_TRUE(out.empty());

    out = {Key(1, 0, 0)};
    EXPECT_EQ(ComputeFarWantedSet(8388608.0 + 1.0, 0.0, out), FarWantedError::CameraOutOfRange);
    EXPECT_TRUE(out.empty());

    out = {Key(1, 0, 0)};
    EXPECT_EQ(ComputeFarWantedSet(0.0, -(8388608.0 + 1.0), out), FarWantedError::CameraOutOfRange);
    EXPECT_TRUE(out.empty());

    EXPECT_EQ(ComputeFarWantedSet(8388608.0, 0.0, out), FarWantedError::None);
    EXPECT_EQ(ComputeFarWantedSet(-8388608.0, 0.0, out), FarWantedError::None);
    EXPECT_EQ(ComputeFarWantedSet(0.0, 8388608.0, out), FarWantedError::None);
    EXPECT_EQ(ComputeFarWantedSet(0.0, -8388608.0, out), FarWantedError::None);
}

TEST(FarWantedSet, ParentUsesFloorDivision) {
    EXPECT_EQ(FarTileParent(Key(1, -1, -2)), Key(2, -1, -1));
    EXPECT_EQ(FarTileParent(Key(1, 3, 2)), Key(2, 1, 1));
    EXPECT_EQ(FarTileParent(Key(1, -3, 0)), Key(2, -2, 0));
    EXPECT_EQ(FarTileParent(Key(5, 7, -9)), Key(5, 7, -9));
}

TEST(FarWantedSet, OutputDeterministicAcrossCalls) {
    std::vector<FarTileKey> first;
    std::vector<FarTileKey> second;
    ASSERT_EQ(ComputeFarWantedSet(-513.0, 2047.75, first), FarWantedError::None);
    ASSERT_EQ(ComputeFarWantedSet(-513.0, 2047.75, second), FarWantedError::None);
    EXPECT_EQ(first, second);
}

TEST(FarArrival, CoarseSurfaceMayYieldRequiresAllChildrenCovering) {
    const FarTileKey coarse = Key(2, 0, 0);
    const std::vector<FarTileKey> wanted = FourChildren();

    EXPECT_TRUE(CoarseSurfaceMayYield(coarse, wanted, ResidentFor(wanted, FarTileState::Uploaded)));
    EXPECT_TRUE(CoarseSurfaceMayYield(coarse, wanted, ResidentFor(wanted, FarTileState::Empty)));

    const std::array<FarTileState, 4> blocking = {
        FarTileState::Ready,
        FarTileState::Building,
        FarTileState::Rejected,
        FarTileState::Wanted,
    };
    for (const FarTileState state : blocking) {
        SCOPED_TRACE(static_cast<int>(state));
        std::vector<FarResidentTile> resident = ResidentFor(wanted, FarTileState::Uploaded);
        resident[2] = MakeResident(wanted[2], state);
        EXPECT_FALSE(CoarseSurfaceMayYield(coarse, wanted, resident));
    }

    {
        std::vector<FarResidentTile> resident = ResidentFor(wanted, FarTileState::Uploaded);
        resident[1].status.current = false;
        EXPECT_FALSE(CoarseSurfaceMayYield(coarse, wanted, resident));
    }
    {
        std::vector<FarResidentTile> resident = ResidentFor(wanted, FarTileState::Uploaded);
        resident[3].status.span_ok = false;
        EXPECT_FALSE(CoarseSurfaceMayYield(coarse, wanted, resident));
    }
    {
        std::vector<FarResidentTile> resident = ResidentFor(wanted, FarTileState::Uploaded);
        resident.pop_back();
        EXPECT_FALSE(CoarseSurfaceMayYield(coarse, wanted, resident));
    }
}

TEST(FarArrival, CoarseSurfaceMayYieldFalseForTierOneAndEmptyChildSet) {
    const std::vector<FarTileKey> tier_one_wanted = {Key(1, 0, 0)};
    const std::vector<FarResidentTile> covering = {
        MakeResident(Key(1, 0, 0), FarTileState::Uploaded)};
    EXPECT_FALSE(CoarseSurfaceMayYield(Key(1, 0, 0), tier_one_wanted, covering));

    const std::vector<FarTileKey> none;
    EXPECT_FALSE(CoarseSurfaceMayYield(Key(2, 0, 0), none, covering));
}

TEST(FarArrival, CoarseSurfaceMayYieldIgnoresUnwantedSiblings) {
    const std::vector<FarTileKey> wanted = {Key(1, 0, 0), Key(1, 1, 0)};
    const std::vector<FarResidentTile> resident = {
        MakeResident(Key(1, 0, 0), FarTileState::Uploaded),
        MakeResident(Key(1, 1, 0), FarTileState::Empty),
        MakeResident(Key(1, 0, 1), FarTileState::Ready),
    };
    EXPECT_TRUE(CoarseSurfaceMayYield(Key(2, 0, 0), wanted, resident));
}

TEST(FarArrival, IsFarTileCoveringTruthTable) {
    const std::array<FarTileState, 6> states = {
        FarTileState::Wanted,
        FarTileState::Building,
        FarTileState::Ready,
        FarTileState::Uploaded,
        FarTileState::Empty,
        FarTileState::Rejected,
    };
    for (const FarTileState state : states) {
        SCOPED_TRACE(static_cast<int>(state));
        const bool covering_state = state == FarTileState::Uploaded || state == FarTileState::Empty;
        EXPECT_EQ(IsFarTileCovering(FarTileStatus{state, true, true}), covering_state);
        EXPECT_FALSE(IsFarTileCovering(FarTileStatus{state, false, true}));
        EXPECT_FALSE(IsFarTileCovering(FarTileStatus{state, true, false}));
    }
}

TEST(FarEviction, UnwantedTilesEvictedFirstFarthestFirstAndNotRejected) {
    const std::vector<FarResidentTile> resident = {
        MakeResident(Key(1, 0, 0), FarTileState::Ready, 10),
        MakeResident(Key(1, 4, 0), FarTileState::Ready, 10),
        MakeResident(Key(1, 20, 0), FarTileState::Ready, 10),
    };
    const std::vector<FarTileKey> wanted;
    const FarEvictionPlan plan = PlanWith(resident, wanted, FarTierBudgets{});

    const std::vector<FarTileKey> expected = {Key(1, 20, 0), Key(1, 4, 0), Key(1, 0, 0)};
    EXPECT_EQ(plan.evict, expected);
    EXPECT_TRUE(plan.rejected.empty());
    EXPECT_FALSE(plan.horizon_overflow);
}

TEST(FarEviction, FinestTierOverBudgetEvictsFarthestUntilWithinBudget) {
    FarTierBudgets budgets{};
    budgets.bytes[0] = 250;

    const FarEvictionPlan plan =
        PlanWith(FinestResident(FarTileState::Uploaded), FinestWanted(), budgets);

    const std::vector<FarTileKey> expected = {Key(1, 1, 1), Key(1, 1, 0)};
    EXPECT_EQ(plan.evict, expected);
    EXPECT_EQ(plan.rejected, expected);
    EXPECT_FALSE(plan.horizon_overflow);
}

TEST(FarEviction, NothingEvictedWhenParentNotCovering) {
    FarTierBudgets budgets{};
    budgets.bytes[0] = 250;

    const FarEvictionPlan plan =
        PlanWith(FinestResident(FarTileState::Building), FinestWanted(), budgets);

    EXPECT_TRUE(plan.evict.empty());
    EXPECT_TRUE(plan.rejected.empty());
}

TEST(FarEviction, TierFiveOverBudgetReportsHorizonOverflowWithoutEviction) {
    const std::vector<FarResidentTile> resident = {
        MakeResident(Key(5, 0, 0), FarTileState::Uploaded, 150)};
    const std::vector<FarTileKey> wanted = {Key(5, 0, 0)};

    FarTierBudgets over{};
    over.bytes[4] = 100;
    const FarEvictionPlan over_plan = PlanWith(resident, wanted, over);
    EXPECT_TRUE(over_plan.evict.empty());
    EXPECT_TRUE(over_plan.horizon_overflow);

    FarTierBudgets within{};
    within.bytes[4] = 200;
    const FarEvictionPlan within_plan = PlanWith(resident, wanted, within);
    EXPECT_TRUE(within_plan.evict.empty());
    EXPECT_FALSE(within_plan.horizon_overflow);
}

TEST(FarEviction, ZeroBudgetMeansUnlimited) {
    std::vector<FarResidentTile> resident = FinestResident(FarTileState::Uploaded);
    resident.push_back(MakeResident(Key(5, 0, 0), FarTileState::Uploaded, 1000000));
    std::vector<FarTileKey> wanted = FinestWanted();
    wanted.push_back(Key(5, 0, 0));

    const FarEvictionPlan plan = PlanWith(resident, wanted, FarTierBudgets{});

    EXPECT_TRUE(plan.evict.empty());
    EXPECT_TRUE(plan.rejected.empty());
    EXPECT_FALSE(plan.horizon_overflow);
}

TEST(FarEviction, FinestTierEvictedBeforeCoarserTier) {
    std::vector<FarResidentTile> resident = {
        MakeResident(Key(1, 0, 0), FarTileState::Ready, 100),
        MakeResident(Key(1, 1, 0), FarTileState::Ready, 100),
        MakeResident(Key(1, 0, 1), FarTileState::Ready, 100),
        MakeResident(Key(1, 1, 1), FarTileState::Ready, 100),
        MakeResident(Key(2, 0, 0), FarTileState::Uploaded, 100),
        MakeResident(Key(2, 1, 0), FarTileState::Ready, 100),
        MakeResident(Key(3, 0, 0), FarTileState::Uploaded),
    };
    const std::vector<FarTileKey> wanted = {
        Key(1, 0, 0),
        Key(1, 1, 0),
        Key(1, 0, 1),
        Key(1, 1, 1),
        Key(2, 0, 0),
        Key(2, 1, 0),
        Key(3, 0, 0),
    };
    FarTierBudgets budgets{};
    budgets.bytes[0] = 250;
    budgets.bytes[1] = 150;

    const FarEvictionPlan plan = PlanWith(resident, wanted, budgets);

    const std::vector<FarTileKey> expected = {Key(1, 1, 1), Key(1, 1, 0), Key(2, 1, 0)};
    EXPECT_EQ(plan.evict, expected);
    EXPECT_EQ(plan.rejected, expected);
}

TEST(FarSelection, SelectFarBuildsOrdersCoarsestFirstThenDistanceThenKey) {
    const std::vector<FarTileKey> wanted = {
        Key(1, 4, 0),
        Key(2, 0, 0),
        Key(1, 0, 0),
        Key(2, 1, 0),
        Key(3, 0, 0),
    };
    const std::vector<FarResidentTile> resident;

    const std::vector<FarTileKey> expected = {
        Key(3, 0, 0),
        Key(2, 0, 0),
        Key(2, 1, 0),
        Key(1, 0, 0),
        Key(1, 4, 0),
    };
    EXPECT_EQ(SelectFarBuilds(0.0, 0.0, wanted, resident, 10), expected);
}

TEST(FarSelection, SelectFarBuildsSkipsBuildingAndRejectedAndCurrentResidents) {
    const std::vector<FarTileKey> wanted = {
        Key(1, 0, 0),
        Key(1, 1, 0),
        Key(1, 2, 0),
        Key(1, 3, 0),
        Key(1, 4, 0),
        Key(1, 5, 0),
        Key(1, 6, 0),
        Key(1, 7, 0),
        Key(1, 8, 0),
        Key(1, 9, 0),
    };
    const std::vector<FarResidentTile> resident = {
        MakeResident(Key(1, 0, 0), FarTileState::Building),
        MakeResident(Key(1, 1, 0), FarTileState::Rejected),
        MakeResident(Key(1, 3, 0), FarTileState::Wanted),
        MakeResident(Key(1, 4, 0), FarTileState::Ready, 0, 0, false),
        MakeResident(Key(1, 5, 0), FarTileState::Uploaded, 0, 0, false),
        MakeResident(Key(1, 6, 0), FarTileState::Empty, 0, 0, false),
        MakeResident(Key(1, 7, 0), FarTileState::Ready),
        MakeResident(Key(1, 8, 0), FarTileState::Uploaded),
        MakeResident(Key(1, 9, 0), FarTileState::Empty),
    };

    const std::vector<FarTileKey> expected = {
        Key(1, 2, 0),
        Key(1, 3, 0),
        Key(1, 4, 0),
        Key(1, 5, 0),
        Key(1, 6, 0),
    };
    EXPECT_EQ(SelectFarBuilds(0.0, 0.0, wanted, resident, 100), expected);
    EXPECT_TRUE(SelectFarBuilds(0.0, 0.0, wanted, resident, 0).empty());

    const std::vector<FarTileKey> limited = SelectFarBuilds(0.0, 0.0, wanted, resident, 2);
    EXPECT_EQ(limited, (std::vector<FarTileKey>{Key(1, 2, 0), Key(1, 3, 0)}));
}

TEST(FarSelection, SelectFarBuildsIndependentOfWantedOrder) {
    const std::vector<FarTileKey> wanted = {
        Key(1, 4, 0),
        Key(2, 0, 0),
        Key(1, 0, 0),
        Key(2, 1, 0),
        Key(3, 0, 0),
    };
    const std::vector<FarResidentTile> resident;
    const std::vector<FarTileKey> expected = SelectFarBuilds(0.0, 0.0, wanted, resident, 10);

    std::vector<FarTileKey> shuffled = wanted;
    std::mt19937 rng{1234};
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    EXPECT_EQ(SelectFarBuilds(0.0, 0.0, shuffled, resident, 10), expected);
}

TEST(FarSelection, SelectFarUploadsOnlyReadyWithOrderAndLimits) {
    const std::vector<FarResidentTile> resident = {
        MakeResident(Key(1, 4, 0), FarTileState::Ready, 100),
        MakeResident(Key(1, 1, 0), FarTileState::Uploaded, 100),
        MakeResident(Key(2, 0, 0), FarTileState::Ready, 100),
        MakeResident(Key(1, 0, 0), FarTileState::Ready, 100),
    };
    const std::vector<FarTileKey> ordered = {Key(2, 0, 0), Key(1, 0, 0), Key(1, 4, 0)};

    EXPECT_EQ(SelectFarUploads(0.0, 0.0, resident, 10, 1000000), ordered);
    EXPECT_EQ(SelectFarUploads(0.0, 0.0, resident, 2, 1000000),
              (std::vector<FarTileKey>{Key(2, 0, 0), Key(1, 0, 0)}));
    EXPECT_TRUE(SelectFarUploads(0.0, 0.0, resident, 0, 1000000).empty());

    EXPECT_EQ(SelectFarUploads(0.0, 0.0, resident, 10, 150),
              (std::vector<FarTileKey>{Key(2, 0, 0)}));
    EXPECT_EQ(SelectFarUploads(0.0, 0.0, resident, 10, 50),
              (std::vector<FarTileKey>{Key(2, 0, 0)}));
    EXPECT_EQ(SelectFarUploads(0.0, 0.0, resident, 10, 200),
              (std::vector<FarTileKey>{Key(2, 0, 0), Key(1, 0, 0)}));
}

TEST(FarSelection, AccumulateFarCountersPerTierAndState) {
    const std::vector<FarResidentTile> resident = {
        MakeResident(Key(1, 0, 0), FarTileState::Building, 10, 1),
        MakeResident(Key(1, 1, 0), FarTileState::Ready, 20, 2),
        MakeResident(Key(1, 2, 0), FarTileState::Uploaded, 30, 3),
        MakeResident(Key(1, 3, 0), FarTileState::Wanted),
        MakeResident(Key(2, 0, 0), FarTileState::Empty, 40, 4),
        MakeResident(Key(2, 1, 0), FarTileState::Rejected, 50, 5),
        MakeResident(Key(0, 0, 0), FarTileState::Ready, 1000, 1000),
        MakeResident(Key(9, 1, 1), FarTileState::Ready, 1000, 1000),
    };
    const std::vector<FarTileKey> wanted = {
        Key(1, 0, 0),
        Key(1, 1, 0),
        Key(1, 3, 0),
        Key(2, 0, 0),
        Key(0, 1, 1),
        Key(9, 1, 1),
    };

    const auto counters = AccumulateFarCounters(resident, wanted);

    EXPECT_EQ(counters[0].wanted, 3u);
    EXPECT_EQ(counters[0].building, 1u);
    EXPECT_EQ(counters[0].ready, 1u);
    EXPECT_EQ(counters[0].uploaded, 1u);
    EXPECT_EQ(counters[0].empty, 0u);
    EXPECT_EQ(counters[0].rejected, 0u);
    EXPECT_EQ(counters[0].cpu_bytes, 60u);
    EXPECT_EQ(counters[0].gpu_bytes, 6u);

    EXPECT_EQ(counters[1].wanted, 1u);
    EXPECT_EQ(counters[1].building, 0u);
    EXPECT_EQ(counters[1].ready, 0u);
    EXPECT_EQ(counters[1].uploaded, 0u);
    EXPECT_EQ(counters[1].empty, 1u);
    EXPECT_EQ(counters[1].rejected, 1u);
    EXPECT_EQ(counters[1].cpu_bytes, 90u);
    EXPECT_EQ(counters[1].gpu_bytes, 9u);

    for (std::size_t tier = 2; tier < kFarTierCount; ++tier) {
        SCOPED_TRACE(tier + 1);
        if (tier == 1) {
            continue;
        }
        EXPECT_EQ(counters[tier].wanted, 0u);
        EXPECT_EQ(counters[tier].cpu_bytes, 0u);
        EXPECT_EQ(counters[tier].gpu_bytes, 0u);
    }
}

TEST(FarSelection, FarBuildIdentityEqualityUsesEveryField) {
    FarBuildIdentity base;
    base.world_epoch = 7;
    base.content_hash = 0x1234;
    base.cave_policy = 2;
    base.key = Key(3, -4, 5);
    base.bake_generation = 9;
    base.span_generation = 11;

    const FarBuildIdentity same = base;
    EXPECT_TRUE(base == same);

    {
        FarBuildIdentity changed = base;
        changed.world_epoch = 8;
        EXPECT_FALSE(base == changed) << "world_epoch";
    }
    {
        FarBuildIdentity changed = base;
        changed.content_hash = 0x1235;
        EXPECT_FALSE(base == changed) << "content_hash";
    }
    {
        FarBuildIdentity changed = base;
        changed.cave_policy = 3;
        EXPECT_FALSE(base == changed) << "cave_policy";
    }
    {
        FarBuildIdentity changed = base;
        changed.key.tier = 4;
        EXPECT_FALSE(base == changed) << "key.tier";
    }
    {
        FarBuildIdentity changed = base;
        changed.key.tx = -3;
        EXPECT_FALSE(base == changed) << "key.tx";
    }
    {
        FarBuildIdentity changed = base;
        changed.key.tz = 6;
        EXPECT_FALSE(base == changed) << "key.tz";
    }
    {
        FarBuildIdentity changed = base;
        changed.bake_generation = 10;
        EXPECT_FALSE(base == changed) << "bake_generation";
    }
    {
        FarBuildIdentity changed = base;
        changed.span_generation = 12;
        EXPECT_FALSE(base == changed) << "span_generation";
    }
}
