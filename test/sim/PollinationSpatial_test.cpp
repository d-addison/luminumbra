#include "../support/PollinationOracle.h"

#include <gtest/gtest.h>

#include "ecs/EntitySnapshot.h"
#include "persistence/PlantPersistence.h"
#include "simulation/DeterministicUniformGrid.h"
#include "systems/PollinationSystem.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace F = luminumbra::foliage;
namespace C = Luminumbra::Components;
using Luminumbra::TestSupport::PollinationOracle::BrutePollination;
using Grid = luminumbra::simulation::DeterministicUniformGrid;
using Gate = Luminumbra::world::RegionConsumerGate;

std::vector<std::size_t>
BruteQuery(const std::vector<Grid::Point>& points, Grid::Point minimum, Grid::Point maximum) {
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto& point = points[index];
        if (point.x >= minimum.x && point.x <= maximum.x && point.z >= minimum.z &&
            point.z <= maximum.z) {
            result.push_back(index);
        }
    }
    return result;
}

void ExpectQuery(Grid& grid,
                 const std::vector<Grid::Point>& points,
                 Grid::Point minimum,
                 Grid::Point maximum) {
    const auto result = grid.Query(minimum, maximum);
    EXPECT_EQ(std::vector<std::size_t>(result.begin(), result.end()),
              BruteQuery(points, minimum, maximum));
}

TEST(DeterministicUniformGrid, RandomizedSetAndOriginalOrderMatchBruteForce) {
    Grid grid;
    auto rng = luminumbra::core::DeterministicRng::seeded(71, 23, 19);
    for (const auto width : {1, 7, 16, 101}) {
        for (int fixture = 0; fixture < 12; ++fixture) {
            std::vector<Grid::Point> points;
            for (int index = 0; index < 257; ++index) {
                const auto x = static_cast<std::int64_t>(rng.next_u64() % 4097) - 2048;
                const auto z = static_cast<std::int64_t>(rng.next_u64() % 4097) - 2048;
                points.push_back(fixture % 2 == 0 ? Grid::Point{x, z} : Grid::Point{x % 3, z % 3});
            }
            grid.Build(points, width);
            for (int query = 0; query < 80; ++query) {
                const auto x = static_cast<std::int64_t>(rng.next_u64() % 4097) - 2048;
                const auto z = static_cast<std::int64_t>(rng.next_u64() % 4097) - 2048;
                const auto extent = static_cast<std::int64_t>(rng.next_u64() % 200);
                ExpectQuery(grid, points, {x - extent, z - extent}, {x + extent, z + extent});
            }
            ExpectQuery(grid, points, {-3, -3}, {3, 3});
        }
    }
}

TEST(DeterministicUniformGrid, BoundariesExtremesDuplicatesAndRebuilds) {
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    const std::vector<Grid::Point> points{
        {16, -16}, {-1, 0}, {0, 0}, {-16, 16}, {-17, -17}, {0, 0}, {low, low}, {high, high}};
    Grid grid;
    EXPECT_TRUE(grid.Query({low, low}, {high, high}).empty());
    for (const auto width : {std::int64_t{1}, std::int64_t{16}, high}) {
        grid.Build(points, width);
        for (const auto& point : points) {
            ExpectQuery(grid, points, point, point);
        }
        ExpectQuery(grid, points, {-16, -16}, {16, 16});
        ExpectQuery(grid, points, {low, low}, {high, high});
        ExpectQuery(grid, points, {1, 0}, {-1, 0});
        ExpectQuery(grid, points, {0, 1}, {0, -1});
        EXPECT_THROW(grid.Build(points, 0), std::invalid_argument);
        EXPECT_THROW(grid.Build(points, -1), std::invalid_argument);
        ExpectQuery(grid, points, {low, low}, {high, high});
    }
    grid.Build({}, 16);
    EXPECT_EQ(grid.CandidateTests(), 0u);
    EXPECT_TRUE(grid.Query({low, low}, {high, high}).empty());
    grid.Build(points, 16);
    ExpectQuery(grid, points, {0, 0}, {0, 0});
}

TEST(DeterministicUniformGrid, SparseCandidateBudgetCountsActualSourceTests) {
    std::vector<Grid::Point> points;
    for (std::int64_t index = 0; index < 1024; ++index) {
        points.push_back({(index % 32) * 128 - 2048, (index / 32) * 128 - 2048});
    }
    Grid grid;
    grid.Build(points, 16);
    for (const auto& point : points) {
        ExpectQuery(grid, points, {point.x - 13, point.z - 13}, {point.x + 13, point.z + 13});
    }
    EXPECT_GE(grid.CandidateTests(), points.size());
    EXPECT_LE(grid.CandidateTests(), 8 * points.size());
}

void AddPlant(entt::registry& registry, float x, float z, std::uint64_t index) {
    const auto entity = registry.create();
    registry.emplace<C::TransformComponent>(entity).position = {x, 0.0f, z};
    auto& genome = registry.emplace<C::PlantGenomeComponent>(entity);
    genome.genes.fill(static_cast<float>(index % 17) / 16.0f);
    registry.emplace<C::PlantGrowthComponent>(entity).stage = static_cast<std::uint8_t>(
        index % 3 == 0 ? C::PlantStage::Juvenile
                       : (index % 3 == 1 ? C::PlantStage::Flowering : C::PlantStage::Fruiting));
    registry.emplace<C::PollinationComponent>(entity).crosses =
        index % 7 == 0 ? std::numeric_limits<std::uint32_t>::max() : 0u;
    registry.emplace<C::PollinationTag>(entity);
    registry.emplace<C::PlantTag>(entity);
    registry.emplace<C::SimulationIdentityComponent>(entity,
                                                     (std::uint64_t{1} << 40) + 10000 - index);
}

std::string PlantBytes(const entt::registry& registry) {
    return Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
        F::BuildPlantEntitySnapshot(registry));
}

void ExpectPollinationParity(entt::registry& indexed,
                             entt::registry& reference,
                             const Luminumbra::Vec2& wind,
                             const Gate& gate) {
    for (const auto tick : {1u, 2u, 99u}) {
        const auto expected = BrutePollination(reference, tick, wind, gate);
        const auto actual = F::RunPollinationOnTick(indexed, tick, wind, 99, 0.05f, gate);
        EXPECT_EQ(actual.considered, expected.considered);
        EXPECT_EQ(actual.donors, expected.donors);
        EXPECT_EQ(actual.crossed, expected.crossed);
        for (const auto entity : reference.view<C::PollinationComponent>()) {
            const auto& lhs = indexed.get<C::PollinationComponent>(entity);
            const auto& rhs = reference.get<C::PollinationComponent>(entity);
            EXPECT_EQ(lhs.pollinated, rhs.pollinated);
            EXPECT_EQ(lhs.last_pollen_tick, rhs.last_pollen_tick);
            EXPECT_EQ(lhs.crosses, rhs.crosses);
            for (std::size_t gene = 0; gene < lhs.next_genome.genes.size(); ++gene) {
                EXPECT_EQ(std::bit_cast<std::uint32_t>(lhs.next_genome.genes[gene]),
                          std::bit_cast<std::uint32_t>(rhs.next_genome.genes[gene]));
            }
        }
        const auto actual_bytes = PlantBytes(indexed);
        const auto expected_bytes = PlantBytes(reference);
        EXPECT_EQ(actual_bytes, expected_bytes);
    }
}

TEST(PollinationSpatial, DenseRandomizedWindBoundariesAndIdentityOrderMatchBruteForce) {
    constexpr std::array winds{Luminumbra::Vec2{0.0f, 0.0f},
                               Luminumbra::Vec2{1.0f, 0.0f},
                               Luminumbra::Vec2{-3.0f, 4.0f},
                               Luminumbra::Vec2{1.0f, 1.0f},
                               Luminumbra::Vec2{1.0e-22f, 2.0e-23f}};
    for (const auto& wind : winds) {
        for (const bool simulation_identity : {false, true}) {
            entt::registry indexed;
            entt::registry reference;
            auto rng = luminumbra::core::DeterministicRng::seeded(71, 11, 17);
            for (std::uint64_t index = 0; index < 160; ++index) {
                const float x =
                    static_cast<float>(static_cast<int>(rng.next_u64() % 257) - 128) / 8;
                const float z =
                    static_cast<float>(static_cast<int>(rng.next_u64() % 257) - 128) / 8;
                AddPlant(indexed, x, z, index);
                AddPlant(reference, x, z, index);
            }
            const std::array boundaries{0.0f,
                                        -0.0f,
                                        -6.0f,
                                        6.0f,
                                        -12.0f,
                                        12.0f,
                                        std::nextafter(12.0f, 0.0f),
                                        std::nextafter(12.0f, 13.0f),
                                        std::nextafter(-6.0f, -7.0f)};
            for (std::size_t index = 0; index < boundaries.size(); ++index) {
                AddPlant(indexed, boundaries[index], 0, index + 160);
                AddPlant(reference, boundaries[index], 0, index + 160);
            }
            const auto gate = simulation_identity ? Gate(Gate::PlantIdentity::Simulation) : Gate{};
            ExpectPollinationParity(indexed, reference, wind, gate);
        }
    }
}

TEST(PollinationSpatial, ExceptionalFloatsKeepOriginalScanResults) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    for (const auto x : {nan, infinity, -infinity, 1.0e30f, -1.0e30f}) {
        for (const auto& wind : {Luminumbra::Vec2{0.0f},
                                 Luminumbra::Vec2{infinity, 0.0f},
                                 Luminumbra::Vec2{nan, 1.0f}}) {
            for (const bool z_axis : {false, true}) {
                entt::registry indexed;
                entt::registry reference;
                for (std::uint64_t index = 0; index < 4; ++index) {
                    const float coordinate = index == 1 ? x : static_cast<float>(index);
                    AddPlant(indexed, z_axis ? 0 : coordinate, z_axis ? coordinate : 0, index);
                    AddPlant(reference, z_axis ? 0 : coordinate, z_axis ? coordinate : 0, index);
                }
                ExpectPollinationParity(indexed, reference, wind, {});
            }
        }
    }
}

TEST(PollinationSpatial, IsolatedWindExpandedBoundariesMatchBruteForce) {
    for (const bool windy : {false, true}) {
        const float radius = windy ? 12.0f : 6.0f;
        for (const float sign : {-1.0f, 1.0f}) {
            const Luminumbra::Vec2 wind{windy ? sign : 0.0f, 0.0f};
            for (const float distance :
                 {std::nextafter(radius, 0.0f), radius, std::nextafter(radius, radius + 1.0f)}) {
                entt::registry indexed;
                entt::registry reference;
                AddPlant(indexed, sign * distance, 0, 0);
                AddPlant(reference, sign * distance, 0, 0);
                AddPlant(indexed, 0, 0, 1);
                AddPlant(reference, 0, 0, 1);
                const auto stats = F::RunPollinationOnTick(indexed, 1, wind, 99);
                EXPECT_EQ(stats.crossed, distance <= radius ? 1 : 0);
                ExpectPollinationParity(indexed, reference, wind, {});
            }
        }
    }
}

TEST(PollinationSpatial, Sparse1024PlantsReduceQuadraticCandidateTests) {
    entt::registry indexed;
    entt::registry reference;
    constexpr std::uint64_t count = 1024;
    for (std::uint64_t index = 0; index < count; ++index) {
        const float x = static_cast<float>(index % 32) * 128 - 2048;
        const float z = static_cast<float>(index / 32) * 128 - 2048;
        AddPlant(indexed, x, z, index);
        AddPlant(reference, x, z, index);
        indexed.get<C::PlantGrowthComponent>(static_cast<entt::entity>(index)).stage =
            static_cast<std::uint8_t>(C::PlantStage::Flowering);
        reference.get<C::PlantGrowthComponent>(static_cast<entt::entity>(index)).stage =
            static_cast<std::uint8_t>(C::PlantStage::Flowering);
    }
    const auto expected = BrutePollination(reference, 1, {1.0f, 0.0f}, {});
    const auto actual = F::RunPollinationOnTick(indexed, 1, {1.0f, 0.0f}, 99);
    EXPECT_EQ(actual.considered, count);
    EXPECT_EQ(actual.donors, count);
    EXPECT_EQ(actual.crossed, expected.crossed);
    EXPECT_EQ(PlantBytes(indexed), PlantBytes(reference));
    EXPECT_EQ(expected.candidate_tests, count * count);
    EXPECT_GT(expected.candidate_tests, 8 * count);
    EXPECT_GE(actual.candidate_tests, count);
    EXPECT_LE(actual.candidate_tests, 8 * count);
}
} // namespace
