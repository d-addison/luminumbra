#include "../fixtures/TerrainFixtureParams.h"
#include "core/JobSystem.h"
#include "luminumbra/core/Types.h"
#include "systems/SHIELD_WorldSystem.h"
#include "systems/WaterSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"

#include <entt/entity/fwd.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
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
    // Direct generation does not perform the streaming lane's Loading -> Idle
    // publication. Adopt a generated, meshable chunk rather than an Unloaded one.
    chunk.set_state(ChunkState::Idle);
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
            auto params = Luminumbra::Test::SyntheticTerrainParams();
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
// Occupy the sole worker through the public queue so simulation can advance
// deterministically after dispatch and before the meshing job reads its inputs.
class PendingMeshGate {
public:
    PendingMeshGate(JobSystem& jobs, SHIELD_WorldSystem& world)
        : m_world(world) {
        auto entered = std::make_shared<std::promise<void>>();
        auto started = entered->get_future();
        const auto release = m_release.get_future().share();
        jobs.dispatch([entered, release]() {
            entered->set_value();
            release.wait();
        });
        EXPECT_EQ(started.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    }
    ~PendingMeshGate() {
        finish();
    }
    void finish() {
        if (!m_finished) {
            m_release.set_value();
            m_finished = true;
        }
        m_world.wait_for_streaming_jobs();
    }

private:
    SHIELD_WorldSystem& m_world;
    std::promise<void> m_release;
    bool m_finished = false;
};

std::unique_ptr<Chunk>
ReferenceMesh(SHIELD_WorldSystem& world, WaterSystem& water, const Chunk& live) {
    auto reference = std::make_unique<Chunk>(live.get_coords());
    reference->sdf_data = live.sdf_data;
    reference->heightmap_data = live.heightmap_data;
    reference->material_data = live.material_data;
    reference->water_depth_mm = live.water_depth_mm;
    reference->water_level_data = live.water_level_data;
    reference->has_water_sim.store(live.has_water_sim.load());
    reference->current_water_resolution.store(live.current_water_resolution.load());
    World::MarchingCubes::PolygoniseTerrain(world, *reference, 0.0f, 1);
    World::MarchingCubes::GenerateWaterMesh(water, world, *reference);
    return reference;
}

TEST(AsyncWaterMeshing, PromotionStageBPublishesExactlyOnce) {
    for (const bool use_workers : {false, true}) {
        for (const bool has_coarse_mesh : {false, true}) {
            SCOPED_TRACE(use_workers);
            SCOPED_TRACE(has_coarse_mesh);
            JobSystem jobs;
            if (use_workers) {
                jobs.startup(1);
            }
            auto params = Luminumbra::Test::SyntheticTerrainParams();
            params.base_amplitude = 0.0f;
            params.height_offset = 8.0f;
            params.caves_enabled = false;
            SHIELD_WorldSystem world(use_workers ? &jobs : nullptr, nullptr, params, 1337);
            world.debug_set_streaming_radius_cap(1);
            WaterSystem water(use_workers ? &jobs : nullptr, &world);
            water.SetBootPaused(true);
            world.SetWaterSystem(&water);
            auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
            FillDugPond(world, *chunk, 8);
            // Promotion regenerates sim truth before taking the stage-B snapshot.
            chunk->material_data.clear();
            world.GenerateChunkData(*chunk);
            auto expected = ReferenceMesh(world, water, *chunk);
            ASSERT_FALSE(expected->mesh_indices.empty());
            ASSERT_FALSE(expected->water_mesh_indices.empty());
            chunk->sdf_data.clear();
            if (has_coarse_mesh) {
                World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 2);
                chunk->current_lod.store(1);
                chunk->set_state(ChunkState::Ready);
            }
            ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
            // A tick stamp prevents opportunistic worker publication during update.
            world.begin_tick(0);
            entt::registry registry;
            const Vec3 anchor(8.0f, 6.0f, 8.0f);
            world.update(registry, anchor, nullptr);
            ASSERT_EQ(world.promotion_dispatch_totals().chunks, 1u);
            world.wait_for_promotion_jobs();
            EXPECT_EQ(chunk->sdf_data, expected->sdf_data);
            // Exercise both forced drains and due-tick publication of stage B.
            world.activate_due(std::numeric_limits<std::int64_t>::max());
            world.wait_for_streaming_jobs();
            EXPECT_EQ(chunk->pending_lod.load(), -1);
            EXPECT_EQ(chunk->current_lod.load(), 0);
            EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
            EXPECT_EQ(chunk->mesh_version.load(), 1u);
            EXPECT_EQ(chunk->water_mesh_version.load(), 1u);
            ExpectVerticesEqual(chunk->mesh_vertices, expected->mesh_vertices);
            EXPECT_EQ(chunk->mesh_indices, expected->mesh_indices);
            ExpectVerticesEqual(chunk->water_mesh_vertices, expected->water_mesh_vertices);
            EXPECT_EQ(chunk->water_mesh_indices, expected->water_mesh_indices);
            world.wait_for_promotion_jobs();
            world.update(registry, anchor, nullptr);
            world.wait_for_streaming_jobs();
            EXPECT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 0u);
            EXPECT_EQ(world.promotion_dispatch_totals().chunks, 1u);
            EXPECT_EQ(chunk->mesh_version.load(), 1u);
            EXPECT_EQ(chunk->water_mesh_version.load(), 1u);
        }
    }
}

TEST(AsyncWaterMeshing, UnusedWaterInputsDoNotQueueRepeatedRemeshes) {
    for (const bool use_workers : {false, true}) {
        for (const std::size_t depth_cells : {0u, 63u, 64u, 65u}) {
            for (const std::size_t level_cells : {64u, 65u}) {
                SCOPED_TRACE(use_workers);
                SCOPED_TRACE(depth_cells);
                SCOPED_TRACE(level_cells);
                JobSystem jobs;
                if (use_workers) {
                    jobs.startup(1);
                }
                auto params = Luminumbra::Test::SyntheticTerrainParams();
                params.base_amplitude = 0.0f;
                params.height_offset = 8.0f;
                params.caves_enabled = false;
                SHIELD_WorldSystem world(use_workers ? &jobs : nullptr, nullptr, params, 1337);
                world.debug_set_streaming_radius_cap(1);
                WaterSystem water(use_workers ? &jobs : nullptr, &world);
                water.SetBootPaused(true);
                world.SetWaterSystem(&water);
                auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
                FillDugPond(world, *chunk, 8);
                chunk->water_depth_mm.resize(depth_cells, 2000);
                chunk->water_level_data.assign(level_cells, 10.0f);
                auto expected = ReferenceMesh(world, water, *chunk);
                ASSERT_FALSE(expected->water_mesh_indices.empty());
                ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
                entt::registry registry;
                const Vec3 anchor(8.0f, 6.0f, 8.0f);
                world.update(registry, anchor, nullptr);
                world.wait_for_streaming_jobs();
                EXPECT_TRUE(chunk->water_mesh_generated.load());
                ExpectVerticesEqual(chunk->water_mesh_vertices, expected->water_mesh_vertices);
                EXPECT_EQ(chunk->water_mesh_indices, expected->water_mesh_indices);
                world.update(registry, anchor, nullptr);
                world.wait_for_streaming_jobs();
                EXPECT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 0u);
                EXPECT_EQ(chunk->water_mesh_version.load(), 1u);
            }
        }
    }
}

enum class WaterInputMutation : std::uint8_t {
    IgnoredTails,
    UnavailableDepth,
    LevelPrefix,
    DepthBecomesAvailable,
    DepthBecomesUnavailable,
    TruncatedSurface,
    MissingSurface
};

TEST(AsyncWaterMeshing, PendingWaterSnapshotComparesOnlyConsumedInputs) {
    // Ignored tails and unavailable depth values may change without invalidating
    // geometry; level prefixes and a change in depth availability must invalidate.
    for (const auto mutation : {WaterInputMutation::IgnoredTails,
                                WaterInputMutation::UnavailableDepth,
                                WaterInputMutation::LevelPrefix,
                                WaterInputMutation::DepthBecomesAvailable,
                                WaterInputMutation::DepthBecomesUnavailable,
                                WaterInputMutation::TruncatedSurface,
                                WaterInputMutation::MissingSurface}) {
        SCOPED_TRACE(static_cast<int>(mutation));
        JobSystem jobs;
        jobs.startup(1);
        auto params = Luminumbra::Test::SyntheticTerrainParams();
        params.base_amplitude = 0.0f;
        params.height_offset = 8.0f;
        params.caves_enabled = false;
        SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
        world.debug_set_streaming_radius_cap(1);
        WaterSystem water(&jobs, &world);
        water.SetBootPaused(true);
        world.SetWaterSystem(&water);
        auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
        FillDugPond(world, *chunk, 8);
        chunk->water_level_data.assign(65u, 10.0f);
        chunk->water_depth_mm.resize(
            mutation == WaterInputMutation::DepthBecomesUnavailable ? 64u : 65u, 2000);
        auto expected = ReferenceMesh(world, water, *chunk);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        PendingMeshGate gate(jobs, world);
        entt::registry registry;
        world.update(registry, Vec3(8.0f, 6.0f, 8.0f), nullptr);
        ASSERT_EQ(chunk->pending_lod.load(), 0);
        if (mutation == WaterInputMutation::IgnoredTails) {
            chunk->water_level_data.back() = 100.0f;
            chunk->water_depth_mm.back() = 0;
        } else if (mutation == WaterInputMutation::UnavailableDepth) {
            chunk->water_depth_mm.clear();
        } else if (mutation == WaterInputMutation::LevelPrefix) {
            chunk->water_level_data.front() = 100.0f;
        } else if (mutation == WaterInputMutation::DepthBecomesAvailable) {
            chunk->water_depth_mm.resize(64u);
        } else if (mutation == WaterInputMutation::DepthBecomesUnavailable) {
            chunk->water_depth_mm.push_back(2000);
        } else if (mutation == WaterInputMutation::TruncatedSurface) {
            chunk->water_level_data.resize(63u);
        } else {
            chunk->water_level_data.clear();
        }
        gate.finish();
        ExpectVerticesEqual(chunk->water_mesh_vertices, expected->water_mesh_vertices);
        EXPECT_EQ(chunk->water_mesh_indices, expected->water_mesh_indices);
        EXPECT_EQ(chunk->water_mesh_generated.load(),
                  mutation == WaterInputMutation::IgnoredTails ||
                      mutation == WaterInputMutation::UnavailableDepth);
    }
}

void CheckAdvancingWater(MeshRequest request, bool resize_pending) {
    JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 8.0f;
    params.caves_enabled = false;
    SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    WaterSystem water(&jobs, &world);
    water.SetSimResolution(8);
    water.SetHydrology(true, 100, 0);
    world.SetWaterSystem(&water);
    auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
    FillDugPond(world, *chunk, 8);
    if (request != MeshRequest::Initial) {
        World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 1);
        chunk->set_state(ChunkState::Ready);
        chunk->current_lod.store(request == MeshRequest::TerrainRemesh ? 1 : 0);
    }
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    PendingMeshGate gate(jobs, world);
    entt::registry registry;
    const Vec3 anchor(8.0f, 6.0f, 8.0f);
    world.update(registry, anchor, nullptr);
    ASSERT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 1u);
    ASSERT_EQ(chunk->pending_lod.load(), 0);
    auto dispatched = ReferenceMesh(world, water, *chunk);
    ASSERT_FALSE(dispatched->water_mesh_vertices.empty());
    const auto dispatch_depth = chunk->water_depth_mm;
    if (resize_pending) {
        water.SetSimResolution(16);
    }
    for (int tick = 0; tick < 2; ++tick) {
        world.update(registry, anchor, nullptr);
        EXPECT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 0u);
        EXPECT_EQ(chunk->pending_lod.load(), 0);
    }
    EXPECT_NE(chunk->water_depth_mm, dispatch_depth);
    EXPECT_EQ(chunk->current_water_resolution.load(), resize_pending ? 16 : 8);
    gate.finish();
    EXPECT_EQ(chunk->water_mesh_version.load(), 1u);
    ExpectVerticesEqual(chunk->water_mesh_vertices, dispatched->water_mesh_vertices);
    EXPECT_EQ(chunk->water_mesh_indices, dispatched->water_mesh_indices);
    ExpectVerticesEqual(chunk->mesh_vertices, dispatched->mesh_vertices);
    EXPECT_EQ(chunk->mesh_indices, dispatched->mesh_indices);
    EXPECT_FALSE(chunk->water_mesh_generated.load());

    // Keep simulation running for a second dispatch. The second worker must
    // mesh the new state, and publication must not lose that invalidation.
    PendingMeshGate next_gate(jobs, world);
    world.update(registry, anchor, nullptr);
    ASSERT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 1u);
    auto newer = ReferenceMesh(world, water, *chunk);
    ASSERT_NE(newer->water_level_data, dispatched->water_level_data);
    next_gate.finish();
    EXPECT_EQ(chunk->water_mesh_version.load(), 2u);
    ExpectVerticesEqual(chunk->water_mesh_vertices, newer->water_mesh_vertices);
    EXPECT_EQ(chunk->water_mesh_indices, newer->water_mesh_indices);
    EXPECT_TRUE(chunk->water_mesh_generated.load());
}

TEST(AsyncWaterMeshing, InitialMeshUsesDispatchStateWhileWaterAdvances) {
    CheckAdvancingWater(MeshRequest::Initial, false);
}

TEST(AsyncWaterMeshing, TerrainRemeshUsesDispatchStateWhileWaterAdvances) {
    CheckAdvancingWater(MeshRequest::TerrainRemesh, false);
}

TEST(AsyncWaterMeshing, WaterOnlyRemeshUsesDispatchStateWhileWaterAdvances) {
    CheckAdvancingWater(MeshRequest::WaterOnly, false);
}

TEST(AsyncWaterMeshing, PendingMeshOwnsWaterGridAcrossResolutionChange) {
    CheckAdvancingWater(MeshRequest::WaterOnly, true);
}

enum class SupersedingChange : std::uint8_t {
    TerrainMesh,
    WaterMesh,
    VoxelEdit
};

TEST(AsyncWaterMeshing, StaleResultsCannotReplaceNewerMeshesOrVoxelEdits) {
    for (const auto change : {SupersedingChange::TerrainMesh,
                              SupersedingChange::WaterMesh,
                              SupersedingChange::VoxelEdit}) {
        SCOPED_TRACE(static_cast<int>(change));
        JobSystem jobs;
        jobs.startup(1);
        auto params = Luminumbra::Test::SyntheticTerrainParams();
        params.base_amplitude = 0.0f;
        params.height_offset = 8.0f;
        params.caves_enabled = false;
        SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
        WaterSystem water(&jobs, &world);
        water.SetSimResolution(8);
        world.SetWaterSystem(&water);
        auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
        FillDugPond(world, *chunk, 8);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        PendingMeshGate gate(jobs, world);
        entt::registry registry;
        world.update(registry, Vec3(8.0f, 6.0f, 8.0f), nullptr);
        ASSERT_EQ(chunk->pending_lod.load(), 0);
        // Model a synchronous public mesh rebuild or a voxel edit while the
        // asynchronous snapshot is queued. Its result must be retained.
        if (change == SupersedingChange::TerrainMesh) {
            chunk->mesh_version++;
        } else if (change == SupersedingChange::WaterMesh) {
            chunk->water_mesh_version++;
        } else {
            for (auto& density : chunk->sdf_data) {
                density += 1.0f;
            }
            chunk->mark_voxel_data_dirty();
            chunk->current_lod.store(-1);
        }
        auto newer = ReferenceMesh(world, water, *chunk);
        chunk->mesh_vertices = newer->mesh_vertices;
        chunk->mesh_indices = newer->mesh_indices;
        chunk->water_mesh_vertices = newer->water_mesh_vertices;
        chunk->water_mesh_indices = newer->water_mesh_indices;
        const auto terrain_version = chunk->mesh_version.load();
        const auto water_version = chunk->water_mesh_version.load();
        gate.finish();
        EXPECT_EQ(chunk->mesh_version.load(), terrain_version);
        EXPECT_EQ(chunk->water_mesh_version.load(), water_version);
        EXPECT_EQ(chunk->pending_lod.load(), -1);
        ExpectVerticesEqual(chunk->mesh_vertices, newer->mesh_vertices);
        EXPECT_EQ(chunk->mesh_indices, newer->mesh_indices);
        ExpectVerticesEqual(chunk->water_mesh_vertices, newer->water_mesh_vertices);
        EXPECT_EQ(chunk->water_mesh_indices, newer->water_mesh_indices);
        if (change == SupersedingChange::VoxelEdit) {
            world.update(registry, Vec3(8.0f, 6.0f, 8.0f), nullptr);
            ASSERT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 1u);
            world.wait_for_streaming_jobs();
            EXPECT_EQ(chunk->mesh_version.load(), terrain_version + 1u);
            ExpectVerticesEqual(chunk->mesh_vertices, newer->mesh_vertices);
        }
    }
}

TEST(AsyncWaterMeshing, EvictedChunkResultCannotPublishIntoReplacementGeneration) {
    JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 8.0f;
    params.caves_enabled = false;
    SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    world.debug_set_streaming_radius_cap(1);
    WaterSystem water(&jobs, &world);
    water.SetSimResolution(8);
    world.SetWaterSystem(&water);
    auto old_chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
    FillDugPond(world, *old_chunk, 8);
    ASSERT_TRUE(world.adopt_streamed_chunk(old_chunk));
    PendingMeshGate gate(jobs, world);
    entt::registry registry;
    world.update(registry, Vec3(8.0f, 6.0f, 8.0f), nullptr);
    ASSERT_EQ(old_chunk->pending_lod.load(), 0);
    const auto interval = world.get_last_streaming_budget_stats().update_interval_frames;
    for (int frame = 1; frame < interval; ++frame) {
        world.update(registry, Vec3(4096.0f, 6.0f, 4096.0f), nullptr);
    }
    ASSERT_EQ(world.find_streamed_chunk(IVec3(0, 0, 0)), nullptr);
    auto replacement = std::make_shared<Chunk>(IVec3(0, 0, 0));
    FillDugPond(world, *replacement, 8);
    auto expected = ReferenceMesh(world, water, *replacement);
    replacement->mesh_vertices = expected->mesh_vertices;
    replacement->mesh_indices = expected->mesh_indices;
    replacement->water_mesh_vertices = expected->water_mesh_vertices;
    replacement->water_mesh_indices = expected->water_mesh_indices;
    replacement->mesh_version.store(7);
    replacement->water_mesh_version.store(9);
    ASSERT_TRUE(world.adopt_streamed_chunk(replacement));
    gate.finish();
    EXPECT_EQ(world.find_streamed_chunk(IVec3(0, 0, 0)), replacement);
    EXPECT_EQ(old_chunk->mesh_version.load(), 0u);
    EXPECT_EQ(old_chunk->water_mesh_version.load(), 0u);
    EXPECT_EQ(replacement->mesh_version.load(), 7u);
    EXPECT_EQ(replacement->water_mesh_version.load(), 9u);
    ExpectVerticesEqual(replacement->mesh_vertices, expected->mesh_vertices);
    ExpectVerticesEqual(replacement->water_mesh_vertices, expected->water_mesh_vertices);
}

TEST(AsyncWaterMeshing, MalformedHeightmapsKeepAnalyticCoarseFallback) {
    constexpr std::size_t kHeightmapCells =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Z + 1);
    for (const bool use_workers : {false, true}) {
        for (const auto cells : {std::size_t{0}, kHeightmapCells - 1u, kHeightmapCells + 1u}) {
            SCOPED_TRACE(use_workers);
            SCOPED_TRACE(cells);
            JobSystem jobs;
            if (use_workers) {
                jobs.startup(1);
            }
            auto params = Luminumbra::Test::SyntheticTerrainParams();
            params.base_amplitude = 0.0f;
            params.height_offset = 8.0f;
            params.caves_enabled = false;
            SHIELD_WorldSystem world(use_workers ? &jobs : nullptr, nullptr, params, 1337);
            auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
            chunk->heightmap_data.assign(cells, 6.0f);
            World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 2);
            const auto expected_vertices = chunk->mesh_vertices;
            const auto expected_indices = chunk->mesh_indices;
            ASSERT_FALSE(expected_vertices.empty());
            chunk->mesh_vertices.clear();
            chunk->mesh_indices.clear();
            chunk->set_state(ChunkState::Idle);
            ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
            entt::registry registry;
            world.update(registry, Vec3(264.0f, 6.0f, 8.0f), nullptr);
            world.wait_for_streaming_jobs();
            EXPECT_EQ(chunk->current_lod.load(), 1);
            ExpectVerticesEqual(chunk->mesh_vertices, expected_vertices);
            EXPECT_EQ(chunk->mesh_indices, expected_indices);
        }
    }
}

TEST(AsyncWaterMeshing, InvalidResolutionAndPartialSurfaceRejectBeforeAllocation) {
    SHIELD_WorldSystem world(nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    const WaterSystem water(nullptr, &world);
    Chunk chunk(IVec3(0, 0, 0));
    chunk.has_water_sim.store(true);
    for (const int resolution : {-1, 0, 1, 33, std::numeric_limits<int>::max(), 8}) {
        SCOPED_TRACE(resolution);
        chunk.current_water_resolution.store(resolution);
        chunk.water_level_data.assign(resolution == 8 ? 63u : 0u, 6.0f);
        chunk.water_mesh_vertices = {{Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f), 0u}};
        chunk.water_mesh_indices = {0u};
        chunk.water_mesh_generated.store(true);
        World::MarchingCubes::GenerateWaterMesh(water, world, chunk);
        EXPECT_TRUE(chunk.water_mesh_vertices.empty());
        EXPECT_TRUE(chunk.water_mesh_indices.empty());
        EXPECT_FALSE(chunk.water_mesh_generated.load());
    }
}
} // namespace
