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
using Luminumbra::Rendering::IsLiveBoltPenUp;
using Luminumbra::Rendering::kLiveBoltPenUp;
using Luminumbra::Rendering::LiveSegmentTouchesView;
using Luminumbra::Rendering::LiveStrikeTracker;
using Luminumbra::Rendering::ProjectedLiveStrike;
using Luminumbra::Rendering::SelectLiveStrikeGeometry;
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
        if (IsLiveBoltPenUp(point)) {
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

TEST(LiveStrikeComposer, StrongestStrikeIsPlacedFirstWhenChannelsExceedTheBudget) {
    const std::vector<ProjectedLiveStrike> strikes{Strike({-0.5f, 0.5f}, 1, 0.1f),
                                                   Strike({0.0f, 0.5f}, 0, 0.1f),
                                                   Strike({0.5f, 0.5f}, 1, 0.4f)};
    // Three channels need 30 + 31 + 31 points; only two fit beside one branch.
    const auto composed = ComposeLiveStrikes(strikes, 82);
    ASSERT_EQ(composed.points.size(), 61u + 21u);
    EXPECT_EQ(Strokes(composed.points), 3u);
    // The strongest strike comes first, then the earlier of the tied weaker strikes.
    EXPECT_EQ(composed.points.front(), strikes[2].main_channel.front());
    EXPECT_EQ(composed.points[31], strikes[0].main_channel.front());
    // Branches follow the same order, so the strongest strike keeps its fork.
    EXPECT_EQ(composed.points[62], strikes[2].branches[0].front());
    EXPECT_FLOAT_EQ(composed.pulse, 0.4f);
    EXPECT_EQ(composed.strike_ndc, strikes[2].strike_ndc);
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
    EXPECT_EQ(composed.points[0], glm::vec2(-1.0f, 0.0f));
    EXPECT_EQ(composed.points[1], glm::vec2(1.0f, 0.0f));
    EXPECT_FALSE(IsLiveBoltPenUp(composed.points[0]));
    EXPECT_FALSE(IsLiveBoltPenUp(composed.points[1]));
    EXPECT_EQ(composed.strike_ndc, crossing.strike_ndc);
}

TEST(LiveStrikeComposer, CrossingSegmentsUploadOnlyClippedRealPoints) {
    const std::array<std::array<glm::vec2, 2>, 8> segments{{
        {{{-2.0f, 0.0f}, {2.0f, 0.0f}}},
        {{{2.0f, 0.0f}, {-2.0f, 0.0f}}},
        {{{-3.0f, 0.0f}, {0.0f, 0.0f}}},
        {{{-1000.0f, 0.0f}, {1000.0f, 0.0f}}},
        {{{0.0f, -3.0f}, {0.0f, 3.0f}}},
        {{{0.0f, 3.0f}, {0.0f, -3.0f}}},
        {{{-2.0f, -2.0f}, {2.0f, 2.0f}}},
        {{{-2.0f, 1.0f}, {2.0f, 1.0f}}},
    }};
    for (const auto& segment : segments) {
        ProjectedLiveStrike strike;
        strike.main_channel.assign(segment.begin(), segment.end());
        const auto composed = ComposeLiveStrikes(std::span(&strike, 1), 2);
        ASSERT_EQ(composed.points.size(), 2u);
        for (const glm::vec2 point : composed.points) {
            // Both the shader's explicit predicate and its former x <= -2 test
            // must distinguish every uploaded real endpoint from a pen-up.
            EXPECT_FALSE(IsLiveBoltPenUp(point));
            EXPECT_GT(point.x, -2.0f);
            EXPECT_GE(point.x, -1.0f);
            EXPECT_LE(point.x, 1.0f);
            EXPECT_GE(point.y, -1.0f);
            EXPECT_LE(point.y, 1.0f);
        }
    }
}

TEST(LiveStrikeComposer, ClippedExcursionsAndPenUpsKeepDisconnectedPiecesSeparate) {
    ProjectedLiveStrike strike;
    strike.main_channel = {{-2.0f, 0.0f},
                           {0.0f, 0.0f},
                           {2.0f, 0.0f},
                           {2.0f, 0.5f},
                           {0.0f, 0.5f},
                           kLiveBoltPenUp,
                           {-2.0f, -0.5f},
                           {2.0f, -0.5f}};
    const std::vector<glm::vec2> expected{{-1.0f, 0.0f},
                                          {0.0f, 0.0f},
                                          {1.0f, 0.0f},
                                          kLiveBoltPenUp,
                                          {1.0f, 0.5f},
                                          {0.0f, 0.5f},
                                          kLiveBoltPenUp,
                                          {-1.0f, -0.5f},
                                          {1.0f, -0.5f}};
    EXPECT_EQ(ComposeLiveStrikes(std::span(&strike, 1), expected.size()).points, expected);
    // A stroke that grows beyond the budget through clipping is dropped whole.
    EXPECT_TRUE(ComposeLiveStrikes(std::span(&strike, 1), expected.size() - 1).points.empty());
    // A turn outside the view also needs a break when both segments touch the view.
    strike.main_channel = {{0.0f, 0.0f}, {2.0f, 0.0f}, {0.0f, 1.0f}};
    EXPECT_EQ(ComposeLiveStrikes(std::span(&strike, 1), 5).points,
              (std::vector<glm::vec2>{
                  {0.0f, 0.0f}, {1.0f, 0.0f}, kLiveBoltPenUp, {1.0f, 0.5f}, {0.0f, 1.0f}}));
    // Overflow rolls back only this stroke, preserving earlier geometry and its flash.
    const ProjectedLiveStrike earlier = Strike({-0.5f, 0.5f}, 0, 0.2f);
    const std::array<ProjectedLiveStrike, 2> strikes{earlier, strike};
    const auto composed = ComposeLiveStrikes(strikes, earlier.main_channel.size() + 5);
    EXPECT_EQ(composed.points, earlier.main_channel);
    EXPECT_FLOAT_EQ(composed.pulse, earlier.pulse);
}

TEST(LiveStrikeComposer, NonFiniteEndpointsDoNotUploadOrJoinAcrossInvalidSegments) {
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const glm::vec2 invalid : {glm::vec2(infinity, 0.0f),
                                    glm::vec2(0.0f, -infinity),
                                    glm::vec2(nan, 0.0f),
                                    glm::vec2(0.0f, nan)}) {
        ProjectedLiveStrike strike;
        strike.main_channel = {{0.0f, 0.0f}, invalid};
        EXPECT_TRUE(ComposeLiveStrikes(std::span(&strike, 1), 96).points.empty());
        EXPECT_FALSE(LiveSegmentTouchesView({0.0f, 0.0f}, invalid));
        EXPECT_FALSE(LiveSegmentTouchesView(invalid, {0.0f, 0.0f}));
        strike.main_channel = {{-0.5f, 0.0f}, {0.0f, 0.0f}, invalid, {0.0f, 0.5f}, {0.5f, 0.5f}};
        EXPECT_EQ(ComposeLiveStrikes(std::span(&strike, 1), 96).points,
                  (std::vector<glm::vec2>{
                      {-0.5f, 0.0f}, {0.0f, 0.0f}, kLiveBoltPenUp, {0.0f, 0.5f}, {0.5f, 0.5f}}));
    }
}

TEST(LiveStrikeComposer, VisibleBranchOfAnOffscreenChannelIsDrawn) {
    ProjectedLiveStrike offscreen_channel = Strike({0.0f, 0.5f}, 1, 0.1f);
    offscreen_channel.main_channel = Stroke({3.0f, 0.5f}, {3.0f, 0.0f}, 30);
    const std::vector<ProjectedLiveStrike> strikes{offscreen_channel};
    // The channel is rejected for visibility, not capacity, so its fork still shows.
    const auto composed = ComposeLiveStrikes(strikes, 96);
    ASSERT_EQ(composed.points.size(), 20u);
    EXPECT_EQ(composed.points.front(), offscreen_channel.branches[0].front());
    EXPECT_EQ(Strokes(composed.points), 1u);
}

TEST(LiveStrikeComposer, DiagonalStrokesOutsideTheViewSpendNoPoints) {
    // Each segment's bounding box overlaps the view, but the segment passes the corner.
    std::vector<ProjectedLiveStrike> strikes;
    for (int i = 0; i < 3; ++i) {
        ProjectedLiveStrike diagonal;
        diagonal.main_channel = Stroke({0.5f, 1.6f}, {1.6f, 0.5f}, 30);
        diagonal.strike_ndc = {1.6f, 0.5f};
        diagonal.pulse = 0.05f;
        strikes.push_back(diagonal);
    }
    const ProjectedLiveStrike visible = Strike({0.0f, 0.5f}, 0, 0.1f);
    strikes.push_back(visible);
    const auto composed = ComposeLiveStrikes(strikes, 96);
    ASSERT_EQ(composed.points.size(), 30u);
    EXPECT_EQ(composed.points.front(), visible.main_channel.front());
    EXPECT_FALSE(LiveSegmentTouchesView({0.5f, 1.6f}, {1.6f, 0.5f}));
    EXPECT_TRUE(LiveSegmentTouchesView({0.5f, 1.4f}, {1.4f, 0.5f}));
    EXPECT_TRUE(LiveSegmentTouchesView({0.0f, 0.0f}, {0.0f, 0.0f}));
    EXPECT_FALSE(LiveSegmentTouchesView({2.0f, 2.0f}, {2.0f, 2.0f}));
}

TEST(LiveStrikeComposer, GeometrySelectionKeepsTheStrongestStrikesInOrder) {
    const std::vector<float> pulses{0.1f, 0.5f, 0.3f, 0.5f, 0.2f};
    EXPECT_EQ(SelectLiveStrikeGeometry(pulses, 3), (std::vector<std::size_t>{1, 2, 3}));
    EXPECT_EQ(SelectLiveStrikeGeometry(pulses, 1), (std::vector<std::size_t>{1}));
    EXPECT_EQ(SelectLiveStrikeGeometry(pulses, 9), (std::vector<std::size_t>{0, 1, 2, 3, 4}));
    EXPECT_TRUE(SelectLiveStrikeGeometry(pulses, 0).empty());
    EXPECT_TRUE(SelectLiveStrikeGeometry({}, 4).empty());
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
