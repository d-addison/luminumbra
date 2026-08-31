#pragma once

// PerceptionSystem — the live ECS read-side of the senses. For every
// perceiver (Transform + PerceptionComponent + AwarenessComponent) it fuses VISION
// (forward cone + range) and HEARING (audiogram) over the SensableComponent
// entities of OTHER factions, picks the strongest sensed target, and advances the
// per-agent awareness meter/state + last-known memory (see Awareness.h). Smell is
// handled field-wise by the ScentField/locomotion gradient path, not here.
//
// PURE-ENOUGH + DETERMINISTIC: perceivers and sensables are visited in entity-id
// order; float-only + DeterministicMath::Sqrt; no RNG/wall-clock. World_hash-
// neutral by construction — it only touches entities that carry these components,
// and the canonical headless determinism roster carries none (terrain/water only),
// so wiring it into the tick does not move world_hash.

#include <algorithm>
#include <vector>

#include "entt/entt.hpp"

#include "../components/CoreComponents.h"
#include "../components/InstinctComponents.h"
#include "../core/DeterministicMath.h"
#include "Awareness.h"
#include "Perception.h"

namespace luminumbra::ai {

struct PerceptionTickStats {
    std::uint64_t perceivers = 0; // agents with perception+awareness considered
    std::uint64_t sensing = 0;    // produced a non-zero fused signal this tick
    std::uint64_t engaged = 0;    // reached the Engaged state
};

inline PerceptionTickStats RunPerceptionSystemOnTick(entt::registry& registry, float dt) {
    using Luminumbra::Components::AwarenessComponent;
    using Luminumbra::Components::PerceptionComponent;
    using Luminumbra::Components::SensableComponent;
    using Luminumbra::Components::TransformComponent;

    PerceptionTickStats stats;

    // Snapshot sensables in id order (deterministic scan; independent of storage).
    struct Sensable {
        entt::entity id;
        float x;
        float z;
        float loudness;
        float pitch;
        std::uint32_t faction;
    };
    std::vector<Sensable> sensables;
    {
        auto view = registry.view<const TransformComponent, const SensableComponent>();
        for (auto e : view) {
            const auto& tf = view.get<const TransformComponent>(e);
            const auto& s = view.get<const SensableComponent>(e);
            sensables.push_back(
                {e, tf.position.x, tf.position.z, s.noise_loudness, s.noise_pitch, s.faction});
        }
        std::sort(sensables.begin(), sensables.end(), [](const Sensable& a, const Sensable& b) {
            return entt::to_integral(a.id) < entt::to_integral(b.id);
        });
    }

    std::vector<entt::entity> perceivers;
    {
        auto view =
            registry
                .view<const TransformComponent, const PerceptionComponent, AwarenessComponent>();
        for (auto e : view)
            perceivers.push_back(e);
        std::sort(perceivers.begin(), perceivers.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
    }

    for (auto e : perceivers) {
        ++stats.perceivers;
        const auto& tf = registry.get<const TransformComponent>(e);
        const auto& perc = registry.get<const PerceptionComponent>(e);
        auto& aw = registry.get<AwarenessComponent>(e);

        float best_signal = 0.0f;
        float bx = 0.0f;
        float bz = 0.0f;
        for (const auto& s : sensables) {
            if (s.id == e || s.faction == perc.faction)
                continue; // self / same faction ignored
            const float dx = s.x - tf.position.x;
            const float dz = s.z - tf.position.z;
            const float dist = Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);

            // Vision: in-cone clarity falls off with range.
            float vis = 0.0f;
            if (InVisionCone(tf.position.x,
                             tf.position.z,
                             perc.facing_x,
                             perc.facing_z,
                             perc.vision_cos_half_fov,
                             perc.vision_range,
                             s.x,
                             s.z)) {
                vis = 1.0f - dist / perc.vision_range;
                if (vis < 0.0f)
                    vis = 0.0f;
            }
            // Hearing: audiogram response, clamped to a [0,1] signal contribution.
            float hear = HeardLoudness(dist, s.loudness, s.pitch, perc.ear);
            if (hear > 1.0f)
                hear = 1.0f;

            const float sig = vis > hear ? vis : hear; // strongest sense wins
            if (sig > best_signal) {
                best_signal = sig;
                bx = s.x;
                bz = s.z;
            }
        }

        if (best_signal > 0.0f)
            ++stats.sensing;
        aw.awareness = UpdateAwareness(aw.awareness, best_signal, bx, bz, dt, aw.params);
        if (aw.awareness.state == AwarenessState::Engaged)
            ++stats.engaged;
    }

    return stats;
}

} // namespace luminumbra::ai
