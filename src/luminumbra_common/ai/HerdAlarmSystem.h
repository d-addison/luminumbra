#pragma once

// sim.herd_alarm: ALARM SIGNALLING / COLLECTIVE VIGILANCE.
//
// When a prey senses a threat it raises an alarm; this system PROPAGATES that alarm to
// nearby SAME-ROLE neighbours so the herd flees together. Concretely it maintains a
// scalar "alarm field" over every creature carrying an AlarmComponent:
//   * a creature is an alarm SOURCE this tick if it is alarmed==1 (effective strength >= 1)
//     or its current level is above kAlarmSourceThreshold,
//   * a source RAISES the level of same-role neighbours (matched on
//     CreatureComponent.is_predator) within kAlarmRadius, scaled by CLOSENESS (full at the
//     source, falling linearly to 0 at the radius edge) and by the source's strength,
//   * a carcass (CreatureComponent.eaten) neither emits nor receives (the dead don't warn),
//   * every creature's level DECAYS multiplicatively toward 0 each tick; the propagated
//     contribution is then folded in, so a level only persists while a source is in range.
//
// CreatureBrainSystem consumes the propagated level when evaluating flee
// behaviour; this system maintains that field.
//
// DETERMINISM. id-ordered traversal (sort by entt::to_integral). TWO-PHASE so the result is
// order-INDEPENDENT:  SNAPSHOTS every participant's pre-tick level/role/position;
//  computes each creature's new level from that frozen snapshot (decay of its own
// snapshot level + the max-closeness contribution from snapshot sources) and writes it.
// Because every read is from the snapshot, no creature's update can see another creature's
// in-this-tick update, so iteration order cannot change the outcome. Math is
// DeterministicMath only (Sqrt for distance; +-*/, clamped to [0,1]); NO rng is consumed
// (propagation is fully deterministic), NO wall-clock, NO libm transcendentals. run==replay
// holds. Seed offset +26 is reserved for collision-avoidance only and never drawn.
//
// GATING. A creature only participates if it carries an AlarmComponent (the opt-in). A world
// whose creatures carry none — and any world with no creatures — does nothing, so the
// canonical NetworkStateHash baseline stays byte-identical.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/AlarmComponents.h"
#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../core/DeterministicMath.h"

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry:... pollination+19, disease+20, irrigation+21,
// lifespan+22, wildlife+23, photo+24, herd-alarm+26). This system is deterministic and
// consumes NO rng; the offset is recorded for collision-avoidance only.
inline constexpr std::uint64_t kHerdAlarmSeedOffset = 26ull;

// Distance (world units) within which an alarmed creature warns same-role neighbours.
inline constexpr float kAlarmRadius = 12.0f;

// A creature whose accumulated level is at/above this acts as a propagating SOURCE (so the
// alarm relays outward in  through a chain of creatures, not just from the originator).
inline constexpr float kAlarmSourceThreshold = 0.25f;

// Per-tick retention of a creature's own level when no source is nearby: new_self = level *
// kAlarmDecay. ~0.85 fades a full alarm to near-zero over ~1s @30Hz once the threat passes.
inline constexpr float kAlarmDecay = 0.85f;

// Fraction of a source's strength that transfers to a neighbour AT zero distance (it falls
// linearly to 0 at kAlarmRadius). Keeps a relayed creature slightly below its source so the
// chain forms a decreasing gradient outward rather than a flat plateau.
inline constexpr float kAlarmTransfer = 0.9f;

struct HerdAlarmStats {
    int participants = 0; // creatures carrying an AlarmComponent that took part
    int sources = 0;      // creatures acting as an alarm source this tick
    int raised = 0;       // creatures whose level INCREASED this tick (propagation reached)
};

[[nodiscard]] inline float AlarmClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// RunHerdAlarmOnTick: maintain the propagating alarm field. id-ordered, two-phase snapshot.
// `dt` scales the decay/propagation rate so the field is frame-rate independent (the sim
// runs a fixed dt; the parameter keeps the signature honest and lets tests exercise decay).
inline HerdAlarmStats RunHerdAlarmOnTick(entt::registry& reg, float dt) {
    HerdAlarmStats stats;

    auto view = reg.view<Comp::AlarmComponent, Comp::CreatureComponent, Comp::TransformComponent>();

    // id-ordered participant list (deterministic traversal).
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.

    // ---- First pass: snapshot each participant's pre-tick state. ----
    struct Snap {
        entt::entity e;
        float x, z;
        float level;        // pre-tick level
        bool predator;      // role to match on
        bool dead;          // carcass: neither emits nor receives
        float src_strength; // 0 if not a source this tick; else its emitting strength
    };
    std::vector<Snap> snap;
    snap.reserve(ents.size());
    for (auto e : ents) {
        const auto& al = view.get<Comp::AlarmComponent>(e);
        const auto& cr = view.get<Comp::CreatureComponent>(e);
        const auto& tf = view.get<Comp::TransformComponent>(e);
        const float level = AlarmClamp01(al.level);
        const bool dead = cr.eaten;
        float src = 0.0f;
        if (!dead) {
            // alarmed==1 is a full-strength source; otherwise above-threshold level relays.
            if (al.alarmed != 0) {
                src = 1.0f;
            } else if (level >= kAlarmSourceThreshold) {
                src = level;
            }
        }
        snap.push_back({e, tf.position.x, tf.position.z, level, cr.is_predator, dead, src});
    }

    stats.participants = static_cast<int>(snap.size());
    for (const Snap& s : snap) {
        if (s.src_strength > 0.0f)
            ++stats.sources;
    }

    // dt-scaled rates. The sim's fixed dt (~1/30s) gives the tuned constants; we keep the
    // behaviour bounded for any dt by clamping the scaled fractions to [0,1].
    const float rate = dt * static_cast<float>(::Luminumbra::TICKS_PER_SECOND); // 1.0 at 30Hz
    // Decay applied this tick: level retains (kAlarmDecay) per 1/30s. Approximate the
    // per-dt retention by scaling the *loss* (1-kAlarmDecay) with rate, clamped so a large
    // dt can drain but never invert.
    float decay_loss = (1.0f - kAlarmDecay) * rate;
    decay_loss = AlarmClamp01(decay_loss);
    const float keep = 1.0f - decay_loss;

    // ---- Second pass: compute each creature's level from the frozen snapshot. ----
    const float invR = kAlarmRadius > 0.0f ? (1.0f / kAlarmRadius) : 0.0f;
    for (std::size_t i = 0; i < snap.size(); ++i) {
        const Snap& self = snap[i];

        auto& al = view.get<Comp::AlarmComponent>(self.e);

        if (self.dead) {
            // The dead don't warn and don't fear: drain to zero deterministically.
            al.level = 0.0f;
            continue;
        }

        // Self term: own pre-tick level decayed toward 0. An alarmed source pins itself to
        // full so the originator stays lit while it senses the threat.
        float next = self.level * keep;
        if (al.alarmed != 0) {
            next = 1.0f; // pin ONLY a genuinely-alarmed originator (not a relay that hit 1.0).
        }

        // Propagation term: take the STRONGEST same-role source contribution in range
        // (max, not sum, so density doesn't blow the field past 1 and the gradient is the
        // distance to the nearest/loudest alarm). Scaled by closeness and source strength.
        float best_in = 0.0f;
        for (std::size_t j = 0; j < snap.size(); ++j) {
            if (j == i)
                continue;
            const Snap& o = snap[j];
            if (o.dead || o.src_strength <= 0.0f)
                continue;
            if (o.predator != self.predator)
                continue; // only warn your own kind/role.
            const float dx = o.x - self.x;
            const float dz = o.z - self.z;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (d >= kAlarmRadius)
                continue;                            // out of earshot.
            const float closeness = 1.0f - d * invR; // 1 at source.. 0 at edge.
            const float contrib = o.src_strength * kAlarmTransfer * closeness * rate;
            if (contrib > best_in)
                best_in = contrib;
        }

        // Fold the incoming alarm in: the field rises to whichever is louder — its decayed
        // self level or the best incoming contribution (so a relayed creature lights up but
        // a creature already louder than its neighbour isn't pulled down by them).
        if (best_in > next)
            next = best_in;

        next = AlarmClamp01(next);
        if (next > self.level + 1.0e-6f)
            ++stats.raised;
        al.level = next;
    }

    return stats;
}

} // namespace luminumbra::ai
