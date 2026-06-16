// T-I3-8: far-LOD tile store + region mesher gates.
// - tile determinism: pristine builds are pure functions of
//   (seed, params, tier, region); pregen hash == hash after a
//   rebuild-from-live pass over an unedited chunk's heightmap.
// - mesher determinism: fixture-region mesh hash is stable.
// - pristine-vs-edited semantics: pristine tiles are regenerable cache keyed
//   (seed, params_hash, tier, region); edited tiles are authoritative.
#include "gtest/gtest.h"

#include "persistence/WorldSaveService.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/FarLodStore.h"
#include "world/MarchingCubes.h"
#include "world/WorldStreamingState.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using Luminumbra::Chunk;
using Luminumbra::IVec3;
using Luminumbra::MaterialType;
using Luminumbra::u16;
using Luminumbra::u32;
using Luminumbra::u64;
using Luminumbra::Vec3;
using Luminumbra::WorldStreamingState;
using Luminumbra::Persistence::WorldSaveService;
using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::Systems::TerrainGenParams;
using namespace Luminumbra::World;

constexpr int kFixtureSeed = 1337;

TerrainGenParams FixtureParams() {
    TerrainGenParams params;
    params.base_frequency = 0.005f;
    params.base_amplitude = 40.0f;
    params.height_offset = 10.0f;
    params.caves_enabled = false;
    params.island_mask_enabled = false;
    return params;
}

std::filesystem::path MakeTempSaveDir(const std::string& tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("luminumbra_farlod_" + tag + "_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

struct TempSaveDir {
    explicit TempSaveDir(const std::string& tag) : path(MakeTempSaveDir(tag)) {}
    ~TempSaveDir() {
        std::error_code remove_error;
        std::filesystem::remove_all(path, remove_error);
    }

    std::filesystem::path path;
};

u64 HashMeshBytes(const FarLodRegionMesh& mesh) {
    u64 hash = 14695981039346656037ull;
    const auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= static_cast<u64>(bytes[i]);
            hash *= 1099511628211ull;
        }
    };
    for (const Luminumbra::VoxelVertex& vertex : mesh.vertices) {
        mix(&vertex.position, sizeof(vertex.position));
        mix(&vertex.normal, sizeof(vertex.normal));
        mix(&vertex.material_id, sizeof(vertex.material_id));
    }
    if (!mesh.indices.empty()) {
        mix(mesh.indices.data(), mesh.indices.size() * sizeof(u32));
    }
    return hash;
}

} // namespace

TEST(FarLodStoreTest, HeightQuantizationRoundTripsWithinHalfStep) {
    for (float height : {-300.0f, -1.25f, 0.0f, 0.03f, 17.5f, 120.0625f, 950.0f}) {
        const u16 q = QuantizeFarLodHeight(height);
        EXPECT_NEAR(DequantizeFarLodHeight(q), height, 0.5f / kFarLodHeightQuantScale);
    }
    // Extremes clamp instead of wrapping.
    EXPECT_EQ(QuantizeFarLodHeight(-1.0e6f), 0u);
    EXPECT_EQ(QuantizeFarLodHeight(1.0e6f), 65535u);
}

TEST(FarLodStoreTest, TierGeometryMatchesPinnedNumbers) {
    // Design-decisions section 4: F1 = 4 m samples, F2 = 8 m; region = 512 m
    // with a shared border row/column.
    EXPECT_EQ(FarLodSampleStepMeters(FarLodTier::F1), 4);
    EXPECT_EQ(FarLodSampleStepMeters(FarLodTier::F2), 8);
    EXPECT_EQ(FarLodSamplesPerSide(FarLodTier::F1), 129u);
    EXPECT_EQ(FarLodSamplesPerSide(FarLodTier::F2), 65u);
}

TEST(FarLodStoreTest, PristineTileBuildIsDeterministic) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);

    const FarLodTile first = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    const FarLodTile second = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    EXPECT_EQ(first.samples_per_side, 129u);
    EXPECT_FALSE(first.edited);
    EXPECT_EQ(ComputeFarLodTileHash(first), ComputeFarLodTileHash(second));
    std::printf("farlod fixture tile hash (seed %d, F1, r0.0): %016llx\n",
        kFixtureSeed, static_cast<unsigned long long>(ComputeFarLodTileHash(first)));

    // Different regions and tiers hash differently.
    const FarLodTile other_region = BuildPristineFarLodTile(world, FarLodTier::F1, 1, 0, params_hash);
    EXPECT_NE(ComputeFarLodTileHash(first), ComputeFarLodTileHash(other_region));
    const FarLodTile f2 = BuildPristineFarLodTile(world, FarLodTier::F2, 0, 0, params_hash);
    EXPECT_EQ(f2.samples_per_side, 65u);
    EXPECT_NE(ComputeFarLodTileHash(first), ComputeFarLodTileHash(f2));

    // Tiles never carry the non-rendering Air material.
    for (Luminumbra::u8 material : first.material) {
        EXPECT_NE(material, static_cast<Luminumbra::u8>(MaterialType::Air));
    }
}

TEST(FarLodStoreTest, PregenHashEqualsRebuildFromLiveOnPristineTerrain) {
    // The far-tile determinism gate: a pristine tile built analytically and
    // the same tile after downsampling an UNEDITED chunk's generated 17x17
    // heightmap must hash equal (the live heightmap and GetTerrainHeightAt
    // sample the same height function; 1/16 m quantization absorbs the
    // batch-vs-scalar noise epsilon).
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);

    const FarLodTile pregen = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    const u64 pregen_hash = ComputeFarLodTileHash(pregen);

    for (const IVec3 coords : {IVec3(0, 1, 0), IVec3(3, 1, 5), IVec3(31, 1, 31)}) {
        Chunk chunk(coords);
        world.GenerateChunkData(chunk); // full generation, pristine heightmap
        FarLodTile rebuilt = pregen;
        const std::size_t written = ApplyChunkHeightmapToFarLodTile(rebuilt, chunk, /*mark_edited=*/false);
        EXPECT_EQ(written, 25u) << "F1: 16 m footprint / 4 m step + shared border = 5x5 samples";
        EXPECT_FALSE(rebuilt.edited);
        EXPECT_EQ(ComputeFarLodTileHash(rebuilt), pregen_hash)
            << "unedited chunk heightmap must not change the pristine tile (chunk "
            << coords.x << "," << coords.z << ")";
    }

    // F2 sees the same chunk at 8 m: 3x3 samples.
    const FarLodTile pregen_f2 = BuildPristineFarLodTile(world, FarLodTier::F2, 0, 0, params_hash);
    Chunk chunk(IVec3(4, 1, 4));
    world.GenerateChunkData(chunk);
    FarLodTile rebuilt_f2 = pregen_f2;
    EXPECT_EQ(ApplyChunkHeightmapToFarLodTile(rebuilt_f2, chunk, false), 9u);
    EXPECT_EQ(ComputeFarLodTileHash(rebuilt_f2), ComputeFarLodTileHash(pregen_f2));

    // Chunks outside the region write nothing.
    Chunk outside(IVec3(40, 1, 0));
    world.GenerateChunkData(outside);
    FarLodTile untouched = pregen;
    EXPECT_EQ(ApplyChunkHeightmapToFarLodTile(untouched, outside, false), 0u);
}

TEST(FarLodStoreTest, EditedRebuildMarksTileAuthoritative) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);

    const FarLodTile pristine = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);

    Chunk chunk(IVec3(2, 1, 2));
    world.GenerateChunkData(chunk);
    for (float& height : chunk.heightmap_data) {
        height += 5.0f; // player terraforming
    }
    chunk.mark_voxel_data_dirty();

    FarLodTile edited = pristine;
    const std::size_t written = ApplyChunkHeightmapToFarLodTile(edited, chunk, /*mark_edited=*/true);
    EXPECT_EQ(written, 25u);
    EXPECT_TRUE(edited.edited);
    EXPECT_NE(ComputeFarLodTileHash(edited), ComputeFarLodTileHash(pristine));

    std::size_t edited_samples = 0;
    for (Luminumbra::u8 flags : edited.flags) {
        if (flags & kFarLodSampleFlagEdited) {
            ++edited_samples;
        }
    }
    EXPECT_EQ(edited_samples, 25u);
}

TEST(FarLodStoreTest, TilePersistenceRoundTripsThroughLmr1Container) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);

    TempSaveDir save_dir("roundtrip");
    const FarLodStore store(save_dir.path);

    const FarLodTile tile = BuildPristineFarLodTile(world, FarLodTier::F1, -1, 2, params_hash);
    std::vector<std::string> errors;
    ASSERT_TRUE(store.save_tile(tile, &errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(std::filesystem::exists(WorldSaveService::region_file_path(save_dir.path, -1, 2)));

    FarLodTile loaded;
    ASSERT_TRUE(store.load_tile(FarLodTier::F1, -1, 2, params_hash, loaded, &errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(loaded.samples_per_side, tile.samples_per_side);
    EXPECT_EQ(loaded.params_hash, tile.params_hash);
    EXPECT_FALSE(loaded.edited);
    EXPECT_EQ(ComputeFarLodTileHash(loaded), ComputeFarLodTileHash(tile));

    // A different (tier, region) is a clean miss.
    FarLodTile miss;
    EXPECT_FALSE(store.load_tile(FarLodTier::F2, -1, 2, params_hash, miss, &errors));
    EXPECT_TRUE(errors.empty());
}

TEST(FarLodStoreTest, PristineCacheMissesOnParamsHashMismatchEditedLoadsAnyway) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);
    const u64 other_params_hash = ComputeTerrainParamsHash(params, kFixtureSeed + 1);
    ASSERT_NE(params_hash, other_params_hash);

    TempSaveDir save_dir("cache");
    const FarLodStore store(save_dir.path);
    std::vector<std::string> errors;

    // Pristine tile: regenerable cache. Mismatching params -> clean miss.
    const FarLodTile pristine = BuildPristineFarLodTile(world, FarLodTier::F2, 0, 0, params_hash);
    ASSERT_TRUE(store.save_tile(pristine, &errors));
    FarLodTile loaded;
    EXPECT_FALSE(store.load_tile(FarLodTier::F2, 0, 0, other_params_hash, loaded, &errors));
    EXPECT_TRUE(errors.empty()) << "params mismatch on a pristine tile must be a clean miss";

    // Edited tile: authoritative - loads regardless of the params hash.
    FarLodTile edited = pristine;
    Chunk chunk(IVec3(1, 1, 1));
    world.GenerateChunkData(chunk);
    for (float& height : chunk.heightmap_data) {
        height -= 3.0f;
    }
    ASSERT_GT(ApplyChunkHeightmapToFarLodTile(edited, chunk, true), 0u);
    ASSERT_TRUE(edited.edited);
    ASSERT_TRUE(store.save_tile(edited, &errors));
    FarLodTile loaded_edited;
    EXPECT_TRUE(store.load_tile(FarLodTier::F2, 0, 0, other_params_hash, loaded_edited, &errors));
    EXPECT_TRUE(loaded_edited.edited);
    EXPECT_EQ(ComputeFarLodTileHash(loaded_edited), ComputeFarLodTileHash(edited));
}

TEST(FarLodStoreTest, TileRecordsCoexistWithChunkRecords) {
    // Tiles share the LMR1 region files with chunk records: the chunk writer
    // must preserve tile records and the chunk loader must skip them.
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);

    TempSaveDir save_dir("coexist");
    const FarLodStore store(save_dir.path);
    const WorldSaveService service;
    std::vector<std::string> errors;

    // Chunk records first.
    WorldStreamingState state;
    auto chunk = state.get_or_create_chunk(IVec3(1, 1, 1));
    chunk->set_state(Luminumbra::ChunkState::Ready);
    world.GenerateChunkData(*chunk);
    ASSERT_TRUE(service.save_world(state, save_dir.path, &errors));

    // Tile record into the SAME region file.
    const FarLodTile tile = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    ASSERT_TRUE(store.save_tile(tile, &errors));

    // Chunk loader skips tile records.
    WorldStreamingState restored;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, errors));
    EXPECT_EQ(restored.size(), 1u);
    EXPECT_EQ(service.world_hash(restored), service.world_hash(state));

    // A chunk rewrite (dirty save) preserves the tile record verbatim.
    chunk->heightmap_data[0] += 1.0f;
    chunk->mark_voxel_data_dirty();
    const auto report = service.save_dirty_chunks(state, save_dir.path, &errors);
    ASSERT_TRUE(report.saved);
    FarLodTile loaded;
    ASSERT_TRUE(store.load_tile(FarLodTier::F1, 0, 0, params_hash, loaded, &errors));
    EXPECT_EQ(ComputeFarLodTileHash(loaded), ComputeFarLodTileHash(tile));
}

TEST(FarLodRegionMesher, FixtureRegionMeshIsDeterministic) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);

    const FarLodTile tile = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);

    FarLodRegionMesh first;
    const auto stats_first = Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(tile, first);
    FarLodRegionMesh second;
    Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(tile, second);

    // 28-byte VoxelVertex layout untouched (meshing determinism contract).
    static_assert(sizeof(Luminumbra::VoxelVertex) == 28, "VoxelVertex layout must stay 28 bytes");

    EXPECT_EQ(HashMeshBytes(first), HashMeshBytes(second));
    std::printf("farlod fixture region mesh hash (seed %d, F1, r0.0): %016llx\n",
        kFixtureSeed, static_cast<unsigned long long>(HashMeshBytes(first)));
    // Surface lattice 129x129 + 4 edges x 128 skirt quads x 4 vertices.
    EXPECT_EQ(stats_first.skirt_quads, 512u);
    EXPECT_EQ(first.vertices.size(), 129u * 129u + 512u * 4u);
    // Whole-tile heightfield: every cell emits two triangles (no ownership
    // holes), plus two per skirt quad.
    EXPECT_EQ(first.indices.size(), (128u * 128u * 2u + 512u * 2u) * 3u);
    EXPECT_EQ(stats_first.triangles, first.indices.size() / 3u);

    for (const Luminumbra::VoxelVertex& vertex : first.vertices) {
        EXPECT_NE(vertex.material_id, static_cast<u32>(MaterialType::Air));
        const float length2 = glm::dot(vertex.normal, vertex.normal);
        EXPECT_NEAR(length2, 1.0f, 1.0e-3f);
    }

    // F2 fixture (65x65 lattice, 64 quads per edge).
    const FarLodTile tile_f2 = BuildPristineFarLodTile(world, FarLodTier::F2, 0, 0, params_hash);
    FarLodRegionMesh mesh_f2;
    const auto stats_f2 = Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(tile_f2, mesh_f2);
    EXPECT_EQ(stats_f2.skirt_quads, 256u);
    EXPECT_EQ(mesh_f2.vertices.size(), 65u * 65u + 256u * 4u);
}

TEST(FarLodRegionMesher, AdjacentRegionsShareBorderVertexPositions) {
    // The shared border row/column contract: the max-X column of region
    // (0,0) and the min-X column of region (1,0) are the same world samples.
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);

    const FarLodTile left = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    const FarLodTile right = BuildPristineFarLodTile(world, FarLodTier::F1, 1, 0, params_hash);
    const u32 n = left.samples_per_side;
    for (u32 z = 0; z < n; ++z) {
        const std::size_t left_index = static_cast<std::size_t>(z) * n + (n - 1u);
        const std::size_t right_index = static_cast<std::size_t>(z) * n;
        EXPECT_EQ(left.height_q[left_index], right.height_q[right_index]) << "row " << z;
        EXPECT_EQ(left.material[left_index], right.material[right_index]) << "row " << z;
    }
}

// T-I6-A1.5: the shaping params (continentalness/erosion/peaks freqs + splines)
// are folded into ComputeTerrainParamsHash, gated on shaping_enabled, so shaped
// presets' pristine far-LOD tiles self-invalidate on a shaping change. Pins the
// contract: shaping-OFF ignores the shaping fields (byte-stable cache key for
// non-shaped worlds), shaping-ON re-keys on any spline/freq/count change, and
// the hash is deterministic.
TEST(FarLodStore, TerrainParamsHashShapingFold) {
    auto make = [](bool shaping) {
        TerrainGenParams p;
        p.base_frequency = 0.008f; p.base_amplitude = 60.0f; p.octaves = 5;
        p.persistence = 0.55f; p.lacunarity = 2.1f; p.height_offset = 12.0f;
        p.shaping_enabled = shaping;
        p.continentalness_frequency = 0.0008f; p.erosion_frequency = 0.0015f;
        p.peaks_frequency = 0.004f; p.peaks_amplitude = 90.0f;
        p.domain_warp_amplitude = 30.0f; p.domain_warp_frequency = 0.006f;
        p.continental_spline = {{-1.0f, -40.0f}, {0.0f, 0.0f}, {1.0f, 40.0f}};
        p.erosion_spline = {{-1.0f, 1.0f}, {1.0f, 0.1f}};
        p.peaks_spline = {{-1.0f, 0.0f}, {1.0f, 1.0f}};
        return p;
    };
    const int seed = 424242;
    const TerrainGenParams off = make(false);
    const TerrainGenParams on = make(true);

    // Determinism: identical params -> identical hash.
    EXPECT_EQ(ComputeTerrainParamsHash(on, seed), ComputeTerrainParamsHash(on, seed));

    // Enabling shaping engages the fold -> hash differs from the shaping-off path.
    EXPECT_NE(ComputeTerrainParamsHash(off, seed), ComputeTerrainParamsHash(on, seed))
        << "shaping fold did not engage";

    // Shaping-OFF ignores the shaping fields: mutating them on a shaping-off
    // params must NOT change the hash (the gated block is skipped -> the far-tile
    // cache key is byte-stable for every non-shaped world, fixtures stay green).
    TerrainGenParams off2 = off;
    off2.continental_spline = {{-1.0f, 99.0f}};
    off2.peaks_amplitude = 1234.0f;
    off2.erosion_frequency = 0.5f;
    EXPECT_EQ(ComputeTerrainParamsHash(off, seed), ComputeTerrainParamsHash(off2, seed))
        << "shaping-off path must ignore shaping fields (byte-stable cache key)";

    // Shaping-ON: a spline control-point change re-keys the hash.
    TerrainGenParams on_spline = on;
    on_spline.peaks_spline = {{-1.0f, 0.0f}, {1.0f, 0.9f}};
    EXPECT_NE(ComputeTerrainParamsHash(on, seed), ComputeTerrainParamsHash(on_spline, seed))
        << "shaping spline content not hashed";

    // Shaping-ON: a frequency change re-keys the hash.
    TerrainGenParams on_freq = on;
    on_freq.erosion_frequency = on.erosion_frequency * 2.0f;
    EXPECT_NE(ComputeTerrainParamsHash(on, seed), ComputeTerrainParamsHash(on_freq, seed))
        << "shaping frequency not hashed";

    // Shaping-ON: spline COUNT matters (the count prefix prevents merge collisions).
    TerrainGenParams on_count = on;
    on_count.continental_spline = {{-1.0f, -40.0f}, {0.0f, 0.0f}, {1.0f, 40.0f}, {0.5f, 20.0f}};
    EXPECT_NE(ComputeTerrainParamsHash(on, seed), ComputeTerrainParamsHash(on_count, seed))
        << "spline count not hashed";
}
