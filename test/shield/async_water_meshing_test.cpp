#include "../fixtures/TerrainFixtureParams.h"
#include "core/JobSystem.h"
#include "luminumbra/core/Types.h"
#include "systems/PhysicsSystem.h"
#include "systems/RegionalWater.h"
#include "systems/SHIELD_WorldSystem.h"
#include "systems/WaterSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"

#include <entt/entity/fwd.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_set>
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
    const auto cells = static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
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
            const auto index = static_cast<std::size_t>(z) * static_cast<std::size_t>(resolution) +
                               static_cast<std::size_t>(x);
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
    void release() {
        if (!m_finished) {
            m_release.set_value();
            m_finished = true;
        }
    }
    void finish() {
        release();
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

TEST(AsyncWaterMeshing, PendingWaterMeshRetainsAnchorLodRecheckUntilPublication) {
    for (const bool due_publication : {false, true}) {
        SCOPED_TRACE(due_publication);
        // An empty anchor set uses the origin. A second nearby anchor and an
        // unchanged pose still demand LOD0 and must not submit terrain work.
        for (const int anchor_case : {0, 1, 2, 3, 4}) {
            SCOPED_TRACE(anchor_case);
            JobSystem jobs;
            jobs.startup(1);
            auto params = Luminumbra::Test::SyntheticTerrainParams();
            params.base_amplitude = 0.0f;
            params.height_offset = 8.0f;
            params.caves_enabled = false;
            SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
            WaterSystem water(&jobs, &world);
            water.SetBootPaused(true);
            world.SetWaterSystem(&water);
            auto chunk = std::make_shared<Chunk>(IVec3(0));
            FillDugPond(world, *chunk, 8);
            World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 1);
            chunk->current_lod.store(0);
            chunk->set_state(ChunkState::Ready);
            ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
            if (anchor_case == 2) {
                auto unrelated = std::make_shared<Chunk>(IVec3(1, 0, 0));
                FillDugPond(world, *unrelated, 8);
                World::MarchingCubes::PolygoniseTerrain(world, *unrelated, 0.0f, 1);
                unrelated->current_lod.store(0);
                unrelated->set_state(ChunkState::Ready);
                unrelated->has_water_sim.store(false);
                ASSERT_TRUE(world.adopt_streamed_chunk(unrelated));
            }
            PendingMeshGate gate(jobs, world);
            if (due_publication) {
                world.begin_tick(0);
            }
            entt::registry registry;
            const Vec3 near_anchor(8.0f, 6.0f, 8.0f);
            const Vec3 far_anchor(-256.0f, 6.0f, 0.0f);
            world.update(registry, near_anchor, nullptr, false);
            ASSERT_EQ(world.get_last_streaming_budget_stats().submitted_meshing, 1u);
            ASSERT_EQ(chunk->pending_lod.load(), 0);
            ASSERT_TRUE(water.mesh_invalidations().contains(chunk->get_id()));

            std::vector<Vec3> anchors{far_anchor};
            if (anchor_case == 1) {
                anchors = {near_anchor};
            } else if (anchor_case == 2) {
                anchors.clear();
            } else if (anchor_case == 3) {
                anchors.push_back(near_anchor);
            }
            world.update(registry, anchors, nullptr, false);
            EXPECT_EQ(world.get_last_streaming_budget_stats().submitted_meshing, 0u);
            EXPECT_TRUE(water.mesh_invalidations().contains(chunk->get_id()));
            if (anchor_case == 2) {
                // No terrain obligation: even with a worker pending, the next
                // indexed pass must not revisit the unrelated resident.
                const auto hits = world.activation_index_counters().span_cache_hits;
                world.update(registry, anchors, nullptr, false);
                EXPECT_EQ(world.activation_index_counters().span_cache_hits, hits);
                EXPECT_EQ(world.get_last_streaming_budget_stats().submitted_meshing, 0u);
            } else if (anchor_case == 4) {
                // Returning to the origin before publication cancels the terrain
                // obligation that the preceding distant-anchor pass retained.
                anchors.clear();
                world.update(registry, anchors, nullptr, false);
                EXPECT_EQ(world.get_last_streaming_budget_stats().submitted_meshing, 0u);
            }
            if (due_publication) {
                gate.release();
                world.quiesce_streaming_jobs_for_save();
                world.activate_due(7);
                EXPECT_EQ(chunk->pending_lod.load(), 0);
                world.begin_tick(8);
                world.activate_due(8);
            } else {
                gate.finish();
            }
            ASSERT_EQ(chunk->pending_lod.load(), -1);
            ASSERT_TRUE(chunk->water_mesh_generated.load());
            ASSERT_EQ(chunk->current_lod.load(), 0);
            ASSERT_EQ(chunk->mesh_version.load(), 0u);

            // No further anchor motion or water mutation can wake this check.
            world.update(registry, anchors, nullptr, false);
            const bool demote = anchor_case == 0;
            EXPECT_EQ(world.get_last_streaming_budget_stats().submitted_meshing, demote ? 1u : 0u);
            world.wait_for_streaming_jobs();
            EXPECT_EQ(chunk->current_lod.load(), demote ? 1 : 0);
            EXPECT_EQ(chunk->mesh_version.load(), demote ? 1u : 0u);
        }
    }
}

enum class SupersedingChange : std::uint8_t {
    TerrainMesh,
    WaterMesh,
    VoxelEdit
};

TEST(AsyncWaterMeshing, SupersedingWaterMeshCannotRetirePendingTerrainRecheck) {
    JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 8.0f;
    params.caves_enabled = false;
    SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    WaterSystem water(&jobs, &world);
    water.SetBootPaused(true);
    world.SetWaterSystem(&water);
    auto chunk = std::make_shared<Chunk>(IVec3(0));
    FillDugPond(world, *chunk, 8);
    World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 1);
    chunk->current_lod.store(0);
    chunk->set_state(ChunkState::Ready);
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    PendingMeshGate gate(jobs, world);
    entt::registry registry;
    const Vec3 anchor(8.0f, 6.0f, 8.0f);
    world.update(registry, anchor, nullptr, false);
    ASSERT_EQ(chunk->pending_lod.load(), 0);

    // A newer synchronous water mesh satisfies water, while a voxel edit
    // invalidates terrain. The queued water-only snapshot still owns a recheck.
    chunk->mark_voxel_data_dirty();
    chunk->current_lod.store(-1);
    World::MarchingCubes::GenerateWaterMesh(water, world, *chunk);
    ++chunk->water_mesh_version;
    ASSERT_TRUE(chunk->water_mesh_generated.load());
    const auto water_version = chunk->water_mesh_version.load();
    world.update(registry, anchor, nullptr, false);
    EXPECT_TRUE(water.mesh_invalidations().contains(chunk->get_id()));
    EXPECT_EQ(world.get_last_streaming_budget_stats().submitted_meshing, 0u);
    gate.finish();
    EXPECT_EQ(chunk->water_mesh_version.load(), water_version);
    EXPECT_EQ(chunk->current_lod.load(), -1);

    world.update(registry, anchor, nullptr, false);
    EXPECT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 1u);
    world.wait_for_streaming_jobs();
    EXPECT_EQ(chunk->current_lod.load(), 0);
    EXPECT_EQ(chunk->mesh_version.load(), 1u);
}

TEST(AsyncWaterMeshing, StaleResultsCannotReplaceNewerMeshesOrVoxelEdits) {
    for (const bool due_publication : {false, true}) {
        SCOPED_TRACE(due_publication);
        for (const auto request :
             {MeshRequest::Initial, MeshRequest::TerrainRemesh, MeshRequest::WaterOnly}) {
            SCOPED_TRACE(static_cast<int>(request));
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
                water.SetBootPaused(true);
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
                if (due_publication) {
                    world.begin_tick(0);
                }
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
                chunk->has_collision.store(true);
                chunk->water_mesh_dirty_ticks = 5;
                if (due_publication) {
                    gate.release();
                    world.quiesce_streaming_jobs_for_save();
                    world.activate_due(7);
                    EXPECT_EQ(chunk->mesh_version.load(), terrain_version);
                    EXPECT_EQ(chunk->water_mesh_version.load(), water_version);
                    EXPECT_EQ(chunk->pending_lod.load(), 0);
                    world.begin_tick(8);
                    world.activate_due(8);
                } else {
                    gate.finish();
                }
                const bool terrain_published =
                    request != MeshRequest::WaterOnly && change == SupersedingChange::WaterMesh;
                const bool water_published =
                    request == MeshRequest::WaterOnly && change != SupersedingChange::WaterMesh;
                EXPECT_EQ(chunk->mesh_version.load(),
                          terrain_version + (terrain_published ? 1u : 0u));
                EXPECT_EQ(chunk->water_mesh_version.load(),
                          water_version + (water_published ? 1u : 0u));
                EXPECT_EQ(chunk->has_collision.load(), !terrain_published);
                EXPECT_EQ(chunk->water_mesh_dirty_ticks, water_published ? 0 : 5);
                EXPECT_FALSE(chunk->pending_water_mesh_generated.load());
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
    }
}

void CheckQueuedVoxelEdit(MeshRequest request, bool due_publication, bool fill, bool change_water) {
    SCOPED_TRACE(fill);
    SCOPED_TRACE(change_water);
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
    const auto dispatched = ReferenceMesh(world, water, *chunk);
    if (request != MeshRequest::Initial) {
        chunk->mesh_vertices = dispatched->mesh_vertices;
        chunk->mesh_indices = dispatched->mesh_indices;
        chunk->current_lod.store(request == MeshRequest::WaterOnly ? 0 : 1);
        chunk->set_state(ChunkState::Ready);
    }
    const auto retained_vertices = chunk->mesh_vertices;
    const auto retained_indices = chunk->mesh_indices;
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    PendingMeshGate gate(jobs, world);
    if (due_publication) {
        world.begin_tick(0);
    }
    entt::registry registry;
    world.update(registry, std::vector<Vec3>{}, nullptr);
    ASSERT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 1u);
    ASSERT_EQ(chunk->pending_lod.load(), 0);
    const auto dispatched_revision = chunk->voxel_revision();
    ASSERT_EQ(world.EditTerrainVoxel(Vec3(8.0f, 4.0f, 8.0f), 1.0f, fill, nullptr), 1);
    ASSERT_GT(chunk->voxel_revision(), dispatched_revision);
    ASSERT_EQ(chunk->current_lod.load(), -1);
    if (change_water) {
        chunk->water_level_data.front() += 1.0f;
        chunk->water_depth_mm.front() = 2000;
    }
    const auto expected = ReferenceMesh(world, water, *chunk);
    const auto edited_sdf = chunk->sdf_data;
    const auto edited_bed = chunk->water_bed_mm;
    const auto edited_depth = chunk->water_depth_mm;
    const auto edited_levels = chunk->water_level_data;
    if (due_publication) {
        gate.release();
        world.quiesce_streaming_jobs_for_save();
        world.activate_due(7);
        EXPECT_EQ(chunk->pending_lod.load(), 0);
        EXPECT_EQ(chunk->water_mesh_version.load(), 0u);
        world.begin_tick(8);
        world.activate_due(8);
    } else {
        gate.finish();
    }
    EXPECT_EQ(chunk->current_lod.load(), -1);
    EXPECT_EQ(chunk->pending_lod.load(), -1);
    EXPECT_EQ(chunk->mesh_version.load(), 0u);
    ExpectVerticesEqual(chunk->mesh_vertices, retained_vertices);
    EXPECT_EQ(chunk->mesh_indices, retained_indices);
    const bool water_only = request == MeshRequest::WaterOnly;
    EXPECT_EQ(chunk->get_state(),
              water_only || request == MeshRequest::TerrainRemesh ? ChunkState::Ready
                                                                  : ChunkState::Idle);
    EXPECT_EQ(chunk->water_mesh_version.load(), water_only ? 1u : 0u);
    if (water_only) {
        ExpectVerticesEqual(chunk->water_mesh_vertices, dispatched->water_mesh_vertices);
        EXPECT_EQ(chunk->water_mesh_indices, dispatched->water_mesh_indices);
        EXPECT_EQ(chunk->water_mesh_generated.load(), !change_water);
    } else {
        EXPECT_TRUE(chunk->water_mesh_vertices.empty());
        EXPECT_TRUE(chunk->water_mesh_indices.empty());
    }
    EXPECT_EQ(chunk->sdf_data, edited_sdf);
    EXPECT_EQ(chunk->water_bed_mm, edited_bed);
    EXPECT_EQ(chunk->water_depth_mm, edited_depth);
    EXPECT_EQ(chunk->water_level_data, edited_levels);
    EXPECT_EQ(chunk->voxel_revision(), dispatched_revision + 1u);
    world.update(registry, std::vector<Vec3>{}, nullptr);
    EXPECT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 1u);
    EXPECT_LE(world.last_meshing_snapshot_bytes(), SHIELD_WorldSystem::kMeshingSnapshotByteBudget);
    world.wait_for_streaming_jobs();
    EXPECT_EQ(chunk->current_lod.load(), 0);
    EXPECT_EQ(chunk->mesh_version.load(), 1u);
    EXPECT_EQ(chunk->water_mesh_version.load(), water_only ? 2u : 1u);
    ExpectVerticesEqual(chunk->mesh_vertices, expected->mesh_vertices);
    EXPECT_EQ(chunk->mesh_indices, expected->mesh_indices);
    ExpectVerticesEqual(chunk->water_mesh_vertices, expected->water_mesh_vertices);
    EXPECT_EQ(chunk->water_mesh_indices, expected->water_mesh_indices);
    EXPECT_EQ(chunk->sdf_data, edited_sdf);
}

void CheckQueuedVoxelEdits(MeshRequest request, bool due_publication) {
    for (const bool fill : {false, true}) {
        for (const bool change_water : {false, true}) {
            CheckQueuedVoxelEdit(request, due_publication, fill, change_water);
        }
    }
}

TEST(AsyncWaterMeshing, QueuedTerrainSnapshotsRetainCarveAndFillInvalidation) {
    CheckQueuedVoxelEdits(MeshRequest::Initial, false);
    CheckQueuedVoxelEdits(MeshRequest::TerrainRemesh, false);
}

TEST(AsyncWaterMeshing, QueuedWaterOnlySnapshotsRemainIndependentOfVoxelEdits) {
    CheckQueuedVoxelEdits(MeshRequest::WaterOnly, false);
}

TEST(AsyncWaterMeshing, DueSnapshotsRetainVoxelInvalidationAndIndependentWater) {
    CheckQueuedVoxelEdits(MeshRequest::Initial, true);
    CheckQueuedVoxelEdits(MeshRequest::TerrainRemesh, true);
    CheckQueuedVoxelEdits(MeshRequest::WaterOnly, true);
}

TEST(AsyncWaterMeshing, SynchronousEditPreparationRejectsCompletedTerrainSnapshot) {
    for (const bool fill : {false, true}) {
        SCOPED_TRACE(fill);
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
        auto expected = ReferenceMesh(world, water, *chunk);
        chunk->mesh_vertices = expected->mesh_vertices;
        chunk->mesh_indices = expected->mesh_indices;
        chunk->current_lod.store(1);
        chunk->set_state(ChunkState::Ready);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        PendingMeshGate gate(jobs, world);
        entt::registry registry;
        world.update(registry, std::vector<Vec3>{}, nullptr);
        ASSERT_EQ(chunk->pending_lod.load(), 0);
        gate.release();
        // Fence worker writes without publishing: EditTerrainVoxel must reject
        // the old completion before its synchronous preparation tests the LOD.
        world.quiesce_streaming_jobs_for_save();
        ASSERT_TRUE(chunk->pending_mesh_ready.load());
        const auto old_revision = chunk->voxel_revision();
        PhysicsSystem physics;
        physics.startup();
        ASSERT_EQ(world.EditTerrainVoxel(Vec3(8.0f, 4.0f, 8.0f), 1.0f, fill, &physics), 1);
        expected = ReferenceMesh(world, water, *chunk);
        EXPECT_EQ(chunk->voxel_revision(), old_revision + 1u);
        EXPECT_EQ(chunk->current_lod.load(), 0);
        EXPECT_EQ(chunk->pending_lod.load(), -1);
        EXPECT_EQ(chunk->mesh_version.load(), 1u);
        EXPECT_EQ(chunk->water_mesh_version.load(), 0u);
        ExpectVerticesEqual(chunk->mesh_vertices, expected->mesh_vertices);
        EXPECT_EQ(chunk->mesh_indices, expected->mesh_indices);
    }
}

TEST(AsyncWaterMeshing, WaterOnlySelectionTracksPublicationInBothDirections) {
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
        auto chunk = std::make_shared<Chunk>(IVec3(0));
        FillDugPond(world, *chunk, 8);
        const auto terrain = chunk->sdf_data;
        const auto wet_depth = chunk->water_depth_mm;
        chunk->sdf_data.assign(terrain.size(), 1.0f);
        chunk->current_lod.store(0);
        chunk->set_state(ChunkState::Ready);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        ASSERT_TRUE(world.renderable_chunks_view().empty());
        entt::registry registry;
        bool previously_wet = false;
        for (const bool wet : {true, true, false, false, true, false}) {
            SCOPED_TRACE(wet);
            const auto before = world.renderable_view_work();
            chunk->water_depth_mm = wet ? wet_depth : std::vector<std::int32_t>(64u, 0);
            chunk->water_mesh_generated.store(false);
            water.retain_mesh_invalidation(chunk->get_id());
            world.update(registry, std::vector<Vec3>{}, nullptr);
            world.wait_for_streaming_jobs();
            ASSERT_EQ(chunk->get_state(), ChunkState::Ready);
            ASSERT_TRUE(chunk->mesh_vertices.empty());
            EXPECT_EQ(chunk->water_mesh_vertices.empty(), !wet);
            EXPECT_EQ(world.renderable_chunks_view(),
                      wet ? std::vector<Chunk*>{chunk.get()} : std::vector<Chunk*>{});
            const auto after = world.renderable_view_work();
            EXPECT_EQ(after.rebuilds - before.rebuilds, wet != previously_wet ? 1u : 0u);
            EXPECT_EQ(after.allocations, before.allocations);
            previously_wet = wet;
        }
        chunk->sdf_data = terrain;
        World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 1);
        ASSERT_FALSE(chunk->mesh_vertices.empty());
        EXPECT_EQ(world.renderable_chunks_view(), std::vector<Chunk*>{chunk.get()});
        chunk->water_depth_mm = wet_depth;
        World::MarchingCubes::GenerateWaterMesh(water, world, *chunk);
        EXPECT_EQ(world.renderable_chunks_view(), std::vector<Chunk*>{chunk.get()});
        chunk->sdf_data.assign(terrain.size(), 1.0f);
        World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 1);
        EXPECT_EQ(world.renderable_chunks_view(), std::vector<Chunk*>{chunk.get()});
    }
}

enum class DryWaterExecution : std::uint8_t {
    Inline,
    Worker,
    Rejected
};

void CheckDryWaterOnlyDispatch(DryWaterExecution execution,
                               int resolution,
                               std::size_t surface_cells) {
    SCOPED_TRACE(static_cast<int>(execution));
    SCOPED_TRACE(resolution);
    SCOPED_TRACE(surface_cells);
    JobSystem jobs;
    if (execution == DryWaterExecution::Worker) {
        jobs.startup(1);
    }
    auto* job_system = execution == DryWaterExecution::Inline ? nullptr : &jobs;
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 8.0f;
    params.caves_enabled = false;
    SHIELD_WorldSystem world(job_system, nullptr, params, 1337);
    WaterSystem water(job_system, &world);
    water.SetBootPaused(true);
    world.SetWaterSystem(&water);
    auto chunk = std::make_shared<Chunk>(IVec3(0));
    FillDugPond(world, *chunk, 8);
    chunk->sdf_data.assign(chunk->sdf_data.size(), 1.0f);
    chunk->water_depth_mm.assign(64u, 0);
    chunk->water_level_data.resize(surface_cells);
    chunk->current_water_resolution.store(resolution);
    chunk->current_lod.store(0);
    chunk->set_state(ChunkState::Ready);
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    ASSERT_TRUE(world.renderable_chunks_view().empty());
    const auto before = world.renderable_view_work();
    world.begin_tick(0);
    entt::registry registry;
    world.update(registry, std::vector<Vec3>{}, nullptr);
    ASSERT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 1u);
    ASSERT_EQ(chunk->pending_lod.load(), 0);
    world.quiesce_streaming_jobs_for_save();
    EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
    EXPECT_TRUE(world.renderable_chunks_view().empty());
    EXPECT_EQ(world.renderable_view_work().rebuilds, before.rebuilds);
    EXPECT_EQ(chunk->mesh_version.load(), 0u);
    EXPECT_EQ(chunk->water_mesh_version.load(), 0u);
    world.activate_due(7);
    EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
    EXPECT_EQ(chunk->pending_lod.load(), 0);
    EXPECT_EQ(chunk->water_mesh_version.load(), 0u);
    world.activate_due(8);
    const bool rejected = execution == DryWaterExecution::Rejected;
    // Refused jobs still return to Idle for the scheduler to retry.
    EXPECT_EQ(chunk->get_state(), rejected ? ChunkState::Idle : ChunkState::Ready);
    EXPECT_EQ(chunk->pending_lod.load(), -1);
    EXPECT_EQ(chunk->current_lod.load(), 0);
    EXPECT_EQ(chunk->mesh_version.load(), 0u);
    EXPECT_EQ(chunk->water_mesh_version.load(), rejected ? 0u : 1u);
    EXPECT_EQ(chunk->water_mesh_generated.load(),
              !rejected && resolution == 8 && surface_cells == 64u);
    EXPECT_TRUE(chunk->mesh_vertices.empty());
    EXPECT_TRUE(chunk->mesh_indices.empty());
    EXPECT_TRUE(chunk->water_mesh_vertices.empty());
    EXPECT_TRUE(chunk->water_mesh_indices.empty());
    EXPECT_TRUE(world.renderable_chunks_view().empty());
    const auto after = world.renderable_view_work();
    EXPECT_EQ(after.rebuilds - before.rebuilds, rejected ? 1u : 0u);
    EXPECT_EQ(after.chunk_visits - before.chunk_visits, rejected ? 1u : 0u);
    EXPECT_EQ(after.allocations, before.allocations);
}

TEST(AsyncWaterMeshing, DryWaterOnlyDispatchPreservesReadyUntilDuePublication) {
    for (const auto execution :
         {DryWaterExecution::Inline, DryWaterExecution::Worker, DryWaterExecution::Rejected}) {
        for (const int resolution : {0, 8, 33}) {
            for (const std::size_t surface_cells : {0u, 63u, 64u}) {
                CheckDryWaterOnlyDispatch(execution, resolution, surface_cells);
            }
        }
    }
}

TEST(AsyncWaterMeshing, HeightmapWaterDepartureInvalidatesReadyWaterOnlySelection) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 8.0f;
    params.caves_enabled = false;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    WaterSystem water(nullptr, &world);
    water.SetBootPaused(true);
    world.SetWaterSystem(&water);
    auto chunk = std::make_shared<Chunk>(IVec3(20, 0, 0));
    FillDugPond(world, *chunk, 8);
    chunk->sdf_data.clear();
    chunk->current_lod.store(1);
    World::MarchingCubes::GenerateWaterMesh(water, world, *chunk);
    ASSERT_FALSE(chunk->water_mesh_vertices.empty());
    chunk->set_state(ChunkState::Ready);
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    ASSERT_EQ(world.renderable_chunks_view(), std::vector<Chunk*>{chunk.get()});
    entt::registry registry;
    world.begin_tick(0);
    world.update(registry, std::vector<Vec3>{}, nullptr, false);
    EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
    EXPECT_EQ(chunk->current_lod.load(), 1);
    EXPECT_TRUE(chunk->water_mesh_vertices.empty());
    EXPECT_TRUE(chunk->water_mesh_indices.empty());
    EXPECT_TRUE(world.renderable_chunks_view().empty());
}

TEST(AsyncWaterMeshing, DirectWaterSelectionInvalidatesForDryAndInvalidGrids) {
    SHIELD_WorldSystem world(nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    WaterSystem water(nullptr, &world);
    auto chunk = std::make_shared<Chunk>(IVec3(0));
    FillDugPond(world, *chunk, 8);
    const auto wet_depth = chunk->water_depth_mm;
    chunk->set_state(ChunkState::Ready);
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    ASSERT_TRUE(world.renderable_chunks_view().empty());
    for (const int resolution : {0, 1, 8, 33}) {
        SCOPED_TRACE(resolution);
        chunk->current_water_resolution.store(8);
        chunk->water_depth_mm = wet_depth;
        World::MarchingCubes::GenerateWaterMesh(water, world, *chunk);
        ASSERT_EQ(world.renderable_chunks_view(), std::vector<Chunk*>{chunk.get()});
        chunk->current_water_resolution.store(resolution);
        chunk->water_depth_mm.assign(64u, 0);
        World::MarchingCubes::GenerateWaterMesh(water, world, *chunk);
        EXPECT_TRUE(world.renderable_chunks_view().empty());
    }
    for (const std::size_t cells : {0u, 63u}) {
        SCOPED_TRACE(cells);
        chunk->current_water_resolution.store(8);
        chunk->water_level_data.assign(64u, 6.0f);
        chunk->water_depth_mm = wet_depth;
        World::MarchingCubes::GenerateWaterMesh(water, world, *chunk);
        ASSERT_EQ(world.renderable_chunks_view(), std::vector<Chunk*>{chunk.get()});
        chunk->water_level_data.resize(cells);
        World::MarchingCubes::GenerateWaterMesh(water, world, *chunk);
        EXPECT_TRUE(world.renderable_chunks_view().empty());
    }
}

TEST(AsyncWaterMeshing, WaterOnlyCompletionDoesNotRequireTerrainGeometryOrCurrentLod) {
    for (const bool due_publication : {false, true}) {
        SCOPED_TRACE(due_publication);
        JobSystem jobs;
        jobs.startup(1);
        auto params = Luminumbra::Test::SyntheticTerrainParams();
        params.base_amplitude = 0.0f;
        params.height_offset = 8.0f;
        params.caves_enabled = false;
        SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
        WaterSystem water(&jobs, &world);
        water.SetBootPaused(true);
        world.SetWaterSystem(&water);
        auto chunk = std::make_shared<Chunk>(IVec3(0));
        const auto wet_cells = FillDugPond(world, *chunk, 8);
        chunk->sdf_data.assign(chunk->sdf_data.size(), 1.0f);
        chunk->current_lod.store(0);
        chunk->set_state(ChunkState::Ready);
        ASSERT_TRUE(chunk->mesh_vertices.empty());
        const auto expected = ReferenceMesh(world, water, *chunk);
        ASSERT_TRUE(expected->mesh_vertices.empty());
        ASSERT_EQ(expected->water_mesh_vertices.size(), wet_cells * 4u);
        ASSERT_EQ(expected->water_mesh_indices.size(), wet_cells * 6u);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        PendingMeshGate gate(jobs, world);
        if (due_publication) {
            world.begin_tick(0);
        }
        entt::registry registry;
        world.update(registry, std::vector<Vec3>{}, nullptr);
        ASSERT_EQ(chunk->pending_lod.load(), 0);
        chunk->sdf_data = {1.0f};
        chunk->mark_voxel_data_dirty();
        chunk->current_lod.store(-1);
        chunk->mesh_version.store(7u);
        chunk->has_collision.store(true);
        if (due_publication) {
            gate.release();
            world.quiesce_streaming_jobs_for_save();
            EXPECT_TRUE(chunk->pending_mesh_vertices.empty());
            world.activate_due(7);
            EXPECT_EQ(chunk->water_mesh_version.load(), 0u);
            EXPECT_EQ(chunk->pending_lod.load(), 0);
            world.activate_due(8);
        } else {
            gate.finish();
        }
        EXPECT_EQ(chunk->water_mesh_version.load(), 1u);
        EXPECT_TRUE(chunk->water_mesh_generated.load());
        ExpectVerticesEqual(chunk->water_mesh_vertices, expected->water_mesh_vertices);
        EXPECT_EQ(chunk->water_mesh_indices, expected->water_mesh_indices);
        EXPECT_TRUE(chunk->mesh_vertices.empty());
        EXPECT_TRUE(chunk->mesh_indices.empty());
        EXPECT_EQ(chunk->mesh_version.load(), 7u);
        EXPECT_EQ(chunk->current_lod.load(), -1);
        EXPECT_TRUE(chunk->has_collision.load());
        EXPECT_EQ(chunk->sdf_data, std::vector<float>{1.0f});
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        EXPECT_EQ(chunk->pending_lod.load(), -1);
        EXPECT_FALSE(chunk->pending_water_mesh_generated.load());
        EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
    }
}

TEST(AsyncWaterMeshing, SynchronousEditPreparationInvalidatesPublishedWaterOnlySnapshot) {
    for (const bool fill : {false, true}) {
        SCOPED_TRACE(fill);
        JobSystem jobs;
        jobs.startup(static_cast<std::size_t>(1));
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
        const auto dispatched = ReferenceMesh(world, water, *chunk);
        ASSERT_FALSE(dispatched->water_mesh_indices.empty());
        chunk->mesh_vertices = dispatched->mesh_vertices;
        chunk->mesh_indices = dispatched->mesh_indices;
        chunk->current_lod.store(0);
        chunk->set_state(ChunkState::Ready);
        const auto saved_bed = chunk->water_bed_mm;
        const auto saved_depth = chunk->water_depth_mm;
        const auto saved_levels = chunk->water_level_data;
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        PendingMeshGate gate(jobs, world);
        entt::registry registry;
        world.update(registry, std::vector<Vec3>{}, nullptr);
        ASSERT_EQ(chunk->pending_lod.load(), 0);
        gate.release();
        world.quiesce_streaming_jobs_for_save();
        ASSERT_TRUE(chunk->pending_mesh_ready.load());
        ASSERT_TRUE(chunk->pending_mesh_vertices.empty());
        ASSERT_FALSE(chunk->pending_water_mesh_indices.empty());
        ASSERT_EQ(chunk->water_mesh_version.load(), 0u);
        const auto old_revision = chunk->voxel_revision();
        PhysicsSystem physics;
        physics.startup();
        // The drain accepts the water-only result before rebuilding terrain.
        // At resolution 8, this 1 m brush misses every water-cell centre, so
        // the bed edit cannot invalidate the flag after the rebuild clears the geometry.
        ASSERT_EQ(world.EditTerrainVoxel(Vec3(8.0f, 4.0f, 8.0f), 1.0f, fill, &physics), 1);
        EXPECT_EQ(chunk->voxel_revision(), old_revision + 1u);
        EXPECT_EQ(chunk->current_lod.load(), 0);
        EXPECT_EQ(chunk->pending_lod.load(), -1);
        EXPECT_EQ(chunk->mesh_version.load(), 1u);
        EXPECT_EQ(chunk->water_mesh_version.load(), 1u);
        EXPECT_TRUE(chunk->water_mesh_vertices.empty());
        EXPECT_TRUE(chunk->water_mesh_indices.empty());
        EXPECT_FALSE(chunk->water_mesh_generated.load());
        EXPECT_EQ(chunk->water_bed_mm, saved_bed);
        EXPECT_EQ(chunk->water_depth_mm, saved_depth);
        EXPECT_EQ(chunk->water_level_data, saved_levels);
        const auto expected = ReferenceMesh(world, water, *chunk);
        ExpectVerticesEqual(chunk->mesh_vertices, expected->mesh_vertices);
        EXPECT_EQ(chunk->mesh_indices, expected->mesh_indices);
    }
}

TEST(AsyncWaterMeshing, RejectedDispatchRetainsVoxelInvalidationWithoutPublishingWater) {
    for (const bool water_only : {false, true}) {
        SCOPED_TRACE(water_only);
        JobSystem jobs;
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
        if (water_only) {
            const auto current = ReferenceMesh(world, water, *chunk);
            chunk->mesh_vertices = current->mesh_vertices;
            chunk->mesh_indices = current->mesh_indices;
            chunk->current_lod.store(0);
            chunk->set_state(ChunkState::Ready);
        }
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        world.begin_tick(0);
        entt::registry registry;
        world.update(registry, std::vector<Vec3>{}, nullptr);
        ASSERT_EQ(chunk->pending_lod.load(), 0);
        ASSERT_EQ(world.EditTerrainVoxel(Vec3(8.0f, 4.0f, 8.0f), 1.0f, false, nullptr), 1);
        const auto expected = ReferenceMesh(world, water, *chunk);
        world.wait_for_streaming_jobs();
        EXPECT_EQ(chunk->pending_lod.load(), -1);
        EXPECT_EQ(chunk->current_lod.load(), -1);
        EXPECT_EQ(chunk->mesh_version.load(), 0u);
        EXPECT_EQ(chunk->water_mesh_version.load(), 0u);
        EXPECT_TRUE(chunk->water_mesh_vertices.empty());
        EXPECT_FALSE(chunk->pending_mesh_ready.load());
        jobs.startup(1);
        world.update(registry, std::vector<Vec3>{}, nullptr);
        world.wait_for_streaming_jobs();
        EXPECT_EQ(chunk->current_lod.load(), 0);
        EXPECT_EQ(chunk->mesh_version.load(), 1u);
        ExpectVerticesEqual(chunk->mesh_vertices, expected->mesh_vertices);
        EXPECT_EQ(chunk->mesh_indices, expected->mesh_indices);
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
        chunk.water_level_data.assign(resolution == 8 ? 63u : (resolution == 1 ? 1u : 0u), 6.0f);
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
        world.wait_for_admitted_streaming_jobs();
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

enum class Settlement : std::uint8_t {
    FullDrain,
    DueTick,
    SurfaceReadiness,
    Clear,
    Regenerate,
    Seed,
    Params
};

void CheckSynchronousSettlement(Settlement settlement) {
    for (const bool use_workers : {false, true}) {
        for (const bool promotion : {false, true}) {
            SCOPED_TRACE(use_workers);
            SCOPED_TRACE(promotion);
            JobSystem jobs;
            if (use_workers) {
                jobs.startup(1);
            }
            auto params = Luminumbra::Test::SyntheticTerrainParams();
            params.base_amplitude = 0.0f;
            params.height_offset = 8.0f;
            params.caves_enabled = false;
            PhysicsSystem physics;
            if (settlement == Settlement::SurfaceReadiness) {
                physics.startup();
            }
            SHIELD_WorldSystem world(use_workers ? &jobs : nullptr, nullptr, params, 1337);
            WaterSystem water(use_workers ? &jobs : nullptr, &world);
            water.SetBootPaused(true);
            world.SetWaterSystem(&water);
            world.begin_tick(0);
            BudgetedBurstMeshes burst;
            ASSERT_NO_FATAL_FAILURE(PrepareBudgetedBurst(world, water, promotion, burst));
            entt::registry registry;
            world.update(registry, std::vector<Vec3>{}, nullptr);
            EXPECT_GT(world.get_last_streaming_budget_stats().deferred_meshing, 0u);
            EXPECT_LE(world.last_meshing_snapshot_bytes(),
                      SHIELD_WorldSystem::kMeshingSnapshotByteBudget);
            // Raw save fences and early activation must retain all deferred work
            // without promoting it. The original dispatch tick owns every suffix.
            world.quiesce_streaming_jobs_for_save();
            world.activate_due(7);
            for (const auto& chunk : burst.chunks) {
                EXPECT_EQ(chunk->mesh_version.load(), 0u);
            }
            switch (settlement) {
                case Settlement::FullDrain:
                    world.wait_for_streaming_jobs();
                    break;
                case Settlement::DueTick:
                    world.begin_tick(8);
                    world.activate_due(8);
                    break;
                case Settlement::SurfaceReadiness:
                    ASSERT_TRUE(world.EnsureSurfaceReadyNear(
                        Vec3(10008.0f, 10.0f, 10008.0f), &physics, 0, 0));
                    break;
                case Settlement::Clear:
                    world.clear_world(nullptr);
                    break;
                case Settlement::Regenerate:
                    world.regenerate_all_chunks(nullptr);
                    break;
                case Settlement::Seed:
                    world.set_seed(7331);
                    break;
                case Settlement::Params:
                    params.height_offset = 12.0f;
                    world.set_params(params);
                    break;
            }
            EXPECT_EQ(CheckCompletedBurst(burst), kBurstChunks);
            if (promotion) {
                EXPECT_EQ(world.promotion_dispatch_totals().chunks, kBurstChunks);
            }
            if (settlement == Settlement::Clear || settlement == Settlement::Regenerate) {
                for (const auto& chunk : burst.chunks) {
                    EXPECT_NE(world.find_streamed_chunk(chunk->get_coords()), chunk);
                }
            }
            world.wait_for_streaming_jobs();
            EXPECT_FALSE(world.get_runtime_chunk_stats().meshing_job_active);
            if (settlement == Settlement::Regenerate) {
                for (const auto& chunk : burst.chunks) {
                    const auto regenerated = world.find_streamed_chunk(chunk->get_coords());
                    ASSERT_NE(regenerated, nullptr);
                    EXPECT_EQ(regenerated->get_state(), ChunkState::Idle);
                    EXPECT_FALSE(regenerated->sdf_data.empty());
                }
            }
        }
    }
}

TEST(AsyncWaterMeshing, BlockingDrainSettlesBudgetDeferredRequests) {
    CheckSynchronousSettlement(Settlement::FullDrain);
}
TEST(AsyncWaterMeshing, DueActivationSettlesBudgetDeferredRequestsAtOriginalTick) {
    CheckSynchronousSettlement(Settlement::DueTick);
}
TEST(AsyncWaterMeshing, SurfaceReadinessSettlesBudgetDeferredRequests) {
    CheckSynchronousSettlement(Settlement::SurfaceReadiness);
}
TEST(AsyncWaterMeshing, ClearSettlesDeferredRequestsWithoutResurrection) {
    CheckSynchronousSettlement(Settlement::Clear);
}
TEST(AsyncWaterMeshing, RegenerateSettlesDeferredRequestsWithoutResurrection) {
    CheckSynchronousSettlement(Settlement::Regenerate);
}
TEST(AsyncWaterMeshing, GeneratorReplacementSettlesBudgetDeferredRequests) {
    CheckSynchronousSettlement(Settlement::Seed);
    CheckSynchronousSettlement(Settlement::Params);
}

TEST(AsyncWaterMeshing, TickStampedGenerationWaitsForDueAcrossExecutionBackends) {
    for (const bool use_workers : {false, true}) {
        for (const int step : {1, 2}) {
            SCOPED_TRACE(use_workers);
            SCOPED_TRACE(step);
            JobSystem jobs;
            if (use_workers) {
                jobs.startup(1);
            }
            SHIELD_WorldSystem world(use_workers ? &jobs : nullptr,
                                     nullptr,
                                     Luminumbra::Test::SyntheticTerrainParams(),
                                     1337);
            world.begin_tick(0);
            const IVec3 coords{0, 0, 0};
            world.dispatch_generation_jobs(
                std::vector<SHIELD_WorldSystem::ChunkGenerationRequest>{{coords, step}});
            const auto chunk = world.find_streamed_chunk(coords);
            ASSERT_NE(chunk, nullptr);
            world.quiesce_streaming_jobs_for_save();
            EXPECT_TRUE(chunk->pending_generation_ready.load());
            EXPECT_EQ(chunk->get_state(), ChunkState::Loading);
            // Empty and already-generated requests must neither replace the
            // batch nor change the original tick-8 publication boundary.
            world.begin_tick(1);
            world.dispatch_generation_jobs(std::vector<IVec3>{});
            world.dispatch_generation_jobs({coords});
            EXPECT_EQ(world.find_streamed_chunk(coords), chunk);
            entt::registry registry;
            world.update(registry, std::vector<Vec3>{}, nullptr);
            EXPECT_EQ(chunk->get_state(), ChunkState::Loading);
            EXPECT_EQ(chunk->pending_lod.load(), -1);
            EXPECT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 0u);
            world.activate_due(7);
            EXPECT_EQ(chunk->get_state(), ChunkState::Loading);
            world.begin_tick(8);
            world.activate_due(8);
            EXPECT_EQ(chunk->get_state(), ChunkState::Idle);
            EXPECT_FALSE(chunk->pending_generation_ready.load());
            EXPECT_EQ(chunk->mesh_version.load(), 0u);
            // Meshing is a distinct dispatch, due eight ticks after this update.
            world.update(registry, std::vector<Vec3>{}, nullptr);
            world.quiesce_streaming_jobs_for_save();
            world.activate_due(15);
            EXPECT_EQ(chunk->mesh_version.load(), 0u);
            world.begin_tick(16);
            world.activate_due(16);
            EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
            EXPECT_EQ(chunk->mesh_version.load(), 1u);
            world.activate_due(16);
            world.wait_for_streaming_jobs();
            EXPECT_EQ(chunk->mesh_version.load(), 1u);
        }
    }
}

enum class ScheduledMeshInput : std::uint8_t {
    Initial,
    TerrainRemesh,
    WaterOnly,
    Promotion
};

TEST(AsyncWaterMeshing, TickStampedMeshesWaitForDueAcrossExecutionBackends) {
    for (const bool use_workers : {false, true}) {
        for (const auto input : {ScheduledMeshInput::Initial,
                                 ScheduledMeshInput::TerrainRemesh,
                                 ScheduledMeshInput::WaterOnly,
                                 ScheduledMeshInput::Promotion}) {
            SCOPED_TRACE(use_workers);
            SCOPED_TRACE(static_cast<int>(input));
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
            auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
            FillDugPond(world, *chunk, 8);
            if (input == ScheduledMeshInput::Promotion) {
                chunk->material_data.clear();
                world.GenerateChunkData(*chunk);
            }
            const auto expected = ReferenceMesh(world, water, *chunk);
            ASSERT_FALSE(expected->mesh_indices.empty());
            ASSERT_FALSE(expected->water_mesh_indices.empty());
            if (input != ScheduledMeshInput::Initial) {
                chunk->mesh_vertices = expected->mesh_vertices;
                chunk->mesh_indices = expected->mesh_indices;
                chunk->current_lod.store(input == ScheduledMeshInput::WaterOnly ? 0 : 1);
                chunk->set_state(ChunkState::Ready);
            }
            if (input == ScheduledMeshInput::Promotion) {
                chunk->sdf_data.clear();
            }
            ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
            world.begin_tick(0);
            entt::registry registry;
            world.update(registry, std::vector<Vec3>{}, nullptr);
            EXPECT_EQ(chunk->mesh_version.load(), 0u);
            EXPECT_EQ(chunk->water_mesh_version.load(), 0u);
            EXPECT_EQ(chunk->pending_lod.load(), 0);
            world.quiesce_streaming_jobs_for_save();
            if (input == ScheduledMeshInput::Promotion) {
                EXPECT_TRUE(chunk->sdf_data.empty());
                EXPECT_FALSE(chunk->pending_sdf_data.empty());
            }
            // A completed worker or inline job still belongs to dispatch tick 0.
            world.begin_tick(1);
            world.update(registry, std::vector<Vec3>{}, nullptr);
            world.activate_due(7);
            EXPECT_EQ(chunk->mesh_version.load(), 0u);
            EXPECT_EQ(chunk->water_mesh_version.load(), 0u);
            EXPECT_EQ(chunk->pending_lod.load(), 0);
            world.begin_tick(8);
            world.activate_due(8);
            const auto terrain_version = input == ScheduledMeshInput::WaterOnly ? 0u : 1u;
            EXPECT_EQ(chunk->mesh_version.load(), terrain_version);
            EXPECT_EQ(chunk->water_mesh_version.load(), 1u);
            EXPECT_EQ(chunk->current_lod.load(), 0);
            EXPECT_EQ(chunk->pending_lod.load(), -1);
            EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
            ExpectVerticesEqual(chunk->mesh_vertices, expected->mesh_vertices);
            EXPECT_EQ(chunk->mesh_indices, expected->mesh_indices);
            ExpectVerticesEqual(chunk->water_mesh_vertices, expected->water_mesh_vertices);
            EXPECT_EQ(chunk->water_mesh_indices, expected->water_mesh_indices);
            world.activate_due(8);
            world.wait_for_streaming_jobs();
            EXPECT_EQ(chunk->mesh_version.load(), terrain_version);
            EXPECT_EQ(chunk->water_mesh_version.load(), 1u);
        }
    }
}

TEST(AsyncWaterMeshing, UntickedInlineGenerationAndPromotionStillPublishOnCompletion) {
    for (const int step : {1, 2}) {
        SCOPED_TRACE(step);
        SHIELD_WorldSystem world(
            nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
        const IVec3 coords{0, 0, 0};
        world.dispatch_generation_jobs(
            std::vector<SHIELD_WorldSystem::ChunkGenerationRequest>{{coords, step}});
        const auto chunk = world.find_streamed_chunk(coords);
        ASSERT_NE(chunk, nullptr);
        EXPECT_EQ(chunk->get_state(), ChunkState::Idle);
        EXPECT_FALSE(chunk->pending_generation_ready.load());
        entt::registry registry;
        world.update(registry, std::vector<Vec3>{}, nullptr);
        EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
        EXPECT_EQ(chunk->mesh_version.load(), 1u);
        EXPECT_EQ(chunk->pending_lod.load(), -1);
        EXPECT_EQ(world.promotion_dispatch_totals().chunks, step == 2 ? 1u : 0u);
        world.activate_due(0);
        EXPECT_EQ(chunk->mesh_version.load(), 1u);
    }
}

TEST(AsyncWaterMeshing, WaterReplacementFencesAdmittedPromotionAndDeferredMeshes) {
    for (const bool promotion : {false, true}) {
        JobSystem jobs;
        jobs.startup(1);
        auto params = Luminumbra::Test::SyntheticTerrainParams();
        params.base_amplitude = 0.0f;
        params.height_offset = 8.0f;
        params.caves_enabled = false;
        SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
        auto water = std::make_unique<WaterSystem>(&jobs, &world);
        water->SetBootPaused(true);
        world.SetWaterSystem(water.get());
        world.begin_tick(0);
        BudgetedBurstMeshes burst;
        ASSERT_NO_FATAL_FAILURE(PrepareBudgetedBurst(world, *water, promotion, burst));
        entt::registry registry;
        world.update(registry, std::vector<Vec3>{}, nullptr);
        ASSERT_GT(world.get_last_streaming_budget_stats().deferred_meshing, 0u);
        // This fence must settle admitted stage A and B before the old water
        // owner can die. Deferred descriptors then use the replacement owner.
        world.SetWaterSystem(nullptr);
        water.reset();
        water = std::make_unique<WaterSystem>(&jobs, &world);
        water->SetBootPaused(true);
        world.SetWaterSystem(water.get());
        world.wait_for_streaming_jobs();
        EXPECT_EQ(CheckCompletedBurst(burst), kBurstChunks);
        world.SetWaterSystem(nullptr);
    }
}

// A queue barrier makes admission/refusal checks independent of worker timing.
// It owns no world reference and releases before the job-system owner tears down.
class QueuedWorkerGate {
public:
    explicit QueuedWorkerGate(JobSystem& jobs) {
        auto entered = std::make_shared<std::promise<void>>();
        auto started = entered->get_future();
        const auto release = m_release.get_future().share();
        jobs.dispatch([entered, release]() {
            entered->set_value();
            release.wait();
        });
        started.get();
    }
    ~QueuedWorkerGate() {
        release();
    }
    void release() {
        if (!m_released) {
            m_released = true;
            m_release.set_value();
        }
    }

private:
    std::promise<void> m_release;
    bool m_released = false;
};

TEST(AsyncWaterMeshing, UpdateSharesSnapshotBudgetWithPromotionHandoff) {
    JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 8.0f;
    params.caves_enabled = false;
    SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    WaterSystem water(&jobs, &world);
    water.SetBootPaused(true);
    world.SetWaterSystem(&water);
    BudgetedBurstMeshes burst;
    ASSERT_NO_FATAL_FAILURE(PrepareBudgetedBurst(world, water, true, burst));
    entt::registry registry;
    world.update(registry, std::vector<Vec3>{}, nullptr);
    world.quiesce_streaming_jobs_for_save();
    std::size_t promotions = 0;
    for (const auto& chunk : burst.chunks) {
        promotions += chunk->pending_lod.load() >= 0 ? 1u : 0u;
    }
    ASSERT_GT(promotions, 0u);
    QueuedWorkerGate gate(jobs);
    world.update(registry, std::vector<Vec3>{}, nullptr);
    const auto stage_b_bytes =
        promotions * (kSnapshotBytes - std::size_t{17} * 17u * 17u * sizeof(u8));
    EXPECT_EQ(world.last_meshing_snapshot_bytes(), stage_b_bytes);
    EXPECT_LE(world.last_meshing_snapshot_bytes(), SHIELD_WorldSystem::kMeshingSnapshotByteBudget);
    gate.release();
    world.wait_for_streaming_jobs();
    EXPECT_EQ(CheckCompletedBurst(burst), kBurstChunks);
}

TEST(AsyncWaterMeshing, EvictedDeferredDescriptorsDoNotCopyOrMeshReplacementChunks) {
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
    world.begin_tick(0);
    BudgetedBurstMeshes burst;
    ASSERT_NO_FATAL_FAILURE(PrepareBudgetedBurst(world, water, false, burst));
    entt::registry registry;
    world.update(registry, std::vector<Vec3>{}, nullptr);
    const auto interval = world.get_last_streaming_budget_stats().update_interval_frames;
    for (int frame = 1; frame < interval - 1; ++frame) {
        world.update(registry, std::vector<Vec3>{}, nullptr);
    }
    const auto deferred =
        std::find_if(burst.chunks.begin(), burst.chunks.end(), [](const auto& chunk) {
            return chunk->pending_lod.load() < 0 && chunk->mesh_version.load() == 0u;
        });
    ASSERT_NE(deferred, burst.chunks.end());
    const auto old = *deferred;
    world.update(registry, Vec3(4096.0f, 6.0f, 4096.0f), nullptr);
    ASSERT_EQ(world.find_streamed_chunk(old->get_coords()), nullptr);
    auto replacement = std::make_shared<Chunk>(old->get_coords());
    FillDugPond(world, *replacement, 32);
    replacement->mesh_version.store(7);
    replacement->water_mesh_version.store(9);
    ASSERT_TRUE(world.adopt_streamed_chunk(replacement));
    world.wait_for_streaming_jobs();
    EXPECT_EQ(world.last_meshing_snapshot_bytes(), 0u);
    EXPECT_EQ(old->mesh_version.load(), 0u);
    EXPECT_EQ(old->pending_lod.load(), -1);
    EXPECT_EQ(replacement->mesh_version.load(), 7u);
    EXPECT_EQ(replacement->water_mesh_version.load(), 9u);
    EXPECT_EQ(replacement->pending_lod.load(), -1);
}

TEST(AsyncWaterMeshing, DueActivationHandsOffPromotionAfterFullMeshingFifoDrains) {
    JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 8.0f;
    params.caves_enabled = false;
    SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    WaterSystem water(&jobs, &world);
    water.SetBootPaused(true);
    world.SetWaterSystem(&water);
    world.begin_tick(0);
    entt::registry registry;
    std::vector<std::shared_ptr<Chunk>> chunks;
    auto promoted = std::make_shared<Chunk>(IVec3(3, 0, 3));
    FillDugPond(world, *promoted, 32);
    promoted->material_data.clear();
    world.GenerateChunkData(*promoted);
    const auto expected = ReferenceMesh(world, water, *promoted);
    promoted->sdf_data.clear();
    // The existing FIFO cap is the eight-tick activation latency plus one.
    // Nine ordinary batches fill it while the last dispatch also starts stage A.
    constexpr std::size_t kBatches = 9;
    for (std::size_t index = 0; index < kBatches; ++index) {
        auto chunk = std::make_shared<Chunk>(
            IVec3(static_cast<int>(index % 3u), 0, static_cast<int>(index / 3u)));
        FillDugPond(world, *chunk, 32);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        chunks.push_back(chunk);
        if (index + 1u == kBatches) {
            ASSERT_TRUE(world.adopt_streamed_chunk(promoted));
        }
        world.update(registry, std::vector<Vec3>{}, nullptr);
    }
    world.quiesce_streaming_jobs_for_save();
    world.wait_for_promotion_jobs();
    EXPECT_EQ(promoted->sdf_data, expected->sdf_data);
    EXPECT_EQ(promoted->mesh_version.load(), 0u);
    world.activate_due(7);
    EXPECT_EQ(promoted->mesh_version.load(), 0u);
    world.begin_tick(8);
    world.activate_due(8);
    EXPECT_EQ(promoted->mesh_indices, expected->mesh_indices);
    EXPECT_EQ(promoted->mesh_version.load(), 1u);
    EXPECT_EQ(promoted->water_mesh_version.load(), 1u);
    EXPECT_EQ(promoted->pending_lod.load(), -1);
    for (const auto& chunk : chunks) {
        EXPECT_EQ(chunk->mesh_version.load(), 1u);
    }
    world.wait_for_streaming_jobs();
    EXPECT_EQ(promoted->mesh_version.load(), 1u);
}

enum class HydroFence : std::uint8_t {
    Save,
    FullDrain,
    Destroy,
    Clear,
    Seed,
    Params,
    WaterDetach
};

void CheckHydroFence(HydroFence fence) {
    JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.hydro_enabled = true;
    params.hydro_iterations = 1;
    auto world = std::make_unique<SHIELD_WorldSystem>(&jobs, nullptr, params, 1337);
    // Warm derived data before mutation, so seed/parameter invalidation is also
    // compared with a fresh world rather than merely testing a cold-cache path.
    world->GetTerrainHeightAt(16.0f, 16.0f);
    QueuedWorkerGate gate(jobs);
    world->PrefetchHydroRegions(0.0f, 0.0f, 0.0f);
    const auto queued = jobs.get_runtime_stats().queue_depth;
    ASSERT_GT(queued, 0u);
    world->PrefetchHydroRegions(16384.0f, 16384.0f, 0.0f);
    EXPECT_EQ(jobs.get_runtime_stats().queue_depth, queued)
        << "one owned hydro batch must bound outstanding callbacks";
    gate.release();
    int seed = 1337;
    switch (fence) {
        case HydroFence::Save:
            world->quiesce_streaming_jobs_for_save();
            break;
        case HydroFence::FullDrain:
            world->wait_for_streaming_jobs();
            break;
        case HydroFence::Destroy:
            world.reset();
            break;
        case HydroFence::Clear:
            world->clear_world(nullptr);
            break;
        case HydroFence::Seed:
            seed = 7331;
            world->set_seed(seed);
            break;
        case HydroFence::Params:
            params.base_amplitude += 4.0f;
            world->set_params(params);
            break;
        case HydroFence::WaterDetach:
            world->SetWaterSystem(nullptr);
            break;
    }
    EXPECT_EQ(jobs.get_runtime_stats().queue_depth, 0u);
    if (fence == HydroFence::Seed || fence == HydroFence::Params) {
        const SHIELD_WorldSystem reference(nullptr, nullptr, params, seed);
        EXPECT_EQ(world->GetTerrainHeightAt(16.0f, 16.0f),
                  reference.GetTerrainHeightAt(16.0f, 16.0f));
    }
}

TEST(AsyncWaterMeshing, HydroPrefetchFencesSaveAndBlockingDrain) {
    CheckHydroFence(HydroFence::Save);
    CheckHydroFence(HydroFence::FullDrain);
}
TEST(AsyncWaterMeshing, HydroPrefetchFencesDestructionAndClear) {
    CheckHydroFence(HydroFence::Destroy);
    CheckHydroFence(HydroFence::Clear);
}
TEST(AsyncWaterMeshing, HydroPrefetchFencesSeedParamsAndDependencyReplacement) {
    CheckHydroFence(HydroFence::Seed);
    CheckHydroFence(HydroFence::Params);
    CheckHydroFence(HydroFence::WaterDetach);
}

TEST(AsyncWaterMeshing, RejectedHydroPrefetchCanBeAdmittedAfterStartup) {
    JobSystem jobs;
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.hydro_enabled = true;
    params.hydro_iterations = 1;
    SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    world.PrefetchHydroRegions(0.0f, 0.0f, 0.0f); // rejected by the stopped pool
    jobs.startup(1);
    QueuedWorkerGate gate(jobs);
    world.PrefetchHydroRegions(0.0f, 0.0f, 0.0f);
    EXPECT_GT(jobs.get_runtime_stats().queue_depth, 0u);
    gate.release();
    world.quiesce_streaming_jobs_for_save();
    EXPECT_EQ(jobs.get_runtime_stats().queue_depth, 0u);
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
            world.wait_for_admitted_streaming_jobs();
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
        world.wait_for_admitted_streaming_jobs();
        for (std::size_t index = 0; index < kChunks - 1u; ++index) {
            EXPECT_EQ(chunks[index]->water_mesh_version.load(), 1u);
        }
        world.update(registry, std::vector<Vec3>{}, nullptr);
        world.wait_for_streaming_jobs();
        EXPECT_EQ(chunks.back()->water_mesh_version.load(), 1u);
        EXPECT_EQ(world.last_meshing_snapshot_bytes(), sizeof(float));
    }
}

TEST(AsyncWaterMeshing, DeferredAdmissionsCountOnlyCurrentCandidatesWithoutLosingBacklog) {
    JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 8.0f;
    params.caves_enabled = false;
    SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    WaterSystem water(&jobs, &world);
    water.SetBootPaused(true);
    world.SetWaterSystem(&water);
    BudgetedBurstMeshes burst;
    PrepareBudgetedBurst(world, water, false, burst);
    entt::registry registry;
    world.begin_tick(1);
    world.update(registry, std::vector<Vec3>{}, nullptr);
    ASSERT_GT(world.get_last_streaming_budget_stats().scheduled_meshing, 0u);
    ASSERT_GT(world.get_last_streaming_budget_stats().deferred_meshing, 0u);
    world.quiesce_streaming_jobs_for_save();

    // Simulate another public producer satisfying the deferred chunks. Their
    // retained descriptors still own a remesh, but no longer match candidates.
    for (std::size_t index = 0; index < burst.chunks.size(); ++index) {
        auto& chunk = *burst.chunks[index];
        if (chunk.pending_lod.load() >= 0)
            continue;
        chunk.mesh_vertices = burst.expected[index]->mesh_vertices;
        chunk.mesh_indices = burst.expected[index]->mesh_indices;
        chunk.current_lod.store(0);
        chunk.water_mesh_generated.store(true);
        chunk.set_state(ChunkState::Ready);
    }
    auto extra = std::make_shared<Chunk>(IVec3(3, 0, 0));
    FillDugPond(world, *extra, 32);
    ASSERT_TRUE(world.adopt_streamed_chunk(extra));
    world.begin_tick(2);
    world.update(registry, std::vector<Vec3>{}, nullptr);
    const auto& frame = world.get_last_streaming_budget_stats();
    EXPECT_EQ(frame.meshing_candidates, 1u);
    EXPECT_GT(frame.scheduled_meshing, 1u);
    EXPECT_EQ(frame.submitted_meshing, 0u);
    EXPECT_EQ(extra->pending_lod.load(), -1);
    EXPECT_EQ(world.get_streaming_telemetry_stats().last_queue_depth, kBurstChunks + 1u);
    EXPECT_EQ(frame.deferred_meshing + world.pending_publication_chunks(), kBurstChunks + 1u);
    world.wait_for_streaming_jobs();
    for (const auto& chunk : burst.chunks) {
        EXPECT_EQ(chunk->pending_lod.load(), -1);
        EXPECT_EQ(chunk->current_lod.load(), 0);
        EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
    }
}

void CheckPromotionDueOrder(bool active_mesh, bool fill_future_queue) {
    JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 8.0f;
    params.caves_enabled = false;
    SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    WaterSystem water(&jobs, &world);
    water.SetBootPaused(true);
    world.SetWaterSystem(&water);
    auto promoted = std::make_shared<Chunk>(IVec3(0, 0, 0));
    FillDugPond(world, *promoted, 32);
    promoted->material_data.clear();
    world.GenerateChunkData(*promoted);
    const auto expected = ReferenceMesh(world, water, *promoted);
    if (active_mesh) {
        World::MarchingCubes::PolygoniseTerrain(world, *promoted, 0.0f, 2);
        ASSERT_FALSE(promoted->mesh_indices.empty());
        promoted->current_lod.store(1);
        promoted->set_state(ChunkState::Ready);
    }
    promoted->sdf_data.clear();
    ASSERT_TRUE(world.adopt_streamed_chunk(promoted));
    entt::registry registry;
    world.begin_tick(0);
    world.update(registry, std::vector<Vec3>{}, nullptr);
    ASSERT_EQ(promoted->pending_lod.load(), 0);
    // Keep stage A unpublished while later ordinary batches occupy the lane.
    world.begin_tick(1);
    std::vector<std::shared_ptr<Chunk>> ordinary;
    const int batches = fill_future_queue ? 9 : 1;
    for (int index = 0; index < batches; ++index) {
        auto chunk = std::make_shared<Chunk>(IVec3(index + 1, 0, 0));
        FillDugPond(world, *chunk, 32);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        ordinary.push_back(chunk);
        world.update(registry, std::vector<Vec3>{}, nullptr);
        EXPECT_LE(world.last_meshing_snapshot_bytes(),
                  SHIELD_WorldSystem::kMeshingSnapshotByteBudget);
    }
    if (fill_future_queue) {
        // Eight future batches plus the reserved stage-B slot fill the cap.
        EXPECT_EQ(ordinary.back()->pending_lod.load(), -1);
        EXPECT_GT(world.get_last_streaming_budget_stats().deferred_meshing, 0u);
    } else {
        ASSERT_EQ(ordinary.front()->pending_lod.load(), 0);
    }
    world.quiesce_streaming_jobs_for_save();
    world.activate_due(7);
    EXPECT_EQ(promoted->mesh_version.load(), 0u);
    world.begin_tick(8);
    world.activate_due(8);
    EXPECT_EQ(promoted->mesh_version.load(), 1u);
    EXPECT_EQ(promoted->water_mesh_version.load(), 1u);
    EXPECT_EQ(promoted->current_lod.load(), 0);
    EXPECT_EQ(promoted->get_state(), ChunkState::Ready);
    EXPECT_EQ(promoted->pending_lod.load(), -1);
    EXPECT_FALSE(promoted->has_collision.load());
    ExpectVerticesEqual(promoted->mesh_vertices, expected->mesh_vertices);
    EXPECT_EQ(promoted->mesh_indices, expected->mesh_indices);
    EXPECT_EQ(promoted->water_mesh_indices, expected->water_mesh_indices);
    for (const auto& chunk : ordinary) {
        EXPECT_EQ(chunk->mesh_version.load(), 0u);
        EXPECT_NE(chunk->get_state(), ChunkState::Ready);
    }
    world.begin_tick(9);
    world.activate_due(9);
    for (const auto& chunk : ordinary) {
        EXPECT_EQ(chunk->mesh_version.load(), 1u);
        EXPECT_EQ(chunk->pending_lod.load(), -1);
    }
    world.wait_for_streaming_jobs();
    EXPECT_EQ(promoted->mesh_version.load(), 1u);
    EXPECT_EQ(world.promotion_dispatch_totals().chunks, 1u);
}

TEST(AsyncWaterMeshing, EarlierPromotionPublishesBeforeLaterOrdinaryBatch) {
    CheckPromotionDueOrder(false, false);
    CheckPromotionDueOrder(true, false);
}

TEST(AsyncWaterMeshing, EarlierPromotionKeepsHandoffSlotAcrossFutureBatchBackpressure) {
    CheckPromotionDueOrder(false, true);
    CheckPromotionDueOrder(true, true);
}

TEST(AsyncWaterMeshing, LastBatchSlotPreservesMixedDispatchInEitherOrder) {
    for (const bool promotion_first : {false, true}) {
        SCOPED_TRACE(promotion_first);
        JobSystem jobs;
        jobs.startup(1);
        auto params = Luminumbra::Test::SyntheticTerrainParams();
        params.base_amplitude = 0.0f;
        params.height_offset = 8.0f;
        params.caves_enabled = false;
        SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
        entt::registry registry;
        world.begin_tick(0);
        for (int index = 0; index < 8; ++index) {
            auto chunk = std::make_shared<Chunk>(IVec3(index + 3, 0, 0));
            world.GenerateChunkData(*chunk);
            chunk->set_state(ChunkState::Idle);
            ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
            world.update(registry, std::vector<Vec3>{}, nullptr);
        }
        auto promoted = std::make_shared<Chunk>(IVec3(0, 0, 0));
        auto ordinary = std::make_shared<Chunk>(IVec3(1, 0, 0));
        for (const auto& chunk : {promoted, ordinary}) {
            world.GenerateChunkData(*chunk);
            chunk->set_state(ChunkState::Idle);
        }
        auto& active = promotion_first ? ordinary : promoted;
        World::MarchingCubes::PolygoniseTerrain(world, *active, 0.0f, 2);
        ASSERT_FALSE(active->mesh_indices.empty());
        active->current_lod.store(1);
        active->set_state(ChunkState::Ready);
        promoted->sdf_data.clear();
        ASSERT_TRUE(world.adopt_streamed_chunk(promoted));
        ASSERT_TRUE(world.adopt_streamed_chunk(ordinary));
        world.begin_tick(1);
        world.update(registry, std::vector<Vec3>{}, nullptr);
        EXPECT_EQ(promoted->pending_lod.load(), 0);
        EXPECT_EQ(ordinary->pending_lod.load(), 0);
        EXPECT_EQ(world.get_last_streaming_budget_stats().deferred_meshing, 0u);
        world.activate_due(8);
        EXPECT_EQ(promoted->mesh_version.load(), 0u);
        EXPECT_EQ(ordinary->mesh_version.load(), 0u);
        world.begin_tick(9);
        world.activate_due(9);
        EXPECT_EQ(promoted->mesh_version.load(), 1u);
        EXPECT_EQ(ordinary->mesh_version.load(), 1u);
        EXPECT_EQ(world.promotion_dispatch_totals().chunks, 1u);
    }
}

enum class RetainedChange : std::uint8_t {
    MovingAnchor,
    NeighbourLod,
    WaterOnly,
    TerrainRequired,
    Satisfied
};

void CheckRetainedMetadata(RetainedChange change, bool use_workers) {
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
    BudgetedBurstMeshes burst;
    ASSERT_NO_FATAL_FAILURE(PrepareBudgetedBurst(world, water, false, burst));
    if (change == RetainedChange::TerrainRequired) {
        for (std::size_t index = 0; index < burst.chunks.size(); ++index) {
            auto& chunk = *burst.chunks[index];
            chunk.mesh_vertices = burst.expected[index]->mesh_vertices;
            chunk.mesh_indices = burst.expected[index]->mesh_indices;
            chunk.current_lod.store(0);
            chunk.set_state(ChunkState::Ready);
        }
    }
    const bool starts_coarse = change == RetainedChange::NeighbourLod;
    entt::registry registry;
    world.begin_tick(0);
    world.update(registry, starts_coarse ? Vec3(-256.0f, 6.0f, 0.0f) : Vec3(0.0f), nullptr);
    world.quiesce_streaming_jobs_for_save();
    const auto retained =
        std::find_if(burst.chunks.begin(), burst.chunks.end(), [](const auto& chunk) {
            return chunk->pending_lod.load() < 0 && chunk->mesh_version.load() == 0u &&
                   chunk->get_state() == ChunkState::Idle;
        });
    auto target = retained == burst.chunks.end() ? std::shared_ptr<Chunk>{} : *retained;
    if (change == RetainedChange::TerrainRequired) {
        const auto water_retained =
            std::find_if(burst.chunks.begin(), burst.chunks.end(), [](const auto& chunk) {
                return chunk->pending_lod.load() < 0 && chunk->water_mesh_version.load() == 0u;
            });
        ASSERT_NE(water_retained, burst.chunks.end());
        target = *water_retained;
    }
    ASSERT_NE(target, nullptr);
    const int expected_lod = starts_coarse || change == RetainedChange::MovingAnchor ||
                                     change == RetainedChange::Satisfied
                                 ? 1
                                 : 0;
    u8 faces = World::MarchingCubes::kNoTransitionFaces;
    if (change == RetainedChange::NeighbourLod) {
        const IVec3 neighbor_coords = target->get_coords() + IVec3(1, 0, 0);
        auto neighbor = world.find_streamed_chunk(neighbor_coords);
        if (!neighbor) {
            neighbor = std::make_shared<Chunk>(neighbor_coords);
            FillDugPond(world, *neighbor, 32);
            ASSERT_TRUE(world.adopt_streamed_chunk(neighbor));
        }
        World::MarchingCubes::PolygoniseTerrain(world, *neighbor, 0.0f, 1);
        neighbor->current_lod.store(0);
        neighbor->set_state(ChunkState::Ready);
        faces = World::MarchingCubes::TransitionFaceMaxX;
    } else if (change == RetainedChange::WaterOnly || change == RetainedChange::Satisfied) {
        World::MarchingCubes::PolygoniseTerrain(world, *target, 0.0f, 1);
        target->current_lod.store(expected_lod);
        target->set_state(ChunkState::Ready);
        target->water_mesh_generated.store(change == RetainedChange::Satisfied);
    } else if (change == RetainedChange::TerrainRequired) {
        target->current_lod.store(1);
    }
    if (expected_lod == 1) {
        const auto chunks = world.snapshot_streamed_chunks();
        const IVec3 coords = target->get_coords();
        for (const auto& neighbor : chunks) {
            if (neighbor->current_lod.load() != 0 || neighbor->mesh_vertices.empty() ||
                neighbor->mesh_indices.empty()) {
                continue;
            }
            const IVec3 delta = neighbor->get_coords() - coords;
            if (delta == IVec3(-1, 0, 0)) {
                faces |= World::MarchingCubes::TransitionFaceMinX;
            } else if (delta == IVec3(1, 0, 0)) {
                faces |= World::MarchingCubes::TransitionFaceMaxX;
            } else if (delta == IVec3(0, 0, -1)) {
                faces |= World::MarchingCubes::TransitionFaceMinZ;
            } else if (delta == IVec3(0, 0, 1)) {
                faces |= World::MarchingCubes::TransitionFaceMaxZ;
            }
        }
    }
    if (change == RetainedChange::Satisfied) {
        target->applied_transition_faces.store(faces);
        World::MarchingCubes::PolygoniseTerrain(world, *target, 0.0f, 2);
        World::MarchingCubes::AddBoundaryTransitionSkirts(*target, 2, faces);
    }
    auto expected = ReferenceMesh(world, water, *target);
    if (expected_lod == 1) {
        World::MarchingCubes::PolygoniseTerrain(world, *expected, 0.0f, 2);
        World::MarchingCubes::AddBoundaryTransitionSkirts(*expected, 2, faces);
    }
    world.begin_tick(1);
    const Vec3 anchor = expected_lod == 1 ? Vec3(-256.0f, 6.0f, 0.0f) : Vec3(0.0f);
    world.update(registry, anchor, nullptr);
    EXPECT_LE(world.last_meshing_snapshot_bytes(), SHIELD_WorldSystem::kMeshingSnapshotByteBudget);
    // The later pass refreshes metadata but keeps the original tick-8 boundary.
    world.begin_tick(8);
    world.activate_due(8);
    EXPECT_EQ(target->pending_lod.load(), -1);
    EXPECT_EQ(target->current_lod.load(), expected_lod);
    EXPECT_EQ(target->get_state(), ChunkState::Ready);
    EXPECT_EQ(target->mesh_version.load(), change == RetainedChange::WaterOnly ? 0u : 1u);
    EXPECT_EQ(target->water_mesh_version.load(), 1u);
    EXPECT_EQ(target->applied_transition_faces.load(), faces);
    ExpectVerticesEqual(target->mesh_vertices, expected->mesh_vertices);
    EXPECT_EQ(target->mesh_indices, expected->mesh_indices);
    world.wait_for_streaming_jobs();
}

TEST(AsyncWaterMeshing, RetainedCandidateRefreshesMovingAnchorLodBeforeAdmission) {
    for (const bool workers : {false, true}) {
        CheckRetainedMetadata(RetainedChange::MovingAnchor, workers);
    }
}
TEST(AsyncWaterMeshing, RetainedCandidateRefreshesNeighbourTransitionMaskBeforeAdmission) {
    CheckRetainedMetadata(RetainedChange::NeighbourLod, true);
}
TEST(AsyncWaterMeshing, RetainedCandidateRefreshesTerrainAndWaterClassificationBeforeAdmission) {
    CheckRetainedMetadata(RetainedChange::WaterOnly, true);
    CheckRetainedMetadata(RetainedChange::TerrainRequired, true);
}
TEST(AsyncWaterMeshing, SatisfiedRetainedRequestKeepsCurrentLodAndOriginalOwnership) {
    CheckRetainedMetadata(RetainedChange::Satisfied, true);
}

enum class RetainedHeightmapChange : std::uint8_t {
    DirtyWater,
    SatisfiedWater,
    VoxelEdit,
    CoarseLod
};

TEST(AsyncWaterMeshing, RetainedHeightmapRequestsRefreshWaterAndTerrainLodsBeforeAdmission) {
    for (const bool workers : {false, true}) {
        for (const auto change : {RetainedHeightmapChange::DirtyWater,
                                  RetainedHeightmapChange::SatisfiedWater,
                                  RetainedHeightmapChange::VoxelEdit,
                                  RetainedHeightmapChange::CoarseLod}) {
            SCOPED_TRACE(::testing::Message() << workers << ':' << static_cast<int>(change));
            JobSystem jobs;
            if (workers) {
                jobs.startup(1);
            }
            auto params = Luminumbra::Test::SyntheticTerrainParams();
            params.base_amplitude = 0.0f;
            params.height_offset = 8.0f;
            params.caves_enabled = false;
            SHIELD_WorldSystem world(workers ? &jobs : nullptr, nullptr, params, 1337);
            WaterSystem water(workers ? &jobs : nullptr, &world);
            water.SetBootPaused(true);
            world.SetWaterSystem(&water);
            BudgetedBurstMeshes burst;
            ASSERT_NO_FATAL_FAILURE(PrepareBudgetedBurst(world, water, false, burst));
            for (const auto& chunk : burst.chunks) {
                chunk->sdf_data.clear();
                World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 2);
                chunk->current_lod.store(1);
                chunk->set_state(ChunkState::Ready);
            }
            entt::registry registry;
            world.begin_tick(0);
            world.update(registry, std::vector<Vec3>{}, nullptr, false);
            world.quiesce_streaming_jobs_for_save();
            ASSERT_GT(world.get_last_streaming_budget_stats().submitted_meshing, 0u);
            ASSERT_GT(world.get_last_streaming_budget_stats().deferred_meshing, 0u);
            std::sort(
                burst.chunks.begin(), burst.chunks.end(), [](const auto& left, const auto& right) {
                    return left->get_id() < right->get_id();
                });
            const auto retained =
                std::find_if(burst.chunks.begin(), burst.chunks.end(), [](const auto& chunk) {
                    return chunk->pending_lod.load() < 0;
                });
            ASSERT_NE(retained, burst.chunks.end());
            const auto& target = *retained;
            const auto terrain_vertices = target->mesh_vertices;
            const auto terrain_indices = target->mesh_indices;
            std::vector<Vec3> anchors;
            if (change == RetainedHeightmapChange::SatisfiedWater) {
                target->water_mesh_generated.store(true);
            } else if (change == RetainedHeightmapChange::VoxelEdit) {
                Chunk prepared(target->get_coords());
                world.GenerateChunkData(prepared);
                target->sdf_data = prepared.sdf_data;
                target->mark_voxel_data_dirty();
                target->current_lod.store(-1);
            } else if (change == RetainedHeightmapChange::CoarseLod) {
                anchors.emplace_back(600.0f, 8.0f, 8.0f);
            }
            // Retained descriptors refresh without needing an invalidation owner.
            while (!water.mesh_invalidations().empty()) {
                water.acknowledge_mesh_invalidation(*water.mesh_invalidations().begin());
            }
            world.begin_tick(1);
            world.update(registry, anchors, nullptr, false);
            const bool terrain_required = change == RetainedHeightmapChange::VoxelEdit ||
                                          change == RetainedHeightmapChange::CoarseLod;
            const int expected_lod = change == RetainedHeightmapChange::VoxelEdit   ? 0
                                     : change == RetainedHeightmapChange::CoarseLod ? 2
                                                                                    : 1;
            world.activate_due(7);
            EXPECT_EQ(target->water_mesh_version.load(), 0u);
            world.activate_due(8);
            EXPECT_EQ(target->pending_lod.load(), -1);
            EXPECT_EQ(target->current_lod.load(), expected_lod);
            EXPECT_EQ(target->mesh_version.load(), terrain_required ? 1u : 0u);
            EXPECT_EQ(target->water_mesh_version.load(),
                      change == RetainedHeightmapChange::CoarseLod ? 0u : 1u);
            if (!terrain_required) {
                ExpectVerticesEqual(target->mesh_vertices, terrain_vertices);
                EXPECT_EQ(target->mesh_indices, terrain_indices);
                EXPECT_FALSE(target->water_mesh_vertices.empty());
                EXPECT_TRUE(target->water_mesh_generated.load());
            }
            EXPECT_EQ(world.promotion_dispatch_totals().chunks, 0u);
        }
    }
}
void CheckStackedWaterSurface(const std::vector<float>& levels,
                              DepthGrid depth_grid,
                              const std::vector<int>& layers = {-1, 0, 1},
                              bool all_wet = false) {
    constexpr int kResolution = 4;
    constexpr std::size_t kCells = kResolution * kResolution;
    ASSERT_EQ(levels.size(), kCells);
    ASSERT_FALSE(layers.empty());
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = -64.0f;
    params.caves_enabled = false;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    const WaterSystem water(nullptr, &world);
    const auto prepare = [depth_grid, &levels, all_wet](Chunk& chunk) {
        chunk.current_water_resolution.store(kResolution);
        chunk.has_water_sim.store(true);
        chunk.water_level_data = levels;
        chunk.water_depth_mm.assign(kCells, 2000);
        for (std::size_t index = 0; index < kCells; ++index) {
            // Dry, threshold, just wet, and deep cells retain the depth contract.
            constexpr std::int32_t kDepths[] = {0, 50, 51, 2000};
            chunk.water_depth_mm[index] = all_wet ? 2000 : kDepths[index % 4u];
        }
        if (depth_grid == DepthGrid::Missing) {
            chunk.water_depth_mm.clear();
        } else if (depth_grid == DepthGrid::Truncated) {
            chunk.water_depth_mm.pop_back();
        }
        chunk.set_state(ChunkState::Ready);
    };

    Chunk reference(IVec3(0));
    prepare(reference);
    reference.water_level_data.assign(kCells, 6.0f);
    World::MarchingCubes::GenerateWaterMesh(water, world, reference);
    const std::size_t single_chunk_quads =
        depth_grid == DepthGrid::Complete && !all_wet ? kCells / 2u : kCells;
    ASSERT_EQ(reference.water_mesh_vertices.size(), single_chunk_quads * 4u);
    ASSERT_EQ(reference.water_mesh_indices.size(), single_chunk_quads * 6u);

    std::vector<std::shared_ptr<Chunk>> chunks;
    for (const int chunk_y : layers) {
        auto chunk = std::make_shared<Chunk>(IVec3(0, chunk_y, 0));
        prepare(*chunk);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        chunks.push_back(std::move(chunk));
    }
    ASSERT_TRUE(world.renderable_chunks_view().empty());
    // Repeated remeshing must preserve ownership and selection without a state transition.
    for (int repeat = 0; repeat < 2; ++repeat) {
        SCOPED_TRACE(repeat);
        std::size_t total_quads = 0;
        std::size_t selected_chunks = 0;
        for (const auto& chunk : chunks) {
            SCOPED_TRACE(chunk->get_coords().y);
            const auto saved_depth = chunk->water_depth_mm;
            World::MarchingCubes::GenerateWaterMesh(water, world, *chunk);
            std::size_t expected_quads = 0;
            for (std::size_t index = 0; index < kCells; ++index) {
                const bool wet = depth_grid != DepthGrid::Complete || saved_depth[index] > 50;
                // Independent resident-owner oracle, including holes and both extremes.
                const double surface_layer =
                    std::floor(static_cast<double>(levels[index]) / CHUNK_SIZE_Y);
                int owner = *std::min_element(layers.begin(), layers.end());
                for (const int layer : layers) {
                    if (layer <= surface_layer && layer > owner) {
                        owner = layer;
                    }
                }
                if (wet && owner == chunk->get_coords().y) {
                    ++expected_quads;
                }
            }
            EXPECT_TRUE(chunk->mesh_vertices.empty());
            EXPECT_TRUE(chunk->mesh_indices.empty());
            EXPECT_EQ(chunk->water_level_data, levels);
            EXPECT_EQ(chunk->water_depth_mm, saved_depth);
            EXPECT_TRUE(chunk->water_mesh_generated.load());
            EXPECT_EQ(chunk->water_mesh_vertices.size(), expected_quads * 4u);
            EXPECT_EQ(chunk->water_mesh_indices.size(), expected_quads * 6u);
            const auto& selected = world.renderable_chunks_view();
            EXPECT_EQ(std::count(selected.begin(), selected.end(), chunk.get()),
                      expected_quads > 0u ? 1 : 0);
            total_quads += chunk->water_mesh_indices.size() / 6u;
            selected_chunks += expected_quads > 0u ? 1u : 0u;
        }
        EXPECT_EQ(total_quads, single_chunk_quads);
        EXPECT_EQ(world.renderable_chunks_view().size(), selected_chunks);
    }
}

TEST(AsyncWaterMeshing, DeepSeabedResidentLayersKeepSeaLevelSurface) {
    for (const auto depth_grid : {DepthGrid::Complete, DepthGrid::Missing, DepthGrid::Truncated}) {
        CheckStackedWaterSurface(std::vector<float>(16u, 0.0f), depth_grid, {-5, -4, -3}, true);
    }
}

TEST(AsyncWaterMeshing, ResidentOwnershipCoversBelowAboveAndGaps) {
    for (const float height : {-1000.0f, -80.0f, -64.0f, -48.0f, -0.001f, 0.0f, 1000.0f}) {
        SCOPED_TRACE(height);
        CheckStackedWaterSurface(std::vector<float>(16u, height), DepthGrid::Complete, {-5, -3, 1});
        CheckStackedWaterSurface(std::vector<float>(16u, height), DepthGrid::Complete, {1, -5, -3});
        CheckStackedWaterSurface(std::vector<float>(16u, height), DepthGrid::Complete, {-4});
    }
}

// Exercise the scheduler, worker snapshot and owner-thread publication, including
// an arrival while the old owner's worker is still holding its previous interval.
TEST(AsyncWaterMeshing, SurfaceOwnerArrivalSupersedesPendingWaterAndRemeshesColumn) {
    JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = -64.0f;
    params.caves_enabled = false;
    SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    WaterSystem water(&jobs, &world);
    water.SetBootPaused(true);
    world.SetWaterSystem(&water);
    const auto make_chunk = [&world](int y) {
        auto chunk = std::make_shared<Chunk>(IVec3(0, y, 0));
        world.GenerateChunkData(*chunk);
        chunk->current_water_resolution.store(8);
        chunk->has_water_sim.store(true);
        chunk->water_level_data.assign(64u, 0.0f);
        chunk->water_depth_mm.assign(64u, 64000);
        chunk->water_flow_data.assign(64u, Vec2(0.0f));
        chunk->water_sim_terrain_height.assign(64u, -64.0f);
        chunk->current_lod.store(0);
        chunk->set_state(ChunkState::Ready);
        return chunk;
    };
    std::vector<std::shared_ptr<Chunk>> chunks;
    for (const int y : {-5, -4, -3}) {
        auto chunk = make_chunk(y);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        chunks.push_back(std::move(chunk));
    }
    entt::registry registry;
    const auto remesh = [&] {
        world.update(registry, std::vector<Vec3>{}, nullptr, false);
        world.wait_for_streaming_jobs();
    };
    remesh();
    ASSERT_EQ(chunks.back()->water_mesh_vertices.size(), 64u * 4u);
    ASSERT_EQ(world.renderable_chunks_view(), std::vector<Chunk*>{chunks.back().get()});
    auto lower = make_chunk(-6);
    {
        PendingMeshGate gate(jobs, world);
        chunks.front()->water_mesh_generated.store(false);
        water.retain_mesh_invalidation(chunks.front()->get_id());
        world.update(registry, std::vector<Vec3>{}, nullptr, false);
        ASSERT_EQ(chunks.front()->pending_lod.load(), 0);
        ASSERT_TRUE(world.adopt_streamed_chunk(lower));
        gate.finish();
        // Moving the lowest bound stales an empty water-only result. Its
        // readiness must survive so the retry does not rebuild terrain.
        EXPECT_EQ(chunks.front()->get_state(), ChunkState::Ready);
        EXPECT_FALSE(chunks.front()->water_mesh_generated.load());
    }
    remesh();
    EXPECT_EQ(chunks.front()->mesh_version.load(), 0u);
    EXPECT_TRUE(lower->water_mesh_vertices.empty());
    EXPECT_EQ(world.renderable_chunks_view(), std::vector<Chunk*>{chunks.back().get()});
    auto owner = make_chunk(0);
    {
        PendingMeshGate gate(jobs, world);
        chunks.back()->water_mesh_generated.store(false);
        water.retain_mesh_invalidation(chunks.back()->get_id());
        world.update(registry, std::vector<Vec3>{}, nullptr, false);
        ASSERT_EQ(chunks.back()->pending_lod.load(), 0);
        const auto previous_version = chunks.back()->water_mesh_version.load();
        ASSERT_TRUE(world.adopt_streamed_chunk(owner));
        EXPECT_EQ(chunks.back()->water_mesh_version.load(), previous_version);
        EXPECT_TRUE(water.mesh_invalidations().contains(chunks.back()->get_id()));
        gate.finish();
        // The stale result must not retire the residency-change invalidation.
        EXPECT_FALSE(chunks.back()->water_mesh_generated.load());
    }
    remesh();
    for (const auto& chunk : chunks) {
        EXPECT_TRUE(chunk->water_mesh_vertices.empty());
        EXPECT_TRUE(chunk->water_mesh_indices.empty());
    }
    ASSERT_EQ(owner->water_mesh_vertices.size(), 64u * 4u);
    EXPECT_EQ(owner->water_mesh_indices.size(), 64u * 6u);
    EXPECT_EQ(world.renderable_chunks_view(), std::vector<Chunk*>{owner.get()});

    // Normal terrain-height activation evicts Y=0, retaining the seabed band.
    world.debug_set_streaming_radius_cap(1);
    const int activation_frames = world.get_last_streaming_budget_stats().update_interval_frames;
    const float camera_y = -static_cast<float>((RENDER_DISTANCE_UP + 3) * CHUNK_SIZE_Y);
    for (int frame = 0; frame < activation_frames; ++frame) {
        world.update(registry, Vec3(8.0f, camera_y, 8.0f), nullptr, false);
        world.wait_for_streaming_jobs();
    }
    ASSERT_FALSE(world.find_streamed_chunk(IVec3(0)));
    remesh();
    ASSERT_EQ(chunks.back()->water_mesh_vertices.size(), 64u * 4u);
    EXPECT_TRUE(chunks[0]->water_mesh_vertices.empty());
    EXPECT_TRUE(chunks[1]->water_mesh_vertices.empty());
}

TEST(AsyncWaterMeshing, StackedWaterOnlyChunksEmitOnlyContainingSurface) {
    for (const auto depth_grid : {DepthGrid::Complete, DepthGrid::Missing, DepthGrid::Truncated}) {
        SCOPED_TRACE(static_cast<int>(depth_grid));
        CheckStackedWaterSurface(std::vector<float>(16u, 6.0f), depth_grid);
    }
}

TEST(AsyncWaterMeshing, StackedWaterSurfaceBoundaryHasExactlyOneOwner) {
    for (const float surface_y :
         {-static_cast<float>(CHUNK_SIZE_Y), 0.0f, static_cast<float>(CHUNK_SIZE_Y)}) {
        SCOPED_TRACE(surface_y);
        CheckStackedWaterSurface(std::vector<float>(16u, surface_y), DepthGrid::Complete);
    }
}

TEST(AsyncWaterMeshing, StackedSlopingWaterAssignsEachCellIndependently) {
    std::vector<float> levels(16u);
    for (std::size_t index = 0; index < levels.size(); ++index) {
        constexpr float kHeights[] = {-6.0f, 6.0f, static_cast<float>(CHUNK_SIZE_Y) + 6.0f};
        levels[index] = kHeights[index % 3u];
    }
    CheckStackedWaterSurface(levels, DepthGrid::Complete);
}

TEST(AsyncWaterMeshing, OwnerIntervalsInvalidateOnlyTheAffectedResidentStack) {
    World::ActivationResidencyIndex index;
    std::vector<std::shared_ptr<Chunk>> unrelated;
    for (int x = 1; x <= 32; ++x) {
        auto chunk = std::make_shared<Chunk>(IVec3(x, 0, 0));
        index.insert(chunk);
        chunk->water_mesh_generated.store(true);
        unrelated.push_back(chunk);
    }
    auto bottom = std::make_shared<Chunk>(IVec3(0, -5, 0));
    auto top = std::make_shared<Chunk>(IVec3(0, 1, 0));
    auto middle = std::make_shared<Chunk>(IVec3(0, -3, 0));
    const auto before = index.counters().water_owner_visits;
    index.insert(top);
    index.insert(bottom);
    top->water_mesh_generated.store(true);
    const auto top_owner_version = top->water_mesh_owner_version;
    index.insert(middle);
    EXPECT_EQ(top->water_mesh_owner_version, top_owner_version + 1u);
    EXPECT_EQ(top->water_mesh_min_y, 1.0 * CHUNK_SIZE_Y);
    EXPECT_EQ(index.counters().water_owner_visits - before, 6u);
    EXPECT_FALSE(top->water_mesh_generated.load());
    EXPECT_EQ(bottom->water_mesh_min_y, -std::numeric_limits<double>::infinity());
    EXPECT_EQ(bottom->water_mesh_max_y, -3.0 * CHUNK_SIZE_Y);
    EXPECT_EQ(middle->water_mesh_min_y, -3.0 * CHUNK_SIZE_Y);
    EXPECT_EQ(middle->water_mesh_max_y, 1.0 * CHUNK_SIZE_Y);
    EXPECT_EQ(top->water_mesh_max_y, std::numeric_limits<double>::infinity());
    index.erase(*middle);
    EXPECT_EQ(bottom->water_mesh_max_y, 1.0 * CHUNK_SIZE_Y);
    index.erase(*bottom);
    EXPECT_EQ(top->water_mesh_min_y, -std::numeric_limits<double>::infinity());
    index.erase(*top);
    EXPECT_EQ(index.counters().water_owner_visits - before, 9u);
    EXPECT_EQ(index.columns().size(), unrelated.size());
    for (const auto& chunk : unrelated) {
        EXPECT_TRUE(chunk->water_mesh_generated.load());
    }
}

TEST(AsyncWaterMeshing, DivergedLayersUseOneColumnSampleAcrossTheBoundary) {
    for (const bool workers : {false, true}) {
        for (const bool reverse : {false, true}) {
            SCOPED_TRACE(::testing::Message() << workers << ':' << reverse);
            JobSystem jobs;
            if (workers) {
                jobs.startup(1);
            }
            auto params = Luminumbra::Test::SyntheticTerrainParams();
            params.base_amplitude = 0.0f;
            params.height_offset = -64.0f;
            params.caves_enabled = false;
            SHIELD_WorldSystem world(workers ? &jobs : nullptr, nullptr, params, 1337);
            WaterSystem water(workers ? &jobs : nullptr, &world);
            water.SetBootPaused(true);
            world.SetWaterSystem(&water);
            auto low = std::make_shared<Chunk>(IVec3(0));
            auto high = std::make_shared<Chunk>(IVec3(0, 1, 0));
            for (const auto& chunk : {high, low}) {
                world.GenerateChunkData(*chunk);
                chunk->has_water_sim.store(true);
                chunk->current_water_resolution.store(chunk == low ? 4 : 8);
                chunk->water_level_data.assign(chunk == low ? 16u : 64u,
                                               chunk == low ? 17.0f : 15.0f);
                chunk->water_depth_mm.assign(chunk == low ? 16u : 64u, chunk == low ? 2000 : 0);
                chunk->current_lod.store(0);
                chunk->set_state(ChunkState::Ready);
                ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
            }
            if (reverse) {
                low->water_level_data.assign(16u, 15.0f);
                high->water_level_data.assign(64u, 17.0f);
            }
            const auto low_levels = low->water_level_data;
            const auto high_levels = high->water_level_data;
            // Direct and scheduled meshing must sample the same grid, including
            // the wet/dry test and corner interpolation, regardless of owner Y.
            auto owner = reverse ? low : high;
            for (const auto& chunk : {low, high}) {
                World::MarchingCubes::GenerateWaterMesh(water, world, *chunk);
                EXPECT_EQ(chunk->water_mesh_indices.size(), chunk == owner ? 96u : 0u);
                chunk->water_mesh_generated.store(false);
                water.retain_mesh_invalidation(chunk->get_id());
            }
            entt::registry registry;
            world.update(registry, std::vector<Vec3>{}, nullptr, false);
            world.wait_for_streaming_jobs();
            EXPECT_EQ(low->water_mesh_indices.size() + high->water_mesh_indices.size(), 96u);
            EXPECT_EQ(owner->water_mesh_indices.size(), 96u);
            for (const auto& vertex : owner->water_mesh_vertices) {
                EXPECT_FLOAT_EQ(vertex.position.y +
                                    static_cast<float>(owner->get_coords().y * CHUNK_SIZE_Y),
                                low_levels.front());
            }
            EXPECT_EQ(low->water_level_data, low_levels);
            EXPECT_EQ(high->water_level_data, high_levels);
            EXPECT_EQ(low->water_depth_mm, std::vector<std::int32_t>(16u, 2000));
            EXPECT_EQ(high->water_depth_mm, std::vector<std::int32_t>(64u, 0));
            // A canonical source wake must reach an already-generated remote owner.
            world.invalidate_column_water_mesh(*low);
            EXPECT_FALSE(low->water_mesh_generated.load());
            EXPECT_FALSE(high->water_mesh_generated.load());
            EXPECT_TRUE(water.mesh_invalidations().contains(high->get_id()));
        }
    }
}

TEST(AsyncWaterMeshing, ColumnSourceRejectsUnsupportedPartialAndUninitializedGrids) {
    SHIELD_WorldSystem world(nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    WaterSystem water(nullptr, &world);
    auto low = std::make_shared<Chunk>(IVec3(0));
    auto high = std::make_shared<Chunk>(IVec3(0, 1, 0));
    for (const auto& chunk : {low, high}) {
        chunk->has_water_sim.store(true);
        chunk->current_water_resolution.store(4);
        chunk->water_level_data.assign(16u, 17.0f);
        chunk->water_depth_mm.assign(16u, 2000);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    }
    EXPECT_EQ(&world.column_water_mesh_source(*high), low.get());
    for (const int resolution : {0, 1, 33, std::numeric_limits<int>::max()}) {
        low->current_water_resolution.store(resolution);
        EXPECT_EQ(&world.column_water_mesh_source(*low), high.get());
    }
    low->current_water_resolution.store(4);
    low->has_water_sim.store(false);
    EXPECT_EQ(&world.column_water_mesh_source(*low), high.get());
    low->has_water_sim.store(true);
    low->water_level_data.pop_back();
    EXPECT_EQ(&world.column_water_mesh_source(*low), high.get());
    World::MarchingCubes::GenerateWaterMesh(water, world, *high);
    EXPECT_EQ(high->water_mesh_indices.size(), 96u);
    high->water_level_data.clear();
    World::MarchingCubes::GenerateWaterMesh(water, world, *high);
    EXPECT_TRUE(high->water_mesh_indices.empty());
    EXPECT_FALSE(high->water_mesh_generated.load());
    Chunk unindexed(IVec3(0));
    EXPECT_EQ(&world.column_water_mesh_source(unindexed), &unindexed);
    Chunk absent(IVec3(9));
    EXPECT_EQ(&world.column_water_mesh_source(absent), &absent);
}

TEST(AsyncWaterMeshing, NonFiniteColumnSamplesNeverPublishNonFiniteVertices) {
    SHIELD_WorldSystem world(nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    WaterSystem water(nullptr, &world);
    Chunk chunk(IVec3(0));
    chunk.has_water_sim.store(true);
    chunk.current_water_resolution.store(4);
    chunk.water_depth_mm.assign(16u, 2000);
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity()}) {
        chunk.water_level_data.assign(16u, 17.0f);
        chunk.water_level_data[5] = invalid;
        World::MarchingCubes::GenerateWaterMesh(water, world, chunk);
        EXPECT_FALSE(chunk.water_mesh_vertices.empty());
        for (const auto& vertex : chunk.water_mesh_vertices) {
            EXPECT_TRUE(std::isfinite(vertex.position.y));
        }
    }
}

TEST(AsyncWaterMeshing, PendingColumnSnapshotRejectsAChangedSourceWithUnchangedBounds) {
    JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = -64.0f;
    params.caves_enabled = false;
    SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    WaterSystem water(&jobs, &world);
    water.SetBootPaused(true);
    world.SetWaterSystem(&water);
    auto low = std::make_shared<Chunk>(IVec3(0));
    auto high = std::make_shared<Chunk>(IVec3(0, 1, 0));
    for (const auto& chunk : {low, high}) {
        world.GenerateChunkData(*chunk);
        chunk->current_water_resolution.store(4);
        chunk->has_water_sim.store(true);
        chunk->water_level_data.assign(16u, 17.0f);
        chunk->water_depth_mm.assign(16u, 2000);
        chunk->current_lod.store(0);
        chunk->set_state(ChunkState::Ready);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    }
    // A partial lower layer cannot supply the dispatch. Making it complete
    // later changes the source without any residency or interval change.
    low->water_level_data.pop_back();
    entt::registry registry;
    {
        PendingMeshGate gate(jobs, world);
        world.update(registry, std::vector<Vec3>{}, nullptr, false);
        ASSERT_EQ(high->pending_lod.load(), 0);
        EXPECT_EQ(world.last_meshing_snapshot_bytes(),
                  2u * 16u * (sizeof(float) + sizeof(std::int32_t)));
        low->water_level_data.assign(16u, 19.0f);
        world.invalidate_column_water_mesh(*low);
        gate.finish();
        EXPECT_EQ(high->water_mesh_version.load(), 0u);
        EXPECT_TRUE(high->water_mesh_indices.empty());
        EXPECT_FALSE(high->water_mesh_generated.load());
        EXPECT_EQ(high->get_state(), ChunkState::Ready);
    }
    world.update(registry, std::vector<Vec3>{}, nullptr, false);
    world.wait_for_streaming_jobs();
    EXPECT_EQ(high->water_mesh_indices.size(), 96u);
    for (const auto& vertex : high->water_mesh_vertices) {
        EXPECT_FLOAT_EQ(vertex.position.y, 3.0f);
    }
}

TEST(AsyncWaterMeshing, ColumnSourcePrefersTheSimulationSourceLayerOverLowerResidents) {
    SHIELD_WorldSystem world(nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    WaterSystem water(nullptr, &world);
    auto below = std::make_shared<Chunk>(IVec3(0, -1, 0));
    auto zero = std::make_shared<Chunk>(IVec3(0));
    auto above = std::make_shared<Chunk>(IVec3(0, 1, 0));
    for (const auto& chunk : {below, above, zero}) {
        chunk->has_water_sim.store(true);
        chunk->current_water_resolution.store(4);
        chunk->water_level_data.assign(16u, chunk == zero ? 17.0f : 15.0f);
        chunk->water_depth_mm.assign(16u, 2000);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    }
    EXPECT_EQ(&world.column_water_mesh_source(*below), zero.get());
    EXPECT_EQ(&world.column_water_mesh_source(*above), zero.get());
    for (const auto& chunk : {below, zero, above}) {
        World::MarchingCubes::GenerateWaterMesh(water, world, *chunk);
        EXPECT_EQ(chunk->water_mesh_indices.size(), chunk == above ? 96u : 0u);
    }
    // Unsupported canonical metadata takes the same bounded fallback path as
    // a missing Y=0 layer; it must not hide the complete seabed grid.
    zero->current_water_resolution.store(33);
    EXPECT_EQ(&world.column_water_mesh_source(*above), below.get());
}

TEST(AsyncWaterMeshing, LegacyAndRegionalCadenceWakeRemoteOwnersOnceAndRefusalDoesNot) {
    for (const bool regional : {false, true}) {
        SCOPED_TRACE(regional);
        auto params = Luminumbra::Test::SyntheticTerrainParams();
        params.base_amplitude = 0.0f;
        params.height_offset = -64.0f;
        params.caves_enabled = false;
        SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
        WaterSystem water(nullptr, &world);
        water.SetBootPaused(true);
        world.SetWaterSystem(&water);
        auto source = std::make_shared<Chunk>(IVec3(0));
        auto owner = std::make_shared<Chunk>(IVec3(0, 1, 0));
        for (const auto& chunk : {source, owner}) {
            world.GenerateChunkData(*chunk);
            chunk->current_water_resolution.store(4);
            chunk->has_water_sim.store(true);
            chunk->water_level_data.assign(16u, 17.0f);
            chunk->water_depth_mm.assign(16u, 2000);
            chunk->water_bed_mm.assign(16u, 15000);
            chunk->water_edge_flux.assign(32u, 0);
            chunk->water_flow_data.assign(16u, Vec2(0.0f));
            chunk->water_sim_terrain_height.assign(16u, 15.0f);
            chunk->water_rest_level.assign(16u, 0.0f);
            chunk->water_src_mm.assign(16u, 0);
            chunk->water_mesh_invalidation_version = 7;
            chunk->current_lod.store(0);
            chunk->set_state(ChunkState::Ready);
            ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        }
        // Unrelated dry residents must not participate in a later source wake.
        for (int index = 0; index < 32; ++index) {
            auto unrelated = std::make_shared<Chunk>(IVec3(1 + index % 8, 0, 1 + index / 8));
            world.GenerateChunkData(*unrelated);
            unrelated->current_lod.store(0);
            unrelated->set_state(ChunkState::Ready);
            ASSERT_TRUE(world.adopt_streamed_chunk(unrelated));
        }
        // Direct publication can satisfy residency wakes before collection.
        World::MarchingCubes::GenerateWaterMesh(water, world, *source);
        World::MarchingCubes::GenerateWaterMesh(water, world, *owner);
        entt::registry registry;
        world.update(registry, std::vector<Vec3>{}, nullptr, false);
        EXPECT_EQ(world.get_last_streaming_budget_stats().submitted_meshing, 0u);
        world.wait_for_streaming_jobs();
        world.update(registry, std::vector<Vec3>{}, nullptr, false);
        world.wait_for_streaming_jobs();
        ASSERT_TRUE(water.mesh_invalidations().empty());
        ASSERT_TRUE(owner->water_mesh_generated.load());
        ASSERT_EQ(owner->water_mesh_indices.size(), 96u);
        const auto before = owner->water_mesh_version.load();
        const auto work_before = world.water_column_mesh_work();
        source->water_mesh_dirty_ticks = 14;
        if (regional) {
            Luminumbra::world::ActiveRegionLedger ledger;
            ledger.mark_edited({0, 0}, Luminumbra::world::WorldClock{});
            const std::vector participants{source};
            RegionalWater::Transaction refused;
            refused.Begin(ledger, 0, participants, {});
            ASSERT_TRUE(refused.StepInterior(source->get_id(), world, {.rain_mm = 2}).applied);
            EXPECT_EQ(source->water_mesh_invalidation_version, 7u);
            EXPECT_THROW(refused.PrepareGrid(source->get_id(), world, 3), std::invalid_argument);
            EXPECT_EQ(source->water_mesh_invalidation_version, 7u);
            EXPECT_TRUE(owner->water_mesh_generated.load());
            RegionalWater::Transaction stage;
            stage.Begin(ledger, 0, participants, {});
            ASSERT_TRUE(stage.StepInterior(source->get_id(), world, {.rain_mm = 2}).applied);
            std::unordered_set<ChunkID> invalidations;
            stage.Publish(nullptr, &invalidations);
            ASSERT_TRUE(invalidations.contains(source->get_id()));
            for (const auto id : invalidations) {
                water.retain_mesh_invalidation(id);
            }
        } else {
            ASSERT_TRUE(
                WaterKernels::StepInterior(*source, world, {.rain_mm = 2}).water_mesh_invalidated);
            water.retain_mesh_invalidation(source->get_id());
        }
        EXPECT_EQ(source->water_mesh_invalidation_version, 8u);
        // Only the source was queued. The shared scheduler boundary must wake
        // its already-generated owner before collecting water-only candidates.
        world.update(registry, std::vector<Vec3>{}, nullptr, false);
        world.wait_for_streaming_jobs();
        EXPECT_EQ(world.water_column_mesh_work().pending_visits - work_before.pending_visits, 1u);
        EXPECT_EQ(world.water_column_mesh_work().owner_visits - work_before.owner_visits, 2u);
        EXPECT_EQ(owner->water_mesh_version.load(), before + 1u);
        for (const auto& vertex : owner->water_mesh_vertices) {
            EXPECT_NEAR(vertex.position.y, 1.002f, 0.00001f);
        }
        const auto settled = owner->water_mesh_version.load();
        world.update(registry, std::vector<Vec3>{}, nullptr, false);
        world.wait_for_streaming_jobs();
        EXPECT_EQ(owner->water_mesh_version.load(), settled);
        ASSERT_TRUE(water.mesh_invalidations().empty());
        // The final two visits retire the completed source/owner requests.
        EXPECT_EQ(world.water_column_mesh_work().pending_visits - work_before.pending_visits, 3u);
        EXPECT_EQ(world.water_column_mesh_work().owner_visits - work_before.owner_visits, 2u);
        world.update(registry, std::vector<Vec3>{}, nullptr, false);
        world.wait_for_streaming_jobs();
        EXPECT_EQ(world.water_column_mesh_work().pending_visits - work_before.pending_visits, 3u);
        EXPECT_EQ(world.water_column_mesh_work().owner_visits - work_before.owner_visits, 2u);
    }
}

TEST(AsyncWaterMeshing, DisabledColumnSourceClearsRemoteOwnerAndRejectsPendingWater) {
    for (const bool pending : {false, true}) {
        SCOPED_TRACE(pending);
        JobSystem jobs;
        jobs.startup(1);
        auto params = Luminumbra::Test::SyntheticTerrainParams();
        params.base_amplitude = 0.0f;
        params.height_offset = -64.0f;
        params.caves_enabled = false;
        SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
        WaterSystem water(&jobs, &world);
        water.SetBootPaused(true);
        world.SetWaterSystem(&water);
        auto source = std::make_shared<Chunk>(IVec3(0));
        auto owner = std::make_shared<Chunk>(IVec3(0, 1, 0));
        for (const auto& chunk : {source, owner}) {
            world.GenerateChunkData(*chunk);
            chunk->current_lod.store(0);
            chunk->set_state(ChunkState::Ready);
            ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        }
        source->current_water_resolution.store(4);
        source->has_water_sim.store(true);
        source->water_level_data.assign(16u, 17.0f);
        source->water_depth_mm.assign(16u, 2000);
        entt::registry registry;
        world.update(registry, std::vector<Vec3>{}, nullptr, false);
        world.wait_for_streaming_jobs();
        ASSERT_EQ(owner->water_mesh_indices.size(), 96u);
        ASSERT_EQ(world.renderable_chunks_view(), std::vector<Chunk*>{owner.get()});
        const auto disable = [&] {
            source->has_water_sim.store(false);
            source->current_water_resolution.store(0);
            source->water_level_data.clear();
            ++source->water_mesh_invalidation_version;
            water.retain_mesh_invalidation(source->get_id());
            world.update(registry, std::vector<Vec3>{}, nullptr, false);
        };
        if (pending) {
            PendingMeshGate gate(jobs, world);
            owner->water_mesh_generated.store(false);
            water.retain_mesh_invalidation(owner->get_id());
            world.update(registry, std::vector<Vec3>{}, nullptr, false);
            ASSERT_EQ(owner->pending_lod.load(), 0);
            disable();
            EXPECT_TRUE(owner->water_mesh_indices.empty());
            gate.finish();
        } else {
            disable();
            world.wait_for_streaming_jobs();
        }
        EXPECT_TRUE(owner->water_mesh_vertices.empty());
        EXPECT_TRUE(owner->water_mesh_indices.empty());
        EXPECT_FALSE(owner->water_mesh_generated.load());
        EXPECT_TRUE(world.renderable_chunks_view().empty());
        EXPECT_EQ(owner->get_state(), ChunkState::Ready);
    }
}

} // namespace
