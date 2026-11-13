#pragma once

#include <cstdint>

namespace Luminumbra::Components {

// One immutable identity per authoritative ECS participant, independent of its roles
// and runtime handle. Assign before admission; copy unchanged when staging the entity.
// Zero is unassigned. Admitted IDs are [1, 2^63); ground objects retain their own id field
// and consume the same ledger allocator. Creature continuity is session-only for now.
struct SimulationIdentityComponent {
    std::uint64_t id = 0;
};

} // namespace Luminumbra::Components
