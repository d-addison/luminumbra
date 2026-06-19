#pragma once

// I9-ECO: creature state for the Utility-AI brain (ai/CreatureBrainSystem.h). The presence
// of this component is the per-entity opt-in (like PlantTag): a world with no creatures runs
// the brain system as a no-op, so the canonical headless world_hash stays byte-identical.
// All fields are sim state (integer-ish floats, deterministic); geometry/rendering is separate.

namespace Luminumbra::Components {

struct CreatureComponent {
    bool is_predator = false;   // role: predators hunt prey; prey flee predators + graze
    float hunger = 0.0f;        // 0 sated .. 1 starving (grows each tick)
    float stamina = 1.0f;       // 0 exhausted .. 1 fresh
    float move_speed = 3.0f;    // m/s cruise
    int last_action = 0;        // last CreatureAction chosen (telemetry / sub-hash)
};

}  // namespace Luminumbra::Components
