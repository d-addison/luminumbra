#include "../support/GameSessionTestWorld.h"

#include "luminumbra_common/components/IrrigationComponents.h"
#include "luminumbra_common/components/SoilComponents.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/systems/PlantTerrainCache.h"
#include "luminumbra_common/systems/WaterSystem.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace {
namespace F = luminumbra::foliage;
namespace C = Luminumbra::Components;
using Luminumbra::MaterialType;
using Luminumbra::TestSupport::ClockTestPlant;
using Luminumbra::TestSupport::HeadlessRoot;
using Luminumbra::TestSupport::SessionWorldHash;
using Luminumbra::world::GameSession;

struct TerrainOracle {
    std::uint64_t generation = 1, authority = 0, water = 0;
    mutable std::uint64_t heights = 0, materials = 0;
    float height = 120.0f;
    MaterialType material = MaterialType::Grass;

    std::uint64_t water_seed_terrain_generation() const {
        return generation;
    }
    std::uint64_t terrain_edit_revision() const {
        return authority;
    }
    std::uint64_t water_epoch() const {
        return water;
    }
    float GetTerrainHeightAt(float, float) const {
        ++heights;
        return height;
    }
    MaterialType SurfaceVertexMaterial(float, float, float) const {
        ++materials;
        return material;
    }
};

template<typename World>
void ExpectUncachedSample(const F::PlantTerrainCache::Sample& actual,
                          const World& world,
                          float x,
                          float z) {
    const float height = world.GetTerrainHeightAt(x, z);
    const auto material = world.SurfaceVertexMaterial(x, z, height);
    const float altitude = height - static_cast<float>(Luminumbra::SEA_LEVEL);
    const float temperature = F::clamp01(0.60f - (altitude > 0.0f ? altitude : 0.0f) * 0.00045f);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.height), std::bit_cast<std::uint32_t>(height));
    EXPECT_EQ(actual.material, material);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.temperature),
              std::bit_cast<std::uint32_t>(temperature));
}

TEST(PlantTerrainCache, StationaryRosterQueriesOnceAndDropsDepartedSlots) {
    TerrainOracle world;
    F::PlantTerrainCache cache;
    constexpr std::size_t count = 1024;
    for (int tick = 0; tick < 5; ++tick) {
        cache.BeginTick(&world);
        for (std::size_t index = 0; index < count; ++index) {
            const auto sample = cache.At(&world, static_cast<float>(index), -1.0f);
            EXPECT_EQ(sample.material, MaterialType::Grass);
        }
        cache.EndTick();
        EXPECT_EQ(cache.misses(), count);
        EXPECT_EQ(world.heights, count);
        EXPECT_EQ(world.materials, count);
        EXPECT_EQ(cache.size(), count);
    }
    cache.BeginTick(&world);
    cache.EndTick();
    EXPECT_EQ(cache.size(), 0u);
    cache.BeginTick(&world);
    (void)cache.At(&world, 0, -1);
    cache.EndTick();
    EXPECT_EQ(cache.misses(), count + 1);
}

TEST(PlantTerrainCache, ExactCoordinateBitsAndEveryRevisionInvalidate) {
    TerrainOracle world;
    F::PlantTerrainCache cache;
    const auto query = [&](float x, float z) {
        cache.BeginTick(&world);
        const auto sample = cache.At(&world, x, z);
        cache.EndTick();
        ExpectUncachedSample(sample, world, x, z);
    };
    query(0.0f, 0.0f);
    query(-0.0f, 0.0f);
    query(-0.0f, -0.0f);
    query(std::bit_cast<float>(std::uint32_t{1}), -0.0f);
    EXPECT_EQ(cache.misses(), 4u);
    query(std::bit_cast<float>(std::uint32_t{1}), -0.0f);
    EXPECT_EQ(cache.misses(), 4u);
    for (auto* revision : {&world.generation, &world.authority, &world.water}) {
        ++*revision;
        world.height += 17.0f;
        query(std::bit_cast<float>(std::uint32_t{1}), -0.0f);
    }
    EXPECT_EQ(cache.misses(), 7u);
    for (auto* revision : {&world.generation, &world.authority, &world.water}) {
        *revision = std::numeric_limits<std::uint64_t>::max();
        query(2, 3);
        query(2, 3);
        *revision = 0;
    }
    EXPECT_EQ(cache.misses(), 13u);
    TerrainOracle replacement;
    cache.BeginTick(&replacement);
    (void)cache.At(&replacement, 2, 3);
    cache.EndTick();
    EXPECT_EQ(cache.misses(), 14u);
    cache.BeginTick<TerrainOracle>(nullptr);
    const auto empty = cache.At<TerrainOracle>(nullptr, 2, 3);
    cache.EndTick();
    EXPECT_EQ(empty.soil, 0.5f);
    EXPECT_EQ(empty.temperature, 0.55f);
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.misses(), 14u);
}

TEST(PlantTerrainCache, MaterialAndAltitudeOutputsMatchOriginalExpressionsBitForBit) {
    TerrainOracle world;
    F::PlantTerrainCache cache;
    constexpr std::array materials{MaterialType::Grass,
                                   MaterialType::Soil,
                                   MaterialType::Sand,
                                   MaterialType::Stone,
                                   MaterialType::Air,
                                   MaterialType::Water,
                                   MaterialType::Deepslate,
                                   MaterialType::LuminCrystal};
    constexpr std::array soils{0.95f, 0.85f, 0.45f, 0.30f, 0.25f, 0.25f, 0.25f, 0.25f};
    for (std::size_t index = 0; index < materials.size(); ++index) {
        for (float height : {-100.0f, -0.0f, 0.0f, 1.0f, 1400.0f}) {
            world.material = materials[index];
            world.height = height;
            ++world.authority;
            for (int tick = 0; tick < 2; ++tick) {
                cache.BeginTick(&world);
                const auto sample = cache.At(&world, -17.25f, 65.5f);
                cache.EndTick();
                ExpectUncachedSample(sample, world, -17.25f, 65.5f);
                EXPECT_EQ(std::bit_cast<std::uint32_t>(sample.soil),
                          std::bit_cast<std::uint32_t>(soils[index]));
            }
        }
    }
}

class PlantTerrainSession : public testing::TestWithParam<bool> {};

TEST_P(PlantTerrainSession, SettledWorkAndDynamicWorldHashesMatchForcedUncachedTicks) {
    const HeadlessRoot root;
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    GameSession cached, uncached;
    constexpr std::size_t count = 8;
    for (auto* session : {&cached, &uncached}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(GetParam());
        ASSERT_TRUE(session->CreateTransientWorld("Terrain cache", "1337", "default"));
        for (std::size_t index = 0; index < count; ++index) {
            auto position = session->GetMetadata().spawnPoint;
            position.x += static_cast<float>(index);
            const auto plant = ClockTestPlant(session->GetRegistry(), position);
            session->GetRegistry().emplace<C::WaterSourceComponent>(plant);
            session->GetRegistry().emplace<C::SoilFeederComponent>(plant);
        }
    }
    for (int tick = 0; tick < 8; ++tick) {
        // The notification changes only derived authority validity, forcing real queries.
        uncached.GetWorldSystem()->notify_far_lod_authority_changed({4096, 0, 4096});
        ASSERT_EQ(cached.TickSimulation(cached.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_EQ(uncached.TickSimulation(uncached.GetSimulationClock().fixed_dt()), 1u);
        EXPECT_EQ(cached.GetPlantTerrainQueryMisses(), count);
        EXPECT_EQ(uncached.GetPlantTerrainQueryMisses(),
                  count * static_cast<std::size_t>(tick + 1));
        EXPECT_EQ(SessionWorldHash(cached), SessionWorldHash(uncached));
    }
}

TEST_P(PlantTerrainSession, VoxelEditAndWorldgenChangesInvalidateWithExactTerrainParity) {
    const HeadlessRoot root;
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(GetParam());
    ASSERT_TRUE(session.CreateTransientWorld("Terrain edits", "1337", "default"));
    const auto position = session.GetMetadata().spawnPoint;
    ClockTestPlant(session.GetRegistry(), position);
    auto* world = session.GetWorldSystem();
    F::PlantTerrainCache cache;
    const auto sample = [&] {
        cache.BeginTick(world);
        const auto result = cache.At(world, position.x, position.z);
        cache.EndTick();
        ExpectUncachedSample(result, *world, position.x, position.z);
    };
    sample();
    sample();
    EXPECT_EQ(cache.misses(), 1u);
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(session.GetPlantTerrainQueryMisses(), 1u);
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(320, 0, -320));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->sdf_data.assign(std::size_t{17} * 17 * 17, 1.0f);
    ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
    const auto revision = world->far_lod_authority_revision();
    ASSERT_GT(world->EditTerrainVoxel({5128, 8, -5112}, 1, true, nullptr), 0);
    EXPECT_NE(world->far_lod_authority_revision(), revision);
    sample();
    EXPECT_EQ(cache.misses(), 2u);
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(session.GetPlantTerrainQueryMisses(), 2u);
    const auto unchanged = world->far_lod_authority_revision();
    EXPECT_EQ(world->EditTerrainVoxel({5128, 8, -5112}, 0, true, nullptr), 0);
    EXPECT_EQ(world->far_lod_authority_revision(), unchanged);
    sample();
    EXPECT_EQ(cache.misses(), 2u);
    auto water_chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(640, 0, -640));
    water_chunk->set_state(Luminumbra::ChunkState::Idle);
    water_chunk->current_water_resolution.store(8);
    water_chunk->water_depth_mm.assign(64, 77);
    water_chunk->water_bed_mm.assign(64, 0);
    water_chunk->water_edge_flux.assign(128, 0);
    water_chunk->water_level_data.assign(64, 0.077f);
    water_chunk->water_flow_data.assign(64, Luminumbra::Vec2(0));
    water_chunk->water_sim_terrain_height.assign(64, 0);
    water_chunk->water_rest_level.assign(64, 0.077f);
    water_chunk->has_water_sim.store(true);
    ASSERT_TRUE(water_chunk->has_complete_water_state());
    ASSERT_TRUE(world->adopt_streamed_chunk(water_chunk));
    if (GetParam()) {
        const auto terrain_revision = world->terrain_edit_revision();
        const auto authority_revision = world->far_lod_authority_revision();
        for (int tick = 0; tick < 2; ++tick) {
            water_chunk->mark_water_state_dirty();
            world->sync_water_authority();
            EXPECT_EQ(world->terrain_edit_revision(), terrain_revision);
            sample();
            EXPECT_EQ(cache.misses(), 2u);
        }
        EXPECT_NE(world->far_lod_authority_revision(), authority_revision);
    }
    const std::unordered_map<Luminumbra::ChunkID, std::shared_ptr<Luminumbra::Chunk>> owners{
        {water_chunk->get_id(), water_chunk}};
    const auto epoch = world->water_epoch();
    ASSERT_GT(session.GetWaterSystem()->EditTerrainBed({10248, 8, -10232}, 100, 2, &owners), 0);
    EXPECT_NE(world->water_epoch(), epoch);
    sample();
    EXPECT_EQ(cache.misses(), 3u);
    EXPECT_EQ(session.GetWaterSystem()->EditTerrainBed({10248, 8, -10232}, 0, 2, &owners), 0);
    sample();
    EXPECT_EQ(cache.misses(), 3u);
    world->set_seed(world->get_seed() + 1);
    sample();
    EXPECT_EQ(cache.misses(), 4u);
    auto params = world->get_params();
    params.height_offset += 7.0f;
    world->set_params(params);
    sample();
    EXPECT_EQ(cache.misses(), 5u);
}

INSTANTIATE_TEST_SUITE_P(RegionModes, PlantTerrainSession, testing::Bool());
} // namespace
