#pragma once

// sim.migration: SEASONAL MIGRATION. The presence of this component is the per-entity
// OPT-IN gate for luminumbra::ai::RunMigrationOnTick (mirrors CreatureGenomeComponent /
// AlarmComponent): a world whose creatures carry NO MigratoryComponent — and any world with no
// creatures — runs the migration system as a pure no-op, so the canonical NetworkStateHash
// baseline stays byte-identical and existing brain/movement behaviour is unchanged.
//
// All fields are SIM state (deterministic floats). `drive` is the per-tick migration urgency
// (0 = settled mid-season .. 1 = peak migration at a season transition); `wish_x`/`wish_z` are
// the world-XZ wish DIRECTION (already scaled by drive) the migration system writes each tick.
// The orchestrator BLENDS this wish into the creature's movement (it does not move the entity
// itself), so this header has no dependency on the ai/ or physics layers. Defaults are inert
// (drive 0, zero wish), so a default-stamped participant contributes nothing until the system
// computes a real seasonal drive.

namespace Luminumbra::Components {

struct MigratoryComponent {
    // Migration urgency this tick: 0 settled (mid-season) .. 1 peak (spring/autumn transition).
    // Written by RunMigrationOnTick from the seasonal-transition curve; read by the orchestrator
    // as the blend weight for the migration wish.
    float drive = 0.0f;

    // Desired horizontal migration direction toward the current seasonal target, ALREADY scaled
    // by `drive` (so magnitude ~= drive when a target is in play, 0 when settled). World XZ.
    float wish_x = 0.0f;
    float wish_z = 0.0f;
};

}  // namespace Luminumbra::Components
