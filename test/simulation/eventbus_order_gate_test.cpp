#include "../../src/luminumbra_common/simulation/SimulationEventBus.h"

#include <gtest/gtest.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int LegacyPublishAndDrainOrder() {
    using luminumbra::simulation::describe_event_order;
    using luminumbra::simulation::OrderedEventBus;
    using luminumbra::simulation::replay_eventbus_order_fixture;

    const std::vector<std::string> expected_order = {
        "1|-1|3|physics.impulse|crate:push",
        "1|0|1|input.command|player:move",
        "1|0|2|script.trigger|door:open",
        "2|-1|6|ai.intent|npc-2:wait",
        "2|0|0|ai.intent|npc-1:turn",
        "2|0|5|script.trigger|torch:light",
        "3|0|4|audio.event|stone:slide",
    };

    const auto replay = replay_eventbus_order_fixture();
    if (!replay.passed) {
        return fail("simulation event bus replay did not pass");
    }
    if (describe_event_order(replay.delivered_events) != expected_order) {
        return fail("simulation event bus replay order changed");
    }
    if (replay.checksum.rfind("fnv1a64:", 0) != 0) {
        return fail("simulation event bus replay checksum is missing");
    }

    OrderedEventBus bus;
    std::vector<std::string> observed_order;
    bus.subscribe([&observed_order](const auto& event) {
        observed_order.push_back(describe_event_order({event}).front());
    });
    bus.publish(2, "ai.intent", "npc-1:turn");
    bus.publish(1, "input.command", "player:move");
    bus.publish(1, "script.trigger", "door:open");
    bus.publish(1, "physics.impulse", "crate:push", -1);
    bus.publish(3, "audio.event", "stone:slide");
    const auto tick_one = bus.drain(1);
    if (tick_one.size() != 3 || bus.pending_count() != 2) {
        return fail("simulation event bus did not preserve future tick events");
    }
    bus.publish(2, "script.trigger", "torch:light");
    bus.publish(2, "ai.intent", "npc-2:wait", -1);
    const auto all_remaining = bus.drain(3);
    if (all_remaining.size() != 4 || bus.pending_count() != 0) {
        return fail("simulation event bus did not drain eligible events");
    }
    if (observed_order != expected_order) {
        return fail("simulation event bus subscribers observed a non-deterministic order");
    }

    return 0;
}

TEST(EventBusOrderGate, LegacyPublishAndDrainOrderRemainsUnchanged) {
    EXPECT_EQ(LegacyPublishAndDrainOrder(), 0);
}
namespace {
using namespace luminumbra::simulation;
SimulationEvent Activation(std::int32_t x = 0) {
    return {1,
            -1,
            0,
            "region.activated",
            std::to_string(x) + ",0,presence",
            ActivationDeliveryKey{1, x, 0}};
}
TEST(EventBusOrderGate, FiveFieldAggregateDefaultsToNoActivation) {
    const SimulationEvent event{1, 0, 0, "ordinary", "payload"};
    EXPECT_FALSE(event.activation.has_value());
}
TEST(EventBusOrderGate, ActivationBatchEnqueueIsAtomic) {
    OrderedEventBus bus;
    bus.publish(1, "existing", "");
    const std::vector events{Activation(), Activation(1)};
    EXPECT_THROW(static_cast<void>(bus.prepare_batch(
                     events, [] { throw std::runtime_error("Preparation failure"); })),
                 std::runtime_error);
    EXPECT_EQ(bus.pending_count(), 1u);
    EXPECT_EQ(bus.publish(1, "probe", ""), 1u);
    bus.commit_batch(bus.prepare_batch(events));
    bus.commit_batch(bus.prepare_batch(events));
    EXPECT_EQ(bus.pending_count(), 4u);
    const auto delivered = bus.drain_enabled(1, [] { return true; }, {});
    ASSERT_EQ(delivered.size(), 4u);
    EXPECT_EQ(delivered[0].sequence, 2u);
    EXPECT_EQ(delivered[1].sequence, 3u);
    EXPECT_EQ(delivered[0].activation, Activation().activation);
    EXPECT_EQ(delivered[1].activation, Activation(1).activation);
}
TEST(EventBusOrderGate, ActivationReconstructionReconcilesOnlyDurableKeys) {
    OrderedEventBus bus;
    bus.publish(1, "ordinary", "retained");
    const std::vector before{Activation(), Activation(1)};
    bus.commit_batch(bus.prepare_batch(before));
    const std::vector restored{Activation(1), Activation(2)};
    bus.commit_batch(bus.prepare_batch(restored, {}, true));
    bus.commit_batch(bus.prepare_batch(restored, {}, true));
    ASSERT_EQ(bus.pending_count(), 3u);
    const auto delivered = bus.drain_enabled(1, [] { return true; }, {});
    ASSERT_EQ(delivered.size(), 3u);
    EXPECT_EQ(delivered[0].activation, Activation(1).activation);
    EXPECT_EQ(delivered[0].sequence, 5u);
    EXPECT_EQ(delivered[1].activation, Activation(2).activation);
    EXPECT_EQ(delivered[1].sequence, 6u);
    EXPECT_EQ(delivered[2].topic, "ordinary");
    EXPECT_EQ(delivered[2].sequence, 0u);
    EXPECT_EQ(bus.publish(2, "probe", ""), 7u);
}
TEST(EventBusOrderGate, ActivationReconstructionOrdersRestoredDataByDurableKey) {
    OrderedEventBus bus;
    const std::vector before{Activation(1)};
    bus.commit_batch(bus.prepare_batch(before));
    auto changed = Activation(1);
    changed.payload = "1,0,edit";
    // Deliberately supply descending keys: reconstruction establishes the order.
    const std::vector restored{changed, Activation()};
    bus.commit_batch(bus.prepare_batch(restored, {}, true));
    const auto delivered = bus.drain_enabled(1, [] { return true; }, {});
    ASSERT_EQ(delivered.size(), 2u);
    EXPECT_EQ(delivered[0].activation, Activation().activation);
    EXPECT_EQ(delivered[0].payload, "0,0,presence");
    EXPECT_EQ(delivered[0].sequence, 1u);
    EXPECT_EQ(delivered[1].activation, changed.activation);
    EXPECT_EQ(delivered[1].payload, "1,0,edit");
    EXPECT_EQ(delivered[1].sequence, 2u);
}
TEST(EventBusOrderGate, ActivationReconstructionFailurePreservesQueueAndSequences) {
    OrderedEventBus bus;
    bus.publish(1, "ordinary", "retained");
    const std::vector before{Activation(1)};
    bus.commit_batch(bus.prepare_batch(before));
    auto changed = Activation(1);
    changed.payload = "1,0,edit";
    const std::vector restored{Activation(), changed};
    EXPECT_THROW(static_cast<void>(bus.prepare_batch(
                     restored, [] { throw std::runtime_error("Reconstruction failure"); }, true)),
                 std::runtime_error);
    ASSERT_EQ(bus.pending_count(), 2u);
    EXPECT_EQ(bus.publish(2, "probe", ""), 2u);
    const auto delivered = bus.drain_enabled(1, [] { return true; }, {});
    ASSERT_EQ(delivered.size(), 2u);
    EXPECT_EQ(delivered[0].activation, before[0].activation);
    EXPECT_EQ(delivered[0].payload, "1,0,presence");
    EXPECT_EQ(delivered[0].sequence, 1u);
    EXPECT_EQ(delivered[1].topic, "ordinary");
    EXPECT_EQ(delivered[1].payload, "retained");
    EXPECT_EQ(delivered[1].sequence, 0u);
}
TEST(EventBusOrderGate, ActivationDeliveryAcknowledgesOnlyAfterSuccessfulHandlers) {
    OrderedEventBus bus;
    const std::vector events{Activation()};
    bus.commit_batch(bus.prepare_batch(events));
    unsigned handlers = 0, acknowledged = 0;
    bool fail_handler = true;
    bus.subscribe([&](const auto&) {
        ++handlers;
        EXPECT_EQ(acknowledged, 0u);
    });
    bus.subscribe([&](const auto&) {
        EXPECT_EQ(acknowledged, 0u);
        EXPECT_EQ(bus.pending_count(), 1u);
        if (fail_handler)
            throw std::runtime_error("Handler failure");
        ++handlers;
    });
    const auto acknowledge = [&](const auto& key) {
        EXPECT_EQ(handlers, 3u);
        EXPECT_EQ(key, (ActivationDeliveryKey{1, 0, 0}));
        ++acknowledged;
    };
    EXPECT_THROW(bus.drain_enabled(1, [] { return true; }, acknowledge), std::runtime_error);
    EXPECT_EQ(acknowledged, 0u);
    EXPECT_EQ(bus.pending_count(), 1u);
    fail_handler = false;
    bus.drain_enabled(1, [] { return true; }, acknowledge);
    EXPECT_EQ(acknowledged, 1u);
    EXPECT_EQ(bus.pending_count(), 0u);
    bus.drain_enabled(1, [] { return true; }, acknowledge);
    EXPECT_EQ(acknowledged, 1u);
}
TEST(EventBusOrderGate, InterruptedActivationDrainRetainsStableDeliveryKey) {
    OrderedEventBus bus;
    const std::vector events{Activation()};
    bus.commit_batch(bus.prepare_batch(events));
    bool keep_going = true;
    unsigned handlers = 0, acknowledged = 0;
    bus.subscribe([&](const auto& event) {
        EXPECT_EQ(event.activation, events[0].activation);
        ++handlers;
        keep_going = false;
    });
    bus.drain_enabled(1, [&] { return keep_going; }, [&](const auto&) { ++acknowledged; });
    EXPECT_EQ(bus.pending_count(), 1u);
    EXPECT_EQ(acknowledged, 0u);
    bus.commit_batch(bus.prepare_batch(events));
    EXPECT_EQ(bus.pending_count(), 1u);
    keep_going = true;
    bus.drain_enabled(1, [&] { return keep_going; }, [&](const auto&) { ++acknowledged; });
    EXPECT_EQ(handlers, 2u);
    EXPECT_EQ(acknowledged, 0u);
    EXPECT_EQ(bus.pending_count(), 1u);
}
} // namespace
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
