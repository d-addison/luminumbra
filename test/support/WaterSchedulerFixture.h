#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"

namespace Luminumbra::TestSupport {

// At most 65 isolated, initialized, awake chunks. Each contains a nonuniform
// finite pond above sea level, so selecting it changes actual integer grids.
inline std::shared_ptr<Chunk> SchedulerChunk(std::size_t index, int resolution) {
    auto chunk = std::make_shared<Chunk>(IVec3(static_cast<int>(index) * 2, 0, 0));
    const auto cells = static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
    constexpr std::size_t lattice =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    chunk->sdf_data.assign(lattice, -1.0f);
    chunk->heightmap_data.assign(std::size_t{17} * 17, 100.0f);
    chunk->water_depth_mm.assign(cells, 1000);
    chunk->water_depth_mm[cells / 2 + static_cast<std::size_t>(resolution) / 2] = 3000;
    chunk->water_bed_mm.assign(cells, 100000);
    chunk->water_edge_flux.assign(cells * 2, 0);
    chunk->water_level_data.assign(cells, 101.0f);
    chunk->water_sim_terrain_height.assign(cells, 100.0f);
    chunk->water_rest_level.assign(cells, 100.0f);
    chunk->water_flow_data.assign(cells, Vec2(0.0f));
    chunk->has_water_sim.store(true);
    chunk->current_water_resolution.store(resolution);
    chunk->is_water_sleeping.store(false);
    chunk->mark_sdf_loaded_or_edited();
    chunk->mark_water_data_dirty();
    chunk->set_state(ChunkState::Idle);
    return chunk;
}

using SchedulerChunks = std::unordered_map<ChunkID, std::shared_ptr<Chunk>>;

inline SchedulerChunks
InstallSchedulerChunks(world::GameSession& session, int resolution, std::size_t count) {
    auto* world = session.GetWorldSystem();
    world->clear_world(session.GetPhysicsSystem());
    world->SetWaterSimWindowCursor(0);
    session.GetWaterSystem()->SetSimResolution(resolution);
    session.GetWaterSystem()->SetHydrology(true, 0, 0);
    session.GetWaterSystem()->SetBootPaused(false);
    SchedulerChunks chunks;
    for (std::size_t index = 0; index < count; ++index) {
        auto chunk = SchedulerChunk(index, resolution);
        EXPECT_TRUE(world->adopt_streamed_chunk(chunk));
        chunks.emplace(chunk->get_id(), std::move(chunk));
    }
    return chunks;
}

class SchedulerWorld {
public:
    SchedulerWorld(bool regions, int resolution, std::size_t count)
        : world(nullptr, nullptr, Systems::TerrainGenParams{}, 1337, regions)
        , water(nullptr, &world)
        , active_regions(regions) {
        world.SetWaterSystem(&water);
        water.SetSimResolution(resolution);
        water.SetHydrology(true, 0, 0);
        AddChunks(0, count, resolution);
    }

    void AddChunks(std::size_t first, std::size_t end, int resolution) {
        for (std::size_t index = first; index < end; ++index) {
            auto chunk = SchedulerChunk(index, resolution);
            EXPECT_TRUE(world.adopt_streamed_chunk(chunk));
            chunks.emplace(chunk->get_id(), std::move(chunk));
        }
    }

    void Step() {
        water.update(registry, chunks, active_regions);
    }

    Systems::SHIELD_WorldSystem world;
    Systems::WaterSystem water;
    entt::registry registry;
    SchedulerChunks chunks;
    bool active_regions;
};

} // namespace Luminumbra::TestSupport
