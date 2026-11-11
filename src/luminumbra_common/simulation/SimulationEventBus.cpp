#include "SimulationEventBus.h"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace luminumbra::simulation {
namespace {

void mix_byte(std::uint64_t& hash, unsigned char value) {
    constexpr std::uint64_t fnv_prime = 1099511628211ull;
    hash ^= static_cast<std::uint64_t>(value);
    hash *= fnv_prime;
}

void mix_text(std::uint64_t& hash, const std::string& value) {
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        mix_byte(hash, byte);
    }
}

std::string event_key(const SimulationEvent& event) {
    std::ostringstream out;
    out << event.tick << '|' << event.lane << '|' << event.sequence << '|' << event.topic << '|'
        << event.payload;
    return out.str();
}

std::size_t quoted_size(const std::string& text) {
    std::size_t bytes = text.size() + 2;
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\' || byte == '\b' || byte == '\t' || byte == '\n' ||
            byte == '\f' || byte == '\r') {
            ++bytes;
        } else if (byte < 0x20) {
            bytes += 5;
        }
    }
    return bytes;
}

} // namespace

std::string SimulationEvent::canonical_bytes() const {
    return "{\"tick\":" + std::to_string(tick) + ",\"lane\":" + std::to_string(lane) +
           ",\"sequence\":" + std::to_string(sequence) +
           ",\"topic\":" + nlohmann::json(topic).dump() +
           ",\"payload\":" + nlohmann::json(payload).dump() + '}';
}

std::size_t SimulationEvent::canonical_size() const {
    // Count escaping without parsing payload text: event delivery also permits opaque bytes.
    constexpr std::string_view kFields =
        "{\"tick\":,\"lane\":,\"sequence\":,\"topic\":,\"payload\":}";
    return kFields.size() + std::to_string(tick).size() + std::to_string(lane).size() +
           std::to_string(sequence).size() + quoted_size(topic) + quoted_size(payload);
}

std::uint64_t OrderedEventBus::publish(std::uint64_t tick,
                                       std::string topic,
                                       std::string payload,
                                       std::int32_t lane) {
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Simulation event sequence exhausted");
    const std::uint64_t sequence = next_sequence_++;
    pending_events_.push_back({SimulationEvent{
                                   tick,
                                   lane,
                                   sequence,
                                   std::move(topic),
                                   std::move(payload),
                               },
                               0,
                               std::nullopt});
    const auto& event = pending_events_.back().event;
    pending_bytes_[event.topic] += event.canonical_size() + 1;
    return sequence;
}

std::vector<SimulationEvent>
OrderedEventBus::drain(std::uint64_t inclusive_tick,
                       const std::function<bool()>& continue_delivery) {
    if (continue_delivery) {
        return drain(inclusive_tick, [&continue_delivery] { return continue_delivery(); });
    }
    return drain(inclusive_tick, [] { return true; });
}

void OrderedEventBus::subscribe(Handler handler) {
    handlers_.push_back(std::move(handler));
}

void OrderedEventBus::clear() {
    pending_events_.clear();
    pending_batch_size_ = 0;
    pending_bytes_.clear();
    handlers_.clear();
    next_sequence_ = 0;
}

std::size_t OrderedEventBus::pending_count() const noexcept {
    return pending_events_.size();
}

std::size_t OrderedEventBus::pending_bytes(std::string_view topic) const {
    const auto found = pending_bytes_.find(topic);
    return found == pending_bytes_.end() ? 0 : found->second;
}

std::vector<SimulationEvent> OrderedEventBus::pending_events() const {
    std::vector<SimulationEvent> events;
    for (const auto& pending : pending_events_)
        events.push_back(pending.event);
    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return a.sequence < b.sequence;
    });
    return events;
}

void OrderedEventBus::restore_pending(std::vector<SimulationEvent> events,
                                      std::uint64_t next_sequence) {
    pending_events_.clear();
    pending_batch_size_ = 0;
    pending_bytes_.clear();
    for (auto& event : events) {
        pending_bytes_[event.topic] += event.canonical_size() + 1;
        pending_events_.push_back({std::move(event), 0, std::nullopt});
    }
    next_sequence_ = next_sequence;
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
