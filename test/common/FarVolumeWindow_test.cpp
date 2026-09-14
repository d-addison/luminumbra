#include "systems/SHIELD_WorldSystem.h"
#include "world/FarVolume.h"
#include "world/MarchingCubes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
constexpr std::uint64_t kColumns = 16'641;
// Independent dimensions, not expectations obtained from FarTierAt.
constexpr std::array<std::array<std::int64_t, 3>, 5> kDimensions{
    {{4, 16, 512}, {8, 32, 1024}, {16, 64, 2048}, {32, 128, 4096}, {64, 256, 8192}}};

FarVolumeLimits LayerBudget(std::size_t layers) {
    const auto samples = kColumns * (4 * layers + 1);
    return {samples,
            1024 * layers,
            static_cast<std::size_t>(kColumns * sizeof(float) + samples * sizeof(FarVolumeSample) +
                                     1024 * layers * sizeof(FarVolumeBrick))};
}
FarVolumeSamplers Plane(float height = 0.0f) {
    return {[height](float, float) { return height; },
            [](const Vec3& p, float h) {
                return FarVolumeDensity{p.y - h, 7};
            }};
}
FarVolumeWindowRequest Window(std::uint32_t tier, std::int64_t first, std::int64_t last) {
    const auto edge = kDimensions.at(tier - 1)[1];
    return {tier, 0, 0, FarCaveMode::BandLimited, {first * edge, last * edge}};
}
void ExpectSameTile(const FarVolumeTile& actual, const FarVolumeTile& expected) {
    EXPECT_EQ(actual.tier, expected.tier);
    EXPECT_EQ(actual.tile_x, expected.tile_x);
    EXPECT_EQ(actual.tile_z, expected.tile_z);
    EXPECT_EQ(actual.caves, expected.caves);
    EXPECT_EQ(actual.first_brick_y, expected.first_brick_y);
    EXPECT_EQ(actual.last_brick_y, expected.last_brick_y);
    EXPECT_EQ(actual.sampled_bricks, expected.sampled_bricks);
    EXPECT_EQ(actual.crc32, expected.crc32);
    ASSERT_EQ(actual.bricks.size(), expected.bricks.size());
    for (std::size_t i = 0; i < actual.bricks.size(); ++i) {
        EXPECT_EQ(actual.bricks[i].key, expected.bricks[i].key);
        EXPECT_EQ(actual.bricks[i].samples, expected.bricks[i].samples);
        EXPECT_EQ(actual.bricks[i].crc32, expected.bricks[i].crc32);
    }
}
using VertexBits = std::array<std::uint32_t, 4>;
using Triangle = std::array<VertexBits, 3>;
std::vector<Triangle> Triangles(const FarVolumeMesh& mesh) {
    std::vector<Triangle> triangles;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        Triangle triangle{};
        for (std::size_t j = 0; j < 3; ++j) {
            const auto& v = mesh.vertices.at(mesh.indices.at(i + j));
            triangle[j] = {std::bit_cast<std::uint32_t>(v.position.x),
                           std::bit_cast<std::uint32_t>(v.position.y),
                           std::bit_cast<std::uint32_t>(v.position.z),
                           v.material};
        }
        // Cyclic rotation preserves orientation; reversing winding cannot pass.
        const Triangle b{triangle[1], triangle[2], triangle[0]};
        const Triangle c{triangle[2], triangle[0], triangle[1]};
        triangles.push_back(std::min({triangle, b, c}));
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}
FarVolumeTile Aggregate(std::vector<FarVolumeTile>& pages, const FarVolumeTile& full) {
    FarVolumeTile merged{full.tier,
                         full.tile_x,
                         full.tile_z,
                         full.caves,
                         full.first_brick_y,
                         full.last_brick_y,
                         0,
                         {},
                         0};
    for (const auto& page : pages) {
        merged.sampled_bricks += page.sampled_bricks;
        merged.bricks.insert(merged.bricks.end(), page.bricks.begin(), page.bricks.end());
    }
    std::sort(merged.bricks.begin(), merged.bricks.end(), [](const auto& a, const auto& b) {
        return a.key < b.key;
    });
    merged.crc32 = FarVolumeTileCrc(merged);
    ValidateFarVolumeTile(merged);
    return merged;
}

TEST(FarVolumeWindow, DiscoveryPreservesSignedSurfaceRoundingAndExtensionAtEveryTier) {
    constexpr std::array<std::int64_t, 5> at_zero{-16, -8, -4, -2, -1};
    constexpr std::array<std::int64_t, 5> below_zero{-17, -9, -5, -3, -2};
    constexpr std::array<std::int64_t, 5> extra_first{-33, -17, -9, -5, -3};
    constexpr std::array<std::int64_t, 5> extra_last{9, 5, 3, 2, 1};
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        const auto [step, edge, tile_edge] = kDimensions[tier - 1];
        for (float h : {0.0f, 10.25f, -10.25f}) {
            std::uint64_t calls = 0;
            const auto height = [&](float x, float z) {
                EXPECT_EQ(x, -tile_edge + static_cast<std::int64_t>(calls % 129) * step);
                EXPECT_EQ(z, 2 * tile_edge + static_cast<std::int64_t>(calls / 129) * step);
                ++calls;
                return h;
            };
            const FarVolumeRequest request{tier, -1, 2};
            const auto found = DiscoverFarVolumeWindow(request, height);
            EXPECT_EQ(calls, kColumns);
            EXPECT_EQ(found.tile_x, -1);
            EXPECT_EQ(found.tile_z, 2);
            EXPECT_EQ(found.window.min_y_meters, (h < 0 ? below_zero : at_zero)[tier - 1] * edge);
            EXPECT_EQ(found.window.max_y_meters, h < 0 ? 0 : edge);
            const auto old = BuildFarVolumeTile(request, Plane(h));
            EXPECT_EQ(found.window.min_y_meters, old.first_brick_y * edge);
            EXPECT_EQ(found.window.max_y_meters, old.last_brick_y * edge);
        }
        auto request = FarVolumeRequest{tier, -1, 2, FarVolumeSpan{-513, 129}};
        const auto extended = DiscoverFarVolumeWindow(request, Plane().height);
        EXPECT_EQ(extended.window.min_y_meters, extra_first[tier - 1] * edge);
        EXPECT_EQ(extended.window.max_y_meters, extra_last[tier - 1] * edge);
        request.extra_span = FarVolumeSpan{0, 0};
        EXPECT_EQ(DiscoverFarVolumeWindow(request, Plane().height).window,
                  (FarVolumeYWindow{at_zero[tier - 1] * edge, edge}));
    }
}

TEST(FarVolumeWindow, TallRefusalBecomesAnExactFiveWindowPlanWithoutDensityDiscovery) {
    const FarVolumeRequest request{1, 0, 0, FarVolumeSpan{-4096, -4080}};
    std::uint64_t height_calls = 0, density_calls = 0;
    const FarVolumeSamplers field{[&](float, float) {
                                      ++height_calls;
                                      return 0.0f;
                                  },
                                  [&](const Vec3& p, float) {
                                      ++density_calls;
                                      return FarVolumeDensity{p.y, 3};
                                  }};
    EXPECT_THROW(BuildFarVolumeTile(request, field), std::length_error);
    EXPECT_EQ(height_calls, kColumns);
    EXPECT_EQ(density_calls, 0u);
    height_calls = 0;
    const auto coverage = DiscoverFarVolumeWindow(request, field.height);
    EXPECT_EQ(coverage.window, (FarVolumeYWindow{-4096, 16}));
    const auto plan = PlanFarVolumeWindows(coverage);
    ASSERT_EQ(plan.WindowCount(), 5u);
    EXPECT_EQ(plan.LayerStride(), 64u);
    EXPECT_EQ(plan.SampledBricks(), 263168u);
    EXPECT_EQ(plan.DensitySampleCalls(), 17190153u);
    EXPECT_EQ(plan.HeightSampleCalls(), 83205u);
    constexpr std::array<FarVolumeYWindow, 5> expected{
        {{-4096, -3072}, {-3072, -2048}, {-2048, -1024}, {-1024, 0}, {0, 16}}};
    for (std::uint32_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(FarVolumeWindowAt(plan, i).window, expected[i]);
    EXPECT_EQ(height_calls, kColumns);
    EXPECT_EQ(density_calls, 0u);
}

TEST(FarVolumeWindow, GlobalNegativePageGridClipsEdgesAndKeepsInteriorPagesOnExtension) {
    constexpr std::array<std::array<std::int64_t, 2>, 4> expected{
        {{-5, -4}, {-4, 0}, {0, 4}, {4, 6}}};
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        const auto edge = kDimensions[tier - 1][1];
        const auto plan = PlanFarVolumeWindows(Window(tier, -5, 6), LayerBudget(4), 4);
        ASSERT_EQ(plan.WindowCount(), expected.size());
        EXPECT_EQ(plan.LayerStride(), 4u);
        EXPECT_EQ(plan.SampledBricks(), 11264u);
        EXPECT_EQ(plan.DensitySampleCalls(), 798768u);
        for (std::uint32_t i = 0; i < expected.size(); ++i)
            EXPECT_EQ(FarVolumeWindowAt(plan, i).window,
                      (FarVolumeYWindow{expected[i][0] * edge, expected[i][1] * edge}));
        const auto expanded = PlanFarVolumeWindows(Window(tier, -9, 10), LayerBudget(4));
        EXPECT_EQ(expanded.LayerStride(), plan.LayerStride());
        EXPECT_EQ(FarVolumeWindowAt(expanded, 2), FarVolumeWindowAt(plan, 1));
        EXPECT_EQ(FarVolumeWindowAt(expanded, 3), FarVolumeWindowAt(plan, 2));
        EXPECT_EQ(PlanFarVolumeWindows(Window(tier, -4, 0), LayerBudget(4)).WindowCount(), 1u);
        EXPECT_THROW(PlanFarVolumeWindows(Window(tier, -5, 6), LayerBudget(4), 3),
                     std::length_error);
        EXPECT_THROW(FarVolumeWindowAt(plan, 4), std::invalid_argument);
        EXPECT_THROW(FarVolumeWindowAt(plan, std::numeric_limits<std::uint32_t>::max()),
                     std::invalid_argument);
    }
}

TEST(FarVolumeWindow, PlanFitsExactByteSampleAndCandidateBoundsWithoutAllocatingPages) {
    static_assert(std::is_trivially_copyable_v<FarVolumeWindowPlan>);
    RecordProperty("plan_bytes", std::to_string(sizeof(FarVolumeWindowPlan)));
    RecordProperty("sample_bytes", std::to_string(sizeof(FarVolumeSample)));
    RecordProperty("brick_bytes", std::to_string(sizeof(FarVolumeBrick)));
    auto limits = LayerBudget(1);
    EXPECT_EQ(PlanFarVolumeWindows(Window(1, -1, 1), limits).LayerStride(), 1u);
    for (int which : {0, 1, 2}) {
        auto short_budget = limits;
        if (which == 0)
            --short_budget.max_density_samples;
        if (which == 1)
            --short_budget.max_bricks;
        if (which == 2)
            --short_budget.max_buffer_bytes;
        EXPECT_THROW(PlanFarVolumeWindows(Window(1, -1, 1), short_budget), std::length_error);
    }
    EXPECT_EQ(PlanFarVolumeWindows(Window(1, 0, 4096), limits).WindowCount(), 4096u);
    EXPECT_THROW(PlanFarVolumeWindows(Window(1, 0, 4097), limits), std::length_error);
    EXPECT_THROW(PlanFarVolumeWindows(Window(1, 0, 1), limits, 0), std::invalid_argument);
    limits = {std::numeric_limits<std::uint64_t>::max(),
              std::numeric_limits<std::size_t>::max(),
              std::numeric_limits<std::size_t>::max()};
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        auto request = Window(tier, -1, 1);
        request.window = {-8387584, 8387584};
        const auto plan = PlanFarVolumeWindows(request, limits);
        EXPECT_LE(plan.LayerStride(), 16775168u / kDimensions[tier - 1][1]);
        EXPECT_EQ(FarVolumeWindowAt(plan, 0).window.min_y_meters, -8387584);
        EXPECT_EQ(FarVolumeWindowAt(plan, plan.WindowCount() - 1).window.max_y_meters, 8387584);
        EXPECT_LE(plan.WindowCount(), 4096u);
    }
}

TEST(FarVolumeWindow, IndependentCoverageFoldRejectsIncompleteOrReorderedAcknowledgements) {
    const auto plan = PlanFarVolumeWindows(Window(1, -5, 6), LayerBudget(4));
    std::vector<FarVolumeYWindow> pages;
    for (std::uint32_t i = 0; i < plan.WindowCount(); ++i)
        pages.push_back(FarVolumeWindowAt(plan, i).window);
    const auto complete = [](const std::vector<FarVolumeYWindow>& acknowledgements) {
        std::int64_t next = -80;
        for (const auto& window : acknowledgements) {
            if (window.min_y_meters != next || window.max_y_meters <= next ||
                window.max_y_meters > 96)
                return false;
            next = window.max_y_meters;
        }
        return next == 96;
    };
    EXPECT_TRUE(complete(pages));
    for (std::size_t i = 0; i < pages.size(); ++i) {
        auto skipped = pages;
        skipped.erase(skipped.begin() + static_cast<std::ptrdiff_t>(i));
        EXPECT_FALSE(complete(skipped));
        auto duplicated = pages;
        duplicated.insert(duplicated.begin() + static_cast<std::ptrdiff_t>(i), pages[i]);
        EXPECT_FALSE(complete(duplicated));
        EXPECT_FALSE(complete({pages.begin(), pages.begin() + static_cast<std::ptrdiff_t>(i)}));
    }
    auto reordered = pages;
    std::swap(reordered[1], reordered[2]);
    EXPECT_FALSE(complete(reordered));
    auto overlap = pages;
    overlap[1].min_y_meters -= 16;
    EXPECT_FALSE(complete(overlap));
    auto wrong_end = pages;
    wrong_end.back().max_y_meters -= 16;
    EXPECT_FALSE(complete(wrong_end));
    // This is an independent consumer-contract oracle, not runtime publication.
}

TEST(FarVolumeWindow, InvalidWindowsAndKnownBudgetsRefuseBeforeEveryCallback) {
    std::size_t calls = 0;
    const FarVolumeSamplers field{[&](float, float) {
                                      ++calls;
                                      return 0.0f;
                                  },
                                  [&](const Vec3&, float) {
                                      ++calls;
                                      return FarVolumeDensity{1, 0};
                                  }};
    const auto rejects = [&](FarVolumeWindowRequest request) {
        EXPECT_THROW(BuildFarVolumeWindow(request, field), std::invalid_argument);
        EXPECT_THROW(PlanFarVolumeWindows(request), std::invalid_argument);
        EXPECT_EQ(calls, 0u);
    };
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        const auto edge = kDimensions[tier - 1][1];
        for (const auto window : {FarVolumeYWindow{0, 0},
                                  FarVolumeYWindow{edge, 0},
                                  FarVolumeYWindow{-edge + 1, 0},
                                  FarVolumeYWindow{0, edge + 1},
                                  FarVolumeYWindow{std::numeric_limits<std::int64_t>::min(), 0},
                                  FarVolumeYWindow{0, std::numeric_limits<std::int64_t>::max()},
                                  FarVolumeYWindow{-8387584 - edge, -8387584},
                                  FarVolumeYWindow{8387584, 8387584 + edge}}) {
            auto request = Window(tier, -1, 0);
            request.window = window;
            rejects(request);
        }
        for (auto value :
             {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()}) {
            auto request = Window(tier, -1, 0);
            request.tile_x = value;
            rejects(request);
            request.tile_x = 0;
            request.tile_z = value;
            rejects(request);
        }
    }
    for (auto tier : {0u, 6u, std::numeric_limits<std::uint32_t>::max()}) {
        auto request = Window(1, -1, 0);
        request.tier = tier;
        rejects(request);
    }
    auto invalid_mode = Window(1, -1, 0);
    invalid_mode.caves = static_cast<FarCaveMode>(255);
    rejects(invalid_mode);
    auto budget = LayerBudget(1);
    --budget.max_density_samples;
    EXPECT_THROW(BuildFarVolumeWindow(Window(1, -1, 0), field, budget), std::length_error);
    budget = LayerBudget(1);
    --budget.max_buffer_bytes;
    EXPECT_THROW(BuildFarVolumeWindow(Window(1, -1, 0), field, budget), std::length_error);
    EXPECT_EQ(calls, 0u);
}

TEST(FarVolumeWindow, ExplicitDeepAndCoordinateEdgeWindowsStayIndependentOfTheSurface) {
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        const auto edge = kDimensions[tier - 1][1];
        for (std::int64_t start : {-8387584ll, -4096ll, 8387584ll - edge}) {
            auto request = Window(tier, -1, 0);
            request.window = {start, start + edge};
            std::uint64_t count = 0;
            const auto page =
                BuildFarVolumeWindow(request, {Plane().height, [&](const Vec3& p, float) {
                                                   EXPECT_GE(p.y, start);
                                                   EXPECT_LE(p.y, start + edge);
                                                   ++count;
                                                   return FarVolumeDensity{1, 255};
                                               }});
            EXPECT_EQ(count, 83205u);
            EXPECT_EQ(page.first_brick_y, start / edge);
            EXPECT_EQ(page.last_brick_y, start / edge + 1);
            EXPECT_TRUE(page.bricks.empty());
            ValidateFarVolumeTile(page);
        }
    }
}

TEST(FarVolumeWindow, HomogeneousZeroAndSubnormalSignsRetainCanonicalQuantization) {
    for (float value : {-1.0f, -0.0f, 0.0f, 1.0f}) {
        auto limits = LayerBudget(1);
        limits.max_bricks = 0;
        const auto tile = BuildFarVolumeWindow(Window(1, -1, 0),
                                               {Plane().height,
                                                [value](const Vec3&, float) {
                                                    return FarVolumeDensity{value, 255};
                                                }},
                                               limits);
        EXPECT_TRUE(tile.bricks.empty());
        ValidateFarVolumeTile(tile, limits);
        EXPECT_THROW(PlanFarVolumeWindows(Window(1, -1, 0), limits), std::length_error);
    }
    for (float value :
         {std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max()}) {
        const auto tile = BuildFarVolumeWindow(
            Window(1, -1, 0), {Plane().height, [value](const Vec3& p, float) {
                                   return FarVolumeDensity{p.y < 0 ? -value : value, 255};
                               }});
        ASSERT_EQ(tile.bricks.size(), 1024u);
        const auto magnitude = value == std::numeric_limits<float>::max() ? 32767 : 1;
        EXPECT_EQ(tile.bricks.front().samples[0].density, -magnitude);
        EXPECT_EQ(tile.bricks.front().samples[20].density, magnitude);
        EXPECT_EQ(tile.bricks.front().samples[20].material, 255);
    }
    const auto near_zero = BuildFarVolumeWindow(
        Window(1, -1, 0), {Plane().height, [](const Vec3& p, float) {
                               return FarVolumeDensity{p.y < 0 ? -0.0001f : 1.0f, 0};
                           }});
    const auto mesh = MarchingCubes::PolygoniseFarVolume(near_zero);
    ASSERT_FALSE(mesh.vertices.empty());
    // Canonical -1/+256 interpolate at 1/257, not the raw float fraction.
    for (const auto& vertex : mesh.vertices)
        EXPECT_NEAR(vertex.position.y, -4.0f + 4.0f / 257.0f, 0.000001f);
}

TEST(FarVolumeWindow, SamplerAndDiscoveryFailuresPreserveThePreviouslyAssignedTile) {
    const auto request = Window(1, -1, 0);
    const auto before = BuildFarVolumeWindow(request, Plane());
    auto output = before;
    for (const auto value : {std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity()}) {
        std::uint64_t calls = 0;
        const FarVolumeSamplers field{Plane().height, [&](const Vec3& p, float) {
                                          return FarVolumeDensity{++calls == 130 ? value : p.y, 7};
                                      }};
        EXPECT_THROW(output = BuildFarVolumeWindow(request, field), std::invalid_argument);
        EXPECT_EQ(calls, 130u);
        ExpectSameTile(output, before);
        EXPECT_THROW(DiscoverFarVolumeWindow({}, [value](float, float) { return value; }),
                     std::invalid_argument);
    }
    const FarVolumeSamplers failure{Plane().height, [](const Vec3&, float) -> FarVolumeDensity {
                                        throw std::runtime_error("sampler failure");
                                    }};
    EXPECT_THROW(output = BuildFarVolumeWindow(request, failure), std::runtime_error);
    ExpectSameTile(output, before);
    auto limit = LayerBudget(1);
    limit.max_bricks = 1023;
    EXPECT_THROW(output = BuildFarVolumeWindow(request, Plane(), limit), std::length_error);
    ExpectSameTile(output, before);
    EXPECT_THROW(BuildFarVolumeWindow(request, {}), std::invalid_argument);
    EXPECT_THROW(DiscoverFarVolumeWindow({}, {}), std::invalid_argument);
    for (const auto span :
         {FarVolumeSpan{1, -1}, FarVolumeSpan{0, std::numeric_limits<float>::infinity()}})
        EXPECT_THROW(DiscoverFarVolumeWindow({1, 0, 0, span}, Plane().height),
                     std::invalid_argument);
    std::uint64_t calls = 0;
    EXPECT_THROW(DiscoverFarVolumeWindow({},
                                         [&](float, float) {
                                             ++calls;
                                             return 0.0f;
                                         },
                                         {1, 0, 1}),
                 std::length_error);
    EXPECT_EQ(calls, 0u);
}

TEST(FarVolumeWindow, LateHeightFailuresPreserveBothTileAndDiscoveredCoverage) {
    const auto request = Window(1, -1, 0);
    const auto before = BuildFarVolumeWindow(request, Plane());
    auto output = before;
    for (const auto value : {std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity()}) {
        std::uint64_t height_calls = 0, density_calls = 0;
        const FarVolumeSamplers late_height{
            [&](float, float) { return ++height_calls == 130 ? value : 0.0f; },
            [&](const Vec3& p, float) {
                ++density_calls;
                return FarVolumeDensity{p.y, 7};
            }};
        EXPECT_THROW(output = BuildFarVolumeWindow(request, late_height), std::invalid_argument);
        EXPECT_EQ(height_calls, 130u);
        EXPECT_EQ(density_calls, 0u);
        ExpectSameTile(output, before);
        const auto previous_coverage = DiscoverFarVolumeWindow({}, Plane().height);
        auto coverage = previous_coverage;
        height_calls = 0;
        EXPECT_THROW(coverage = DiscoverFarVolumeWindow({}, late_height.height),
                     std::invalid_argument);
        EXPECT_EQ(height_calls, 130u);
        EXPECT_EQ(coverage, previous_coverage);
    }
}

TEST(FarVolumeWindow, FullSpansAndOrderedPagesHaveIdenticalBricksCrcAndOrientedGeometry) {
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        const auto tile_edge = static_cast<float>(kDimensions[tier - 1][2]);
        const FarVolumeSamplers field{
            Plane().height, [tile_edge](const Vec3& p, float) {
                const auto material = static_cast<std::uint8_t>(
                    (static_cast<std::uint64_t>(static_cast<std::int64_t>(p.x)) ^
                     static_cast<std::uint64_t>(static_cast<std::int64_t>(p.z))) &
                    255);
                return FarVolumeDensity{p.x + tile_edge * 0.5f + p.z * 0.125f + p.y * 0.125f,
                                        material};
            }};
        for (auto mode : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
            const FarVolumeRequest request{tier, -1, 0, FarVolumeSpan{-256, 64}, mode};
            const auto full = BuildFarVolumeTile(request, field);
            const auto plan = PlanFarVolumeWindows(DiscoverFarVolumeWindow(request, field.height),
                                                   LayerBudget(1));
            std::vector<FarVolumeTile> pages;
            std::vector<Triangle> triangles;
            for (std::uint32_t i = 0; i < plan.WindowCount(); ++i) {
                pages.push_back(
                    BuildFarVolumeWindow(FarVolumeWindowAt(plan, i), field, LayerBudget(1)));
                ValidateFarVolumeTile(pages.back(), LayerBudget(1));
                const auto part = Triangles(MarchingCubes::PolygoniseFarVolume(pages.back()));
                triangles.insert(triangles.end(), part.begin(), part.end());
                EXPECT_NE(pages.back().crc32, full.crc32);
                EXPECT_LE(pages.back().bricks.capacity(), 1024u);
            }
            // A naive page concatenation would break X-primary ordering.
            ASSERT_GE(pages.size(), 2u);
            ASSERT_FALSE(pages.front().bricks.empty());
            ASSERT_FALSE(pages[1].bricks.empty());
            EXPECT_LT(pages[1].bricks.front().key.x, pages.front().bricks.back().key.x);
            ExpectSameTile(Aggregate(pages, full), full);
            std::sort(triangles.begin(), triangles.end());
            EXPECT_EQ(triangles, Triangles(MarchingCubes::PolygoniseFarVolume(full)));
        }
    }
}

TEST(FarVolumeWindow, PagedTorusKeepsOutwardWallsAndUndersideAcrossTheZeroBoundary) {
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        const auto spacing = static_cast<float>(kDimensions[tier - 1][0]);
        const auto edge = static_cast<float>(kDimensions[tier - 1][1]);
        const Vec3 center(8 * spacing, 0, 8 * spacing);
        const auto major = 3 * spacing;
        const FarVolumeSamplers field{Plane().height, [=](const Vec3& p, float) {
                                          const auto v = p - center;
                                          const float radial = std::hypot(v.x, v.z);
                                          return FarVolumeDensity{
                                              std::hypot(radial - major, v.y) - 1.5f * spacing,
                                              static_cast<std::uint8_t>(v.y < 0 ? 17 : 23)};
                                      }};
        const FarVolumeRequest request{tier, 0, 0, FarVolumeSpan{-2 * edge, 2 * edge}};
        const auto full = BuildFarVolumeTile(request, field);
        const auto plan =
            PlanFarVolumeWindows(DiscoverFarVolumeWindow(request, field.height), LayerBudget(1));
        std::vector<FarVolumeTile> pages;
        std::vector<Triangle> triangles;
        std::size_t tops = 0, undersides = 0, inner_walls = 0;
        for (std::uint32_t i = 0; i < plan.WindowCount(); ++i) {
            pages.push_back(
                BuildFarVolumeWindow(FarVolumeWindowAt(plan, i), field, LayerBudget(1)));
            const auto mesh = MarchingCubes::PolygoniseFarVolume(pages.back());
            const auto part = Triangles(mesh);
            triangles.insert(triangles.end(), part.begin(), part.end());
            for (std::size_t j = 0; j < mesh.indices.size(); j += 3) {
                const auto a = mesh.vertices.at(mesh.indices.at(j)).position;
                const auto b = mesh.vertices.at(mesh.indices.at(j + 1)).position;
                const auto c = mesh.vertices.at(mesh.indices.at(j + 2)).position;
                const auto v = (a + b + c) / 3.0f - center;
                const auto radial = std::hypot(v.x, v.z);
                ASSERT_GT(radial, 0.0f);
                const Vec3 gradient(
                    (radial - major) * v.x / radial, v.y, (radial - major) * v.z / radial);
                EXPECT_GT(glm::dot(glm::cross(b - a, c - a), gradient), 0.0f);
                tops += v.y > 0;
                undersides += v.y < 0;
                inner_walls += radial < major;
            }
        }
        EXPECT_GT(tops, 0u);
        EXPECT_GT(undersides, 0u);
        EXPECT_GT(inner_walls, 0u);
        ExpectSameTile(Aggregate(pages, full), full);
        std::sort(triangles.begin(), triangles.end());
        EXPECT_EQ(triangles, Triangles(MarchingCubes::PolygoniseFarVolume(full)));
    }
}

TEST(FarVolumeWindow, PristineWrappersPreserveFullSamplesMaterialsAndBothCaveModesAtEveryTier) {
    Systems::TerrainGenParams params;
    params.base_amplitude = 0;
    params.height_offset = 12.25f;
    params.island_mask_enabled = false;
    params.caves_enabled = true;
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    const auto scope = world.acquire_worldgen_sample_scope();
    for (std::uint32_t tier = 1; tier <= 5; ++tier)
        for (const auto mode : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
            const FarVolumeRequest request{tier, -1, -2, FarVolumeSpan{-256, 64}, mode};
            const auto full = BuildPristineFarVolumeTile(world, request);
            const auto coverage = DiscoverPristineFarVolumeWindow(world, request);
            const auto plan = PlanFarVolumeWindows(coverage, LayerBudget(8));
            std::vector<FarVolumeTile> pages;
            for (std::uint32_t i = 0; i < plan.WindowCount(); ++i)
                pages.push_back(BuildPristineFarVolumeWindow(
                    world, FarVolumeWindowAt(plan, i), LayerBudget(8)));
            ExpectSameTile(Aggregate(pages, full), full);
        }
}

using Position = std::array<std::int64_t, 3>;
using BoundarySamples = std::map<Position, FarVolumeSample>;
BoundarySamples Samples(const FarVolumeTile& tile, std::int64_t step) {
    BoundarySamples samples;
    for (const auto& brick : tile.bricks)
        for (std::size_t i = 0; i < 125; ++i) {
            const Position position{(4ll * brick.key.x + static_cast<std::int64_t>(i % 5)) * step,
                                    (4ll * brick.key.y + static_cast<std::int64_t>((i / 5) % 5)) *
                                        step,
                                    (4ll * brick.key.z + static_cast<std::int64_t>(i / 25)) * step};
            const auto [existing, inserted] = samples.emplace(position, brick.samples[i]);
            if (!inserted)
                EXPECT_EQ(existing->second, brick.samples[i]);
        }
    return samples;
}

TEST(FarVolumeWindow, IndependentlyBuiltPagesShareYFacesAndXZEdgesAndCornersAtEveryTier) {
    const FarVolumeSamplers wall{Plane().height, [](const Vec3& p, float) {
                                     return FarVolumeDensity{p.x + 0.5f * p.z, 19};
                                 }};
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        BoundarySamples seen;
        std::size_t faces = 0, edges = 0, corners = 0;
        for (int y : {-1, 0})
            for (int z : {-1, 0})
                for (int x : {-1, 0}) {
                    auto request = Window(tier, y, y + 1);
                    request.tile_x = x;
                    request.tile_z = z;
                    const auto tile = BuildFarVolumeWindow(request, wall);
                    ValidateFarVolumeTile(tile);
                    for (const auto& [position, sample] : Samples(tile, kDimensions[tier - 1][0])) {
                        const auto [previous, inserted] = seen.emplace(position, sample);
                        if (inserted)
                            continue;
                        EXPECT_EQ(previous->second, sample);
                        if (position[1] == 0) {
                            const int extra =
                                (position[0] == 0 ? 1 : 0) + (position[2] == 0 ? 1 : 0);
                            faces += extra == 0;
                            edges += extra == 1;
                            corners += extra == 2;
                        }
                    }
                }
        EXPECT_GT(faces, 0u);
        EXPECT_GT(edges, 0u);
        EXPECT_GT(corners, 0u);
    }
}

TEST(FarVolumeWindow, CrossPageMaterialDisagreementIsVisibleEvenWithValidIndividualCrcs) {
    const FarVolumeSamplers wall{Plane().height, [](const Vec3& p, float) {
                                     return FarVolumeDensity{p.x + 0.5f * p.z, 19};
                                 }};
    auto request = Window(1, -1, 0);
    request.tile_x = request.tile_z = -1;
    auto below = BuildFarVolumeWindow(request, wall);
    request.window = {0, 16};
    const auto above = BuildFarVolumeWindow(request, wall);
    ASSERT_EQ(below.bricks.size(), 1u);
    below.bricks[0].samples[124].material = 255;
    below.bricks[0].crc32 = FarVolumeBrickCrc(below.bricks[0]);
    below.crc32 = FarVolumeTileCrc(below);
    EXPECT_NO_THROW(ValidateFarVolumeTile(below));
    EXPECT_NO_THROW(ValidateFarVolumeTile(above));
    EXPECT_NE(Samples(below, 4).at(Position{0, 0, 0}), Samples(above, 4).at(Position{0, 0, 0}));
    // This independent seam oracle is not a claimed runtime aggregate validator.
}

TEST(FarVolumeWindow, PagingRetainsAbsoluteSamplingPhaseAndHonestSubSpacingMisses) {
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        const auto spacing = static_cast<float>(kDimensions[tier - 1][0]);
        for (bool on_sample : {false, true}) {
            const auto center = spacing * (on_sample ? 1.0f : 0.5f);
            const FarVolumeSamplers field{Plane().height, [=](const Vec3& p, float) {
                                              return FarVolumeDensity{
                                                  std::abs(p.y - center) - spacing * 0.25f, 1};
                                          }};
            const auto full = BuildFarVolumeTile({tier, 0, 0}, field);
            const auto page = BuildFarVolumeWindow(Window(tier, 0, 1), field);
            EXPECT_EQ(full.bricks.size(), on_sample ? 1024u : 0u);
            ASSERT_EQ(page.bricks.size(), full.bricks.size());
            for (std::size_t i = 0; i < page.bricks.size(); ++i) {
                EXPECT_EQ(page.bricks[i].key, full.bricks[i].key);
                EXPECT_EQ(page.bricks[i].samples, full.bricks[i].samples);
                EXPECT_EQ(page.bricks[i].crc32, full.bricks[i].crc32);
            }
        }
    }
}
} // namespace
