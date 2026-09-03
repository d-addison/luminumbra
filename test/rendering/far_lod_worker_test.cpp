#include "gtest/gtest.h"

#include "luminumbra_client/rendering/FarLodSystem.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/MarchingCubes.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
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
               ("luminumbra_far_worker_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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

using PlanePointBits = std::array<u32, 3>;
using PlaneSegmentBits = std::array<PlanePointBits, 2>;
enum class SharedPlaneAxis {
    X,
    Z
};

std::vector<PlaneSegmentBits> SharedPlaneSegments(const FarLodRegionMesh& mesh,
                                                  int region_x,
                                                  int region_z,
                                                  SharedPlaneAxis axis,
                                                  float world_plane) {
    std::vector<PlaneSegmentBits> segments;
    const float origin_x = static_cast<float>(region_x * kFarLodRegionSizeMeters);
    const float origin_z = static_cast<float>(region_z * kFarLodRegionSizeMeters);
    const float local_plane = world_plane - (axis == SharedPlaneAxis::X ? origin_x : origin_z);
    const auto point_bits = [&](const VoxelVertex& vertex) {
        return PlanePointBits{
            std::bit_cast<u32>(vertex.position.x + origin_x),
            std::bit_cast<u32>(vertex.position.y),
            std::bit_cast<u32>(vertex.position.z + origin_z),
        };
    };

    for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3) {
        const std::array<u32, 3> indices{
            mesh.indices[triangle], mesh.indices[triangle + 1], mesh.indices[triangle + 2]};
        if (indices[0] >= mesh.vertices.size() || indices[1] >= mesh.vertices.size() ||
            indices[2] >= mesh.vertices.size()) {
            continue;
        }
        for (int edge = 0; edge < 3; ++edge) {
            const VoxelVertex& a = mesh.vertices[indices[edge]];
            const VoxelVertex& b = mesh.vertices[indices[(edge + 1) % 3]];
            // A region mesh is closed on one side of this exact sample plane,
            // so every non-degenerate triangle/plane intersection is an edge
            // whose endpoints lie on it. Include every material/path: this is
            // the complete plane multiset, not an authority-only sample.
            const float a_plane = axis == SharedPlaneAxis::X ? a.position.x : a.position.z;
            const float b_plane = axis == SharedPlaneAxis::X ? b.position.x : b.position.z;
            if (a_plane != local_plane || b_plane != local_plane) {
                continue;
            }
            PlaneSegmentBits segment{point_bits(a), point_bits(b)};
            if (segment[0] == segment[1]) {
                continue;
            }
            if (segment[1] < segment[0]) {
                std::swap(segment[0], segment[1]);
            }
            segments.push_back(segment);
        }
    }
    std::sort(segments.begin(), segments.end());
    return segments;
}

std::size_t CountIndexedWorldVertex(const FarLodRegionMesh& mesh,
                                    int region_x,
                                    int region_z,
                                    float world_x,
                                    float world_y,
                                    float world_z,
                                    u32 material) {
    const float local_x = world_x - static_cast<float>(region_x * kFarLodRegionSizeMeters);
    const float local_z = world_z - static_cast<float>(region_z * kFarLodRegionSizeMeters);
    std::set<u32> matches;
    for (const u32 index : mesh.indices) {
        if (index >= mesh.vertices.size())
            continue;
        const VoxelVertex& vertex = mesh.vertices[index];
        if (vertex.position.x == local_x && vertex.position.y == world_y &&
            vertex.position.z == local_z && vertex.material_id == material) {
            matches.insert(index);
        }
    }
    return matches.size();
}

std::size_t CountIndexedWorldPosition(const FarLodRegionMesh& mesh,
                                      int region_x,
                                      int region_z,
                                      float world_x,
                                      float world_y,
                                      float world_z) {
    const float local_x = world_x - static_cast<float>(region_x * kFarLodRegionSizeMeters);
    const float local_z = world_z - static_cast<float>(region_z * kFarLodRegionSizeMeters);
    std::set<u32> matches;
    for (const u32 index : mesh.indices) {
        if (index >= mesh.vertices.size())
            continue;
        const VoxelVertex& vertex = mesh.vertices[index];
        if (vertex.position.x == local_x && vertex.position.y == world_y &&
            vertex.position.z == local_z) {
            matches.insert(index);
        }
    }
    return matches.size();
}

bool PlaneSegmentsContainHeight(const std::vector<PlaneSegmentBits>& segments, float world_y) {
    const u32 height_bits = std::bit_cast<u32>(world_y);
    return std::any_of(segments.begin(), segments.end(), [&](const auto& segment) {
        return segment[0][1] == height_bits || segment[1][1] == height_bits;
    });
}

struct CrossRegionBuildResult {
    bool ok = false;
    std::string error;
    u64 home_tile_hash = 0;
    u64 home_mesh_hash = 0;
    u64 target_tile_hash = 0;
    u64 target_mesh_hash = 0;
    u64 target_pristine_mesh_hash = 0;
    u64 persisted_home_hash = 0;
    std::size_t home_bricks = 0;
    std::size_t target_bricks = 0;
    u8 home_local_chunk_x = 0;
    u8 home_local_chunk_z = 0;
    bool target_record_missing = false;
    bool target_miss_clean = false;
    std::vector<PlaneSegmentBits> home_plane_segments;
    std::vector<PlaneSegmentBits> target_plane_segments;
};

bool SetPlanarAuthority(Chunk& chunk, float surface_y, u8 material) {
    const int side = CHUNK_SIZE_X + 1;
    const std::size_t expected_samples = static_cast<std::size_t>(side) * side * side;
    if (chunk.sdf_data.size() != expected_samples) {
        return false;
    }
    chunk.material_data.assign(expected_samples, material);
    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
            for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
                const std::size_t index = static_cast<std::size_t>(x) +
                                          static_cast<std::size_t>(y) * side +
                                          static_cast<std::size_t>(z) * side * side;
                chunk.sdf_data[index] = static_cast<float>(y) - surface_y;
            }
        }
    }
    chunk.mark_voxel_data_dirty();
    return true;
}

bool SetBoundaryRampedAuthority(Chunk& chunk, float shared_face_y, float interior_y, u8 material) {
    const int side = CHUNK_SIZE_X + 1;
    const std::size_t expected_samples = static_cast<std::size_t>(side) * side * side;
    if (chunk.sdf_data.size() != expected_samples)
        return false;
    chunk.material_data.assign(expected_samples, material);
    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
            for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
                const float surface = x == 0 ? shared_face_y : interior_y;
                const std::size_t index = static_cast<std::size_t>(x) +
                                          static_cast<std::size_t>(y) * side +
                                          static_cast<std::size_t>(z) * side * side;
                chunk.sdf_data[index] = static_cast<float>(y) - surface;
            }
        }
    }
    chunk.mark_voxel_data_dirty();
    return true;
}

CrossRegionBuildResult BuildCrossRegionBoundary(FarLodTier tier,
                                                const IVec3& authority_coords,
                                                int home_rx,
                                                int home_rz,
                                                int target_rx,
                                                int target_rz,
                                                SharedPlaneAxis axis,
                                                bool home_first) {
    constexpr u8 kAuthorityMaterial = 231u;
    CrossRegionBuildResult result;
    const auto fail = [&](std::string message) {
        result.error = std::move(message);
        return result;
    };

    const TerrainGenParams params = FlatParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    auto chunk = std::make_shared<Chunk>(authority_coords);
    world.GenerateChunkData(*chunk, 1);
    if (!SetPlanarAuthority(*chunk, 7.0f, kAuthorityMaterial)) {
        return fail("generated authority chunk did not contain a full SDF lattice");
    }
    if (!world.adopt_streamed_chunk(chunk)) {
        return fail("failed to adopt authoritative boundary chunk");
    }

    const auto home_snapshot = world.capture_far_lod_sdf_snapshot(home_rx, home_rz);
    const auto target_snapshot = world.capture_far_lod_sdf_snapshot(target_rx, target_rz);
    if (!home_snapshot || !target_snapshot) {
        return fail("failed to capture a cross-region SDF snapshot");
    }

    TempSaveDir save;
    FarLodWorkerBuildOutcome home;
    FarLodWorkerBuildOutcome target;
    std::vector<std::string> errors;
    const auto build_home_and_save = [&]() -> bool {
        home = BuildFarLodWorkerTile(world, *home_snapshot, tier, home_rx, home_rz, save.path);
        if (!home.ok) {
            result.error = "home build failed: " + home.error;
            return false;
        }
        if (!FarLodStore(save.path).save_tile(home.tile, &errors)) {
            result.error = errors.empty() ? "home tile save failed" : errors.front();
            return false;
        }
        return true;
    };
    const auto build_target = [&]() -> bool {
        target =
            BuildFarLodWorkerTile(world, *target_snapshot, tier, target_rx, target_rz, save.path);
        if (!target.ok) {
            result.error = "target build failed: " + target.error;
            return false;
        }
        return true;
    };
    if (home_first) {
        if (!build_home_and_save() || !build_target()) {
            return result;
        }
    } else if (!build_target() || !build_home_and_save()) {
        return result;
    }

    result.home_tile_hash = ComputeFarLodTileHash(home.tile);
    result.home_mesh_hash = HashMesh(home.mesh);
    result.target_tile_hash = ComputeFarLodTileHash(target.tile);
    result.target_mesh_hash = HashMesh(target.mesh);
    const FarLodTile pristine_target =
        BuildPristineFarLodTile(world, tier, target_rx, target_rz, target_snapshot->params_hash);
    FarLodRegionMesh pristine_target_mesh;
    MarchingCubes::GenerateFarLodRegionMesh(pristine_target, pristine_target_mesh);
    result.target_pristine_mesh_hash = HashMesh(pristine_target_mesh);
    result.home_bricks = home.tile.sdf_bricks.size();
    result.target_bricks = target.tile.sdf_bricks.size();
    if (!home.tile.sdf_bricks.empty()) {
        result.home_local_chunk_x = home.tile.sdf_bricks.front().local_chunk_x;
        result.home_local_chunk_z = home.tile.sdf_bricks.front().local_chunk_z;
    }

    FarLodTile persisted_home;
    errors.clear();
    if (!FarLodStore(save.path).load_tile(
            tier, home_rx, home_rz, home_snapshot->params_hash, persisted_home, &errors)) {
        return fail(errors.empty() ? "persisted home tile was missing" : errors.front());
    }
    result.persisted_home_hash = ComputeFarLodTileHash(persisted_home);

    FarLodTile persisted_target;
    errors.clear();
    result.target_record_missing = !FarLodStore(save.path).load_tile(
        tier, target_rx, target_rz, target_snapshot->params_hash, persisted_target, &errors);
    result.target_miss_clean = errors.empty();

    const int home_region_axis = axis == SharedPlaneAxis::X ? home_rx : home_rz;
    const int target_region_axis = axis == SharedPlaneAxis::X ? target_rx : target_rz;
    const float world_plane = static_cast<float>(
        (target_region_axis > home_region_axis ? target_region_axis : home_region_axis) *
        kFarLodRegionSizeMeters);
    result.home_plane_segments =
        SharedPlaneSegments(home.mesh, home_rx, home_rz, axis, world_plane);
    result.target_plane_segments =
        SharedPlaneSegments(target.mesh, target_rx, target_rz, axis, world_plane);
    result.ok = true;
    return result;
}

} // namespace

TEST(FarLodWorker, ZeroAuthorityMatchesPristinePath) {
    const TerrainGenParams params = FlatParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    const auto snapshot = world.capture_far_lod_sdf_snapshot(0, 0);
    ASSERT_TRUE(snapshot);
    ASSERT_TRUE(snapshot->entries.empty());

    const auto outcome = BuildFarLodWorkerTile(world, *snapshot, FarLodTier::F1, 0, 0, {});
    ASSERT_TRUE(outcome.ok) << outcome.error;
    EXPECT_TRUE(outcome.tile.sdf_bricks.empty());

    const FarLodTile pristine =
        BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, snapshot->params_hash);
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
    const FarLodTile previous =
        BuildPristineFarLodTile(world, FarLodTier::F1, 0, 0, snapshot->params_hash);
    std::vector<std::string> errors;
    ASSERT_TRUE(FarLodStore(save.path).save_tile(previous, &errors));
    const auto outcome = BuildFarLodWorkerTile(world, *snapshot, FarLodTier::F1, 0, 0, save.path);
    ASSERT_TRUE(outcome.ok) << outcome.error;
    EXPECT_TRUE(outcome.changed);
    // Generated stack support is transient mesh input.  The persisted home
    // tile retains only its home-region authoritative brick.
    ASSERT_EQ(outcome.tile.sdf_bricks.size(), 1u);
    EXPECT_EQ(std::count_if(outcome.tile.sdf_bricks.begin(),
                            outcome.tile.sdf_bricks.end(),
                            [](const FarLodSdfBrickDescriptor& brick) {
                                return brick.source_kind == FarLodBrickSourceKind::Authoritative;
                            }),
              1);
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
    const auto old_outcome =
        BuildFarLodWorkerTile(old_world, *old_snapshot, FarLodTier::F1, 0, 0, save.path);
    ASSERT_TRUE(old_outcome.ok) << old_outcome.error;
    std::vector<std::string> save_errors;
    ASSERT_TRUE(FarLodStore(save.path).save_tile(old_outcome.tile, &save_errors));
    const auto old_authority =
        std::find_if(old_outcome.tile.sdf_bricks.begin(),
                     old_outcome.tile.sdf_bricks.end(),
                     [](const FarLodSdfBrickDescriptor& brick) {
                         return brick.source_kind == FarLodBrickSourceKind::Authoritative;
                     });
    ASSERT_NE(old_authority, old_outcome.tile.sdf_bricks.end());
    const std::size_t old_authority_index =
        static_cast<std::size_t>(old_authority - old_outcome.tile.sdf_bricks.begin());
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

    const auto rebased =
        BuildFarLodWorkerTile(new_world, *new_snapshot, FarLodTier::F1, 0, 0, save.path);
    ASSERT_TRUE(rebased.ok) << rebased.error;
    EXPECT_EQ(rebased.tile.params_hash, new_snapshot->params_hash);
    EXPECT_NE(rebased.tile.height_q[0], old_outcome.tile.height_q[0]);
    const auto new_authority =
        std::find_if(rebased.tile.sdf_bricks.begin(),
                     rebased.tile.sdf_bricks.end(),
                     [](const FarLodSdfBrickDescriptor& brick) {
                         return brick.source_kind == FarLodBrickSourceKind::Authoritative;
                     });
    ASSERT_NE(new_authority, rebased.tile.sdf_bricks.end());
    const std::size_t new_authority_index =
        static_cast<std::size_t>(new_authority - rebased.tile.sdf_bricks.begin());
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
                                              static_cast<std::size_t>(y) * side +
                                              static_cast<std::size_t>(z) * side * side;
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

TEST(FarLodWorker, RejectsRegionsOutsideTheInvertibleChunkIdRange) {
    const TerrainGenParams params = FlatParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    const auto snapshot = world.capture_far_lod_sdf_snapshot(0, 0);
    ASSERT_TRUE(snapshot);
    for (const int region_x : {-32768, 32768}) {
        EXPECT_FALSE(world.capture_far_lod_sdf_snapshot(region_x, 0));
        const auto outcome =
            BuildFarLodWorkerTile(world, *snapshot, FarLodTier::F1, region_x, 0, {});
        EXPECT_FALSE(outcome.ok);
        EXPECT_NE(outcome.error.find("supported world range"), std::string::npos) << outcome.error;
    }
}

TEST(FarLodWorker, AuthorityRevisionInvalidationTargetsHomeEdgesAndCorners) {
    struct InvalidationCase {
        IVec3 coords;
        std::vector<std::pair<int, int>> affected;
        std::vector<std::pair<int, int>> unaffected;
    };
    const std::array<InvalidationCase, 4> cases{{
        {IVec3(5, 0, 5), {{0, 0}}, {{1, 0}, {0, 1}, {-1, 0}}},
        {IVec3(31, 0, 5), {{0, 0}, {1, 0}}, {{0, 1}, {1, 1}, {-1, 0}}},
        {IVec3(31, 0, 31), {{0, 0}, {1, 0}, {0, 1}, {1, 1}}, {{-1, 0}, {0, -1}}},
        {IVec3(-32, 0, -32), {{-1, -1}, {-2, -1}, {-1, -2}, {-2, -2}}, {{0, -1}, {-1, 0}}},
    }};
    for (const InvalidationCase& test_case : cases) {
        SCOPED_TRACE(::testing::Message() << test_case.coords.x << "," << test_case.coords.z);
        const TerrainGenParams params = FlatParams();
        SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
        auto chunk = std::make_shared<Chunk>(test_case.coords);
        world.GenerateChunkData(*chunk, 1);
        ASSERT_TRUE(SetPlanarAuthority(*chunk, 7.0f, 231u));
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        const u64 global_revision = world.far_lod_authority_revision();
        ASSERT_GT(global_revision, 0u);
        for (const auto& [rx, rz] : test_case.affected) {
            EXPECT_EQ(world.far_lod_region_authority_revision(rx, rz), global_revision);
            const auto snapshot = world.capture_far_lod_sdf_snapshot(rx, rz);
            ASSERT_TRUE(snapshot);
            EXPECT_EQ(snapshot->region_authority_revision, global_revision);
        }
        for (const auto& [rx, rz] : test_case.unaffected) {
            EXPECT_EQ(world.far_lod_region_authority_revision(rx, rz), 0u);
        }
    }
}

TEST(FarLodWorker, CrossRegionBoundariesAreBitIdenticalAndHomeOwnedForBothTiers) {
    struct BoundaryCase {
        const char* name;
        IVec3 authority_coords;
        int home_rx;
        int home_rz;
        int target_rx;
        int target_rz;
        u8 expected_local_x;
        u8 expected_local_z;
        SharedPlaneAxis axis;
    };
    const std::array<BoundaryCase, 4> boundaries{{
        {"positive-max-x", IVec3(31, 0, 5), 0, 0, 1, 0, 31u, 5u, SharedPlaneAxis::X},
        {"negative-min-x", IVec3(-32, 0, -27), -1, -1, -2, -1, 0u, 5u, SharedPlaneAxis::X},
        {"positive-max-z", IVec3(5, 0, 31), 0, 0, 0, 1, 5u, 31u, SharedPlaneAxis::Z},
        {"negative-min-z", IVec3(-27, 0, -32), -1, -1, -1, -2, 5u, 0u, SharedPlaneAxis::Z},
    }};

    for (const FarLodTier tier : {FarLodTier::F1, FarLodTier::F2}) {
        for (const BoundaryCase& boundary : boundaries) {
            SCOPED_TRACE(::testing::Message()
                         << boundary.name << " tier=" << static_cast<int>(tier));
            const CrossRegionBuildResult home_first =
                BuildCrossRegionBoundary(tier,
                                         boundary.authority_coords,
                                         boundary.home_rx,
                                         boundary.home_rz,
                                         boundary.target_rx,
                                         boundary.target_rz,
                                         boundary.axis,
                                         true);
            const CrossRegionBuildResult target_first =
                BuildCrossRegionBoundary(tier,
                                         boundary.authority_coords,
                                         boundary.home_rx,
                                         boundary.home_rz,
                                         boundary.target_rx,
                                         boundary.target_rz,
                                         boundary.axis,
                                         false);

            ASSERT_TRUE(home_first.ok) << home_first.error;
            ASSERT_TRUE(target_first.ok) << target_first.error;

            // Persistence is half-open and home-owned.  The target consumes
            // foreign authority transiently but never acquires a record or a
            // foreign persisted brick of its own.
            EXPECT_EQ(home_first.home_bricks, 1u);
            EXPECT_EQ(target_first.home_bricks, 1u);
            EXPECT_EQ(home_first.target_bricks, 0u);
            EXPECT_EQ(target_first.target_bricks, 0u);
            EXPECT_EQ(home_first.home_local_chunk_x, boundary.expected_local_x);
            EXPECT_EQ(target_first.home_local_chunk_x, boundary.expected_local_x);
            EXPECT_EQ(home_first.home_local_chunk_z, boundary.expected_local_z);
            EXPECT_EQ(target_first.home_local_chunk_z, boundary.expected_local_z);
            EXPECT_EQ(home_first.persisted_home_hash, home_first.home_tile_hash);
            EXPECT_EQ(target_first.persisted_home_hash, target_first.home_tile_hash);
            EXPECT_TRUE(home_first.target_record_missing);
            EXPECT_TRUE(target_first.target_record_missing);
            EXPECT_TRUE(home_first.target_miss_clean);
            EXPECT_TRUE(target_first.target_miss_clean);

            // Building the neighbor from a persisted home record plus a live
            // snapshot must equal building it from the live snapshot first.
            EXPECT_EQ(home_first.home_tile_hash, target_first.home_tile_hash);
            EXPECT_EQ(home_first.target_tile_hash, target_first.target_tile_hash);
            EXPECT_EQ(home_first.home_mesh_hash, target_first.home_mesh_hash);
            EXPECT_EQ(home_first.target_mesh_hash, target_first.target_mesh_hash);
            EXPECT_NE(home_first.target_mesh_hash, home_first.target_pristine_mesh_hash)
                << "foreign authority did not alter target-region geometry";
            EXPECT_NE(target_first.target_mesh_hash, target_first.target_pristine_mesh_hash)
                << "foreign authority did not alter target-region geometry";

            // Compare the exact triangle edges on the shared world plane, not
            // merely an aggregate count.  Endpoint float bit patterns and
            // duplicate multiplicity must agree and the proof must be nonempty.
            ASSERT_FALSE(home_first.home_plane_segments.empty());
            ASSERT_FALSE(target_first.home_plane_segments.empty());
            EXPECT_EQ(home_first.home_plane_segments, home_first.target_plane_segments);
            EXPECT_EQ(target_first.home_plane_segments, target_first.target_plane_segments);
            EXPECT_EQ(home_first.home_plane_segments, target_first.home_plane_segments);
        }
    }
}

TEST(FarLodWorker, RegionCornerAuthorityBuildsAllFourRegionsDeterministically) {
    struct CornerCase {
        const char* name;
        IVec3 authority_coords;
        std::array<std::pair<int, int>, 4> regions; // home, X, Z, diagonal
    };
    const std::array<CornerCase, 2> corners{{
        {"positive-max-corner", IVec3(31, 0, 31), {{{0, 0}, {1, 0}, {0, 1}, {1, 1}}}},
        {"negative-min-corner", IVec3(-32, 0, -32), {{{-1, -1}, {-2, -1}, {-1, -2}, {-2, -2}}}},
    }};

    for (const FarLodTier tier : {FarLodTier::F1, FarLodTier::F2}) {
        for (const CornerCase& corner : corners) {
            SCOPED_TRACE(::testing::Message() << corner.name << " tier=" << static_cast<int>(tier));
            const TerrainGenParams params = FlatParams();
            SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
            auto chunk = std::make_shared<Chunk>(corner.authority_coords);
            world.GenerateChunkData(*chunk, 1);
            ASSERT_TRUE(SetPlanarAuthority(*chunk, 7.0f, 231u));
            ASSERT_TRUE(world.adopt_streamed_chunk(chunk));

            const auto build_order = [&](const std::array<int, 4>& order) {
                std::array<FarLodWorkerBuildOutcome, 4> outcomes;
                for (const int index : order) {
                    const auto [rx, rz] = corner.regions[static_cast<std::size_t>(index)];
                    const auto snapshot = world.capture_far_lod_sdf_snapshot(rx, rz);
                    EXPECT_TRUE(snapshot);
                    if (snapshot) {
                        outcomes[static_cast<std::size_t>(index)] =
                            BuildFarLodWorkerTile(world, *snapshot, tier, rx, rz, {});
                    }
                }
                return outcomes;
            };
            const auto forward = build_order({0, 1, 2, 3});
            const auto reverse = build_order({3, 2, 1, 0});
            for (std::size_t i = 0; i < forward.size(); ++i) {
                ASSERT_TRUE(forward[i].ok) << forward[i].error;
                ASSERT_TRUE(reverse[i].ok) << reverse[i].error;
                EXPECT_EQ(ComputeFarLodTileHash(forward[i].tile),
                          ComputeFarLodTileHash(reverse[i].tile));
                EXPECT_EQ(HashMesh(forward[i].mesh), HashMesh(reverse[i].mesh));
                EXPECT_EQ(forward[i].tile.sdf_bricks.size(), i == 0 ? 1u : 0u);
                const auto [rx, rz] = corner.regions[i];
                const FarLodTile pristine =
                    BuildPristineFarLodTile(world, tier, rx, rz, forward[i].tile.params_hash);
                FarLodRegionMesh pristine_mesh;
                MarchingCubes::GenerateFarLodRegionMesh(pristine, pristine_mesh);
                EXPECT_NE(HashMesh(forward[i].mesh), HashMesh(pristine_mesh))
                    << "corner authority did not alter region index " << i;
            }

            const auto compare_plane = [&](std::size_t a, std::size_t b, SharedPlaneAxis axis) {
                const auto [arx, arz] = corner.regions[a];
                const auto [brx, brz] = corner.regions[b];
                const int region_a = axis == SharedPlaneAxis::X ? arx : arz;
                const int region_b = axis == SharedPlaneAxis::X ? brx : brz;
                const float plane =
                    static_cast<float>(std::max(region_a, region_b) * kFarLodRegionSizeMeters);
                const auto a_segments = SharedPlaneSegments(forward[a].mesh, arx, arz, axis, plane);
                const auto b_segments = SharedPlaneSegments(forward[b].mesh, brx, brz, axis, plane);
                EXPECT_FALSE(a_segments.empty());
                EXPECT_EQ(a_segments, b_segments);
            };
            compare_plane(0, 1, SharedPlaneAxis::X);
            compare_plane(2, 3, SharedPlaneAxis::X);
            compare_plane(0, 2, SharedPlaneAxis::Z);
            compare_plane(1, 3, SharedPlaneAxis::Z);

            TempSaveDir save;
            std::vector<std::string> errors;
            ASSERT_TRUE(FarLodStore(save.path).save_tile(forward[0].tile, &errors));
            for (std::size_t i = 1; i < corner.regions.size(); ++i) {
                const auto [rx, rz] = corner.regions[i];
                FarLodTile foreign;
                errors.clear();
                EXPECT_FALSE(FarLodStore(save.path).load_tile(
                    tier, rx, rz, forward[0].tile.params_hash, foreign, &errors));
                EXPECT_TRUE(errors.empty());
            }
        }
    }
}

TEST(FarLodWorker, ConflictingCrossRegionAuthorityFailsWithoutTouchingTheStore) {
    constexpr u8 kAuthorityMaterial = 231u;
    for (const FarLodTier tier : {FarLodTier::F1, FarLodTier::F2}) {
        for (const bool material_mismatch : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "tier=" << static_cast<int>(tier) << " mismatch="
                                              << (material_mismatch ? "material" : "density"));
            const TerrainGenParams params = FlatParams();
            SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
            auto left = std::make_shared<Chunk>(IVec3(31, 0, 5));
            auto right = std::make_shared<Chunk>(IVec3(32, 0, 5));
            world.GenerateChunkData(*left, 1);
            world.GenerateChunkData(*right, 1);
            ASSERT_TRUE(SetPlanarAuthority(*left, 7.0f, kAuthorityMaterial));
            ASSERT_TRUE(SetPlanarAuthority(*right, 7.0f, kAuthorityMaterial));

            const int side = CHUNK_SIZE_X + 1;
            const std::size_t shared_sample = 0u + 8u * static_cast<std::size_t>(side) +
                                              8u * static_cast<std::size_t>(side) * side;
            if (material_mismatch) {
                right->material_data[shared_sample] = kAuthorityMaterial - 1u;
            } else {
                right->sdf_data[shared_sample] += 1.0f;
            }
            right->mark_voxel_data_dirty();
            ASSERT_TRUE(world.adopt_streamed_chunk(left));
            ASSERT_TRUE(world.adopt_streamed_chunk(right));

            const auto snapshot = world.capture_far_lod_sdf_snapshot(0, 0);
            ASSERT_TRUE(snapshot);
            ASSERT_EQ(snapshot->entries.size(), 2u);
            TempSaveDir save;
            const FarLodTile sentinel =
                BuildPristineFarLodTile(world, tier, 0, 0, snapshot->params_hash);
            const u64 sentinel_hash = ComputeFarLodTileHash(sentinel);
            std::vector<std::string> errors;
            ASSERT_TRUE(FarLodStore(save.path).save_tile(sentinel, &errors));

            const auto outcome = BuildFarLodWorkerTile(world, *snapshot, tier, 0, 0, save.path);
            EXPECT_FALSE(outcome.ok);
            EXPECT_NE(outcome.error.find("disagree on a shared world sample"), std::string::npos)
                << outcome.error;

            FarLodTile after_failure;
            errors.clear();
            ASSERT_TRUE(FarLodStore(save.path).load_tile(
                tier, 0, 0, snapshot->params_hash, after_failure, &errors));
            EXPECT_TRUE(errors.empty());
            EXPECT_EQ(ComputeFarLodTileHash(after_failure), sentinel_hash);
        }
    }
}

TEST(FarLodWorker, LiveBoundarySnapshotSupersedesPersistedNeighborAuthority) {
    constexpr u8 kOldMaterial = 231u;
    constexpr u8 kNewMaterial = 229u;
    for (const FarLodTier tier : {FarLodTier::F1, FarLodTier::F2}) {
        SCOPED_TRACE(::testing::Message() << "tier=" << static_cast<int>(tier));
        const TerrainGenParams params = FlatParams();
        TempSaveDir save;

        SHIELD_WorldSystem old_world(nullptr, nullptr, params, 1337);
        auto old_chunk = std::make_shared<Chunk>(IVec3(31, 0, 5));
        old_world.GenerateChunkData(*old_chunk, 1);
        ASSERT_TRUE(SetPlanarAuthority(*old_chunk, 7.0f, kOldMaterial));
        ASSERT_TRUE(old_world.adopt_streamed_chunk(old_chunk));
        const auto old_home_snapshot = old_world.capture_far_lod_sdf_snapshot(0, 0);
        const auto old_target_snapshot = old_world.capture_far_lod_sdf_snapshot(1, 0);
        ASSERT_TRUE(old_home_snapshot);
        ASSERT_TRUE(old_target_snapshot);

        const auto old_home =
            BuildFarLodWorkerTile(old_world, *old_home_snapshot, tier, 0, 0, save.path);
        ASSERT_TRUE(old_home.ok) << old_home.error;
        std::vector<std::string> errors;
        ASSERT_TRUE(FarLodStore(save.path).save_tile(old_home.tile, &errors));
        const u64 persisted_old_hash = ComputeFarLodTileHash(old_home.tile);
        const auto old_target =
            BuildFarLodWorkerTile(old_world, *old_target_snapshot, tier, 1, 0, save.path);
        ASSERT_TRUE(old_target.ok) << old_target.error;

        // Simulate a reload followed by a newer live edit whose process-local
        // revision restarts below the persisted descriptor revision.  Current
        // owner-thread bytes must win regardless of the numeric restart.
        SHIELD_WorldSystem new_world(nullptr, nullptr, params, 1337);
        auto new_chunk = std::make_shared<Chunk>(IVec3(31, 0, 5));
        new_world.GenerateChunkData(*new_chunk, 1);
        ASSERT_TRUE(SetPlanarAuthority(*new_chunk, 11.0f, kNewMaterial));
        ASSERT_TRUE(new_world.adopt_streamed_chunk(new_chunk));
        const auto new_target_snapshot = new_world.capture_far_lod_sdf_snapshot(1, 0);
        ASSERT_TRUE(new_target_snapshot);

        const auto mixed =
            BuildFarLodWorkerTile(new_world, *new_target_snapshot, tier, 1, 0, save.path);
        const auto live_only =
            BuildFarLodWorkerTile(new_world, *new_target_snapshot, tier, 1, 0, {});
        ASSERT_TRUE(mixed.ok) << mixed.error;
        ASSERT_TRUE(live_only.ok) << live_only.error;
        EXPECT_TRUE(mixed.tile.sdf_bricks.empty());
        EXPECT_EQ(ComputeFarLodTileHash(mixed.tile), ComputeFarLodTileHash(live_only.tile));
        EXPECT_EQ(HashMesh(mixed.mesh), HashMesh(live_only.mesh));
        EXPECT_NE(HashMesh(mixed.mesh), HashMesh(old_target.mesh));

        // Worker assembly is read-only: consuming the old record plus the live
        // overlay must not rewrite the home record or create a target record.
        FarLodTile persisted_home;
        errors.clear();
        ASSERT_TRUE(FarLodStore(save.path).load_tile(
            tier, 0, 0, new_target_snapshot->params_hash, persisted_home, &errors));
        EXPECT_EQ(ComputeFarLodTileHash(persisted_home), persisted_old_hash);
        FarLodTile persisted_target;
        errors.clear();
        EXPECT_FALSE(FarLodStore(save.path).load_tile(
            tier, 1, 0, new_target_snapshot->params_hash, persisted_target, &errors));
        EXPECT_TRUE(errors.empty());
    }
}

TEST(FarLodWorker, FullSdfBoundaryAuthoritySupersedesLegacyNeighborMaterial) {
    constexpr u8 kLegacyMaterial = 83u;
    const TerrainGenParams params = FlatParams();
    for (const FarLodTier tier : {FarLodTier::F1, FarLodTier::F2}) {
        SCOPED_TRACE(::testing::Message() << "tier=" << static_cast<int>(tier));
        TempSaveDir save;
        SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);

        FarLodTile legacy =
            BuildPristineFarLodTile(world, tier, 0, 0, ComputeTerrainParamsHash(params, 1337));
        legacy.edited = true;
        legacy.legacy_surface_authority = true;
        const int sample_step = FarLodSampleStepMeters(tier);
        for (u32 z = 0; z < legacy.samples_per_side; ++z) {
            for (u32 x = 0; x < legacy.samples_per_side; ++x) {
                const int world_x = static_cast<int>(x) * sample_step;
                const int world_z = static_cast<int>(z) * sample_step;
                if (world_x < 31 * CHUNK_SIZE_X || world_x > 32 * CHUNK_SIZE_X ||
                    world_z < 5 * CHUNK_SIZE_Z || world_z > 6 * CHUNK_SIZE_Z) {
                    continue;
                }
                const std::size_t index = static_cast<std::size_t>(x) +
                                          static_cast<std::size_t>(z) * legacy.samples_per_side;
                legacy.material[index] = kLegacyMaterial;
                legacy.flags[index] |= kFarLodSampleFlagEdited;
            }
        }
        std::vector<std::string> errors;
        ASSERT_TRUE(FarLodStore(save.path).save_tile(legacy, &errors));

        auto chunk = std::make_shared<Chunk>(IVec3(31, 0, 5));
        world.GenerateChunkData(*chunk, 1);
        ASSERT_TRUE(SetPlanarAuthority(*chunk, 7.0f, 231u));
        // An empty authored channel reduces to the analytic-material sentinel;
        // the fallback must come from current terrain after real SDF authority
        // supersedes the migrated legacy footprint.
        chunk->material_data.clear();
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        const auto snapshot = world.capture_far_lod_sdf_snapshot(1, 0);
        ASSERT_TRUE(snapshot);

        const auto mixed = BuildFarLodWorkerTile(world, *snapshot, tier, 1, 0, save.path);
        const auto live_only = BuildFarLodWorkerTile(world, *snapshot, tier, 1, 0, {});
        ASSERT_TRUE(mixed.ok) << mixed.error;
        ASSERT_TRUE(live_only.ok) << live_only.error;
        EXPECT_EQ(HashMesh(mixed.mesh), HashMesh(live_only.mesh));
        EXPECT_EQ(std::count_if(mixed.mesh.vertices.begin(),
                                mixed.mesh.vertices.end(),
                                [](const VoxelVertex& vertex) {
                                    return vertex.material_id == kLegacyMaterial;
                                }),
                  0);
    }
}

TEST(FarLodWorker, LegacyHaloUsesIndexedGeometryAndFinalAuthorityPrecedence) {
    constexpr u8 kLegacyMaterial = 83u;
    constexpr u8 kSupersededFaceMaterial = 84u;
    constexpr u8 kSdfMaterial = 231u;
    constexpr float kLegacyHeight = 18.0f;
    constexpr float kSupersededFaceHeight = 26.0f;
    constexpr int kProbeWorldX = 520;
    constexpr int kProbeWorldZ = 88;
    const TerrainGenParams params = FlatParams();

    for (const FarLodTier tier : {FarLodTier::F1, FarLodTier::F2}) {
        for (const bool home_first : {true, false}) {
            SCOPED_TRACE(::testing::Message()
                         << "tier=" << static_cast<int>(tier) << " home_first=" << home_first);
            TempSaveDir save;
            SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
            const int step = FarLodSampleStepMeters(tier);
            const u64 params_hash = ComputeTerrainParamsHash(params, 1337);

            // The region-1 sample at (520,88) belongs to chunk 32 and must
            // survive beside real chunk 31. The region-min sample at x=512 is
            // also stored in region 1, but is chunk 31's max face and must be
            // superseded by that real brick regardless of record load order.
            FarLodTile legacy = BuildPristineFarLodTile(world, tier, 1, 0, params_hash);
            legacy.edited = true;
            legacy.legacy_surface_authority = true;
            const auto set_legacy =
                [&](int world_x, int world_z, float height, u8 material, u8 flags) {
                    const std::size_t x =
                        static_cast<std::size_t>((world_x - kFarLodRegionSizeMeters) / step);
                    const std::size_t z = static_cast<std::size_t>(world_z / step);
                    const std::size_t index = x + z * legacy.samples_per_side;
                    legacy.height_q[index] = QuantizeFarLodHeight(height);
                    legacy.material[index] = material;
                    legacy.flags[index] = flags;
                    return index;
                };
            const std::size_t probe_index =
                set_legacy(kProbeWorldX,
                           kProbeWorldZ,
                           kLegacyHeight,
                           kLegacyMaterial,
                           kFarLodSampleFlagEdited | kFarLodSampleFlagWater);
            const std::size_t face_index =
                set_legacy(kFarLodRegionSizeMeters,
                           kProbeWorldZ,
                           kSupersededFaceHeight,
                           kSupersededFaceMaterial,
                           kFarLodSampleFlagEdited | kFarLodSampleFlagWater);
            std::vector<std::string> errors;
            ASSERT_TRUE(FarLodStore(save.path).save_tile(legacy, &errors));

            auto authority = std::make_shared<Chunk>(IVec3(31, 0, 5));
            world.GenerateChunkData(*authority, 1);
            ASSERT_TRUE(SetPlanarAuthority(*authority, 7.0f, kSdfMaterial));
            ASSERT_TRUE(world.adopt_streamed_chunk(authority));
            const auto home_snapshot = world.capture_far_lod_sdf_snapshot(0, 0);
            const auto target_snapshot = world.capture_far_lod_sdf_snapshot(1, 0);
            ASSERT_TRUE(home_snapshot);
            ASSERT_TRUE(target_snapshot);

            FarLodWorkerBuildOutcome home;
            FarLodWorkerBuildOutcome target;
            const auto build_home = [&]() -> bool {
                home = BuildFarLodWorkerTile(world, *home_snapshot, tier, 0, 0, save.path);
                if (!home.ok)
                    return false;
                errors.clear();
                return FarLodStore(save.path).save_tile(home.tile, &errors);
            };
            const auto build_target = [&]() -> bool {
                target = BuildFarLodWorkerTile(world, *target_snapshot, tier, 1, 0, save.path);
                return target.ok;
            };
            if (home_first) {
                ASSERT_TRUE(build_home())
                    << (home.error.empty() ? (errors.empty() ? "home save failed" : errors.front())
                                           : home.error);
                ASSERT_TRUE(build_target()) << target.error;
            } else {
                ASSERT_TRUE(build_target()) << target.error;
                ASSERT_TRUE(build_home())
                    << (home.error.empty() ? (errors.empty() ? "home save failed" : errors.front())
                                           : home.error);
            }

            ASSERT_EQ(home.tile.sdf_bricks.size(), 1u);
            EXPECT_EQ(home.tile.sdf_bricks.front().local_chunk_x, 31u);
            EXPECT_TRUE(target.tile.sdf_bricks.empty())
                << "foreign chunk 31 must remain transient in region 1";
            EXPECT_GT(CountIndexedWorldVertex(target.mesh,
                                              1,
                                              0,
                                              static_cast<float>(kProbeWorldX),
                                              kLegacyHeight,
                                              static_cast<float>(kProbeWorldZ),
                                              kLegacyMaterial),
                      0u)
                << "the exact legacy witness must be referenced by an index";
            EXPECT_EQ(CountIndexedWorldVertex(target.mesh,
                                              1,
                                              0,
                                              static_cast<float>(kFarLodRegionSizeMeters),
                                              kSupersededFaceHeight,
                                              static_cast<float>(kProbeWorldZ),
                                              kSupersededFaceMaterial),
                      0u);
            EXPECT_EQ(CountIndexedWorldPosition(target.mesh,
                                                1,
                                                0,
                                                static_cast<float>(kFarLodRegionSizeMeters),
                                                kSupersededFaceHeight,
                                                static_cast<float>(kProbeWorldZ)),
                      0u)
                << "supersession must remove the legacy plane independent of material";
            EXPECT_GT(CountIndexedWorldVertex(target.mesh,
                                              1,
                                              0,
                                              static_cast<float>(kFarLodRegionSizeMeters),
                                              7.0f,
                                              static_cast<float>(kProbeWorldZ),
                                              kSdfMaterial),
                      0u);
            EXPECT_EQ(target.tile.flags[probe_index] &
                          (kFarLodSampleFlagEdited | kFarLodSampleFlagWater),
                      kFarLodSampleFlagEdited | kFarLodSampleFlagWater);
            EXPECT_EQ(target.tile.flags[face_index] & kFarLodSampleFlagEdited, 0u);

            const auto home_segments = SharedPlaneSegments(
                home.mesh, 0, 0, SharedPlaneAxis::X, static_cast<float>(kFarLodRegionSizeMeters));
            const auto target_segments = SharedPlaneSegments(
                target.mesh, 1, 0, SharedPlaneAxis::X, static_cast<float>(kFarLodRegionSizeMeters));
            ASSERT_FALSE(home_segments.empty());
            EXPECT_EQ(home_segments, target_segments);
            EXPECT_TRUE(PlaneSegmentsContainHeight(home_segments, 7.0f));

            FarLodTile persisted_home;
            errors.clear();
            ASSERT_TRUE(
                FarLodStore(save.path).load_tile(tier, 0, 0, params_hash, persisted_home, &errors));
            ASSERT_EQ(persisted_home.sdf_bricks.size(), 1u);
            FarLodTile persisted_target;
            errors.clear();
            ASSERT_TRUE(FarLodStore(save.path).load_tile(
                tier, 1, 0, params_hash, persisted_target, &errors));
            EXPECT_TRUE(persisted_target.sdf_bricks.empty());
            EXPECT_EQ(persisted_target.height_q[probe_index], QuantizeFarLodHeight(kLegacyHeight));

            // Replay with no streamed chunks: region 0 loads its persisted
            // authority before the later region-1 legacy record. The final
            // merge sweep must still erase x=512 while retaining x=520.
            SHIELD_WorldSystem replay_world(nullptr, nullptr, params, 1337);
            const auto replay_home_snapshot = replay_world.capture_far_lod_sdf_snapshot(0, 0);
            const auto replay_target_snapshot = replay_world.capture_far_lod_sdf_snapshot(1, 0);
            ASSERT_TRUE(replay_home_snapshot);
            ASSERT_TRUE(replay_target_snapshot);
            FarLodWorkerBuildOutcome replay_home;
            FarLodWorkerBuildOutcome replay_target;
            const auto build_replay_home = [&]() -> bool {
                replay_home = BuildFarLodWorkerTile(
                    replay_world, *replay_home_snapshot, tier, 0, 0, save.path);
                return replay_home.ok;
            };
            const auto build_replay_target = [&]() -> bool {
                replay_target = BuildFarLodWorkerTile(
                    replay_world, *replay_target_snapshot, tier, 1, 0, save.path);
                return replay_target.ok;
            };
            if (home_first) {
                ASSERT_TRUE(build_replay_home()) << replay_home.error;
                ASSERT_TRUE(build_replay_target()) << replay_target.error;
            } else {
                ASSERT_TRUE(build_replay_target()) << replay_target.error;
                ASSERT_TRUE(build_replay_home()) << replay_home.error;
            }
            EXPECT_EQ(CountIndexedWorldVertex(replay_home.mesh,
                                              0,
                                              0,
                                              static_cast<float>(kFarLodRegionSizeMeters),
                                              kSupersededFaceHeight,
                                              static_cast<float>(kProbeWorldZ),
                                              kSupersededFaceMaterial),
                      0u);
            EXPECT_EQ(CountIndexedWorldPosition(replay_home.mesh,
                                                0,
                                                0,
                                                static_cast<float>(kFarLodRegionSizeMeters),
                                                kSupersededFaceHeight,
                                                static_cast<float>(kProbeWorldZ)),
                      0u);
            EXPECT_GT(CountIndexedWorldVertex(replay_target.mesh,
                                              1,
                                              0,
                                              static_cast<float>(kProbeWorldX),
                                              kLegacyHeight,
                                              static_cast<float>(kProbeWorldZ),
                                              kLegacyMaterial),
                      0u);
            EXPECT_EQ(replay_target.tile.flags[face_index] & kFarLodSampleFlagEdited, 0u);
            const auto replay_home_segments =
                SharedPlaneSegments(replay_home.mesh,
                                    0,
                                    0,
                                    SharedPlaneAxis::X,
                                    static_cast<float>(kFarLodRegionSizeMeters));
            const auto replay_target_segments =
                SharedPlaneSegments(replay_target.mesh,
                                    1,
                                    0,
                                    SharedPlaneAxis::X,
                                    static_cast<float>(kFarLodRegionSizeMeters));
            ASSERT_FALSE(replay_home_segments.empty());
            EXPECT_EQ(replay_home_segments, replay_target_segments);
            EXPECT_TRUE(PlaneSegmentsContainHeight(replay_home_segments, 7.0f));

            // Persisting the promoted target may retain height-only metadata,
            // but all synthesized 3D support remains transient.
            errors.clear();
            ASSERT_TRUE(FarLodStore(save.path).save_tile(replay_target.tile, &errors));
            FarLodTile persisted_promoted_target;
            errors.clear();
            ASSERT_TRUE(FarLodStore(save.path).load_tile(
                tier, 1, 0, params_hash, persisted_promoted_target, &errors));
            EXPECT_TRUE(persisted_promoted_target.sdf_bricks.empty());
            EXPECT_TRUE(persisted_promoted_target.sdf_density_q.empty());
            EXPECT_TRUE(persisted_promoted_target.sdf_material.empty());
            EXPECT_EQ(persisted_promoted_target.flags[probe_index] & kFarLodSampleFlagEdited,
                      kFarLodSampleFlagEdited);
            EXPECT_EQ(persisted_promoted_target.flags[face_index] & kFarLodSampleFlagEdited, 0u);

            // A real chunk-32 SDF is the exact supersession path for the
            // (520,88) witness. Its x=0 face agrees with chunk 31; the interior
            // surface at x=520 is deliberately different and replaces legacy.
            auto replacement = std::make_shared<Chunk>(IVec3(32, 0, 5));
            world.GenerateChunkData(*replacement, 1);
            ASSERT_TRUE(SetBoundaryRampedAuthority(*replacement, 7.0f, 11.0f, kSdfMaterial));
            ASSERT_TRUE(world.adopt_streamed_chunk(replacement));
            const auto replacement_snapshot = world.capture_far_lod_sdf_snapshot(1, 0);
            ASSERT_TRUE(replacement_snapshot);
            const auto superseded =
                BuildFarLodWorkerTile(world, *replacement_snapshot, tier, 1, 0, save.path);
            ASSERT_TRUE(superseded.ok) << superseded.error;
            EXPECT_EQ(superseded.tile.flags[probe_index] & kFarLodSampleFlagEdited, 0u);
            EXPECT_EQ(CountIndexedWorldVertex(superseded.mesh,
                                              1,
                                              0,
                                              static_cast<float>(kProbeWorldX),
                                              kLegacyHeight,
                                              static_cast<float>(kProbeWorldZ),
                                              kLegacyMaterial),
                      0u);
            EXPECT_EQ(CountIndexedWorldPosition(superseded.mesh,
                                                1,
                                                0,
                                                static_cast<float>(kProbeWorldX),
                                                kLegacyHeight,
                                                static_cast<float>(kProbeWorldZ)),
                      0u)
                << "real chunk 32 must remove the old height for every material";
            EXPECT_GT(CountIndexedWorldVertex(superseded.mesh,
                                              1,
                                              0,
                                              static_cast<float>(kProbeWorldX),
                                              11.0f,
                                              static_cast<float>(kProbeWorldZ),
                                              kSdfMaterial),
                      0u);

            errors.clear();
            ASSERT_TRUE(FarLodStore(save.path).save_tile(superseded.tile, &errors));
            FarLodTile persisted_superseded_target;
            errors.clear();
            ASSERT_TRUE(FarLodStore(save.path).load_tile(
                tier, 1, 0, params_hash, persisted_superseded_target, &errors));
            ASSERT_EQ(persisted_superseded_target.sdf_bricks.size(), 1u);
            EXPECT_EQ(persisted_superseded_target.sdf_bricks.front().local_chunk_x, 0u);
            EXPECT_EQ(persisted_superseded_target.sdf_bricks.front().source_kind,
                      FarLodBrickSourceKind::Authoritative);
            EXPECT_EQ(persisted_superseded_target.flags[probe_index] & kFarLodSampleFlagEdited, 0u);
            EXPECT_EQ(persisted_superseded_target.flags[face_index] & kFarLodSampleFlagEdited, 0u);
            EXPECT_FALSE(persisted_superseded_target.legacy_surface_authority);
            EXPECT_EQ(persisted_superseded_target.sdf_density_q.size(),
                      FarLodSdfBrickSampleCount(tier));
            EXPECT_EQ(persisted_superseded_target.sdf_material.size(),
                      FarLodSdfBrickSampleCount(tier));
        }
    }
}

TEST(FarLodWorker, ForeignLegacyMaxFaceAppliesExactSavedWaterBit) {
    constexpr u8 kLegacyMaterial = 83u;
    constexpr u8 kSdfMaterial = 231u;
    constexpr int kWorldX = kFarLodRegionSizeMeters;
    constexpr int kWorldZ = 88;
    constexpr float kLegacyHeight = 18.0f;

    for (const FarLodTier tier : {FarLodTier::F1, FarLodTier::F2}) {
        for (const bool saved_water : {true, false}) {
            SCOPED_TRACE(::testing::Message()
                         << "tier=" << static_cast<int>(tier) << " saved_water=" << saved_water);
            TempSaveDir save;
            TerrainGenParams params = FlatParams();
            params.height_offset = saved_water ? SEA_LEVEL + 24.0f : SEA_LEVEL - 24.0f;
            SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
            const int step = FarLodSampleStepMeters(tier);
            const u64 params_hash = ComputeTerrainParamsHash(params, 1337);

            FarLodTile foreign = BuildPristineFarLodTile(world, tier, 1, 0, params_hash);
            foreign.edited = true;
            foreign.legacy_surface_authority = true;
            const std::size_t foreign_index =
                static_cast<std::size_t>(kWorldZ / step) * foreign.samples_per_side;
            const std::size_t home_index =
                static_cast<std::size_t>(kFarLodRegionSizeMeters / step) +
                static_cast<std::size_t>(kWorldZ / step) * foreign.samples_per_side;
            const FarLodTile pristine_home =
                BuildPristineFarLodTile(world, tier, 0, 0, params_hash);
            EXPECT_EQ((pristine_home.flags[home_index] & kFarLodSampleFlagWater) != 0u,
                      !saved_water)
                << "the requested witness must begin with the opposite water state";
            foreign.height_q[foreign_index] = QuantizeFarLodHeight(kLegacyHeight);
            foreign.material[foreign_index] = kLegacyMaterial;
            foreign.flags[foreign_index] = static_cast<u8>(
                kFarLodSampleFlagEdited | (saved_water ? kFarLodSampleFlagWater : 0u));
            std::vector<std::string> errors;
            ASSERT_TRUE(FarLodStore(save.path).save_tile(foreign, &errors));

            // Chunk 30 owns a 3x3 halo ending at chunk 31. World x=512 is the
            // max face of owned chunk 31 even though floor(512/16) is chunk 32.
            auto authority = std::make_shared<Chunk>(IVec3(30, 0, 5));
            world.GenerateChunkData(*authority, 1);
            ASSERT_TRUE(SetPlanarAuthority(*authority, 7.0f, kSdfMaterial));
            ASSERT_TRUE(world.adopt_streamed_chunk(authority));
            const auto snapshot = world.capture_far_lod_sdf_snapshot(0, 0);
            ASSERT_TRUE(snapshot);
            const auto outcome = BuildFarLodWorkerTile(world, *snapshot, tier, 0, 0, save.path);
            ASSERT_TRUE(outcome.ok) << outcome.error;
            EXPECT_GT(CountIndexedWorldVertex(outcome.mesh,
                                              0,
                                              0,
                                              static_cast<float>(kWorldX),
                                              kLegacyHeight,
                                              static_cast<float>(kWorldZ),
                                              kLegacyMaterial),
                      0u);

            EXPECT_EQ((outcome.tile.flags[home_index] & kFarLodSampleFlagWater) != 0u, saved_water)
                << "the saved bit must set or clear, never merely OR";
            EXPECT_EQ(outcome.tile.flags[home_index] & kFarLodSampleFlagEdited, 0u)
                << "foreign persistence ownership must never be imported";
            EXPECT_FALSE(outcome.tile.legacy_surface_authority);
        }
    }
}

TEST(FarLodWorker, NegativeLegacyMaxFaceUsesFloorOwnedCell) {
    constexpr u8 kLegacyMaterial = 83u;
    constexpr u8 kSdfMaterial = 231u;
    constexpr int kHomeRegionX = -2;
    constexpr int kHomeRegionZ = -1;
    constexpr int kForeignRegionX = -1;
    constexpr int kWorldX = -kFarLodRegionSizeMeters;
    constexpr int kWorldZ = -88;
    constexpr float kLegacyHeight = 18.0f;

    for (const FarLodTier tier : {FarLodTier::F1, FarLodTier::F2}) {
        SCOPED_TRACE(::testing::Message() << "tier=" << static_cast<int>(tier));
        TempSaveDir save;
        const TerrainGenParams params = FlatParams();
        SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
        const int step = FarLodSampleStepMeters(tier);
        const u64 params_hash = ComputeTerrainParamsHash(params, 1337);

        FarLodTile foreign =
            BuildPristineFarLodTile(world, tier, kForeignRegionX, kHomeRegionZ, params_hash);
        foreign.edited = true;
        foreign.legacy_surface_authority = true;
        const int foreign_origin_z = kHomeRegionZ * kFarLodRegionSizeMeters;
        const std::size_t foreign_index =
            static_cast<std::size_t>((kWorldZ - foreign_origin_z) / step) *
            foreign.samples_per_side;
        foreign.height_q[foreign_index] = QuantizeFarLodHeight(kLegacyHeight);
        foreign.material[foreign_index] = kLegacyMaterial;
        foreign.flags[foreign_index] = kFarLodSampleFlagEdited | kFarLodSampleFlagWater;
        std::vector<std::string> errors;
        ASSERT_TRUE(FarLodStore(save.path).save_tile(foreign, &errors));

        // Chunk -33 ends at x=-512. Authority in chunk -34 owns -33 as
        // scratch halo, while z=-88 exercises negative non-zero remainder
        // floor division (the incident z cell belongs to chunk -6).
        auto authority = std::make_shared<Chunk>(IVec3(-34, 0, -6));
        world.GenerateChunkData(*authority, 1);
        ASSERT_TRUE(SetPlanarAuthority(*authority, 7.0f, kSdfMaterial));
        ASSERT_TRUE(world.adopt_streamed_chunk(authority));
        const auto snapshot = world.capture_far_lod_sdf_snapshot(kHomeRegionX, kHomeRegionZ);
        ASSERT_TRUE(snapshot);
        const auto outcome =
            BuildFarLodWorkerTile(world, *snapshot, tier, kHomeRegionX, kHomeRegionZ, save.path);
        ASSERT_TRUE(outcome.ok) << outcome.error;
        EXPECT_GT(CountIndexedWorldVertex(outcome.mesh,
                                          kHomeRegionX,
                                          kHomeRegionZ,
                                          static_cast<float>(kWorldX),
                                          kLegacyHeight,
                                          static_cast<float>(kWorldZ),
                                          kLegacyMaterial),
                  0u);
        EXPECT_GT(CountIndexedWorldPosition(outcome.mesh,
                                            kHomeRegionX,
                                            kHomeRegionZ,
                                            static_cast<float>(kWorldX),
                                            kLegacyHeight,
                                            static_cast<float>(kWorldZ)),
                  0u);

        const std::size_t home_index =
            static_cast<std::size_t>(kFarLodRegionSizeMeters / step) +
            static_cast<std::size_t>((kWorldZ - foreign_origin_z) / step) *
                outcome.tile.samples_per_side;
        EXPECT_EQ(outcome.tile.flags[home_index] & kFarLodSampleFlagEdited, 0u);
        EXPECT_FALSE(outcome.tile.legacy_surface_authority);
        ASSERT_EQ(outcome.tile.sdf_bricks.size(), 1u);
        EXPECT_EQ(outcome.tile.sdf_bricks.front().local_chunk_x, 30u);
    }
}

TEST(FarLodWorker, DurableChunkTruthRepairsOldFarWithoutAStreamedSnapshot) {
    constexpr u8 kOldMaterial = 231u;
    constexpr u8 kNewMaterial = 229u;
    for (const FarLodTier tier : {FarLodTier::F1, FarLodTier::F2}) {
        SCOPED_TRACE(::testing::Message() << "tier=" << static_cast<int>(tier));
        const TerrainGenParams params = FlatParams();
        TempSaveDir save;

        // Establish old derived far bytes first.
        SHIELD_WorldSystem old_world(nullptr, nullptr, params, 1337);
        auto old_chunk = std::make_shared<Chunk>(IVec3(31, 0, 5));
        old_world.GenerateChunkData(*old_chunk, 1);
        ASSERT_TRUE(SetPlanarAuthority(*old_chunk, 7.0f, kOldMaterial));
        ASSERT_TRUE(old_world.adopt_streamed_chunk(old_chunk));
        const auto old_home_snapshot = old_world.capture_far_lod_sdf_snapshot(0, 0);
        const auto old_target_snapshot = old_world.capture_far_lod_sdf_snapshot(1, 0);
        ASSERT_TRUE(old_home_snapshot);
        ASSERT_TRUE(old_target_snapshot);
        const auto old_home = BuildFarLodWorkerTile(old_world, *old_home_snapshot, tier, 0, 0, {});
        const auto old_target =
            BuildFarLodWorkerTile(old_world, *old_target_snapshot, tier, 1, 0, {});
        ASSERT_TRUE(old_home.ok) << old_home.error;
        ASSERT_TRUE(old_target.ok) << old_target.error;
        std::vector<std::string> errors;
        ASSERT_TRUE(FarLodStore(save.path).save_tile(old_home.tile, &errors));

        // Phase A commits newer authoritative chunk truth while deliberately
        // preserving the older FSD2 record in the same LMR1 container.
        SHIELD_WorldSystem generation_world(nullptr, nullptr, params, 1337);
        auto durable_chunk = std::make_shared<Chunk>(IVec3(31, 0, 5));
        generation_world.GenerateChunkData(*durable_chunk, 1);
        ASSERT_TRUE(SetPlanarAuthority(*durable_chunk, 11.0f, kNewMaterial));
        WorldStreamingState durable_state;
        durable_state.insert_chunk(durable_chunk);
        Persistence::WorldSaveService save_service;
        errors.clear();
        ASSERT_TRUE(save_service.save_world(durable_state, save.path, &errors));
        ASSERT_TRUE(errors.empty());

        // Simulate process restart with no resident chunks. The far worker must
        // read lod-0 authority from the bounded 3x3 LMR1 set and overlay it over
        // old derived far bytes; an empty live snapshot cannot justify the old
        // cache winning.
        SHIELD_WorldSystem recovery_world(nullptr, nullptr, params, 1337);
        const auto recovery_home_snapshot = recovery_world.capture_far_lod_sdf_snapshot(0, 0);
        const auto recovery_target_snapshot = recovery_world.capture_far_lod_sdf_snapshot(1, 0);
        ASSERT_TRUE(recovery_home_snapshot);
        ASSERT_TRUE(recovery_target_snapshot);
        ASSERT_TRUE(recovery_home_snapshot->entries.empty());
        ASSERT_TRUE(recovery_target_snapshot->entries.empty());
        const auto recovered_home =
            BuildFarLodWorkerTile(recovery_world, *recovery_home_snapshot, tier, 0, 0, save.path);
        const auto recovered_target =
            BuildFarLodWorkerTile(recovery_world, *recovery_target_snapshot, tier, 1, 0, save.path);
        ASSERT_TRUE(recovered_home.ok) << recovered_home.error;
        ASSERT_TRUE(recovered_target.ok) << recovered_target.error;

        // A live-only build of the same new authority is the reference. Mesh
        // and reduced payload bytes must match even though persisted revisions
        // remain monotonic across the restart.
        SHIELD_WorldSystem reference_world(nullptr, nullptr, params, 1337);
        auto reference_chunk = std::make_shared<Chunk>(IVec3(31, 0, 5));
        reference_world.GenerateChunkData(*reference_chunk, 1);
        ASSERT_TRUE(SetPlanarAuthority(*reference_chunk, 11.0f, kNewMaterial));
        ASSERT_TRUE(reference_world.adopt_streamed_chunk(reference_chunk));
        const auto reference_home_snapshot = reference_world.capture_far_lod_sdf_snapshot(0, 0);
        const auto reference_target_snapshot = reference_world.capture_far_lod_sdf_snapshot(1, 0);
        ASSERT_TRUE(reference_home_snapshot);
        ASSERT_TRUE(reference_target_snapshot);
        const auto reference_home =
            BuildFarLodWorkerTile(reference_world, *reference_home_snapshot, tier, 0, 0, {});
        const auto reference_target =
            BuildFarLodWorkerTile(reference_world, *reference_target_snapshot, tier, 1, 0, {});
        ASSERT_TRUE(reference_home.ok) << reference_home.error;
        ASSERT_TRUE(reference_target.ok) << reference_target.error;

        EXPECT_EQ(recovered_home.tile.sdf_density_q, reference_home.tile.sdf_density_q);
        EXPECT_EQ(recovered_home.tile.sdf_material, reference_home.tile.sdf_material);
        EXPECT_EQ(HashMesh(recovered_home.mesh), HashMesh(reference_home.mesh));
        EXPECT_EQ(ComputeFarLodTileHash(recovered_target.tile),
                  ComputeFarLodTileHash(reference_target.tile));
        EXPECT_EQ(HashMesh(recovered_target.mesh), HashMesh(reference_target.mesh));
        EXPECT_NE(HashMesh(recovered_target.mesh), HashMesh(old_target.mesh));

        // Phase B may now advance FSD2 without altering Phase-A chunk truth.
        errors.clear();
        ASSERT_TRUE(FarLodStore(save.path).save_tile(recovered_home.tile, &errors));
        std::vector<std::shared_ptr<Chunk>> durable_after_phase_b;
        ASSERT_TRUE(Persistence::WorldSaveService::read_region_chunks(
            Persistence::WorldSaveService::region_file_path(save.path, 0, 0),
            durable_after_phase_b,
            &errors));
        ASSERT_EQ(durable_after_phase_b.size(), 1u);
        EXPECT_EQ(durable_after_phase_b.front()->sdf_data, durable_chunk->sdf_data);
        EXPECT_EQ(durable_after_phase_b.front()->material_data, durable_chunk->material_data);
    }
}
