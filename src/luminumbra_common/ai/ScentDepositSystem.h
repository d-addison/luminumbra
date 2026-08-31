#pragma once

// ScentDepositSystem — the WRITE side of stigmergy. Each tick, every
// entity with a SensableComponent that emits scent (scent_channel >= 0,
// scent_deposit > 0) lays that amount into the ScentField at its cell. This is how
// prey leave a trackable trail, predators leave a scent prey can flee, and ant
// foragers lay food/home trails. Pair with ScentField::Step (diffuse + evaporate)
// and::Clamp each tick, and the read side (GradientSteer / ScentSteeringSystem).
//
// DETERMINISTIC: emitters visited in entity-id order; manual floor-to-cell (no
// libm); the field write is the solver's deterministic add_impulse. World_hash
// folds in via the ScentField sub-hash once the field is owned by the sim tick.

#include <algorithm>
#include <vector>

#include "entt/entt.hpp"

#include "../components/CoreComponents.h"
#include "../components/InstinctComponents.h"
#include "ScentField.h"
#include "ScentSteeringSystem.h" // WorldToCell

namespace luminumbra::ai {

struct ScentDepositStats {
    std::uint64_t emitters = 0; // entities that deposited this tick
};

inline ScentDepositStats RunScentDepositOnTick(
    entt::registry& registry, ScentField& field, float origin_x, float origin_z, float cell_size) {
    using Luminumbra::Components::SensableComponent;
    using Luminumbra::Components::TransformComponent;

    ScentDepositStats stats;
    if (cell_size <= 0.0f)
        return stats;

    std::vector<entt::entity> emitters;
    {
        auto view = registry.view<const TransformComponent, const SensableComponent>();
        for (auto e : view)
            emitters.push_back(e);
        std::sort(emitters.begin(), emitters.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
    }

    for (auto e : emitters) {
        const auto& s = registry.get<const SensableComponent>(e);
        if (s.scent_channel < 0 || s.scent_deposit <= 0.0f)
            continue;
        const auto& tf = registry.get<const TransformComponent>(e);
        const int cx = WorldToCell(tf.position.x, origin_x, cell_size);
        const int cz = WorldToCell(tf.position.z, origin_z, cell_size);
        field.Deposit(s.scent_channel, cx, cz, static_cast<double>(s.scent_deposit));
        ++stats.emitters;
    }
    return stats;
}

} // namespace luminumbra::ai
