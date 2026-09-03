#pragma once

// sim.lifespan: MORTALITY participant component. Closes the
// birth -> life -> death cycle: a creature carrying this component AGES every
// tick and is MARKED dead when it outlives its lifespan (old age) or, if it is
// also a CreatureComponent, when it has starved (hunger maxed). Bounding death
// keeps the population finite so reproduction can't grow without limit.
//
// LOAD-BEARING RULE: this is simulation truth —
// small, integer/fixed-point, deterministic — never render state. Geometry /
// carcass rendering is derived elsewhere.
//
// GATING: an entity participates in the lifespan tick ONLY if it carries this
// component. A registry with NONE of these => RunLifespanOnTick does nothing,
// so the canonical NetworkStateHash baseline stays byte-identical until
// mortal entities are deliberately spawned. (Same opt-in shape as PlantTag /
// CreatureGenomeComponent / SoilFeederComponent.)
//
// This is intentionally SEPARATE from CreatureGenomeComponent.age_ticks (which
// drives reproduction maturity): keeping mortality on its own additive
// component means lifespan is fully testable on ANY entity — not just
// creatures with a genome — and the four parallel tracks stay conflict-free.

#include <cstdint>

namespace Luminumbra::Components {

// Per-entity mortality marker + sim bookkeeping. POD; copies/hashes trivially.
//
// `age_ticks` is an independent integer age clock (monotonic, saturating). When
// it reaches `lifespan_ticks` the entity dies of old age. `dead` is the latched
// death flag (0 = alive, 1 = dead) — once set it never clears; the system stops
// aging a dead entity so its age and death are stable for run==replay.
struct MortalComponent {
    std::uint32_t age_ticks = 0;                // ticks lived (monotonic, saturating)
    std::uint32_t lifespan_ticks = 0xFFFFFFFFu; // dies of old age at this age (default: ~never)
    std::uint8_t dead = 0;                      // 0 = alive, 1 = dead (latched)
};

} // namespace Luminumbra::Components
