#pragma once

// InstinctSystem — runs the (already-generic) InstinctPlanner over
// the EnTT registry on the fixed 30 Hz simulation tick (slot 2 of the
// deterministic tick order, the deterministic runtime contract , after animation pose
// sampling).
//
// Per tick:
//   1. Need growth: every InstinctAgentComponent + NeedsComponent entity has
//      each need's pressure advanced by growth_per_tick (clamped to [0, 1]).
//   2. Replanning: agents whose replan interval elapsed (or that never
//      planned) gather OpportunityComponent entities — sorted by opportunity
//      id so iteration order never depends on registry internals — compute
//      agent-to-opportunity distance when both carry TransformComponent
//      (rounded to 1e-4 for cross-run stability), respect the opportunity
//      radius, call PlanInstincts, store the ranking in the agent and write
//      the winning action into ActionPlanComponent.
//
// The system is pure engine: need names, actions, and targets are game data.
//
// an OPTIONAL ecology stimulus-channel context may be supplied. It
// is consumed ONLY for entities that carry a StimulusSubscriptionComponent (game-
// data opt-in); for those creatures each subscribed channel scalar modulates the
// mapped need's pressure before planning. The default roster carries no
// subscription, so passing nullptr (the default) leaves the tick path BYTE-
// UNCHANGED and world_hash stays `d950a6afc12a5cdc` (regression review / ).

#include <cstdint>

#include "entt/entt.hpp"

namespace luminumbra::ai {

class StimulusChannelRegistry;

struct InstinctSystemTickStats {
    std::uint64_t agents_seen = 0;
    std::uint64_t agents_replanned = 0;
    std::uint64_t opportunities_considered = 0;
    //  how many subscribing creatures had a channel modulate a need this
    // tick (0 for the canonical roster -- the registry stays inert).
    std::uint64_t stimulus_subscribers_applied = 0;
};

// `stimulus` is OPTIONAL: when null (default), no stimulus-channel work runs and
// the planner tick path is byte-identical to the pre- behavior. When non-
// null, ONLY entities carrying a StimulusSubscriptionComponent sample channels.
//
// `use_perception_substrate` defaults true: the shared substrate is the canonical
// path; false selects the retained inline scan
// (kept only as the byte-identical regression reference, proven equivalent by test).
// When false, the per-agent opportunity gather runs the original inline scan and
// this tick path is byte-identical. When true, the gather routes through the
// shared PerceptionField (PerceptionSubstrate.h) — a spatially-bucketed scan that
// produces the SAME distance-gated opportunity set in the SAME id order (proven
// equivalent by test/ai/perception_substrate_test.cpp), so the resulting plan is
// unchanged. The flag exists so the shared substrate can be exercised without
// touching the default sim trajectory; world_hash is untouched either way (the
// canonical roster carries no InstinctAgentComponent).
InstinctSystemTickStats RunInstinctSystemOnTick(entt::registry& registry,
                                                std::uint64_t tick,
                                                const StimulusChannelRegistry* stimulus = nullptr,
                                                bool use_perception_substrate = true);

} // namespace luminumbra::ai
