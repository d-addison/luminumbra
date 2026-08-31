#pragma once

// sim.lifespan: creatures (and any opted-in entity) AGE and DIE. This is
// the closing half of the ecology life-cycle: CreatureReproductionSystem makes
// BIRTHS, this makes DEATHS, so the population is bounded rather than exploding.
//
// MODEL: every entity carrying a MortalComponent (components/MortalComponents.h)
// is the per-entity opt-in. Each tick the system, in ASCENDING entity-id order:
//   * ages the entity (age_ticks++ , saturating; never ages a dead entity), and
//   * MARKS it dead when EITHER
//       - it has reached its lifespan  (age_ticks >= lifespan_ticks)  [old age], OR
//       - it ALSO carries a CreatureComponent whose hunger has maxed out
//         (hunger >= 1.0)                                             [starvation].
//
// Marking dead sets MortalComponent.dead = 1 and, when a CreatureComponent is
// present, ALSO sets CreatureComponent.eaten = true so the existing brain /
// floating-id marker treat the corpse as an inert carcass (it stops deciding /
// moving and predators ignore it) — reusing the carcass seam rather than adding
// a new one. The system does NOT reg.destroy anything: the orchestrator owns
// removal/cleanup later (destroying here could desync the avatar pool).
//
// DETERMINISM (this feeds world_hash once wired): id-ordered traversal; all
// state is integer (age/lifespan are uint32 ticks, dead is a flag); the only
// float read is CreatureComponent.hunger against a constant threshold (a pure
// compare, no accumulation in mortal sim truth). NO rng (death here is a
// DETERMINISTIC threshold, so the reserved seed offset +22 is recorded for
// collision-avoidance but intentionally never consumed), NO wall-clock, NO libm
// transcendentals. run==replay holds.
//
// GATING: a registry with NO MortalComponent => RunLifespanOnTick performs zero
// work and touches nothing, so the canonical NetworkStateHash baseline stays
// byte-identical until mortal entities are deliberately spawned.
//
// SEED OFFSET REGISTRY: wind+11, weather+12/13, aether+14, plant+15,
// creature-repro+16, fire+17, soil+18, pollination+19, disease+20 taken;
// LIFESPAN CLAIMS +22 (reserved; unused — death is a deterministic threshold).

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CreatureComponents.h"  // CreatureComponent (starvation + carcass seam)
#include "../components/MortalComponents.h"     // MortalComponent (the opt-in)

namespace luminumbra::ai {

// Components live in the capital-L namespace (engine convention).
namespace Comp = ::Luminumbra::Components;

// Reserved seed-stream offset for the lifespan track. Death here is a pure
// DETERMINISTIC threshold (no stochastic culling), so this is recorded for
// collision-avoidance but intentionally never consumed.
inline constexpr std::uint64_t kLifespanSeedOffset = 22ull;

// Hunger at/above this is fatal starvation (hunger is 0 sated .. 1 starving).
inline constexpr float kStarvationHunger = 1.0f;

// Telemetry / sub-hash for the lifespan tick.
struct LifespanStats {
    int aged = 0;  // mortal entities advanced this tick (alive before the step)
    int died = 0;  // entities newly MARKED dead this tick (old age OR starvation)
};

// Advance the lifespan of every opted-in entity by ONE fixed tick. Pure function
// of registry state. id-ordered so the death-marking order (and thus any derived
// telemetry) is deterministic. No creation/destruction -> no two-phase needed,
// no view invalidation. Safe to call every tick; a no-op when no entity opts in.
inline LifespanStats RunLifespanOnTick(entt::registry& reg, std::uint64_t /*tick*/ = 0) {
    LifespanStats stats;

    auto view = reg.view<Comp::MortalComponent>();

    // id-ordered traversal so death-marking is deterministic / order-stable.
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    for (auto e : ents) {
        auto& mortal = view.get<Comp::MortalComponent>(e);

        // A latched-dead entity is inert: it neither ages nor dies again, so its
        // age and death flag stay stable across run==replay.
        if (mortal.dead != 0) {
            continue;
        }

        ++stats.aged;

        // Age (saturating so a long-lived entity can't wrap its clock).
        if (mortal.age_ticks < 0xFFFFFFFFu) {
            ++mortal.age_ticks;
        }

        // Old-age death: reaching lifespan is fatal.
        bool dies = mortal.age_ticks >= mortal.lifespan_ticks;

        // Starvation death: only if this entity is also a creature (the brain's
        // hunger model lives there). Reading hunger >= 1.0 as "maxed out".
        if (!dies) {
            if (auto* cr = reg.try_get<Comp::CreatureComponent>(e)) {
                if (cr->hunger >= kStarvationHunger) {
                    dies = true;
                }
            }
        }

        if (dies) {
            mortal.dead = 1;
            // Reuse the existing carcass seam so the brain / markers treat the
            // corpse as inert (stops deciding/moving; predators ignore it).
            if (auto* cr = reg.try_get<Comp::CreatureComponent>(e)) {
                cr->eaten = true;
            }
            ++stats.died;
        }
    }

    return stats;
}

}  // namespace luminumbra::ai
