#pragma once

// sim.pollination — the SIMULATION-SIDE pollination opt-in + result state.
//
// A plant field DRIFTS genetically over seasons because flowering plants
// cross-pollinate their neighbours: a receiver's NEXT-generation seed genome is
// a deterministic blend (the existing PlantGrowthSystem BreedPlants crossover) of
// itself and a pollen donor within range. This header carries ONLY the new,
// fully-additive state so the pollination tick never has to write any existing
// component:
//   * PollinationTag      — the per-entity opt-in (gating marker). A registry with
//                           none of these => the pollination tick is a no-op =>
//                           the canonical world_hash baseline is byte-identical.
//   * PollinationComponent — records the PENDING mixed genome a plant received this
//                           season. The farming/seeding loop (or a later wiring
//                           step) reads `next_genome` when the plant re-seeds; the
//                           live PlantGenomeComponent is left untouched so this
//                           system stays purely additive.
//
// LOAD-BEARING RULE (living-world foliage system): sim truth stays deterministic;
// the genome type itself is reused (Luminumbra::Components::PlantGenomeComponent),
// not reinvented.

#include <cstdint>

#include "PlantComponents.h" // PlantGenomeComponent (the heritable unit we mix)

namespace Luminumbra::Components {

// Opt-in marker. ONLY entities tagged with this participate in cross-pollination.
// Mirrors the PlantTag / CreatureGenomeComponent opt-in pattern so a roster with
// no pollinating plants leaves world_hash unperturbed.
struct PollinationTag {};

// The pending result of cross-pollination for one plant. `pollinated` is set the
// tick a donor's pollen reaches this plant; `next_genome` is the mixed genome the
// plant will pass to its next generation of seed. `last_pollen_tick` is the most
// recent tick this plant was pollinated (telemetry / replay aid). All state is a
// pure deterministic function of the participating plants + the tick.
struct PollinationComponent {
    bool pollinated = false;            // has a donor reached it yet?
    std::uint64_t last_pollen_tick = 0; // tick of the most recent cross
    std::uint32_t crosses = 0;          // how many times pollinated (telemetry)
    PlantGenomeComponent next_genome{}; // mixed genome for the next generation
};

} // namespace Luminumbra::Components
