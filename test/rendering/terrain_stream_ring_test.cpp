#include "luminumbra_client/rendering/RenderPreparationScratch.h"
#include "luminumbra_client/rendering/TerrainStreamRing.h"
#include "luminumbra_client/rendering/UploadBudget.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>

namespace {
using Luminumbra::Rendering::TerrainStreamRing;

TEST(TerrainStreamRing, FiveSubmissionsAcrossDelayedFramesNeverOverlapLiveStorage) {
    TerrainStreamRing ring;
    constexpr std::size_t frames = 64;
    constexpr std::size_t submissions = frames * TerrainStreamRing::kSubmissionsPerFrame;
    struct Range {
        std::size_t slot = 0;
        std::size_t generation = 0;
    };
    std::array<Range, submissions> live{};
    std::array<std::size_t, TerrainStreamRing::kSlotCount> storage_generations{};
    std::size_t count = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        ring.begin_frame();
        for (std::size_t submit = 0; submit < TerrainStreamRing::kSubmissionsPerFrame; ++submit) {
            // No GPU consumer has completed. Orphan creates a distinct backing
            // store for the same buffer name, as glBufferData(nullptr) does.
            const auto allocation = ring.acquire(false);
            EXPECT_EQ(allocation.slot, count % TerrainStreamRing::kSlotCount);
            EXPECT_EQ(allocation.orphan, count >= TerrainStreamRing::kSlotCount);
            if (allocation.orphan)
                ++storage_generations[allocation.slot];
            const Range range{.slot = allocation.slot,
                              .generation = storage_generations[allocation.slot]};
            for (std::size_t earlier = 0; earlier < count; ++earlier) {
                EXPECT_FALSE(live[earlier].slot == range.slot &&
                             live[earlier].generation == range.generation);
            }
            live[count++] = range;
        }
        EXPECT_EQ(ring.diagnostics().submissions, 5u);
        EXPECT_EQ(ring.diagnostics().same_frame_slot_reuses, 0u);
    }
}

TEST(TerrainStreamRing, CompletedStorageReusesInDeterministicSlotOrder) {
    TerrainStreamRing ring;
    for (std::size_t round = 0; round < 8; ++round) {
        ring.begin_frame();
        for (std::size_t slot = 0; slot < TerrainStreamRing::kSlotCount; ++slot) {
            EXPECT_EQ(ring.cursor(), slot);
            const auto allocation = ring.acquire(true);
            EXPECT_EQ(allocation.slot, slot);
            EXPECT_FALSE(allocation.orphan);
        }
        EXPECT_EQ(ring.cursor(), 0u);
        EXPECT_EQ(ring.diagnostics().same_frame_slot_reuses, 0u);
        EXPECT_EQ(ring.diagnostics().orphanings, 0u);
    }
}

TEST(TerrainStreamRing, MissingPendingAndFailedCompletionRequireReplacement) {
    TerrainStreamRing ring;
    for (std::size_t slot = 0; slot < TerrainStreamRing::kSlotCount; ++slot)
        EXPECT_FALSE(ring.acquire(false).orphan);
    ring.begin_frame();
    // All three GL failure outcomes supply false; only positive completion
    // permits writes into the previous store, even on the oldest frame.
    for (std::size_t outcome = 0; outcome < 3; ++outcome)
        EXPECT_TRUE(ring.acquire(false).orphan);
    EXPECT_FALSE(ring.acquire(true).orphan);
    EXPECT_EQ(ring.diagnostics().orphanings, 3u);
}

TEST(TerrainStreamRing, ReplayRestoresSchedulingButNeverRollsBackLiveOwnership) {
    TerrainStreamRing ring;
    const auto saved_cursor = ring.cursor();
    EXPECT_FALSE(ring.acquire(false, true).orphan);
    ring.restore_cursor(saved_cursor);
    EXPECT_TRUE(ring.acquire(false, true).orphan);
    ring.restore_cursor(saved_cursor);
    EXPECT_TRUE(ring.acquire(false).orphan);
    EXPECT_EQ(ring.diagnostics().submissions, 1u);
    EXPECT_EQ(ring.diagnostics().same_frame_slot_reuses, 0u);
    EXPECT_EQ(ring.diagnostics().orphanings, 1u);
    // Restoring an out-of-range cursor is bounded and deterministic as well.
    ring.restore_cursor(TerrainStreamRing::kSlotCount + 2u);
    EXPECT_EQ(ring.cursor(), 2u);
}

TEST(TerrainStreamRing, CounterDetectsSameFrameReuseAndResetsWithoutReleasingStorage) {
    TerrainStreamRing ring;
    ring.begin_frame();
    EXPECT_EQ(ring.diagnostics().submissions, 0u); // empty terrain frame
    for (std::size_t slot = 0; slot <= TerrainStreamRing::kSlotCount; ++slot)
        (void)ring.acquire(false);
    EXPECT_EQ(ring.diagnostics().same_frame_slot_reuses, 1u);
    ring.begin_frame();
    EXPECT_EQ(ring.diagnostics().submissions, 0u);
    EXPECT_EQ(ring.diagnostics().same_frame_slot_reuses, 0u);
    EXPECT_EQ(ring.diagnostics().orphanings, 0u);
    EXPECT_TRUE(ring.acquire(false).orphan);
}

TEST(TerrainStreamRing, CapacityAndExactSizesRejectOverflowBeforeAllocation) {
    constexpr auto maximum = TerrainStreamRing::kMaxCommands;
    constexpr auto size_limit = std::numeric_limits<std::size_t>::max();
    EXPECT_EQ(TerrainStreamRing::capacity(0), 384u);
    EXPECT_EQ(TerrainStreamRing::capacity(256), 384u);
    EXPECT_EQ(TerrainStreamRing::capacity(maximum), maximum);
    EXPECT_EQ(TerrainStreamRing::capacity(maximum - 1u), maximum);
    EXPECT_THROW((void)TerrainStreamRing::capacity(maximum + 1u), std::length_error);
    EXPECT_THROW((void)TerrainStreamRing::capacity(size_limit), std::length_error);
    EXPECT_EQ(TerrainStreamRing::storage_bytes(0, 20, size_limit), 0u);
    EXPECT_EQ(TerrainStreamRing::storage_bytes(maximum, 20, size_limit), maximum * 20u);
    EXPECT_EQ(TerrainStreamRing::storage_bytes(2, 20, 40), 40u);
    EXPECT_THROW((void)TerrainStreamRing::storage_bytes(2, 20, 39), std::length_error);
    EXPECT_THROW((void)TerrainStreamRing::storage_bytes(2, size_limit, size_limit),
                 std::length_error);
    EXPECT_THROW((void)TerrainStreamRing::storage_bytes(1, 0, size_limit), std::length_error);
    EXPECT_THROW((void)TerrainStreamRing::storage_bytes(maximum + 1u, 1, size_limit),
                 std::length_error);
}
} // namespace

namespace {
using namespace Luminumbra::Rendering;

TEST(RenderPreparationScratch, AllocatorCountsGrowthAndRejectsOverflowBeforeAllocation) {
    std::uint64_t allocations = 0;
    RenderScratchAllocator<std::uint64_t> allocator(allocations);
    std::vector<std::uint64_t, decltype(allocator)> scratch(allocator);
    scratch.reserve(64);
    EXPECT_EQ(allocations, 1u);
    for (std::uint64_t frame = 0; frame < 8; ++frame) {
        scratch.clear();
        for (std::uint64_t item = 0; item < 64; ++item) {
            scratch.push_back(frame + item);
        }
    }
    EXPECT_EQ(allocations, 1u);
    EXPECT_EQ(scratch.back(), 70u);
    EXPECT_THROW((void)allocator.allocate(std::numeric_limits<std::size_t>::max()),
                 std::bad_array_new_length);
    EXPECT_EQ(allocations, 1u);
    // Rebinding for node containers must charge the same owner's counter.
    RenderScratchAllocator<std::uint32_t> rebound(allocator);
    auto* storage = rebound.allocate(1);
    EXPECT_EQ(allocations, 2u);
    rebound.deallocate(storage, 1);
}

TEST(RenderPreparationScratch, ExactMembershipIgnoresOrderAndDetectsSameSizeReplacement) {
    std::uint64_t allocations = 0;
    TerrainResidencyRevision membership(allocations);
    membership.begin_frame();
    membership.end_frame();
    EXPECT_EQ(membership.revision(), 0u);
    for (std::uint64_t frame = 0; frame < 8; ++frame) {
        membership.begin_frame();
        for (std::uint64_t item = 0; item < 64; ++item) {
            membership.mark(frame % 2 == 0 ? item : 63 - item);
        }
        membership.mark(0); // Repeated observations do not change membership.
        membership.end_frame();
        EXPECT_EQ(membership.size(), 64u);
        EXPECT_EQ(membership.revision(), 64u);
        if (frame == 0) {
            EXPECT_GT(allocations, 0u);
        }
    }
    const auto warmed_allocations = allocations;
    membership.begin_frame();
    for (std::uint64_t item = 0; item < 64; ++item) {
        membership.mark(item);
    }
    membership.end_frame();
    EXPECT_EQ(allocations, warmed_allocations);
    membership.begin_frame();
    for (std::uint64_t item = 1; item <= 64; ++item) {
        membership.mark(item);
    }
    membership.end_frame();
    EXPECT_EQ(membership.size(), 64u);
    EXPECT_NE(membership.revision(), 64u);
    const auto replaced_revision = membership.revision();
    membership.begin_frame();
    membership.end_frame();
    EXPECT_EQ(membership.size(), 0u);
    EXPECT_NE(membership.revision(), replaced_revision);
    const auto empty_revision = membership.revision();
    membership.begin_frame();
    membership.end_frame();
    EXPECT_EQ(membership.revision(), empty_revision);
    membership.begin_frame();
    membership.mark(0);
    membership.end_frame();
    EXPECT_EQ(membership.size(), 1u);
    EXPECT_NE(membership.revision(), empty_revision);
}

TEST(RenderPreparationScratch, StampWrapCannotKeepAnUnvisitedRecordAlive) {
    struct Record {
        std::uint64_t last_seen_frame = 0;
    };
    std::unordered_map<std::uint64_t, Record> records{
        {1, {.last_seen_frame = 1}},
        {2, {.last_seen_frame = std::numeric_limits<std::uint64_t>::max()}}};
    EXPECT_EQ(NextRenderFrameStamp(0, records), 1u);
    EXPECT_EQ(NextRenderFrameStamp(12, records), 13u);
    EXPECT_EQ(records.at(1).last_seen_frame, 1u);
    const auto wrapped = NextRenderFrameStamp(std::numeric_limits<std::uint64_t>::max(), records);
    EXPECT_EQ(wrapped, 1u);
    for (const auto& [id, record] : records) {
        (void)id;
        EXPECT_EQ(record.last_seen_frame, 0u);
        EXPECT_NE(record.last_seen_frame, wrapped);
    }
    records.clear();
    EXPECT_EQ(NextRenderFrameStamp(std::numeric_limits<std::uint64_t>::max(), records), 1u);
}

TEST(RenderPreparationScratch, EmptyRefreshPrunesBacklogWithEitherCandidateAllocator) {
    UploadQueue<std::uint64_t> queue;
    queue.refresh(1, 1);
    std::uint64_t allocations = 0;
    std::vector<std::uint64_t, RenderScratchAllocator<std::uint64_t>> candidates{
        RenderScratchAllocator<std::uint64_t>(allocations)};
    const auto key = [](std::uint64_t id) {
        return id;
    };
    refresh_uploads(queue, candidates, key, key);
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_EQ(allocations, 0u);
    refresh_uploads(queue, std::vector<std::uint64_t>{}, key, key);
    EXPECT_EQ(queue.size(), 0u);
    candidates.push_back(7);
    refresh_uploads(queue, candidates, key, key);
    ASSERT_EQ(queue.size(), 1u);
    EXPECT_EQ(queue.front(), 7u);
}
} // namespace
