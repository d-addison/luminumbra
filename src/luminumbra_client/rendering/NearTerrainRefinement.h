#pragma once

#include "luminumbra_common/world/Chunk.h"

#include <cstddef>
#include <span>
#include <vector>

namespace Luminumbra::Rendering {

inline constexpr std::size_t kNearTerrainMaxInputTriangles = 2048;
inline constexpr std::size_t kNearTerrainMaxInputVertices = 6144;
inline constexpr std::size_t kNearTerrainMaxOutputTriangles = 6 * kNearTerrainMaxInputTriangles;
inline constexpr std::size_t kNearTerrainMaxAddedVertices = 5 * kNearTerrainMaxInputTriangles / 2;
inline constexpr float kNearTerrainRadiusMeters = 32.0f;
inline constexpr float kNearTerrainMaxDisplacementMeters = 0.25f;

struct NearTerrainRefinementInput {
    std::span<const VoxelVertex> vertices;
    std::span<const u32> indices;
    // Complete chunk-local padded lattice, X fastest, then Y, then Z.
    std::span<const float> densities;
    int sample_step = 0;
    // Eye-to-chunk-centre distance squared in metres. Whole admitted chunks
    // fit within 32 + 8*sqrt(3) metres of the eye (less than 46 metres).
    float chunk_distance_squared = -1.0f;
    bool enabled = false;
    bool generated_current_params = false;
    // Caller must establish an unedited one-cell halo from the same immutable
    // snapshot as the mesh/lattice. Missing or stale evidence must remain false.
    bool unedited_halo = false;
};

struct NearTerrainRenderMesh {
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;
};

enum class NearTerrainRefinementResult {
    Refined,
    Ineligible,
    InvalidMesh,
    CapacityExceeded,
};

// Build-time render copy only; never pass the output to collision or authority.
// All original vertices and every topological/chunk-face boundary edge survive
// exactly. Only shared manifold interior edges split, once each; triangle fans
// join those splits without T-junctions. Heights use cubic Hermite controls
// derived from normals, with vertical displacement clamped to 0.25 m. X/Z stay
// barycentric, so every resulting surface point is within that bound of the
// original triangle at the same X/Z (allow 0.001 m for float interpolation
// roundoff). Non-height-like lattices refuse wholesale. The mesh and lattice
// must come from the same immutable snapshot; this kernel never accesses a world.
// Child triangles preserve the source's flat last-vertex material convention.
//
// Work is capped before scanning: 4913 densities, 6144 vertices, 2048 triangles,
// and sorting 6144 edge records. At most 12288 output triangles and 5120 added
// vertices. Vector payload peak (excluding input/prior output and allocator
// overhead) is <= (6144+5120)*sizeof(VoxelVertex) + 12288*3*sizeof(u32)
// + 6144*(3*sizeof(u32)+sizeof(u32)). No per-frame work or retained scratch.
// Refusal and allocation exceptions leave output unchanged; success replaces it.
[[nodiscard]] NearTerrainRefinementResult RefineNearTerrain(const NearTerrainRefinementInput& input,
                                                            NearTerrainRenderMesh& output);

} // namespace Luminumbra::Rendering
