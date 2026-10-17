#include "../fixtures/TerrainFixtureParams.h"
#include "core/JobSystem.h"
#include "luminumbra/core/Types.h"
#include "systems/SHIELD_WorldSystem.h"
#include "systems/WaterSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"

#include <entt/entity/fwd.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <utility>
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

// A fixed 5*5 burst exceeds three dispatch budgets, even with every channel
// present. Empty anchors keep activation from adding unrelated chunks.
constexpr std::size_t kBurstChunks = 25;
constexpr std::size_t kSnapshotBytes =
    std::size_t{17} * 17u * 17u * (sizeof(float) + sizeof(u8)) +
    std::size_t{17} * 17u * sizeof(float) +
    std::size_t{32} * 32u * (2u * sizeof(float) + sizeof(Vec2) + sizeof(std::int32_t));
static_assert(kBurstChunks * kSnapshotBytes > 3u * SHIELD_WorldSystem::kMeshingSnapshotByteBudget);

struct BudgetedBurstMeshes {
    std::vector<std::shared_ptr<Chunk>> chunks;
    std::vector<std::unique_ptr<Chunk>> expected;
};

void PrepareBudgetedBurst(SHIELD_WorldSystem& world,
                          WaterSystem& water,
                          bool promotion,
                          BudgetedBurstMeshes& burst) {
    auto& chunks = burst.chunks;
    auto& expected = burst.expected;
    chunks.reserve(kBurstChunks);
    expected.reserve(kBurstChunks);
    for (int z = -2; z <= 2; ++z) {
        for (int x = -2; x <= 2; ++x) {
            auto chunk = std::make_shared<Chunk>(IVec3(x, 0, z));
            FillDugPond(world, *chunk, 32);
            if (promotion) {
                chunk->material_data.clear();
                world.GenerateChunkData(*chunk);
            }
            expected.push_back(ReferenceMesh(world, water, *chunk));
            ASSERT_FALSE(expected.back()->mesh_indices.empty());
            ASSERT_FALSE(expected.back()->water_mesh_indices.empty());
            // Alternate meshless/high-lane work with active-mesh/normal-lane work.
            if (chunks.size() % 2u != 0u) {
                chunk->mesh_vertices = expected.back()->mesh_vertices;
                chunk->mesh_indices = expected.back()->mesh_indices;
                chunk->set_state(ChunkState::Ready);
                chunk->current_lod.store(1);
            }
            if (promotion) {
                chunk->sdf_data.clear();
            }
            ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
            chunks.push_back(std::move(chunk));
        }
    }
}

void EditDeferredBurstChunk(SHIELD_WorldSystem& world,
                            WaterSystem& water,
                            Chunk& chunk,
                            std::unique_ptr<Chunk>& expected) {
    // The next pass must capture this new terrain, material and water
    // state, rather than an eager snapshot of the deferred suffix.
    for (auto& density : chunk.sdf_data) {
        density += 1.0f;
    }
    for (auto& height : chunk.heightmap_data) {
        height -= 1.0f;
    }
    std::fill(chunk.material_data.begin(),
              chunk.material_data.end(),
              static_cast<u8>(MaterialType::Soil));
    std::fill(chunk.water_level_data.begin(), chunk.water_level_data.end(), 7.0f);
    chunk.mark_voxel_data_dirty();
    auto edited_reference = ReferenceMesh(world, water, chunk);
    ASSERT_FALSE(edited_reference->mesh_vertices.empty());
    ASSERT_NE(edited_reference->mesh_vertices.front().position,
              expected->mesh_vertices.front().position);
    expected = std::move(edited_reference);
}

void InspectDeferredBurst(SHIELD_WorldSystem& world,
                          WaterSystem& water,
                          bool edit_deferred,
                          BudgetedBurstMeshes& burst,
                          bool& saw_deferred,
                          bool& edited) {
    auto& chunks = burst.chunks;
    auto& expected = burst.expected;
    // Preparation fixes this scan at 25 chunks, with at most one bounded edit.
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        auto& chunk = *chunks[index];
        if (chunk.mesh_version.load() != 0u || chunk.pending_lod.load() >= 0) {
            continue;
        }
        saw_deferred = true;
        EXPECT_EQ(chunk.get_state(), index % 2u == 0u ? ChunkState::Idle : ChunkState::Ready);
        EXPECT_EQ(chunk.current_lod.load(), index % 2u == 0u ? -1 : 1);
        EXPECT_EQ(chunk.pending_lod.load(), -1);
        EXPECT_EQ(chunk.water_mesh_version.load(), 0u);
        EXPECT_FALSE(chunk.pending_mesh_ready.load());
        EXPECT_FALSE(chunk.pending_mesh_failed.load());
        EXPECT_TRUE(chunk.pending_mesh_vertices.empty());
        EXPECT_TRUE(chunk.pending_water_mesh_vertices.empty());
        if (edit_deferred && !edited) {
            ASSERT_NO_FATAL_FAILURE(EditDeferredBurstChunk(world, water, chunk, expected[index]));
            edited = true;
        }
    }
}

std::size_t CheckCompletedBurst(const BudgetedBurstMeshes& burst) {
    const auto& chunks = burst.chunks;
    const auto& expected = burst.expected;
    // Each comparison covers one chunk's bounded terrain and water geometry.
    std::size_t completed = 0;
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const auto& chunk = *chunks[index];
        if (chunk.mesh_version.load() == 0u) {
            continue;
        }
        ++completed;
        EXPECT_EQ(chunk.mesh_version.load(), 1u);
        EXPECT_EQ(chunk.water_mesh_version.load(), 1u);
        EXPECT_EQ(chunk.pending_lod.load(), -1);
        EXPECT_EQ(chunk.get_state(), ChunkState::Ready);
        ExpectVerticesEqual(chunk.mesh_vertices, expected[index]->mesh_vertices);
        EXPECT_EQ(chunk.mesh_indices, expected[index]->mesh_indices);
        ExpectVerticesEqual(chunk.water_mesh_vertices, expected[index]->water_mesh_vertices);
        EXPECT_EQ(chunk.water_mesh_indices, expected[index]->water_mesh_indices);
    }
    return completed;
}

void CheckBudgetedBurst(bool use_workers, bool promotion, bool edit_deferred) {
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
    water.SetBootPaused(true);
    world.SetWaterSystem(&water);
    world.begin_tick(0);

    BudgetedBurstMeshes burst;
    ASSERT_NO_FATAL_FAILURE(PrepareBudgetedBurst(world, water, promotion, burst));

    entt::registry registry;
    bool saw_deferred = false;
    bool edited = false;
    std::size_t completed = 0;
    std::size_t passes = 0;
    // At least one item must progress per pass, so the fixed burst is also the
    // pass bound. All inner scans/comparisons cover only these 25 bounded chunks.
    for (; passes < kBurstChunks && completed < kBurstChunks; ++passes) {
        const auto completed_before = completed;
        world.update(registry, std::vector<Vec3>{}, nullptr);
        EXPECT_LE(world.last_meshing_snapshot_bytes(),
                  SHIELD_WorldSystem::kMeshingSnapshotByteBudget);
        ASSERT_NO_FATAL_FAILURE(
            InspectDeferredBurst(world, water, edit_deferred, burst, saw_deferred, edited));
        world.wait_for_streaming_jobs();
        completed = CheckCompletedBurst(burst);
        EXPECT_GT(completed, completed_before);
        EXPECT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing,
                  completed - completed_before);
        EXPECT_EQ(world.get_last_streaming_budget_stats().deferred_meshing,
                  kBurstChunks - completed);
        const auto copied_per_chunk =
            kSnapshotBytes - (promotion ? std::size_t{17} * 17u * 17u * sizeof(u8) : 0u);
        EXPECT_EQ(world.last_meshing_snapshot_bytes(),
                  (completed - completed_before) * copied_per_chunk);
        EXPECT_LE(world.last_meshing_snapshot_bytes(),
                  SHIELD_WorldSystem::kMeshingSnapshotByteBudget);
    }
    EXPECT_EQ(completed, kBurstChunks);
    EXPECT_GE(passes, 3u);
    EXPECT_TRUE(saw_deferred);
    EXPECT_EQ(edited, edit_deferred);
    if (promotion) {
        EXPECT_EQ(world.promotion_dispatch_totals().chunks, kBurstChunks);
    }
    world.update(registry, std::vector<Vec3>{}, nullptr);
    world.wait_for_streaming_jobs();
    EXPECT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 0u);
}

TEST(AsyncWaterMeshing, SnapshotByteBudgetDrainsBurstWithIdenticalMeshes) {
    for (const bool use_workers : {false, true}) {
        SCOPED_TRACE(use_workers);
        CheckBudgetedBurst(use_workers, false, false);
    }
}

TEST(AsyncWaterMeshing, BudgetDeferredChunkCapturesEditsOnNextPass) {
    for (const bool use_workers : {false, true}) {
        SCOPED_TRACE(use_workers);
        CheckBudgetedBurst(use_workers, false, true);
    }
}

TEST(AsyncWaterMeshing, SnapshotByteBudgetPreservesPromotionHandoff) {
    for (const bool use_workers : {false, true}) {
        SCOPED_TRACE(use_workers);
        CheckBudgetedBurst(use_workers, true, false);
    }
}

TEST(AsyncWaterMeshing, SnapshotByteAccountingBoundsPartialAndOversizedChannels) {
    // Each row supplies a fixed 15-chunk burst. Expected byte counts include only
    // channels the snapshot copies, including short mirrors and rejected tails.
    struct InputCase {
        int resolution;
        bool water_only;
        std::size_t levels;
        std::size_t flows;
        std::size_t beds;
        std::size_t depths;
        std::size_t heights;
        std::size_t materials;
        std::size_t expected_bytes;
    };
    constexpr std::size_t kSdfBytes = 4913u * sizeof(float);
    constexpr std::size_t kHeightBytes = 289u * sizeof(float);
    constexpr std::size_t kWaterBytes =
        1024u * (2u * sizeof(float) + sizeof(Vec2) + sizeof(std::int32_t));
    constexpr InputCase kCases[] = {
        {32,
         false,
         1024,
         1024,
         1024,
         1024,
         289,
         4913,
         kSdfBytes + kHeightBytes + 4913u + kWaterBytes},
        {32, false, 3, 2, 1, 1023, 288, 4912, kSdfBytes + 4u * sizeof(float) + 2u * sizeof(Vec2)},
        {32,
         false,
         1025,
         1025,
         1025,
         1025,
         290,
         4914,
         kSdfBytes + kWaterBytes - 1024u * sizeof(std::int32_t)},
        {0, false, 1024, 1024, 1024, 1024, 0, 0, kSdfBytes},
        {std::numeric_limits<int>::max(),
         false,
         1024,
         1024,
         1024,
         1024,
         289,
         4913,
         kSdfBytes + kHeightBytes + 4913u},
        {32, true, 1024, 1024, 1024, 1024, 289, 4913, kWaterBytes},
        {0, true, 0, 0, 0, 0, 0, 0, 0}};
    for (const bool use_workers : {false, true}) {
        for (const auto& input : kCases) {
            SCOPED_TRACE(use_workers);
            SCOPED_TRACE(input.expected_bytes);
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
            water.SetBootPaused(true);
            world.SetWaterSystem(&water);
            constexpr std::size_t kChunks = 15;
            std::vector<std::shared_ptr<Chunk>> chunks;
            for (std::size_t index = 0; index < kChunks; ++index) {
                auto chunk = std::make_shared<Chunk>(
                    IVec3(static_cast<int>(index % 4u), 0, static_cast<int>(index / 4u)));
                FillDugPond(world, *chunk, 32);
                if (input.water_only) {
                    World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 1);
                    chunk->set_state(ChunkState::Ready);
                    chunk->current_lod.store(0);
                }
                chunk->current_water_resolution.store(input.resolution);
                chunk->water_level_data.resize(input.levels);
                chunk->water_flow_data.resize(input.flows);
                chunk->water_sim_terrain_height.resize(input.beds);
                chunk->water_depth_mm.resize(input.depths);
                chunk->heightmap_data.resize(input.heights);
                chunk->material_data.resize(input.materials);
                ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
                chunks.push_back(std::move(chunk));
            }
            entt::registry registry;
            world.update(registry, std::vector<Vec3>{}, nullptr);
            world.wait_for_streaming_jobs();
            const auto admitted = input.expected_bytes == 0u
                                      ? kChunks
                                      : std::min(kChunks,
                                                 SHIELD_WorldSystem::kMeshingSnapshotByteBudget /
                                                     input.expected_bytes);
            EXPECT_EQ(world.last_meshing_snapshot_bytes(), admitted * input.expected_bytes);
            EXPECT_LE(world.last_meshing_snapshot_bytes(),
                      SHIELD_WorldSystem::kMeshingSnapshotByteBudget);
            std::size_t published = 0;
            for (const auto& chunk : chunks) {
                if (chunk->water_mesh_version.load() == 1u) {
                    ++published;
                    EXPECT_EQ(chunk->mesh_version.load(), input.water_only ? 0u : 1u);
                }
                EXPECT_EQ(chunk->pending_lod.load(), -1);
            }
            EXPECT_EQ(published, admitted);
        }
    }
}

TEST(AsyncWaterMeshing, SnapshotByteBudgetAdmitsExactFitAndEmptySnapshot) {
    for (const bool use_workers : {false, true}) {
        SCOPED_TRACE(use_workers);
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
        water.SetBootPaused(true);
        world.SetWaterSystem(&water);
        world.begin_tick(0);
        std::vector<std::shared_ptr<Chunk>> chunks;
        // Fifteen nearby active chunks request water-only snapshots. With no
        // anchors, the existing candidate order is chunk id within this LOD.
        constexpr std::size_t kChunks = 15;
        for (std::size_t index = 0; index < kChunks; ++index) {
            auto chunk = std::make_shared<Chunk>(
                IVec3(static_cast<int>(index % 4u), 0, static_cast<int>(index / 4u)));
            FillDugPond(world, *chunk, 32);
            World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 1);
            chunk->set_state(ChunkState::Ready);
            chunk->current_lod.store(0);
            ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
            chunks.push_back(std::move(chunk));
        }
        std::sort(chunks.begin(), chunks.end(), [](const auto& left, const auto& right) {
            return left->get_id() < right->get_id();
        });
        // 12 * 20 KiB + 16 KiB = exactly 256 KiB. A zero-byte snapshot still
        // fits; the following four-byte partial surface must remain untouched.
        chunks[12]->water_depth_mm.clear();
        chunks[13]->current_water_resolution.store(0);
        chunks[14]->water_level_data.resize(1);
        chunks[14]->water_flow_data.clear();
        chunks[14]->water_sim_terrain_height.clear();
        chunks[14]->water_depth_mm.clear();
        entt::registry registry;
        world.update(registry, std::vector<Vec3>{}, nullptr);
        EXPECT_EQ(world.last_meshing_snapshot_bytes(),
                  SHIELD_WorldSystem::kMeshingSnapshotByteBudget);
        EXPECT_EQ(chunks.back()->pending_lod.load(), -1);
        EXPECT_EQ(chunks.back()->water_mesh_version.load(), 0u);
        world.wait_for_streaming_jobs();
        for (std::size_t index = 0; index < kChunks - 1u; ++index) {
            EXPECT_EQ(chunks[index]->water_mesh_version.load(), 1u);
        }
        world.update(registry, std::vector<Vec3>{}, nullptr);
        world.wait_for_streaming_jobs();
        EXPECT_EQ(chunks.back()->water_mesh_version.load(), 1u);
        EXPECT_EQ(world.last_meshing_snapshot_bytes(), sizeof(float));
    }
}
} // namespace
