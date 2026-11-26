#pragma once

#include "../components/CreatureComponents.h"
#include "../systems/RegionConsumerGate.h"

#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <entt/entity/storage.hpp>

namespace luminumbra::ai {

// Reused tick-local decisions for the creature roster, captured before any movement.
// Storage retains its pages across captures; lookup is constant time per participant.
// The elapsed value belongs to the pre-movement owner, just like boolean admission.
// Entities born after capture retain one current tick, never an owner's pre-birth window.
// Other entity kinds retain their existing scheduling. This cache is neither saved nor hashed.
class CreatureTickAdmission {
public:
    void Capture(const entt::registry& registry,
                 const ::Luminumbra::world::RegionConsumerGate& gate) {
        m_entries.clear();
        for (const auto entity :
             registry.view<const ::Luminumbra::Components::CreatureComponent>()) {
            const auto admission = gate.AdmissionFor(entity);
            m_entries.emplace(entity, admission.allowed, admission.elapsed_ticks);
        }
    }

    [[nodiscard]] bool Allows(entt::entity entity) const {
        return !m_entries.contains(entity) || m_entries.get(entity).allowed;
    }

    // Uncaptured newborns and non-creature opt-ins can consume only the current tick.
    // EnTT membership includes the generation, so a reused handle cannot inherit history.
    [[nodiscard]] std::uint32_t ElapsedTicks(entt::entity entity) const {
        return m_entries.contains(entity) ? m_entries.get(entity).elapsed_ticks : 1;
    }

    // Called after this tick's consumers, before a captured owner is destroyed.
    void Forget(entt::entity entity) {
        m_entries.remove(entity);
    }

    // Const diagnostic: number of creature owners visited by the latest capture.
    [[nodiscard]] std::size_t owner_visits() const {
        return m_entries.size();
    }

private:
    struct Entry {
        bool allowed;
        std::uint32_t elapsed_ticks;
    };
    entt::storage<Entry> m_entries;
};

// Optional elapsed opt-in for state assignments. With no elapsed gate, preserve the
// legacy boolean contract (including an allowed zero window). Captured creature owners
// survive movement; uncaptured newborns keep the cache's single current tick. Other
// opted-in entities resolve their own regional owner rather than the cache's fallback.
[[nodiscard]] inline bool
AllowsElapsedParticipant(const entt::registry& registry,
                         entt::entity entity,
                         const CreatureTickAdmission* admission,
                         const ::Luminumbra::world::RegionConsumerGate* elapsed_gate) {
    if (admission && !admission->Allows(entity)) {
        return false;
    }
    if (!elapsed_gate) {
        return true;
    }
    if (admission && registry.all_of<::Luminumbra::Components::CreatureComponent>(entity)) {
        return admission->ElapsedTicks(entity) != 0;
    }
    return elapsed_gate->ElapsedTicks(entity) != 0;
}

} // namespace luminumbra::ai
