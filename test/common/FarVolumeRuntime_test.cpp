#include "luminumbra_common/world/FarVolumeRuntime.h"

#include <gtest/gtest.h>

#include <limits>

namespace {
using namespace Luminumbra::World;
constexpr FarVolumeSpanLedgerLimits kLedgerLimits{-4096, 4096, 1024};
} // namespace

TEST(FarVolumeRuntime, KeyRoundTripIncludesEveryTierAndNegativeCoordinates) {
    for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier) {
        for (const auto coordinate : {-3, 0, 7}) {
            const FarTileKey key{tier, coordinate, -coordinate};
            FarVolumeTileKey volume;
            ASSERT_TRUE(ToFarVolumeTileKey(key, volume));
            EXPECT_EQ(volume, (FarVolumeTileKey{tier, coordinate, -coordinate}));
            FarTileKey roundtrip;
            ASSERT_TRUE(ToFarTileKey(volume, roundtrip));
            EXPECT_EQ(roundtrip, key);
        }
    }
}

TEST(FarVolumeRuntime, KeyRefusalRetainsOutputAtDomainAndIntegerBoundaries) {
    for (std::size_t tier = 1; tier <= kFarTierCount; ++tier) {
        const auto edge = kFarTierTable[tier - 1].tile_edge_meters;
        const auto limit = kFarVolumeExactCoordinateLimitMeters / edge;
        const auto t = static_cast<std::uint8_t>(tier);
        FarVolumeTileKey output{2, 4, 5};
        const auto previous = output;
        for (const auto x : {-limit,
                             limit - 1,
                             std::numeric_limits<std::int64_t>::min(),
                             std::numeric_limits<std::int64_t>::max()}) {
            EXPECT_FALSE(ToFarVolumeTileKey({t, x, 0}, output));
            EXPECT_EQ(output, previous);
        }
        EXPECT_TRUE(ToFarVolumeTileKey({t, -limit + 1, limit - 2}, output));
    }
    FarTileKey key{3, 4, 5};
    EXPECT_FALSE(ToFarTileKey({std::numeric_limits<std::size_t>::max(), 0, 0}, key));
    EXPECT_EQ(key, (FarTileKey{3, 4, 5}));
    FarVolumeTileKey volume{3, 4, 5};
    EXPECT_FALSE(ToFarVolumeTileKey({0, 0, 0}, volume));
    EXPECT_FALSE(ToFarVolumeTileKey({6, 0, 0}, volume));
    EXPECT_EQ(volume, (FarVolumeTileKey{3, 4, 5}));
}

TEST(FarVolumeRuntime, SpanUnionNeverShrinksAndGenerationOnlyTracksGrowth) {
    FarVolumeSpanLedger ledger;
    EXPECT_FALSE(ledger.Intent());
    EXPECT_FALSE(ledger.Extend({0, 1}, kLedgerLimits));
    ASSERT_TRUE(ledger.Reset({1, -1, 2}));
    EXPECT_EQ(ledger.generation(), 0u);
    ASSERT_TRUE(ledger.Extend({100, 100}, kLedgerLimits));
    EXPECT_EQ(ledger.generation(), 1u);
    ASSERT_TRUE(ledger.Extend({-200, 400}, kLedgerLimits));
    EXPECT_EQ(ledger.generation(), 2u);
    ASSERT_TRUE(ledger.Extend({0, 20}, kLedgerLimits));
    EXPECT_EQ(ledger.generation(), 2u);
    ASSERT_TRUE(ledger.Intent());
    EXPECT_EQ(ledger.Intent()->extra_span, (FarVolumeCoverageSpan{-200, 400}));
    EXPECT_FALSE(ledger.Covers({-192, 400}));
    EXPECT_FALSE(ledger.Covers({-208, 384}));
    EXPECT_TRUE(ledger.Covers({-208, 400}));
    EXPECT_FALSE(ledger.Covers({-209, 400}));
}

TEST(FarVolumeRuntime, SpanGrowthRefusalIsTransactionalIncludingInvalidReset) {
    FarVolumeSpanLedger ledger;
    ASSERT_TRUE(ledger.Reset({2, 1, -2}));
    ASSERT_TRUE(ledger.Extend({-128, 128}, kLedgerLimits));
    const auto previous = ledger.Intent();
    const auto generation = ledger.generation();
    for (const auto span : {FarVolumeCoverageSpan{-4097, 0},
                            {0, 4097},
                            {-600, 600},
                            {1, -1},
                            {0, std::numeric_limits<float>::infinity()},
                            {std::numeric_limits<float>::quiet_NaN(), 0}}) {
        EXPECT_FALSE(ledger.Extend(span, kLedgerLimits));
        EXPECT_EQ(ledger.Intent(), previous);
        EXPECT_EQ(ledger.generation(), generation);
    }
    EXPECT_FALSE(ledger.Reset({0, 0, 0}));
    EXPECT_EQ(ledger.Intent(), previous);
    EXPECT_EQ(ledger.generation(), generation);
    EXPECT_FALSE(ledger.Extend({0, 0}, {-4096, 4096, 0}));
    EXPECT_FALSE(ledger.Extend({0, 0}, {4096, -4096, 1024}));
    EXPECT_FALSE(ledger.Extend({0, 0}, {-8388608, 8388608, 1024}));
    EXPECT_EQ(ledger.Intent(), previous);
    EXPECT_EQ(ledger.generation(), generation);
    ASSERT_TRUE(ledger.Reset({3, -1, 1}));
    EXPECT_EQ(ledger.generation(), 0u);
    EXPECT_FALSE(ledger.Intent()->extra_span);
}

TEST(FarVolumeRuntime, RequestOrderProducesIdenticalUnionAndResolvedCoverage) {
    FarVolumeSpanLedger first, second;
    ASSERT_TRUE(first.Reset({3, 0, 0}));
    ASSERT_TRUE(second.Reset({3, 0, 0}));
    for (const auto span : {FarVolumeCoverageSpan{-256, 0}, {0, 256}, {-128, 384}})
        ASSERT_TRUE(first.Extend(span, kLedgerLimits));
    for (const auto span : {FarVolumeCoverageSpan{-128, 384}, {0, 256}, {-256, 0}})
        ASSERT_TRUE(second.Extend(span, kLedgerLimits));
    EXPECT_EQ(first.Intent(), second.Intent());
    EXPECT_TRUE(first.Covers({-256, 384}));
    EXPECT_TRUE(second.Covers({-256, 384}));
    // Generations count actual growth events; each caller binds the local value.
    EXPECT_EQ(first.generation(), 3u);
    EXPECT_EQ(second.generation(), 2u);
}
