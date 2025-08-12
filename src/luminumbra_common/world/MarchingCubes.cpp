#include "MarchingCubes.h"
#include <vector>
#include <cmath> 

#include "world/Chunk.h"
#include "../../../include/luminumbra/core/Types.h"
#include <glm/glm.hpp>
#include "systems/SHIELD_WorldSystem.h"

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

MaterialType GetMaterialAt(const Systems::SHIELD_WorldSystem& world_system, const Vec3& world_pos) {
    // Rule 1: Sand near sea level (y=0)
    // Check this first, as it should override other materials.
    if (world_pos.y < 2.0f) {
        // Only place sand if the terrain is actually near sea level
        float height = world_system.GetTerrainHeightAt(world_pos.x, world_pos.z);
        if (height < 2.5f) {
             return MaterialType::Sand;
        }
    }

    // Rule 2: Strata (layers based on depth)
    float height = world_system.GetTerrainHeightAt(world_pos.x, world_pos.z);
    float depth = height - world_pos.y;

    if (depth < 1.0f) return MaterialType::Grass;
    if (depth < 5.0f) return MaterialType::Soil;
    
    // Default to stone
    return MaterialType::Stone;
}
namespace World::MarchingCubes {

void PolygoniseChunk(const Systems::SHIELD_WorldSystem& world_system, Chunk& chunk, float isolevel, int step) {
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;

    // Reserve memory to avoid reallocations
    vertices.reserve(CHUNK_VOLUME / 4); 
    indices.reserve(CHUNK_VOLUME / 2);

    const IVec3 corner_offsets[8] = {
        {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1},
        {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}
    };
    
    for (int z = 0; z < CHUNK_SIZE_Z; z += step) {
        for (int y = 0; y < CHUNK_SIZE_Y; y += step) {
            for (int x = 0; x < CHUNK_SIZE_X; x += step) {
                GridCell gridcell;
                int cube_index = 0;

                for (int i = 0; i < 8; ++i) {
                    // FIX: Corner positions are scaled by the step size.
                    IVec3 corner_pos = IVec3(x, y, z) + (corner_offsets[i] * step);
                    
                    int sdf_idx = corner_pos.x + corner_pos.y * (CHUNK_SIZE_X + 1) + corner_pos.z * (CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1);
                    
                    // Prevent reading out of bounds on chunk edges when step > 1
                    if (corner_pos.x > CHUNK_SIZE_X || corner_pos.y > CHUNK_SIZE_Y || corner_pos.z > CHUNK_SIZE_Z) {
                        gridcell.val[i] = 1.0f; // Treat as air to avoid meshing artifacts at LOD boundaries
                    } else {
                        gridcell.val[i] = chunk.sdf_data[sdf_idx];
                    }

                    gridcell.p[i] = Vec3(corner_pos);
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
                    Vec3 normal = glm::normalize(glm::cross(edge_u, edge_v));

                    if (glm::dot(normal, normal) < 1e-12f) {
                        continue; // Skip this degenerate triangle
                    }
                    normal = glm::normalize(normal);

                    // --- MATERIAL LOGIC ---
                    // Calculate the center of the triangle
                    Vec3 triangle_center = (p1 + p2 + p3) / 3.0f;
                    // Convert to world space coordinates
                    Vec3 world_space_pos = triangle_center + Vec3(chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z));
                    
                    // Get the material for this position
                    MaterialType material = GetMaterialAt(world_system, world_space_pos);
                    // --- END MATERIAL LOGIC ---
                    
                    u32 base_index = static_cast<u32>(vertices.size());

                    vertices.push_back({p1, normal, static_cast<u32>(material)});
                    vertices.push_back({p2, normal, static_cast<u32>(material)});
                    vertices.push_back({p3, normal, static_cast<u32>(material)});

                    indices.push_back(base_index);
                    indices.push_back(base_index + 1);
                    indices.push_back(base_index + 2);
                }
            }
        }
    }
    
    chunk.mesh_vertices = std::move(vertices);
    chunk.mesh_indices = std::move(indices);
}
} // namespace World::MarchingCubes

} // namespace Luminumbra