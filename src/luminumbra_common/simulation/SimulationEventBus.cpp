#include "SimulationEventBus.h"
#include "world/WorldClock.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

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
    for (const char byte : value) {
        mix_byte(hash, static_cast<unsigned char>(byte));
    }
}

std::string event_key(const SimulationEvent& event) {
    std::ostringstream out;
    out << event.tick << '|' << event.lane << '|' << event.sequence << '|' << event.topic << '|'
        << event.payload;
    return out.str();
}

} // namespace

bool PendingEventLimits::has_capacity(std::size_t count,
                                      std::size_t text_bytes,
                                      std::size_t topic_bytes,
                                      std::size_t payload_bytes) noexcept {
    return count < kMaxEvents && text_bytes <= kMaxTextBytes &&
           topic_bytes <= kMaxTextBytes - text_bytes &&
           payload_bytes <= kMaxTextBytes - text_bytes - topic_bytes;
}

void PendingEventLimits::admit(std::uint64_t tick,
                               std::size_t topic_bytes,
                               std::size_t payload_bytes,
                               const std::optional<ActivationDeliveryKey>& activation,
                               std::size_t& count,
                               std::size_t& text_bytes) {
    if (!has_capacity(count, text_bytes, topic_bytes, payload_bytes))
        throw std::length_error("Durable event queue limit exceeded");
    if (tick >= Luminumbra::world::WorldClock::kTickLimit ||
        (activation &&
         (activation->activation_tick != tick || activation->x < -32768 || activation->x >= 32768 ||
          activation->z < -32768 || activation->z >= 32768)))
        throw std::invalid_argument("Invalid durable event tick or activation key");
    ++count;
    text_bytes += topic_bytes + payload_bytes;
}

void OrderedEventBus::set_durable(bool enabled) {
    if (durable_ == enabled)
        return;
    if (enabled) {
        auto batch = prepare_restore(snapshot());
        durable_ = true;
        commit_batch(std::move(batch));
        pending_events_.clear();
    } else {
        auto state = snapshot();
        std::sort(state.events.begin(), state.events.end(), [](const auto& a, const auto& b) {
            return a.sequence < b.sequence;
        });
        pending_events_ = std::move(state.events);
        durable_events_.clear();
        pending_activations_.clear();
        pending_text_bytes_ = 0;
        durable_ = false;
    }
}

std::uint64_t OrderedEventBus::publish(std::uint64_t tick,
                                       std::string topic,
                                       std::string payload,
                                       std::int32_t lane) {
    auto text_bytes = pending_text_bytes_;
    if (durable_) {
        auto count = durable_events_.size();
        PendingEventLimits::admit(tick, topic.size(), payload.size(), {}, count, text_bytes);
        if (next_sequence_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Event sequence space exhausted");
    }
    // Legacy publication keeps its original sequence-allocation behavior.
    const std::uint64_t sequence = durable_ ? next_sequence_ : next_sequence_++;
    SimulationEvent event{
        tick,
        lane,
        sequence,
        std::move(topic),
        std::move(payload),
    };
    if (durable_)
        durable_events_.emplace(EventKey{tick, lane, sequence}, std::move(event));
    else
        pending_events_.push_back(std::move(event));
    if (durable_) {
        ++next_sequence_;
        pending_text_bytes_ = text_bytes;
    }
    return sequence;
}

std::vector<SimulationEvent>
OrderedEventBus::drain(std::uint64_t inclusive_tick,
                       const std::function<bool()>& continue_delivery) {
    if (durable_) {
        std::vector<SimulationEvent> delivered;
        auto end = durable_events_.begin();
        while (end != durable_events_.end() && end->second.tick <= inclusive_tick) {
            delivered.push_back(end->second);
            ++end;
        }
        for (const auto& event : delivered) {
            pending_text_bytes_ -= event.topic.size() + event.payload.size();
            if (event.activation)
                pending_activations_.erase(*event.activation);
        }
        durable_events_.erase(durable_events_.begin(), end);
        for (const auto& event : delivered)
            for (const auto& handler : handlers_) {
                if (continue_delivery && !continue_delivery())
                    return delivered;
                handler(event);
            }
        return delivered;
    }
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
            if (continue_delivery && !continue_delivery())
                return delivered;
            handler(event);
        }
    }

    return delivered;
}

void OrderedEventBus::subscribe(Handler handler) {
    handlers_.push_back(std::move(handler));
}

OrderedEventBus::PreparedBatch
OrderedEventBus::prepare_legacy_batch(std::span<const SimulationEvent> events,
                                      bool reconcile_activations,
                                      bool preserve_sequences) const {
    PreparedBatch batch;
    batch.legacy_events_ = pending_events_;
    batch.next_sequence_ = next_sequence_;
    std::vector<SimulationEvent> restored_events;
    std::map<ActivationDeliveryKey, SimulationEvent> previous_activations;
    if (reconcile_activations) {
        // Reconstruct activations from the authoritative outbox, including payloads.
        // Ordinary queued events survive; live-only activation keys do not.
        for (const auto& queued : batch.legacy_events_)
            if (queued.activation)
                previous_activations.emplace(*queued.activation, queued);
        std::erase_if(batch.legacy_events_,
                      [](const auto& queued) { return queued.activation.has_value(); });
        restored_events.assign(events.begin(), events.end());
        std::stable_sort(
            restored_events.begin(), restored_events.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.activation < rhs.activation;
            });
        // Preserve exact admitted deliveries; reconstructed or changed activations get
        // fresh sequences in durable-key order.
        events = restored_events;
    }
    std::set<ActivationDeliveryKey> queued_keys;
    for (const auto& event : batch.legacy_events_) {
        if (event.activation)
            queued_keys.insert(*event.activation);
    }
    for (const auto& event : events) {
        if (event.activation && !queued_keys.insert(*event.activation).second)
            continue;
        if (preserve_sequences && event.activation) {
            const auto prior = previous_activations.find(*event.activation);
            if (prior != previous_activations.end()) {
                auto expected = event;
                expected.sequence = prior->second.sequence;
                if (expected == prior->second) {
                    batch.legacy_events_.push_back(prior->second);
                    continue;
                }
            }
        }
        if (batch.next_sequence_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Event sequence space exhausted");
        auto candidate = event;
        candidate.sequence = batch.next_sequence_++;
        batch.legacy_events_.push_back(std::move(candidate));
    }
    return batch;
}

OrderedEventBus::PreparedBatch
OrderedEventBus::prepare_batch(std::span<const SimulationEvent> events,
                               const std::function<void()>& preparation,
                               bool reconcile_activations,
                               bool preserve_sequences) const {
    if (!durable_) {
        auto batch = prepare_legacy_batch(events, reconcile_activations, preserve_sequences);
        if (preparation)
            preparation();
        return batch;
    }
    PreparedBatch batch;
    batch.next_sequence_ = next_sequence_;
    batch.count_ = durable_events_.size();
    batch.text_bytes_ = pending_text_bytes_;
    std::vector<SimulationEvent> restored;
    if (reconcile_activations) {
        // Only activation deliveries can be replaced. Ordinary future events are
        // neither copied nor revisited; their validated accounting carries forward.
        for (const auto& [activation, key] : pending_activations_) {
            (void)activation;
            const auto& event = durable_events_.at(key);
            batch.removals_.push_back(key);
            --batch.count_;
            batch.text_bytes_ -= event.topic.size() + event.payload.size();
            ++batch.scanned_;
        }
        restored.assign(events.begin(), events.end());
        std::stable_sort(restored.begin(), restored.end(), [](const auto& a, const auto& b) {
            return a.activation < b.activation;
        });
        events = restored;
    }
    for (const auto& event : events) {
        ++batch.scanned_;
        if (event.activation &&
            (batch.activations_.contains(*event.activation) ||
             (!reconcile_activations && pending_activations_.contains(*event.activation))))
            continue;
        // Overflow remains in the independently durable ledger outbox.
        if (reconcile_activations && event.activation &&
            !PendingEventLimits::has_capacity(
                batch.count_, batch.text_bytes_, event.topic.size(), event.payload.size()))
            break;
        PendingEventLimits::admit(event.tick,
                                  event.topic.size(),
                                  event.payload.size(),
                                  event.activation,
                                  batch.count_,
                                  batch.text_bytes_);
        auto candidate = event;
        bool preserved = false;
        if (preserve_sequences && reconcile_activations && event.activation) {
            const auto prior = pending_activations_.find(*event.activation);
            if (prior != pending_activations_.end()) {
                const auto& previous = durable_events_.at(prior->second);
                candidate.sequence = previous.sequence;
                preserved = candidate == previous;
            }
        }
        if (!preserved) {
            if (batch.next_sequence_ == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("Event sequence space exhausted");
            candidate.sequence = batch.next_sequence_++;
        }
        const EventKey key{candidate.tick, candidate.lane, candidate.sequence};
        if (candidate.activation)
            batch.activations_.emplace(*candidate.activation, key);
        batch.insertions_.emplace(key, std::move(candidate));
    }
    if (preparation)
        preparation();
    return batch;
}

OrderedEventBus::PreparedBatch OrderedEventBus::prepare_restore(Snapshot state) {
    PreparedBatch batch;
    batch.replacement_ = true;
    batch.next_sequence_ = state.next_sequence;
    std::set<std::uint64_t> sequences;
    for (auto& event : state.events) {
        ++batch.scanned_;
        PendingEventLimits::admit(event.tick,
                                  event.topic.size(),
                                  event.payload.size(),
                                  event.activation,
                                  batch.count_,
                                  batch.text_bytes_);
        if (event.sequence >= state.next_sequence || !sequences.insert(event.sequence).second)
            throw std::invalid_argument("Invalid restored event sequence");
        const EventKey key{event.tick, event.lane, event.sequence};
        if (event.activation && !batch.activations_.emplace(*event.activation, key).second)
            throw std::invalid_argument("Duplicate restored activation");
        batch.insertions_.emplace(key, std::move(event));
    }
    return batch;
}

OrderedEventBus::Snapshot OrderedEventBus::snapshot() const {
    if (!durable_)
        return {pending_events_, next_sequence_, 0};
    Snapshot result;
    result.next_sequence = next_sequence_;
    result.text_bytes = pending_text_bytes_;
    result.events.reserve(durable_events_.size());
    for (const auto& [key, event] : durable_events_) {
        (void)key;
        result.events.push_back(event);
    }
    return result;
}
OrderedEventBus::PreparedBatch OrderedEventBus::release_pending() noexcept {
    PreparedBatch batch;
    batch.replacement_ = true;
    batch.next_sequence_ = next_sequence_;
    batch.count_ = durable_events_.size();
    batch.text_bytes_ = pending_text_bytes_;
    batch.insertions_.swap(durable_events_);
    batch.activations_.swap(pending_activations_);
    next_sequence_ = 0;
    pending_text_bytes_ = 0;
    return batch;
}
void OrderedEventBus::commit_batch(PreparedBatch&& batch) noexcept {
    commit_events_scanned_ = batch.scanned_;
    if (!durable_) {
        pending_events_.swap(batch.legacy_events_);
    } else if (batch.replacement_) {
        durable_events_.swap(batch.insertions_);
        pending_activations_.swap(batch.activations_);
    } else {
        for (const auto& key : batch.removals_) {
            const auto found = durable_events_.find(key);
            if (found != durable_events_.end()) {
                ++commit_events_scanned_;
                if (found->second.activation)
                    pending_activations_.erase(*found->second.activation);
                durable_events_.erase(found);
            }
        }
        // Prepared nodes transfer without allocation or copying retained events.
        durable_events_.merge(batch.insertions_);
        pending_activations_.merge(batch.activations_);
    }
    next_sequence_ = batch.next_sequence_;
    pending_text_bytes_ = batch.text_bytes_;
}
std::size_t OrderedEventBus::commit_events_scanned_for_testing() const noexcept {
    return commit_events_scanned_;
}
std::vector<SimulationEvent>
OrderedEventBus::drain_enabled(std::uint64_t inclusive_tick,
                               const std::function<bool()>& continue_delivery,
                               const std::function<void(const ActivationDeliveryKey&)>& acknowledge,
                               const std::function<void(bool)>& activation_delivery) {
    if (draining_enabled_)
        throw std::logic_error("Recursive enabled event drain");
    struct DrainGuard {
        bool& draining;
        explicit DrainGuard(bool& value)
            : draining(value) {
            draining = true;
        }
        ~DrainGuard() {
            draining = false;
        }
    } guard(draining_enabled_);
    std::vector<SimulationEvent> delivered;
    std::vector<SimulationEvent> eligible;
    if (durable_) {
        for (const auto& [key, event] : durable_events_) {
            (void)key;
            if (event.tick > inclusive_tick)
                break;
            eligible.push_back(event);
        }
    } else {
        eligible = pending_events_;
        std::stable_sort(eligible.begin(), eligible.end(), event_order_less);
    }
    for (const auto& event : eligible) {
        if (event.tick > inclusive_tick || (continue_delivery && !continue_delivery()))
            break;
        // Handlers can relocate the live queue; the original event remains pending until ack.
        const auto handlers = handlers_;
        struct DeliveryGuard {
            const std::function<void(bool)>& boundary;
            bool active;
            DeliveryGuard(const std::function<void(bool)>& callback, bool enabled)
                : boundary(callback)
                , active(enabled && static_cast<bool>(callback)) {
                if (active)
                    boundary(true);
            }
            ~DeliveryGuard() {
                if (active)
                    boundary(false);
            }
        } delivery(activation_delivery, event.activation.has_value());
        for (const auto& handler : handlers) {
            if (continue_delivery && !continue_delivery())
                return delivered;
            handler(event);
        }
        if (continue_delivery && !continue_delivery())
            return delivered;
        if (event.activation && acknowledge)
            acknowledge(*event.activation);
        if (durable_) {
            const auto found = durable_events_.find({event.tick, event.lane, event.sequence});
            if (found != durable_events_.end()) {
                pending_text_bytes_ -= found->second.topic.size() + found->second.payload.size();
                if (found->second.activation)
                    pending_activations_.erase(*found->second.activation);
                durable_events_.erase(found);
            }
        } else {
            std::erase_if(pending_events_,
                          [&](const auto& queued) { return queued.sequence == event.sequence; });
        }
        delivered.push_back(event);
    }
    return delivered;
}

void OrderedEventBus::clear() {
    if (draining_enabled_)
        throw std::logic_error("Cannot clear an enabled delivery in progress");
    pending_events_.clear();
    durable_events_.clear();
    pending_activations_.clear();
    handlers_.clear();
    next_sequence_ = 0;
    pending_text_bytes_ = 0;
    commit_events_scanned_ = 0;
}

std::size_t OrderedEventBus::pending_count() const noexcept {
    return durable_ ? durable_events_.size() : pending_events_.size();
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
