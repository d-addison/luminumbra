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
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <entt/entity/registry.hpp>

#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../components/MortalComponents.h"
#include "../components/ScavengerComponent.h"
#include "../components/SimulationIdentityComponent.h"
#include "../core/DeterministicMath.h"
#include "CreatureTickAdmission.h"

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
    // Diagnostic: actual identity reads by the radix passes; unrelated residents add no visits.
    std::uint64_t identity_order_visits = 0;
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
namespace detail {

// Eight byte passes bound ordering to O(participants), with scratch bounded by the
// supplied role view. Unique admitted IDs make the final order independent of input order.
template<bool IdentityOrder>
inline std::uint64_t SortScavengingEntities(const entt::registry& reg,
                                            std::vector<entt::entity>& entities) {
    if constexpr (!IdentityOrder) {
        std::sort(entities.begin(), entities.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        return 0;
    } else {
        if (entities.size() < 2)
            return 0;
        std::uint64_t visits = 0;
        std::vector<entt::entity> scratch(entities.size());
        for (unsigned shift = 0; shift < 64; shift += 8) {
            std::array<std::size_t, 256> offsets{};
            for (auto entity : entities) {
                ++visits;
                const auto id =
                    reg.get<::Luminumbra::Components::SimulationIdentityComponent>(entity).id;
                ++offsets[static_cast<std::size_t>((id >> shift) & 255u)];
            }
            std::size_t total = 0;
            for (auto& offset : offsets) {
                const auto count = offset;
                offset = total;
                total += count; // At most entities.size(), so the prefix sum cannot overflow.
            }
            for (auto entity : entities) {
                ++visits;
                const auto id =
                    reg.get<::Luminumbra::Components::SimulationIdentityComponent>(entity).id;
                scratch[offsets[static_cast<std::size_t>((id >> shift) & 255u)]++] = entity;
            }
            entities.swap(scratch);
        }
        return visits;
    }
}

// Identity mode requires admitted, unique SimulationIdentityComponent IDs on participants
// and resource sources. Legacy mode deliberately retains runtime-handle ordering.
template<bool IdentityOrder>
inline ScavengingStats RunScavengingOnTickImpl(entt::registry& reg,
                                               std::uint64_t /*tick*/,
                                               const ScavengingTuning& tuning = {},
                                               const CreatureTickAdmission* admission = nullptr) {
    ScavengingStats stats;

    // ---- First pass: snapshot every carcass position. ----
    // Iterate ALL creatures (a carcass need not be a scavenger). MortalComponent is optional,
    // so fetch it via try_get rather than constraining the view.
    struct Carcass {
        float x, z;
    };
    struct IdentifiedCarcass {
        float x, z;
        std::uint64_t id;
    };
    using CarcassSnapshot = std::conditional_t<IdentityOrder, IdentifiedCarcass, Carcass>;
    std::vector<CarcassSnapshot> carcasses;
    {
        auto cview = reg.view<Comp::CreatureComponent, Comp::TransformComponent>();
        for (auto e : cview) {
            if (admission && !admission->Allows(e)) {
                continue;
            }
            const auto& cr = cview.get<Comp::CreatureComponent>(e);
            const auto* mortal = reg.try_get<Comp::MortalComponent>(e);
            if (!ScavengeIsCarcass(cr, mortal))
                continue;
            const auto& tf = cview.get<Comp::TransformComponent>(e);
            if constexpr (IdentityOrder) {
                const auto id = reg.get<Comp::SimulationIdentityComponent>(e).id;
                carcasses.push_back({tf.position.x, tf.position.z, id});
            } else {
                carcasses.push_back({tf.position.x, tf.position.z});
            }
        }
    }
    stats.carcasses = static_cast<int>(carcasses.size());

    // ---- gather scavengers (id-ordered for deterministic traversal) ----
    auto sview =
        reg.view<Comp::ScavengerComponent, Comp::CreatureComponent, Comp::TransformComponent>();
    std::vector<entt::entity> ents(sview.begin(), sview.end());
    stats.identity_order_visits += SortScavengingEntities<IdentityOrder>(reg, ents);
    if (ents.empty())
        return stats; // empty roster -> pure no-op.

    // ---- Second pass: steer and feed against the frozen carcass list. ----
    for (auto e : ents) {
        if (admission && !admission->Allows(e)) {
            continue;
        }
        ++stats.scavengers;
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

        // V2 resolves equal distances by source identity directly: no additional sort
        // or neighborhood scan. Legacy keeps its storage/position tie rules.
        float bestD = -1.0f;
        float bestDx = 0.0f;
        float bestDz = 0.0f;
        std::uint64_t bestId = 0;
        for (const auto& c : carcasses) {
            const float dx = c.x - tf.position.x;
            const float dz = c.z - tf.position.z;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            bool nearer = bestD < 0.0f || d < bestD;
            if constexpr (IdentityOrder) {
                nearer = nearer || (d == bestD && c.id < bestId);
            } else {
                nearer = nearer || (admission && d == bestD &&
                                    (dx < bestDx || (dx == bestDx && dz < bestDz)));
            }
            if (nearer) {
                bestD = d;
                bestDx = dx;
                bestDz = dz;
                if constexpr (IdentityOrder)
                    bestId = c.id;
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

} // namespace detail

inline ScavengingStats RunScavengingOnTick(entt::registry& reg,
                                           std::uint64_t tick,
                                           const ScavengingTuning& tuning = {},
                                           const CreatureTickAdmission* admission = nullptr) {
    return detail::RunScavengingOnTickImpl<false>(reg, tick, tuning, admission);
}

// Dormant candidate: shipping callers select this only with the v2 authority activation.
inline ScavengingStats RunScavengingOnTickV2(entt::registry& reg,
                                             std::uint64_t tick,
                                             const ScavengingTuning& tuning = {},
                                             const CreatureTickAdmission* admission = nullptr) {
    return detail::RunScavengingOnTickImpl<true>(reg, tick, tuning, admission);
}

} // namespace luminumbra::ai
