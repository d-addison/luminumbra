#include <gtest/gtest.h>
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"

TEST(WorldGenerationTest, ChunkIsGeneratedWithSDF) {
    Luminumbra::Systems::TerrainGenParams params; // Create a default set of terrain parameters.
    int seed = 1337;                              // Use a fixed seed for a predictable test.

    // --- FIX: Call the constructor with all three arguments ---
    // The JobSystem can be nullptr for this test if you are only testing data generation.
    Luminumbra::Systems::SHIELD_WorldSystem worldSystem(nullptr, params, seed);

    // --- EXECUTE: Run the function you want to test ---
    Luminumbra::Chunk chunk({0, 0, 0});
    worldSystem.GenerateChunkData(chunk);

    bool hasNonZeroSDF = false;
    for (const auto& val : chunk.sdf_data) {
        if (val != 0.0f) {
            hasNonZeroSDF = true;
            break;
        }
    }

    ASSERT_EQ(chunk.get_coords(), Luminumbra::IVec3(0, 0, 0));
    ASSERT_FALSE(chunk.sdf_data.empty());
    ASSERT_TRUE(hasNonZeroSDF);
}
