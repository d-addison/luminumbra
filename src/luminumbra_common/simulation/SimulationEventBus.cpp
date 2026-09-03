#include "SimulationEventBus.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace luminumbra::simulation {
namespace {

bool event_order_less(const SimulationEvent& lhs, const SimulationEvent& rhs) {
    if (lhs.tick != rhs.tick) {
        return lhs.tick < rhs.tick;
    }
    if (lhs.lane != rhs.lane) {
        return lhs.lane < rhs.lane;
    }
    return lhs.sequence < rhs.sequence;
}

void mix_byte(std::uint64_t& hash, unsigned char value) {
    constexpr std::uint64_t fnv_prime = 1099511628211ull;
    hash ^= static_cast<std::uint64_t>(value);
    hash *= fnv_prime;
}

void mix_text(std::uint64_t& hash, const std::string& value) {
    for (const unsigned char byte : value) {
        mix_byte(hash, byte);
    }
}

std::string event_key(const SimulationEvent& event) {
    std::ostringstream out;
    out << event.tick << '|' << event.lane << '|' << event.sequence << '|' << event.topic << '|'
        << event.payload;
    return out.str();
}

} // namespace

std::uint64_t OrderedEventBus::publish(std::uint64_t tick,
                                       std::string topic,
                                       std::string payload,
                                       std::int32_t lane) {
    const std::uint64_t sequence = next_sequence_++;
    pending_events_.push_back(SimulationEvent{
        tick,
        lane,
        sequence,
        std::move(topic),
        std::move(payload),
    });
    return sequence;
}

std::vector<SimulationEvent> OrderedEventBus::drain(std::uint64_t inclusive_tick) {
    std::stable_sort(pending_events_.begin(), pending_events_.end(), event_order_less);

    std::vector<SimulationEvent> delivered;
    auto first_future = pending_events_.begin();
    while (first_future != pending_events_.end() && first_future->tick <= inclusive_tick) {
        delivered.push_back(*first_future);
        ++first_future;
    }
    pending_events_.erase(pending_events_.begin(), first_future);

    for (const SimulationEvent& event : delivered) {
        for (const Handler& handler : handlers_) {
            handler(event);
        }
    }

    return delivered;
}

void OrderedEventBus::subscribe(Handler handler) {
    handlers_.push_back(std::move(handler));
}

void OrderedEventBus::clear() {
    pending_events_.clear();
    handlers_.clear();
    next_sequence_ = 0;
}

std::size_t OrderedEventBus::pending_count() const noexcept {
    return pending_events_.size();
}

std::vector<SimulationEvent> make_eventbus_order_fixture() {
    OrderedEventBus bus;
    bus.publish(2, "ai.intent", "npc-1:turn");
    bus.publish(1, "input.command", "player:move");
    bus.publish(1, "script.trigger", "door:open");
    bus.publish(1, "physics.impulse", "crate:push", -1);
    bus.publish(3, "audio.event", "stone:slide");

    std::vector<SimulationEvent> delivered = bus.drain(1);
    bus.publish(2, "script.trigger", "torch:light");
    bus.publish(2, "ai.intent", "npc-2:wait", -1);

    std::vector<SimulationEvent> remaining = bus.drain(3);
    delivered.insert(delivered.end(), remaining.begin(), remaining.end());
    return delivered;
}

EventBusReplayResult replay_eventbus_order_fixture() {
    static const std::vector<std::string> expected_order = {
        "1|-1|3|physics.impulse|crate:push",
        "1|0|1|input.command|player:move",
        "1|0|2|script.trigger|door:open",
        "2|-1|6|ai.intent|npc-2:wait",
        "2|0|0|ai.intent|npc-1:turn",
        "2|0|5|script.trigger|torch:light",
        "3|0|4|audio.event|stone:slide",
    };

    EventBusReplayResult result;
    result.delivered_events = make_eventbus_order_fixture();
    result.checksum = eventbus_order_checksum(result.delivered_events);
    result.passed =
        describe_event_order(result.delivered_events) == expected_order && !result.checksum.empty();
    return result;
}

std::string eventbus_order_checksum(const std::vector<SimulationEvent>& events) {
    std::uint64_t hash = 14695981039346656037ull;
    bool first = true;
    for (const SimulationEvent& event : events) {
        if (!first) {
            mix_byte(hash, '\n');
        }
        first = false;
        mix_text(hash, event_key(event));
    }

    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

std::vector<std::string> describe_event_order(const std::vector<SimulationEvent>& events) {
    std::vector<std::string> order;
    order.reserve(events.size());
    for (const SimulationEvent& event : events) {
        order.push_back(event_key(event));
    }
    return order;
}

} // namespace luminumbra::simulation
