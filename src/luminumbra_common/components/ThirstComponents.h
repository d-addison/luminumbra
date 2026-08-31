#pragma once

// sim.thirst: CREATURE THIRST /  (complements hunger).
//
// A creature that carries a ThirstComponent gets THIRSTIER each tick. Above a threshold it
// wants to walk toward the nearest WATER SOURCE; the system writes that desire into
// wish_x/wish_z (a horizontal direction scaled by how thirsty it is) and the orchestrator
// blends it into the creature's movement. When the creature is INSIDE a water hole's radius
// it DRINKS — drinking is flagged and thirst falls back toward 0.
//
// GATING. Presence of this component is the per-entity opt-in (like the other ecology
// systems). A world whose creatures carry no ThirstComponent — and any world with no
// creatures — runs the thirst system as a pure no-op, so the canonical headless
// NetworkStateHash baseline stays byte-identical.
//
// DETERMINISM. All fields are plain floats updated by integer-ish, clamped float math
// (DeterministicMath::Sqrt for distance); no rng is consumed (the seed offset is reserved
// for collision-avoidance only). run==replay holds.

#include <cstdint>

namespace Luminumbra::Components {

// Per-creature thirst state. The orchestrator reads wish_x/wish_z each tick and blends the
// water-seeking direction into the creature's movement (mirrors CreatureComponent.wish_*).
struct ThirstComponent {
    // 0 quenched.. 1 parched. Rises each tick; drinking inside a water hole drops it.
    float thirst = 0.0f;
    // Desired HORIZONTAL move direction toward the nearest water hole, scaled by thirst.
    // Zero when the creature is not thirsty enough to seek (or there is no water hole).
    float wish_x = 0.0f;
    float wish_z = 0.0f;
    // 1 while the creature is within a water hole's radius and actively drinking; else 0.
    std::uint8_t drinking = 0;
    //  ( needs arbitration): nearness of the nearest known water hole in [0,1]
    // (1 = at it, 0 = none within the sensing horizon). Written by RunThirstOnTick;
    // read by the brain's Drink utility so thirst competes INSIDE the arbiter
    // instead of the old out-of-band additive wish blend.
    float water_proximity = 0.0f;
};

// A WATER SOURCE the thirst system steers creatures toward. Any entity carrying this plus a
// TransformComponent is a place creatures can drink; `radius` is how close (world units) a
// creature must get before it starts drinking.
struct WaterHoleComponent {
    float radius = 4.0f;
};

} // namespace Luminumbra::Components
