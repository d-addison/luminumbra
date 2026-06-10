# Engine Frontier Ultimate Plan

## Wave 1

The first task is the deterministic Material/texture-layer visual gate for sand versus grey fallback. This wave contains only emphasis-stream work: world/streaming visual verification and the rendering/visual loop. No persistence, GPU SDF runtime integration, Aetheric Field, GOAP/Instinct, Lua hot reload, networking, physics architecture, audio feature work, UI expansion, render extraction, scheduler replacement, or water beauty pass starts here.

Scope:

- Implement `material_visual_smoke` as the first dispatch task.
- Produce `build/debug/test-artifacts/runtime/material-visual/material-visual-analysis.json` with schema `luminumbra.material_visual_analysis.v1`.
- Include per-material ROI entries, with Sand mandatory, classified pixel counts, grey-fallback pixel counts, thresholds, `gl_debug` error count, a normal screenshot, and a material-ID heatmap screenshot.
- Re-run `MaterialVisual`, `LodGround`, and `WaterVisual` together and record the result.
- Preserve the current `ctest --preset debug --output-on-failure -E "_NOT_BUILT$"` lane. The scan run during plan authoring on 2026-06-10 passed 58/58, including the three known suspect tests, so no current Wave 1 repair task is required. If this changes before execution, insert one Wave 1 fix task per failing test before Wave 2.

Verification commands:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode LodGround -SmokeSeconds 30
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode WaterVisual -SmokeSeconds 30
ctest --preset debug --output-on-failure -E "_NOT_BUILT$"
```

Before Wave 2 starts:

- Sand has valid material/texture-layer agreement and no grey fallback above threshold.
- The material-ID heatmap and final screenshot both exist and are referenced by the analysis artifact.
- `gl_debug.errors` is zero for `MaterialVisual`, `LodGround`, and `WaterVisual`.
- The full non-placeholder debug CTest lane passes.
- No broad visual claim is accepted from draw counts alone.

Critique mitigations addressed:

- Finding 1 is addressed by the dispatch graph created with this plan.
- Finding 2 is addressed by making `T-EF-1-material-visual-gate` the first executable task.
- Finding 6 is addressed by serializing tasks touching `main_client.cpp`, `RenderPipeline.cpp`, validators, sources manifests, world streaming, and the physics loop through explicit dependencies and dispatch conflict policy.
- Finding 13 is addressed by keeping Wave 1 to one implementation task plus verification tasks under the $30/day budget expectation.
- Finding 15 is addressed by using command exits, schema fields, pixel counts, screenshot paths, thresholds, and GL counters instead of prose-only acceptance.

## Wave 2

This wave turns named risks into executable gates before behavior changes. It still stays inside the mature emphasis streams: rendering/visual health and world/streaming telemetry. Missing validator modes are implemented before any task may cite them as acceptance.

Scope:

- Add a `FrontierDisabled` source/runtime validator proving persistence runtime paths, GPU SDF runtime toggle, far-field SDF, networking authority, Aetheric simulation, GOAP/Instinct planner, and Lua hot reload remain disabled unless their deterministic gate exists and passes.
- Add `RenderHealth` before any `RenderPipeline.cpp` extraction. The artifact must compare pass metadata, pass stats, resource dimensions, GL debug counters, shader health, and structured screenshot classifications against the baseline.
- Add no-behavior-change streaming telemetry and gate schemas for rolling backlog, lane/job stats, upload drain, and endurance-stream drain.
- Do not modify scheduling policy, LOD thresholds, render pass ownership, or water quality behavior in this wave except as required to emit truthful telemetry.

Verification commands:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode FrontierDisabled
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode RenderHealth
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode EnduranceStreamDrain
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode LodGround -SmokeSeconds 30
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode WaterVisual -SmokeSeconds 30
ctest --preset debug --output-on-failure -E "_NOT_BUILT$"
```

Before Wave 3 starts:

- `FrontierDisabled`, `RenderHealth`, and the first streaming drain validator modes exist and fail on missing or malformed artifacts.
- Render extraction remains blocked unless `RenderHealth`, `MaterialVisual`, `LodGround`, and `WaterVisual` are green.
- Streaming behavior changes remain blocked unless telemetry shows rolling backlog, lane/job pressure, upload drain, and visual results.

Critique mitigations addressed:

- Finding 4 is addressed by requiring `RenderHealth` before render pass extraction.
- Finding 5 is addressed by splitting every missing gate into validator/schema work before implementation work.
- Finding 7 is addressed by making SHIELD streaming work telemetry-first.
- Finding 14 is addressed by adding `FrontierDisabled`.
- Finding 15 is addressed by rejecting criteria that cannot be expressed as command exits, artifact schemas, thresholds, source invariants, or documented non-goals.

## Wave 3

This wave deepens world/streaming and the render/visual loop after the gate surfaces exist. It may change mature behavior only behind gates that prove no regression in near-field coverage, upload drain, and visual captures.

Scope:

- Add LOD boundary churn and hysteresis gates before retuning LOD policy.
- Add seam-arrival gates for asynchronous finer-neighbor arrival before changing transition geometry.
- Add shader inventory and shader-suite health so shaders are classified as wired, compiled-only, dead asset, or explicitly exempted.
- Keep water quality as a ladder of feature-specific gates. Do not claim SSR, caustics, foam, or depth tint complete without a dedicated artifact and validator.
- Any render pass extraction task must rerun `MaterialVisual`, `LodGround`, `WaterVisual`, and `RenderHealth` and prove pass metadata, resource dimensions, draw counts, GL counters, and screenshot classifications match the baseline.

Verification commands:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode LodBoundaryHysteresis
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode LodSeamRisk
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode ShaderInventory
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode RenderHealth
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode LodGround -SmokeSeconds 30
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode WaterVisual -SmokeSeconds 30
ctest --preset debug --output-on-failure -E "_NOT_BUILT$"
```

Before Wave 4 starts:

- LOD churn and seam-arrival validators exist and pass on deterministic artifacts.
- Shader inventory is manifest-backed and does not silently accept dead or drifted shader assets.
- No scheduler, render extraction, or water-quality change is accepted without the visual gates and its specific no-regression gate.

Critique mitigations addressed:

- Finding 4 remains enforced for render extraction.
- Finding 7 remains enforced by telemetry/gates before scheduling changes.
- Finding 13 remains enforced by limiting daily work to at most three narrow implementation tasks plus one integration task, fewer when `RenderPipeline.cpp` or `main_client.cpp` is involved.

## Wave 4

This wave adds narrow contracts for physics/player, audio, UI, tooling, and tests. These are not batched into broad architecture rewrites.

Scope:

- Physics starts with stale collider lifecycle and `ChunkCollisionLifecycle`, then fixed-tick replay, then crouch shape/state synchronization, then audio physics query fairness. Collision geometry authority remains a spec decision before mesh/SDF collision changes.
- Audio splits null-device telemetry from playback-handle integration. Null counters cannot prove volume, pitch, occlusion, reverb send, or lifetime updates; a fake-backend or miniaudio-facing handle test must prove those updates.
- UI/tooling starts with CTest labels/presets and a coverage producer for a named fast target set before enforcing 95 percent. UI screenshot gates must define viewport sizes, PPM paths, nonblank thresholds, occupied tile metrics, and clipping heuristics before becoming blocking.

Verification commands:

```powershell
ctest --preset debug --output-on-failure -R "ChunkCollisionLifecycle|PhysicsReplay|Audio|UiSmokeTest|CTestMetadataGate|CoverageGate"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode PhysicsReplay
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode AudioRuntime
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode UiTestBaseline
ctest --preset debug --output-on-failure -E "_NOT_BUILT$"
```

Before Wave 5 starts:

- Physics changes are split into runnable gates and do not mix stale collider replacement, fixed tick, crouch, water traversal, and audio query fairness in one task.
- Audio no-device telemetry is not used to overclaim playback integration.
- UI screenshot and coverage acceptance is measurement-defined before enforcement.

Critique mitigations addressed:

- Finding 8 is addressed by splitting physics into narrow tasks and leaving collision geometry authority as an explicit decision.
- Finding 11 is addressed by separating null audio telemetry from handle application tests.
- Finding 12 is addressed by adding labels/presets and measurement baselines before enforcement.

## Wave 5

This wave introduces simulation, Lua, Aetheric Field, and GOAP/Instinct in a strict unit-level sequence. No gameplay breadth, visual coupling, hot reload, global Aetheric streaming, or planner breadth starts until the preceding contract exists.

Scope:

- Add deterministic EventBus ordering and replay first.
- Add Lua sandbox/API manifest next, restricted to deterministic gameplay iteration APIs.
- Add Aetheric Field v1 as a chunk-local fixed-point two-channel diffusion unit with source injection and checksums.
- Add Instinct planner v1 as one deterministic needs-to-goal-to-action scenario with stable tie-breaks.

Verification commands:

```powershell
ctest --preset debug --output-on-failure -R "EventBusDeterministicOrderTest|LuaApiManifest|AethericFieldDiffusionTest|InstinctPlannerNeedSatisfactionTest"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode SimulationEventBus
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode LuaApiManifest
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode AethericDiffusion
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode InstinctPlanner
ctest --preset debug --output-on-failure -E "_NOT_BUILT$"
```

Before Wave 6 starts:

- Event ordering is deterministic and replayable.
- Lua exposes only manifest-approved deterministic APIs.
- Aetheric diffusion is unit-level and checksum-backed.
- Instinct proves one need-satisfaction plan only; broader GOAP remains blocked.

Critique mitigations addressed:

- Finding 9 is addressed by sequencing EventBus, Lua, Aetheric, and Instinct instead of placing them in one broad wave.

## Wave 6

This wave handles persistence prerequisites before networking. It also establishes world hashes and durable IDs needed by replay and authority work.

Scope:

- Persist complete world identity: `worldId`, spawn point, stable numeric seed, world format version, preset digest, and schema version.
- Add chunk persistence with versioned headers, coordinates, dimensions, section table, sizes, LZ4-compressed sections where used, checksum, atomic write, and strict decode errors. Mesh and collision remain derived.
- Add deterministic world hash over sorted chunks and stable entities.
- Add a limited entity snapshot for stable IDs and durable components.

Verification commands:

```powershell
ctest --preset debug --output-on-failure -R "WorldPersistenceRoundTripTest|ChunkPersistenceFormatTest|WorldHashTest|EntitySnapshot"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode PersistenceRoundTrip
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode ChunkFormat
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode WorldHash
ctest --preset debug --output-on-failure -E "_NOT_BUILT$"
```

Before Wave 7 starts:

- Persistence round-trip is deterministic for the named seed and mutation case.
- Bad chunk formats fail closed.
- World hash is stable across insertion order.
- Networking remains blocked until world identity, durable IDs, sorted hashes, and fixed-tick authority prerequisites exist.

Critique mitigations addressed:

- Finding 10 is addressed by ordering persistence and world hash/durable IDs before networking.

## Wave 7

This wave handles GPU SDF runtime integration separately from persistence and networking. Runtime GPU SDF remains disabled until callback safety and CPU/GPU parity pass.

Scope:

- Add GPU SDF callback safety first, including invalid callback handling and fail-closed CPU fallback rules.
- Add CPU/GPU compute parity for known seeds and chunks.
- Add runtime GPU SDF toggle only after safety and parity, with render-thread ownership, queue counters, fallback counters, and parity artifacts.
- Keep far-field SDF visual mode blocked until runtime parity is green.

Verification commands:

```powershell
ctest --preset debug --output-on-failure -R "SdfGpuCallbackTest|SdfGpuCpuParityTest"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode GpuSdfCallbackSafety
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode GpuSdfComputeParity
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode GpuSdfRuntimeParity
ctest --preset debug --output-on-failure -E "_NOT_BUILT$"
```

Before Wave 8 starts:

- No worker generation job calls GL compute.
- Callback output is validated and fails closed.
- CPU/GPU parity artifacts exist and pass for known cases.
- Runtime GPU SDF remains opt-in and gated.

Critique mitigations addressed:

- Finding 10 is addressed again by keeping GPU SDF in a separate render/GPU lane and ordering runtime toggle after safety and parity.

## Wave 8

This wave introduces networking only after persistence, durable IDs, world hash, and fixed-tick prerequisites. It is loopback-only: no sockets, prediction, NAT, multiplayer UI, or session discovery.

Scope:

- Add in-memory loopback authority for two local sessions.
- Apply ordered inputs at fixed ticks.
- Broadcast deterministic state hashes.
- Keep disconnect-free local transport only.

Verification commands:

```powershell
ctest --preset debug --output-on-failure -R "LoopbackAuthorityTest|NetworkStateHash"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode NetworkLoopback
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode NetworkStateHash
ctest --preset debug --output-on-failure -E "_NOT_BUILT$"
```

Before Wave 9 starts:

- Two loopback sessions converge deterministically.
- State hashes are sorted and stable.
- Runtime sockets and prediction remain disabled.

Critique mitigations addressed:

- Finding 10 is closed for networking by making loopback depend on persistence, world hash/durable IDs, and fixed-tick authority prerequisites.

## Wave 9

This is the terminal visual-stability and endurance wave. It does not add new feature work. It revalidates the mature visual loop and the long visible run after all visual gate tasks have landed.

Scope:

- Rerun the full non-placeholder CTest lane.
- Rerun `MaterialVisual`, `LodGround`, and `WaterVisual`.
- Rerun `Endurance300` after the visual gates are green.
- Record prior visual artifact paths and timestamps or content hashes in the endurance result.
- Endurance must prove no early visible-window close, final readiness true, drained generation/meshing/job/upload tails, zero upload-priority inversions, zero GL debug errors, and no visual gate regression.

Verification commands:

```powershell
ctest --preset debug --output-on-failure -E "_NOT_BUILT$"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode LodGround -SmokeSeconds 30
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode WaterVisual -SmokeSeconds 30
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode Endurance300
```

Before closure:

- Endurance runs after, not before, all visual gates.
- Any failure becomes a gate or source fix task; it is not deferred.
- Daily execution remains budget-aware. A long endurance run may span a later day if the $30/day contract budget is exhausted.

Critique mitigations addressed:

- Finding 3 is addressed by making endurance the terminal gate after visual gates.
- Finding 13 is addressed by isolating endurance from heavy implementation waves.

No critique mitigation is rejected. Broad proposals are staged behind validator/schema tasks or marked as non-goals until the measurable gate exists.

## Success Definition

Engine frontier succeeds when:

- `forge tasks validate .forge/tasks/engine-frontier/dispatch.json` passes.
- `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Files` passes.
- `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Sections` passes.
- Wave 1 lands first and passes `MaterialVisual`, `LodGround`, `WaterVisual`, and the full non-placeholder debug CTest lane.
- All later frontier streams are gate-first: persistence before networking, GPU SDF runtime after callback safety and parity, Aetheric after deterministic EventBus and Lua manifest, GOAP/Instinct after one unit-level planner gate, and networking after fixed-tick/world-hash/durable-ID prerequisites.
- Render extraction, scheduler changes, water quality, physics/player, audio, UI, simulation, persistence, GPU SDF, and networking all have executable validators before implementation tasks rely on those gates.
- Endurance is the final terminal revalidation after all visual gates and records the exact prior visual artifacts it depends on.
