#pragma once

// sim.wildlife_foliage: the ECOLOGY x FOLIAGE coupling participant component.
//
// LOAD-BEARING RULE (mirrors the foliage + soil subsystems): this is SIM truth — small,
// deterministic, replay-exact — never render state. A plant that opts in by carrying
// this component can be GRAZED/TRAMPLED down by nearby non-predator creatures and
// slowly REGROWS its biomass when left alone. The grazed/trampled accumulation lives
// HERE (a NEW component) rather than on the existing PlantGrowthComponent so the four
// parallel foliage tracks stay conflict-free (additive only).
//
// GATING: a plant participates in the wildlife-foliage tick ONLY if it carries this
// component. A registry with NONE of these => RunWildlifeFoliageOnTick does nothing
// (no biomass changes anywhere), so the canonical NetworkStateHash baseline stays
// byte-identical until grazeable plants are deliberately spawned. (Same opt-in shape
// as PlantTag / CreatureGenomeComponent / SoilFeederComponent.)
//
// DETERMINISM: biomass is updated only with deterministic float ops — constant
// integer-valued grazing/regrowth steps and a single sqrt distance (Luminumbra::
// DeterministicMath::Sqrt, IEEE-deterministic). With -ffp-contract=off (pinned on
// luminumbra_common) these add/sub/compare sequences are bit-stable across platforms,
// and grazing demand is SUMMED before being applied so the result is independent of
// creature traversal order. NO wall-clock / std::random / libm transcendentals.

#include <cstdint>

namespace Luminumbra::Components {

// Per-plant grazeable marker + sim bookkeeping. POD; copies/hashes trivially.
//
// `biomass` is the edible/standing biomass in [0, 1]: 1.0 = full lush plant, 0.0 =
// grazed/trampled to the ground. Creatures within graze range reduce it; when no
// creature grazes it on a tick it REGROWS slowly toward 1.0. `last_grazed_tick`
// records the most recent tick a creature fed on this plant (telemetry + lets the
// regrowth path tell "grazed this tick" from "ungrazed").
struct GrazeableComponent {
    float biomass = 1.0f;               // [0,1] standing biomass (1 = full, 0 = bare)
    std::uint32_t last_grazed_tick = 0; // last tick a creature grazed this plant
};

} // namespace Luminumbra::Components
