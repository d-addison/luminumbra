#pragma once

#include "luminumbra_common/systems/PhysicsSystem.h"

namespace Luminumbra::Systems {

// Test-only construction of the owner-thread continuation adapter. Production
// code reaches it only through GameSession, which validates owners first.
struct OwnerContinuationTestAccess {
    static PhysicsSystem::OwnerContinuation Make(PhysicsSystem& physics) {
        return PhysicsSystem::OwnerContinuation(physics);
    }
};

} // namespace Luminumbra::Systems
