#pragma once

//  E5: read-only EVOLUTION TELEMETRY. Computes the mean sensory traits over the live
// creature roster (split by role) so trait DIVERGENCE under selection is observable across
// generations — predator vision cones narrowing, prey cones widening, ranges drifting. This is a
// pure READ: it never mutates state and never feeds world_hash, so it is safe to call from any tick
// or tool without re-pin concerns.

#include <cstdint>

#include <entt/entt.hpp>

#include "../components/CreatureComponents.h"

namespace luminumbra::ai {

struct SensoryMeans {
    std::uint32_t count = 0;
    double vision_cos_half_fov =
        0.0; // higher = NARROWER cone (focused predator); lower = wider (prey)
    double vision_range = 0.0;
    double hearing_range = 0.0;
};

// Mean sensory genes over creatures whose is_predator matches `predators`. Pure function of
// registry state; id order is irrelevant to a mean, so no sort is needed.
[[nodiscard]] inline SensoryMeans ComputeSensoryMeans(const entt::registry& reg, bool predators) {
    namespace Comp = ::Luminumbra::Components;
    SensoryMeans m;
    auto view = reg.view<const Comp::CreatureGenomeComponent, const Comp::CreatureComponent>();
    for (auto e : view) {
        if (view.get<const Comp::CreatureComponent>(e).is_predator != predators)
            continue;
        const auto& gn = view.get<const Comp::CreatureGenomeComponent>(e);
        m.vision_cos_half_fov += static_cast<double>(gn.vision_cos_half_fov);
        m.vision_range += static_cast<double>(gn.vision_range);
        m.hearing_range += static_cast<double>(gn.hearing_range);
        ++m.count;
    }
    if (m.count > 0) {
        const double inv = 1.0 / static_cast<double>(m.count);
        m.vision_cos_half_fov *= inv;
        m.vision_range *= inv;
        m.hearing_range *= inv;
    }
    return m;
}

} // namespace luminumbra::ai
