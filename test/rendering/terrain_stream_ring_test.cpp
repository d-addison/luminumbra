#include "luminumbra_client/rendering/TerrainStreamRing.h"

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
