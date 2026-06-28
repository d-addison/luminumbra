# 017-B Activation Queue — code-grounded design + prerequisite analysis

Status: DESIGN (2026-06-28). Grounded in the streaming code + the `--avail-trace` / 017-D
data landed this session. The naive "remove the barrier" approach WOULD break determinism;
this note scopes the real work so it doesn't.

## The problem (quantified — 017-D instrumentation)

`ServerWorldRunner::RunFixedTicks` blocks the main thread in `wait_for_streaming_jobs()`
(`ServerWorldRunner.cpp:489`) every tick. Measured (debug `--smoke` / `--smoke-moving`):

| anchor | p50 | p95 | p99 | total / 90 ticks |
|--------|-----|-----|-----|------------------|
| STATIC | 0.001ms | 0.002ms | 0.004ms | 0.1ms |
| MOVING | 28ms | 178ms | **239ms** | **3979ms (~50% of wall)** |

The static barrier is free (the resident set is constant post-boot — `--avail-trace` shows 1
distinct digest over 90 ticks). The cost is entirely in the MOVING/streaming case: ~50% of wall
is the main thread blocked, with a 239ms p99 hitch. That is the moving-lag and the only thing
017-B should target. (cf. memory moving-lag-streaming-amortization.)

## The determinism gate (landed — use it)

`--avail-trace` digests the per-tick availability set (sorted coords of `Ready` chunks, FNV-1a)
right after the barrier; `--avail-trace --artifact <p>` writes it. Properties (measured):
- STATIC: per-tick availability is run==replay DETERMINISTIC (gate the queue here per-tick).
- MOVING: per-tick residency CONVERGES (diverges run-to-run at ~tick 16 due to the budget +
  pipeline timing below) but the final `world_hash` matches (`cf501b66`). Gate the moving case on
  final-hash + the static per-tick trace, not per-tick moving identity.

Gate for any barrier change: STATIC `--avail-trace` MATCH unchanged + both finals
(`6f008a9f` debug-static / `cf501b66` debug-moving) run==replay, with the moving p99 wait dropping.

## Why the naive approach breaks the hash (the blocker)

The streaming pipeline is generate -> mesh -> publish -> Ready -> collision, and it is BUDGETED:
- `update(anchor)` enqueues out-of-range evictions + in-range candidates, then dispatches only
  `generation_budget` chunks/tick (`SHIELD_WorldSystem.cpp:2710-2738`); the rest defer to later
  ticks. This is already proactive amortization.
- A chunk reaches `ChunkState::Ready` ONLY in `process_completed_meshing_jobs()` (`:4289/4293`),
  i.e. after the MESHING wait. Collision is gated on `Ready` (`:2426`).
- **CRITICAL COUPLING:** for LOD0 chunks the meshing job ALSO publishes the SIM-TRUTH voxel field
  — the "LOD0-promotion backfill" at `:4262-4274` moves `pending_sdf_data` -> `sdf_data` and
  `pending_heightmap_data` -> `heightmap_data`. Those feed `world_hash` and (via the heightmap)
  collision. So meshing is on the HASH-CRITICAL path; you cannot skip or async-defer the per-tick
  meshing wait on the server without changing which sim-truth data is published per tick -> hash break.

The barrier's determinism comes from settling the SAME availability set per tick regardless of job
timing. Removing it and advancing with "whatever's ready" makes residency job-timing-dependent ->
nondeterministic sim reads -> hash break. The activation set must stay a deterministic, tick-keyed
function.

## The real design (prerequisite first)

1. **PREREQUISITE — decouple sim-truth publish from render meshing.** Generate + publish the
   sim-truth SDF/heightmap/material in the GENERATION job (or a dedicated publish step), so a chunk
   becomes "sim-resident" (heightmap present -> collision-eligible -> hashable) WITHOUT waiting for
   the render mesh. The render mesh stays a separate, render-only job (already excluded from
   world_hash). This removes the LOD0 backfill from `process_completed_meshing_jobs`. Hash-affecting
   -> re-pin both baselines; gate with `--avail-trace` (static per-tick must stay MATCH) + run==replay.

2. **Deterministic activation queue.** Define "available at tick T" as a pure function of
   (anchor history, tick) with a FIXED pipeline latency K: a chunk dispatched at tick D is activated
   (visible to the sim) at tick D+K. The queue activates exactly that set each tick, blocking ONLY if
   a scheduled chunk isn't ready yet (shouldn't happen if K covers the gen latency under budget).
   Replaces the per-tick `wait_for_streaming_jobs` (only sim-truth publish gates the sim; render mesh
   never blocks the tick). FR-B-005 hash-neutral: same availability set per tick as the barrier.

3. **EnsureSurfaceReadyNear (the interactive LOAD hang)** — the boot-time unbounded waits
   (`:2825/2827/2925`) are a separate target; bound them via the same activation model + the landed
   stopgap (watchdog + SDF guard). Not exercised by `--smoke` (needs interactive repro).

## Validation plan
- `--avail-trace --artifact baseline_static.json` (debug) BEFORE step 1; after each step the STATIC
  per-tick trace must be identical and `--smoke` == `6f008a9f` (re-pin only on an intended bump).
- `--smoke-moving` run==replay (final hash) + the 017-D p99 wait must drop materially from 239ms.
- `validate-determinism-matrix.ps1` (workers {1,2} + multiprocess) green.

Routing: opus inline (determinism-sensitive). This is a multi-session effort; do step 1
(the decoupling) as its own gated change before the queue.

## Step 1 decoupling — the PRECISE mechanism (code-grounded, 2026-06-28)

Traced the exact LOD0 sim-truth coupling in the STREAMING path:
- `GenerateChunkData(chunk, target_step)` (`SHIELD_WorldSystem.cpp:3794`): for `target_step > 1`
  (coarse/far) it generates the HEIGHTMAP ONLY — no SDF (`:3812-3851`); for `target_step <= 1` it
  generates the full 17^3 SDF.
- So in streaming, a far chunk is generated coarse (heightmap only). When the anchor approaches and
  it's promoted to LOD0, the MESHING job (`:4142`) sees `step <= 1 && chunk->sdf_data.empty()` and
  calls `GenerateChunkData(scratch, 1)` (`:4154-4162`) to backfill the full SDF, staging it into
  `pending_sdf_data` (`:4190-4198`), published on the main thread by
  `process_completed_meshing_jobs` (`:4262-4274`). THIS is why meshing is hash-critical.
- The LOAD path (`EnsureSurfaceReadyNear` build_jobs, `:2892`) already generates the full SDF in the
  GENERATION job for LOD0 (`GenerateChunkData(*chunk, build_chunk.step)`), so it does NOT backfill —
  the coupling is STREAMING-only.

**THREAD-SAFETY CONSTRAINT (the crux — do not break it):** the backfill deliberately builds into a
`scratch` Chunk and stages to `pending_*`, because the live chunk's `sdf_data` "is never touched
off-thread" (`:4156-4160`) — a concurrent far-LOD/sampler reader must never observe a half-written
live `sdf_data`. So the decoupling CANNOT just write `sdf_data` eagerly on a worker.

**The cut (step 1):** when the streaming path PROMOTES a chunk to LOD0 (the dispatch site that today
queues a meshing job for a `sdf_data.empty()` chunk — `dispatch_meshing_jobs` callers around
`:2323/2385`), instead first dispatch a GENERATION job that builds the full SDF into a staging buffer,
PUBLISH it to the live `sdf_data` on the MAIN thread (the existing pending→live publish discipline),
THEN dispatch the meshing job (which now takes the `else` branch `:4163-4166`, reads the populated
`sdf_data`, and produces the render mesh ONLY — no sim-truth publish). Net: sim-truth (sdf/heightmap →
hash + collision) is available after GENERATION; the render mesh no longer gates the sim tick, so the
per-tick `wait_for_streaming_jobs` meshing wait can be removed from the sim's critical path.

**Gate (mandatory, every increment):** DEBUG `--smoke == 6f008a9f637c40b7` byte-identical (the SDF
VALUES are unchanged — only WHEN they're generated moves; the static set fully settles by tick 90, so
the final hash must hold) + the STATIC `--avail-trace` per-tick trace identical + `--smoke-moving`
converges to `cf501b66`. If the static hash moves, the cut changed sim-truth ordering/values — revert
and find why. Risk: the generation budget (`:2710`) now does more work per LOD0 chunk, which can shift
the moving per-tick set (already convergent-not-identical) — acceptable iff the final moving hash holds.
