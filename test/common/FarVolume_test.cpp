#include "systems/SHIELD_WorldSystem.h"
#include "world/FarLodStore.h"
#include "world/FarVolume.h"
#include "world/MarchingCubes.h"
#include "world/TerrainPresetLoader.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::Systems::TerrainGenParams;

FarVolumeSamplers Plane(float height) {
    return {[height](float, float) { return height; },
            [](const Vec3& p, float h) {
                return FarVolumeDensity{p.y - h, 1};
            }};
}

TEST(FarVolume, SurfaceBandIsSparseAlignedAndExtensibleAtEveryTier) {
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        SCOPED_TRACE(tier);
        FarVolumeRequest request{tier, -1, 0};
        const auto tile = BuildFarVolumeTile(request, Plane(10.25f));
        const auto dimensions = FarTierAt(tier);
        if (!dimensions.has_value()) {
            ADD_FAILURE() << "tier " << tier << " is absent from the table";
            continue;
        }
        const auto d = *dimensions;
        ValidateFarVolumeTile(tile);
        EXPECT_LE(static_cast<float>(tile.first_brick_y * static_cast<int>(d.brick_edge_meters)),
                  10.25f - 256.0f);
        ASSERT_EQ(tile.bricks.size(), 1024u);
        for (const auto& brick : tile.bricks) {
            EXPECT_GE(brick.key.x, -32);
            EXPECT_LT(brick.key.x, 0);
            EXPECT_EQ(brick.key.y, 0);
            for (std::size_t z = 0; z < 5; ++z)
                for (std::size_t y = 0; y < 5; ++y)
                    for (std::size_t x = 0; x < 5; ++x) {
                        const auto& sample = brick.samples[FarVolumeSampleIndex(x, y, z)];
                        EXPECT_EQ(sample.density,
                                  QuantizeFarLodSdf(
                                      static_cast<float>(y * d.sample_spacing_meters) - 10.25f));
                    }
        }
        request.extra_span = FarVolumeSpan{-4097.0f, 333.0f};
        FarVolumeLimits limits;
        limits.max_density_samples = 40'000'000;
        limits.max_buffer_bytes = 512u * 1024u * 1024u;
        const auto extended = BuildFarVolumeTile(request, Plane(10.25f), limits);
        EXPECT_LT(extended.first_brick_y, tile.first_brick_y);
        EXPECT_GT(extended.last_brick_y, tile.last_brick_y);
        EXPECT_EQ(extended.bricks.size(), tile.bricks.size());
        for (std::size_t i = 0; i < tile.bricks.size(); ++i) {
            EXPECT_EQ(extended.bricks[i].samples, tile.bricks[i].samples);
            EXPECT_EQ(extended.bricks[i].key, tile.bricks[i].key);
        }
    }
}

TEST(FarVolume, RefusesInvalidAndIncompleteRequests) {
    auto request = FarVolumeRequest{};
    const auto sample = Plane(0.0f);
    request.tier = 0;
    EXPECT_THROW(BuildFarVolumeTile(request, sample), std::invalid_argument);
    request.tier = 6;
    EXPECT_THROW(BuildFarVolumeTile(request, sample), std::invalid_argument);
    request = {};
    request.extra_span = FarVolumeSpan{20.0f, -20.0f};
    EXPECT_THROW(BuildFarVolumeTile(request, sample), std::invalid_argument);
    request.extra_span = FarVolumeSpan{-1.0f, std::numeric_limits<float>::infinity()};
    EXPECT_THROW(BuildFarVolumeTile(request, sample), std::invalid_argument);
    request = {};
    request.tile_x = std::numeric_limits<std::int32_t>::max();
    EXPECT_THROW(BuildFarVolumeTile(request, sample), std::invalid_argument);
    request = {};
    EXPECT_THROW(BuildFarVolumeTile(request, sample, {1, 100, 1000}), std::length_error);
    EXPECT_THROW(BuildFarVolumeTile(request, sample, {8'000'000, 1, 128u * 1024u * 1024u}),
                 std::length_error);
    EXPECT_THROW(BuildFarVolumeTile(request, sample, {8'000'000, 65'536, 1000}), std::length_error);
    EXPECT_THROW(BuildFarVolumeTile(request,
                                    {[](float, float) { return 0.0f; },
                                     [](const Vec3&, float) {
                                         return FarVolumeDensity{
                                             std::numeric_limits<float>::quiet_NaN(), 1};
                                     }}),
                 std::invalid_argument);
}

TEST(FarVolume, CorruptionAndSharedFaceDisagreementFailClosed) {
    auto tile = BuildFarVolumeTile({3, -1, -1}, Plane(10.25f));
    auto corrupt = tile;
    corrupt.bricks.front().samples[0].density += 1;
    EXPECT_THROW(MarchingCubes::PolygoniseFarVolume(corrupt), std::invalid_argument);
    corrupt = tile;
    corrupt.sampled_bricks -= 1;
    corrupt.crc32 = FarVolumeTileCrc(corrupt);
    EXPECT_THROW(MarchingCubes::PolygoniseFarVolume(corrupt), std::invalid_argument);
    corrupt = tile;
    // First two bricks share a Z face. Recompute both checksums to prove the
    // mesher validates the shared lattice independently of CRC integrity.
    corrupt.bricks.front().samples[FarVolumeSampleIndex(0, 0, 4)].density += 1;
    corrupt.bricks.front().crc32 = FarVolumeBrickCrc(corrupt.bricks.front());
    corrupt.crc32 = FarVolumeTileCrc(corrupt);
    EXPECT_THROW(MarchingCubes::PolygoniseFarVolume(corrupt), std::invalid_argument);
    corrupt = tile;
    std::swap(corrupt.bricks[0], corrupt.bricks[1]);
    corrupt.crc32 = FarVolumeTileCrc(corrupt);
    EXPECT_THROW(MarchingCubes::PolygoniseFarVolume(corrupt), std::invalid_argument);
    corrupt = tile;
    corrupt.bricks[0].samples[0].density = kFarLodSdfInvalid;
    corrupt.bricks[0].crc32 = FarVolumeBrickCrc(corrupt.bricks[0]);
    corrupt.crc32 = FarVolumeTileCrc(corrupt);
    EXPECT_THROW(MarchingCubes::PolygoniseFarVolume(corrupt), std::invalid_argument);
    EXPECT_THROW(MarchingCubes::PolygoniseFarVolume(tile, {}, {1, 1, 64}), std::length_error);
}

TEST(FarVolume, HomogeneousPristineFieldsEmitNoBackgroundOrInventedHalo) {
    for (float density : {-1.0f, 1.0f}) {
        const FarVolumeSamplers field{[](float, float) { return 0.0f; },
                                      [density](const Vec3&, float) {
                                          return FarVolumeDensity{density, 1};
                                      }};
        const auto tile = BuildFarVolumeTile({5, -1, -1}, field);
        EXPECT_TRUE(tile.bricks.empty());
        const auto mesh = MarchingCubes::PolygoniseFarVolume(tile);
        EXPECT_TRUE(mesh.vertices.empty());
        EXPECT_TRUE(mesh.indices.empty());
        EXPECT_EQ(tile.crc32, BuildFarVolumeTile({5, -1, -1}, field).crc32);
    }
}

TEST(FarVolume, DisconnectedComponentsAndCavitiesHaveOutwardWinding) {
    constexpr float spacing = 16.0f;
    // Two interior lattice samples produce case 65 in cell (1,1,1),
    // surrounded by otherwise homogeneous samples. Negation produces case 190.
    // Unequal magnitudes make the cell-wide gradient favor one component.
    for (const auto amplitudes : {Vec3(2.0f, 1.0f, 0.0f),
                                  Vec3(1.0f, 2.0f, 0.0f),
                                  Vec3(4.0f, 0.5f, 0.0f),
                                  Vec3(0.5f, 4.0f, 0.0f)}) {
        for (const float sign : {1.0f, -1.0f}) {
            SCOPED_TRACE(::testing::Message() << "amplitudes=" << amplitudes.x << ","
                                              << amplitudes.y << " sign=" << sign);
            const FarVolumeSamplers field{[](float, float) { return 0.0f; },
                                          [amplitudes, sign](const Vec3& p, float) {
                                              const auto q = p / spacing;
                                              const float value = q == Vec3(1)   ? -amplitudes.x
                                                                  : q == Vec3(2) ? -amplitudes.y
                                                                                 : 1.0f;
                                              return FarVolumeDensity{sign * value, 1};
                                          }};
            const auto tile = BuildFarVolumeTile({3, 0, 0}, field);
            ASSERT_EQ(tile.bricks.size(), 1u);
            const auto mesh = MarchingCubes::PolygoniseFarVolume(tile);
            ASSERT_EQ(mesh.indices.size(), 48u); // Eight faces around each component.
            std::array<std::size_t, 2> component_faces{};
            // Independent continuous oracle: the lattice's trilinear interpolant
            // is a constant plus two tensor-product tents. No mesher gradient or
            // triangle table is used to decide which side is air.
            const auto density = [amplitudes, sign](const Vec3& p) {
                const auto tent = [](const Vec3& q) {
                    return std::max(0.0, 1.0 - std::abs(static_cast<double>(q.x))) *
                           std::max(0.0, 1.0 - std::abs(static_cast<double>(q.y))) *
                           std::max(0.0, 1.0 - std::abs(static_cast<double>(q.z)));
                };
                return sign * (1.0 - (1.0 + amplitudes.x) * tent(p - Vec3(1)) -
                               (1.0 + amplitudes.y) * tent(p - Vec3(2)));
            };
            for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
                const auto a = mesh.vertices[mesh.indices[i]].position / spacing;
                const auto b = mesh.vertices[mesh.indices[i + 1]].position / spacing;
                const auto c = mesh.vertices[mesh.indices[i + 2]].position / spacing;
                const auto center = (a + b + c) / 3.0f;
                const auto normal = glm::normalize(glm::cross(b - a, c - a));
                const auto first_distance = glm::length(center - Vec3(1));
                const auto second_distance = glm::length(center - Vec3(2));
                const std::size_t component = first_distance < second_distance ? 0 : 1;
                ++component_faces[component];
                EXPECT_GT(density(center + normal * 0.001f), density(center - normal * 0.001f))
                    << "triangle=" << i / 3 << " component=" << component;
                // The sign complement makes the same closed surface a cavity;
                // its outward (toward air) direction is toward the sample.
                EXPECT_GT(
                    glm::dot(normal, sign * (center - Vec3(static_cast<float>(component + 1)))),
                    0.0f);
            }
            EXPECT_EQ(component_faces[0], 8u);
            EXPECT_EQ(component_faces[1], 8u);
        }
    }
}

TEST(FarVolume, MaterialOnlySharedEdgeAndCornerCorruptionFailsClosed) {
    constexpr float spacing = 16.0f;
    for (const bool corner : {false, true}) {
        const Vec3 second(5, corner ? 5 : 1, 5);
        const FarVolumeSamplers field{[](float, float) { return 0.0f; },
                                      [second](const Vec3& p, float) {
                                          const auto q = p / spacing;
                                          return FarVolumeDensity{
                                              q == Vec3(1) || q == second ? -1.0f : 1.0f, 1};
                                      }};
        auto tile = BuildFarVolumeTile({3, 0, 0, FarVolumeSpan{0, 8 * spacing}}, field);
        ASSERT_EQ(tile.bricks.size(), 2u);
        ValidateFarVolumeTile(tile);
        // The only two retained bricks meet on an edge or a corner, never a face.
        tile.bricks[0].samples[FarVolumeSampleIndex(4, corner ? 4 : 2, 4)].material = 2;
        tile.bricks[0].crc32 = FarVolumeBrickCrc(tile.bricks[0]);
        tile.crc32 = FarVolumeTileCrc(tile);
        EXPECT_THROW(MarchingCubes::PolygoniseFarVolume(tile), std::invalid_argument);
    }
}

using VertexBits = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t>;
std::set<VertexBits> Border(const FarVolumeMesh& mesh, float x) {
    std::set<VertexBits> result;
    for (const auto& v : mesh.vertices)
        if (v.position.x == x)
            result.emplace(std::bit_cast<std::uint32_t>(v.position.x),
                           std::bit_cast<std::uint32_t>(v.position.y),
                           std::bit_cast<std::uint32_t>(v.position.z),
                           v.material);
    return result;
}
TEST(FarVolume, MeshHasExactAdjacentTileVerticesAndOutwardWinding) {
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        const auto field = FarVolumeSamplers{[](float, float z) { return 10.25f - z * 0.0125f; },
                                             [](const Vec3& p, float h) {
                                                 return FarVolumeDensity{p.y - h, 1};
                                             }};
        const auto left =
            MarchingCubes::PolygoniseFarVolume(BuildFarVolumeTile({tier, -1, -1}, field));
        const auto right =
            MarchingCubes::PolygoniseFarVolume(BuildFarVolumeTile({tier, 0, -1}, field));
        ASSERT_FALSE(left.indices.empty());
        ASSERT_FALSE(right.indices.empty());
        EXPECT_EQ(Border(left, 0), Border(right, 0));
        EXPECT_FALSE(Border(left, 0).empty());
        for (std::size_t i = 0; i < left.indices.size(); i += 3) {
            const auto& a = left.vertices[left.indices[i]].position;
            const auto& b = left.vertices[left.indices[i + 1]].position;
            const auto& c = left.vertices[left.indices[i + 2]].position;
            EXPECT_GT(glm::dot(glm::cross(b - a, c - a), Vec3(0, 1, 0.0125f)), 0.0f);
        }
    }
}

using PositionBits = std::array<std::uint32_t, 3>;
using SegmentBits = std::pair<PositionBits, PositionBits>;
std::multiset<SegmentBits> BoundarySegments(const FarVolumeMesh& mesh, int axis, float plane) {
    std::multiset<SegmentBits> result;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3)
        for (int j = 0; j < 3; ++j) {
            const auto a = mesh.vertices[mesh.indices[i + j]].position;
            const auto b = mesh.vertices[mesh.indices[i + (j + 1) % 3]].position;
            if (a[axis] != plane || b[axis] != plane)
                continue;
            PositionBits ab{std::bit_cast<std::uint32_t>(a.x),
                            std::bit_cast<std::uint32_t>(a.y),
                            std::bit_cast<std::uint32_t>(a.z)};
            PositionBits bb{std::bit_cast<std::uint32_t>(b.x),
                            std::bit_cast<std::uint32_t>(b.y),
                            std::bit_cast<std::uint32_t>(b.z)};
            if (bb < ab)
                std::swap(ab, bb);
            result.emplace(ab, bb);
        }
    return result;
}
TEST(FarVolume, AmbiguousFacesHaveExactAdjacentTileConnectivity) {
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        const auto dimensions = FarTierAt(tier);
        if (!dimensions.has_value()) {
            ADD_FAILURE() << "tier " << tier << " is absent from the table";
            continue;
        }
        const float spacing = static_cast<float>(dimensions->sample_spacing_meters);
        const auto field = FarVolumeSamplers{
            [](float, float) { return 0.25f; },
            [spacing](const Vec3& p, float h) {
                if (std::abs(p.x) <= 3 * spacing && std::abs(p.z) <= 3 * spacing &&
                    p.y >= -3 * spacing && p.y <= 0) {
                    const int x = static_cast<int>(std::round(p.x / spacing));
                    const int y = static_cast<int>(std::round(p.y / spacing));
                    const int z = static_cast<int>(std::round(p.z / spacing));
                    return FarVolumeDensity{((x + y + z) % 2 == 0 ? 1.25f : -0.75f) * spacing, 1};
                }
                return FarVolumeDensity{p.y - h, 1};
            }};
        const auto center =
            MarchingCubes::PolygoniseFarVolume(BuildFarVolumeTile({tier, 0, 0}, field));
        const auto west =
            MarchingCubes::PolygoniseFarVolume(BuildFarVolumeTile({tier, -1, 0}, field));
        const auto south =
            MarchingCubes::PolygoniseFarVolume(BuildFarVolumeTile({tier, 0, -1}, field));
        const auto xs = BoundarySegments(center, 0, 0);
        const auto zs = BoundarySegments(center, 2, 0);
        ASSERT_FALSE(xs.empty());
        ASSERT_FALSE(zs.empty());
        EXPECT_EQ(xs, BoundarySegments(west, 0, 0)) << "tier=" << tier;
        EXPECT_EQ(zs, BoundarySegments(south, 2, 0)) << "tier=" << tier;
    }
}

TerrainGenParams Caverns() {
    const auto preset = Luminumbra::world::LoadTerrainPreset(
        std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/caverns.json");
    if (!preset.ok)
        throw std::runtime_error("Could not load caverns preset");
    return preset.params;
}
TEST(FarVolume, FineTiersMatchLiveCaveBitsAndNestedSamples) {
    auto params = Caverns();
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    for (int z = -4; z <= 4; ++z)
        for (int x = -4; x <= 4; ++x)
            for (int y = 0; y < 8; ++y) {
                const Vec3 p(static_cast<float>(x * 8),
                             static_cast<float>(-y * 8),
                             static_cast<float>(z * 8));
                const float h = world.GetTerrainHeightAt(p.x, p.z);
                const auto expected =
                    std::bit_cast<std::uint32_t>(world.get_density_at_from_precalculated(p, h));
                for (auto mode : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
                    EXPECT_EQ(
                        std::bit_cast<std::uint32_t>(world.SamplePristineFarDensity(p, h, 4, mode)),
                        expected);
                    EXPECT_EQ(
                        std::bit_cast<std::uint32_t>(world.SamplePristineFarDensity(p, h, 8, mode)),
                        expected);
                }
            }
}

TEST(FarVolume, CoarseAnalyticOpeningSurvivesNoiseCutoffAndOldSignControlFails) {
    auto params = Caverns();
    params.carve_smoothness = 0.0f;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    const auto feature = world.FindLargestSurfaceBreak(-12288.0f, -4096.0f, 512.0f);
    ASSERT_TRUE(feature.found);
    EXPECT_GT(std::hypot(feature.x, feature.z), 8192.0f);
    const float h = world.GetTerrainHeightAt(feature.x, feature.z);
    const Vec3 inside(feature.x, h - feature.depth * 0.5f, feature.z);
    const float signed_opening = world.AnalyticSurfaceOpeningDensity(inside, h);
    ASSERT_GT(signed_opening, 0.0f);
    // Actual old composition with noise suppressed by its existing cap branch.
    const float original = world.EvaluateCaveDensity(inside, inside.y - h, 1.0e6f, signed_opening);
    EXPECT_LT(original, 0.0f);
    for (auto spacing : {32u, 64u}) {
        EXPECT_GT(world.SamplePristineFarDensity(inside, h, spacing, FarCaveMode::BandLimited),
                  0.0f);
        EXPECT_GT(world.SamplePristineFarDensity(inside, h, spacing, FarCaveMode::BoxFiltered),
                  0.0f);
        const Vec3 below(feature.x, h - feature.depth - feature.radius * 2, feature.z);
        EXPECT_LT(world.SamplePristineFarDensity(below, h, spacing, FarCaveMode::BandLimited),
                  0.0f);
    }
    RecordProperty("old_analytic_density", std::to_string(original));
    RecordProperty("signed_analytic_density", std::to_string(signed_opening));
}
} // namespace

namespace {
TEST(FarVolume, SubspacingCavityVisibilityDependsOnSamplingPhase) {
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        const auto dimensions = FarTierAt(tier);
        if (!dimensions.has_value()) {
            ADD_FAILURE() << "tier " << tier << " is absent from the table";
            continue;
        }
        const float s = static_cast<float>(dimensions->sample_spacing_meters);
        FarVolumeRequest request{tier, 0, 0, FarVolumeSpan{-12 * s, 8 * s}};
        std::array<std::size_t, 2> counts{};
        for (int phase = 0; phase < 2; ++phase) {
            const Vec3 center =
                Vec3(8 * s, -8 * s, 8 * s) + Vec3(static_cast<float>(phase) * 0.5f * s);
            const auto field = FarVolumeSamplers{
                [](float, float) { return 0.0f; },
                [center, s](const Vec3& p, float) {
                    return FarVolumeDensity{std::max(p.y, 0.35f * s - glm::length(p - center)), 1};
                }};
            const auto tile = BuildFarVolumeTile(request, field);
            counts[phase] = tile.bricks.size();
            ValidateFarVolumeTile(tile);
        }
        EXPECT_GT(counts[0], 1024u) << "tier=" << tier;
        EXPECT_EQ(counts[1], 1024u) << "tier=" << tier;
    }
}

TEST(FarVolume, AnalyticalArchProducesWallsAndUndersideWithoutHeightfieldBackground) {
    constexpr float s = 16.0f;
    const Vec3 center(256, 0, 256);
    const auto field =
        FarVolumeSamplers{[](float, float) { return 0.0f; },
                          [center](const Vec3& p, float) {
                              const Vec3 q = p - center;
                              const float ring = std::sqrt(q.x * q.x + q.y * q.y) - 4 * s;
                              const float torus = std::sqrt(ring * ring + q.z * q.z) - 1.5f * s;
                              return FarVolumeDensity{std::min(p.y, torus), 1};
                          }};
    const auto tile = BuildFarVolumeTile({3, 0, 0, FarVolumeSpan{-256, 8 * s}}, field);
    const auto mesh = MarchingCubes::PolygoniseFarVolume(tile);
    std::size_t underside = 0, walls = 0, upper = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto a = mesh.vertices[mesh.indices[i]].position;
        const auto b = mesh.vertices[mesh.indices[i + 1]].position;
        const auto c = mesh.vertices[mesh.indices[i + 2]].position;
        const auto normal = glm::normalize(glm::cross(b - a, c - a));
        if (std::min({a.y, b.y, c.y}) > 0) {
            underside += normal.y < -0.25f;
            walls += std::abs(normal.y) < 0.5f;
            upper += normal.y > 0.25f;
        }
    }
    EXPECT_GT(underside, 0u);
    EXPECT_GT(walls, 0u);
    EXPECT_GT(upper, 0u);
}

TEST(FarVolume, BoxKernelHasAnalyticalTransferAndExposesResidualPhaseAlias) {
    constexpr double pi = 3.14159265358979323846;
    for (auto spacing : {16u, 32u, 64u})
        for (double cycles : {0.0, 0.25, 0.5, 1.0, 2.0}) {
            for (int phase = 0; phase < 8; ++phase) {
                const double phi = 2 * pi * phase / 8.0;
                const auto value = BoxFilterFarVolumeCarve(Vec3(0), spacing, [=](const Vec3& p) {
                    return static_cast<float>(2.0 +
                                              std::cos(2 * pi * cycles * p.x / spacing + phi));
                });
                const double expected = 2.0 + std::cos(phi) * std::cos(pi * cycles * 0.5);
                EXPECT_NEAR(value, expected, 3.0e-7)
                    << "spacing=" << spacing << " cycles=" << cycles << " phase=" << phase;
            }
        }
    // The spacing-frequency mode is cancelled; twice that frequency aliases at
    // full amplitude. Reporting the latter prevents calling this ideal filtering.
    EXPECT_NEAR(
        BoxFilterFarVolumeCarve(
            Vec3(0),
            16,
            [](const Vec3& p) { return static_cast<float>(2.0 + std::cos(2 * pi * p.x / 16.0)); }),
        2.0f,
        1.0e-6f);
    EXPECT_NEAR(
        BoxFilterFarVolumeCarve(
            Vec3(0),
            16,
            [](const Vec3& p) { return static_cast<float>(2.0 + std::cos(4 * pi * p.x / 16.0)); }),
        1.0f,
        1.0e-6f);
    EXPECT_THROW(BoxFilterFarVolumeCarve(Vec3(0), 8, [](const Vec3&) { return 1.0f; }),
                 std::invalid_argument);
}

TEST(FarVolume, CavernsAlternativesHaveMeasuredPhaseAndCostControls) {
    const auto params = Caverns();
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    constexpr int count = 512;
    struct Probe {
        Vec3 p, shifted;
        float height, shifted_height, live;
    };
    for (auto spacing : {16u, 32u, 64u}) {
        std::array<Probe, count> probes;
        for (int i = 0; i < count; ++i) {
            const float x = static_cast<float>((i % 16 - 8) * static_cast<int>(spacing));
            const float z = static_cast<float>((i / 16 % 8 - 4) * static_cast<int>(spacing));
            const float h = world.GetTerrainHeightAt(x, z);
            const Vec3 p(x, h - 32.0f - static_cast<float>(i / 128) * 24.0f, z);
            const Vec3 offset =
                p + Vec3(static_cast<float>(spacing) * 0.5f, 0, static_cast<float>(spacing) * 0.5f);
            probes[i] = {p,
                         offset,
                         h,
                         world.GetTerrainHeightAt(offset.x, offset.z),
                         world.get_density_at_from_precalculated(p, h)};
        }
        for (auto mode : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
            std::size_t different_from_live = 0, air = 0, phase_changes = 0;
            double squared_error = 0;
            std::array<float, count> values, shifted;
            // Equal warm-up for the tap-height cache; time only the chosen sampler.
            for (const auto& probe : probes) {
                world.SamplePristineFarDensity(probe.p, probe.height, spacing, mode);
                world.SamplePristineFarDensity(probe.shifted, probe.shifted_height, spacing, mode);
            }
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < count; ++i) {
                values[i] =
                    world.SamplePristineFarDensity(probes[i].p, probes[i].height, spacing, mode);
                shifted[i] = world.SamplePristineFarDensity(
                    probes[i].shifted, probes[i].shifted_height, spacing, mode);
            }
            const auto ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            for (int i = 0; i < count; ++i) {
                different_from_live += (values[i] >= 0) != (probes[i].live >= 0);
                air += values[i] >= 0;
                phase_changes += (values[i] >= 0) != (shifted[i] >= 0);
                squared_error +=
                    static_cast<double>(values[i] - probes[i].live) * (values[i] - probes[i].live);
                EXPECT_TRUE(std::isfinite(values[i]));
            }
            const std::string key =
                std::to_string(spacing) + (mode == FarCaveMode::BandLimited ? "_band" : "_box");
            RecordProperty(key + "_milliseconds", std::to_string(ms));
            RecordProperty(key + "_timed_samples", count * 2);
            RecordProperty(key + "_phase_pairs", count);
            RecordProperty(key + "_air", static_cast<int>(air));
            RecordProperty(key + "_classification_changes", static_cast<int>(different_from_live));
            RecordProperty(key + "_phase_changes", static_cast<int>(phase_changes));
            RecordProperty(key + "_rms_density_error",
                           std::to_string(std::sqrt(squared_error / count)));
        }
    }
}

TEST(FarVolume, PristineSamplerRejectsInvalidInputsAndHonorsDisabledCaves) {
    auto params = Caverns();
    params.caves_enabled = false;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    for (auto spacing : {4u, 8u, 16u, 32u, 64u})
        for (auto mode : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
            EXPECT_EQ(world.SamplePristineFarDensity(Vec3(-32, -50, -64), 10, spacing, mode),
                      -60.0f);
            EXPECT_THROW(world.SamplePristineFarDensity(
                             Vec3(std::numeric_limits<float>::max()), 0, spacing, mode),
                         std::invalid_argument);
        }
    EXPECT_THROW(world.SamplePristineFarDensity(Vec3(0), 0, 12, FarCaveMode::BandLimited),
                 std::invalid_argument);
    EXPECT_THROW(world.SamplePristineFarDensity(Vec3(0), 0, 16, static_cast<FarCaveMode>(255)),
                 std::invalid_argument);
}
// Full pristine tile construction (not a window proxy). Run in isolation for
// cost evidence; elapsed time is measured, never a portable timing assertion.
TEST(FarVolume, CavernsPristineTilesRecordAlternativeCostAndSize) {
    const auto params = Caverns();
    for (auto tier : {3u, 4u, 5u})
        for (auto mode : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
            SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
            FarVolumeRequest request{tier, -1, -1, std::nullopt, mode};
            const auto start = std::chrono::steady_clock::now();
            const auto tile = BuildPristineFarVolumeTile(world, request);
            const auto generated = std::chrono::steady_clock::now();
            const auto mesh = MarchingCubes::PolygoniseFarVolume(tile);
            const auto meshed = std::chrono::steady_clock::now();
            ASSERT_FALSE(tile.bricks.empty());
            ASSERT_FALSE(mesh.indices.empty());
            const std::string key =
                "v" + std::to_string(tier) + (mode == FarCaveMode::BandLimited ? "_band" : "_box");
            RecordProperty(
                key + "_generation_ms",
                std::to_string(
                    std::chrono::duration<double, std::milli>(generated - start).count()));
            RecordProperty(
                key + "_meshing_ms",
                std::to_string(
                    std::chrono::duration<double, std::milli>(meshed - generated).count()));
            RecordProperty(key + "_candidate_bricks", std::to_string(tile.sampled_bricks));
            RecordProperty(key + "_retained_bricks", std::to_string(tile.bricks.size()));
            RecordProperty(key + "_vertices", std::to_string(mesh.vertices.size()));
            RecordProperty(key + "_indices", std::to_string(mesh.indices.size()));
            RecordProperty(key + "_crc", std::to_string(tile.crc32));
        }
}
} // namespace
