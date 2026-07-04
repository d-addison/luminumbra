# 017-B Activation Queue — code-grounded design + prerequisite analysis

Status: DESIGN (2026-06-28); **STEP 1 (SHIELD-02) LANDED 2026-07-03** — see "Step 1
LANDED" at the bottom. Grounded in the streaming code + the `--avail-trace` / 017-D
data landed this session. The naive "remove the barrier" approach WOULD break determinism;
this note scopes the real work so it doesn't. (Line drift note: the barrier call site is
now `ServerWorldRunner.cpp:501`, not `:489`.)

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
   never blocks the tick). FR-B-005: the STATIC per-tick set + static literal must hold
   byte-identical; the MOVING schedule legitimately moves from settle-tick to D+K (a reviewed
   world_hash bump — the original "same availability set per tick as the barrier" wording here was
   over-tight for the moving case, whose barrier-era per-tick sets were run-varying anyway;
   corrected at landing, see Step 2 LANDED).

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

## Step 1 LANDED (SHIELD-02, 2026-07-03 — commits 35a7cf71 + 30b2ff40)

**Outcome: ZERO hash movement.** Both baselines held byte-identical (static
`6f008a9f637c40b7` with the 90-tick per-tick trace IDENTICAL cross-build against the
pre-change baseline artifact; moving `cf501b6676d67249` run==replay, converging at tick 16
exactly as before). The "hash-affecting -> re-pin both baselines" prediction above proved
avoidable — no re-pin was needed. Determinism matrix (workers {1,2} + multiprocess) green.

**Load-bearing correction to this doc's implicit tick topology:** `wait_for_meshing_jobs()`
itself calls `process_completed_meshing_jobs()` (`SHIELD_WorldSystem.cpp:365`), so under the
per-tick barrier a promotion dispatched in tick T's Step-3 pass published sim truth + mesh +
`Ready` at the END OF TICK T, inside the barrier, before the avail-trace digest. The variant
sketched above ("publish, THEN dispatch the meshing job" across the update/tick boundary)
would have moved mesh/Ready publication one tick later, shifted the collision tick, and
changed tick-90 in-flight state (`state`/`has_collision` are hashed) — deterministically
breaking `cf501b66` under a moving anchor. **What landed instead preserves same-tick
settlement BY CONSTRUCTION:** a two-stage promotion pipeline sequenced INSIDE the barrier —
`wait_for_streaming_jobs` = gen drain -> mesh drain (publish) -> promotion drain (publish
staged sim truth on the main thread + dispatch the render-mesh stage B) -> mesh drain
(publish stage B). Every per-tick observation point sees exactly the settled state the old
fused pipeline produced, in both static and moving runs.

What landed (see the commits for full detail):
- `dispatch_promotion_jobs` / `process_completed_promotion_jobs` / `wait_for_promotion_jobs`
  — stage A generates the full voxel field into the existing `pending_sdf/heightmap/
  material_data` staging (never touching live `sdf_data` off-thread, the crux above);
  the main thread publishes it (the old backfill publish verbatim, dirty flag clear) and
  only then dispatches stage B down the ordinary meshing lane.
- The meshing lane is render-only: backfill deleted from the worker lambda and from
  `process_completed_meshing_jobs`; an in-job tripwire fails any unit-step mesh that
  reaches the lane without full sim truth; promotion classification happens at DISPATCH
  time on the main thread (value identical — `sdf_data` is main-thread-owned between
  dispatch and job start; covers the SHIELD-04 malformed case via one size test).
- Scheduler gating parity: dispatch gate, quiescence test, and radius-pressure OR in
  `promotion_pipeline_pending()` (byte-neutral on the per-tick-quiesced server paths;
  correct one-batch-in-flight backpressure on the client). `Ready`/`has_collision`
  semantics did NOT change — that redefinition is step 2's contract (SHIELD-03).
- Proving pin: `test/common/PromotionSimTruthDecoupling_test.cpp` — RED before the cut
  (sim truth only went live inside the render-mesh publish), GREEN after (sim truth live
  via `wait_for_promotion_jobs` while `current_lod` is still coarse; byte-equal to pure
  generation output; unchanged across the mesh publish). Static runs dispatch ZERO
  promotions (constant resident set post-boot — by design); the moving run's byte-identical
  hash + the gtest are the lane's exercise evidence.

**Step 2 (SHIELD-03) consequence:** meshing is now OFF the hash-critical path — the
per-tick barrier's meshing wait gates only render state. The queue work can proceed per
the step-2 section, with the additional scheduler-de-timing scope documented in the
Wave-B plan (the dispatch path still reads live job-activity state at
`streaming_radius_for_pressure` / budget zeroing / dispatch refusal / quiescence, and the
water `current_lod==0` init gate at `WaterSystem.cpp:381` + collision eligibility mesh
check at `:2434-2436` still key sim behavior off render artifacts).

## Step 2 LANDED (SHIELD-03, 2026-07-03 — the barrier swap)

Landed as SIX gated increments (each byte-identical on both baselines until the swap):
activation-latency shadow (K evidence: gen→Ready exactly 1 tick under the barrier,
promotion→publish 0 ticks; wall p99 ~240 ms ≈ 7.2 ticks ⇒ K=8), scheduler de-timing
(every dispatch-path decision reads PUBLICATION-KEYED main-thread state), sim-consumer
re-key (collision eligibility → the centralized `sim_available_lod0`; water needed
NOTHING — its init selection is dispatch-keyed map membership and the :381 lod gate is a
bit-identical fast-path selector, refuting this doc's earlier worry), non-publishing
save quiesce (a save is never an activation event), main-thread generation publication
(gen jobs stage `pending_generation_ready`; the LAST off-thread lifecycle flip removed),
per-lane batch FIFOs + `activate_due(tick)` (due = dispatch + K; FIFO publication order;
per-batch blocking on due-but-unfinished, proven by the ActivationQueueSemantics gtest
BEFORE the swap), then the swap itself: the `ServerWorldRunner` tick path calls
`activate_due(tick)`; explicit full drains remain at boot/hash/mutate/teardown; per-frame
client hooks publish only drained-AND-due batches (due -1 = client = when-drained);
dispatch backpressure = deterministic FIFO depth budgets (generation 3, meshing K+1 —
never binding in steady state).

**Measured at the swap (90-tick runs):**
- STATIC: debug `6f008a9f637c40b7` / release `ea9a0121d13bc3bd` BYTE-IDENTICAL + the
  debug per-tick trace IDENTICAL to the pre-Wave-B baseline artifact — the strong
  oracle held through the entire wave.
- MOVING: run==replay at the reviewed new literals — debug `0431682a3f8a8a24`,
  release `d79fdbbdbfe6580f` — and the per-tick moving trace is now **run==replay
  MATCH** (barrier era: diverged at tick 16, only converged). Better: the moving hash
  is now **WORKER-COUNT-INVARIANT** (identical at workers {1,2,4} in both builds, an
  18-cell matrix PASS) — a determinism property the engine never had (the barrier-era
  moving hash was only convergent per-run). Availability is a pure function of
  (dispatch schedule, tick): FR-B-005 measured, not asserted.
- **Main-thread streaming wait: p50/p95/p99/max = 0.001 ms, total ≤0.1 ms over 90
  moving ticks — down from p50 28 ms / p99 239–262 ms / total ~4.2 s (~50% of wall).**
  The moving-lag main-thread block is eliminated.
- Hard-won rule (found by the release-moving matrix cells): a tick-stamped batch
  publishes EXCLUSIVELY via activate_due / the explicit force drains. A per-frame hook
  publishing a merely drained-and-due batch lands the state flips BEFORE that tick's
  candidate pass while activate_due lands them after — whether workers happen to be
  drained at update-start is wall-clock, and debug only passed by timing luck. The
  non-force hooks therefore touch ONLY due_tick<0 (client) batches.
- Two stale gate pins found + re-synced while assembling the evidence bundle:
  ReplayRoundtrip / LockstepLoopback / LockstepFaultInjection pinned the 2026-06-22-era
  static canonical `ab0869af701f1816`; re-synced to `6f008a9f637c40b7` (their own
  comments state they must equal the HeadlessServerTick canonical).

Evidence bundle: determinism matrix (workers {1,2,4} × static/moving × multiprocess ×
debug/release), LREC1 ReplayRoundtrip/ReplayDivergence, LockstepLoopback, heavy-oracle
terrain+entities legs (the water leg is WATER-17 — the pre-existing settle-idempotence
defect, filed with evidence), MovingResidency gate, ActivationQueueSemantics +
PromotionSimTruthDecoupling + the meshing/streaming/water/persistence suites.
Step 3 (the boot path — SHIELD-01) is next.
