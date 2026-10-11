#include "luminumbra_common/fields/FieldPageState.h"
#include "luminumbra_common/fields/FoliageFieldPages.h"
#include <gtest/gtest.h>

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
} // namespace
