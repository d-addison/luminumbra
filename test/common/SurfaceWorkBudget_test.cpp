#include "../fixtures/TerrainFixtureParams.h"
#include "../support/UniqueTempPath.h"
#include "luminumbra/core/Types.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/SurfaceWorkBudget.h"
#include "luminumbra_server/ServerCliOptions.h"
#include "luminumbra_server/ServerWorldRunner.h"
#include "luminumbra_server/modes/ModeHelpers.h"
#include "luminumbra_server/modes/Modes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
namespace budget = Luminumbra::World;
namespace replay = Luminumbra::Replay;
namespace fs = std::filesystem;

TEST(SurfaceWorkBudget, CheckedColumnsAndExactVerticalBudget) {
    EXPECT_EQ(budget::SurfaceColumnCount(0), 1u);
    EXPECT_EQ(budget::SurfaceColumnCount(4), 81u);
    EXPECT_EQ(budget::SurfaceColumnCount(12), 625u);
    for (const auto radius : {13ULL,
                              10000ULL,
                              1073741824ULL,
                              4294967295ULL,
                              std::numeric_limits<unsigned long long>::max()})
        EXPECT_FALSE(budget::SurfaceColumnCount(radius).has_value());
    std::size_t total = 0;
    EXPECT_TRUE(budget::AddSurfaceColumnChunks(-4096, 4093, total));
    EXPECT_EQ(total, 8192u);
    EXPECT_FALSE(budget::AddSurfaceColumnChunks(0, 0, total));
    EXPECT_EQ(total, 8192u);
    for (const auto& [min_y, max_y] :
         {std::pair{0, 8190},
          std::pair{1, 0},
          std::pair{std::numeric_limits<int>::min(), 0},
          std::pair{0, std::numeric_limits<int>::max() - 1},
          std::pair{0, std::numeric_limits<int>::max()},
          std::pair{std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}}) {
        total = 0;
        EXPECT_FALSE(budget::AddSurfaceColumnChunks(min_y, max_y, total));
        EXPECT_EQ(total, 0u);
    }
    total = budget::kMaxSurfaceChunks + 1;
    EXPECT_FALSE(budget::AddSurfaceColumnChunks(0, 0, total));
    EXPECT_EQ(total, budget::kMaxSurfaceChunks + 1);
    for (const int endpoint :
         {std::numeric_limits<int>::min() + 1, std::numeric_limits<int>::max() - 2}) {
        total = 0;
        EXPECT_TRUE(budget::AddSurfaceColumnChunks(endpoint, endpoint, total));
        EXPECT_EQ(total, 3u);
    }
}

TEST(SurfaceWorkBudget, DecimalAdmissionHasNoOverflowOrPartialFallback) {
    int radius = 4;
    for (const auto value : {"",
                             "-1",
                             "13",
                             "10000",
                             "2147483647",
                             "4294967295",
                             "99999999999999999999999999",
                             "1junk",
                             " 1",
                             "+1",
                             "1.0"}) {
        EXPECT_FALSE(budget::TryParseSurfaceRadius(value, radius));
        EXPECT_EQ(radius, 4);
    }
    EXPECT_TRUE(budget::TryParseSurfaceRadius("0", radius));
    EXPECT_EQ(radius, 0);
    EXPECT_TRUE(budget::TryParseSurfaceRadius("12", radius));
    EXPECT_EQ(radius, 12);
    EXPECT_TRUE(budget::SurfaceRadiiSupported(12, 12));
    EXPECT_TRUE(budget::SurfaceRadiiSupported(0, 0));
    EXPECT_FALSE(budget::SurfaceRadiiSupported(-1, 0));
    EXPECT_FALSE(budget::SurfaceRadiiSupported(0, -1));
    EXPECT_FALSE(budget::SurfaceRadiiSupported(13, 0));
    EXPECT_FALSE(budget::SurfaceRadiiSupported(0, 13));
}

struct SurfaceWorkAdmissionData {
    fs::path root;
};

class SurfaceWorkAdmission : public ::testing::Test, protected SurfaceWorkAdmissionData {
protected:
    void SetUp() override {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("surface-admission-");
    }
    void TearDown() override {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

TEST_F(SurfaceWorkAdmission, HeaderDecodeAndWriterRefuseBothRadiiBeforeOutput) {
    const auto path = root / "replay.lrec";
    // Mutate actual encoded header bytes so writer admission cannot mask a
    // missing decoder check. The two u32 fields are at fixed LREC1 offsets.
    for (const std::streamoff offset : {34, 38}) {
        for (const std::uint32_t radius : {13u, 10000u, 1073741824u, 0xffffffffu}) {
            replay::ReplayHeader header;
            header.surface_radius = 12;
            header.collision_radius = 12;
            replay::ReplayWriter writer;
            ASSERT_TRUE(writer.Open(path.string(), header));
            ASSERT_TRUE(writer.Finalize(0));
            ASSERT_TRUE(replay::ReadReplay(path.string()).has_value());
            {
                std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
                ASSERT_TRUE(stream);
                stream.seekp(offset);
                for (unsigned shift = 0; shift < 32; shift += 8)
                    stream.put(static_cast<char>((radius >> shift) & 0xffu));
                stream.flush();
                ASSERT_TRUE(stream.good());
            }
            EXPECT_FALSE(replay::ReadReplay(path.string()).has_value());
            ServerCliOptions options;
            options.root = (root / "absent").string();
            options.replay_path = path.string();
            options.artifact_path = (root / "playback.json").string();
            EXPECT_NE(RunReplay(options), 0);
            EXPECT_FALSE(fs::exists(options.root));
            EXPECT_FALSE(fs::exists(options.artifact_path));
            fs::remove(path);
            (offset == 34 ? header.surface_radius : header.collision_radius) = radius;
            replay::ReplayWriter refused;
            EXPECT_FALSE(refused.Open(path.string(), header));
            EXPECT_FALSE(refused.Finalize(0));
            EXPECT_FALSE(fs::exists(path));
        }
    }
}

TEST_F(SurfaceWorkAdmission, RunnerAndRecordingRefuseBeforeSessionOrFiles) {
    ServerCliOptions options;
    options.root = (root / "absent").string();
    options.record_path = (root / "recording.lrec").string();
    options.artifact_path = (root / "artifact.json").string();
    for (const bool collision : {false, true}) {
        for (const int radius : {-1, 13, 10000, std::numeric_limits<int>::max()}) {
            options.surface_radius = collision ? 4 : radius;
            options.collision_radius = collision ? radius : 2;
            Luminumbra::Server::ServerWorldRunner runner(RunnerConfigFrom(options));
            EXPECT_FALSE(runner.Boot());
            EXPECT_EQ(runner.Session(), nullptr);
            EXPECT_EQ(runner.GetBootError(),
                      "Surface and collision radii must be between 0 and 12 chunks");
            EXPECT_NE(RunRecord(options), 0);
            EXPECT_FALSE(fs::exists(options.record_path));
            EXPECT_FALSE(fs::exists(options.artifact_path));
            EXPECT_FALSE(fs::exists(options.root));
        }
    }
}

TEST_F(SurfaceWorkAdmission, RefusedWriterReopenRetainsQueuedRecordingAndPath) {
    const auto original = root / "original.lrec";
    const auto refused = root / "refused.lrec";
    replay::ReplayHeader header;
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(original.string(), header));
    writer.RecordInput(1, {7, 9});
    header.surface_radius = 13;
    EXPECT_FALSE(writer.Open(refused.string(), header));
    ASSERT_TRUE(writer.Finalize(1));
    EXPECT_FALSE(fs::exists(refused));
    const auto contents = replay::ReadReplay(original.string());
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value())
        return;
    EXPECT_EQ(contents->tick_count, 1u);
    const auto* input = replay::FindInput(*contents, 1);
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->inputs, (std::vector<std::uint8_t>{7, 9}));
}

TEST(SurfaceWorkBudget, DirectSurfaceAndCollisionRefuseBeforeChunkMutation) {
    Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    Luminumbra::Systems::PhysicsSystem physics;
    const Luminumbra::Vec3 center(8.0f, 40.0f, 8.0f);
    EXPECT_TRUE(world.snapshot_streamed_chunks().empty());
    EXPECT_FALSE(world.EnsureSurfaceReadyNear(center, nullptr, 0, 0));
    for (const int radius : {13, 10000, 1073741824, std::numeric_limits<int>::max()}) {
        EXPECT_FALSE(world.EnsureSurfaceReadyNear(center, &physics, radius, 0));
        EXPECT_FALSE(world.EnsureSurfaceReadyNear(center, &physics, 0, radius));
        EXPECT_FALSE(world.EnsureCollisionReadyNear(center, &physics, radius));
        EXPECT_TRUE(world.snapshot_streamed_chunks().empty());
    }
    for (const float radius : {-1.0f,
                               0.0f,
                               161.0f,
                               std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()}) {
        EXPECT_EQ(world.EditTerrainVoxel(center, radius, false, &physics), 0);
        EXPECT_TRUE(world.snapshot_streamed_chunks().empty());
    }
}

TEST(SurfaceWorkBudget, TallSurfaceRefusesExactWorkBeforeCreatingChunks) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 10000000.0f;
    params.base_frequency = 0.1f;
    params.surface_sampling_revision = Luminumbra::Systems::kSurfaceSamplingRevision;
    Luminumbra::Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    Luminumbra::Systems::PhysicsSystem physics;
    const std::array<float, 5> xs{8.0f, 0.0f, 16.0f, 0.0f, 16.0f};
    const std::array<float, 5> zs{8.0f, 0.0f, 0.0f, 16.0f, 16.0f};
    std::array<float, 5> heights{};
    world.ComputeShapedHeightsAtPositions(xs.data(), zs.data(), xs.size(), heights.data());
    const auto [lowest, highest] = std::minmax_element(heights.begin(), heights.end());
    const auto low = Luminumbra::Systems::SHIELD_WorldSystem::world_to_chunk_coords(
                         Luminumbra::Vec3(8.0f, *lowest, 8.0f))
                         .y;
    const auto high = Luminumbra::Systems::SHIELD_WorldSystem::world_to_chunk_coords(
                          Luminumbra::Vec3(8.0f, *highest, 8.0f))
                          .y;
    ASSERT_GT(static_cast<std::int64_t>(high) - low + 3,
              static_cast<std::int64_t>(budget::kMaxSurfaceChunks));
    EXPECT_FALSE(world.EnsureSurfaceReadyNear(Luminumbra::Vec3(8.0f, 0.0f, 8.0f), &physics, 0, 0));
    EXPECT_TRUE(world.snapshot_streamed_chunks().empty());
    // A refused remesh must also leave an existing edited world untouched.
    const auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0));
    constexpr std::size_t samples = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                    (Luminumbra::CHUNK_SIZE_Y + 1) * (Luminumbra::CHUNK_SIZE_Z + 1);
    chunk->sdf_data.assign(samples, -1.0f);
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    const auto original_sdf = chunk->sdf_data;
    const auto original_revision = world.far_lod_authority_revision();
    EXPECT_EQ(world.EditTerrainVoxel(Luminumbra::Vec3(8.0f), 1.0f, false, &physics), 0);
    EXPECT_EQ(chunk->sdf_data, original_sdf);
    EXPECT_EQ(world.find_streamed_chunk(Luminumbra::IVec3(0)), chunk);
    EXPECT_EQ(world.far_lod_authority_revision(), original_revision);
}

TEST(SurfaceWorkBudget, TallSurfaceRefusalRetainsUnpublishedMeshBatch) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 10000000.0f;
    params.base_frequency = 0.1f;
    params.surface_sampling_revision = Luminumbra::Systems::kSurfaceSamplingRevision;
    Luminumbra::Systems::SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    Luminumbra::Systems::PhysicsSystem physics;
    entt::registry registry;
    const Luminumbra::Vec3 anchor(8.0f);
    const auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0));
    // One full lattice is sufficient to dispatch a real, tick-stamped mesh job.
    constexpr std::size_t samples = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                    (Luminumbra::CHUNK_SIZE_Y + 1) * (Luminumbra::CHUNK_SIZE_Z + 1);
    chunk->sdf_data.assign(samples, -1.0f);
    chunk->material_data.assign(samples, 0u);
    chunk->set_state(Luminumbra::ChunkState::Idle);
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    world.begin_tick(0);
    world.update(registry, anchor, nullptr);
    world.quiesce_streaming_jobs_for_save();
    ASSERT_EQ(world.pending_publication_chunks(), 1u);
    ASSERT_EQ(chunk->get_state(), Luminumbra::ChunkState::Meshing);
    ASSERT_EQ(chunk->pending_lod.load(), 0);
    ASSERT_TRUE(chunk->pending_mesh_ready.load());
    ASSERT_FALSE(chunk->pending_mesh_failed.load());
    const auto original_lod = chunk->current_lod.load();
    const auto original_version = chunk->mesh_version.load();
    const auto original_sdf = chunk->sdf_data;
    const auto original_revision = world.far_lod_authority_revision();

    EXPECT_FALSE(world.EnsureSurfaceReadyNear(anchor, &physics, 0, 0));
    EXPECT_FALSE(world.EnsureCollisionReadyNear(anchor, &physics, 0));
    EXPECT_EQ(world.EditTerrainVoxel(anchor, 1.0f, false, &physics), 0);
    EXPECT_EQ(world.pending_publication_chunks(), 1u);
    EXPECT_EQ(chunk->get_state(), Luminumbra::ChunkState::Meshing);
    EXPECT_EQ(chunk->pending_lod.load(), 0);
    EXPECT_EQ(chunk->current_lod.load(), original_lod);
    EXPECT_EQ(chunk->mesh_version.load(), original_version);
    EXPECT_EQ(chunk->sdf_data, original_sdf);
    EXPECT_EQ(world.find_streamed_chunk(Luminumbra::IVec3(0)), chunk);
    EXPECT_EQ(world.snapshot_streamed_chunks().size(), 1u);
    EXPECT_EQ(world.far_lod_authority_revision(), original_revision);

    // Refusal must leave the original batch available for its legal publication.
    world.begin_tick(8);
    world.activate_due(8);
    EXPECT_EQ(world.pending_publication_chunks(), 0u);
    EXPECT_EQ(chunk->get_state(), Luminumbra::ChunkState::Ready);
    EXPECT_EQ(chunk->pending_lod.load(), -1);
    EXPECT_EQ(chunk->current_lod.load(), 0);
    EXPECT_EQ(chunk->mesh_version.load(), original_version + 1u);
}

TEST(SurfaceWorkBudget, TallSurfaceRefusalRetainsUnpublishedGenerationBatch) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 10000000.0f;
    params.base_frequency = 0.1f;
    params.surface_sampling_revision = Luminumbra::Systems::kSurfaceSamplingRevision;
    Luminumbra::Systems::SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    Luminumbra::Systems::PhysicsSystem physics;
    const Luminumbra::Vec3 anchor(8.0f);
    world.begin_tick(0);
    world.dispatch_generation_jobs(std::vector<Luminumbra::IVec3>{Luminumbra::IVec3(0)});
    world.quiesce_streaming_jobs_for_save();
    const auto chunk = world.find_streamed_chunk(Luminumbra::IVec3(0));
    ASSERT_NE(chunk, nullptr);
    ASSERT_EQ(world.pending_publication_chunks(), 1u);
    ASSERT_EQ(chunk->get_state(), Luminumbra::ChunkState::Loading);
    ASSERT_TRUE(chunk->pending_generation_ready.load());
    ASSERT_FALSE(chunk->sdf_data.empty());
    const auto original_sdf = chunk->sdf_data;
    const auto original_lod = chunk->current_lod.load();
    const auto original_pending_lod = chunk->pending_lod.load();
    const auto original_version = chunk->mesh_version.load();
    const auto original_revision = world.far_lod_authority_revision();

    EXPECT_FALSE(world.EnsureSurfaceReadyNear(anchor, &physics, 0, 0));
    EXPECT_FALSE(world.EnsureCollisionReadyNear(anchor, &physics, 0));
    EXPECT_EQ(world.EditTerrainVoxel(anchor, 1.0f, false, &physics), 0);
    EXPECT_EQ(world.pending_publication_chunks(), 1u);
    EXPECT_EQ(chunk->get_state(), Luminumbra::ChunkState::Loading);
    EXPECT_TRUE(chunk->pending_generation_ready.load());
    EXPECT_EQ(chunk->current_lod.load(), original_lod);
    EXPECT_EQ(chunk->pending_lod.load(), original_pending_lod);
    EXPECT_EQ(chunk->mesh_version.load(), original_version);
    EXPECT_EQ(chunk->sdf_data, original_sdf);
    EXPECT_EQ(world.find_streamed_chunk(Luminumbra::IVec3(0)), chunk);
    EXPECT_EQ(world.snapshot_streamed_chunks().size(), 1u);
    EXPECT_EQ(world.far_lod_authority_revision(), original_revision);

    world.begin_tick(8);
    world.activate_due(8);
    EXPECT_EQ(world.pending_publication_chunks(), 0u);
    EXPECT_EQ(chunk->get_state(), Luminumbra::ChunkState::Idle);
    EXPECT_FALSE(chunk->pending_generation_ready.load());
    EXPECT_EQ(chunk->sdf_data, original_sdf);
    EXPECT_EQ(chunk->mesh_version.load(), original_version);
}

TEST(SurfaceWorkBudget, TallSurfaceRefusalRetainsUnpublishedPromotionBatch) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 10000000.0f;
    params.base_frequency = 0.1f;
    params.surface_sampling_revision = Luminumbra::Systems::kSurfaceSamplingRevision;
    Luminumbra::Systems::SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    Luminumbra::Systems::PhysicsSystem physics;
    entt::registry registry;
    const Luminumbra::Vec3 anchor(8.0f);
    const auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0));
    // One coarse column triggers stage A through the ordinary LOD0 scheduler.
    constexpr std::size_t samples =
        static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) * (Luminumbra::CHUNK_SIZE_Z + 1);
    chunk->heightmap_data.assign(samples, 0.0f);
    chunk->set_state(Luminumbra::ChunkState::Idle);
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    world.begin_tick(0);
    world.update(registry, anchor, nullptr);
    world.quiesce_streaming_jobs_for_save();
    ASSERT_EQ(world.pending_publication_chunks(), 1u);
    ASSERT_EQ(chunk->get_state(), Luminumbra::ChunkState::Meshing);
    ASSERT_EQ(chunk->pending_lod.load(), 0);
    ASSERT_TRUE(chunk->pending_promotion_ready.load());
    ASSERT_FALSE(chunk->pending_promotion_failed.load());
    ASSERT_TRUE(chunk->sdf_data.empty());
    ASSERT_FALSE(chunk->pending_sdf_data.empty());
    const auto original_sdf = chunk->sdf_data;
    const auto staged_sdf = chunk->pending_sdf_data;
    const auto original_heightmap = chunk->heightmap_data;
    const auto original_lod = chunk->current_lod.load();
    const auto original_version = chunk->mesh_version.load();
    const auto original_revision = world.far_lod_authority_revision();

    EXPECT_FALSE(world.EnsureSurfaceReadyNear(anchor, &physics, 0, 0));
    EXPECT_FALSE(world.EnsureCollisionReadyNear(anchor, &physics, 0));
    EXPECT_EQ(world.EditTerrainVoxel(anchor, 1.0f, false, &physics), 0);
    EXPECT_EQ(world.pending_publication_chunks(), 1u);
    EXPECT_EQ(chunk->get_state(), Luminumbra::ChunkState::Meshing);
    EXPECT_EQ(chunk->pending_lod.load(), 0);
    EXPECT_TRUE(chunk->pending_promotion_ready.load());
    EXPECT_EQ(chunk->current_lod.load(), original_lod);
    EXPECT_EQ(chunk->mesh_version.load(), original_version);
    EXPECT_EQ(chunk->sdf_data, original_sdf);
    EXPECT_EQ(chunk->pending_sdf_data, staged_sdf);
    EXPECT_EQ(chunk->heightmap_data, original_heightmap);
    EXPECT_EQ(world.find_streamed_chunk(Luminumbra::IVec3(0)), chunk);
    EXPECT_EQ(world.snapshot_streamed_chunks().size(), 1u);
    EXPECT_EQ(world.far_lod_authority_revision(), original_revision);

    // Completed stage-A data stays unpublished until the original due tick.
    world.begin_tick(7);
    world.activate_due(7);
    EXPECT_EQ(world.pending_publication_chunks(), 1u);
    EXPECT_EQ(chunk->get_state(), Luminumbra::ChunkState::Meshing);
    EXPECT_EQ(chunk->pending_lod.load(), 0);
    EXPECT_TRUE(chunk->pending_promotion_ready.load());
    EXPECT_FALSE(chunk->pending_promotion_failed.load());
    EXPECT_EQ(chunk->current_lod.load(), original_lod);
    EXPECT_EQ(chunk->mesh_version.load(), original_version);
    EXPECT_EQ(chunk->sdf_data, original_sdf);
    EXPECT_EQ(chunk->pending_sdf_data, staged_sdf);
    EXPECT_EQ(chunk->heightmap_data, original_heightmap);
    EXPECT_EQ(world.far_lod_authority_revision(), original_revision);

    // Stage B inherits stage A's due tick, so this activation publishes both.
    world.begin_tick(8);
    world.activate_due(8);
    EXPECT_EQ(chunk->sdf_data, staged_sdf);
    EXPECT_TRUE(chunk->pending_sdf_data.empty());
    EXPECT_FALSE(chunk->pending_promotion_ready.load());
    EXPECT_EQ(world.pending_publication_chunks(), 0u);
    EXPECT_EQ(chunk->get_state(), Luminumbra::ChunkState::Ready);
    EXPECT_EQ(chunk->pending_lod.load(), -1);
    EXPECT_EQ(chunk->current_lod.load(), 0);
    EXPECT_EQ(chunk->mesh_version.load(), original_version + 1u);

    // A later fence and activation cannot publish the same mesh twice.
    world.quiesce_streaming_jobs_for_save();
    world.begin_tick(16);
    world.activate_due(16);
    EXPECT_EQ(world.pending_publication_chunks(), 0u);
    EXPECT_EQ(chunk->get_state(), Luminumbra::ChunkState::Ready);
    EXPECT_EQ(chunk->pending_lod.load(), -1);
    EXPECT_EQ(chunk->current_lod.load(), 0);
    EXPECT_EQ(chunk->mesh_version.load(), original_version + 1u);
}

} // namespace
