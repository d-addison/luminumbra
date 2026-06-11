#pragma once

// T-I3-17: InstinctSystem — runs the (already-generic) InstinctPlanner over
// the EnTT registry on the fixed 30 Hz simulation tick (slot 2 of the
// deterministic tick order, design-decisions.md §1, after animation pose
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

#include <cstdint>

#include "entt/entt.hpp"

namespace luminumbra::ai {

struct InstinctSystemTickStats {
    std::uint64_t agents_seen = 0;
    std::uint64_t agents_replanned = 0;
    std::uint64_t opportunities_considered = 0;
};

InstinctSystemTickStats RunInstinctSystemOnTick(entt::registry& registry, std::uint64_t tick);

} // namespace luminumbra::ai
