#include "../fixtures/TerrainFixtureParams.h"
#include "../support/UniqueTempPath.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/world/FarWorldAuthorityProducer.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

namespace {
using namespace Luminumbra;
using Producer = World::FarWorldAuthorityProducer;
using Save = Persistence::WorldSaveService;

Producer::Limits Limits() {
    return {.index_records = 512, .copy_bytes = 1024u * 1024u, .query_columns = 64};
}
std::shared_ptr<Chunk> Edited(const IVec3& coords, float value = 1.0f) {
    auto chunk = std::make_shared<Chunk>(coords);
    constexpr std::size_t samples =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    chunk->sdf_data.assign(samples, value);
    chunk->mark_voxel_data_dirty();
    chunk->set_state(ChunkState::Idle);
    return chunk;
}
std::vector<Producer::Image> Copy(Producer& producer) {
    std::vector<Producer::Image> images;
    EXPECT_EQ(producer.copy_published({0, 0, 0, 0}, images), Producer::Error::None);
    return images;
}

TEST(FarWorldAuthorityProducer, FlagOffMembershipAndDirectNotificationsPreserveObservers) {
    Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    Producer producer(Limits());
    ASSERT_TRUE(world.set_far_world_authority_changes(producer.changes()));
    const std::array<Save::AuthorityWindow, 1> wanted{{{0, 0, 0, 0}}};
    ASSERT_TRUE(producer.set_wanted(wanted));
    unsigned voxel_calls = 0, water_calls = 0;
    world.SetVoxelEditObserver([&](const IVec3&) { ++voxel_calls; });
    world.SetWaterAuthorityObserver([&](const IVec3&) { ++water_calls; });
    auto chunk = Edited({0, 0, 0});
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    ASSERT_EQ(Copy(producer).size(), 1u);
    producer.tick();
    chunk->mark_voxel_data_dirty();
    auto dirty = producer.tick();
    EXPECT_TRUE(dirty.tiles.test(0));
    EXPECT_EQ(dirty.terrain, 1u);
    const auto voxel_revision = chunk->voxel_revision();
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        chunk->mark_water_state_dirty();
        world.sync_water_authority();
        dirty = producer.tick();
        EXPECT_TRUE(dirty.tiles.test(0));
        EXPECT_GE(dirty.water, 1u);
        EXPECT_EQ(chunk->voxel_revision(), voxel_revision);
    }
    EXPECT_EQ(water_calls, 3u);
    EXPECT_EQ(voxel_calls, 0u);
    EXPECT_FALSE(world.has_region_members(0, 0));
}

TEST(FarWorldAuthorityProducer, LoadingAndPendingBuffersCannotReplacePublishedAuthority) {
    Producer producer(Limits());
    auto parked = Edited({0, 0, 0}, 2.0f);
    auto live = Edited({0, 0, 0}, 3.0f);
    producer.changes()->membership(parked, true, true);
    live->set_state(ChunkState::Loading);
    live->pending_sdf_data = live->sdf_data;
    live->pending_generation_ready.store(true);
    producer.changes()->membership(live, false, true);
    auto images = Copy(producer);
    ASSERT_EQ(images.size(), 1u);
    EXPECT_TRUE(images[0].parked);
    EXPECT_EQ(images[0].sdf[0], 2.0f);
    const std::array<Save::AuthorityWindow, 1> wanted{{{0, 0, 0, 0}}};
    ASSERT_TRUE(producer.set_wanted(wanted));
    producer.tick();
    live->set_state(ChunkState::Idle);
    producer.changes()->membership(live, false, true);
    images = Copy(producer);
    ASSERT_EQ(images.size(), 1u);
    EXPECT_FALSE(images[0].parked);
    EXPECT_EQ(images[0].sdf[0], 3.0f);
    live->sdf_data[0] = 9.0f;
    EXPECT_EQ(images[0].sdf[0], 3.0f);
    producer.changes()->membership(live, false, false);
    EXPECT_TRUE(Copy(producer)[0].parked);
    parked->set_state(ChunkState::Loading);
    EXPECT_TRUE(Copy(producer).empty());
}

TEST(FarWorldAuthorityProducer, GeneratedLiveDataNeverOverridesParkedEditsOrPristine) {
    Producer producer(Limits());
    auto live = Edited({0, 0, 0});
    live->mark_sdf_generated_current_params();
    producer.changes()->membership(live, false, true);
    EXPECT_TRUE(Copy(producer).empty());
    auto parked = Edited({0, 0, 0}, 4.0f);
    producer.changes()->membership(parked, true, true);
    ASSERT_EQ(Copy(producer).size(), 1u);
    EXPECT_EQ(Copy(producer)[0].sdf[0], 4.0f);
    producer.changes()->membership(parked, true, false);
    EXPECT_TRUE(Copy(producer).empty());
}

TEST(FarWorldAuthorityProducer, DirtyOverflowCoalescesAllWantedAndStaysBounded) {
    Producer producer(Limits());
    std::array<Save::AuthorityWindow, 512> wanted{};
    ASSERT_TRUE(producer.set_wanted(wanted));
    for (std::size_t change = 0; change < 513; ++change) {
        producer.changes()->notify({0, 0, 0}, World::FarWorldAuthorityChange::Water);
    }
    auto dirty = producer.tick();
    EXPECT_TRUE(dirty.overflow);
    EXPECT_EQ(dirty.tiles.count(), 512u);
    EXPECT_EQ(dirty.visited, 512u);
    EXPECT_EQ(producer.counters().dirty_overflows, 1u);
    dirty = producer.tick();
    EXPECT_TRUE(dirty.tiles.none());
    EXPECT_EQ(dirty.visited, 0u);
    std::array<Save::AuthorityWindow, 513> excessive{};
    EXPECT_FALSE(producer.set_wanted(excessive));
    producer.changes()->notify({0, 0, 0}, World::FarWorldAuthorityChange::Terrain);
    EXPECT_EQ(producer.tick().tiles.count(), 512u);
}

TEST(FarWorldAuthorityProducer, TickVisitsOnlyChangesAndCopyVisitsOnlyRequestedColumn) {
    Producer producer(Limits());
    const std::array<Save::AuthorityWindow, 1> wanted{{{0, 0, 0, 0}}};
    ASSERT_TRUE(producer.set_wanted(wanted));
    for (int x = 0; x < 200; ++x) {
        producer.changes()->membership(Edited({x, 0, 0}), false, true);
    }
    producer.tick();
    const auto before = producer.counters();
    producer.changes()->notify({100, 0, 0}, World::FarWorldAuthorityChange::Water);
    EXPECT_EQ(producer.tick().visited, 0u);
    producer.changes()->notify({0, 0, 0}, World::FarWorldAuthorityChange::Water);
    EXPECT_EQ(producer.tick().visited, 1u);
    ASSERT_EQ(Copy(producer).size(), 1u);
    const auto after = producer.counters();
    EXPECT_EQ(after.tick_items - before.tick_items, 1u);
    EXPECT_EQ(after.query_items - before.query_items, 1u);
    EXPECT_EQ(after.copied_records - before.copied_records, 1u);
    EXPECT_GT(after.copied_bytes, before.copied_bytes);
    EXPECT_EQ(producer.tick().visited, 0u);
}

TEST(FarWorldAuthorityProducer, IndexAndCopyRefusalsPreserveOutput) {
    auto limits = Limits();
    limits.index_records = 1;
    Producer producer(limits);
    producer.changes()->membership(Edited({0, 0, 0}), false, true);
    auto images = Copy(producer);
    ASSERT_EQ(images.size(), 1u);
    producer.changes()->membership(Edited({1, 0, 0}), false, true);
    EXPECT_EQ(producer.copy_published({0, 0, 0, 0}, images), Producer::Error::IndexCapacity);
    EXPECT_EQ(images.size(), 1u);
    Producer small({.index_records = 1, .copy_bytes = 1, .query_columns = 1});
    small.changes()->membership(Edited({0, 0, 0}), false, true);
    EXPECT_EQ(small.copy_published({0, 0, 0, 0}, images), Producer::Error::CopyLimit);
    EXPECT_EQ(images.size(), 1u);
    EXPECT_EQ(small.copy_published({1, 0, 0, 0}, images), Producer::Error::InvalidWindow);
    EXPECT_EQ(small.copy_published({0, 1, 0, 0}, images), Producer::Error::InvalidWindow);
    Producer invalid({});
    EXPECT_EQ(invalid.copy_published({0, 0, 0, 0}, images), Producer::Error::InvalidLimits);
}

TEST(FarWorldAuthorityProducer, PartialAndAmbiguousWaterRefuseWhileDryIsOwned) {
    Producer producer(Limits());
    auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
    chunk->set_state(ChunkState::Idle);
    chunk->set_water_authority();
    producer.changes()->membership(chunk, false, true);
    std::vector<Producer::Image> images;
    EXPECT_EQ(producer.copy_published({0, 0, 0, 0}, images), Producer::Error::WaterUnavailable);
    chunk->current_water_resolution.store(4);
    chunk->water_depth_mm.assign(16, 0);
    chunk->water_bed_mm.assign(16, 1000);
    ASSERT_EQ(Copy(producer).size(), 1u);
    EXPECT_TRUE(Copy(producer)[0].water_authority);
    chunk->water_depth_mm[0] = -1;
    EXPECT_EQ(producer.copy_published({0, 0, 0, 0}, images), Producer::Error::WaterUnavailable);
    chunk->water_depth_mm[0] = 0;
    chunk->current_water_resolution.store(3);
    EXPECT_EQ(producer.copy_published({0, 0, 0, 0}, images), Producer::Error::WaterUnavailable);
    chunk->current_water_resolution.store(4);
    chunk->water_bed_mm.pop_back();
    EXPECT_EQ(producer.copy_published({0, 0, 0, 0}, images), Producer::Error::WaterUnavailable);
    chunk->water_bed_mm.push_back(1000);
    auto other = Edited({0, 1, 0});
    other->set_water_authority();
    other->current_water_resolution.store(4);
    other->water_depth_mm.assign(16, 1);
    other->water_bed_mm.assign(16, 1000);
    producer.changes()->membership(other, false, true);
    EXPECT_EQ(producer.copy_published({0, 0, 0, 0}, images), Producer::Error::WaterUnavailable);
}

TEST(FarWorldAuthorityProducer, IndependentLayersKeepParkedWaterWithLiveTerrain) {
    Producer producer(Limits());
    auto live = Edited({0, 0, 0}, 8.0f);
    auto parked = Edited({0, 0, 0}, 2.0f);
    parked->set_water_authority();
    parked->current_water_resolution.store(4);
    parked->water_bed_mm.assign(16, 0);
    parked->water_depth_mm.assign(16, 3);
    producer.changes()->membership(parked, true, true);
    producer.changes()->membership(live, false, true);
    auto images = Copy(producer);
    ASSERT_EQ(images.size(), 2u);
    EXPECT_TRUE(images[0].terrain_authority);
    EXPECT_FALSE(images[0].water_authority);
    EXPECT_EQ(images[0].sdf[0], 8.0f);
    EXPECT_TRUE(images[1].water_authority);
    EXPECT_TRUE(images[1].parked);
    EXPECT_EQ(images[1].depth[0], 3);
    live->set_water_authority();
    live->current_water_resolution.store(4);
    live->water_bed_mm.assign(16, 1);
    live->water_depth_mm.assign(16, 4);
    images = Copy(producer);
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(images[0].depth[0], 4);
}

TEST(FarWorldAuthorityProducer, MalformedPublishedLayersRefuseWithoutReplacingImages) {
    Producer producer(Limits());
    auto chunk = Edited({0, 0, 0});
    producer.changes()->membership(chunk, false, true);
    auto images = Copy(producer);
    chunk->material_data.assign(1, 1);
    EXPECT_EQ(producer.copy_published({0, 0, 0, 0}, images), Producer::Error::AuthorityUnavailable);
    chunk->material_data.clear();
    chunk->sdf_data[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(producer.copy_published({0, 0, 0, 0}, images), Producer::Error::AuthorityUnavailable);
    chunk->sdf_data.clear();
    EXPECT_EQ(producer.copy_published({0, 0, 0, 0}, images), Producer::Error::AuthorityUnavailable);
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(images[0].sdf[0], 1.0f);
    chunk->set_state(ChunkState::Unloading);
    EXPECT_TRUE(Copy(producer).empty());
    chunk->set_state(ChunkState::Unloaded);
    EXPECT_TRUE(Copy(producer).empty());
}

TEST(FarWorldAuthorityProducer, AttachmentIsBoundedAndClearDetachesOldChunks) {
    Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    auto first = Edited({0, 0, 0});
    ASSERT_TRUE(world.adopt_streamed_chunk(first));
    ASSERT_TRUE(world.adopt_streamed_chunk(Edited({1, 0, 0})));
    Producer producer(Limits());
    ASSERT_TRUE(world.set_far_world_authority_changes(producer.changes()));
    const std::array<Save::AuthorityWindow, 1> wanted{{{0, 0, 0, 0}}};
    ASSERT_TRUE(producer.set_wanted(wanted));
    Producer small({.index_records = 1, .copy_bytes = 1024u * 1024u, .query_columns = 1});
    EXPECT_FALSE(world.set_far_world_authority_changes(small.changes()));
    ASSERT_EQ(Copy(producer).size(), 1u);
    world.clear_world(nullptr);
    EXPECT_TRUE(Copy(producer).empty());
    producer.tick();
    first->mark_voxel_data_dirty();
    EXPECT_EQ(producer.tick().visited, 0u);
}

TEST(FarWorldAuthorityProducer, MembershipReplacementAndParkingKeepBindings) {
    Producer producer(Limits());
    const std::array<Save::AuthorityWindow, 1> wanted{{{0, 0, 0, 0}}};
    ASSERT_TRUE(producer.set_wanted(wanted));
    auto first = Edited({0, 0, 0}, 1.0f);
    auto replacement = Edited({0, 0, 0}, 2.0f);
    producer.changes()->membership(first, false, true);
    producer.changes()->membership(first, true, true);
    producer.changes()->membership(replacement, false, true);
    producer.tick();
    first->mark_water_state_dirty();
    EXPECT_GT(producer.tick().visited, 0u);
    producer.changes()->membership(first, false, false);
    EXPECT_EQ(Copy(producer)[0].sdf[0], 2.0f);
    producer.changes()->membership(replacement, false, false);
    EXPECT_TRUE(Copy(producer)[0].parked);
    producer.changes()->membership(first, false, true);
    producer.changes()->membership(first, true, false);
    producer.tick();
    first->mark_voxel_data_dirty();
    EXPECT_EQ(producer.tick().terrain, 1u);
    EXPECT_FALSE(Copy(producer)[0].parked);
}

TEST(FarWorldAuthorityProducer, OwnsLegacySourceAndFailedReadCannotBecomeAbsence) {
    const auto root = TestSupport::CreateUniqueTempDirectory("authority_");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};
    WorldStreamingState state;
    ASSERT_TRUE(state.insert_chunk(Edited({0, 0, 0}, 7.0f)));
    ASSERT_TRUE(Save{}.save_world(state, root));
    Producer producer(Limits());
    const Save::AuthorityReadLimits limits{
        64, 128, 256, 256, 4194304, 16777216, 4194304, 16777216, 131072};
    std::string error;
    ASSERT_TRUE(producer.admit_saved_source(root, limits, error)) << error;
    auto result = producer.read_committed({0, 0, 0, 0}, limits);
    ASSERT_EQ(result.membership, Save::AuthorityMembership::Present);
    ASSERT_EQ(result.chunks.size(), 1u);
    EXPECT_EQ(result.generation, 0u);
    EXPECT_EQ(result.chunks[0]->sdf_data[0], 7.0f);
    EXPECT_FALSE(producer.admit_saved_source(root / "missing", limits, error));
    EXPECT_EQ(producer.read_committed({0, 0, 0, 0}, limits).membership,
              Save::AuthorityMembership::Present);
    std::filesystem::remove_all(root);
    EXPECT_EQ(producer.read_committed({0, 0, 0, 0}, limits).membership,
              Save::AuthorityMembership::Unavailable);
}
} // namespace

namespace {
class FarWorldSourceCaptureTest : public testing::Test {
protected:
    std::filesystem::path root = TestSupport::CreateUniqueTempDirectory("capture_");
    Producer producer{Limits()};
    Save::AuthorityReadLimits reads{
        64, 128, 256, 256, 4194304, 16777216, 4194304, 16777216, 131072};
    void SetUp() override {
        WorldStreamingState empty;
        ASSERT_TRUE(Save{}.save_world(empty, root));
        std::string error;
        ASSERT_TRUE(producer.admit_saved_source(root, reads, error)) << error;
        ASSERT_TRUE(producer.configure_capture({reads, 3}));
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void Request() {
        const std::array<Save::AuthorityWindow, 2> windows{{{0, 0, 0, 0}, {1, 1, 0, 0}}};
        ASSERT_TRUE(producer.request_capture(windows));
    }
};

TEST_F(FarWorldSourceCaptureTest, CopiesAllWindowsAtOneBoundaryAndCoalescesSuccessorEdits) {
    auto first = Edited({0, 0, 0}, 1.0f);
    auto second = Edited({1, 0, 0}, 2.0f);
    first->set_water_authority();
    first->current_water_resolution.store(4);
    first->water_bed_mm.assign(16, 0);
    first->water_depth_mm.assign(16, 3);
    producer.changes()->membership(first, false, true);
    producer.changes()->membership(second, true, true);
    Request();
    producer.pump_capture();
    EXPECT_EQ(producer.counters().copied_records, 2u);
    EXPECT_FALSE(producer.configure_capture({reads, 3}));
    first->sdf_data[0] = 3.0f;
    second->sdf_data[0] = 4.0f;
    first->water_depth_mm[0] = 8;
    first->mark_water_state_dirty();
    first->mark_voxel_data_dirty();
    second->mark_voxel_data_dirty();
    producer.pump_capture();
    ASSERT_NE(producer.capture(), nullptr);
    ASSERT_EQ(producer.capture()->published.size(), 2u);
    EXPECT_EQ(producer.capture()->published[0].sdf[0], 1.0f);
    EXPECT_EQ(producer.capture()->published[1].sdf[0], 2.0f);
    EXPECT_EQ(producer.capture()->published[0].depth[0], 3);
    EXPECT_TRUE(producer.capture_status().queued);
    EXPECT_FALSE(producer.capture_status().latest_ready);
    producer.pump_capture();
    producer.pump_capture();
    ASSERT_NE(producer.capture(), nullptr);
    EXPECT_EQ(producer.capture()->published[0].sdf[0], 3.0f);
    EXPECT_EQ(producer.capture()->published[1].sdf[0], 4.0f);
    EXPECT_EQ(producer.capture()->published[0].depth[0], 8);
    EXPECT_TRUE(producer.capture_status().latest_ready);
    EXPECT_EQ(producer.counters().copied_records, 4u);
    const auto before = producer.counters();
    producer.pump_capture();
    EXPECT_EQ(producer.counters().copied_records, before.copied_records);
    EXPECT_EQ(producer.counters().query_items, before.query_items);
}

TEST_F(FarWorldSourceCaptureTest, WantedReplacementRetainsActiveWindowAndOnlyLatestSuccessor) {
    for (int x = 0; x < 3; ++x) {
        producer.changes()->membership(Edited({x, 0, 0}, static_cast<float>(x)), false, true);
    }
    const std::array<Save::AuthorityWindow, 1> first{{{0, 0, 0, 0}}};
    const std::array<Save::AuthorityWindow, 1> second{{{1, 1, 0, 0}}};
    const std::array<Save::AuthorityWindow, 1> latest{{{2, 2, 0, 0}}};
    ASSERT_TRUE(producer.request_capture(first));
    producer.pump_capture();
    ASSERT_TRUE(producer.request_capture(second));
    ASSERT_TRUE(producer.request_capture(latest));
    producer.pump_capture();
    ASSERT_NE(producer.capture(), nullptr);
    EXPECT_EQ(producer.capture()->window.min_chunk_x, 0);
    EXPECT_EQ(producer.capture()->published[0].sdf[0], 0.0f);
    producer.pump_capture();
    producer.pump_capture();
    ASSERT_NE(producer.capture(), nullptr);
    EXPECT_EQ(producer.capture()->window.min_chunk_x, 2);
    EXPECT_EQ(producer.capture()->published[0].sdf[0], 2.0f);
    EXPECT_EQ(producer.counters().copied_records, 2u);
    EXPECT_TRUE(producer.capture_status().latest_ready);
}

TEST_F(FarWorldSourceCaptureTest, ContinuousEditsFinishActiveCaptureEveryTwoPumps) {
    auto chunk = Edited({0, 0, 0});
    producer.changes()->membership(chunk, false, true);
    Request();
    for (unsigned frame = 0; frame < 12; ++frame) {
        chunk->sdf_data[0] = static_cast<float>(frame);
        chunk->mark_voxel_data_dirty();
        producer.pump_capture();
        if (frame % 2u == 1u) {
            ASSERT_NE(producer.capture(), nullptr);
            EXPECT_EQ(producer.capture()->serial, frame / 2u + 1u);
            EXPECT_EQ(producer.capture()->published[0].sdf[0], static_cast<float>(frame - 1u));
        }
    }
    EXPECT_LE(producer.counters().copied_records, 6u);
}

TEST_F(FarWorldSourceCaptureTest, DeadlineRefusesAndPreservesServingCapture) {
    Request();
    producer.pump_capture();
    producer.pump_capture();
    const auto* serving = producer.capture();
    ASSERT_NE(serving, nullptr);
    ASSERT_TRUE(producer.configure_capture({reads, 1}));
    Request();
    producer.pump_capture();
    producer.pump_capture();
    EXPECT_EQ(producer.capture_status().error, Producer::Error::DeadlineExceeded);
    EXPECT_EQ(producer.capture(), serving);
    EXPECT_FALSE(producer.capture_status().latest_ready);
    EXPECT_FALSE(producer.capture_status().active);
}

TEST_F(FarWorldSourceCaptureTest, ReaderFenceRetainsStateAndDeadlineIncludesPausedFrames) {
    Request();
    producer.fence_source_reads(true);
    std::string error;
    EXPECT_FALSE(producer.admit_saved_source(root, reads, error));
    EXPECT_FALSE(producer.refresh_saved_source(root, error));
    EXPECT_EQ(producer.read_committed({}, reads).membership,
              Save::AuthorityMembership::Unavailable);
    producer.pump_capture();
    EXPECT_FALSE(producer.capture_status().active);
    producer.fence_source_reads(false);
    producer.pump_capture();
    producer.fence_source_reads(true);
    producer.pump_capture();
    EXPECT_EQ(producer.capture(), nullptr);
    producer.fence_source_reads(false);
    producer.pump_capture();
    ASSERT_NE(producer.capture(), nullptr);
    Request();
    producer.pump_capture();
    producer.fence_source_reads(true);
    for (unsigned frame = 0; frame < 3; ++frame) {
        producer.pump_capture();
    }
    EXPECT_EQ(producer.capture_status().error, Producer::Error::DeadlineExceeded);
    EXPECT_FALSE(producer.capture_status().latest_ready);
}

TEST_F(FarWorldSourceCaptureTest, SourceReplacementCannotMixCapturedVolatileAndCommittedData) {
    auto chunk = Edited({0, 0, 0}, 5.0f);
    producer.changes()->membership(chunk, false, true);
    Request();
    producer.pump_capture();
    producer.invalidate_source();
    EXPECT_EQ(producer.read_committed({}, reads).membership,
              Save::AuthorityMembership::Unavailable);
    WorldStreamingState replacement;
    ASSERT_TRUE(replacement.insert_chunk(Edited({1, 0, 0}, 7.0f)));
    ASSERT_TRUE(Save{}.save_world(replacement, root));
    std::string error;
    ASSERT_TRUE(producer.admit_saved_source(root, reads, error));
    chunk->sdf_data[0] = 9.0f;
    producer.pump_capture();
    EXPECT_EQ(producer.capture(), nullptr);
    producer.pump_capture();
    ASSERT_NE(producer.capture(), nullptr);
    EXPECT_EQ(producer.capture()->published[0].sdf[0], 9.0f);
    ASSERT_EQ(producer.capture()->committed.chunks.size(), 1u);
    EXPECT_EQ(producer.capture()->committed.chunks[0]->sdf_data[0], 7.0f);
    EXPECT_EQ(producer.capture()->committed.generation, 0u);
}

TEST_F(FarWorldSourceCaptureTest, ChangedCommittedGenerationRefusesUntilFreshBoundary) {
    WorldStreamingState disk;
    auto chunk = Edited({0, 0, 0}, 1.0f);
    ASSERT_TRUE(disk.insert_chunk(chunk));
    const auto commit = [&]() {
        Persistence::SaveTransaction transaction(root, 0, {}, Save::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(Save{}.save_world(disk, root));
        ASSERT_TRUE(Save::finalize_indexed_envelope(root, nullptr));
        ASSERT_TRUE(transaction.commit(nullptr));
    };
    ASSERT_NO_FATAL_FAILURE(commit());
    std::string error;
    ASSERT_TRUE(producer.admit_saved_source(root, reads, error)) << error;
    Request();
    producer.pump_capture();
    producer.pump_capture();
    ASSERT_NE(producer.capture(), nullptr);
    const auto generation = producer.capture()->committed.generation;
    EXPECT_GT(generation, 0u);
    const auto* accepted = producer.capture();
    Request();
    producer.pump_capture();
    chunk->sdf_data[0] = 3.0f;
    chunk->mark_voxel_data_dirty();
    ASSERT_NO_FATAL_FAILURE(commit());
    producer.pump_capture();
    EXPECT_EQ(producer.capture_status().error, Producer::Error::CommittedReadRefused);
    EXPECT_FALSE(producer.capture_read_error().empty());
    EXPECT_EQ(producer.capture(), accepted);
    EXPECT_EQ(producer.capture()->committed.generation, generation);
    ASSERT_TRUE(producer.refresh_saved_source(root, error)) << error;
    producer.pump_capture();
    producer.pump_capture();
    ASSERT_NE(producer.capture(), nullptr);
    EXPECT_GT(producer.capture()->committed.generation, generation);
    EXPECT_EQ(producer.capture()->committed.chunks[0]->sdf_data[0], 3.0f);
}

TEST_F(FarWorldSourceCaptureTest, InvalidRequestsAndMissingSourcesNeverReplaceAcceptedCapture) {
    Request();
    producer.pump_capture();
    producer.pump_capture();
    const auto* serving = producer.capture();
    ASSERT_NE(serving, nullptr);
    EXPECT_FALSE(producer.request_capture({}));
    Producer cold(Limits());
    const std::array<Save::AuthorityWindow, 1> single{{{0, 0, 0, 0}}};
    EXPECT_FALSE(cold.request_capture(single));
    ASSERT_TRUE(cold.configure_capture({reads, 3}));
    EXPECT_FALSE(cold.request_capture(single));
    const std::array<Save::AuthorityWindow, 2> wide{{{0, 0, 0, 0}, {100, 100, 0, 0}}};
    EXPECT_FALSE(producer.request_capture(wide));
    auto invalid = reads;
    invalid.decoded_total_bytes = 0;
    EXPECT_FALSE(producer.configure_capture({invalid, 3}));
    EXPECT_FALSE(producer.configure_capture({reads, 0}));
    Request();
    producer.pump_capture();
    std::filesystem::remove_all(root);
    producer.pump_capture();
    EXPECT_EQ(producer.capture_status().error, Producer::Error::CommittedReadRefused);
    EXPECT_FALSE(producer.capture_read_error().empty());
    EXPECT_EQ(producer.capture(), serving);
    EXPECT_FALSE(producer.capture_status().latest_ready);
}

TEST_F(FarWorldSourceCaptureTest, MalformedVolatileCaptureRefusesBeforeReadingAndCanRetry) {
    auto chunk = Edited({0, 0, 0});
    producer.changes()->membership(chunk, false, true);
    chunk->sdf_data.clear();
    Request();
    producer.pump_capture();
    EXPECT_EQ(producer.capture_status().error, Producer::Error::AuthorityUnavailable);
    EXPECT_EQ(producer.capture(), nullptr);
    EXPECT_FALSE(producer.capture_status().active);
    chunk->sdf_data = Edited({0, 0, 0})->sdf_data;
    chunk->mark_voxel_data_dirty();
    producer.pump_capture();
    producer.pump_capture();
    ASSERT_NE(producer.capture(), nullptr);
}

class FarWorldFrozenComposeTest : public testing::Test {
protected:
    std::filesystem::path root = TestSupport::CreateUniqueTempDirectory("frozen_compose_");
    Producer producer{{512, 4u * 1024u * 1024u, 8192}};
    Save::AuthorityReadLimits reads{
        64, 512, 1024, 8192, 4194304, 16777216, 4194304, 16777216, 131072};
    World::FarFrozenCaptureLimits limits{1, 100'000, 2'000'000, 200'000, 32u * 1024u * 1024u};
    std::shared_ptr<World::FarVolumeMemoryPool> pool =
        std::make_shared<World::FarVolumeMemoryPool>(64u * 1024u * 1024u);
    std::unique_ptr<Systems::SHIELD_WorldSystem> world;
    Producer::FrozenPolicy policy;
    void SetUp() override {
        auto params = Luminumbra::Test::SyntheticTerrainParams();
        params.caves_enabled = false;
        params.base_amplitude = params.peaks_amplitude = 0;
        params.continental_spline = {{-1, 0}, {1, 0}};
        world = std::make_unique<Systems::SHIELD_WorldSystem>(nullptr, nullptr, params, 1337);
        policy = {World::WorldFarVolumeIdentity(*world,
                                                World::FarCaveMode::BandLimited,
                                                World::FarVolumeNumericProfile::RawFloat,
                                                4),
                  World::FarCaveMode::BandLimited,
                  World::FarVolumeNumericProfile::RawFloat,
                  4};
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void Capture(const WorldStreamingState& disk) {
        ASSERT_TRUE(Save{}.save_world(disk, root));
        std::string error;
        ASSERT_TRUE(producer.admit_saved_source(root, reads, error)) << error;
        ASSERT_TRUE(producer.configure_capture({reads, 4}));
        const std::array<Save::AuthorityWindow, 1> windows{{{-34, 66, -34, 34}}};
        ASSERT_TRUE(producer.request_capture(windows));
        producer.pump_capture();
        producer.pump_capture();
        ASSERT_NE(producer.capture(), nullptr);
    }
    World::FarVolumeRequest Request(std::int64_t x = 0) const {
        return {{1, x, 0}, {-16, 16}, policy.pristine_identity};
    }
    Producer::FrozenResult Compose(std::shared_ptr<const World::FarFrozenObservation>& output,
                                   std::int64_t x = 0) {
        return producer.compose_frozen(*world, Request(x), policy, limits, pool, output);
    }
};

TEST_F(FarWorldFrozenComposeTest, ClimateFrequenciesInvalidateOnlyEnabledBiomePolicies) {
    auto dependencies = std::make_shared<World::WorldgenDependencySnapshot>();
    dependencies->biome_table = World::BiomeTable::Load(
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "data/common/biomes.json");
    ASSERT_TRUE(dependencies->biome_table.ok());
    ASSERT_FALSE(dependencies->biome_table.empty());
    WorldStreamingState disk;
    ASSERT_NO_FATAL_FAILURE(Capture(disk));
    for (const bool enabled : {false, true}) {
        auto params = world->get_params();
        params.biomes_enabled = enabled;
        params.worldgen_dependencies = dependencies;
        world->set_params(params);
        policy.pristine_identity =
            World::WorldFarVolumeIdentity(*world, policy.caves, policy.numeric, 4);
        ASSERT_NE(policy.pristine_identity, 0u);
        std::shared_ptr<const World::FarFrozenObservation> image;
        ASSERT_TRUE(Compose(image).ok());
        const auto accepted = image;
        for (const bool temperature : {true, false}) {
            auto changed = params;
            if (temperature) {
                changed.temperature_frequency *= 2;
            } else {
                changed.humidity_frequency *= 2;
            }
            world->set_params(changed);
            const auto identity =
                World::WorldFarVolumeIdentity(*world, policy.caves, policy.numeric, 4);
            const auto result = Compose(image);
            if (enabled) {
                EXPECT_NE(identity, policy.pristine_identity);
                EXPECT_EQ(result.capture, World::FarFrozenCaptureError::IdentityMismatch);
                EXPECT_EQ(image, accepted);
            } else {
                EXPECT_EQ(identity, policy.pristine_identity);
                EXPECT_TRUE(result.ok());
                ASSERT_TRUE(image);
                EXPECT_EQ(image->content_identity(), accepted->content_identity());
            }
            world->set_params(params);
        }
    }
}

TEST_F(FarWorldFrozenComposeTest, OwnedSessionLayersComposeTerrainWaterAndDryWithoutRereading) {
    WorldStreamingState disk;
    ASSERT_TRUE(disk.insert_chunk(Edited({0, 0, 0}, 3)));
    ASSERT_TRUE(disk.insert_chunk(Edited({2, 0, 0}, 6)));
    auto live = Edited({0, 0, 0}, 9);
    auto parked = Edited({1, 0, 0}, 8);
    live->set_water_authority();
    live->current_water_resolution.store(4);
    live->water_bed_mm.assign(16, 1000);
    live->water_depth_mm.assign(16, 2000);
    producer.changes()->membership(live, false, true);
    producer.changes()->membership(parked, true, true);
    ASSERT_NO_FATAL_FAILURE(Capture(disk));
    live->sdf_data.assign(live->sdf_data.size(), 20);
    live->water_depth_mm.assign(16, 7000);
    parked->sdf_data.assign(parked->sdf_data.size(), 21);
    std::shared_ptr<const World::FarFrozenObservation> image;
    const auto result = Compose(image);
    ASSERT_TRUE(result.ok()) << static_cast<int>(result.capture);
    ASSERT_TRUE(image);
    EXPECT_EQ(image->use(), World::FarFrozenUse::SessionRender);
    EXPECT_EQ(image->durable_identity().generation, producer.capture()->committed.generation);
    EXPECT_EQ(image->Sample(1, {4, 4, 4}), (std::optional<World::FarVolumeSample>{{9, 0}}));
    EXPECT_EQ(image->Sample(1, {20, 4, 4}), (std::optional<World::FarVolumeSample>{{8, 0}}));
    EXPECT_EQ(image->Sample(1, {36, 4, 4}), (std::optional<World::FarVolumeSample>{{6, 0}}));
    EXPECT_EQ(image->WaterSample(0, 4, 4), (std::optional<World::FarFrozenWaterSample>{{true, 3}}));
    EXPECT_EQ(image->WaterSample(0, 100, 100),
              (std::optional<World::FarFrozenWaterSample>{{false, 0}}));
    const auto fingerprint = image->content_identity();
    producer.invalidate_source();
    EXPECT_EQ(image->content_identity(), fingerprint);
    EXPECT_EQ(image->Sample(1, {4, 4, 4}), (std::optional<World::FarVolumeSample>{{9, 0}}));
}

TEST_F(FarWorldFrozenComposeTest, NegativeOffLatticeSharedConflictRefusesWithoutReplacingImage) {
    WorldStreamingState disk;
    ASSERT_NO_FATAL_FAILURE(Capture(disk));
    std::shared_ptr<const World::FarFrozenObservation> image;
    ASSERT_TRUE(Compose(image, -1).ok());
    producer.changes()->membership(Edited({-2, 0, 0}, 1), false, true);
    auto neighbour = Edited({-1, 0, 0}, 1);
    producer.changes()->membership(neighbour, false, true);
    producer.pump_capture();
    producer.pump_capture();
    ASSERT_TRUE(Compose(image, -1).ok());
    const auto accepted = image;
    // The requested four-metre grid never visits this shared node.
    neighbour->sdf_data[(CHUNK_SIZE_X + 1) * (1 + CHUNK_SIZE_Y + 1)] = 2;
    neighbour->mark_voxel_data_dirty();
    producer.pump_capture();
    producer.pump_capture();
    const auto bytes = pool->used();
    const auto result = Compose(image, -1);
    EXPECT_EQ(result.source, Producer::Error::AuthorityUnavailable);
    EXPECT_EQ(image, accepted);
    EXPECT_EQ(pool->used(), bytes);
}

TEST_F(FarWorldFrozenComposeTest, IntegerWaterLayersUseDryOverridesHalfOpenEdgesAndBaseProjection) {
    const auto water_chunk = [](const IVec3& coords, std::int32_t bed, std::int32_t depth) {
        auto chunk = Edited(coords);
        chunk->set_water_authority();
        chunk->current_water_resolution.store(4);
        chunk->water_bed_mm.assign(16, bed);
        chunk->water_depth_mm.assign(16, depth);
        return chunk;
    };
    WorldStreamingState disk;
    ASSERT_TRUE(disk.insert_chunk(water_chunk({0, 0, 0}, 5000, 1000)));
    auto committed = water_chunk({2, 0, 0}, 6000, 1000);
    committed->water_depth_mm[0] = 0;
    ASSERT_TRUE(disk.insert_chunk(committed));
    producer.changes()->membership(water_chunk({0, 0, 0}, 9000, 0), false, true);
    auto parked = water_chunk({1, 0, 0}, 7000, 1000);
    parked->water_depth_mm[0] = 0;
    producer.changes()->membership(parked, true, true);
    producer.changes()->membership(water_chunk({-1, 0, 0}, -3000, 1000), false, true);
    ASSERT_NO_FATAL_FAILURE(Capture(disk));
    std::shared_ptr<const World::FarFrozenObservation> image;
    ASSERT_TRUE(Compose(image).ok());
    EXPECT_EQ(image->WaterSample(0, 4, 4),
              (std::optional<World::FarFrozenWaterSample>{{false, 9}}));
    EXPECT_EQ(image->WaterSample(0, 16, 0),
              (std::optional<World::FarFrozenWaterSample>{{false, 7}}));
    EXPECT_EQ(image->WaterSample(0, 32, 0),
              (std::optional<World::FarFrozenWaterSample>{{false, 6}}));
    EXPECT_EQ(image->WaterSample(0, 16, 4),
              (std::optional<World::FarFrozenWaterSample>{{true, 8}}));
    EXPECT_EQ(image->WaterSample(0, 36, 4),
              (std::optional<World::FarFrozenWaterSample>{{true, 7}}));
    EXPECT_EQ(image->WaterSample(0, -4, 4),
              (std::optional<World::FarFrozenWaterSample>{{true, -2}}));
    auto params = world->get_params();
    params.height_offset = -1;
    world->set_params(params);
    policy.pristine_identity =
        World::WorldFarVolumeIdentity(*world, policy.caves, policy.numeric, 4);
    ASSERT_NO_FATAL_FAILURE(Capture(disk));
    ASSERT_TRUE(Compose(image).ok());
    EXPECT_EQ(image->WaterSample(0, 100, 100),
              (std::optional<World::FarFrozenWaterSample>{{true, SEA_LEVEL}}));
}

TEST_F(FarWorldFrozenComposeTest, PartialAndAmbiguousExpectedWaterRefusesInsteadOfBecomingBase) {
    WorldStreamingState disk;
    ASSERT_NO_FATAL_FAILURE(Capture(disk));
    std::shared_ptr<const World::FarFrozenObservation> image;
    ASSERT_TRUE(Compose(image).ok());
    const auto accepted = image;
    auto chunk = Edited({0, 0, 0});
    chunk->set_water_authority();
    chunk->current_water_resolution.store(4);
    chunk->water_bed_mm.assign(16, 1000);
    chunk->water_depth_mm.assign(15, 0);
    producer.changes()->membership(chunk, false, true);
    producer.pump_capture();
    EXPECT_EQ(Compose(image).source, Producer::Error::WaterUnavailable);
    EXPECT_EQ(image, accepted);
    chunk->water_depth_mm.assign(16, 0);
    chunk->mark_water_state_dirty();
    producer.pump_capture();
    producer.pump_capture();
    ASSERT_TRUE(Compose(image).ok());
    ASSERT_TRUE(disk.insert_chunk(chunk));
    auto other = Edited({0, 1, 0});
    other->set_water_authority();
    other->current_water_resolution.store(4);
    other->water_bed_mm.assign(16, 1000);
    other->water_depth_mm.assign(16, 0);
    ASSERT_TRUE(disk.insert_chunk(other));
    ASSERT_NO_FATAL_FAILURE(Capture(disk));
    const auto dry = image;
    EXPECT_EQ(Compose(image).source, Producer::Error::WaterUnavailable);
    EXPECT_EQ(image, dry);
}

TEST_F(FarWorldFrozenComposeTest, HaloIdentityWorkAndMemoryRefusalsPreserveAcceptedOutput) {
    WorldStreamingState disk;
    ASSERT_NO_FATAL_FAILURE(Capture(disk));
    std::shared_ptr<const World::FarFrozenObservation> image;
    ASSERT_TRUE(Compose(image).ok());
    EXPECT_EQ(image->WaterCapability(0), World::FarFrozenWaterCapability::ExplicitlyDry);
    const auto accepted = image;
    const auto bytes = pool->used();
    EXPECT_EQ(Compose(image, 2).source, Producer::Error::InvalidWindow);
    auto request = Request();
    request.field_identity ^= 1;
    EXPECT_EQ(producer.compose_frozen(*world, request, policy, limits, pool, image).capture,
              World::FarFrozenCaptureError::IdentityMismatch);
    world->set_seed(2);
    EXPECT_EQ(Compose(image).capture, World::FarFrozenCaptureError::IdentityMismatch);
    world->set_seed(1337);
    for (unsigned cap = 0; cap < 4; ++cap) {
        auto limited = limits;
        if (cap == 0) {
            limited.max_height_samples = 1;
        } else if (cap == 1) {
            limited.max_density_samples = 1;
        } else if (cap == 2) {
            limited.max_water_samples = 1;
        } else {
            limited.max_bytes = 1;
        }
        const auto attempts = pool->attempts();
        EXPECT_EQ(producer.compose_frozen(*world, Request(), policy, limited, pool, image).capture,
                  cap == 3 ? World::FarFrozenCaptureError::ByteLimit
                           : World::FarFrozenCaptureError::WorkLimit);
        EXPECT_EQ(pool->attempts(), attempts);
        EXPECT_EQ(image, accepted);
        EXPECT_EQ(pool->used(), bytes);
    }
    auto refused = std::make_shared<World::FarVolumeMemoryPool>(0);
    EXPECT_EQ(producer.compose_frozen(*world, Request(), policy, limits, refused, image).capture,
              World::FarFrozenCaptureError::ByteLimit);
    EXPECT_EQ(image, accepted);
    producer.invalidate_source();
    EXPECT_EQ(Compose(image).source, Producer::Error::AuthorityUnavailable);
    EXPECT_EQ(image, accepted);
}
} // namespace
