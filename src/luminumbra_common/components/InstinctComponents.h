#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "../ai/Awareness.h"
#include "../ai/InstinctPlanner.h"
#include "../ai/Perception.h"
#include "../ai/StimulusChannels.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Luminumbra::Components {

// --- Core AI Data ---

// A single named need ( migration: the old fixed-field struct
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
    //  flee/avoidance flag (engine-generic: a movement-DIRECTION modifier,
    // not keyed on `name`). false (default) = seek toward target (unchanged). true
    // = move AWAY from target until beyond slow_radius (the "safe" distance), then
    // the action completes. Default false keeps every existing plan byte-identical,
    // so world_hash is unchanged.
    bool flee = false;
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

// --- Locomotion (GOAP action execution) ---

// GAME DATA: steering parameters for an agent that physically moves toward the
// target of its current ActionPlanComponent action. The engine reads these; the
// archetype/spawn decides the values. ( multiplayer polish: action->steering.)
struct LocomotionProfile {
    f32 move_speed = 2.0f;     // m/s horizontal cruise speed
    f32 arrival_radius = 1.5f; // m; within this the agent is "arrived" and holds
    f32 slow_radius = 4.0f;    // m; linear arrival speed-ramp begins at this range
    //  obstacle/crowd avoidance (Reynolds separation). DATA-FLAGGED OFF by
    // default: separation_strength == 0 makes the avoidance term a no-op, so the
    // canonical headless roster (which leaves these at 0) is byte-identical and
    // world_hash is unchanged. Set > 0 per-archetype to keep agents from piling
    // up on a shared target at scale (the 20-48-agent multiplayer scenes).
    f32 separation_radius = 0.0f;   // m; neighbors within this push the agent away
    f32 separation_strength = 0.0f; // 0 = off; >0 scales the avoidance vs the seek
    //  flocking/herding: the other two Reynolds rules. Cohesion steers
    // toward the local group's center of mass; alignment matches the group's mean
    // heading. Both use flock_radius as the perception neighborhood and are
    // DATA-FLAGGED OFF (strength 0) so the canonical roster stays byte-identical
    // and world_hash is unchanged. Set all three (separation+cohesion+alignment)
    // for true flocking; cohesion alone reads as loose herding.
    f32 flock_radius = 0.0f;       // m; perception radius for cohesion + alignment
    f32 cohesion_strength = 0.0f;  // 0 = off; pull toward neighbor center of mass
    f32 alignment_strength = 0.0f; // 0 = off; match neighbor mean heading
};

// marks an entity OTHERS can sense (prey, predator, herd-mate). Pure
// game data — what it emits to each sense modality. Default emits nothing.
struct SensableComponent {
    int scent_channel = -1;    // ScentField channel this entity deposits into (-1 none)
    f32 scent_deposit = 0.0f;  // amount laid into scent_channel per tick (0 = none)
    f32 noise_loudness = 0.0f; // current emitted sound loudness (0 = silent)
    f32 noise_pitch = 0.5f;    // emitted sound pitch [0,1] (footstep low.. alarm high)
    std::uint32_t faction = 0; // perceivers sense entities of OTHER factions
};

// a perceiver's senses. Vision FOV is stored as cos(half-angle) so the
// per-tick cone test is libm-free (set from a degrees gene once at spawn). facing
// is the unit heading the cone points down (updated from locomotion). All fields
// are evolvable genes. DATA-FLAGGED by presence: an entity without this component
// (the canonical headless roster has none) is unaffected -> world_hash unchanged.
struct PerceptionComponent {
    f32 vision_cos_half_fov = 0.5f; // cos(half FOV); 0.5 ~= 120 deg total
    f32 vision_range = 20.0f;       // m
    f32 facing_x = 1.0f;            // unit heading the vision cone faces
    f32 facing_z = 0.0f;
    luminumbra::ai::HearingProfile ear; // hearing audiogram (range/peak/bandwidth/gain/threshold)
    std::uint32_t faction = 0;          // this perceiver's faction (senses OTHER factions)
};

// per-agent fused awareness (the detection meter + state + last-known).
struct AwarenessComponent {
    luminumbra::ai::Awareness awareness;
    luminumbra::ai::AwarenessParams params;
};

// ENGINE OUTPUT: the horizontal wish velocity produced by InstinctLocomotionSystem
// from the agent's current action. The physics owner (server NPC tick) maps wish_xz
// onto set_avatar_wish_velocity(char_index, wish_xz) before stepping the character.
// Pure data; no physics dependency lives in the common AI layer.
struct LocomotionIntentComponent {
    Vec2 wish_xz{0.0f};
    bool arrived = false;
};

// smell-driven steering. A creature with this senses ScentField channel
// `channel` and biases its locomotion wish along the gradient: sign +1 = TRACK
// toward the source (predator hunting prey scent / ant following a food trail),
// sign -1 = FLEE down it (prey avoiding predator scent). `strength` scales the bias
// vs the seek wish; `floor` is the cold-trail cutoff; `k` the Weber half-confidence.
// DATA-FLAGGED by presence — no component (canonical roster) => unchanged.
struct ScentSenseComponent {
    int channel = -1;    // ScentField channel to follow (-1 = inactive)
    f32 sign = 1.0f;     // +1 track toward source, -1 flee
    f32 strength = 1.0f; // bias magnitude vs cruise speed
    f32 floor = 0.0f;    // min gradient magnitude to act (tracking-window cutoff)
    f32 weber_k = 1.0f;  // Weber half-confidence gradient magnitude
};

//  path-following: an optional precomputed route (e.g. from grid A*, see
// AStarGrid.h) the locomotion executor walks before falling back to the direct
// action-target seek. Waypoints are WORLD-space XZ. DATA-FLAGGED by presence: an
// agent WITHOUT this component (the canonical roster) is byte-identical and
// world_hash is unchanged. The executor seeks waypoint[index], advances index on
// arrival, and once the path is exhausted resumes the plain action-target seek so
// the agent still finishes onto its goal.
struct LocomotionPathComponent {
    std::vector<Vec2> waypoints; // world-space XZ route, start..goal
    std::uint32_t index = 0;     // current waypoint being sought
};

// GAME-DATA opt-in for the ecology stimulus channels. A creature
// that carries this component REACTS to the environment: each subscription maps
// a stimulus channel onto a named need, scaling the need's pressure by the
// channel's [0, 1] scalar times `gain` every replan tick (clamped to [0, 1]).
//
// This is the INERT switch (regression review / documented design). The InstinctSystem
// touches the stimulus registry ONLY for entities that carry this component. The
// canonical world (the HeadlessServerTick default roster) spawns NO entity with a
// StimulusSubscriptionComponent, so the default planner tick path is byte-
// unchanged and world_hash stays `d950a6afc12a5cdc`. The engine names no channel-
// to-need mapping; the archetype JSON does.
struct StimulusSubscription {
    luminumbra::ai::StimulusChannel channel = luminumbra::ai::StimulusChannel::Weather;
    std::string need; // the named need this channel drives (game data)
    f32 gain = 1.0f;  // scale applied to the channel scalar before adding to pressure
};

struct StimulusSubscriptionComponent {
    std::vector<StimulusSubscription> subscriptions;
};

// Identity + planner state for an instinct agent. The
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
