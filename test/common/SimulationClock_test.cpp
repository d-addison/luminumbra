// SimulationClock accumulator/clamp behavior and the deterministic
// fixed-tick + ordered-event-bus hosting in GameSession::TickSimulation.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "luminumbra_common/core/SimulationClock.h"
#include "luminumbra_common/simulation/SimulationEventBus.h"
#include "luminumbra_common/world/GameSession.h"

namespace {

using luminumbra::core::SimulationClock;
using luminumbra::simulation::OrderedEventBus;
using luminumbra::simulation::SimulationEvent;
using Luminumbra::world::GameSession;

TEST(SimulationClockTest, CanonicalRateIs30Hz) {
    SimulationClock clock;
    EXPECT_DOUBLE_EQ(clock.tick_rate_hz(), 30.0);
    EXPECT_DOUBLE_EQ(clock.fixed_dt(), 1.0 / 30.0);
    EXPECT_EQ(clock.max_catch_up_ticks(), 4u);
    EXPECT_EQ(clock.tick_count(), 0u);
}

TEST(SimulationClockTest, ThreeFramesOfPointOneSecondYieldNineTicksAt30Hz) {
    SimulationClock clock;
    std::uint32_t total = 0;
    for (int frame = 0; frame < 3; ++frame) {
        total += clock.advance(0.1);
    }
    EXPECT_EQ(total, 9u);
    EXPECT_EQ(clock.tick_count(), 9u);
    EXPECT_LT(clock.accumulator(), clock.fixed_dt());
    EXPECT_DOUBLE_EQ(clock.dropped_time_seconds(), 0.0);
    EXPECT_EQ(clock.dropped_frame_count(), 0u);
}

TEST(SimulationClockTest, SubTickFramesAccumulateWithoutLosingTime) {
    SimulationClock clock;
    // 1/60 s frames at 30 Hz: every second frame produces one tick.
    std::uint32_t total = 0;
    for (int frame = 0; frame < 10; ++frame) {
        total += clock.advance(1.0 / 60.0);
    }
    EXPECT_EQ(total, 5u);
    EXPECT_EQ(clock.tick_count(), 5u);
    EXPECT_DOUBLE_EQ(clock.dropped_time_seconds(), 0.0);
}

TEST(SimulationClockTest, CatchUpClampDropsExcessTimeWithTelemetry) {
    SimulationClock clock;
    // A one-second stall at 30 Hz wants 30 ticks; the clamp allows 4 and
    // drops the remaining 26 whole ticks of simulated time.
    const std::uint32_t ticks = clock.advance(1.0);
    EXPECT_EQ(ticks, 4u);
    EXPECT_EQ(clock.tick_count(), 4u);
    EXPECT_NEAR(clock.dropped_time_seconds(), 26.0 / 30.0, 1e-9);
    EXPECT_EQ(clock.dropped_frame_count(), 1u);
    EXPECT_LT(clock.accumulator(), clock.fixed_dt());

    // After the clamp the clock resumes normal cadence with no replay debt.
    const std::uint32_t next = clock.advance(1.0 / 30.0);
    EXPECT_EQ(next, 1u);
    EXPECT_EQ(clock.tick_count(), 5u);
    EXPECT_EQ(clock.dropped_frame_count(), 1u);
}

TEST(SimulationClockTest, ConfigurableRateForTests) {
    SimulationClock clock(10.0, 2);
    EXPECT_DOUBLE_EQ(clock.fixed_dt(), 0.1);
    EXPECT_EQ(clock.advance(0.35), 2u); // 3 possible, clamp 2, drop 1
    EXPECT_EQ(clock.tick_count(), 2u);
    EXPECT_NEAR(clock.dropped_time_seconds(), 0.1, 1e-9);
    EXPECT_EQ(clock.dropped_frame_count(), 1u);
}

TEST(SimulationClockTest, NonPositiveAndNonFiniteFrameDtProducesNoTicks) {
    SimulationClock clock;
    EXPECT_EQ(clock.advance(0.0), 0u);
    EXPECT_EQ(clock.advance(-1.0), 0u);
    EXPECT_EQ(clock.tick_count(), 0u);
    EXPECT_DOUBLE_EQ(clock.accumulator(), 0.0);
}

TEST(SimulationClockTest, ResetClearsAllCounters) {
    SimulationClock clock;
    clock.advance(1.0);
    clock.reset();
    EXPECT_EQ(clock.tick_count(), 0u);
    EXPECT_DOUBLE_EQ(clock.accumulator(), 0.0);
    EXPECT_DOUBLE_EQ(clock.dropped_time_seconds(), 0.0);
    EXPECT_EQ(clock.dropped_frame_count(), 0u);
}

// --- GameSession::TickSimulation hosting ---

struct SessionRunResult {
    std::uint64_t tick_count = 0;
    std::vector<std::string> delivered_order;
    std::string checksum;
};

SessionRunResult RunHostedSimulation() {
    GameSession session;
    OrderedEventBus& bus = session.GetSimulationEventBus();

    std::vector<SimulationEvent> delivered;
    bus.subscribe([&delivered](const SimulationEvent& event) { delivered.push_back(event); });

    // Publish out of tick order, across lanes, before any tick runs.
    bus.publish(2, "ai.intent", "npc-1:turn");
    bus.publish(1, "input.command", "player:move");
    bus.publish(1, "script.trigger", "door:open");
    bus.publish(1, "physics.impulse", "crate:push", -1);
    bus.publish(3, "audio.event", "stone:slide");

    // GameSession's clock clamps catch-up at 2 ticks/frame (GameSession.h: spike guard), so a 0.1 s
    // frame yields 2 ticks (the 3rd possible tick's TIME is dropped, but no tick id is skipped —
    // the cadence just falls behind real-time). Frame 1 -> ticks 1..2; events for ticks <= 2 drain
    // in deterministic tick/lane/sequence order.
    EXPECT_EQ(session.TickSimulation(0.1), 2u);

    // Late-published events for an already-future tick drain when that tick runs.
    bus.publish(5, "script.trigger", "torch:light");
    bus.publish(5, "ai.intent", "npc-2:wait", -1);

    // Frame 2 -> ticks 3..4 (draining the tick-3 audio event); Frame 3 -> ticks 5..6 (draining the
    // tick-5 events). Three 0.1 s frames reach tick 6 under the 2-tick/frame clamp.
    EXPECT_EQ(session.TickSimulation(0.1), 2u);
    EXPECT_EQ(session.TickSimulation(0.1), 2u);

    SessionRunResult result;
    result.tick_count = session.GetSimulationTickCount();
    result.delivered_order = luminumbra::simulation::describe_event_order(delivered);
    result.checksum = luminumbra::simulation::eventbus_order_checksum(delivered);
    return result;
}

TEST(GameSessionTickSimulationTest, AdvancesClockAndDrainsBusPerTick) {
    const SessionRunResult run = RunHostedSimulation();
    EXPECT_EQ(run.tick_count, 6u);

    const std::vector<std::string> expected_order = {
        "1|-1|3|physics.impulse|crate:push",
        "1|0|1|input.command|player:move",
        "1|0|2|script.trigger|door:open",
        "2|0|0|ai.intent|npc-1:turn",
        "3|0|4|audio.event|stone:slide",
        "5|-1|6|ai.intent|npc-2:wait",
        "5|0|5|script.trigger|torch:light",
    };
    EXPECT_EQ(run.delivered_order, expected_order);
}

TEST(GameSessionTickSimulationTest, DrainOrderIsDeterministicAcrossIdenticalRuns) {
    const SessionRunResult first = RunHostedSimulation();
    const SessionRunResult second = RunHostedSimulation();

    EXPECT_EQ(first.tick_count, second.tick_count);
    EXPECT_EQ(first.delivered_order, second.delivered_order);
    EXPECT_EQ(first.checksum, second.checksum);
    EXPECT_FALSE(first.checksum.empty());
}

TEST(GameSessionTickSimulationTest, FutureTickEventsStayQueuedUntilEligible) {
    GameSession session;
    OrderedEventBus& bus = session.GetSimulationEventBus();
    bus.publish(100, "future.topic", "payload");

    EXPECT_EQ(session.TickSimulation(0.1), 2u); // ticks 1..2 (clamped at 2/frame in GameSession)
    EXPECT_EQ(bus.pending_count(), 1u);
}

} // namespace
