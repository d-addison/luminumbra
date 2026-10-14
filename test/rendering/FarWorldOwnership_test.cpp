#include "luminumbra_client/rendering/FarWorldOwnership.h"
#include "luminumbra_common/world/FarWantedSet.h"

#include <cmath>
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;
FarWorldLiveUpload Current(FarWorldLiveCell cell) {
    return {cell, 42, 42, 3, true};
}
} // namespace

TEST(FarWorldOwnership, CurrentStaleMissingFailedEvictedAndEmptyUploads) {
    auto current = Current({0, 0, 0, 0});
    EXPECT_TRUE(IsFarWorldLiveUploadCurrent(current));
    auto stale = current;
    --stale.uploaded_version;
    EXPECT_FALSE(IsFarWorldLiveUploadCurrent(stale));
    auto missing = current;
    missing.valid_allocation = false;
    EXPECT_FALSE(IsFarWorldLiveUploadCurrent(missing));
    auto empty = current;
    empty.uploaded_indices = 0;
    EXPECT_FALSE(IsFarWorldLiveUploadCurrent(empty));
    FarWorldOwnership mask;
    mask.Begin(1);
    ASSERT_TRUE(mask.Add(current));
    ASSERT_TRUE(mask.Finish());
    EXPECT_TRUE(mask.Owns({8, 8, 8}));
    // A failed rebuild keeps the old upload, which cannot establish new coverage.
    mask.Begin(2);
    ASSERT_TRUE(mask.Add(stale));
    ASSERT_TRUE(mask.Finish());
    EXPECT_FALSE(mask.Owns({8, 8, 8}));
    mask.Begin(3);
    ASSERT_TRUE(mask.Add(missing)); // evicted allocation
    ASSERT_TRUE(mask.Add(empty));
    ASSERT_TRUE(mask.Finish());
    EXPECT_TRUE(mask.cells().empty());
}

TEST(FarWorldOwnership, CheckedNegativeHalfOpenChunkBoundaries) {
    FarWorldOwnership mask;
    mask.Begin(1);
    ASSERT_TRUE(mask.Add(Current({-1, -1, -1, 0})));
    ASSERT_TRUE(mask.Finish());
    EXPECT_TRUE(mask.Owns({-16, -16, -16}));
    EXPECT_TRUE(mask.Owns({-.001, -.001, -.001}));
    EXPECT_FALSE(mask.Owns({-16.001, -1, -1}));
    EXPECT_FALSE(mask.Owns({0, -1, -1}));
    EXPECT_FALSE(mask.Owns({-1, 0, -1}));
    EXPECT_FALSE(mask.Owns({-1, -1, 0}));
    FarWorldLiveCell cell{9, 9, 9, 0};
    EXPECT_FALSE(FarWorldChunkCell({std::numeric_limits<double>::infinity(), 0, 0}, cell));
    EXPECT_FALSE(FarWorldChunkCell({1e30, 0, 0}, cell));
    EXPECT_EQ(cell, (FarWorldLiveCell{9, 9, 9, 0}));
}

TEST(FarWorldOwnership, FloorDoesNotOwnUnmeshedCeilingOrDeeperCaveInCameraRegion) {
    FarWorldOwnership mask;
    mask.Begin(1);
    ASSERT_TRUE(mask.Add(Current({0, -2, 0, 0})));
    ASSERT_TRUE(mask.Finish());
    EXPECT_TRUE(mask.Owns({8, -24, 8}));
    EXPECT_FALSE(mask.Owns({8, 8, 8}));
    EXPECT_FALSE(mask.Owns({8, -48, 8}));
    // Same XZ camera region, independently uploaded ceiling establishes its own cell.
    mask.Begin(2);
    ASSERT_TRUE(mask.Add(Current({0, -2, 0, 0})));
    ASSERT_TRUE(mask.Add(Current({0, 0, 0, 0})));
    ASSERT_TRUE(mask.Finish());
    EXPECT_TRUE(mask.Owns({8, 8, 8}));
    EXPECT_FALSE(mask.Owns({8, -8, 8}));
}

TEST(FarWorldOwnership, IncompleteOrOverLimitMaskCannotPublishOrReusePriorFrame) {
    FarWorldOwnership mask;
    mask.Begin(1, 1);
    ASSERT_TRUE(mask.Add(Current({0, 0, 0, 0})));
    EXPECT_FALSE(mask.Owns({1, 1, 1})); // unfinished
    ASSERT_TRUE(mask.Finish());
    EXPECT_TRUE(mask.Owns({1, 1, 1}));
    mask.Begin(2, 1);
    ASSERT_TRUE(mask.Add(Current({0, 0, 0, 0})));
    EXPECT_FALSE(mask.Add(Current({1, 0, 0, 0})));
    EXPECT_FALSE(mask.Finish());
    EXPECT_FALSE(mask.complete());
    EXPECT_TRUE(mask.cells().empty());
    EXPECT_FALSE(mask.Owns({1, 1, 1}));
    mask.Begin(3, kFarWorldLiveCellLimit + 1);
    EXPECT_FALSE(mask.Finish());
    mask.Begin(4);
    auto missing = Current({0, 0, 0, 0});
    missing.valid_allocation = false;
    for (std::size_t i = 0; i < kFarWorldLiveCellLimit; ++i)
        ASSERT_TRUE(mask.Add(missing));
    EXPECT_FALSE(mask.Add(missing)); // callback work is bounded even for stale/missing candidates
    EXPECT_FALSE(mask.Finish());
}

TEST(FarWorldOwnership, SortedUniqueCellsMatchBoundedGpuLayout) {
    FarWorldOwnership mask;
    mask.Begin(1);
    for (const auto& cell : std::array{FarWorldLiveCell{1, -1, 0, 0},
                                       FarWorldLiveCell{-1, 3, -2, 0},
                                       FarWorldLiveCell{1, -1, 0, 0}})
        ASSERT_TRUE(mask.Add(Current(cell)));
    ASSERT_TRUE(mask.Finish());
    ASSERT_EQ(mask.cells().size(), 2u);
    EXPECT_EQ(mask.cells()[0], (FarWorldLiveCell{-1, 3, -2, 0}));
    EXPECT_EQ(mask.cells()[1], (FarWorldLiveCell{1, -1, 0, 0}));
    EXPECT_EQ(kFarWorldOwnershipGpuBytes, 131072u);
}

TEST(FarWorldArrival, V2OriginAndNegativeTilesRetainFragmentsOutsideFinerRadius) {
    FarWorldArrival arrival{true, FarWantedRadiusMeters(1), {}};
    arrival.children.fill({-256, 256});
    for (const double sign : {-1.0, 1.0}) {
        // Origin tile or (-1,-1): near point replaces; the 1448 m corner cannot.
        EXPECT_TRUE(FarWorldArrivalReplaces(arrival, 1024, {1, 0, 1}, 0, sign, sign));
        EXPECT_FALSE(
            FarWorldArrivalReplaces(arrival, 1024, {1023, 0, 1023}, 0, sign * 1023, sign * 1023));
        EXPECT_TRUE(FarWorldArrivalReplaces(arrival, 1024, {500, 0, 500}, 0, sign * 1040, 0));
        EXPECT_FALSE(FarWorldArrivalReplaces(arrival, 1024, {500, 0, 500}, 0, sign * 1040.001, 0));
    }
}

TEST(FarWorldArrival, RelevantChildBuiltSpanAndHalfOpenQuadrants) {
    FarWorldArrival arrival{true, 1040, {{{-20, 0}, {0, 20}, {-40, -20}, {20, 40}}}};
    EXPECT_TRUE(FarWorldArrivalReplaces(arrival, 1024, {511, -10, 511}, 0, 0, 0));
    EXPECT_FALSE(FarWorldArrivalReplaces(arrival, 1024, {512, -10, 511}, 0, 0, 0));
    EXPECT_TRUE(FarWorldArrivalReplaces(arrival, 1024, {512, 20, 511}, 0, 0, 0));
    EXPECT_TRUE(FarWorldArrivalReplaces(arrival, 1024, {511, -40, 512}, 0, 0, 0));
    EXPECT_TRUE(FarWorldArrivalReplaces(arrival, 1024, {512, 40, 512}, 0, 0, 0));
    EXPECT_FALSE(FarWorldArrivalReplaces(arrival, 1024, {1024, 40, 512}, 0, 0, 0));
    arrival.eligible = false; // partial, stale, failed or evicted child: next frame fallback
    EXPECT_FALSE(FarWorldArrivalReplaces(arrival, 1024, {511, -10, 511}, 0, 0, 0));
}

TEST(FarWorldOwnership, ExactSphericalInnerAndHorizontalOuterLimitsForEveryTier) {
    EXPECT_FALSE(FarWorldFragmentInClip({0, 0, -175.999}, 1040));
    EXPECT_TRUE(FarWorldFragmentInClip({0, 0, -176}, 1040));
    EXPECT_TRUE(FarWorldFragmentInClip({0, 176, 0}, 1040));
    EXPECT_FALSE(FarWorldFragmentInClip({0, 175.999, 0}, 1040));
    for (std::size_t tier = 1; tier <= kFarTierCount; ++tier) {
        const auto radius = FarWantedRadiusMeters(tier);
        EXPECT_TRUE(FarWorldFragmentInClip({radius, 500, 0}, radius));
        EXPECT_FALSE(FarWorldFragmentInClip({radius + .001, 500, 0}, radius));
    }
    EXPECT_EQ(FarWantedRadiusMeters(5), 16384);
}

TEST(FarWorldOwnership, TileLocalCellMirrorRejectsFractionalCameraRoundTrip) {
    // Reproduce the rereview's float32 counterexample. The old shader used
    // (-256 - 83.32f) + 256 + camera remainder, and claimed cell -1 at world Y=0.
    const float camera_y = 83.32f;
    const float relative_origin_y = static_cast<float>(-256.0 - static_cast<double>(camera_y));
    const float relative_y = relative_origin_y + 256.0f;
    const auto camera_key_y = static_cast<std::int32_t>(std::floor(camera_y / 16.0f));
    const float camera_remainder_y = camera_y - static_cast<float>(camera_key_y) * 16.0f;
    EXPECT_EQ(camera_key_y +
                  static_cast<std::int32_t>(std::floor((relative_y + camera_remainder_y) / 16.0f)),
              -1);

    struct Boundary {
        std::array<double, 3> origin;
        std::array<float, 3> local;
        FarWorldLiveCell expected;
        std::size_t axis;
    };
    const std::array boundaries{
        Boundary{{0, -256, -1024}, {8, 256, 824}, {0, 0, -13, 0}, 1},
        Boundary{{-1024, -512, -1024}, {1016, 496, 824}, {-1, -1, -13, 0}, 1},
        Boundary{{-1024, -256, -1024}, {1024, 232, 824}, {0, -2, -13, 0}, 0},
        Boundary{{-1024, -256, -1024}, {1008, 232, 824}, {-1, -2, -13, 0}, 0},
        Boundary{{-1024, -256, -1024}, {1016, 232, 1024}, {-1, -2, 0, 0}, 2},
        Boundary{{-1024, -256, -1024}, {1016, 232, 816}, {-1, -2, -13, 0}, 2}};
    for (const double translation : {0.0, 4194304.0, -4194304.0}) {
        for (const auto& boundary : boundaries) {
            auto tile_origin = boundary.origin;
            for (auto& coordinate : tile_origin)
                coordinate += translation;
            FarWorldCellOrigin origin;
            ASSERT_TRUE(MakeFarWorldCellOrigin(tile_origin, origin));
            // Canonical tile/span origins are multiples of 16, so remainder=0;
            // zero + stored local and binary /16 are exact. Integer boundary
            // locals are <2^24; translated keys never pass through float.
            EXPECT_EQ(origin.remainder, (std::array<float, 3>{0, 0, 0}));
            for (std::size_t axis = 0; axis < boundary.local.size(); ++axis) {
                EXPECT_EQ(static_cast<double>(origin.remainder[axis] + boundary.local[axis]),
                          static_cast<double>(boundary.local[axis]));
                EXPECT_EQ(static_cast<double>(boundary.local[axis] / 16.0f),
                          static_cast<double>(boundary.local[axis]) / 16.0);
            }
            auto expected = boundary.expected;
            const auto key_shift = static_cast<std::int32_t>(translation / 16.0);
            expected.x += key_shift;
            expected.y += key_shift;
            expected.z += key_shift;
            FarWorldLiveCell actual;
            ASSERT_TRUE(FarWorldChunkCell(origin, boundary.local, actual));
            EXPECT_EQ(actual, expected);
            FarWorldOwnership mask;
            mask.Begin(1);
            ASSERT_TRUE(mask.Add(Current(expected)));
            ASSERT_TRUE(mask.Finish());
            EXPECT_TRUE(mask.Owns(origin, boundary.local));
            auto below = boundary.local;
            below[boundary.axis] -= .125f;
            EXPECT_FALSE(mask.Owns(origin, below));
            auto above = boundary.local;
            above[boundary.axis] += .125f;
            EXPECT_TRUE(mask.Owns(origin, above));
        }
    }
}

TEST(FarWorldOwnership, ExactOriginRemainderAndNegativeLocalFloorSemantics) {
    FarWorldCellOrigin origin;
    ASSERT_TRUE(MakeFarWorldCellOrigin({-257, -1, -513}, origin));
    EXPECT_EQ(origin.key, (FarWorldLiveCell{-17, -1, -33, 0}));
    EXPECT_EQ(origin.remainder, (std::array<float, 3>{15, 15, 15}));
    FarWorldLiveCell cell;
    ASSERT_TRUE(FarWorldChunkCell(origin, {1, 1, 1}, cell));
    EXPECT_EQ(cell, (FarWorldLiveCell{-16, 0, -32, 0}));
    ASSERT_TRUE(FarWorldChunkCell(origin, {-16, -16, -16}, cell));
    EXPECT_EQ(cell, (FarWorldLiveCell{-18, -2, -34, 0}));
    const auto previous = origin;
    EXPECT_FALSE(MakeFarWorldCellOrigin({1e30, 0, 0}, origin));
    EXPECT_EQ(origin.key, previous.key);
    EXPECT_EQ(origin.remainder, previous.remainder);
    EXPECT_FALSE(MakeFarWorldCellOrigin({.1, 0, 0}, origin)); // inexact float remainder
    EXPECT_FALSE(FarWorldChunkCell(origin, {std::numeric_limits<float>::infinity(), 0, 0}, cell));
    origin.key.x = std::numeric_limits<std::int32_t>::max();
    EXPECT_FALSE(FarWorldChunkCell(origin, {16, 0, 0}, cell)); // checked integer-key addition
}

TEST(FarWorldArrival, TileLocalSpanAndChildBoundariesSurviveLargeTranslation) {
    for (const std::int64_t translation : {0, 4194304, -4194304}) {
        FarWorldArrival arrival{true, 1040, {}};
        // Coarse span begins at -256. Child 3 covers [0,256], so the exact Y=0
        // surface has local Y=256 regardless of camera Y=83.32f or translation.
        arrival.children.fill({translation + 16, translation + 32});
        arrival.children[3] = {translation, translation + 256};
        const auto origin_y = translation - 256;
        EXPECT_TRUE(FarWorldArrivalReplaces(arrival, 1024, {512, 256, 512}, origin_y, 200, 300));
        EXPECT_FALSE(
            FarWorldArrivalReplaces(arrival, 1024, {512, 255.875f, 512}, origin_y, 200, 300));
        EXPECT_TRUE(
            FarWorldArrivalReplaces(arrival, 1024, {512, 256.125f, 512}, origin_y, 200, 300));
        EXPECT_FALSE(
            FarWorldArrivalReplaces(arrival, 1024, {511.875f, 256, 512}, origin_y, 200, 300));
        EXPECT_FALSE(
            FarWorldArrivalReplaces(arrival, 1024, {512, 256, 511.875f}, origin_y, 200, 300));
        EXPECT_FALSE(FarWorldArrivalReplaces(arrival, 1024, {1024, 256, 512}, origin_y, 200, 300));
    }
}
