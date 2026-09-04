#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "luminumbra_common/systems/WaterSystem.h"

namespace {

using Luminumbra::ChunkID;
using Luminumbra::Systems::WaterScheduling::Candidate;
using Luminumbra::Systems::WaterScheduling::SelectPriorityWindow;

TEST(WaterSimulationCadence, ActiveHighResolutionChunksKeepMediumLikeCadence) {
    constexpr std::size_t kAwakeChunks = 80;
    constexpr std::size_t kMediumWindow = 64;
    constexpr std::size_t kHighWindow = 16;
    constexpr std::size_t kTicks = 20;

    std::vector<Candidate> candidates;
    candidates.reserve(kAwakeChunks);
    for (std::size_t i = 0; i < kAwakeChunks; ++i) {
        const bool active_front = i >= 36 && i < 44;
        candidates.push_back(
            {static_cast<ChunkID>(1000 + i), active_front ? std::uint64_t{400} : 0});
    }

    std::vector<std::size_t> medium_passes(kAwakeChunks, 0);
    std::vector<std::size_t> high_passes(kAwakeChunks, 0);
    std::size_t medium_cursor = 0;
    std::size_t high_cursor = 0;
    for (std::size_t tick = 0; tick < kTicks; ++tick) {
        for (std::size_t i = 0; i < kMediumWindow; ++i) {
            ++medium_passes[(medium_cursor + i) % kAwakeChunks];
        }
        medium_cursor = (medium_cursor + kMediumWindow) % kAwakeChunks;

        const auto high = SelectPriorityWindow(candidates, kHighWindow, high_cursor);
        ASSERT_EQ(high.chunk_ids.size(), kHighWindow);
        high_cursor = high.next_cursor;
        for (const ChunkID id : high.chunk_ids) {
            ++high_passes[static_cast<std::size_t>(id - 1000)];
        }
    }

    for (std::size_t i = 36; i < 44; ++i) {
        EXPECT_EQ(high_passes[i], kTicks)
            << "active High chunk " << i << " did not receive one simulation pass per wall tick";
        EXPECT_GE(high_passes[i], medium_passes[i])
            << "active High chunk cadence fell below the flat Medium scheduler";
    }
}

TEST(WaterSimulationCadence, PriorityOrderIsTotalAndCursorSharePreventsStarvation) {
    const std::vector<Candidate> candidates = {
        {19, 0}, {13, 50}, {17, 50}, {11, 100}, {23, 0}, {29, 0}, {31, 0}, {37, 0}};
    const auto first = SelectPriorityWindow(candidates, 4, 6);

    ASSERT_EQ(first.chunk_ids.size(), 4u);
    EXPECT_EQ(first.chunk_ids[0], static_cast<ChunkID>(11));
    EXPECT_EQ(first.chunk_ids[1], static_cast<ChunkID>(13));
    EXPECT_EQ(first.chunk_ids[2], static_cast<ChunkID>(31));
    EXPECT_EQ(first.chunk_ids[3], static_cast<ChunkID>(37));
    EXPECT_EQ(first.next_cursor, 0u);

    std::size_t cursor = first.next_cursor;
    std::vector<ChunkID> fair_ids;
    for (std::size_t tick = 0; tick < 16; ++tick) {
        const auto selection = SelectPriorityWindow(candidates, 4, cursor);
        cursor = selection.next_cursor;
        for (const ChunkID id : selection.chunk_ids) {
            if (id != 11 && id != 13) {
                fair_ids.push_back(id);
            }
        }
    }
    for (const ChunkID id :
         {ChunkID{17}, ChunkID{19}, ChunkID{23}, ChunkID{29}, ChunkID{31}, ChunkID{37}}) {
        EXPECT_NE(std::find(fair_ids.begin(), fair_ids.end(), id), fair_ids.end())
            << "round-robin share starved calm chunk " << id;
    }
}

TEST(WaterSimulationCadence, PersistedCursorResumesIdenticalPrioritySelection) {
    const std::vector<Candidate> candidates = {
        {47, 0}, {41, 200}, {53, 0}, {43, 100}, {59, 0}, {61, 0}, {67, 0}};
    const auto before_save = SelectPriorityWindow(candidates, 3, 4);
    const auto uninterrupted = SelectPriorityWindow(candidates, 3, before_save.next_cursor);

    auto reloaded_candidates = candidates;
    std::reverse(reloaded_candidates.begin(), reloaded_candidates.end());
    const auto resumed = SelectPriorityWindow(reloaded_candidates, 3, before_save.next_cursor);

    EXPECT_EQ(resumed.chunk_ids, uninterrupted.chunk_ids);
    EXPECT_EQ(resumed.next_cursor, uninterrupted.next_cursor);
}

} // namespace
