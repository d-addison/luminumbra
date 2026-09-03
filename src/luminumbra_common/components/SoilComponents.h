#pragma once

// sim.soil: SOIL NUTRIENT participant component.
//
// LOAD-BEARING RULE (mirrors the foliage system): this is SIM truth — small,
// integer/fixed-point, deterministic — never render state. The soil nutrient
// field (systems/SoilNutrientSystem.h) is consumed/produced by plants; this
// component is the per-plant OPT-IN + per-plant sim bookkeeping for that loop.
//
// GATING: a plant participates in the soil-nutrient tick ONLY if it carries this
// component (in addition to the foliage PlantTag). A registry with NONE of these
// => the soil system does nothing and the soil grid is never touched, so the
// canonical NetworkStateHash baseline stays byte-identical until soil-aware
// plants are deliberately spawned. (Same opt-in shape as PlantTag /
// CreatureGenomeComponent.)

#include <cstdint>

namespace Luminumbra::Components {

// Per-plant soil-feeder marker + sim bookkeeping. POD; copies/hashes trivially.
//
// `uptake` is a fixed-point STRENGTH (milli-units) scaling how aggressively this
// plant draws nutrient from its cell at full maturity; the per-tick draw is
// further scaled by the plant's current growth stage (a seed barely feeds, a
// fruiting plant feeds hard). `absorbed` accumulates the total nutrient this
// plant has taken (milli-units), a deterministic integer available to the
// farming/quality loop. Kept here, not on the
// existing PlantGrowthComponent, so the four parallel tracks stay conflict-free.
struct SoilFeederComponent {
    std::uint16_t uptake = 1000; // fixed-point uptake strength (milli-units), default 1.0
    std::uint32_t absorbed = 0;  // total nutrient drawn so far (milli-units, integer)
};

} // namespace Luminumbra::Components
