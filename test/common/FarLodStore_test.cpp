// far-LOD tile store + region mesher gates.
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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Luminumbra::Chunk;
using Luminumbra::i16;
using Luminumbra::i32;
using Luminumbra::IVec3;
using Luminumbra::MaterialType;
using Luminumbra::u16;
using Luminumbra::u32;
using Luminumbra::u64;
using Luminumbra::u8;
using Luminumbra::Vec3;
using Luminumbra::WorldStreamingState;
using Luminumbra::Persistence::WorldSaveService;
using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::Systems::TerrainGenParams;
using namespace Luminumbra::World;

constexpr int kFixtureSeed = 1337;

constexpr u64 ToolchainHash(u64 msvc, u64 gcc_clang) {
#ifdef _MSC_VER
    (void)gcc_clang;
    return msvc;
#else
    (void)msvc;
    return gcc_clang;
#endif
}

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
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("luminumbra_farlod_" + tag + "_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

struct TempSaveDir {
    explicit TempSaveDir(const std::string& tag)
        : path(MakeTempSaveDir(tag)) {}
    ~TempSaveDir() {
        std::error_code remove_error;
        std::filesystem::remove_all(path, remove_error);
    }

    std::filesystem::path path;
};

std::string ReadTestFileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool HasRegionTemporaryFile(const std::filesystem::path& region_path) {
    const std::string prefix = region_path.filename().string() + ".tmp.";
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(region_path.parent_path(), error)) {
        if (!error && entry.path().filename().string().rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

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

u64 HashTerrainMeshBytes(const Chunk& chunk) {
    u64 hash = 14695981039346656037ull;
    const auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= static_cast<u64>(bytes[i]);
            hash *= 1099511628211ull;
        }
    };
    for (const Luminumbra::VoxelVertex& vertex : chunk.mesh_vertices) {
        mix(&vertex.position, sizeof(vertex.position));
        mix(&vertex.normal, sizeof(vertex.normal));
        mix(&vertex.material_id, sizeof(vertex.material_id));
    }
    if (!chunk.mesh_indices.empty()) {
        mix(chunk.mesh_indices.data(), chunk.mesh_indices.size() * sizeof(u32));
    }
    return hash;
}

TerrainGenParams FlatSdfFixtureParams() {
    TerrainGenParams params = FixtureParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 12.0f;
    return params;
}

void CarveAlignedResidentSdfCavity(Chunk& chunk, int step) {
    const int size_x = Luminumbra::CHUNK_SIZE_X + 1;
    const int size_y = Luminumbra::CHUNK_SIZE_Y + 1;
    const int size_z = Luminumbra::CHUNK_SIZE_Z + 1;
    ASSERT_EQ(chunk.sdf_data.size(), static_cast<std::size_t>(size_x * size_y * size_z));
    for (int z = 4; z <= 12; z += step) {
        for (int y = 4; y <= 8; y += step) {
            for (int x = 4; x <= 12; x += step) {
                const std::size_t index = static_cast<std::size_t>(x) +
                                          static_cast<std::size_t>(y) * size_x +
                                          static_cast<std::size_t>(z) * size_x * size_y;
                chunk.sdf_data[index] = 1.0f;
            }
        }
    }
}

std::size_t FullSdfIndex(int x, int y, int z) {
    const std::size_t side_x = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X) + 1u;
    const std::size_t side_y = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_Y) + 1u;
    return static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * side_x +
           static_cast<std::size_t>(z) * side_x * side_y;
}

FarLodSdfSnapshot AuthoritativeSdfSnapshot(const IVec3& coords, u32 revision) {
    const std::size_t side_x = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X) + 1u;
    const std::size_t side_y = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_Y) + 1u;
    const std::size_t side_z = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_Z) + 1u;
    FarLodSdfSnapshot snapshot;
    snapshot.coords = coords;
    snapshot.revision = revision;
    snapshot.source_kind = FarLodBrickSourceKind::Authoritative;
    snapshot.sdf_data.assign(side_x * side_y * side_z, -2.0f);
    snapshot.material_data.assign(snapshot.sdf_data.size(), 17u);
    return snapshot;
}

void FillFlatSdf(FarLodSdfSnapshot& snapshot, int surface_y = 12) {
    for (int z = 0; z <= Luminumbra::CHUNK_SIZE_Z; ++z) {
        for (int y = 0; y <= Luminumbra::CHUNK_SIZE_Y; ++y) {
            for (int x = 0; x <= Luminumbra::CHUNK_SIZE_X; ++x) {
                const int world_y = snapshot.coords.y * Luminumbra::CHUNK_SIZE_Y + y;
                snapshot.sdf_data[FullSdfIndex(x, y, z)] = static_cast<float>(world_y - surface_y);
            }
        }
    }
}

void AddCompleteFlatSdfHalo(FarLodTile& tile, const IVec3& center) {
    std::string error;
    for (int z_offset = -1; z_offset <= 1; ++z_offset) {
        for (int x_offset = -1; x_offset <= 1; ++x_offset) {
            for (int chunk_y = center.y - 1; chunk_y <= center.y + 1; ++chunk_y) {
                FarLodSdfSnapshot snapshot = AuthoritativeSdfSnapshot(
                    IVec3(center.x + x_offset, chunk_y, center.z + z_offset), 1u);
                FillFlatSdf(snapshot);
                snapshot.source_kind = FarLodBrickSourceKind::RegenerableCache;
                ASSERT_EQ(ReduceChunkSdfIntoFarTile(tile, snapshot, &error),
                          FarLodSdfReduceResult::Inserted)
                    << error;
            }
        }
    }
}

std::size_t ReducedSdfIndex(FarLodTier tier, int x, int y, int z) {
    const std::size_t side = FarLodSdfBrickSamplesPerSide(tier);
    const int step = FarLodSampleStepMeters(tier);
    return static_cast<std::size_t>(x / step) + static_cast<std::size_t>(y / step) * side +
           static_cast<std::size_t>(z / step) * side * side;
}

template<typename T>
void AppendTestValue(std::string& buffer, const T& value) {
    buffer.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

template<typename T>
void WriteTestValue(std::string& buffer, std::size_t offset, const T& value) {
    ASSERT_LE(offset + sizeof(value), buffer.size());
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

struct Fsd2TestLayout {
    std::size_t tier = 6u;
    std::size_t height_count = 28u;
    std::size_t descriptor_stream = 0u;
    std::size_t density_stream = 0u;
};

Fsd2TestLayout Fsd2LayoutFor(const FarLodTile& tile) {
    Fsd2TestLayout layout;
    const std::size_t count = tile.sample_count();
    const std::size_t height_stream = layout.height_count + 2u * sizeof(u32);
    const std::size_t material_header = height_stream + count * sizeof(u16);
    const std::size_t flags_header = material_header + 2u * sizeof(u32) + count;
    const std::size_t brick_count = flags_header + 2u * sizeof(u32) + count;
    layout.descriptor_stream = brick_count + sizeof(u32);
    layout.density_stream =
        layout.descriptor_stream + tile.sdf_bricks.size() * 16u + 2u * sizeof(u32);
    return layout;
}

void AppendLegacyTilePayload(std::string& payload, const FarLodTile& tile) {
    AppendTestValue(payload, static_cast<u8>(tile.tier));
    AppendTestValue(payload, tile.rx);
    AppendTestValue(payload, tile.rz);
    AppendTestValue(payload, tile.samples_per_side);
    AppendTestValue(payload, tile.params_hash);
    AppendTestValue(payload, static_cast<u8>(tile.edited ? 1 : 0));
    payload.append(reinterpret_cast<const char*>(tile.height_q.data()),
                   tile.height_q.size() * sizeof(u16));
    payload.append(reinterpret_cast<const char*>(tile.material.data()), tile.material.size());
    payload.append(reinterpret_cast<const char*>(tile.flags.data()), tile.flags.size());
}

} // namespace

TEST(MarchingCubesAuthoritativeSdf, CoarseStep2UsesResidentLattice) {
    const TerrainGenParams params = FlatSdfFixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    Chunk pristine(IVec3(0, 0, 0));
    Chunk edited(IVec3(0, 0, 0));
    world.GenerateChunkData(pristine);
    world.GenerateChunkData(edited);
    ASSERT_EQ(pristine.heightmap_data, edited.heightmap_data);
    CarveAlignedResidentSdfCavity(edited, 2);

    Luminumbra::World::MarchingCubes::PolygoniseTerrain(world, pristine, 0.0f, 2);
    Luminumbra::World::MarchingCubes::PolygoniseTerrain(world, edited, 0.0f, 2);

    ASSERT_FALSE(pristine.mesh_indices.empty());
    ASSERT_FALSE(edited.mesh_indices.empty());
    EXPECT_NE(HashTerrainMeshBytes(pristine), HashTerrainMeshBytes(edited));
    EXPECT_TRUE(std::any_of(
        edited.mesh_vertices.begin(),
        edited.mesh_vertices.end(),
        [](const Luminumbra::VoxelVertex& vertex) { return vertex.position.y < 10.0f; }));
}

TEST(MarchingCubesAuthoritativeSdf, CoarseStep4UsesResidentLattice) {
    const TerrainGenParams params = FlatSdfFixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    Chunk pristine(IVec3(0, 0, 0));
    Chunk edited(IVec3(0, 0, 0));
    world.GenerateChunkData(pristine);
    world.GenerateChunkData(edited);
    ASSERT_EQ(pristine.heightmap_data, edited.heightmap_data);
    CarveAlignedResidentSdfCavity(edited, 4);

    Luminumbra::World::MarchingCubes::PolygoniseTerrain(world, pristine, 0.0f, 4);
    Luminumbra::World::MarchingCubes::PolygoniseTerrain(world, edited, 0.0f, 4);

    ASSERT_FALSE(pristine.mesh_indices.empty());
    ASSERT_FALSE(edited.mesh_indices.empty());
    EXPECT_NE(HashTerrainMeshBytes(pristine), HashTerrainMeshBytes(edited));
    EXPECT_TRUE(std::any_of(
        edited.mesh_vertices.begin(),
        edited.mesh_vertices.end(),
        [](const Luminumbra::VoxelVertex& vertex) { return vertex.position.y < 10.0f; }));
}

TEST(MarchingCubesAuthoritativeSdf, EmptySdfKeepsPristineHeightFallback) {
    const TerrainGenParams params = FlatSdfFixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    Chunk generated_coarse(IVec3(0, 0, 0));
    Chunk emptied_full(IVec3(0, 0, 0));
    world.GenerateChunkData(generated_coarse, 2);
    world.GenerateChunkData(emptied_full);
    emptied_full.sdf_data.clear();

    Luminumbra::World::MarchingCubes::PolygoniseTerrain(world, generated_coarse, 0.0f, 2);
    Luminumbra::World::MarchingCubes::PolygoniseTerrain(world, emptied_full, 0.0f, 2);

    ASSERT_FALSE(generated_coarse.mesh_indices.empty());
    EXPECT_EQ(HashTerrainMeshBytes(generated_coarse), HashTerrainMeshBytes(emptied_full));
}

TEST(MarchingCubesAuthoritativeSdf, MalformedNeverFallsBackToHeight) {
    const TerrainGenParams params = FlatSdfFixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    Chunk malformed(IVec3(0, 0, 0));
    world.GenerateChunkData(malformed);
    malformed.sdf_data.resize(3u);

    Luminumbra::World::MarchingCubes::PolygoniseTerrain(world, malformed, 0.0f, 2);

    EXPECT_TRUE(malformed.mesh_vertices.empty());
    EXPECT_TRUE(malformed.mesh_indices.empty());
}

TEST(FarLodStoreTest, HeightQuantizationRoundTripsWithinHalfStep) {
    for (float height : {-300.0f, -1.25f, 0.0f, 0.03f, 17.5f, 120.0625f, 950.0f}) {
        const u16 q = QuantizeFarLodHeight(height);
        EXPECT_NEAR(DequantizeFarLodHeight(q), height, 0.5f / kFarLodHeightQuantScale);
    }
    // Extremes clamp instead of wrapping.
    EXPECT_EQ(QuantizeFarLodHeight(-1.0e6f), 0u);
    EXPECT_EQ(QuantizeFarLodHeight(1.0e6f), 65535u);
}

TEST(FarLodStoreTest, SdfQuantizationPreservesSignsAndReservesInvalidSentinel) {
    EXPECT_EQ(QuantizeFarLodSdf(-0.0001f), -1);
    EXPECT_EQ(QuantizeFarLodSdf(0.0001f), 1);
    EXPECT_EQ(QuantizeFarLodSdf(0.0f), 0);
    EXPECT_EQ(QuantizeFarLodSdf(-1.0e6f), -32767);
    EXPECT_EQ(QuantizeFarLodSdf(1.0e6f), 32767);
    EXPECT_NE(QuantizeFarLodSdf(-1.0e6f), kFarLodSdfInvalid);
    EXPECT_EQ(QuantizeFarLodSdf(std::numeric_limits<float>::quiet_NaN()), kFarLodSdfInvalid);
    EXPECT_EQ(QuantizeFarLodSdf(std::numeric_limits<float>::infinity()), kFarLodSdfInvalid);
    EXPECT_NEAR(DequantizeFarLodSdf(static_cast<i16>(-384)), -1.5f, 0.0f);
}

TEST(FarLodStoreTest, ReducesAuthoritativeLatticeByAlignedDecimation) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);
    FarLodSdfSnapshot snapshot = AuthoritativeSdfSnapshot(IVec3(3, 2, 5), 7u);
    snapshot.sdf_data[FullSdfIndex(4, 8, 12)] = 1.25f;
    snapshot.material_data[FullSdfIndex(4, 8, 12)] = 91u;

    FarLodTile f1 = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    std::string error;
    EXPECT_EQ(ReduceChunkSdfIntoFarTile(f1, snapshot, &error), FarLodSdfReduceResult::Inserted)
        << error;
    ASSERT_EQ(f1.sdf_bricks.size(), 1u);
    EXPECT_EQ(f1.sdf_bricks.front().local_chunk_x, 3u);
    EXPECT_EQ(f1.sdf_bricks.front().local_chunk_z, 5u);
    EXPECT_EQ(f1.sdf_bricks.front().chunk_y, 2);
    EXPECT_EQ(f1.sdf_bricks.front().revision, 7u);
    const std::size_t f1_index = ReducedSdfIndex(FarLodTier::F1, 4, 8, 12);
    EXPECT_EQ(f1.sdf_density_q[f1_index], QuantizeFarLodSdf(1.25f));
    EXPECT_EQ(f1.sdf_material[f1_index], 91u);

    FarLodTile f2 = BuildPristineFarLodTile(world, FarLodTier::F2, 0, 0, params_hash);
    snapshot.sdf_data[FullSdfIndex(8, 8, 8)] = 0.5f;
    snapshot.material_data[FullSdfIndex(8, 8, 8)] = 44u;
    EXPECT_EQ(ReduceChunkSdfIntoFarTile(f2, snapshot, &error), FarLodSdfReduceResult::Inserted)
        << error;
    const std::size_t f2_index = ReducedSdfIndex(FarLodTier::F2, 8, 8, 8);
    EXPECT_EQ(f2.sdf_density_q[f2_index], QuantizeFarLodSdf(0.5f));
    EXPECT_EQ(f2.sdf_material[f2_index], 44u);
}

TEST(FarLodSdfMesher, AuthoritativeBricksChangeF1AndF2Geometry) {
    const TerrainGenParams params = FlatSdfFixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);

    for (const FarLodTier tier : {FarLodTier::F1, FarLodTier::F2}) {
        FarLodTile pristine = BuildPristineFarLodTile(world, tier, 0, 0, params_hash);
        FarLodTile edited = pristine;
        const IVec3 center(3, 0, 5);
        AddCompleteFlatSdfHalo(edited, center);
        FarLodSdfSnapshot snapshot = AuthoritativeSdfSnapshot(center, 2u);
        FillFlatSdf(snapshot);
        // This cavity reaches aligned F1/F2 lattice points while leaving the
        // height-only background unchanged.
        for (int z = 4; z <= 12; z += 4) {
            for (int y = 4; y <= 8; y += 4) {
                for (int x = 4; x <= 12; x += 4) {
                    snapshot.sdf_data[FullSdfIndex(x, y, z)] = 1.0f;
                }
            }
        }

        std::string error;
        ASSERT_EQ(ReduceChunkSdfIntoFarTile(edited, snapshot, &error),
                  FarLodSdfReduceResult::Replaced)
            << error;

        FarLodRegionMesh pristine_mesh;
        FarLodRegionMesh edited_mesh;
        Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(pristine, pristine_mesh);
        Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(edited, edited_mesh);

        FarLodRegionMesh repeat_mesh;
        Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(edited, repeat_mesh);

        EXPECT_NE(HashMeshBytes(pristine_mesh), HashMeshBytes(edited_mesh));
        EXPECT_EQ(HashMeshBytes(edited_mesh), HashMeshBytes(repeat_mesh));
        EXPECT_TRUE(std::any_of(
            edited_mesh.vertices.begin(),
            edited_mesh.vertices.end(),
            [](const Luminumbra::VoxelVertex& vertex) { return vertex.position.y < 10.0f; }));
    }
}

TEST(FarLodSdfMesher, IncompleteHaloAndMismatchedSharedFacesFailClosed) {
    const TerrainGenParams params = FlatSdfFixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);
    const IVec3 center(3, 0, 5);

    FarLodTile incomplete = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    FarLodSdfSnapshot single = AuthoritativeSdfSnapshot(center, 1u);
    FillFlatSdf(single);
    std::string error;
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(incomplete, single, &error),
              FarLodSdfReduceResult::Inserted)
        << error;
    FarLodRegionMesh incomplete_mesh;
    const auto incomplete_stats =
        Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(incomplete, incomplete_mesh);
    EXPECT_TRUE(incomplete_mesh.vertices.empty());
    EXPECT_TRUE(incomplete_mesh.indices.empty());
    EXPECT_EQ(incomplete_stats.triangles, 0u);

    FarLodTile mismatched = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    AddCompleteFlatSdfHalo(mismatched, center);
    FarLodSdfSnapshot authoritative = AuthoritativeSdfSnapshot(center, 2u);
    FillFlatSdf(authoritative);
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(mismatched, authoritative, &error),
              FarLodSdfReduceResult::Replaced)
        << error;

    const auto right = std::find_if(mismatched.sdf_bricks.begin(),
                                    mismatched.sdf_bricks.end(),
                                    [](const FarLodSdfBrickDescriptor& brick) {
                                        return brick.local_chunk_x == 4u &&
                                               brick.local_chunk_z == 5u && brick.chunk_y == 0;
                                    });
    ASSERT_NE(right, mismatched.sdf_bricks.end());
    const std::size_t right_index = static_cast<std::size_t>(right - mismatched.sdf_bricks.begin());
    const u32 side = FarLodSdfBrickSamplesPerSide(mismatched.tier);
    mismatched.sdf_density_q[right_index * FarLodSdfBrickSampleCount(mismatched.tier)] =
        QuantizeFarLodSdf(-11.0f); // differs from the shared x=16 face value (-12)
    ASSERT_EQ(side, 5u);

    FarLodRegionMesh mismatched_mesh;
    const auto mismatched_stats =
        Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(mismatched, mismatched_mesh);
    EXPECT_TRUE(mismatched_mesh.vertices.empty());
    EXPECT_TRUE(mismatched_mesh.indices.empty());
    EXPECT_EQ(mismatched_stats.triangles, 0u);
}

TEST(FarLodSdfMesher, MalformedCompleteStacksFailClosedBeforeVertexEmission) {
    const TerrainGenParams params = FlatSdfFixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);
    const IVec3 center(3, 0, 5);
    const auto complete_tile = [&]() {
        FarLodTile tile = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
        AddCompleteFlatSdfHalo(tile, center);
        FarLodSdfSnapshot authoritative = AuthoritativeSdfSnapshot(center, 2u);
        FillFlatSdf(authoritative);
        std::string error;
        EXPECT_EQ(ReduceChunkSdfIntoFarTile(tile, authoritative, &error),
                  FarLodSdfReduceResult::Replaced)
            << error;
        return tile;
    };
    const auto expect_empty = [](const FarLodTile& tile) {
        FarLodRegionMesh mesh;
        const auto stats = Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(tile, mesh);
        EXPECT_TRUE(mesh.vertices.empty());
        EXPECT_TRUE(mesh.indices.empty());
        EXPECT_EQ(stats.triangles, 0u);
    };

    FarLodTile bad_crc = complete_tile();
    bad_crc.sdf_bricks.front().payload_crc32 ^= 1u;
    expect_empty(bad_crc);

    FarLodTile missing_vertical = complete_tile();
    const auto missing = std::find_if(missing_vertical.sdf_bricks.begin(),
                                      missing_vertical.sdf_bricks.end(),
                                      [center](const FarLodSdfBrickDescriptor& brick) {
                                          return brick.local_chunk_x == center.x &&
                                                 brick.local_chunk_z == center.z &&
                                                 brick.chunk_y == -1;
                                      });
    ASSERT_NE(missing, missing_vertical.sdf_bricks.end());
    const std::size_t missing_index =
        static_cast<std::size_t>(missing - missing_vertical.sdf_bricks.begin());
    const std::size_t samples = FarLodSdfBrickSampleCount(missing_vertical.tier);
    missing_vertical.sdf_bricks.erase(missing);
    missing_vertical.sdf_density_q.erase(
        missing_vertical.sdf_density_q.begin() + missing_index * samples,
        missing_vertical.sdf_density_q.begin() + (missing_index + 1u) * samples);
    missing_vertical.sdf_material.erase(
        missing_vertical.sdf_material.begin() + missing_index * samples,
        missing_vertical.sdf_material.begin() + (missing_index + 1u) * samples);
    expect_empty(missing_vertical);

    FarLodTile unsorted = complete_tile();
    ASSERT_GE(unsorted.sdf_bricks.size(), 2u);
    std::swap(unsorted.sdf_bricks[0], unsorted.sdf_bricks[1]);
    std::swap_ranges(unsorted.sdf_density_q.begin(),
                     unsorted.sdf_density_q.begin() + samples,
                     unsorted.sdf_density_q.begin() + samples);
    std::swap_ranges(unsorted.sdf_material.begin(),
                     unsorted.sdf_material.begin() + samples,
                     unsorted.sdf_material.begin() + samples);
    expect_empty(unsorted);

    FarLodTile wrong_size = complete_tile();
    wrong_size.sdf_density_q.pop_back();
    expect_empty(wrong_size);

    FarLodTile invalid_tier = complete_tile();
    invalid_tier.tier = static_cast<FarLodTier>(99);
    expect_empty(invalid_tier);
}

TEST(FarLodStoreTest, RejectsMalformedSdfAndKeepsExistingBrick) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);
    FarLodTile tile = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    FarLodSdfSnapshot snapshot = AuthoritativeSdfSnapshot(IVec3(0, 1, 0), 1u);
    std::string error;
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(tile, snapshot, &error), FarLodSdfReduceResult::Inserted);
    const u64 before = ComputeFarLodTileHash(tile);

    snapshot.revision = 2u;
    snapshot.sdf_data[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(ReduceChunkSdfIntoFarTile(tile, snapshot, &error), FarLodSdfReduceResult::Error);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(ComputeFarLodTileHash(tile), before);
}

TEST(FarLodStoreTest, BrickUpsertsUseCanonicalOrderAndRejectDuplicateStreams) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);
    FarLodTile tile = BuildPristineFarLodTile(world, FarLodTier::F1, -1, -1, params_hash);
    FarLodSdfSnapshot later = AuthoritativeSdfSnapshot(IVec3(-23, 1, -25), 1u);
    FarLodSdfSnapshot same_column = AuthoritativeSdfSnapshot(IVec3(-30, 3, -28), 1u);
    FarLodSdfSnapshot earlier = AuthoritativeSdfSnapshot(IVec3(-30, -3, -28), 1u);
    later.sdf_data[0] = -3.0f;
    same_column.sdf_data[0] = -2.0f;
    earlier.sdf_data[0] = -1.0f;
    std::string error;

    ASSERT_EQ(ReduceChunkSdfIntoFarTile(tile, later, &error), FarLodSdfReduceResult::Inserted);
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(tile, same_column, &error),
              FarLodSdfReduceResult::Inserted);
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(tile, earlier, &error), FarLodSdfReduceResult::Inserted);
    ASSERT_EQ(tile.sdf_bricks.size(), 3u);
    EXPECT_EQ(tile.sdf_bricks[0].local_chunk_z, 4u);
    EXPECT_EQ(tile.sdf_bricks[0].local_chunk_x, 2u);
    EXPECT_EQ(tile.sdf_bricks[0].chunk_y, -3);
    EXPECT_EQ(tile.sdf_bricks[1].local_chunk_z, 4u);
    EXPECT_EQ(tile.sdf_bricks[1].local_chunk_x, 2u);
    EXPECT_EQ(tile.sdf_bricks[1].chunk_y, 3);
    EXPECT_EQ(tile.sdf_bricks[2].local_chunk_z, 7u);
    EXPECT_EQ(tile.sdf_bricks[2].local_chunk_x, 9u);
    const std::size_t samples = FarLodSdfBrickSampleCount(tile.tier);
    EXPECT_EQ(tile.sdf_density_q[0], QuantizeFarLodSdf(-1.0f));
    EXPECT_EQ(tile.sdf_density_q[samples], QuantizeFarLodSdf(-2.0f));
    EXPECT_EQ(tile.sdf_density_q[2u * samples], QuantizeFarLodSdf(-3.0f));

    const u32 persisted_revision = tile.sdf_bricks[0].revision;
    FarLodSdfSnapshot post_reload_edit = earlier;
    post_reload_edit.revision = 0u;
    post_reload_edit.sdf_data[0] = 4.0f;
    EXPECT_EQ(ReduceChunkSdfIntoFarTile(tile, post_reload_edit, &error),
              FarLodSdfReduceResult::Replaced);
    EXPECT_GT(tile.sdf_bricks[0].revision, persisted_revision);
    const u32 advanced_revision = tile.sdf_bricks[0].revision;
    post_reload_edit.sdf_data[0] = 5.0f;
    EXPECT_EQ(ReduceChunkSdfIntoFarTile(tile, post_reload_edit, &error),
              FarLodSdfReduceResult::Replaced);
    EXPECT_GT(tile.sdf_bricks[0].revision, advanced_revision);

    // A second descriptor with the same (z, x, y) key is not a valid
    // persisted stream, even when its payload is otherwise well formed.
    FarLodTile duplicate = tile;
    duplicate.sdf_bricks.push_back(duplicate.sdf_bricks.back());
    const std::vector<i16> duplicate_density(duplicate.sdf_density_q.end() - samples,
                                             duplicate.sdf_density_q.end());
    const std::vector<u8> duplicate_material(duplicate.sdf_material.end() - samples,
                                             duplicate.sdf_material.end());
    duplicate.sdf_density_q.insert(
        duplicate.sdf_density_q.end(), duplicate_density.begin(), duplicate_density.end());
    duplicate.sdf_material.insert(
        duplicate.sdf_material.end(), duplicate_material.begin(), duplicate_material.end());

    TempSaveDir save_dir("duplicate_brick");
    const FarLodStore store(save_dir.path);
    std::vector<std::string> errors;
    EXPECT_FALSE(store.save_tile(duplicate, &errors));
    EXPECT_FALSE(errors.empty());
}

TEST(FarLodStoreTest, LegacyHeightRecordsRefusedWithoutPartialTileOrWrites) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 hash = ComputeTerrainParamsHash(params, kFixtureSeed);
    for (bool edited : {false, true}) {
        TempSaveDir dir("old_height");
        auto tile = BuildPristineFarLodTile(world, FarLodTier::F2, -1, 2, hash);
        tile.edited = edited;
        WorldSaveService::ContainerRecord record;
        record.id = FarLodStore::tile_record_id(tile.tier, tile.rx, tile.rz);
        record.lod_level = static_cast<u8>(tile.tier);
        record.flags = edited ? 1u : 0u;
        AppendLegacyTilePayload(record.payload, tile);
        const auto path = WorldSaveService::region_file_path(dir.path, -1, 2);
        ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
        const auto bytes = ReadTestFileBytes(path);
        FarLodTile loaded = tile;
        std::vector<std::string> errors;
        EXPECT_FALSE(FarLodStore(dir.path).load_tile(tile.tier, -1, 2, hash, loaded, &errors));
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_EQ(errors.front(), WorldSaveService::kObsoleteWorldMessage);
        EXPECT_TRUE(loaded.height_q.empty());
        EXPECT_FALSE(FarLodStore(dir.path).save_tile(tile));
        EXPECT_EQ(ReadTestFileBytes(path), bytes);
    }
}

TEST(FarLodStoreTest, MalformedPristineAndAuthorityAreHardErrors) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);

    const auto corrupt_payload =
        [](const std::filesystem::path& save_dir, FarLodTier tier, i32 rx, i32 rz) {
            std::vector<WorldSaveService::ContainerRecord> records;
            std::vector<std::string> io_errors;
            EXPECT_TRUE(WorldSaveService::read_container_records(
                WorldSaveService::region_file_path(save_dir, rx, rz), records, &io_errors));
            EXPECT_TRUE(io_errors.empty());
            ASSERT_EQ(records.size(), 1u);
            EXPECT_EQ(records.front().id, FarLodStore::tile_record_id(tier, rx, rz));
            records.front().payload.resize(4u); // FSD2 magic without its required header.
            EXPECT_TRUE(WorldSaveService::upsert_container_records(
                WorldSaveService::region_file_path(save_dir, rx, rz), records, &io_errors));
            EXPECT_TRUE(io_errors.empty());
        };

    TempSaveDir pristine_dir("malformed_pristine");
    const FarLodStore pristine_store(pristine_dir.path);
    std::vector<std::string> errors;
    ASSERT_TRUE(pristine_store.save_tile(
        BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash), &errors));
    corrupt_payload(pristine_dir.path, FarLodTier::F1, 0, 0);
    FarLodTile pristine_loaded;
    errors.clear();
    EXPECT_FALSE(
        pristine_store.load_tile(FarLodTier::F1, 0, 0, params_hash, pristine_loaded, &errors));
    EXPECT_FALSE(errors.empty());
    errors.clear();

    TempSaveDir authoritative_dir("malformed_authority");
    const FarLodStore authoritative_store(authoritative_dir.path);
    FarLodTile authoritative = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    std::string reduction_error;
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(
                  authoritative, AuthoritativeSdfSnapshot(IVec3(0, 1, 0), 1u), &reduction_error),
              FarLodSdfReduceResult::Inserted);
    ASSERT_TRUE(authoritative_store.save_tile(authoritative, &errors));
    corrupt_payload(authoritative_dir.path, FarLodTier::F1, 0, 0);
    FarLodTile authoritative_loaded;
    errors.clear();
    EXPECT_FALSE(authoritative_store.load_tile(
        FarLodTier::F1, 0, 0, params_hash, authoritative_loaded, &errors));
    EXPECT_FALSE(errors.empty());
}

TEST(FarLodStoreTest, Fsd2AuthorityRejectsEveryMalformedStreamClass) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);
    FarLodTile authority = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    std::string reduction_error;
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(
                  authority, AuthoritativeSdfSnapshot(IVec3(0, 0, 0), 1u), &reduction_error),
              FarLodSdfReduceResult::Inserted);
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(
                  authority, AuthoritativeSdfSnapshot(IVec3(1, 0, 0), 1u), &reduction_error),
              FarLodSdfReduceResult::Inserted);
    const Fsd2TestLayout layout = Fsd2LayoutFor(authority);

    const auto expect_hard_failure = [&](const std::string& tag, const auto& mutate) {
        TempSaveDir save_dir("fsd2_" + tag);
        FarLodStore store(save_dir.path);
        std::vector<std::string> errors;
        ASSERT_TRUE(store.save_tile(authority, &errors));
        ASSERT_TRUE(errors.empty());
        std::vector<WorldSaveService::ContainerRecord> records;
        ASSERT_TRUE(WorldSaveService::read_container_records(
            WorldSaveService::region_file_path(save_dir.path, 0, 0), records, &errors));
        ASSERT_EQ(records.size(), 1u);
        mutate(records.front().payload);
        ASSERT_TRUE(WorldSaveService::upsert_container_records(
            WorldSaveService::region_file_path(save_dir.path, 0, 0), records, &errors));
        FarLodTile untouched;
        untouched.rx = 777;
        errors.clear();
        EXPECT_FALSE(store.load_tile(FarLodTier::F1, 0, 0, params_hash, untouched, &errors)) << tag;
        EXPECT_FALSE(errors.empty()) << tag;
        EXPECT_TRUE(untouched.height_q.empty()) << tag;
    };

    expect_hard_failure("truncated_header", [](std::string& payload) { payload.resize(4u); });
    expect_hard_failure("truncated_stream", [layout](std::string& payload) {
        payload.resize(layout.density_stream + 1u);
    });
    expect_hard_failure("invalid_count", [layout](std::string& payload) {
        WriteTestValue<u32>(payload, layout.height_count, 0u);
    });
    expect_hard_failure("invalid_tier", [layout](std::string& payload) {
        payload[layout.tier] = static_cast<char>(99);
    });
    expect_hard_failure("duplicate_key", [layout](std::string& payload) {
        constexpr std::size_t kDescriptorBytes = 16u;
        const std::size_t first = layout.descriptor_stream;
        const std::size_t second = first + kDescriptorBytes;
        payload[second] = payload[first];
        payload[second + 1u] = payload[first + 1u];
        i32 first_y = 0;
        std::memcpy(&first_y, payload.data() + first + 4u, sizeof(first_y));
        WriteTestValue<i32>(payload, second + 4u, first_y);
    });
    expect_hard_failure("reserved_density", [layout](std::string& payload) {
        WriteTestValue<i16>(payload, layout.density_stream, kFarLodSdfInvalid);
    });
    expect_hard_failure("crc_mismatch", [layout](std::string& payload) {
        i16 density = 0;
        std::memcpy(&density, payload.data() + layout.density_stream, sizeof(density));
        density = static_cast<i16>(density + 1);
        WriteTestValue<i16>(payload, layout.density_stream, density);
    });
    expect_hard_failure("trailing_bytes", [](std::string& payload) { payload.push_back('\0'); });
}

TEST(FarLodStoreTest, RegenerableBrickIsCacheMissAcrossParamsHashChange) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);
    FarLodTile tile = BuildPristineFarLodTile(world, FarLodTier::F2, 0, 0, params_hash);
    FarLodSdfSnapshot snapshot = AuthoritativeSdfSnapshot(IVec3(0, 0, 0), 1u);
    snapshot.source_kind = FarLodBrickSourceKind::RegenerableCache;
    std::string reduction_error;
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(tile, snapshot, &reduction_error),
              FarLodSdfReduceResult::Inserted);
    EXPECT_FALSE(tile.edited);

    TempSaveDir save_dir("regenerable_mismatch");
    FarLodStore store(save_dir.path);
    std::vector<std::string> errors;
    ASSERT_TRUE(store.save_tile(tile, &errors));
    FarLodTile out;
    EXPECT_FALSE(store.load_tile(FarLodTier::F2, 0, 0, params_hash + 1u, out, &errors));
    EXPECT_TRUE(errors.empty());
}

TEST(FarLodStoreTest, SaveRejectsInvalidTier) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    FarLodTile tile = BuildPristineFarLodTile(
        world, FarLodTier::F1, 0, 0, ComputeTerrainParamsHash(params, kFixtureSeed));
    tile.tier = static_cast<FarLodTier>(99);
    TempSaveDir save_dir("invalid_tier");
    std::vector<std::string> errors;
    EXPECT_FALSE(FarLodStore(save_dir.path).save_tile(tile, &errors));
    EXPECT_FALSE(errors.empty());
    errors.clear();
    FarLodTile out;
    EXPECT_FALSE(
        FarLodStore(save_dir.path).load_tile(static_cast<FarLodTier>(99), 0, 0, 0u, out, &errors));
    EXPECT_FALSE(errors.empty());
}

TEST(FarLodStoreTest, TierGeometryMatchesPinnedNumbers) {
    // F1 uses 4 m samples, F2 uses 8 m samples, and each region spans 512 m.
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
    const SHIELD_WorldSystem reference(nullptr, nullptr, params, kFixtureSeed);
    const auto replay = BuildPristineFarLodTile(reference, FarLodTier::F1, 0, 0, params_hash);
    EXPECT_EQ(ComputeFarLodTileHash(first), ComputeFarLodTileHash(replay));
    // v0.3.0 FSD3 metadata now hashes zero-brick tiles on every compiler.
    // Re-pinned from measured terrain bytes plus the current payload metadata.
    EXPECT_EQ(ComputeFarLodTileHash(first),
              ToolchainHash(0xe05d0200d292d48cull, 0xa217e66441cc964bull))
        << "pristine far-LOD tile bytes changed";
    std::printf("farlod fixture tile hash (seed %d, F1, r0.0): %016llx\n",
                kFixtureSeed,
                static_cast<unsigned long long>(ComputeFarLodTileHash(first)));

    // Different regions and tiers hash differently.
    const FarLodTile other_region =
        BuildPristineFarLodTile(world, FarLodTier::F1, 1, 0, params_hash);
    EXPECT_NE(ComputeFarLodTileHash(first), ComputeFarLodTileHash(other_region));
    const FarLodTile f2 = BuildPristineFarLodTile(world, FarLodTier::F2, 0, 0, params_hash);
    EXPECT_EQ(f2.samples_per_side, 65u);
    EXPECT_NE(ComputeFarLodTileHash(first), ComputeFarLodTileHash(f2));

    // Tiles never carry the non-rendering Air material.
    for (Luminumbra::u8 material : first.material) {
        EXPECT_NE(material, static_cast<Luminumbra::u8>(MaterialType::Air));
    }
}

TEST(FarLodStoreTest, ZeroBrickHashIncludesCurrentMetadata) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const auto tile = BuildPristineFarLodTile(
        world, FarLodTier::F1, 0, 0, ComputeTerrainParamsHash(params, kFixtureSeed));
    auto changed = tile;
    ++changed.params_hash;
    EXPECT_NE(ComputeFarLodTileHash(tile), ComputeFarLodTileHash(changed));
    changed = tile;
    changed.edited = true;
    EXPECT_NE(ComputeFarLodTileHash(tile), ComputeFarLodTileHash(changed));
    changed = tile;
    auto snapshot = AuthoritativeSdfSnapshot(IVec3(0, 0, 0), 1u);
    snapshot.sdf_data.clear();
    EXPECT_EQ(ReduceChunkSdfIntoFarTile(changed, snapshot), FarLodSdfReduceResult::Error);
    EXPECT_TRUE(changed.sdf_bricks.empty());
}

TEST(FarLodStoreTest, EditedSdfRebuildMarksTileAuthoritative) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    auto tile = BuildPristineFarLodTile(
        world, FarLodTier::F1, 0, 0, ComputeTerrainParamsHash(params, kFixtureSeed));
    const auto pristine_hash = ComputeFarLodTileHash(tile);
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(tile, AuthoritativeSdfSnapshot(IVec3(2, 1, 2), 1u)),
              FarLodSdfReduceResult::Inserted);
    EXPECT_TRUE(tile.edited);
    ASSERT_EQ(tile.sdf_bricks.size(), 1u);
    EXPECT_EQ(tile.sdf_bricks.front().source_kind, FarLodBrickSourceKind::Authoritative);
    EXPECT_NE(ComputeFarLodTileHash(tile), pristine_hash);
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

TEST(FarLodStoreTest, InterruptedFarRewriteLeavesPriorRegionCompleteAndRetryPreservesOtherRecords) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);

    TempSaveDir save_dir("atomic_far_rewrite");
    const FarLodStore store(save_dir.path);
    const FarLodTile original = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    const FarLodTile untargeted = BuildPristineFarLodTile(world, FarLodTier::F2, 0, 0, params_hash);
    std::vector<std::string> errors;
    ASSERT_TRUE(store.save_tile(original, &errors));
    ASSERT_TRUE(store.save_tile(untargeted, &errors));
    ASSERT_TRUE(errors.empty());

    const std::filesystem::path region = WorldSaveService::region_file_path(save_dir.path, 0, 0);
    const std::string prior_region_bytes = ReadTestFileBytes(region);
    ASSERT_FALSE(prior_region_bytes.empty());

    std::vector<WorldSaveService::ContainerRecord> records_before;
    ASSERT_TRUE(WorldSaveService::read_container_records(region, records_before, &errors));
    const auto untargeted_before = std::find_if(
        records_before.begin(),
        records_before.end(),
        [id = FarLodStore::tile_record_id(FarLodTier::F2, 0, 0)](
            const WorldSaveService::ContainerRecord& record) {
            return record.lod_level == static_cast<u8>(FarLodTier::F2) && record.id == id;
        });
    ASSERT_NE(untargeted_before, records_before.end());
    const std::string untargeted_payload_before = untargeted_before->payload;
    const u8 untargeted_flags_before = untargeted_before->flags;

    FarLodTile updated = original;
    ASSERT_FALSE(updated.height_q.empty());
    ++updated.height_q[0];
    ASSERT_NE(ComputeFarLodTileHash(updated), ComputeFarLodTileHash(original));

    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool interrupted = store.save_tile(updated, &errors);
    WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(interrupted);
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(ReadTestFileBytes(region), prior_region_bytes);
    EXPECT_FALSE(HasRegionTemporaryFile(region));

    // Both old far records remain complete and readable after the simulated
    // interruption; no partially-written FSD2 payload can become visible.
    errors.clear();
    FarLodTile loaded_original;
    FarLodTile loaded_untargeted;
    ASSERT_TRUE(store.load_tile(FarLodTier::F1, 0, 0, params_hash, loaded_original, &errors));
    ASSERT_TRUE(store.load_tile(FarLodTier::F2, 0, 0, params_hash, loaded_untargeted, &errors));
    ASSERT_TRUE(errors.empty());
    EXPECT_EQ(ComputeFarLodTileHash(loaded_original), ComputeFarLodTileHash(original));
    EXPECT_EQ(ComputeFarLodTileHash(loaded_untargeted), ComputeFarLodTileHash(untargeted));

    // The successful retry changes only F1 and keeps the co-resident F2
    // record exactly unchanged at the uncompressed container boundary.
    ASSERT_TRUE(store.save_tile(updated, &errors));
    ASSERT_TRUE(errors.empty());
    EXPECT_NE(ReadTestFileBytes(region), prior_region_bytes);

    std::vector<WorldSaveService::ContainerRecord> records_after;
    ASSERT_TRUE(WorldSaveService::read_container_records(region, records_after, &errors));
    const auto untargeted_after = std::find_if(
        records_after.begin(),
        records_after.end(),
        [id = FarLodStore::tile_record_id(FarLodTier::F2, 0, 0)](
            const WorldSaveService::ContainerRecord& record) {
            return record.lod_level == static_cast<u8>(FarLodTier::F2) && record.id == id;
        });
    ASSERT_NE(untargeted_after, records_after.end());
    EXPECT_EQ(untargeted_after->payload, untargeted_payload_before);
    EXPECT_EQ(untargeted_after->flags, untargeted_flags_before);

    FarLodTile committed;
    loaded_untargeted = FarLodTile{};
    ASSERT_TRUE(store.load_tile(FarLodTier::F1, 0, 0, params_hash, committed, &errors));
    ASSERT_TRUE(store.load_tile(FarLodTier::F2, 0, 0, params_hash, loaded_untargeted, &errors));
    EXPECT_EQ(ComputeFarLodTileHash(committed), ComputeFarLodTileHash(updated));
    EXPECT_EQ(ComputeFarLodTileHash(loaded_untargeted), ComputeFarLodTileHash(untargeted));
}

TEST(FarLodStoreTest, AuthoritativeSdfBrickSurvivesPersistenceAndParamsMismatch) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);
    const u64 other_params_hash = ComputeTerrainParamsHash(params, kFixtureSeed + 11);
    FarLodTile tile = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    FarLodSdfSnapshot snapshot = AuthoritativeSdfSnapshot(IVec3(4, -2, 7), 19u);
    snapshot.sdf_data[FullSdfIndex(8, 8, 8)] = 0.75f;
    std::string reduction_error;
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(tile, snapshot, &reduction_error),
              FarLodSdfReduceResult::Inserted)
        << reduction_error;

    TempSaveDir save_dir("sdf_roundtrip");
    const FarLodStore store(save_dir.path);
    std::vector<std::string> errors;
    ASSERT_TRUE(store.save_tile(tile, &errors));
    ASSERT_TRUE(errors.empty());

    FarLodTile loaded;
    ASSERT_TRUE(store.load_tile(FarLodTier::F1, 0, 0, other_params_hash, loaded, &errors));
    EXPECT_TRUE(errors.empty());
    ASSERT_EQ(loaded.sdf_bricks.size(), 1u);
    EXPECT_EQ(loaded.sdf_bricks.front().source_kind, FarLodBrickSourceKind::Authoritative);
    EXPECT_EQ(loaded.sdf_bricks.front().revision, 19u);
    EXPECT_EQ(loaded.sdf_density_q, tile.sdf_density_q);
    EXPECT_EQ(loaded.sdf_material, tile.sdf_material);
    EXPECT_EQ(ComputeFarLodTileHash(loaded), ComputeFarLodTileHash(tile));
}

TEST(FarLodStoreTest, PostReloadLowerChunkRevisionStillReplacesPersistedAuthority) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kFixtureSeed);
    const u64 params_hash = ComputeTerrainParamsHash(params, kFixtureSeed);
    FarLodTile tile = BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, params_hash);
    FarLodSdfSnapshot before_restart = AuthoritativeSdfSnapshot(IVec3(4, 0, 7), 3u);
    std::string reduction_error;
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(tile, before_restart, &reduction_error),
              FarLodSdfReduceResult::Inserted);

    TempSaveDir save_dir("revision_restart");
    FarLodStore store(save_dir.path);
    std::vector<std::string> errors;
    ASSERT_TRUE(store.save_tile(tile, &errors));
    FarLodTile loaded;
    ASSERT_TRUE(store.load_tile(FarLodTier::F1, 0, 0, params_hash, loaded, &errors));
    ASSERT_EQ(loaded.sdf_bricks.front().revision, 3u);

    FarLodSdfSnapshot after_restart_edit = before_restart;
    after_restart_edit.revision = 2u;
    after_restart_edit.sdf_data[0] = 4.0f;
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(loaded, after_restart_edit, &reduction_error),
              FarLodSdfReduceResult::Replaced)
        << reduction_error;
    EXPECT_GT(loaded.sdf_bricks.front().revision, 3u);
    EXPECT_EQ(loaded.sdf_density_q.front(), QuantizeFarLodSdf(4.0f));
    ASSERT_TRUE(store.save_tile(loaded, &errors));
    FarLodTile reloaded;
    ASSERT_TRUE(store.load_tile(FarLodTier::F1, 0, 0, params_hash, reloaded, &errors));
    EXPECT_EQ(reloaded.sdf_density_q.front(), QuantizeFarLodSdf(4.0f));
    EXPECT_EQ(reloaded.sdf_bricks.front().revision, loaded.sdf_bricks.front().revision);
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
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(edited, AuthoritativeSdfSnapshot(chunk.get_coords(), 1u)),
              FarLodSdfReduceResult::Inserted);
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
    const auto stats_first =
        Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(tile, first);
    FarLodRegionMesh second;
    Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(tile, second);

    // 28-byte VoxelVertex layout untouched (meshing determinism contract).
    static_assert(sizeof(Luminumbra::VoxelVertex) == 28, "VoxelVertex layout must stay 28 bytes");

    EXPECT_EQ(HashMeshBytes(first), HashMeshBytes(second));
    // Re-pinned for cave_style/shaping_enabled retirement; each compiler remains gated.
    EXPECT_EQ(HashMeshBytes(first), ToolchainHash(0x9134d309929f8b26ull, 0x7291723bda29eb0cull))
        << "pristine F1 mesh bytes changed";
    std::printf("farlod fixture region mesh hash (seed %d, F1, r0.0): %016llx\n",
                kFixtureSeed,
                static_cast<unsigned long long>(HashMeshBytes(first)));
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
    const auto stats_f2 =
        Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(tile_f2, mesh_f2);
    FarLodRegionMesh mesh_f2_repeat;
    Luminumbra::World::MarchingCubes::GenerateFarLodRegionMesh(tile_f2, mesh_f2_repeat);
    std::printf("farlod fixture region mesh hash (seed %d, F2, r0.0): %016llx\n",
                kFixtureSeed,
                static_cast<unsigned long long>(HashMeshBytes(mesh_f2)));
    EXPECT_EQ(stats_f2.skirt_quads, 256u);
    EXPECT_EQ(mesh_f2.vertices.size(), 65u * 65u + 256u * 4u);
    EXPECT_EQ(HashMeshBytes(mesh_f2), HashMeshBytes(mesh_f2_repeat))
        << "zero-brick F2 mesh bytes must remain deterministic";
    // Re-pinned for cave_style/shaping_enabled retirement; each compiler remains gated.
    EXPECT_EQ(HashMeshBytes(mesh_f2), ToolchainHash(0x48568b20f7fdbca1ull, 0x48568b20f7fdbca1ull))
        << "zero-brick F2 mesh bytes changed";
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

// Shaping controls always contribute to terrain identity after selector retirement.
// Spline content/count and every frequency must invalidate pristine tiles.
TEST(FarLodStore, TerrainParamsHashShapingFold) {
    auto make = []() {
        TerrainGenParams p;
        p.base_frequency = 0.008f;
        p.base_amplitude = 60.0f;
        p.octaves = 5;
        p.persistence = 0.55f;
        p.lacunarity = 2.1f;
        p.height_offset = 12.0f;
        p.continentalness_frequency = 0.0008f;
        p.erosion_frequency = 0.0015f;
        p.peaks_frequency = 0.004f;
        p.peaks_amplitude = 90.0f;
        p.domain_warp_amplitude = 30.0f;
        p.domain_warp_frequency = 0.006f;
        p.continental_spline = {{-1.0f, -40.0f}, {0.0f, 0.0f}, {1.0f, 40.0f}};
        p.erosion_spline = {{-1.0f, 1.0f}, {1.0f, 0.1f}};
        p.peaks_spline = {{-1.0f, 0.0f}, {1.0f, 1.0f}};
        return p;
    };
    const int seed = 424242;
    const TerrainGenParams on = make();
    EXPECT_EQ(ComputeTerrainParamsHash(on, seed), ComputeTerrainParamsHash(make(), seed));

    // Even default params without authored splines use and hash shaping controls.
    const TerrainGenParams defaults;
    TerrainGenParams changed = defaults;
    changed.domain_warp_amplitude += 1.0f;
    EXPECT_NE(ComputeTerrainParamsHash(defaults, seed), ComputeTerrainParamsHash(changed, seed));

    // Shaping: a spline control-point change re-keys the hash.
    TerrainGenParams on_spline = on;
    on_spline.peaks_spline = {{-1.0f, 0.0f}, {1.0f, 0.9f}};
    EXPECT_NE(ComputeTerrainParamsHash(on, seed), ComputeTerrainParamsHash(on_spline, seed))
        << "shaping spline content not hashed";

    // Shaping: a frequency change re-keys the hash.
    TerrainGenParams on_freq = on;
    on_freq.erosion_frequency = on.erosion_frequency * 2.0f;
    EXPECT_NE(ComputeTerrainParamsHash(on, seed), ComputeTerrainParamsHash(on_freq, seed))
        << "shaping frequency not hashed";

    // Shaping: spline COUNT matters (the count prefix prevents merge collisions).
    TerrainGenParams on_count = on;
    on_count.continental_spline = {{-1.0f, -40.0f}, {0.0f, 0.0f}, {1.0f, 40.0f}, {0.5f, 20.0f}};
    EXPECT_NE(ComputeTerrainParamsHash(on, seed), ComputeTerrainParamsHash(on_count, seed))
        << "spline count not hashed";
}

TEST(FarLodStore, TerrainParamsHashTracksNoiseRouterWhenCavesEnabled) {
    const TerrainGenParams defaults;
    constexpr int seed = 424242;
    for (float TerrainGenParams::*control : {&TerrainGenParams::spaghetti_frequency,
                                             &TerrainGenParams::spaghetti_thickness,
                                             &TerrainGenParams::worley_frequency,
                                             &TerrainGenParams::worley_threshold}) {
        TerrainGenParams changed = defaults;
        changed.*control += 0.01f;
        EXPECT_NE(ComputeTerrainParamsHash(defaults, seed),
                  ComputeTerrainParamsHash(changed, seed));
        TerrainGenParams disabled = defaults;
        disabled.caves_enabled = false;
        changed.caves_enabled = false;
        EXPECT_EQ(ComputeTerrainParamsHash(disabled, seed),
                  ComputeTerrainParamsHash(changed, seed));
    }
}

TEST(FarLodStore, TerrainParamsHashTracksHydraulicKernel) {
    TerrainGenParams disabled = FixtureParams();
    TerrainGenParams disabled_changed = disabled;
    disabled_changed.hydro_iterations += 1;
    disabled_changed.hydro_max_offset += 1.0f;

    constexpr int seed = 424242;
    EXPECT_EQ(ComputeTerrainParamsHash(disabled, seed),
              ComputeTerrainParamsHash(disabled_changed, seed));

    TerrainGenParams enabled = disabled;
    enabled.hydro_enabled = true;
    TerrainGenParams enabled_changed = enabled;
    enabled_changed.hydro_iterations += 1;
    EXPECT_NE(ComputeTerrainParamsHash(enabled, seed),
              ComputeTerrainParamsHash(enabled_changed, seed));

    // Re-pinned for unconditional shaping after cave_style/shaping_enabled retirement.
    EXPECT_EQ(ComputeTerrainParamsHash(enabled, seed), 0x47800606d5b30551ull);
}
