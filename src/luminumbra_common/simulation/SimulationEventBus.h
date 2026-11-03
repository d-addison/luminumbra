#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace luminumbra::simulation {

struct SimulationEvent {
    std::uint64_t tick = 0;
    std::int32_t lane = 0;
    std::uint64_t sequence = 0;
    std::string topic;
    std::string payload;
};

struct EventBusReplayResult {
    bool passed = false;
    std::string checksum;
    std::vector<SimulationEvent> delivered_events;
};

class OrderedEventBus {
public:
    using Handler = std::function<void(const SimulationEvent&)>;

    std::uint64_t
    publish(std::uint64_t tick, std::string topic, std::string payload, std::int32_t lane = 0);
    // Aborted delivery retains per-event handler progress. A throwing handler counts as invoked;
    // retry resumes with the next handler, then proceeds through the remaining events in order.
    std::vector<SimulationEvent> drain(std::uint64_t inclusive_tick,
                                       const std::function<bool()>& continue_delivery = {});
    void subscribe(Handler handler);
    void clear();
    [[nodiscard]] std::size_t pending_count() const noexcept;
    [[nodiscard]] std::vector<SimulationEvent> pending_events() const;
    [[nodiscard]] std::uint64_t next_sequence() const noexcept {
        return next_sequence_;
    }
    // Replace queued events after loading; retain subscribers, reset delivery progress.
    void restore_pending(std::vector<SimulationEvent> events, std::uint64_t next_sequence);

private:
    struct PendingEvent {
        SimulationEvent event;
        // A handler that throws has been invoked and must not be invoked again on retry.
        std::size_t next_handler = 0;
    };
    std::uint64_t next_sequence_ = 0;
    std::vector<PendingEvent> pending_events_;
    std::deque<Handler> handlers_;
};

std::vector<SimulationEvent> make_eventbus_order_fixture();
EventBusReplayResult replay_eventbus_order_fixture();
std::string eventbus_order_checksum(const std::vector<SimulationEvent>& events);
std::vector<std::string> describe_event_order(const std::vector<SimulationEvent>& events);

} // namespace luminumbra::simulation
