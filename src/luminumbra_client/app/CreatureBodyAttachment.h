#pragma once

#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/systems/PhysicsSystem.h"

#include <entt/entt.hpp>

namespace Luminumbra::Client {
// Attach newborn creatures in registry order. Success and admission work have
// separate bounds: by default four creations and sixteen admission attempts per call.
inline void AttachMissingCreatureBodies(Systems::PhysicsSystem* phys,
                                        entt::registry& reg,
                                        int maxPerFrame = 4,
                                        int maxAttemptsPerFrame = 16) {
    if (!phys) {
        return;
    }
    int made = 0;
    int attempted = 0;
    auto view =
        reg.view<const Components::CreatureComponent, const Components::TransformComponent>();
    for (auto entity : view) {
        if (made >= maxPerFrame || attempted >= maxAttemptsPerFrame) {
            break;
        }
        if (reg.all_of<Components::CreaturePhysicsComponent>(entity)) {
            continue;
        }
        const auto& transform = view.get<const Components::TransformComponent>(entity);
        ++attempted;
        const std::size_t idx = phys->create_avatar_character(transform.position);
        if (idx == Systems::PhysicsSystem::kInvalidAvatar) {
            continue; // Retry this candidate later; another horizon may fit now.
        }
        reg.emplace<Components::CreaturePhysicsComponent>(entity, idx);
        ++made;
    }
}
} // namespace Luminumbra::Client
