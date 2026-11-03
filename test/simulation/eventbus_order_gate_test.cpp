#include "../../src/luminumbra_common/simulation/SimulationEventBus.h"

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

int main() {
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

    for (const bool throws : {false, true}) {
        OrderedEventBus interrupted;
        bool keep_delivering = true;
        std::vector<std::string> first, middle, last;
        interrupted.subscribe([&](const auto& event) { first.push_back(event.payload); });
        interrupted.subscribe([&](const auto& event) {
            middle.push_back(event.payload);
            if (event.payload == "one") {
                interrupted.publish(2, "test", "three");
                keep_delivering = false;
                if (throws)
                    throw std::runtime_error("consumer fault");
            }
        });
        interrupted.subscribe([&](const auto& event) { last.push_back(event.payload); });
        interrupted.publish(1, "test", "one");
        interrupted.publish(1, "test", "two");
        interrupted.publish(3, "test", "future");
        bool caught = false;
        try {
            const auto partial = interrupted.drain(1, [&] { return keep_delivering; });
            if (!partial.empty())
                return fail("aborted delivery reported an incomplete event as delivered");
        } catch (const std::runtime_error&) {
            caught = true;
        }
        if (caught != throws || first != std::vector<std::string>{"one"} || middle != first ||
            !last.empty() || interrupted.pending_count() != 4)
            return fail("aborted delivery lost pending events or reached later handlers");
        keep_delivering = true;
        const auto retried = interrupted.drain(2, [&] { return keep_delivering; });
        const std::vector<std::string> expected{"one", "two", "three"};
        if (retried.size() != 3 || first != expected || middle != expected || last != expected ||
            interrupted.pending_count() != 1)
            return fail("delivery retry skipped or duplicated a handler");
        interrupted.drain(3);
        const std::vector<std::string> complete{"one", "two", "three", "future"};
        if (first != complete || middle != complete || last != complete ||
            interrupted.pending_count() != 0)
            return fail("future events did not survive aborted delivery");
    }
    return 0;
}
