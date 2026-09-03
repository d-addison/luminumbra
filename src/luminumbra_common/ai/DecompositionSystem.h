#pragma once

// sim.decomposition: DEAD creatures DECAY and RELEASE NUTRIENTS, closing the
// death -> soil cycle. A creature that has died (MortalComponent.dead==1, OR
// CreatureComponent.eaten==1 if present) slowly decomposes over a fixed number of ticks; as
// it rots it releases nutrient into the world. This system maintains that per-corpse
// decomposition clock and the cumulative nutrient released; the orchestrator routes the
// per-tick release into the SoilGrid during wiring (this system never touches the grid).
//
//   * A LIVE entity (not dead) does NOT decay — its clock holds at 0.
//   * A DEAD entity advances decay_ticks by one each tick (saturating at decay_duration).
//   * Nutrient is released in PROPORTION to elapsed decay, in fixed-point milli-units, so the
//     cumulative total rises MONOTONICALLY to kNutrientPerCorpseMilli exactly at
//     decay_duration. The per-tick release (the stat) is the increase this tick.
//   * At decay_duration the corpse is fully_decomposed (latched) and stops advancing — its
//     clock and total are then stable forever, so run==replay holds.
//
// DETERMINISM. id-ordered traversal (sort by entt::to_integral); pure INTEGER math (the
// cumulative release is total = kNutrientPerCorpseMilli * decay_ticks / decay_duration, an
// exact integer division). NO rng (seed offset +27 is reserved for collision-avoidance only
// and never drawn), NO wall-clock, NO libm. run==replay holds.
//
// GATING. An entity participates ONLY if it carries a DecayComponent (the opt-in). A world
// whose entities carry none — and any world with no entities — does nothing, so the canonical
// NetworkStateHash baseline stays byte-identical.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CreatureComponents.h"
#include "../components/DecayComponents.h"
#include "../components/MortalComponents.h"

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;

// Reserved seed-stream offset (registry: ... lifespan+22, wildlife+23, photo+24, herd-alarm+26,
// DECOMPOSITION CLAIMS +27). This system is deterministic and consumes NO rng; the offset is
// recorded for collision-avoidance only.
inline constexpr std::uint64_t kDecompositionSeedOffset = 27ull;

// Total nutrient a single fully-decomposed corpse releases, in milli-units (fixed-point:
// 1000 = one whole nutrient unit). The cumulative release reaches exactly this at
// decay_duration. Sized to fit the component's std::uint16_t store (<= 65535).
inline constexpr std::uint32_t kNutrientPerCorpseMilli = 1000u;

struct DecompositionStats {
    int decaying = 0;                 // dead entities that advanced their decay this tick
    std::uint32_t released_total = 0; // nutrient released THIS TICK across all corpses (milli)
    int finished = 0;                 // entities that reached full decomposition this tick
};

// Is this entity dead? Death is marked by MortalComponent.dead, OR (if the entity is a
// creature) by CreatureComponent.eaten (the carcass seam used by the brain/predation).
[[nodiscard]] inline bool DecompIsDead(entt::registry& reg, entt::entity e) {
    if (const auto* m = reg.try_get<Comp::MortalComponent>(e)) {
        if (m->dead != 0)
            return true;
    }
    if (const auto* cr = reg.try_get<Comp::CreatureComponent>(e)) {
        if (cr->eaten)
            return true;
    }
    return false;
}

// RunDecompositionOnTick: advance the decomposition clock + nutrient release for every dead
// participant. id-ordered, pure-integer, deterministic. `tick` is accepted for signature
// parity with the other sim systems (it is NOT consumed — decomposition is tick-count driven,
// not absolute-tick driven, so re-running from a snapshot reproduces exactly).
inline DecompositionStats RunDecompositionOnTick(entt::registry& reg, std::uint64_t /*tick*/) {
    DecompositionStats stats;

    auto view = reg.view<Comp::DecayComponent>();

    // id-ordered participant list (deterministic traversal).
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.

    for (auto e : ents) {
        auto& dc = view.get<Comp::DecayComponent>(e);

        // Already finished: inert and stable (latched) — never advance again.
        if (dc.fully_decomposed != 0)
            continue;

        // A degenerate duration is treated as immediate-complete on first dead tick (avoids a
        // divide-by-zero and keeps the contract: a 0-duration corpse releases its full load at
        // once). A live entity still must not advance.
        if (!DecompIsDead(reg, e))
            continue; // LIVE -> does not decay.

        // The pre-tick cumulative release for this corpse (already accounted in the component).
        const std::uint32_t prev_total = dc.nutrient_released_milli;

        // Advance the decomposition clock by one tick, saturating at the duration.
        if (dc.decay_ticks < dc.decay_duration) {
            ++dc.decay_ticks;
        }
        ++stats.decaying;

        // Cumulative release is EXACTLY proportional to elapsed decay (integer division), so it
        // rises monotonically and lands on kNutrientPerCorpseMilli precisely at decay_duration.
        std::uint32_t new_total;
        if (dc.decay_duration == 0u || dc.decay_ticks >= dc.decay_duration) {
            new_total = kNutrientPerCorpseMilli; // finished -> full load.
        } else {
            new_total = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(kNutrientPerCorpseMilli) * dc.decay_ticks) /
                dc.decay_duration);
        }
        if (new_total < prev_total)
            new_total = prev_total; // clamp monotonic (defensive).

        dc.nutrient_released_milli = static_cast<std::uint16_t>(new_total);
        stats.released_total += (new_total - prev_total);

        // Latch full decomposition once the clock reaches the duration.
        if (dc.decay_ticks >= dc.decay_duration) {
            dc.fully_decomposed = 1;
            ++stats.finished;
        }
    }

    return stats;
}

} // namespace luminumbra::ai
