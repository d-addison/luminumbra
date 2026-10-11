#include "gtest/gtest.h"

#include "luminumbra_client/debug/DebugCamera.h"
#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FarLodSystem.h"
#include "luminumbra_client/rendering/Shader.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#define GLFW_INCLUDE_NONE
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/MarchingCubes.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace Luminumbra;
using namespace Luminumbra::Rendering;
using namespace Luminumbra::Systems;
using namespace Luminumbra::World;
using Luminumbra::Persistence::WorldSaveService;

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

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

TEST(FarLodWorker, ObsoleteHomePayloadRefused) {
    const TerrainGenParams params = FlatParams();
    TempSaveDir save;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    for (const auto tier : {FarLodTier::F1, FarLodTier::F2}) {
        const int rx = 1;
        const auto path = WorldSaveService::region_file_path(save.path, rx, 0);
        // Raw record seam builds an obsolete/future/corrupt fixture without a migration writer.
        WorldSaveService::ContainerRecord record;
        record.id = FarLodStore::tile_record_id(tier, rx, 0);
        record.lod_level = static_cast<u8>(tier);
        record.flags = 1u;
        record.payload = std::string("FSD2", 4) + char(2) + char(0);
        ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
        const auto bytes = ReadFileBytes(path);
        const auto snapshot = world.capture_far_lod_sdf_snapshot(rx, 0);
        ASSERT_TRUE(snapshot);
        const auto outcome = BuildFarLodWorkerTile(world, *snapshot, tier, rx, 0, save.path);
        EXPECT_FALSE(outcome.ok);
        EXPECT_FALSE(outcome.error.empty());
        EXPECT_TRUE(outcome.mesh.vertices.empty());
        EXPECT_EQ(ReadFileBytes(path), bytes);
    }
}

TEST(FarLodWorker, ObsoleteHaloPayloadRefused) {
    const TerrainGenParams params = FlatParams();
    TempSaveDir save;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    for (const auto tier : {FarLodTier::F1, FarLodTier::F2}) {
        const int rx = 1;
        const auto path = WorldSaveService::region_file_path(save.path, rx, 0);
        // Raw record seam builds an obsolete/future/corrupt fixture without a migration writer.
        WorldSaveService::ContainerRecord record;
        record.id = FarLodStore::tile_record_id(tier, rx, 0);
        record.lod_level = static_cast<u8>(tier);
        record.flags = 1u;
        record.payload = std::string("FSD2", 4) + char(2) + char(0);
        ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
        const auto bytes = ReadFileBytes(path);
        const auto snapshot = world.capture_far_lod_sdf_snapshot(0, 0);
        ASSERT_TRUE(snapshot);
        const auto outcome = BuildFarLodWorkerTile(world, *snapshot, tier, 0, 0, save.path);
        EXPECT_FALSE(outcome.ok);
        EXPECT_FALSE(outcome.error.empty());
        EXPECT_TRUE(outcome.mesh.vertices.empty());
        EXPECT_EQ(ReadFileBytes(path), bytes);
    }
}
TEST(FarLodWorker, FuturePayloadRefused) {
    const TerrainGenParams params = FlatParams();
    TempSaveDir save;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    for (const auto tier : {FarLodTier::F1, FarLodTier::F2}) {
        const int rx = -1;
        const auto path = WorldSaveService::region_file_path(save.path, rx, 0);
        // Raw record seam builds an obsolete/future/corrupt fixture without a migration writer.
        WorldSaveService::ContainerRecord record;
        record.id = FarLodStore::tile_record_id(tier, rx, 0);
        record.lod_level = static_cast<u8>(tier);
        record.flags = 1u;
        record.payload = std::string("FSD2", 4) + char(4) + char(0);
        ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
        const auto bytes = ReadFileBytes(path);
        const auto snapshot = world.capture_far_lod_sdf_snapshot(rx, 0);
        ASSERT_TRUE(snapshot);
        const auto outcome = BuildFarLodWorkerTile(world, *snapshot, tier, rx, 0, save.path);
        EXPECT_FALSE(outcome.ok);
        EXPECT_FALSE(outcome.error.empty());
        EXPECT_TRUE(outcome.mesh.vertices.empty());
        EXPECT_EQ(ReadFileBytes(path), bytes);
    }
}

TEST(FarLodWorker, CorruptPayloadRefused) {
    const TerrainGenParams params = FlatParams();
    TempSaveDir save;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    for (const auto tier : {FarLodTier::F1, FarLodTier::F2}) {
        const int rx = -1;
        const auto path = WorldSaveService::region_file_path(save.path, rx, 0);
        // Raw record seam builds an obsolete/future/corrupt fixture without a migration writer.
        WorldSaveService::ContainerRecord record;
        record.id = FarLodStore::tile_record_id(tier, rx, 0);
        record.lod_level = static_cast<u8>(tier);
        record.flags = 1u;
        record.payload = std::string("FSD2", 4) + char(3) + char(0);
        ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
        const auto bytes = ReadFileBytes(path);
        const auto snapshot = world.capture_far_lod_sdf_snapshot(rx, 0);
        ASSERT_TRUE(snapshot);
        const auto outcome = BuildFarLodWorkerTile(world, *snapshot, tier, rx, 0, save.path);
        EXPECT_FALSE(outcome.ok);
        EXPECT_FALSE(outcome.error.empty());
        EXPECT_TRUE(outcome.mesh.vertices.empty());
        EXPECT_EQ(ReadFileBytes(path), bytes);
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

TEST(TreeImpostorMaterial, FilteredLeafEdgesPreserveColorNormalRoughnessAndOcclusion) {
    if (!glfwInit())
        GTEST_SKIP() << "glfwInit failed";
    struct GlLifetime {
        GLFWwindow* window = nullptr;
        GLuint fbo = 0, vao = 0;
        std::array<GLuint, 7> textures{};
        ~GlLifetime() {
            if (fbo) {
                glDeleteFramebuffers(1, &fbo);
                glDeleteVertexArrays(1, &vao);
                glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
            }
            if (window)
                glfwDestroyWindow(window);
            glfwTerminate();
        }
    } gl;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    gl.window = glfwCreateWindow(16, 16, "impostor material", nullptr, nullptr);
    if (!gl.window)
        GTEST_SKIP() << "OpenGL 4.5 context unavailable";
    glfwMakeContextCurrent(gl.window);
    ASSERT_TRUE(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)));
    TempSaveDir fixture;
    const auto vertex_path = fixture.path / "impostor.vert";
    std::ofstream(vertex_path) << R"(#version 450 core
uniform vec2 sampleUV;
out vec2 vQuadUV;
out vec3 vViewDir, vWorldPos, vViewPos;
flat out mat3 vObjectToWorld;
flat out vec3 vTint;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0, 1);
    vQuadUV = sampleUV; vViewDir = vec3(0, 1, 0);
    vWorldPos = vec3(0); vViewPos = vec3(0, 0, -10); vTint = vec3(1);
    vObjectToWorld = mat3(0,0,-1, 0,1,0, 1,0,0); // object +Z becomes world +X
}
)";
    const auto source_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto fragment_path = source_root / "res/shaders/tree_impostor.frag";
    Shader shader(vertex_path.string().c_str(), fragment_path.string().c_str());
    ASSERT_TRUE(shader.IsValid()) << shader.Diagnostic();
    glGenFramebuffers(1, &gl.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gl.fbo);
    glGenTextures(static_cast<GLsizei>(gl.textures.size()), gl.textures.data());
    std::array<GLenum, 5> attachments{};
    for (std::size_t i = 0; i < attachments.size(); ++i) {
        attachments[i] = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
        glBindTexture(GL_TEXTURE_2D, gl.textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachments[i], GL_TEXTURE_2D, gl.textures[i], 0);
    }
    glDrawBuffers(static_cast<GLsizei>(attachments.size()), attachments.data());
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
    // One occupied texel and transparent black beside it, as emitted by the bake.
    const std::array<float, 8> albedo{0.2f, 0.4f, 0.05f, 1.0f, 0, 0, 0, 0};
    const std::array<float, 8> normal_surface{0.5f, 0.5f, 0.8f, 0.4f, 0, 0, 0, 0};
    for (int i = 0; i < 2; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, gl.textures[static_cast<std::size_t>(5 + i)]);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA32F,
                     2,
                     1,
                     0,
                     GL_RGBA,
                     GL_FLOAT,
                     i == 0 ? albedo.data() : normal_surface.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    shader.use();
    shader.setInt("u_albedo", 0);
    shader.setInt("u_normal", 1);
    shader.setFloat("u_grid", 1.0f);
    shader.setFloat("u_materialId", 3.0f / 255.0f);
    shader.setMat4("u_view", glm::mat4(1));
    glGenVertexArrays(1, &gl.vao);
    glBindVertexArray(gl.vao);
    glViewport(0, 0, 1, 1);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    const std::array<float, 4> clear{-1, -1, -1, -1};
    for (float u : {0.25f, 0.375f, 0.625f}) {
        SCOPED_TRACE(u);
        for (int i = 0; i < 5; ++i)
            glClearBufferfv(GL_COLOR, i, clear.data());
        shader.setVec2("sampleUV", u, 0.5f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::array<float, 4> color{}, normal{}, surface{};
        glReadBuffer(GL_COLOR_ATTACHMENT2);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, color.data());
        if (u > 0.5f) {
            EXPECT_EQ(color, clear) << "low-coverage gaps must remain empty";
            continue;
        }
        for (int i = 0; i < 3; ++i)
            EXPECT_NEAR(color[i], albedo[i], 0.002f);
        EXPECT_NEAR(color[3], 0.8f, 0.002f);
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, normal.data());
        EXPECT_NEAR(normal[0], 1.0f, 0.002f);
        EXPECT_NEAR(normal[1], 0.5f, 0.002f);
        glReadBuffer(GL_COLOR_ATTACHMENT3);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, surface.data());
        EXPECT_NEAR(surface[0], 0.0f, 0.002f);
        EXPECT_NEAR(surface[1], 0.4f, 0.002f);
    }
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(FarLodWorker, ElevatedCameraSeesTerrainInsideItsOwnRegion) {
    if (!glfwInit())
        GTEST_SKIP() << "glfwInit failed";
    struct GlLifetime {
        GLFWwindow* window = nullptr;
        ~GlLifetime() {
            if (window)
                glfwDestroyWindow(window);
            glfwTerminate();
        }
    } gl;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    gl.window = glfwCreateWindow(64, 64, "far region coverage", nullptr, nullptr);
    if (!gl.window)
        GTEST_SKIP() << "OpenGL 4.5 context unavailable";
    glfwMakeContextCurrent(gl.window);
    ASSERT_TRUE(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)));
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    TempSaveDir fixture;
    const auto vertex_path = fixture.path / "coverage.vert";
    const auto fragment_path = fixture.path / "coverage.frag";
    std::ofstream(vertex_path) << R"(#version 450 core
layout(location=0) in vec3 position;
uniform mat4 model, view, projection;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[1]; };
void main() { gl_Position = projection * view * model * vec4(position, 1); gl_ClipDistance[0] = 1; }
)";
    std::ofstream(fragment_path) << R"(#version 450 core
out vec4 color;
void main() { color = vec4(1); }
)";
    Shader shader(vertex_path.string().c_str(), fragment_path.string().c_str());
    ASSERT_TRUE(shader.IsValid());
    // One FIFO worker lets a queued fence wait for all preceding tile jobs,
    // without sleeps or exposing scheduler internals to the fixture.
    JobSystem jobs;
    jobs.startup(1);
    SHIELD_WorldSystem world(nullptr, nullptr, FlatParams(), 1337);
    FarLodSystem far;
    far.attach_job_system(&jobs);
    const glm::vec3 camera(256, 1200, 256);
    for (int frame = 0; frame < 12; ++frame) {
        far.update(world, camera);
        jobs.wait(jobs.dispatch_batch({[] {
        }}));
    }
    ASSERT_GT(far.stats().regions_resident, 0u);
    shader.use();
    const auto view = glm::lookAt(camera, camera - glm::vec3(0, 1, 0), glm::vec3(0, 0, -1));
    shader.setMat4("view", view);
    shader.setMat4("projection",
                   ReversedZPerspective(glm::radians(15.0f), 1.0f, NEAR_PLANE, FAR_PLANE));
    const glm::vec4 planes[6]{}; // hardware frustum clips the actual geometry
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    std::size_t draws = 0, indices = 0;
    far.draw_gbuffer(shader, view, planes, draws, indices);
    std::array<unsigned char, 64 * 64 * 4> pixels{};
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    // This entire narrow view lies in the camera's 512 m region. Other
    // resident regions cannot fill a missing centre tile from this camera.
    for (int y : {16, 32, 48})
        for (int x : {16, 32, 48})
            EXPECT_EQ(pixels[(y * 64 + x) * 4], 255) << "missing ground at " << x << "," << y;
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
    far.shutdown();
    jobs.shutdown();
}

// Uses the production vertex and fragment shaders, the real region scheduler,
// and a small explicit live patch. This diagnoses one draw predicate; it does
// not approve enabling analytic far terrain at cave/edited boundaries.
TEST(FarLodWorker, LowCameraCoverageDiagnosticUsesProductionClippingAcrossRegionBoundaries) {
    if (!glfwInit())
        GTEST_SKIP() << "glfwInit failed";
    struct GlLifetime {
        GLFWwindow* window = nullptr;
        bool loaded = false;
        GLuint fbo = 0, depth = 0, vao = 0, vbo = 0, lut = 0, array = 0;
        std::array<GLuint, 5> attachments{};
        ~GlLifetime() {
            if (loaded) {
                glDeleteFramebuffers(1, &fbo);
                glDeleteRenderbuffers(1, &depth);
                glDeleteVertexArrays(1, &vao);
                glDeleteBuffers(1, &vbo);
                glDeleteTextures(1, &lut);
                glDeleteTextures(1, &array);
                glDeleteTextures(static_cast<GLsizei>(attachments.size()), attachments.data());
            }
            if (window)
                glfwDestroyWindow(window);
            glfwTerminate();
        }
    } gl;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    gl.window = glfwCreateWindow(128, 64, "low camera coverage diagnostic", nullptr, nullptr);
    if (!gl.window)
        GTEST_SKIP() << "OpenGL 4.5 context unavailable";
    glfwMakeContextCurrent(gl.window);
    ASSERT_TRUE(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)));
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    gl.loaded = true;
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto vertex = root / "res/shaders/g_buffer.vert";
    const auto fragment = root / "res/shaders/g_buffer.frag";
    Shader shader(vertex.string().c_str(), fragment.string().c_str());
    ASSERT_TRUE(shader.IsValid()) << shader.Diagnostic();
    glGenFramebuffers(1, &gl.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gl.fbo);
    glGenTextures(static_cast<GLsizei>(gl.attachments.size()), gl.attachments.data());
    std::array<GLenum, 5> buffers{};
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        buffers[i] = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
        glBindTexture(GL_TEXTURE_2D, gl.attachments[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 128, 64, 0, GL_RGBA, GL_FLOAT, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, buffers[i], GL_TEXTURE_2D, gl.attachments[i], 0);
    }
    glDrawBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
    glGenRenderbuffers(1, &gl.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, gl.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, 128, 64);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, gl.depth);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
    // All sampler types get distinct compatible bindings even when the flat
    // material branch does not fetch a texture. No fixture clipping shader.
    glGenTextures(1, &gl.lut);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl.lut);
    const std::array<float, 4> value{0, 0.7f, 1, 0};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, value.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &gl.array);
    for (int unit = 1; unit <= 6; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, gl.array);
    }
    const std::array<unsigned char, 4> texel{128, 128, 255, 255};
    glTexImage3D(
        GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel.data());
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    shader.use();
    shader.setInt("u_materialLUT", 0);
    shader.setInt("u_terrainTextures", 1);
    shader.setInt("u_terrainNormals", 2);
    shader.setInt("u_skinnedTextures", 3);
    shader.setInt("u_terrainRoughness", 4);
    shader.setInt("u_macroRockOverlay", 0);
    shader.setInt("u_useInstanceOrigin", 0);
    shader.setMat4("u_prev_view_proj", glm::mat4(1));
    shader.setVec2("u_inv_screen_size", 1.0f / 128.0f, 1.0f / 64.0f);
    glGenVertexArrays(1, &gl.vao);
    glGenBuffers(1, &gl.vbo);
    glBindVertexArray(gl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(VoxelVertex),
                          reinterpret_cast<void*>(offsetof(VoxelVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(VoxelVertex),
                          reinterpret_cast<void*>(offsetof(VoxelVertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2,
                           1,
                           GL_UNSIGNED_INT,
                           sizeof(VoxelVertex),
                           reinterpret_cast<void*>(offsetof(VoxelVertex, material_id)));
    std::array<VoxelVertex, 6> live{};
    const std::array<glm::vec2, 6> corners{
        {{-128, -128}, {128, -128}, {128, 128}, {-128, -128}, {128, 128}, {-128, 128}}};
    for (std::size_t i = 0; i < live.size(); ++i) {
        live[i].position = glm::vec3(corners[i].x, 12, corners[i].y);
        live[i].normal = glm::vec3(0, 1, 0);
        live[i].material_id = 1;
    }
    glBufferData(GL_ARRAY_BUFFER, sizeof(live), live.data(), GL_STATIC_DRAW);
    glViewport(0, 0, 128, 64);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glClearDepth(0.0);
    JobSystem jobs;
    jobs.startup(1);
    SHIELD_WorldSystem world(nullptr, nullptr, FlatParams(), 1337);
    FarLodSystem far;
    far.attach_job_system(&jobs);
    far.set_coverage_diagnostics_enabled(true);
    const glm::mat4 projection =
        ReversedZPerspective(glm::radians(45.0f), 2.0f, NEAR_PLANE, FAR_PLANE);
    struct Pose {
        glm::vec3 camera;
        float yaw;
    };
    const std::array<Pose, 4> poses{
        {{{8, 56, 8}, 35}, {{504, 56, 504}, 215}, {{-8, 56, -8}, 215}, {{-504, 56, -504}, 35}}};
    for (const auto& pose : poses) {
        SCOPED_TRACE(::testing::Message()
                     << pose.camera.x << ',' << pose.camera.z << " yaw " << pose.yaw);
        const float yaw = glm::radians(pose.yaw), pitch = glm::radians(-6.0f);
        const glm::vec3 horizontal(std::cos(yaw), 0, std::sin(yaw));
        const auto view = glm::lookAt(pose.camera,
                                      pose.camera + horizontal * std::cos(pitch) +
                                          glm::vec3(0, std::sin(pitch), 0),
                                      glm::vec3(0, 1, 0));
        for (int frame = 0; frame < 12; ++frame) {
            far.update(world, pose.camera);
            jobs.wait(jobs.dispatch_batch({[] {
            }}));
        }
        const int rx = static_cast<int>(std::floor(pose.camera.x / 512.0f));
        const int rz = static_cast<int>(std::floor(pose.camera.z / 512.0f));
        const auto camera_row = [&]() -> const FarLodSystem::RegionCoverage* {
            for (const auto& r : far.coverage_diagnostics().neighbourhood)
                if (r.rx == rx && r.rz == rz)
                    return &r;
            return nullptr;
        };
        const auto& coverage = far.coverage_diagnostics();
        EXPECT_EQ(coverage.wanted, coverage.resident + coverage.missing);
        EXPECT_LE(coverage.stale, coverage.resident);
        EXPECT_EQ(coverage.neighbourhood.size(), 9u);
        ASSERT_NE(camera_row(), nullptr);
        ASSERT_TRUE(camera_row()->resident);
        EXPECT_TRUE(camera_row()->current);
        const auto sample = [&](float distance) {
            const auto point = pose.camera + horizontal * distance;
            const auto clip = projection * view * glm::vec4(point.x, 12, point.z, 1);
            const int x = static_cast<int>((clip.x / clip.w * 0.5f + 0.5f) * 128);
            const int y = static_cast<int>((clip.y / clip.w * 0.5f + 0.5f) * 64);
            EXPECT_GE(x, 0);
            EXPECT_LT(x, 128);
            EXPECT_GE(y, 0);
            EXPECT_LT(y, 64);
            float depth = 0;
            glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
            return depth;
        };
        const auto draw = [&](bool bypass) {
            far.set_coverage_camera_region_guard_bypass(bypass);
            glBindFramebuffer(GL_FRAMEBUFFER, gl.fbo);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            shader.use();
            shader.setMat4("view", view);
            shader.setMat4("projection", projection);
            shader.setMat3("u_normalViewMatrix", glm::mat3(view));
            shader.setMat4(
                "model", glm::translate(glm::mat4(1), glm::vec3(pose.camera.x, 0, pose.camera.z)));
            shader.setMat3("normalMatrix", glm::mat3(view));
            glBindVertexArray(gl.vao);
            glDrawArrays(GL_TRIANGLES, 0, 6); // the explicit live patch owns the near point
            const glm::vec4 planes[6]{}; // actual hardware clip, without a synthetic frustum veto
            std::size_t draws = 0, indices = 0;
            far.draw_gbuffer(shader, view, planes, draws, indices);
            return std::array<float, 2>{sample(100), sample(300)};
        };
        const auto guarded = draw(false);
        ASSERT_TRUE(far.coverage_diagnostics().draw_observed);
        EXPECT_STREQ(camera_row()->terrain_decision, "camera_region_guard");
        EXPECT_GT(guarded[0], 0.0f) << "live near patch missing";
        EXPECT_EQ(guarded[1], 0.0f) << "expected diagnostic guard footprint outside live patch";
        const auto bypassed = draw(true);
        EXPECT_STREQ(camera_row()->terrain_decision, "submitted");
        EXPECT_EQ(bypassed[0], guarded[0]) << "176 m production clip must preserve near ownership";
        EXPECT_GT(bypassed[1], 0.0f) << "production shader should cover the in-region far point";
        EXPECT_GT(bypassed[0], bypassed[1]) << "reversed depth must keep the live patch nearer";
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
        // A bypass request cannot modify ordinary rendering without diagnostics.
        far.set_coverage_diagnostics_enabled(false);
        const auto disabled = draw(true);
        EXPECT_EQ(disabled, guarded);
        EXPECT_FALSE(far.coverage_diagnostics().enabled);
        far.set_coverage_diagnostics_enabled(true);
    }
    far.shutdown();
    jobs.shutdown();
}

TEST(FarLodWorker, CaveLocatorReturnsAnAirPositionInTheDefaultPreset) {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto preset =
        Luminumbra::world::LoadTerrainPreset(root / "worlds/atlas/presets/default.json");
    ASSERT_TRUE(preset.ok);
    SHIELD_WorldSystem world(nullptr, nullptr, preset.params, 424242);
    const auto pose = Luminumbra::Debug::FindEnclosedCave(world, glm::vec3(8, 17.15f, 8), 256);
    ASSERT_TRUE(pose.has_value());
    if (pose.has_value()) {
        EXPECT_GE(world.get_density_at(pose.value().pos), 0.0f)
            << "cave capture must not put the camera inside solid rock";
    }
}

TEST(FarLodWorker, DefaultCaveStreamsEveryNeighbouringMesh) {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto preset =
        Luminumbra::world::LoadTerrainPreset(root / "worlds/atlas/presets/default.json");
    ASSERT_TRUE(preset.ok);
    JobSystem jobs;
    jobs.startup();
    {
        SHIELD_WorldSystem world(&jobs, nullptr, preset.params, 424242);
        world.debug_set_streaming_radius_cap(8);
        entt::registry registry;
        const Vec3 camera(8.469266f, -46.070644f, 11.695518f);
        ASSERT_GT(world.get_density_at(camera), 0.0f);
        for (int frame = 0; frame < 48; ++frame) {
            world.update(registry, camera, nullptr);
            world.wait_for_streaming_jobs();
        }
        // At the default 110-degree FOV, the leftward cave wall is over 100 m
        // away. The old 32 m neighbourhood exposed sky inside this air pocket.
        for (const auto coords : {IVec3(4, -6, -7), IVec3(2, -3, -4)}) {
            const auto wall = world.find_streamed_chunk(coords);
            ASSERT_NE(wall, nullptr) << "visible cave wall must be resident";
            EXPECT_EQ(wall->get_state(), ChunkState::Ready);
            EXPECT_FALSE(wall->mesh_indices.empty());
        }
        EXPECT_LT(world.get_runtime_chunk_stats().total_chunks, 4000u);
        const auto center = world.world_to_chunk_coords(camera);
        for (int dz = -2; dz <= 2; ++dz) {
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const auto coords = center + IVec3(dx, dy, dz);
                    SCOPED_TRACE(::testing::Message()
                                 << coords.x << ',' << coords.y << ',' << coords.z);
                    const auto streamed = world.find_streamed_chunk(coords);
                    ASSERT_NE(streamed, nullptr);
                    EXPECT_EQ(streamed->get_state(), ChunkState::Ready);
                    Chunk reference(coords);
                    world.GenerateChunkData(reference, 1);
                    MarchingCubes::PolygoniseTerrain(world, reference, 0.0f, 1);
                    EXPECT_EQ(streamed->sdf_data, reference.sdf_data);
                    EXPECT_EQ(streamed->mesh_indices, reference.mesh_indices);
                }
            }
        }
    }
    jobs.shutdown();
}
