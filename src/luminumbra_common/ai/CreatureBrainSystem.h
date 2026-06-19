#pragma once

// I9-ECO: the live creature tick — reads each creature's senses, decides via the Utility-AI
// brain (CreatureBrain.h -> IAUS), and MOVES the creature deterministically (flee = away from
// the nearest predator, hunt = toward the nearest prey, wander = golden-angle drift, graze/rest
// = recover). This is "wire Utility AI into the creature tick" in practice.
//
// DETERMINISM (this changes sim state -> world_hash): id-ordered traversal; senses use a
// PRE-tick position snapshot so the result is independent of move order; libm-free
// (DeterministicMath Sin/Cos/Sqrt only). A world with no CreatureComponent runs it as a no-op,
// so the canonical roster is byte-identical (the component is the opt-in, like PlantTag).

#include "CreatureBrain.h"

#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../core/DeterministicMath.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

struct CreatureBrainStats {
    int updated = 0;
};

// Advance every CreatureComponent by one fixed tick. Pure function of registry state + dt.
inline CreatureBrainStats RunCreatureBrainSystemOnTick(entt::registry& reg, float dt) {
    CreatureBrainStats stats;
    auto view = reg.view<Comp::CreatureComponent, Comp::TransformComponent>();

    std::vector<entt::entity> ents;
    for (auto e : view) ents.push_back(e);
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    // Pre-tick snapshot so all creatures sense the SAME state regardless of update order.
    struct Snap {
        entt::entity e;
        float x, z;
        bool predator;
    };
    std::vector<Snap> snap;
    snap.reserve(ents.size());
    for (auto e : ents) {
        const auto& tf = view.get<Comp::TransformComponent>(e);
        snap.push_back({e, tf.position.x, tf.position.z, view.get<Comp::CreatureComponent>(e).is_predator});
    }

    for (auto e : ents) {
        auto& tf = view.get<Comp::TransformComponent>(e);
        auto& cr = view.get<Comp::CreatureComponent>(e);
        const float sx = tf.position.x, sz = tf.position.z;

        // Nearest OPPOSITE-role creature: prey -> nearest predator (threat); predator -> prey (food).
        float bestDist = 1.0e9f, tx = sx, tz = sz;
        bool found = false;
        for (const Snap& o : snap) {
            if (o.e == e || o.predator == cr.is_predator) continue;
            const float dx = o.x - sx, dz = o.z - sz;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (d < bestDist) {
                bestDist = d;
                tx = o.x;
                tz = o.z;
                found = true;
            }
        }

        CreatureSenses s;
        s.is_predator = cr.is_predator;
        s.hunger = cr.hunger;
        s.stamina = cr.stamina;
        const float nearNorm = found ? (1.0f - utility_clamp01(bestDist / 30.0f)) : 0.0f;
        if (cr.is_predator) {
            s.food_proximity = nearNorm;
        } else {
            s.threat_proximity = nearNorm;
            s.food_proximity = 0.6f;  // prey graze ambient plants
        }

        const CreatureAction act = DecideCreatureAction(s);
        cr.last_action = static_cast<int>(act);

        float dirx = 0.0f, dirz = 0.0f;
        switch (act) {
            case CreatureAction::Flee:
                if (found) { dirx = sx - tx; dirz = sz - tz; }
                break;
            case CreatureAction::Hunt:
                if (found) { dirx = tx - sx; dirz = tz - sz; }
                break;
            case CreatureAction::Wander: {
                const float ang = 2.39996323f *
                    static_cast<float>(entt::to_integral(e) % 16u + 1u);  // deterministic per-id heading
                dirx = dm::Cos(ang);
                dirz = dm::Sin(ang);
                break;
            }
            case CreatureAction::Graze:
                cr.hunger = utility_clamp01(cr.hunger - 0.5f * dt);  // eating sates
                break;
            case CreatureAction::Rest:
                cr.stamina = utility_clamp01(cr.stamina + 0.3f * dt);  // recover
                break;
        }

        const float len = dm::Sqrt(dirx * dirx + dirz * dirz);
        if (len > 1.0e-5f) {
            const float inv = 1.0f / len;
            const float speed = (act == CreatureAction::Flee || act == CreatureAction::Hunt)
                                    ? cr.move_speed * 1.5f
                                    : cr.move_speed;
            tf.position.x += dirx * inv * speed * dt;
            tf.position.z += dirz * inv * speed * dt;
            cr.stamina = utility_clamp01(cr.stamina - 0.10f * dt);  // moving tires
        }
        cr.hunger = utility_clamp01(cr.hunger + 0.02f * dt);  // hunger grows
        ++stats.updated;
    }
    return stats;
}

}  // namespace luminumbra::ai
