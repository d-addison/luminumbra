#pragma once

#include <cstddef>
#include <cstdint>

// Forward declarations
namespace Luminumbra { class Chunk; }
namespace Luminumbra::Systems { 
    class SHIELD_WorldSystem; 
    class WaterSystem; 
}

namespace Luminumbra::World::MarchingCubes {
    struct TerrainMeshBuildStats {
        std::size_t jobs = 0;
        std::size_t step1_jobs = 0;
        std::size_t step2_jobs = 0;
        std::size_t step4_jobs = 0;
        std::size_t cells_visited = 0;
        std::size_t active_cells = 0;
        std::size_t vertices = 0;
        std::size_t indices = 0;
        std::size_t triangles = 0;
        std::uint64_t elapsed_us = 0;
    };

    void ResetTerrainMeshBuildStats();
    TerrainMeshBuildStats GetTerrainMeshBuildStats();
    
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

    enum TerrainTransitionFace : std::uint8_t {
        TransitionFaceMinX = 1u << 0,
        TransitionFaceMaxX = 1u << 1,
        TransitionFaceMinZ = 1u << 2,
        TransitionFaceMaxZ = 1u << 3,
    };

    using TerrainTransitionFaceMask = std::uint8_t;

    constexpr TerrainTransitionFaceMask kNoTransitionFaces = 0u;
    constexpr TerrainTransitionFaceMask kAllHorizontalTransitionFaces =
        TransitionFaceMinX | TransitionFaceMaxX | TransitionFaceMinZ | TransitionFaceMaxZ;

    struct TerrainTransitionSkirtStats {
        std::size_t boundary_edges = 0;
        std::size_t vertices_added = 0;
        std::size_t indices_added = 0;
        std::size_t triangles_added = 0;
    };

    /**
     * @brief Adds downward boundary skirts to coarse terrain chunk faces that touch finer LOD neighbors.
     * @param chunk The chunk whose mesh will be extended in-place.
     * @param step The terrain meshing step for this chunk. Step 1 chunks do not receive skirts.
     * @param faces Bitmask of horizontal chunk faces that need transition coverage.
     */
    TerrainTransitionSkirtStats AddBoundaryTransitionSkirts(
        Chunk& chunk,
        int step,
        TerrainTransitionFaceMask faces = kAllHorizontalTransitionFaces
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
