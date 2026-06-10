#include "MarchingCubes.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>
#include <cmath> 
#include "world/Chunk.h"
#include "../../../include/luminumbra/core/Types.h"
#include <glm/glm.hpp>
#include "systems/SHIELD_WorldSystem.h"
#include "systems/WaterSystem.h"
#include <unordered_map>
#include <unordered_set>
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

    // Determines terrain material based on world position and height.
    // Meshable terrain must never carry the non-rendering Air material into the
    // G-buffer; isosurface interpolation can land just outside the solid side.
    MaterialType GetTerrainMaterialAt(const Systems::SHIELD_WorldSystem& world_system, const Vec3& world_pos) {
        const auto sample = world_system.SampleWorldGenLayers(world_pos - Vec3(0.0f, 0.25f, 0.0f));
        if (sample.material != MaterialType::Air && sample.material != MaterialType::Water) {
            return sample.material;
        }

        const float terrain_height = world_system.GetTerrainHeightAt(world_pos.x, world_pos.z);
        if (world_pos.y < 34.0f && terrain_height < 36.0f) {
            return MaterialType::Sand;
        }

        const float depth = terrain_height - world_pos.y;
        if (depth < 1.0f) {
            return MaterialType::Grass;
        }
        if (depth < 5.0f) {
            return MaterialType::Soil;
        }
        return MaterialType::Stone;
    }

    struct AtomicTerrainMeshBuildStats {
        std::atomic<std::size_t> jobs{0};
        std::atomic<std::size_t> step1_jobs{0};
        std::atomic<std::size_t> step2_jobs{0};
        std::atomic<std::size_t> step4_jobs{0};
        std::atomic<std::size_t> cells_visited{0};
        std::atomic<std::size_t> active_cells{0};
        std::atomic<std::size_t> vertices{0};
        std::atomic<std::size_t> indices{0};
        std::atomic<std::size_t> triangles{0};
        std::atomic<std::uint64_t> elapsed_us{0};
    };

    AtomicTerrainMeshBuildStats g_terrain_mesh_build_stats;

    void RecordTerrainMeshBuildStats(int step, std::size_t cells_visited, std::size_t active_cells,
                                     std::size_t vertices, std::size_t indices, std::uint64_t elapsed_us) {
        g_terrain_mesh_build_stats.jobs.fetch_add(1, std::memory_order_relaxed);
        if (step <= 1) {
            g_terrain_mesh_build_stats.step1_jobs.fetch_add(1, std::memory_order_relaxed);
        } else if (step == 2) {
            g_terrain_mesh_build_stats.step2_jobs.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_terrain_mesh_build_stats.step4_jobs.fetch_add(1, std::memory_order_relaxed);
        }
        g_terrain_mesh_build_stats.cells_visited.fetch_add(cells_visited, std::memory_order_relaxed);
        g_terrain_mesh_build_stats.active_cells.fetch_add(active_cells, std::memory_order_relaxed);
        g_terrain_mesh_build_stats.vertices.fetch_add(vertices, std::memory_order_relaxed);
        g_terrain_mesh_build_stats.indices.fetch_add(indices, std::memory_order_relaxed);
        g_terrain_mesh_build_stats.triangles.fetch_add(indices / 3u, std::memory_order_relaxed);
        g_terrain_mesh_build_stats.elapsed_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    }

    constexpr float kBoundaryEpsilon = 1.0e-4f;

    bool Near(float value, float target) {
        return std::abs(value - target) <= kBoundaryEpsilon;
    }

    bool HasFace(TerrainTransitionFaceMask mask, TerrainTransitionFace face) {
        return (mask & static_cast<TerrainTransitionFaceMask>(face)) != 0u;
    }

    Vec3 TransitionFaceNormal(TerrainTransitionFace face) {
        switch (face) {
        case TransitionFaceMinX:
            return Vec3(-1.0f, 0.0f, 0.0f);
        case TransitionFaceMaxX:
            return Vec3(1.0f, 0.0f, 0.0f);
        case TransitionFaceMinZ:
            return Vec3(0.0f, 0.0f, -1.0f);
        case TransitionFaceMaxZ:
            return Vec3(0.0f, 0.0f, 1.0f);
        }
        return Vec3(0.0f, 0.0f, 0.0f);
    }

    int TransitionFaceSlot(TerrainTransitionFace face) {
        switch (face) {
        case TransitionFaceMinX:
            return 0;
        case TransitionFaceMaxX:
            return 1;
        case TransitionFaceMinZ:
            return 2;
        case TransitionFaceMaxZ:
            return 3;
        }
        return 0;
    }

    bool VertexOnTransitionFace(const VoxelVertex& vertex, TerrainTransitionFace face) {
        switch (face) {
        case TransitionFaceMinX:
            return Near(vertex.position.x, 0.0f);
        case TransitionFaceMaxX:
            return Near(vertex.position.x, static_cast<float>(CHUNK_SIZE_X));
        case TransitionFaceMinZ:
            return Near(vertex.position.z, 0.0f);
        case TransitionFaceMaxZ:
            return Near(vertex.position.z, static_cast<float>(CHUNK_SIZE_Z));
        }
        return false;
    }

    bool EdgeOnTransitionFace(const VoxelVertex& a, const VoxelVertex& b, TerrainTransitionFace face) {
        return VertexOnTransitionFace(a, face) && VertexOnTransitionFace(b, face);
    }

    u64 TransitionEdgeKey(u32 a, u32 b, TerrainTransitionFace face) {
        const u64 lo = static_cast<u64>(std::min(a, b));
        const u64 hi = static_cast<u64>(std::max(a, b));
        return (static_cast<u64>(face) << 56u) ^ (lo << 28u) ^ hi;
    }

    bool ExistingTransitionSkirtOnFace(const Chunk& chunk, TerrainTransitionFace face) {
        const Vec3 face_normal = TransitionFaceNormal(face);
        std::size_t stamped_vertices = 0;
        for (const VoxelVertex& vertex : chunk.mesh_vertices) {
            if (VertexOnTransitionFace(vertex, face) &&
                std::abs(vertex.normal.x - face_normal.x) <= kBoundaryEpsilon &&
                std::abs(vertex.normal.y - face_normal.y) <= kBoundaryEpsilon &&
                std::abs(vertex.normal.z - face_normal.z) <= kBoundaryEpsilon)
            {
                ++stamped_vertices;
            }
        }
        return stamped_vertices >= 2u;
    }

    bool AppendOrientedTriangle(std::vector<u32>& indices, const std::vector<VoxelVertex>& vertices,
                                u32 a, u32 b, u32 c, const Vec3& desired_normal) {
        if (a == b || b == c || c == a) {
            return false;
        }

        Vec3 face_normal = glm::cross(
            vertices[b].position - vertices[a].position,
            vertices[c].position - vertices[a].position
        );
        if (glm::dot(face_normal, face_normal) <= 1.0e-10f) {
            return false;
        }

        if (glm::dot(face_normal, desired_normal) < 0.0f) {
            std::swap(b, c);
        }

        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
        return true;
    }

    std::size_t SdfIndex(int x, int y, int z) {
        return static_cast<std::size_t>(x)
            + static_cast<std::size_t>(y) * static_cast<std::size_t>(CHUNK_SIZE_X + 1)
            + static_cast<std::size_t>(z) * static_cast<std::size_t>(CHUNK_SIZE_X + 1) * static_cast<std::size_t>(CHUNK_SIZE_Y + 1);
    }

    bool ReadSdfValue(const Chunk& chunk, int x, int y, int z, float& value) {
        if (x < 0 || x > CHUNK_SIZE_X || y < 0 || y > CHUNK_SIZE_Y || z < 0 || z > CHUNK_SIZE_Z) {
            return false;
        }
        const std::size_t index = SdfIndex(x, y, z);
        if (index >= chunk.sdf_data.size()) {
            return false;
        }
        value = chunk.sdf_data[index];
        return true;
    }

    Vec3 TransitionFacePosition(TerrainTransitionFace face, int major, int y) {
        switch (face) {
        case TransitionFaceMinX:
            return Vec3(0.0f, static_cast<float>(y), static_cast<float>(major));
        case TransitionFaceMaxX:
            return Vec3(static_cast<float>(CHUNK_SIZE_X), static_cast<float>(y), static_cast<float>(major));
        case TransitionFaceMinZ:
            return Vec3(static_cast<float>(major), static_cast<float>(y), 0.0f);
        case TransitionFaceMaxZ:
            return Vec3(static_cast<float>(major), static_cast<float>(y), static_cast<float>(CHUNK_SIZE_Z));
        }
        return Vec3(0.0f);
    }

    bool ReadTransitionFaceSdf(const Chunk& chunk, TerrainTransitionFace face, int major, int y, float& value) {
        switch (face) {
        case TransitionFaceMinX:
            return ReadSdfValue(chunk, 0, y, major, value);
        case TransitionFaceMaxX:
            return ReadSdfValue(chunk, CHUNK_SIZE_X, y, major, value);
        case TransitionFaceMinZ:
            return ReadSdfValue(chunk, major, y, 0, value);
        case TransitionFaceMaxZ:
            return ReadSdfValue(chunk, major, y, CHUNK_SIZE_Z, value);
        }
        return false;
    }

    bool HasCompleteWaterGrid(const Chunk& chunk, int resolution) {
        if (!chunk.has_water_sim.load(std::memory_order_relaxed) || resolution <= 1) {
            return false;
        }
        const std::size_t cell_count = static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
        return chunk.water_level_data.size() >= cell_count;
    }

    float SampleChunkWaterLevel(const Chunk& chunk, float world_x, float world_z, int resolution) {
        const Vec3 base_pos = Vec3(chunk.get_coords() * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z));
        const float local_x = world_x - base_pos.x;
        const float local_z = world_z - base_pos.z;

        const float sim_xf = (local_x / static_cast<float>(CHUNK_SIZE_X)) * static_cast<float>(resolution) - 0.5f;
        const float sim_zf = (local_z / static_cast<float>(CHUNK_SIZE_Z)) * static_cast<float>(resolution) - 0.5f;

        int x0 = static_cast<int>(std::floor(sim_xf));
        int z0 = static_cast<int>(std::floor(sim_zf));
        x0 = std::clamp(x0, 0, resolution - 2);
        z0 = std::clamp(z0, 0, resolution - 2);

        const float tx = std::clamp(sim_xf - static_cast<float>(x0), 0.0f, 1.0f);
        const float tz = std::clamp(sim_zf - static_cast<float>(z0), 0.0f, 1.0f);

        const float h00 = chunk.water_level_data[static_cast<std::size_t>(z0 * resolution + x0)];
        const float h10 = chunk.water_level_data[static_cast<std::size_t>(z0 * resolution + (x0 + 1))];
        const float h01 = chunk.water_level_data[static_cast<std::size_t>((z0 + 1) * resolution + x0)];
        const float h11 = chunk.water_level_data[static_cast<std::size_t>((z0 + 1) * resolution + (x0 + 1))];

        const float h_z0 = glm::mix(h00, h10, tx);
        const float h_z1 = glm::mix(h01, h11, tx);
        return glm::mix(h_z0, h_z1, tz);
    }

    u32 FallbackTransitionMaterial(const Chunk& chunk, TerrainTransitionFace face) {
        for (const VoxelVertex& vertex : chunk.mesh_vertices) {
            if (VertexOnTransitionFace(vertex, face)) {
                return vertex.material_id;
            }
        }
        return static_cast<u32>(MaterialType::Stone);
    }

    void AppendFallbackFacePatches(Chunk& chunk, int step, TerrainTransitionFace face, TerrainTransitionSkirtStats& stats) {
        if (chunk.sdf_data.empty()) {
            return;
        }

        const int sample_step = std::max(1, step);
        // Patch vertices carry the outward face normal for the same reason as
        // edge skirts: idempotency detection and wall-correct lighting.
        const Vec3 face_normal = TransitionFaceNormal(face);
        const u32 material_id = FallbackTransitionMaterial(chunk, face);
        for (int major = 0; major + sample_step <= CHUNK_SIZE_X; major += sample_step) {
            for (int y = 0; y + sample_step <= CHUNK_SIZE_Y; y += sample_step) {
                float v00 = 0.0f;
                float v10 = 0.0f;
                float v01 = 0.0f;
                float v11 = 0.0f;
                if (!ReadTransitionFaceSdf(chunk, face, major, y, v00) ||
                    !ReadTransitionFaceSdf(chunk, face, major + sample_step, y, v10) ||
                    !ReadTransitionFaceSdf(chunk, face, major, y + sample_step, v01) ||
                    !ReadTransitionFaceSdf(chunk, face, major + sample_step, y + sample_step, v11))
                {
                    continue;
                }

                const float min_density = std::min(std::min(v00, v10), std::min(v01, v11));
                const float max_density = std::max(std::max(v00, v10), std::max(v01, v11));
                const bool crosses_surface = min_density <= 0.0f && max_density >= 0.0f;
                bool near_surface =
                    std::abs(v00) <= 0.75f || std::abs(v10) <= 0.75f ||
                    std::abs(v01) <= 0.75f || std::abs(v11) <= 0.75f;
                for (int local_major = 0; local_major <= sample_step && !near_surface; ++local_major) {
                    for (int local_y = 0; local_y <= sample_step; ++local_y) {
                        float sample = 0.0f;
                        if (ReadTransitionFaceSdf(chunk, face, major + local_major, y + local_y, sample) &&
                            std::abs(sample) <= 0.75f)
                        {
                            near_surface = true;
                            break;
                        }
                    }
                }
                if (!crosses_surface && !near_surface) {
                    continue;
                }

                const u32 base = static_cast<u32>(chunk.mesh_vertices.size());
                chunk.mesh_vertices.push_back({TransitionFacePosition(face, major, y), face_normal, material_id});
                chunk.mesh_vertices.push_back({TransitionFacePosition(face, major + sample_step, y), face_normal, material_id});
                chunk.mesh_vertices.push_back({TransitionFacePosition(face, major + sample_step, y + sample_step), face_normal, material_id});
                chunk.mesh_vertices.push_back({TransitionFacePosition(face, major, y + sample_step), face_normal, material_id});

                const std::size_t index_count_before = chunk.mesh_indices.size();
                const bool first = AppendOrientedTriangle(chunk.mesh_indices, chunk.mesh_vertices, base, base + 1u, base + 2u, face_normal);
                const bool second = AppendOrientedTriangle(chunk.mesh_indices, chunk.mesh_vertices, base, base + 2u, base + 3u, face_normal);
                if (!first && !second) {
                    chunk.mesh_vertices.pop_back();
                    chunk.mesh_vertices.pop_back();
                    chunk.mesh_vertices.pop_back();
                    chunk.mesh_vertices.pop_back();
                    continue;
                }

                ++stats.boundary_edges;
                stats.vertices_added += 4u;
                stats.indices_added += chunk.mesh_indices.size() - index_count_before;
                stats.triangles_added += (first ? 1u : 0u) + (second ? 1u : 0u);
            }
        }
    }

    void GenerateCoarseHeightfieldTerrain(
        const Systems::SHIELD_WorldSystem& world_system,
        Chunk& chunk,
        int sample_step,
        const std::chrono::steady_clock::time_point& build_start)
    {
        const IVec3 chunk_base_pos = chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
        const int cells_x = CHUNK_SIZE_X / sample_step;
        const int cells_z = CHUNK_SIZE_Z / sample_step;
        const int vertices_x = cells_x + 1;
        const int vertices_z = cells_z + 1;

        std::vector<VoxelVertex> vertices;
        std::vector<u32> indices;
        vertices.reserve(static_cast<std::size_t>(vertices_x * vertices_z));
        indices.reserve(static_cast<std::size_t>(cells_x * cells_z * 6));

        for (int gz = 0; gz < vertices_z; ++gz) {
            const int local_z = std::min(CHUNK_SIZE_Z, gz * sample_step);
            const float world_z = static_cast<float>(chunk_base_pos.z + local_z);
            for (int gx = 0; gx < vertices_x; ++gx) {
                const int local_x = std::min(CHUNK_SIZE_X, gx * sample_step);
                const float world_x = static_cast<float>(chunk_base_pos.x + local_x);
                const float terrain_height = world_system.GetTerrainHeightAt(world_x, world_z);
                const float local_y = terrain_height - static_cast<float>(chunk_base_pos.y);
                const MaterialType material = GetTerrainMaterialAt(
                    world_system,
                    Vec3(world_x, terrain_height - 0.1f, world_z)
                );
                vertices.push_back({
                    Vec3(static_cast<float>(local_x), local_y, static_cast<float>(local_z)),
                    Vec3(0.0f),
                    static_cast<u32>(material)
                });
            }
        }

        auto vertex_index = [vertices_x](int gx, int gz) {
            return static_cast<u32>(gz * vertices_x + gx);
        };

        std::size_t cells_visited = 0;
        std::size_t active_cells = 0;
        for (int gz = 0; gz < cells_z; ++gz) {
            for (int gx = 0; gx < cells_x; ++gx) {
                ++cells_visited;
                const u32 i00 = vertex_index(gx, gz);
                const u32 i10 = vertex_index(gx + 1, gz);
                const u32 i01 = vertex_index(gx, gz + 1);
                const u32 i11 = vertex_index(gx + 1, gz + 1);
                const float cell_surface_y =
                    (vertices[i00].position.y + vertices[i10].position.y +
                     vertices[i01].position.y + vertices[i11].position.y) * 0.25f;

                if (cell_surface_y < 0.0f || cell_surface_y > static_cast<float>(CHUNK_SIZE_Y)) {
                    continue;
                }

                indices.push_back(i00);
                indices.push_back(i11);
                indices.push_back(i10);
                indices.push_back(i00);
                indices.push_back(i01);
                indices.push_back(i11);
                ++active_cells;
            }
        }

        for (std::size_t i = 0; i + 2u < indices.size(); i += 3u) {
            VoxelVertex& v0 = vertices[indices[i]];
            VoxelVertex& v1 = vertices[indices[i + 1u]];
            VoxelVertex& v2 = vertices[indices[i + 2u]];
            const Vec3 face_normal = glm::cross(v1.position - v0.position, v2.position - v0.position);
            v0.normal += face_normal;
            v1.normal += face_normal;
            v2.normal += face_normal;
        }

        for (VoxelVertex& vertex : vertices) {
            if (glm::dot(vertex.normal, vertex.normal) > 0.0f) {
                vertex.normal = glm::normalize(vertex.normal);
            } else {
                vertex.normal = Vec3(0.0f, 1.0f, 0.0f);
            }
        }

        std::vector<u32> remap(vertices.size(), static_cast<u32>(-1));
        std::vector<VoxelVertex> compact_vertices;
        compact_vertices.reserve(vertices.size());
        for (u32& index : indices) {
            if (remap[index] == static_cast<u32>(-1)) {
                remap[index] = static_cast<u32>(compact_vertices.size());
                compact_vertices.push_back(vertices[index]);
            }
            index = remap[index];
        }

        chunk.mesh_vertices = std::move(compact_vertices);
        chunk.mesh_indices = std::move(indices);
        const auto elapsed_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - build_start).count()
        );
        RecordTerrainMeshBuildStats(
            sample_step,
            cells_visited,
            active_cells,
            chunk.mesh_vertices.size(),
            chunk.mesh_indices.size(),
            elapsed_us
        );
    }

} // anonymous namespace

void ResetTerrainMeshBuildStats() {
    g_terrain_mesh_build_stats.jobs.store(0, std::memory_order_relaxed);
    g_terrain_mesh_build_stats.step1_jobs.store(0, std::memory_order_relaxed);
    g_terrain_mesh_build_stats.step2_jobs.store(0, std::memory_order_relaxed);
    g_terrain_mesh_build_stats.step4_jobs.store(0, std::memory_order_relaxed);
    g_terrain_mesh_build_stats.cells_visited.store(0, std::memory_order_relaxed);
    g_terrain_mesh_build_stats.active_cells.store(0, std::memory_order_relaxed);
    g_terrain_mesh_build_stats.vertices.store(0, std::memory_order_relaxed);
    g_terrain_mesh_build_stats.indices.store(0, std::memory_order_relaxed);
    g_terrain_mesh_build_stats.triangles.store(0, std::memory_order_relaxed);
    g_terrain_mesh_build_stats.elapsed_us.store(0, std::memory_order_relaxed);
}

TerrainMeshBuildStats GetTerrainMeshBuildStats() {
    TerrainMeshBuildStats stats;
    stats.jobs = g_terrain_mesh_build_stats.jobs.load(std::memory_order_relaxed);
    stats.step1_jobs = g_terrain_mesh_build_stats.step1_jobs.load(std::memory_order_relaxed);
    stats.step2_jobs = g_terrain_mesh_build_stats.step2_jobs.load(std::memory_order_relaxed);
    stats.step4_jobs = g_terrain_mesh_build_stats.step4_jobs.load(std::memory_order_relaxed);
    stats.cells_visited = g_terrain_mesh_build_stats.cells_visited.load(std::memory_order_relaxed);
    stats.active_cells = g_terrain_mesh_build_stats.active_cells.load(std::memory_order_relaxed);
    stats.vertices = g_terrain_mesh_build_stats.vertices.load(std::memory_order_relaxed);
    stats.indices = g_terrain_mesh_build_stats.indices.load(std::memory_order_relaxed);
    stats.triangles = g_terrain_mesh_build_stats.triangles.load(std::memory_order_relaxed);
    stats.elapsed_us = g_terrain_mesh_build_stats.elapsed_us.load(std::memory_order_relaxed);
    return stats;
}

// ===================== TERRAIN MESH GENERATION =====================

void PolygoniseTerrain(
    const Systems::SHIELD_WorldSystem& world_system,
    Chunk& chunk,
    float isolevel,
    int step
) {
    const auto build_start = std::chrono::steady_clock::now();
    const int sample_step = std::max(1, step);

    if (sample_step > 1) {
        GenerateCoarseHeightfieldTerrain(world_system, chunk, sample_step, build_start);
        return;
    }

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
        const auto elapsed_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - build_start).count()
        );
        RecordTerrainMeshBuildStats(sample_step, 0, 0, 0, 0, elapsed_us);
        return;
    }

    // NOW continue with the actual mesh generation...
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;
    const int sample_step_cubed = sample_step * sample_step * sample_step;
    vertices.reserve(std::max(16, CHUNK_VOLUME / (8 * sample_step_cubed)));
    indices.reserve(std::max(32, CHUNK_VOLUME / (4 * sample_step_cubed)));

    std::unordered_map<u64, u32> vertex_cache;
    vertex_cache.reserve(vertices.capacity());

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
    std::size_t cells_visited = 0;
    std::size_t active_cells = 0;
    for (int z = 0; z < CHUNK_SIZE_Z; z += sample_step) {
        for (int y = 0; y < CHUNK_SIZE_Y; y += sample_step) {
            for (int x = 0; x < CHUNK_SIZE_X; x += sample_step) {
                GridCell gridcell;
                int cube_index = 0;
                u32 corner_abs_indices[8];
                ++cells_visited;

                for (int i = 0; i < 8; ++i) {
                    IVec3 corner_pos = IVec3(x, y, z) + (corner_offsets[i] * sample_step);
                    u32 sdf_idx = corner_pos.x + corner_pos.y * y_stride + corner_pos.z * z_stride;
                    corner_abs_indices[i] = sdf_idx;
                    
                    if (corner_pos.x > CHUNK_SIZE_X || corner_pos.y > CHUNK_SIZE_Y || corner_pos.z > CHUNK_SIZE_Z) {
                        gridcell.val[i] = 1.0f;
                    } else {
                        gridcell.val[i] = chunk.sdf_data[sdf_idx];
                    }

                    if (gridcell.val[i] < isolevel) {
                        cube_index |= (1 << i);
                    }
                }

                if (edgeTable[cube_index] == 0) continue;
                ++active_cells;

                for (int i = 0; i < 8; ++i) {
                    gridcell.p[i] = Vec3(IVec3(x, y, z) + (corner_offsets[i] * sample_step));
                }

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

                    if (i0 == i1 || i1 == i2 || i2 == i0) {
                        continue;
                    }

                    const Vec3 face_normal = glm::cross(
                        vertices[i1].position - vertices[i0].position,
                        vertices[i2].position - vertices[i0].position
                    );

                    if (glm::dot(face_normal, face_normal) <= 1.0e-10f) {
                        continue;
                    }

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

    std::vector<u32> remap(vertices.size(), static_cast<u32>(-1));
    std::vector<VoxelVertex> compact_vertices;
    compact_vertices.reserve(vertices.size());
    for (u32& index : indices) {
        if (remap[index] == static_cast<u32>(-1)) {
            remap[index] = static_cast<u32>(compact_vertices.size());
            compact_vertices.push_back(vertices[index]);
        }
        index = remap[index];
    }
    
    chunk.mesh_vertices = std::move(compact_vertices);
    chunk.mesh_indices = std::move(indices);
    const auto elapsed_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - build_start).count()
    );
    RecordTerrainMeshBuildStats(
        sample_step,
        cells_visited,
        active_cells,
        chunk.mesh_vertices.size(),
        chunk.mesh_indices.size(),
        elapsed_us
    );
    
    // Mesh generation complete
}

TerrainTransitionSkirtStats AddBoundaryTransitionSkirts(
    Chunk& chunk,
    int step,
    TerrainTransitionFaceMask faces
) {
    TerrainTransitionSkirtStats stats;
    if (step <= 1 || faces == kNoTransitionFaces || chunk.mesh_vertices.empty() || chunk.mesh_indices.empty()) {
        return stats;
    }

    TerrainTransitionFaceMask active_faces = faces;
    constexpr TerrainTransitionFace kFaces[] = {
        TransitionFaceMinX,
        TransitionFaceMaxX,
        TransitionFaceMinZ,
        TransitionFaceMaxZ,
    };
    for (const TerrainTransitionFace face : kFaces) {
        if (HasFace(active_faces, face) && ExistingTransitionSkirtOnFace(chunk, face)) {
            active_faces &= ~static_cast<TerrainTransitionFaceMask>(face);
        }
    }
    if (active_faces == kNoTransitionFaces) {
        return stats;
    }

    const float skirt_depth = std::max(0.75f, static_cast<float>(step) * 0.75f);
    const Vec3 drop(0.0f, -skirt_depth, 0.0f);
    const std::size_t original_index_count = chunk.mesh_indices.size();
    std::size_t face_edges_added[4] = {};
    std::unordered_set<u64> visited_edges;
    visited_edges.reserve(original_index_count);

    auto append_edge_skirt = [&](u32 a, u32 b, TerrainTransitionFace face) {
        if (a >= chunk.mesh_vertices.size() || b >= chunk.mesh_vertices.size()) {
            return;
        }
        const VoxelVertex& top_a = chunk.mesh_vertices[a];
        const VoxelVertex& top_b = chunk.mesh_vertices[b];
        if (!EdgeOnTransitionFace(top_a, top_b, face)) {
            return;
        }

        const Vec3 edge = top_b.position - top_a.position;
        const Vec3 area = glm::cross(edge, drop);
        if (glm::dot(area, area) <= 1.0e-10f) {
            return;
        }

        const u64 edge_key = TransitionEdgeKey(a, b, face);
        if (!visited_edges.insert(edge_key).second) {
            return;
        }

        // Stamp dropped vertices with the outward face normal: this is what
        // ExistingTransitionSkirtOnFace keys on for idempotency, and it lights
        // the skirt as the vertical wall it is.
        const Vec3 face_normal = TransitionFaceNormal(face);
        const u32 down_a = static_cast<u32>(chunk.mesh_vertices.size());
        VoxelVertex skirt_a = top_a;
        skirt_a.position += drop;
        skirt_a.normal = face_normal;
        chunk.mesh_vertices.push_back(skirt_a);

        const u32 down_b = static_cast<u32>(chunk.mesh_vertices.size());
        VoxelVertex skirt_b = top_b;
        skirt_b.position += drop;
        skirt_b.normal = face_normal;
        chunk.mesh_vertices.push_back(skirt_b);

        const std::size_t index_count_before = chunk.mesh_indices.size();
        const bool first = AppendOrientedTriangle(chunk.mesh_indices, chunk.mesh_vertices, a, b, down_b, face_normal);
        const bool second = AppendOrientedTriangle(chunk.mesh_indices, chunk.mesh_vertices, a, down_b, down_a, face_normal);
        if (!first && !second) {
            chunk.mesh_vertices.pop_back();
            chunk.mesh_vertices.pop_back();
            return;
        }

        ++stats.boundary_edges;
        ++face_edges_added[TransitionFaceSlot(face)];
        stats.vertices_added += 2u;
        stats.indices_added += chunk.mesh_indices.size() - index_count_before;
        stats.triangles_added += (first ? 1u : 0u) + (second ? 1u : 0u);
    };

    for (std::size_t i = 0; i + 2u < original_index_count; i += 3u) {
        const u32 i0 = chunk.mesh_indices[i];
        const u32 i1 = chunk.mesh_indices[i + 1u];
        const u32 i2 = chunk.mesh_indices[i + 2u];

        for (const TerrainTransitionFace face : kFaces) {
            if (!HasFace(active_faces, face)) {
                continue;
            }

            append_edge_skirt(i0, i1, face);
            append_edge_skirt(i1, i2, face);
            append_edge_skirt(i2, i0, face);
        }
    }

    for (const TerrainTransitionFace face : kFaces) {
        if (HasFace(active_faces, face) && face_edges_added[TransitionFaceSlot(face)] == 0u) {
            AppendFallbackFacePatches(chunk, step, face, stats);
        }
    }

    return stats;
}

// ===================== NEW WATER MESH GENERATION =====================

void GenerateWaterMesh(
    const Systems::WaterSystem& water_system,
    const Systems::SHIELD_WorldSystem& world_system,
    Chunk& chunk
) {
    (void)water_system;
    const int resolution = chunk.current_water_resolution.load(std::memory_order_acquire);
    std::vector<VoxelVertex> water_vertices;
    std::vector<u32> water_indices;
    water_vertices.reserve(static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution) * 4u);
    water_indices.reserve(static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution) * 6u);

    if (!HasCompleteWaterGrid(chunk, resolution)) {
        chunk.water_mesh_vertices.clear();
        chunk.water_mesh_indices.clear();
        chunk.water_mesh_generated.store(false, std::memory_order_release);
        return;
    }

    const IVec3 chunk_base_pos = chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    const Vec3 normal = {0.0f, 1.0f, 0.0f}; // Water surface normal is always up
    const u32 water_mat_id = static_cast<u32>(MaterialType::Water);
    constexpr float kMinRenderableWaterDepth = 0.05f;

    const float cell_width_x = static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(resolution);
    const float cell_width_z = static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(resolution);

    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            // Get the water and terrain heights at the four corners of this water grid cell
            float world_x0 = chunk_base_pos.x + x * cell_width_x;
            float world_z0 = chunk_base_pos.z + z * cell_width_z;
            float world_x1 = world_x0 + cell_width_x;
            float world_z1 = world_z0 + cell_width_z;

            float water_h00 = SampleChunkWaterLevel(chunk, world_x0, world_z0, resolution);
            float water_h10 = SampleChunkWaterLevel(chunk, world_x1, world_z0, resolution);
            float water_h01 = SampleChunkWaterLevel(chunk, world_x0, world_z1, resolution);
            float water_h11 = SampleChunkWaterLevel(chunk, world_x1, world_z1, resolution);

            float terrain_h00 = world_system.GetTerrainHeightAt(world_x0, world_z0);
            float terrain_h10 = world_system.GetTerrainHeightAt(world_x1, world_z0);
            float terrain_h01 = world_system.GetTerrainHeightAt(world_x0, world_z1);
            float terrain_h11 = world_system.GetTerrainHeightAt(world_x1, world_z1);
            
            // Only generate a quad if water has meaningful depth above the terrain at any corner.
            if ((water_h00 - terrain_h00) > kMinRenderableWaterDepth ||
                (water_h10 - terrain_h10) > kMinRenderableWaterDepth ||
                (water_h01 - terrain_h01) > kMinRenderableWaterDepth ||
                (water_h11 - terrain_h11) > kMinRenderableWaterDepth) {
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
    chunk.water_mesh_generated.store(true, std::memory_order_release);
}

} // namespace World::MarchingCubes
} // namespace Luminumbra
