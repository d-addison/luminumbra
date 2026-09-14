#include "luminumbra_client/rendering/FarVolumeRenderMesh.h"
#include "luminumbra_common/world/MarchingCubes.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;

TEST(FarVolumeRenderMesh, KeepsAbsolutePositionsMaterialsAndGroundWallCeilingNormals) {
    // All positions are negative and far from a tile origin. Translating by the
    // legacy region origin or using a global-up normal violates these oracles.
    FarVolumeMesh source;
    const std::array<std::array<Vec3, 3>, 3> triangles{
        {{{{-1300, -24, -400}, {-1300, -24, -398}, {-1298, -24, -400}}},
         {{{-1400, -80, -700}, {-1400, -78, -700}, {-1400, -80, -698}}},
         {{{-1600, -10, -900}, {-1598, -10, -900}, {-1600, -10, -898}}}}};
    const std::array<Vec3, 3> normals{{{0, 1, 0}, {1, 0, 0}, {0, -1, 0}}};
    for (const auto& triangle : triangles)
        for (std::size_t corner = 0; corner < 3; ++corner) {
            source.indices.push_back(static_cast<std::uint32_t>(source.vertices.size()));
            source.vertices.push_back(
                {triangle[corner], static_cast<std::uint8_t>(1 + corner * 100)});
        }
    source.vertices.push_back({Vec3(1'000'000), 255}); // unreferenced: not geometry bounds
    const auto result = AdaptFarVolumeRenderMesh(source);
    ASSERT_EQ(result.vertices.size(), 9u);
    ASSERT_EQ(result.indices.size(), 9u);
    if (!result.bounds.has_value()) {
        FAIL() << "Expected result.bounds to contain a value";
    }
    EXPECT_EQ(result.bounds.value().min, Vec3(-1600, -80, -900));
    EXPECT_EQ(result.bounds.value().max, Vec3(-1298, -10, -398));
    for (std::size_t i = 0; i < 9; ++i) {
        EXPECT_EQ(result.indices[i], i);
        EXPECT_EQ(result.vertices[i].position, source.vertices[i].position);
        EXPECT_EQ(result.vertices[i].material_id, source.vertices[i].material);
        EXPECT_EQ(result.vertices[i].normal, normals[i / 3]);
    }
    EXPECT_EQ(result.owned_bytes(),
              result.vertices.capacity() * sizeof(VoxelVertex) +
                  result.indices.capacity() * sizeof(std::uint32_t));
}

TEST(FarVolumeRenderMesh, EmptyAndDegenerateGeometryHasNoInventedBounds) {
    const auto empty = AdaptFarVolumeRenderMesh({}, 0);
    EXPECT_TRUE(empty.vertices.empty());
    EXPECT_TRUE(empty.indices.empty());
    EXPECT_FALSE(empty.bounds.has_value());
    FarVolumeMesh line{{{{-1, -2, -3}, 1}, {{0, -2, -3}, 2}, {{1, -2, -3}, 3}}, {0, 1, 2}};
    const auto degenerate = AdaptFarVolumeRenderMesh(line, 0);
    EXPECT_TRUE(degenerate.vertices.empty());
    EXPECT_FALSE(degenerate.bounds.has_value());
}

FarVolumeMesh Triangle() {
    return {{{{0, 0, 0}, 1}, {{1, 0, 0}, 2}, {{0, 1, 0}, 3}}, {0, 1, 2}};
}

TEST(FarVolumeRenderMesh, RefusesInvalidIndicesPositionsAndWholeOutputBudget) {
    auto source = Triangle();
    constexpr auto exact_bytes = 3u * (sizeof(VoxelVertex) + sizeof(std::uint32_t));
    EXPECT_THROW(AdaptFarVolumeRenderMesh(source, exact_bytes - 1), std::length_error);
    EXPECT_EQ(AdaptFarVolumeRenderMesh(source, exact_bytes).vertices.size(), 3u);
    EXPECT_THROW(AdaptFarVolumeRenderMesh(source, std::numeric_limits<std::size_t>::max()),
                 std::invalid_argument);
    source.indices.push_back(0);
    EXPECT_THROW(AdaptFarVolumeRenderMesh(source), std::invalid_argument);
    source = Triangle();
    source.indices[0] = std::numeric_limits<std::uint32_t>::max();
    EXPECT_THROW(AdaptFarVolumeRenderMesh(source), std::invalid_argument);
    for (const auto invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        source = Triangle();
        source.vertices.push_back({Vec3(invalid), 0});
        EXPECT_THROW(AdaptFarVolumeRenderMesh(source), std::invalid_argument);
    }
}

TEST(FarVolumeRenderMesh, LargeFiniteCoordinatesKeepFiniteUnitNormals) {
    const auto big = std::numeric_limits<float>::max() / 4;
    FarVolumeMesh source{{{{-big, 0, 0}, 1}, {{big, 0, 0}, 1}, {{0, big, 0}, 1}}, {0, 1, 2}};
    const auto result = AdaptFarVolumeRenderMesh(source);
    ASSERT_EQ(result.vertices.size(), 3u);
    EXPECT_EQ(result.vertices.front().normal, Vec3(0, 0, 1));
}

TEST(FarVolumeRenderMesh, CorrectedCase65ComponentsRemainOutward) {
    const FarVolumeSamplers field{
        [](float, float) { return 0.0f; },
        [](const Vec3& position, float) {
            const auto q = position / 16.0f;
            return FarVolumeDensity{q == Vec3(1) ? -2.0f : q == Vec3(2) ? -1.0f : 1.0f, 7};
        }};
    const auto source =
        MarchingCubes::PolygoniseFarVolume(BuildFarVolumeTile({3, 0, 0, std::nullopt}, field));
    const auto result = AdaptFarVolumeRenderMesh(source);
    ASSERT_EQ(result.indices.size(), 48u);
    std::array<std::size_t, 2> counts{};
    for (std::size_t i = 0; i < result.vertices.size(); i += 3) {
        const auto center = (result.vertices[i].position + result.vertices[i + 1].position +
                             result.vertices[i + 2].position) /
                            48.0f;
        const std::size_t component =
            glm::length(center - Vec3(1)) < glm::length(center - Vec3(2)) ? 0u : 1u;
        const Vec3 radial = center - Vec3(static_cast<float>(component + 1));
        EXPECT_GT(glm::dot(result.vertices[i].normal, radial), 0.0f);
        ++counts[component];
        for (std::size_t corner = 0; corner < 3; ++corner) {
            EXPECT_EQ(result.vertices[i + corner].position,
                      source.vertices[source.indices[i + corner]].position);
            EXPECT_EQ(result.vertices[i + corner].material_id, 7u);
        }
    }
    EXPECT_EQ(counts, (std::array<std::size_t, 2>{8, 8}));
}

TEST(FarVolumeRenderMesh, TorusWallsAndUndersideFollowIndependentAnalyticGradient) {
    const Vec3 center(96, -64, 96);
    const FarVolumeSamplers field{
        [](float, float) { return 0.0f; },
        [center](const Vec3& p, float) {
            const auto q = p - center;
            const auto radial = std::sqrt(q.x * q.x + q.z * q.z);
            return FarVolumeDensity{std::sqrt((radial - 56) * (radial - 56) + q.y * q.y) - 24, 1};
        }};
    const auto source =
        MarchingCubes::PolygoniseFarVolume(BuildFarVolumeTile({2, 0, 0, std::nullopt}, field));
    const auto result = AdaptFarVolumeRenderMesh(source);
    ASSERT_FALSE(result.indices.empty());
    std::size_t ceiling = 0, ground = 0, wall = 0;
    for (std::size_t i = 0; i < result.vertices.size(); i += 3) {
        const auto q = (result.vertices[i].position + result.vertices[i + 1].position +
                        result.vertices[i + 2].position) /
                           3.0f -
                       center;
        const auto radius = std::sqrt(q.x * q.x + q.z * q.z);
        ASSERT_GT(radius, 0.0f);
        const Vec3 gradient((radius - 56) * q.x / radius, q.y, (radius - 56) * q.z / radius);
        const auto n = result.vertices[i].normal;
        EXPECT_GT(glm::dot(n, glm::normalize(gradient)), 0.5f);
        EXPECT_NEAR(glm::length(n), 1.0f, 1.0e-6f);
        ceiling += n.y < -0.5f ? 1u : 0u;
        ground += n.y > 0.5f ? 1u : 0u;
        wall += std::abs(n.y) < 0.25f ? 1u : 0u;
    }
    EXPECT_GT(ceiling, 0u);
    EXPECT_GT(ground, 0u);
    EXPECT_GT(wall, 0u);
}

} // namespace
