#pragma once

// sim.irrigation: WATER SOURCE participant component.
//
// LOAD-BEARING RULE (mirrors the foliage system / SoilComponents.h): this is SIM
// truth — small, integer/fixed-point, deterministic — never render state. The
// soil-MOISTURE field (systems/IrrigationSystem.h) is a diffusion grid that a
// dug channel / spring DEPOSITS into; this component is the per-entity OPT-IN +
// per-tick output the irrigation tick reads.
//
// GATING: the irrigation tick only DEPOSITS for entities carrying this component
// (a dug channel / spring at a world position). A registry with NONE of these =>
// the grid receives no input and simply DRAINS toward its dry baseline; if it
// started at baseline it STAYS at baseline (a true no-op), so the canonical
// NetworkStateHash baseline stays byte-identical until water sources are
// deliberately spawned. (Same opt-in shape as PlantTag / SoilFeederComponent /
// CreatureGenomeComponent.)

#include <cstdint>

namespace Luminumbra::Components {

// Per-source water emitter. POD; copies/hashes trivially.
//
// `output` is a fixed-point flow STRENGTH (milli-units) the source adds to its
// cell's moisture each tick (a spring trickle vs an open channel). The grid then
// diffuses + drains that moisture deterministically. Kept here, NOT folded onto
// an existing component, so the parallel sim tracks stay conflict-free.
struct WaterSourceComponent {
    std::uint16_t output = 1000;     // fixed-point per-tick moisture output (milli-units), default 1.0
};

} // namespace Luminumbra::Components
