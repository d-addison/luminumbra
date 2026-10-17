#include "SimulationPendingState.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace Luminumbra::Persistence {
namespace {
using Event = luminumbra::simulation::SimulationEvent;
using Deposit = luminumbra::fields::EnergyFieldState::PendingDeposit;
using Energy = luminumbra::fields::EnergyFieldState;
using State = SimulationPendingState;
using EventLimits = luminumbra::simulation::PendingEventLimits;
// Even with maximum event framing and deposits, admission's text/count caps fit
// the member ceiling. No additional byte-budget refusal can arise during save.
static_assert(128 + State::kMaxEvents * 45 + State::kMaxTextBytes +
                  Energy::kMaxRegionalDeposits * 24 <=
              State::kMaxFileBytes);
std::atomic<std::uint64_t> DecodeCount{0};
auto EventKey(const Event& event) {
    return std::tuple{event.tick, event.lane, event.sequence};
}
auto DepositKey(const Deposit& deposit) {
    return std::tuple{deposit.cell_key, deposit.channel, deposit.emitter_id, deposit.amount};
}
void Require(bool valid) {
    if (!valid)
        throw std::runtime_error("Invalid simulation pending state");
}
void Append(std::string& bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
}
void Text(std::string& bytes, const std::string& value) {
    Append(bytes, value.size(), 4);
    bytes.append(value);
}
std::uint64_t Checksum(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}
std::string EventBytes(const State& state) {
    Require(state.events.events.size() <= State::kMaxEvents);
    std::size_t count = 0, text = 0;
    std::set<std::uint64_t> sequences;
    std::set<luminumbra::simulation::ActivationDeliveryKey> activations;
    for (const auto& event : state.events.events) {
        EventLimits::admit(
            event.tick, event.topic.size(), event.payload.size(), event.activation, count, text);
        Require(event.sequence < state.events.next_sequence &&
                sequences.insert(event.sequence).second);
        if (event.activation) {
            const auto& key = *event.activation;
            Require(activations.insert(key).second);
        }
    }
    auto events = state.events.events;
    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return EventKey(a) < EventKey(b);
    });
    std::string bytes = "pending_events:v1:";
    Append(bytes, state.events.next_sequence, 8);
    Append(bytes, events.size(), 4);
    for (const auto& event : events) {
        Append(bytes, event.tick, 8);
        Append(bytes, static_cast<std::uint32_t>(event.lane), 4);
        Append(bytes, event.sequence, 8);
        Text(bytes, event.topic);
        Text(bytes, event.payload);
        Append(bytes, event.activation ? 1 : 0, 1);
        if (event.activation) {
            Append(bytes, event.activation->activation_tick, 8);
            Append(bytes, static_cast<std::uint32_t>(event.activation->x), 4);
            Append(bytes, static_cast<std::uint32_t>(event.activation->z), 4);
        }
    }
    return bytes;
}
std::string DepositBytes(const State& state) {
    Require(state.energy_channels >= 0 && state.energy_channels <= 64 &&
            state.deposits.size() <= Energy::kMaxRegionalDeposits);
    for (const auto& deposit : state.deposits)
        Require(Energy::ValidPendingDeposit(deposit, state.energy_channels));
    auto deposits = state.deposits;
    std::sort(deposits.begin(), deposits.end(), [](const auto& a, const auto& b) {
        return DepositKey(a) < DepositKey(b);
    });
    std::string bytes = "pending_deposits:v1:";
    Append(bytes, static_cast<std::uint32_t>(state.energy_channels), 4);
    Append(bytes, deposits.size(), 4);
    for (const auto& deposit : deposits) {
        Append(bytes, static_cast<std::uint64_t>(deposit.cell_key), 8);
        Append(bytes, static_cast<std::uint32_t>(deposit.channel), 4);
        Append(bytes, deposit.emitter_id, 8);
        Append(bytes, deposit.amount, 4);
    }
    return bytes;
}
class Reader {
public:
    explicit Reader(std::string_view bytes)
        : m_bytes(bytes) {}
    std::uint64_t integer(unsigned width) {
        Require(width <= m_bytes.size());
        std::uint64_t result = 0;
        for (unsigned i = 0; i < width; ++i)
            result |= std::uint64_t{static_cast<unsigned char>(m_bytes[i])} << (8 * i);
        m_bytes.remove_prefix(width);
        return result;
    }
    void tag(std::string_view expected) {
        Require(m_bytes.starts_with(expected));
        m_bytes.remove_prefix(expected.size());
    }
    std::string_view text(std::size_t& total) {
        const auto size = integer(4);
        Require(size <= State::kMaxTextBytes - total && size <= m_bytes.size());
        total += static_cast<std::size_t>(size);
        const auto result = m_bytes.substr(0, static_cast<std::size_t>(size));
        m_bytes.remove_prefix(static_cast<std::size_t>(size));
        return result;
    }
    [[nodiscard]] bool empty() const {
        return m_bytes.empty();
    }

private:
    std::string_view m_bytes;
};
// First pass validates all lengths/counts/order without allocating. Only the second pass
// materializes strings and vectors; the input is immutable across both passes.
void ReadBody(std::string_view bytes, State* output) {
    Reader reader(bytes);
    reader.tag("SPN1");
    Require(reader.integer(4) == 1);
    reader.tag("pending_events:v1:");
    const auto next = reader.integer(8);
    const auto count = reader.integer(4);
    Require(count <= State::kMaxEvents);
    if (output) {
        output->events.next_sequence = next;
        output->events.events.reserve(static_cast<std::size_t>(count));
    }
    std::size_t total = 0;
    std::size_t admitted_count = 0, admitted_text = 0;
    std::optional<std::tuple<std::uint64_t, std::int32_t, std::uint64_t>> previous;
    for (std::uint64_t i = 0; i < count; ++i) {
        Event event;
        event.tick = reader.integer(8);
        event.lane = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.integer(4)));
        event.sequence = reader.integer(8);
        Require(event.sequence < next && (!previous || *previous < EventKey(event)));
        previous = EventKey(event);
        const auto topic = reader.text(total);
        const auto payload = reader.text(total);
        const auto activation = reader.integer(1);
        Require(activation <= 1);
        if (activation != 0) {
            const auto tick = reader.integer(8);
            const auto x =
                std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.integer(4)));
            const auto z =
                std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.integer(4)));
            event.activation = luminumbra::simulation::ActivationDeliveryKey{tick, x, z};
        }
        EventLimits::admit(event.tick,
                           topic.size(),
                           payload.size(),
                           event.activation,
                           admitted_count,
                           admitted_text);
        if (output) {
            event.topic = topic;
            event.payload = payload;
            output->events.events.push_back(std::move(event));
        }
    }
    reader.tag("pending_deposits:v1:");
    const auto channels = reader.integer(4);
    const auto deposits = reader.integer(4);
    Require(channels <= 64 && deposits <= Energy::kMaxRegionalDeposits);
    if (output) {
        output->energy_channels = static_cast<int>(channels);
        output->deposits.reserve(static_cast<std::size_t>(deposits));
    }
    std::optional<Deposit> prior;
    for (std::uint64_t i = 0; i < deposits; ++i) {
        Deposit deposit;
        deposit.cell_key = std::bit_cast<std::int64_t>(reader.integer(8));
        const auto channel = reader.integer(4);
        Require(channel < channels);
        deposit.channel = static_cast<int>(channel);
        deposit.emitter_id = reader.integer(8);
        deposit.amount = static_cast<std::uint32_t>(reader.integer(4));
        Require(Energy::ValidPendingDeposit(deposit, static_cast<int>(channels)) &&
                (!prior || DepositKey(*prior) <= DepositKey(deposit)));
        prior = deposit;
        if (output)
            output->deposits.push_back(deposit);
    }
    Require(reader.empty());
}
} // namespace

std::string SimulationPendingState::event_bytes() const {
    return EventBytes(*this);
}
std::string SimulationPendingState::deposit_bytes() const {
    return DepositBytes(*this);
}
std::string SimulationPendingState::encode() const {
    std::string bytes = "SPN1";
    Append(bytes, 1, 4);
    bytes += event_bytes();
    bytes += deposit_bytes();
    Require(bytes.size() <= kMaxFileBytes - 8);
    Append(bytes, Checksum(bytes), 8);
    return bytes;
}
bool SimulationPendingState::decode(std::string_view bytes,
                                    SimulationPendingState& out,
                                    std::string& error) {
    DecodeCount.fetch_add(1, std::memory_order_relaxed);
    try {
        Require(bytes.size() >= 16 && bytes.size() <= kMaxFileBytes);
        const auto body = bytes.substr(0, bytes.size() - 8);
        Reader checksum(bytes.substr(bytes.size() - 8));
        Require(checksum.integer(8) == Checksum(body));
        ReadBody(body, nullptr);
        State candidate;
        ReadBody(body, &candidate);
        std::set<std::uint64_t> sequences;
        std::set<luminumbra::simulation::ActivationDeliveryKey> activations;
        for (const auto& event : candidate.events.events) {
            Require(sequences.insert(event.sequence).second);
            if (event.activation)
                Require(activations.insert(*event.activation).second);
        }
        out = std::move(candidate);
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}
std::uint64_t SimulationPendingState::decode_count_for_testing() {
    return DecodeCount.load(std::memory_order_relaxed);
}
namespace {
Event ActivationEvent(const world::PendingRegionActivation& activation) {
    const auto cause = activation.cause == world::ActivationCause::Edit  ? "edit"
                       : activation.cause == world::ActivationCause::Pin ? "pin"
                                                                         : "presence";
    return {activation.activation_tick,
            std::numeric_limits<std::int32_t>::min(),
            0,
            "region.activated",
            std::to_string(activation.key.x) + "," + std::to_string(activation.key.z) + "," + cause,
            luminumbra::simulation::ActivationDeliveryKey{
                activation.activation_tick, activation.key.x, activation.key.z}};
}
} // namespace
std::vector<Event> PendingActivationEvents(const world::ActiveRegionLedger& ledger) {
    std::vector<Event> events;
    for (const auto& [key, activation] : ledger.pending_activations()) {
        (void)key;
        if (events.size() == State::kMaxEvents)
            break;
        events.push_back(ActivationEvent(activation));
    }
    return events;
}
bool SimulationPendingState::validate_activations(const world::ActiveRegionLedger& ledger) const {
    for (const auto& event : events.events) {
        if (!event.activation)
            continue;
        const auto& key = *event.activation;
        const auto match = ledger.pending_activations().find({key.activation_tick, {key.x, key.z}});
        if (match == ledger.pending_activations().end())
            return false;
        auto wanted = ActivationEvent(match->second);
        wanted.sequence = event.sequence;
        if (wanted != event)
            return false;
    }
    return true;
}
} // namespace Luminumbra::Persistence
