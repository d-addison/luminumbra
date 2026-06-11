# Iteration 4 — Critique (Fable, 2026-06-11)

Adversarial pass over `ENGINE-ITERATION-4-2026-06-11.md`. Verified facts
first, then findings. All findings are RESOLVED into the dispatch; none
block.

## Verified against the codebase

- **FP flags (spec risk 3)**: no -ffast-math / -ffp-contract / -march
  anywhere in the build (only -Wall/-Wextra/-Wpedantic/-Werror). Baseline
  x86-64 codegen cannot emit FMA, so pinning `-ffp-contract=off` +
  excluding fast-math for common/server is EXPECTED hash-neutral. C1 pins
  the flags and proves neutrality (full ctest + HeadlessServerTick hash
  unchanged) in the same commit; if neutrality fails, C1 becomes the
  mega-bump commit and C2+ wait. Not a blocker for A/B.
- **Far-tile cache key (spec risk 1)**: FarLodSystem::bind_world keys on
  ComputeTerrainParamsHash(params, seed) (FarLodSystem.cpp:103); pristine
  tiles with stale params_hash are discarded on load (FarLodStore.cpp:314).
  Therefore A2 MUST extend ComputeTerrainParamsHash with the biome-table
  content hash — stale caches then self-invalidate (they are cache;
  regeneration is the policy, documented).

## Findings → resolutions

- F1 (A∥B materials.json contention): Wave 0 defines the COMPLETE LUT
  schema up front (palette columns for A2; texture/normal layer indices +
  tiling for B2; roughness column for B5; emission calibration for B4).
  A2 merges before any B-task merge that edits materials.json; B-agent
  rebases.
- F2 (B2 close-range gate needs guaranteed framing): adopt the calibration-
  plate pattern — the scenario teleports to authored per-material test
  patches (coordinates in the scenario, patches placed by world-edit at
  scenario start), two sun angles via existing time-of-day control for the
  normal-response check. Specified in Wave 0.
- F3 (TCP head-of-line blocking): accepted for co-op LAN scope (research
  concurs for delay-based, non-competitive); v1 session semantics: any
  disconnect ends the session cleanly (no mid-session rejoin); max input
  horizon documented in the design doc.
- F4 (replay indexing, spec risk 5): replay records are TICK-indexed;
  SimulationClock catch-up/drop telemetry is excluded from the record;
  playback drives ticks directly (no wall clock). C1's lint (no time/RNG
  in sim paths) is the guard that makes this sound.
- F5 (rivers × water, spec risk — new): river carve uses existing water
  machinery: carve PV-band terrain below the river waterline, water fill
  at that level via the same path archipelago sea uses. WaterVisual gains
  no new scope; the river atlas gate checks waterline continuity instead.
  Decided in Wave 0.
- F6 (spike scope creep): single task, single artifact, explicit
  stop-condition: no second benchmark round without owner review.
- F7 (O1 MDI blast radius, spec risk 6): O1 runs strictly AFTER C4 merges
  (sole render-path owner at that point), live-chunk paths only (G-buffer +
  shadow), far regions explicitly out of v1; Endurance300 mandatory at the
  optimization-wave boundary; RenderHealth must be byte-stable (drawing
  mechanism, not output, changes).
- F8 (executor sizing): Wave A splits into two sequential agents (A1-A3
  worldgen core; A4-A5 structures+audio) to stay under per-agent budget
  observed in iteration 3 (characters agent: 517k tokens for 3 deep
  tasks). Wave B splits B1-B2 / B3-B5. All implementation `opus-agent`.
- F9 (MaterialVisual re-home ownership): explicitly owned by B2's gate
  task — the deferred iteration-3 diagnosis (handoff) is its starting
  input; closeout asserts the re-homed gate green.

## Verdict

Spec sound with the above folded in. Sequencing: Wave 0 inline → A(2
agents serial) ∥ B(2 agents serial) ∥ spike → C(1 agent, after A re-bless)
→ O(1 agent) → closeout. The dispatch graph encodes this.
