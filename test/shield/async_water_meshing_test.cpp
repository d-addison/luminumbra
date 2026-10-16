#include "../fixtures/TerrainFixtureParams.h"
#include "core/JobSystem.h"
#include "luminumbra/core/Types.h"
#include "systems/SHIELD_WorldSystem.h"
#include "systems/WaterSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"

#include <entt/entity/fwd.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::Systems;

enum class MeshRequest : std::uint8_t {
    Initial,
    TerrainRemesh,
    WaterOnly
};
enum class DepthGrid : std::uint8_t {
    Complete,
    Missing,
    Truncated
};

std::size_t FillDugPond(SHIELD_WorldSystem& world, Chunk& chunk, int resolution) {
    world.GenerateChunkData(chunk);
    // A saved edit: a rectangular basin with its floor at 4 m, below the
    // procedural 8 m surface. Keep both terrain channels consistent.
    constexpr std::size_t kSideX = CHUNK_SIZE_X + 1;
    constexpr std::size_t kSideY = CHUNK_SIZE_Y + 1;
    for (int z = 4; z <= 12; ++z) {
        for (int x = 4; x <= 12; ++x) {
            chunk.heightmap_data[static_cast<std::size_t>(x) +
                                 static_cast<std::size_t>(z) * kSideX] = 4.0f;
            for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
                chunk.sdf_data[static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * kSideX +
                               static_cast<std::size_t>(z) * kSideX * kSideY] =
                    static_cast<float>(y) - 4.0f;
            }
        }
    }
    // Authored material must survive the same scratch snapshot as the edited SDF.
    chunk.material_data.assign(chunk.sdf_data.size(), static_cast<u8>(MaterialType::Stone));
    chunk.mark_voxel_data_dirty();
    const auto cells = static_cast<std::size_t>(resolution) * resolution;
    chunk.current_water_resolution.store(resolution);
    chunk.has_water_sim.store(true);
    chunk.water_level_data.assign(cells, 6.0f);
    chunk.water_flow_data.assign(cells, Vec2(0.0f));
    chunk.water_sim_terrain_height.assign(cells, 4.0f);
    chunk.water_bed_mm.assign(cells, 4000);
    chunk.water_depth_mm.assign(cells, 0);
    std::size_t wet_cells = 0;
    for (int z = resolution / 4; z < 3 * resolution / 4; ++z) {
        for (int x = resolution / 4; x < 3 * resolution / 4; ++x) {
            const auto index = static_cast<std::size_t>(z) * resolution + x;
            // Exercise the strict 50 mm culling boundary as well as dry cells.
            const std::int32_t depth = (x + z) % 3 == 0 ? 50 : 2000;
            chunk.water_depth_mm[index] = depth;
            if (depth > 50) {
                ++wet_cells;
            }
        }
    }
    return wet_cells;
}

void ExpectVerticesEqual(const std::vector<VoxelVertex>& actual,
                         const std::vector<VoxelVertex>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        SCOPED_TRACE(index);
        EXPECT_EQ(actual[index].position, expected[index].position);
        EXPECT_EQ(actual[index].normal, expected[index].normal);
        EXPECT_EQ(actual[index].material_id, expected[index].material_id);
    }
}

void CheckDugPondMesh(bool use_workers, MeshRequest request) {
    // All supported water resolutions, including the maximum scratch-copy size.
    for (const int resolution : {4, 8, 16, 32}) {
        for (const DepthGrid depth_grid :
             {DepthGrid::Complete, DepthGrid::Missing, DepthGrid::Truncated}) {
            SCOPED_TRACE(resolution);
            SCOPED_TRACE(static_cast<int>(depth_grid));
            JobSystem jobs;
            if (use_workers) {
                jobs.startup(1);
            }
            auto params = Test::SyntheticTerrainParams();
            params.base_amplitude = 0.0f;
            params.height_offset = 8.0f;
            params.caves_enabled = false;
            SHIELD_WorldSystem world(use_workers ? &jobs : nullptr, nullptr, params, 1337);
            WaterSystem water(use_workers ? &jobs : nullptr, &world);
            // Preserve the saved standing water while update dispatches real meshing jobs.
            water.SetBootPaused(true);
            world.SetWaterSystem(&water);

            auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
            const std::size_t wet_cells = FillDugPond(world, *chunk, resolution);
            if (depth_grid == DepthGrid::Missing) {
                chunk->water_depth_mm.clear();
            } else if (depth_grid == DepthGrid::Truncated) {
                chunk->water_depth_mm.pop_back();
            }
            const auto saved_depth = chunk->water_depth_mm;
            ASSERT_GT(world.GetTerrainHeightAt(8.0f, 8.0f), 6.0f);
            World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 1);
            const auto expected_terrain_vertices = chunk->mesh_vertices;
            const auto expected_terrain_indices = chunk->mesh_indices;
            ASSERT_FALSE(expected_terrain_vertices.empty());
            World::MarchingCubes::GenerateWaterMesh(water, world, *chunk);
            const auto expected_water_vertices = chunk->water_mesh_vertices;
            const auto expected_water_indices = chunk->water_mesh_indices;
            if (depth_grid == DepthGrid::Complete) {
                ASSERT_GT(wet_cells, 0u);
                ASSERT_EQ(expected_water_vertices.size(), wet_cells * 4u);
                ASSERT_EQ(expected_water_indices.size(), wet_cells * 6u);
            } else {
                // Legacy/incomplete depth grids use procedural culling, so this
                // below-terrain surface must stay empty on both paths.
                ASSERT_TRUE(expected_water_vertices.empty());
                ASSERT_TRUE(expected_water_indices.empty());
            }

            if (request == MeshRequest::Initial) {
                chunk->mesh_vertices.clear();
                chunk->mesh_indices.clear();
                chunk->set_state(ChunkState::Idle);
            } else {
                chunk->set_state(ChunkState::Ready);
                chunk->current_lod.store(request == MeshRequest::TerrainRemesh ? 1 : 0);
            }
            chunk->water_mesh_vertices.clear();
            chunk->water_mesh_indices.clear();
            chunk->water_mesh_generated.store(false);
            ASSERT_TRUE(world.adopt_streamed_chunk(chunk));

            entt::registry registry;
            world.update(registry, Vec3(8.0f, 6.0f, 8.0f), nullptr);
            world.wait_for_streaming_jobs();
            ASSERT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 1u);

            EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
            EXPECT_EQ(chunk->current_lod.load(), 0);
            EXPECT_EQ(chunk->water_mesh_version.load(), 1u);
            EXPECT_EQ(chunk->mesh_version.load(), request == MeshRequest::WaterOnly ? 0u : 1u);
            EXPECT_TRUE(chunk->water_mesh_generated.load());
            ExpectVerticesEqual(chunk->water_mesh_vertices, expected_water_vertices);
            EXPECT_EQ(chunk->water_mesh_indices, expected_water_indices);
            ExpectVerticesEqual(chunk->mesh_vertices, expected_terrain_vertices);
            EXPECT_EQ(chunk->mesh_indices, expected_terrain_indices);
            EXPECT_EQ(chunk->water_depth_mm, saved_depth);
        }
    }
}

TEST(AsyncWaterMeshing, InitialMeshKeepsDugPondDepth) {
    CheckDugPondMesh(true, MeshRequest::Initial);
}

TEST(AsyncWaterMeshing, TerrainRemeshKeepsDugPondDepth) {
    CheckDugPondMesh(true, MeshRequest::TerrainRemesh);
}

TEST(AsyncWaterMeshing, WaterOnlyRemeshKeepsDugPondDepth) {
    CheckDugPondMesh(true, MeshRequest::WaterOnly);
}

TEST(AsyncWaterMeshing, InlineInitialMeshKeepsDugPondDepth) {
    CheckDugPondMesh(false, MeshRequest::Initial);
}

TEST(AsyncWaterMeshing, InlineTerrainRemeshKeepsDugPondDepth) {
    CheckDugPondMesh(false, MeshRequest::TerrainRemesh);
}

TEST(AsyncWaterMeshing, InlineWaterOnlyRemeshKeepsDugPondDepth) {
    CheckDugPondMesh(false, MeshRequest::WaterOnly);
}
} // namespace
