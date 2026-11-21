#include "luminumbra_common/core/PaletteTable.h"
#include <luminumbra/core/Types.h>

#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra {
namespace {

constexpr std::string_view kBaseJson =
    R"({"entries":[["luminumbra:air",false],["luminumbra:stone",false],["luminumbra:soil",false],["luminumbra:grass",false],["luminumbra:sand",false],["luminumbra:deepslate",false],["luminumbra:lumin_crystal",false],["luminumbra:water",false]],"version":1})";

std::string WithRecords(std::string_view records) {
    std::string bytes(kBaseJson);
    bytes.insert(bytes.size() - std::string_view(R"(],"version":1})").size(), records);
    return bytes;
}

TEST(PaletteTableTest, ReservesEveryLegacyMaterialIdAndCanonicalBaseBytes) {
    const PaletteTable table;
    constexpr std::array types{MaterialType::Air,
                               MaterialType::Stone,
                               MaterialType::Soil,
                               MaterialType::Grass,
                               MaterialType::Sand,
                               MaterialType::Deepslate,
                               MaterialType::LuminCrystal,
                               MaterialType::Water};
    constexpr std::array<std::string_view, 8> keys{"luminumbra:air",
                                                   "luminumbra:stone",
                                                   "luminumbra:soil",
                                                   "luminumbra:grass",
                                                   "luminumbra:sand",
                                                   "luminumbra:deepslate",
                                                   "luminumbra:lumin_crystal",
                                                   "luminumbra:water"};
    ASSERT_EQ(table.size(), types.size());
    for (std::size_t id = 0; id < types.size(); ++id) {
        EXPECT_EQ(static_cast<std::uint32_t>(types[id]), id);
        EXPECT_EQ(table.find(keys[id]), id);
        ASSERT_NE(table.entry(id), nullptr);
        EXPECT_EQ(table.entry(id)->key, keys[id]);
        EXPECT_FALSE(table.entry(id)->tombstone);
    }
    EXPECT_EQ(table.to_json(), kBaseJson);
}

TEST(PaletteTableTest, ValidatesBoundedLocaleIndependentNamespacedKeys) {
    for (const std::string_view key : {"a:b", "acme.alpine_fauna:snow_lynx", "0_.-:9_.-"}) {
        EXPECT_TRUE(PaletteTable::valid_key(key)) << key;
    }
    for (const std::string_view key : {"",
                                       ":",
                                       ":name",
                                       "namespace:",
                                       "name",
                                       "a:b:c",
                                       "A:b",
                                       "a:B",
                                       "a:b/c",
                                       "a:b\\c",
                                       "a:b c",
                                       "a:\"b",
                                       "a:b\n",
                                       "a:\xc3\xa9"}) {
        EXPECT_FALSE(PaletteTable::valid_key(key)) << key;
    }
    EXPECT_FALSE(PaletteTable::valid_key(std::string("a:b\0c", 5)));
    const std::string maximum = "a:" + std::string(PaletteTable::kMaxKeyBytes - 2, 'z');
    EXPECT_TRUE(PaletteTable::valid_key(maximum));
    EXPECT_FALSE(PaletteTable::valid_key(maximum + 'z'));
}

TEST(PaletteTableTest, AppendsDeterministicallyAndNeverRenumbersExistingIds) {
    std::vector<std::string> keys{"z.mod:zinc", "a.mod:amber", "m.mod:marble", "a.mod:amber"};
    std::string error = "stale error";
    PaletteTable first;
    ASSERT_TRUE(first.register_ids(keys, error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(first.find("a.mod:amber"), 8);
    EXPECT_EQ(first.find("m.mod:marble"), 9);
    EXPECT_EQ(first.find("z.mod:zinc"), 10);
    std::reverse(keys.begin(), keys.end());
    PaletteTable second;
    ASSERT_TRUE(second.register_ids(keys, error)) << error;
    EXPECT_EQ(first.to_json(), second.to_json());
    keys = {"0:new", "luminumbra:air", "a.mod:amber"};
    ASSERT_TRUE(first.register_ids(keys, error)) << error;
    EXPECT_EQ(first.find("0:new"), 11);
    EXPECT_EQ(first.find("a.mod:amber"), 8);
    EXPECT_EQ(first.find("m.mod:marble"), 9);
    EXPECT_EQ(first.find("z.mod:zinc"), 10);
    EXPECT_EQ(first.find("luminumbra:air"), 0);
    EXPECT_EQ(first.size(), 12);
    const auto before = first.to_json();
    ASSERT_TRUE(first.register_ids({}, error)) << error;
    EXPECT_EQ(first.to_json(), before);
}

TEST(PaletteTableTest, CanonicalJsonRoundTripsWithReorderedAvailability) {
    PaletteTable original;
    std::vector<std::string> keys{"z:last", "a:first"};
    std::string error;
    ASSERT_TRUE(original.register_ids(keys, error)) << error;
    const auto bytes = original.to_json();
    EXPECT_EQ(bytes, WithRecords(R"(,["a:first",false],["z:last",false])"));
    std::reverse(keys.begin(), keys.end());
    PaletteTable loaded;
    ASSERT_TRUE(loaded.load_json(bytes, keys, error)) << error;
    EXPECT_EQ(loaded.to_json(), bytes);
    EXPECT_EQ(loaded.find("a:first"), 8);
    EXPECT_EQ(loaded.find("z:last"), 9);
    ASSERT_TRUE(loaded.load_json(kBaseJson, {}, error)) << error;
    EXPECT_EQ(loaded.to_json(), kBaseJson);
}

TEST(PaletteTableTest, MissingKeysBecomePermanentTombstonesWithoutRecycling) {
    const auto bytes = WithRecords(R"(,["z:removed",false],["a:kept",false])");
    const std::vector<std::string> available{"a:kept", "b:new", "0:new"};
    PaletteTable loaded;
    std::string error;
    ASSERT_TRUE(loaded.load_json(bytes, available, error)) << error;
    EXPECT_EQ(loaded.find("z:removed"), 8);
    EXPECT_EQ(loaded.find("a:kept"), 9);
    EXPECT_EQ(loaded.find("0:new"), 10);
    EXPECT_EQ(loaded.find("b:new"), 11);
    ASSERT_NE(loaded.entry(8), nullptr);
    EXPECT_TRUE(loaded.entry(8)->tombstone);
    EXPECT_FALSE(loaded.entry(9)->tombstone);
    const auto tombstoned = loaded.to_json();
    const std::vector<std::string> restored{"z:removed", "b:new", "a:kept", "0:new"};
    ASSERT_TRUE(loaded.register_ids(restored, error)) << error;
    EXPECT_EQ(loaded.to_json(), tombstoned);
    ASSERT_TRUE(loaded.load_json(tombstoned, restored, error)) << error;
    EXPECT_EQ(loaded.to_json(), tombstoned);
    ASSERT_TRUE(loaded.load_json(tombstoned, {}, error)) << error;
    for (std::size_t id = 0; id < loaded.size(); ++id) {
        ASSERT_NE(loaded.entry(id), nullptr);
        EXPECT_EQ(loaded.entry(id)->tombstone, id >= PaletteTable::kBaseEntries);
    }
}

TEST(PaletteTableTest, InvalidRegistrationIsAtomicAndLookupsDistinguishAbsence) {
    PaletteTable table;
    std::string error;
    const std::vector<std::string> existing{"z:existing"};
    ASSERT_TRUE(table.register_ids(existing, error)) << error;
    const std::vector<std::string> keys{"a:valid", "invalid"};
    const auto before = table.to_json();
    EXPECT_FALSE(table.register_ids(keys, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(table.to_json(), before);
    EXPECT_FALSE(table.find("a:valid").has_value());
    EXPECT_FALSE(table.find("invalid").has_value());
    EXPECT_FALSE(table.find("a:absent").has_value());
    EXPECT_EQ(table.entry(table.size()), nullptr);
    EXPECT_EQ(table.entry(std::numeric_limits<std::size_t>::max()), nullptr);
}

TEST(PaletteTableTest, RefusesEveryTruncatedCanonicalDocumentAtomically) {
    PaletteTable table;
    std::string error;
    const std::vector<std::string> existing{"z:existing"};
    ASSERT_TRUE(table.register_ids(existing, error)) << error;
    const auto before = table.to_json();
    const auto bytes = WithRecords(R"(,["test:item",true])");
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        EXPECT_FALSE(table.load_json(std::string_view(bytes).substr(0, length), {}, error))
            << length;
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(table.to_json(), before);
    }
}

TEST(PaletteTableTest, RefusesMalformedRecordsAndNoncanonicalEnvelopesAtomically) {
    PaletteTable table;
    std::string error;
    const auto before = table.to_json();
    std::vector<std::string> malformed{
        "",
        "null",
        "[]",
        "{}",
        R"({"entries":[],"version":1})",
        " " + std::string(kBaseJson),
        std::string(kBaseJson) + "\n",
        WithRecords(","),
        WithRecords(",[]"),
        WithRecords(R"(,["test:item",0])"),
        WithRecords(R"(,["test:item",null])"),
        WithRecords(R"(,["test:item",false,0])"),
        WithRecords(R"(,["test:item",false])") + "x",
        WithRecords(R"(,["test:item",false],)"),
        WithRecords(R"(,["test:item",False])"),
        WithRecords(R"(,["test:item",false],["test:item",true])"),
        WithRecords(R"(,["luminumbra:air",false])"),
        WithRecords(R"(,["test:\u0061",false])"),
        WithRecords(R"(,["test:item",false]["test:other",false])"),
        WithRecords(R"(,["test:item",false],[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[)"),
        WithRecords(R"(,["invalid",false])"),
        WithRecords(R"(,["a:b:c",false])")};
    auto wrong_version = std::string(kBaseJson);
    wrong_version.replace(wrong_version.size() - 2, 1, "2");
    malformed.push_back(wrong_version);
    for (const auto& bytes : malformed) {
        EXPECT_FALSE(table.load_json(bytes, {}, error)) << bytes;
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(table.to_json(), before);
    }
}

TEST(PaletteTableTest, RefusesChangedReorderedOrTombstonedBaseEntries) {
    PaletteTable table;
    std::string error;
    for (const std::string_view key :
         {"air", "stone", "soil", "grass", "sand", "deepslate", "lumin_crystal", "water"}) {
        auto changed = std::string(kBaseJson);
        const auto offset = changed.find("luminumbra:" + std::string(key));
        ASSERT_NE(offset, std::string::npos);
        changed[offset] = 'x';
        EXPECT_FALSE(table.load_json(changed, {}, error));
        auto tombstoned = std::string(kBaseJson);
        const auto flag = tombstoned.find("false", offset);
        ASSERT_NE(flag, std::string::npos);
        tombstoned.replace(flag, 5, "true");
        EXPECT_FALSE(table.load_json(tombstoned, {}, error));
        EXPECT_EQ(table.to_json(), kBaseJson);
    }
    auto reordered = std::string(kBaseJson);
    constexpr std::string_view first_pair =
        R"(["luminumbra:air",false],["luminumbra:stone",false])";
    reordered.replace(reordered.find(first_pair),
                      first_pair.size(),
                      R"(["luminumbra:stone",false],["luminumbra:air",false])");
    EXPECT_FALSE(table.load_json(reordered, {}, error));
    EXPECT_EQ(table.to_json(), kBaseJson);
}

TEST(PaletteTableTest, InvalidAvailabilityAndOversizedInputsLeavePriorTableUnchanged) {
    PaletteTable table;
    std::string error;
    const std::vector<std::string> existing{"z:existing"};
    ASSERT_TRUE(table.register_ids(existing, error)) << error;
    const auto before = table.to_json();
    const auto bytes = WithRecords(R"(,["a:saved",false])");
    const std::vector<std::string> invalid{"0:new", "invalid"};
    EXPECT_FALSE(table.load_json(bytes, invalid, error));
    EXPECT_EQ(table.to_json(), before);
    const std::vector<std::string> too_many(PaletteTable::kMaxEntries + 1, "a:b");
    EXPECT_FALSE(table.register_ids(too_many, error));
    EXPECT_FALSE(table.load_json(bytes, too_many, error));
    EXPECT_EQ(table.to_json(), before);
    const std::string too_long = "a:" + std::string(PaletteTable::kMaxKeyBytes - 1, 'b');
    const std::vector<std::string> invalid_length{too_long};
    EXPECT_FALSE(table.register_ids(invalid_length, error));
    EXPECT_FALSE(table.load_json(WithRecords(",[\"" + too_long + "\",false]"), {}, error));
    EXPECT_FALSE(table.load_json(std::string(PaletteTable::kMaxJsonBytes + 1, ' '), {}, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(table.to_json(), before);
}

TEST(PaletteTableTest, MaximumKeyLengthRoundTripsByteExactly) {
    const std::vector<std::string> keys{"a:" + std::string(PaletteTable::kMaxKeyBytes - 2, 'z')};
    PaletteTable table;
    std::string error;
    ASSERT_TRUE(table.register_ids(keys, error)) << error;
    const auto bytes = table.to_json();
    ASSERT_TRUE(table.load_json(bytes, keys, error)) << error;
    EXPECT_EQ(table.to_json(), bytes);
}

TEST(PaletteTableTest, ExhaustedIdSpaceNeverWrapsOrRecyclesTombstones) {
    std::vector<std::string> keys;
    keys.reserve(PaletteTable::kMaxEntries - PaletteTable::kBaseEntries);
    for (std::size_t id = PaletteTable::kBaseEntries; id < PaletteTable::kMaxEntries; ++id) {
        keys.push_back("test:item_" + std::to_string(id));
    }
    PaletteTable table;
    std::string error;
    ASSERT_TRUE(table.register_ids(keys, error)) << error;
    ASSERT_EQ(table.size(), PaletteTable::kMaxEntries);
    const auto last = table.entry(std::numeric_limits<PaletteId>::max());
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(table.find(last->key), std::numeric_limits<PaletteId>::max());
    const auto full = table.to_json();
    ASSERT_TRUE(table.load_json(full, {}, error)) << error;
    const auto tombstoned = table.to_json();
    const std::vector<std::string> extra{"test:overflow"};
    EXPECT_FALSE(table.register_ids(extra, error));
    EXPECT_EQ(table.to_json(), tombstoned);
    EXPECT_FALSE(table.load_json(full, extra, error));
    EXPECT_EQ(table.to_json(), tombstoned);
    ASSERT_TRUE(table.register_ids(keys, error)) << error;
    EXPECT_EQ(table.to_json(), tombstoned);
    const auto extra_record =
        full.substr(0, full.size() - std::string_view(R"(],"version":1})").size()) +
        R"(,["test:overflow",false]],"version":1})";
    EXPECT_FALSE(table.load_json(extra_record, {}, error));
    EXPECT_EQ(table.to_json(), tombstoned);
}

TEST(PaletteTableTest, CopiesOwnTheirEntriesAndLookupIndexes) {
    PaletteTable original;
    std::string error;
    const std::vector<std::string> keys{"a:item"};
    ASSERT_TRUE(original.register_ids(keys, error)) << error;
    PaletteTable copied(original);
    PaletteTable assigned;
    assigned = original;
    ASSERT_TRUE(original.load_json(kBaseJson, {}, error)) << error;
    EXPECT_FALSE(original.find("a:item").has_value());
    EXPECT_EQ(copied.find("a:item"), 8);
    EXPECT_EQ(assigned.find("a:item"), 8);
    EXPECT_EQ(copied.to_json(), assigned.to_json());
}

} // namespace
} // namespace Luminumbra
