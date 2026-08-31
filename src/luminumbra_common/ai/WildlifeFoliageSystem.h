#pragma once

// sim.wildlife_foliage: the ECOLOGY x FOLIAGE coupling: non-predator CREATURES
// GRAZE/TRAMPLE the nearby PLANTS, and the plants slowly REGROW when left alone. This
// closes the loop between the creature ecology (ai/CreatureBrain, CreatureReproduction)
// and the living-world foliage system (PlantGrowth / Soil): herds draw down the standing
// biomass where they congregate (overgrazed patches go bare), and fallow ground recovers,
// so creature density visibly shapes the plant cover (and, optionally, grazing feeds the
// creature — lowering its hunger).
//
// MODEL. Each tick, every NON-PREDATOR creature (CreatureComponent.is_predator == false,
// not eaten) within `kGrazeRadius` of a grazeable plant takes a bite: the plant's biomass
// drops by a graze step scaled by how many grazers are on it (trampling). Plants NOT
// grazed this tick REGROW their biomass by a small step toward the 1.0 cap. The grazed
// accumulation lives on a NEW component (GrazeableComponent) so no existing component is
// touched. Grazing optionally feeds the creature by lowering its hunger.
//
// DETERMINISM (feeds world_hash once wired):
//   * id-ordered traversal everywhere (sort by entt::to_integral).
//   * Creature positions are SNAPSHOTTED first, so the per-plant graze pressure is a pure
//     function of the snapshot and is independent of plant traversal order.
//   * Per-plant graze demand and per-creature feed are ACCUMULATED, then applied in a
//     single id-ordered pass — so two creatures on one plant (or one creature grazing two
//     plants) produce the same result regardless of entity-id ordering.
//   * Distance uses Luminumbra::DeterministicMath::Sqrt (IEEE-deterministic); all biomass /
//     hunger arithmetic is constant-step float add/sub/clamp, bit-stable under
//     -ffp-contract=off. NO wall-clock, NO std::random, NO libm transcendentals, NO rng
//     (the flow is pure — seed offset +23 is reserved for collision-avoidance, unused).
//
// GATING: a plant participates ONLY if it carries GrazeableComponent. A registry with NO
// grazeable plant => ZERO biomass changes anywhere => the system is a true no-op and the
// canonical NetworkStateHash baseline stays byte-identical until grazeable plants are
// deliberately spawned. (Same opt-in shape as PlantTag / CreatureGenomeComponent.)
//
// SEED OFFSET REGISTRY: wind+11, weather+12/13, aether+14, plant+15, creature-repro+16,
// fire+17, soil+18, pollination+19, disease+20 taken; WILDLIFE-FOLIAGE CLAIMS +23 (pure
// flow, no rng — recorded for collision-avoidance only).

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CoreComponents.h"        // TransformComponent
#include "../components/CreatureComponents.h"    // CreatureComponent
#include "../components/GrazeableComponent.h"    // GrazeableComponent (opt-in)
#include "../core/DeterministicMath.h"           // Sqrt

namespace luminumbra::ai {

// Components live in the capital-L namespace (engine convention).
namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset for the wildlife-foliage track. The flow is PURE (no rng),
// so this is recorded for collision-avoidance but intentionally never consumed.
inline constexpr std::uint64_t kWildlifeFoliageSeedOffset = 23ull;

// Geometry + rates. All are constant (integer-valued) floats so the per-tick arithmetic
// is bit-stable. Biomass lives in [0,1].
inline constexpr float kGrazeRadius      = 3.0f;   // a creature grazes plants within this (m)
inline constexpr float kGrazeRadiusSq    = kGrazeRadius * kGrazeRadius;
inline constexpr float kGrazePerCreature = 0.05f;  // biomass removed per grazing creature/tick
inline constexpr float kRegrowPerTick    = 0.01f;  // biomass regained per tick when ungrazed
inline constexpr float kFeedPerGraze     = 0.02f;  // hunger reduced on a creature per plant fed
inline constexpr float kBiomassMin       = 0.0f;
inline constexpr float kBiomassMax       = 1.0f;

// Full-control tuning: fields default to the constants above, so a default-constructed
// WildlifeFoliageTuning is byte-identical. Threaded via the optional Run arg; resolved from
// SystemConfig sim.wildlife_foliage when that system is enabled.
struct WildlifeFoliageTuning {
    float graze_radius       = kGrazeRadius;
    float graze_per_creature = kGrazePerCreature;
    float regrow_per_tick    = kRegrowPerTick;
    float feed_per_graze     = kFeedPerGraze;
};

inline float clampBiomass(float v) {
    if (v < kBiomassMin) return kBiomassMin;
    if (v > kBiomassMax) return kBiomassMax;
    return v;
}

// Telemetry / sub-hash for the wildlife-foliage tick.
struct WildlifeFoliageStats {
    int grazers = 0;        // non-predator creatures considered this tick
    int plants = 0;         // grazeable plants considered this tick
    int grazed_plants = 0;  // plants that were grazed (had >=1 grazer in range) this tick
    float biomass_removed = 0.0f; // total biomass removed by grazing this tick
    float biomass_regrown = 0.0f; // total biomass regrown this tick
};

// Advance the wildlife-foliage coupling by ONE fixed tick. Pure function of registry
// state. id-ordered; graze pressure is summed per plant (order-independent) and feed is
// summed per creature, then both are applied in id-ordered passes.
inline WildlifeFoliageStats RunWildlifeFoliageOnTick(entt::registry& reg, std::uint64_t tick,
                                                     const WildlifeFoliageTuning& tuning = {}) {
    WildlifeFoliageStats stats;
    const float graze_radius_sq = tuning.graze_radius * tuning.graze_radius;

    // --- 1. Snapshot NON-PREDATOR creature positions, id-ordered (order-stable). ---
    struct Grazer { entt::entity e; float x, z; };
    std::vector<Grazer> grazers;
    {
        auto cview = reg.view<const Comp::CreatureComponent, const Comp::TransformComponent>();
        std::vector<entt::entity> cents(cview.begin(), cview.end());
        std::sort(cents.begin(), cents.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        grazers.reserve(cents.size());
        for (auto e : cents) {
            const auto& cr = cview.get<const Comp::CreatureComponent>(e);
            if (cr.is_predator || cr.eaten) continue;  // only live herbivores graze
            const auto& tf = cview.get<const Comp::TransformComponent>(e);
            grazers.push_back({e, tf.position.x, tf.position.z});
            ++stats.grazers;
        }
    }

    // --- 2. Gather grazeable plants, id-ordered. ---
    auto pview = reg.view<Comp::GrazeableComponent, const Comp::TransformComponent>();
    std::vector<entt::entity> pents(pview.begin(), pview.end());
    std::sort(pents.begin(), pents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    // Per-creature feed accumulator, indexed parallel to `grazers` (summed so applying it
    // afterward is order-independent).
    std::vector<float> feed(grazers.size(), 0.0f);

    // --- 3. id-ordered pass over plants: count grazers in range, graze or regrow. ---
    for (auto pe : pents) {
        ++stats.plants;
        auto& gz = pview.get<Comp::GrazeableComponent>(pe);
        const auto& ptf = pview.get<const Comp::TransformComponent>(pe);

        // Count how many grazers are within range (trampling pressure) and remember which
        // (to feed them). Summing the count first keeps the bite order-independent.
        // A depleted plant (no biomass) still gets trampled but feeds NO ONE -- gate the
        // feed accrual on having biomass to give, so creatures can't eat from bare ground.
        const bool has_biomass = clampBiomass(gz.biomass) > 0.0f;
        int n_in_range = 0;
        for (std::size_t gi = 0; gi < grazers.size(); ++gi) {
            const float dx = grazers[gi].x - ptf.position.x;
            const float dz = grazers[gi].z - ptf.position.z;
            const float d2 = dx * dx + dz * dz;
            if (d2 <= graze_radius_sq) {
                ++n_in_range;
                if (has_biomass) feed[gi] += tuning.feed_per_graze;  // only a living plant feeds
            }
        }

        if (n_in_range > 0) {
            // Graze/trample: biomass drops proportional to the number of grazers (so a
            // herd flattens a patch faster). Constant-step float arithmetic.
            const float removed_req = tuning.graze_per_creature * static_cast<float>(n_in_range);
            const float before = clampBiomass(gz.biomass);
            const float after = clampBiomass(before - removed_req);
            gz.biomass = after;
            gz.last_grazed_tick = static_cast<std::uint32_t>(tick);
            stats.biomass_removed += (before - after);
            ++stats.grazed_plants;
        } else {
            // Ungrazed: regrow slowly toward the cap.
            const float before = clampBiomass(gz.biomass);
            const float after = clampBiomass(before + tuning.regrow_per_tick);
            gz.biomass = after;
            stats.biomass_regrown += (after - before);
        }
    }

    // --- 4. Apply accumulated feed to creatures, id-ordered (lower hunger). ---
    // `grazers` is already id-ordered, so this pass is deterministic. Hunger clamps to
    // [0,1] (0 = sated). This only RUNS when grazeable plants exist (feed is otherwise
    // all-zero), preserving the no-op-when-no-plants gate.
    for (std::size_t gi = 0; gi < grazers.size(); ++gi) {
        if (feed[gi] <= 0.0f) continue;
        auto& cr = reg.get<Comp::CreatureComponent>(grazers[gi].e);
        float h = cr.hunger - feed[gi];
        if (h < 0.0f) h = 0.0f;
        cr.hunger = h;
    }

    return stats;
}

} // namespace luminumbra::ai
