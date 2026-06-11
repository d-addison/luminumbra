# Iteration 3 Synthesis — Adversarial Critique

Critique of `research.md` + `ENGINE-ITERATION-3-2026-06-10.md` against the four
panels, the iteration-2 closeout, and the ORIGINAL VISION DOCUMENTS the
synthesis did not have: the current README ("Quantum" engine TDD) and the
273-line game design README on the old branches (`develop` etc. — "Project
Capture", surfaced by the owner 2026-06-10 after synthesis).

### Finding 1: Scope is ~42 tasks — two iterations wearing one trenchcoat
- Finding: Panel task counts (12 + 8 + 10 + 12) match iteration 2's ENTIRE
  19-task graph twice over. Iteration 2 took a full working day of multi-agent
  execution for 19 tasks with mature gates; 42 tasks with five new subsystems
  (far-LOD store, skeletal pipeline, headless server, biome system, fixed
  tick) is not one iteration.
- Mitigation: adopt the Cut Line below (~22 tasks). Defer wholesale: biomes,
  rivers, structures, GPU SDF enablement, Lua hot-reload, multi-anchor
  streaming, meshing-skip StreamingProfile, WorldList, audio. Shaping +
  data-driven materials already deliver the owner-visible "normal land +
  variety" without the biome system.
- Verdict: blocker

### Finding 2: The vision documents resolve the synthesis's open conflicts —
adopt them as decisions, not options
- Finding: The synthesis leaves the tick rate (30 vs 60 Hz) and the far-field
  end-state as design-time questions. The TDD is explicit: **30 Hz fixed tick**
  (§3.2, matching WaterSystem's documented tick) and **input-based lockstep
  replication** (only inputs sent; bit-identical sim) — which also retroactively
  justifies every determinism gate we built. Far-field destination is **SDF
  raytracing beyond ~256 m ("infinite view distance")**: panel 1's DH-style
  heightfield store remains the correct NOW-step, but the F3 (3 km) stretch
  tier should be deprioritized in favor of keeping the GPU SDF/raymarch road
  open — don't gold-plate the waypoint.
- Mitigation: plan pins tick = 30 Hz; networking posture = input-lockstep
  contracts (per-tick world-hash exchange as desync oracle — already aligned);
  far-LOD F1/F2 only, F3 cut; record SHIELD-RT as the far-field end-state in
  the plan's Success Definition.
- Verdict: blocker (decisions must be IN the plan)

### Finding 3: The "free" SDF-skip breaks the seam fallback patches
- Finding: Panel 1's hyper-optimization ("step>1 chunks generate a 17³ SDF the
  mesher never reads") is wrong about "never reads":
  `AddBoundaryTransitionSkirts` → `AppendFallbackFacePatches`
  (MarchingCubes.cpp) reads `chunk.sdf_data` on coarse chunks to emit
  transition cover where the mesh has no boundary edges — that path closed the
  iteration-2 black-sliver defect. Skipping SDF generation for step>1 chunks
  silently disables fallback patches (the function early-returns on empty
  sdf_data) and regresses LodSeamRisk.
- Mitigation: the SDF-skip task must EITHER (a) generate boundary-face-band
  SDF only (the 4 faces, ~17×17×4 samples — still reclaims the volume +
  3D cave noise), or (b) rewrite the fallback patch emitter to sample
  GetTerrainHeightAt directly, gated by LodSeamRisk red/green before/after.
  Acceptance: LodSeamRisk + LodGround + the meshing determinism hashes green
  with the skip enabled.
- Verdict: blocker

### Finding 4: Span fix has unlisted consumers and perf-baseline fallout
- Finding: `m_column_surface_chunk_y_cache` is consumed by the T-I2-14
  streaming optimization (candidate collection + activation) and the LOD band
  logic; replacing point-sampling with span-sampling changes candidate
  volumes, so streaming_walk/chunk_churn baselines (now 7–11 ms after the
  iteration-2 win) WILL shift. The synthesis flags the snapshot-test count but
  not the baseline re-bless or the five cache consumers.
- Mitigation: the span task explicitly lists every cache consumer, pairs with
  the SDF-skip in ONE merge window, and budgets a deliberate
  `capture-perf-baseline.ps1` re-bless (previous block preserved) in the same
  PR with before/after telemetry in the commit.
- Verdict: revise

### Finding 5: File-contention map missing — claimed parallelism is optimistic
- Finding: SHIELD_WorldSystem.{h,cpp} is touched by: span fix, SDF-skip,
  shaping, preset loader, server runner (reads), FieldSystem hosting, biome
  hooks. RuntimeScenarioHarness.{h,cpp} by: player_view, farlod gate, skinned
  smoke. validate-engine-frontier.ps1 by nearly everything. The synthesis
  implies stream-level parallelism that would put 3+ agents in the same TUs.
- Mitigation: the plan assigns FILE OWNERSHIP per wave like iteration 2 did:
  wave-internal SHIELD tasks serialize through one agent; harness/validator
  additions are append-only and merge-serialized; the worldgen stream and the
  character stream are the only truly disjoint pair and get the worktrees.
- Verdict: revise

### Finding 6: "6x" is still unpinned in acceptance numbers
- Finding: The spec's farlod gate says "6x" but panel 1 itself flagged
  radius-12-vs-RENDER_DISTANCE ambiguity. An acceptance criterion that needs
  interpretation is prose, not a gate.
- Mitigation: pin it: baseline = radius 12 (192 m); 6x = **1152 m minimum
  visible horizon**; far-LOD gate asserts complete coverage to 1536 m (F2
  outer edge) so the requirement is met with margin. Write the numbers into
  the spec and the farlod gate thresholds.
- Verdict: revise

### Finding 7: Persistence-v2 + far-LOD co-design couples two unbuilt systems
- Finding: "Co-design one container" is schedule glue between two large tasks
  owned by different streams; if either slips, both slip.
- Mitigation: extract a 1-day "region container spec" task (header layout,
  addressing, lod_level field, hash-stability contract) that BOTH implement
  against independently; the spec doc is the dependency, not each other's
  code.
- Verdict: revise

### Finding 8: The game slice should be a Project Capture slice
- Finding: Panel 3's "character content slice" is generic (spawn a rigged
  model, planner picks clips). The vision README defines the actual game:
  photography of creatures reacting to light/shadow. A generic slice spends
  the same effort without testing ANY game-defining mechanic.
- Mitigation: the slice becomes: one creature (vision MVP roster) with
  idle/walk clips, planner-driven behavior responding to a light stimulus
  (the emissive/field hookup doubles as the Glimmer-stone precursor), in the
  archipelago world — i.e. the minimal photographable moment. Camera/lens
  systems stay iteration 4+, but the slice must be observable through the
  existing free camera.
- Verdict: revise

### Finding 9: Open items buried as task footnotes
- Finding: durable-entity-id design (blocks panel 2 T8), NeedsComponent dual
  representation (breaks EngineContracts tests if migrated carelessly), sol2
  under warnings-as-errors — each is named but none has an owner/decision in
  the synthesis.
- Mitigation: durable-id design folds into the region-container/spec task
  (ids live in snapshots); T8 (real-world loopback) is CUT this iteration;
  NeedsComponent migration gets its own micro-task with the contract tests
  updated atomically; sol2 pragma-wrapping is task 1 of the Lua work — and if
  Lua slips to iteration 4, nothing else depends on it (verify: planner
  runtime must NOT require Lua).
- Verdict: revise

### Finding 10: Atmospheric pillar absent without acknowledgment
- Finding: The vision's third pillar (weather/seasons/wind grids) has its seed
  (weather overlay) but no iteration-3 presence and no roadmap note —
  silently dropped streams are how visions rot.
- Mitigation: one line in the plan's deferred table: seasons/wind-grid stream
  scheduled for the iteration after characters land (foliage + wind is the
  "Living Diorama" pillar and pairs with instanced foliage).
- Verdict: accept-with-note

## Verdict Summary
- Blockers: F1 (scope cut), F2 (vision decisions into plan), F3 (SDF-skip vs
  seam fallback).
- Revise: F4 (span consumers + baseline re-bless), F5 (contention map),
  F6 (pin 6x numbers), F7 (container spec task), F8 (Project Capture slice),
  F9 (open-item owners).
- Accept-with-note: F10 (Atmospheric roadmap line).

## Recommended Cut Line (iteration 3 proper, ~22 tasks)

Wave 0 (decisions, 1 task): design doc — seed-offset registry, 30 Hz tick,
region container spec (incl. lod_level + durable-id note), 6x = 1152 m pin.
Wave 1 (enablers ∥ where files allow): SimulationClock @30 Hz;
TerrainPresetLoader; asset-manifest split; SDF-skip WITH seam-fallback safety;
span fix + vertical-unload exemption (one SHIELD agent, serialized);
player_view_smoke gate.
Wave 2: far-LOD store (F1+F2 tiers) + region mesher/render/scheduler +
live/far seam gate + farlod_horizon_smoke @1536 m; terrain shaping +
slope-histogram gate (parallel worktree); persistence v2 (against the
container spec).
Wave 3: headless server runner + headless_server_smoke (meshing ON only);
skeletal import (LMS2 + .lanim) + anim core + pose determinism gate; skinned
G-buffer path + capture gate; fixed-tick hosting + planner runtime +
relocations + split-lint; Project Capture creature slice.
Kept small extras: LOD hysteresis + ratchet; release perf lane; UI UAF fix.
Deferred to iteration 4 (recorded): biomes, rivers, structures, GPU SDF
enablement, Lua bindings/hot-reload, multi-anchor, StreamingProfile
meshing-skip, WorldList, audio reverb, F3 tier, camera/photography systems,
seasons/wind.
