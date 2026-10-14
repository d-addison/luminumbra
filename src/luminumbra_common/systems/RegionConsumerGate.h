#pragma once
#include "../components/CreatureComponents.h"
#include "../components/PlantComponents.h"
#include <entt/entt.hpp>
#include <functional>

namespace Luminumbra::world {
// Empty is the exact legacy full-rate path. Enabled consumers share a pure
// admission predicate for source AND destination entities; never mutate a
// participant or use it as an interacting source when its owner is not due.
using RegionEntityWork = std::function<void(entt::entity)>;
using RegionEntityGate = std::function<bool(entt::entity)>;
inline std::uint64_t
RegionEntityIdentity(const entt::registry& registry, entt::entity e, bool enabled) {
    if (enabled) {
        if (const auto* owner = registry.try_get<const Components::PlantRegionComponent>(e))
            return owner->persistent_id;
        if (const auto* owner = registry.try_get<const Components::CreatureRegionComponent>(e))
            return owner->persistent_id;
    }
    return static_cast<std::uint64_t>(entt::to_integral(e));
}
inline bool AdmitRegionEntity(const RegionEntityGate& gate, entt::entity entity) {
    return !gate || gate(entity);
}
} // namespace Luminumbra::world
