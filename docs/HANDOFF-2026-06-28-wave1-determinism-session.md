# Handoff — 2026-06-28 (session 2): Wave 0/1 execution + determinism findings

Branch `feat/polyglot-audit-roadmap`. Continues the merged engine-frontier roadmap
(Codex GPT-5.5 High audited, owner-approved; plan + memory `engine-frontier-merged-roadmap`).

## THE determinism fact (load-bearing — don't relearn)
`world_hash` is **BUILD-MODE-DEPENDENT**: **DEBUG `--smoke` == `6f008a9f637c40b7`** (the canonical
gate baseline — the gates run `build/debug`), **RELEASE == `ea9a0121d13bc3bd`** (distinct, also
run==replay deterministic). FP/optimization differences in the sim. A long detour this session
mistook the release hash for "drift" and wrongly re-pinned the spec literals; bisect + a debug
build settled it. **Always verify the DEBUG tree** against `6f008a9f`. The 11-commit history below
keeps both baselines run==replay.

## Landed this session (11 commits, all verified DEBUG `--smoke == 6f008a9f`)
- `4922583a` preview UAF fix — drain far-LOD (`prepare_world_swap`) before the world is freed in
  `swap_pending_into_live()` AND the dtor. The real create-screen-pan crash fix.
- `8532ab19` preview far-LOD OFF for the turntable — re-enabling it churned (bounded radius-4
  diorama + camera-anchored far-LOD re-streams empty tiles on orbit). Drain safety kept.
  Owner-tested: stable.
- `6fefa5fd` load-hang stopgap — malformed-SDF guard before PolygoniseTerrain + opt-in
  `LUMINUMBRA_JOB_WATCHDOG`. Hash-neutral.
- `727fbc8c` 018-B — ResidencyContract test (locks the availability-set contract, 4 tests green).
- `db02457d` + `f5225b5f` per-tick AVAILABILITY-SET TRACE gate (`--avail-trace`): digests the
  resident Ready-chunk coords after the streaming barrier each tick. STATIC = run==replay
  deterministic; MOVING = convergent (per-tick residency varies, final hash matches). Report-only.
- `77816ce7` 017-D main-thread wait instrumentation + DATA: STATIC barrier is free (p99 0.004ms);
  MOVING blocks ~50% of wall (p50 28ms / **p99 239ms** / total 3979ms) — the real 017-B target.
- `d3076491` 017-B activation-queue DESIGN note (`docs/specs/017.../activation-queue-design.md`).
- `38079c1f`→`9c56e488` the erroneous baseline re-pin + its revert (net: docs correct; build-mode
  lesson captured).
- `13f7ec5b` FR-D determinism-matrix per-build baseline fix (debug `6f008a9f` / release `ea9a0121`;
  it previously failed every release cell against the single debug literal). NOTE: run it with the
  call operator `& script.ps1 -Builds @('debug','release')`, NOT `powershell -File` (which mangles
  the `@(...)` array args).

## The 017-B blocker (why the activation queue is a dedicated sub-project)
Removing the per-tick `wait_for_streaming_jobs` barrier naively breaks the hash: for LOD0 chunks the
**meshing job publishes the sim-truth SDF/heightmap** (the LOD0-promotion backfill,
`process_completed_meshing_jobs:4262-4274`), which feed `world_hash` + collision. So meshing is on
the hash-critical path. **Prerequisite:** decouple sim-truth publish (generation) from render
meshing, THEN a deterministic tick-keyed activation queue. Gate every step on the static
`--avail-trace` MATCH + both finals run==replay, with the moving p99 dropping. See the design note.

## Remaining (prioritized)
1. **Preview far-field follow-up** (small): a distant diorama vista needs far-LOD anchored to the
   diorama CENTRE (not the orbiting camera) or a larger near radius; + near-field <256m hole audit.
2. **017-B** activation queue (the decoupling above, then the queue) — multi-session, gated.
3. **Wave 0 visual** — 0.2 Pillar A finish (moon radiance channel + photo EV + midnight re-bless,
   needs owner GPU re-bless); 0.3 create-world UI polish (tabs/value-beside-slider/weather particles);
   0.4 020-A preflight + 020-B config codegen (mechanical, headless-verifiable).
4. **Wave 2/3** — 016 FR-C/D/E, 015 C-1/B/C-2, 014 RHI pilot, 019-C1 soak.

## Constraints carried forward
- Prepend `C:\msys64\ucrt64\bin` to PATH; build the tree you test (`build/debug` for the gate).
- No `libtsan`/`libasan` in the toolchain → sanitizers infeasible; the `--avail-trace` + `--smoke`
  oracles are the race detectors.
- Kill stray `luminumbra_client_app` before relinking the client.
