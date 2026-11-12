#pragma once

#include "components/SimulationIdentityComponent.h"
#include "world/ActiveRegionLedger.h"

#include <cstdint>
#include <optional>

namespace Luminumbra::simulation {

[[nodiscard]] inline bool IsValidSimulationId(std::uint64_t id) noexcept {
    return id != 0 && id < world::ActiveRegionLedger::kPersistentIdLimit;
}

// Candidate factory support; shipping factories select this in the activation change.
// Exhaustion returns nullopt; envelope capacity throws exactly as the shared allocator.
// Allocated IDs stay consumed even if the participant is subsequently destroyed.
[[nodiscard]] inline std::optional<Components::SimulationIdentityComponent>
AllocateSimulationIdentity(world::ActiveRegionLedger& ledger) {
    std::uint64_t id = 0;
    if (!ledger.allocate_persistent_id(id))
        return std::nullopt;
    return Components::SimulationIdentityComponent{.id = id};
}

} // namespace Luminumbra::simulation
