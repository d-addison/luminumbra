#pragma once

#include <cstddef>
#include <cstdint>
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
    std::vector<SimulationEvent> drain(std::uint64_t inclusive_tick);
    void subscribe(Handler handler);
    void clear();
    [[nodiscard]] std::size_t pending_count() const noexcept;

private:
    std::uint64_t next_sequence_ = 0;
    std::vector<SimulationEvent> pending_events_;
    std::vector<Handler> handlers_;
};

std::vector<SimulationEvent> make_eventbus_order_fixture();
EventBusReplayResult replay_eventbus_order_fixture();
std::string eventbus_order_checksum(const std::vector<SimulationEvent>& events);
std::vector<std::string> describe_event_order(const std::vector<SimulationEvent>& events);

} // namespace luminumbra::simulation
