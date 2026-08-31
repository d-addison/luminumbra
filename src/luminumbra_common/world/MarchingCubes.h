#pragma once

#include "../../../include/luminumbra/core/Types.h"

#include <cstddef>
#include <cstdint>

// Forward declarations
namespace Luminumbra {
class Chunk;
}
namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
class WaterSystem;
} // namespace Luminumbra::Systems
namespace Luminumbra::World {
struct FarLodTile;
struct FarLodRegionMesh;
struct FarLodRegionSdfAssembly;
} // namespace Luminumbra::World

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
 * @param chunk The chunk to generate the mesh for. Its mesh_vertices and mesh_indices will be
 * populated.
 * @param isolevel The density value that represents the surface (typically 0.0).
 * @param step The cell stride used for LOD (1 = full detail). An exact
 * full SDF lattice is polygonised at this stride; only an empty SDF at a
 * coarse stride uses the heightfield fallback.
 */
void PolygoniseTerrain(const Systems::SHIELD_WorldSystem& world_system,
                       Chunk& chunk,
                       float isolevel,
                       int step);

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
 * @brief Adds downward boundary skirts to coarse terrain chunk faces that touch finer LOD
 * neighbors.
 * @param chunk The chunk whose mesh will be extended in-place.
 * @param step The terrain meshing step for this chunk. Step 1 chunks do not receive skirts.
 * @param faces Bitmask of horizontal chunk faces that need transition coverage.
 */
TerrainTransitionSkirtStats AddBoundaryTransitionSkirts(
    Chunk& chunk, int step, TerrainTransitionFaceMask faces = kAllHorizontalTransitionFaces);

/**
 * @brief Terrain material at the surface of a column, exactly as the
 * coarse heightfield chunk mesher classifies it (sampled just below the
 * surface so isosurface interpolation can never land on Air). Exposed for
 * the far-LOD tile builder so the far field matches the live
 * field at the seam.
 */
MaterialType TerrainSurfaceMaterialAt(const Systems::SHIELD_WorldSystem& world_system,
                                      float world_x,
                                      float world_z,
                                      float terrain_height);

struct FarLodRegionMeshStats {
    std::size_t vertices = 0;
    std::size_t indices = 0;
    std::size_t triangles = 0;
    std::size_t skirt_quads = 0;
};

/**
 * @brief Hybrid far-LOD region mesher. Tiles without SDF bricks retain the
 * original byte-stable whole-region heightfield path. A tile with aligned
 * SDF bricks promotes an authoritative column only when its supplied
 * one-column halo is a complete, matching brick stack; remaining cells
 * stay on the heightfield path, so the two representations never own the
 * same cell. Malformed, incomplete, or mismatched brick streams fail
 * closed and leave the output mesh empty.
 * Vertex positions are region-local in X/Z and absolute in Y; VoxelVertex
 * layout is untouched (28 bytes). Tile border samples are shared with
 * adjacent regions, and the perimeter skirt masks tier-boundary cracks.
 */
FarLodRegionMeshStats GenerateFarLodRegionMesh(const World::FarLodTile& tile,
                                               World::FarLodRegionMesh& out_mesh);

// Cross-region SDF variant.  The background remains the requested home
// tile, while all SDF reads use the owned world-coordinate assembly.
FarLodRegionMeshStats GenerateFarLodRegionMesh(const World::FarLodTile& tile,
                                               const World::FarLodRegionSdfAssembly& assembly,
                                               World::FarLodRegionMesh& out_mesh);

/**
 * @brief Generates a water surface mesh for a chunk based on its water simulation data.
 * @param water_system A const reference to the water system for querying water levels.
 * @param world_system A const reference to the world system for querying terrain height.
 * @param chunk The chunk to generate the water mesh for. Its water_mesh_vertices and
 * water_mesh_indices will be populated.
 */
void GenerateWaterMesh(const Systems::WaterSystem& water_system,
                       const Systems::SHIELD_WorldSystem& world_system,
                       Chunk& chunk);

} // namespace Luminumbra::World::MarchingCubes
