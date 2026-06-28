# Spec 018: Determinism Hardening — Two-Worlds Residency + Determinism Test Matrix + Audit Gate

> Status: SPEC (created 2026-06-26). Derived from the engine-infrastructure devil's-advocate
> critique (`.forge/critique-engine-infrastructure-framework-20260626-200421.md`, finding F5).

## The framing insight (why this spec exists)

The run/replay + `world_hash` system is a genuinely strong foundation — but it is a *discipline*,
not yet a *framework*. The difference matters: a discipline is a culture engineers must remember to
follow; a framework makes the illegal state hard to express. Today the engine has the discipline
(fixed-tick, hash exchange, halt-on-mismatch, mesh excluded from the sim hash) but the framework
boundary is informal. Three concrete cracks prove it is fragile, not finished:

1. **Streaming arrival timing already leaked into sim state.** A chunk's `water_depth_mm` at a
   checkpoint tick depended on *which tick it streamed in* (async generation/meshing completion),
   not on the deterministic tick alone — a real latent lockstep desync masked only by single-PC
   testing (`docs/water-sim-lockstep-determinism.md:31`, `:50`). The fix was two ad-hoc patterns:
   a boot-settle of the initial residency (`ServerWorldRunner.cpp:369`) and a per-tick
   `wait_for_streaming_jobs()` barrier (`ServerWorldRunner.cpp:489`). Those patterns work, but they
   live in one system's code, not in a contract every streamed system inherits.

2. **The two-worlds split exists, but only as a hand-maintained exclusion list.** The render mesh
   is excluded from the determinism match because "collision uses the heightmap, not this mesh"
   (`main_server.cpp:465-468`); the exclusion is enforced by the `sub_hashes_match` expression
   *omitting* `mesh` (`main_server.cpp:469-478`). `SystemConfig` likewise excludes `render.*` from
   the config sub-hash and only emits enabled sim params (`SystemConfig.cpp:217`, exclusion at
   `:223`). This is the right idea — a deterministic SIMULATION world and a nondeterministic RENDER
   world — but it is encoded as "remember to leave this out of the match," which scales badly as
   spec 015 adds exposure metering, froxel temporal jitter, colored shadows, and OIT, and as spec
   014 changes the rendering substrate underneath all of it.

3. **The test matrix is one-dimensional.** `--smoke` double-runs on ONE machine, back-to-back,
   same worker count, same build mode (`main_server.cpp:469-481`). It caught the water flake only
   *eventually* and *by luck of timing*. The hostile conditions that would surface a desync —
   different worker counts, fast-vs-slow job completion, separate processes, replay drift, and
   debug-vs-release floating-point/codegen differences — are not exercised.

This spec turns F5's best-in-class recommendation into a framework: **formalize TWO WORLDS**, make
any `world_hash`-affecting system consume **only deterministic inputs**, promote water's
deterministic-residency model to the **pattern** every future streamed system inherits, build a
**determinism test matrix** across the hostile axes, and gate every new render-adjacent feature
behind a **determinism audit checklist**. It defines the **deterministic availability set** contract
that spec 017 (engine concurrency) consumes, and the **determinism audit gate** that guards spec 015
(atmospheric lighting) and spec 016 (render framework).

**This spec writes ZERO engine behavior change.** Its deliverables are: a residency *contract* and
*assertions* (illegal states fail to compile / fail a gate), test *harnesses* over existing flags,
and a *checklist gate*. The canonical baseline `--smoke == 6f008a9f637c40b7`, run==replay, must hold
unchanged at every step — if any FR here moves the hash, that FR is wrong.

## Goals

- **G-1** — Formalize the **two-worlds residency model**: a deterministic SIMULATION residency whose
  state can affect `world_hash`, and a nondeterministic RENDER residency that cannot. Make the
  boundary a declared, asserted contract — not a hand-maintained exclusion list.
- **G-2** — Establish the **deterministic availability set** contract: sim state advances from a
  deterministically-ordered availability set (chunk residency settled via the per-tick barrier /
  boot-settle), never from wall-clock job-completion order. This is the contract spec 017 consumes.
- **G-3** — Promote water's deterministic-residency model (boot-settle + per-tick streaming barrier)
  to the **reusable pattern** every future streamed system (foliage growth, erosion, weather-driven
  hydrology, creature ecology) inherits, instead of re-deriving it ad hoc.
- **G-4** — Build a **determinism test matrix** that runs the existing `--smoke` / `--smoke-moving`
  oracle across hostile axes: cross-worker-count, fast/slow job completion, multiprocess (one box),
  replay, and build-mode (debug vs release).
- **G-5** — Define a **GPU→CPU readback discipline**: GPU-sourced data is render-only unless it is
  explicitly delayed, quantized, and hashed — guarding spec 015's exposure metering and froxel
  temporal jitter from accidentally becoming sim inputs.
- **G-6** — Add a **determinism audit gate / checklist** applied to every new render-adjacent
  feature (exposure, weather, volumetrics, glass, foliage, physics), and expand **sub-hash
  localization** beyond the oldest terrain/water/entities categories so future desyncs localize fast.
- **G-7** — Preserve the canonical baseline: `--smoke == 6f008a9f637c40b7`, run==replay, unchanged.

## Non-Goals

- **NG-1** — No new simulation, worldgen, streaming, water, or render *behavior*. This spec adds
  contracts, assertions, harnesses, and a gate; it does not change what the engine computes. Any
  `world_hash` movement is a defect of this spec, not an outcome.
- **NG-2** — Not the concurrency rewrite itself (async ownership, readback rings, removing
  main-thread waits) — that is **spec 017**. This spec defines the *determinism contract* 017 must
  satisfy; 017 does the threading work.
- **NG-3** — Not the render-framework / frame-graph extraction (**spec 016**) nor the RHI migration
  (**spec 014**). This spec's audit gate is a *guard* those specs pass through, not their content.
- **NG-4** — Not the visual-parity (FLIP) gate. That guards the RENDER world's *appearance*; this
  spec guards the SIMULATION world's *determinism*. They are orthogonal and complementary.
- **NG-5** — No two-box LAN / real-Steam-transport validation. The single-PC testing constraint
  holds: multiprocess soak runs as N processes on ONE box (NG of spec 019's transport matrix).
- **NG-6** — Not a change to which sub-hashes are *folded into* `world_hash` (that would re-pin the
  baseline). Sub-hash *localization* (G-6) is additive/reporting-only, per the existing additive
  sub-hash design (`ServerWorldRunner.cpp:536-573`).

## Functional Requirements

IDs are grouped. Each is anchored to a verified file/line in the current tree (see **Key files**).
Paths corrected from the critique where the directory had drifted (the line numbers were accurate;
see the **Anchor corrections** note under Key files).

### Group A — Two-worlds residency contract

- **FR-A-001 — Declared residency partition.** The engine shall declare two residency classes in a
  single authoritative location: `SimResidency` (deterministic; may contribute to `world_hash`) and
  `RenderResidency` (nondeterministic; must not). Today the partition is implicit — the render mesh
  sub-hash is computed and *reported* but omitted from the run==replay match
  (`main_server.cpp:465-478`), and `SystemConfig` hashes only `Section::Sim` keys, skipping
  `render.*` (`SystemConfig.cpp:223`). FR-A-001 makes that partition an explicit named contract.
- **FR-A-002 — Deterministic-input invariant.** Any value that feeds `ComputeWorldHash`
  (`ServerWorldRunner.cpp:510`) or `ComputeWorldSubHashes` (`:536`) shall be derivable purely from
  `(seed, preset, deterministic-config, tick)` and the deterministic availability set (FR-B-001) —
  never from wall-clock, job-completion order, thread scheduling, or GPU readback. This invariant
  shall be stated as a contract docstring/assertion at the hash-assembly site so additions are
  reviewed against it.
- **FR-A-003 — Render-residency exclusion is enforced, not remembered.** The mesh-style exclusion
  shall be expressed so that a SIM-truth sub-hash CANNOT be silently dropped from the match and a
  RENDER value CANNOT be silently added to the hash. Concretely: the `sub_hashes_match` set
  (`main_server.cpp:469-478`) and the folded composite (`ServerWorldRunner.cpp:527-533`) shall be
  driven from the same declared partition (FR-A-001), with a test asserting every `SimResidency`
  sub-hash is in the match set and no `RenderResidency` value is folded.
- **FR-A-004 — Config residency parity.** `SystemConfig::ComputeConfigSubHash` shall remain
  render-excluded and sim-only-when-enabled (`SystemConfig.cpp:217-244`), and a test shall assert
  that every config key's `Section` agrees with the FR-A-001 partition (no `render.*` key reachable
  by the hash; every `Sim` key declared sim-residency). All-sim-defaults must still produce the
  empty/byte-identical baseline (`SystemConfig.cpp:243`).

### Group B — Deterministic availability set (the contract spec 017 consumes)

- **FR-B-001 — Availability-set definition.** The spec shall define the **deterministic availability
  set** as the contract: at each sim tick, the set of chunks/resources a deterministic system may
  read is the *settled residency* produced by the per-tick streaming barrier
  (`world_system->wait_for_streaming_jobs()`, `ServerWorldRunner.cpp:489`/`:516`/`:591`) plus the
  boot-settle of initial residency (`ServerWorldRunner.cpp:369-393`). Sim advances from this set in
  a deterministic order, **not** from the order jobs happen to finish.
- **FR-B-002 — Barrier is the only legal availability source.** No deterministic system shall read
  chunk/streamed state outside the availability set (i.e., shall not observe a chunk whose
  generation/meshing has not been quiesced for the current tick). The per-tick barrier at
  `ServerWorldRunner.cpp:489` and the hash-time quiesce at `:516`/`:546`/`:591` are the sanctioned
  entry points; new streamed sim systems shall consume residency through the same barrier.
- **FR-B-003 — Spec 017 consumption clause.** This spec shall state, as the explicit interface spec
  017 (engine concurrency) builds against: when 017 replaces main-thread `wait_for_streaming_jobs()`
  with async ownership / per-tick completion budgets, the deterministic sim must still advance from
  the *deterministic availability set* (a stable, tick-keyed activation queue), not from wall-clock
  completion. 017's perf work may change *how* residency settles; it must not change *what the sim
  sees per tick*. (Cross-ref: critique F4 — main-thread waits; spec 017.)
- **FR-B-004 — Bed/sampler purity carry-forward.** The water lesson — read terrain via the pure
  scalar sampler (`GetTerrainHeightAt`, gated `current_lod == 0`) rather than shared mutable
  `heightmap_data` (`docs/water-sim-lockstep-determinism.md:50-61`) — shall be generalized into a
  contract clause: deterministic systems read terrain through the **pure sampler**, never through
  shared mutable streaming buffers. (Cross-ref memory: water-heightmap-read-race.)

### Group C — Streamed-system residency pattern (water as the template)

- **FR-C-001 — Boot-settle pattern.** The boot-settle template (`ServerWorldRunner.cpp:369-393`):
  drive streaming to stable residency, then drive the system to a trajectory-independent steady
  state on the SHARED boot path both peers run — shall be documented as the reusable pattern for any
  system whose per-tick state depends on streaming arrival (foliage growth, erosion, finite
  hydrology, ecology). The pattern shall be expressed so a new system opts in, not re-implements.
- **FR-C-002 — Moving-residency harness coverage.** Any new streamed sim system shall be required to
  pass the moving-anchor harness (`--smoke-moving`, `ServerWorldRunner.cpp:472-479`,
  `main_server.cpp:266-268`), which streams chunks IN ahead / OUT behind during the run to reveal
  per-tick trajectory coupling that boot-settle alone does not cover.
- **FR-C-003 — Halting-oracle classification.** Each streamed sim system shall declare whether it is
  a "halting peer hash oracle" (its sub-hash is exchanged and halts the lockstep session on
  mismatch — `docs/water-sim-lockstep-determinism.md:38-46`). Systems that are halting oracles MUST
  satisfy FR-C-001 + FR-C-002 before their sub-hash is folded into the exchanged set.

### Group D — Determinism test matrix

- **FR-D-001 — Cross-worker-count axis.** The matrix shall run `--smoke` / `--smoke-moving` under at
  least two distinct job-system worker counts (e.g. 1 and N) and assert identical
  `world_hash`/sub-hashes across them. Different worker counts change job-completion order, which is
  exactly the timing variable the water flake exploited.
- **FR-D-002 — Fast/slow job axis.** The matrix shall run with an artificially throttled (slowed)
  job completion vs. unthrottled and assert identical hashes. This simulates the fast-vs-slow-machine
  divergence that single-PC back-to-back runs (`main_server.cpp:469-481`) cannot.
- **FR-D-003 — Multiprocess axis (one box).** The matrix shall run two SEPARATE `--smoke` processes
  on ONE machine and compare their `--artifact` JSON (`main_server.cpp:295-296`,
  artifact emit `:421-510`); their SIM-truth sub-hashes must match. Per NG-5 / the single-PC
  constraint, this is N processes on one box — not a two-box LAN.
- **FR-D-004 — Replay axis.** The matrix shall record (`--record`) and replay (`--replay`,
  `main_server.cpp:230-235`) a run and assert `world_hash == world_hash_replay`
  (the existing in-process double-run already asserts this — `main_server.cpp:469-481`; this axis
  extends it to a recorded fixture replayed in a fresh process).
- **FR-D-005 — Build-mode axis.** The matrix shall run `--smoke` in BOTH debug and release builds
  and assert identical SIM-truth hashes, surfacing optimizer-dependent float/codegen divergence.
  (Cross-ref memory: procgen-hash-signed-overflow-UB — a release-only hang from signed-overflow UB;
  the two build trees, build-tree-gotcha memory, make this axis cheap to run.)
- **FR-D-006 — Localized failure output.** When any matrix axis fails, the harness shall emit the
  per-system sub-hash diff (which `desync_section` differs) using the existing sub-hash artifact
  fields (`main_server.cpp:421-428`, `:499-510`), so the failing system is named, not just "desync".

### Group E — GPU→CPU readback discipline

- **FR-E-001 — Readback is render-only by default.** Any value read back from the GPU
  (e.g. `glGetBufferSubData`, fenced SDF/foliage readbacks — critique F4 cites
  `RenderPipeline.cpp:4308`/`:4323`, `FoliagePass.cpp:784`) shall be classified `RenderResidency`
  (FR-A-001) and shall NOT feed `world_hash` unless it satisfies FR-E-002.
- **FR-E-002 — Delayed + quantized + hashed escape hatch.** A GPU→CPU value may become a sim input
  ONLY if it is (a) **delayed** to a deterministic tick boundary (not consumed same-frame), (b)
  **quantized** to an integer/fixed-point representation (no raw float bit patterns), and (c)
  **hashed** into a sub-hash so divergence is caught. Absent all three, it stays render-only.
- **FR-E-003 — Spec 015 guard (exposure + froxel jitter).** Exposure metering and froxel temporal
  jitter from spec 015 shall be asserted render-only: spec 015's exposure stage (FR-A-004 there) and
  froxel temporal reproject (its OQ-6) are auto-exposure / GPU-derived and MUST NOT feed
  `world_hash`. This spec's audit gate (Group F) is the mechanism that enforces it. (Cross-ref:
  spec 015 NFR-001, which already declares every pillar render-only; this spec makes that assertable.)

### Group F — Determinism audit gate + sub-hash localization

- **FR-F-001 — Audit checklist.** A determinism audit checklist shall be authored and applied to
  every new render-adjacent or streamed feature (exposure, weather, volumetrics, glass, foliage,
  physics). Checklist items: residency class declared (FR-A-001); no GPU readback feeds the hash
  (FR-E-001); reads terrain via pure sampler not shared buffers (FR-B-004); if streamed-sim, passes
  boot-settle + `--smoke-moving` (Group C); if a halting oracle, sub-hash folded only after C-passes
  (FR-C-003).
- **FR-F-002 — Gate enforcement.** The checklist shall be enforced as a gate (extending the
  engine-frontier validator, `.forge/scripts/validate-engine-frontier.ps1`) so a feature that
  touches a hash-feeding path without a completed checklist fails verification.
- **FR-F-003 — Guard specs 015 and 016.** The gate shall be the determinism guard that specs 015
  (atmospheric lighting — exposure/froxel must stay render-only) and 016 (render framework — frame
  graph / pass-resource contract must not leak render state into sim) pass through. Their
  render-only claims become *checked* claims, not asserted ones.
- **FR-F-004 — Sub-hash localization expansion.** Per-system sub-hash localization shall expand
  beyond the oldest terrain/water/entities categories. The localization set already carries
  wind/weather/aether/scent/ecology/plant (`ServerWorldRunner.cpp:527-533`, `:561-572`;
  `main_server.cpp:421-428`); FR-F-004 mandates that every NEW streamed sim system add its own
  localization sub-hash (reporting-only, additive — NOT necessarily folded into `world_hash`, per
  NG-6) so a future desync names the system on first failure.

## Non-Functional Requirements

- **NFR-001 — Baseline immovable (hard gate).** `luminumbra_server_app --smoke` must equal
  `6f008a9f637c40b7`, run==replay, before and after EVERY change in this spec. This spec adds
  contracts/tests/gates only; if the hash moves, the change is reverted. (Render mesh stays excluded
  from the match — `main_server.cpp:465-468`; the localization sub-hashes added by FR-F-004 are
  additive/reporting-only, NOT folded — NG-6.)
- **NFR-002 — Single-PC constraint.** The multiprocess axis (FR-D-003) and all soak runs execute as
  N processes on ONE box. No two-box LAN, no real-Steam-transport validation (deferred to spec 019's
  transport matrix). The TCP/loopback paths may be used for over-the-wire local exercise.
- **NFR-003 — Two build trees.** The build-mode axis (FR-D-005) must run the binary it tests in each
  tree: `cmake --build build` → `build/bin` vs the engine-frontier `cmake --build --preset debug` →
  `build/debug`. The matrix must record which tree/binary produced each result (build-tree-gotcha:
  testing a stale binary silently invalidates the gate). (Cross-ref: critique F7 — build operability.)
- **NFR-004 — Additive, zero re-pin.** All sub-hash localization (FR-F-004) and config-residency
  assertions (FR-A-004) are additive and sim-only-when-enabled, matching the existing
  ComputeConfigSubHash design (`SystemConfig.cpp:217-244`): all-off / all-default baselines stay
  byte-identical, so no determinism golden is re-pinned by this spec.
- **NFR-005 — Cost-bounded matrix.** The full matrix (Group D) is heavier than a single `--smoke`;
  it shall be runnable as a tiered gate — a fast subset (cross-worker + replay) per change, the full
  matrix (incl. build-mode + multiprocess soak) on a cadence — so the framework does not slow the
  inner loop. (Cross-ref: critique F7 — manual/heavy tests should be tiered, not invisible.)
- **NFR-006 — Contract surfaces are the documentation.** The residency partition (FR-A-001), the
  availability-set contract (FR-B-001), and the audit checklist (FR-F-001) shall live next to the
  code they govern (hash assembly site, SystemConfig, the engine-frontier validator), not only in
  this spec, so the framework is discoverable from the code.

## Acceptance Criteria

Each names a measurable signal / test command. Server flags grounded in
`src/luminumbra_server/main_server.cpp` arg parsing.

### Cross-cutting
- [ ] **AC-001** — `luminumbra_server_app --smoke` equals `6f008a9f637c40b7`, run==replay, after
  every change (the gate's `deterministic` + `passed` flags — `main_server.cpp:480-485`).
- [ ] **AC-002** — `--smoke-moving` is run==replay (the moving-anchor harness still passes;
  `main_server.cpp:266-268`).
- [ ] **AC-003** — No determinism golden literal is re-pinned (grep the baseline `6f008a9f` across
  docs/tests is unchanged); all additions are additive/reporting-only (NFR-004).

### Group A — two-worlds residency
- [ ] **AC-A-001** — A `SimResidency`/`RenderResidency` partition is declared in one authoritative
  location, and a test asserts the `sub_hashes_match` set (`main_server.cpp:469-478`) is exactly the
  declared `SimResidency` sub-hashes (mesh — `RenderResidency` — is absent).
- [ ] **AC-A-002** — A test asserts no `RenderResidency` value is folded into `ComputeWorldHash`
  (`ServerWorldRunner.cpp:527-533`).
- [ ] **AC-A-003** — A test asserts every `SystemConfig` key's `Section` agrees with the partition;
  `ComputeConfigSubHash` with all sim defaults returns empty (`SystemConfig.cpp:243`).

### Group B — availability-set contract
- [ ] **AC-B-001** — The spec defines the deterministic availability set anchored to the per-tick
  barrier (`ServerWorldRunner.cpp:489`/`:516`/`:591`) + boot-settle (`:369`); a doc/contract comment
  exists at the hash-assembly site stating the deterministic-input invariant (FR-A-002).
- [ ] **AC-B-002** — A negative test (or static assertion) demonstrates that reading streamed state
  outside the barrier is rejected/flagged (FR-B-002).
- [ ] **AC-B-003** — The 017-consumption clause (FR-B-003) is written and cross-referenced from
  spec 017's concurrency contract section.

### Group C — streamed-system pattern
- [ ] **AC-C-001** — The boot-settle + barrier pattern is documented as a reusable opt-in template
  (FR-C-001), referencing `ServerWorldRunner.cpp:369-393`.
- [ ] **AC-C-002** — A new (or existing candidate) streamed sim system is shown passing both
  `--smoke` and `--smoke-moving`, demonstrating the pattern (FR-C-002).
- [ ] **AC-C-003** — Each halting-oracle system declares its classification (FR-C-003); water is the
  documented exemplar (`docs/water-sim-lockstep-determinism.md:38-46`).

### Group D — test matrix
- [ ] **AC-D-001** — `--smoke` produces identical SIM-truth hashes under 1 worker vs N workers.
- [ ] **AC-D-002** — `--smoke` produces identical SIM-truth hashes throttled vs unthrottled job
  completion.
- [ ] **AC-D-003** — Two separate `--smoke` processes on one box produce matching SIM-truth
  sub-hashes (compare their `--artifact` JSON — `main_server.cpp:295-296`, `:421-510`).
- [ ] **AC-D-004** — `--record` then `--replay` in a fresh process yields
  `world_hash == world_hash_replay` (`main_server.cpp:230-235`, `:498`).
- [ ] **AC-D-005** — `--smoke` SIM-truth hashes match between debug (`build/bin`) and release builds;
  the run records which build tree produced each (NFR-003).
- [ ] **AC-D-006** — An injected sub-hash mismatch causes the harness to NAME the failing system via
  the sub-hash diff (`main_server.cpp:421-428`), not just report "desync".

### Group E — readback discipline
- [ ] **AC-E-001** — A test asserts no GPU-readback path (e.g. SDF/foliage readback —
  `RenderPipeline.cpp:4308`/`:4323`, `FoliagePass.cpp:784`) feeds a hash-contributing value
  unless it is delayed+quantized+hashed (FR-E-002).
- [ ] **AC-E-002** — Spec 015's exposure stage and froxel temporal jitter are asserted render-only:
  `--smoke == 6f008a9f637c40b7` holds with spec-015 features toggled (FR-E-003; cross-ref 015
  NFR-001).

### Group F — audit gate + localization
- [ ] **AC-F-001** — The determinism audit checklist exists and is enumerated (FR-F-001).
- [ ] **AC-F-002** — The engine-frontier validator (`.forge/scripts/validate-engine-frontier.ps1`)
  fails a feature that touches a hash-feeding path without a completed checklist (FR-F-002).
- [ ] **AC-F-003** — Specs 015 and 016 reference this gate as their determinism guard (FR-F-003).
- [ ] **AC-F-004** — Every new streamed sim system carries a localization sub-hash present in the
  artifact (`main_server.cpp:421-428` extended), reporting-only/additive (FR-F-004 / NG-6).

## Phasing (sequenced by leverage — contract first, then enforcement)

1. **Group A + B — the contract.** Declare the two-worlds partition and the deterministic
   availability set; add the deterministic-input invariant assertion at the hash-assembly site.
   Pure formalization of what mesh-exclusion / SystemConfig already do informally — lowest risk,
   highest clarifying payoff, and it is the contract spec 017 needs *before* it starts.
   *(cheap, unblocks 017)*
2. **Group D — the test matrix.** Build the hostile-axis harness over existing `--smoke` /
   `--smoke-moving` / `--artifact` / `--record`/`--replay`. This is where a latent desync (like the
   water flake) becomes *visible* instead of lucky. Tiered (NFR-005). *(high detection value)*
3. **Group C — the streamed-system pattern.** Document/extract water's boot-settle + barrier as the
   reusable template, with `--smoke-moving` as the entry gate for new streamed systems.
4. **Group E — readback discipline.** Classify GPU readbacks render-only; add the
   delayed+quantized+hashed escape hatch. Guards spec 015's exposure/froxel directly.
5. **Group F — the audit gate.** Wire the checklist into the engine-frontier validator; make
   specs 015/016 pass through it. Lands last because it depends on A–E being the things it checks.

## Blocking gates (per phase)

1. **Baseline (AC-001/002/003):** `--smoke == 6f008a9f637c40b7`, `--smoke-moving` run==replay, no
   golden re-pinned — at every phase. This spec must never move the hash.
2. **Matrix (AC-D-*):** the relevant matrix axes pass for the phase landed (cross-worker + replay as
   the fast subset per change; full matrix incl. build-mode + multiprocess on cadence).
3. **Additivity (AC-003 / NFR-004):** localization sub-hashes + config-residency assertions are
   additive; all-off/all-default baselines byte-identical.
4. **Gate self-consistency (AC-F-002):** the engine-frontier validator passes on the current tree
   and fails a synthetic checklist-incomplete feature.

## Risks / unknowns

- **The matrix may surface a REAL latent desync** (like water did) — that is the *point*, but it
  means a Group D failure is a discovery, not a harness bug; budget for a localize-and-fix loop
  using the per-system sub-hash diff (FR-D-006).
- **Build-mode axis (FR-D-005) is the most likely to bite** — optimizer-dependent float/codegen
  divergence (procgen-hash-signed-overflow-UB memory: release-only UB). If debug≠release surfaces,
  it is a real hardening win, but may require a hash-purity fix outside this spec's "no behavior
  change" envelope — flag it as a follow-up, do not silently change sim math here.
- **Throttle axis realism (FR-D-002)** — an artificial job slowdown must perturb completion ORDER,
  not just wall-time, to actually probe the timing coupling; a naive sleep that preserves order
  proves nothing. Design the throttle to reorder.
- **Gate friction (NFR-005)** — if the full matrix runs per change it will slow the loop; the tiered
  design must be honored or engineers will route around the gate.
- **Spec 017 coupling** — the availability-set contract (Group B) is consumed by 017's concurrency
  rewrite; if 017 changes residency mechanics, this contract's assertions must be revalidated
  against the new mechanism (the contract is *what the sim sees per tick*, which 017 must preserve).

## Open Questions

- **OQ-1 (blocks FR-A-001 shape)** — Should the residency partition be a compile-time type
  distinction (e.g. distinct handle types so a `RenderResidency` value cannot be passed to a
  hash-feeding function) or a runtime-asserted registry? Compile-time makes illegal states
  unrepresentable (the F5 ideal) but is more invasive; runtime is cheaper but discipline-shaped.
- **OQ-2 (drives FR-D-002 design)** — How to throttle job completion to perturb ORDER deterministically
  enough to be a repeatable gate, yet adversarially enough to expose timing coupling? (e.g. seeded
  per-job artificial latency vs. forced single-worker serialization in a shuffled order.)
- **OQ-3 (drives FR-D-005 scope)** — Is debug==release SIM-truth currently true? If not, that is a
  pre-existing latent bug surfaced by this spec; decide whether the fix lands here (violating NG-1's
  "no behavior change") or as an explicit follow-up spec.
- **OQ-4 (FR-F-002 enforcement granularity)** — How does the gate detect "touches a hash-feeding
  path"? Options: a manual checklist artifact required in the PR/contract, a static check that new
  code under the hash-assembly call graph references the residency contract, or a heuristic on
  changed files. Manual is reliable-but-skippable; static is strict-but-fiddly.
- **OQ-5 (FR-E-002)** — What quantization granularity makes a delayed GPU readback safe as a sim
  input without aliasing away the signal it carries? (Per-feature; relevant only if any GPU→CPU
  value is ever promoted to sim — none today.)
- **OQ-6 (FR-F-004 fold boundary)** — Which new localization sub-hashes, if any, should eventually
  be *folded* into `world_hash` (re-pinning the baseline, which local-dev allows) vs. stay
  reporting-only? Default per NG-6 is reporting-only; folding is a deliberate, separately-sequenced
  re-pin.

## Key files

**Anchor corrections (verified 2026-06-26):** the critique's *line numbers were all accurate*; two
*directory paths* had drifted and are corrected below.
- `ServerWorldRunner.cpp` — critique cited `src/luminumbra_common/world/`; actual
  `src/luminumbra_server/`. Lines 369 / 473 / 510 / 579 all verified accurate.
- `SystemConfig.cpp` — critique cited `src/luminumbra_common/systems/`; actual
  `src/luminumbra_common/core/`. Line 217 verified (the `render.* never hashed` exclusion is the
  comment at `:223`).
- `docs/water-sim-lockstep-determinism.md` — lines 31 / 50 verified accurate.

Anchored files:
- `src/luminumbra_server/ServerWorldRunner.cpp` — boot-settle of initial residency (`:369-393`);
  moving-anchor harness (`:472-479`); per-tick streaming barrier (`:489`); `ComputeWorldHash` +
  composite fold (`:510-534`); `ComputeWorldSubHashes` + additive sub-hash slots (`:536-573`);
  single-quiesce combined capture (`:576-594`). The hash-assembly contract surfaces (FR-A-002,
  FR-B-001) plug in here.
- `src/luminumbra_server/main_server.cpp` — `--smoke`/`--smoke-moving`/`--artifact`/`--record`/
  `--replay` arg parsing (`:224`, `:266-268`, `:295-296`, `:230-235`); run==replay assertion +
  mesh-excluded `sub_hashes_match` (`:465-485`); artifact JSON with per-system sub-hashes
  (`:421-428`, `:498-510`). The two-worlds enforcement (FR-A-003) and the matrix output (FR-D-006)
  anchor here.
- `src/luminumbra_common/core/SystemConfig.cpp` — `ComputeConfigSubHash` render-excluded, sim-only-
  when-enabled (`:217-245`; `render.* never hashed` `:223`; empty-baseline `:243`). Config-residency
  parity (FR-A-004) anchors here.
- `docs/water-sim-lockstep-determinism.md` — the streaming-arrival desync root cause (`:31`, `:50`),
  the halting-oracle/lockstep-halt mechanics (`:38-46`), and the bed/sampler purity fix
  (`:50-61`). Source of the FR-B-004 / FR-C-001 / FR-C-003 patterns.
- `.forge/scripts/validate-engine-frontier.ps1` — the engine-frontier gate the audit checklist
  (FR-F-002) extends; also the preset/build-tree assumptions relevant to NFR-003.
- `src/luminumbra_client/rendering/RenderPipeline.cpp` (~`:4308`/`:4323`) and
  `.../passes/FoliagePass.cpp` (~`:784`) — the synchronous GPU readback paths classified
  `RenderResidency` by FR-E-001 (line refs from critique F4; verify at implementation time).
- Cross-referenced sibling specs (forward — to be created/extended):
  `docs/specs/015-atmospheric-lighting-colored-glass/spec.md` (guarded: exposure + froxel jitter
  render-only — its NFR-001/OQ-6); `docs/specs/016-render-framework` (guarded: frame-graph must not
  leak render state into sim); `docs/specs/017-engine-concurrency` (consumes the availability-set
  contract, FR-B-003); `docs/specs/019-networking-scale-out` (relies on lockstep determinism as its
  oracle — Group C halting-oracle classification, NG-5 single-PC).

## Verification (end-to-end)

1. Build both trees (prepend `C:\msys64\ucrt64\bin`): `cmake --build build` (`build/bin`) and
   `cmake --build --preset debug` (`build/debug`). Record which binary each result came from
   (NFR-003).
2. `luminumbra_server_app --smoke == 6f008a9f637c40b7`, run==replay; `--smoke-moving` run==replay —
   before and after every phase (AC-001/002; NFR-001).
3. Run the determinism matrix (Group D): cross-worker, throttled/unthrottled, two-process
   (compare `--artifact` JSON), record→replay-in-fresh-process, debug-vs-release. Fast subset per
   change; full matrix on cadence (NFR-005).
4. Run the residency/partition tests (Group A) and the readback-discipline test (Group E):
   `sub_hashes_match` == declared `SimResidency` set; no `RenderResidency` value folded; no GPU
   readback feeds a hash without delayed+quantized+hashed.
5. Confirm no determinism golden literal is re-pinned (AC-003); all additions additive (NFR-004).
6. Run the engine-frontier validator with the audit checklist wired; confirm it passes the tree and
   fails a synthetic checklist-incomplete feature (AC-F-002).
