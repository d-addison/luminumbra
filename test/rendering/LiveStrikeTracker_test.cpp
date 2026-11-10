#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "luminumbra_client/rendering/LiveStrikeComposer.h"
#include "luminumbra_client/rendering/LiveStrikeTracker.h"

namespace {

using Luminumbra::Rendering::ComposeLiveStrikes;
using Luminumbra::Rendering::kLiveBoltPenUp;
using Luminumbra::Rendering::LiveStrikeTracker;
using Luminumbra::Rendering::ProjectedLiveStrike;
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

std::vector<glm::vec2> Stroke(glm::vec2 from, glm::vec2 to, std::size_t count) {
    std::vector<glm::vec2> points;
    for (std::size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count - 1);
        points.push_back(from + (to - from) * t);
    }
    return points;
}

ProjectedLiveStrike Strike(glm::vec2 origin, std::size_t branches, float pulse) {
    ProjectedLiveStrike strike;
    strike.main_channel = Stroke(origin, origin + glm::vec2(0.0f, -0.5f), 30);
    for (std::size_t i = 0; i < branches; ++i) {
        strike.branches.push_back(Stroke(origin, origin + glm::vec2(0.2f, -0.2f), 20));
    }
    strike.strike_ndc = origin;
    strike.pulse = pulse;
    return strike;
}

std::size_t Strokes(const std::vector<glm::vec2>& points) {
    std::size_t strokes = points.empty() ? 0 : 1;
    for (const glm::vec2& point : points) {
        if (point == kLiveBoltPenUp) {
            ++strokes;
        }
    }
    return strokes;
}

TEST(LiveStrikeComposer, LaterVisibleChannelSurvivesHeavilyBranchedEarlierStrikes) {
    const std::vector<ProjectedLiveStrike> strikes{Strike({-0.5f, 0.5f}, 4, 0.1f),
                                                   Strike({0.5f, 0.5f}, 4, 0.1f)};
    // Each strike alone needs 30 + 4 * (1 + 20) = 114 points, beyond the cap.
    const auto composed = ComposeLiveStrikes(strikes, 96);
    ASSERT_LE(composed.points.size(), 96u);
    // Both channels (30 + 1 + 30) come first, then one whole branch fits.
    ASSERT_EQ(composed.points.size(), 61u + 21u);
    EXPECT_EQ(Strokes(composed.points), 3u);
    EXPECT_EQ(composed.points.front(), strikes[0].main_channel.front());
    EXPECT_EQ(composed.points[31], strikes[1].main_channel.front());
    EXPECT_EQ(composed.points[30], kLiveBoltPenUp);
    EXPECT_EQ(composed.points[61], kLiveBoltPenUp);
    EXPECT_EQ(composed.points[62], strikes[0].branches[0].front());
}

TEST(LiveStrikeComposer, OffscreenStrikesKeepTheFlashButSpendNoPoints) {
    ProjectedLiveStrike offscreen = Strike({2.5f, 0.5f}, 4, 0.2f);
    ProjectedLiveStrike behind;
    behind.main_channel.assign(30, kLiveBoltPenUp);
    behind.pulse = 0.05f;
    const ProjectedLiveStrike visible = Strike({0.0f, 0.5f}, 2, 0.1f);
    const std::vector<ProjectedLiveStrike> strikes{offscreen, behind, visible};
    const auto composed = ComposeLiveStrikes(strikes, 96);
    // Only the visible strike is drawn: 30 + 2 * (1 + 20) points.
    ASSERT_EQ(composed.points.size(), 72u);
    EXPECT_EQ(composed.points.front(), visible.main_channel.front());
    EXPECT_EQ(Strokes(composed.points), 3u);
    // The strongest flash comes from the offscreen strike and still lights the scene.
    EXPECT_FLOAT_EQ(composed.pulse, 0.2f);
    EXPECT_EQ(composed.strike_ndc, offscreen.strike_ndc);
}

TEST(LiveStrikeComposer, CrossingStrokeIsVisibleAndTiesKeepTheEarlierFlash) {
    ProjectedLiveStrike crossing;
    crossing.main_channel = Stroke({-2.0f, 0.0f}, {2.0f, 0.0f}, 2);
    crossing.strike_ndc = {-2.0f, 0.0f};
    crossing.pulse = 0.1f;
    const ProjectedLiveStrike later = Strike({0.5f, 0.5f}, 0, 0.1f);
    const std::vector<ProjectedLiveStrike> strikes{crossing, later};
    const auto composed = ComposeLiveStrikes(strikes, 96);
    ASSERT_EQ(composed.points.size(), 2u + 1u + 30u);
    EXPECT_EQ(composed.points.front(), crossing.main_channel.front());
    EXPECT_EQ(composed.strike_ndc, crossing.strike_ndc);
}

TEST(LiveStrikeComposer, BranchesNeedTheirOwnPlacedChannel) {
    ProjectedLiveStrike offscreen_channel = Strike({0.0f, 0.5f}, 1, 0.1f);
    offscreen_channel.main_channel = Stroke({3.0f, 0.5f}, {3.0f, 0.0f}, 30);
    const std::vector<ProjectedLiveStrike> strikes{offscreen_channel};
    // The branch is visible, but drawing it without its channel would float in the sky.
    EXPECT_TRUE(ComposeLiveStrikes(strikes, 96).points.empty());
}

TEST(LiveStrikeComposer, EmptyInputAndTinyCapacityDrawNothing) {
    const auto empty = ComposeLiveStrikes({}, 96);
    EXPECT_TRUE(empty.points.empty());
    EXPECT_FLOAT_EQ(empty.pulse, 0.0f);
    const std::vector<ProjectedLiveStrike> strikes{Strike({0.0f, 0.5f}, 1, 0.3f)};
    // The 20-point branch would fit, but its 30-point channel does not.
    const auto tiny = ComposeLiveStrikes(strikes, 25);
    EXPECT_TRUE(tiny.points.empty());
    EXPECT_FLOAT_EQ(tiny.pulse, 0.3f);
    const auto none = ComposeLiveStrikes(strikes, 0);
    EXPECT_TRUE(none.points.empty());
}

} // namespace
