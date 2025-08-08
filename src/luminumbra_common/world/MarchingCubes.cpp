#include "MarchingCubes.h"
#include <vector>
#include <cmath> 

#include "world/Chunk.h"
#include "../../../include/luminumbra/core/Types.h"
#include <glm/glm.hpp>

// --- FIX: Wrap the entire file in the Luminumbra namespace ---
namespace Luminumbra {

// Anonymous namespace to keep helper structures and tables local to this file
namespace {
    // These tables are the core of the Marching Cubes algorithm
    #include "MarchingCubesTables.inl"

    // Represents a single "cube" cell in the 3D grid
    struct GridCell {
        Vec3 p[8];    // Position of the 8 corners of the cube
        f32 val[8];   // SDF value at each of the 8 corners
    };

    // Linearly interpolates to find the point on an edge where the surface crosses
    Vec3 VertexInterp(f32 isolevel, Vec3 p1, Vec3 p2, f32 valp1, f32 valp2) {
        if (std::abs(isolevel - valp1) < 0.00001f) return p1;
        if (std::abs(isolevel - valp2) < 0.00001f) return p2;
        if (std::abs(valp1 - valp2) < 0.00001f) return p1;

        f32 mu = (isolevel - valp1) / (valp2 - valp1);
        return p1 + mu * (p2 - p1);
    }
}

namespace World::MarchingCubes {

void PolygoniseChunk(Chunk& chunk, float isolevel) {
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;

    // Reserve memory to avoid reallocations
    vertices.reserve(CHUNK_VOLUME / 4); 
    indices.reserve(CHUNK_VOLUME / 2);

    const IVec3 corner_offsets[8] = {
        {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1},
        {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}
    };
    
    for (int z = 0; z < CHUNK_SIZE_Z -1; ++z) {
        for (int y = 0; y < CHUNK_SIZE_Y -1; ++y) {
            for (int x = 0; x < CHUNK_SIZE_X -1; ++x) {
                GridCell gridcell;
                int cube_index = 0;

                for (int i = 0; i < 8; ++i) {
                    IVec3 corner_pos = IVec3(x, y, z) + corner_offsets[i];
                    int sdf_idx = corner_pos.x + corner_pos.y * CHUNK_SIZE_X + corner_pos.z * CHUNK_SIZE_X * CHUNK_SIZE_Y;
                    
                    gridcell.p[i] = Vec3(corner_pos);
                    gridcell.val[i] = chunk.sdf_data[sdf_idx];
                    if (gridcell.val[i] < isolevel) {
                        cube_index |= (1 << i);
                    }
                }

                if (edgeTable[cube_index] == 0) {
                    continue;
                }

                Vec3 vertlist[12];
                if (edgeTable[cube_index] & 1)    vertlist[0] = VertexInterp(isolevel, gridcell.p[0], gridcell.p[1], gridcell.val[0], gridcell.val[1]);
                if (edgeTable[cube_index] & 2)    vertlist[1] = VertexInterp(isolevel, gridcell.p[1], gridcell.p[2], gridcell.val[1], gridcell.val[2]);
                if (edgeTable[cube_index] & 4)    vertlist[2] = VertexInterp(isolevel, gridcell.p[2], gridcell.p[3], gridcell.val[2], gridcell.val[3]);
                if (edgeTable[cube_index] & 8)    vertlist[3] = VertexInterp(isolevel, gridcell.p[3], gridcell.p[0], gridcell.val[3], gridcell.val[0]);
                if (edgeTable[cube_index] & 16)   vertlist[4] = VertexInterp(isolevel, gridcell.p[4], gridcell.p[5], gridcell.val[4], gridcell.val[5]);
                if (edgeTable[cube_index] & 32)   vertlist[5] = VertexInterp(isolevel, gridcell.p[5], gridcell.p[6], gridcell.val[5], gridcell.val[6]);
                if (edgeTable[cube_index] & 64)   vertlist[6] = VertexInterp(isolevel, gridcell.p[6], gridcell.p[7], gridcell.val[6], gridcell.val[7]);
                if (edgeTable[cube_index] & 128)  vertlist[7] = VertexInterp(isolevel, gridcell.p[7], gridcell.p[4], gridcell.val[7], gridcell.val[4]);
                if (edgeTable[cube_index] & 256)  vertlist[8] = VertexInterp(isolevel, gridcell.p[0], gridcell.p[4], gridcell.val[0], gridcell.val[4]);
                if (edgeTable[cube_index] & 512)  vertlist[9] = VertexInterp(isolevel, gridcell.p[1], gridcell.p[5], gridcell.val[1], gridcell.val[5]);
                if (edgeTable[cube_index] & 1024) vertlist[10] = VertexInterp(isolevel, gridcell.p[2], gridcell.p[6], gridcell.val[2], gridcell.val[6]);
                if (edgeTable[cube_index] & 2048) vertlist[11] = VertexInterp(isolevel, gridcell.p[3], gridcell.p[7], gridcell.val[3], gridcell.val[7]);

                for (int i = 0; triTable[cube_index][i] != -1; i += 3) {
                    Vec3 p1 = vertlist[triTable[cube_index][i]];
                    Vec3 p2 = vertlist[triTable[cube_index][i+1]];
                    Vec3 p3 = vertlist[triTable[cube_index][i+2]];

                    Vec3 edge_u = p2 - p1;
                    Vec3 edge_v = p3 - p1;
                    Vec3 normal = glm::cross(edge_u, edge_v);

                    if (glm::dot(normal, normal) < 1e-12f) {
                        continue; // Skip this degenerate triangle
                    }
                    normal = glm::normalize(normal);

                    u32 material = 1;

                    // --- START INDEXING FIX ---
                    // Get the current size of the vertex buffer as the base index
                    u32 base_index = static_cast<u32>(vertices.size());

                    vertices.push_back({p1, normal, material});
                    vertices.push_back({p2, normal, material});
                    vertices.push_back({p3, normal, material});

                    indices.push_back(base_index);
                    indices.push_back(base_index + 1);
                    indices.push_back(base_index + 2);
                    // --- END INDEXING FIX ---
                }
            }
        }
    }
    
    chunk.mesh_vertices = std::move(vertices);
    chunk.mesh_indices = std::move(indices);
}
} // namespace World::MarchingCubes

} // namespace Luminumbra