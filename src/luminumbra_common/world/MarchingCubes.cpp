#include "MarchingCubes.h"
#include <algorithm>
#include <vector>
#include <cmath> 
#include "world/Chunk.h"
#include "../../../include/luminumbra/core/Types.h"
#include <glm/glm.hpp>
#include "systems/SHIELD_WorldSystem.h"
#include "systems/WaterSystem.h"
#include <unordered_map>
#include "../core/Log.h"

namespace Luminumbra {
namespace World::MarchingCubes {

// ===================== CORE MARCHING CUBES HELPERS (REFACTORED) =====================
namespace { // Anonymous namespace for internal implementation details

    // These tables are the core of the Marching Cubes algorithm
    #include "MarchingCubesTables.inl"

    struct GridCell {
        Vec3 p[8];  // Position of the 8 corners of the cube
        f32 val[8]; // SDF value at each of the 8 corners
    };

    // Linearly interpolates to find the point on an edge where the surface crosses
    Vec3 VertexInterp(f32 isolevel, Vec3 p1, Vec3 p2, f32 valp1, f32 valp2) {
        if (std::abs(valp1 - valp2) < 0.00001f) return p1;
        f32 mu = (isolevel - valp1) / (valp2 - valp1);
        return p1 + mu * (p2 - p1);
    }

    Vec3 EstimateDensityGradient(const GridCell& gridcell) {
        const f32 dx = (gridcell.val[1] + gridcell.val[2] + gridcell.val[5] + gridcell.val[6])
                     - (gridcell.val[0] + gridcell.val[3] + gridcell.val[4] + gridcell.val[7]);
        const f32 dy = (gridcell.val[4] + gridcell.val[5] + gridcell.val[6] + gridcell.val[7])
                     - (gridcell.val[0] + gridcell.val[1] + gridcell.val[2] + gridcell.val[3]);
        const f32 dz = (gridcell.val[2] + gridcell.val[3] + gridcell.val[6] + gridcell.val[7])
                     - (gridcell.val[0] + gridcell.val[1] + gridcell.val[4] + gridcell.val[5]);

        return Vec3(dx, dy, dz);
    }

    // Determines terrain material based on world position and height
    MaterialType GetTerrainMaterialAt(const Systems::SHIELD_WorldSystem& world_system, const Vec3& world_pos) {
        float terrain_height = world_system.GetTerrainHeightAt(world_pos.x, world_pos.z);
        
        // Example material logic:
        if (world_pos.y < 34.0f && terrain_height < 36.0f) { // Beach level
            return MaterialType::Sand;
        }

        float depth = terrain_height - world_pos.y;
        if (depth < 1.0f) return MaterialType::Grass;
        if (depth < 5.0f) return MaterialType::Soil;
        
        return MaterialType::Stone;
    }

} // anonymous namespace


// ===================== TERRAIN MESH GENERATION =====================

void PolygoniseTerrain(
    const Systems::SHIELD_WorldSystem& world_system,
    Chunk& chunk,
    float isolevel,
    int step
) {
    const int sample_step = std::max(1, step);

    // Debug: Check if chunk has a surface
    bool has_negative = false;
    bool has_positive = false;
    
    for (float val : chunk.sdf_data) {
        if (val < isolevel) {
            has_negative = true;
        }
        if (val > isolevel) {
            has_positive = true;
        }
    }

    if (!has_negative || !has_positive) {
        // Chunk is entirely above or below the surface
        chunk.mesh_vertices.clear();
        chunk.mesh_indices.clear();
        return;
    }

    // NOW continue with the actual mesh generation...
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;
    vertices.reserve(CHUNK_VOLUME / 8); 
    indices.reserve(CHUNK_VOLUME / 4);

    std::unordered_map<u64, u32> vertex_cache;

    const IVec3 chunk_base_pos = chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    const u32 y_stride = CHUNK_SIZE_X + 1;
    const u32 z_stride = (CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1);

    const IVec3 corner_offsets[8] = {
        {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1},
        {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}
    };
    
    const int edge_connections[12][2] = {
        {0,1}, {1,2}, {2,3}, {3,0}, {4,5}, {5,6},
        {6,7}, {7,4}, {0,4}, {1,5}, {2,6}, {3,7}
    };

    // --- PASS 1: Generate unique vertices and triangle indices ---
    for (int z = 0; z < CHUNK_SIZE_Z; z += sample_step) {
        for (int y = 0; y < CHUNK_SIZE_Y; y += sample_step) {
            for (int x = 0; x < CHUNK_SIZE_X; x += sample_step) {
                GridCell gridcell;
                int cube_index = 0;
                u32 corner_abs_indices[8];

                for (int i = 0; i < 8; ++i) {
                    IVec3 corner_pos = IVec3(x, y, z) + (corner_offsets[i] * sample_step);
                    u32 sdf_idx = corner_pos.x + corner_pos.y * y_stride + corner_pos.z * z_stride;
                    corner_abs_indices[i] = sdf_idx;
                    
                    if (corner_pos.x > CHUNK_SIZE_X || corner_pos.y > CHUNK_SIZE_Y || corner_pos.z > CHUNK_SIZE_Z) {
                        gridcell.val[i] = 1.0f;
                    } else {
                        gridcell.val[i] = chunk.sdf_data[sdf_idx];
                    }

                    gridcell.p[i] = Vec3(corner_pos);
                    if (gridcell.val[i] < isolevel) {
                        cube_index |= (1 << i);
                    }
                }

                if (edgeTable[cube_index] == 0) continue;

                u32 vert_indices[12];
                for (int i = 0; i < 12; ++i) {
                    if (edgeTable[cube_index] & (1 << i)) {
                        u32 c1_idx = corner_abs_indices[edge_connections[i][0]];
                        u32 c2_idx = corner_abs_indices[edge_connections[i][1]];
                        
                        u64 edge_key = (static_cast<u64>(std::min(c1_idx, c2_idx)) << 32) | std::max(c1_idx, c2_idx);

                        auto it = vertex_cache.find(edge_key);
                        if (it != vertex_cache.end()) {
                            vert_indices[i] = it->second;
                        } else {
                            Vec3 p1 = gridcell.p[edge_connections[i][0]];
                            Vec3 p2 = gridcell.p[edge_connections[i][1]];
                            f32 v1 = gridcell.val[edge_connections[i][0]];
                            f32 v2 = gridcell.val[edge_connections[i][1]];
                            Vec3 new_pos = VertexInterp(isolevel, p1, p2, v1, v2);

                            Vec3 world_pos = Vec3(chunk_base_pos) + new_pos;
                            MaterialType mat = GetTerrainMaterialAt(world_system, world_pos);
                            
                            vertices.push_back({new_pos, Vec3(0.0f), static_cast<u32>(mat)});
                            u32 new_idx = static_cast<u32>(vertices.size() - 1);
                            vert_indices[i] = new_idx;
                            vertex_cache[edge_key] = new_idx;
                        }
                    }
                }

                const Vec3 density_gradient = EstimateDensityGradient(gridcell);
                for (int i = 0; triTable[cube_index][i] != -1; i += 3) {
                    u32 i0 = vert_indices[triTable[cube_index][i]];
                    u32 i1 = vert_indices[triTable[cube_index][i+1]];
                    u32 i2 = vert_indices[triTable[cube_index][i+2]];

                    const Vec3 face_normal = glm::cross(
                        vertices[i1].position - vertices[i0].position,
                        vertices[i2].position - vertices[i0].position
                    );

                    if (glm::dot(face_normal, density_gradient) < 0.0f) {
                        std::swap(i1, i2);
                    }

                    indices.push_back(i0);
                    indices.push_back(i1);
                    indices.push_back(i2);
                }
            }
        }
    }
    
    // --- PASS 2: Calculate smoothed normals ---
    for (size_t i = 0; i < indices.size(); i += 3) {
        VoxelVertex& v1 = vertices[indices[i]];
        VoxelVertex& v2 = vertices[indices[i+1]];
        VoxelVertex& v3 = vertices[indices[i+2]];

        Vec3 face_normal = glm::cross(v2.position - v1.position, v3.position - v1.position);

        v1.normal += face_normal;
        v2.normal += face_normal;
        v3.normal += face_normal;
    }

    // --- PASS 3: Normalize all vertex normals ---
    for (auto& vertex : vertices) {
        if (glm::dot(vertex.normal, vertex.normal) > 0.0f) {
            vertex.normal = glm::normalize(vertex.normal);
        }
    }
    
    chunk.mesh_vertices = std::move(vertices);
    chunk.mesh_indices = std::move(indices);
    
    // Mesh generation complete
}

// ===================== NEW WATER MESH GENERATION =====================

void GenerateWaterMesh(
    const Systems::WaterSystem& water_system,
    const Systems::SHIELD_WorldSystem& world_system,
    Chunk& chunk
) {
    std::vector<VoxelVertex> water_vertices;
    std::vector<u32> water_indices;
    water_vertices.reserve(WATER_SIM_RESOLUTION_X * WATER_SIM_RESOLUTION_Z * 4);
    water_indices.reserve(WATER_SIM_RESOLUTION_X * WATER_SIM_RESOLUTION_Z * 6);

    if (!chunk.has_water_sim.load()) {
        chunk.water_mesh_vertices.clear();
        chunk.water_mesh_indices.clear();
        return;
    }

    const IVec3 chunk_base_pos = chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    const Vec3 normal = {0.0f, 1.0f, 0.0f}; // Water surface normal is always up
    const u32 water_mat_id = static_cast<u32>(MaterialType::Water);

    const float cell_width_x = (float)CHUNK_SIZE_X / WATER_SIM_RESOLUTION_X;
    const float cell_width_z = (float)CHUNK_SIZE_Z / WATER_SIM_RESOLUTION_Z;

    for (int z = 0; z < WATER_SIM_RESOLUTION_Z; ++z) {
        for (int x = 0; x < WATER_SIM_RESOLUTION_X; ++x) {
            // Get the water and terrain heights at the four corners of this water grid cell
            float world_x0 = chunk_base_pos.x + x * cell_width_x;
            float world_z0 = chunk_base_pos.z + z * cell_width_z;
            float world_x1 = world_x0 + cell_width_x;
            float world_z1 = world_z0 + cell_width_z;

            float water_h00 = water_system.get_water_level_at(world_x0, world_z0);
            float water_h10 = water_system.get_water_level_at(world_x1, world_z0);
            float water_h01 = water_system.get_water_level_at(world_x0, world_z1);
            float water_h11 = water_system.get_water_level_at(world_x1, world_z1);

            float terrain_h00 = world_system.GetTerrainHeightAt(world_x0, world_z0);
            float terrain_h10 = world_system.GetTerrainHeightAt(world_x1, world_z0);
            float terrain_h01 = world_system.GetTerrainHeightAt(world_x0, world_z1);
            float terrain_h11 = world_system.GetTerrainHeightAt(world_x1, world_z1);
            
            // Only generate a quad if water is above the terrain at any corner
            if (water_h00 > terrain_h00 || water_h10 > terrain_h10 || water_h01 > terrain_h01 || water_h11 > terrain_h11) {
                u32 base_idx = static_cast<u32>(water_vertices.size());
                
                // Define vertices relative to chunk origin
                Vec3 p00 = {x * cell_width_x, water_h00 - chunk_base_pos.y, z * cell_width_z};
                Vec3 p10 = {(x+1) * cell_width_x, water_h10 - chunk_base_pos.y, z * cell_width_z};
                Vec3 p01 = {x * cell_width_x, water_h01 - chunk_base_pos.y, (z+1) * cell_width_z};
                Vec3 p11 = {(x+1) * cell_width_x, water_h11 - chunk_base_pos.y, (z+1) * cell_width_z};

                water_vertices.push_back({p00, normal, water_mat_id});
                water_vertices.push_back({p01, normal, water_mat_id});
                water_vertices.push_back({p11, normal, water_mat_id});
                water_vertices.push_back({p10, normal, water_mat_id});

                water_indices.push_back(base_idx);
                water_indices.push_back(base_idx + 1);
                water_indices.push_back(base_idx + 2);
                water_indices.push_back(base_idx);
                water_indices.push_back(base_idx + 2);
                water_indices.push_back(base_idx + 3);
            }
        }
    }
    
    chunk.water_mesh_vertices = std::move(water_vertices);
    chunk.water_mesh_indices = std::move(water_indices);
}

} // namespace World::MarchingCubes
} // namespace Luminumbra
