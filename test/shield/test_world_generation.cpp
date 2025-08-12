#include <gtest/gtest.h>
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/MarchingCubes.h"
#include <numeric>

using namespace Luminumbra;
using namespace Luminumbra::Systems;

class WorldGenerationTest : public ::testing::Test {
protected:
    void SetUp() override {
        params_guaranteed_surface.base_frequency = 0.01f;
        params_guaranteed_surface.base_amplitude = 50.0f;
        params_guaranteed_surface.octaves = 4;
        params_guaranteed_surface.persistence = 0.5f;
        params_guaranteed_surface.lacunarity = 2.0f;
        params_guaranteed_surface.height_offset = 16.0f;
        params_guaranteed_surface.caves_enabled = false;

        params_archipelago_solid.base_frequency = 0.009f;
        params_archipelago_solid.base_amplitude = 10.0f;
        params_archipelago_solid.octaves = 5;
        params_archipelago_solid.persistence = 0.5f;
        params_archipelago_solid.lacunarity = 2.0f;
        params_archipelago_solid.height_offset = -20.0f;
        params_archipelago_solid.caves_enabled = false;
    }

    TerrainGenParams params_guaranteed_surface;
    TerrainGenParams params_archipelago_solid;
};

TEST_F(WorldGenerationTest, ChunkIsGeneratedWithSDF) {
    SHIELD_WorldSystem world_system(nullptr, params_guaranteed_surface, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);
    
    // FIX: Added the 4th argument (step = 1 for full detail)
    World::MarchingCubes::PolygoniseChunk(world_system, chunk, 0.0f, 1);

    ASSERT_FALSE(chunk.sdf_data.empty()) << "SDF data should not be empty after generation.";
    constexpr size_t PADDED_VOLUME = (CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    ASSERT_EQ(chunk.sdf_data.size(), PADDED_VOLUME) << "SDF data vector has an incorrect size.";
}

TEST_F(WorldGenerationTest, SurfaceIsGeneratedWithinChunk) {
    SHIELD_WorldSystem world_system(nullptr, params_guaranteed_surface, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);

    bool has_positive_sdf = false;
    bool has_negative_sdf = false;
    for (const auto& sdf_value : chunk.sdf_data) {
        if (sdf_value > 0.0f) has_positive_sdf = true;
        else if (sdf_value <= 0.0f) has_negative_sdf = true;
        if (has_positive_sdf && has_negative_sdf) break;
    }
    ASSERT_TRUE(has_positive_sdf && has_negative_sdf)
        << "A valid surface requires both positive (air) and negative (solid) SDF values in the chunk.";
}

TEST_F(WorldGenerationTest, MeshingProducesNonEmptyVertexBuffer) {
    SHIELD_WorldSystem world_system(nullptr, params_guaranteed_surface, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);

    // FIX: Added the 4th argument (step = 1 for full detail)
    World::MarchingCubes::PolygoniseChunk(world_system, chunk, 0.0f, 1);

    ASSERT_FALSE(chunk.mesh_vertices.empty())
        << "The vertex buffer should not be empty after meshing a valid surface.";
    ASSERT_FALSE(chunk.mesh_indices.empty())
        << "The index buffer should not be empty after meshing a valid surface.";
    ASSERT_EQ(chunk.mesh_indices.size() % 3, 0)
        << "The number of indices must be a multiple of 3 to form valid triangles.";
}

TEST_F(WorldGenerationTest, NormalsPointUpwardsOnHorizontalSurface) {
    SHIELD_WorldSystem world_system(nullptr, params_guaranteed_surface, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);
    
    // FIX: Added the 4th argument (step = 1 for full detail)
    World::MarchingCubes::PolygoniseChunk(world_system, chunk, 0.0f, 1);

    ASSERT_FALSE(chunk.mesh_vertices.empty()) << "Cannot test normals on an empty mesh.";

    Vec3 total_normal(0.0f);
    for (const auto& vertex : chunk.mesh_vertices) {
        total_normal += vertex.normal;
    }
    Vec3 average_normal = glm::normalize(total_normal);
    EXPECT_GT(average_normal.y, 0.7f) 
        << "The average normal vector of the mesh is not pointing upwards, indicating inverted normals.";
}

TEST_F(WorldGenerationTest, GenerationIsDeterministicWithSameSeed) {
    SHIELD_WorldSystem world_system_A(nullptr, params_guaranteed_surface, 42);
    Chunk chunk_A({1, 2, 3});
    world_system_A.GenerateChunkData(chunk_A);

    SHIELD_WorldSystem world_system_B(nullptr, params_guaranteed_surface, 42);
    Chunk chunk_B({1, 2, 3});
    world_system_B.GenerateChunkData(chunk_B);

    ASSERT_FALSE(chunk_A.sdf_data.empty());
    ASSERT_EQ(chunk_A.sdf_data.size(), chunk_B.sdf_data.size());
    EXPECT_EQ(chunk_A.sdf_data, chunk_B.sdf_data)
        << "SDF data for two chunks with the same seed and coordinates should be identical.";
}

TEST_F(WorldGenerationTest, KnownSolidPresetGeneratesEmptyMesh) {
    SHIELD_WorldSystem world_system(nullptr, params_archipelago_solid, 1337);
    Chunk chunk({0, 0, 0});
    world_system.GenerateChunkData(chunk);
    
    // FIX: Added the 4th argument (step = 1 for full detail)
    World::MarchingCubes::PolygoniseChunk(world_system, chunk, 0.0f, 1);

    EXPECT_TRUE(chunk.mesh_vertices.empty())
        << "The 'archipelago_solid' preset was expected to produce an empty vertex buffer.";
    EXPECT_TRUE(chunk.mesh_indices.empty())
        << "The 'archipelago_solid' preset was expected to produce an empty index buffer.";
}