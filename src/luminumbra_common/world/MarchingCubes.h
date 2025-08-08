#pragma once

// Forward declare the Chunk class to avoid circular dependencies
namespace Luminumbra {
    class Chunk;
}

namespace Luminumbra::World::MarchingCubes {
    // A helper function that encapsulates the whole meshing process for a chunk.
    // It reads from chunk.sdf_data and populates chunk.mesh_vertices and chunk.mesh_indices.
    void PolygoniseChunk(Chunk& chunk, float isolevel);
}