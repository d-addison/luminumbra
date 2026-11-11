#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace luminumbra::simulation {

struct SimulationEvent {
    std::uint64_t tick = 0;
    std::int32_t lane = 0;
    std::uint64_t sequence = 0;
    std::string topic;
    std::string payload;

    [[nodiscard]] std::string canonical_bytes() const;
    [[nodiscard]] std::size_t canonical_size() const;
};

struct EventBusReplayResult {
    bool passed = false;
    std::string checksum;
    std::vector<SimulationEvent> delivered_events;
};

class OrderedEventBus {
public:
    using Handler = std::function<void(const SimulationEvent&)>;

    // Throws std::overflow_error without mutation when the sequence cursor is exhausted.
    std::uint64_t
    publish(std::uint64_t tick, std::string topic, std::string payload, std::int32_t lane = 0);
    // Aborted delivery retains per-event handler progress. A throwing handler counts as invoked;
    // retry resumes with the next handler, then finishes the batch before sorting publications
    // from callbacks into the next batch.
    std::vector<SimulationEvent> drain(std::uint64_t inclusive_tick,
                                       const std::function<bool()>& continue_delivery = {});
    // A concrete continuation is checked before each handler without type erasure.
    // It must be callable; the nullable overload above retains empty-function semantics.
    template<typename ContinueDelivery>
        requires(std::is_class_v<ContinueDelivery> &&
                 !std::is_base_of_v<std::function<bool()>, ContinueDelivery> &&
                 std::is_invocable_r_v<bool, const ContinueDelivery&>)
    std::vector<SimulationEvent> drain(std::uint64_t inclusive_tick,
                                       const ContinueDelivery& continue_delivery) {
        std::vector<SimulationEvent> delivered;
        for (;;) {
            const bool resuming = pending_batch_size_ != 0;
            if (!resuming) {
                std::stable_sort(pending_events_.begin(),
                                 pending_events_.end(),
                                 [](const auto& a, const auto& b) {
                                     if (a.event.tick != b.event.tick) {
                                         return a.event.tick < b.event.tick;
                                     }
                                     if (a.event.lane != b.event.lane) {
                                         return a.event.lane < b.event.lane;
                                     }
                                     return a.event.sequence < b.event.sequence;
                                 });
            }

            std::vector<PendingEvent> due;
            auto first_future = pending_events_.begin();
            const auto batch_end = resuming ? pending_events_.begin() +
                                                  static_cast<std::ptrdiff_t>(pending_batch_size_)
                                            : pending_events_.end();
            while (first_future != batch_end && first_future->event.tick <= inclusive_tick) {
                const auto& event = first_future->event;
                const auto bytes = pending_bytes_.find(event.topic);
                bytes->second -= event.canonical_size() + 1;
                if (bytes->second == 0)
                    pending_bytes_.erase(bytes);
                due.push_back(std::move(*first_future));
                ++first_future;
            }
            pending_events_.erase(pending_events_.begin(), first_future);
            if (resuming)
                pending_batch_size_ -= due.size();

            std::size_t current = 0;
            const auto requeue = [&] {
                for (std::size_t i = current; i < due.size(); ++i) {
                    const auto& event = due[i].event;
                    pending_bytes_[event.topic] += event.canonical_size() + 1;
                }
                // Preserve this batch's order and handler cutoffs before forming the next batch.
                pending_events_.insert(
                    pending_events_.begin(),
                    std::make_move_iterator(due.begin() + static_cast<std::ptrdiff_t>(current)),
                    std::make_move_iterator(due.end()));
                pending_batch_size_ += due.size() - current;
            };
            try {
                for (; current < due.size(); ++current) {
                    auto& pending = due[current];
                    if (!pending.handler_count)
                        pending.handler_count = handlers_.size();
                    while (pending.next_handler < pending.handler_count.value_or(0)) {
                        if (!continue_delivery()) {
                            requeue();
                            return delivered;
                        }
                        // std::deque appends keep references to existing handler elements valid.
                        const Handler& handler = handlers_[pending.next_handler];
                        ++pending.next_handler;
                        handler(pending.event);
                    }
                    delivered.push_back(pending.event);
                }
            } catch (...) {
                requeue();
                throw;
            }

            if (!resuming || pending_batch_size_ != 0)
                return delivered;
        }
    }

    // A subscription added during delivery starts with the next event, in subscription order.
    void subscribe(Handler handler);
    void clear();
    [[nodiscard]] std::size_t pending_count() const noexcept;
    [[nodiscard]] std::vector<SimulationEvent> pending_events() const;
    // Canonical JSON event bytes plus one array separator per queued event of this topic.
    [[nodiscard]] std::size_t pending_bytes(std::string_view topic) const;
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
        std::optional<std::size_t> handler_count;
    };
    std::uint64_t next_sequence_ = 0;
    std::vector<PendingEvent> pending_events_;
    // An interrupted batch stays at the front, ahead of callback publications.
    std::size_t pending_batch_size_ = 0;
    std::map<std::string, std::size_t, std::less<>> pending_bytes_;
    std::deque<Handler> handlers_;
};

std::vector<SimulationEvent> make_eventbus_order_fixture();
EventBusReplayResult replay_eventbus_order_fixture();
std::string eventbus_order_checksum(const std::vector<SimulationEvent>& events);
std::vector<std::string> describe_event_order(const std::vector<SimulationEvent>& events);

} // namespace luminumbra::simulation
