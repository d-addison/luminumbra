# GOAP-driven NPC locomotion — spec (T-I6 multiplayer polish)

## Problem
`InstinctSystem` (the GOAP planner tick) ranks opportunities and writes the
winning `Action` into each agent's `ActionPlanComponent` — but **nothing
executes that plan as motion**. Server NPCs (`--npcs` in `main_server`) are real
Jolt `CharacterVirtual` capsules driven by a *hardcoded circle-wander* wish
velocity. The AI "decides" to go to water/food and then walks in a meaningless
circle. This closes the action→locomotion gap so a planned action actually
steers the NPC.

## Research grounding
- **Steering behaviours** (Reynolds, GDC 1999, *Steering Behaviors for
  Autonomous Characters*): "seek" = desired velocity toward target at max speed;
  "arrival" = seek with a stopping radius inside which speed ramps to zero. This
  is the canonical, minimal execution model for "move to the thing the planner
  chose," and it composes with the existing physics character (it produces a
  *desired velocity*, the character integrates it).
- **GOAP execution** (Orkin, *Three States and a Plan: the A.I. of F.E.A.R.*,
  GDC 2006): the planner emits an ordered action list; a separate, dumb executor
  advances actions and drives effectors. We mirror that split — the planner stays
  pure ranking; a new *locomotion* system is the effector for the move portion.
- Determinism: `core/DeterministicMath.h` contract — sim math is float-only,
  `-ffp-contract=off`, libm transcendentals banned by `SimDeterminismLint`.
  Steer/arrival needs only vector subtract, `Sqrt` (IEEE-correctly-rounded →
  allowed), and divide. **No trig required**, so the path is deterministic by
  construction; NPC-lane `run==replay` is preserved and the default (0-NPC)
  lane stays byte-identical.

## Design (engine-generic, no action-name vocabulary)
The engine must not hardcode action strings ("MoveTo"/"Flee"). The generic rule:
**an agent steers toward the `TransformComponent` of its current action's
`target`, at a data-driven speed, and holds once within an arrival radius.**
"Approach the opportunity you planned for" is exactly GOAP execution for the
dominant case (seek food/water/shelter). Avoidance (flee) is a later, explicitly
data-flagged extension — out of scope for this cut.

### New components (`components/InstinctComponents.h`)
```cpp
// GAME DATA: steering params for an agent that physically moves toward its plan.
struct LocomotionProfile {
    f32 move_speed = 2.0f;      // m/s horizontal cruise
    f32 arrival_radius = 1.5f;  // m; inside this the agent is "arrived" and holds
    f32 slow_radius = 4.0f;     // m; linear speed ramp begins here (arrival)
};

// ENGINE OUTPUT: the per-tick horizontal wish velocity the executor produced.
// The server maps this onto set_avatar_wish_velocity(char_index, wish_xz).
struct LocomotionIntentComponent {
    glm::vec2 wish_xz{0.0f};
    bool      arrived = false;
};
```

### New system (`ai/InstinctLocomotionSystem.{h,cpp}`, `luminumbra::ai`)
```cpp
struct InstinctLocomotionTickStats {
    std::uint64_t agents_steered = 0;   // produced a non-zero wish this tick
    std::uint64_t agents_arrived = 0;   // within arrival_radius of target
    std::uint64_t agents_idle = 0;      // no plan / no positioned target
};
InstinctLocomotionTickStats RunInstinctLocomotionOnTick(entt::registry&);
```
Per agent carrying `ActionPlanComponent` + `TransformComponent` +
`LocomotionProfile` (iteration ordered by entity to match the existing
deterministic sort discipline):
1. Resolve the current action (`plan[current_action_index]`). If no plan or the
   target is null / has no `TransformComponent` → write zero intent, `idle++`.
2. `to = target.pos.xz - agent.pos.xz`; `dist = Sqrt(to.x^2 + to.z^2)`
   (DeterministicMath::Sqrt).
3. If `dist <= arrival_radius` → `wish = 0`, `arrived = true`, `arrived++`
   (and advance `current_action_index` / reset timer so the plan progresses).
4. Else `speed = move_speed * (dist >= slow_radius ? 1 : dist/slow_radius)`
   (arrival ramp); `wish = (to/dist) * speed`; `steered++`.
Pure: reads transforms, writes only `LocomotionIntentComponent`. No physics, no
RNG, no time input (speed is per-tick cruise; physics applies `dt`).

### Server wiring (`main_server.cpp`, `--npcs` path only)
Replace the circle-wander. Each NPC gets: an `OpportunityComponent` target
(e.g. a water/feed point near spawn) + `InstinctAgent`/`InstinctAgentComponent`
+ `NeedsComponent` + `LocomotionProfile`. Per fixed tick: run the planner
(already wired), run `RunInstinctLocomotionOnTick`, then for each NPC map
`registry.get<LocomotionIntentComponent>(e).wish_xz` →
`npc_physics->set_avatar_wish_velocity(char_index, wish)` before
`update_avatars(dt)`. Default lane has zero NPCs → unchanged.

## Determinism / world_hash
- `luminumbra_common` only; float-only; `Sqrt`-only → `SimDeterminismLint` clean.
- Default lane (0 NPCs, 0 avatars) spawns no agent with these components →
  planner/locomotion tick path byte-unchanged → `world_hash` stays
  `f17726d44054d133` (P1 default-lane baseline).
- NPC lane: positions now driven by planned steering instead of a circle. The
  NPC entities sub-hash CHANGES vs the old wander, but is itself deterministic
  (`run==replay`), which is the contract for a non-default lane.

## Tests (TDD, `test/common/InstinctLocomotion_test.cpp`)
1. **Seek**: agent far from target → wish points toward target, |wish| == move_speed.
2. **Arrival ramp**: agent inside slow_radius → 0 < |wish| < move_speed, monotonic in dist.
3. **Arrived/hold**: agent within arrival_radius → wish == 0, arrived == true, action index advanced.
4. **No target / no plan**: zero intent, idle counted, no throw.
5. **Determinism**: identical registry state two runs → byte-identical intents
   (and a multi-tick integrate matches run==replay).
6. **Ordering independence**: insertion order permuted → identical per-entity intent.

## Out of scope (follow-ups)
Flee/avoidance (data-flagged), multi-step path actions, obstacle avoidance,
chunk-index AOI, prune-into-tick despawn.
