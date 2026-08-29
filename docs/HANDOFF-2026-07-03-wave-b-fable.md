# HANDOFF — Spec 021 orchestration, Wave B and beyond (2026-07-03)

> For the next **Fable orchestrator session**. You inherit the spec-021 mandate
> (`docs/specs/021-engine-framework-audit-charter/spec.md`, Group F): drive the ranked
> backlog through spec→plan→execute→verify autonomously, commit (never push) per green
> gate, pause only at wave boundaries and true external blockers. The owner has standing
> autonomous authority granted; do not ask permission for reversible in-scope work.

## 1. Single source of truth

- **`docs/audit/021/`** is the canonical engine state: 12 verified pillar critiques,
  `backlog.json` (**188 items, 65 done**, every item rank-stamped with a TDD
  proving-signal — `validate_backlog.py` enforces both), `priority-ranking.md` (the ranked
  order + wave execution records), `gpu-modernization-plan.md`, `orchestration-plan.md`
  (waves, routing table, serialization rule, cadence).
- After every wave: flip statuses in `backlog.json`, re-run `validate_backlog.py`, append
  the wave record to `priority-ranking.md`, commit.
- The tree is on `feat/polyglot-audit-roadmap`. main/origin-HEAD is a STALE predecessor
  repo — never base worktrees on it without a base-check.

## 2. State at handoff (all verified this session)

**Determinism law:** `build/debug/bin/luminumbra_server_app --smoke` ==
`6f008a9f637c40b7`, run==replay (DEBUG; release is `ea9a0121d13bc3bd`). Held after every
commit below.

**Landed 2026-07-02/03** (audit commit `7d459b4b` → this handoff):

- The full spec-021 audit package + validator (see §1).
- **Wave A complete** (ranks 34–48): RENDER-01 capture stall fixed (+ the
  `HeadlessInGameCapture` gate — first-ever green headless night capture), FOLIAGE-01
  world defoliation fixed (inverted SDF density convention; FoliageInstancing GREEN with a
  re-blessed saturation contract + 100k decimation floor), FOLIAGE-11 refusal diagnostics,
  WATER-06, FOLIAGE-08, AETHER-09 (EngineGameSplitLint green), RENDER-10 (`.ipp` gate
  hole), OPS-10 (25 tracked artifacts untracked — `git status build/` clean), OPS-11
  (JobWatchdog on all three EnsureSurfaceReadyNear waits), SHIELD-04/05 (SDF OOB guards +
  save-adoption quarantine), AUDIO-04 (bank-integrity ctest), FOLIAGE-06, UI-07
  (`--ui-fixtures` + hermetic gallery), UI-12 (gate honesty vs `ctest --show-only`
  reality), OPS-12 (derived roster), OPS-04 (build-tree preflight wired + provenance on
  RenderBudget/WorldVisualSweep).
- **Pre-B rank-50 items**: SHIELD-17 (stale mesh-hash pins root-caused by two-point check
  to `d53c99c5` analytic-MC-normals — deliberate render-only; re-pinned WITH the evidence
  chain in the test comment), WATER-16 (test pinned the pre-spec-009 dry==SEA_LEVEL
  convention; refreshed to assert depth==0 + surface≤bed), RENDER-19 (spec-013
  world-entry scans backgrounded: 6m25s frame-2 stall → 33 s of background jobs,
  byte-identical crystal placement, teardown drains at all three world-transition sites).

**Full default ctest lane:** green except `ForestPerfBudget` (intentionally-RED,
manual-labeled — flipping it green is FOLIAGE-05, rank 133).

## 3. Your next work: Wave B — the 017-B concurrency keystone

Ranks 50–56 in `priority-ranking.md`. **HIGH hash risk. Strictly serialized — one
increment at a time, never fanned out** (the parallel-pass-divergence hazard is real and
memory-documented). Run inline on Fable/opus; do not delegate determinism-sensitive code.

Order (all prerequisites verified LANDED — 018-B contract, avail-trace harness, 017-D
instrumentation, the activation-queue design doc):

1. **SHIELD-02** — 017-B step 1: decouple sim-truth publish from render meshing (remove
   the LOD0-promotion backfill from `process_completed_meshing_jobs`; the design is in
   `docs/specs/017-engine-concurrency-async-execution/activation-queue-design.md`).
   Gate: `--smoke` byte-identical + static `--avail-trace` per-tick MATCH
   (`main_server.cpp` records `availability_trace_match` — report-only field; VERIFY it
   reads true) + `--smoke-moving` run==replay + `validate-determinism-matrix.ps1`.
2. **SHIELD-03** — the deterministic activation queue (fixed pipeline-latency-K,
   tick-keyed availability) replacing the per-tick `wait_for_streaming_jobs` barrier.
   Target: the measured moving-anchor p99 239 ms main-thread block (017-D percentiles are
   in the MovingResidency artifact). Same gates per increment.
3. **SHIELD-01** — the world-load hang ROOT FIX: bound/replace the three
   `EnsureSurfaceReadyNear` waits (now watchdog-instrumented, `core/JobWatchdog.h`) with
   the activation model on the boot path. Signal: NEW Test-WorldLoadBounded (20×
   interactive-style loads < 60 s each, zero watchdog wedge reports).
4. **SHIELD-07 + OPS-13** (joint) — the FR-D-002 fast/slow-job determinism-matrix
   throttle axis (adversarial job timing proof).
5. **RENDER-06** — 016 FR-E: retire the GPU-SDF sync readback onto the AsyncReadbackRing,
   empty the FR-G-001 allowlist (now 2 entries: RenderPipeline.cpp + SkyAtmosphereLut.ipp
   — retire both).
6. **SHIELD-06** — enforce 018-B in production (derive hash-exclusion scope from
   `ResidencyContract.h`).
7. **SHIELD-09** — preview reinit-vs-far-LOD quiesce (replaces the point-guard).

Wave gate: static avail-trace MATCH + determinism matrix + `--smoke-moving` 0-flake + p99
reduction recorded; `--smoke` byte-identical unless ONE deliberate re-blessed bump. Then
pause for owner review; **Wave C** (014 pilot-gate closure → RHI pilot, ranks 57–66) is
next — registry resource ownership (RENDER-12+GPU-12) is the ONE unfinished gate leg.

Also queued near-term: **RENDER-20** (rank 76, new) — the ~30 s wildlife/procgen-tree
bring-up frame (pre-existing; visible again now that trees actually build). And the
carried minor: the foliage `windy_max_sway` measure reports 8.46 m for ≤0.44 m blades —
audit its scale when next in FoliagePass.

## 4. Operating rules (non-negotiable, all owner-standing or charter-mandated)

- **Toolchain:** prepend `C:\msys64\ucrt64\bin` to PATH on EVERY build/ctest/validator
  call. Build the tree you test: `cmake --build --preset debug`; gates read `build/debug`.
  `-Mode Build` now runs the two-tree preflight (root `build/` still exists → WARN until
  OPS-05).
- **TDD:** lock the proving signal (failing test/gate) BEFORE production code. Every
  backlog item's signal is in `backlog.json`.
- **Cadence:** commit per green gate (never push); ≤1 deliberate re-blessed `world_hash`
  bump per change (heavy oracle + LREC1 + lockstep evidence); batch PopulatedWorldReplay
  re-pins.
- **Routing:** Fable/opus inline for shaders + determinism-sensitive code + the
  queue/ring/RHI seam; Codex gpt-5.5 (`model_reasoning_effort="high"`, NEVER xhigh — it
  wedges; deliverable as final message, read-only sandbox for critique) for spec→plan +
  mechanical whole-file C++ + sign-off when stuck (do NOT block on the owner mid-wave);
  Sonnet fan-out only for file-disjoint UI/doc work against committed interfaces.
  `git status` after every generation workflow.
- **Visual changes require a LOOKED-AT capture** (the density-convention blackout lesson);
  broken capture path = merge freeze for visual work. Captures now work:
  `--frame-scan out.json --no-audio` (noon) and `--scene-config` night both green via
  `-Mode HeadlessInGameCapture`.

## 5. Session gotchas (will bite you)

- **PowerShell commit messages:** embedded double quotes inside `git commit -m @'...'@`
  here-strings BREAK native argument passing in PS 5.1 (pathspec errors) — write commit
  bodies without `"` characters.
- **PowerShell `>` redirection writes UTF-16** — never materialize a source file via
  `git show ... > file` (it will fail to compile with bizarre errors); use
  `git checkout <rev> -- <file>` for A/B probes, restore with `git checkout HEAD -- <file>`.
- **SDF density convention:** NEGATIVE = solid, ≥0 = air (`final_density = (y - height)` +
  carve; the mesher's solid corner is `val < isolevel 0`). Check the MESHER, not
  neighboring comments — documented at `DebugCamera.cpp` helpers and both roof probes.
- **Mesh bytes are world_hash-EXCLUDED** — `--smoke` is structurally blind to mesh drift;
  only `MeshingDeterminism.*` catches it, and only if the full ctest lane RUNS (OPS-09,
  rank 132, would automate this — the lane went silently red for 6 days).
- **Repo-root `**` globs time out** (the giant `build/` tree) — scope Glob/Grep to
  `src/`, `docs/`, `test/`, `res/`, `data/`, `.forge/scripts/`.
- The two `--ui-screenshot`/`--frame-scan`-adjacent parity flags (`--render-parity-*`)
  ride the same IN_GAME boot RENDER-01 fixed — they work now; use them for 016 conversion
  evidence.
- `Invoke-Checked` in the validator has a default 120 s timeout — pass `-TimeoutSeconds`
  for world-loading clients (HeadlessInGameCapture uses 420 s).

## 6. Verification quick reference

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake --build --preset debug
build\debug\bin\luminumbra_server_app.exe --smoke            # 6f008a9f637c40b7 run==replay
build\debug\bin\luminumbra_server_app.exe --smoke --avail-trace  # per-tick availability trace
ctest --test-dir build/debug                                  # full lane (green except ForestPerfBudget)
powershell -File tools\gates\validate-engine-frontier.ps1 -Mode <gate>
#   gates this session added/uses: HeadlessInGameCapture, FoliageInstancing,
#   EngineGameSplitLint, UiTestBaseline, Build, RenderReadbackAllowlist, MovingResidency
powershell -File tools\gates\validate-determinism-matrix.ps1 -Quick
python docs\audit\021\validate_backlog.py                     # backlog schema + signals
```

## 7. Key files for Wave B

- `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp` — EnsureSurfaceReadyNear
  (:2822–3080 region), `dispatch_meshing_jobs`/`process_completed_meshing_jobs`
  (:4073+), the LOD0 backfill (the step-1 decoupling target).
- `src/luminumbra_server/ServerWorldRunner.cpp` — the per-tick
  `wait_for_streaming_jobs` barrier + availability-trace recording;
  `src/luminumbra_server/main_server.cpp` — `--avail-trace` artifact fields.
- `src/luminumbra_common/world/ResidencyContract.h` + `test/common/ResidencyContract_test.cpp`.
- `docs/specs/017-engine-concurrency-async-execution/activation-queue-design.md` — the
  code-grounded design incl. why naive barrier removal breaks the hash.
- `src/luminumbra_common/core/JobWatchdog.h` — the named-phase wait wrapper (reuse it).
- `docs/audit/021/pillar-shield.md` — the full evidence map for this wave.

Memory (`~/.claude/projects/D--Coding-luminumbra/memory/`) is current as of this handoff —
trust `docs/audit/021/` + this file over any older memory that conflicts.
