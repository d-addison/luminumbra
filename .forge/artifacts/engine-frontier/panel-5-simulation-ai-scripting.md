## Subsystem State

- Aetheric Field is currently a vision plus render/data hooks: README requires persistent Lumin/Umbra diffusion, `LuminCrystal` has material ID/emission data, and `crystal_field_effect.frag` renders a crystal-driven screen-space field, but the common source manifest has no Aetheric simulation system.
- Instinct AI has C++ data containers and Lua design samples, not a C++ planner. `NeedsComponent`, `SensesComponent`, and `ActionPlanComponent` exist, while the Lua GroveStrider describes goals/actions the missing planner would consume.
- Lua is linked through `sol2`/`lua`, but the `LuaState` wrapper is only a constructor/destructor shell. Existing scripts require entity handles, component access, world queries, movement commands, field sampling, and directive lifecycle calls that are not exposed yet.
- EventBus is an empty class and stub implementation. The Aetheric, Instinct, and Lua frontier should not add ad hoc callbacks before a deterministic event contract exists.
- Sequencing constraint: Wave 2 is engine contracts. The cheapest credible entry point is the EventBus deterministic ordering/recording contract, then Lua API manifest validation, then isolated Aetheric and Instinct deterministic scenarios.

## Findings

- The engine target is fixed-tick deterministic simulation: README defines `TICKS_PER_SECOND = 30` behavior at `README.md:104` to `README.md:114`, and `include/luminumbra/core/Types.h:42` to `include/luminumbra/core/Types.h:43` exposes the same tick constants. Any simulation frontier must be replayable, not frame-time driven.
- The Aetheric vision is explicit but not implemented as C++ simulation: README says the field is persistent Lumin/Umbra data at `README.md:70` to `README.md:77`, and emissive materials should glow based on field strength at `README.md:132` to `README.md:140`; `src/luminumbra_common/sources.cmake:4` to `src/luminumbra_common/sources.cmake:31` lists the common sources and includes no Aetheric system/component file.
- Current Aetheric rendering is a shader-local effect, not a gameplay field. `res/shaders/crystal_field_effect.frag:19` to `res/shaders/crystal_field_effect.frag:26` takes up to 16 crystal positions/intensities plus field uniforms, `res/shaders/crystal_field_effect.frag:86` to `res/shaders/crystal_field_effect.frag:104` modulates intensity with `u_time`, noise, direction, and height, and `res/shaders/crystal_field_effect.frag:175` to `res/shaders/crystal_field_effect.frag:183` doubles the effect for material ID 6.
- LuminCrystal already has a stable material hook: `include/luminumbra/core/Types.h:62` to `include/luminumbra/core/Types.h:70` maps `LuminCrystal = 6`, and `data/common/materials.json:50` to `data/common/materials.json:55` defines `LuminCrystal` with emission `[0.4, 0.6, 0.8]`.
- Lua/data already assume Aetheric gameplay APIs that do not exist in the C++ surface read here. `scripts/common/systems/system_aetheric_feedback.lua:9` to `scripts/common/systems/system_aetheric_feedback.lua:18` expects `AethericFieldComponent`, `NeedsComponent`, `TransformComponent`, and `get_world_api():get_aetheric_value`, while `scripts/common/archetypes/glimmercap.json:13` to `scripts/common/archetypes/glimmercap.json:16` defines an Aetheric emitter.
- Instinct C++ is component-only. `src/luminumbra_common/components/InstinctComponents.h:13` to `src/luminumbra_common/components/InstinctComponents.h:28` defines needs/senses, `src/luminumbra_common/components/InstinctComponents.h:34` to `src/luminumbra_common/components/InstinctComponents.h:46` defines action/plan storage, and `src/luminumbra_common/components/InstinctComponents.h:48` to `src/luminumbra_common/components/InstinctComponents.h:49` only tags agents.
- The Lua GroveStrider provides the intended planner contract shape: `scripts/common/ai/agents/grovestrider.lua:20` to `scripts/common/ai/agents/grovestrider.lua:35` maps components into boolean world state, `scripts/common/ai/agents/grovestrider.lua:38` to `scripts/common/ai/agents/grovestrider.lua:73` emits prioritized goals, and `scripts/common/ai/agents/grovestrider.lua:76` to `scripts/common/ai/agents/grovestrider.lua:87` lists action scripts.
- Action samples require a much larger scripting API than `LuaState` exposes: `action_find_food.lua:25` to `action_find_food.lua:45` expects tag queries, component reads, movement destinations, reachability, and action failure; `action_drink.lua:19` to `action_drink.lua:30` expects animation, world time, and component mutation.
- The current Lua samples are not deterministic/valid fixtures yet. `scripts/common/ai/actions/action_wander.lua:27` to `scripts/common/ai/actions/action_wander.lua:36` uses `math.random`, and `scripts/common/ai/actions/action_find_water.lua:1` to `scripts/common/ai/actions/action_find_water.lua:22` is a copy of `action_find_food.lua` that returns `ActionFindFood` and produces `isNearFood`, not water.
- Lua integration is only linked, not surfaced. `src/luminumbra_common/CMakeLists.txt:14` to `src/luminumbra_common/CMakeLists.txt:20` links `sol2` and `lua`, but `src/luminumbra_common/scripting/LuaState.h:5` to `src/luminumbra_common/scripting/LuaState.h:9` and `src/luminumbra_common/scripting/LuaState.cpp:5` to `src/luminumbra_common/scripting/LuaState.cpp:11` contain only lifecycle stubs.
- EventBus is near-empty by file evidence. `src/luminumbra_common/core/EventBus.h:5` to `src/luminumbra_common/core/EventBus.h:9` declares only constructor/destructor, and `src/luminumbra_common/core/EventBus.cpp:5` to `src/luminumbra_common/core/EventBus.cpp:11` only contains empty lifecycle bodies.
- The existing job pattern can support pure planning/diffusion jobs only if commits are ordered. `src/luminumbra_common/core/JobSystem.h:34` to `src/luminumbra_common/core/JobSystem.h:39` exposes startup/dispatch/batch/wait/stats, and `src/luminumbra_common/systems/WaterSystem.h:41` plus `src/luminumbra_common/systems/WaterSystem.cpp:317` to `src/luminumbra_common/systems/WaterSystem.cpp:321` show a once-per-tick job batch followed by main-thread collection.
- The roadmap says to keep gameplay breadth out of Wave 2. `.forge/artifacts/engine-framework-roadmap/ultimate-plan.md:13` blocks broad gameplay before Phase 1 and Phase 3 gates, while `.forge/artifacts/engine-framework-roadmap/ultimate-plan.md:23` to `.forge/artifacts/engine-framework-roadmap/ultimate-plan.md:28` defines Wave 2 as lifecycle/dependency/resource/config contracts.

## Must-Fix

- Gate-first fix: implement EventBus deterministic ordering plus replay recording before Aetheric, Instinct, or Lua systems publish cross-system state. Without this, replay/debug claims in `README.md:114` have no event substrate.
- Gate-first fix: add a minimal Lua API manifest and sandboxed loader before running gameplay scripts. Current scripts cannot execute against `LuaState`, and hot reload would be unsafe without an allowed API list, script digests, and rollback.
- Gate-first fix: repair sample-script contract defects before promoting scripts to gates. `action_find_water.lua` must represent water/thirst, and random movement must use engine-provided deterministic RNG or a precomputed planner command.
- Gate-first fix: add the smallest Aetheric C++ model before tying gameplay to shader output. Required minimum is a chunk-local fixed-point Lumin/Umbra scalar grid with deterministic source injection, diffusion, sampling, and checksum output.
- Gate-first fix: add the smallest Instinct planner before claiming GOAP behavior. Required minimum is needs -> goal selection -> bounded action plan with deterministic tie-breaks and one provable need-satisfaction scenario.

## Deepening Opportunities

- Gate-first: extend `materials.json` with `aetheric_emission` metadata for `LuminCrystal` and future flora, validated against `MaterialType` IDs, instead of hard-coding source strength in shader/client code.
- Gate-first: drive `crystal_field_effect.frag` from deterministic field/source artifacts once the unit field exists; until then, keep it as a visual-only effect and gate shader/pixel behavior separately from simulation.
- Gate-first: build a script-contract validator that loads `scripts/common` in a sandbox, extracts agent/action/directive manifests, and rejects banned APIs or unknown engine calls before any runtime hot reload feature lands.
- Gate-first: use the existing `JobSystem` only for read-only planner/diffusion batches after single-thread deterministic tests pass; commit results on the simulation thread sorted by tick, phase, entity ID, and sequence number.
- Gate-first: add an AI/event trace artifact before richer behavior. Goal choice, action starts/completions/failures, script digest, and field sample values should be inspectable before adding more creatures or directives.

## Frontier Proposals

- Gate-first: EventBus v1 should be a typed simulation event queue with `{tick, phase, sequence, source_entity, event_type, payload}`. Drain order must be deterministic, subscriptions must be stable by explicit priority/subscription ID, and replay JSON must record the drained stream. Initial event types should cover `FieldSourceChanged`, `FieldThresholdCrossed`, `AgentGoalSelected`, `ActionStarted`, `ActionCompleted`, `ActionFailed`, and `LuaScriptReloaded`.
- Gate-first: Aetheric Field v1 should be chunk-local and fixed-point. Use a small per-chunk grid, two scalar channels (`lumin`, `umbra`), deterministic source injection from `AethericFieldComponent`/material emission, six-neighbor diffusion for one fixed tick, and integer checksums. Do not attempt global field streaming, player redirection tools, GPU simulation, Radiance Cascade integration, or cross-chunk conservation yet.
- Gate-first: Instinct planner v1 should implement `NeedsComponent` -> goal scores -> action graph search. Goal priority should be deterministic (`safety`, `hunger`, `thirst`, `fatigue`, `curiosity`, then stable goal ID), actions should expose boolean preconditions/effects plus fixed costs/durations, and the planner should output `ActionPlanComponent`. Run it after senses/field sampling and before movement/physics action execution; use job batches only for pure planning snapshots, then commit sorted by entity ID.
- Gate-first: Lua v1 should expose only deterministic gameplay iteration APIs: entity ID handles, whitelisted component field reads/writes, fixed tick time, deterministic RNG supplied by the engine, stable world queries sorted by distance/entity ID, aetheric sampling, movement/action commands, and event emission. Exclude `os`, `io`, `debug`, wall-clock time, uncontrolled `math.random`, threads, coroutines, and direct registry iteration.
- Gate-first: hot reload should compile and validate a new Lua module in isolation, compare required function signatures/manifests, swap only at a tick boundary, emit `LuaScriptReloaded`, and keep the old digest active on failure. Do not hot-reload live multiplayer behavior until replay traces prove identical results before/after valid reload.
- Gate-first sequencing: start Wave 2 with `simulation_eventbus_order_replay_v1`, then `lua_api_manifest_common_v1`, then `aetheric_chunk_diffusion_single_crystal_v1`, then `instinct_grovestrider_hunger_v1`. Do not dispatch non-Codex agents, do not add broad gameplay content, and do not couple AI/Lua behavior to visuals before the deterministic gates exist.

## Proposed Gates

- `simulation_eventbus_order_replay_v1`: unit test `EventBusDeterministicOrderTest`; deterministic scenario enqueues `LuaScriptReloaded`, `FieldSourceChanged`, and `AgentGoalSelected` in mixed producer order for the same tick; emits `eventbus-replay.json`; validator mode `validate-engine-frontier.ps1 -Mode SimulationEventBus` asserts stable drain order, sequence numbers, and replay equality.
- `aetheric_chunk_diffusion_single_crystal_v1`: unit test `AethericFieldDiffusionTest.SingleLuminCrystal16Ticks`; deterministic scenario uses one chunk, one center Lumin source, fixed-point strength, 16 ticks; emits `aetheric-field-diffusion.json` with source config, sample cells, energy bounds, and grid checksum.
- `aetheric_lumincrystal_emission_visual_v1`: runtime/render scenario `--scenario aetheric_lumincrystal_emission_smoke`; emits `aetheric-visual.json` plus `aetheric-lumincrystal.ppm`; validator mode `validate-engine-frontier.ps1 -Mode AethericVisual` asserts material ID 6 pixels brighten with field enabled and remain checksum-stable from the fixed seed.
- `instinct_grovestrider_hunger_v1`: unit/integration test `InstinctPlannerNeedSatisfactionTest.GroveStriderHunger`; deterministic scenario uses fixed seed/entity IDs, hunger `0.9`, one known food source, and no threat; emits `instinct-grovestrider-hunger.json`; validator asserts selected goal `satisfy_hunger`, stable plan, and hunger below threshold within `N` ticks.
- `lua_api_manifest_common_v1`: validator mode `validate-engine-frontier.ps1 -Mode LuaApiManifest`; sandbox-loads `scripts/common`; emits `lua-api-manifest.json`; rejects banned APIs, unknown engine calls, missing action lifecycle functions, filename/module mismatches, and `action_find_water.lua` returning food effects.
- `lua_hot_reload_tick_boundary_v1`: unit test `LuaHotReloadGateTest.TickBoundaryRollback`; deterministic scenario requests one valid reload and one invalid reload at fixed ticks; emits `lua-hot-reload.json`; validator asserts tick-boundary swap, old digest retention on failure, and a matching `LuaScriptReloaded` event trace.
- `simulation_event_trace_cross_run_v1`: validator mode `validate-engine-frontier.ps1 -Mode SimulationTrace`; deterministic scenario runs field sampling plus one GroveStrider plan twice from the same seed; emits `simulation-event-trace.json`; validator diffs event stream, script digests, field checksums, selected goal, and selected actions.

## References

- README.md
- .forge/artifacts/engine-framework-roadmap/ultimate-plan.md
- .forge/artifacts/engine-frontier/panel-4-audio.md
- include/luminumbra/core/Types.h
- src/CMakeLists.txt
- src/luminumbra_common/CMakeLists.txt
- src/luminumbra_common/sources.cmake
- src/luminumbra_common/components/InstinctComponents.h
- src/luminumbra_common/core/EventBus.h
- src/luminumbra_common/core/EventBus.cpp
- src/luminumbra_common/core/JobSystem.h
- src/luminumbra_common/scripting/LuaState.h
- src/luminumbra_common/scripting/LuaState.cpp
- src/luminumbra_common/systems/WaterSystem.h
- src/luminumbra_common/systems/WaterSystem.cpp
- src/luminumbra_common/systems/SHIELD_WorldSystem.h
- src/luminumbra_common/systems/SHIELD_WorldSystem.cpp
- data/common/materials.json
- res/shaders/crystal_field_effect.frag
- scripts/common/systems/system_aetheric_feedback.lua
- scripts/common/directives/directive_lunar_bloom.lua
- scripts/common/directives/scenario_majestic_silhouette.lua
- scripts/common/archetypes/glimmercap.json
- scripts/common/archetypes/grovestrider.json
- scripts/common/archetypes/shadowstalker.json
- scripts/common/ai/agents/grovestrider.lua
- scripts/common/ai/actions/action_drink.lua
- scripts/common/ai/actions/action_find_food.lua
- scripts/common/ai/actions/action_find_water.lua
- scripts/common/ai/actions/action_flee.lua
- scripts/common/ai/actions/action_graze.lua
- scripts/common/ai/actions/action_rest.lua
- scripts/common/ai/actions/action_wander.lua
