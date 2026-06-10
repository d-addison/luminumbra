## Adversarial Critique

### Finding 1: The roadmap cannot be finalized without a concrete task graph
- Finding: The spec accepts `forge tasks validate .forge/tasks/engine-frontier/dispatch.json`, but that dispatch graph does not exist yet. The research and panels name dozens of gates and frontier entry points, but there is no machine-checkable dependency graph proving material-first sequencing, render serialization, persistence-before-networking, or the `$30/day` and `max_parallel_default: 4` constraints from `.forge/config.yaml`. This leaves the most important risks to human interpretation during revision.
- Mitigation: The revision must create `.forge/tasks/engine-frontier/dispatch.json` before any roadmap is considered final. It must encode explicit dependencies, max four parallel tasks, per-task verification commands, and conflict groups for shared files. The first executable task must be `MaterialVisual`; render/main-client/validator tasks must be serialized unless they only read files.
- Verdict: blocker

### Finding 2: MaterialVisual is stated as first, but not enforced as first everywhere
- Finding: `research.md` and `panel-2-rendering-visual-loop.md` correctly say the material/texture-layer visual gate must be the first Wave 1 task. The spec also gives `MaterialVisual` top-level acceptance, but `.forge/workflows/engine-frontier.yaml` post-verification runs `Smoke`, `LodGround`, and `WaterVisual` before `MaterialVisual`. That contradicts the premise that grey fallback and material/layer drift can invalidate visual conclusions from LOD and water captures.
- Mitigation: Make `MaterialVisual` the first dispatch task and the first post-verify visual gate. No water-quality, render-health, endurance, shader polish, or render extraction task should start until `MaterialVisual` passes with the Sand ROI, heatmap, final-color plausibility, and zero GL debug errors.
- Verdict: blocker

### Finding 3: Endurance revalidation is still not a hard terminal gate
- Finding: The handoff says visible 300s endurance remains open, and the spec says endurance must be revalidated after `MaterialVisual`, `LodGround`, and `WaterVisual` are green. The workflow post-verify does not run `Endurance300`, and the spec does not require the endurance artifact to reference the exact visual-gate artifacts it depends on. This allows a roadmap to claim endurance closure without proving it ran after the stabilized material/water/LOD gates.
- Mitigation: Add a final `Endurance300` or `EnduranceStreamDrain` task after all three visual gates. Its artifact must record the prior gate artifact paths, timestamps or content hashes, readiness, upload/job drain, zero upload-priority inversions, zero GL debug errors, and no early visible-window close. Do not place endurance in an early daily wave.
- Verdict: blocker

### Finding 4: RenderPipeline modularization has no enforced no-behavior-change gate
- Finding: The rendering panel correctly identifies `RenderPipeline.cpp` as monolithic and high risk, and it proposes `RenderHealth` before pass extraction. The spec only makes render graph replacement a non-goal; it does not ban or gate staged pass extraction. Current `validate-engine-frontier.ps1` exposes no `RenderHealth`, `ShaderSuiteHealth`, `ShaderInventory`, `RenderTiming`, or `VisualRegression` mode. Therefore a future roadmap could modularize `RenderPipeline.cpp` with only generic build/tests plus `MaterialVisual`, which is not enough to prove unchanged pass order, GL state, resource dimensions, draw counts, and screenshots.
- Mitigation: Before any `RenderPipeline.cpp` extraction task, add an executable `RenderHealth` validator and baseline artifact. Every render extraction wave must rerun `MaterialVisual`, `LodGround`, `WaterVisual`, and `RenderHealth`, and must compare pass metadata, pass stats, resource dimensions, GL debug counters, shader health, and structured screenshot classifications against the baseline.
- Verdict: blocker

### Finding 5: Many proposed gates are only names, not executable acceptance criteria
- Finding: The panels list validators such as `EnduranceStreamDrain`, `StreamingUploadDrain`, `LodBoundaryHysteresis`, `LodSeamRisk`, `JobContention`, `ShaderSuiteHealth`, `RenderHealth`, `PersistenceRoundTrip`, `NetworkLoopback`, `SimulationEventBus`, and multiple audio validators. The current validator surfaces are much smaller: `validate-engine-frontier.ps1` supports only `CodexOnly`, `Panels`, `Files`, `Sections`, `Build`, `UnitTests`, `MaterialVisual`, and `All`; `validate-runtime-stability-phase-1.ps1` supports `LodGround`, `WaterVisual`, and `Endurance300` but not the new streaming seam/job modes. A task that says "pass validator mode X" is not executable until mode X exists.
- Mitigation: Split every missing gate into a validator/artifact-schema task first, then an implementation task. A dispatch task may not use a named gate as acceptance unless the validator mode is present in the relevant script and has at least one failing fixture or missing-artifact failure path.
- Verdict: blocker

### Finding 6: Hidden file coupling will collide under parallel dispatch
- Finding: Several tasks look independent by subsystem but share the same files. `MaterialVisual`, water quality, shader health, render health, GPU SDF, and render extraction all touch `RenderPipeline.cpp` or `RenderPipeline.h`. Material/water/LOD scenarios, audio telemetry, physics fixed tick, networking loopback, and runtime recording all touch `main_client.cpp`. Most gate additions touch `.forge/scripts/validate-engine-frontier.ps1`, and many subsystem additions touch `sources.cmake` files. Four parallel tasks against those files will create merge conflicts or, worse, interleaved behavior changes that one task's verification cannot isolate.
- Mitigation: Add dispatch conflict groups such as `render-pipeline`, `main-client-runtime`, `validators`, `common-sources-cmake`, `common-world-streaming`, and `physics-loop`. Only one task in a conflict group should run per wave. Follow each wave with a single integration task that runs the affected validators from a clean build state.
- Verdict: blocker

### Finding 7: SHIELD streaming and job-system changes are too close to mature behavior
- Finding: The synthesis says to deepen mature SHIELD streaming, but the frontier list still includes priority lanes, work stealing, predictive prefetch, LOD hysteresis, meshlet streaming, and adaptive uploads. The handoff shows `LodGround` and `WaterVisual` recently became green after targeted fixes. Replacing FIFO behavior or scheduling policy before telemetry and boundary gates exist risks regressing the now-working stream/upload loop without a no-behavior-change story.
- Mitigation: Stage SHIELD work as telemetry first, then gates, then behavior changes. Add rolling backlog, lane/job stats, LOD boundary churn, and seam-arrival validators before scheduler or LOD policy changes. Any first scheduling task should be no-behavior-change telemetry unless its gate proves unchanged near-field coverage, upload drain, and visual results.
- Verdict: revise

### Finding 8: Physics/player proposals mix bug fixes, architecture, and design policy
- Finding: The physics panel identifies real defects, especially stale collider replacement, variable-dt stepping, unordered collision creation, and heightfield/render divergence. But bundling fixed tick, stale collider rebuilds, authoritative collision geometry, crouch state synchronization, water traversal, and audio query fairness into one wave would touch `PhysicsSystem`, `PlayerController`, `SHIELD_WorldSystem`, and the main loop at once. "Authoritative collision geometry" is also a design decision, not a single executable task.
- Mitigation: Split physics into narrow tasks: first stale collider lifecycle with a `ChunkCollisionLifecycle` gate, then fixed-tick replay harness, then crouch shape/state synchronization, then audio query fairness. Leave collision geometry authority as an explicit spec decision with acceptance rules before mesh/SDF collision implementation begins.
- Verdict: revise

### Finding 9: Simulation, Lua, Aetheric, and Instinct are too large for one contract wave
- Finding: The panel evidence says `EventBus` is empty, `LuaState` is a constructor/destructor shell, Aetheric has no common simulation system, and Instinct has only component containers. Yet the synthesis carries EventBus, Lua API manifest, Aetheric diffusion, Instinct planner, Lua hot reload, and simulation trace gates as adjacent Wave 2-style work. That is too much new surface area and too many new source/build/validator touches for a single Codex dispatch wave.
- Mitigation: Make this a strict sequence: EventBus deterministic order/replay only; then Lua sandbox/API manifest only; then Aetheric fixed-point unit diffusion only; then Instinct one-need planner only. Do not add Aetheric visual coupling, Lua hot reload, or cross-run simulation traces until the prior unit-level contract exists and has a runnable validator.
- Verdict: revise

### Finding 10: Persistence, GPU SDF, and networking are grouped despite hard prerequisites
- Finding: The panel correctly says persistence is metadata-only, GPU SDF runtime is disabled and unsafe from worker generation jobs, and networking/server are stubs. However the synthesis groups persistence, GPU SDF, and networking under one section, and the spec only says frontier systems remain disabled or gated. Networking loopback and state hashes depend on stable world identity, durable entity IDs, sorted world hashes, and eventually fixed tick. Those prerequisites are not enforced as blockers before `NetworkLoopback`.
- Mitigation: Order this area as persistence first, then world hash/durable IDs, then fixed-tick authority prerequisites, then loopback networking. Keep GPU SDF callback safety and CPU/GPU parity in a separate render/GPU lane; do not combine it with persistence or networking. Runtime GPU SDF toggle must remain after callback safety and compute parity.
- Verdict: blocker

### Finding 11: Audio gates overclaim what no-device telemetry can prove
- Finding: `audio_spatial_cluster_headless --no-audio` is useful for deterministic counters, but it cannot prove that miniaudio handles receive attenuation, doppler pitch, filters, or reverb sends. The audio panel's highest-risk issue is that cluster outputs are not connected back to playback. A null-audio artifact with active voice counts would not close that integration risk.
- Mitigation: Split audio into two gate types. First add null audio telemetry schema and deterministic counters. Separately add a miniaudio-facing or fake-backend handle test that proves per-handle volume, pitch, occlusion, and lifetime updates are applied. Bank validation and material absorption should be independent tasks.
- Verdict: revise

### Finding 12: UI/tooling/test gates include subjective or unbaselined acceptance
- Finding: UI screenshot smoke, clipped-text risk, artifact dashboard, shader manifests, ASan coverage, and 95 percent first-party coverage are all directionally good. But the current config advertises `coverage_gate: 95` without an enforced coverage producer, and the proposed UI screenshot/clipping checks do not define thresholds, viewport sizes, font conditions, or the exact first-party fast target set. These are not single-task executable unless the measurement harness is defined first.
- Mitigation: First add CTest labels/presets and a coverage producer for a named fast target set, then report the current baseline before enforcing 95 percent. Define UI screenshot viewports, PPM paths, nonblank thresholds, occupied tile metrics, and clipping heuristics before making the gate blocking.
- Verdict: revise

### Finding 13: Budget realism is not reflected in wave size or verification cost
- Finding: `.forge/config.yaml` sets `daily_per_developer: $30.00` and `max_parallel_default: 4`. The synthesis retains dozens of gates across rendering, streaming, physics, audio, simulation, persistence, GPU, networking, and UI. Several tasks require C++ builds plus runtime visual runs; `Endurance300` alone is expensive and time-consuming. A broad Wave 1 with four heavy tasks plus full integration is unlikely to fit one day of budget and review.
- Mitigation: Cap each daily wave at three narrow implementation tasks plus one integration/validator task, or fewer when `RenderPipeline.cpp` or `main_client.cpp` is involved. Each task should have a targeted build/test command, while full build plus `MaterialVisual`/`LodGround`/`WaterVisual` runs at wave integration. Reserve `Endurance300` for the final visual-stability wave.
- Verdict: blocker

### Finding 14: "Frontier systems remain disabled or explicitly gated" is not machine-checked
- Finding: The spec says persistence runtime paths, GPU SDF runtime toggle, far-field SDF, networking authority, Aetheric simulation, GOAP/Instinct, and Lua hot reload must remain disabled or explicitly gated. No current validator checks source flags, runtime flags, build manifests, or scenario availability to prove those systems are disabled. This makes the no-deferral rule aspirational.
- Mitigation: Add a `FrontierDisabled` or equivalent source/runtime validator before feature work begins. It should inspect build manifests and runtime flags for disabled frontier paths, then allowlist any path only when its deterministic validator exists and passes.
- Verdict: revise

### Finding 15: Acceptance criteria still contain broad wording that cannot fail deterministically
- Finding: Phrases such as "preserve the visual gates", "water quality claims", "frontier systems remain disabled or explicitly gated", "debug build succeeds without warnings-as-errors breaks", and "coverage ambition" are meaningful goals but incomplete acceptance criteria. The strongest parts of the research are artifact names, schemas, pixel counts, counters, and concrete commands; the weakest parts fall back to prose.
- Mitigation: Convert every acceptance criterion into one of: command exits zero, artifact exists with schema version, field threshold passes, source invariant check passes, or documented non-goal. Remove or demote any criterion that cannot be measured by a named command in the task.
- Verdict: revise

## Verdict Summary

Blockers:
- Finding 1: The roadmap cannot be finalized without a concrete task graph.
- Finding 2: MaterialVisual is stated as first, but not enforced as first everywhere.
- Finding 3: Endurance revalidation is still not a hard terminal gate.
- Finding 4: RenderPipeline modularization has no enforced no-behavior-change gate.
- Finding 5: Many proposed gates are only names, not executable acceptance criteria.
- Finding 6: Hidden file coupling will collide under parallel dispatch.
- Finding 10: Persistence, GPU SDF, and networking are grouped despite hard prerequisites.
- Finding 13: Budget realism is not reflected in wave size or verification cost.

Revise:
- Finding 7: SHIELD streaming and job-system changes are too close to mature behavior.
- Finding 8: Physics/player proposals mix bug fixes, architecture, and design policy.
- Finding 9: Simulation, Lua, Aetheric, and Instinct are too large for one contract wave.
- Finding 11: Audio gates overclaim what no-device telemetry can prove.
- Finding 12: UI/tooling/test gates include subjective or unbaselined acceptance.
- Finding 14: "Frontier systems remain disabled or explicitly gated" is not machine-checked.
- Finding 15: Acceptance criteria still contain broad wording that cannot fail deterministically.

Accept with note:
- None. The synthesis is directionally sound, but the revision step must narrow scope, enforce sequencing, and make the named gates executable before any roadmap or dispatch graph is approved.
