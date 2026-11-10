#pragma once

#include "../components/CreatureComponents.h"
#include "../systems/RegionConsumerGate.h"

#include <cstddef>
#include <entt/entity/registry.hpp>
#include <entt/entity/storage.hpp>

namespace luminumbra::ai {

// Reused tick-local decisions for the creature roster, captured before any movement.
// Storage retains its pages across captures; lookup is constant time per participant.
// Entities born after capture inherit their admitted parents' tick. Other entity kinds
// retain their existing scheduling. This cache is neither saved nor hashed.
class CreatureTickAdmission {
public:
    void Capture(const entt::registry& registry,
                 const ::Luminumbra::world::RegionConsumerGate& gate) {
        m_entries.clear();
        for (const auto entity :
             registry.view<const ::Luminumbra::Components::CreatureComponent>()) {
            m_entries.emplace(entity, gate.Allows(entity));
        }
    }

    [[nodiscard]] bool Allows(entt::entity entity) const {
        return !m_entries.contains(entity) || m_entries.get(entity).allowed;
    }

    // Const diagnostic: number of creature owners visited by the latest capture.
    [[nodiscard]] std::size_t owner_visits() const {
        return m_entries.size();
    }

private:
    struct Entry {
        bool allowed;
    };
    entt::storage<Entry> m_entries;
};

} // namespace luminumbra::ai
