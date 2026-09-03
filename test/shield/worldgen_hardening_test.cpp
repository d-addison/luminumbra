// =====================================================================================
// worldgen_hardening_test.cpp
//
// ADVERSARIAL determinism-hardening suite for WORLD GENERATION + VOXEL MESHING.
//
// The happy-path sibling (test_world_generation.cpp) proves "a chunk gets generated
// and meshes". This suite hunts the bugs that ride along underneath that:
//   - SEED PURITY: same (seed,coord) -> byte-identical field; order independence
//     (no hidden static/global state bleeding between chunks).
//   - CHUNK-SEAM CONTINUITY: the density/height field MUST agree sample-for-sample
//     across a shared chunk boundary (a mismatch = a visible crack / seam).
//   - LOD CONSISTENCY: a coarse LOD is a deterministic reduction of the fine field,
//     not a re-roll.
//   - BIOME / RIVER boundaries: deterministic + stable at an exact boundary coord;
//     a river straddling a seam is carved consistently from both sides.
//   - MESHING PURITY: same field -> same triangle set (positions/normals/winding/
//     index order), order-stable float accumulation; degenerate input -> no-op.
//
// DETERMINISM CONTRACT: generation is a PURE function of (seed, region/chunk coord).
// run==replay is asserted BYTE/FLOAT EXACT (EXPECT_EQ on flattened field vectors,
// EXPECT_FLOAT_EQ on individual floats). Deterministic only -- no wall-clock, no
// std::random.
//
// Style, includes, namespace usage, and world/mesher CONSTRUCTION are copied from
// the compiling sibling test/shield/test_world_generation.cpp.
// =====================================================================================

#include "gtest/gtest.h"
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "FastNoise/FastNoise.h"
#include "core/JobSystem.h"
#include "entt/entt.hpp"
#include "world/Chunk.h"

#include "systems/SHIELD_WorldSystem.h"

#include "systems/WaterSystem.h"
#include "world/MarchingCubes.h"

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

// SDF / heightmap padded dimension (matches GenerateChunkData: CHUNK_SIZE + 1).
constexpr int kPad = 1;
constexpr int kSizeX = CHUNK_SIZE_X + kPad; // 17
constexpr int kSizeY = CHUNK_SIZE_Y + kPad; // 17
constexpr int kSizeZ = CHUNK_SIZE_Z + kPad; // 17

// SDF linear index, matching GenerateChunkData's layout exactly:
//   idx = x + y*size_x + z*size_x*size_y
inline size_t SdfIdx(int x, int y, int z) {
    return static_cast<size_t>(x) + static_cast<size_t>(y) * kSizeX +
           static_cast<size_t>(z) * kSizeX * kSizeY;
}

// Heightmap linear index: idx = x + z*size_x
inline size_t HmIdx(int x, int z) {
    return static_cast<size_t>(x) + static_cast<size_t>(z) * kSizeX;
}

} // namespace

// =====================================================================================
// FIXTURE -- mirrors test_world_generation.cpp's presets so construction matches.
// =====================================================================================
class WorldgenHardeningTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Guaranteed flat solid surface at y=8 (predictable geometry).
        params_flat.base_amplitude = 0.0f;
        params_flat.height_offset = 8.0f;
        params_flat.caves_enabled = false;

        // Complex-but-in-bounds terrain (real fields, no caves, no island mask).
        params_terrain.base_frequency = 0.004f;
        params_terrain.base_amplitude = 8.0f;
        params_terrain.octaves = 6;
        params_terrain.persistence = 0.5f;
        params_terrain.lacunarity = 2.2f;
        params_terrain.height_offset = 8.0f;
        params_terrain.island_mask_enabled = false;
        params_terrain.caves_enabled = false;

        // Same as terrain but with caves so we exercise the 3D cave grid.
        params_caved = params_terrain;
        params_caved.height_offset = 40.0f;
        params_caved.caves_enabled = true;
        params_caved.cave_threshold = 0.55f;
        params_caved.cave_frequency = 0.15f;
        params_caved.cave_carve_value = 4.0f;

        // Rivers enabled. Rivers need ONLY the +10 river noise generator (built by
        // reinitialize_noise when rivers_enabled) -- no biome table / data file.
        params_rivers = params_terrain;
        params_rivers.rivers_enabled = true;
        params_rivers.river_frequency = 0.0016f;
        params_rivers.river_pv_min = -1.0f;
        params_rivers.river_pv_max = -0.85f;
        params_rivers.river_depth = 8.0f;
        params_rivers.river_max_carve = 60.0f;
    }

    TerrainGenParams params_flat;
    TerrainGenParams params_terrain;
    TerrainGenParams params_caved;
    TerrainGenParams params_rivers;

    static constexpr int kSeed = 1337;
};

// =====================================================================================
// 1. SEED PURITY -- same (seed,coord) -> byte/float-identical field.
// =====================================================================================

// Two independent world systems with the same seed/params must produce the exact
// same SDF + heightmap bytes for the same chunk coord (no construction-order state).
TEST_F(WorldgenHardeningTest, SeedPurity_TwoWorldsSameChunkByteIdentical) {
    SHIELD_WorldSystem world_a(nullptr, nullptr, params_terrain, kSeed);
    SHIELD_WorldSystem world_b(nullptr, nullptr, params_terrain, kSeed);
    Chunk a({3, 1, -2});
    Chunk b({3, 1, -2});
    world_a.GenerateChunkData(a);
    world_b.GenerateChunkData(b);
    ASSERT_EQ(a.sdf_data.size(), b.sdf_data.size());
    EXPECT_EQ(a.sdf_data, b.sdf_data) << "SDF field drifted between identically-seeded worlds";
    ASSERT_EQ(a.heightmap_data.size(), b.heightmap_data.size());
    EXPECT_EQ(a.heightmap_data, b.heightmap_data)
        << "heightmap drifted between identically-seeded worlds";
}

// Re-generating the SAME chunk object's coord through the SAME world twice must be
// byte-identical (no hidden per-call state / RNG advance).
TEST_F(WorldgenHardeningTest, SeedPurity_RegenSameWorldByteIdentical) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk first({0, 0, 0});
    Chunk second({0, 0, 0});
    world.GenerateChunkData(first);
    world.GenerateChunkData(second);
    EXPECT_EQ(first.sdf_data, second.sdf_data);
    EXPECT_EQ(first.heightmap_data, second.heightmap_data);
}

// Re-generating into the SAME chunk object (in place) must overwrite to the same
// bytes -- a generator that appended / accumulated would diverge.
TEST_F(WorldgenHardeningTest, SeedPurity_RegenInPlaceStable) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_caved, kSeed);
    Chunk chunk({1, 2, 1});
    world.GenerateChunkData(chunk);
    std::vector<f32> first_sdf = chunk.sdf_data;
    std::vector<f32> first_hm = chunk.heightmap_data;
    world.GenerateChunkData(chunk);
    EXPECT_EQ(chunk.sdf_data, first_sdf);
    EXPECT_EQ(chunk.heightmap_data, first_hm);
}

// ORDER INDEPENDENCE: chunk A generated before B vs after B must be identical.
// A different result reveals hidden global/static state bleeding between chunks.
TEST_F(WorldgenHardeningTest, SeedPurity_GenerationOrderIndependent) {
    const IVec3 coord_a{2, 0, 5};
    const IVec3 coord_b{-7, 1, 3};

    // Order 1: A then B.
    SHIELD_WorldSystem world1(nullptr, nullptr, params_caved, kSeed);
    Chunk a1(coord_a);
    Chunk b1(coord_b);
    world1.GenerateChunkData(a1);
    world1.GenerateChunkData(b1);

    // Order 2: B then A (same world instance, reversed order).
    SHIELD_WorldSystem world2(nullptr, nullptr, params_caved, kSeed);
    Chunk b2(coord_b);
    Chunk a2(coord_a);
    world2.GenerateChunkData(b2);
    world2.GenerateChunkData(a2);

    EXPECT_EQ(a1.sdf_data, a2.sdf_data) << "chunk A field depends on neighbour generation order";
    EXPECT_EQ(b1.sdf_data, b2.sdf_data) << "chunk B field depends on neighbour generation order";
    EXPECT_EQ(a1.heightmap_data, a2.heightmap_data);
    EXPECT_EQ(b1.heightmap_data, b2.heightmap_data);
}

// Generating a busy neighbourhood between two regenerations of the same chunk must
// not perturb that chunk (interleaved-order purity).
TEST_F(WorldgenHardeningTest, SeedPurity_InterleavedNeighboursDoNotPerturb) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    const IVec3 target{0, 0, 0};

    Chunk baseline(target);
    world.GenerateChunkData(baseline);

    for (int dx = -1; dx <= 1; ++dx) {
        for (int dz = -1; dz <= 1; ++dz) {
            Chunk neighbour({dx * 4, 0, dz * 4});
            world.GenerateChunkData(neighbour);
        }
    }

    Chunk after(target);
    world.GenerateChunkData(after);
    EXPECT_EQ(baseline.sdf_data, after.sdf_data)
        << "target chunk perturbed by neighbour generation";
    EXPECT_EQ(baseline.heightmap_data, after.heightmap_data);
}

// set_seed/set_params re-init must be reversible: switching to another seed and back
// reproduces the original field exactly (no leaked noise state).
TEST_F(WorldgenHardeningTest, SeedPurity_SeedSwitchIsReversible) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk original({1, 0, 1});
    world.GenerateChunkData(original);

    world.set_seed(99999);
    Chunk other({1, 0, 1});
    world.GenerateChunkData(other);
    // Different seed should generally change the field (sanity; not the assertion).

    world.set_seed(kSeed);
    Chunk restored({1, 0, 1});
    world.GenerateChunkData(restored);
    EXPECT_EQ(original.sdf_data, restored.sdf_data)
        << "seed switch+restore did not reproduce original field";
    EXPECT_EQ(original.heightmap_data, restored.heightmap_data);
}

// A different seed must actually change something (purity must not collapse to a
// constant -- otherwise the "determinism" is trivially-but-uselessly satisfied).
TEST_F(WorldgenHardeningTest, SeedPurity_DifferentSeedDiffersOnRealTerrain) {
    SHIELD_WorldSystem world_a(nullptr, nullptr, params_terrain, 1);
    SHIELD_WorldSystem world_b(nullptr, nullptr, params_terrain, 2);
    Chunk a({0, 0, 0});
    Chunk b({0, 0, 0});
    world_a.GenerateChunkData(a);
    world_b.GenerateChunkData(b);
    ASSERT_EQ(a.heightmap_data.size(), b.heightmap_data.size());
    EXPECT_NE(a.heightmap_data, b.heightmap_data) << "seed has no effect on the terrain field";
}

// =====================================================================================
// 2. ANALYTIC HEIGHT PURITY -- GetTerrainHeightAt is the seam authority.
// =====================================================================================

// GetTerrainHeightAt must be a pure function: same (x,z) -> exactly the same float.
TEST_F(WorldgenHardeningTest, HeightPurity_AnalyticHeightFloatExact) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    for (float x = -40.0f; x <= 40.0f; x += 7.0f) {
        for (float z = -40.0f; z <= 40.0f; z += 7.0f) {
            float h1 = world.GetTerrainHeightAt(x, z);
            float h2 = world.GetTerrainHeightAt(x, z);
            EXPECT_FLOAT_EQ(h1, h2) << "GetTerrainHeightAt not pure at (" << x << "," << z << ")";
        }
    }
}

// get_density_at must be a pure function of world position (float-exact on replay).
TEST_F(WorldgenHardeningTest, HeightPurity_DensityFloatExact) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_caved, kSeed);
    for (float y = -16.0f; y <= 60.0f; y += 11.0f) {
        Vec3 p{12.0f, y, -5.0f};
        EXPECT_FLOAT_EQ(world.get_density_at(p), world.get_density_at(p));
    }
}

// The heightmap stored in a chunk must match the analytic GetTerrainHeightAt at the
// same world column (single source of truth; a mismatch means the cached field and
// the collision/streaming queries disagree -> creatures float / sink).
TEST_F(WorldgenHardeningTest, HeightPurity_ChunkHeightmapMatchesAnalytic) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    const IVec3 coord{2, 0, -3};
    Chunk chunk(coord);
    world.GenerateChunkData(chunk);
    const IVec3 base = coord * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    for (int z = 0; z < kSizeZ; ++z) {
        for (int x = 0; x < kSizeX; ++x) {
            float wx = static_cast<float>(base.x + x);
            float wz = static_cast<float>(base.z + z);
            float analytic = world.GetTerrainHeightAt(wx, wz);
            float stored = chunk.heightmap_data[HmIdx(x, z)];
            EXPECT_FLOAT_EQ(stored, analytic)
                << "stored heightmap != analytic at world (" << wx << "," << wz << ")";
        }
    }
}

// =====================================================================================
// 3. CHUNK-SEAM CONTINUITY -- adjacent chunks must agree on the shared boundary.
// =====================================================================================

// The shared X boundary: chunk (cx)'s x=CHUNK_SIZE_X column == chunk (cx+1)'s x=0
// column, sample-for-sample over the whole boundary plane. A mismatch is a visible
// crack / seam in the meshed terrain.
TEST_F(WorldgenHardeningTest, Seam_SharedXBoundaryPlaneMatches) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk left({0, 0, 0});
    Chunk right({1, 0, 0});
    world.GenerateChunkData(left);
    world.GenerateChunkData(right);

    for (int z = 0; z < kSizeZ; ++z) {
        for (int y = 0; y < kSizeY; ++y) {
            float lv = left.sdf_data[SdfIdx(CHUNK_SIZE_X, y, z)];
            float rv = right.sdf_data[SdfIdx(0, y, z)];
            EXPECT_FLOAT_EQ(lv, rv) << "X-seam mismatch at boundary y=" << y << " z=" << z;
        }
    }
}

// The shared Z boundary likewise: chunk(cz)'s z=CHUNK_SIZE_Z == chunk(cz+1)'s z=0.
TEST_F(WorldgenHardeningTest, Seam_SharedZBoundaryPlaneMatches) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk near({0, 0, 0});
    Chunk far({0, 0, 1});
    world.GenerateChunkData(near);
    world.GenerateChunkData(far);

    for (int x = 0; x < kSizeX; ++x) {
        for (int y = 0; y < kSizeY; ++y) {
            float nv = near.sdf_data[SdfIdx(x, y, CHUNK_SIZE_Z)];
            float fv = far.sdf_data[SdfIdx(x, y, 0)];
            EXPECT_FLOAT_EQ(nv, fv) << "Z-seam mismatch at boundary x=" << x << " y=" << y;
        }
    }
}

// The vertical Y boundary: chunk(cy)'s y=CHUNK_SIZE_Y == chunk(cy+1)'s y=0.
TEST_F(WorldgenHardeningTest, Seam_SharedYBoundaryPlaneMatches) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_caved, kSeed);
    Chunk lower({0, 0, 0});
    Chunk upper({0, 1, 0});
    world.GenerateChunkData(lower);
    world.GenerateChunkData(upper);

    for (int z = 0; z < kSizeZ; ++z) {
        for (int x = 0; x < kSizeX; ++x) {
            float lv = lower.sdf_data[SdfIdx(x, CHUNK_SIZE_Y, z)];
            float uv = upper.sdf_data[SdfIdx(x, 0, z)];
            EXPECT_FLOAT_EQ(lv, uv) << "Y-seam mismatch at boundary x=" << x << " z=" << z;
        }
    }
}

// Heightmap seam continuity across X: the shared column heights must match.
TEST_F(WorldgenHardeningTest, Seam_HeightmapXBoundaryMatches) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk left({0, 0, 0});
    Chunk right({1, 0, 0});
    world.GenerateChunkData(left);
    world.GenerateChunkData(right);
    for (int z = 0; z < kSizeZ; ++z) {
        EXPECT_FLOAT_EQ(left.heightmap_data[HmIdx(CHUNK_SIZE_X, z)],
                        right.heightmap_data[HmIdx(0, z)])
            << "heightmap X-seam mismatch at z=" << z;
    }
}

// Diagonal corner seam: 4 chunks meeting at a corner must all agree on the shared
// vertical edge column (the worst case for off-by-one seam bugs).
TEST_F(WorldgenHardeningTest, Seam_FourChunkCornerColumnAgrees) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk c00({0, 0, 0});
    Chunk c10({1, 0, 0});
    Chunk c01({0, 0, 1});
    Chunk c11({1, 0, 1});
    world.GenerateChunkData(c00);
    world.GenerateChunkData(c10);
    world.GenerateChunkData(c01);
    world.GenerateChunkData(c11);

    for (int y = 0; y < kSizeY; ++y) {
        float v00 = c00.sdf_data[SdfIdx(CHUNK_SIZE_X, y, CHUNK_SIZE_Z)];
        float v10 = c10.sdf_data[SdfIdx(0, y, CHUNK_SIZE_Z)];
        float v01 = c01.sdf_data[SdfIdx(CHUNK_SIZE_X, y, 0)];
        float v11 = c11.sdf_data[SdfIdx(0, y, 0)];
        EXPECT_FLOAT_EQ(v00, v10) << "corner column 00/10 mismatch at y=" << y;
        EXPECT_FLOAT_EQ(v00, v01) << "corner column 00/01 mismatch at y=" << y;
        EXPECT_FLOAT_EQ(v00, v11) << "corner column 00/11 mismatch at y=" << y;
    }
}

// Off-by-one guard: the boundary plane is the LAST voxel column of one chunk and
// the FIRST of the next. The analytic height at that exact world coordinate must
// equal both chunks' stored values at that index (no off-by-one in the base offset).
TEST_F(WorldgenHardeningTest, Seam_FirstLastVoxelColumnNoOffByOne) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk left({0, 0, 0});
    Chunk right({1, 0, 0});
    world.GenerateChunkData(left);
    world.GenerateChunkData(right);
    // World x of the shared boundary plane = 16.
    const float boundary_wx = static_cast<float>(CHUNK_SIZE_X);
    for (int z = 0; z < kSizeZ; ++z) {
        float analytic = world.GetTerrainHeightAt(boundary_wx, static_cast<float>(z));
        EXPECT_FLOAT_EQ(left.heightmap_data[HmIdx(CHUNK_SIZE_X, z)], analytic)
            << "left chunk's last column != analytic boundary height at z=" << z;
        EXPECT_FLOAT_EQ(right.heightmap_data[HmIdx(0, z)], analytic)
            << "right chunk's first column != analytic boundary height at z=" << z;
    }
}

// Negative-coordinate seam: same continuity must hold across x=0 (chunk -1 vs 0),
// where integer truncation / floor bugs in coord->world math typically surface.
TEST_F(WorldgenHardeningTest, Seam_NegativeCoordBoundaryMatches) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk left({-1, 0, 0});
    Chunk right({0, 0, 0});
    world.GenerateChunkData(left);
    world.GenerateChunkData(right);
    for (int z = 0; z < kSizeZ; ++z) {
        for (int y = 0; y < kSizeY; ++y) {
            EXPECT_FLOAT_EQ(left.sdf_data[SdfIdx(CHUNK_SIZE_X, y, z)],
                            right.sdf_data[SdfIdx(0, y, z)])
                << "negative-coord X-seam mismatch at y=" << y << " z=" << z;
        }
    }
}

// =====================================================================================
// 4. RIVER / CARVE SEAM CONTINUITY + boundary stability.
// =====================================================================================

// RiverInfluenceAt must be a pure function (float-exact on replay).
TEST_F(WorldgenHardeningTest, River_InfluencePure) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_rivers, kSeed);
    for (float x = -200.0f; x <= 200.0f; x += 37.0f) {
        for (float z = -200.0f; z <= 200.0f; z += 37.0f) {
            EXPECT_FLOAT_EQ(world.RiverInfluenceAt(x, z), world.RiverInfluenceAt(x, z));
        }
    }
}

// A carved river straddling a chunk seam must be carved IDENTICALLY from both sides:
// the carved (final) terrain height at the shared boundary world coordinate must
// match whether sampled as one chunk's last column or the next chunk's first.
TEST_F(WorldgenHardeningTest, River_CarvedHeightSeamContinuous) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_rivers, kSeed);
    Chunk left({0, 0, 0});
    Chunk right({1, 0, 0});
    world.GenerateChunkData(left);
    world.GenerateChunkData(right);
    for (int z = 0; z < kSizeZ; ++z) {
        // Carved height lives in the heightmap (carve is inside the shared height helper).
        EXPECT_FLOAT_EQ(left.heightmap_data[HmIdx(CHUNK_SIZE_X, z)],
                        right.heightmap_data[HmIdx(0, z)])
            << "river-carved heightmap seam mismatch at z=" << z;
    }
}

// Rivers-on must actually carve SOMEWHERE relative to rivers-off (the carve path is
// real, not a no-op) -- otherwise the seam test above passes vacuously.
TEST_F(WorldgenHardeningTest, River_CarveChangesTerrainSomewhere) {
    SHIELD_WorldSystem world_off(nullptr, nullptr, params_terrain, kSeed);
    SHIELD_WorldSystem world_on(nullptr, nullptr, params_rivers, kSeed);
    bool any_carve = false;
    int carve_checks = 0;
    for (float x = -1024.0f; x <= 1024.0f; x += 4.0f) {
        for (float z = -1024.0f; z <= 1024.0f; z += 4.0f) {
            float influence = world_on.RiverInfluenceAt(x, z);
            if (influence > 0.0f)
                any_carve = true;
            // Where the river carve is strong, the carved terrain must be no higher
            // than the un-carved terrain (the channel lowers, never raises, ground).
            if (influence > 0.5f) {
                ++carve_checks;
                EXPECT_LE(world_on.GetTerrainHeightAt(x, z),
                          world_off.GetTerrainHeightAt(x, z) + 1e-3f)
                    << "river channel center did NOT lower terrain at (" << x << "," << z << ")";
            }
        }
    }
    ASSERT_TRUE(any_carve) << "no river influence found anywhere -- carve path is dead";
    EXPECT_GT(carve_checks, 0) << "no strong river samples exercised the carve-height assertion";
}

// Boundary stability: river influence sampled AT an exact band edge must not flicker
// (sampling the identical coordinate twice must give the identical value -- no
// nondeterministic branch on the >= / > comparison).
TEST_F(WorldgenHardeningTest, River_BandEdgeDeterministic) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_rivers, kSeed);
    for (int i = -100; i <= 100; ++i) {
        float x = static_cast<float>(i) * 6.25f; // hit a spread of band positions
        float z = 0.0f;
        float a = world.RiverInfluenceAt(x, z);
        float b = world.RiverInfluenceAt(x, z);
        EXPECT_FLOAT_EQ(a, b);
        EXPECT_GE(a, 0.0f);
        EXPECT_LE(a, 1.0f);
    }
}

// Rivers must not break seed purity: two same-seed river worlds agree byte-for-byte.
TEST_F(WorldgenHardeningTest, River_FieldDeterministicAcrossWorlds) {
    SHIELD_WorldSystem world_a(nullptr, nullptr, params_rivers, 2024);
    SHIELD_WorldSystem world_b(nullptr, nullptr, params_rivers, 2024);
    Chunk a({4, 0, 4});
    Chunk b({4, 0, 4});
    world_a.GenerateChunkData(a);
    world_b.GenerateChunkData(b);
    EXPECT_EQ(a.sdf_data, b.sdf_data);
    EXPECT_EQ(a.heightmap_data, b.heightmap_data);
}

// =====================================================================================
// 5. CAVE determinism + surface cap (off-by-one near the surface).
// =====================================================================================

// Cave carving must be deterministic: same seed/params -> identical carved field.
TEST_F(WorldgenHardeningTest, Cave_FieldDeterministic) {
    SHIELD_WorldSystem world_a(nullptr, nullptr, params_caved, 7);
    SHIELD_WorldSystem world_b(nullptr, nullptr, params_caved, 7);
    Chunk a({0, 0, 0});
    Chunk b({0, 0, 0});
    world_a.GenerateChunkData(a);
    world_b.GenerateChunkData(b);
    EXPECT_EQ(a.sdf_data, b.sdf_data) << "cave-carved field is not deterministic";
}

// Cave-carved SDF seam: the 3D cave noise grid must also agree across a chunk
// boundary (the cave grid is sampled per-chunk; an off-by-one in the base offset
// would open a crack only when caves are on).
TEST_F(WorldgenHardeningTest, Cave_SeamContinuousAcrossXBoundary) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_caved, kSeed);
    Chunk left({0, 0, 0});
    Chunk right({1, 0, 0});
    world.GenerateChunkData(left);
    world.GenerateChunkData(right);
    for (int z = 0; z < kSizeZ; ++z) {
        for (int y = 0; y < kSizeY; ++y) {
            EXPECT_FLOAT_EQ(left.sdf_data[SdfIdx(CHUNK_SIZE_X, y, z)],
                            right.sdf_data[SdfIdx(0, y, z)])
                << "cave-carved X-seam mismatch at y=" << y << " z=" << z;
        }
    }
}

// =====================================================================================
// 6. LOD CONSISTENCY -- coarse LOD is a deterministic reduction of the SAME field.
// =====================================================================================

// Meshing the SAME SDF at the same LOD step twice must yield byte-identical meshes
// (vertex positions, normals, materials, and index order) -- meshing purity.
TEST_F(WorldgenHardeningTest, Mesh_PolygoniseIsPure) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk a({0, 0, 0});
    world.GenerateChunkData(a);
    Chunk b({0, 0, 0});
    b.sdf_data = a.sdf_data;
    b.heightmap_data = a.heightmap_data;
    World::MarchingCubes::PolygoniseTerrain(world, a, 0.0f, 1);
    World::MarchingCubes::PolygoniseTerrain(world, b, 0.0f, 1);

    ASSERT_EQ(a.mesh_vertices.size(), b.mesh_vertices.size());
    ASSERT_EQ(a.mesh_indices.size(), b.mesh_indices.size());
    EXPECT_EQ(a.mesh_indices, b.mesh_indices) << "index order is not deterministic";
    for (size_t i = 0; i < a.mesh_vertices.size(); ++i) {
        EXPECT_FLOAT_EQ(a.mesh_vertices[i].position.x, b.mesh_vertices[i].position.x);
        EXPECT_FLOAT_EQ(a.mesh_vertices[i].position.y, b.mesh_vertices[i].position.y);
        EXPECT_FLOAT_EQ(a.mesh_vertices[i].position.z, b.mesh_vertices[i].position.z);
        EXPECT_FLOAT_EQ(a.mesh_vertices[i].normal.x, b.mesh_vertices[i].normal.x)
            << "normal X drifted at vertex " << i << " (order-unstable accumulation?)";
        EXPECT_FLOAT_EQ(a.mesh_vertices[i].normal.y, b.mesh_vertices[i].normal.y);
        EXPECT_FLOAT_EQ(a.mesh_vertices[i].normal.z, b.mesh_vertices[i].normal.z);
        EXPECT_EQ(a.mesh_vertices[i].material_id, b.mesh_vertices[i].material_id);
    }
}

// Meshing the SAME SDF at a coarse step (4) twice must be byte-identical -- the LOD
// reduction is deterministic, not a re-roll.
TEST_F(WorldgenHardeningTest, Mesh_CoarseLODIsDeterministic) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk src({0, 0, 0});
    world.GenerateChunkData(src);

    Chunk a({0, 0, 0});
    a.sdf_data = src.sdf_data;
    a.heightmap_data = src.heightmap_data;
    Chunk b({0, 0, 0});
    b.sdf_data = src.sdf_data;
    b.heightmap_data = src.heightmap_data;
    World::MarchingCubes::PolygoniseTerrain(world, a, 0.0f, 4);
    World::MarchingCubes::PolygoniseTerrain(world, b, 0.0f, 4);

    ASSERT_EQ(a.mesh_vertices.size(), b.mesh_vertices.size());
    EXPECT_EQ(a.mesh_indices, b.mesh_indices);
    for (size_t i = 0; i < a.mesh_vertices.size(); ++i) {
        EXPECT_FLOAT_EQ(a.mesh_vertices[i].position.x, b.mesh_vertices[i].position.x);
        EXPECT_FLOAT_EQ(a.mesh_vertices[i].position.y, b.mesh_vertices[i].position.y);
        EXPECT_FLOAT_EQ(a.mesh_vertices[i].position.z, b.mesh_vertices[i].position.z);
    }
}

// Coarse LOD reads the SAME SDF -> a coarse mesh from an identical field must not
// depend on which world object meshed it (the mesher must hold no per-world state).
TEST_F(WorldgenHardeningTest, Mesh_CoarseLODIndependentOfWorldInstance) {
    SHIELD_WorldSystem world1(nullptr, nullptr, params_terrain, kSeed);
    SHIELD_WorldSystem world2(nullptr, nullptr, params_terrain, kSeed);
    Chunk gen({0, 0, 0});
    world1.GenerateChunkData(gen);

    Chunk a({0, 0, 0});
    a.sdf_data = gen.sdf_data;
    a.heightmap_data = gen.heightmap_data;
    Chunk b({0, 0, 0});
    b.sdf_data = gen.sdf_data;
    b.heightmap_data = gen.heightmap_data;
    World::MarchingCubes::PolygoniseTerrain(world1, a, 0.0f, 2);
    World::MarchingCubes::PolygoniseTerrain(world2, b, 0.0f, 2);
    ASSERT_EQ(a.mesh_vertices.size(), b.mesh_vertices.size());
    EXPECT_EQ(a.mesh_indices, b.mesh_indices);
}

// A coarse LOD mesh must contain fewer triangles than the fine mesh of the same
// field (it is a reduction). Uses the in-bounds terrain preset that meshes.
TEST_F(WorldgenHardeningTest, Mesh_CoarseLODReducesTriangleCount) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk fine({0, 0, 0});
    world.GenerateChunkData(fine);
    World::MarchingCubes::PolygoniseTerrain(world, fine, 0.0f, 1);

    Chunk coarse({0, 0, 0});
    coarse.sdf_data = fine.sdf_data;
    coarse.heightmap_data = fine.heightmap_data;
    World::MarchingCubes::PolygoniseTerrain(world, coarse, 0.0f, 4);

    ASSERT_FALSE(fine.mesh_indices.empty());
    EXPECT_LT(coarse.mesh_indices.size(), fine.mesh_indices.size())
        << "coarse LOD did not reduce the triangle count";
}

// =====================================================================================
// 7. MESHING PURITY -- winding / watertight indices / no stray geometry.
// =====================================================================================

// Every emitted index must reference a real vertex (no dangling indices -> GPU OOB).
TEST_F(WorldgenHardeningTest, Mesh_AllIndicesInRange) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk chunk({0, 0, 0});
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    ASSERT_EQ(chunk.mesh_indices.size() % 3, 0u) << "index buffer is not whole triangles";
    for (u32 idx : chunk.mesh_indices) {
        EXPECT_LT(idx, chunk.mesh_vertices.size()) << "dangling index references no vertex";
    }
}

// No degenerate triangles: the three indices of every triangle must be distinct
// (a triangle with a repeated index is a zero-area sliver -> NaN normals).
TEST_F(WorldgenHardeningTest, Mesh_NoDegenerateTriangleIndices) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk chunk({0, 0, 0});
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    ASSERT_FALSE(chunk.mesh_indices.empty());
    for (size_t i = 0; i < chunk.mesh_indices.size(); i += 3) {
        u32 a = chunk.mesh_indices[i];
        u32 b = chunk.mesh_indices[i + 1];
        u32 c = chunk.mesh_indices[i + 2];
        EXPECT_TRUE(a != b && b != c && a != c)
            << "degenerate (repeated-index) triangle at tri " << (i / 3);
    }
}

// Normals must be unit-length (within tolerance) and never NaN.
TEST_F(WorldgenHardeningTest, Mesh_NormalsAreFiniteUnitVectors) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk chunk({0, 0, 0});
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    ASSERT_FALSE(chunk.mesh_vertices.empty());
    for (const auto& v : chunk.mesh_vertices) {
        EXPECT_TRUE(std::isfinite(v.normal.x) && std::isfinite(v.normal.y) &&
                    std::isfinite(v.normal.z))
            << "non-finite normal";
        float len =
            std::sqrt(v.normal.x * v.normal.x + v.normal.y * v.normal.y + v.normal.z * v.normal.z);
        EXPECT_NEAR(len, 1.0f, 1e-3f) << "normal is not unit length (len=" << len << ")";
    }
}

// On a guaranteed-flat surface, every triangle must wind upward (consistent winding
// -> no back-faces / holes). Mirrors the sibling's winding check at LOD0.
TEST_F(WorldgenHardeningTest, Mesh_FlatSurfaceUpwardWinding) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_flat, kSeed);
    Chunk chunk({0, 0, 0});
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    ASSERT_FALSE(chunk.mesh_indices.empty());
    for (size_t i = 0; i < chunk.mesh_indices.size(); i += 3) {
        const auto& v0 = chunk.mesh_vertices[chunk.mesh_indices[i]];
        const auto& v1 = chunk.mesh_vertices[chunk.mesh_indices[i + 1]];
        const auto& v2 = chunk.mesh_vertices[chunk.mesh_indices[i + 2]];
        const Vec3 ea = v1.position - v0.position;
        const Vec3 eb = v2.position - v0.position;
        const float face_ny = ea.z * eb.x - ea.x * eb.z;
        EXPECT_GT(face_ny, 0.0f) << "downward-wound triangle " << (i / 3) << " on a flat surface";
    }
}

// All emitted LOD0 vertices must lie within (or on) the chunk's LOCAL cell bounds
// [0, CHUNK_SIZE] -- the LOD0 mesher emits chunk-LOCAL positions (PolygoniseTerrain
// pass 1 uses Vec3(IVec3(x,y,z)+corner) directly; world space is only used
// transiently for material classification). A vertex outside [0, CHUNK_SIZE] would
// be an off-by-one in the cell scan -> stray geometry / seam overshoot.
TEST_F(WorldgenHardeningTest, Mesh_VerticesWithinLocalChunkBounds) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    const IVec3 coord{2, 0, -1};
    Chunk chunk(coord);
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    ASSERT_FALSE(chunk.mesh_vertices.empty());
    const float eps = 1e-2f;
    for (const auto& v : chunk.mesh_vertices) {
        EXPECT_GE(v.position.x, 0.0f - eps);
        EXPECT_LE(v.position.x, static_cast<float>(CHUNK_SIZE_X) + eps);
        EXPECT_GE(v.position.y, 0.0f - eps);
        EXPECT_LE(v.position.y, static_cast<float>(CHUNK_SIZE_Y) + eps);
        EXPECT_GE(v.position.z, 0.0f - eps);
        EXPECT_LE(v.position.z, static_cast<float>(CHUNK_SIZE_Z) + eps);
    }
}

// Terrain mesh must never carry Air/Water as a vertex material (mirrors sibling's
// renderable-materials check; a stray Air vertex = a hole-causing classify bug).
TEST_F(WorldgenHardeningTest, Mesh_NoAirOrWaterVertexMaterial) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk chunk({0, 0, 0});
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    ASSERT_FALSE(chunk.mesh_vertices.empty());
    for (const auto& v : chunk.mesh_vertices) {
        EXPECT_NE(v.material_id, static_cast<u32>(MaterialType::Air));
        EXPECT_NE(v.material_id, static_cast<u32>(MaterialType::Water));
    }
}

// =====================================================================================
// 8. DEGENERATE / EMPTY no-op cases -- no crash, no stray triangle.
// =====================================================================================

// All-air chunk (terrain far below the chunk) -> empty mesh, no crash.
TEST_F(WorldgenHardeningTest, Degenerate_AllAirChunkEmptyMesh) {
    TerrainGenParams deep = params_terrain;
    deep.height_offset = -200.0f; // terrain far below chunk (0,0,0)
    SHIELD_WorldSystem world(nullptr, nullptr, deep, kSeed);
    Chunk chunk({0, 0, 0});
    world.GenerateChunkData(chunk);
    // Every SDF sample should be positive (air): no isosurface, hence no mesh.
    bool any_solid = false;
    for (float d : chunk.sdf_data)
        if (d <= 0.0f) {
            any_solid = true;
            break;
        }
    EXPECT_FALSE(any_solid) << "all-air chunk unexpectedly contains solid samples";
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    EXPECT_TRUE(chunk.mesh_vertices.empty()) << "all-air chunk produced stray vertices";
    EXPECT_TRUE(chunk.mesh_indices.empty()) << "all-air chunk produced stray indices";
}

// All-solid chunk (terrain far above the chunk) -> interior fully solid; the
// marching-cubes mesher emits no interior surface (only boundary if any). Must not
// crash and must not produce an isosurface inside a uniformly-solid block.
TEST_F(WorldgenHardeningTest, Degenerate_AllSolidChunkNoInteriorSurface) {
    TerrainGenParams high = params_flat;
    high.height_offset = 500.0f; // surface far above chunk (0,0,0) -> all solid
    SHIELD_WorldSystem world(nullptr, nullptr, high, kSeed);
    Chunk chunk({0, 0, 0});
    world.GenerateChunkData(chunk);
    bool any_air = false;
    for (float d : chunk.sdf_data)
        if (d > 0.0f) {
            any_air = true;
            break;
        }
    EXPECT_FALSE(any_air) << "all-solid chunk unexpectedly contains air samples";
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    // A fully-solid SDF has no zero-crossing, so marching cubes emits nothing.
    EXPECT_TRUE(chunk.mesh_vertices.empty()) << "uniformly-solid chunk produced interior surface";
}

// Empty SDF (the step>1 surface-band-only generation path) must leave sdf_data
// empty and never crash when re-generated -- a degenerate but supported state.
TEST_F(WorldgenHardeningTest, Degenerate_CoarseGenerationLeavesSdfEmpty) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk chunk({0, 0, 0});
    world.GenerateChunkData(chunk, /*target_step=*/4);
    EXPECT_TRUE(chunk.sdf_data.empty()) << "coarse (step>1) generation should skip the SDF";
    EXPECT_FALSE(chunk.heightmap_data.empty())
        << "coarse generation must still produce the heightmap";
    // The coarse heightmap must still match the analytic height (seam authority).
    const IVec3 base{0, 0, 0};
    for (int z = 0; z < kSizeZ; ++z) {
        for (int x = 0; x < kSizeX; ++x) {
            float analytic = world.GetTerrainHeightAt(static_cast<float>(x), static_cast<float>(z));
            EXPECT_FLOAT_EQ(chunk.heightmap_data[HmIdx(x, z)], analytic)
                << "coarse heightmap != analytic at (" << x << "," << z << ")";
            (void)base;
        }
    }
}

// Coarse (step>1) heightmap must EQUAL the full-generation heightmap byte-for-byte:
// the two generation paths must not diverge (documented contract in the header).
TEST_F(WorldgenHardeningTest, Degenerate_CoarseHeightmapMatchesFull) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk full({1, 0, 1});
    world.GenerateChunkData(full, /*target_step=*/1);
    Chunk coarse({1, 0, 1});
    world.GenerateChunkData(coarse, /*target_step=*/4);
    ASSERT_EQ(full.heightmap_data.size(), coarse.heightmap_data.size());
    EXPECT_EQ(full.heightmap_data, coarse.heightmap_data)
        << "full vs coarse heightmap diverged (LOD re-roll instead of reduction)";
}

// Re-meshing an already-meshed chunk (without clearing) must be idempotent OR a clean
// rebuild -- it must never crash and the result must remain valid (whole triangles,
// in-range indices). Guards the in-place remesh path.
TEST_F(WorldgenHardeningTest, Degenerate_RemeshInPlaceStaysValid) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_terrain, kSeed);
    Chunk chunk({0, 0, 0});
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    size_t first_v = chunk.mesh_vertices.size();
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    EXPECT_EQ(chunk.mesh_vertices.size(), first_v) << "remesh changed vertex count (state leak)";
    ASSERT_EQ(chunk.mesh_indices.size() % 3, 0u);
    for (u32 idx : chunk.mesh_indices) {
        EXPECT_LT(idx, chunk.mesh_vertices.size());
    }
}

// =====================================================================================
// 9. FLAT-SURFACE INVARIANTS -- ground truth for the analytic field.
// =====================================================================================

// On the guaranteed-flat preset, every heightmap cell equals height_offset exactly
// across MANY chunks at MANY coords -- proving the height path has no coord-dependent
// drift when amplitude is zero.
TEST_F(WorldgenHardeningTest, Flat_HeightmapConstantAcrossChunks) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_flat, kSeed);
    const IVec3 coords[] = {{0, 0, 0}, {5, 0, -9}, {-3, 0, 12}, {123, 0, -77}};
    for (const IVec3& c : coords) {
        Chunk chunk(c);
        world.GenerateChunkData(chunk);
        for (float h : chunk.heightmap_data) {
            EXPECT_NEAR(h, params_flat.height_offset, 1e-5f)
                << "flat height drifted at chunk (" << c.x << "," << c.y << "," << c.z << ")";
        }
    }
}

// On a flat surface, the analytic density crosses zero exactly at height_offset and
// is monotone in y (solid below, air above) -- a sign-flip elsewhere is a bug.
TEST_F(WorldgenHardeningTest, Flat_DensitySignMonotoneInY) {
    SHIELD_WorldSystem world(nullptr, nullptr, params_flat, kSeed);
    const float surf = params_flat.height_offset;
    for (float below = surf - 1.0f; below >= surf - 6.0f; below -= 1.0f) {
        EXPECT_LT(world.get_density_at({8.0f, below, 8.0f}), 0.0f)
            << "expected solid below surface at y=" << below;
    }
    for (float above = surf + 1.0f; above <= surf + 6.0f; above += 1.0f) {
        EXPECT_GT(world.get_density_at({8.0f, above, 8.0f}), 0.0f)
            << "expected air above surface at y=" << above;
    }
}
