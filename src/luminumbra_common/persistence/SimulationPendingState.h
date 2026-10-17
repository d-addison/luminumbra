#pragma once

#include "fields/EnergyFieldState.h"
#include "simulation/SimulationEventBus.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra::Persistence {

// Enabled simulation continuation. Activation deliveries remain jointly owned by the
// ledger outbox; subscriptions and runtime delivery guards are never serialized.
struct SimulationPendingState {
    static constexpr std::string_view kMember = "chunks/region/simulation-pending.spn";
    static constexpr std::size_t kMaxEvents =
        luminumbra::simulation::PendingEventLimits::kMaxEvents;
    static constexpr std::size_t kMaxTextBytes =
        luminumbra::simulation::PendingEventLimits::kMaxTextBytes;
    static constexpr std::size_t kMaxFileBytes = 64u * 1024u * 1024u;
    luminumbra::simulation::OrderedEventBus::Snapshot events;
    int energy_channels = 0;
    std::vector<luminumbra::fields::EnergyFieldState::PendingDeposit> deposits;

    // Canonical little-endian, length-framed SPN1, with a trailing FNV-1a checksum.
    // Invalid/over-cap in-memory state throws; a present empty file is invalid.
    [[nodiscard]] std::string encode() const;
    [[nodiscard]] std::string event_bytes() const;
    [[nodiscard]] std::string deposit_bytes() const;
    static bool decode(std::string_view bytes, SimulationPendingState& out, std::string& error);
    static std::uint64_t decode_count_for_testing();
    [[nodiscard]] bool validate_activations(const world::ActiveRegionLedger& ledger) const;
};

// Bounded prefix in durable-key order, shared with enabled delivery preparation.
// Remaining activations stay in the ledger outbox for later ticks.
[[nodiscard]] std::vector<luminumbra::simulation::SimulationEvent>
PendingActivationEvents(const world::ActiveRegionLedger& ledger);

} // namespace Luminumbra::Persistence
