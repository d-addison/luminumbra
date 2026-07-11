// T-I2-10a: Marching cubes meshing determinism gate.
//
// Locks the bit-exact bytes of PolygoniseTerrain output (mesh_vertices +
// mesh_indices) for a set of fixture chunks at LOD steps 1, 2 and 4 via
// FNV-1a-64 hashes captured from the reference implementation. Any hot-path
// optimization of the mesher must keep these hashes UNCHANGED.
//
// Also contains a small steady_clock benchmark of the step-1 hot path so
// before/after optimization timings can be read from test output.

#include "gtest/gtest.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

#include "world/Chunk.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/MarchingCubes.h"

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

// VoxelVertex must stay tightly packed (Vec3 + Vec3 + u32 = 28 bytes) for raw
// byte hashing to be meaningful. If padding ever appears, this gate must be
// rewritten to hash fields explicitly.
static_assert(sizeof(VoxelVertex) == 28, "VoxelVertex layout changed; meshing determinism hashes are stale");

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t Fnv1a64(const void* data, std::size_t size) {
    std::uint64_t hash = kFnvOffsetBasis;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

struct MeshHashes {
    std::uint64_t vertex_hash = 0;
    std::uint64_t index_hash = 0;
    std::size_t vertex_count = 0;
    std::size_t index_count = 0;
};

MeshHashes HashChunkMesh(const TerrainGenParams& params, int seed, const IVec3& coords, int step) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params, seed);
    Chunk chunk(coords);
    world_system.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, step);

    MeshHashes hashes;
    hashes.vertex_count = chunk.mesh_vertices.size();
    hashes.index_count = chunk.mesh_indices.size();
    hashes.vertex_hash = Fnv1a64(chunk.mesh_vertices.data(), chunk.mesh_vertices.size() * sizeof(VoxelVertex));
    hashes.index_hash = Fnv1a64(chunk.mesh_indices.data(), chunk.mesh_indices.size() * sizeof(u32));
    return hashes;
}

void PrintHashes(const char* combo, int step, const MeshHashes& hashes) {
    std::cout << "[ MESHHASH ] " << combo << " step " << step
              << " vertices=" << hashes.vertex_count
              << " indices=" << hashes.index_count
              << " vertex_hash=0x" << std::hex << std::setfill('0') << std::setw(16) << hashes.vertex_hash
              << " index_hash=0x" << std::setw(16) << hashes.index_hash
              << std::dec << std::setfill(' ') << std::endl;
}

struct ExpectedMeshHashes {
    int step;
    std::uint64_t vertex_hash;
    std::uint64_t index_hash;
};

void VerifyCombo(const char* combo, const TerrainGenParams& params, int seed, const IVec3& coords,
                 const ExpectedMeshHashes (&expected)[3]) {
    for (const ExpectedMeshHashes& exp : expected) {
        const MeshHashes hashes = HashChunkMesh(params, seed, coords, exp.step);
        PrintHashes(combo, exp.step, hashes);
        EXPECT_EQ(hashes.vertex_hash, exp.vertex_hash)
            << combo << " step " << exp.step << " mesh_vertices bytes changed";
        EXPECT_EQ(hashes.index_hash, exp.index_hash)
            << combo << " step " << exp.step << " mesh_indices bytes changed";
    }
}

TerrainGenParams MakeArchipelagoParams() {
    TerrainGenParams params;
    params.base_frequency = 0.004f;
    params.base_amplitude = 8.0f;
    params.octaves = 6;
    params.persistence = 0.5f;
    params.lacunarity = 2.2f;
    params.height_offset = 8.0f;
    params.island_mask_enabled = false;
    params.caves_enabled = false;
    return params;
}

TerrainGenParams MakeCaveParams() {
    TerrainGenParams params;
    params.base_amplitude = 0.0f;
    params.height_offset = 40.0f;
    params.caves_enabled = true;
    params.cave_threshold = 0.55f;
    params.cave_frequency = 0.15f;
    params.cave_carve_value = 4.0f;
    return params;
}

TerrainGenParams MakeFlatSurfaceParams() {
    TerrainGenParams params;
    params.base_amplitude = 0.0f;
    params.height_offset = 8.0f;
    params.caves_enabled = false;
    return params;
}

} // namespace

// =====================================================================================
// DETERMINISM HASH GATES
// =====================================================================================

// SHIELD-17 re-pin (spec 021, 2026-07-03): the step-1 VERTEX hashes below were
// re-pinned for commit d53c99c5 (2026-06-26, "analytic MC normals") — a
// deliberate render-only change to unit-step vertex NORMALS. Evidence chain:
// index hashes + vertex counts UNCHANGED (same topology), coarse steps 2/4
// UNCHANGED (heightfield path untouched), all pins pass byte-exact at
// d53c99c5~1, and mesh bytes are world_hash-EXCLUDED so --smoke was unaffected
// throughout (which is exactly why this drift sat invisible until the full
// ctest lane ran — the OPS-09 lesson).
TEST(MeshingDeterminism, ArchipelagoChunkHashesAreStable) {
    const ExpectedMeshHashes expected[3] = {
        {1, 0xc9f19ad96ac304caull, 0x3810ee7a8afe33d3ull},
        {2, 0xdc1a13f81cb0558full, 0x394253726e701f4dull},
        {4, 0x7e98cc1435877992ull, 0x8a8b92607bf5a5e1ull},
    };
    VerifyCombo("archipelago seed=42 chunk=(0,0,0)", MakeArchipelagoParams(), 42, IVec3(0, 0, 0), expected);
}

TEST(MeshingDeterminism, CaveChunkHashesAreStable) {
    // Steps 2 and 4 use the coarse heightfield path; the 40m-high terrain
    // surface is above this chunk, so the coarse mesh is legitimately empty
    // (FNV-1a-64 offset basis == hash of zero bytes).
    // Step-1 vertex hash re-pinned for d53c99c5 analytic normals (see above).
    const ExpectedMeshHashes expected[3] = {
        {1, 0x3c13cffb2df9002bull, 0x5c5461c111230d31ull},
        {2, 0xcbf29ce484222325ull, 0xcbf29ce484222325ull},
        {4, 0xcbf29ce484222325ull, 0xcbf29ce484222325ull},
    };
    VerifyCombo("caves seed=12345 chunk=(0,0,0)", MakeCaveParams(), 12345, IVec3(0, 0, 0), expected);
}

TEST(MeshingDeterminism, FlatSurfaceChunkHashesAreStable) {
    const ExpectedMeshHashes expected[3] = {
        {1, 0x8108751b0ed03207ull, 0xcbacacb06ef692e3ull},
        {2, 0xba1b3e29a667172full, 0x394253726e701f4dull},
        {4, 0xfa87d667e9478c47ull, 0x8a8b92607bf5a5e1ull},
    };
    VerifyCombo("flat seed=1337 chunk=(0,0,0)", MakeFlatSurfaceParams(), 1337, IVec3(0, 0, 0), expected);
}

TEST(MeshingDeterminism, CoarseStepUsesAuthoritativeSdfLattice) {
    TerrainGenParams params = MakeFlatSurfaceParams();
    params.height_offset = 0.0f;

    SHIELD_WorldSystem world_system(nullptr, nullptr, params, 9001);
    Chunk chunk(IVec3(0, 0, 0));

    constexpr std::size_t lattice_width = static_cast<std::size_t>(CHUNK_SIZE_X + 1);
    constexpr std::size_t lattice_height = static_cast<std::size_t>(CHUNK_SIZE_Y + 1);
    chunk.sdf_data.resize(lattice_width * lattice_height * static_cast<std::size_t>(CHUNK_SIZE_Z + 1));

    // This authoritative lattice describes a horizontal surface at y=12. It
    // deliberately disagrees with the flat analytic terrain (y=0), so using
    // GetTerrainHeightAtCoarse instead of the resident SDF cannot pass.
    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
            for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
                const std::size_t index = static_cast<std::size_t>(x)
                    + static_cast<std::size_t>(y) * lattice_width
                    + static_cast<std::size_t>(z) * lattice_width * lattice_height;
                chunk.sdf_data[index] = static_cast<float>(y) - 12.0f;
            }
        }
    }

    World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, 4);

    ASSERT_FALSE(chunk.mesh_vertices.empty());
    ASSERT_FALSE(chunk.mesh_indices.empty());
    for (const VoxelVertex& vertex : chunk.mesh_vertices) {
        EXPECT_FLOAT_EQ(vertex.position.y, 12.0f);
    }
}

// =====================================================================================
// HOT-PATH BENCHMARK (informational; prints before/after optimization timings)
// =====================================================================================

TEST(MeshingDeterminism, Benchmark_PolygoniseTerrainStep1) {
    struct Fixture {
        const char* name;
        TerrainGenParams params;
        int seed;
        IVec3 coords;
    };
    const Fixture fixtures[] = {
        {"archipelago", MakeArchipelagoParams(), 42, IVec3(0, 0, 0)},
        {"caves", MakeCaveParams(), 12345, IVec3(0, 0, 0)},
        {"flat", MakeFlatSurfaceParams(), 1337, IVec3(0, 0, 0)},
    };

    constexpr int kIterations = 200;
    for (const Fixture& fixture : fixtures) {
        SHIELD_WorldSystem world_system(nullptr, nullptr, fixture.params, fixture.seed);
        Chunk chunk(fixture.coords);
        world_system.GenerateChunkData(chunk);

        // Warm-up run (also validates the fixture produces a mesh).
        World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, 1);
        ASSERT_FALSE(chunk.mesh_vertices.empty()) << fixture.name << " fixture produced an empty mesh";

        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kIterations; ++i) {
            World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, 1);
        }
        const auto end = std::chrono::steady_clock::now();
        const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "[ MESHPERF ] " << fixture.name << " PolygoniseTerrain step=1 x" << kIterations
                  << ": total " << total_us << " us, avg "
                  << (static_cast<double>(total_us) / kIterations) << " us/chunk" << std::endl;
    }
}
