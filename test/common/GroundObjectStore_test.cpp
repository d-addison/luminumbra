#include <gtest/gtest.h>

#include "luminumbra_common/world/GroundObjectStore.h"

#include <cstdint>
#include <limits>
#include <string>

namespace {
using namespace Luminumbra::world;

constexpr std::int64_t kMax = GroundObjectStore::kMaxCoordinateMm;
constexpr std::int64_t kRegion = GroundObjectStore::kRegionSizeMm;

GroundObject MakeObject(std::uint64_t id, std::int64_t x = 0, std::int64_t z = 0) {
    GroundObject object;
    object.id = id;
    object.type = 1;
    object.x_mm = x;
    object.z_mm = z;
    return object;
}

} // namespace

TEST(GroundObjectStoreTest, ValidAcceptsBoundaryCoordinates) {
    EXPECT_TRUE(GroundObjectStore::valid(MakeObject(1)));
    EXPECT_FALSE(GroundObjectStore::valid(MakeObject(1, -kMax, -kMax)));
    EXPECT_TRUE(GroundObjectStore::valid(MakeObject(1, -kMax + 1, -kMax + 1)));
    EXPECT_TRUE(GroundObjectStore::valid(MakeObject(1, kMax - 1, kMax - 1)));
    EXPECT_FALSE(GroundObjectStore::valid(MakeObject(1, kMax, 0)));
    EXPECT_FALSE(GroundObjectStore::valid(MakeObject(1, 0, kMax)));
    EXPECT_FALSE(GroundObjectStore::valid(MakeObject(1, -kMax - 1, 0)));
    EXPECT_FALSE(GroundObjectStore::valid(MakeObject(1, 0, -kMax - 1)));

    GroundObject top = MakeObject(1);
    top.y_mm = GroundObjectStore::kMaxVerticalMm - 1;
    EXPECT_TRUE(GroundObjectStore::valid(top));
    top.y_mm = -GroundObjectStore::kMaxVerticalMm + 1;
    EXPECT_TRUE(GroundObjectStore::valid(top));
    top.y_mm = kMax + 1;
    EXPECT_FALSE(GroundObjectStore::valid(top));
    top.y_mm = -kMax - 1;
    EXPECT_FALSE(GroundObjectStore::valid(top));
}

TEST(GroundObjectStoreTest, ValidRejectsBadFields) {
    GroundObject object = MakeObject(1);
    object.id = 0;
    EXPECT_FALSE(GroundObjectStore::valid(object));

    object = MakeObject(1);
    object.type = 0;
    EXPECT_FALSE(GroundObjectStore::valid(object));

    object = MakeObject(1);
    object.flags = 2;
    EXPECT_FALSE(GroundObjectStore::valid(object));
    object.flags = 1;
    EXPECT_TRUE(GroundObjectStore::valid(object));

    object = MakeObject(1);
    object.despawn_after = 5;
    object.active_age = 5;
    EXPECT_FALSE(GroundObjectStore::valid(object));
    object.active_age = 4;
    EXPECT_TRUE(GroundObjectStore::valid(object));
}

TEST(GroundObjectStoreTest, RegionOfFloorsNegativeValues) {
    EXPECT_EQ(GroundObjectStore::region_of(MakeObject(1, -1, 0)).x, -1);
    EXPECT_EQ(GroundObjectStore::region_of(MakeObject(1, kRegion - 1, 0)).x, 0);
    EXPECT_EQ(GroundObjectStore::region_of(MakeObject(1, kRegion, 0)).x, 1);
    EXPECT_EQ(GroundObjectStore::region_of(MakeObject(1, -kRegion, 0)).x, -1);
    EXPECT_EQ(GroundObjectStore::region_of(MakeObject(1, -kRegion - 1, 0)).x, -2);
    EXPECT_EQ(GroundObjectStore::region_of(MakeObject(1, 0, -kRegion - 1)).z, -2);
    EXPECT_EQ(GroundObjectStore::region_of(MakeObject(1, -kMax, kMax - 1)),
              (RegionKey{-32768, 32767}));
}

TEST(GroundObjectStoreTest, InsertRefusesInvalidDuplicateAndFullStore) {
    GroundObjectStore store;
    EXPECT_FALSE(store.insert(MakeObject(0)));
    EXPECT_FALSE(store.insert(MakeObject(1, kMax, 0)));
    EXPECT_TRUE(store.empty());

    EXPECT_TRUE(store.insert(MakeObject(1)));
    EXPECT_FALSE(store.insert(MakeObject(1, 5, 5)));
    ASSERT_NE(store.find(1), nullptr);
    EXPECT_EQ(store.find(1)->x_mm, 0);
    EXPECT_EQ(store.find(2), nullptr);

    GroundObjectStore full;
    std::uint64_t refused_id = 1;
    while (refused_id <= GroundObjectStore::kMaxObjects && full.insert(MakeObject(refused_id)))
        ++refused_id;
    EXPECT_EQ(full.size(), GroundObjectStore::kMaxObjects);
    const auto unchanged = full.canonical_bytes();
    EXPECT_FALSE(full.insert(MakeObject(refused_id)));
    EXPECT_EQ(full.canonical_bytes(), unchanged);
    EXPECT_TRUE(full.erase(1));
    EXPECT_TRUE(full.insert(MakeObject(refused_id)));
}

TEST(GroundObjectStoreTest, OwnsRegionInRegionAndRegionsAreOrdered) {
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeObject(5, 0, 0)));
    ASSERT_TRUE(store.insert(MakeObject(2, 0, 0)));
    ASSERT_TRUE(store.insert(MakeObject(9, -1, 0)));

    EXPECT_TRUE(store.owns_region(RegionKey{0, 0}));
    EXPECT_TRUE(store.owns_region(RegionKey{-1, 0}));
    EXPECT_FALSE(store.owns_region(RegionKey{3, 3}));

    const auto inZero = store.in_region(RegionKey{0, 0});
    ASSERT_EQ(inZero.size(), 2u);
    EXPECT_EQ(inZero[0].id, 2u);
    EXPECT_EQ(inZero[1].id, 5u);
    EXPECT_TRUE(store.in_region(RegionKey{3, 3}).empty());

    const std::vector<RegionKey> expected{RegionKey{-1, 0}, RegionKey{0, 0}};
    EXPECT_EQ(store.regions(), expected);
}

TEST(GroundObjectStoreTest, RegionVisitorUsesStoredReferencesInIdOrder) {
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeObject(5)));
    ASSERT_TRUE(store.insert(MakeObject(2)));
    ASSERT_TRUE(store.insert(MakeObject(9, -1)));
    std::vector<std::uint64_t> visited;
    store.for_each_in_region({0, 0}, [&](const GroundObject& object) {
        EXPECT_EQ(&object, store.find(object.id));
        visited.push_back(object.id);
    });
    EXPECT_EQ(visited, (std::vector<std::uint64_t>{2, 5}));
    store.for_each_in_region({3, 3}, [](const GroundObject&) {
        ADD_FAILURE() << "Absent region must not invoke the visitor";
    });
}

TEST(GroundObjectStoreTest, EraseRemovesFromBothIndexes) {
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeObject(1, 0, 0)));
    ASSERT_TRUE(store.insert(MakeObject(2, 0, 0)));

    EXPECT_TRUE(store.erase(1));
    EXPECT_FALSE(store.erase(1));
    EXPECT_EQ(store.find(1), nullptr);
    EXPECT_TRUE(store.owns_region(RegionKey{0, 0}));
    EXPECT_EQ(store.in_region(RegionKey{0, 0}).size(), 1u);

    EXPECT_TRUE(store.erase(2));
    EXPECT_FALSE(store.owns_region(RegionKey{0, 0}));
    EXPECT_TRUE(store.regions().empty());
    EXPECT_TRUE(store.empty());
}

TEST(GroundObjectStoreTest, DespawnedRegionsStayEvictableBeyondLedgerCapacity) {
    constexpr std::uint32_t kBatch = 4096;
    constexpr auto kTotal = ActiveRegionLedger::kMaxRegions + kBatch;
    ActiveRegionLedger ledger;
    GroundObjectStore store;
    std::uint64_t live_id = 0;
    ASSERT_TRUE(ledger.allocate_persistent_id(live_id));
    const auto live = MakeObject(live_id, -kRegion + 1, -kRegion + 1);
    const auto live_key = GroundObjectStore::region_of(live);
    ASSERT_TRUE(store.insert(live));
    ledger.activate_owned(live_key, WorldClock{});
    const auto owned = [&](RegionKey key) {
        return store.owns_region(key);
    };
    const auto key_at = [](std::uint32_t index) {
        return RegionKey{static_cast<std::int32_t>(index % 1024u),
                         static_cast<std::int32_t>(index / 1024u)};
    };
    std::uint64_t tick = 0;
    for (std::uint32_t first = 0; first < kTotal; first += kBatch) {
        SCOPED_TRACE(first);
        for (std::uint32_t index = first; index < first + kBatch; ++index) {
            std::uint64_t id = 0;
            ASSERT_TRUE(ledger.allocate_persistent_id(id));
            const auto key = key_at(index);
            ASSERT_TRUE(ledger.can_activate(key));
            auto object = MakeObject(id, key.x * kRegion + 1, key.z * kRegion + 1);
            object.spawn_tick = tick;
            object.despawn_after = 1;
            ASSERT_TRUE(store.insert(object));
            ledger.activate_owned(key, WorldClock(tick));
        }
        ASSERT_EQ(ledger.records().size(), kBatch + 1u);
        ASSERT_LT(ledger.records().size(), ActiveRegionLedger::kMaxRegions);
        tick += ActiveRegionLedger::kIdleEvictionTicks;
        ledger.schedule(WorldClock(tick), {}, {}, {}, owned);
        ASSERT_EQ(ledger.records().size(), kBatch + 1u);
        ASSERT_TRUE(ledger.records().contains(live_key));
        for (std::uint32_t index = first; index < first + kBatch; ++index) {
            const auto key = key_at(index);
            ASSERT_EQ(store.age_region(key, {}).despawned.size(), 1u);
            ASSERT_FALSE(store.owns_region(key));
        }
        const auto schedule = ledger.schedule(WorldClock(++tick), {}, {}, {}, owned);
        ASSERT_EQ(store.size(), 1u);
        ASSERT_EQ(ledger.records().size(), 1u);
        ASSERT_TRUE(ledger.records().contains(live_key));
        EXPECT_EQ(schedule.ledger_records, 1u);
    }
    EXPECT_GT(kTotal, ActiveRegionLedger::kMaxRegions);
    EXPECT_EQ(ledger.next_persistent_id(), kTotal + 2u);
    ASSERT_TRUE(store.erase(live_id));
    ledger.schedule(WorldClock(++tick), {}, {}, {}, owned);
    EXPECT_TRUE(ledger.records().empty());
}

TEST(GroundObjectStoreTest, EmptyStoreHasEmptyHashProjections) {
    GroundObjectStore store;
    EXPECT_EQ(store.canonical_bytes(), "");
    EXPECT_EQ(store.region_bytes(RegionKey{0, 0}), "");
}

TEST(GroundObjectStoreTest, AgeRegionDespawnOrderAndCounts) {
    GroundObjectStore store;
    GroundObject a = MakeObject(1);
    a.despawn_after = 2;
    GroundObject b = MakeObject(2);
    GroundObject c = MakeObject(3);
    c.despawn_after = 1;
    ASSERT_TRUE(store.insert(a));
    ASSERT_TRUE(store.insert(b));
    ASSERT_TRUE(store.insert(c));
    const RegionKey home{0, 0};
    const GroundObjectStore::SettleFn noSettle = [](GroundObject&) {
        return false;
    };

    const auto first = store.age_region(home, noSettle);
    EXPECT_EQ(first.aged, 3u);
    EXPECT_EQ(first.settled, 0u);
    ASSERT_EQ(first.despawned.size(), 1u);
    EXPECT_EQ(first.despawned[0].id, 3u);
    EXPECT_EQ(first.despawned[0].active_age, 1u);
    EXPECT_EQ(store.size(), 2u);

    const auto second = store.age_region(home, noSettle);
    EXPECT_EQ(second.aged, 2u);
    ASSERT_EQ(second.despawned.size(), 1u);
    EXPECT_EQ(second.despawned[0].id, 1u);
    EXPECT_EQ(second.despawned[0].active_age, 2u);
    ASSERT_NE(store.find(2), nullptr);
    EXPECT_EQ(store.find(2)->active_age, 2u);
    EXPECT_EQ(store.size(), 1u);
}

TEST(GroundObjectStoreTest, AgeRegionSkipsAbsentRegionAndSaturates) {
    GroundObjectStore store;
    GroundObject saturated = MakeObject(4);
    saturated.active_age = std::numeric_limits<std::uint32_t>::max();
    ASSERT_TRUE(store.insert(saturated));

    const GroundObjectStore::SettleFn noSettle = [](GroundObject&) {
        return false;
    };
    const auto absent = store.age_region(RegionKey{9, 9}, noSettle);
    EXPECT_EQ(absent.aged, 0u);
    EXPECT_TRUE(absent.despawned.empty());

    store.age_region(RegionKey{0, 0}, noSettle);
    ASSERT_NE(store.find(4), nullptr);
    EXPECT_EQ(store.find(4)->active_age, std::numeric_limits<std::uint32_t>::max());
}

TEST(GroundObjectStoreTest, AgeRegionRemovesEntireRegionWithoutSkippingIds) {
    GroundObjectStore store;
    for (const std::uint64_t id : {3u, 1u, 2u}) {
        auto object = MakeObject(id);
        object.despawn_after = 1;
        ASSERT_TRUE(store.insert(object));
    }
    ASSERT_TRUE(store.insert(MakeObject(4, kRegion, 0)));
    const RegionKey home{0, 0}, other{1, 0};
    EXPECT_EQ(store.region_size(home), 3u);
    EXPECT_EQ(store.region_size(other), 1u);
    EXPECT_EQ(store.region_size({2, 0}), 0u);
    const auto result = store.age_region(home, {});
    EXPECT_EQ(result.aged, 3u);
    ASSERT_EQ(result.despawned.size(), 3u);
    for (std::size_t i = 0; i < result.despawned.size(); ++i) {
        EXPECT_EQ(result.despawned[i].id, i + 1);
    }
    EXPECT_EQ(store.region_size(home), 0u);
    EXPECT_FALSE(store.owns_region(home));
    EXPECT_EQ(store.region_size(other), 1u);
    ASSERT_NE(store.find(4), nullptr);
    EXPECT_EQ(store.find(4)->active_age, 0u);
    ASSERT_TRUE(store.insert(MakeObject(5)));
    EXPECT_EQ(store.region_size(home), 1u);
    EXPECT_EQ(store.age_region(home, {}).aged, 1u);
}

TEST(GroundObjectStoreTest, AgeRegionRevertsSettleThatMovesObject) {
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeObject(7)));
    int calls = 0;
    const GroundObjectStore::SettleFn mover = [&calls](GroundObject& object) {
        ++calls;
        object.x_mm = 5 * kRegion;
        object.y_mm = 9;
        object.flags |= GroundObject::kSettled;
        return true;
    };

    const auto result = store.age_region(RegionKey{0, 0}, mover);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(result.settled, 0u);
    EXPECT_EQ(result.aged, 1u);
    ASSERT_NE(store.find(7), nullptr);
    EXPECT_EQ(store.find(7)->x_mm, 0);
    EXPECT_EQ(store.find(7)->y_mm, 0);
    EXPECT_FALSE(store.find(7)->settled());
    EXPECT_EQ(store.find(7)->active_age, 1u);
    EXPECT_TRUE(store.owns_region(RegionKey{0, 0}));
}

TEST(GroundObjectStoreTest, AgeRegionSettlesWhenOnlyYAndFlagChange) {
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeObject(7)));
    const GroundObjectStore::SettleFn lander = [](GroundObject& object) {
        object.y_mm = -40;
        object.flags |= GroundObject::kSettled;
        return true;
    };

    const auto result = store.age_region(RegionKey{0, 0}, lander);
    EXPECT_EQ(result.settled, 1u);
    ASSERT_NE(store.find(7), nullptr);
    EXPECT_EQ(store.find(7)->y_mm, -40);
    EXPECT_TRUE(store.find(7)->settled());
}

TEST(GroundObjectStoreTest, AgeRegionRevertsSettleThatLeavesInvalidObject) {
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeObject(7)));
    const GroundObjectStore::SettleFn bad = [](GroundObject& object) {
        object.y_mm = kMax + 1;
        object.flags |= GroundObject::kSettled;
        return true;
    };

    const auto result = store.age_region(RegionKey{0, 0}, bad);
    EXPECT_EQ(result.settled, 0u);
    ASSERT_NE(store.find(7), nullptr);
    EXPECT_EQ(store.find(7)->y_mm, 0);
    EXPECT_FALSE(store.find(7)->settled());
}

TEST(GroundObjectStoreTest, AgeRegionDoesNotCallSettleForSettledObjects) {
    GroundObjectStore store;
    GroundObject settled = MakeObject(7);
    settled.flags = GroundObject::kSettled;
    ASSERT_TRUE(store.insert(settled));
    int calls = 0;
    const GroundObjectStore::SettleFn counter = [&calls](GroundObject&) {
        ++calls;
        return false;
    };

    store.age_region(RegionKey{0, 0}, counter);
    EXPECT_EQ(calls, 0);
}

TEST(GroundObjectStoreTest, CanonicalBytesEmptyAndStable) {
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeObject(1, 0, 0)));
    EXPECT_EQ(store.canonical_bytes(), "ground_objects:v1:1,1,0,0,0,0,0,0,0,0;");
    EXPECT_EQ(store.canonical_bytes(), store.canonical_bytes());
    EXPECT_EQ(store.region_bytes(RegionKey{0, 0}), "1,1,0,0,0,0,0,0,0,0;");
    EXPECT_EQ(store.region_bytes(RegionKey{5, 5}), "");
}
