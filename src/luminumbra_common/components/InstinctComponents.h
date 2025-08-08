#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include <vector>
#include <string>

namespace Luminumbra::Components {

// --- Core AI Data ---

// Represents the fundamental physiological and psychological needs of an AI agent.
// Values are typically normalized between 0.0 (satisfied) and 1.0 (critical).
struct NeedsComponent {
    f32 hunger = 0.5f;
    f32 thirst = 0.5f;
    f32 fatigue = 0.0f;
    f32 safety = 1.0f;
    f32 curiosity = 0.1f;
};

// Represents what an AI agent is currently perceiving in the world.
// This is populated by sensory systems (vision, hearing, etc.).
struct SensesComponent {
    EntityID nearest_food_source = entt::null;
    EntityID nearest_water_source = entt::null;
    EntityID nearest_threat = entt::null;
    f32 threat_distance = -1.0f;
};

// --- Action & Planning ---

// Represents a single, atomic action that an agent can perform.
// This is the building block for the GOAP (Goal-Oriented Action Planning) system.
struct Action {
    std::string name; // e.g., "MoveTo", "Eat", "Flee"
    EntityID target = entt::null;
    f32 duration = 1.0f; // Time in seconds to complete the action
};

// Holds the sequence of actions an agent will perform to satisfy a goal.
// This is the output of the GOAP planner.
struct ActionPlanComponent {
    std::vector<Action> plan;
    u32 current_action_index = 0;
    f32 time_in_current_action = 0.0f;
};

// A tag component to identify an entity as being controlled by the Instinct Engine.
struct InstinctAgent {};

} // namespace Luminumbra::Components
