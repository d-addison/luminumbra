#pragma once

//  multiplayer polish: InstinctLocomotionSystem — the GOAP *executor* for the
// move portion of a plan. InstinctSystem (the planner) ranks opportunities and
// writes the winning Action into ActionPlanComponent but never moves the agent.
// This system converts that action into a horizontal WISH VELOCITY intent using a
// seek + arrival steering model (Reynolds, GDC 1999): steer toward the target's
// TransformComponent at LocomotionProfile.move_speed, ramping down inside
// slow_radius and holding once inside arrival_radius.
//
// PURE + DETERMINISTIC. Reads TransformComponent of agent + target; writes only
// LocomotionIntentComponent. Steering math is float-only and uses DeterministicMath::Sqrt
// (IEEE-correctly-rounded — the one transcendental the determinism contract
// allows on a sim path); no libm transcendentals, so SimDeterminismLint stays
// clean and run==replay holds. Agents are visited in a deterministic id order so
// the result never depends on registry storage internals.
// A per-tick XZ grid selects candidates using checked double-to-integer cell
// coordinates; candidates are sorted back into id order before steering math.
// Before gathering or allocating, queries exceeding N / max(2, ceil(log2 N))
// candidates switch to an exhaustive id-ordered scan. This bounds gathering
// and sorting to O(N) per query, including near-dense rosters. Each query examines
// at most N entries, with no double visits: O(N squared) per enabled pass per tick
// (two passes with separation and flocking enabled), as in the exhaustive path.
//
// Engine-generic: the engine hardcodes NO action-name vocabulary. The rule is
// "approach the target of the planned action, hold on arrival" — exactly GOAP
// execution for the dominant seek-food/water/shelter case. Avoidance (flee) is a
// later, explicitly data-flagged extension.

#include "../systems/RegionConsumerGate.h"
#include <cstdint>

#include "entt/entt.hpp"

namespace luminumbra::ai {

struct InstinctLocomotionTickStats {
    std::uint64_t agents_seen = 0;    // agents with profile + transform + plan considered
    std::uint64_t agents_steered = 0; // produced a non-zero wish toward a target this tick
    std::uint64_t agents_arrived = 0; // within arrival_radius of the target (holding)
    std::uint64_t agents_idle = 0;    // no plan / no positioned target -> zero wish
    // Counts entries visited by separation and flock queries, including self,
    // before the exact distance test. Each pass counts its candidates separately.
    std::uint64_t neighbor_candidates_examined = 0;
    // Queries that scan the full snapshot without gathering or sorting indices.
    std::uint64_t neighbor_exhaustive_queries = 0;
};

// For every entity carrying ActionPlanComponent + TransformComponent +
// LocomotionProfile, compute the seek/arrival wish velocity toward the current
// action's target and store it in LocomotionIntentComponent (created if absent).
// On arrival the current action index is advanced so the plan can progress.
InstinctLocomotionTickStats
RunInstinctLocomotionOnTick(entt::registry& registry,
                            const Luminumbra::world::RegionEntityGate& gate = {},
                            const Luminumbra::world::RegionEntityWork& completed = {});

} // namespace luminumbra::ai
