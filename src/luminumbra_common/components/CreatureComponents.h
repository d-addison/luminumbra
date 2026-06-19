#pragma once

// I9-ECO: creature state for the Utility-AI brain (ai/CreatureBrainSystem.h). The presence
// of this component is the per-entity opt-in (like PlantTag): a world with no creatures runs
// the brain system as a no-op, so the canonical headless world_hash stays byte-identical.
// All fields are sim state (integer-ish floats, deterministic); geometry/rendering is separate.

#include <cstddef>

namespace Luminumbra::Components {

struct CreatureComponent {
    bool is_predator = false;   // role: predators hunt prey; prey flee predators + graze
    float hunger = 0.0f;        // 0 sated .. 1 starving (grows each tick)
    float stamina = 1.0f;       // 0 exhausted .. 1 fresh
    float move_speed = 3.0f;    // m/s cruise
    int last_action = 0;        // last CreatureAction chosen (telemetry / sub-hash)
    // The brain's desired HORIZONTAL velocity (m/s) this tick. When the creature has a
    // CreaturePhysicsComponent the brain writes this and the Jolt character controller owns
    // the resulting position (gravity / terrain collision / slopes); otherwise the brain
    // integrates position directly (the pure, unit-tested path).
    float wish_x = 0.0f;
    float wish_z = 0.0f;
};

// I9-ECO + physics: opt-in TRUE-PHYSICS locomotion. When present, the creature is driven by
// a deterministic Jolt CharacterVirtual (the same controller the player/networked avatars
// use): the brain produces a wish velocity, the avatar resolves it against the terrain
// heightfield (gravity, ground-stick, 50deg max slope), and the resolved position is read
// back into the TransformComponent. avatar_index is the slot in PhysicsSystem's avatar pool.
struct CreaturePhysicsComponent {
    std::size_t avatar_index = 0;
};

}  // namespace Luminumbra::Components
