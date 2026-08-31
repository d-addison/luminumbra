#pragma once

// sim.thirst: CREATURE THIRST /  (complements hunger).
//
// Each tick this system, for every creature carrying a ThirstComponent (the opt-in gate):
//   * RAISES its thirst by a fixed per-second rate (clamped to [0,1]); a carcass
//     (CreatureComponent.eaten) neither thirsts nor drinks,
//   * finds the NEAREST water hole — an entity carrying a WaterHoleComponent +
//     TransformComponent — and, if the creature is thirsty enough to bother, writes a
//     normalised direction toward it (scaled by thirst) into wish_x/wish_z so the
//     orchestrator can blend it into movement; with no water hole in the world the wish
//     stays zero (the creature still gets thirsty, it just has nowhere to go),
//   * if the creature is WITHIN that nearest hole's radius, sets drinking=1 and DROPS thirst
//     (clamped >= 0); otherwise drinking=0.
//
// DETERMINISM. id-ordered traversal (sort by entt::to_integral) so the per-creature work and
// any telemetry are order-independent (each creature only reads water-hole transforms, never
// another creature's this-tick update, so a single pass is already order-stable). Math is
// DeterministicMath only (Sqrt for distance; +-*/, clamped); NO rng is consumed — seed offset
// +33 is reserved for collision-avoidance only and never drawn. NO wall-clock, NO libm
// transcendentals. run==replay holds.
//
// GATING. A creature participates only if it carries a ThirstComponent. A world whose
// creatures carry none — and any world with no creatures — does nothing, so the canonical
// NetworkStateHash baseline stays byte-identical.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../components/ThirstComponents.h"
#include "../core/DeterministicMath.h"

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry:... herd-alarm+26, thirst+33). This system is
// deterministic and consumes NO rng; the offset is recorded for collision-avoidance only.
inline constexpr std::uint64_t kThirstSeedOffset = 33ull;

// Per-second rate at which an idle creature's thirst rises (full parch in ~1/kThirstRise s
// at the sim's fixed dt). Tuned slow enough to complement, not dominate, hunger.
inline constexpr float kThirstRise = 0.04f;

// Per-second rate at which thirst falls while drinking inside a water hole (drinks faster
// than it accrues so a creature at a hole quenches and then leaves).
inline constexpr float kThirstDrink = 0.5f;

// A creature only bothers walking toward water once it is at least this thirsty; below it the
// wish is zeroed (it grazes/wanders normally). Keeps creatures from constantly micro-steering.
inline constexpr float kThirstSeekThreshold = 0.3f;

struct ThirstStats {
    int participants = 0; // creatures carrying a ThirstComponent that took part
    int seeking = 0;      // creatures that wrote a non-zero water-seeking wish this tick
    int drinking = 0;     // creatures inside a water hole's radius this tick
};

// Full-control tuning (defaults == the k* constants -> byte-identical). From SystemConfig
// sim.thirst.
struct ThirstTuning {
    float rise_rate = kThirstRise;
    float drink_rate = kThirstDrink;
    float seek_threshold = kThirstSeekThreshold;
};

[[nodiscard]] inline float ThirstClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// RunThirstOnTick: raise thirst, steer thirsty creatures toward the nearest water hole, and
// let them drink when in range. id-ordered. `dt` scales the rise/drink rates so the behaviour
// is frame-rate independent (the sim runs a fixed dt; the parameter keeps the signature honest
// and lets tests exercise multiple ticks).
inline ThirstStats RunThirstOnTick(entt::registry& reg, float dt, const ThirstTuning& tuning = {}) {
    ThirstStats stats;

    // Snapshot every water hole once (deterministic, read-only this tick). id-ordered for a
    // stable nearest-tie resolution.
    struct Hole {
        float x, z;
        float radius;
    };
    std::vector<Hole> holes;
    {
        auto hview = reg.view<Comp::WaterHoleComponent, Comp::TransformComponent>();
        std::vector<entt::entity> hents(hview.begin(), hview.end());
        std::sort(hents.begin(), hents.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        holes.reserve(hents.size());
        for (auto e : hents) {
            const auto& wh = hview.get<Comp::WaterHoleComponent>(e);
            const auto& tf = hview.get<Comp::TransformComponent>(e);
            holes.push_back({tf.position.x, tf.position.z, wh.radius});
        }
    }

    auto view = reg.view<Comp::ThirstComponent, Comp::TransformComponent>();

    // id-ordered participant list (deterministic traversal).
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.

    // dt-scaled rates: the sim's fixed dt (~1/30s) gives the tuned per-second constants.
    const float rise = tuning.rise_rate * dt;
    const float drink = tuning.drink_rate * dt;

    for (auto e : ents) {
        auto& th = view.get<Comp::ThirstComponent>(e);
        const auto& tf = view.get<Comp::TransformComponent>(e);

        // A carcass neither thirsts nor drinks: hold it inert and skip.
        if (const auto* cr = reg.try_get<Comp::CreatureComponent>(e); cr && cr->eaten) {
            th.wish_x = 0.0f;
            th.wish_z = 0.0f;
            th.drinking = 0;
            ++stats.participants;
            continue;
        }

        ++stats.participants;

        // Thirst rises this tick (clamped).
        th.thirst = ThirstClamp01(th.thirst + rise);

        // Find the nearest water hole (by squared distance; id-order breaks ties since the
        // first hole at a given distance wins the strict-less comparison).
        bool have_nearest = false;
        float best_d2 = 0.0f;
        float best_dx = 0.0f, best_dz = 0.0f;
        float best_radius = 0.0f;
        for (const Hole& h : holes) {
            const float dx = h.x - tf.position.x;
            const float dz = h.z - tf.position.z;
            const float d2 = dx * dx + dz * dz;
            if (!have_nearest || d2 < best_d2) {
                have_nearest = true;
                best_d2 = d2;
                best_dx = dx;
                best_dz = dz;
                best_radius = h.radius;
            }
        }

        // Default: not steering, not drinking, no water known.
        th.wish_x = 0.0f;
        th.wish_z = 0.0f;
        th.drinking = 0;
        th.water_proximity = 0.0f;

        if (!have_nearest) {
            // No water in the world: the creature still gets thirsty, but has nowhere to go.
            continue;
        }

        const float dist = dm::Sqrt(best_d2);
        // nearness for the arbiter's Drink utility — 1 at the hole,
        // fading to 0 at the 40 m sensing horizon.
        th.water_proximity = ThirstClamp01(1.0f - dist / 40.0f);

        if (dist <= best_radius) {
            // In range of the nearest hole: drink. Thirst falls (clamped >= 0). No wish — it
            // has arrived; the orchestrator can stop pulling it toward water.
            th.drinking = 1;
            th.thirst = ThirstClamp01(th.thirst - drink);
            ++stats.drinking;
            continue;
        }

        // Thirsty enough to seek? Write a unit direction toward the hole, scaled by thirst,
        // so a parched creature pulls harder than a mildly-thirsty one.
        if (th.thirst >= tuning.seek_threshold && dist > 0.0f) {
            const float inv = 1.0f / dist;
            th.wish_x = best_dx * inv * th.thirst;
            th.wish_z = best_dz * inv * th.thirst;
            ++stats.seeking;
        }
    }

    return stats;
}

} // namespace luminumbra::ai
