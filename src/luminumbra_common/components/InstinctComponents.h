#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "../ai/InstinctPlanner.h"
#include "../ai/StimulusChannels.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Luminumbra::Components {

// --- Core AI Data ---

// A single named need (T-I3-17 migration: the old fixed-field struct
// (hunger/thirst/fatigue/safety/curiosity) became a data-driven named list —
// game archetypes under data/ decide which needs exist; the engine never
// hardcodes a need vocabulary). Pressure is normalized: 0.0 satisfied,
// 1.0 critical. growth_per_tick is applied by the InstinctSystem on each
// fixed simulation tick (30 Hz) and clamps to [0, 1].
struct Need {
    std::string name;
    f32 pressure = 0.0f;
    f32 growth_per_tick = 0.0f;
};

struct NeedsComponent {
    std::vector<Need> needs;
};

// Represents what an AI agent is currently perceiving in the world.
// This is populated by sensory systems (vision, hearing, etc.).
struct SensesComponent {
    EntityID nearest_food_source = entt::null;
    EntityID nearest_water_source = entt::null;
    EntityID nearest_threat = entt::null;
    f32 threat_distance = -1.0f;
};

// --- Opportunities (planner inputs scattered in the world) ---

// An entity advertising an action an instinct agent could take (a food
// source, a shelter, a point of interest...). Values are game data; the
// engine only scores them. When radius > 0 and both the agent and the
// opportunity carry transforms, agents outside the radius do not consider
// the opportunity.
struct OpportunityComponent {
    std::string id;
    std::string action;
    std::string target;
    std::string need;
    f32 satisfaction = 0.0f;
    f32 urgency = 0.0f;
    f32 risk = 0.0f;
    f32 stamina_cost = 0.0f;
    f32 radius = 0.0f; // 0 = unbounded
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

// T-I5b-2 (E1): GAME-DATA opt-in for the ecology stimulus channels. A creature
// that carries this component REACTS to the environment: each subscription maps
// a stimulus channel onto a named need, scaling the need's pressure by the
// channel's [0, 1] scalar times `gain` every replan tick (clamped to [0, 1]).
//
// This is the INERT switch (critique F1 / design-decisions §0). The InstinctSystem
// touches the stimulus registry ONLY for entities that carry this component. The
// canonical world (the HeadlessServerTick default roster) spawns NO entity with a
// StimulusSubscriptionComponent, so the default planner tick path is byte-
// unchanged and world_hash stays `d950a6afc12a5cdc`. The engine names no channel-
// to-need mapping; the archetype JSON does.
struct StimulusSubscription {
    luminumbra::ai::StimulusChannel channel = luminumbra::ai::StimulusChannel::Weather;
    std::string need;     // the named need this channel drives (game data)
    f32 gain = 1.0f;      // scale applied to the channel scalar before adding to pressure
};

struct StimulusSubscriptionComponent {
    std::vector<StimulusSubscription> subscriptions;
};

// Identity + planner state for an instinct agent (T-I3-17). The
// InstinctSystem replans every replan_interval_ticks fixed ticks and stores
// the full deterministic ranking here; the winner is also written into
// ActionPlanComponent.
struct InstinctAgentComponent {
    std::string actor_id;
    std::string archetype;
    std::uint32_t replan_interval_ticks = 10; // ~3 Hz at the 30 Hz tick
    std::uint64_t last_planned_tick = 0;
    std::uint64_t plans_executed = 0;
    luminumbra::ai::InstinctPlan current_plan;
};

} // namespace Luminumbra::Components
