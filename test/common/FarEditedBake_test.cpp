#include "luminumbra_common/world/FarEditedBake.h"
#include "luminumbra_common/world/FarVolume.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra::World;
using Error = FarEditedBakeError;

FarEditedBakeLimits Limits() {
    FarEditedBakeLimits limits;
    limits.max_edits = 32;
    limits.max_source_records = 32;
    limits.max_affected_bricks = 100000;
    limits.max_tiles = 256;
    limits.max_brick_visits_per_tier.fill(100000);
    limits.max_source_samples = 100000;
    limits.max_source_probes = 10000000;
    limits.max_density_samples = 2000000;
    limits.max_scratch_bytes = 32 * 1024 * 1024;
    limits.max_output_bytes = 64 * 1024 * 1024;
    return limits;
}

struct OwnedRecord {
    FarVolumePosition origin;
    std::uint32_t spacing = 16;
    std::array<std::uint32_t, 3> side{2, 2, 2};
    std::vector<float> density;
    std::vector<std::uint8_t> material;
    std::uint64_t generation = 7;
};

// Owns every byte; the views reference only this test image, never a world.
class TestSource final : public FarEditedSourceView {
public:
    FarEditedImageIdentity identity{12, FarEditedImageState::Prepared};
    std::vector<OwnedRecord> owned;
    std::vector<FarEditedLattice> records;

    void Add(const FarVolumePosition& origin,
             std::uint32_t spacing,
             const std::function<float(const FarVolumePosition&)>& field,
             std::uint8_t material = 3,
             std::uint32_t side = 2) {
        OwnedRecord record;
        record.origin = origin;
        record.spacing = spacing;
        record.side.fill(side);
        for (std::uint32_t z = 0; z < side; ++z) {
            for (std::uint32_t y = 0; y < side; ++y) {
                for (std::uint32_t x = 0; x < side; ++x) {
                    record.density.push_back(
                        field({origin.x + static_cast<std::int64_t>(x) * spacing,
                               origin.y + static_cast<std::int64_t>(y) * spacing,
                               origin.z + static_cast<std::int64_t>(z) * spacing}));
                    record.material.push_back(material);
                }
            }
        }
        owned.push_back(std::move(record));
    }

    void Refresh() {
        records.clear();
        for (const auto& record : owned)
            records.push_back({record.origin,
                               record.spacing,
                               record.side,
                               record.density,
                               record.material,
                               record.generation});
    }

    FarEditedImageIdentity Identity() const override {
        return identity;
    }
    std::span<const FarEditedLattice> Lattices() const override {
        return records;
    }
};

FarEditedPristineSampler Constant(float density, std::uint8_t material = 9) {
    return [=](std::uint8_t, const FarVolumePosition&) {
        return std::optional<FarVolumeSample>{{density, material}};
    };
}

FarEditedFootprint Footprint(const OwnedRecord& record) {
    return {record.origin,
            {record.origin.x + (record.side[0] - 1) * static_cast<std::int64_t>(record.spacing),
             record.origin.y + (record.side[1] - 1) * static_cast<std::int64_t>(record.spacing),
             record.origin.z + (record.side[2] - 1) * static_cast<std::int64_t>(record.spacing)}};
}

std::vector<FarOverlayTile> Overlays(const FarEditedBakeResult& result) {
    std::vector<FarOverlayTile> tiles;
    for (const auto& tile : result.tiles)
        tiles.push_back(tile.overlay);
    return tiles;
}

const FarOverlayBrick* FindBrick(const FarEditedBakeResult& result,
                                 std::uint8_t tier,
                                 std::int32_t bx,
                                 std::int32_t by,
                                 std::int32_t bz) {
    for (const auto& tile : result.tiles) {
        if (tile.overlay.key.tier == tier) {
            for (const auto& brick : tile.overlay.bricks) {
                if (brick.bx == bx && brick.by == by && brick.bz == bz)
                    return &brick;
            }
        }
    }
    return nullptr;
}

// Independent brute-force oracle: scan a generous fixed window around each
// edit, compute interval separation per axis, and admit core intersections or
// one nonzero separation within a normal step. No production range/intersection
// helper is used. Floor is evaluated in double in this small exact domain.
std::vector<FarEditedBrickKey> Oracle(std::span<const FarEditedFootprint> edits) {
    std::vector<FarEditedBrickKey> expected;
    for (std::size_t t = 0; t < kFarTierCount; ++t) {
        const auto edge = static_cast<std::int64_t>(kFarTierTable[t].brick_edge_meters);
        const auto spacing = kFarTierTable[t].sample_spacing_meters;
        for (const auto& edit : edits) {
            const std::array<std::int64_t, 3> min{edit.min.x, edit.min.y, edit.min.z};
            const std::array<std::int64_t, 3> max{edit.max.x, edit.max.y, edit.max.z};
            const auto x0 =
                static_cast<std::int64_t>(std::floor(static_cast<double>(edit.min.x) / edge));
            const auto y0 =
                static_cast<std::int64_t>(std::floor(static_cast<double>(edit.min.y) / edge));
            const auto z0 =
                static_cast<std::int64_t>(std::floor(static_cast<double>(edit.min.z) / edge));
            for (auto bz = z0 - 4; bz <= z0 + 4; ++bz) {
                for (auto bx = x0 - 4; bx <= x0 + 4; ++bx) {
                    for (auto by = y0 - 4; by <= y0 + 4; ++by) {
                        const std::array<std::int64_t, 3> low{bx * edge, by * edge, bz * edge};
                        std::int64_t separation = 0;
                        int separated_axes = 0;
                        for (std::size_t axis = 0; axis < low.size(); ++axis) {
                            const auto gap = std::max({std::int64_t{0},
                                                       min[axis] - low[axis] - edge,
                                                       low[axis] - max[axis]});
                            separation += gap;
                            separated_axes += gap > 0 ? 1 : 0;
                        }
                        if (separated_axes <= 1 && separation <= spacing) {
                            expected.push_back({{static_cast<std::uint8_t>(t + 1),
                                                 static_cast<std::int64_t>(
                                                     std::floor(static_cast<double>(bx) / 32)),
                                                 static_cast<std::int64_t>(
                                                     std::floor(static_cast<double>(bz) / 32))},
                                                static_cast<std::int32_t>(bx),
                                                static_cast<std::int32_t>(by),
                                                static_cast<std::int32_t>(bz)});
                        }
                    }
                }
            }
        }
    }
    std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
        return std::tie(a.tile.tier, a.tile.tz, a.tile.tx, a.bz, a.bx, a.by) <
               std::tie(b.tile.tier, b.tile.tz, b.tile.tx, b.bz, b.bx, b.by);
    });
    expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
    return expected;
}

void ExpectSame(const FarEditedBakeResult& a, const FarEditedBakeResult& b) {
    EXPECT_EQ(a.affected.bricks, b.affected.bricks);
    EXPECT_EQ(a.affected.tiles, b.affected.tiles);
    EXPECT_EQ(a.source.generation, b.source.generation);
    EXPECT_EQ(a.source.state, b.source.state);
    ASSERT_EQ(a.tiles.size(), b.tiles.size());
    for (std::size_t i = 0; i < a.tiles.size(); ++i) {
        EXPECT_EQ(a.tiles[i].bytes, b.tiles[i].bytes);
        EXPECT_EQ(a.tiles[i].max_member_generation, b.tiles[i].max_member_generation);
        EXPECT_EQ(a.tiles[i].edited_span, b.tiles[i].edited_span);
    }
}
void ExpectNoAffectedWork(const FarEditedAffectedWork& work) {
    EXPECT_EQ(work.reserved_brick_slots, 0u);
    EXPECT_EQ(work.reserved_tile_slots, 0u);
    EXPECT_EQ(work.unique_bricks, 0u);
    EXPECT_EQ(work.unique_tiles, 0u);
    EXPECT_EQ(work.candidate_visits_per_tier, (std::array<std::uint64_t, kFarTierCount>{}));
}
} // namespace

TEST(FarEditedBake, IndependentOracleAllTiersNegativeFacesEdgesCornersAndHalos) {
    for (const auto& tier : kFarTierTable) {
        const auto boundary = -static_cast<std::int64_t>(tier.tile_edge_meters);
        const auto step = static_cast<std::int64_t>(tier.sample_spacing_meters);
        const std::array<FarEditedFootprint, 5> edits{
            {{{boundary, -3, -3}, {boundary, 3, 3}},
             {{boundary, boundary, -3}, {boundary, boundary, 3}},
             {{boundary, boundary, boundary}, {boundary, boundary, boundary}},
             {{boundary - 2, -2, boundary - 2}, {boundary + 2, 2, boundary + 2}},
             {{boundary + step, step, boundary + step}, {boundary + step, step, boundary + step}}}};
        for (const auto& edit : edits) {
            const std::array input{edit};
            FarEditedAffectedSet affected;
            ASSERT_EQ(ComputeFarEditedAffectedSet(input, Limits(), affected), Error::None);
            EXPECT_EQ(affected.bricks, Oracle(input));
            for (std::uint8_t t = 1; t <= kFarTierCount; ++t)
                EXPECT_TRUE(std::any_of(affected.tiles.begin(),
                                        affected.tiles.end(),
                                        [=](const auto& key) { return key.tier == t; }));
        }
    }
}

TEST(FarEditedBake, HugeValidFootprintZeroCapsRefuseBeforeAllocationAndEnumeration) {
    const std::array<FarEditedFootprint, 1> huge{
        {{{-10000, -10000, -10000}, {10000, 10000, 10000}}}};
    const std::array<FarEditedFootprint, 1> seed{{{{0, 0, 0}, {0, 0, 0}}}};
    FarEditedAffectedSet previous;
    ASSERT_EQ(ComputeFarEditedAffectedSet(seed, Limits(), previous), Error::None);
    for (const bool zero_bricks : {false, true}) {
        auto limits = Limits();
        limits.max_brick_visits_per_tier.fill(std::numeric_limits<std::uint64_t>::max());
        limits.max_scratch_bytes = std::numeric_limits<std::uint64_t>::max();
        limits.max_affected_bricks = std::numeric_limits<std::size_t>::max();
        limits.max_tiles = std::numeric_limits<std::size_t>::max();
        if (zero_bricks)
            limits.max_affected_bricks = 0;
        else
            limits.max_tiles = 0;
        auto affected = previous;
        FarEditedAffectedWork work;
        work.reserved_brick_slots = 99; // Reset even on early refusal.
        EXPECT_EQ(ComputeFarEditedAffectedSet(huge, limits, affected, &work), Error::WorkLimit);
        ExpectNoAffectedWork(work);
        EXPECT_EQ(affected.bricks, previous.bricks);
        EXPECT_EQ(affected.tiles, previous.tiles);
    }
}

TEST(FarEditedBake, ExactFootprintCapsIncludeHalosAndAllTiersBeforeEnumeration) {
    for (const auto& tier : kFarTierTable) {
        const auto step = static_cast<std::int64_t>(tier.sample_spacing_meters);
        const auto boundary = -static_cast<std::int64_t>(tier.tile_edge_meters);
        const std::array<FarEditedFootprint, 1> input{
            {{{boundary + step, step, boundary + step}, {boundary + step, step, boundary + step}}}};
        const auto expected = Oracle(input);
        std::vector<FarTileKey> expected_tiles;
        for (const auto& brick : expected) {
            if (expected_tiles.empty() || expected_tiles.back() != brick.tile)
                expected_tiles.push_back(brick.tile);
        }
        auto limits = Limits();
        limits.max_affected_bricks = expected.size();
        limits.max_tiles = expected_tiles.size();
        FarEditedAffectedSet affected;
        FarEditedAffectedWork work;
        ASSERT_EQ(ComputeFarEditedAffectedSet(input, limits, affected, &work), Error::None);
        EXPECT_EQ(affected.bricks, expected);
        EXPECT_EQ(affected.tiles, expected_tiles);
        EXPECT_EQ(work.reserved_brick_slots, expected.size());
        EXPECT_EQ(work.reserved_tile_slots, expected_tiles.size());
        const auto previous = affected;
        for (const bool fewer_bricks : {false, true}) {
            auto refused = limits;
            if (fewer_bricks)
                --refused.max_affected_bricks;
            else
                --refused.max_tiles;
            EXPECT_EQ(ComputeFarEditedAffectedSet(input, refused, affected, &work),
                      Error::WorkLimit);
            ExpectNoAffectedWork(work);
            EXPECT_EQ(affected.bricks, previous.bricks);
            EXPECT_EQ(affected.tiles, previous.tiles);
        }
    }
}

TEST(FarEditedBake, DisjointFootprintsBoundUniqueGrowthAtExactCapsDuringEnumeration) {
    const std::array<FarEditedFootprint, 2> input{
        {{{-4096, 0, -4096}, {-4096, 0, -4096}}, {{4096, 0, 4096}, {4096, 0, 4096}}}};
    const auto expected = Oracle(input);
    FarEditedAffectedSet previous;
    auto limits = Limits();
    limits.max_affected_bricks = expected.size();
    ASSERT_EQ(ComputeFarEditedAffectedSet(input, limits, previous), Error::None);
    EXPECT_EQ(previous.bricks, expected);
    limits.max_tiles = previous.tiles.size();
    FarEditedAffectedSet affected;
    FarEditedAffectedWork work;
    ASSERT_EQ(ComputeFarEditedAffectedSet(input, limits, affected, &work), Error::None);
    for (const bool fewer_bricks : {false, true}) {
        auto refused = limits;
        if (fewer_bricks)
            --refused.max_affected_bricks;
        else
            --refused.max_tiles;
        EXPECT_EQ(ComputeFarEditedAffectedSet(input, refused, affected, &work), Error::WorkLimit);
        EXPECT_TRUE(std::any_of(work.candidate_visits_per_tier.begin(),
                                work.candidate_visits_per_tier.end(),
                                [](auto visits) { return visits != 0; }));
        EXPECT_LE(work.reserved_brick_slots, refused.max_affected_bricks);
        EXPECT_LE(work.reserved_tile_slots, refused.max_tiles);
        EXPECT_LE(work.unique_bricks, refused.max_affected_bricks);
        EXPECT_LE(work.unique_tiles, refused.max_tiles);
        if (fewer_bricks)
            EXPECT_EQ(work.unique_bricks, refused.max_affected_bricks);
        else
            EXPECT_EQ(work.unique_tiles, refused.max_tiles);
        EXPECT_EQ(affected.bricks, previous.bricks);
        EXPECT_EQ(affected.tiles, previous.tiles);
    }
}

TEST(FarEditedBake, DuplicateFootprintsUseOnlyAdmittedUniqueStorage) {
    const FarEditedFootprint edit{{4, 4, 4}, {4, 4, 4}};
    std::array<FarEditedFootprint, 32> input;
    input.fill(edit);
    const auto expected = Oracle(input);
    FarEditedAffectedSet single;
    ASSERT_EQ(ComputeFarEditedAffectedSet(std::span(&edit, 1), Limits(), single), Error::None);
    auto limits = Limits();
    limits.max_affected_bricks = single.bricks.size();
    limits.max_tiles = single.tiles.size();
    limits.max_scratch_bytes =
        single.bricks.size() * sizeof(FarEditedBrickKey) + single.tiles.size() * sizeof(FarTileKey);
    FarEditedAffectedSet affected;
    FarEditedAffectedWork work;
    ASSERT_EQ(ComputeFarEditedAffectedSet(input, limits, affected, &work), Error::None);
    EXPECT_EQ(affected.bricks, expected);
    EXPECT_EQ(affected.tiles, single.tiles);
    EXPECT_EQ(work.reserved_brick_slots, single.bricks.size());
    EXPECT_EQ(work.reserved_tile_slots, single.tiles.size());
    EXPECT_EQ(work.unique_bricks, single.bricks.size());
    EXPECT_EQ(work.unique_tiles, single.tiles.size());
}

TEST(FarEditedBake, OverflowEdgeFootprintsRefuseBeforeAllocationAndEnumeration) {
    const auto edge = kFarVolumeExactCoordinateLimitMeters - 1;
    const std::array<FarEditedFootprint, 1> huge{{{{-edge, -edge, -edge}, {edge, edge, edge}}}};
    auto limits = Limits();
    limits.max_brick_visits_per_tier.fill(std::numeric_limits<std::uint64_t>::max());
    limits.max_scratch_bytes = std::numeric_limits<std::uint64_t>::max();
    FarEditedAffectedSet previous;
    const std::array<FarEditedFootprint, 1> seed{{{{0, 0, 0}, {0, 0, 0}}}};
    ASSERT_EQ(ComputeFarEditedAffectedSet(seed, Limits(), previous), Error::None);
    auto affected = previous;
    FarEditedAffectedWork work;
    EXPECT_EQ(ComputeFarEditedAffectedSet(huge, limits, affected, &work), Error::WorkLimit);
    ExpectNoAffectedWork(work);
    EXPECT_EQ(affected.bricks, previous.bricks);
    EXPECT_EQ(affected.tiles, previous.tiles);
    // Counts fit in 64 bits, but the requested key byte product does not.
    if (std::numeric_limits<std::size_t>::max() == std::numeric_limits<std::uint64_t>::max()) {
        auto unrestricted = limits;
        unrestricted.max_affected_bricks = std::numeric_limits<std::size_t>::max();
        unrestricted.max_tiles = std::numeric_limits<std::size_t>::max();
        EXPECT_EQ(ComputeFarEditedAffectedSet(huge, unrestricted, affected, &work),
                  Error::MemoryLimit);
        ExpectNoAffectedWork(work);
        EXPECT_EQ(affected.bricks, previous.bricks);
        EXPECT_EQ(affected.tiles, previous.tiles);
    }
    for (const auto coordinate :
         {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
        const std::array<FarEditedFootprint, 1> outside{
            {{{coordinate, coordinate, coordinate}, {coordinate, coordinate, coordinate}}}};
        EXPECT_EQ(ComputeFarEditedAffectedSet(outside, limits, affected, &work),
                  Error::CoordinateRange);
        ExpectNoAffectedWork(work);
        EXPECT_EQ(affected.bricks, previous.bricks);
        EXPECT_EQ(affected.tiles, previous.tiles);
    }
}

TEST(FarEditedBake, ExactSharedTileCornerIncludesFourTilesPerTier) {
    const std::array<FarEditedFootprint, 1> edits{{{{0, 0, 0}, {0, 0, 0}}}};
    FarEditedAffectedSet affected;
    ASSERT_EQ(ComputeFarEditedAffectedSet(edits, Limits(), affected), Error::None);
    ASSERT_EQ(affected.tiles.size(), 4 * kFarTierCount);
    EXPECT_EQ(affected.bricks, Oracle(edits));
    for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier) {
        for (const auto tx : {-1, 0}) {
            for (const auto tz : {-1, 0})
                EXPECT_TRUE(std::binary_search(
                    affected.tiles.begin(), affected.tiles.end(), FarTileKey{tier, tx, tz}));
        }
    }
}

TEST(FarEditedBake, HaloReachesAdjacentBrickWithoutDiagonalOverInvalidation) {
    const std::array<FarEditedFootprint, 1> edits{{{{20, 8, 8}, {20, 8, 8}}}};
    FarEditedAffectedSet affected;
    ASSERT_EQ(ComputeFarEditedAffectedSet(edits, Limits(), affected), Error::None);
    EXPECT_EQ(affected.bricks, Oracle(edits));
    EXPECT_TRUE(std::binary_search(
        affected.bricks.begin(), affected.bricks.end(), FarEditedBrickKey{{1, 0, 0}, 0, 0, 0}));
    const std::array<FarEditedFootprint, 1> diagonal{{{{20, 20, 8}, {20, 20, 8}}}};
    ASSERT_EQ(ComputeFarEditedAffectedSet(diagonal, Limits(), affected), Error::None);
    EXPECT_FALSE(std::binary_search(
        affected.bricks.begin(), affected.bricks.end(), FarEditedBrickKey{{1, 0, 0}, 0, 0, 0}));
}

TEST(FarEditedBake, RebakeRetainsEarlierEditsOutsideChangedBricksAndTallDeepExtents) {
    TestSource source;
    source.Add({32, 128, 32}, 16, [](const auto& p) { return static_cast<float>(p.y - 136); });
    source.Add({64, -160, 64}, 16, [](const auto&) { return -3.0f; });
    source.Refresh();
    const std::array changed{Footprint(source.owned.back())};
    FarEditedBakeResult result;
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(11), Limits(), result), Error::None);
    const auto* old = FindBrick(result, 1, 2, 8, 2);
    ASSERT_NE(old, nullptr);
    EXPECT_EQ(old->state, FarBrickState::Present);
    EXPECT_EQ(old->density[0], QuantizeFarDensity(-8));
    EXPECT_EQ(old->density[5 * 2], 0);
    EXPECT_EQ(old->material[0], 3);
    EXPECT_FALSE(std::binary_search(result.affected.bricks.begin(),
                                    result.affected.bricks.end(),
                                    FarEditedBrickKey{{1, 0, 0}, 2, 8, 2}));
    for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier)
        EXPECT_TRUE(std::any_of(result.tiles.begin(), result.tiles.end(), [=](const auto& tile) {
            return tile.overlay.key.tier == tier;
        }));
    for (const auto& tile : result.tiles) {
        ASSERT_TRUE(tile.edited_span.has_value());
        EXPECT_LE(tile.edited_span->min_y, -160);
        EXPECT_GE(tile.edited_span->max_y, 144);
        EXPECT_EQ(tile.max_member_generation, 7u);
        EXPECT_EQ(tile.overlay.generation, 12u);
        EXPECT_EQ(DecodeFarOverlay(tile.bytes, tile.overlay.key).error, FarOverlayError::None);
    }
}

TEST(FarEditedBake, AirAndSolidTombstonesAcrossAllFiveTiersOverrideOppositePristineSign) {
    for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier) {
        const auto edge = kFarTierTable[tier - 1].brick_edge_meters;
        for (const auto density : {-7.0f, 7.0f}) {
            TestSource source;
            const auto e = static_cast<std::int64_t>(edge);
            source.Add({-e, -e, -e}, edge, [=](const auto&) { return density; });
            source.Refresh();
            const std::array changed{Footprint(source.owned.front())};
            FarEditedBakeResult result;
            ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(-density), Limits(), result),
                      Error::None);
            const auto* brick = FindBrick(result, tier, -1, -1, -1);
            ASSERT_NE(brick, nullptr);
            EXPECT_EQ(brick->state,
                      density < 0 ? FarBrickState::HomogeneousSolid
                                  : FarBrickState::HomogeneousAir);
            EXPECT_TRUE(std::all_of(
                brick->density.begin(), brick->density.end(), [](auto q) { return q == 0; }));
            auto tiles = Overlays(result);
            std::optional<FarVolumeSample> sample;
            ASSERT_EQ(
                SampleFarOverlay(tiles, tier, {-e / 2, -e / 2, -e / 2}, 19, tiles.size(), sample),
                FarOverlaySampleError::None);
            ASSERT_TRUE(sample.has_value());
            EXPECT_EQ(sample->density,
                      QuantizeFarDensity(density < 0 ? -static_cast<float>(edge)
                                                     : static_cast<float>(edge)) /
                          kFsv1DensityScale);
            EXPECT_EQ(sample->material, 19);
        }
    }
}

TEST(FarEditedBake, ExactZeroAndPresentTombstoneBordersUseIdenticalCoordinateSamples) {
    TestSource source;
    source.Add({-16, -16, -16}, 16, [](const auto& p) { return static_cast<float>(p.y); });
    source.Refresh();
    const std::array changed{Footprint(source.owned.front())};
    FarEditedBakeResult result;
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(7), Limits(), result), Error::None);
    const auto* core = FindBrick(result, 1, -1, -1, -1);
    ASSERT_NE(core, nullptr);
    EXPECT_EQ(core->state, FarBrickState::Present);
    EXPECT_EQ(core->density[20], 0);
    auto tiles = Overlays(result);
    std::optional<FarVolumeSample> sample;
    for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier) {
        ASSERT_EQ(SampleFarOverlay(tiles, tier, {0, 0, 0}, 9, tiles.size(), sample),
                  FarOverlaySampleError::None);
        ASSERT_TRUE(sample.has_value());
        EXPECT_EQ(*sample, (FarVolumeSample{0, 3}));
    }
    // Homogeneous core beside Present neighbours: every shared boundary resolves
    // to the Present record's original 7 m, not the tombstone's 16 m constant.
    source.owned.front().density.assign(8, 7);
    source.Refresh();
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(-7), Limits(), result), Error::None);
    tiles = Overlays(result);
    ASSERT_EQ(SampleFarOverlay(tiles, 1, {0, -8, -8}, 9, tiles.size(), sample),
              FarOverlaySampleError::None);
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(*sample, (FarVolumeSample{7, 3}));
}

TEST(FarEditedBake, InterpolationMaterialTieFallbackAndTinyNonzeroSigns) {
    TestSource source;
    source.Add({0, -4, 0}, 8, [](const auto& p) { return static_cast<float>(p.x - 4); }, 5);
    source.owned.front().material[1] = 17;
    source.Refresh();
    const std::array changed{Footprint(source.owned.front())};
    FarEditedBakeResult result;
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(9), Limits(), result), Error::None);
    auto tiles = Overlays(result);
    std::optional<FarVolumeSample> sample;
    ASSERT_EQ(SampleFarOverlay(tiles, 1, {4, 0, 4}, 9, tiles.size(), sample),
              FarOverlaySampleError::None);
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(*sample, (FarVolumeSample{0, 5}));
    source.owned.front().material.clear();
    source.Refresh();
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(9), Limits(), result), Error::None);
    tiles = Overlays(result);
    ASSERT_EQ(SampleFarOverlay(tiles, 1, {4, 0, 4}, 9, tiles.size(), sample),
              FarOverlaySampleError::None);
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(*sample, (FarVolumeSample{0, 0}));
    source.owned.front().density.assign(8, -0.0001f);
    source.Refresh();
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(0.0001f), Limits(), result),
              Error::None);
    const auto* brick = FindBrick(result, 1, 0, 0, 0);
    ASSERT_NE(brick, nullptr);
    EXPECT_EQ(brick->state, FarBrickState::Present);
    EXPECT_NE(std::find(brick->density.begin(), brick->density.end(), -1), brick->density.end());
    EXPECT_NE(std::find(brick->density.begin(), brick->density.end(), 1), brick->density.end());
}

TEST(FarEditedBake, ShuffledChangesAndSourceInventoryProduceIdenticalBytes) {
    TestSource source;
    for (const auto origin : {FarVolumePosition{-16, -16, -16}, {32, 32, 32}, {-528, 48, -16}})
        source.Add(origin, 16, [](const auto& p) { return static_cast<float>(p.y - 40); });
    std::vector<FarEditedFootprint> changed;
    for (const auto& record : source.owned)
        changed.push_back(Footprint(record));
    changed.push_back(changed.front());
    source.Refresh();
    FarEditedBakeResult baseline;
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(11), Limits(), baseline), Error::None);
    std::mt19937 random(1729);
    for (int iteration = 0; iteration < 8; ++iteration) {
        std::shuffle(source.owned.begin(), source.owned.end(), random);
        std::shuffle(changed.begin(), changed.end(), random);
        source.Refresh();
        FarEditedBakeResult result;
        ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(11), Limits(), result), Error::None);
        ExpectSame(baseline, result);
    }
}

TEST(FarEditedBake, EmptyInventoryRemovesOldOverridesAndRetainsChangedSpanRequests) {
    TestSource source;
    source.identity.state = FarEditedImageState::Committed;
    const std::array<FarEditedFootprint, 1> changed{{{{32, -2048, 32}, {48, -2032, 48}}}};
    FarEditedBakeResult result;
    ASSERT_EQ(BakeFarEditedTiles(source, changed, {}, Limits(), result), Error::None);
    ASSERT_FALSE(result.tiles.empty());
    for (const auto& tile : result.tiles) {
        EXPECT_TRUE(tile.overlay.bricks.empty());
        EXPECT_EQ(tile.bytes.size(), kFsv1HeaderBytes + kFsv1TrailerBytes);
        EXPECT_EQ(tile.max_member_generation, 0u);
        ASSERT_TRUE(tile.edited_span.has_value());
        EXPECT_LE(tile.edited_span->min_y, -2048);
        EXPECT_GE(tile.edited_span->max_y, -2032);
    }
}

TEST(FarEditedBake, MemberWatermarkIncludesHaloOnlyContributors) {
    TestSource source;
    source.Add({516, 32, 32}, 1, [](const auto&) { return -1.0f; });
    source.owned.front().generation = 10;
    source.Refresh();
    const std::array changed{Footprint(source.owned.front())};
    FarEditedBakeResult result;
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(7), Limits(), result), Error::None);
    const auto tile = std::find_if(result.tiles.begin(), result.tiles.end(), [](const auto& value) {
        return value.overlay.key == FarTileKey{1, 0, 0};
    });
    ASSERT_NE(tile, result.tiles.end());
    EXPECT_EQ(tile->max_member_generation, 10u);
    EXPECT_EQ(tile->overlay.generation, 12u);
}

TEST(FarEditedBake, PreparedAndCommittedImagesKeepPublicationStateOutsideFsv1Bytes) {
    TestSource source;
    source.Add({0, 0, 0}, 16, [](const auto&) { return -1.0f; });
    source.Refresh();
    const std::array changed{Footprint(source.owned.front())};
    FarEditedBakeResult prepared, committed;
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(7), Limits(), prepared), Error::None);
    EXPECT_EQ(prepared.source.state, FarEditedImageState::Prepared);
    source.identity.state = FarEditedImageState::Committed;
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(7), Limits(), committed), Error::None);
    EXPECT_EQ(committed.source.state, FarEditedImageState::Committed);
    ASSERT_EQ(prepared.tiles.size(), committed.tiles.size());
    for (std::size_t i = 0; i < prepared.tiles.size(); ++i)
        EXPECT_EQ(prepared.tiles[i].bytes, committed.tiles[i].bytes);
    source.identity.state = static_cast<FarEditedImageState>(2);
    const auto previous = committed;
    EXPECT_EQ(BakeFarEditedTiles(source, changed, Constant(7), Limits(), committed),
              Error::IdentityMismatch);
    ExpectSame(previous, committed);
}

TEST(FarEditedBake,
     SharedAuthoritativeNodesRefuseDensityAndMaterialConflictsBeforePristineSampling) {
    TestSource source;
    source.Add({0, 0, 0}, 16, [](const auto&) { return -1.0f; });
    source.Add({16, 0, 0}, 16, [](const auto&) { return -2.0f; });
    source.Refresh();
    const std::array changed{Footprint(source.owned.front())};
    int calls = 0;
    const FarEditedPristineSampler pristine = [&](std::uint8_t, const FarVolumePosition&) {
        ++calls;
        return std::optional<FarVolumeSample>{{7, 9}};
    };
    FarEditedBakeResult result;
    EXPECT_EQ(BakeFarEditedTiles(source, changed, pristine, Limits(), result),
              Error::BoundaryConflict);
    EXPECT_EQ(calls, 0);
    source.owned.back().density.assign(8, -1);
    source.owned.back().material.assign(8, 17);
    source.Refresh();
    EXPECT_EQ(BakeFarEditedTiles(source, changed, pristine, Limits(), result),
              Error::BoundaryConflict);
    EXPECT_EQ(calls, 0);
    source.owned.back().material.assign(8, 3);
    source.Refresh();
    EXPECT_EQ(BakeFarEditedTiles(source, changed, pristine, Limits(), result), Error::None);
}

TEST(FarEditedBake, MalformedAndUnreadableAuthorityNeverFallsBackToPristine) {
    TestSource source;
    source.Add({0, 0, 0}, 1, [](const auto&) { return -1.0f; }, 3, 17);
    const std::array changed{Footprint(source.owned.front())};
    int calls = 0;
    const FarEditedPristineSampler pristine = [&](std::uint8_t, const FarVolumePosition&) {
        ++calls;
        return std::optional<FarVolumeSample>{{7, 9}};
    };
    FarEditedBakeResult result;
    source.owned.front().density[1] = std::numeric_limits<float>::quiet_NaN();
    source.Refresh();
    EXPECT_EQ(BakeFarEditedTiles(source, changed, pristine, Limits(), result),
              Error::NonFiniteDensity);
    source.owned.front().density[1] = std::numeric_limits<float>::infinity();
    source.Refresh();
    EXPECT_EQ(BakeFarEditedTiles(source, changed, pristine, Limits(), result),
              Error::NonFiniteDensity);
    source.owned.front().density[1] = -1;
    source.owned.front().material.resize(1);
    source.Refresh();
    EXPECT_EQ(BakeFarEditedTiles(source, changed, pristine, Limits(), result),
              Error::InvalidMaterial);
    source.owned.front().material.clear();
    source.owned.front().density.clear();
    source.Refresh();
    EXPECT_EQ(BakeFarEditedTiles(source, changed, pristine, Limits(), result),
              Error::InvalidLattice);
    source.owned.front().spacing = 0;
    source.Refresh();
    EXPECT_EQ(BakeFarEditedTiles(source, changed, pristine, Limits(), result),
              Error::InvalidLattice);
    source.owned.front().spacing = 1;
    source.owned.front().side[1] = 1;
    source.Refresh();
    EXPECT_EQ(BakeFarEditedTiles(source, changed, pristine, Limits(), result),
              Error::InvalidLattice);
    EXPECT_EQ(calls, 0);
}

TEST(FarEditedBake, AllFiniteWorkAndMemoryCapsRefuseWithoutReplacingPreviousOutput) {
    TestSource source;
    source.Add({0, 0, 0}, 16, [](const auto&) { return -1.0f; });
    source.Refresh();
    const std::array changed{Footprint(source.owned.front())};
    FarEditedBakeResult previous;
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(7), Limits(), previous), Error::None);
    std::vector<FarEditedBakeLimits> refused;
    for (int cap = 0; cap < 10; ++cap) {
        auto limits = Limits();
        switch (cap) {
            case 0:
                limits.max_edits = 0;
                break;
            case 1:
                limits.max_source_records = 0;
                break;
            case 2:
                limits.max_source_samples = 1;
                break;
            case 3:
                limits.max_source_probes = 1;
                break;
            case 4:
                limits.max_density_samples = 1;
                break;
            case 5:
                limits.max_brick_visits_per_tier[0] = 1;
                break;
            case 6:
                limits.max_affected_bricks = 1;
                break;
            case 7:
                limits.max_tiles = 1;
                break;
            case 8:
                limits.max_scratch_bytes = 1;
                break;
            case 9:
                limits.max_output_bytes = 1;
                break;
        }
        refused.push_back(limits);
    }
    for (std::size_t i = 0; i < refused.size(); ++i) {
        auto result = previous;
        int calls = 0;
        const FarEditedPristineSampler pristine = [&](std::uint8_t, const FarVolumePosition&) {
            ++calls;
            return std::optional<FarVolumeSample>{{7, 9}};
        };
        EXPECT_EQ(BakeFarEditedTiles(source, changed, pristine, refused[i], result),
                  i >= 8 ? Error::MemoryLimit : Error::WorkLimit);
        EXPECT_EQ(calls, 0);
        ExpectSame(previous, result);
    }
    const std::array<FarEditedFootprint, 1> huge{
        {{{-100000, -100000, -100000}, {100000, 100000, 100000}}}};
    FarEditedAffectedSet affected = previous.affected;
    EXPECT_EQ(ComputeFarEditedAffectedSet(huge, Limits(), affected), Error::WorkLimit);
    EXPECT_EQ(affected.bricks, previous.affected.bricks);
}

TEST(FarEditedBake, CoordinateIdentityAndSamplerFailuresPreserveOutput) {
    TestSource source;
    source.Add({0, 0, 0}, 16, [](const auto&) { return -1.0f; });
    source.Refresh();
    const std::array changed{Footprint(source.owned.front())};
    FarEditedBakeResult result;
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(7), Limits(), result), Error::None);
    const auto previous = result;
    EXPECT_EQ(BakeFarEditedTiles(source, changed, {}, Limits(), result), Error::InvalidSampler);
    ExpectSame(previous, result);
    const FarEditedPristineSampler missing = [](std::uint8_t, const FarVolumePosition&) {
        return std::optional<FarVolumeSample>{};
    };
    EXPECT_EQ(BakeFarEditedTiles(source, changed, missing, Limits(), result),
              Error::SamplerFailure);
    ExpectSame(previous, result);
    EXPECT_EQ(
        BakeFarEditedTiles(
            source, changed, Constant(std::numeric_limits<float>::infinity()), Limits(), result),
        Error::NonFiniteDensity);
    ExpectSame(previous, result);
    source.owned.front().generation = 13;
    source.Refresh();
    EXPECT_EQ(BakeFarEditedTiles(source, changed, Constant(7), Limits(), result),
              Error::IdentityMismatch);
    ExpectSame(previous, result);
    FarEditedAffectedSet affected = previous.affected;
    for (const auto coordinate : {kFarVolumeExactCoordinateLimitMeters,
                                  -kFarVolumeExactCoordinateLimitMeters,
                                  std::numeric_limits<std::int64_t>::min(),
                                  std::numeric_limits<std::int64_t>::max()}) {
        const std::array<FarEditedFootprint, 1> outside{{{{coordinate, 0, 0}, {coordinate, 0, 0}}}};
        EXPECT_EQ(ComputeFarEditedAffectedSet(outside, Limits(), affected), Error::CoordinateRange);
        EXPECT_EQ(affected.bricks, previous.affected.bricks);
    }
    const std::array<FarEditedFootprint, 1> inverted{{{{1, 0, 0}, {0, 0, 0}}}};
    EXPECT_EQ(ComputeFarEditedAffectedSet(inverted, Limits(), affected), Error::InvalidFootprint);
}

TEST(FarEditedBake, ThrowingSamplerAndAllocationFailurePreserveCompleteOutput) {
    TestSource source;
    source.Add({0, 0, 0}, 16, [](const auto&) { return -1.0f; });
    source.Refresh();
    const std::array changed{Footprint(source.owned.front())};
    FarEditedBakeResult result;
    ASSERT_EQ(BakeFarEditedTiles(source, changed, Constant(7), Limits(), result), Error::None);
    const auto previous = result;
    const FarEditedPristineSampler throwing =
        [](std::uint8_t, const FarVolumePosition&) -> std::optional<FarVolumeSample> {
        throw std::runtime_error("unavailable frozen observation");
    };
    EXPECT_EQ(BakeFarEditedTiles(source, changed, throwing, Limits(), result),
              Error::SamplerFailure);
    ExpectSame(previous, result);
    const FarEditedPristineSampler allocation_failure =
        [](std::uint8_t, const FarVolumePosition&) -> std::optional<FarVolumeSample> {
        throw std::bad_alloc();
    };
    EXPECT_EQ(BakeFarEditedTiles(source, changed, allocation_failure, Limits(), result),
              Error::AllocationFailure);
    ExpectSame(previous, result);
}

TEST(FarEditedBake, OverlayResolverChecksPresentConflictsAbsenceAndFiniteInventory) {
    FarOverlayTile negative{{1, -1, 0}, 12, {}};
    FarOverlayBrick left;
    left.bx = -1;
    left.density.fill(256);
    left.material.fill(3);
    negative.bricks.push_back(left);
    FarOverlayTile positive{{1, 0, 0}, 12, {}};
    FarOverlayBrick right;
    right.density.fill(256);
    right.material.fill(3);
    positive.bricks.push_back(right);
    std::array tiles{negative, positive};
    std::optional<FarVolumeSample> sample{{9, 9}};
    // At 4 m spacing, (0, 4, 4) is right-brick local (0, 1, 1), slot 30.
    constexpr FarVolumePosition boundary{0, 4, 4};
    ASSERT_EQ(SampleFarOverlay(tiles, 1, boundary, 9, 2, sample), FarOverlaySampleError::None);
    EXPECT_EQ(sample, (std::optional<FarVolumeSample>{{1, 3}}));
    tiles[1].bricks.front().density[30] = 257;
    EXPECT_EQ(SampleFarOverlay(tiles, 1, boundary, 9, 2, sample),
              FarOverlaySampleError::BoundaryConflict);
    EXPECT_EQ(sample, (std::optional<FarVolumeSample>{{1, 3}}));
    tiles[1].bricks.front().density[30] = 256;
    tiles[1].bricks.front().material[30] = 4;
    EXPECT_EQ(SampleFarOverlay(tiles, 1, boundary, 9, 2, sample),
              FarOverlaySampleError::BoundaryConflict);
    EXPECT_EQ(SampleFarOverlay(tiles, 1, boundary, 9, 1, sample), FarOverlaySampleError::WorkLimit);
    EXPECT_EQ(SampleFarOverlay(tiles, 1, {1, 4, 4}, 9, 2, sample),
              FarOverlaySampleError::InvalidInput);
    ASSERT_EQ(SampleFarOverlay(tiles, 1, {64, 64, 64}, 9, 2, sample), FarOverlaySampleError::None);
    EXPECT_FALSE(sample);
    tiles[0].bricks.front().state = FarBrickState::HomogeneousAir;
    tiles[1].bricks.front().state = FarBrickState::HomogeneousSolid;
    EXPECT_EQ(SampleFarOverlay(tiles, 1, boundary, 9, 2, sample),
              FarOverlaySampleError::BoundaryConflict);
    tiles[1].bricks.front().state = FarBrickState::Present;
    ASSERT_EQ(SampleFarOverlay(tiles, 1, boundary, 9, 2, sample), FarOverlaySampleError::None);
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(*sample, (FarVolumeSample{1, 4}));
}
