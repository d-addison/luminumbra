#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace luminumbra::simulation {

struct ActivationDeliveryKey {
    std::uint64_t activation_tick = 0;
    std::int32_t x = 0, z = 0;
    auto operator<=>(const ActivationDeliveryKey&) const = default;
};

struct SimulationEvent {
    std::uint64_t tick = 0;
    std::int32_t lane = 0;
    std::uint64_t sequence = 0;
    std::string topic;
    std::string payload;
    std::optional<ActivationDeliveryKey> activation = std::nullopt;
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
    std::vector<SimulationEvent> drain(std::uint64_t inclusive_tick,
                                       const std::function<bool()>& continue_delivery = {});
    void subscribe(Handler handler);
    // A prepared queue owns every allocation, including sequences. Publishing it cannot
    // invoke handlers or component signals. Prepare and commit are adjacent on the sim thread.
    struct PreparedBatch {
        std::vector<SimulationEvent> events;
        std::uint64_t next_sequence = 0;
    };
    [[nodiscard]] PreparedBatch prepare_batch(std::span<const SimulationEvent> events,
                                              const std::function<void()>& preparation = {},
                                              bool reconcile_activations = false) const;
    void commit_batch(PreparedBatch&& batch) noexcept;
    std::vector<SimulationEvent>
    drain_enabled(std::uint64_t inclusive_tick,
                  const std::function<bool()>& continue_delivery,
                  const std::function<void(const ActivationDeliveryKey&)>& acknowledge,
                  const std::function<void(bool)>& activation_delivery = {});
    void clear();
    [[nodiscard]] std::size_t pending_count() const noexcept;

private:
    std::uint64_t next_sequence_ = 0;
    std::vector<SimulationEvent> pending_events_;
    std::vector<Handler> handlers_;
    bool draining_enabled_ = false;
};

std::vector<SimulationEvent> make_eventbus_order_fixture();
EventBusReplayResult replay_eventbus_order_fixture();
std::string eventbus_order_checksum(const std::vector<SimulationEvent>& events);
std::vector<std::string> describe_event_order(const std::vector<SimulationEvent>& events);

} // namespace luminumbra::simulation
