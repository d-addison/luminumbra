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
#include <cstddef>
#include <cstdint>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CreatureComponents.h"
#include "../components/DecayComponents.h"
#include "../components/MortalComponents.h"
#include "CreatureTickAdmission.h"

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
    std::size_t visited = 0;          // runtime diagnostic: roster entries examined this tick
};

namespace decomposition_detail {
// Runtime index only: components and entity lifetimes remain unchanged. The set
// contains at most one handle per admitted decay participant (EnTT entity bound).
// Maintain ID order on admission, rather than sorting/copying history every tick.
struct Roster {
    std::set<entt::entity> active;

    static void Track(entt::registry& reg, entt::entity entity) {
        auto& roster = reg.ctx().get<Roster>();
        const auto& decay = reg.get<Comp::DecayComponent>(entity);
        if (decay.fully_decomposed == 0) {
            roster.active.insert(entity);
        } else {
            roster.active.erase(entity);
        }
    }

    static void Forget(entt::registry& reg, entt::entity entity) {
        reg.ctx().get<Roster>().active.erase(entity);
    }
};
} // namespace decomposition_detail

// Read-only runtime roster/diagnostic. The first call indexes existing components;
// registry signals admit later additions/patches and forget removals/recycled IDs.
// A direct reset of a completed component must use registry.patch or replace so
// it is admitted again. Ordinary field updates on unfinished entries need no hook.
// Lookup never scans an initialized roster. Direct completions retire when the
// live traversal visits them or a detached stage publishes their completion.
inline const std::set<entt::entity>& DecompositionParticipants(entt::registry& reg) {
    using Roster = decomposition_detail::Roster;
    auto* roster = reg.ctx().find<Roster>();
    if (!roster) {
        Roster initial;
        for (const auto entity : reg.view<Comp::DecayComponent>()) {
            if (reg.get<Comp::DecayComponent>(entity).fully_decomposed == 0) {
                initial.active.insert(entity);
            }
        }
        roster = &reg.ctx().emplace<Roster>(std::move(initial));
        reg.on_construct<Comp::DecayComponent>().connect<&Roster::Track>();
        reg.on_update<Comp::DecayComponent>().connect<&Roster::Track>();
        reg.on_destroy<Comp::DecayComponent>().connect<&Roster::Forget>();
    }
    return roster->active;
}

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

namespace decomposition_detail {
// One ID-ordered traversal shared by the live and detached entry points.
template<typename Participants>
inline void AdvanceParticipants(entt::registry& reg,
                                Participants& ents,
                                const CreatureTickAdmission* admission,
                                const ::Luminumbra::world::RegionConsumerGate& gate,
                                DecompositionStats& stats,
                                bool use_elapsed) {
    for (auto next = ents.begin(); next != ents.end();) {
        const auto current = next++;
        const auto e = *current;
        // Only an owned, mutable roster can retire entries. Borrowed detached
        // candidates leave the live roster intact until successful publication.
        const auto retire = [&] {
            if constexpr (std::is_same_v<Participants, std::set<entt::entity>>) {
                ents.erase(current);
            }
        };
        ++stats.visited;
        auto* decay = reg.try_get<Comp::DecayComponent>(e);
        if (!decay || decay->fully_decomposed != 0) {
            retire();
            continue; // A detached candidate can omit a live participant.
        }
        const bool captured = admission && reg.all_of<Comp::CreatureComponent>(e);
        const bool allowed = captured ? admission->Allows(e) : gate.Allows(e);
        if (!allowed) {
            continue;
        }
        const auto ticks =
            use_elapsed ? (captured ? admission->ElapsedTicks(e) : gate.ElapsedTicks(e)) : 1u;
        if (ticks == 0) {
            continue;
        }
        auto& dc = *decay;

        // A degenerate duration is treated as immediate-complete on first dead tick (avoids a
        // divide-by-zero and keeps the contract: a 0-duration corpse releases its full load at
        // once). A live entity still must not advance.
        if (!DecompIsDead(reg, e))
            continue; // LIVE -> does not decay.

        // The pre-tick cumulative release for this corpse (already accounted in the component).
        const std::uint32_t prev_total = dc.nutrient_released_milli;

        // Subtract before adding: even a UINT32_MAX window or duration cannot wrap.
        // A single cumulative division telescopes exactly to repeated one-tick release.
        if (dc.decay_ticks < dc.decay_duration) {
            dc.decay_ticks += std::min(ticks, dc.decay_duration - dc.decay_ticks);
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
            retire();
        }
    }
}
} // namespace decomposition_detail

// RunDecompositionOnTick: advance the decomposition clock + nutrient release for every dead
// participant. id-ordered, pure-integer, deterministic. `tick` is accepted for signature
// parity with the other sim systems (it is NOT consumed — decomposition is tick-count driven,
// not absolute-tick driven, so re-running from a snapshot reproduces exactly).
// Elapsed opt-in uses captured ownership for creatures and the gate for other opt-ins;
// death is sampled at due time. Zero windows leave corpses untouched. Stats count corpses,
// not substeps; released_total is the exact cumulative delta for the whole window.
// Default calls retain one-tick behaviour, including observational zero-window admission.
inline DecompositionStats
RunDecompositionOnTick(entt::registry& reg,
                       std::uint64_t /*tick*/,
                       const CreatureTickAdmission* admission = nullptr,
                       const ::Luminumbra::world::RegionConsumerGate& gate = {},
                       const std::set<entt::entity>* participants = nullptr,
                       bool use_elapsed = false) {
    DecompositionStats stats;

    // Detached stages borrow the live roster: rebuilding an index from their
    // copied component pool would revisit every historical carcass each tick.
    // Only the owning live registry may retire entries during traversal.
    const auto& ents = participants ? *participants : DecompositionParticipants(reg);
    auto* roster = reg.ctx().find<decomposition_detail::Roster>();
    if (roster && &ents == &roster->active) {
        decomposition_detail::AdvanceParticipants(
            reg, roster->active, admission, gate, stats, use_elapsed);
    } else {
        decomposition_detail::AdvanceParticipants(reg, ents, admission, gate, stats, use_elapsed);
    }
    return stats;
}

// Detached regional stages copy only staged entities. Traverse the smaller of
// the live unfinished roster and the staged decay pool, so per-tick work stays
// bounded by both the region scope and the unfinished count. Both candidate
// lists are ID ordered and select the same unfinished staged handles.
inline DecompositionStats
RunDetachedDecompositionOnTick(entt::registry& detached,
                               std::uint64_t tick,
                               const CreatureTickAdmission* admission,
                               const ::Luminumbra::world::RegionConsumerGate& gate,
                               const std::set<entt::entity>& live,
                               std::vector<entt::entity>& scratch,
                               bool use_elapsed = false) {
    const auto staged = detached.view<Comp::DecayComponent>();
    if (live.size() <= staged.size()) {
        return RunDecompositionOnTick(detached, tick, admission, gate, &live, use_elapsed);
    }
    scratch.clear();
    for (const auto entity : staged) {
        if (staged.get<Comp::DecayComponent>(entity).fully_decomposed == 0 &&
            live.contains(entity)) {
            scratch.push_back(entity);
        }
    }
    std::sort(scratch.begin(), scratch.end());
    DecompositionStats stats;
    decomposition_detail::AdvanceParticipants(
        detached, scratch, admission, gate, stats, use_elapsed);
    return stats;
}

// One session decay stage. `participants` is the live roster. A detached stage
// borrows it and is bounded by its staged decay pool; the live stage passes it
// as its own roster. An empty roster does no work.
inline DecompositionStats RunDecompositionStage(entt::registry& reg,
                                                std::uint64_t tick,
                                                const CreatureTickAdmission* admission,
                                                const ::Luminumbra::world::RegionConsumerGate& gate,
                                                const std::set<entt::entity>& participants,
                                                bool detached,
                                                bool use_elapsed = false) {
    if (participants.empty()) {
        return {};
    }
    if (detached) {
        std::vector<entt::entity> staged_decay;
        return RunDetachedDecompositionOnTick(
            reg, tick, admission, gate, participants, staged_decay, use_elapsed);
    }
    return RunDecompositionOnTick(reg, tick, admission, gate, &participants, use_elapsed);
}

} // namespace luminumbra::ai
