#pragma once

// ScentSteeringSystem — turns the scent gradient into MOVEMENT. For
// each creature with a ScentSenseComponent (+ Transform + LocomotionIntent +
// LocomotionProfile) it samples the ScentField gradient at the creature's cell
// (Weber chemotaxis, ScentField::GradientSteer) and ADDS a bias to the locomotion
// wish — toward the source when tracking (sign +1, a predator hunting prey scent /
// an ant on a food trail) or away when fleeing (sign -1). Runs AFTER the seek/
// flocking locomotion so it biases the existing wish, then clamps to the move_speed
// budget. This is the behavioral payoff of the stigmergy substrate: creatures
// follow smells.
//
// DETERMINISTIC: creatures visited in entity-id order; float-only + manual
// floor-to-cell (no libm); the field read is pure. World_hash-neutral by
// construction (only entities carrying ScentSenseComponent are touched; the
// canonical headless roster carries none).

#include <algorithm>
#include <vector>

#include "entt/entt.hpp"

#include "../components/CoreComponents.h"
#include "../components/InstinctComponents.h"
#include "../core/DeterministicMath.h"
#include "ScentField.h"

namespace luminumbra::ai {

struct ScentSteeringStats {
    std::uint64_t considered = 0;
    std::uint64_t steered = 0; // produced a non-zero scent bias this tick
};

// Map a world XZ coordinate to a grid cell index (floor toward -inf, so negative
// coords map correctly — unlike a plain truncating cast).
inline int WorldToCell(float world, float origin, float cell_size) {
    const float v = (world - origin) / cell_size;
    int c = static_cast<int>(v);
    if (v < static_cast<float>(c))
        --c;
    return c;
}

inline ScentSteeringStats RunScentSteeringOnTick(entt::registry& registry,
                                                 const ScentField& field,
                                                 float origin_x,
                                                 float origin_z,
                                                 float cell_size) {
    using Luminumbra::Components::LocomotionIntentComponent;
    using Luminumbra::Components::LocomotionProfile;
    using Luminumbra::Components::ScentSenseComponent;
    using Luminumbra::Components::TransformComponent;

    ScentSteeringStats stats;
    if (cell_size <= 0.0f)
        return stats;

    std::vector<entt::entity> agents;
    {
        auto view = registry.view<const TransformComponent,
                                  const ScentSenseComponent,
                                  LocomotionIntentComponent,
                                  const LocomotionProfile>();
        for (auto e : view)
            agents.push_back(e);
        std::sort(agents.begin(), agents.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
    }

    for (auto e : agents) {
        ++stats.considered;
        const auto& sense = registry.get<const ScentSenseComponent>(e);
        if (sense.channel < 0)
            continue;
        const auto& tf = registry.get<const TransformComponent>(e);
        const auto& profile = registry.get<const LocomotionProfile>(e);
        auto& intent = registry.get<LocomotionIntentComponent>(e);

        const int cx = WorldToCell(tf.position.x, origin_x, cell_size);
        const int cz = WorldToCell(tf.position.z, origin_z, cell_size);
        float dx = 0.0f;
        float dz = 0.0f;
        const float conf = field.GradientSteer(
            sense.channel, cx, cz, sense.sign, sense.floor, sense.weber_k, dx, dz);
        if (conf <= 0.0f)
            continue;

        intent.wish_xz.x += dx * conf * sense.strength * profile.move_speed;
        intent.wish_xz.y += dz * conf * sense.strength * profile.move_speed;
        const float wmag = Luminumbra::DeterministicMath::Sqrt(intent.wish_xz.x * intent.wish_xz.x +
                                                               intent.wish_xz.y * intent.wish_xz.y);
        if (wmag > profile.move_speed && wmag > 0.0f) {
            const float s = profile.move_speed / wmag;
            intent.wish_xz.x *= s;
            intent.wish_xz.y *= s;
        }
        ++stats.steered;
    }
    return stats;
}

} // namespace luminumbra::ai
