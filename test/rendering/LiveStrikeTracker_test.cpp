#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "luminumbra_client/rendering/LiveStrikeTracker.h"

namespace {

using Luminumbra::Rendering::LiveStrikeTracker;
using Luminumbra::Systems::StrikeEvent;

TEST(LiveStrikeTracker, DistinctSameTickIdentitiesAreConsumedOnce) {
    LiveStrikeTracker tracker;
    tracker.AdvanceTo(42);
    const std::array<StrikeEvent, 4> strikes{{
        {.strike_tick = 42, .world_x = 10.0f, .world_z = 20.0f, .magnitude = 0.8f, .storm_salt = 7},
        {.strike_tick = 42, .world_x = 10.0f, .world_z = 20.0f, .magnitude = 0.8f, .storm_salt = 8},
        {.strike_tick = 42, .world_x = 11.0f, .world_z = 20.0f, .magnitude = 0.8f, .storm_salt = 7},
        {.strike_tick = 42, .world_x = 10.0f, .world_z = 21.0f, .magnitude = 0.8f, .storm_salt = 7},
    }};
    for (const auto& strike : strikes) {
        EXPECT_TRUE(tracker.TryConsume(strike));
    }
    tracker.AdvanceTo(42);
    for (const auto& strike : strikes) {
        EXPECT_FALSE(tracker.TryConsume(strike));
    }
    auto changed_magnitude = strikes.front();
    changed_magnitude.magnitude = 0.4f;
    EXPECT_FALSE(tracker.TryConsume(changed_magnitude));
    EXPECT_EQ(tracker.size(), strikes.size());
}

TEST(LiveStrikeTracker, DueWindowAcceptsOutOfOrderTicksAndRefusesOutsideWithoutMutation) {
    LiveStrikeTracker tracker;
    tracker.AdvanceTo(42);
    for (const std::uint64_t tick : {42u, 40u, 41u}) {
        EXPECT_TRUE(tracker.TryConsume(StrikeEvent{.strike_tick = tick}));
    }
    EXPECT_FALSE(tracker.TryConsume(StrikeEvent{.strike_tick = 39}));
    EXPECT_FALSE(tracker.TryConsume(StrikeEvent{.strike_tick = 43}));
    EXPECT_EQ(tracker.size(), 3u);
    tracker.AdvanceTo(43);
    EXPECT_EQ(tracker.size(), 2u);
    EXPECT_TRUE(tracker.TryConsume(StrikeEvent{.strike_tick = 43}));
    EXPECT_FALSE(tracker.TryConsume(StrikeEvent{.strike_tick = 41}));
    tracker.AdvanceTo(46);
    EXPECT_EQ(tracker.size(), 0u);
    tracker.AdvanceTo(47);
    EXPECT_EQ(tracker.size(), 0u);
}

TEST(LiveStrikeTracker, ExpiryReclaimsCapacityWithoutForgettingLiveIdentities) {
    LiveStrikeTracker tracker;
    tracker.AdvanceTo(2);
    for (std::uint64_t tick = 0; tick <= 2; ++tick) {
        std::size_t consumed = 0;
        for (std::uint32_t salt = 0; salt < LiveStrikeTracker::kStrikesPerTick; ++salt) {
            consumed +=
                tracker.TryConsume(StrikeEvent{.strike_tick = tick, .storm_salt = salt}) ? 1u : 0u;
        }
        ASSERT_EQ(consumed, LiveStrikeTracker::kStrikesPerTick);
    }
    EXPECT_EQ(tracker.size(), LiveStrikeTracker::kCapacity);
    EXPECT_FALSE(tracker.TryConsume(StrikeEvent{.strike_tick = 2, .storm_salt = 999999}));
    EXPECT_EQ(tracker.size(), LiveStrikeTracker::kCapacity);
    tracker.AdvanceTo(3);
    EXPECT_EQ(tracker.size(), LiveStrikeTracker::kCapacity - LiveStrikeTracker::kStrikesPerTick);
    for (std::uint32_t salt = 0; salt < LiveStrikeTracker::kStrikesPerTick; ++salt) {
        EXPECT_FALSE(tracker.TryConsume(StrikeEvent{.strike_tick = 0, .storm_salt = salt}));
        EXPECT_FALSE(tracker.TryConsume(StrikeEvent{.strike_tick = 1, .storm_salt = salt}));
        EXPECT_TRUE(tracker.TryConsume(StrikeEvent{.strike_tick = 3, .storm_salt = salt}));
    }
    EXPECT_EQ(tracker.size(), LiveStrikeTracker::kCapacity);
}

TEST(LiveStrikeTracker, EarlierPollDropsFutureHistoryWithoutUnsignedUnderflow) {
    LiveStrikeTracker tracker;
    tracker.AdvanceTo(42);
    EXPECT_TRUE(tracker.TryConsume(StrikeEvent{.strike_tick = 42}));
    tracker.AdvanceTo(41);
    EXPECT_EQ(tracker.size(), 0u);
    EXPECT_FALSE(tracker.TryConsume(StrikeEvent{.strike_tick = 42}));
    EXPECT_TRUE(tracker.TryConsume(StrikeEvent{.strike_tick = 41}));
    tracker.AdvanceTo(42);
    EXPECT_TRUE(tracker.TryConsume(StrikeEvent{.strike_tick = 42}));
    EXPECT_FALSE(tracker.TryConsume(StrikeEvent{.strike_tick = 41}));
}

TEST(LiveStrikeTracker, ClearAndWorldStateReplacementStartFresh) {
    LiveStrikeTracker tracker;
    tracker.AdvanceTo(100);
    const StrikeEvent strike{.strike_tick = 100};
    ASSERT_TRUE(tracker.TryConsume(strike));
    tracker.Clear();
    EXPECT_EQ(tracker.size(), 0u);
    tracker.AdvanceTo(100);
    EXPECT_TRUE(tracker.TryConsume(strike));
    tracker = {};
    EXPECT_EQ(tracker.size(), 0u);
    EXPECT_TRUE(tracker.TryConsume(StrikeEvent{.strike_tick = 0}));
    tracker.AdvanceTo(100);
    EXPECT_TRUE(tracker.TryConsume(strike));
}

TEST(LiveStrikeTracker, TickZeroAndMaximumTickDoNotOverflowTheWindow) {
    LiveStrikeTracker tracker;
    const StrikeEvent first{.strike_tick = 0};
    EXPECT_TRUE(tracker.TryConsume(first));
    EXPECT_FALSE(tracker.TryConsume(first));
    const auto last_tick = std::numeric_limits<std::uint64_t>::max();
    tracker.AdvanceTo(last_tick);
    EXPECT_EQ(tracker.size(), 0u);
    EXPECT_FALSE(tracker.TryConsume(StrikeEvent{.strike_tick = last_tick - 3}));
    EXPECT_TRUE(tracker.TryConsume(StrikeEvent{.strike_tick = last_tick - 2}));
    const StrikeEvent last{.strike_tick = last_tick};
    EXPECT_TRUE(tracker.TryConsume(last));
    tracker.AdvanceTo(last_tick);
    EXPECT_FALSE(tracker.TryConsume(last));
    EXPECT_EQ(tracker.size(), 2u);
}

} // namespace
