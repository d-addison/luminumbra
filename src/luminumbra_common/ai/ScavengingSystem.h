#pragma once

// sim.scavenging: SCAVENGERS EAT CARCASSES. Ties death -> food, complementing active
// hunting: predators (and old age / starvation via MortalComponent) make carcasses; this
// system lets HUNGRY scavengers find the nearest carcass, steer toward it, and FEED when in
// range — converting a dead body back into sated hunger so the ecology's energy doesn't just
// vanish at death.
//
// A SCAVENGER is a creature carrying BOTH a CreatureComponent and a ScavengerComponent (the
// presence of the latter is the per-entity opt-in gate). A CARCASS is any creature that is
// DEAD: CreatureComponent.eaten==1 OR (a MortalComponent is present AND its dead==1). A
// scavenger is only interested when it is HUNGRY (hunger above a threshold) and is itself
// alive (a dead scavenger doesn't scavenge).
//
// Each tick, for every hungry live scavenger (id-ordered):
//   * find the NEAREST carcass (DeterministicMath::Sqrt distance over a frozen snapshot of
//     carcass positions taken first, so the result is order-INDEPENDENT),
//   * write a wish toward it into ScavengerComponent.wish_x/z (a unit steer; the orchestrator
//     blends this into real movement, same shape as CreatureComponent.wish_x/z),
//   * if WITHIN the feed radius, set feeding=1 and LOWER the scavenger's CreatureComponent
//     .hunger by a fixed amount (clamped to [0,1]) — eating fills it up.
// A well-fed scavenger (hunger <= threshold), or one with no carcass in the world, writes a
// ZERO wish and does NOT feed.
//
// DETERMINISM. id-ordered traversal (sort by entt::to_integral). TWO-PHASE so the result is
// order-INDEPENDENT:  SNAPSHOTS every carcass's position (a read-only pass);
// steers/feeds each scavenger against that frozen list. A scavenger is never itself a carcass
// target (a hungry scavenger is alive, hence excluded from the carcass set), so the two roles
// don't interfere. Math is DeterministicMath only (Sqrt for distance; +-*/, clamped to
// [0,1]); NO rng is consumed (seed offset +34 is RESERVED for collision-avoidance only and
// never drawn), NO wall-clock, NO libm transcendentals. run==replay holds.
//
// GATING. A creature only participates as a scavenger if it carries a ScavengerComponent. A
// world whose creatures carry none — and any world with no creatures — does nothing (it
// writes/changes nothing), so the canonical NetworkStateHash baseline stays byte-identical.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../components/MortalComponents.h"
#include "../components/ScavengerComponent.h"
#include "../core/DeterministicMath.h"

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry: ... herd-alarm+26, ..., scavenging+34). This system
// is deterministic and consumes NO rng; the offset is recorded for collision-avoidance only.
inline constexpr std::uint64_t kScavengingSeedOffset = 34ull;

// A scavenger only seeks food when its hunger is ABOVE this (0 sated .. 1 starving). Below it
// the scavenger is well-fed and ignores carcasses (zero wish, no feeding).
inline constexpr float kScavengeHungerThreshold = 0.3f;

// Horizontal distance (world units) within which a scavenger can FEED on a carcass.
inline constexpr float kScavengeFeedRadius = 1.5f;

// Hunger removed per tick of feeding (clamped so hunger never goes below 0). ~0.05/tick at
// 30Hz sates a starving scavenger over ~0.6s of feeding.
inline constexpr float kScavengeFeedRate = 0.05f;

struct ScavengingStats {
    int scavengers = 0; // creatures carrying a ScavengerComponent that took part
    int carcasses = 0;  // dead creatures available as food this tick
    int seeking = 0;    // hungry live scavengers that found a carcass to steer toward
    int feeding = 0;    // scavengers within feed radius that lowered hunger this tick
};

[[nodiscard]] inline float ScavengeClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// Full-control tuning (defaults == the k* constants -> byte-identical). From SystemConfig
// sim.scavenging.
struct ScavengingTuning {
    float hunger_threshold = kScavengeHungerThreshold;
    float feed_radius = kScavengeFeedRadius;
    float feed_rate = kScavengeFeedRate;
};

// Is this creature a CARCASS (dead body available to scavenge)? eaten prey OR a flagged-dead
// mortal. `mortal` may be null (creature carries no MortalComponent).
[[nodiscard]] inline bool ScavengeIsCarcass(const Comp::CreatureComponent& cr,
                                            const Comp::MortalComponent* mortal) {
    if (cr.eaten)
        return true;
    if (mortal != nullptr && mortal->dead != 0)
        return true;
    return false;
}

// RunScavengingOnTick: hungry scavengers seek + eat the nearest carcass. id-ordered,
// two-phase snapshot (order-independent). Returns telemetry; mutates ScavengerComponent
// .wish_x/z + .feeding and CreatureComponent.hunger on feeding scavengers.
inline ScavengingStats RunScavengingOnTick(entt::registry& reg,
                                           std::uint64_t /*tick*/,
                                           const ScavengingTuning& tuning = {}) {
    ScavengingStats stats;

    // ---- First pass: snapshot every carcass position. ----
    // Iterate ALL creatures (a carcass need not be a scavenger). MortalComponent is optional,
    // so fetch it via try_get rather than constraining the view.
    struct Carcass {
        float x, z;
    };
    std::vector<Carcass> carcasses;
    {
        auto cview = reg.view<Comp::CreatureComponent, Comp::TransformComponent>();
        for (auto e : cview) {
            const auto& cr = cview.get<Comp::CreatureComponent>(e);
            const auto* mortal = reg.try_get<Comp::MortalComponent>(e);
            if (!ScavengeIsCarcass(cr, mortal))
                continue;
            const auto& tf = cview.get<Comp::TransformComponent>(e);
            carcasses.push_back({tf.position.x, tf.position.z});
        }
    }
    stats.carcasses = static_cast<int>(carcasses.size());

    // ---- gather scavengers (id-ordered for deterministic traversal) ----
    auto sview =
        reg.view<Comp::ScavengerComponent, Comp::CreatureComponent, Comp::TransformComponent>();
    std::vector<entt::entity> ents(sview.begin(), sview.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.
    stats.scavengers = static_cast<int>(ents.size());

    // ---- Second pass: steer and feed against the frozen carcass list. ----
    for (auto e : ents) {
        auto& sc = sview.get<Comp::ScavengerComponent>(e);
        auto& cr = sview.get<Comp::CreatureComponent>(e);
        const auto& tf = sview.get<Comp::TransformComponent>(e);

        // Default: no target this tick. (Reset so a stale wish/feeding doesn't persist.)
        sc.wish_x = 0.0f;
        sc.wish_z = 0.0f;
        sc.feeding = 0;

        // A dead scavenger doesn't scavenge; a well-fed one doesn't seek.
        const auto* selfMortal = reg.try_get<Comp::MortalComponent>(e);
        const bool selfDead = ScavengeIsCarcass(cr, selfMortal);
        if (selfDead)
            continue;
        if (cr.hunger <= tuning.hunger_threshold)
            continue; // sated enough — ignore food.
        if (carcasses.empty())
            continue; // nothing to scavenge.

        // Find the NEAREST carcass (deterministic Sqrt distance; first-found wins ties, and
        // the carcass list is built in a fixed order so ties are stable).
        float bestD = -1.0f;
        float bestDx = 0.0f;
        float bestDz = 0.0f;
        for (const Carcass& c : carcasses) {
            const float dx = c.x - tf.position.x;
            const float dz = c.z - tf.position.z;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (bestD < 0.0f || d < bestD) {
                bestD = d;
                bestDx = dx;
                bestDz = dz;
            }
        }
        // (carcasses non-empty here, so bestD >= 0.)
        ++stats.seeking;

        // Within feed radius: feed (lower hunger) and flag feeding. Hold position (zero wish)
        // so the scavenger settles on the carcass rather than overshooting it.
        if (bestD <= tuning.feed_radius) {
            sc.feeding = 1;
            cr.hunger = ScavengeClamp01(cr.hunger - tuning.feed_rate);
            ++stats.feeding;
            // wish stays (0,0) — already on the food.
            continue;
        }

        // Otherwise steer toward the carcass: unit direction (guard the degenerate near-zero
        // distance, though bestD > feed radius > 0 here makes that impossible).
        if (bestD > 1.0e-5f) {
            const float inv = 1.0f / bestD;
            sc.wish_x = bestDx * inv;
            sc.wish_z = bestDz * inv;
        }
    }

    return stats;
}

} // namespace luminumbra::ai
