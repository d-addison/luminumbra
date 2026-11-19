#pragma once

#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/systems/PhysicsSystem.h"

#include <entt/entt.hpp>

#include <cstddef>

namespace Luminumbra::Client {
struct CreatureBodyAttachmentCursor {
    // Next slot in the creature storage's deterministic packed order. Keep no
    // entity handle or iterator: deletion may swap a new entity into this slot.
    std::size_t next = 0;
};

// Client-only newborn attachment. Each call creates at most four bodies and
// attempts at most sixteen admissions by default. The attempt budget also caps
// visited slots, including attached creatures and creatures without transforms.
inline void AttachMissingCreatureBodies(Systems::PhysicsSystem* phys,
                                        entt::registry& reg,
                                        CreatureBodyAttachmentCursor& cursor,
                                        int maxPerFrame = 4,
                                        int maxAttemptsPerFrame = 16) {
    if (!phys || maxPerFrame <= 0 || maxAttemptsPerFrame <= 0) {
        return;
    }
    const auto& creatures = reg.storage<Components::CreatureComponent>();
    const auto count = creatures.size();
    // Shrinking or clearing the storage can remove the next slot. Start a new
    // pass in that case; a reused entity is simply the current occupant.
    if (cursor.next >= count) {
        cursor.next = 0;
    }
    int made = 0;
    int attempted = 0;
    for (std::size_t visited = 0;
         visited < count && visited < static_cast<std::size_t>(maxAttemptsPerFrame);
         ++visited) {
        if (made >= maxPerFrame || attempted >= maxAttemptsPerFrame) {
            break;
        }
        const auto entity = creatures.data()[cursor.next];
        ++cursor.next;
        if (cursor.next == count) {
            cursor.next = 0;
        }
        if (reg.all_of<Components::CreaturePhysicsComponent>(entity) ||
            !reg.all_of<Components::TransformComponent>(entity)) {
            continue;
        }
        const auto& transform = reg.get<Components::TransformComponent>(entity);
        ++attempted;
        const std::size_t idx = phys->create_avatar_character(transform.position);
        if (idx == Systems::PhysicsSystem::kInvalidAvatar) {
            continue; // A later candidate may fit; retry this one after wrapping.
        }
        reg.emplace<Components::CreaturePhysicsComponent>(entity, idx);
        ++made;
    }
}
} // namespace Luminumbra::Client
