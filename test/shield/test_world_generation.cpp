#include "gtest/gtest.h"
#include <chrono> // For performance testing
#include <functional>
#include <iostream> // For printing benchmark results
#include <memory>   // For std::unique_ptr
#include <numeric>  // For std::accumulate
#include <unordered_map>
#include <vector>

#include "FastNoise/FastNoise.h"
#include "core/JobSystem.h"
#include "entt/entt.hpp"
#include "world/Chunk.h"

#include "systems/SHIELD_WorldSystem.h"

#include "systems/WaterSystem.h"
#include "world/MarchingCubes.h"

using namespace Luminumbra;
using namespace Luminumbra::Systems;

// =====================================================================================
// TEST FIXTURES
// =====================================================================================

/**
 * @brief Base test fixture for world generation tests.
 *
 * Sets up several distinct terrain generation parameter presets to test various scenarios:
 * - A guaranteed flat surface for predictable geometry tests.
 * - A complex archipelago preset for more realistic data tests.
 * - A preset guaranteed to be deep underwater.
 * - A preset guaranteed to be high in the air.
 */
class WorldGenerationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // A preset that should always generate a solid flat surface at y=8.0
        params_guaranteed_surface.base_amplitude = 0;
        params_guaranteed_surface.height_offset = 8.0f;

        // A realistic preset that generates complex terrain within chunk bounds
        // Assuming CHUNK_SIZE_Y is 16 or 32, place terrain in the middle
        params_archipelago.base_frequency = 0.004f;
        params_archipelago.base_amplitude = 8.0f; // Reduced from 120 to keep within chunk
        params_archipelago.octaves = 6;
        params_archipelago.persistence = 0.5f;
        params_archipelago.lacunarity = 2.2f;
        params_archipelago.height_offset = 8.0f; // Changed from 30 to be within chunk (0,0,0)

        // A preset that generates a low, relatively flat surface guaranteed to be below sea level.
        params_underwater_world.height_offset = -20.0f;
        params_underwater_world.base_amplitude = 5.0f;

        // A preset that generates a surface high in the air, guaranteed to be above sea level.
        params_sky_world.height_offset = 100.0f;
        params_sky_world.base_amplitude = 10.0f;
    }

    TerrainGenParams params_guaranteed_surface;
    TerrainGenParams params_archipelago;
    TerrainGenParams params_underwater_world;
    TerrainGenParams params_sky_world;
};

/**
 * @brief Test fixture specifically for tests involving the WaterSystem.
 */
class WorldAndWaterTest : public WorldGenerationTest {
protected:
    void SetUp() override {
        WorldGenerationTest::SetUp();
        world_system =
            std::make_unique<SHIELD_WorldSystem>(nullptr, nullptr, params_underwater_world, 1337);
        water_system = std::make_unique<WaterSystem>(nullptr, world_system.get());
        world_system->SetWaterSystem(water_system.get());
    }

    std::unique_ptr<SHIELD_WorldSystem> world_system;
    std::unique_ptr<WaterSystem> water_system;
};

// =====================================================================================
// CORE FUNCTIONALITY TESTS
// =====================================================================================

TEST_F(WorldGenerationTest, ChunkIsGeneratedWithSDFAndHeightmap) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_guaranteed_surface, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);
    ASSERT_FALSE(chunk.sdf_data.empty());
    const size_t expected_sdf_size =
        (size_t)(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    ASSERT_EQ(chunk.sdf_data.size(), expected_sdf_size);
    ASSERT_FALSE(chunk.heightmap_data.empty());
    const size_t expected_heightmap_size = (size_t)(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Z + 1);
    ASSERT_EQ(chunk.heightmap_data.size(), expected_heightmap_size);
}

TEST_F(WorldGenerationTest, UpdateSkipsMeshingChunksStillLoading) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_guaranteed_surface, 1337);
    const IVec3 coords{0, 0, 0};

    world_system.dispatch_generation_jobs({coords});

    auto chunk = world_system.find_streamed_chunk(coords);
    ASSERT_NE(chunk, nullptr);
    ASSERT_EQ(chunk->get_state(), ChunkState::Loading);

    entt::registry registry;
    world_system.update(registry, Vec3{0.0f, 0.0f, 0.0f}, nullptr);

    EXPECT_EQ(chunk->get_state(), ChunkState::Loading);
    EXPECT_TRUE(chunk->mesh_vertices.empty());
    EXPECT_TRUE(chunk->mesh_indices.empty());
}

TEST_F(WorldGenerationTest, SurfaceIsGeneratedAtCorrectHeight) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_guaranteed_surface, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);
    float inside_density = world_system.get_density_at({8, 4, 8});
    float outside_density = world_system.get_density_at({8, 12, 8});
    EXPECT_LT(inside_density, 0.0f);
    EXPECT_GT(outside_density, 0.0f);
}

TEST_F(WorldGenerationTest, HeightmapMatchesModernSurface) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_guaranteed_surface, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);
    ASSERT_FALSE(chunk.heightmap_data.empty());
    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
            EXPECT_FLOAT_EQ(
                chunk.heightmap_data[x + z * (CHUNK_SIZE_X + 1)],
                world_system.GetTerrainHeightAt(static_cast<float>(x), static_cast<float>(z)));
        }
    }
}

TEST_F(WorldGenerationTest, GenerationIsDeterministicWithSameSeed) {
    SHIELD_WorldSystem world_system_A(nullptr, nullptr, params_archipelago, 42);
    Chunk chunk_A({1, 2, 3});
    world_system_A.GenerateChunkData(chunk_A);
    SHIELD_WorldSystem world_system_B(nullptr, nullptr, params_archipelago, 42);
    Chunk chunk_B({1, 2, 3});
    world_system_B.GenerateChunkData(chunk_B);
    ASSERT_EQ(chunk_A.sdf_data.size(), chunk_B.sdf_data.size());
    EXPECT_EQ(chunk_A.sdf_data, chunk_B.sdf_data);
    ASSERT_EQ(chunk_A.heightmap_data.size(), chunk_B.heightmap_data.size());
    EXPECT_EQ(chunk_A.heightmap_data, chunk_B.heightmap_data);
}

TEST_F(WorldGenerationTest, CaveTuningChangesGeneratedMesh) {
    TerrainGenParams baseline_params = params_guaranteed_surface;

    baseline_params.height_offset = 40.0f;

    TerrainGenParams params_with_caves = baseline_params;

    params_with_caves.cave_threshold = 0.55f;
    params_with_caves.cave_frequency = 0.15f;
    params_with_caves.cave_carve_value = 4.0f;

    int seed = 12345;

    SHIELD_WorldSystem baseline_world(nullptr, nullptr, baseline_params, seed);
    Chunk baseline_chunk({0, 0, 0});
    baseline_world.GenerateChunkData(baseline_chunk);
    World::MarchingCubes::PolygoniseTerrain(baseline_world, baseline_chunk, 0.0f, 1);

    SHIELD_WorldSystem world_caves(nullptr, nullptr, params_with_caves, seed);
    Chunk chunk_caves({0, 0, 0});
    world_caves.GenerateChunkData(chunk_caves);
    World::MarchingCubes::PolygoniseTerrain(world_caves, chunk_caves, 0.0f, 1);

    size_t carved_air_samples = 0;
    size_t remaining_solid_samples = 0;
    ASSERT_EQ(baseline_chunk.sdf_data.size(), chunk_caves.sdf_data.size());
    for (size_t i = 0; i < baseline_chunk.sdf_data.size(); ++i) {
        if (baseline_chunk.sdf_data[i] < 0.0f && chunk_caves.sdf_data[i] > 0.0f) {
            carved_air_samples++;
        }
        if (chunk_caves.sdf_data[i] < 0.0f) {
            remaining_solid_samples++;
        }
    }

    ASSERT_FALSE(chunk_caves.mesh_vertices.empty()) << "Caves chunk should have vertices";
    ASSERT_GT(carved_air_samples, 0u) << "Caves should carve solid SDF samples into air";
    ASSERT_GT(remaining_solid_samples, 0u)
        << "Caves should not erase the entire solid terrain volume";
    EXPECT_NE(chunk_caves.mesh_vertices.size(), baseline_chunk.mesh_vertices.size());
}

TEST_F(WorldGenerationTest, CavesDoNotPunchThroughSurfaceCap) {
    TerrainGenParams params_with_forced_caves = params_guaranteed_surface;
    params_with_forced_caves.height_offset = 40.0f;

    params_with_forced_caves.cave_threshold = -1.0f;
    params_with_forced_caves.cave_frequency = 0.15f;
    params_with_forced_caves.cave_carve_value = 4.0f;

    SHIELD_WorldSystem world_system(nullptr, nullptr, params_with_forced_caves, 12345);
    EXPECT_LT(world_system.get_density_at({8.0f, 39.0f, 8.0f}), 0.0f)
        << "surface cap should remain solid one meter below the heightfield";
    EXPECT_LT(world_system.get_density_at({8.0f, 24.0f, 8.0f}), 0.0f)
        << "surface cap should remain solid sixteen meters below the heightfield";
    EXPECT_GT(world_system.get_density_at({8.0f, 0.0f, 8.0f}), 0.0f)
        << "deep caves should still carve interior terrain";
}

// =====================================================================================
// MESHING TESTS (TERRAIN)
// =====================================================================================

TEST_F(WorldGenerationTest, MeshingProducesNonEmptyVertexBuffer) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_guaranteed_surface, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, 1);
    ASSERT_FALSE(chunk.mesh_vertices.empty());
    ASSERT_FALSE(chunk.mesh_indices.empty());
}

TEST_F(WorldGenerationTest, TerrainMeshUsesRenderableMaterials) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_guaranteed_surface, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, 1);

    ASSERT_FALSE(chunk.mesh_vertices.empty());
    for (const auto& vertex : chunk.mesh_vertices) {
        EXPECT_NE(vertex.material_id, static_cast<u32>(MaterialType::Air));
        EXPECT_NE(vertex.material_id, static_cast<u32>(MaterialType::Water));
    }
}

TEST_F(WorldGenerationTest, KnownEmptyChunkGeneratesEmptyMesh) {
    TerrainGenParams params_deep_ocean = params_archipelago;
    params_deep_ocean.height_offset = -80.0f; // Ensure chunk volume is above terrain
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_deep_ocean, 1337);
    Chunk chunk({0, 0, 0}); // Chunk at y=0 will be air
    world_system.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, 1);
    ASSERT_TRUE(chunk.mesh_vertices.empty());
    ASSERT_TRUE(chunk.mesh_indices.empty());
}

TEST_F(WorldGenerationTest, NormalsPointUpwardsOnHorizontalSurface) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_guaranteed_surface, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, 1);
    ASSERT_FALSE(chunk.mesh_vertices.empty());
    for (const auto& vertex : chunk.mesh_vertices) {
        EXPECT_NEAR(vertex.normal.y, 1.0f, 1e-5);
        EXPECT_NEAR(vertex.normal.x, 0.0f, 1e-5);
        EXPECT_NEAR(vertex.normal.z, 0.0f, 1e-5);
    }
}

// This test should now pass because params_archipelago is fixed in SetUp.
TEST_F(WorldGenerationTest, LOD_MeshingReducesVertexCount) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_archipelago, 42);
    Chunk chunk_lod0({0, 0, 0});
    world_system.GenerateChunkData(chunk_lod0);
    World::MarchingCubes::PolygoniseTerrain(world_system, chunk_lod0, 0.0f, 1);
    Chunk chunk_lod2({0, 0, 0});
    chunk_lod2.sdf_data = chunk_lod0.sdf_data;
    World::MarchingCubes::PolygoniseTerrain(world_system, chunk_lod2, 0.0f, 4);
    ASSERT_FALSE(chunk_lod0.mesh_vertices.empty());
    ASSERT_FALSE(chunk_lod2.mesh_vertices.empty());
    EXPECT_LT(chunk_lod2.mesh_vertices.size(), chunk_lod0.mesh_vertices.size());
    EXPECT_LT(chunk_lod2.mesh_indices.size(), chunk_lod0.mesh_indices.size());
}

TEST_F(WorldGenerationTest, LOD_MeshingPreservesUpwardTriangleWindingOnHorizontalSurface) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_guaranteed_surface, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, 4);

    ASSERT_FALSE(chunk.mesh_vertices.empty());
    ASSERT_FALSE(chunk.mesh_indices.empty());
    ASSERT_EQ(chunk.mesh_indices.size() % 3, 0u);

    for (size_t i = 0; i < chunk.mesh_indices.size(); i += 3) {
        ASSERT_LT(chunk.mesh_indices[i], chunk.mesh_vertices.size());
        ASSERT_LT(chunk.mesh_indices[i + 1], chunk.mesh_vertices.size());
        ASSERT_LT(chunk.mesh_indices[i + 2], chunk.mesh_vertices.size());

        const auto& v0 = chunk.mesh_vertices[chunk.mesh_indices[i]];
        const auto& v1 = chunk.mesh_vertices[chunk.mesh_indices[i + 1]];
        const auto& v2 = chunk.mesh_vertices[chunk.mesh_indices[i + 2]];

        const Vec3 edge_a = v1.position - v0.position;
        const Vec3 edge_b = v2.position - v0.position;
        const float face_normal_y = edge_a.z * edge_b.x - edge_a.x * edge_b.z;

        EXPECT_GT(face_normal_y, 0.0f) << "LOD triangle " << (i / 3) << " has downward winding";
    }
}

// =====================================================================================
// MESHING TESTS (WATER)
// =====================================================================================

TEST_F(WorldAndWaterTest, WaterMeshIsGeneratedForSubmergedChunk) {
    Chunk chunk({0, 0, 0});
    world_system->GenerateChunkData(chunk);
    chunk.water_level_data.assign(WATER_SIM_RESOLUTION_X * WATER_SIM_RESOLUTION_Z, SEA_LEVEL);
    chunk.has_water_sim.store(true);
    World::MarchingCubes::GenerateWaterMesh(*water_system, *world_system, chunk);
    ASSERT_FALSE(chunk.water_mesh_vertices.empty());
    ASSERT_FALSE(chunk.water_mesh_indices.empty());
}

TEST_F(WorldAndWaterTest, WaterMeshIsEmptyForHighAltitudeChunk) {
    world_system->set_params(params_sky_world);
    Chunk chunk({0, 10, 0});
    world_system->GenerateChunkData(chunk);
    chunk.water_level_data.assign(WATER_SIM_RESOLUTION_X * WATER_SIM_RESOLUTION_Z, SEA_LEVEL);
    chunk.has_water_sim.store(true);
    World::MarchingCubes::GenerateWaterMesh(*water_system, *world_system, chunk);
    ASSERT_TRUE(chunk.water_mesh_vertices.empty());
    ASSERT_TRUE(chunk.water_mesh_indices.empty());
}

TEST_F(WorldAndWaterTest, DryHighAltitudeCellsStayDryAfterSimulation) {
    Luminumbra::JobSystem jobs;
    jobs.startup();

    SHIELD_WorldSystem world(&jobs, nullptr, params_sky_world, 1337);
    WaterSystem water(&jobs, &world);
    world.SetWaterSystem(&water);

    auto chunk = std::make_shared<Chunk>(IVec3{0, 6, 0});
    std::unordered_map<ChunkID, std::shared_ptr<Chunk>> active_chunks;
    active_chunks.emplace(chunk->get_id(), chunk);

    entt::registry registry;
    water.update(registry, active_chunks);

    ASSERT_TRUE(chunk->has_water_sim.load());
    ASSERT_FALSE(chunk->water_level_data.empty());
    //  contract refresh (, 2026-07-03):  made the integer
    // DEPTH the authoritative dryness (water_depth_mm == 0) and repurposed the
    // float water_level_data as the resting SURFACE mirror (bed + depth — i.e.
    // ~terrain height on dry land), seeded from WaterLevelAt at init
    // (WaterSystem.cpp seed_chunk_water). The old assertion pinned the
    // pre- sentinel convention (dry == SEA_LEVEL) and went RED the day
    // the new seeding landed — this asserts what "dry" actually means now:
    // zero standing depth, and a surface that never rises above the bed.
    ASSERT_EQ(chunk->water_depth_mm.size(), chunk->water_level_data.size());
    ASSERT_EQ(chunk->water_sim_terrain_height.size(), chunk->water_level_data.size());
    for (std::size_t i = 0; i < chunk->water_level_data.size(); ++i) {
        EXPECT_EQ(chunk->water_depth_mm[i], 0)
            << "high-altitude cell " << i << " gained standing water";
        EXPECT_LE(chunk->water_level_data[i], chunk->water_sim_terrain_height[i] + 0.002f)
            << "cell " << i << " surface mirror rises above its bed (renders wet)";
    }

    jobs.shutdown();
}

TEST_F(WorldAndWaterTest, WaterMeshNormalsPointUp) {
    Chunk chunk({0, 0, 0});
    world_system->GenerateChunkData(chunk);
    chunk.water_level_data.assign(WATER_SIM_RESOLUTION_X * WATER_SIM_RESOLUTION_Z, SEA_LEVEL);
    chunk.has_water_sim.store(true);
    World::MarchingCubes::GenerateWaterMesh(*water_system, *world_system, chunk);
    ASSERT_FALSE(chunk.water_mesh_vertices.empty());
    for (const auto& vertex : chunk.water_mesh_vertices) {
        EXPECT_NEAR(vertex.normal.y, 1.0f, 1e-5);
        EXPECT_NEAR(vertex.normal.x, 0.0f, 1e-5);
        EXPECT_NEAR(vertex.normal.z, 0.0f, 1e-5);
    }
}

// =====================================================================================
// PERFORMANCE TESTS
// =====================================================================================

TEST_F(WorldGenerationTest, Performance_GenerateChunkData) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_archipelago, 1337);
    const int num_chunks_to_test = 100;
    std::vector<std::unique_ptr<Chunk>> chunks;
    chunks.reserve(num_chunks_to_test);
    for (int i = 0; i < num_chunks_to_test; ++i) {
        chunks.push_back(std::make_unique<Chunk>(IVec3(i, 0, 0)));
    }
    auto start = std::chrono::high_resolution_clock::now();
    for (auto& chunk_ptr : chunks) {
        world_system.GenerateChunkData(*chunk_ptr);
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration_ms = end - start;
    double avg_time_ms = duration_ms.count() / num_chunks_to_test;
    std::cout << "\n[  PERF  ] GenerateChunkData (" << num_chunks_to_test
              << " chunks): " << duration_ms.count() << " ms total, " << avg_time_ms
              << " ms average." << std::endl;
    EXPECT_LT(avg_time_ms, 50.0);
}

// This test should now pass because params_archipelago is fixed in SetUp.
TEST_F(WorldGenerationTest, Performance_PolygoniseTerrain) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_archipelago, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, 1);
    ASSERT_FALSE(chunk.mesh_vertices.empty()) << "Cannot benchmark meshing on an empty chunk.";
    const int num_iterations = 50;
    std::vector<double> timings;
    timings.reserve(num_iterations);
    for (int i = 0; i < num_iterations; ++i) {
        chunk.mesh_vertices.clear();
        chunk.mesh_indices.clear();
        auto start = std::chrono::high_resolution_clock::now();
        World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, 1);
        auto end = std::chrono::high_resolution_clock::now();
        timings.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    double total_time_ms = std::accumulate(timings.begin(), timings.end(), 0.0);
    double avg_time_ms = total_time_ms / num_iterations;
    std::cout << "\n[  PERF  ] PolygoniseTerrain (" << num_iterations
              << " iterations): " << total_time_ms << " ms total, " << avg_time_ms << " ms average."
              << std::endl;
    EXPECT_LT(avg_time_ms, 30.0);
}
