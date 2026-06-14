# T-I4-DR-server-streaming-race — Progress Log

Recovery file (prior agents lost final reports to session limits). Append a dated
entry after EACH milestone (repro confirmed, root cause, fix landed, verified).

Branch: feat/polyglot-audit-roadmap. Build: build/debug (ninja, MinGW ucrt64).
Canonical end_hash = 2fa007951a21e140 (90-tick run, must stay unchanged).
Repro command (default 16 workers, NO LUMINUMBRA_JOB_WORKERS override):
  build\debug\bin\luminumbra_server_app.exe --record build\debug\test-artifacts\replay\race-probe.lrec1 --seed 424242 --preset default --ticks 90

## 2026-06-14 — REPRO CONFIRMED + crash localized to frame 4 / early streaming

REPRO (clean build, default 16 workers, NO worker override):
  recorder crashed 6/10 (exit -1073741819 = 0xC0000005). Every completing run
  yields canonical end_hash=2fa007951a21e140. Matches documented ~40-50%.
  smoke (--smoke, RunFixedTicks(90) once): 9/10+ CLEAN. Confirms recorder-specific.

LOCALIZATION via stderr [TRACE] markers (since gdb serializes the race and is
too slow on the -O0 debug build to catch a backtrace in reasonable time):
- Crash is NOT in the checkpoint hash (checkpoints fire at ticks 30/60/90; the
  crash hits at tick 4, long before any checkpoint). This OVERTURNS the
  "mid-run checkpoint snapshot races streaming" hypothesis in the dispatch.
- Crash consistently lands INSIDE update_chunk_activation's candidate-build loop
  (SHIELD_WorldSystem.cpp ~1623-1668), the FIRST allocation-heavy pass (it runs
  at frame 4 because STREAMING_ACTIVATION_INTERVAL_FRAMES=4). The loop is single-
  threaded main-thread code (column_surface_span cache emplace, seen_candidate_ids
  insert, to_create.push_back) with NO concurrent writers at that instant ->
  therefore the access violation there is HEAP CORRUPTION that already happened
  in frames 1-3 and only surfaces at frame 4's next big allocation.
- Frames 1-3 each: water update (sync) -> dispatch_meshing (192/192/78 jobs) ->
  collision loop -> wait_for_streaming_jobs. The corruption happens during this.
- PROBE: added wait_for_streaming_jobs() right BEFORE the collision loop
  (removing the collision-vs-meshing overlap). Crash PERSISTED (8/16). So the
  collision loop is NOT the racing reader.
- Job BODIES (generation + meshing) were audited: they operate on a per-job
  `scratch` Chunk + the live chunk's `pending_*` fields/atomics only, and read
  world-gen via const/thread-safe FastNoise generators + read-only m_params.
  No mutable shared statics in MarchingCubes/GenerateChunkData. No chunk-map
  access inside PolygoniseTerrain/GenerateWaterMesh.
- The non-thread-safe m_column_surface_span_cache (unordered_map, written by the
  non-const column_surface_span) is MAIN-THREAD ONLY; workers call the const
  compute_column_surface_span (no cache). Ruled out.

OPEN: exact unsynchronized write that corrupts the heap in frames 1-3. Measuring
1-worker vs 16-worker crash rate on the clean build to decide main-vs-worker vs
worker-vs-worker. Continuing.

## 2026-06-14 — ACTUAL ROOT CAUSE: FastNoise2 GenPositionArray2D SIMD over-read (NOT a race)

The "race"/"hang" framing from the dispatch (and the JobSystem lost-wakeup below)
were RED HERRINGS. Decisive evidence:
- smoke (NOT just the recorder) ALSO crashes: smoke --ticks 4 = 6/20; smoke
  --ticks 90 just happened to crash less often (~10%), which made it look
  recorder-specific. recorder --ticks 4 = 10/20. Both crash in the early frames.
- Making streaming FULLY SYNCHRONOUS (drain gen+mesh right after dispatch in
  update()) did NOT fix it (10/20). So it is not a streaming/meshing data race.
- Caught the fault IN-PROCESS via a SetUnhandledExceptionFilter + DbgHelp
  StackWalk (gdb masks it; no post-mortem dumper installed). addr2line on the
  captured module offsets resolved the EXACT stack (MAIN thread):
    #0 _mm512_loadu_ps (avx512fintrin.h)
    #1 FastNoise::Generator::GenPositionArray2D  (vendor/.../Generator.inl:260)
    #2 SHIELD_WorldSystem::ComputeShapedHeightsAtPositions  (SHIELD_WorldSystem.cpp:840)
    #3 SHIELD_WorldSystem::compute_column_surface_span      (:1038)
    #4 SHIELD_WorldSystem::column_surface_span              (:1064)
    #5 SHIELD_WorldSystem::update_chunk_activation          (:1627)
    #6 update -> #7 RunFixedTicks -> RunSmokeOnce ...
  Fault code 0xc0000005, faulting addr ...000 (a PAGE BOUNDARY) -> a READ past
  the end of a heap buffer.

ROOT CAUSE (file:line): vendor/fastnoise/include/FastNoise/Generators/Generator.inl
GenPositionArray2D, lines 244-265. The SIMD tail does an UNCONDITIONAL full-width
load `FS_Load_f32(&xPosArray[index])` (line 260) -- e.g. _mm512_loadu_ps reads 16
floats -- and DoRemaining stores only the valid ones. When count < SIMD width
(AVX512 = 16 floats) the main loop never runs and the tail loads 16 floats from a
`count`-sized std::vector. compute_column_surface_span calls it with count = 5
(center + 4 corners). The 5-float (20-byte) input vectors (wx_in/base_x/peaks_x/
... in ComputeShapedHeightsAtPositions) over-read 11 floats past the end; when the
allocation happens to abut an unmapped page, the load faults -> 0xC0000005.

Why every earlier signal pointed elsewhere:
- INTERMITTENT: only faults when a small unpadded buffer's tail lands on a page
  boundary (heap-layout dependent).
- WORKER-COUNT SENSITIVE: more concurrent allocations -> different heap layouts ->
  different page-boundary-hit probability (NOT a threading bug).
- 1 worker still crashes (the over-read is single-threaded; column_surface_span
  runs on the main thread; ComputeShapedHeightGrid runs on workers).
- gdb "masks" it: gdb's allocator/guard pages change whether the tail hits an
  unmapped page.
- The instrumentation probes "moved" the crash: each probe perturbed heap layout.

THE FIX (surgical, hash-neutral): pad EVERY input/output buffer handed to
GenPositionArray2D up to the max SIMD width (16 floats) so the tail full-width
SIMD load/store always stays in mapped memory. Padding bytes are never read into
results (DoRemaining writes only [0,count)), so the computed heights/materials for
indices [0,count) are byte-identical -> canonical 2fa007951a21e140 unchanged.
GenUniformGrid2D/3D are NOT affected (they synthesize positions from indices --
no input array over-read -- and their grids are large; DoRemaining masks the
output tail). Sites: ComputeShapedHeightsAtPositions and ClassifyVertexMaterials
in SHIELD_WorldSystem.cpp.

## 2026-06-14 — FIX VERIFIED (SIMD padding)

Repro before fix (clean build, default 16 workers): recorder t90 ~60%
(12/20, 10/20 in two runs); smoke t4 6/20; smoke t90 ~10%; 1-worker 8/8.
After SIMD-padding fix (default 16 workers, NO env override):
- smoke  --ticks 4 : 0/20 crashes (was 6/20)
- record --ticks 90: 0/20 crashes, EVERY run end_hash=2fa007951a21e140 (canonical)
=> acceptance bar met. Hash unchanged -> fix is hash-neutral as predicted.

Files (the FIX):
- src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:
  - new constexpr kNoiseSimdWidth=16 + PadToNoiseSimd() helper.
  - ComputeShapedHeightGrid: SIMD-pad base_px/py, peaks_px/py, base_noise,
    peaks_noise (the generation-worker path; count=size_x*size_z rarely %16).
  - ComputeShapedHeightsAtPositions: SIMD-pad all GenPositionArray2D in/out
    arrays (the column-span footprint path; count=5 -- the actual crash site).
  - ClassifyVertexMaterials: same padding (render-side; latent over-read).
- src/luminumbra_common/core/JobSystem.cpp: notify-under-mutex hardening in
  complete_job + dispatch_batch reject path (correct CV idiom; NOT the root
  cause, kept as defensive hardening).

Mitigation reduction (validate-engine-frontier.ps1):
- Invoke-ServerWithCrashRetry: removed the LUMINUMBRA_JOB_WORKERS=1 pin and the
  8x 0xC0000005 retry loop; now a single default-worker invocation.
- Test-ReplayDivergence: removed its inline worker-pin + 8x retry; single replay.
- The LUMINUMBRA_JOB_WORKERS knob stays in ServerWorldRunner (harmless probe),
  no longer load-bearing.

Diagnostics removed: the per-tick/update [TRACE] fprintf probes and the
SetUnhandledExceptionFilter+DbgHelp crash dumper (and the temporary DbgHelp link)
were reverted; cstdio includes added for them removed.

## 2026-06-14 — FULL VERIFICATION GREEN

Clean full build (all targets): exit 0.
Gates (ALL at DEFAULT worker count, no LUMINUMBRA_JOB_WORKERS pin, no retry):
- HeadlessServerTick:       GREEN. world_hash=2fa007951a21e140 == replay;
  sub-hashes [terrain=9e1b9316d5eeca32 mesh=812c3bb1c19b127a
  water=ed4265f8b090adf7 entities=5735a5094c1e92a8] match. 4498 chunks/run.
- HeadlessServerTickHeavy:  GREEN. 60->save->load->+30 resim, world_hash=
  2fa007951a21e140 round-trips + resims identically.
- ReplayRoundtrip:          GREEN. recorded 90, replayed to end_hash=
  2fa007951a21e140 (canonical, recording hash-neutral), 3 checkpoints verified.
- ReplayDivergence:         GREEN. corrupted checkpoint CAUGHT at tick 30
  (section=terrain), replay refused exit 1; oracle non-vacuous.
- SimDeterminismLint:       GREEN. 39 files, 0 violations, 4 allowlist sites
  (the SIMD-pad fix introduced no wall-clock / unordered-iteration / libm hazard).
Full ctest: 189/189 PASSED, 0 failed (suite count is 189 on this tree; no test
added or removed by this task; no regressions).

No golden/baseline re-blessed. Changed (tracked) files exactly:
- src/luminumbra_common/systems/SHIELD_WorldSystem.cpp  (the fix)
- src/luminumbra_common/core/JobSystem.cpp              (CV notify hardening)
- .forge/scripts/validate-engine-frontier.ps1          (mitigation removal)
main_server.cpp / ServerWorldRunner.cpp have NO net diff from this task (all
diagnostics reverted); CMakeLists DbgHelp probe reverted. world_hash unchanged at
the canonical 2fa007951a21e140 throughout.

Extra acceptance: recorder --ticks 90 at default 16 workers, 25 consecutive
runs = 0/25 crashes, every run end_hash=2fa007951a21e140. (Pre-fix: ~50-60%.)

TASK COMPLETE.

## 2026-06-14 — (superseded) JobSystem lost-wakeup hardening

KEY DISCOVERY: at 1 worker the recorder does NOT "crash ~20%" (prior agents
mis-measured) -- it DEADLOCKS. Controlled 1-worker run (per-run 30s timeout to
distinguish hang vs crash): crash=2 hang=10 clean=0 of 12. The hang is the same
root cause as the 16-worker 0xC0000005.

GDB on a hung 1-worker process (hang does NOT race -> clean backtrace obtained):
- Main thread blocked in JobSystem::wait -> std::condition_variable::wait
  (JobSystem.cpp:164), called from SHIELD_WorldSystem::wait_for_meshing_jobs:167
  (the HIGH meshing-lane handle) <- RunFixedTicks <- RunRecord.
- `print handle.completion->counter` = 0. THE BATCH IS ALREADY COMPLETE, yet the
  waiter is still asleep in condition_variable::wait. => classic LOST WAKEUP.

ROOT CAUSE (file:line): src/luminumbra_common/core/JobSystem.cpp
- complete_job() (old lines 20-31): the LAST job did
    counter.fetch_sub(1, acq_rel);        // counter -> 0, OUTSIDE the mutex
    { lock_guard lock(completion->mutex); } // take+RELEASE empty
    completion->condition.notify_all();   // notify UNLOCKED
- JobSystem::wait() checks `counter.load() <= 0` UNDER completion->mutex, then
  enters condition_variable::wait (release-mutex + enqueue-on-CV, two steps).
- The counter is an atomic mutated OUTSIDE the mutex, so the empty-lock does NOT
  serialize the waiter's predicate check against the decrement. A notify emitted
  in the gap between the waiter releasing the mutex (inside cv.wait) and finishing
  its CV enqueue is LOST; since counter already hit 0, no further notify ever
  comes -> the waiter blocks forever (1-worker hang). With many workers the same
  unsynchronized window let wait() return while a worker was still finishing ->
  main read/mutated world state mid-flight -> heap corruption surfacing as the
  0xC0000005 at the next big allocation (frame 4 candidate loop), which is why
  every instrumentation probe "moved" and draining-before-collision didn't help:
  the corruption was upstream in the wait barrier itself, NOT in the chunk code.

THE FIX (hash-neutral; pure synchronization, no sim-path change):
- complete_job(): notify_all() with completion->mutex HELD (lock_guard spanning
  the notify), not an empty lock then unlocked notify. Holding the mutex across
  the notify closes the gap: the waiter is either pre-predicate (sees counter==0
  under the lock, never blocks) or already enqueued (gets the notify). Wakeup
  cannot be lost.
- dispatch_batch() shutdown-rejection path: same discipline (store 0 + notify
  under the mutex).
- WHY HASH-NEUTRAL: this only changes WHEN the completion signal is delivered, not
  WHAT any job computes nor the order of simulation. Draining correctly (instead
  of returning early from a half-drained batch) yields exactly the settled state
  the smoke already observed -> canonical 2fa007951a21e140 unchanged.

Files: src/luminumbra_common/core/JobSystem.cpp (complete_job + dispatch_batch
reject path). Tagged T-I4-DR-server-streaming-race.

Verification pending (rebuild + 20x recorder at 16 workers, 1-worker no-hang,
gates, ctest). Next entry.

## 2026-06-14 — Investigation start

Read both prior progress files (t-i4-11, t-i4-12). Confirmed the defect shape:
intermittent 0xC0000005 during the per-tick replay recorder run (not --smoke).

Code read so far:
- ServerWorldRunner::RunFixedTicks calls world_system->update() then
  world_system->wait_for_streaming_jobs() after EVERY frame. The checkpoint hash
  path (ComputeWorldHashAndSubHashes) ALSO calls wait_for_streaming_jobs() then
  snapshot_streamed_chunks(). So at the snapshot point jobs are nominally drained.
- SHIELD_WorldSystem job lambdas (generation + meshing) operate on individual
  Chunk objects (sdf_data / pending_mesh_* fields), NOT on the m_streaming_state.
  chunks unordered_map. The map is mutated only on the main thread (emplace at
  EnsureSurfaceReadyNear:1819 and dispatch_generation_jobs:2517; erase in
  update_chunk_activation:1764). So a concurrent map rehash-under-read is NOT the
  obvious culprit IF wait_for_streaming_jobs truly drains.
- PRIME SUSPECT: JobSystem::wait vs complete_job. wait() returns the instant the
  atomic counter hits 0 (set by the LAST job's CompletionGuard via fetch_sub).
  Investigating whether a worker can still be executing job BODY (or touching
  shared state) when the counter reaches 0 / whether wait can return before all
  workers are quiescent. See next entry.
