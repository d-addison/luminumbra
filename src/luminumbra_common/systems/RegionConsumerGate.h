#pragma once

#include "../components/CoreComponents.h"
#include "../components/PlantComponents.h"
#include "../world/ActiveRegionLedger.h"
#include "RegionElapsedTicks.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <entt/entity/registry.hpp>

namespace Luminumbra::world {

// A default gate is the legacy full-rate path. An installed gate views the sealed schedule's
// due owners and builds a sorted membership vector on the first query only (never for an
// empty roster of participants). A caller-owned scratch vector is cleared and reused, so a
// steady-state tick allocates nothing. Sources and targets share one membership per tick.
class RegionConsumerGate {
public:
    enum class PlantIdentity {
        Legacy,
        Simulation
    };
    struct Admission {
        bool allowed;
        std::uint32_t elapsed_ticks;
    };
    RegionConsumerGate() = default;
    explicit RegionConsumerGate(PlantIdentity identity)
        : m_simulationIdentity(identity == PlantIdentity::Simulation) {}
    RegionConsumerGate(const entt::registry& registry,
                       std::span<const RegionKey> due,
                       const RegionElapsedTicks& elapsed = {},
                       std::optional<std::uint64_t> due_tick = {})
        : m_registry(&registry)
        , m_due(due)
        , m_elapsed(elapsed)
        , m_dueTick(due_tick) {}
    RegionConsumerGate(const entt::registry& registry,
                       std::span<const RegionKey> due,
                       std::vector<std::uint64_t>& scratch,
                       PlantIdentity identity = PlantIdentity::Legacy,
                       const RegionElapsedTicks& elapsed = {},
                       std::optional<std::uint64_t> due_tick = {})
        : m_registry(&registry)
        , m_due(due)
        , m_scratch(&scratch)
        , m_simulationIdentity(identity == PlantIdentity::Simulation)
        , m_elapsed(elapsed)
        , m_dueTick(due_tick) {}

    // Explicit candidate selection, independent of regional scheduling. Shipping
    // construction keeps the legacy identity/ordering path until activation.
    [[nodiscard]] bool UsesSimulationPlantIdentity() const {
        return m_simulationIdentity;
    }
    // Candidate callers retain the gate across ticks to reuse radix-order storage.
    [[nodiscard]] std::vector<entt::entity>& PlantIdentityScratch() const {
        return m_plantIdentityScratch;
    }

    // Installed gates select persistent IDs and tagged legacy IDs for simulation streams.
    [[nodiscard]] bool UsesPersistentPlantIdentity() const {
        return m_registry != nullptr;
    }

    [[nodiscard]] bool Allows(entt::entity entity) const {
        return AdmissionFor(entity).allowed;
    }

    // Elapsed windows are observational: a due owner with a zero window remains allowed
    // for legacy consumers. Elapsed consumers must explicitly query the tick count.
    [[nodiscard]] std::uint32_t ElapsedTicks(entt::entity entity) const {
        return AdmissionFor(entity).elapsed_ticks;
    }

    // Resolve membership and elapsed ownership together, once, before creature movement.
    [[nodiscard]] Admission AdmissionFor(entt::entity entity) const {
        if (!m_registry) {
            return {true, 1};
        }
        Admission result{false, 0};
        if (const auto* owner =
                m_registry->try_get<const Components::PlantRegionComponent>(entity)) {
            result = ForOwner({owner->owner_x, owner->owner_z});
        } else if (const auto* transform =
                       m_registry->try_get<const Components::TransformComponent>(entity)) {
            result = AtPosition(transform->position);
        }
        // Optional due-time binding clips plant windows to their birth. Later consumers
        // can see crop children created in this transaction, but never pre-birth history.
        // Gates without due-time binding retain their existing owner-window semantics.
        if (m_dueTick && result.elapsed_ticks != 0) {
            if (const auto* growth =
                    m_registry->try_get<const Components::PlantGrowthComponent>(entity)) {
                if (growth->planted_tick > *m_dueTick) {
                    result.elapsed_ticks = 0;
                } else {
                    const auto age = *m_dueTick - growth->planted_tick;
                    if (age < result.elapsed_ticks) {
                        result.elapsed_ticks = static_cast<std::uint32_t>(age) + 1u;
                    }
                }
            }
        }
        return result;
    }

    [[nodiscard]] bool AllowsPosition(const Vec3& position) const {
        return AtPosition(position).allowed;
    }

    [[nodiscard]] std::uint32_t ElapsedTicksAtPosition(const Vec3& position) const {
        return AtPosition(position).elapsed_ticks;
    }

    // Observational work count only (membership builds plus lookups); never persisted or hashed.
    [[nodiscard]] std::size_t owner_visits() const {
        return m_ownerVisits;
    }

private:
    [[nodiscard]] Admission AtPosition(const Vec3& position) const {
        if (!m_registry) {
            return {true, 1};
        }
        return ActiveRegionLedger::valid_anchor(position)
                   ? ForOwner(ActiveRegionLedger::region_at(position))
                   : Admission{false, 0};
    }
    [[nodiscard]] Admission ForOwner(RegionKey owner) const {
        const bool allowed = Contains(owner);
        return {allowed, allowed ? m_elapsed.ForOwner(owner) : 0};
    }
    [[nodiscard]] bool Contains(RegionKey key) const {
        std::vector<std::uint64_t>& members = Members();
        ++m_ownerVisits;
        return std::binary_search(members.begin(), members.end(), Pack(key));
    }
    [[nodiscard]] std::vector<std::uint64_t>& Members() const {
        std::vector<std::uint64_t>& members = m_scratch ? *m_scratch : m_owned;
        if (!m_built) {
            members.clear();
            members.reserve(m_due.size());
            for (const auto key : m_due) {
                ++m_ownerVisits;
                members.push_back(Pack(key));
            }
            std::sort(members.begin(), members.end());
            m_built = true;
        }
        return members;
    }
    static std::uint64_t Pack(RegionKey key) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.x)) << 32) |
               static_cast<std::uint32_t>(key.z);
    }
    const entt::registry* m_registry = nullptr;
    std::span<const RegionKey> m_due;
    std::vector<std::uint64_t>* m_scratch = nullptr;
    bool m_simulationIdentity = false;
    RegionElapsedTicks m_elapsed;
    std::optional<std::uint64_t> m_dueTick;
    mutable std::vector<entt::entity> m_plantIdentityScratch;
    mutable std::vector<std::uint64_t> m_owned;
    mutable bool m_built = false;
    mutable std::size_t m_ownerVisits = 0;
};

} // namespace Luminumbra::world
