#include <gtest/gtest.h>

#include "luminumbra_common/world/GroundObjectStore.h"

#include <cstdint>
#include <limits>
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
    EXPECT_TRUE(GroundObjectStore::valid(MakeObject(1, -kMax, -kMax)));
    EXPECT_TRUE(GroundObjectStore::valid(MakeObject(1, kMax - 1, kMax - 1)));
    EXPECT_FALSE(GroundObjectStore::valid(MakeObject(1, kMax, 0)));
    EXPECT_FALSE(GroundObjectStore::valid(MakeObject(1, 0, kMax)));
    EXPECT_FALSE(GroundObjectStore::valid(MakeObject(1, -kMax - 1, 0)));
    EXPECT_FALSE(GroundObjectStore::valid(MakeObject(1, 0, -kMax - 1)));

    GroundObject top = MakeObject(1);
    top.y_mm = kMax;
    EXPECT_TRUE(GroundObjectStore::valid(top));
    top.y_mm = -kMax;
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
    for (std::uint64_t id = 1; id <= GroundObjectStore::kMaxObjects; ++id) {
        ASSERT_TRUE(full.insert(MakeObject(id)));
    }
    EXPECT_EQ(full.size(), GroundObjectStore::kMaxObjects);
    EXPECT_FALSE(full.insert(MakeObject(GroundObjectStore::kMaxObjects + 1)));
    EXPECT_EQ(full.size(), GroundObjectStore::kMaxObjects);
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
    for (std::uint64_t i = 0; i < GroundObjectStore::kMaxObjects; ++i) {
        GroundObject object;
        object.id = 10'000'000'000'000'000'000ull + i;
        object.type = std::numeric_limits<std::uint32_t>::max();
        object.x_mm = kMax - 1;
        object.y_mm = -kMax;
        object.z_mm = -kMax;
        object.spawn_tick = kU64Max;
        object.active_age = kU64Max - 1;
        object.despawn_after = kU64Max;
        object.payload = std::numeric_limits<std::uint32_t>::max();
        object.flags = GroundObject::kSettled;
        ASSERT_TRUE(store.insert(object));
    }
    const std::string bytes = store.serialize();
    EXPECT_LT(bytes.size(), GroundObjectStore::kMaxFileBytes);

    GroundObjectStore parsed;
    std::string error;
    ASSERT_TRUE(GroundObjectStore::parse(bytes, kU64Max, parsed, error)) << error;
    EXPECT_EQ(parsed.size(), GroundObjectStore::kMaxObjects);
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
    saturated.active_age = kU64Max;
    ASSERT_TRUE(store.insert(saturated));

    const GroundObjectStore::SettleFn noSettle = [](GroundObject&) {
        return false;
    };
    const auto absent = store.age_region(RegionKey{9, 9}, noSettle);
    EXPECT_EQ(absent.aged, 0u);
    EXPECT_TRUE(absent.despawned.empty());

    store.age_region(RegionKey{0, 0}, noSettle);
    ASSERT_NE(store.find(4), nullptr);
    EXPECT_EQ(store.find(4)->active_age, kU64Max);
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
