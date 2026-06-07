#pragma once

// Forward declarations
namespace Luminumbra { class Chunk; }
namespace Luminumbra::Systems { 
    class SHIELD_WorldSystem; 
    class WaterSystem; 
}

namespace Luminumbra::World::MarchingCubes {
    
    /**
     * @brief Generates a terrain mesh for a chunk using the Marching Cubes algorithm on its SDF data.
     * @param world_system A const reference to the world system for querying terrain parameters.
     * @param chunk The chunk to generate the mesh for. Its mesh_vertices and mesh_indices will be populated.
     * @param isolevel The density value that represents the surface (typically 0.0).
     * @param step The step size for the algorithm, used for LOD (1 = full detail).
     */
    void PolygoniseTerrain(
        const Systems::SHIELD_WorldSystem& world_system, 
        Chunk& chunk, 
        float isolevel, 
        int step
    );

    /**
     * @brief Generates a water surface mesh for a chunk based on its water simulation data.
     * @param water_system A const reference to the water system for querying water levels.
     * @param world_system A const reference to the world system for querying terrain height.
     * @param chunk The chunk to generate the water mesh for. Its water_mesh_vertices and water_mesh_indices will be populated.
     */
    void GenerateWaterMesh(
        const Systems::WaterSystem& water_system,
        const Systems::SHIELD_WorldSystem& world_system,
        Chunk& chunk
    );

} // namespace Luminumbra::World::MarchingCubes