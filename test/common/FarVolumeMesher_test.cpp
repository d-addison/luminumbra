#include "luminumbra_common/world/FarVolumeMesher.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace {
using namespace Luminumbra::World;
// Independent accepted dimensions, not values read back from the production table.
constexpr std::array<std::int64_t, 5> steps{4, 8, 16, 32, 64};
constexpr std::array<std::int64_t, 5> brick_edges{16, 32, 64, 128, 256};
constexpr std::array<std::int64_t, 5> tile_edges{512, 1024, 2048, 4096, 8192};
constexpr FarVolumeMeshLimits allowance{2000000, 4000000, 1000000, 4000000};
constexpr std::uint64_t identity = 731;

FarVolumeTile build(FarVolumeRequest request, const FarVolumeSampler& sampler) {
    FarVolumeTile tile;
    EXPECT_TRUE(BuildFarVolumeTile(request, {32768, 32768}, sampler, tile));
    return tile;
}
FarVolumeMesh mesh(const FarVolumeTile& tile, const FarVolumeSampler& sampler) {
    FarVolumeMesh result;
    FarVolumeMeshError error;
    EXPECT_TRUE(MeshFarVolumeTile(tile, identity, sampler, allowance, result, &error));
    EXPECT_EQ(error, FarVolumeMeshError::None);
    return result;
}
FarVolumeSample plane(const FarVolumePosition& p) {
    return {static_cast<float>(p.y) + 2.0f, p.y < -2 ? std::uint8_t{255} : std::uint8_t{19}};
}
FarVolumeTile baseline() {
    return build({{1, -1, -1}, {-16, 0}, identity}, plane);
}
FarVolumeVector world_position(const FarVolumeMesh& value, const FarVolumeVertex& vertex) {
    return {static_cast<double>(value.origin_meters.x) + vertex.position[0],
            static_cast<double>(value.origin_meters.y) + vertex.position[1],
            static_cast<double>(value.origin_meters.z) + vertex.position[2]};
}
FarVolumeVector face(const FarVolumeVector& a, const FarVolumeVector& b, const FarVolumeVector& c) {
    const FarVolumeVector u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const FarVolumeVector v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    return {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
}
void check_geometry(const FarVolumeMesh& value) {
    ASSERT_FALSE(value.vertices.empty());
    ASSERT_TRUE(value.bounds);
    ASSERT_EQ(value.indices.size() % 3, 0u);
    auto lo = value.vertices.front().position, hi = lo;
    std::set<std::uint32_t> used;
    for (auto index : value.indices) {
        ASSERT_LT(index, value.vertices.size());
        used.insert(index);
    }
    EXPECT_EQ(used.size(), value.vertices.size());
    for (const auto& vertex : value.vertices) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            EXPECT_TRUE(std::isfinite(vertex.position[axis]));
            EXPECT_TRUE(std::isfinite(vertex.normal[axis]));
            lo[axis] = std::min(lo[axis], vertex.position[axis]);
            hi[axis] = std::max(hi[axis], vertex.position[axis]);
        }
        EXPECT_NEAR(std::hypot(vertex.normal[0], vertex.normal[1], vertex.normal[2]), 1.0, 1e-12);
    }
    EXPECT_EQ(value.bounds->min, lo);
    EXPECT_EQ(value.bounds->max, hi);
    for (std::size_t i = 0; i < value.indices.size(); i += 3) {
        const auto& a = value.vertices[value.indices[i]];
        const auto& b = value.vertices[value.indices[i + 1]];
        const auto& c = value.vertices[value.indices[i + 2]];
        const auto normal = face(a.position, b.position, c.position);
        EXPECT_GT(std::hypot(normal[0], normal[1], normal[2]), 0.0);
    }
}

TEST(FarVolumeMesher, AllFiveTiersMeshAnalyticWallsAndCeilingsAtNegativeOrigins) {
    for (std::size_t tier = 1; tier <= 5; ++tier) {
        const auto edge = brick_edges[tier - 1], tile_edge = tile_edges[tier - 1];
        for (int axis : {0, 1, 2}) {
            const double cut = axis == 1 ? -1.5 * static_cast<double>(steps[tier - 1])
                                         : -static_cast<double>(tile_edge) +
                                               1.5 * static_cast<double>(steps[tier - 1]);
            const double sign = axis == 1 ? -1.0 : 1.0; // Ceiling's outside points downward.
            const auto field = [=](const FarVolumePosition& p) -> FarVolumeSample {
                const auto coordinate = axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
                const auto density =
                    static_cast<float>(sign * (static_cast<double>(coordinate) - cut));
                return {density, density < 0 ? std::uint8_t{255} : std::uint8_t{19}};
            };
            const auto tile = build({{tier, -1, -1}, {-edge, 0}, identity}, field);
            const auto result = mesh(tile, field);
            check_geometry(result);
            EXPECT_EQ(result.request, tile.request);
            EXPECT_EQ(result.origin_meters, tile.min_meters);
            EXPECT_EQ(result.sampled_max_meters, tile.max_meters);
            EXPECT_EQ(result.cells_visited, tile.bricks.size() * 64);
            for (const auto& vertex : result.vertices) {
                EXPECT_DOUBLE_EQ(world_position(result, vertex)[static_cast<std::size_t>(axis)],
                                 cut);
                EXPECT_EQ(vertex.material, 255u); // Never interprets 255 as a background lookup.
                for (std::size_t a = 0; a < 3; ++a)
                    EXPECT_DOUBLE_EQ(vertex.normal[a],
                                     a == static_cast<std::size_t>(axis) ? sign : 0.0);
            }
            for (std::size_t i = 0; i < result.indices.size(); i += 3) {
                const auto normal = face(result.vertices[result.indices[i]].position,
                                         result.vertices[result.indices[i + 1]].position,
                                         result.vertices[result.indices[i + 2]].position);
                EXPECT_GT(normal[static_cast<std::size_t>(axis)] * sign, 0.0);
            }
            // Independent cell/triangle/vertex counts prove full rectangular coverage.
            const std::size_t side = axis == 1 ? 128 : 4;
            EXPECT_EQ(result.vertices.size(), 129 * (side + 1));
            EXPECT_EQ(result.indices.size(), 128 * side * 6);
        }
    }
}

TEST(FarVolumeMesher, CurvedSharedBordersHaveIdenticalPositionsMaterialsAndNormalsForAllTiers) {
    using Border = std::map<FarVolumeVector, FarVolumeVector>;
    for (std::size_t tier = 1; tier <= 5; ++tier) {
        const auto edge = brick_edges[tier - 1], tile_edge = tile_edges[tier - 1];
        const auto step = steps[tier - 1];
        // A gently curved bowl spans one brick vertically and crosses both tile borders.
        // Squared terms are powers-of-two scaled, preserving analytic central gradients.
        const auto field = [=](const FarVolumePosition& p) -> FarVolumeSample {
            const double x = static_cast<double>(p.x), z = static_cast<double>(p.z);
            return {static_cast<float>(static_cast<double>(p.y) + static_cast<double>(step) * 2.5 +
                                       (x * x + z * z) / (static_cast<double>(tile_edge) * 128.0)),
                    42};
        };
        const auto left = mesh(build({{tier, -1, -1}, {-edge, 0}, identity}, field), field);
        for (int axis : {0, 2}) {
            const auto right =
                mesh(build({{tier, axis == 0 ? 0 : -1, axis == 2 ? 0 : -1}, {-edge, 0}, identity},
                           field),
                     field);
            const auto border = [axis](const FarVolumeMesh& value) {
                Border vertices;
                for (const auto& vertex : value.vertices) {
                    const auto p = world_position(value, vertex);
                    if (p[static_cast<std::size_t>(axis)] == 0.0) {
                        EXPECT_EQ(vertex.material, 42u);
                        EXPECT_TRUE(vertices.emplace(p, vertex.normal).second);
                    }
                }
                return vertices;
            };
            const auto a = border(left), b = border(right);
            ASSERT_GE(a.size(), 129u);
            EXPECT_EQ(a, b); // Exact double normal equality, not merely visually close.
            for (const auto& [p, normal] : a) {
                const double gx = p[0] / (static_cast<double>(tile_edge) * 64.0);
                const double gz = p[2] / (static_cast<double>(tile_edge) * 64.0);
                const double length = std::hypot(gx, 1.0, gz);
                EXPECT_NEAR(normal[0], gx / length, 1e-12);
                EXPECT_NEAR(normal[1], 1.0 / length, 1e-12);
                EXPECT_NEAR(normal[2], gz / length, 1e-12);
            }
        }
    }
}

TEST(FarVolumeMesher, CrossingsAndVerticalBordersUseVolumeBoundsWithoutHeightClamping) {
    for (const std::int64_t origin_y : {-16384, 16384}) {
        const auto field = [=](const FarVolumePosition& p) -> FarVolumeSample {
            return {static_cast<float>(p.x + p.y - origin_y + p.z) - 17.0f, 7};
        };
        const auto lower =
            mesh(build({{1, 0, 0}, {origin_y, origin_y + 16}, identity}, field), field);
        const auto upper =
            mesh(build({{1, 0, 0}, {origin_y + 16, origin_y + 32}, identity}, field), field);
        check_geometry(lower);
        check_geometry(upper);
        using Border = std::map<FarVolumeVector, FarVolumeVector>;
        const auto border = [=](const FarVolumeMesh& value) {
            Border result;
            for (const auto& vertex : value.vertices) {
                const auto p = world_position(value, vertex);
                EXPECT_NEAR(p[0] + p[1] - static_cast<double>(origin_y) + p[2], 17.0, 1e-10);
                EXPECT_NEAR(vertex.normal[0], 1.0 / std::sqrt(3.0), 1e-12);
                EXPECT_NEAR(vertex.normal[1], 1.0 / std::sqrt(3.0), 1e-12);
                EXPECT_NEAR(vertex.normal[2], 1.0 / std::sqrt(3.0), 1e-12);
                if (p[1] == static_cast<double>(origin_y + 16))
                    result.emplace(p, vertex.normal);
            }
            return result;
        };
        const auto a = border(lower), b = border(upper);
        EXPECT_FALSE(a.empty());
        EXPECT_EQ(a, b);
        ASSERT_TRUE(upper.bounds);
        EXPECT_DOUBLE_EQ(upper.bounds->max[1], 1.0);
    }
}

TEST(FarVolumeMesher, IndependentColdRebuildAndReorderedBricksAreBitIdentical) {
    const auto first_tile = baseline();
    const auto first = mesh(first_tile, plane);
    auto second_tile = baseline();
    std::reverse(second_tile.bricks.begin(), second_tile.bricks.end());
    const auto second = mesh(second_tile, plane);
    EXPECT_EQ(first, second);
    ASSERT_EQ(first.vertices.size(), second.vertices.size());
    for (std::size_t i = 0; i < first.vertices.size(); ++i)
        for (std::size_t axis = 0; axis < 3; ++axis) {
            EXPECT_EQ(std::bit_cast<std::uint64_t>(first.vertices[i].position[axis]),
                      std::bit_cast<std::uint64_t>(second.vertices[i].position[axis]));
            EXPECT_EQ(std::bit_cast<std::uint64_t>(first.vertices[i].normal[axis]),
                      std::bit_cast<std::uint64_t>(second.vertices[i].normal[axis]));
        }
}

TEST(FarVolumeMesher, HomogeneousTilesHaveValidEmptyMetadataAtEveryTier) {
    for (std::size_t tier = 1; tier <= 5; ++tier)
        for (float density : {-1.0f, 0.0f, 1.0f}) {
            const auto field = [density](const FarVolumePosition&) {
                return FarVolumeSample{density, 8};
            };
            const auto tile = build({{tier, -1, 2}, {-brick_edges[tier - 1], 0}, identity}, field);
            FarVolumeMesh result;
            ASSERT_TRUE(MeshFarVolumeTile(tile, identity, field, {}, result));
            EXPECT_EQ(result.request, tile.request);
            EXPECT_EQ(result.origin_meters, tile.min_meters);
            EXPECT_EQ(result.sampled_max_meters, tile.max_meters);
            EXPECT_EQ(result.content,
                      density < 0.0f ? FarVolumeContent::Solid : FarVolumeContent::Air);
            EXPECT_TRUE(result.vertices.empty());
            EXPECT_TRUE(result.indices.empty());
            EXPECT_FALSE(result.bounds);
            EXPECT_EQ(result.cells_visited, 0u);
            EXPECT_EQ(result.samples_evaluated, 0u);
        }
}

TEST(FarVolumeMesher, ExactZeroAndSubnormalCrossingsAreNotEpsilonSnapped) {
    for (const bool subnormal : {false, true}) {
        const auto field = [subnormal](const FarVolumePosition& p) -> FarVolumeSample {
            const float density = static_cast<float>(p.y + (subnormal ? 2 : 0));
            return {subnormal ? density * std::numeric_limits<float>::denorm_min() : density, 11};
        };
        const auto result = mesh(build({{1, 0, 0}, {-16, 0}, identity}, field), field);
        check_geometry(result);
        EXPECT_EQ(result.vertices.size(), 129u * 129u);
        EXPECT_EQ(result.indices.size(), 128u * 128u * 6u);
        for (const auto& vertex : result.vertices) {
            EXPECT_DOUBLE_EQ(world_position(result, vertex)[1], subnormal ? -2.0 : 0.0);
            EXPECT_EQ(vertex.normal, (FarVolumeVector{0, 1, 0}));
        }
    }
}

TEST(FarVolumeMesher, IsolatedZeroPointProducesValidEmptyMixedGeometry) {
    // A zero-measure point has crossing bricks, but every table triangle collapses.
    const auto field = [](const FarVolumePosition& p) {
        return FarVolumeSample{p == FarVolumePosition{4, 4, 4} ? 0.0f : -1.0f, 6};
    };
    const auto tile = build({{1, 0, 0}, {0, 16}, identity}, field);
    ASSERT_EQ(tile.bricks.size(), 1u);
    FarVolumeMesh result;
    ASSERT_TRUE(MeshFarVolumeTile(tile, identity, field, {64, 125, 0, 0}, result));
    EXPECT_EQ(result.content, FarVolumeContent::Mixed);
    EXPECT_EQ(result.request, tile.request);
    EXPECT_EQ(result.origin_meters, tile.min_meters);
    EXPECT_EQ(result.sampled_max_meters, tile.max_meters);
    EXPECT_EQ(result.cells_visited, 64u);
    EXPECT_EQ(result.samples_evaluated, 125u);
    EXPECT_TRUE(result.vertices.empty());
    EXPECT_TRUE(result.indices.empty());
    EXPECT_FALSE(result.bounds);
}

TEST(FarVolumeMesher, SampleCacheChargesUniqueCoordinatesAndExactBudgetsSucceed) {
    const auto tile = baseline();
    std::set<std::tuple<std::int64_t, std::int64_t, std::int64_t>> seen;
    const auto field = [&](const FarVolumePosition& p) -> FarVolumeSample {
        EXPECT_TRUE(seen.emplace(p.x, p.y, p.z).second);
        return plane(p);
    };
    const auto first = mesh(tile, field);
    EXPECT_EQ(first.samples_evaluated, seen.size());
    EXPECT_LT(first.samples_evaluated, tile.samples_evaluated);
    FarVolumeMesh second;
    const FarVolumeMeshLimits exact{
        first.cells_visited, first.samples_evaluated, first.vertices.size(), first.indices.size()};
    ASSERT_TRUE(MeshFarVolumeTile(tile, identity, plane, exact, second));
    EXPECT_EQ(first, second);
    for (int budget = 0; budget < 4; ++budget) {
        auto limits = exact;
        FarVolumeMeshError expected = FarVolumeMeshError::CellLimit;
        if (budget == 0)
            --limits.max_cells;
        if (budget == 1) {
            --limits.max_sample_evaluations;
            expected = FarVolumeMeshError::SampleLimit;
        }
        if (budget == 2) {
            --limits.max_vertices;
            expected = FarVolumeMeshError::VertexLimit;
        }
        if (budget == 3) {
            --limits.max_indices;
            expected = FarVolumeMeshError::IndexLimit;
        }
        FarVolumeMeshError error;
        EXPECT_FALSE(MeshFarVolumeTile(tile, identity, plane, limits, second, &error));
        EXPECT_EQ(error, expected);
        EXPECT_EQ(second, first);
    }
}

TEST(FarVolumeMesher, MalformedMetadataAndResidentSamplesRefuseTransactionally) {
    const auto original = baseline();
    const auto previous = mesh(original, plane);
    const auto check = [&](FarVolumeTile tile, FarVolumeMeshError expected) {
        auto output = previous;
        FarVolumeMeshError error;
        EXPECT_FALSE(MeshFarVolumeTile(tile, identity, plane, allowance, output, &error));
        EXPECT_EQ(error, expected);
        EXPECT_EQ(output, previous);
    };
    for (int mode = 0; mode < 10; ++mode) {
        auto tile = original;
        if (mode == 0)
            tile.request.key.tier = 0;
        if (mode == 1)
            ++tile.request.span.min_y;
        if (mode == 2)
            ++tile.max_meters.x;
        if (mode == 3)
            --tile.candidates_visited;
        if (mode == 4)
            --tile.samples_evaluated;
        if (mode == 5)
            tile.content = FarVolumeContent::Air;
        if (mode == 6)
            ++tile.bricks.back().origin_meters.x;
        if (mode == 7)
            tile.bricks.back().origin_meters.y += 16;
        if (mode == 8)
            tile.bricks.back() = tile.bricks.front();
        if (mode == 9)
            tile.request.key.x = std::numeric_limits<std::int64_t>::max();
        check(std::move(tile), FarVolumeMeshError::InvalidTile);
    }
    auto tile = original;
    tile.bricks.back().samples[0].density += 1.0f; // Shared sample conflicts with cached neighbour.
    check(tile, FarVolumeMeshError::SampleMismatch);
    tile = original;
    ++tile.bricks.back().samples[0].material;
    check(tile, FarVolumeMeshError::SampleMismatch);
    tile = original;
    tile.bricks.back().samples[0].density = std::numeric_limits<float>::quiet_NaN();
    check(tile, FarVolumeMeshError::NonFiniteDensity);
}

TEST(FarVolumeMesher, AuthorityIdentityAndDensityBitsMustMatchIncludingSignedZero) {
    auto tile = baseline();
    auto output = mesh(tile, plane);
    const auto previous = output;
    FarVolumeMeshError error;
    std::uint64_t calls = 0;
    const auto field = [&](const FarVolumePosition& p) {
        ++calls;
        return plane(p);
    };
    EXPECT_FALSE(MeshFarVolumeTile(tile, identity + 1, field, allowance, output, &error));
    EXPECT_EQ(error, FarVolumeMeshError::FieldIdentityMismatch);
    EXPECT_EQ(calls, 0u);
    EXPECT_FALSE(MeshFarVolumeTile(tile, identity, {}, allowance, output, &error));
    EXPECT_EQ(error, FarVolumeMeshError::InvalidSampler);
    const auto zero = [](const FarVolumePosition& p) {
        return FarVolumeSample{static_cast<float>(p.y), 0};
    };
    tile = build({{1, 0, 0}, {-16, 0}, identity}, zero);
    const auto wrong_zero = [&](const FarVolumePosition& p) {
        auto value = zero(p);
        if (value.density == 0.0f)
            value.density = -0.0f;
        return value;
    };
    EXPECT_FALSE(MeshFarVolumeTile(tile, identity, wrong_zero, allowance, output, &error));
    EXPECT_EQ(error, FarVolumeMeshError::SampleMismatch);
    EXPECT_EQ(output, previous);
}

TEST(FarVolumeMesher, HaloRefusalNonFiniteSamplesAndExceptionsPreservePreviousOutput) {
    const auto tile = baseline();
    const auto previous = mesh(tile, plane);
    for (int mode = 0; mode < 5; ++mode) {
        auto output = previous;
        bool halo_reached = false;
        const auto field = [&](const FarVolumePosition& p) -> std::optional<FarVolumeSample> {
            if (p.x < -512 || p.x > 0 || p.y < -16 || p.y > 0 || p.z < -512 || p.z > 0) {
                halo_reached = true;
                if (mode == 0)
                    return std::nullopt;
                if (mode == 1)
                    throw std::runtime_error("cancelled sampler");
                const auto density = mode == 2
                                         ? std::numeric_limits<float>::quiet_NaN()
                                         : (mode == 3 ? std::numeric_limits<float>::infinity()
                                                      : -std::numeric_limits<float>::infinity());
                return FarVolumeSample{density, 0};
            }
            return plane(p);
        };
        FarVolumeMeshError error;
        EXPECT_FALSE(MeshFarVolumeTile(tile, identity, field, allowance, output, &error));
        EXPECT_TRUE(halo_reached);
        EXPECT_EQ(error,
                  mode < 2 ? FarVolumeMeshError::SamplerFailure
                           : FarVolumeMeshError::NonFiniteDensity);
        EXPECT_EQ(output, previous);
    }
}

TEST(FarVolumeMesher, RequiredHaloAtIntegerLimitRefusesWithoutOverflow) {
    constexpr auto lowest = std::numeric_limits<std::int64_t>::min();
    const auto field = [](const FarVolumePosition& p) {
        return FarVolumeSample{static_cast<float>(p.y) + 2.0f, 1};
    };
    const auto tile = build({{1, lowest / 512, 0}, {-16, 0}, identity}, field);
    auto output = mesh(baseline(), plane);
    const auto previous = output;
    FarVolumeMeshError error;
    EXPECT_FALSE(MeshFarVolumeTile(tile, identity, field, allowance, output, &error));
    EXPECT_EQ(error, FarVolumeMeshError::CoordinateOverflow);
    EXPECT_EQ(output, previous);
}

TEST(FarVolumeMesher, ZeroInterpolatedGradientRefusesInsteadOfPublishingSeamNormals) {
    // Alternating X samples produce crossings but central differences cancel exactly.
    const auto field = [](const FarVolumePosition& p) {
        return FarVolumeSample{(p.x / 4) % 2 == 0 ? -1.0f : 1.0f, 2};
    };
    const auto tile = build({{1, 0, 0}, {0, 16}, identity}, field);
    auto output = mesh(baseline(), plane);
    const auto previous = output;
    FarVolumeMeshError error;
    EXPECT_FALSE(MeshFarVolumeTile(tile, identity, field, allowance, output, &error));
    EXPECT_EQ(error, FarVolumeMeshError::ZeroGradient);
    EXPECT_EQ(output, previous);
}

TEST(FarVolumeMesher, UnequalDisconnectedComponentsAndCavitiesKeepOutwardWinding) {
    // Case 65 (and complement 190) in the cell between the two interior samples.
    // Independent oracle: the continuous trilinear field is two tensor-product tents.
    for (std::size_t tier = 1; tier <= 5; ++tier) {
        const auto spacing = steps[tier - 1];
        for (const auto amplitudes : {std::array<double, 2>{2, 1}, {1, 2}, {4, .5}, {.5, 4}}) {
            for (double sign : {1.0, -1.0}) {
                const auto field = [=](const FarVolumePosition& p) {
                    const double value =
                        p == FarVolumePosition{spacing, spacing, spacing}
                            ? -amplitudes[0]
                            : (p == FarVolumePosition{2 * spacing, 2 * spacing, 2 * spacing}
                                   ? -amplitudes[1]
                                   : 1.0);
                    return FarVolumeSample{static_cast<float>(sign * value), 13};
                };
                const auto tile =
                    build({{tier, 0, 0}, {0, brick_edges[tier - 1]}, identity}, field);
                ASSERT_EQ(tile.bricks.size(), 1u);
                const auto result = mesh(tile, field);
                ASSERT_EQ(result.indices.size(), 48u);
                const auto density = [=](const FarVolumeVector& p) {
                    const auto tent = [&](double center) {
                        return std::max(0.0, 1.0 - std::abs(p[0] - center)) *
                               std::max(0.0, 1.0 - std::abs(p[1] - center)) *
                               std::max(0.0, 1.0 - std::abs(p[2] - center));
                    };
                    return sign * (1.0 - (1.0 + amplitudes[0]) * tent(1) -
                                   (1.0 + amplitudes[1]) * tent(2));
                };
                std::array<std::size_t, 2> faces{};
                for (std::size_t i = 0; i < result.indices.size(); i += 3) {
                    std::array<FarVolumeVector, 3> p{};
                    for (std::size_t c = 0; c < 3; ++c)
                        for (std::size_t axis = 0; axis < 3; ++axis)
                            p[c][axis] = result.vertices[result.indices[i + c]].position[axis] /
                                         static_cast<double>(spacing);
                    const FarVolumeVector center{(p[0][0] + p[1][0] + p[2][0]) / 3.0,
                                                 (p[0][1] + p[1][1] + p[2][1]) / 3.0,
                                                 (p[0][2] + p[1][2] + p[2][2]) / 3.0};
                    auto normal = face(p[0], p[1], p[2]);
                    const double length = std::hypot(normal[0], normal[1], normal[2]);
                    ASSERT_GT(length, 0.0);
                    for (auto& v : normal)
                        v /= length;
                    auto outside = center, inside = center;
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        outside[axis] += normal[axis] * .001;
                        inside[axis] -= normal[axis] * .001;
                    }
                    const std::size_t component =
                        std::hypot(center[0] - 1, center[1] - 1, center[2] - 1) <
                                std::hypot(center[0] - 2, center[1] - 2, center[2] - 2)
                            ? 0
                            : 1;
                    ++faces[component];
                    EXPECT_GT(density(outside), density(inside))
                        << "tier=" << tier << " triangle=" << i / 3;
                    double outward = 0;
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        outward += normal[axis] * sign *
                                   (center[axis] - static_cast<double>(component + 1));
                    EXPECT_GT(outward, 0.0);
                }
                EXPECT_EQ(faces, (std::array<std::size_t, 2>{8, 8}));
            }
        }
    }
}

TEST(FarVolumeMesher, MixedSharedFacesAndComplementsAgreeAcrossAllAxesAndTiers) {
    using Segment = std::array<FarVolumeVector, 2>;
    struct Border {
        std::map<FarVolumeVector, FarVolumeVector> normals;
        std::map<Segment, std::size_t> segments;
        bool operator==(const Border&) const = default;
    };
    for (std::size_t tier = 1; tier <= 5; ++tier) {
        const auto spacing = steps[tier - 1], edge = brick_edges[tier - 1];
        for (std::size_t axis = 0; axis < 3; ++axis) {
            for (const double sign : {1.0, -1.0}) {
                const FarVolumePosition a{
                    axis == 0 ? 0 : spacing, axis == 1 ? 0 : spacing, axis == 2 ? 0 : spacing};
                const FarVolumePosition b{2 * a.x, 2 * a.y, 2 * a.z};
                const auto field = [=](const FarVolumePosition& p) {
                    return FarVolumeSample{
                        static_cast<float>(sign * (p == a ? -2.0 : (p == b ? -1.0 : 1.0))), 31};
                };
                FarVolumeRequest left{{tier, axis == 0 ? -1 : 0, axis == 2 ? -1 : 0},
                                      {axis == 1 ? -edge : 0, axis == 1 ? 0 : edge},
                                      identity};
                FarVolumeRequest right{{tier, 0, 0}, {0, edge}, identity};
                const auto left_mesh = mesh(build(left, field), field);
                const auto right_mesh = mesh(build(right, field), field);
                const auto border = [&](const FarVolumeMesh& value) {
                    Border result;
                    for (const auto& vertex : value.vertices) {
                        const auto p = world_position(value, vertex);
                        if (p[axis] == 0.0) {
                            EXPECT_EQ(vertex.material, 31u);
                            result.normals.emplace(p, vertex.normal);
                        }
                    }
                    for (std::size_t i = 0; i < value.indices.size(); i += 3)
                        for (std::size_t j = 0; j < 3; ++j) {
                            Segment segment{
                                world_position(value, value.vertices[value.indices[i + j]]),
                                world_position(value,
                                               value.vertices[value.indices[i + (j + 1) % 3]])};
                            if (segment[0][axis] == 0.0 && segment[1][axis] == 0.0) {
                                if (segment[1] < segment[0])
                                    std::swap(segment[0], segment[1]);
                                ++result.segments[segment];
                            }
                        }
                    return result;
                };
                const auto first = border(left_mesh), second = border(right_mesh);
                EXPECT_GE(first.normals.size(), 6u);
                EXPECT_GE(first.segments.size(), 4u);
                EXPECT_EQ(first, second) << "tier=" << tier << " axis=" << axis << " sign=" << sign;
            }
        }
    }
}
} // namespace
