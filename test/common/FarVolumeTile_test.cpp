#include "luminumbra_common/world/FarVolumeTile.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace {
using namespace Luminumbra::World;
constexpr FarVolumeBuildLimits kOneLayer{1024, 1024};

FarVolumeSample plane(const FarVolumePosition& p) {
    return {static_cast<float>(p.y) + 2.0f,
            static_cast<std::uint8_t>(static_cast<std::uint64_t>(p.x) ^
                                      static_cast<std::uint64_t>(p.z))};
}

FarVolumeTile baseline() {
    FarVolumeTile tile;
    EXPECT_TRUE(BuildFarVolumeTile({{1, 0, 0}, {-16, 0}, 87}, kOneLayer, plane, tile));
    return tile;
}

TEST(FarVolumeTile, AllFiveDimensionsOriginsAndIndependentAnalyticSamples) {
    // Independent literal rows; do not derive expected dimensions from production.
    constexpr std::array<std::array<std::int64_t, 3>, 5> expected{
        {{4, 16, 512}, {8, 32, 1024}, {16, 64, 2048}, {32, 128, 4096}, {64, 256, 8192}}};
    for (std::size_t tier = 1; tier <= 5; ++tier) {
        const auto [step, edge, tile_edge] = expected[tier - 1];
        const FarVolumeRequest request{{tier, -2, 3}, {-edge, 0}, 0x1234};
        FarVolumeTile tile;
        ASSERT_TRUE(BuildFarVolumeTile(request, kOneLayer, plane, tile));
        EXPECT_EQ(tile.request, request);
        EXPECT_EQ(tile.min_meters, (FarVolumePosition{-2 * tile_edge, -edge, 3 * tile_edge}));
        EXPECT_EQ(tile.max_meters, (FarVolumePosition{-tile_edge, 0, 4 * tile_edge}));
        EXPECT_EQ(tile.content, FarVolumeContent::Mixed);
        EXPECT_EQ(tile.candidates_visited, 1024u);
        EXPECT_EQ(tile.samples_evaluated, 128000u);
        ASSERT_EQ(tile.bricks.size(), 1024u);
        for (std::size_t i = 0; i < tile.bricks.size(); ++i) {
            const auto& brick = tile.bricks[i];
            EXPECT_EQ(
                brick.origin_meters,
                (FarVolumePosition{-2 * tile_edge + static_cast<std::int64_t>(i % 32) * edge,
                                   -edge,
                                   3 * tile_edge + static_cast<std::int64_t>(i / 32) * edge}));
            for (std::size_t sample = 0; sample < 125; ++sample) {
                const auto x = brick.origin_meters.x + static_cast<std::int64_t>(sample % 5) * step;
                const auto y = -edge + static_cast<std::int64_t>((sample / 5) % 5) * step;
                const auto z =
                    brick.origin_meters.z + static_cast<std::int64_t>(sample / 25) * step;
                EXPECT_EQ(brick.samples[sample].density, static_cast<float>(y) + 2.0f);
                EXPECT_EQ(brick.samples[sample].material,
                          static_cast<std::uint8_t>(static_cast<std::uint64_t>(x) ^
                                                    static_cast<std::uint64_t>(z)));
            }
        }
    }
}

TEST(FarVolumeTile, SharedWorldBordersAndIndependentColdRebuildsAcrossEveryTier) {
    constexpr std::array<std::int64_t, 5> edges{16, 32, 64, 128, 256};
    for (std::size_t tier = 1; tier <= 5; ++tier) {
        const FarVolumeRequest request{{tier, -1, -1}, {-edges[tier - 1], 0}, 7};
        FarVolumeTile left, right, above, repeated;
        ASSERT_TRUE(BuildFarVolumeTile(request, kOneLayer, plane, left));
        auto next = request;
        next.key.x = 0;
        ASSERT_TRUE(BuildFarVolumeTile(next, kOneLayer, plane, right));
        next = request;
        next.key.z = 0;
        ASSERT_TRUE(BuildFarVolumeTile(next, kOneLayer, plane, above));
        ASSERT_TRUE(BuildFarVolumeTile(request, kOneLayer, plane, repeated));
        EXPECT_EQ(left, repeated);
        for (std::size_t i = 0; i < left.bricks.size(); ++i)
            for (std::size_t sample = 0; sample < 125; ++sample)
                EXPECT_EQ(std::bit_cast<std::uint32_t>(left.bricks[i].samples[sample].density),
                          std::bit_cast<std::uint32_t>(repeated.bricks[i].samples[sample].density));
        for (std::size_t cell = 0; cell < 32; ++cell) {
            for (std::size_t y = 0; y < 5; ++y) {
                for (std::size_t cross = 0; cross < 5; ++cross) {
                    EXPECT_EQ(left.bricks[cell * 32 + 31].samples[4 + 5 * (y + 5 * cross)],
                              right.bricks[cell * 32].samples[5 * (y + 5 * cross)]);
                    EXPECT_EQ(left.bricks[31 * 32 + cell].samples[cross + 5 * (y + 20)],
                              above.bricks[cell].samples[cross + 5 * y]);
                }
            }
        }
    }
}

TEST(FarVolumeTile, HomogeneousAirSolidAndZeroAreSuccessfulEmptyTiles) {
    for (std::size_t tier = 1; tier <= 5; ++tier) {
        const std::int64_t edge = 16LL << (tier - 1);
        for (float density : {-1.0f, 0.0f, 1.0f}) {
            FarVolumeTile tile;
            ASSERT_TRUE(BuildFarVolumeTile(
                {{tier, 0, 0}, {-edge, edge}, 12},
                {2048, 0},
                [density](const auto&) { return FarVolumeSample{density, 9}; },
                tile));
            EXPECT_TRUE(tile.bricks.empty());
            EXPECT_EQ(tile.content, density < 0 ? FarVolumeContent::Solid : FarVolumeContent::Air);
            EXPECT_EQ(tile.candidates_visited, 2048u);
            EXPECT_EQ(tile.samples_evaluated, 256000u);
        }
    }
}

TEST(FarVolumeTile, SparseStorageRetainsOnlyIntersectedLayerInCanonicalOrder) {
    FarVolumeTile tile;
    ASSERT_TRUE(BuildFarVolumeTile({{1, 0, 0}, {-32, 16}, 91}, {3072, 1024}, plane, tile));
    EXPECT_EQ(tile.candidates_visited, 3072u);
    ASSERT_EQ(tile.bricks.size(), 1024u);
    for (const auto& brick : tile.bricks)
        EXPECT_EQ(brick.origin_meters.y, -16);
}

TEST(FarVolumeTile, SharedVerticalBordersAndExactZeroUseExistingMesherSignConvention) {
    FarVolumeTile below, above;
    auto field = [](const FarVolumePosition& p) {
        return FarVolumeSample{static_cast<float>(p.x % 16 - 8), 3};
    };
    ASSERT_TRUE(BuildFarVolumeTile({{1, 0, 0}, {-16, 0}, 1}, kOneLayer, field, below));
    ASSERT_TRUE(BuildFarVolumeTile({{1, 0, 0}, {0, 16}, 1}, kOneLayer, field, above));
    ASSERT_EQ(below.bricks.size(), 1024u);
    ASSERT_EQ(above.bricks.size(), 1024u);
    for (std::size_t i = 0; i < 1024; ++i)
        for (std::size_t z = 0; z < 5; ++z)
            for (std::size_t x = 0; x < 5; ++x)
                EXPECT_EQ(below.bricks[i].samples[x + 5 * (4 + 5 * z)],
                          above.bricks[i].samples[x + 25 * z]);
    FarVolumeTile touches_zero;
    ASSERT_TRUE(BuildFarVolumeTile(
        {{1, 0, 0}, {-16, 0}, 2},
        kOneLayer,
        [](const auto& p) { return FarVolumeSample{static_cast<float>(p.y), 0}; },
        touches_zero));
    EXPECT_EQ(touches_zero.content, FarVolumeContent::Mixed);
    EXPECT_EQ(touches_zero.bricks.size(), 1024u);
}

TEST(FarVolumeTile, PreservesSubnormalDensityAndOpaqueMaterialWithoutQuantization) {
    const float tiny = std::numeric_limits<float>::denorm_min();
    FarVolumeTile tile;
    ASSERT_TRUE(BuildFarVolumeTile(
        {{1, 0, 0}, {-16, 0}, 5},
        kOneLayer,
        [tiny](const auto& p) { return FarVolumeSample{p.y < 0 ? -tiny : tiny, 255}; },
        tile));
    ASSERT_FALSE(tile.bricks.empty());
    EXPECT_EQ(std::bit_cast<std::uint32_t>(tile.bricks[0].samples[0].density),
              std::bit_cast<std::uint32_t>(-tiny));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(tile.bricks[0].samples[20].density),
              std::bit_cast<std::uint32_t>(tiny));
    EXPECT_EQ(tile.bricks[0].samples[20].material, 255);
}

TEST(FarVolumeTile, SamplingPhaseCanMissOrRetainSubSpacingFeatures) {
    FarVolumeTile missed, retained;
    auto field = [](std::int64_t center) {
        return [center](const FarVolumePosition& p) {
            return FarVolumeSample{std::abs(static_cast<float>(p.y - center)) < 1.0f ? 1.0f : -1.0f,
                                   1};
        };
    };
    const FarVolumeRequest request{{1, 0, 0}, {0, 16}, 6};
    ASSERT_TRUE(BuildFarVolumeTile(request, kOneLayer, field(2), missed));
    ASSERT_TRUE(BuildFarVolumeTile(request, kOneLayer, field(4), retained));
    EXPECT_EQ(missed.content, FarVolumeContent::Solid);
    EXPECT_TRUE(missed.bricks.empty());
    EXPECT_EQ(retained.bricks.size(), 1024u);
}

TEST(FarVolumeTile, InvalidRequestsAndWorkLimitsRefuseBeforeSamplingAndPreserveOutput) {
    auto output = baseline();
    const auto before = output;
    std::size_t calls = 0;
    const FarVolumeSampler sampler = [&calls](const auto& p) {
        ++calls;
        return plane(p);
    };
    const auto rejects =
        [&](FarVolumeRequest request, FarVolumeBuildLimits limits, FarVolumeBuildError expected) {
            FarVolumeBuildError error = FarVolumeBuildError::None;
            EXPECT_FALSE(BuildFarVolumeTile(request, limits, sampler, output, &error));
            EXPECT_EQ(error, expected);
            EXPECT_EQ(calls, 0u);
            EXPECT_EQ(output, before);
        };
    for (const auto tier :
         {std::size_t{0}, std::size_t{6}, std::numeric_limits<std::size_t>::max()})
        rejects({{tier, 0, 0}, {-16, 0}, 1}, kOneLayer, FarVolumeBuildError::InvalidTier);
    for (const FarVolumeSpan span : {FarVolumeSpan{0, 0}, {16, 0}, {-15, 0}, {-16, 1}})
        rejects({{1, 0, 0}, span, 1}, kOneLayer, FarVolumeBuildError::InvalidSpan);
    for (const auto value :
         {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
        rejects({{1, value, 0}, {-16, 0}, 1}, kOneLayer, FarVolumeBuildError::CoordinateOverflow);
        rejects({{5, 0, value}, {-256, 0}, 1}, kOneLayer, FarVolumeBuildError::CoordinateOverflow);
    }
    rejects({{1, 0, 0}, {-16, 0}, 1}, {1023, 1024}, FarVolumeBuildError::WorkLimit);
    rejects(
        {{1, 0, 0},
         {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max() - 15},
         1},
        {std::numeric_limits<std::uint64_t>::max(), 1024},
        FarVolumeBuildError::WorkLimit);
}

TEST(FarVolumeTile, ExactCoordinateExtremesRemainIntegerAndDoNotOverflow) {
    for (std::size_t tier = 1; tier <= 5; ++tier) {
        const std::int64_t tile_edge = 512LL << (tier - 1), edge = 16LL << (tier - 1);
        const auto lo = std::numeric_limits<std::int64_t>::min();
        const auto hi = std::numeric_limits<std::int64_t>::max();
        FarVolumeTile tile;
        const FarVolumeRequest request{
            {tier, lo / tile_edge, (hi - tile_edge) / tile_edge}, {lo, lo + edge}, 0};
        ASSERT_TRUE(BuildFarVolumeTile(
            request, {1024, 0}, [](const auto&) { return FarVolumeSample{1, 0}; }, tile));
        EXPECT_EQ(tile.min_meters.x, lo);
        EXPECT_EQ(tile.max_meters.y, lo + edge);
        EXPECT_LE(tile.max_meters.z, hi);
        EXPECT_EQ(tile.samples_evaluated, 128000u);
    }
}

TEST(FarVolumeTile, SamplerFailuresNonFiniteValuesAndSurfaceLimitsAreTransactional) {
    auto output = baseline();
    const auto before = output;
    const FarVolumeRequest request{{1, 0, 0}, {-16, 0}, 9};
    const auto rejects = [&](const FarVolumeSampler& sampler,
                             FarVolumeBuildLimits limits,
                             FarVolumeBuildError expected) {
        FarVolumeBuildError error = FarVolumeBuildError::None;
        EXPECT_FALSE(BuildFarVolumeTile(request, limits, sampler, output, &error));
        EXPECT_EQ(error, expected);
        EXPECT_EQ(output, before);
    };
    rejects({}, kOneLayer, FarVolumeBuildError::InvalidSampler);
    rejects(
        [](const auto&) { return std::nullopt; }, kOneLayer, FarVolumeBuildError::SamplerFailure);
    rejects([](const auto&) -> FarVolumeSample { throw std::runtime_error("refused"); },
            kOneLayer,
            FarVolumeBuildError::SamplerFailure);
    for (const auto density : {std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()}) {
        std::size_t calls = 0;
        rejects(
            [&](const auto& p) { return ++calls == 130 ? FarVolumeSample{density, 1} : plane(p); },
            kOneLayer,
            FarVolumeBuildError::NonFiniteDensity);
        EXPECT_EQ(calls, 130u);
    }
    rejects(plane, {1024, 1023}, FarVolumeBuildError::SurfaceLimit);
    FarVolumeBuildError error = FarVolumeBuildError::SamplerFailure;
    EXPECT_TRUE(BuildFarVolumeTile(request, kOneLayer, plane, output, &error));
    EXPECT_EQ(error, FarVolumeBuildError::None);
    EXPECT_EQ(output.request, request);
}
} // namespace
