#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <tuple>
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
    bool operator==(const SimulationEvent&) const = default;
};

struct EventBusReplayResult {
    bool passed = false;
    std::string checksum;
    std::vector<SimulationEvent> delivered_events;
};

// Durable queue bounds shared by admission and the pending-state codec.
struct PendingEventLimits {
    static constexpr std::size_t kMaxEvents = 16384;
    static constexpr std::size_t kMaxTextBytes = 8u * 1024u * 1024u;
    [[nodiscard]] static bool has_capacity(std::size_t count,
                                           std::size_t text_bytes,
                                           std::size_t topic_bytes,
                                           std::size_t payload_bytes) noexcept;
    // Throws before changing either total when the next event cannot be persisted.
    static void admit(std::uint64_t tick,
                      std::size_t topic_bytes,
                      std::size_t payload_bytes,
                      const std::optional<ActivationDeliveryKey>& activation,
                      std::size_t& count,
                      std::size_t& text_bytes);
};

class OrderedEventBus {
    using EventKey = std::tuple<std::uint64_t, std::int32_t, std::uint64_t>;
    using EventQueue = std::map<EventKey, SimulationEvent>;
    using ActivationIndex = std::map<ActivationDeliveryKey, EventKey>;

public:
    using Handler = std::function<void(const SimulationEvent&)>;

    void set_durable(bool enabled);

    std::uint64_t
    publish(std::uint64_t tick, std::string topic, std::string payload, std::int32_t lane = 0);
    std::vector<SimulationEvent> drain(std::uint64_t inclusive_tick,
                                       const std::function<bool()>& continue_delivery = {});
    void subscribe(Handler handler);
    struct Snapshot {
        std::vector<SimulationEvent> events;
        std::uint64_t next_sequence = 0;
        std::size_t text_bytes = 0;
    };
    // A prepared change owns every allocation, including sequences. Publishing it cannot
    // invoke handlers or component signals. Prepare and commit are adjacent on the sim thread.
    class PreparedBatch {
        friend class OrderedEventBus;
        EventQueue insertions_;
        ActivationIndex activations_;
        std::vector<EventKey> removals_;
        std::vector<SimulationEvent> legacy_events_;
        std::uint64_t next_sequence_ = 0;
        std::size_t count_ = 0, text_bytes_ = 0, scanned_ = 0;
        bool replacement_ = false;

    public:
        PreparedBatch() = default;
    };
    // Durable activation reconciliation admits a bounded prefix. The caller retains
    // excess activations in its authoritative outbox until a later reconciliation.
    [[nodiscard]] PreparedBatch prepare_batch(std::span<const SimulationEvent> events,
                                              const std::function<void()>& preparation = {},
                                              bool reconcile_activations = false,
                                              bool preserve_sequences = false) const;
    void commit_batch(PreparedBatch&& batch) noexcept;
    // Full replacement is prepared only at adoption/restore boundaries, never each tick.
    [[nodiscard]] static PreparedBatch prepare_restore(Snapshot state);
    // Transfer an already validated candidate queue for atomic world adoption.
    [[nodiscard]] PreparedBatch release_pending() noexcept;
    // Owner-thread copy; subscriptions and runtime delivery guards are excluded.
    [[nodiscard]] Snapshot snapshot() const;
    // Event inspections in preparation and publication of the most recent commit.
    // Snapshot copies and explicit restore validation are outside the tick path.
    [[nodiscard]] std::size_t commit_events_scanned_for_testing() const noexcept;
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
    EventQueue durable_events_;
    ActivationIndex pending_activations_;
    std::vector<Handler> handlers_;
    bool draining_enabled_ = false;
    bool durable_ = false;
    std::size_t pending_text_bytes_ = 0;
    std::size_t commit_events_scanned_ = 0;
    [[nodiscard]] PreparedBatch prepare_legacy_batch(std::span<const SimulationEvent> events,
                                                     bool reconcile_activations,
                                                     bool preserve_sequences) const;
};

std::vector<SimulationEvent> make_eventbus_order_fixture();
EventBusReplayResult replay_eventbus_order_fixture();
std::string eventbus_order_checksum(const std::vector<SimulationEvent>& events);
std::vector<std::string> describe_event_order(const std::vector<SimulationEvent>& events);

} // namespace luminumbra::simulation
