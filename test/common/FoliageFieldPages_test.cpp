#include <gtest/gtest.h>

#include "luminumbra_common/fields/FieldPageState.h"
#include "luminumbra_common/fields/FoliageFieldPages.h"

#include <cstdint>
#include <limits>
#include <string>

namespace {
using namespace Luminumbra::world;
TEST(FoliageFieldPages, ExactUnitsAndInitializedBaselineSurviveTypedSave) {
    FieldPageState state;
    const ScentCellKey negative{-1, -1}, positive{512, 0};
    ASSERT_TRUE(state.set_soil(negative, 999997));
    ASSERT_TRUE(state.set_soil(positive, FieldPageState::kSoilBaseline));
    ASSERT_TRUE(state.set_irrigation(negative, 3999999));
    const auto bytes = state.serialize();
    FieldPageState restored;
    std::string error;
    ASSERT_TRUE(FieldPageState::parse(bytes, restored, error)) << error;
    EXPECT_EQ(restored.serialize(), bytes);
    EXPECT_EQ(restored.soil().sample(negative, 1000000), 999997);
    EXPECT_EQ(restored.irrigation().sample(negative, 0), 3999999);
    EXPECT_EQ(restored.soil().sample({0, 0}, 1000000), 1000000);
    EXPECT_TRUE(restored.owns_region({-1, -1}));
    EXPECT_TRUE(restored.owns_region({1, 0}));
    EXPECT_FALSE(restored.owns_region({0, 0}));
    EXPECT_NE(restored.region_bytes({1, 0}), "");    // baseline still initialized/owned
    EXPECT_EQ(restored.ambient_bytes(31, true), ""); // ambient slots unchanged by foliage
    EXPECT_EQ(restored.region_bytes({-1, -1}), state.region_bytes({-1, -1}));
}
TEST(FoliageFieldPages, StrictRecordValidationLeavesDestinationUntouched) {
    FoliageFieldPages original;
    ASSERT_TRUE(original.set({0, 0}, 10, 1000000));
    const auto record = original.records();
    FoliageFieldPages restored;
    ASSERT_TRUE(FoliageFieldPages::parse_records(record, 1, 1000000, restored));
    const auto prior = restored.records();
    for (const auto offset : {0u, 8u, 40u, 44u}) {
        auto corrupt = record;
        if (offset == 0)
            corrupt.replace(0, 4, std::string(4, '\x7f')); // invalid page
        else if (offset == 8)
            corrupt.replace(8, 32, std::string(32, '\0')); // empty mask
        else
            corrupt[offset] = '\xff'; // initialized overflow or nonzero uninitialized
        EXPECT_FALSE(FoliageFieldPages::parse_records(corrupt, 1, 100, restored));
        EXPECT_EQ(restored.records(), prior);
    }
    EXPECT_FALSE(FoliageFieldPages::parse_records(record + record, 2, 1000000, restored));
    EXPECT_FALSE(FoliageFieldPages::parse_records(
        record, FoliageFieldPages::kMaxPages + 1, 1000000, restored));
    EXPECT_EQ(restored.records(), prior);
}
TEST(FoliageFieldPages, HalfOpenWorldBoundsAndRefusalAreAtomic) {
    FoliageFieldPages pages;
    ASSERT_TRUE(pages.set({-16777216, -16777216}, 1, 1000000));
    ASSERT_TRUE(pages.set({16777215, 16777215}, 0, 1000000));
    const auto before = pages.records();
    EXPECT_FALSE(pages.set({16777216, 0}, 1, 1000000));
    EXPECT_FALSE(pages.set({-16777217, 0}, 1, 1000000));
    EXPECT_FALSE(pages.set({0, 0}, -1, 1000000));
    EXPECT_FALSE(pages.set({0, 0}, 1000001, 1000000));
    EXPECT_EQ(pages.records(), before);
}

TEST(FoliageFieldPages, BaselineHistoryAndClearPreserveInitializedMasks) {
    FieldPageState state;
    const ScentCellKey cell{-17, 16};
    EXPECT_FALSE(state.soil().initialized(cell));
    EXPECT_FALSE(state.irrigation().initialized(cell));
    ASSERT_TRUE(state.set_soil(cell, 1));
    ASSERT_TRUE(state.set_irrigation(cell, FieldPageState::kIrrigationLimit));
    ASSERT_TRUE(state.set_soil(cell, FieldPageState::kSoilBaseline));
    ASSERT_TRUE(state.set_irrigation(cell, 0));
    FieldPageState restored;
    std::string error;
    ASSERT_TRUE(FieldPageState::parse(state.serialize(), restored, error)) << error;
    EXPECT_TRUE(restored.soil().initialized(cell));
    EXPECT_TRUE(restored.irrigation().initialized(cell));
    EXPECT_EQ(restored.soil().sample(cell, -1), FieldPageState::kSoilBaseline);
    EXPECT_EQ(restored.irrigation().sample(cell, -1), 0);
    EXPECT_EQ(restored.soil().pages(), state.soil().pages());
    EXPECT_EQ(restored.irrigation().pages(), state.irrigation().pages());
    EXPECT_TRUE(restored.owns_region({-1, 0}));
    EXPECT_FALSE(restored.empty());
    restored.clear();
    EXPECT_TRUE(restored.empty());
    EXPECT_TRUE(restored.soil().pages().empty());
    EXPECT_TRUE(restored.irrigation().pages().empty());
    EXPECT_FALSE(restored.owns_region({-1, 0}));
    EXPECT_TRUE(restored.serialize().empty());
}

TEST(FoliageFieldPages, PageCapacityRefusesNewPagesButAllowsExistingPageEdits) {
    FoliageFieldPages pages;
    for (std::size_t index = 0; index < FoliageFieldPages::kMaxPages; ++index) {
        ASSERT_TRUE(pages.set({static_cast<std::int32_t>(index) * 16, 0}, 7, 1000000));
    }
    ASSERT_EQ(pages.pages().size(), FoliageFieldPages::kMaxPages);
    const auto before = pages.records();
    EXPECT_FALSE(pages.set({-1, 0}, 3, 1000000));
    EXPECT_EQ(pages.records(), before);
    ASSERT_TRUE(pages.set({0, 0}, 9, 1000000));
    ASSERT_TRUE(pages.set({15, 15}, 0, 1000000));
    EXPECT_EQ(pages.sample({0, 0}, -1), 9);
    EXPECT_TRUE(pages.initialized({15, 15}));
    EXPECT_EQ(pages.pages().size(), FoliageFieldPages::kMaxPages);
    FoliageFieldPages restored;
    ASSERT_TRUE(FoliageFieldPages::parse_records(
        pages.records(), FoliageFieldPages::kMaxPages, 1000000, restored));
    EXPECT_EQ(restored.records(), pages.records());
}

TEST(FoliageFieldPages, RecordLengthsLimitsAndOrderingRefuseAtomically) {
    FoliageFieldPages pages;
    ASSERT_TRUE(pages.set({-1, -1}, 1, 1000000));
    ASSERT_TRUE(pages.set({16, 16}, 1000000, 1000000));
    const auto bytes = pages.records();
    FoliageFieldPages restored;
    ASSERT_TRUE(restored.set({512, 512}, 9, 1000000));
    const auto before = restored.records();
    const auto refuses = [&](const std::string& candidate, std::size_t count, std::int32_t limit) {
        EXPECT_FALSE(FoliageFieldPages::parse_records(candidate, count, limit, restored));
        EXPECT_EQ(restored.records(), before);
    };
    refuses({}, 0, 1000000);
    refuses(bytes, 0, 1000000);
    refuses(bytes.substr(0, bytes.size() - 1), 2, 1000000);
    refuses(bytes + '\0', 2, 1000000);
    refuses(bytes, 1, 1000000);
    refuses(bytes, 2, -1);
    refuses(bytes, 2, 999999);
    refuses(bytes, (std::numeric_limits<std::size_t>::max)(), 1000000);
    refuses(bytes.substr(FoliageFieldPages::kRecordBytes) +
                bytes.substr(0, FoliageFieldPages::kRecordBytes),
            2,
            1000000);
    auto negative = bytes;
    negative.replace(FoliageFieldPages::kRecordBytes - 4, 4, std::string(4, '\xff'));
    refuses(negative, 2, 1000000);
    ASSERT_TRUE(FoliageFieldPages::parse_records(bytes, 2, 1000000, restored));
    EXPECT_EQ(restored.pages(), pages.pages());
}
} // namespace
