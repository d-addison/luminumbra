#pragma once

#include "SimulationIdentity.h"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include <entt/entity/registry.hpp>

namespace Luminumbra::simulation {

// Candidate-only ordering. Admission requires unique, valid common identities;
// refuse broken invariants before any simulation state is changed. Eight byte passes
// bound ordering to O(participants), with scratch bounded by this participant list.
inline std::size_t OrderSimulationParticipants(const entt::registry& reg,
                                               std::vector<entt::entity>& entities) {
    using Identity = Components::SimulationIdentityComponent;
    std::size_t visits = 0;
    if (entities.empty())
        return visits;
    for (const auto entity : entities) {
        ++visits;
        const auto* identity = reg.try_get<Identity>(entity);
        if (!identity || !IsValidSimulationId(identity->id))
            throw std::logic_error("Missing or invalid simulation identity");
    }
    std::vector<entt::entity> scratch(entities.size());
    for (unsigned shift = 0; shift < 64; shift += 8) {
        std::array<std::size_t, 256> offsets{};
        for (const auto entity : entities) {
            ++visits;
            ++offsets[static_cast<std::size_t>((reg.get<Identity>(entity).id >> shift) & 255u)];
        }
        std::size_t offset = 0;
        for (auto& bucket : offsets) {
            const auto count = bucket;
            bucket = offset;
            offset += count; // sum is bounded by entities.size()
        }
        for (const auto entity : entities) {
            ++visits;
            scratch[offsets[static_cast<std::size_t>((reg.get<Identity>(entity).id >> shift) &
                                                     255u)]++] = entity;
        }
        entities.swap(scratch);
    }
    for (std::size_t index = 1; index < entities.size(); ++index) {
        ++visits;
        if (reg.get<Identity>(entities[index - 1]).id == reg.get<Identity>(entities[index]).id)
            throw std::logic_error("Duplicate simulation identity");
    }
    // One validation, sixteen radix visits and one uniqueness pass per participant.
    return visits;
}

} // namespace Luminumbra::simulation
