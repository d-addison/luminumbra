#include "gtest/gtest.h"

#include "luminumbra_client/rendering/FarLodSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/MarchingCubes.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace Luminumbra;
using namespace Luminumbra::Rendering;
using namespace Luminumbra::Systems;
using namespace Luminumbra::World;

struct TempSaveDir {
    TempSaveDir() {
        path = std::filesystem::temp_directory_path() /
            ("luminumbra_far_worker_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempSaveDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

TerrainGenParams FlatParams() {
    TerrainGenParams params;
    params.base_frequency = 0.005f;
    params.base_amplitude = 0.0f;
    params.height_offset = 12.0f;
    params.caves_enabled = false;
    params.island_mask_enabled = false;
    return params;
}

u64 HashMesh(const FarLodRegionMesh& mesh) {
    u64 hash = 14695981039346656037ull;
    const auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= static_cast<u64>(bytes[i]);
            hash *= 1099511628211ull;
        }
    };
    for (const VoxelVertex& vertex : mesh.vertices) {
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

TEST(FarLodWorker, ZeroAuthorityMatchesPristinePath) {
    const TerrainGenParams params = FlatParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    const auto snapshot = world.capture_far_lod_sdf_snapshot(0, 0);
    ASSERT_TRUE(snapshot);
    ASSERT_TRUE(snapshot->entries.empty());

    const auto outcome = BuildFarLodWorkerTile(
        world, *snapshot, FarLodTier::F1, 0, 0, {});
    ASSERT_TRUE(outcome.ok) << outcome.error;
    EXPECT_TRUE(outcome.tile.sdf_bricks.empty());

    const FarLodTile pristine = BuildPristineFarLodTile(
        world, FarLodTier::F1, 0, 0, snapshot->params_hash);
    FarLodRegionMesh pristine_mesh;
    MarchingCubes::GenerateFarLodRegionMesh(pristine, pristine_mesh);
    EXPECT_EQ(ComputeFarLodTileHash(outcome.tile), ComputeFarLodTileHash(pristine));
    EXPECT_EQ(HashMesh(outcome.mesh), HashMesh(pristine_mesh));
}

TEST(FarLodWorker, AuthoritativeCaptureBuildsPersistsAndStales) {
    const TerrainGenParams params = FlatParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    auto chunk = std::make_shared<Chunk>(IVec3(3, 0, 5));
    world.GenerateChunkData(*chunk, 1);
    const int side = CHUNK_SIZE_X + 1;
    for (int z = 4; z <= 12; z += 4) {
        for (int y = 4; y <= 8; y += 4) {
            for (int x = 4; x <= 12; x += 4) {
                const std::size_t index = static_cast<std::size_t>(x) +
                    static_cast<std::size_t>(y) * side +
                    static_cast<std::size_t>(z) * side * side;
                chunk->sdf_data[index] = 1.0f;
            }
        }
    }
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    const auto snapshot = world.capture_far_lod_sdf_snapshot(0, 0);
    ASSERT_TRUE(snapshot);
    ASSERT_EQ(snapshot->entries.size(), 1u);

    TempSaveDir save;
    const FarLodTile previous = BuildPristineFarLodTile(
        world, FarLodTier::F1, 0, 0, snapshot->params_hash);
    std::vector<std::string> errors;
    ASSERT_TRUE(FarLodStore(save.path).save_tile(previous, &errors));
    const auto outcome = BuildFarLodWorkerTile(
        world, *snapshot, FarLodTier::F1, 0, 0, save.path);
    ASSERT_TRUE(outcome.ok) << outcome.error;
    EXPECT_TRUE(outcome.changed);
    // Generated stack support is transient mesh input.  The persisted home
    // tile retains only its home-region authoritative brick.
    ASSERT_EQ(outcome.tile.sdf_bricks.size(), 1u);
    EXPECT_EQ(std::count_if(outcome.tile.sdf_bricks.begin(), outcome.tile.sdf_bricks.end(),
        [](const FarLodSdfBrickDescriptor& brick) {
            return brick.source_kind == FarLodBrickSourceKind::Authoritative;
        }), 1);
    EXPECT_FALSE(outcome.mesh.vertices.empty());
    EXPECT_FALSE(outcome.mesh.indices.empty());

    FarLodTile loaded;
    ASSERT_TRUE(FarLodStore(save.path).load_tile(
        FarLodTier::F1, 0, 0, snapshot->params_hash, loaded, &errors));
    EXPECT_EQ(ComputeFarLodTileHash(loaded), ComputeFarLodTileHash(previous))
        << "the worker helper must not write before owner-thread stale validation";
    errors.clear();
    ASSERT_TRUE(FarLodStore(save.path).save_tile(outcome.tile, &errors));
    ASSERT_TRUE(FarLodStore(save.path).load_tile(
        FarLodTier::F1, 0, 0, snapshot->params_hash, loaded, &errors));
    EXPECT_EQ(ComputeFarLodTileHash(loaded), ComputeFarLodTileHash(outcome.tile));
    EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*snapshot));
    chunk->sdf_data.front() += 1.0f;
    chunk->mark_voxel_data_dirty();
    EXPECT_FALSE(world.is_far_lod_sdf_snapshot_current(*snapshot));
}

TEST(FarLodWorker, ParamsRebasePreservesAuthorityAndRegeneratesBackground) {
    TerrainGenParams old_params = FlatParams();
    SHIELD_WorldSystem old_world(nullptr, nullptr, old_params, 1337);
    auto old_chunk = std::make_shared<Chunk>(IVec3(3, 0, 5));
    old_world.GenerateChunkData(*old_chunk, 1);
    old_chunk->sdf_data[1u + 4u * (CHUNK_SIZE_X + 1u)] = 3.0f;
    old_chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(old_world.adopt_streamed_chunk(old_chunk));
    const auto old_snapshot = old_world.capture_far_lod_sdf_snapshot(0, 0);
    ASSERT_TRUE(old_snapshot);

    TempSaveDir save;
    const auto old_outcome = BuildFarLodWorkerTile(
        old_world, *old_snapshot, FarLodTier::F1, 0, 0, save.path);
    ASSERT_TRUE(old_outcome.ok) << old_outcome.error;
    std::vector<std::string> save_errors;
    ASSERT_TRUE(FarLodStore(save.path).save_tile(old_outcome.tile, &save_errors));
    const auto old_authority = std::find_if(
        old_outcome.tile.sdf_bricks.begin(), old_outcome.tile.sdf_bricks.end(),
        [](const FarLodSdfBrickDescriptor& brick) {
            return brick.source_kind == FarLodBrickSourceKind::Authoritative;
        });
    ASSERT_NE(old_authority, old_outcome.tile.sdf_bricks.end());
    const std::size_t old_authority_index = static_cast<std::size_t>(
        old_authority - old_outcome.tile.sdf_bricks.begin());
    const std::size_t samples = FarLodSdfBrickSampleCount(FarLodTier::F1);
    const std::vector<i16> old_density(
        old_outcome.tile.sdf_density_q.begin() + old_authority_index * samples,
        old_outcome.tile.sdf_density_q.begin() + (old_authority_index + 1u) * samples);

    TerrainGenParams new_params = old_params;
    new_params.height_offset = 24.0f;
    SHIELD_WorldSystem new_world(nullptr, nullptr, new_params, 1337);
    auto loaded_chunk = std::make_shared<Chunk>(IVec3(3, 0, 5));
    loaded_chunk->sdf_data = old_chunk->sdf_data;
    loaded_chunk->material_data = old_chunk->material_data;
    loaded_chunk->mark_sdf_loaded_or_edited();
    ASSERT_TRUE(new_world.adopt_streamed_chunk(loaded_chunk));
    const auto new_snapshot = new_world.capture_far_lod_sdf_snapshot(0, 0);
    ASSERT_TRUE(new_snapshot);
    ASSERT_NE(new_snapshot->params_hash, old_snapshot->params_hash);

    const auto rebased = BuildFarLodWorkerTile(
        new_world, *new_snapshot, FarLodTier::F1, 0, 0, save.path);
    ASSERT_TRUE(rebased.ok) << rebased.error;
    EXPECT_EQ(rebased.tile.params_hash, new_snapshot->params_hash);
    EXPECT_NE(rebased.tile.height_q[0], old_outcome.tile.height_q[0]);
    const auto new_authority = std::find_if(
        rebased.tile.sdf_bricks.begin(), rebased.tile.sdf_bricks.end(),
        [](const FarLodSdfBrickDescriptor& brick) {
            return brick.source_kind == FarLodBrickSourceKind::Authoritative;
        });
    ASSERT_NE(new_authority, rebased.tile.sdf_bricks.end());
    const std::size_t new_authority_index = static_cast<std::size_t>(
        new_authority - rebased.tile.sdf_bricks.begin());
    const std::vector<i16> new_density(
        rebased.tile.sdf_density_q.begin() + new_authority_index * samples,
        rebased.tile.sdf_density_q.begin() + (new_authority_index + 1u) * samples);
    EXPECT_EQ(new_density, old_density);
}

TEST(FarLodWorker, SurfaceWaterComesFromHighestAuthoritativeSdfCrossing) {
    const auto build = [](float background_surface, float authoritative_surface, bool cave) {
        TerrainGenParams params = FlatParams();
        params.height_offset = background_surface;
        SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
        auto chunk = std::make_shared<Chunk>(IVec3(3, 0, 5));
        world.GenerateChunkData(*chunk, 1);
        const int side = CHUNK_SIZE_X + 1;
        for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
            for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
                for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
                    const std::size_t index = static_cast<std::size_t>(x) +
                        static_cast<std::size_t>(y) * side +
                        static_cast<std::size_t>(z) * side * side;
                    chunk->sdf_data[index] = static_cast<float>(y) - authoritative_surface;
                }
            }
        }
        if (cave) {
            for (int z = 4; z <= 12; z += 4) {
                for (int y = 4; y <= 8; y += 4) {
                    for (int x = 4; x <= 12; x += 4) {
                        const std::size_t index = static_cast<std::size_t>(x) +
                            static_cast<std::size_t>(y) * side +
                            static_cast<std::size_t>(z) * side * side;
                        chunk->sdf_data[index] = 2.0f;
                    }
                }
            }
        }
        chunk->mark_voxel_data_dirty();
        EXPECT_TRUE(world.adopt_streamed_chunk(chunk));
        const auto snapshot = world.capture_far_lod_sdf_snapshot(0, 0);
        EXPECT_TRUE(snapshot);
        return BuildFarLodWorkerTile(world, *snapshot, FarLodTier::F1, 0, 0, {});
    };

    const auto lowered = build(12.0f, -4.0f, false);
    ASSERT_TRUE(lowered.ok) << lowered.error;
    const std::size_t center = 14u + 22u * lowered.tile.samples_per_side;
    EXPECT_NE(lowered.tile.flags[center] & kFarLodSampleFlagWater, 0u);

    const auto raised = build(-4.0f, 12.0f, false);
    ASSERT_TRUE(raised.ok) << raised.error;
    EXPECT_EQ(raised.tile.flags[center] & kFarLodSampleFlagWater, 0u);

    const auto underground_cave = build(12.0f, 12.0f, true);
    ASSERT_TRUE(underground_cave.ok) << underground_cave.error;
    EXPECT_EQ(underground_cave.tile.flags[center] & kFarLodSampleFlagWater, 0u);
}

TEST(FarLodWorker, CrossRegionEdgeAndCornerAuthorityRemainTransientForBothTiers) {
    for (const FarLodTier tier : {FarLodTier::F1, FarLodTier::F2}) {
        const TerrainGenParams params = FlatParams();
        SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
        // This chunk is the max-X/max-Z corner of region (0,0), and must
        // therefore promote the touching boundary columns of region (1,1).
        auto chunk = std::make_shared<Chunk>(IVec3(31, 0, 31));
        world.GenerateChunkData(*chunk, 1);
        const int side = CHUNK_SIZE_X + 1;
        for (int z = 0; z <= CHUNK_SIZE_Z; z += FarLodSampleStepMeters(tier)) {
            for (int y = 0; y <= CHUNK_SIZE_Y; y += FarLodSampleStepMeters(tier)) {
                for (int x = 0; x <= CHUNK_SIZE_X; x += FarLodSampleStepMeters(tier)) {
                    const std::size_t index = static_cast<std::size_t>(x) +
                        static_cast<std::size_t>(y) * side + static_cast<std::size_t>(z) * side * side;
                    chunk->sdf_data[index] = static_cast<float>(y) - 7.0f;
                }
            }
        }
        chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));

        TempSaveDir save;
        const auto source_snapshot = world.capture_far_lod_sdf_snapshot(0, 0);
        ASSERT_TRUE(source_snapshot);
        const auto source = BuildFarLodWorkerTile(world, *source_snapshot, tier, 0, 0, save.path);
        ASSERT_TRUE(source.ok) << source.error;
        std::vector<std::string> errors;
        ASSERT_TRUE(FarLodStore(save.path).save_tile(source.tile, &errors));

        const auto target_snapshot = world.capture_far_lod_sdf_snapshot(1, 1);
        ASSERT_TRUE(target_snapshot);
        const auto target = BuildFarLodWorkerTile(world, *target_snapshot, tier, 1, 1, save.path);
        ASSERT_TRUE(target.ok) << target.error;
        EXPECT_FALSE(target.mesh.indices.empty());
        for (const FarLodSdfBrickDescriptor& descriptor : target.tile.sdf_bricks) {
            EXPECT_GE(descriptor.local_chunk_x, 0u);
            EXPECT_LT(descriptor.local_chunk_x, 32u);
            EXPECT_GE(descriptor.local_chunk_z, 0u);
            EXPECT_LT(descriptor.local_chunk_z, 32u);
        }
    }
}
