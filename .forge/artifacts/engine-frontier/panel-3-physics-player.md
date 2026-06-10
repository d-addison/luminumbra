## Subsystem State

- Physics is owned by `GameSession`: worlds create and start `PhysicsSystem` on create/load (`src/luminumbra_common/world/GameSession.cpp:194-197`, `src/luminumbra_common/world/GameSession.cpp:291-293`) and expose it through `GetPhysicsSystem` (`src/luminumbra_common/world/GameSession.h:61-64`).
- Runtime order is player update, then Jolt physics, then world streaming (`src/luminumbra_client/main_client.cpp:2350-2363`). This means newly streamed collision added by world update is not visible to the character until a later frame.
- Chunk collision is streaming-owned: ready LOD0 chunks get collision on the main thread with a hard cap of 16 bodies/frame (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:19`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:431-443`), removed on stream-out (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:621-627`), and also created by the initial near-surface readiness path (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:777-788`).
- The player controller currently has only `Walking` and `Noclip` modes (`src/luminumbra_client/player/PlayerController.h:18-21`). Jump, crouch, and camera eye height are client-side state layered over Jolt `CharacterVirtual` (`src/luminumbra_client/player/PlayerController.cpp:170-198`).
- Batched physics queries exist primarily for audio occlusion: audio queues close-source raycasts with distance-derived priority (`src/luminumbra_client/audio/AudioSpatialCluster.cpp:337-379`), and physics processes the queue after each Jolt update (`src/luminumbra_common/systems/PhysicsSystem.cpp:144-150`).

## Findings

- Determinism is not yet a contract. The main loop passes variable `deltaTime` into both player and physics (`src/luminumbra_client/main_client.cpp:2351-2353`), Jolt is stepped directly with that value and one collision step (`src/luminumbra_common/systems/PhysicsSystem.cpp:144-146`), and character gravity integrates with caller `dt` (`src/luminumbra_common/systems/PhysicsSystem.cpp:263-275`). Startup also sizes the Jolt job system from `std::thread::hardware_concurrency()` (`src/luminumbra_common/systems/PhysicsSystem.cpp:104-106`), so worker topology is host-dependent.
- Iteration order can affect collision body creation under load. Active chunks live in an `std::unordered_map` (`src/luminumbra_common/systems/SHIELD_WorldSystem.h:177-179`), collision creation iterates that map and stops at the per-frame cap (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:431-443`), and physics body IDs are tracked in another `std::unordered_map` (`src/luminumbra_common/systems/PhysicsSystem.h:129-133`). That is not replay/lockstep-ready.
- There is a stale chunk-collider bug path. Completed terrain remeshes replace mesh data, update LOD/version, and set `has_collision=false` (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1330-1337`), but `PhysicsSystem::add_chunk_collision` returns early if the chunk ID already has a body (`src/luminumbra_common/systems/PhysicsSystem.cpp:153-156`). The caller then marks `has_collision=true` after the no-op add (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:431-440`), leaving the old Jolt body authoritative.
- Render and collision geometry are divergent. Rendering builds marching-cubes terrain and transition skirts (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1260-1263`), while physics collision is a Jolt `HeightFieldShape` built from `chunk.heightmap_data` (`src/luminumbra_common/systems/PhysicsSystem.cpp:157-195`). Collision is only registered for LOD0 chunks (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:435-438`), so caves, overhangs, skirts, and mixed-LOD surface repairs are not guaranteed to match player collision.
- Character controller feel/correctness is thin around edges. Jump is a one-frame intent consumed after `update_player` (`src/luminumbra_client/player/PlayerController.cpp:148-150`, `src/luminumbra_client/player/PlayerController.cpp:193-194`), and `PhysicsSystem` only accepts it when Jolt already reports `OnGround` (`src/luminumbra_common/systems/PhysicsSystem.cpp:247-262`), so there is no coyote time or jump buffer. Crouch toggles client state before/alongside shape changes (`src/luminumbra_client/player/PlayerController.cpp:173-185`), while `SetShape` is called without propagating success/failure (`src/luminumbra_common/systems/PhysicsSystem.cpp:278-287`).
- Water exists as a world system, but player traversal has no water contract. `GameSession` creates and links `WaterSystem` (`src/luminumbra_common/world/GameSession.cpp:215-221`), while the player mode enum has no swim/water state (`src/luminumbra_client/player/PlayerController.h:18-21`) and walking update only delegates horizontal wish velocity, jump, crouch, and position sync (`src/luminumbra_client/player/PlayerController.cpp:170-198`).
- Audio raycast batching has a count cap, not a frame-time or fairness contract. The queue defaults to 64 queries/frame (`src/luminumbra_common/systems/PhysicsSystem.h:97-105`), sorts by priority, processes the top slice, and carries the rest forward (`src/luminumbra_common/systems/PhysicsSystem.cpp:512-553`). Because priorities are recomputed from distance at enqueue (`src/luminumbra_client/audio/AudioSpatialCluster.cpp:346-355`) and there is no age/deadline boost, low-priority queries can starve behind new close-source work.

## Must-Fix

- Fixed-tick physics contract. Before shipping replay, lockstep, or deterministic runtime gates, move player movement and `PhysicsSystem::update` behind a fixed-step accumulator with a recorded tick index, fixed substep count, max catch-up policy, and deterministic state hash.
- Stale collider replacement. A terrain mesh/LOD version change must remove or replace the existing Jolt body before `has_collision` is reset, or `ChunkCollisionData` must carry `mesh_version/current_lod` and `add_chunk_collision` must rebuild mismatches instead of returning early.
- Authoritative collision geometry policy. Decide whether terrain collision is heightfield-only or mesh/SDF-accurate. If caves, overhangs, or marching-cubes repairs are shippable surfaces, LOD0 collision must be generated from the same authoritative surface or explicitly gated as non-traversable content.
- Crouch shape/state synchronization. `set_player_crouched` needs a success result, and `PlayerController` should update `m_isCrouching` and camera eye height only after the physics shape transition succeeds.
- Player-water contract. Shipping worlds with traversable water need either a swim/wade state using water level/flow data or a validator/content rule proving required routes never depend on swimming.
- Batched query fairness and bounds. Add max queue age, deadline/aging priority, queue length telemetry, and drop/expire policy so audio occlusion cannot build unbounded latency under high source counts.

## Deepening Opportunities

- Add coyote time and jump buffering after the fixed tick exists; keep the artifact in ticks, not seconds, so it is replayable.
- Define explicit step-up, step-down, slope, and ground-snap expectations for marching-cubes terrain instead of relying only on Jolt defaults plus `mMaxSlopeAngle`/supporting volume (`src/luminumbra_common/systems/PhysicsSystem.cpp:234-239`).
- Extend runtime artifacts with collision readiness: body count, stale body count, mesh version attached to each collision body, near-field collision coverage, and time spent building collision.
- Make audio query ordering deterministic under equal priorities by adding query sequence/tick tie-breakers before spatial sorting.
- Add water-aware movement as an independent controller mode with buoyancy, drag, current influence, and clear transition thresholds.

## Frontier Proposals

- Gate-first: deterministic physics replay. Record input events, fixed ticks, player transforms/velocities, chunk collision add/remove events, and end-state hashes; run the same scenario twice and fail on any hash divergence before attempting lockstep claims.
- Gate-first: runtime physics smoke scenario. Add a harness scenario that walks a fixed route across slopes, ledges, chunk boundaries, water, and a crouch tunnel, then emits collision/controller telemetry.
- Gate-first: character-controller obstacle course. Build a deterministic marching-cubes obstacle course that proves coyote, jump buffer, step-up/step-down, crouch-under/stand-up, and water transitions with explicit pass/fail positions.
- Gate-first: collision churn stress. Stream across an LOD boundary repeatedly while forcing terrain remeshes and assert that Jolt body versions follow mesh versions exactly.
- Gate-first: audio physics query budget. Drive more raycasts than the per-frame cap with mixed priorities and assert bounded latency, no starvation, and stable frame cost.

## Proposed Gates

- `-Mode PhysicsReplay`: scenario `physics_replay_gate`; artifact `build/<preset>/test-artifacts/runtime/physics-replay/physics-replay-endstate.json`; validator compares two runs for identical tick count, player position/velocity hash, Jolt body count, chunk body version map, and final end-state hash.
- `-Mode PhysicsSmoke`: scenario `physics_smoke`; artifact `build/<preset>/test-artifacts/runtime/physics-smoke/physics-smoke.json`; validator asserts no penetration, fixed tick count, grounded-state transitions, collision-ready near field, and zero stale collision bodies.
- `-Mode PlayerObstacleCourse`: scenario `player_obstacle_course`; artifact `build/<preset>/test-artifacts/runtime/player-obstacle-course/player-obstacle-course.json`; validator asserts coyote/jump-buffer windows, step-up/step-down success, crouch shape/state agreement, and water-state transitions or explicit no-swim compliance.
- `-Mode ChunkCollisionLifecycle`: scenario `chunk_collision_churn`; artifact `build/<preset>/test-artifacts/runtime/chunk-collision-lifecycle/chunk-collision-lifecycle.json`; validator asserts every terrain `mesh_version/current_lod` change either rebuilds or removes the matching Jolt body, and `collision_chunks == physics_body_map_count`.
- `-Mode AudioPhysicsQueries`: scenario `audio_physics_query_budget`; artifact `build/<preset>/test-artifacts/runtime/audio-physics-query-budget/audio-physics-query-budget.json`; validator asserts max queue age, max pending queries, processed/dropped counts, p95 query latency, and no priority class starvation.
- `-Mode WaterPlayer`: scenario `water_player_interaction`; artifact `build/<preset>/test-artifacts/runtime/water-player/water-player-interaction.json`; validator asserts swim/wade movement against water height/flow, or asserts content-scoped no-swim routes if swimming is intentionally out of scope.
- Validator naming should extend the existing `validate-runtime-stability-phase-1.ps1` pattern: modes are declared in `ValidateSet` (`.forge/scripts/validate-runtime-stability-phase-1.ps1:1-3`) and dispatched by the mode switch (`.forge/scripts/validate-runtime-stability-phase-1.ps1:568-590`), as already done for `LodGround`, `WaterVisual`, and `Endurance300` (`.forge/scripts/validate-runtime-stability-phase-1.ps1:247-358`, `.forge/scripts/validate-runtime-stability-phase-1.ps1:360-380`, `.forge/scripts/validate-runtime-stability-phase-1.ps1:423-463`).

## References

- `src/luminumbra_common/systems/PhysicsSystem.h`
- `src/luminumbra_common/systems/PhysicsSystem.cpp`
- `src/luminumbra_client/player/PlayerController.h`
- `src/luminumbra_client/player/PlayerController.cpp`
- `src/luminumbra_common/world/GameSession.h`
- `src/luminumbra_common/world/GameSession.cpp`
- `src/luminumbra_common/systems/SHIELD_WorldSystem.h`
- `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp`
- `src/luminumbra_common/world/Chunk.h`
- `src/luminumbra_client/main_client.cpp`
- `src/luminumbra_client/audio/AudioSpatialCluster.cpp`
- `src/luminumbra_client/audio/AudioPropagationSystem.cpp`
- `.forge/scripts/validate-runtime-stability-phase-1.ps1`
- `.forge/artifacts/visual-performance-improvement/handoff.md`
- `.forge/artifacts/visual-performance-improvement/next-plan.md`
- `.forge/artifacts/engine-framework-roadmap/ultimate-plan.md`
