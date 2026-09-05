// =====================================================================================
// ADVERSARIAL meshing + worldgen determinism hardening gate.
//
// Hunts the bugs the happy-path meshing tests (test_meshing_determinism.cpp,
// test_world_generation.cpp) MISS: re-mesh byte drift, per-vertex normal ULP
// drift from non-canonical accumulation order, chunk-seam field/mesh
// discontinuity, LOD "re-roll" instead of deterministic reduction, generation
// order leakage (hidden global/static state), and degenerate-input crashes /
// stray triangles.
//
// Construction mirrors the compiling siblings EXACTLY:
//   SHIELD_WorldSystem world(nullptr, nullptr, params, seed);
//   Chunk chunk(coords); world.GenerateChunkData(chunk);
//   World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, step);
//
// All assertions are deterministic (no wall-clock, no std::random). run==replay
// is asserted BYTE/FLOAT exact: EXPECT_EQ on flattened field/vertex byte spans,
// EXPECT_FLOAT_EQ on individual floats. Tests that assert CORRECT behaviour which
// the current implementation may violate are flagged in-line and in the
// structured report; they are never written to assert the bug is correct.
// =====================================================================================

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

// VoxelVertex must stay tightly packed for raw byte comparison to be meaningful.
// If padding ever appears, the byte-equality helpers below must hash fields.
static_assert(sizeof(VoxelVertex) == 28,
              "VoxelVertex layout changed; meshing hardening byte compares are stale");

// ----------------------------------------------------------------------------
// FNV-1a-64 over a raw byte span -- used to compare flattened vertex/index/SDF
// buffers for byte-exact equality across re-generation / re-mesh.
// ----------------------------------------------------------------------------
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

template<typename T>
std::uint64_t HashVec(const std::vector<T>& v) {
    return Fnv1a64(v.data(), v.size() * sizeof(T));
}

// ----------------------------------------------------------------------------
// Param presets (copied from the siblings so we share their byte behaviour).
// ----------------------------------------------------------------------------
TerrainGenParams MakeFlatSurfaceParams() {
    TerrainGenParams params;
    params.base_amplitude = 0.0f;
    params.height_offset = 8.0f;

    return params;
}

TerrainGenParams MakeArchipelagoParams() {
    TerrainGenParams params;
    params.base_frequency = 0.004f;
    params.base_amplitude = 8.0f;
    params.octaves = 6;
    params.persistence = 0.5f;
    params.lacunarity = 2.2f;
    params.height_offset = 8.0f;

    return params;
}

TerrainGenParams MakeCaveParams() {
    TerrainGenParams params;
    params.base_amplitude = 0.0f;
    params.height_offset = 40.0f;

    params.cave_threshold = 0.55f;
    params.cave_frequency = 0.15f;
    params.cave_carve_value = 4.0f;
    return params;
}

// A preset whose surface sits far below chunk (0,0,0): that chunk is all-air.
TerrainGenParams MakeDeepOceanParams() {
    TerrainGenParams params = MakeArchipelagoParams();
    params.height_offset = -200.0f;
    return params;
}

// A preset whose surface sits far ABOVE chunk (0,0,0): that chunk is all-solid.

// ----------------------------------------------------------------------------
// Mesh-of-chunk helper. Builds a fresh world + chunk, generates voxel data, and
// meshes it. Returned by value so each call is fully independent.
// ----------------------------------------------------------------------------
struct MeshResult {
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;
    std::vector<f32> sdf;
    std::vector<f32> heightmap;
};

MeshResult GenAndMesh(const TerrainGenParams& params, int seed, const IVec3& coords, int step) {
    SHIELD_WorldSystem world(nullptr, nullptr, params, seed);
    Chunk chunk(coords);
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, step);
    MeshResult out;
    out.vertices = chunk.mesh_vertices;
    out.indices = chunk.mesh_indices;
    out.sdf = chunk.sdf_data;
    out.heightmap = chunk.heightmap_data;
    return out;
}

// SDF lattice geometry: (CHUNK_SIZE+1)^3 row-major, x fastest then y then z.
constexpr int kLat = CHUNK_SIZE_X + 1; // 17
std::size_t SdfIndex(int x, int y, int z) {
    return static_cast<std::size_t>(x) +
           static_cast<std::size_t>(y) * static_cast<std::size_t>(kLat) +
           static_cast<std::size_t>(z) * static_cast<std::size_t>(kLat) *
               static_cast<std::size_t>(kLat);
}

[[maybe_unused]] bool VertexBytesEqual(const VoxelVertex& a, const VoxelVertex& b) {
    return std::memcmp(&a, &b, sizeof(VoxelVertex)) == 0;
}

} // namespace

// =====================================================================================
// 1. MESH PURITY -- re-meshing the SAME field must be BYTE-EXACT
// =====================================================================================

// Mesh the same generated chunk twice (same world, fresh re-poly) -- the vertex,
// normal and index buffers must be byte-for-byte identical. Catches any hidden
// per-call state (arena leak, accumulation drift) in the unit-step path.
TEST(MeshingHardening, RemeshSameChunkIsByteExact_Archipelago) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeArchipelagoParams(), 42);
    Chunk chunk(IVec3(0, 0, 0));
    world.GenerateChunkData(chunk);

    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    const std::vector<VoxelVertex> verts_a = chunk.mesh_vertices;
    const std::vector<u32> idx_a = chunk.mesh_indices;

    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    const std::vector<VoxelVertex> verts_b = chunk.mesh_vertices;
    const std::vector<u32> idx_b = chunk.mesh_indices;

    ASSERT_FALSE(verts_a.empty());
    ASSERT_EQ(verts_a.size(), verts_b.size());
    ASSERT_EQ(idx_a.size(), idx_b.size());
    EXPECT_EQ(HashVec(verts_a), HashVec(verts_b)) << "vertex+normal bytes drifted on re-mesh";
    EXPECT_EQ(HashVec(idx_a), HashVec(idx_b)) << "index order drifted on re-mesh";
}

// Two independent world systems, same seed/params, same chunk -> byte-identical
// mesh. Catches construction-order or first-call-warmup state leaking into output.
TEST(MeshingHardening, TwoWorldsSameSeedMeshByteExact_Archipelago) {
    const MeshResult a = GenAndMesh(MakeArchipelagoParams(), 42, IVec3(0, 0, 0), 1);
    const MeshResult b = GenAndMesh(MakeArchipelagoParams(), 42, IVec3(0, 0, 0), 1);
    ASSERT_FALSE(a.vertices.empty());
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    EXPECT_EQ(HashVec(a.vertices), HashVec(b.vertices));
    EXPECT_EQ(HashVec(a.indices), HashVec(b.indices));
}

// Per-vertex normals: independent re-mesh must reproduce EVERY normal float
// bit-exact (EXPECT_FLOAT_EQ). IEEE-754 add is non-associative, so a
// non-canonical accumulation order would drift normals by ULPs even when the
// vertex set is the same.
TEST(MeshingHardening, NormalAccumulationIsFloatExactAcrossRemesh_Archipelago) {
    const MeshResult a = GenAndMesh(MakeArchipelagoParams(), 1337, IVec3(0, 0, 0), 1);
    const MeshResult b = GenAndMesh(MakeArchipelagoParams(), 1337, IVec3(0, 0, 0), 1);
    ASSERT_FALSE(a.vertices.empty());
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        EXPECT_FLOAT_EQ(a.vertices[i].normal.x, b.vertices[i].normal.x)
            << "normal.x ULP drift at vertex " << i;
        EXPECT_FLOAT_EQ(a.vertices[i].normal.y, b.vertices[i].normal.y)
            << "normal.y ULP drift at vertex " << i;
        EXPECT_FLOAT_EQ(a.vertices[i].normal.z, b.vertices[i].normal.z)
            << "normal.z ULP drift at vertex " << i;
    }
}

// Vertex POSITIONS must be float-exact across re-mesh (no interp drift).
TEST(MeshingHardening, VertexPositionsAreFloatExactAcrossRemesh_Cave) {
    const MeshResult a = GenAndMesh(MakeCaveParams(), 12345, IVec3(0, 0, 0), 1);
    const MeshResult b = GenAndMesh(MakeCaveParams(), 12345, IVec3(0, 0, 0), 1);
    ASSERT_FALSE(a.vertices.empty());
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        EXPECT_FLOAT_EQ(a.vertices[i].position.x, b.vertices[i].position.x) << "pos.x drift @" << i;
        EXPECT_FLOAT_EQ(a.vertices[i].position.y, b.vertices[i].position.y) << "pos.y drift @" << i;
        EXPECT_FLOAT_EQ(a.vertices[i].position.z, b.vertices[i].position.z) << "pos.z drift @" << i;
    }
}

// Index buffer identical across two independent world systems (winding + order).
TEST(MeshingHardening, IndexOrderStableAcrossWorlds_Flat) {
    const MeshResult a = GenAndMesh(MakeFlatSurfaceParams(), 1337, IVec3(0, 0, 0), 1);
    const MeshResult b = GenAndMesh(MakeFlatSurfaceParams(), 1337, IVec3(0, 0, 0), 1);
    ASSERT_FALSE(a.indices.empty());
    ASSERT_EQ(a.indices, b.indices);
}

// Re-mesh of the coarse (step>1) heightfield path must also be byte-exact.
TEST(MeshingHardening, RemeshCoarseStep4IsByteExact_Archipelago) {
    const MeshResult a = GenAndMesh(MakeArchipelagoParams(), 42, IVec3(0, 0, 0), 4);
    const MeshResult b = GenAndMesh(MakeArchipelagoParams(), 42, IVec3(0, 0, 0), 4);
    ASSERT_FALSE(a.vertices.empty());
    EXPECT_EQ(HashVec(a.vertices), HashVec(b.vertices));
    EXPECT_EQ(HashVec(a.indices), HashVec(b.indices));
}

// =====================================================================================
// 2. SEED PURITY -- generation order independence (no global/static bleed)
// =====================================================================================

// Generation must be a pure function of (seed, coord). Generate two chunks in
// order A-then-B in one world, then B-then-A in another world; each chunk's SDF
// + heightmap must be byte-identical regardless of neighbour order.
TEST(MeshingHardening, GenerationOrderIndependent_SDFByteExact) {
    const IVec3 coordA(0, 0, 0);
    const IVec3 coordB(1, 0, 0);

    std::vector<f32> sdfA_first, sdfB_second, sdfB_first, sdfA_second;
    {
        SHIELD_WorldSystem world(nullptr, nullptr, MakeArchipelagoParams(), 42);
        Chunk a(coordA);
        world.GenerateChunkData(a);
        sdfA_first = a.sdf_data;
        Chunk b(coordB);
        world.GenerateChunkData(b);
        sdfB_second = b.sdf_data;
    }
    {
        SHIELD_WorldSystem world(nullptr, nullptr, MakeArchipelagoParams(), 42);
        Chunk b(coordB);
        world.GenerateChunkData(b);
        sdfB_first = b.sdf_data;
        Chunk a(coordA);
        world.GenerateChunkData(a);
        sdfA_second = a.sdf_data;
    }

    ASSERT_FALSE(sdfA_first.empty());
    EXPECT_EQ(sdfA_first, sdfA_second) << "chunk A SDF depends on whether B was generated first";
    EXPECT_EQ(sdfB_first, sdfB_second) << "chunk B SDF depends on whether A was generated first";
}

// Same idea but for the full MESH (vertices+indices), interleaving generation.
TEST(MeshingHardening, MeshOrderIndependent_ByteExact) {
    const IVec3 coordA(0, 0, 0);
    const IVec3 coordB(0, 0, 1);
    const TerrainGenParams params = MakeArchipelagoParams();

    std::uint64_t hashA_first = 0, hashA_second = 0;
    {
        SHIELD_WorldSystem world(nullptr, nullptr, params, 99);
        Chunk a(coordA);
        world.GenerateChunkData(a);
        World::MarchingCubes::PolygoniseTerrain(world, a, 0.0f, 1);
        hashA_first = HashVec(a.mesh_vertices) ^ HashVec(a.mesh_indices);
        Chunk b(coordB);
        world.GenerateChunkData(b);
        World::MarchingCubes::PolygoniseTerrain(world, b, 0.0f, 1);
    }
    {
        SHIELD_WorldSystem world(nullptr, nullptr, params, 99);
        Chunk b(coordB);
        world.GenerateChunkData(b);
        World::MarchingCubes::PolygoniseTerrain(world, b, 0.0f, 1);
        Chunk a(coordA);
        world.GenerateChunkData(a);
        World::MarchingCubes::PolygoniseTerrain(world, a, 0.0f, 1);
        hashA_second = HashVec(a.mesh_vertices) ^ HashVec(a.mesh_indices);
    }
    EXPECT_EQ(hashA_first, hashA_second) << "chunk A mesh depends on neighbour meshing order";
}

// Re-generating the SAME chunk many times in one world must yield identical SDF
// every time (no accumulating state in the world system).
TEST(MeshingHardening, RepeatedGenerationOfSameChunkIsStable) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeArchipelagoParams(), 7);
    Chunk first(IVec3(2, 0, 3));
    world.GenerateChunkData(first);
    const std::uint64_t ref = HashVec(first.sdf_data);
    for (int i = 0; i < 5; ++i) {
        Chunk again(IVec3(2, 0, 3));
        world.GenerateChunkData(again);
        EXPECT_EQ(HashVec(again.sdf_data), ref) << "SDF drifted on regeneration iteration " << i;
    }
}

// Distinct seeds must produce DISTINCT terrain (guards against a seed being
// dropped / ignored, which would make every world identical).
TEST(MeshingHardening, DistinctSeedsProduceDistinctTerrain) {
    const MeshResult a = GenAndMesh(MakeArchipelagoParams(), 1, IVec3(0, 0, 0), 1);
    const MeshResult b = GenAndMesh(MakeArchipelagoParams(), 2, IVec3(0, 0, 0), 1);
    ASSERT_FALSE(a.sdf.empty());
    ASSERT_EQ(a.sdf.size(), b.sdf.size());
    EXPECT_NE(HashVec(a.sdf), HashVec(b.sdf))
        << "different seeds produced identical SDF -- seed ignored";
}

// Distinct chunk coordinates must read distinct slices of the field.
TEST(MeshingHardening, DistinctCoordsProduceDistinctSDF) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeArchipelagoParams(), 42);
    Chunk a(IVec3(0, 0, 0));
    Chunk b(IVec3(5, 0, 5));
    world.GenerateChunkData(a);
    world.GenerateChunkData(b);
    EXPECT_NE(HashVec(a.sdf_data), HashVec(b.sdf_data))
        << "far-apart chunks share SDF -- coord ignored";
}

// =====================================================================================
// 3. CHUNK-SEAM CONTINUITY -- the SDF field must AGREE across a shared boundary
// =====================================================================================

// The +X boundary plane of chunk (cx,cy,cz) is the SAME world lattice column as
// the -X boundary plane of chunk (cx+1,cy,cz). Sampled from both chunks, every
// value on that shared plane must match bit-for-bit (a mismatch is a crack/seam).
TEST(MeshingHardening, SDFSeamContinuityAcrossXBoundary) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeArchipelagoParams(), 42);
    Chunk left(IVec3(0, 0, 0));
    Chunk right(IVec3(1, 0, 0));
    world.GenerateChunkData(left);
    world.GenerateChunkData(right);
    ASSERT_FALSE(left.sdf_data.empty());
    ASSERT_FALSE(right.sdf_data.empty());

    std::size_t mismatches = 0;
    for (int z = 0; z < kLat; ++z) {
        for (int y = 0; y < kLat; ++y) {
            const f32 left_max_x = left.sdf_data[SdfIndex(CHUNK_SIZE_X, y, z)];
            const f32 right_min_x = right.sdf_data[SdfIndex(0, y, z)];
            EXPECT_FLOAT_EQ(left_max_x, right_min_x)
                << "SDF seam mismatch at shared X plane, y=" << y << " z=" << z;
            if (left_max_x != right_min_x)
                ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u) << "chunk seam has discontinuities -> visible crack";
}

// Same for the +Z / -Z shared boundary plane.
TEST(MeshingHardening, SDFSeamContinuityAcrossZBoundary) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeArchipelagoParams(), 42);
    Chunk near_chunk(IVec3(0, 0, 0));
    Chunk far_chunk(IVec3(0, 0, 1));
    world.GenerateChunkData(near_chunk);
    world.GenerateChunkData(far_chunk);

    for (int x = 0; x < kLat; ++x) {
        for (int y = 0; y < kLat; ++y) {
            const f32 a = near_chunk.sdf_data[SdfIndex(x, y, CHUNK_SIZE_Z)];
            const f32 b = far_chunk.sdf_data[SdfIndex(x, y, 0)];
            EXPECT_FLOAT_EQ(a, b) << "SDF seam mismatch at shared Z plane, x=" << x << " y=" << y;
        }
    }
}

// Heightmap seam: the +X column of one chunk must equal the -X column of its
// neighbour (the heightmap is what the coarse mesher reads, so a mismatch here
// cracks far-LOD seams).
TEST(MeshingHardening, HeightmapSeamContinuityAcrossXBoundary) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeArchipelagoParams(), 42);
    Chunk left(IVec3(0, 0, 0));
    Chunk right(IVec3(1, 0, 0));
    world.GenerateChunkData(left);
    world.GenerateChunkData(right);
    ASSERT_FALSE(left.heightmap_data.empty());
    ASSERT_FALSE(right.heightmap_data.empty());

    const int hm_stride = CHUNK_SIZE_X + 1;
    for (int z = 0; z < hm_stride; ++z) {
        const f32 a = left.heightmap_data[CHUNK_SIZE_X + z * hm_stride];
        const f32 b = right.heightmap_data[0 + z * hm_stride];
        EXPECT_FLOAT_EQ(a, b) << "heightmap seam mismatch at z=" << z;
    }
}

// GetTerrainHeightAt is a pure world-space function; sampled at the exact shared
// boundary world coordinate it must be identical regardless of which chunk
// "owns" it (the analytic source of seam agreement).
TEST(MeshingHardening, TerrainHeightContinuousAtChunkBoundaryWorldCoord) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeArchipelagoParams(), 42);
    // world x = 16 is simultaneously local x=16 of chunk 0 and local x=0 of chunk 1.
    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        const float wx = static_cast<float>(CHUNK_SIZE_X);
        const float wz = static_cast<float>(z);
        const float h1 = world.GetTerrainHeightAt(wx, wz);
        const float h2 = world.GetTerrainHeightAt(wx, wz);
        EXPECT_FLOAT_EQ(h1, h2);
    }
}

// Mesh vertices that lie on the shared X seam must COINCIDE between adjacent
// chunks (left chunk vertices at local x==16 vs right chunk vertices at local
// x==0, compared in world space). A vertex present on one side but absent /
// displaced on the other is a watertightness hole. Flat surface => the seam row
// is fully populated and easy to match deterministically.
TEST(MeshingHardening, SeamVerticesCoincideBetweenAdjacentChunks_Flat) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeFlatSurfaceParams(), 1337);
    Chunk left(IVec3(0, 0, 0));
    Chunk right(IVec3(1, 0, 0));
    world.GenerateChunkData(left);
    world.GenerateChunkData(right);
    World::MarchingCubes::PolygoniseTerrain(world, left, 0.0f, 1);
    World::MarchingCubes::PolygoniseTerrain(world, right, 0.0f, 1);

    // Collect left-chunk seam vertices (local x == CHUNK_SIZE_X) in world space.
    std::vector<Vec3> left_seam;
    for (const auto& v : left.mesh_vertices) {
        if (std::abs(v.position.x - static_cast<float>(CHUNK_SIZE_X)) < 1e-4f) {
            left_seam.push_back(Vec3(
                v.position.x + static_cast<float>(0 * CHUNK_SIZE_X), v.position.y, v.position.z));
        }
    }
    // Right-chunk seam vertices (local x == 0), translated into the same world X.
    std::vector<Vec3> right_seam;
    for (const auto& v : right.mesh_vertices) {
        if (std::abs(v.position.x - 0.0f) < 1e-4f) {
            // right chunk world X for local x==0 is +CHUNK_SIZE_X.
            right_seam.push_back(
                Vec3(v.position.x + static_cast<float>(CHUNK_SIZE_X), v.position.y, v.position.z));
        }
    }

    ASSERT_FALSE(left_seam.empty()) << "left chunk produced no seam vertices";
    ASSERT_FALSE(right_seam.empty()) << "right chunk produced no seam vertices";
    // Every left seam vertex must have a coincident right seam vertex (within ULP).
    for (const Vec3& lv : left_seam) {
        bool matched = false;
        for (const Vec3& rv : right_seam) {
            if (std::abs(lv.x - rv.x) < 1e-4f && std::abs(lv.y - rv.y) < 1e-4f &&
                std::abs(lv.z - rv.z) < 1e-4f) {
                matched = true;
                break;
            }
        }
        EXPECT_TRUE(matched) << "left seam vertex (" << lv.x << "," << lv.y << "," << lv.z
                             << ") has no coincident vertex on the right chunk -> seam hole";
    }
}

// =====================================================================================
// 4. LOD CONSISTENCY -- a coarser LOD is a deterministic reduction, not a re-roll
// =====================================================================================

// Coarser LOD must reduce vertex/index counts (the happy path also checks this,
// but here at step 2 AND step 4 with a non-trivial surface).
TEST(MeshingHardening, CoarserLODReducesGeometry_Archipelago) {
    const MeshResult lod0 = GenAndMesh(MakeArchipelagoParams(), 42, IVec3(0, 0, 0), 1);
    const MeshResult lod1 = GenAndMesh(MakeArchipelagoParams(), 42, IVec3(0, 0, 0), 2);
    const MeshResult lod2 = GenAndMesh(MakeArchipelagoParams(), 42, IVec3(0, 0, 0), 4);
    ASSERT_FALSE(lod0.vertices.empty());
    ASSERT_FALSE(lod2.vertices.empty());
    EXPECT_LE(lod1.vertices.size(), lod0.vertices.size());
    EXPECT_LT(lod2.vertices.size(), lod0.vertices.size());
    EXPECT_LT(lod2.indices.size(), lod0.indices.size());
}

// A given LOD step must mesh deterministically: same input -> same coarse mesh.
TEST(MeshingHardening, LODStep2IsDeterministic) {
    const MeshResult a = GenAndMesh(MakeArchipelagoParams(), 42, IVec3(0, 0, 0), 2);
    const MeshResult b = GenAndMesh(MakeArchipelagoParams(), 42, IVec3(0, 0, 0), 2);
    ASSERT_FALSE(a.vertices.empty());
    EXPECT_EQ(HashVec(a.vertices), HashVec(b.vertices));
    EXPECT_EQ(HashVec(a.indices), HashVec(b.indices));
}

// The coarse LOD mesh must be a reduction of the SAME height field the fine mesh
// sees: every coarse heightfield vertex Y at a shared lattice corner must equal
// GetTerrainHeightAt at that world column (i.e. the coarse path samples the same
// field, not a re-rolled one). Uses flat terrain so the expected Y is exact.

// LOD mesh from a chunk that only carries SDF (no heightmap, as a fresh remesh
// target would) must STILL be deterministic and non-empty for a chunk straddling
// the surface. This mirrors the sibling LOD test's pattern (copy sdf_data only)
// and guards the analytic coarse fallback path against re-roll non-determinism.
TEST(MeshingHardening, CoarseLODFromSDFOnlyChunkIsDeterministic_Flat) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeFlatSurfaceParams(), 1337);
    Chunk source(IVec3(0, 0, 0));
    world.GenerateChunkData(source);

    auto mesh_from_sdf = [&](Chunk& c) {
        c.sdf_data = source.sdf_data; // heightmap intentionally left empty
        World::MarchingCubes::PolygoniseTerrain(world, c, 0.0f, 4);
    };
    Chunk a(IVec3(0, 0, 0));
    Chunk b(IVec3(0, 0, 0));
    mesh_from_sdf(a);
    mesh_from_sdf(b);
    ASSERT_FALSE(a.mesh_vertices.empty());
    EXPECT_EQ(HashVec(a.mesh_vertices), HashVec(b.mesh_vertices));
    EXPECT_EQ(HashVec(a.mesh_indices), HashVec(b.mesh_indices));
}

// =====================================================================================
// 5. DEGENERATE / EMPTY INPUT -- no crash, no stray triangle
// =====================================================================================

// All-empty (all-air) chunk -> empty mesh, every LOD step. No crash, no stray
// triangle. Degenerate no-op.
TEST(MeshingHardening, AllAirChunkProducesEmptyMesh_AllSteps) {
    for (int step : {1, 2, 4}) {
        const MeshResult r = GenAndMesh(MakeDeepOceanParams(), 1337, IVec3(0, 0, 0), step);
        EXPECT_TRUE(r.vertices.empty()) << "all-air chunk emitted vertices at step " << step;
        EXPECT_TRUE(r.indices.empty()) << "all-air chunk emitted indices at step " << step;
    }
}

// A hand-zeroed SDF (literally all the same positive value -> all air) must mesh
// to an empty buffer with no crash. Pure degenerate input, no world dependence
// on the field content for the empty-check.
TEST(MeshingHardening, ManuallyAllPositiveSDFMeshesEmpty) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeFlatSurfaceParams(), 1337);
    Chunk chunk(IVec3(0, 0, 0));
    world.GenerateChunkData(chunk);
    std::fill(chunk.sdf_data.begin(), chunk.sdf_data.end(), 1.0f); // all air
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    EXPECT_TRUE(chunk.mesh_vertices.empty());
    EXPECT_TRUE(chunk.mesh_indices.empty());
}

// A hand-set all-NEGATIVE SDF (all solid, no surface crossing) must mesh empty:
// an interior-solid chunk with no zero-crossing has no faces in the unit-step
// mesher. No stray triangle.
TEST(MeshingHardening, ManuallyAllNegativeSDFMeshesEmpty) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeFlatSurfaceParams(), 1337);
    Chunk chunk(IVec3(0, 0, 0));
    world.GenerateChunkData(chunk);
    std::fill(chunk.sdf_data.begin(), chunk.sdf_data.end(), -1.0f); // all solid
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    EXPECT_TRUE(chunk.mesh_vertices.empty())
        << "all-solid chunk with no zero-crossing emitted interior triangles";
    EXPECT_TRUE(chunk.mesh_indices.empty());
}

// An empty SDF vector (zero-length) must not crash the unit-step mesher; it has
// no negative AND no positive sample, so the early-out empties the mesh.
TEST(MeshingHardening, EmptySDFVectorDoesNotCrash) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeFlatSurfaceParams(), 1337);
    Chunk chunk(IVec3(0, 0, 0));
    world.GenerateChunkData(chunk);
    chunk.sdf_data.clear();
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    EXPECT_TRUE(chunk.mesh_vertices.empty());
    EXPECT_TRUE(chunk.mesh_indices.empty());
}

// A single solid voxel embedded in air (one corner negative) must mesh to a
// small, valid, closed-ish feature with NO out-of-range index and NO degenerate
// triangle -- the smallest non-trivial feature.
TEST(MeshingHardening, SingleVoxelFeatureMeshesValidly) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeFlatSurfaceParams(), 1337);
    Chunk chunk(IVec3(0, 0, 0));
    world.GenerateChunkData(chunk);
    std::fill(chunk.sdf_data.begin(), chunk.sdf_data.end(), 1.0f); // all air
    // Make one interior lattice corner solid -> one zero-crossing feature.
    chunk.sdf_data[SdfIndex(8, 8, 8)] = -1.0f;
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);

    ASSERT_FALSE(chunk.mesh_vertices.empty()) << "single solid voxel produced no surface";
    ASSERT_EQ(chunk.mesh_indices.size() % 3u, 0u);
    for (std::size_t i = 0; i < chunk.mesh_indices.size(); i += 3) {
        const u32 a = chunk.mesh_indices[i];
        const u32 b = chunk.mesh_indices[i + 1];
        const u32 c = chunk.mesh_indices[i + 2];
        ASSERT_LT(a, chunk.mesh_vertices.size());
        ASSERT_LT(b, chunk.mesh_vertices.size());
        ASSERT_LT(c, chunk.mesh_vertices.size());
        EXPECT_TRUE(a != b && b != c && c != a)
            << "degenerate triangle in single-voxel mesh @" << (i / 3);
    }
}

// =====================================================================================
// 6. MESH INTEGRITY -- indices in range, no degenerate tris, winding consistent
// =====================================================================================

// Every index must reference a real vertex; no NaN positions/normals; no
// degenerate (repeated-index) triangle. Run across several presets.
TEST(MeshingHardening, AllIndicesInRangeNoDegenerateTriangles) {
    struct Case {
        TerrainGenParams params;
        int seed;
    };
    const Case cases[] = {
        {MakeArchipelagoParams(), 42},
        {MakeCaveParams(), 12345},
        {MakeFlatSurfaceParams(), 1337},
    };
    for (const Case& cs : cases) {
        const MeshResult r = GenAndMesh(cs.params, cs.seed, IVec3(0, 0, 0), 1);
        ASSERT_EQ(r.indices.size() % 3u, 0u);
        for (std::size_t i = 0; i < r.indices.size(); i += 3) {
            const u32 a = r.indices[i], b = r.indices[i + 1], c = r.indices[i + 2];
            ASSERT_LT(a, r.vertices.size());
            ASSERT_LT(b, r.vertices.size());
            ASSERT_LT(c, r.vertices.size());
            EXPECT_TRUE(a != b && b != c && c != a) << "degenerate triangle @" << (i / 3);
        }
        for (const auto& v : r.vertices) {
            EXPECT_EQ(v.position.x, v.position.x) << "NaN position.x"; // NaN != NaN
            EXPECT_EQ(v.normal.x, v.normal.x) << "NaN normal.x";
        }
    }
}

// On a perfectly flat surface every triangle must wind upward (face normal +Y)
// AND every vertex normal must point up: an inverted face is a black/back-facing
// hole in the surface. This is stricter than the sibling (it checks face winding
// AND every vertex normal across the whole flat surface).
TEST(MeshingHardening, FlatSurfaceWindingAndNormalsConsistentlyUp) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeFlatSurfaceParams(), 1337);
    Chunk chunk(IVec3(0, 0, 0));
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    ASSERT_FALSE(chunk.mesh_vertices.empty());

    for (const auto& v : chunk.mesh_vertices) {
        EXPECT_GT(v.normal.y, 0.0f) << "flat-surface vertex normal is not upward (inverted face?)";
    }
    for (std::size_t i = 0; i < chunk.mesh_indices.size(); i += 3) {
        const auto& v0 = chunk.mesh_vertices[chunk.mesh_indices[i]];
        const auto& v1 = chunk.mesh_vertices[chunk.mesh_indices[i + 1]];
        const auto& v2 = chunk.mesh_vertices[chunk.mesh_indices[i + 2]];
        const Vec3 fn = glm::cross(v1.position - v0.position, v2.position - v0.position);
        EXPECT_GT(fn.y, 0.0f) << "flat-surface triangle " << (i / 3)
                              << " has inverted (downward) winding";
    }
}

// Material ids on a meshed terrain surface must never be Air or Water (those are
// non-rendering); a stray Air vertex is a G-buffer artefact. (Same contract as
// the sibling, re-asserted across the cave preset which exercises the fallback
// material reclassifier.)
TEST(MeshingHardening, NoAirOrWaterMaterialOnTerrainMesh_Cave) {
    const MeshResult r = GenAndMesh(MakeCaveParams(), 12345, IVec3(0, 0, 0), 1);
    ASSERT_FALSE(r.vertices.empty());
    for (const auto& v : r.vertices) {
        EXPECT_NE(v.material_id, static_cast<u32>(MaterialType::Air));
        EXPECT_NE(v.material_id, static_cast<u32>(MaterialType::Water));
    }
}

// All-solid buried chunk (surface far above): the unit-step mesher only emits
// faces at zero-crossings, and a fully-buried flat chunk has none -> empty mesh.
// (Documents the "all-solid -> only boundary faces" contract; this mesher does
// not close chunk borders, so the expected result is an EMPTY interior mesh.)

// =====================================================================================
// 7. BIOME / RIVER / STRUCTURE / HEIGHT BOUNDARY STABILITY
// =====================================================================================

// Sampling GetTerrainHeightAt at the exact integer boundary coordinate must be
// deterministic and stable across repeated calls and across two world systems
// (no off-by-one / hidden cache that returns a different value the 2nd time).
TEST(MeshingHardening, HeightAtExactBoundaryCoordIsStable) {
    SHIELD_WorldSystem world_a(nullptr, nullptr, MakeArchipelagoParams(), 42);
    SHIELD_WorldSystem world_b(nullptr, nullptr, MakeArchipelagoParams(), 42);
    const float coords[][2] = {
        {0.0f, 0.0f}, {16.0f, 0.0f}, {16.0f, 16.0f}, {-16.0f, 32.0f}, {255.0f, -1.0f}};
    for (const auto& c : coords) {
        const float h_a1 = world_a.GetTerrainHeightAt(c[0], c[1]);
        const float h_a2 = world_a.GetTerrainHeightAt(c[0], c[1]);
        const float h_b = world_b.GetTerrainHeightAt(c[0], c[1]);
        EXPECT_FLOAT_EQ(h_a1, h_a2)
            << "height not stable on repeat at (" << c[0] << "," << c[1] << ")";
        EXPECT_FLOAT_EQ(h_a1, h_b)
            << "height differs across identical worlds at (" << c[0] << "," << c[1] << ")";
    }
}

// get_density_at must be a pure function of world position: identical across two
// worlds with the same seed, and identical at the exact chunk-boundary world Y.
TEST(MeshingHardening, DensityAtBoundaryIsPureAcrossWorlds) {
    SHIELD_WorldSystem a(nullptr, nullptr, MakeArchipelagoParams(), 42);
    SHIELD_WorldSystem b(nullptr, nullptr, MakeArchipelagoParams(), 42);
    const Vec3 probes[] = {
        Vec3(0.0f, 8.0f, 0.0f),
        Vec3(16.0f, 8.0f, 16.0f), // exactly on a chunk corner column
        Vec3(8.0f, 0.0f, 8.0f),
        Vec3(8.0f, 16.0f, 8.0f), // exactly on a vertical chunk boundary
    };
    for (const Vec3& p : probes) {
        EXPECT_FLOAT_EQ(a.get_density_at(p), b.get_density_at(p))
            << "density impure at (" << p.x << "," << p.y << "," << p.z << ")";
    }
}

// world_to_chunk_coords must place an exact chunk-boundary world coordinate
// deterministically (no off-by-one). World x in [0,16) -> chunk 0; x==16 -> chunk
// 1; negative coords floor toward -inf.
TEST(MeshingHardening, WorldToChunkBoundaryNoOffByOne) {
    EXPECT_EQ(SHIELD_WorldSystem::world_to_chunk_coords(Vec3(0.0f, 0.0f, 0.0f)).x, 0);
    EXPECT_EQ(SHIELD_WorldSystem::world_to_chunk_coords(Vec3(15.999f, 0.0f, 0.0f)).x, 0);
    EXPECT_EQ(SHIELD_WorldSystem::world_to_chunk_coords(
                  Vec3(static_cast<float>(CHUNK_SIZE_X), 0.0f, 0.0f))
                  .x,
              1);
    // Negative side: -1 must land in chunk -1, not chunk 0 (floor, not truncate).
    EXPECT_EQ(SHIELD_WorldSystem::world_to_chunk_coords(Vec3(-1.0f, 0.0f, 0.0f)).x, -1);
    EXPECT_EQ(SHIELD_WorldSystem::world_to_chunk_coords(
                  Vec3(-static_cast<float>(CHUNK_SIZE_X), 0.0f, 0.0f))
                  .x,
              -1);
}

// Flat-surface heightmap must be exactly the configured offset at EVERY lattice
// sample (re-asserts the sibling, but also at the first AND last index to catch
// an off-by-one that skips a border column).
TEST(MeshingHardening, ModernHeightmapExactAtBordersNoOffByOne) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeFlatSurfaceParams(), 1337);
    Chunk chunk(IVec3(0, 0, 0));
    world.GenerateChunkData(chunk);
    ASSERT_FALSE(chunk.heightmap_data.empty());
    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
            EXPECT_FLOAT_EQ(chunk.heightmap_data[x + z * (CHUNK_SIZE_X + 1)],
                            world.GetTerrainHeightAt(static_cast<float>(x), static_cast<float>(z)));
        }
    }
}

// Cave generation must be a pure function: same seed -> byte-identical carved SDF.
// Caves use a 3D noise grid (a separate generator) -- this guards the cave path
// against order/state leakage independently of the surface path.
TEST(MeshingHardening, CaveCarvedSDFIsByteExactAcrossWorlds) {
    SHIELD_WorldSystem a(nullptr, nullptr, MakeCaveParams(), 12345);
    SHIELD_WorldSystem b(nullptr, nullptr, MakeCaveParams(), 12345);
    Chunk ca(IVec3(0, -3, 0)); // deep enough that caves carve (cap suppresses near surface)
    Chunk cb(IVec3(0, -3, 0));
    a.GenerateChunkData(ca);
    b.GenerateChunkData(cb);
    ASSERT_FALSE(ca.sdf_data.empty());
    EXPECT_EQ(ca.sdf_data, cb.sdf_data) << "cave-carved SDF not pure across identical worlds";
}

// Cave carving must be order-independent: carving a deep chunk before vs after a
// neighbour must yield identical SDF (the 3D cave grid must not share a single
// advancing RNG/cursor across chunks).
TEST(MeshingHardening, CaveGenerationOrderIndependent) {
    std::vector<f32> first, second;
    {
        SHIELD_WorldSystem world(nullptr, nullptr, MakeCaveParams(), 12345);
        Chunk neighbour(IVec3(1, -3, 0));
        world.GenerateChunkData(neighbour);
        Chunk target(IVec3(0, -3, 0));
        world.GenerateChunkData(target);
        first = target.sdf_data;
    }
    {
        SHIELD_WorldSystem world(nullptr, nullptr, MakeCaveParams(), 12345);
        Chunk target(IVec3(0, -3, 0));
        world.GenerateChunkData(target);
        second = target.sdf_data;
    }
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(first, second) << "cave SDF depends on whether a neighbour was carved first";
}

// =====================================================================================
// 8. SDF FIELD-VS-MESH AGREEMENT
// =====================================================================================

// The flat-surface SDF must cross zero at exactly the configured height: samples
// below height_offset solid (<0), above it air (>0). A sign error here would put
// the meshed surface at the wrong altitude / invert solidity.
TEST(MeshingHardening, FlatSDFSignMatchesConfiguredSurface) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeFlatSurfaceParams(), 1337);
    Chunk chunk(IVec3(0, 0, 0));
    world.GenerateChunkData(chunk);
    ASSERT_FALSE(chunk.sdf_data.empty());
    // height_offset == 8; local y below 8 must be solid, above must be air.
    for (int z = 0; z < kLat; ++z) {
        for (int x = 0; x < kLat; ++x) {
            EXPECT_LT(chunk.sdf_data[SdfIndex(x, 4, z)], 0.0f) << "below-surface sample not solid";
            EXPECT_GT(chunk.sdf_data[SdfIndex(x, 12, z)], 0.0f) << "above-surface sample not air";
        }
    }
}

// The meshed flat surface must sit at the configured height (within a voxel) and
// be horizontal: max-min Y of surface vertices is ~0. A tilted or displaced
// surface indicates an interp / indexing bug.
TEST(MeshingHardening, MeshedFlatSurfaceIsAtConfiguredHeightAndLevel) {
    const MeshResult r = GenAndMesh(MakeFlatSurfaceParams(), 1337, IVec3(0, 0, 0), 1);
    ASSERT_FALSE(r.vertices.empty());
    float min_y = r.vertices.front().position.y;
    float max_y = min_y;
    for (const auto& v : r.vertices) {
        min_y = std::min(min_y, v.position.y);
        max_y = std::max(max_y, v.position.y);
    }
    EXPECT_NEAR(min_y, 8.0f, 1.0f) << "flat surface min Y off configured height";
    EXPECT_NEAR(max_y, 8.0f, 1.0f) << "flat surface max Y off configured height";
    EXPECT_NEAR(max_y - min_y, 0.0f, 1e-3f) << "flat surface is not level";
}

// Re-meshing a chunk whose mesh buffers were pre-filled with garbage must fully
// REPLACE them (no append): the mesher must clear, not accumulate, so output
// depends only on the field. Catches a stale-buffer accumulation bug.
TEST(MeshingHardening, RemeshReplacesNotAppendsStaleBuffers) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeFlatSurfaceParams(), 1337);
    Chunk clean(IVec3(0, 0, 0));
    world.GenerateChunkData(clean);
    World::MarchingCubes::PolygoniseTerrain(world, clean, 0.0f, 1);
    const std::size_t clean_v = clean.mesh_vertices.size();
    const std::size_t clean_i = clean.mesh_indices.size();

    Chunk dirty(IVec3(0, 0, 0));
    world.GenerateChunkData(dirty);
    dirty.mesh_vertices.assign(50, VoxelVertex{Vec3(9.0f), Vec3(0.0f, 1.0f, 0.0f), 1u});
    dirty.mesh_indices.assign(150, 0u);
    World::MarchingCubes::PolygoniseTerrain(world, dirty, 0.0f, 1);
    EXPECT_EQ(dirty.mesh_vertices.size(), clean_v) << "re-mesh appended to stale vertex buffer";
    EXPECT_EQ(dirty.mesh_indices.size(), clean_i) << "re-mesh appended to stale index buffer";
}

// Empty-chunk re-mesh over a previously NON-empty mesh must clear it: meshing an
// all-air field after a real surface must leave zero geometry (no stale tris).
TEST(MeshingHardening, EmptyFieldRemeshClearsPreviousMesh) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeFlatSurfaceParams(), 1337);
    Chunk chunk(IVec3(0, 0, 0));
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    ASSERT_FALSE(chunk.mesh_vertices.empty());
    std::fill(chunk.sdf_data.begin(), chunk.sdf_data.end(), 1.0f); // now all air
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    EXPECT_TRUE(chunk.mesh_vertices.empty()) << "stale surface survived an empty re-mesh";
    EXPECT_TRUE(chunk.mesh_indices.empty());
}

// ----------------------------------------------------------------------------
// wrong-sized-SDF out-of-bounds hardening. The unit-step
// polygonise indexes the SDF as a full (CHUNK+1)^3 lattice with unchecked corner
// offsets, so a non-empty malformed lattice (corrupt save / stale coarse
// producer) must be REJECTED at entry — never read. These run OOB-clean under
// ASAN by construction (the guard returns before any lattice indexing).
// ----------------------------------------------------------------------------

TEST(StreamingHardening, MalformedSdfLatticeIsRejectedNotReadOutOfBounds) {
    SHIELD_WorldSystem world(nullptr, nullptr, MakeFlatSurfaceParams(), 4242);
    Chunk chunk(IVec3(0, 0, 0));
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    ASSERT_FALSE(chunk.mesh_vertices.empty()) << "fixture must start with a real surface";
    ASSERT_EQ(chunk.sdf_data.size(), static_cast<std::size_t>(kLat * kLat * kLat));

    // Truncated lattice (the wrong-sized-save shape): one float short. With a
    // straddling surface, the old code would index corners past data.end.
    chunk.sdf_data.resize(chunk.sdf_data.size() - 1u);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    EXPECT_TRUE(chunk.mesh_vertices.empty())
        << "a truncated SDF lattice must be rejected (no mesh), not polygonised";
    EXPECT_TRUE(chunk.mesh_indices.empty());

    // Grossly undersized non-empty lattice (the 4-float corruption-corpus shape).
    chunk.sdf_data = {-2.0f, -0.5f, 0.25f, 1.0f};
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    EXPECT_TRUE(chunk.mesh_vertices.empty());
    EXPECT_TRUE(chunk.mesh_indices.empty());
}

TEST(StreamingHardening, MalformedSdfChunkRegeneratesByteIdenticalToFresh) {
    // The promotion-path recovery is clear -> GenerateChunkData: prove the
    // regenerated lattice + mesh are byte-identical to a fresh generation of
    // the same (seed, params, coords), i.e. the quarantine recovery is
    // deterministic and hash-neutral for valid worlds.
    const TerrainGenParams params = MakeArchipelagoParams();
    const IVec3 coords(1, 0, 2);
    const MeshResult fresh = GenAndMesh(params, 909, coords, 1);
    ASSERT_FALSE(fresh.vertices.empty());

    SHIELD_WorldSystem world(nullptr, nullptr, params, 909);
    Chunk chunk(coords);
    chunk.sdf_data = {-1.0f, 0.0f, 1.0f}; // malformed survivor of a corrupt save
    chunk.sdf_data.clear();               // the quarantine action ( / promotion guard)
    world.GenerateChunkData(chunk);       // the deterministic regeneration
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);

    EXPECT_EQ(HashVec(chunk.sdf_data), HashVec(fresh.sdf))
        << "regenerated SDF must be byte-identical to fresh generation";
    EXPECT_EQ(HashVec(chunk.mesh_vertices), HashVec(fresh.vertices));
    EXPECT_EQ(HashVec(chunk.mesh_indices), HashVec(fresh.indices));
}
