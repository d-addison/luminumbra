#pragma once

// sim.territory: HOME-RANGE / TERRITORIALITY.
//
// A creature carrying a TerritoryComponent claims a HOME (its current position the first time
// the system sees it) and prefers to stay near it. Each tick the system produces a HOMING wish
// bias (TerritoryBiasComponent) that points from the creature back toward home and GROWS the
// further it strays beyond its territory radius:
//   * established==0 -> CLAIM: stamp home_{x,z} from the current TransformComponent, set
//     established=1. A creature claims exactly where it is, so on the claiming tick it is at
//     home and its bias is zero.
//   * INSIDE the radius (distance <= radius): bias is ZERO — the creature roams its home range
//     freely.
//   * OUTSIDE the radius (distance > radius): bias points TOWARD home with magnitude that
//     rises with the OVERSHOOT (distance - radius) past the edge, so the pull is gentle just
//     outside the boundary and stronger the further it has wandered. The bias is capped so an
//     arbitrarily distant creature cannot produce an unbounded steer.
//
// The orchestrator blends TerritoryBiasComponent.wish_{x,z} into the creature's movement. By
// keeping every creature near its OWN home, a predator that drifts into another predator's
// claimed ground is steered back toward its own territory (territorial separation falls out of
// each animal homing to its own range).
//
// DETERMINISM. id-ordered traversal (sort by entt::to_integral). TWO-PHASE:  claims
// home (writing only the creature's OWN territory) and snapshots its position + home;
// computes the bias from that snapshot and writes TerritoryBiasComponent. Each creature's
// output depends only on its own position + its own fixed home, so the result is fully
// order-independent regardless. Math is DeterministicMath only (Sqrt for distance; +-*/); NO
// rng is consumed (the homing field is fully deterministic — seed offset +30 is reserved for
// collision-avoidance only and never drawn), NO wall-clock, NO libm transcendentals.
// run==replay holds.
//
// GATING. A creature only participates if it carries a TerritoryComponent (the opt-in). A
// world whose creatures carry none — and any world with no creatures — does nothing (creates
// no TerritoryBiasComponent, mutates nothing), so the canonical NetworkStateHash baseline
// stays byte-identical.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CoreComponents.h"
#include "../components/TerritoryComponents.h"
#include "../core/DeterministicMath.h"

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry: territory+30). This system is deterministic and
// consumes NO rng; the offset is recorded for collision-avoidance only.
inline constexpr std::uint64_t kTerritorySeedOffset = 30ull;

// How strongly the homing bias grows per world-unit of OVERSHOOT past the territory edge.
// At an overshoot of 1/kTerritoryHomingGain world units the bias magnitude reaches its cap.
// Tuned gentle so the pull blends with (rather than overrides) normal wander/seek movement.
inline constexpr float kTerritoryHomingGain = 0.25f;

// Maximum homing-bias magnitude (m/s-scaled steer), so a creature stranded very far from home
// produces a bounded, blendable pull rather than an explosive velocity.
inline constexpr float kTerritoryMaxBias = 3.0f;

struct TerritoryStats {
    int participants = 0; // creatures carrying a TerritoryComponent that took part
    int claimed = 0;      // creatures that claimed their home this tick (first sighting)
    int homing = 0;       // creatures with a non-zero homing bias this tick (outside radius)
};

// RunTerritoryOnTick: claim home on first sight, then emit each creature's homing wish bias.
// id-ordered, two-phase. `tick` is accepted for signature parity with the other tick systems
// (this system is rng-free, so it does not consume the tick, but keeping it keeps the wire
// site uniform).
inline TerritoryStats RunTerritoryOnTick(entt::registry& reg, std::uint64_t /*tick*/ = 0) {
    TerritoryStats stats;

    auto view = reg.view<Comp::TerritoryComponent, Comp::TransformComponent>();

    // id-ordered participant list (deterministic traversal).
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.

    // ---- First pass: claim home where needed and snapshot state. ----
    struct Snap {
        entt::entity e;
        float x, z;   // current position
        float hx, hz; // claimed home
        float radius;
    };
    std::vector<Snap> snap;
    snap.reserve(ents.size());
    for (auto e : ents) {
        auto& terr = view.get<Comp::TerritoryComponent>(e);
        const auto& tf = view.get<Comp::TransformComponent>(e);
        if (terr.established == 0) {
            // First sighting: claim the CURRENT position as home.
            terr.home_x = tf.position.x;
            terr.home_z = tf.position.z;
            terr.established = 1;
            ++stats.claimed;
        }
        snap.push_back({e, tf.position.x, tf.position.z, terr.home_x, terr.home_z, terr.radius});
    }
    stats.participants = static_cast<int>(snap.size());

    // ---- Second pass: compute and write homing bias from the snapshot. ----
    for (const Snap& s : snap) {
        auto& bias = reg.get_or_emplace<Comp::TerritoryBiasComponent>(s.e);

        const float dx = s.hx - s.x; // vector TOWARD home
        const float dz = s.hz - s.z;
        const float dist = dm::Sqrt(dx * dx + dz * dz);

        // Inside the home range (or exactly at the edge): no homing pull.
        const float radius = s.radius > 0.0f ? s.radius : 0.0f;
        if (dist <= radius || dist <= 1.0e-6f) {
            bias.wish_x = 0.0f;
            bias.wish_z = 0.0f;
            continue;
        }

        // Outside: magnitude rises with the OVERSHOOT past the edge, capped.
        const float overshoot = dist - radius;        // > 0 here
        float mag = overshoot * kTerritoryHomingGain; // grows linearly with how far it strayed
        if (mag > kTerritoryMaxBias)
            mag = kTerritoryMaxBias;

        // Unit vector toward home * magnitude. dist > radius >= 0 and dist > 1e-6, safe divide.
        const float inv = mag / dist;
        bias.wish_x = dx * inv;
        bias.wish_z = dz * inv;
        ++stats.homing;
    }

    return stats;
}

} // namespace luminumbra::ai
