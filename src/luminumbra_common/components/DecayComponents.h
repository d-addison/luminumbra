#pragma once

// sim.decomposition: DECAY / DECOMPOSITION participant component. Closes the
// death -> soil cycle: a creature (or any opted-in entity) that is DEAD slowly DECOMPOSES
// over a fixed number of ticks and RELEASES nutrient as it does so. The orchestrator routes
// the released nutrient into the soil grid during wiring (this component is sim truth only —
// it never touches the SoilGrid itself).
//
// LOAD-BEARING RULE: this is simulation truth —
// small, integer/fixed-point, deterministic — never render state. Any carcass-rot geometry
// is derived elsewhere from `decay_ticks` / `fully_decomposed`.
//
// GATING: an entity participates in the decomposition tick ONLY if it carries this component
// (the opt-in, same shape as MortalComponent / PlantTag / CreatureGenomeComponent). A
// registry with NONE of these => RunDecompositionOnTick does nothing, so the canonical
// NetworkStateHash baseline stays byte-identical until decaying entities are deliberately
// spawned.
//
// This is intentionally SEPARATE from MortalComponent: death is marked by MortalComponent.dead
// (or CreatureComponent.eaten); decomposition is the AFTER-death nutrient-release clock. Keeping
// it on its own additive component means decay is fully testable on ANY dead entity and the
// parallel tracks stay conflict-free.

#include <cstdint>

namespace Luminumbra::Components {

// Per-entity decomposition marker + sim bookkeeping. POD; copies/hashes trivially.
//
// `decay_ticks` counts ticks of decomposition elapsed (monotonic, saturating at
// `decay_duration`); it only advances while the entity is DEAD. `decay_duration` is how many
// ticks a full decomposition takes. `nutrient_released_milli` is the TOTAL nutrient released
// so far, in milli-units (fixed-point: 1000 = one full nutrient unit), accumulated in
// proportion to elapsed decay so it rises monotonically to kNutrientPerCorpseMilli by the end.
// `fully_decomposed` latches to 1 when `decay_ticks` reaches `decay_duration` — once set it
// never clears and the system stops advancing the entity (stable age/release for run==replay).
struct DecayComponent {
    std::uint32_t decay_ticks = 0;        // ticks of decomposition elapsed (saturating)
    std::uint32_t decay_duration = 900;   // ticks for a full decompose (~30s @ 30Hz)
    std::uint16_t nutrient_released_milli = 0;  // cumulative nutrient released (milli-units)
    std::uint8_t  fully_decomposed = 0;   // 0 = decomposing/intact, 1 = finished (latched)
};

} // namespace Luminumbra::Components
