#pragma once

//   ( germination): the per-plant LIFECYCLE state that turns growth into a
// GENERATIONAL loop. A plant grows (PlantGrowthSystem) to the terminal Fruiting stage, ripens for
// `lifespan_ticks`, then SENESCES: an ANNUAL dies and drops a germinated seed; a PERENNIAL resets
// to a vegetative stage to fruit again next season — both reseed a child from the pollination cross
// (PollinationComponent.next_genome) or self. CropLifecycleSystem owns the transition.
//
// LOAD-BEARING RULE (foliage system): SIM truth — small, integer, deterministic; never render
// state. OPT-IN: a plant participates ONLY if it carries this component, so a world with none runs
// the lifecycle system as a no-op and the canonical world_hash baseline is byte-identical.

#include <cstdint>

namespace Luminumbra::Components {

struct CropLifecycleComponent {
    bool perennial = false;              // false = annual (die + reseed); true = reset + regrow
    std::uint32_t ripe_ticks = 0;        // ticks spent at the terminal (Fruiting) stage
    std::uint32_t lifespan_ticks = 1200; // ripe ticks before senescence (death / reset)
    std::uint16_t generations = 0;       // reseed/reset count (telemetry)
    std::uint16_t species_id = 0;        // carried to germinated children
};

} // namespace Luminumbra::Components
