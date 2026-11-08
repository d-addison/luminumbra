#pragma once

#include "../components/CoreComponents.h"
#include "../components/PlantComponents.h"
#include "../world/ActiveRegionLedger.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
    RegionConsumerGate() = default;
    RegionConsumerGate(const entt::registry& registry, std::span<const RegionKey> due)
        : m_registry(&registry)
        , m_due(due) {}
    RegionConsumerGate(const entt::registry& registry,
                       std::span<const RegionKey> due,
                       std::vector<std::uint64_t>& scratch)
        : m_registry(&registry)
        , m_due(due)
        , m_scratch(&scratch) {}

    [[nodiscard]] bool Allows(entt::entity entity) const {
        if (!m_registry) {
            return true;
        }
        if (const auto* owner =
                m_registry->try_get<const Components::PlantRegionComponent>(entity)) {
            return Contains({owner->owner_x, owner->owner_z});
        }
        const auto* transform = m_registry->try_get<const Components::TransformComponent>(entity);
        return transform && AllowsPosition(transform->position);
    }

    [[nodiscard]] bool AllowsPosition(const Vec3& position) const {
        return !m_registry || (ActiveRegionLedger::valid_anchor(position) &&
                               Contains(ActiveRegionLedger::region_at(position)));
    }

    // Observational work count only (membership builds plus lookups); never persisted or hashed.
    [[nodiscard]] std::size_t owner_visits() const {
        return m_ownerVisits;
    }

private:
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
    mutable std::vector<std::uint64_t> m_owned;
    mutable bool m_built = false;
    mutable std::size_t m_ownerVisits = 0;
};

} // namespace Luminumbra::world
