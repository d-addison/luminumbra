#include "MarchingCubes.h"
#include "../../../include/luminumbra/core/Types.h"
#include "../core/Log.h"
#include "FarLodStore.h"
#include "core/Crc32.h"
#include "systems/SHIELD_WorldSystem.h"
#include "systems/WaterSystem.h"
#include "world/Chunk.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Luminumbra {
namespace World::MarchingCubes {

// ===================== CORE MARCHING CUBES HELPERS (REFACTORED) =====================
namespace { // Anonymous namespace for internal implementation details

// =====================  RESET-PER-JOB MESHING ARENA =====================
// Reset-per-job linear (bump) allocator for the meshing hot path. This is a
// pure ALLOCATION-STRATEGY change: it replaces the repeated malloc/free of
// the per-job PURE-SCRATCH buffers (the index-addressed temporaries that are
// NEVER moved into the chunk - edge_vertex_cache, world_positions, materials,
// remap) with bump allocations from a thread_local arena that is RESET (not
// freed) at the start of each meshing job and reused across jobs, growing
// only to a per-worker high-water mark.
//
// Thread-safety: the arena is thread_local, so each JobSystem worker owns its
// own arena. Meshing runs inside Job lambdas on worker threads (see
// SHIELD_WorldSystem chunk-build / remesh job loops), so an arena is never
// shared across concurrently running jobs - no locks, no atomics, no sharing.
//
// Byte-identical guarantee: the arena only backs index-addressed, FIXED-SIZE
// scratch buffers. None of them GROW (no push_back / reallocation) and none
// ESCAPE the meshing function (the buffers that ARE moved into the chunk -
// mesh_vertices / mesh_indices / compact_vertices / water meshes - stay real
// owning std::vectors, because the Chunk takes ownership and outlives the
// job). Each arena buffer is value-initialized to the SAME init value the old
// std::vector ctor used (kNoCachedVertex, 0xFFFFFFFF remap sentinel, or
// default), so every byte the mesher reads/writes is identical. The arena
// therefore changes WHERE scratch memory comes from, never the geometry.
//
// Skirt by-value invariant: UNAFFECTED. The
// dangling-reference hazard lives in the skirt generators, which read/grow
// `vertices` / `out_mesh.vertices` - those remain real std::vectors that can
// still reallocate on push_back, so the existing by-value source-vertex
// copies are still required and are left exactly in place. The arena never
// backs any vertex buffer that a skirt generator pushes into.
class MeshArena {
public:
    // Reset to empty without releasing the backing storage (reused next job).
    void Reset() noexcept {
        m_offset = 0;
    }

    unsigned char* base() const noexcept {
        return m_storage.get();
    }

    // Bump-allocate `count` objects of trivially-copyable T, value-set to
    // `init`. Returns the BYTE OFFSET of the allocation within the arena
    // block (NOT a raw pointer). Callers resolve the pointer lazily against
    // base on every access (see ArenaSpan), which keeps outstanding
    // allocations valid across a mid-job grow: a grow that needs more space
    // while earlier allocations are still live copies the existing
    // [0, m_offset) bytes into the larger block, so resolving the offset
    // against the NEW base yields the same logical data. This is the
    // determinism-critical property - an earlier raw-pointer design dangled
    // world_positions when the subsequent materials allocation reallocated
    // the block, leaking job-order-dependent garbage and breaking the
    // canonical world_hash.
    template<typename T>
    std::size_t Allocate(std::size_t count, const T& init) {
        static_assert(std::is_trivially_copyable<T>::value,
                      " arena only backs trivially-copyable scratch");
        static_assert(std::is_trivially_destructible<T>::value, " arena never runs destructors");
        if (count == 0) {
            return 0;
        }
        constexpr std::size_t align = alignof(T);
        const std::size_t aligned_offset = (m_offset + (align - 1)) & ~(align - 1);
        const std::size_t bytes = count * sizeof(T);
        EnsureCapacity(aligned_offset + bytes);
        T* const ptr = reinterpret_cast<T*>(m_storage.get() + aligned_offset);
        m_offset = aligned_offset + bytes;
        for (std::size_t i = 0; i < count; ++i) {
            // Placement-construct each element with the requested init value;
            // matches the std::vector(count, init) the arena replaces.
            ::new (static_cast<void*>(ptr + i)) T(init);
        }
        return aligned_offset;
    }

private:
    void EnsureCapacity(std::size_t required_bytes) {
        if (required_bytes <= m_capacity) {
            return;
        }
        // Grow geometrically to amortize. A grow may fire mid-job while
        // earlier allocations are still live, so preserve their bytes by
        // copying [0, m_offset) into the new block. Offset-based ArenaSpans
        // (which resolve against base lazily) then keep pointing at the
        // same logical scratch after the block moves.
        std::size_t new_capacity = m_capacity == 0 ? 4096 : m_capacity;
        while (new_capacity < required_bytes) {
            new_capacity *= 2;
        }
        // Over-aligned to the strictest scratch type we hand out so every
        // typed sub-allocation's alignment math stays in-bounds.
        constexpr std::size_t kBlockAlign = alignof(std::max_align_t);
        auto* raw = static_cast<unsigned char*>(
            ::operator new(new_capacity, std::align_val_t{kBlockAlign}));
        if (m_storage && m_offset > 0) {
            std::memcpy(raw, m_storage.get(), m_offset);
        }
        m_storage = std::unique_ptr<unsigned char[], BlockDeleter>(raw);
        m_capacity = new_capacity;
    }

    struct BlockDeleter {
        void operator()(unsigned char* p) const noexcept {
            ::operator delete(p, std::align_val_t{alignof(std::max_align_t)});
        }
    };

    std::unique_ptr<unsigned char[], BlockDeleter> m_storage;
    std::size_t m_capacity = 0; // high-water mark in bytes (never shrinks)
    std::size_t m_offset = 0;   // bump cursor; Reset() rewinds to 0
};

// Per-worker meshing arena. thread_local => one arena per JobSystem worker,
// never shared across concurrently running meshing jobs.
thread_local MeshArena t_mesh_arena;

// RAII guard: reset the worker arena at job entry so each meshing job starts
// from a clean bump cursor while reusing the high-water-mark storage.
struct MeshArenaScope {
    MeshArenaScope() noexcept {
        t_mesh_arena.Reset();
    }
    ~MeshArenaScope() noexcept {
        t_mesh_arena.Reset();
    }
    MeshArenaScope(const MeshArenaScope&) = delete;
    MeshArenaScope& operator=(const MeshArenaScope&) = delete;
};

// Lightweight typed view over an arena allocation. Index-addressed only (the
// arena never backs a growing buffer), so it deliberately exposes no
// push_back: that keeps the no-reallocation / stable-address guarantee that
// makes the scratch byte-identical to the std::vectors it replaces. The view
// holds a BYTE OFFSET, not a pointer, and resolves data against the current
// arena base on every access, so it survives a mid-job arena grow (the grow
// copies existing bytes forward; see MeshArena::EnsureCapacity).
template<typename T>
struct ArenaSpan {
    std::size_t offset = 0;
    std::size_t count = 0;
    T* data() const noexcept {
        return count == 0 ? nullptr : reinterpret_cast<T*>(t_mesh_arena.base() + offset);
    }
    std::size_t size() const noexcept {
        return count;
    }
    T& operator[](std::size_t i) const noexcept {
        return data()[i];
    }
};

template<typename T>
ArenaSpan<T> ArenaAlloc(std::size_t count, const T& init) {
    return ArenaSpan<T>{t_mesh_arena.Allocate<T>(count, init), count};
}
// ===================== END  MESHING ARENA =====================

// These tables are the core of the Marching Cubes algorithm
#include "MarchingCubesTables.inl"

struct GridCell {
    Vec3 p[8];  // Position of the 8 corners of the cube
    f32 val[8]; // SDF value at each of the 8 corners
};

// Linearly interpolates to find the point on an edge where the surface crosses
Vec3 VertexInterp(f32 isolevel, Vec3 p1, Vec3 p2, f32 valp1, f32 valp2) {
    if (std::abs(valp1 - valp2) < 0.00001f)
        return p1;
    f32 mu = (isolevel - valp1) / (valp2 - valp1);
    return p1 + mu * (p2 - p1);
}

Vec3 EstimateDensityGradient(const GridCell& gridcell) {
    const f32 dx = (gridcell.val[1] + gridcell.val[2] + gridcell.val[5] + gridcell.val[6]) -
                   (gridcell.val[0] + gridcell.val[3] + gridcell.val[4] + gridcell.val[7]);
    const f32 dy = (gridcell.val[4] + gridcell.val[5] + gridcell.val[6] + gridcell.val[7]) -
                   (gridcell.val[0] + gridcell.val[1] + gridcell.val[2] + gridcell.val[3]);
    const f32 dz = (gridcell.val[2] + gridcell.val[3] + gridcell.val[6] + gridcell.val[7]) -
                   (gridcell.val[0] + gridcell.val[1] + gridcell.val[4] + gridcell.val[5]);

    return Vec3(dx, dy, dz);
}

// Determines terrain material based on world position and height.
// Meshable terrain must never carry the non-rendering Air material into the
// G-buffer; isosurface interpolation can land just outside the solid side.
MaterialType GetTerrainMaterialAt(const Systems::SHIELD_WorldSystem& world_system,
                                  const Vec3& world_pos) {
    const auto sample = world_system.SampleWorldGenLayers(world_pos - Vec3(0.0f, 0.25f, 0.0f));
    if (sample.material != MaterialType::Air && sample.material != MaterialType::Water) {
        return sample.material;
    }

    // Fallback when isosurface interpolation landed just outside the solid
    // side (sample classified Air/Water): reclassify from the column height
    // through the SAME biome-aware band selector. With biomes
    // disabled this reproduces the legacy Sand/Grass/Soil/Stone bands
    // bit-for-bit.
    const float terrain_height = world_system.GetTerrainHeightAt(world_pos.x, world_pos.z);
    const u8 biome_id = world_system.BiomeIdAt(world_pos.x, world_pos.z);
    const bool river_bank = world_system.RiverInfluenceAt(world_pos.x, world_pos.z) > 0.25f;
    return world_system.SurfaceMaterialForColumn(world_pos.y, terrain_height, biome_id, river_bank);
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

void RecordTerrainMeshBuildStats(int step,
                                 std::size_t cells_visited,
                                 std::size_t active_cells,
                                 std::size_t vertices,
                                 std::size_t indices,
                                 std::uint64_t elapsed_us) {
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
            std::abs(vertex.normal.z - face_normal.z) <= kBoundaryEpsilon) {
            ++stamped_vertices;
        }
    }
    return stamped_vertices >= 2u;
}

bool AppendOrientedTriangle(std::vector<u32>& indices,
                            const std::vector<VoxelVertex>& vertices,
                            u32 a,
                            u32 b,
                            u32 c,
                            const Vec3& desired_normal) {
    if (a == b || b == c || c == a) {
        return false;
    }

    Vec3 face_normal = glm::cross(vertices[b].position - vertices[a].position,
                                  vertices[c].position - vertices[a].position);
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

Vec3 TransitionFacePosition(TerrainTransitionFace face, int major, int y) {
    switch (face) {
        case TransitionFaceMinX:
            return Vec3(0.0f, static_cast<float>(y), static_cast<float>(major));
        case TransitionFaceMaxX:
            return Vec3(
                static_cast<float>(CHUNK_SIZE_X), static_cast<float>(y), static_cast<float>(major));
        case TransitionFaceMinZ:
            return Vec3(static_cast<float>(major), static_cast<float>(y), 0.0f);
        case TransitionFaceMaxZ:
            return Vec3(
                static_cast<float>(major), static_cast<float>(y), static_cast<float>(CHUNK_SIZE_Z));
    }
    return Vec3(0.0f);
}

// Terrain density at a lattice point on a horizontal chunk face. A resident
// full SDF is authoritative, including its cave/edit crossings, so use it
// first. Empty-SDF coarse chunks retain the heightmap-derived fallback.
bool ReadTransitionFaceTerrainDensity(
    const Chunk& chunk, TerrainTransitionFace face, int major, int y, float& value) {
    if (major < 0 || major > CHUNK_SIZE_X || y < 0 || y > CHUNK_SIZE_Y) {
        return false;
    }
    int x = 0;
    int z = 0;
    switch (face) {
        case TransitionFaceMinX:
            x = 0;
            z = major;
            break;
        case TransitionFaceMaxX:
            x = CHUNK_SIZE_X;
            z = major;
            break;
        case TransitionFaceMinZ:
            x = major;
            z = 0;
            break;
        case TransitionFaceMaxZ:
            x = major;
            z = CHUNK_SIZE_Z;
            break;
    }
    constexpr std::size_t kSdfSizeX = CHUNK_SIZE_X + 1;
    constexpr std::size_t kFullLatticeCount = kSdfSizeX *
                                              static_cast<std::size_t>(CHUNK_SIZE_Y + 1) *
                                              static_cast<std::size_t>(CHUNK_SIZE_Z + 1);
    const std::size_t sdf_index =
        static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * kSdfSizeX +
        static_cast<std::size_t>(z) * kSdfSizeX * static_cast<std::size_t>(CHUNK_SIZE_Y + 1);
    if (chunk.sdf_data.size() == kFullLatticeCount) {
        value = chunk.sdf_data[sdf_index];
        return true;
    }

    const std::size_t heightmap_index =
        static_cast<std::size_t>(x) + static_cast<std::size_t>(z) * kSdfSizeX;
    if (heightmap_index >= chunk.heightmap_data.size()) {
        return false;
    }
    const float world_y = static_cast<float>(chunk.get_coords().y * CHUNK_SIZE_Y + y);
    value = world_y - chunk.heightmap_data[heightmap_index];
    return true;
}

bool HasCompleteWaterGrid(const Chunk& chunk, int resolution) {
    if (!chunk.has_water_sim.load(std::memory_order_relaxed) || resolution <= 1) {
        return false;
    }
    const std::size_t cell_count =
        static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
    return chunk.water_level_data.size() >= cell_count;
}

float SampleChunkWaterLevel(const Chunk& chunk, float world_x, float world_z, int resolution) {
    const Vec3 base_pos = Vec3(chunk.get_coords() * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z));
    const float local_x = world_x - base_pos.x;
    const float local_z = world_z - base_pos.z;

    const float sim_xf =
        (local_x / static_cast<float>(CHUNK_SIZE_X)) * static_cast<float>(resolution) - 0.5f;
    const float sim_zf =
        (local_z / static_cast<float>(CHUNK_SIZE_Z)) * static_cast<float>(resolution) - 0.5f;

    int x0 = static_cast<int>(std::floor(sim_xf));
    int z0 = static_cast<int>(std::floor(sim_zf));
    x0 = std::clamp(x0, 0, resolution - 2);
    z0 = std::clamp(z0, 0, resolution - 2);

    const float tx = std::clamp(sim_xf - static_cast<float>(x0), 0.0f, 1.0f);
    const float tz = std::clamp(sim_zf - static_cast<float>(z0), 0.0f, 1.0f);

    const float h00 = chunk.water_level_data[static_cast<std::size_t>(z0 * resolution + x0)];
    const float h10 = chunk.water_level_data[static_cast<std::size_t>(z0 * resolution + (x0 + 1))];
    const float h01 = chunk.water_level_data[static_cast<std::size_t>((z0 + 1) * resolution + x0)];
    const float h11 =
        chunk.water_level_data[static_cast<std::size_t>((z0 + 1) * resolution + (x0 + 1))];

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

void AppendFallbackFacePatches(Chunk& chunk,
                               int step,
                               TerrainTransitionFace face,
                               TerrainTransitionSkirtStats& stats) {
    constexpr std::size_t kFullLatticeCount =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    if (chunk.heightmap_data.empty() && chunk.sdf_data.size() != kFullLatticeCount) {
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
            if (!ReadTransitionFaceTerrainDensity(chunk, face, major, y, v00) ||
                !ReadTransitionFaceTerrainDensity(chunk, face, major + sample_step, y, v10) ||
                !ReadTransitionFaceTerrainDensity(chunk, face, major, y + sample_step, v01) ||
                !ReadTransitionFaceTerrainDensity(
                    chunk, face, major + sample_step, y + sample_step, v11)) {
                continue;
            }

            const float min_density = std::min(std::min(v00, v10), std::min(v01, v11));
            const float max_density = std::max(std::max(v00, v10), std::max(v01, v11));
            const bool crosses_surface = min_density <= 0.0f && max_density >= 0.0f;
            bool near_surface = std::abs(v00) <= 0.75f || std::abs(v10) <= 0.75f ||
                                std::abs(v01) <= 0.75f || std::abs(v11) <= 0.75f;
            for (int local_major = 0; local_major <= sample_step && !near_surface; ++local_major) {
                for (int local_y = 0; local_y <= sample_step; ++local_y) {
                    float sample = 0.0f;
                    if (ReadTransitionFaceTerrainDensity(
                            chunk, face, major + local_major, y + local_y, sample) &&
                        std::abs(sample) <= 0.75f) {
                        near_surface = true;
                        break;
                    }
                }
            }
            if (!crosses_surface && !near_surface) {
                continue;
            }

            const u32 base = static_cast<u32>(chunk.mesh_vertices.size());
            chunk.mesh_vertices.push_back(
                {TransitionFacePosition(face, major, y), face_normal, material_id});
            chunk.mesh_vertices.push_back(
                {TransitionFacePosition(face, major + sample_step, y), face_normal, material_id});
            chunk.mesh_vertices.push_back(
                {TransitionFacePosition(face, major + sample_step, y + sample_step),
                 face_normal,
                 material_id});
            chunk.mesh_vertices.push_back(
                {TransitionFacePosition(face, major, y + sample_step), face_normal, material_id});

            const std::size_t index_count_before = chunk.mesh_indices.size();
            const bool first = AppendOrientedTriangle(
                chunk.mesh_indices, chunk.mesh_vertices, base, base + 1u, base + 2u, face_normal);
            const bool second = AppendOrientedTriangle(
                chunk.mesh_indices, chunk.mesh_vertices, base, base + 2u, base + 3u, face_normal);
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

void GenerateCoarseHeightfieldTerrain(const Systems::SHIELD_WorldSystem& world_system,
                                      Chunk& chunk,
                                      int sample_step,
                                      const std::chrono::steady_clock::time_point& build_start) {
    const IVec3 chunk_base_pos =
        chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    const int cells_x = CHUNK_SIZE_X / sample_step;
    const int cells_z = CHUNK_SIZE_Z / sample_step;
    const int vertices_x = cells_x + 1;
    const int vertices_z = cells_z + 1;

    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;
    vertices.reserve(static_cast<std::size_t>(vertices_x * vertices_z));
    indices.reserve(static_cast<std::size_t>(cells_x * cells_z * 6));

    // when the chunk already carries its surface-band
    // heightmap (generated by GenerateChunkData for this same step) AND the
    // world has no rivers, the per-vertex height is BYTE-IDENTICAL to
    // GetTerrainHeightAtCoarse - the coarse grid points (local = g*step) are
    // exactly the heightmap lattice, and for !rivers_enabled the coarse
    // carve anti-alias is a no-op (GetTerrainHeightAtCoarse just forwards to
    // GetTerrainHeightAt, whose bytes the heightmap already holds, proven by
    // the batch-vs-scalar parity gtest). Reading the cached heightmap then
    // skips one full per-column shaped-noise re-evaluation per vertex (the
    // dominant meshing cost on shaped presets). River worlds and chunks with
    // no heightmap keep the analytic coarse sampler unchanged.
    const int hm_size_x = CHUNK_SIZE_X + 1;
    const bool use_cached_heightmap =
        !world_system.get_params().rivers_enabled &&
        chunk.heightmap_data.size() ==
            static_cast<std::size_t>(hm_size_x) * static_cast<std::size_t>(CHUNK_SIZE_Z + 1);

    for (int gz = 0; gz < vertices_z; ++gz) {
        const int local_z = std::min(CHUNK_SIZE_Z, gz * sample_step);
        const float world_z = static_cast<float>(chunk_base_pos.z + local_z);
        for (int gx = 0; gx < vertices_x; ++gx) {
            const int local_x = std::min(CHUNK_SIZE_X, gx * sample_step);
            const float world_x = static_cast<float>(chunk_base_pos.x + local_x);
            // coarse-LOD river-carve anti-alias
            // (matches the far tile sampler) so the live coarse ring does
            // not aliased-notch a narrow channel into a sliver triangle.
            const float terrain_height =
                use_cached_heightmap
                    ? chunk.heightmap_data[static_cast<std::size_t>(local_x) +
                                           static_cast<std::size_t>(local_z) *
                                               static_cast<std::size_t>(hm_size_x)]
                    : world_system.GetTerrainHeightAtCoarse(world_x, world_z, sample_step);
            const float local_y = terrain_height - static_cast<float>(chunk_base_pos.y);
            // on the cached-heightmap (non-river) path
            // the surface height is already known, so classify the skin
            // material WITHOUT a second shaped-height re-evaluation. Returns
            // bytes identical to GetTerrainMaterialAt for a surface vertex
            // (proven equivalent in SurfaceVertexMaterial). The analytic
            // fallback keeps the exact prior call for river / no-heightmap
            // chunks.
            const MaterialType material =
                use_cached_heightmap
                    ? world_system.SurfaceVertexMaterial(world_x, world_z, terrain_height)
                    : GetTerrainMaterialAt(world_system,
                                           Vec3(world_x, terrain_height - 0.1f, world_z));
            vertices.push_back(
                {Vec3(static_cast<float>(local_x), local_y, static_cast<float>(local_z)),
                 Vec3(0.0f),
                 static_cast<u32>(material)});
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
            const float cell_surface_y = (vertices[i00].position.y + vertices[i10].position.y +
                                          vertices[i01].position.y + vertices[i11].position.y) *
                                         0.25f;

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

    // remap is pure index-addressed scratch (never escapes), so it
    // comes from the reset-per-job arena. Value-set to 0xFFFFFFFF, the exact
    // sentinel the prior std::vector(count, -1) used - byte-identical compaction.
    ArenaSpan<u32> remap = ArenaAlloc<u32>(vertices.size(), static_cast<u32>(-1));
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
    const auto elapsed_us =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - build_start)
                                       .count());
    RecordTerrainMeshBuildStats(sample_step,
                                cells_visited,
                                active_cells,
                                chunk.mesh_vertices.size(),
                                chunk.mesh_indices.size(),
                                elapsed_us);
}

} // anonymous namespace

MaterialType TerrainSurfaceMaterialAt(const Systems::SHIELD_WorldSystem& world_system,
                                      float world_x,
                                      float world_z,
                                      float terrain_height) {
    // Identical sampling to GenerateCoarseHeightfieldTerrain: just below the
    // surface so the classifier never lands on Air/Water.
    return GetTerrainMaterialAt(world_system, Vec3(world_x, terrain_height - 0.1f, world_z));
}

FarLodRegionMeshStats GenerateFarLodRegionMesh(const World::FarLodTile& tile,
                                               World::FarLodRegionMesh& out_mesh) {
    FarLodRegionMeshStats stats;
    out_mesh.vertices.clear();
    out_mesh.indices.clear();

    const u32 n = tile.samples_per_side;
    if ((tile.tier != World::FarLodTier::F1 && tile.tier != World::FarLodTier::F2) ||
        n != World::FarLodSamplesPerSide(tile.tier) ||
        tile.height_q.size() != tile.sample_count() ||
        tile.material.size() != tile.sample_count() || tile.flags.size() != tile.sample_count()) {
        return stats;
    }

    const float step = static_cast<float>(World::FarLodSampleStepMeters(tile.tier));
    std::vector<VoxelVertex>& vertices = out_mesh.vertices;
    std::vector<u32>& indices = out_mesh.indices;
    const std::size_t surface_vertex_count = tile.sample_count();
    vertices.reserve(surface_vertex_count + static_cast<std::size_t>(4u * (n - 1u)) * 4u);
    indices.reserve(static_cast<std::size_t>(n - 1u) * (n - 1u) * 6u +
                    static_cast<std::size_t>(4u * (n - 1u)) * 6u);

    // The no-brick path below deliberately remains byte-for-byte the original
    // heightfield mesher. Once a tile carries an SDF brick, the brick's chunk
    // column and a one-column halo are owned by the 3D path so the two meshers
    // never emit the same horizontal cells.
    const bool has_sdf_bricks = !tile.sdf_bricks.empty();
    std::vector<bool> sdf_owned_columns(32u * 32u, false);
    struct SdfSampleKey {
        int x;
        int y;
        int z;

        bool operator==(const SdfSampleKey& other) const {
            return x == other.x && y == other.y && z == other.z;
        }
    };
    struct SdfSampleKeyHash {
        std::size_t operator()(const SdfSampleKey& key) const {
            std::size_t hash = std::hash<int>{}(key.x);
            hash ^= std::hash<int>{}(key.y) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
            hash ^= std::hash<int>{}(key.z) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
            return hash;
        }
    };
    struct SdfSample {
        float density;
        u8 material;
    };
    std::unordered_map<SdfSampleKey, SdfSample, SdfSampleKeyHash> sdf_samples;
    struct SdfColumnStack {
        bool present = false;
        bool authoritative = false;
        int min_y = 0;
        int max_y = 0;
        int authority_min_y = 0;
        int authority_max_y = 0;
    };
    std::array<SdfColumnStack, 32u * 32u> sdf_column_stacks{};

    if (has_sdf_bricks) {
        const std::size_t brick_samples = World::FarLodSdfBrickSampleCount(tile.tier);
        if (tile.sdf_density_q.size() != tile.sdf_bricks.size() * brick_samples ||
            tile.sdf_material.size() != tile.sdf_bricks.size() * brick_samples) {
            // A malformed authoritative stream must not fall back to a surface
            // reconstruction. Persistence validates this too; this guard keeps
            // direct mesher callers from producing a misleading mesh.
            return stats;
        }

        sdf_samples.reserve(tile.sdf_bricks.size() * brick_samples);
        const int sdf_step = World::FarLodSampleStepMeters(tile.tier);
        bool have_previous_descriptor = false;
        World::FarLodSdfBrickDescriptor previous_descriptor{};
        for (std::size_t brick_index = 0; brick_index < tile.sdf_bricks.size(); ++brick_index) {
            const World::FarLodSdfBrickDescriptor& brick = tile.sdf_bricks[brick_index];
            if (brick.local_chunk_x >= 32u || brick.local_chunk_z >= 32u || brick.reserved != 0u ||
                (brick.source_kind != World::FarLodBrickSourceKind::RegenerableCache &&
                 brick.source_kind != World::FarLodBrickSourceKind::Authoritative) ||
                brick.chunk_y < std::numeric_limits<int>::min() / CHUNK_SIZE_Y ||
                brick.chunk_y > std::numeric_limits<int>::max() / CHUNK_SIZE_Y) {
                return stats;
            }

            if (have_previous_descriptor &&
                (brick.local_chunk_z < previous_descriptor.local_chunk_z ||
                 (brick.local_chunk_z == previous_descriptor.local_chunk_z &&
                  (brick.local_chunk_x < previous_descriptor.local_chunk_x ||
                   (brick.local_chunk_x == previous_descriptor.local_chunk_x &&
                    brick.chunk_y <= previous_descriptor.chunk_y))))) {
                // Descriptor order is part of the payload contract.  Rejecting a
                // duplicate here also prevents a second brick from owning the
                // same half-open cell range.
                return stats;
            }
            previous_descriptor = brick;
            have_previous_descriptor = true;

            const int base_x = static_cast<int>(brick.local_chunk_x) * CHUNK_SIZE_X;
            const int base_y = brick.chunk_y * CHUNK_SIZE_Y;
            const int base_z = static_cast<int>(brick.local_chunk_z) * CHUNK_SIZE_Z;
            SdfColumnStack& stack =
                sdf_column_stacks[static_cast<std::size_t>(brick.local_chunk_x) +
                                  static_cast<std::size_t>(brick.local_chunk_z) * 32u];
            if (!stack.present) {
                stack.present = true;
                stack.authoritative =
                    brick.source_kind == World::FarLodBrickSourceKind::Authoritative;
                stack.min_y = base_y;
                stack.max_y = base_y;
                if (stack.authoritative) {
                    stack.authority_min_y = base_y;
                    stack.authority_max_y = base_y;
                }
            } else {
                // A stack is only complete when every chunk-Y slab is present.
                // The descriptor stream is sorted by (z, x, y), so a gap is
                // unambiguous and cannot safely be filled from a heightfield.
                if (stack.max_y > std::numeric_limits<int>::max() - CHUNK_SIZE_Y ||
                    base_y != stack.max_y + CHUNK_SIZE_Y) {
                    return stats;
                }
                stack.max_y = base_y;
                if (brick.source_kind == World::FarLodBrickSourceKind::Authoritative) {
                    if (!stack.authoritative) {
                        stack.authority_min_y = base_y;
                        stack.authority_max_y = base_y;
                    } else {
                        stack.authority_min_y = std::min(stack.authority_min_y, base_y);
                        stack.authority_max_y = std::max(stack.authority_max_y, base_y);
                    }
                    stack.authoritative = true;
                }
            }

            const std::size_t payload_base = brick_index * brick_samples;
            Core::Crc32Accumulator payload_crc;
            payload_crc.Update(tile.sdf_density_q.data() + payload_base,
                               brick_samples * sizeof(i16));
            payload_crc.Update(tile.sdf_material.data() + payload_base, brick_samples);
            if (brick.payload_crc32 != payload_crc.Value()) {
                return stats;
            }
            const u32 side = World::FarLodSdfBrickSamplesPerSide(tile.tier);
            for (u32 local_z = 0; local_z < side; ++local_z) {
                for (u32 local_y = 0; local_y < side; ++local_y) {
                    for (u32 local_x = 0; local_x < side; ++local_x) {
                        const std::size_t sample_index =
                            payload_base + static_cast<std::size_t>(local_x) +
                            static_cast<std::size_t>(local_y) * side +
                            static_cast<std::size_t>(local_z) * side * side;
                        const i16 density_q = tile.sdf_density_q[sample_index];
                        if (density_q == World::kFarLodSdfInvalid) {
                            return stats;
                        }
                        const SdfSampleKey key{base_x + static_cast<int>(local_x) * sdf_step,
                                               base_y + static_cast<int>(local_y) * sdf_step,
                                               base_z + static_cast<int>(local_z) * sdf_step};
                        const SdfSample sample{World::DequantizeFarLodSdf(density_q),
                                               tile.sdf_material[sample_index]};
                        const auto [it, inserted] = sdf_samples.emplace(key, sample);
                        if (!inserted && (it->second.density != sample.density ||
                                          it->second.material != sample.material)) {
                            // Shared brick faces are a single world-aligned
                            // lattice. Unequal duplicates are corrupt authority,
                            // never an invitation to choose one side.
                            return stats;
                        }
                    }
                }
            }
        }

        bool have_authoritative_column = false;
        for (int column_z = 0; column_z < 32; ++column_z) {
            for (int column_x = 0; column_x < 32; ++column_x) {
                const SdfColumnStack& source =
                    sdf_column_stacks[static_cast<std::size_t>(column_x) +
                                      static_cast<std::size_t>(column_z) * 32u];
                if (!source.authoritative) {
                    continue;
                }
                have_authoritative_column = true;

                // A real SDF column is surrounded by already-reduced analytic
                // bricks.  We never manufacture that halo in this mesher: every
                // one of its stacks must be present and cover the same vertical
                // interval before it can take ownership from the heightfield.
                if (column_x == 0 || column_x == 31 || column_z == 0 || column_z == 31) {
                    return stats;
                }
                int required_min_chunk_y = source.authority_min_y / CHUNK_SIZE_Y - 1;
                int required_max_chunk_y = source.authority_max_y / CHUNK_SIZE_Y + 1;
                for (int halo_z = column_z - 1; halo_z <= column_z + 1; ++halo_z) {
                    for (int halo_x = column_x - 1; halo_x <= column_x + 1; ++halo_x) {
                        const int base_x = halo_x * CHUNK_SIZE_X;
                        const int base_z = halo_z * CHUNK_SIZE_Z;
                        for (int local_z = 0; local_z <= CHUNK_SIZE_Z; local_z += sdf_step) {
                            for (int local_x = 0; local_x <= CHUNK_SIZE_X; local_x += sdf_step) {
                                const std::size_t sample_x =
                                    static_cast<std::size_t>((base_x + local_x) / sdf_step);
                                const std::size_t sample_z =
                                    static_cast<std::size_t>((base_z + local_z) / sdf_step);
                                const float height = World::DequantizeFarLodHeight(
                                    tile.height_q[sample_x + sample_z * n]);
                                const int surface_chunk_y = static_cast<int>(
                                    std::floor(height / static_cast<float>(CHUNK_SIZE_Y)));
                                required_min_chunk_y =
                                    std::min(required_min_chunk_y, surface_chunk_y - 1);
                                required_max_chunk_y =
                                    std::max(required_max_chunk_y, surface_chunk_y + 1);
                            }
                        }
                    }
                }
                const int required_min_y = required_min_chunk_y * CHUNK_SIZE_Y;
                const int required_max_y = required_max_chunk_y * CHUNK_SIZE_Y;
                for (int halo_z = column_z - 1; halo_z <= column_z + 1; ++halo_z) {
                    for (int halo_x = column_x - 1; halo_x <= column_x + 1; ++halo_x) {
                        const SdfColumnStack& halo =
                            sdf_column_stacks[static_cast<std::size_t>(halo_x) +
                                              static_cast<std::size_t>(halo_z) * 32u];
                        if (!halo.present || halo.min_y > required_min_y ||
                            halo.max_y < required_max_y) {
                            return stats;
                        }
                        sdf_owned_columns[static_cast<std::size_t>(halo_x) +
                                          static_cast<std::size_t>(halo_z) * 32u] = true;
                    }
                }
            }
        }
        if (!have_authoritative_column) {
            return stats;
        }
    }

    const int sample_step_i = World::FarLodSampleStepMeters(tile.tier);
    const auto sdf_owned_cell = [&](u32 sample_x, u32 sample_z) {
        if (!has_sdf_bricks) {
            return false;
        }
        const std::size_t column_x =
            (static_cast<std::size_t>(sample_x) * sample_step_i) / CHUNK_SIZE_X;
        const std::size_t column_z =
            (static_cast<std::size_t>(sample_z) * sample_step_i) / CHUNK_SIZE_Z;
        return column_x < 32u && column_z < 32u && sdf_owned_columns[column_x + column_z * 32u];
    };

    // Surface lattice: region-local X/Z, absolute (dequantized) world Y.
    std::size_t sample_index = 0;
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x, ++sample_index) {
            vertices.push_back({Vec3(static_cast<float>(x) * step,
                                     World::DequantizeFarLodHeight(tile.height_q[sample_index]),
                                     static_cast<float>(z) * step),
                                Vec3(0.0f),
                                static_cast<u32>(tile.material[sample_index])});
        }
    }

    const auto vertex_index = [n](u32 x, u32 z) {
        return z * n + x;
    };

    // Whole-tile heightfield: every cell is emitted (a region tile owns its
    // full vertical extent - no chunk-Y ownership test, no ownership holes).
    for (u32 z = 0; z + 1u < n; ++z) {
        for (u32 x = 0; x + 1u < n; ++x) {
            if (sdf_owned_cell(x, z)) {
                continue;
            }
            const u32 i00 = vertex_index(x, z);
            const u32 i10 = vertex_index(x + 1u, z);
            const u32 i01 = vertex_index(x, z + 1u);
            const u32 i11 = vertex_index(x + 1u, z + 1u);
            indices.push_back(i00);
            indices.push_back(i11);
            indices.push_back(i10);
            indices.push_back(i00);
            indices.push_back(i01);
            indices.push_back(i11);
        }
    }

    if (has_sdf_bricks) {
        const IVec3 far_corner_offsets[8] = {
            {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}};
        const int far_edge_connections[12][2] = {{0, 1},
                                                 {1, 2},
                                                 {2, 3},
                                                 {3, 0},
                                                 {4, 5},
                                                 {5, 6},
                                                 {6, 7},
                                                 {7, 4},
                                                 {0, 4},
                                                 {1, 5},
                                                 {2, 6},
                                                 {3, 7}};
        const auto background_material = [&](const IVec3& position) {
            const std::size_t sample_x = static_cast<std::size_t>(position.x / sample_step_i);
            const std::size_t sample_z = static_cast<std::size_t>(position.z / sample_step_i);
            return tile.material[sample_x + sample_z * n];
        };
        const std::size_t brick_samples = World::FarLodSdfBrickSampleCount(tile.tier);
        const u32 brick_side = World::FarLodSdfBrickSamplesPerSide(tile.tier);

        // Descriptor order is canonical and every brick owns only its half-open
        // local cell range. Missing cells are never reconstructed from heights.
        for (std::size_t brick_index = 0; brick_index < tile.sdf_bricks.size(); ++brick_index) {
            const World::FarLodSdfBrickDescriptor& brick = tile.sdf_bricks[brick_index];
            if (!sdf_owned_columns[static_cast<std::size_t>(brick.local_chunk_x) +
                                   static_cast<std::size_t>(brick.local_chunk_z) * 32u]) {
                continue;
            }
            const int base_x = static_cast<int>(brick.local_chunk_x) * CHUNK_SIZE_X;
            const int base_y = brick.chunk_y * CHUNK_SIZE_Y;
            const int base_z = static_cast<int>(brick.local_chunk_z) * CHUNK_SIZE_Z;
            const std::size_t payload_base = brick_index * brick_samples;
            for (u32 local_z = 0; local_z + 1u < brick_side; ++local_z) {
                for (u32 local_y = 0; local_y + 1u < brick_side; ++local_y) {
                    for (u32 local_x = 0; local_x + 1u < brick_side; ++local_x) {
                        GridCell cell;
                        SdfSample corner_samples[8];
                        int cube_index = 0;
                        for (int corner = 0; corner < 8; ++corner) {
                            const IVec3 local = IVec3(static_cast<int>(local_x),
                                                      static_cast<int>(local_y),
                                                      static_cast<int>(local_z)) +
                                                far_corner_offsets[corner];
                            const std::size_t brick_sample_index =
                                payload_base + static_cast<std::size_t>(local.x) +
                                static_cast<std::size_t>(local.y) * brick_side +
                                static_cast<std::size_t>(local.z) * brick_side * brick_side;
                            corner_samples[corner] = {
                                World::DequantizeFarLodSdf(tile.sdf_density_q[brick_sample_index]),
                                tile.sdf_material[brick_sample_index]};
                            const IVec3 position(base_x + local.x * sample_step_i,
                                                 base_y + local.y * sample_step_i,
                                                 base_z + local.z * sample_step_i);
                            cell.p[corner] = Vec3(position);
                            cell.val[corner] = corner_samples[corner].density;
                            if (cell.val[corner] < 0.0f) {
                                cube_index |= 1 << corner;
                            }
                        }

                        const unsigned int edge_mask = edgeTable[cube_index];
                        if (edge_mask == 0u) {
                            continue;
                        }
                        u32 edge_vertices[12]{};
                        for (int edge = 0; edge < 12; ++edge) {
                            if ((edge_mask & (1u << edge)) == 0u) {
                                continue;
                            }
                            const int corner_a = far_edge_connections[edge][0];
                            const int corner_b = far_edge_connections[edge][1];
                            const int solid_corner =
                                cell.val[corner_a] < 0.0f ? corner_a : corner_b;
                            const u8 authored_material = corner_samples[solid_corner].material;
                            const IVec3 solid_position(static_cast<int>(cell.p[solid_corner].x),
                                                       static_cast<int>(cell.p[solid_corner].y),
                                                       static_cast<int>(cell.p[solid_corner].z));
                            const u8 material = authored_material == 0xffu
                                                    ? background_material(solid_position)
                                                    : authored_material;
                            edge_vertices[edge] = static_cast<u32>(vertices.size());
                            vertices.push_back({VertexInterp(0.0f,
                                                             cell.p[corner_a],
                                                             cell.p[corner_b],
                                                             cell.val[corner_a],
                                                             cell.val[corner_b]),
                                                Vec3(0.0f),
                                                static_cast<u32>(material)});
                        }

                        const Vec3 gradient = EstimateDensityGradient(cell);
                        const auto& triangle_row = triTable[cube_index];
                        for (int triangle = 0; triangle_row[triangle] != -1; triangle += 3) {
                            u32 i0 = edge_vertices[triangle_row[triangle]];
                            u32 i1 = edge_vertices[triangle_row[triangle + 1]];
                            u32 i2 = edge_vertices[triangle_row[triangle + 2]];
                            const Vec3 face_normal =
                                glm::cross(vertices[i1].position - vertices[i0].position,
                                           vertices[i2].position - vertices[i0].position);
                            if (glm::dot(face_normal, face_normal) <= 1.0e-10f) {
                                continue;
                            }
                            if (glm::dot(face_normal, gradient) < 0.0f) {
                                std::swap(i1, i2);
                            }
                            indices.push_back(i0);
                            indices.push_back(i1);
                            indices.push_back(i2);
                        }
                    }
                }
            }
        }
    }

    // Accumulated face normals over the surface lattice (same smoothing the
    // coarse chunk mesher uses).
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

    // Perimeter skirts dropped one sample-step deep: tile borders share
    // sample positions with the neighboring region (no crack on same-tier
    // boundaries), and the skirt masks the residual mismatch on tier
    // boundaries (F1 vs F2 sample density, live ring vs F1).
    const float drop = step;
    const auto add_skirt_edge = [&](u32 top_a, u32 top_b, const Vec3& outward) {
        // copy the two source vertices BY VALUE.
        // The four push_back calls below can reallocate `vertices`, and the
        // reads of b.position on the 2nd/3rd push (and a.position on the 4th)
        // would otherwise dereference dangling references (UB: garbage skirt
        // vertices, or a crash when the stale storage is reclaimed).
        const VoxelVertex a = vertices[top_a];
        const VoxelVertex b = vertices[top_b];
        const u32 base = static_cast<u32>(vertices.size());
        vertices.push_back({a.position, outward, a.material_id});
        vertices.push_back({b.position, outward, b.material_id});
        vertices.push_back({b.position - Vec3(0.0f, drop, 0.0f), outward, b.material_id});
        vertices.push_back({a.position - Vec3(0.0f, drop, 0.0f), outward, a.material_id});
        const bool first =
            AppendOrientedTriangle(indices, vertices, base, base + 1u, base + 2u, outward);
        const bool second =
            AppendOrientedTriangle(indices, vertices, base, base + 2u, base + 3u, outward);
        if (!first && !second) {
            vertices.pop_back();
            vertices.pop_back();
            vertices.pop_back();
            vertices.pop_back();
            return;
        }
        ++stats.skirt_quads;
    };
    for (u32 x = 0; x + 1u < n; ++x) {
        if (!sdf_owned_cell(x, 0)) {
            add_skirt_edge(vertex_index(x, 0), vertex_index(x + 1u, 0), Vec3(0.0f, 0.0f, -1.0f));
        }
        if (!sdf_owned_cell(x, n - 2u)) {
            add_skirt_edge(
                vertex_index(x, n - 1u), vertex_index(x + 1u, n - 1u), Vec3(0.0f, 0.0f, 1.0f));
        }
    }
    for (u32 z = 0; z + 1u < n; ++z) {
        if (!sdf_owned_cell(0, z)) {
            add_skirt_edge(vertex_index(0, z), vertex_index(0, z + 1u), Vec3(-1.0f, 0.0f, 0.0f));
        }
        if (!sdf_owned_cell(n - 2u, z)) {
            add_skirt_edge(
                vertex_index(n - 1u, z), vertex_index(n - 1u, z + 1u), Vec3(1.0f, 0.0f, 0.0f));
        }
    }

    stats.vertices = vertices.size();
    stats.indices = indices.size();
    stats.triangles = indices.size() / 3u;
    return stats;
}

FarLodRegionMeshStats GenerateFarLodRegionMesh(const World::FarLodTile& tile,
                                               const World::FarLodRegionSdfAssembly& assembly,
                                               World::FarLodRegionMesh& out_mesh) {
    // Preserve the original zero-authority path (including its mesh-byte pin),
    // but only for a genuinely empty assembly bound to this tile. Metadata
    // that claims authority without payload is malformed and must fail closed.
    if (assembly.bricks.empty()) {
        if (assembly.tier != tile.tier || assembly.rx != tile.rx || assembly.rz != tile.rz ||
            assembly.params_hash != tile.params_hash || !assembly.authority_columns.empty() ||
            !assembly.owned_columns.empty() || !assembly.density_q.empty() ||
            !assembly.material.empty() || !assembly.legacy_surface_samples.empty()) {
            out_mesh.vertices.clear();
            out_mesh.indices.clear();
            return {};
        }
        return GenerateFarLodRegionMesh(tile, out_mesh);
    }

    FarLodRegionMeshStats stats;
    out_mesh.vertices.clear();
    out_mesh.indices.clear();
    const auto valid_region_coordinate = [](int region) {
        const std::int64_t minimum =
            static_cast<std::int64_t>(region) * World::kFarLodRegionSizeMeters;
        const std::int64_t maximum = minimum + World::kFarLodRegionSizeMeters;
        const std::int64_t min_chunk = static_cast<std::int64_t>(region) * 32 - 1;
        const std::int64_t max_chunk = min_chunk + 33;
        return minimum >= std::numeric_limits<int>::min() &&
               maximum <= std::numeric_limits<int>::max() && min_chunk >= Chunk::kPackedMinXz &&
               max_chunk <= Chunk::kPackedMaxXz;
    };
    const auto valid_chunk_coordinate = [](int chunk, int extent) {
        const std::int64_t minimum = static_cast<std::int64_t>(chunk) * extent;
        const std::int64_t maximum = minimum + extent;
        return minimum >= std::numeric_limits<int>::min() &&
               maximum <= std::numeric_limits<int>::max();
    };
    const u32 n = tile.samples_per_side;
    const int step_i = World::FarLodSampleStepMeters(tile.tier);
    if (assembly.tier != tile.tier || assembly.rx != tile.rx || assembly.rz != tile.rz ||
        assembly.params_hash != tile.params_hash || !valid_region_coordinate(tile.rx) ||
        !valid_region_coordinate(tile.rz) || n != World::FarLodSamplesPerSide(tile.tier) ||
        tile.height_q.size() != tile.sample_count() ||
        tile.material.size() != tile.sample_count() || tile.flags.size() != tile.sample_count())
        return stats;
    const std::size_t count = World::FarLodSdfBrickSampleCount(tile.tier);
    if (assembly.density_q.size() != assembly.bricks.size() * count ||
        assembly.material.size() != assembly.density_q.size())
        return stats;

    const auto fail = [&]() -> FarLodRegionMeshStats {
        out_mesh.vertices.clear();
        out_mesh.indices.clear();
        return {};
    };
    if (!std::is_sorted(assembly.authority_columns.begin(), assembly.authority_columns.end()) ||
        std::adjacent_find(assembly.authority_columns.begin(), assembly.authority_columns.end()) !=
            assembly.authority_columns.end() ||
        !std::is_sorted(assembly.owned_columns.begin(), assembly.owned_columns.end()) ||
        std::adjacent_find(assembly.owned_columns.begin(), assembly.owned_columns.end()) !=
            assembly.owned_columns.end()) {
        return fail();
    }
    const auto legacy_less = [](const World::FarLodWorldLegacySurfaceSample& lhs,
                                const World::FarLodWorldLegacySurfaceSample& rhs) {
        return std::tie(lhs.world_z, lhs.world_x) < std::tie(rhs.world_z, rhs.world_x);
    };
    if (!std::is_sorted(assembly.legacy_surface_samples.begin(),
                        assembly.legacy_surface_samples.end(),
                        legacy_less) ||
        std::adjacent_find(assembly.legacy_surface_samples.begin(),
                           assembly.legacy_surface_samples.end(),
                           [](const auto& lhs, const auto& rhs) {
                               return lhs.world_x == rhs.world_x && lhs.world_z == rhs.world_z;
                           }) != assembly.legacy_surface_samples.end())
        return fail();
    std::map<std::pair<int, int>, World::FarLodWorldLegacySurfaceSample> legacy_metadata;
    for (const auto& sample : assembly.legacy_surface_samples) {
        constexpr u8 kKnownLegacyFlags =
            World::kFarLodSampleFlagWater | World::kFarLodSampleFlagEdited;
        if ((sample.flags & World::kFarLodSampleFlagEdited) == 0u ||
            (sample.flags & static_cast<u8>(~kKnownLegacyFlags)) != 0u ||
            sample.world_x % step_i != 0 || sample.world_z % step_i != 0) {
            return fail();
        }
        legacy_metadata.emplace(std::make_pair(sample.world_z, sample.world_x), sample);
    }
    const std::set<std::pair<i32, i32>> authority_columns(assembly.authority_columns.begin(),
                                                          assembly.authority_columns.end());
    if (authority_columns.empty())
        return fail();
    std::set<std::pair<i32, i32>> expected_owned;
    for (const auto& [chunk_z, chunk_x] : authority_columns) {
        if (chunk_x < Chunk::kPackedMinXz || chunk_x > Chunk::kPackedMaxXz ||
            chunk_z < Chunk::kPackedMinXz || chunk_z > Chunk::kPackedMaxXz ||
            !valid_chunk_coordinate(chunk_x, CHUNK_SIZE_X) ||
            !valid_chunk_coordinate(chunk_z, CHUNK_SIZE_Z))
            return fail();
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const std::int64_t owned_z = static_cast<std::int64_t>(chunk_z) + dz;
                const std::int64_t owned_x = static_cast<std::int64_t>(chunk_x) + dx;
                if (owned_z < std::numeric_limits<int>::min() ||
                    owned_z > std::numeric_limits<int>::max() ||
                    owned_x < std::numeric_limits<int>::min() ||
                    owned_x > std::numeric_limits<int>::max() || owned_z < Chunk::kPackedMinXz ||
                    owned_z > Chunk::kPackedMaxXz || owned_x < Chunk::kPackedMinXz ||
                    owned_x > Chunk::kPackedMaxXz ||
                    !valid_chunk_coordinate(static_cast<int>(owned_z), CHUNK_SIZE_Z) ||
                    !valid_chunk_coordinate(static_cast<int>(owned_x), CHUNK_SIZE_X))
                    return fail();
                expected_owned.emplace(static_cast<int>(owned_z), static_cast<int>(owned_x));
            }
        }
    }
    const std::set<std::pair<i32, i32>> owned(assembly.owned_columns.begin(),
                                              assembly.owned_columns.end());
    if (owned != expected_owned)
        return fail();
    const auto floor_div = [](int value, int divisor) {
        const int quotient = value / divisor;
        const int remainder = value % divisor;
        return remainder < 0 ? quotient - 1 : quotient;
    };
    const auto sample_touches_owned_cell = [&](int world_x, int world_z) {
        const int chunk_x = floor_div(world_x, CHUNK_SIZE_X);
        const int chunk_z = floor_div(world_z, CHUNK_SIZE_Z);
        const int first_x = world_x % CHUNK_SIZE_X == 0 ? chunk_x - 1 : chunk_x;
        const int first_z = world_z % CHUNK_SIZE_Z == 0 ? chunk_z - 1 : chunk_z;
        for (int z = first_z; z <= chunk_z; ++z) {
            for (int x = first_x; x <= chunk_x; ++x) {
                if (owned.count({z, x}) != 0u)
                    return true;
            }
        }
        return false;
    };
    if (std::any_of(assembly.legacy_surface_samples.begin(),
                    assembly.legacy_surface_samples.end(),
                    [&](const auto& sample) {
                        return !sample_touches_owned_cell(sample.world_x, sample.world_z);
                    }))
        return fail();

    struct Sample {
        i16 density;
        u8 material;
    };
    std::map<std::tuple<int, int, int>, Sample> samples;
    const u32 brick_side = World::FarLodSdfBrickSamplesPerSide(tile.tier);
    std::set<std::pair<i32, i32>> seen_authority_columns;
    std::map<std::pair<i32, i32>, std::vector<i32>> stack_levels;
    std::vector<World::FarLodWorldSdfBrickDescriptor> authority_bricks;
    std::set<std::pair<int, int>> seen_legacy_metadata;
    bool have_previous_brick = false;
    std::tuple<i32, i32, i32> previous_brick{};
    for (std::size_t brick_index = 0; brick_index < assembly.bricks.size(); ++brick_index) {
        const auto& brick = assembly.bricks[brick_index];
        if (brick.source_kind != World::FarLodBrickSourceKind::Authoritative &&
            brick.source_kind != World::FarLodBrickSourceKind::RegenerableCache)
            return fail();
        if (brick.chunk_x < Chunk::kPackedMinXz || brick.chunk_x > Chunk::kPackedMaxXz ||
            brick.chunk_y < Chunk::kPackedMinY || brick.chunk_y > Chunk::kPackedMaxY ||
            brick.chunk_z < Chunk::kPackedMinXz || brick.chunk_z > Chunk::kPackedMaxXz ||
            !valid_chunk_coordinate(brick.chunk_x, CHUNK_SIZE_X) ||
            !valid_chunk_coordinate(brick.chunk_y, CHUNK_SIZE_Y) ||
            !valid_chunk_coordinate(brick.chunk_z, CHUNK_SIZE_Z))
            return fail();
        const auto brick_key = std::make_tuple(brick.chunk_z, brick.chunk_x, brick.chunk_y);
        if (have_previous_brick && !(previous_brick < brick_key))
            return fail();
        previous_brick = brick_key;
        have_previous_brick = true;
        stack_levels[{brick.chunk_z, brick.chunk_x}].push_back(brick.chunk_y);
        if (brick.source_kind == World::FarLodBrickSourceKind::Authoritative) {
            seen_authority_columns.emplace(brick.chunk_z, brick.chunk_x);
            authority_bricks.push_back(brick);
        }
        const std::size_t base = brick_index * count;
        Core::Crc32Accumulator crc;
        crc.Update(assembly.density_q.data() + base, count * sizeof(i16));
        crc.Update(assembly.material.data() + base, count);
        if (brick.payload_crc32 != crc.Value())
            return fail();
        for (u32 z = 0; z < brick_side; ++z)
            for (u32 y = 0; y < brick_side; ++y)
                for (u32 x = 0; x < brick_side; ++x) {
                    const std::size_t offset =
                        base + static_cast<std::size_t>(x) +
                        static_cast<std::size_t>(y) * brick_side +
                        static_cast<std::size_t>(z) * brick_side * brick_side;
                    if (assembly.density_q[offset] == World::kFarLodSdfInvalid)
                        return fail();
                    const int world_x = brick.chunk_x * CHUNK_SIZE_X + static_cast<int>(x) * step_i;
                    const int world_y = brick.chunk_y * CHUNK_SIZE_Y + static_cast<int>(y) * step_i;
                    const int world_z = brick.chunk_z * CHUNK_SIZE_Z + static_cast<int>(z) * step_i;
                    const Sample value{assembly.density_q[offset], assembly.material[offset]};
                    const auto legacy = legacy_metadata.find({world_z, world_x});
                    if (legacy != legacy_metadata.end()) {
                        // Legacy columns must already have been promoted into scratch
                        // regenerable streams. Metadata may never overlay or coexist
                        // with a real authoritative footprint in the mesher.
                        if (brick.source_kind != World::FarLodBrickSourceKind::RegenerableCache ||
                            value.density !=
                                World::QuantizeFarLodSdf(
                                    static_cast<float>(world_y) -
                                    World::DequantizeFarLodHeight(legacy->second.height_q)) ||
                            value.material != legacy->second.material) {
                            return fail();
                        }
                        seen_legacy_metadata.emplace(world_z, world_x);
                    }
                    const auto key = std::make_tuple(world_x, world_y, world_z);
                    const auto inserted = samples.emplace(key, value);
                    if (!inserted.second && (inserted.first->second.density != value.density ||
                                             inserted.first->second.material != value.material))
                        return fail();
                }
    }
    if (seen_legacy_metadata.size() != legacy_metadata.size())
        return fail();
    if (seen_authority_columns != authority_columns)
        return fail();
    for (const auto& column : owned) {
        const auto stack = stack_levels.find(column);
        if (stack == stack_levels.end() || stack->second.empty())
            return fail();
        for (std::size_t i = 1; i < stack->second.size(); ++i) {
            if (static_cast<std::int64_t>(stack->second[i - 1]) + 1 != stack->second[i])
                return fail();
        }
    }
    // Every authoritative brick requires one vertical support brick above and
    // below in its complete 3x3 horizontal influence. Without this invariant
    // an assembly can suppress background cells while providing no canonical
    // corners for the replacement SDF cells.
    for (const auto& authority : authority_bricks) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto stack = stack_levels.find(
                    {static_cast<int>(static_cast<std::int64_t>(authority.chunk_z) + dz),
                     static_cast<int>(static_cast<std::int64_t>(authority.chunk_x) + dx)});
                if (stack == stack_levels.end())
                    return fail();
                for (int dy = -1; dy <= 1; ++dy) {
                    const std::int64_t required_y =
                        static_cast<std::int64_t>(authority.chunk_y) + dy;
                    if (required_y < std::numeric_limits<int>::min() ||
                        required_y > std::numeric_limits<int>::max() ||
                        !std::binary_search(stack->second.begin(),
                                            stack->second.end(),
                                            static_cast<int>(required_y)))
                        return fail();
                }
            }
        }
    }
    const int min_x = tile.rx * World::kFarLodRegionSizeMeters;
    const int min_z = tile.rz * World::kFarLodRegionSizeMeters;
    const auto owned_cell = [&](u32 x, u32 z) {
        const int wx = min_x + static_cast<int>(x) * step_i,
                  wz = min_z + static_cast<int>(z) * step_i;
        return owned.count({floor_div(wz, CHUNK_SIZE_Z), floor_div(wx, CHUNK_SIZE_X)}) != 0u;
    };

    std::vector<VoxelVertex>& vertices = out_mesh.vertices;
    std::vector<u32>& indices = out_mesh.indices;
    vertices.reserve(tile.sample_count());
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) {
            const std::size_t index = static_cast<std::size_t>(x) + static_cast<std::size_t>(z) * n;
            vertices.push_back({Vec3(static_cast<float>(x * step_i),
                                     World::DequantizeFarLodHeight(tile.height_q[index]),
                                     static_cast<float>(z * step_i)),
                                Vec3(0.0f),
                                static_cast<u32>(tile.material[index])});
        }
    const auto vertex_index = [n](u32 x, u32 z) {
        return z * n + x;
    };
    for (u32 z = 0; z + 1 < n; ++z)
        for (u32 x = 0; x + 1 < n; ++x) {
            if (owned_cell(x, z))
                continue;
            const u32 a = vertex_index(x, z), b = vertex_index(x + 1, z),
                      c = vertex_index(x, z + 1), d = vertex_index(x + 1, z + 1);
            indices.insert(indices.end(), {a, d, b, a, c, d});
        }

    const IVec3 corners[8] = {
        {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}};
    const int edges[12][2] = {{0, 1},
                              {1, 2},
                              {2, 3},
                              {3, 0},
                              {4, 5},
                              {5, 6},
                              {6, 7},
                              {7, 4},
                              {0, 4},
                              {1, 5},
                              {2, 6},
                              {3, 7}};
    for (const auto& brick : assembly.bricks) {
        if (owned.count({brick.chunk_z, brick.chunk_x}) == 0u)
            continue;
        for (u32 z = 0; z + 1 < brick_side; ++z)
            for (u32 y = 0; y + 1 < brick_side; ++y)
                for (u32 x = 0; x + 1 < brick_side; ++x) {
                    const int wx0 = brick.chunk_x * CHUNK_SIZE_X + static_cast<int>(x) * step_i;
                    const int wz0 = brick.chunk_z * CHUNK_SIZE_Z + static_cast<int>(z) * step_i;
                    if (wx0 < min_x || wx0 >= min_x + World::kFarLodRegionSizeMeters ||
                        wz0 < min_z || wz0 >= min_z + World::kFarLodRegionSizeMeters)
                        continue;
                    GridCell cell;
                    Sample values[8];
                    int cube = 0;
                    for (int c = 0; c < 8; ++c) {
                        const int wx = wx0 + corners[c].x * step_i,
                                  wy = brick.chunk_y * CHUNK_SIZE_Y +
                                       (static_cast<int>(y) + corners[c].y) * step_i,
                                  wz = wz0 + corners[c].z * step_i;
                        const auto found = samples.find(std::make_tuple(wx, wy, wz));
                        if (found == samples.end())
                            return fail();
                        values[c] = found->second;
                        cell.p[c] = Vec3(static_cast<float>(wx - min_x),
                                         static_cast<float>(wy),
                                         static_cast<float>(wz - min_z));
                        cell.val[c] = World::DequantizeFarLodSdf(values[c].density);
                        if (cell.val[c] < 0.0f)
                            cube |= 1 << c;
                    }
                    const unsigned int mask = edgeTable[cube];
                    if (mask == 0u)
                        continue;
                    u32 edge_vertices[12]{};
                    for (int edge = 0; edge < 12; ++edge)
                        if ((mask & (1u << edge)) != 0u) {
                            const int a = edges[edge][0], b = edges[edge][1],
                                      solid = cell.val[a] < 0.0f ? a : b;
                            const int local_x = static_cast<int>(cell.p[solid].x) / step_i,
                                      local_z = static_cast<int>(cell.p[solid].z) / step_i;
                            if (local_x < 0 || local_z < 0 || local_x >= static_cast<int>(n) ||
                                local_z >= static_cast<int>(n))
                                return fail();
                            u8 fallback_material =
                                tile.material[static_cast<std::size_t>(local_x) +
                                              static_cast<std::size_t>(local_z) * n];
                            const u8 material = values[solid].material == 0xffu
                                                    ? fallback_material
                                                    : values[solid].material;
                            edge_vertices[edge] = static_cast<u32>(vertices.size());
                            vertices.push_back(
                                {VertexInterp(0.0f, cell.p[a], cell.p[b], cell.val[a], cell.val[b]),
                                 Vec3(0.0f),
                                 static_cast<u32>(material)});
                        }
                    const Vec3 gradient = EstimateDensityGradient(cell);
                    for (int t = 0; triTable[cube][t] != -1; t += 3) {
                        u32 a = edge_vertices[triTable[cube][t]],
                            b = edge_vertices[triTable[cube][t + 1]],
                            c = edge_vertices[triTable[cube][t + 2]];
                        const Vec3 normal = glm::cross(vertices[b].position - vertices[a].position,
                                                       vertices[c].position - vertices[a].position);
                        if (glm::dot(normal, normal) <= 1.0e-10f)
                            continue;
                        if (glm::dot(normal, gradient) < 0.0f)
                            std::swap(b, c);
                        indices.insert(indices.end(), {a, b, c});
                    }
                }
    }

    const float drop = static_cast<float>(step_i);
    const auto skirt = [&](u32 a_index, u32 b_index, const Vec3& out) {
        const VoxelVertex a = vertices[a_index], b = vertices[b_index];
        const u32 base = static_cast<u32>(vertices.size());
        vertices.push_back({a.position, out, a.material_id});
        vertices.push_back({b.position, out, b.material_id});
        vertices.push_back({b.position - Vec3(0.0f, drop, 0.0f), out, b.material_id});
        vertices.push_back({a.position - Vec3(0.0f, drop, 0.0f), out, a.material_id});
        AppendOrientedTriangle(indices, vertices, base, base + 1u, base + 2u, out);
        AppendOrientedTriangle(indices, vertices, base, base + 2u, base + 3u, out);
        ++stats.skirt_quads;
    };
    for (u32 x = 0; x + 1 < n; ++x) {
        if (!owned_cell(x, 0))
            skirt(vertex_index(x, 0), vertex_index(x + 1, 0), Vec3(0, 0, -1));
        if (!owned_cell(x, n - 2))
            skirt(vertex_index(x, n - 1), vertex_index(x + 1, n - 1), Vec3(0, 0, 1));
    }
    for (u32 z = 0; z + 1 < n; ++z) {
        if (!owned_cell(0, z))
            skirt(vertex_index(0, z), vertex_index(0, z + 1), Vec3(-1, 0, 0));
        if (!owned_cell(n - 2, z))
            skirt(vertex_index(n - 1, z), vertex_index(n - 1, z + 1), Vec3(1, 0, 0));
    }
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        VoxelVertex &a = vertices[indices[i]], &b = vertices[indices[i + 1]],
                    &c = vertices[indices[i + 2]];
        const Vec3 normal = glm::cross(b.position - a.position, c.position - a.position);
        a.normal += normal;
        b.normal += normal;
        c.normal += normal;
    }
    for (VoxelVertex& vertex : vertices)
        vertex.normal = glm::dot(vertex.normal, vertex.normal) > 0.0f
                            ? glm::normalize(vertex.normal)
                            : Vec3(0, 1, 0);
    stats.vertices = vertices.size();
    stats.indices = indices.size();
    stats.triangles = indices.size() / 3u;
    return stats;
}

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

void PolygoniseTerrain(const Systems::SHIELD_WorldSystem& world_system,
                       Chunk& chunk,
                       float isolevel,
                       int step) {
    const auto build_start = std::chrono::steady_clock::now();
    // reset the per-worker meshing arena at job entry. Covers BOTH the
    // coarse heightfield path (remap) and the unit-step path below (edge cache,
    // world positions, materials, remap). Reset is an offset rewind - negligible
    // vs. the elapsed_us this build is timed against.
    MeshArenaScope mesh_arena_scope;
    const int sample_step = std::max(1, step);

    constexpr std::size_t kFullLatticeCount =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    const bool has_full_sdf = chunk.sdf_data.size() == kFullLatticeCount;
    if (!chunk.sdf_data.empty() && !has_full_sdf) {
        // A non-empty partial/oversized field must never silently degrade to
        // the coarse heightfield. The world-system routes this state through
        // full-lattice regeneration; this last-line guard keeps direct callers
        // safe until that happens.
        LUMINUMBRA_CORE_WARN(
            "PolygoniseTerrain: chunk ({},{},{}) sdf_data size {} != full lattice {} — "
            "rejecting (no mesh) instead of falling back to heightfield data",
            chunk.get_coords().x,
            chunk.get_coords().y,
            chunk.get_coords().z,
            chunk.sdf_data.size(),
            kFullLatticeCount);
        chunk.mesh_vertices.clear();
        chunk.mesh_indices.clear();
        return;
    }

    // Coarse chunks with no resident lattice preserve the established analytic
    // heightfield path. Every exact full lattice, including coarse LODs, is
    // polygonised from its authoritative 3D densities below.
    if (!has_full_sdf && sample_step > 1) {
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
        if (has_negative && has_positive) {
            break;
        }
    }

    if (!has_negative || !has_positive) {
        // Chunk is entirely above or below the surface
        chunk.mesh_vertices.clear();
        chunk.mesh_indices.clear();
        const auto elapsed_us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - build_start)
                                           .count());
        RecordTerrainMeshBuildStats(sample_step, 0, 0, 0, 0, elapsed_us);
        return;
    }

    // Exact full lattices share this one Marching Cubes path at every supported
    // stride. Coarse cells start at 0,step,...,16-step and interpolate from
    // the original float densities, retaining caves and edits representable at
    // that stride.
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;
    vertices.reserve(CHUNK_VOLUME / 4);
    indices.reserve(CHUNK_VOLUME);

    const IVec3 chunk_base_pos =
        chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    constexpr u32 kYStride = CHUNK_SIZE_X + 1;
    constexpr u32 kZStride = (CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1);
    constexpr u32 kLatticeCount = (CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);

    const IVec3 corner_offsets[8] = {
        {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}};

    const int edge_connections[12][2] = {{0, 1},
                                         {1, 2},
                                         {2, 3},
                                         {3, 0},
                                         {4, 5},
                                         {5, 6},
                                         {6, 7},
                                         {7, 4},
                                         {0, 4},
                                         {1, 5},
                                         {2, 6},
                                         {3, 7}};

    // Flat per-edge vertex cache replacing the previous unordered_map keyed
    // by corner-index pairs. Every unique cell edge is one of three canonical
    // axis edges (+X, +Y, +Z) anchored at an SDF lattice point, so the cache
    // is lattice_point * 3 + axis: O(1) lookups, no hashing, no node
    // allocations. First-writer-wins semantics are identical to the map
    // because the cell scan order and the per-cell edge order are unchanged.
    constexpr u32 kNoCachedVertex = 0xFFFFFFFFu;
    constexpr u8 kEdgeAnchorCorner[12] = {0u, 1u, 3u, 0u, 4u, 5u, 7u, 4u, 0u, 1u, 2u, 3u};
    constexpr u8 kEdgeAxis[12] = {0u, 2u, 0u, 2u, 0u, 2u, 0u, 2u, 1u, 1u, 1u, 1u};
    // the per-edge vertex cache is the dominant per-job scratch alloc
    // (kLatticeCount*3 u32 ~= 59 KB at 17^3). It is pure scratch (index-addressed,
    // never escapes), so it comes from the reset-per-job arena, value-set to
    // kNoCachedVertex exactly as the prior std::vector(count, kNoCachedVertex) did.
    // The cache stores vertex INDICES into `vertices` (a real std::vector), never
    // pointers, so the arena gives it stable storage with no aliasing hazard.
    ArenaSpan<u32> edge_vertex_cache =
        ArenaAlloc<u32>(static_cast<std::size_t>(kLatticeCount) * 3u, kNoCachedVertex);

    const float* const sdf = chunk.sdf_data.data();

    // per-voxel structure material channel (parallel to sdf, same index
    // formula). Empty for chunks with no authored structure voxels => the stored
    // material is never consulted and PASS 1b stays byte-identical to today.
    const bool has_material = chunk.material_data.size() == chunk.sdf_data.size();
    const u8* const material = has_material ? chunk.material_data.data() : nullptr;
    // Stored override material captured per emitted vertex during PASS 1 (0 = no
    // override; classify analytically). Re-applied after PASS 1b only where != 0.
    std::vector<u8> vertex_material_override;
    if (has_material) {
        vertex_material_override.reserve(CHUNK_VOLUME / 4);
    }

    // --- PASS 1: Generate unique vertices and triangle indices ---
    std::size_t cells_visited = 0;
    std::size_t active_cells = 0;
    for (int z = 0; z < CHUNK_SIZE_Z; z += sample_step) {
        for (int y = 0; y < CHUNK_SIZE_Y; y += sample_step) {
            for (int x = 0; x < CHUNK_SIZE_X; x += sample_step) {
                GridCell gridcell;
                int cube_index = 0;
                u32 corner_lattice_indices[8];
                ++cells_visited;

                for (int i = 0; i < 8; ++i) {
                    const IVec3 lattice_corner = IVec3(x, y, z) + corner_offsets[i] * sample_step;
                    const u32 lattice_index = static_cast<u32>(lattice_corner.x) +
                                              static_cast<u32>(lattice_corner.y) * kYStride +
                                              static_cast<u32>(lattice_corner.z) * kZStride;
                    corner_lattice_indices[i] = lattice_index;
                    gridcell.val[i] = sdf[lattice_index];
                    if (gridcell.val[i] < isolevel) {
                        cube_index |= (1 << i);
                    }
                }

                const unsigned int edge_mask = edgeTable[cube_index];
                if (edge_mask == 0)
                    continue;
                ++active_cells;

                for (int i = 0; i < 8; ++i) {
                    gridcell.p[i] = Vec3(IVec3(x, y, z) + corner_offsets[i] * sample_step);
                }

                u32 vert_indices[12];
                for (int i = 0; i < 12; ++i) {
                    if (edge_mask & (1u << i)) {
                        const u32 cache_slot =
                            corner_lattice_indices[kEdgeAnchorCorner[i]] * 3u + kEdgeAxis[i];
                        u32& cached_index = edge_vertex_cache[cache_slot];
                        if (cached_index != kNoCachedVertex) {
                            vert_indices[i] = cached_index;
                        } else {
                            const int corner_a = edge_connections[i][0];
                            const int corner_b = edge_connections[i][1];
                            Vec3 p1 = gridcell.p[corner_a];
                            Vec3 p2 = gridcell.p[corner_b];
                            f32 v1 = gridcell.val[corner_a];
                            f32 v2 = gridcell.val[corner_b];
                            Vec3 new_pos = VertexInterp(isolevel, p1, p2, v1, v2);

                            // material is classified in a
                            // single SIMD-batched pass after all vertices are
                            // emitted (ClassifyVertexMaterials below) instead of
                            // a per-vertex GetTerrainMaterialAt - byte-identical
                            // result, but the shaped-height + climate noise reads
                            // ride FastNoise's GenPositionArray2D batch path. The
                            // Sentinel material_id is overwritten by that pass.
                            vertices.push_back({new_pos, Vec3(0.0f), 0u});
                            u32 new_idx = static_cast<u32>(vertices.size() - 1);
                            vert_indices[i] = new_idx;
                            cached_index = new_idx;

                            // capture the authored material of this edge's
                            // SOLID corner (val < isolevel). First-writer-wins on
                            // the shared edge cache => stamped per edge exactly
                            // once, fixed scan order => deterministic. 0 = no
                            // authored material (classify analytically in 1b).
                            if (has_material) {
                                const int solid_corner = (v1 < isolevel) ? corner_a : corner_b;
                                vertex_material_override.push_back(
                                    material[corner_lattice_indices[solid_corner]]);
                            }
                        }
                    }
                }

                const Vec3 density_gradient = EstimateDensityGradient(gridcell);
                // #7 SMOOTHER MC NORMALS (rendering contract): the analytic SDF-density
                // gradient is the true surface normal direction (points toward AIR /
                // +density = outward). It was computed here only to orient triangle
                // winding; seed it onto each emitted vertex too, BLENDED with the
                // area-weighted face-normal smoothing in PASS 2 below. On thin features
                // / cave walls the pure face-average produced washboard ripple +
                // faceting; the smooth analytic gradient settles them. Render-only:
                // mesh NORMALS are excluded from world_hash.
                const Vec3 grad_n = (glm::dot(density_gradient, density_gradient) > 1.0e-12f)
                                        ? glm::normalize(density_gradient)
                                        : Vec3(0.0f);
                const auto& tri_row = triTable[cube_index];
                for (int i = 0; tri_row[i] != -1; i += 3) {
                    u32 i0 = vert_indices[tri_row[i]];
                    u32 i1 = vert_indices[tri_row[i + 1]];
                    u32 i2 = vert_indices[tri_row[i + 2]];

                    if (i0 == i1 || i1 == i2 || i2 == i0) {
                        continue;
                    }

                    const Vec3 face_normal =
                        glm::cross(vertices[i1].position - vertices[i0].position,
                                   vertices[i2].position - vertices[i0].position);

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
                // Seed the analytic gradient onto every vertex this cell emitted (valid
                // edges only; vert_indices is set under the same edge_mask gate above).
                // Shared edge-cache vertices accumulate each adjacent cell's gradient ->
                // smooth cross-cell averaging before PASS 2 adds the face normals.
                for (int e = 0; e < 12; ++e) {
                    if (edge_mask & (1u << e)) {
                        vertices[vert_indices[e]].normal += grad_n;
                    }
                }
            }
        }
    }

    // --- PASS 1b: SIMD-batched material classification.
    // One pass over every emitted vertex world position; ClassifyVertexMaterials
    // returns the exact GetTerrainMaterialAt material per vertex but evaluates
    // the shaped-height/climate noise through FastNoise's batch entry points.
    if (!vertices.empty()) {
        // both scratch arrays are index-filled then consumed in-place
        // (ClassifyVertexMaterials reads positions, writes materials) and never
        // escape, so they are arena-backed. world_positions is overwritten for
        // every element below before use; materials is fully written by the batch
        // classifier. Init values match the prior std::vector default-init.
        ArenaSpan<Vec3> world_positions = ArenaAlloc<Vec3>(vertices.size(), Vec3(0.0f));
        ArenaSpan<u32> materials = ArenaAlloc<u32>(vertices.size(), 0u);
        const Vec3 base = Vec3(chunk_base_pos);
        for (size_t i = 0; i < vertices.size(); ++i) {
            world_positions[i] = base + vertices[i].position;
        }
        world_system.ClassifyVertexMaterials(
            world_positions.data(), world_positions.size(), materials.data());
        for (size_t i = 0; i < vertices.size(); ++i) {
            vertices[i].material_id = materials[i];
        }
        // override the analytic classification with the authored structure
        // material on vertices created on a structure edge-corner (override != 0).
        // Empty material_data => vertex_material_override is empty => no-op =>
        // PASS 1b byte-identical to today.
        if (has_material && vertex_material_override.size() == vertices.size()) {
            for (size_t i = 0; i < vertices.size(); ++i) {
                if (vertex_material_override[i] != 0) {
                    vertices[i].material_id = static_cast<u32>(vertex_material_override[i]);
                }
            }
        }
    }

    // --- PASS 2: Calculate smoothed normals ---
    for (size_t i = 0; i < indices.size(); i += 3) {
        VoxelVertex& v1 = vertices[indices[i]];
        VoxelVertex& v2 = vertices[indices[i + 1]];
        VoxelVertex& v3 = vertices[indices[i + 2]];

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

    // remap is pure index-addressed scratch (never escapes) -> arena.
    // Value-set to 0xFFFFFFFF, identical to the prior std::vector(count, -1).
    ArenaSpan<u32> remap = ArenaAlloc<u32>(vertices.size(), static_cast<u32>(-1));
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
    const auto elapsed_us =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - build_start)
                                       .count());
    RecordTerrainMeshBuildStats(sample_step,
                                cells_visited,
                                active_cells,
                                chunk.mesh_vertices.size(),
                                chunk.mesh_indices.size(),
                                elapsed_us);

    // Mesh generation complete
}

TerrainTransitionSkirtStats
AddBoundaryTransitionSkirts(Chunk& chunk, int step, TerrainTransitionFaceMask faces) {
    TerrainTransitionSkirtStats stats;
    if (step <= 1 || faces == kNoTransitionFaces || chunk.mesh_vertices.empty() ||
        chunk.mesh_indices.empty()) {
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
        // copy the two top vertices BY VALUE before
        // any push_back below. They previously aliased chunk.mesh_vertices via
        // reference; the first push_back can reallocate the vector, leaving
        // later reads dereferencing dangling references (UB: garbage skirt
        // vertices, or a crash when the stale storage is reclaimed - observed
        // as an AV in offline skirt scans). Holding values makes the reads
        // reallocation-proof.
        const VoxelVertex top_a = chunk.mesh_vertices[a];
        const VoxelVertex top_b = chunk.mesh_vertices[b];
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
        const bool first = AppendOrientedTriangle(
            chunk.mesh_indices, chunk.mesh_vertices, a, b, down_b, face_normal);
        const bool second = AppendOrientedTriangle(
            chunk.mesh_indices, chunk.mesh_vertices, a, down_b, down_a, face_normal);
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

void GenerateWaterMesh(const Systems::WaterSystem& water_system,
                       const Systems::SHIELD_WorldSystem& world_system,
                       Chunk& chunk) {
    (void)water_system;
    const int resolution = chunk.current_water_resolution.load(std::memory_order_acquire);
    std::vector<VoxelVertex> water_vertices;
    std::vector<u32> water_indices;
    water_vertices.reserve(static_cast<std::size_t>(resolution) *
                           static_cast<std::size_t>(resolution) * 4u);
    water_indices.reserve(static_cast<std::size_t>(resolution) *
                          static_cast<std::size_t>(resolution) * 6u);

    if (!HasCompleteWaterGrid(chunk, resolution)) {
        chunk.water_mesh_vertices.clear();
        chunk.water_mesh_indices.clear();
        chunk.water_mesh_generated.store(false, std::memory_order_release);
        return;
    }

    const IVec3 chunk_base_pos =
        chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    const Vec3 normal = {0.0f, 1.0f, 0.0f}; // Water surface normal is always up
    const u32 water_mat_id = static_cast<u32>(MaterialType::Water);
    constexpr float kMinRenderableWaterDepth = 0.05f;
    //  fix: cull on the ACTUAL standing depth (water_depth_mm, which terraform edits update), NOT
    // on (water_level - GetTerrainHeightAt). GetTerrainHeightAt is pure worldgen and is blind to
    // runtime digs, so water filling a CARVED basin (its surface below the original worldgen
    // height) was wrongly judged "underground" and never rendered. The per-cell depth is
    // edit-aware, so dug/rain ponds show.
    constexpr std::int32_t kMinRenderableWaterDepthMm = 50; // 5 cm
    const bool have_depth =
        (chunk.water_depth_mm.size() ==
         static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution));

    const float cell_width_x = static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(resolution);
    const float cell_width_z = static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(resolution);

    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            // Get the water surface heights at the four corners of this water grid cell
            float world_x0 = chunk_base_pos.x + x * cell_width_x;
            float world_z0 = chunk_base_pos.z + z * cell_width_z;
            float world_x1 = world_x0 + cell_width_x;
            float world_z1 = world_z0 + cell_width_z;

            float water_h00 = SampleChunkWaterLevel(chunk, world_x0, world_z0, resolution);
            float water_h10 = SampleChunkWaterLevel(chunk, world_x1, world_z0, resolution);
            float water_h01 = SampleChunkWaterLevel(chunk, world_x0, world_z1, resolution);
            float water_h11 = SampleChunkWaterLevel(chunk, world_x1, world_z1, resolution);

            // Render a quad where this cell holds standing water (edit-aware depth). Fall back to
            // the legacy worldgen-height test only if the depth array is unavailable.
            bool renderable;
            if (have_depth) {
                renderable = chunk.water_depth_mm[static_cast<std::size_t>(z) * resolution + x] >
                             kMinRenderableWaterDepthMm;
            } else {
                renderable = (water_h00 - world_system.GetTerrainHeightAt(world_x0, world_z0)) >
                             kMinRenderableWaterDepth;
            }
            if (renderable) {
                u32 base_idx = static_cast<u32>(water_vertices.size());

                // Define vertices relative to chunk origin
                Vec3 p00 = {x * cell_width_x, water_h00 - chunk_base_pos.y, z * cell_width_z};
                Vec3 p10 = {(x + 1) * cell_width_x, water_h10 - chunk_base_pos.y, z * cell_width_z};
                Vec3 p01 = {x * cell_width_x, water_h01 - chunk_base_pos.y, (z + 1) * cell_width_z};
                Vec3 p11 = {
                    (x + 1) * cell_width_x, water_h11 - chunk_base_pos.y, (z + 1) * cell_width_z};

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
