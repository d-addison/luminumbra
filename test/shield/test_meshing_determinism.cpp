// Marching cubes meshing determinism gate.
//
// Locks the bit-exact bytes of PolygoniseTerrain output (mesh_vertices +
// mesh_indices) for a set of fixture chunks at LOD steps 1, 2 and 4 via
// FNV-1a-64 hashes captured from the reference implementation. Any hot-path
// optimization of the mesher must keep these hashes UNCHANGED.
//
// Also contains a small steady_clock benchmark of the step-1 hot path so
// before/after optimization timings can be read from test output.

#include "gtest/gtest.h"

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

#include <FastNoise/FastNoise.h>
#include <nlohmann/json.hpp>

#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

// VoxelVertex must stay tightly packed (Vec3 + Vec3 + u32 = 28 bytes) for raw
// byte hashing to be meaningful. If padding ever appears, this gate must be
// rewritten to hash fields explicitly.
static_assert(sizeof(VoxelVertex) == 28,
              "VoxelVertex layout changed; meshing determinism hashes are stale");

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

// Retain the inputs and individual vertex fields alongside the hash oracle.
// CI uploads test-artifacts, allowing the first field divergence to be located
// across compilers and CPUs without replacing a failed baseline.
void WriteMeshEvidence(const TerrainGenParams& params, int seed, const Chunk& chunk, int step) {
    const auto bits = [](float value) {
        return std::bit_cast<std::uint32_t>(value);
    };
    auto perlin = FastNoise::New<FastNoise::Perlin>(FastSIMD::Level_AVX2);
    auto worley = FastNoise::New<FastNoise::CellularDistance>(FastSIMD::Level_AVX2);
    worley->SetDistanceIndex0(0);
    worley->SetDistanceIndex1(2);
    worley->SetReturnType(FastNoise::CellularDistance::ReturnType::Index0Div1);
    nlohmann::json evidence;
#ifdef _MSC_VER
    evidence["compiler"] = _MSC_FULL_VER;
#else
    evidence["compiler"] = __VERSION__;
#endif
    evidence["cpu_max_simd"] = static_cast<unsigned>(FastSIMD::CPUMaxSIMDLevel());
    evidence["noise_simd"] = static_cast<unsigned>(perlin->GetSIMDLevel());
    evidence["seed"] = seed;
    evidence["step"] = step;
    evidence["float_encoding"] = "IEEE754 binary32 bits";
    evidence["sdf"] = nlohmann::json::array();
    for (float value : chunk.sdf_data)
        evidence["sdf"].push_back(bits(value));
    evidence["heights"] = nlohmann::json::array();
    for (float value : chunk.heightmap_data)
        evidence["heights"].push_back(bits(value));
    evidence["noise_fields"] = nlohmann::json::array();
    const IVec3 base = chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    if (params.caves_enabled) {
        for (int z = 0; z <= CHUNK_SIZE_Z; ++z)
            for (int y = 0; y <= CHUNK_SIZE_Y; ++y)
                for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
                    const Vec3 p(base + IVec3(x, y, z));
                    const auto sample = [&](float frequency, int offset) {
                        return bits(perlin->GenSingle3D(
                            p.x * frequency, p.y * frequency, p.z * frequency, seed + offset));
                    };
                    evidence["noise_fields"].push_back(
                        {sample(params.cave_frequency, 1),
                         sample(params.spaghetti_frequency, 20),
                         bits(worley->GenSingle3D(p.x * params.worley_frequency,
                                                  p.y * params.worley_frequency,
                                                  p.z * params.worley_frequency,
                                                  seed + 21))});
                }
    }
    evidence["vertices"] = nlohmann::json::array();
    for (const auto& vertex : chunk.mesh_vertices)
        evidence["vertices"].push_back({bits(vertex.position.x),
                                        bits(vertex.position.y),
                                        bits(vertex.position.z),
                                        bits(vertex.normal.x),
                                        bits(vertex.normal.y),
                                        bits(vertex.normal.z),
                                        vertex.material_id});
    evidence["indices"] = chunk.mesh_indices;
    const std::filesystem::path directory =
        std::filesystem::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "meshing-determinism";
    std::filesystem::create_directories(directory);
    const auto path = directory / (std::to_string(seed) + "-" + std::to_string(step) + ".json");
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << evidence.dump() << '\n';
    ASSERT_TRUE(output.good()) << path.string();
}

MeshHashes HashChunkMesh(const TerrainGenParams& params, int seed, const IVec3& coords, int step) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params, seed);
    Chunk chunk(coords);
    world_system.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, step);
    WriteMeshEvidence(params, seed, chunk, step);

    MeshHashes hashes;
    hashes.vertex_count = chunk.mesh_vertices.size();
    hashes.index_count = chunk.mesh_indices.size();
    hashes.vertex_hash =
        Fnv1a64(chunk.mesh_vertices.data(), chunk.mesh_vertices.size() * sizeof(VoxelVertex));
    hashes.index_hash = Fnv1a64(chunk.mesh_indices.data(), chunk.mesh_indices.size() * sizeof(u32));
    return hashes;
}

void PrintHashes(const char* combo, int step, const MeshHashes& hashes) {
    std::cout << "[ MESHHASH ] " << combo << " step " << step << " vertices=" << hashes.vertex_count
              << " indices=" << hashes.index_count << " vertex_hash=0x" << std::hex
              << std::setfill('0') << std::setw(16) << hashes.vertex_hash << " index_hash=0x"
              << std::setw(16) << hashes.index_hash << std::dec << std::setfill(' ') << std::endl;
}

struct ExpectedMeshHashes {
    int step;
    std::uint64_t vertex_hash;
    std::uint64_t index_hash;
};

constexpr std::uint64_t ToolchainVertexHash(std::uint64_t msvc, std::uint64_t gcc_clang) {
#ifdef _MSC_VER
    (void)gcc_clang;
    return msvc;
#else
    (void)msvc;
    return gcc_clang;
#endif
}

void VerifyCombo(const char* combo,
                 const TerrainGenParams& params,
                 int seed,
                 const IVec3& coords,
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

// Retirement of cave_style/shaping_enabled moves the synthetic terrain hashes.
// Pin vertex bytes for each compiler family and topology on every compiler.
// Independent-world replay additionally proves current determinism.
void VerifyCurrentCombo(const char* combo,
                        const TerrainGenParams& params,
                        int seed,
                        const IVec3& coords,
                        const ExpectedMeshHashes (&expected)[3]) {
    for (const auto& exp : expected) {
        const auto first = HashChunkMesh(params, seed, coords, exp.step);
        const auto replay = HashChunkMesh(params, seed, coords, exp.step);
        PrintHashes(combo, exp.step, first);
        EXPECT_EQ(first.vertex_count, replay.vertex_count);
        EXPECT_EQ(first.index_count, replay.index_count);
        EXPECT_EQ(first.vertex_hash, replay.vertex_hash);
        EXPECT_EQ(first.index_hash, replay.index_hash);
        EXPECT_EQ(first.vertex_hash, exp.vertex_hash) << combo << " step " << exp.step;
        EXPECT_EQ(first.index_hash, exp.index_hash) << combo << " step " << exp.step;
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

// Index topology is toolchain-independent; vertex baselines cover each compiler
// family, with additional independent-world determinism checks.
TEST(MeshingDeterminism, ArchipelagoChunkHashesAreStable) {
    // Re-pinned for cave_style/shaping_enabled retirement.
    const ExpectedMeshHashes expected[3] = {
        {1,
         ToolchainVertexHash(0x9d9c9b1d3942db3eull, 0x43b033500a7e70afull),
         0x7a014cd2e589b3a1ull},
        {2,
         ToolchainVertexHash(0x71c0e159fcaa096dull, 0xd055fb106b361fddull),
         0x9177a54dc6654457ull},
        {4,
         ToolchainVertexHash(0x701ce4034488d411ull, 0x701ce4034488d411ull),
         0x7257517ea6a6be50ull},
    };
    VerifyCurrentCombo(
        "archipelago seed=42 chunk=(0,0,0)", MakeArchipelagoParams(), 42, IVec3(0, 0, 0), expected);
}

TEST(MeshingDeterminism, CaveChunkHashesAreStable) {
    // Precise AVX reciprocal/sqrt operations preserve the reference topology.
    // CPU-specific estimates previously crossed the Worley carve threshold at
    // (0,10,15), changing 52 vertices / 174 indices into 51 / 168. Vertex bytes
    // below follow the corrected field; all three index hashes remain unchanged.
    const ExpectedMeshHashes expected[3] = {
        {1,
         ToolchainVertexHash(0x52a14947bf315497ull, 0x52a14947bf315497ull),
         0x501667b909047bbcull},
        {2,
         ToolchainVertexHash(0x186c4e8e9203bc6eull, 0x186c4e8e9203bc6eull),
         0xc026220b072ee6ddull},
        {4,
         ToolchainVertexHash(0xf5a604c650bdef3full, 0xf5a604c650bdef3full),
         0x7298410a91b6706full},
    };
    VerifyCurrentCombo(
        "caves seed=12345 chunk=(0,0,0)", MakeCaveParams(), 12345, IVec3(0, 0, 0), expected);
}

TEST(MeshingDeterminism, WorleyCarveThresholdUsesPreciseNoise) {
    auto worley = FastNoise::New<FastNoise::CellularDistance>(FastSIMD::Level_AVX2);
    worley->SetDistanceIndex0(0);
    worley->SetDistanceIndex1(2);
    worley->SetReturnType(FastNoise::CellularDistance::ReturnType::Index0Div1);
    const auto params = MakeCaveParams();
    // The first differing noise sample and the sole occupancy flip from the
    // failing CI trace, respectively. Compare bits before meshing/interpolation.
    const auto sample = [&](const Vec3& p) {
        return worley->GenSingle3D(p.x * params.worley_frequency,
                                   p.y * params.worley_frequency,
                                   p.z * params.worley_frequency,
                                   12345 + 21);
    };
    EXPECT_EQ(std::bit_cast<std::uint32_t>(sample(Vec3(3, 0, 0))), 0x3e83ea99u);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(sample(Vec3(0, 10, 15))), 0x3f1ebe77u);
    SHIELD_WorldSystem world(nullptr, nullptr, params, 12345);
    EXPECT_LT(world.get_density_at(Vec3(0, 10, 15)), 0.0f);
}

TEST(MeshingDeterminism, FlatSurfaceChunkHashesAreStable) {
    const ExpectedMeshHashes expected[3] = {
        {1, 0x8108751b0ed03207ull, 0xcbacacb06ef692e3ull},
        {2, 0x440ca86c8e7feb2full, 0x84202cee8631b65full},
        {4, 0xbc6a9e9e583d2347ull, 0x08953fb8355470d3ull},
    };
    VerifyCombo(
        "flat seed=1337 chunk=(0,0,0)", MakeFlatSurfaceParams(), 1337, IVec3(0, 0, 0), expected);
}

TEST(MeshingDeterminism, CoarseStepUsesAuthoritativeSdfLattice) {
    TerrainGenParams params = MakeFlatSurfaceParams();
    params.height_offset = 0.0f;

    SHIELD_WorldSystem world_system(nullptr, nullptr, params, 9001);
    Chunk chunk(IVec3(0, 0, 0));

    constexpr std::size_t lattice_width = static_cast<std::size_t>(CHUNK_SIZE_X + 1);
    constexpr std::size_t lattice_height = static_cast<std::size_t>(CHUNK_SIZE_Y + 1);
    chunk.sdf_data.resize(lattice_width * lattice_height *
                          static_cast<std::size_t>(CHUNK_SIZE_Z + 1));

    // This authoritative lattice describes a horizontal surface at y=12. It
    // deliberately disagrees with the flat analytic terrain (y=0), so using
    // GetTerrainHeightAtCoarse instead of the resident SDF cannot pass.
    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
            for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
                const std::size_t index =
                    static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * lattice_width +
                    static_cast<std::size_t>(z) * lattice_width * lattice_height;
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
        ASSERT_FALSE(chunk.mesh_vertices.empty())
            << fixture.name << " fixture produced an empty mesh";

        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kIterations; ++i) {
            World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, 1);
        }
        const auto end = std::chrono::steady_clock::now();
        const auto total_us =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "[ MESHPERF ] " << fixture.name << " PolygoniseTerrain step=1 x" << kIterations
                  << ": total " << total_us << " us, avg "
                  << (static_cast<double>(total_us) / kIterations) << " us/chunk" << std::endl;
    }
}
