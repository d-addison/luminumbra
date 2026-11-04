#include "../../src/luminumbra_common/simulation/SimulationEventBus.h"

#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

bool pending_bytes_match(const luminumbra::simulation::OrderedEventBus& bus) {
    std::map<std::string, std::size_t> expected;
    for (const auto& event : bus.pending_events())
        expected[event.topic] += event.canonical_bytes().size() + 1;
    for (const auto& [topic, bytes] : expected) {
        if (bus.pending_bytes(topic) != bytes)
            return false;
    }
    return bus.pending_bytes("test") == expected["test"] &&
           bus.pending_bytes("ground.spawned") == expected["ground.spawned"] &&
           bus.pending_bytes("ground.despawned") == expected["ground.despawned"];
}

bool sequence_exhaustion_is_atomic() {
    using luminumbra::simulation::OrderedEventBus;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (const auto* topic : {"ground.spawned", "ground.despawned", "script.trigger"}) {
        OrderedEventBus bus;
        bus.restore_pending({}, maximum - 1);
        if (bus.publish(1, topic, "last") != maximum - 1 || bus.next_sequence() != maximum)
            return false;
        const auto pending = bus.pending_events().front().canonical_bytes();
        const auto bytes = bus.pending_bytes(topic);
        for (const bool drained : {false, true}) {
            if (drained)
                bus.drain(1);
            try {
                bus.publish(2, topic, "refused");
                return false;
            } catch (const std::overflow_error&) {}
            if (bus.next_sequence() != maximum || bus.pending_count() != (drained ? 0u : 1u) ||
                bus.pending_bytes(topic) != (drained ? 0u : bytes))
                return false;
            if (!drained && bus.pending_events().front().canonical_bytes() != pending)
                return false;
        }
    }
    return true;
}

bool subscriptions_during_delivery_start_with_later_events() {
    using luminumbra::simulation::OrderedEventBus;
    for (const bool interrupted : {false, true}) {
        OrderedEventBus bus;
        bool keep_delivering = true;
        std::vector<std::string> observed;
        bus.subscribe([&](const auto& event) {
            observed.push_back("first:" + event.payload);
            if (event.payload == "one") {
                bus.subscribe(
                    [&](const auto& later) { observed.push_back("new:" + later.payload); });
                // Continue using this handler's captures after the deque append.
                observed.push_back("subscribed:" + event.payload);
                keep_delivering = !interrupted;
            }
        });
        bus.subscribe([&](const auto& event) { observed.push_back("second:" + event.payload); });
        bus.publish(2, "test", "four");
        bus.publish(1, "test", "three");
        bus.publish(1, "test", "one", -1);
        bus.publish(1, "test", "two", -1);
        bus.drain(1, [&] { return keep_delivering; });
        if (interrupted) {
            if (observed != std::vector<std::string>{"first:one", "subscribed:one"})
                return false;
            keep_delivering = true;
            bus.drain(1);
        }
        const std::vector<std::string> expected{"first:one",
                                                "subscribed:one",
                                                "second:one",
                                                "first:two",
                                                "second:two",
                                                "new:two",
                                                "first:three",
                                                "second:three",
                                                "new:three"};
        if (observed != expected || bus.pending_count() != 1)
            return false;
        bus.drain(2);
        auto complete = expected;
        complete.insert(complete.end(), {"first:four", "second:four", "new:four"});
        if (observed != complete || bus.pending_count() != 0 || !pending_bytes_match(bus))
            return false;
    }
    return true;
}

bool callback_publications_follow_the_interrupted_batch() {
    using luminumbra::simulation::OrderedEventBus;
    std::vector<std::string> uninterrupted;
    for (const int mode : {0, 1, 2}) {
        OrderedEventBus bus;
        bool keep_delivering = true;
        std::vector<std::string> first, second, added, observed;
        bus.subscribe([&](const auto& event) {
            first.push_back(event.payload);
            observed.push_back("first:" + event.payload);
            if (event.payload == "original") {
                bus.publish(1, "test", "published", -1);
                bus.subscribe([&](const auto& later) {
                    added.push_back(later.payload);
                    observed.push_back("added:" + later.payload);
                });
                if (mode == 1)
                    throw std::runtime_error("consumer fault");
                keep_delivering = mode != 2;
            }
        });
        bus.subscribe([&](const auto& event) {
            second.push_back(event.payload);
            observed.push_back("second:" + event.payload);
        });
        bus.publish(1, "test", "original");
        bus.publish(1, "test", "batch tail", 1);
        bus.publish(2, "test", "future");
        bool caught = false;
        try {
            bus.drain(1, [&] { return keep_delivering; });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        if (caught != (mode == 1) || !pending_bytes_match(bus))
            return false;
        if (mode != 0) {
            if (!second.empty() || bus.pending_count() != 4)
                return false;
            // A lower retry horizon must not release the interrupted batch's ordering barrier.
            if (!bus.drain(0).empty() || !second.empty() || !pending_bytes_match(bus))
                return false;
        }
        keep_delivering = true;
        bus.drain(1);
        const std::vector<std::string> expected{"original", "batch tail", "published"};
        if (first != expected || second != expected ||
            added != std::vector<std::string>{"batch tail", "published"} ||
            bus.pending_count() != 1 || !pending_bytes_match(bus))
            return false;
        bus.drain(2);
        if (first.back() != "future" || second != first || added.back() != "future" ||
            bus.pending_count() != 0 || !pending_bytes_match(bus))
            return false;
        if (mode == 0)
            uninterrupted = observed;
        else if (observed != uninterrupted)
            return false;
    }
    return true;
}

} // namespace

int main() {
    if (!sequence_exhaustion_is_atomic())
        return fail("exhausted event sequence mutated pending state or wrapped the cursor");
    if (!subscriptions_during_delivery_start_with_later_events())
        return fail("subscription during delivery changed handler lifetime or event order");
    if (!callback_publications_follow_the_interrupted_batch())
        return fail("callback publication overtook an interrupted delivery batch");
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
        if (!pending_bytes_match(interrupted))
            return fail("aborted delivery lost pending byte reservations");
        keep_delivering = true;
        const auto retried = interrupted.drain(2, [&] { return keep_delivering; });
        const std::vector<std::string> expected{"one", "two", "three"};
        if (retried.size() != 3 || first != expected || middle != expected || last != expected ||
            interrupted.pending_count() != 1)
            return fail("delivery retry skipped or duplicated a handler");
        if (!pending_bytes_match(interrupted))
            return fail("delivery retry retained the wrong pending byte reservations");
        interrupted.drain(3);
        const std::vector<std::string> complete{"one", "two", "three", "future"};
        if (first != complete || middle != complete || last != complete ||
            interrupted.pending_count() != 0)
            return fail("future events did not survive aborted delivery");
        if (!pending_bytes_match(interrupted))
            return fail("completed delivery retained pending byte reservations");
    }
    OrderedEventBus restored;
    restored.restore_pending({{std::numeric_limits<std::uint64_t>::max(),
                               std::numeric_limits<std::int32_t>::min(),
                               std::numeric_limits<std::uint64_t>::max() - 1,
                               "ground.spawned",
                               std::string("\0\b\f\n\r\t\"\\", 8) + "\xc3\xa9"},
                              {4, 2, 2, "ground.despawned", "future"}},
                             std::numeric_limits<std::uint64_t>::max());
    if (!pending_bytes_match(restored))
        return fail("restored events lost pending byte reservations");
    restored.clear();
    if (!pending_bytes_match(restored))
        return fail("clearing events retained pending byte reservations");
    const std::string opaque(1, static_cast<char>(0xff));
    restored.publish(1, "test", opaque);
    const auto delivered = restored.drain(1);
    if (delivered.size() != 1 || delivered.front().payload != opaque)
        return fail("byte reservations changed opaque payload delivery");
    return 0;
}
