#include "luminumbra_common/world/RegionRng.h"

#include <gtest/gtest.h>

#include <limits>

namespace {
using Luminumbra::world::RegionRng;
using Luminumbra::world::RegionRngTag;

TEST(RegionRng, VersionOneKnownAnswers) {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(RegionRng::kVersion, 1u);
    // Values pinned independently from the documented unsigned mixing recipe.
    EXPECT_EQ(RegionRng(0, {0, 0}, RegionRngTag::CreatureNeeds, 0).draw(0), 0x8c5206a620527dd8ull);
    EXPECT_EQ(RegionRng(0, {0, 0}, RegionRngTag::CreatureTravel, 0).draw(0), 0x4c55828bd8100161ull);
    const RegionRng negative(123, {-1, -32768}, RegionRngTag::CreatureNeeds, 9000, 42);
    EXPECT_EQ(negative.draw(0), 0x464509dfc0fd8d71ull);
    EXPECT_EQ(negative.draw(1), 0x10109867b3cf3918ull);
    EXPECT_EQ(RegionRng(max, {32767, -2}, RegionRngTag::CreatureTravel, max, max >> 1).draw(max),
              0x94ff14c8b716d580ull);
    EXPECT_EQ(RegionRng(123, {-1, -32768}, RegionRngTag::Population, 9000, 42).draw(0),
              0x4a08950ea413a0abull);
    EXPECT_EQ(RegionRng(123, {-1, -32768}, RegionRngTag::CreatureReproduction, 9000, 42).draw(0),
              0x3c6f0397c458b7b2ull);
}

TEST(RegionRng, IndexedDrawsHaveNoHiddenStateAndDomainsAreSeparate) {
    const RegionRng needs(123, {-1, -32768}, RegionRngTag::CreatureNeeds, 9000, 42);
    const auto first = needs.draw(0);
    (void)needs.draw(100);
    EXPECT_EQ(needs.draw(0), first);
    EXPECT_NE(RegionRng(123, {-1, -32768}, RegionRngTag::CreatureTravel, 9000, 42).draw(0), first);
    EXPECT_NE(RegionRng(123, {-1, -32768}, RegionRngTag::Population, 9000, 42).draw(0), first);
    EXPECT_NE(RegionRng(123, {-1, -32768}, RegionRngTag::CreatureReproduction, 9000, 42).draw(0),
              first);
    EXPECT_NE(RegionRng(123, {-32768, -1}, RegionRngTag::CreatureNeeds, 9000, 42).draw(0), first);
    EXPECT_NE(RegionRng(123, {-1, -32768}, RegionRngTag::CreatureNeeds, 9001, 42).draw(0), first);
    EXPECT_NE(RegionRng(123, {-1, -32768}, RegionRngTag::CreatureNeeds, 9000, 43).draw(0), first);
}
} // namespace
