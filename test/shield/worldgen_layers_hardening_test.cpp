// ADVERSARIAL determinism-hardening gate for the layered worldgen snapshot
// pipeline + structure/feature placement.
//
// The sibling test/shield/test_worldgen_layer_snapshots.cpp exercises the
// happy path: a chunk generates, meshes, and its layer metrics look sane. This
// file hunts the bugs that path MISSES -- the ones that make the seed->world
// foundation NON-deterministic or NON-watertight:
//
//   * SEED PURITY: identical (seed, region) must produce byte/float-identical
//     SDF, heightmap, and mesh, on a second generation AND when chunks are
//     generated in a different neighbour ORDER (no global/static bleed).
//   * CHUNK-SEAM CONTINUITY: the density/height field sampled on a shared
//     boundary plane must AGREE from both adjacent chunks (a mismatch is a
//     visible crack). Watertightness across the seam.
//   * LOD CONSISTENCY: a coarse LOD is a deterministic reduction of the fine
//     field -- same input, same coarse mesh, re-run identical.
//   * STRUCTURE / FEATURE placement: site presence/jitter/assembly is a pure
//     function of (seed, salt, cell); a seam-straddling site is placed
//     identically from both adjacent regions; no off-by-one at the region edge.
//   * MESHING PURITY: same field -> same triangle set (positions, normals,
//     winding, index order), order-stable. Degenerate input -> empty / boundary
//     mesh, no crash, no stray triangle.
//
// Deterministic only (no wall-clock, no std::random). Where a real defect is
// found the assertion encodes the CORRECT behaviour and is flagged in the
// returned structured report; it is never asserted that the bug is correct.

#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"
#include "world/StructurePlacement.h"

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

constexpr int kSeed = 424242;

// ------------------------------------------------------------------ helpers --

// The archipelago preset from the sibling/world-gen fixtures: real terrain that
// crosses the iso-surface inside chunk (0,0,0) so meshes are non-trivial.
TerrainGenParams ArchipelagoParams() {
    TerrainGenParams params;
    params.base_frequency = 0.035f;
    params.base_amplitude = 8.0f;
    params.octaves = 5;
    params.persistence = 0.5f;
    params.lacunarity = 2.1f;
    params.height_offset = 10.0f;
    params.caves_enabled = false;
    params.island_mask_enabled = false;
    return params;
}

TerrainGenParams CaveParams() {
    TerrainGenParams params = ArchipelagoParams();
    params.caves_enabled = true;
    params.cave_frequency = 0.15f;
    params.cave_threshold = 0.55f;
    params.cave_carve_value = 5.0f;
    return params;
}

// A guaranteed-flat solid surface at a fixed height: predictable for normals /
// winding / degenerate-input reasoning.
TerrainGenParams FlatParams(float height_offset) {
    TerrainGenParams params;
    params.base_frequency = 0.01f;
    params.base_amplitude = 0.0f;
    params.octaves = 4;
    params.persistence = 0.5f;
    params.lacunarity = 2.0f;
    params.height_offset = height_offset;
    params.caves_enabled = false;
    params.island_mask_enabled = false;
    return params;
}

std::size_t SdfIndex(int x, int y, int z) {
    constexpr int size_x = CHUNK_SIZE_X + 1;
    constexpr int size_y = CHUNK_SIZE_Y + 1;
    return static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * size_x +
           static_cast<std::size_t>(z) * size_x * size_y;
}

std::size_t HeightIndex(int x, int z) {
    constexpr int size_x = CHUNK_SIZE_X + 1;
    return static_cast<std::size_t>(x) + static_cast<std::size_t>(z) * size_x;
}

// Generate full-resolution SDF + heightmap for one chunk with a fresh world.
void GenerateFull(const TerrainGenParams& params, int seed, Chunk& out) {
    SHIELD_WorldSystem world(nullptr, nullptr, params, seed);
    world.GenerateChunkData(out);
}

// Generate field + mesh into a chunk owned by the caller.
void GenerateAndMesh(SHIELD_WorldSystem& world, Chunk& chunk, int step) {
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, step);
}

bool VerticesEqual(const VoxelVertex& a, const VoxelVertex& b) {
    return a.position == b.position && a.normal == b.normal && a.material_id == b.material_id;
}

bool MeshesByteEqual(const Chunk& a, const Chunk& b) {
    if (a.mesh_vertices.size() != b.mesh_vertices.size())
        return false;
    if (a.mesh_indices.size() != b.mesh_indices.size())
        return false;
    for (std::size_t i = 0; i < a.mesh_vertices.size(); ++i) {
        if (!VerticesEqual(a.mesh_vertices[i], b.mesh_vertices[i]))
            return false;
    }
    for (std::size_t i = 0; i < a.mesh_indices.size(); ++i) {
        if (a.mesh_indices[i] != b.mesh_indices[i])
            return false;
    }
    return true;
}

// In-memory structure pool (no data files needed) so the pure placement /
// assembly functions can be hammered deterministically.
World::StructureTemplatePool
MakePool(const std::string& type, u32 salt, int spacing, int separation, float density) {
    World::StructureTemplatePool pool;
    pool.type = type;
    pool.spacing = spacing;
    pool.separation = separation;
    pool.salt = salt;
    pool.density = density;

    World::StructurePiece root;
    root.name = "root";
    World::StructureBox box;
    box.min = IVec3(0, 0, 0);
    box.size = IVec3(3, 4, 3);
    box.material = static_cast<u8>(MaterialType::Stone);
    root.boxes.push_back(box);
    World::StructureSocket socket;
    socket.name = "top";
    socket.position = IVec3(1, 4, 1);
    root.sockets.push_back(socket);
    pool.pieces.push_back(root);

    World::StructurePiece cap;
    cap.name = "cap";
    World::StructureBox cap_box;
    cap_box.min = IVec3(0, 0, 0);
    cap_box.size = IVec3(2, 2, 2);
    cap_box.material = static_cast<u8>(MaterialType::Grass);
    cap.boxes.push_back(cap_box);
    World::StructureSocket cap_socket;
    cap_socket.name = "base";
    cap_socket.position = IVec3(0, 0, 0);
    cap.sockets.push_back(cap_socket);
    pool.pieces.push_back(cap);

    // content_hash is consulted by ok only via pieces non-empty; set a value.
    pool.content_hash = 0xABCDEF1234567890ull;
    return pool;
}

} // namespace

// =====================================================================================
// SEED PURITY -- pure function of (seed, region/coord)
// =====================================================================================

TEST(WorldgenHardening, RegenIsByteIdenticalSdfAndHeightmap) {
    const TerrainGenParams params = ArchipelagoParams();
    const IVec3 coords(2, 0, -3);

    Chunk a(coords);
    Chunk b(coords);
    GenerateFull(params, kSeed, a);
    GenerateFull(params, kSeed, b);

    ASSERT_EQ(a.sdf_data.size(), b.sdf_data.size());
    ASSERT_EQ(a.heightmap_data.size(), b.heightmap_data.size());
    // EXPECT_EQ on the flattened float vectors: byte-exact run==replay.
    EXPECT_EQ(a.sdf_data, b.sdf_data);
    EXPECT_EQ(a.heightmap_data, b.heightmap_data);
}

TEST(WorldgenHardening, RegenSameWorldInstanceIsByteIdentical) {
    // A second generation through the SAME world object must not drift (no
    // accumulating internal state, e.g. a mutable noise cursor).
    SHIELD_WorldSystem world(nullptr, nullptr, CaveParams(), kSeed);
    const IVec3 coords(0, -3, 0);

    Chunk a(coords);
    Chunk b(coords);
    world.GenerateChunkData(a);
    world.GenerateChunkData(b);

    EXPECT_EQ(a.sdf_data, b.sdf_data);
    EXPECT_EQ(a.heightmap_data, b.heightmap_data);
}

TEST(WorldgenHardening, GenerationOrderDoesNotAffectChunkA) {
    // Generate {A then B} vs {B then A} on one world; A's field must be
    // identical either way (no neighbour bleed / hidden global state).
    const TerrainGenParams params = ArchipelagoParams();
    const IVec3 a_coords(0, 0, 0);
    const IVec3 b_coords(1, 0, 0);

    SHIELD_WorldSystem world_ab(nullptr, nullptr, params, kSeed);
    Chunk a_first(a_coords);
    Chunk b_second(b_coords);
    world_ab.GenerateChunkData(a_first);
    world_ab.GenerateChunkData(b_second);

    SHIELD_WorldSystem world_ba(nullptr, nullptr, params, kSeed);
    Chunk b_first(b_coords);
    Chunk a_second(a_coords);
    world_ba.GenerateChunkData(b_first);
    world_ba.GenerateChunkData(a_second);

    EXPECT_EQ(a_first.sdf_data, a_second.sdf_data)
        << "chunk A SDF depends on whether neighbour B was generated first";
    EXPECT_EQ(a_first.heightmap_data, a_second.heightmap_data)
        << "chunk A heightmap depends on generation order";
    EXPECT_EQ(b_first.sdf_data, b_second.sdf_data) << "chunk B SDF depends on generation order";
}

TEST(WorldgenHardening, MeshIsByteIdenticalAcrossOrderings) {
    const TerrainGenParams params = ArchipelagoParams();
    const IVec3 a_coords(0, 0, 0);
    const IVec3 b_coords(-1, 0, 1);

    SHIELD_WorldSystem world_ab(nullptr, nullptr, params, kSeed);
    Chunk a1(a_coords), b1(b_coords);
    GenerateAndMesh(world_ab, a1, 1);
    GenerateAndMesh(world_ab, b1, 1);

    SHIELD_WorldSystem world_ba(nullptr, nullptr, params, kSeed);
    Chunk b2(b_coords), a2(a_coords);
    GenerateAndMesh(world_ba, b2, 1);
    GenerateAndMesh(world_ba, a2, 1);

    EXPECT_TRUE(MeshesByteEqual(a1, a2))
        << "chunk A mesh (verts/normals/indices) is order-dependent";
}

TEST(WorldgenHardening, SampleWorldGenLayersMatchesStoredSdfExactly) {
    // The analytic layer sampler (SampleWorldGenLayers) and the stored SDF must
    // agree at every grid node -- they are two views of the same pure field.
    const TerrainGenParams params = CaveParams();
    const IVec3 coords(0, -3, 0);
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk chunk(coords);
    world.GenerateChunkData(chunk);

    const IVec3 base = coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    float max_err = 0.0f;
    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
            for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
                const Vec3 wp(base + IVec3(x, y, z));
                const WorldGenLayerSample s = world.SampleWorldGenLayers(wp);
                const float stored = chunk.sdf_data[SdfIndex(x, y, z)];
                max_err = std::max(max_err, std::abs(s.final_density - stored));
            }
        }
    }
    EXPECT_LT(max_err, 1.0e-4f)
        << "analytic layer sampler diverges from the generated SDF (cave layer order?)";
}

TEST(WorldgenHardening, TerrainHeightIsPureFunctionOfXZ) {
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    // Repeated queries (interleaved with neighbours) must return identical bits.
    const float h0 = world.GetTerrainHeightAt(13.0f, 7.0f);
    (void)world.GetTerrainHeightAt(1000.0f, -1000.0f);
    (void)world.GetTerrainHeightAt(-42.0f, 42.0f);
    const float h1 = world.GetTerrainHeightAt(13.0f, 7.0f);
    EXPECT_FLOAT_EQ(h0, h1);
}

TEST(WorldgenHardening, TwoWorldsSameSeedAgreeOnHeight) {
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world_a(nullptr, nullptr, params, kSeed);
    SHIELD_WorldSystem world_b(nullptr, nullptr, params, kSeed);
    for (float x = -20.0f; x <= 20.0f; x += 6.5f) {
        for (float z = -20.0f; z <= 20.0f; z += 6.5f) {
            EXPECT_FLOAT_EQ(world_a.GetTerrainHeightAt(x, z), world_b.GetTerrainHeightAt(x, z))
                << "height differs across two same-seed worlds at (" << x << "," << z << ")";
        }
    }
}

TEST(WorldgenHardening, DifferentSeedChangesField) {
    const TerrainGenParams params = ArchipelagoParams();
    Chunk a(IVec3(0, 0, 0));
    Chunk b(IVec3(0, 0, 0));
    GenerateFull(params, kSeed, a);
    GenerateFull(params, kSeed + 7, b);
    ASSERT_EQ(a.sdf_data.size(), b.sdf_data.size());
    EXPECT_NE(a.sdf_data, b.sdf_data) << "seed has no effect on the field";
}

// =====================================================================================
// CHUNK-SEAM CONTINUITY -- adjacent chunks must agree on the shared plane
// =====================================================================================

TEST(WorldgenHardening, SeamSdfAgreesAcrossXBoundary) {
    // The max-X plane of chunk(0,0,0) is the SAME world column as the min-X
    // plane of chunk(1,0,0). Their stored SDF must be float-identical, or the
    // marching-cubes surface cracks at the seam.
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk left(IVec3(0, 0, 0));
    Chunk right(IVec3(1, 0, 0));
    world.GenerateChunkData(left);
    world.GenerateChunkData(right);

    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
            const float lv = left.sdf_data[SdfIndex(CHUNK_SIZE_X, y, z)];
            const float rv = right.sdf_data[SdfIndex(0, y, z)];
            EXPECT_FLOAT_EQ(lv, rv) << "X-seam SDF mismatch at y=" << y << " z=" << z
                                    << " (crack between chunk (0,0,0) and (1,0,0))";
        }
    }
}

TEST(WorldgenHardening, SeamSdfAgreesAcrossZBoundary) {
    const TerrainGenParams params = CaveParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk near_z(IVec3(0, -3, 0));
    Chunk far_z(IVec3(0, -3, 1));
    world.GenerateChunkData(near_z);
    world.GenerateChunkData(far_z);

    for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
        for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
            const float nv = near_z.sdf_data[SdfIndex(x, y, CHUNK_SIZE_Z)];
            const float fv = far_z.sdf_data[SdfIndex(x, y, 0)];
            EXPECT_FLOAT_EQ(nv, fv)
                << "Z-seam SDF mismatch at x=" << x << " y=" << y << " (cave layer seam)";
        }
    }
}

TEST(WorldgenHardening, SeamSdfAgreesAcrossYBoundary) {
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk lower(IVec3(0, 0, 0));
    Chunk upper(IVec3(0, 1, 0));
    world.GenerateChunkData(lower);
    world.GenerateChunkData(upper);

    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
            const float lv = lower.sdf_data[SdfIndex(x, CHUNK_SIZE_Y, z)];
            const float uv = upper.sdf_data[SdfIndex(x, 0, z)];
            EXPECT_FLOAT_EQ(lv, uv) << "Y-seam SDF mismatch at x=" << x << " z=" << z;
        }
    }
}

TEST(WorldgenHardening, SeamHeightmapAgreesAcrossXBoundary) {
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk left(IVec3(0, 0, 0));
    Chunk right(IVec3(1, 0, 0));
    world.GenerateChunkData(left);
    world.GenerateChunkData(right);

    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        const float lv = left.heightmap_data[HeightIndex(CHUNK_SIZE_X, z)];
        const float rv = right.heightmap_data[HeightIndex(0, z)];
        EXPECT_FLOAT_EQ(lv, rv) << "X-seam heightmap mismatch at z=" << z;
    }
}

TEST(WorldgenHardening, SeamHeightMatchesGetTerrainHeightAt) {
    // The stored heightmap node and the analytic GetTerrainHeightAt at the same
    // world (x,z) must agree -- otherwise the coarse far-LOD mesher (which reads
    // the analytic height) and the near mesher (heightmap) disagree at the seam.
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk chunk(IVec3(1, 0, 0));
    world.GenerateChunkData(chunk);

    const IVec3 base = chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
            const float stored = chunk.heightmap_data[HeightIndex(x, z)];
            const float analytic = world.GetTerrainHeightAt(static_cast<float>(base.x + x),
                                                            static_cast<float>(base.z + z));
            // BYTE-EXACT (was EXPECT_FLOAT_EQ = 4-ULP). The batched heightmap fill
            // (ComputeShapedHeightGrid / GenUniformGrid2D, SIMD) and the scalar
            // GetTerrainHeightAt (GenSingle2D) must agree to the BIT for either to be a
            // determinism-safe stand-in for the other (e.g. the water init reading the
            // heightmap instead of re-sampling, where lround->mm tips on a sub-ULP diff).
            EXPECT_EQ(stored, analytic)
                << "heightmap node BIT-disagrees with GetTerrainHeightAt at world (" << (base.x + x)
                << "," << (base.z + z) << ")";
        }
    }
}

TEST(WorldgenHardening, SeamMeshVerticesShareBoundaryPositions) {
    // Watertightness probe across the shared plane at world x = CHUNK_SIZE_X.
    //
    // PolygoniseTerrain emits CHUNK-LOCAL vertex positions (the renderer adds the
    // chunk's world translation at draw time -- see GenerateCoarseHeightfieldTerrain
    // and the unit-step path, which both push Vec3(local_x, ...)). So the two
    // chunks express the SAME world plane in DIFFERENT local frames:
    //   * left  Chunk(0,0,0): shared plane is local x = CHUNK_SIZE_X (its MAX-x face).
    //   * right Chunk(1,0,0): shared plane is local x = 0           (its MIN-x face).
    // Both are world x = CHUNK_SIZE_X. Matching the two LOCAL-x=CHUNK_SIZE_X planes
    // (as a naive same-frame compare would) instead compares left's seam against
    // right's OPPOSITE (world x = 2*CHUNK_SIZE_X) face -- a false "hole".
    //
    // At equal LOD a per-chunk Marching Cubes mesher is watertight IFF both chunks
    // sample the identical SDF on the shared lattice plane (left local x=CHUNK_SIZE_X
    // == right local x=0, both world x=CHUNK_SIZE_X): identical corner SDF -> bit-
    // identical edge interpolation -> coincident boundary vertices. This test asserts
    // BOTH guarantees: (1) the shared-plane SDF is identical index-by-index (the
    // root property -- a mismatch is a generation seam bug), and (2) every left
    // max-x-face mesh vertex has a (y,z)-coincident partner on the right min-x face
    // (the geometric consequence -- a missing partner is a real crack).
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk left(IVec3(0, 0, 0));
    Chunk right(IVec3(1, 0, 0));
    GenerateAndMesh(world, left, 1);
    GenerateAndMesh(world, right, 1);

    constexpr float kEps = 1.0e-4f;

    // (1) Root property: the shared lattice plane must be a pure function of world
    // coordinate, i.e. left's max-x SDF column == right's min-x SDF column exactly.
    ASSERT_FALSE(left.sdf_data.empty()) << "left chunk produced no SDF";
    ASSERT_FALSE(right.sdf_data.empty()) << "right chunk produced no SDF";
    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
            const float lv = left.sdf_data[SdfIndex(CHUNK_SIZE_X, y, z)];
            const float rv = right.sdf_data[SdfIndex(0, y, z)];
            EXPECT_FLOAT_EQ(lv, rv) << "shared-plane SDF disagrees at (y=" << y << ", z=" << z
                                    << ") -- generation is not a pure function of world coordinate";
        }
    }

    // (2) Geometric consequence: collect each chunk's vertices on its OWN local
    // face of the shared plane (left max-x, right min-x), then require every left
    // seam vertex to have a (y,z)-matching right seam vertex.
    const float left_face_x = static_cast<float>(CHUNK_SIZE_X); // left local max-x
    const float right_face_x = 0.0f;                            // right local min-x
    auto on_plane = [&](const std::vector<VoxelVertex>& verts, float local_x) {
        std::vector<Vec3> out;
        for (const VoxelVertex& v : verts) {
            if (std::abs(v.position.x - local_x) <= kEps)
                out.push_back(v.position);
        }
        return out;
    };
    const std::vector<Vec3> left_plane = on_plane(left.mesh_vertices, left_face_x);
    const std::vector<Vec3> right_plane = on_plane(right.mesh_vertices, right_face_x);

    // The seam must actually be exercised by this fixture for the test to mean
    // anything.
    ASSERT_FALSE(left_plane.empty()) << "fixture produced no left-chunk seam vertices";
    ASSERT_FALSE(right_plane.empty()) << "fixture produced no right-chunk seam vertices";

    for (const Vec3& lp : left_plane) {
        bool matched = false;
        for (const Vec3& rp : right_plane) {
            if (std::abs(lp.y - rp.y) <= kEps && std::abs(lp.z - rp.z) <= kEps) {
                matched = true;
                break;
            }
        }
        EXPECT_TRUE(matched) << "left seam vertex at (y=" << lp.y << ", z=" << lp.z
                             << ") has no matching right-chunk vertex (watertightness hole)";
    }
}

// =====================================================================================
// LOD CONSISTENCY -- coarse LOD is a deterministic reduction, not a re-roll
// =====================================================================================

TEST(WorldgenHardening, CoarseLodMeshIsDeterministic) {
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);

    Chunk a(IVec3(0, 0, 0));
    Chunk b(IVec3(0, 0, 0));
    GenerateAndMesh(world, a, 4);
    GenerateAndMesh(world, b, 4);

    EXPECT_TRUE(MeshesByteEqual(a, b)) << "step-4 LOD mesh is not reproducible from the same field";
}

TEST(WorldgenHardening, LodMeshFromSharedFieldIsStable) {
    // Polygonise the SAME sdf_data at step 4 twice into independent chunks: the
    // coarse mesh must be a pure function of the field (no per-call state).
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk source(IVec3(0, 0, 0));
    world.GenerateChunkData(source);

    Chunk c1(IVec3(0, 0, 0));
    Chunk c2(IVec3(0, 0, 0));
    c1.sdf_data = source.sdf_data;
    c1.heightmap_data = source.heightmap_data;
    c2.sdf_data = source.sdf_data;
    c2.heightmap_data = source.heightmap_data;
    World::MarchingCubes::PolygoniseTerrain(world, c1, 0.0f, 2);
    World::MarchingCubes::PolygoniseTerrain(world, c2, 0.0f, 2);

    EXPECT_TRUE(MeshesByteEqual(c1, c2));
}

TEST(WorldgenHardening, CoarseLodReducesVertexCount) {
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk fine(IVec3(0, 0, 0));
    Chunk coarse(IVec3(0, 0, 0));
    GenerateAndMesh(world, fine, 1);
    GenerateAndMesh(world, coarse, 4);
    ASSERT_FALSE(fine.mesh_vertices.empty());
    ASSERT_FALSE(coarse.mesh_vertices.empty());
    EXPECT_LT(coarse.mesh_vertices.size(), fine.mesh_vertices.size());
}

TEST(WorldgenHardening, LodMeshIndicesAreInBounds) {
    // T-junction / index-corruption probe: every index must reference a real
    // vertex and every triangle must be non-degenerate at coarse LOD.
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk chunk(IVec3(0, 0, 0));
    GenerateAndMesh(world, chunk, 4);
    ASSERT_EQ(chunk.mesh_indices.size() % 3u, 0u);
    for (u32 idx : chunk.mesh_indices) {
        EXPECT_LT(idx, chunk.mesh_vertices.size()) << "coarse LOD index out of range";
    }
}

// =====================================================================================
// MESHING PURITY -- field -> identical triangle set; degenerate input
// =====================================================================================

TEST(WorldgenHardening, MeshIsByteIdenticalOnRemeshSameField) {
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk chunk(IVec3(0, 0, 0));
    world.GenerateChunkData(chunk);

    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    const std::vector<VoxelVertex> v0 = chunk.mesh_vertices;
    const std::vector<u32> i0 = chunk.mesh_indices;

    // Re-mesh in place: must overwrite to the exact same bytes.
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    ASSERT_EQ(chunk.mesh_vertices.size(), v0.size());
    ASSERT_EQ(chunk.mesh_indices.size(), i0.size());
    for (std::size_t i = 0; i < v0.size(); ++i) {
        EXPECT_TRUE(VerticesEqual(v0[i], chunk.mesh_vertices[i]))
            << "vertex " << i << " drifted on remesh of identical field";
    }
    EXPECT_EQ(i0, chunk.mesh_indices);
}

TEST(WorldgenHardening, FlatSurfaceNormalsAreExactlyUp) {
    // On a perfectly horizontal surface every averaged vertex normal must be
    // (0,1,0). Order-unstable float accumulation drifts the X/Z components.
    SHIELD_WorldSystem world(nullptr, nullptr, FlatParams(8.0f), kSeed);
    Chunk chunk(IVec3(0, 0, 0));
    GenerateAndMesh(world, chunk, 1);
    ASSERT_FALSE(chunk.mesh_vertices.empty());
    for (const VoxelVertex& v : chunk.mesh_vertices) {
        EXPECT_NEAR(v.normal.y, 1.0f, 1.0e-5f);
        EXPECT_NEAR(v.normal.x, 0.0f, 1.0e-5f) << "non-zero X on a flat surface (normal drift)";
        EXPECT_NEAR(v.normal.z, 0.0f, 1.0e-5f) << "non-zero Z on a flat surface (normal drift)";
    }
}

TEST(WorldgenHardening, AllNormalsAreUnitLength) {
    const TerrainGenParams params = CaveParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk chunk(IVec3(0, -3, 0));
    GenerateAndMesh(world, chunk, 1);
    ASSERT_FALSE(chunk.mesh_vertices.empty());
    for (const VoxelVertex& v : chunk.mesh_vertices) {
        const float len =
            std::sqrt(v.normal.x * v.normal.x + v.normal.y * v.normal.y + v.normal.z * v.normal.z);
        EXPECT_TRUE(std::isfinite(len));
        EXPECT_NEAR(len, 1.0f, 1.0e-3f) << "non-unit vertex normal";
    }
}

TEST(WorldgenHardening, AllTrianglesNonDegenerateAndIndexed) {
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk chunk(IVec3(0, 0, 0));
    GenerateAndMesh(world, chunk, 1);
    ASSERT_EQ(chunk.mesh_indices.size() % 3u, 0u);
    for (std::size_t i = 0; i + 2u < chunk.mesh_indices.size(); i += 3u) {
        const u32 a = chunk.mesh_indices[i], b = chunk.mesh_indices[i + 1],
                  c = chunk.mesh_indices[i + 2];
        ASSERT_LT(a, chunk.mesh_vertices.size());
        ASSERT_LT(b, chunk.mesh_vertices.size());
        ASSERT_LT(c, chunk.mesh_vertices.size());
        const Vec3 ea = chunk.mesh_vertices[b].position - chunk.mesh_vertices[a].position;
        const Vec3 eb = chunk.mesh_vertices[c].position - chunk.mesh_vertices[a].position;
        const Vec3 cr(
            ea.y * eb.z - ea.z * eb.y, ea.z * eb.x - ea.x * eb.z, ea.x * eb.y - ea.y * eb.x);
        const float area2 = std::sqrt(cr.x * cr.x + cr.y * cr.y + cr.z * cr.z);
        EXPECT_GT(area2, 1.0e-7f) << "degenerate (zero-area) triangle " << (i / 3);
    }
}

TEST(WorldgenHardening, EmptyAirChunkMeshesToNothing) {
    // DEGENERATE no-op: a chunk entirely above the terrain is all-air -> empty
    // SDF crossing -> empty mesh. No crash, no stray triangle.
    TerrainGenParams params = ArchipelagoParams();
    params.height_offset = -200.0f; // surface far below this chunk
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk chunk(IVec3(0, 5, 0)); // high in the air
    GenerateAndMesh(world, chunk, 1);
    EXPECT_TRUE(chunk.mesh_vertices.empty()) << "all-air chunk produced vertices";
    EXPECT_TRUE(chunk.mesh_indices.empty()) << "all-air chunk produced indices";
}

TEST(WorldgenHardening, AllSolidChunkHasNoInteriorTriangles) {
    // A chunk entirely below the surface with no caves is all-solid. The fine
    // marching-cubes mesher only crosses the iso-surface at the terrain skin,
    // which is far above; so this interior chunk must mesh to nothing (no
    // spurious interior faces).
    TerrainGenParams params = FlatParams(400.0f); // surface in chunk-Y 25
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    Chunk chunk(IVec3(0, 0, 0)); // deep interior, fully solid
    GenerateAndMesh(world, chunk, 1);
    // Every SDF sample must be solid (negative) -- confirm the fixture.
    bool all_solid = true;
    for (float d : chunk.sdf_data) {
        if (d >= 0.0f) {
            all_solid = false;
            break;
        }
    }
    ASSERT_TRUE(all_solid) << "fixture chunk is not fully solid";
    EXPECT_TRUE(chunk.mesh_vertices.empty())
        << "fully-solid interior chunk produced stray triangles";
}

TEST(WorldgenHardening, EmptyChunkRemeshClearsPreviousMesh) {
    // A chunk that previously had a mesh, then is re-generated as all-air, must
    // come back EMPTY (no stale geometry left behind).
    SHIELD_WorldSystem solid_world(nullptr, nullptr, FlatParams(8.0f), kSeed);
    Chunk chunk(IVec3(0, 0, 0));
    GenerateAndMesh(solid_world, chunk, 1);
    ASSERT_FALSE(chunk.mesh_vertices.empty());

    TerrainGenParams air = ArchipelagoParams();
    air.height_offset = -200.0f;
    SHIELD_WorldSystem air_world(nullptr, nullptr, air, kSeed);
    Chunk air_chunk(IVec3(0, 5, 0));
    air_world.GenerateChunkData(air_chunk);
    // Reuse the same chunk object's mesh buffers: copy field over and remesh.
    chunk.sdf_data = air_chunk.sdf_data;
    chunk.heightmap_data = air_chunk.heightmap_data;
    World::MarchingCubes::PolygoniseTerrain(air_world, chunk, 0.0f, 1);
    EXPECT_TRUE(chunk.mesh_vertices.empty()) << "stale mesh survived a re-mesh into empty field";
    EXPECT_TRUE(chunk.mesh_indices.empty());
}

// =====================================================================================
// STRUCTURE / FEATURE placement -- pure (seed, salt, cell); seam consistency
// =====================================================================================

TEST(WorldgenHardening, StructureSiteIsPureFunctionOfCell) {
    const World::StructureTemplatePool pool = MakePool("tower", 0x1234u, 64, 8, 0.5f);
    for (int cz = -3; cz <= 3; ++cz) {
        for (int cx = -3; cx <= 3; ++cx) {
            const auto a = World::SiteInCell(pool, kSeed, cx, cz);
            const auto b = World::SiteInCell(pool, kSeed, cx, cz);
            ASSERT_EQ(a.has_value(), b.has_value());
            if (a) {
                EXPECT_EQ(a->origin, b->origin);
                EXPECT_EQ(a->site_seed, b->site_seed);
            }
        }
    }
}

TEST(WorldgenHardening, StructureSiteOriginStaysInsideItsCell) {
    // CORE invariant SitesInArea / LocateNearestSite rely on: the jittered
    // origin of the site for cell (cx,cz) must remain inside that cell's
    // [cx*spacing, (cx+1)*spacing) span. If the jitter range or separation math
    // pushes the origin past the cell edge, the same physical site is owned by
    // two different cells -- it appears or vanishes depending on which region's
    // scan reaches it first (a seam-straddling duplication / drop).
    const World::StructureTemplatePool pool = MakePool("tower", 0x9E37u, 64, 8, 1.0f);
    for (int cz = -2; cz <= 2; ++cz) {
        for (int cx = -2; cx <= 2; ++cx) {
            const auto site = World::SiteInCell(pool, kSeed, cx, cz);
            ASSERT_TRUE(site.has_value()) << "density 1.0 should always host a site";
            const int lo_x = cx * pool.spacing;
            const int hi_x = (cx + 1) * pool.spacing;
            const int lo_z = cz * pool.spacing;
            const int hi_z = (cz + 1) * pool.spacing;
            EXPECT_GE(site->origin.x, lo_x);
            EXPECT_LT(site->origin.x, hi_x)
                << "site for cell " << cx << " escaped past its X edge to " << site->origin.x;
            EXPECT_GE(site->origin.z, lo_z);
            EXPECT_LT(site->origin.z, hi_z)
                << "site for cell " << cz << " escaped past its Z edge to " << site->origin.z;
        }
    }
}

TEST(WorldgenHardening, StructureSiteStaysInCellWhenSeparationCrowdsSpacing) {
    // Adversarial config: separation large relative to spacing collapses the
    // jitter range. SiteInCell clamps jitter_range to >= 1, so the origin can
    // be placed at separation..separation+jitter, which may exceed the cell
    // when 2*separation approaches spacing. This pins that the placement never
    // leaves the cell even under a crowded config (a real seam-stability risk).
    const World::StructureTemplatePool pool = MakePool("vault", 0x55AAu, 20, 9, 1.0f);
    for (int cx = -2; cx <= 2; ++cx) {
        const auto site = World::SiteInCell(pool, kSeed, cx, 0);
        ASSERT_TRUE(site.has_value());
        const int lo_x = cx * pool.spacing;
        const int hi_x = (cx + 1) * pool.spacing;
        EXPECT_GE(site->origin.x, lo_x);
        EXPECT_LT(site->origin.x, hi_x)
            << "crowded-spacing site escaped cell " << cx << " to x=" << site->origin.x
            << " (jitter range clamp pushes origin past the cell edge)";
    }
}

TEST(WorldgenHardening, SeamStraddlingSiteEnumeratedFromBothRegions) {
    // A site living near a region boundary must be enumerated identically
    // whether the region scanned is the LEFT tile [x0,x1) or the RIGHT tile
    // [x1,x2) that abut at the seam. SitesInArea only KEEPS a site whose origin
    // falls inside the queried window, so the union over two abutting windows
    // must reproduce every site exactly once with identical origins -- no
    // off-by-one drop at the seam, no duplicate.
    const World::StructureTemplatePool pool = MakePool("camp", 0x0BADu, 32, 6, 0.6f);

    const int seam = 0;  // world-x seam between the two tiles
    const int half = 96; // tile half-width (multiple of spacing-ish)
    const int z0 = -half, z1 = half;

    const std::vector<World::StructureSite> left =
        World::SitesInArea(pool, kSeed, seam - half, z0, seam, z1);
    const std::vector<World::StructureSite> right =
        World::SitesInArea(pool, kSeed, seam, z0, seam + half, z1);
    const std::vector<World::StructureSite> whole =
        World::SitesInArea(pool, kSeed, seam - half, z0, seam + half, z1);

    // Build comparable origin sets.
    auto key = [](const World::StructureSite& s) {
        return std::make_tuple(s.origin.x, s.origin.z, s.site_seed);
    };
    std::map<std::tuple<int, int, u64>, int> union_count;
    for (const auto& s : left)
        ++union_count[key(s)];
    for (const auto& s : right)
        ++union_count[key(s)];

    std::map<std::tuple<int, int, u64>, int> whole_count;
    for (const auto& s : whole)
        ++whole_count[key(s)];

    // No site may be produced by BOTH halves (the seam is exclusive on one
    // side), and the union of the two halves must exactly equal the whole-tile
    // enumeration -- every seam-straddling site placed consistently from both
    // sides.
    for (const auto& [k, n] : union_count) {
        EXPECT_EQ(n, 1) << "site at x=" << std::get<0>(k) << " z=" << std::get<1>(k)
                        << " enumerated by both abutting regions (seam double-count)";
    }
    EXPECT_EQ(union_count.size(), whole_count.size())
        << "split-region enumeration drops or adds sites vs the whole-tile scan (seam off-by-one)";
    for (const auto& [k, n] : whole_count) {
        EXPECT_EQ(union_count[k], n)
            << "site at x=" << std::get<0>(k) << " z=" << std::get<1>(k)
            << " missing from the split-region union (seam-straddling drop)";
    }
}

TEST(WorldgenHardening, SitesInAreaIncludesSiteWhoseJitterReachesIntoWindow) {
    // Off-by-one hunt: SitesInArea derives its cell scan from
    // FloorDiv(min_x) .. FloorDiv(max_x-1). A site jittered toward the high edge
    // of a cell just LEFT of the window can land at an x >= min_x. If the scan
    // never visits that left cell, the site is dropped even though its origin is
    // inside the window. Compare against a brute-force scan over a wide cell
    // band that keeps only origins inside the window.
    const World::StructureTemplatePool pool = MakePool("ruin", 0xFEEDu, 24, 2, 1.0f);
    const int min_x = 10, max_x = 130, min_z = -50, max_z = 50;

    const std::vector<World::StructureSite> got =
        World::SitesInArea(pool, kSeed, min_x, min_z, max_x, max_z);

    // Brute force: scan a generous band of cells and keep every site inside the
    // window.
    std::map<std::tuple<int, int>, int> brute;
    for (int cz = -10; cz <= 10; ++cz) {
        for (int cx = -5; cx <= 15; ++cx) {
            const auto s = World::SiteInCell(pool, kSeed, cx, cz);
            if (s && s->origin.x >= min_x && s->origin.x < max_x && s->origin.z >= min_z &&
                s->origin.z < max_z) {
                ++brute[std::make_tuple(s->origin.x, s->origin.z)];
            }
        }
    }
    std::map<std::tuple<int, int>, int> got_set;
    for (const auto& s : got)
        ++got_set[std::make_tuple(s.origin.x, s.origin.z)];

    EXPECT_EQ(got_set.size(), brute.size())
        << "SitesInArea cell-scan range misses sites whose jitter reaches into the window";
    for (const auto& [k, n] : brute) {
        EXPECT_EQ(got_set[k], n) << "site at x=" << std::get<0>(k) << " z=" << std::get<1>(k)
                                 << " inside the window but not enumerated (cell-scan off-by-one)";
    }
}

TEST(WorldgenHardening, LocateNearestSiteIsDeterministicAndSymmetric) {
    const World::StructureTemplatePool pool = MakePool("tower", 0x1234u, 48, 6, 0.7f);
    for (int x = -100; x <= 100; x += 37) {
        for (int z = -100; z <= 100; z += 41) {
            const auto a = World::LocateNearestSite(pool, kSeed, x, z, 2);
            const auto b = World::LocateNearestSite(pool, kSeed, x, z, 2);
            ASSERT_EQ(a.has_value(), b.has_value());
            if (a) {
                EXPECT_EQ(a->origin, b->origin);
                EXPECT_EQ(a->site_seed, b->site_seed);
            }
        }
    }
}

TEST(WorldgenHardening, AssembledStructureIsPureFunctionOfSiteSeed) {
    // The jigsaw assembly must be byte-identical for the same site, regardless
    // of how/when it is queried (the same physical structure is generated from
    // every chunk that overlaps it).
    const World::StructureTemplatePool pool = MakePool("tower", 0x1234u, 64, 8, 1.0f);
    const auto site = World::SiteInCell(pool, kSeed, 1, 1);
    ASSERT_TRUE(site.has_value());

    const std::vector<World::StructureVoxel> v1 = World::AssembleStructure(pool, *site);
    const std::vector<World::StructureVoxel> v2 = World::AssembleStructure(pool, *site);
    ASSERT_EQ(v1.size(), v2.size());
    for (std::size_t i = 0; i < v1.size(); ++i) {
        EXPECT_EQ(v1[i].position, v2[i].position);
        EXPECT_EQ(v1[i].material, v2[i].material);
    }
    EXPECT_EQ(World::ComputeAssembledVoxelHash(v1), World::ComputeAssembledVoxelHash(v2));
}

TEST(WorldgenHardening, SeamStraddlingAssemblyAgreesFromBothSides) {
    // A structure whose voxels cross a chunk seam must stamp the SAME voxels
    // into the shared boundary regardless of which side queries it. Locate the
    // SAME site from two query points on opposite sides of the structure and
    // assert the assembled voxel sets are identical (the voxels that land in
    // the neighbouring chunk are placed consistently).
    const World::StructureTemplatePool pool = MakePool("tower", 0x1234u, 64, 8, 1.0f);
    const auto site = World::SiteInCell(pool, kSeed, 0, 0);
    ASSERT_TRUE(site.has_value());

    // Query the nearest site from a point just LEFT and just RIGHT of the site
    // origin; both must resolve to the same site, and assembly must match.
    const auto from_left =
        World::LocateNearestSite(pool, kSeed, site->origin.x - 4, site->origin.z, 2);
    const auto from_right =
        World::LocateNearestSite(pool, kSeed, site->origin.x + 4, site->origin.z, 2);
    ASSERT_TRUE(from_left.has_value());
    ASSERT_TRUE(from_right.has_value());
    EXPECT_EQ(from_left->origin, from_right->origin)
        << "the same structure resolves to different sites from the two sides of the seam";
    EXPECT_EQ(from_left->site_seed, from_right->site_seed);

    const u64 hl = World::ComputeAssembledVoxelHash(World::AssembleStructure(pool, *from_left));
    const u64 hr = World::ComputeAssembledVoxelHash(World::AssembleStructure(pool, *from_right));
    EXPECT_EQ(hl, hr) << "seam-straddling structure assembles differently from the two sides";
}

TEST(WorldgenHardening, AssembledVoxelHashIsOrderIndependent) {
    // ComputeAssembledVoxelHash canonicalizes by sorting -- emission order must
    // not change the hash. Reverse the emission and confirm the hash holds.
    const World::StructureTemplatePool pool = MakePool("tower", 0x1234u, 64, 8, 1.0f);
    const auto site = World::SiteInCell(pool, kSeed, 2, 0);
    ASSERT_TRUE(site.has_value());
    std::vector<World::StructureVoxel> voxels = World::AssembleStructure(pool, *site);
    ASSERT_FALSE(voxels.empty());
    std::vector<World::StructureVoxel> reversed(voxels.rbegin(), voxels.rend());
    EXPECT_EQ(World::ComputeAssembledVoxelHash(voxels), World::ComputeAssembledVoxelHash(reversed))
        << "assembled voxel hash depends on emission order (not canonicalized)";
}

TEST(WorldgenHardening, StructureDensityZeroPlacesNothing) {
    // DEGENERATE feature density: a pool with density 0 must place no sites
    // anywhere (no off-by-one that spawns a site at the boundary roll).
    const World::StructureTemplatePool pool = MakePool("none", 0x0001u, 32, 4, 0.0f);
    for (int cz = -4; cz <= 4; ++cz) {
        for (int cx = -4; cx <= 4; ++cx) {
            EXPECT_FALSE(World::SiteInCell(pool, kSeed, cx, cz).has_value())
                << "density 0 still spawned a site at cell (" << cx << "," << cz << ")";
        }
    }
}

TEST(WorldgenHardening, StructureSaltSeparatesTypes) {
    // Two pools with different salt must not produce identical site streams
    // (salt actually participates in the cell seed).
    const World::StructureTemplatePool a = MakePool("a", 0x0001u, 64, 8, 0.5f);
    const World::StructureTemplatePool b = MakePool("b", 0x0002u, 64, 8, 0.5f);
    int differences = 0;
    for (int cz = -5; cz <= 5; ++cz) {
        for (int cx = -5; cx <= 5; ++cx) {
            const auto sa = World::SiteInCell(a, kSeed, cx, cz);
            const auto sb = World::SiteInCell(b, kSeed, cx, cz);
            if (sa.has_value() != sb.has_value()) {
                ++differences;
                continue;
            }
            if (sa && sb && sa->origin != sb->origin)
                ++differences;
        }
    }
    EXPECT_GT(differences, 0) << "type salt does not separate placement streams";
}

TEST(WorldgenHardening, DifferentWorldSeedMovesSites) {
    const World::StructureTemplatePool pool = MakePool("tower", 0x1234u, 64, 8, 0.5f);
    int differences = 0;
    for (int cz = -5; cz <= 5; ++cz) {
        for (int cx = -5; cx <= 5; ++cx) {
            const auto a = World::SiteInCell(pool, kSeed, cx, cz);
            const auto b = World::SiteInCell(pool, kSeed + 1, cx, cz);
            if (a.has_value() != b.has_value()) {
                ++differences;
                continue;
            }
            if (a && b && (a->origin != b->origin || a->site_seed != b->site_seed))
                ++differences;
        }
    }
    EXPECT_GT(differences, 0) << "world seed has no effect on structure placement";
}

// =====================================================================================
// BIOME / RIVER boundary determinism (when the systems are off, the contract is
// still that the query is a stable pure function -- no crash, no drift).
// =====================================================================================

TEST(WorldgenHardening, BiomeIdAtIsStableAtExactBoundaryCoord) {
    // BiomeIdAt at an exact integer boundary coordinate must be deterministic
    // (no NaN-driven branch, no per-call drift) even with biomes disabled.
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    const float bx = static_cast<float>(CHUNK_SIZE_X); // exact chunk seam x
    const u8 a = world.BiomeIdAt(bx, 0.0f);
    const u8 b = world.BiomeIdAt(bx, 0.0f);
    EXPECT_EQ(a, b);
}

TEST(WorldgenHardening, RiverInfluenceIsStableAndBounded) {
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    for (float x = -32.0f; x <= 32.0f; x += 8.0f) {
        const float a = world.RiverInfluenceAt(x, 0.0f);
        const float b = world.RiverInfluenceAt(x, 0.0f);
        EXPECT_FLOAT_EQ(a, b) << "river influence drifts at x=" << x;
        EXPECT_GE(a, 0.0f);
        EXPECT_LE(a, 1.0f) << "river influence escaped [0,1] at x=" << x;
    }
}

TEST(WorldgenHardening, CoarseHeightMatchesFineHeightForStepOne) {
    // GetTerrainHeightAtCoarse with sample_step 1 must be byte-identical to the
    // full-resolution GetTerrainHeightAt (documented contract; a divergence is a
    // far-LOD seam crack).
    const TerrainGenParams params = ArchipelagoParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    for (float x = -40.0f; x <= 40.0f; x += 9.0f) {
        for (float z = -40.0f; z <= 40.0f; z += 11.0f) {
            EXPECT_FLOAT_EQ(world.GetTerrainHeightAt(x, z), world.GetTerrainHeightAtCoarse(x, z, 1))
                << "coarse step-1 height diverges from fine height at (" << x << "," << z << ")";
        }
    }
}
