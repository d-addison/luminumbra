#pragma once

// sim.scavenging: SCAVENGING participant component. Ties DEATH -> FOOD: a creature
// carrying this component (alongside a CreatureComponent) is a SCAVENGER that, when hungry,
// seeks out and EATS CARCASSES (creatures that are dead — CreatureComponent.eaten==1, or
// MortalComponent.dead==1). This complements active hunting: predators make carcasses, the
// dead of old age/starvation make carcasses, and scavengers convert those carcasses back
// into sated hunger, closing the death->food loop.
//
// The presence of this component is the per-entity OPT-IN for
// luminumbra::ai::RunScavengingOnTick (mirrors the PlantTag / CreatureGenomeComponent /
// AlarmComponent / MortalComponent gating pattern): a world whose creatures carry NO
// ScavengerComponent — and any world with NO creatures — runs the scavenging system as a
// PURE no-op, so the canonical headless NetworkStateHash baseline stays byte-identical
// until scavengers are deliberately spawned.
//
// LOAD-BEARING RULE: this is simulation truth — small,
// integer/fixed-point-friendly, deterministic — never render state. The scavenger's desire
// to MOVE toward food is written here as a wish vector; the orchestrator blends it into
// actual movement (the same shape as CreatureComponent.wish_x/z). Geometry / feeding VFX
// is derived elsewhere.

#include <cstdint>

namespace Luminumbra::Components {

// Per-entity scavenger marker + sim bookkeeping. POD; copies/hashes trivially.
//
// `wish_x`/`wish_z` are the horizontal direction the scavenger WANTS to move this tick to
// reach the nearest carcass (a unit-ish steer toward the target; 0,0 = no target / hold).
// The orchestrator blends this into the creature's movement (it does NOT own position).
// `feeding` latches to 1 on any tick the scavenger is within feed radius of a carcass and
// is actively lowering its hunger (telemetry / feeding-pose cue); 0 otherwise.
struct ScavengerComponent {
    float wish_x = 0.0f;      // desired x steer toward nearest carcass (0 = none)
    float wish_z = 0.0f;      // desired z steer toward nearest carcass (0 = none)
    std::uint8_t feeding = 0; // 1 = feeding on a carcass this tick, 0 = not
};

} // namespace Luminumbra::Components
