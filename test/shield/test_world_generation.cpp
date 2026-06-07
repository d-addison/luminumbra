#include "gtest/gtest.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"
#include "systems/WaterSystem.h"

#include <chrono>   // For performance testing
#include <numeric>  // For std::accumulate
#include <iostream> // For printing benchmark results
#include <memory>   // For std::unique_ptr

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
        params_guaranteed_surface.caves_enabled = false;

        // A realistic preset that generates complex terrain within chunk bounds
        // Assuming CHUNK_SIZE_Y is 16 or 32, place terrain in the middle
        params_archipelago.base_frequency = 0.004f;
        params_archipelago.base_amplitude = 8.0f;  // Reduced from 120 to keep within chunk
        params_archipelago.octaves = 6;
        params_archipelago.persistence = 0.5f;
        params_archipelago.lacunarity = 2.2f;
        params_archipelago.height_offset = 8.0f;  // Changed from 30 to be within chunk (0,0,0)
        params_archipelago.island_mask_enabled = false;
        params_archipelago.caves_enabled = false;

        // A preset that generates a low, relatively flat surface guaranteed to be below sea level.
        params_underwater_world.height_offset = -20.0f;
        params_underwater_world.base_amplitude = 5.0f;
        params_underwater_world.caves_enabled = false;
        
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
        world_system = std::make_unique<SHIELD_WorldSystem>(nullptr, nullptr, params_underwater_world, 1337);
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
    const size_t expected_sdf_size = (size_t)(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    ASSERT_EQ(chunk.sdf_data.size(), expected_sdf_size);
    ASSERT_FALSE(chunk.heightmap_data.empty());
    const size_t expected_heightmap_size = (size_t)(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Z + 1);
    ASSERT_EQ(chunk.heightmap_data.size(), expected_heightmap_size);
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

TEST_F(WorldGenerationTest, HeightmapIsCorrectForFlatSurface) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params_guaranteed_surface, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);
    ASSERT_FALSE(chunk.heightmap_data.empty());
    for(float height : chunk.heightmap_data) {
        EXPECT_NEAR(height, params_guaranteed_surface.height_offset, 1e-6f);
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

TEST_F(WorldGenerationTest, CavesChangeGeneratedMesh) {
    TerrainGenParams params_no_caves = params_archipelago;
    params_no_caves.caves_enabled = false;
    // Lower the terrain to ensure it intersects with chunk at (0,0,0)
    params_no_caves.height_offset = 8.0f;  // Place terrain in middle of chunk
    params_no_caves.base_amplitude = 4.0f; // Reduce variation to keep it in bounds

    TerrainGenParams params_with_caves = params_archipelago;
    params_with_caves.caves_enabled = true;
    params_with_caves.height_offset = 8.0f;  // Same as no caves
    params_with_caves.base_amplitude = 4.0f; // Same as no caves
    params_with_caves.cave_threshold = 0.4f;  // Lower threshold = more caves
    params_with_caves.cave_frequency = 0.04f; // Higher frequency = more caves
    
    int seed = 12345;

    SHIELD_WorldSystem world_no_caves(nullptr, nullptr, params_no_caves, seed);
    Chunk chunk_no_caves({0,0,0});
    world_no_caves.GenerateChunkData(chunk_no_caves);
    
    // Debug: Check if the SDF data has both positive and negative values
    bool has_positive = false;
    bool has_negative = false;
    for (float val : chunk_no_caves.sdf_data) {
        if (val > 0) has_positive = true;
        if (val < 0) has_negative = true;
    }
    ASSERT_TRUE(has_positive && has_negative) << "Chunk SDF should have surface crossing (both + and - values)";
    
    World::MarchingCubes::PolygoniseTerrain(world_no_caves, chunk_no_caves, 0.0f, 1);
    
    SHIELD_WorldSystem world_caves(nullptr, nullptr, params_with_caves, seed);
    Chunk chunk_caves({0,0,0});
    world_caves.GenerateChunkData(chunk_caves);
    World::MarchingCubes::PolygoniseTerrain(world_caves, chunk_caves, 0.0f, 1);

    ASSERT_FALSE(chunk_no_caves.mesh_vertices.empty()) << "No-caves chunk should have vertices";
    ASSERT_FALSE(chunk_caves.mesh_vertices.empty()) << "Caves chunk should have vertices";
    EXPECT_NE(chunk_caves.mesh_vertices.size(), chunk_no_caves.mesh_vertices.size());
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
    for(const auto& vertex : chunk.mesh_vertices) {
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

TEST_F(WorldAndWaterTest, WaterMeshNormalsPointUp) {
    Chunk chunk({0, 0, 0});
    world_system->GenerateChunkData(chunk);
    chunk.water_level_data.assign(WATER_SIM_RESOLUTION_X * WATER_SIM_RESOLUTION_Z, SEA_LEVEL);
    chunk.has_water_sim.store(true);
    World::MarchingCubes::GenerateWaterMesh(*water_system, *world_system, chunk);
    ASSERT_FALSE(chunk.water_mesh_vertices.empty());
    for(const auto& vertex : chunk.water_mesh_vertices) {
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
    std::cout << "\n[  PERF  ] GenerateChunkData (" << num_chunks_to_test << " chunks): "
              << duration_ms.count() << " ms total, "
              << avg_time_ms << " ms average." << std::endl;
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
    std::cout << "\n[  PERF  ] PolygoniseTerrain (" << num_iterations << " iterations): "
              << total_time_ms << " ms total, "
              << avg_time_ms << " ms average." << std::endl;
    EXPECT_LT(avg_time_ms, 30.0);
}