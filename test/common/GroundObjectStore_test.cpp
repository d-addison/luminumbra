#include <gtest/gtest.h>

#include "luminumbra_common/world/GroundObjectStore.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace Luminumbra::world;

constexpr std::int64_t kMax = GroundObjectStore::kMaxCoordinateMm;
constexpr std::int64_t kRegion = GroundObjectStore::kRegionSizeMm;
constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();

GroundObject MakeObject(std::uint64_t id, std::int64_t x = 0, std::int64_t z = 0) {
    GroundObject object;
    object.id = id;
    object.type = 1;
    object.x_mm = x;
    object.z_mm = z;
    return object;
}

std::string ObjectJson(std::uint64_t id) {
    return "{\"id\":" + std::to_string(id) +
           ",\"type\":1,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0,\"spawn_tick\":0,\"active_age\":0,"
           "\"despawn_after\":0,\"payload\":0,\"flags\":0}";
}

std::string FileJson(const std::string& objects) {
    return std::string("{\"schema\":\"luminumbra.ground_objects.v1\",\"objects\":[") + objects +
           "]}\n";
}

std::string Replace(std::string text, const std::string& from, const std::string& to) {
    const auto position = text.find(from);
    if (position != std::string::npos) {
        text.replace(position, from.size(), to);
    }
    return text;
}

// True when parse refuses the bytes, reports an error and leaves the existing store untouched.
bool RefusedUntouched(std::string_view bytes, std::uint64_t next_id = 100) {
    GroundObjectStore out;
    out.insert(MakeObject(99));
    std::string error;
    const bool accepted = GroundObjectStore::parse(bytes, next_id, out, error);
    return !accepted && !error.empty() && out.size() == 1 && out.find(99) != nullptr;
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
    EXPECT_LT(full.size(), GroundObjectStore::kMaxObjects); // file budget also binds
    const auto unchanged = full.canonical_bytes();
    EXPECT_FALSE(full.insert(MakeObject(refused_id)));
    EXPECT_EQ(full.canonical_bytes(), unchanged);
    EXPECT_LE(full.serialize().size(), GroundObjectStore::kMaxFileBytes);
    EXPECT_TRUE(full.erase(1));
    EXPECT_TRUE(full.insert(MakeObject(refused_id)));
}

TEST(GroundObjectStoreTest, NotificationAdmissionChecksLimitMinusOneLimitAndLimitPlusOne) {
    GroundObject future = MakeObject(1);
    future.active_age = std::numeric_limits<std::uint32_t>::max();
    future.y_mm = -GroundObjectStore::kMaxVerticalMm + 1;
    future.flags = GroundObject::kSettled;
    GroundObjectStore reference;
    ASSERT_TRUE(reference.insert(future));
    const auto reserved = reference.serialize().size() - FileJson("").size() + 1;
    const auto limit = GroundObjectStore::kMaxFileBytes - 128 - reserved;
    for (const auto bytes : {limit - 1, limit, limit + 1}) {
        SCOPED_TRACE(bytes);
        GroundObjectStore store;
        EXPECT_EQ(store.insert(MakeObject(1), bytes), bytes <= limit);
        if (bytes > limit) {
            EXPECT_TRUE(store.empty());
            continue;
        }
        GroundNotifications notifications{kU64Max,
                                          {{kU64Max, 0, kU64Max - 1, "ground.spawned", ""}}};
        auto& event = notifications.events.front();
        event.payload.assign(bytes - event.canonical_bytes().size() - 1, 'x');
        const auto settle = [&future](GroundObject& object) {
            object.y_mm = future.y_mm;
            object.flags = future.flags;
            return true;
        };
        store.age_region({0, 0}, settle);
        EXPECT_TRUE(store.fits_file_budget(notifications));
        const auto serialized = store.serialize(notifications);
        EXPECT_LE(serialized.size(), GroundObjectStore::kMaxFileBytes);
        GroundObjectStore restored;
        GroundNotifications loaded;
        std::string error;
        ASSERT_TRUE(GroundObjectStore::parse(serialized, 2, restored, error, &loaded)) << error;
        EXPECT_EQ(restored.serialize(loaded), serialized);
    }
    GroundObjectStore store;
    EXPECT_FALSE(store.insert(MakeObject(1), std::numeric_limits<std::size_t>::max()));
    EXPECT_TRUE(store.empty());
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

TEST(GroundObjectStoreTest, SerializeEmptyIsEmpty) {
    GroundObjectStore store;
    EXPECT_EQ(store.serialize(), "");
    EXPECT_EQ(store.canonical_bytes(), "");
    EXPECT_EQ(store.region_bytes(RegionKey{0, 0}), "");
}

TEST(GroundObjectStoreTest, SerializeParseRoundTrip) {
    GroundObjectStore store;
    GroundObject first = MakeObject(3, -1, kRegion * 4);
    first.type = 7;
    first.y_mm = -250;
    first.spawn_tick = 12;
    first.active_age = 4;
    first.despawn_after = 9;
    first.payload = 77;
    first.flags = GroundObject::kSettled;
    ASSERT_TRUE(store.insert(first));
    ASSERT_TRUE(store.insert(MakeObject(8, 0, 0)));

    const std::string bytes = store.serialize();
    EXPECT_EQ(bytes.back(), '\n');
    EXPECT_EQ(bytes.find(' '), std::string::npos);

    GroundObjectStore parsed;
    std::string error;
    ASSERT_TRUE(GroundObjectStore::parse(bytes, 100, parsed, error)) << error;
    EXPECT_EQ(parsed.objects(), store.objects());
    EXPECT_EQ(parsed.serialize(), bytes);
}

TEST(GroundObjectStoreTest, WorstCaseRecordsRoundTripUnderFileCap) {
    GroundObjectStore store;
    std::uint64_t admitted = 0;
    for (std::uint64_t i = 0; i < GroundObjectStore::kMaxObjects; ++i) {
        GroundObject object;
        object.id = ActiveRegionLedger::kPersistentIdLimit - GroundObjectStore::kMaxObjects + i;
        object.type = std::numeric_limits<std::uint32_t>::max();
        object.x_mm = kMax - 1;
        object.y_mm = -GroundObjectStore::kMaxVerticalMm + 1;
        object.z_mm = -kMax + 1;
        object.spawn_tick = kU64Max;
        object.active_age = std::numeric_limits<std::uint32_t>::max() - 1;
        object.despawn_after = std::numeric_limits<std::uint32_t>::max();
        object.payload = std::numeric_limits<std::uint32_t>::max();
        object.flags = GroundObject::kSettled;
        if (!store.insert(object))
            break;
        ++admitted;
    }
    ASSERT_GT(admitted, 0u);
    const std::string bytes = store.serialize();
    EXPECT_LE(bytes.size(), GroundObjectStore::kMaxFileBytes);
    GroundObjectStore parsed;
    std::string error;
    ASSERT_TRUE(
        GroundObjectStore::parse(bytes, ActiveRegionLedger::kPersistentIdLimit, parsed, error))
        << error;
    EXPECT_EQ(parsed.size(), admitted);
    EXPECT_EQ(parsed.serialize(), bytes);
}

TEST(GroundObjectStoreTest, ParseAcceptsCanonicalFormat) {
    GroundObjectStore out;
    std::string error;
    ASSERT_TRUE(GroundObjectStore::parse(FileJson(ObjectJson(1)), 2, out, error)) << error;
    EXPECT_EQ(out.size(), 1u);
}

TEST(GroundObjectStoreTest, ParseRefusesAndLeavesOutUntouched) {
    const std::string good = FileJson(ObjectJson(1));

    EXPECT_TRUE(RefusedUntouched(Replace(good, "v1", "v2")));
    EXPECT_TRUE(RefusedUntouched(Replace(good, "{\"schema\"", "{\"extra\":1,\"schema\"")));
    EXPECT_TRUE(RefusedUntouched(Replace(good, ",\"objects\":[", "}")));
    EXPECT_TRUE(RefusedUntouched(Replace(good, ",\"flags\":0}", "}")));
    EXPECT_TRUE(RefusedUntouched(Replace(good, ",\"flags\":0}", ",\"flags\":0,\"extra\":0}")));
    EXPECT_TRUE(RefusedUntouched(Replace(good, "\"payload\":0", "\"payload\":0.5")));
    EXPECT_TRUE(RefusedUntouched(Replace(good, "\"id\":1", "\"id\":-1")));
    EXPECT_TRUE(RefusedUntouched(Replace(good, "\"type\":1", "\"type\":0")));
    EXPECT_TRUE(RefusedUntouched(FileJson(ObjectJson(2) + "," + ObjectJson(1))));
    EXPECT_TRUE(RefusedUntouched(FileJson(ObjectJson(1) + "," + ObjectJson(1))));
    EXPECT_TRUE(RefusedUntouched(FileJson(ObjectJson(5)), 5));
    EXPECT_TRUE(RefusedUntouched(FileJson("")));
    EXPECT_TRUE(RefusedUntouched(good + "x"));
    EXPECT_TRUE(RefusedUntouched(Replace(good, "\"schema\":", "\"schema\": ")));
    EXPECT_TRUE(RefusedUntouched(good.substr(0, good.size() - 1)));
    EXPECT_TRUE(RefusedUntouched("{"));
    EXPECT_TRUE(RefusedUntouched(""));
    EXPECT_TRUE(RefusedUntouched(std::string(GroundObjectStore::kMaxFileBytes + 1, ' ')));
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
    EXPECT_EQ(store.serialize(), FileJson(ObjectJson(1)));
}

TEST(GroundObjectStoreTest, DeeplyNestedInputRefusesBeforeParsingAndPreservesOutputs) {
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeObject(99)));
    GroundNotifications notifications{7, {{3, 0, 6, "ground.spawned", "kept"}}};
    const auto before = store.serialize(notifications);
    const std::string nested = std::string("{\"schema\":") + std::string(100000, '[') + "0" +
                               std::string(100000, ']') + "}";
    std::string error;
    EXPECT_FALSE(GroundObjectStore::parse(nested, 100, store, error, &notifications));
    EXPECT_NE(error.find("nesting depth"), std::string::npos);
    EXPECT_EQ(store.serialize(notifications), before);
}

TEST(GroundObjectStoreTest, NotificationsRoundTripWithoutChangingLegacyBytesOrHash) {
    GroundObjectStore store, restored;
    ASSERT_TRUE(store.insert(MakeObject(1)));
    const auto legacy = store.serialize();
    const auto hash = store.canonical_bytes();
    GroundNotifications notifications{9,
                                      {{4, -1, 3, "ground.spawned", "1,1,0,0,0"},
                                       {100, 2, 8, "ground.despawned", "quoted \"[{}]\\"}}};
    GroundNotifications loaded;
    std::string error;
    ASSERT_TRUE(
        GroundObjectStore::parse(store.serialize(notifications), 2, restored, error, &loaded))
        << error;
    EXPECT_EQ(restored.serialize(loaded), store.serialize(notifications));
    EXPECT_EQ(restored.canonical_bytes(), hash);
    EXPECT_EQ(loaded.next_sequence, 9u);
    ASSERT_EQ(loaded.events.size(), 2u);
    EXPECT_EQ(loaded.events.back().tick, 100u);
    ASSERT_TRUE(GroundObjectStore::parse(legacy, 2, restored, error, &loaded));
    EXPECT_EQ(restored.serialize(), legacy);
    EXPECT_EQ(loaded.next_sequence, 0u);
    EXPECT_TRUE(loaded.events.empty());
    // Notifications also survive the removal of the final object.
    ASSERT_TRUE(store.erase(1));
    ASSERT_TRUE(
        GroundObjectStore::parse(store.serialize(notifications), 2, restored, error, &loaded));
    EXPECT_TRUE(restored.empty());
    EXPECT_EQ(restored.serialize(loaded), store.serialize(notifications));
}

TEST(GroundObjectStoreTest,
     LoadedNotificationCursorRefusesPublicationAtExhaustionWithoutChangingState) {
    for (const auto cursor : {kU64Max, kU64Max - 1}) {
        SCOPED_TRACE(cursor);
        GroundObjectStore store;
        GroundNotifications loaded{7, {{3, 0, 6, "ground.spawned", "kept"}}};
        std::string error;
        ASSERT_TRUE(GroundObjectStore::parse(
            store.serialize(GroundNotifications{cursor, {}}), 1, store, error, &loaded))
            << error;
        luminumbra::simulation::OrderedEventBus bus;
        bus.restore_pending(loaded.events, loaded.next_sequence);
        if (cursor < kU64Max) {
            EXPECT_EQ(bus.publish(0, "ground.spawned", "last"), kU64Max - 1);
        }
        const GroundNotifications before{bus.next_sequence(), bus.pending_events()};
        const auto bytes = store.serialize(before);
        EXPECT_THROW(bus.publish(0, "ground.despawned", "refused"), std::overflow_error);
        EXPECT_EQ(bus.next_sequence(), kU64Max);
        EXPECT_EQ(store.serialize(GroundNotifications{bus.next_sequence(), bus.pending_events()}),
                  bytes);
        // Both cursor-only state and the final pending event survive a round trip.
        ASSERT_TRUE(GroundObjectStore::parse(bytes, 1, store, error, &loaded)) << error;
        EXPECT_TRUE(store.empty());
        EXPECT_EQ(loaded.next_sequence, kU64Max);
        EXPECT_EQ(loaded.events.size(), cursor < kU64Max ? 1u : 0u);
        EXPECT_EQ(store.serialize(loaded), bytes);
        bus.restore_pending(loaded.events, loaded.next_sequence);
        EXPECT_THROW(bus.publish(0, "ground.spawned", "refused"), std::overflow_error);
        EXPECT_EQ(bus.next_sequence(), kU64Max);
        EXPECT_EQ(store.serialize(GroundNotifications{bus.next_sequence(), bus.pending_events()}),
                  bytes);
        EXPECT_EQ(bus.pending_bytes("ground.spawned"),
                  loaded.events.empty() ? 0u : loaded.events.front().canonical_bytes().size() + 1);
        bus.drain(0);
        EXPECT_EQ(bus.pending_bytes("ground.spawned"), 0u);
        EXPECT_THROW(bus.publish(0, "ground.despawned", "refused"), std::overflow_error);
        EXPECT_EQ(bus.next_sequence(), kU64Max);
        EXPECT_EQ(bus.pending_count(), 0u);
    }
}

TEST(GroundObjectStoreTest, ParseReservesObjectGrowthAlongsidePendingNotifications) {
    GroundObjectStore source;
    ASSERT_TRUE(source.insert(MakeObject(1)));
    GroundNotifications notifications{kU64Max - 1,
                                      {{kU64Max, 0, kU64Max - 2, "ground.spawned", ""}}};
    notifications.events.front().payload.assign(
        GroundObjectStore::kMaxFileBytes - source.serialize(notifications).size(), 'x');
    const auto bytes = source.serialize(notifications);
    ASSERT_EQ(bytes.size(), GroundObjectStore::kMaxFileBytes);
    EXPECT_FALSE(source.fits_file_budget(notifications));
    GroundObjectStore restored;
    ASSERT_TRUE(restored.insert(MakeObject(99)));
    GroundNotifications loaded{7, {{3, 0, 6, "ground.spawned", "kept"}}};
    const auto before = restored.serialize(loaded);
    std::string error;
    EXPECT_FALSE(GroundObjectStore::parse(bytes, 100, restored, error, &loaded));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(restored.serialize(loaded), before);
}

TEST(GroundObjectStoreTest, InvalidNotificationsRefuseTransactionally) {
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeObject(1)));
    const GroundNotifications notifications{2, {{4, 0, 1, "ground.spawned", "1,1,0,0,0"}}};
    const auto bytes = store.serialize(notifications);
    EXPECT_TRUE(RefusedUntouched(Replace(bytes, "\"next_sequence\":2", "\"next_sequence\":1")));
    for (const auto* cursor : {"18446744073709551616", "-1", "2.0"}) {
        SCOPED_TRACE(cursor);
        EXPECT_TRUE(RefusedUntouched(
            Replace(bytes, "\"next_sequence\":2", std::string("\"next_sequence\":") + cursor)));
    }
    EXPECT_TRUE(RefusedUntouched(Replace(bytes, "ground.spawned", "script.trigger")));
    EXPECT_TRUE(RefusedUntouched(Replace(bytes, "\"lane\":0", "\"lane\":2147483648")));
    EXPECT_TRUE(RefusedUntouched(Replace(bytes, "\"tick\":4", "\"tick\":4.5")));
    EXPECT_TRUE(
        RefusedUntouched(Replace(bytes, "\"sequence\":1", "\"sequence\":1,\"sequence\":1")));
}

namespace {
struct DirectSettlement {
    std::vector<std::uint64_t>& visited;
    int mode;
    DirectSettlement(std::vector<std::uint64_t>& ids, int behavior)
        : visited(ids)
        , mode(behavior) {}
    DirectSettlement(const DirectSettlement&) = delete;
    DirectSettlement& operator=(const DirectSettlement&) = delete;
    bool operator()(GroundObject& object) const {
        visited.push_back(object.id);
        if (mode == 0) {
            return false;
        }
        object.y_mm = 10;
        object.flags |= GroundObject::kSettled;
        if (mode == 2) {
            ++object.payload;
        }
        return true;
    }
};
} // namespace

TEST(GroundObjectStoreTest, ConcreteSettlementNeedsNoCopyAndVisitsOnlyUnsettledOwnedObjects) {
    for (const int mode : {0, 1, 2}) {
        SCOPED_TRACE(mode);
        GroundObjectStore store;
        auto settled = MakeObject(2);
        settled.flags = GroundObject::kSettled;
        auto expiring = MakeObject(3);
        expiring.despawn_after = 1;
        ASSERT_TRUE(store.insert(expiring));
        ASSERT_TRUE(store.insert(settled));
        ASSERT_TRUE(store.insert(MakeObject(1)));
        ASSERT_TRUE(store.insert(MakeObject(4, kRegion)));
        std::vector<std::uint64_t> visited;
        const DirectSettlement settle(visited, mode);
        EXPECT_EQ(store.age_region({9, 9}, settle).aged, 0u);
        EXPECT_TRUE(visited.empty());
        const auto result = store.age_region({0, 0}, settle);
        EXPECT_EQ(visited, (std::vector<std::uint64_t>{1, 3}));
        EXPECT_EQ(result.aged, 3u);
        EXPECT_EQ(result.settled, mode == 1 ? 2u : 0u);
        ASSERT_EQ(result.despawned.size(), 1u);
        EXPECT_EQ(result.despawned.front().id, 3u);
        const auto* live = store.find(1);
        ASSERT_NE(live, nullptr);
        EXPECT_EQ(live->active_age, 1u);
        EXPECT_EQ(live->y_mm, mode == 1 ? 10 : 0);
        EXPECT_EQ(live->payload, 0u);
        EXPECT_EQ(live->settled(), mode == 1);
        const auto* other = store.find(4);
        ASSERT_NE(other, nullptr);
        EXPECT_EQ(other->active_age, 0u);
        EXPECT_FALSE(other->settled());
    }
    GroundObjectStore empty;
    std::vector<std::uint64_t> visited;
    const DirectSettlement settle(visited, 1);
    EXPECT_EQ(empty.age_region({0, 0}, settle).aged, 0u);
    EXPECT_TRUE(visited.empty());
}

TEST(GroundObjectStoreTest, NullableSettlementRetainsLegacyAdmission) {
    for (const int mode : {0, 1, 2, 3}) {
        GroundObjectStore store;
        ASSERT_TRUE(store.insert(MakeObject(1)));
        bool (*const missing)(GroundObject&) = nullptr;
        GroundObjectStore::AgeResult result;
        if (mode == 0) {
            result = store.age_region({0, 0}, missing);
        } else if (mode == 1) {
            result = store.age_region({0, 0}, nullptr);
        } else if (mode == 2) {
            result = store.age_region({0, 0}, {});
        } else {
            const GroundObjectStore::SettleFn empty;
            result = store.age_region({0, 0}, empty);
        }
        EXPECT_EQ(result.aged, 1u);
        EXPECT_EQ(result.settled, 0u);
        const auto* live = store.find(1);
        ASSERT_NE(live, nullptr);
        EXPECT_FALSE(live->settled());
    }
}
