# T-I4-17-jobsystem-pod-pool progress

## 2026-06-14 - Milestone 0: worktree provisioning + design

### Worktree base mismatch (IMPORTANT for orchestrator)
This isolated worktree was checked out on a STALE branch
`worktree-agent-a0bce35935ca33dc9` (HEAD `972c133`) whose JobSystem is only a
two-line stub in namespace `Luminumbra::core` -- it predates ALL the engine
work the contract describes (priority lanes, completion counters, the
lost-wakeup discipline). The real, contract-matching JobSystem lives on
`feat/polyglot-audit-roadmap` (the env-declared current branch).

Action taken: `git reset --hard feat/polyglot-audit-roadmap` in the worktree
(HEAD -> `3681e41`). Working tree was clean beforehand, nothing lost.

Vendor provisioning: the polyglot branch expects vendored third-party dirs that
are present in the MAIN checkout's working tree but are untracked there (not
submodules, except googletest which was an uninitialized submodule). They were
absent in the worktree. Copied the missing/partial dirs from
`D:\Coding\luminumbra\vendor`: EnTT, glad, glfw, glm, Jolt, lua, miniaudio,
nlohmann, rmlui, soil2, meshoptimizer, stb, googletest (content), and filled
sol2/spdlog/fastnoise (committed empty in the worktree). These are untracked in
the worktree -- do NOT commit them.

### Design (research ladder, boring wins only)
Public API is unchanged: `Job = std::function<void()>`,
`dispatch(Job, JobPriority)`, `dispatch_batch(const std::vector<Job>&,
JobPriority) -> JobHandle`, `wait`, `get_runtime_stats`. The `std::vector<Job>`
signature is load-bearing across many call sites (SHIELD_WorldSystem,
FarLodSystem, WaterSystem, EngineContracts, tests) so it stays.

1. POOLED POD JOBS. The old hot path paid TWO allocations per dispatched job:
   (a) `dispatch_batch` wrapped every job in a SECOND `std::function` to attach
   a per-job `CompletionGuard` -- its `{job, completion}` capture (~32 B) blew
   past libstdc++'s 16 B SBO, forcing a heap block; (b) the `std::queue<Job>`
   (std::deque-backed) churned deque nodes. Replaced both:
   - `struct PooledJob { Job job; shared_ptr<JobCompletionState> completion; }`
     stores the caller's Job + completion DIRECTLY (no wrapper std::function).
   - `class PooledQueue`: a contiguous ring of PooledJob slots, pre-sized to
     256/lane, growing by doubling (never shrinks). push() into an already-grown
     lane and pop() are allocation-free; slots are reused. Serialized by the
     existing `m_queue_mutex` -- NOT lock-free.
   - The completion guard moved into `JobSystem::run_slot`, run by the worker
     outside the queue lock; a local `Finisher` RAII object calls `complete_job`
     on every exit path (normal or throwing job).
   Remaining allocation is the caller-side `std::function` the public API still
   accepts (out of scope -- changing it to a template would break the stable
   API). Net: dispatch_batch goes from 2 heap allocs/job to 0 in steady state.

2. CACHE-ALIGNED COUNTERS. `JobCompletionState::counter` is now
   `alignas(hardware_destructive_interference_size)` (fallback 64) and the
   `mutex` that follows is aligned onto a separate line, so the batch-wide
   atomic decrement (contended by every finishing worker) no longer false-shares
   with the `mutex`/`condition` the single waiter polls under the lock.

3. STOPPED THERE. No lock-free queue, no work-stealing, no fibers (per
   contract). See FOLLOW-UP at the bottom.

### Correctness argument
- Lost-wakeup discipline UNCHANGED: `complete_job` still does the final
  decrement and notifies with `completion->mutex` held; the reject path still
  stores 0 + notifies under the lock; `wait()` still evaluates `counter <= 0`
  under the same mutex. Only the counter's MEMORY PLACEMENT changed (alignas),
  not its atomic ops or the CV idiom.
- Barrier still drains: every PooledJob in a batch shares the same `completion`;
  `run_slot`'s `Finisher` decrements once per slot on all paths (including a
  throwing job, since the destructor runs during unwind before the worker's
  try/catch swallows it). Counter reaches 0 exactly when all N slots have run,
  identical to the old CompletionGuard semantics.
- Determinism: jobs are still served in the same lane order (FIFO per lane via
  the ring's head/tail; High preferred with the unchanged starvation guard).
  Execution order is identical to before, so sim results / world_hash unaffected.
- No wall-clock / unordered iteration / non-seeded RNG introduced.

### FOLLOW-UP recommendation (NOT implemented, out of scope)
A lock-free MPMC queue would remove the m_queue_mutex contention on the dispatch
hot path, but the contract explicitly defers it ("after the boring wins"). If
profiling later shows the queue mutex is the bottleneck (it is held only for the
O(1) push/pop, so unlikely vs the job bodies), revisit then.

### Files touched
- src/luminumbra_common/core/JobSystem.h
- src/luminumbra_common/core/JobSystem.cpp
- test/common/JobSystem_test.cpp (pending milestone 1)

### Next
Build clean (-Werror), expand tests, run ctest + streaming/determinism gates,
measure perf delta.

## 2026-06-14 - Milestone 1: build + JobSystem tests + perf

### Build
`cmake --build build/debug --target common_tests` builds clean under -Werror
(only the pre-existing PhysicsSystem `-Wunused-parameter` note, which is
`-Wno-error=unused-parameter`). JobSystem.h/.cpp + JobSystem_test.cpp all
compile with no new warnings. Note: builds must run at reduced parallelism
(`-j 2`/`-j 3`); the worktree OOMs at full `-j` on the heavy TUs
(TerrainPresetLoader, MarchingCubes) -- a machine/worktree constraint, not a
code issue.

### JobSystem tests (12, all PASS)
Existing 6 (stress x3, priority x3) still green. Added 5 pool tests + 1 disabled
benchmark:
- RingGrowsAndRecyclesAcrossManyCycles: 200 cycles x 1000 jobs (forces ring
  growth past the 256/lane initial cap and full recycle each cycle); asserts
  exact run counts.
- NestedDispatchCompletesFully: outer jobs dispatch inner batches (collect
  handles, main thread drains). NOTE: a worker that BLOCKS in wait() on inner
  work it must itself service deadlocks a fixed pool -- a pre-existing hazard,
  unrelated to pooling; the engine never blocks inside a job, and the test uses
  nested *dispatch* not nested blocking-wait.
- EmptyBatchReturnsNullHandleAndWaitIsNoop
- ThrowingJobsStillDrainTheBarrier: 128 jobs, 1/3 throw; wait() must not hang
  (Finisher decrements during unwind) and the pool stays healthy afterward.
- DispatchAfterShutdownRejectsWithoutHang: reject path leaves a waitable
  (counter=0) handle; single dispatch after shutdown is a no-op.

Command:
`build/debug/bin/common_tests.exe --gtest_filter=JobSystem* --gtest_also_run_disabled_tests`
-> 12 tests PASSED.

### PERF (debug build, report only -- NOT re-blessed)
Method: DISABLED_DispatchThroughputBenchmark dispatches 100 batches x 1000
empty jobs, dispatch_batch + wait per batch, times the whole loop.
- BEFORE (git HEAD JobSystem, std::queue<Job> + wrapper std::function/job):
  487.4 / 471.4 / 499.5 ns/job (~2.0-2.1 M jobs/s) across 3 runs.
- AFTER (pooled PooledQueue, no wrapper std::function): 468.5 / 493.4 ns/job
  (~2.0-2.1 M jobs/s).
- DELTA (wall-clock): within run-to-run noise -- this debug micro-benchmark is
  SCHEDULING-BOUND (condition_variable wakeup latency of the dispatch+wait
  round-trip dominates), so the allocation savings do not move debug wall-clock.

Allocation win (the actual point of the change), argued from the code since a
process-global operator-new probe destabilized the test binary and was removed:
- BEFORE, per dispatched job: dispatch_batch wrapped each job in a SECOND
  std::function whose {job, completion} capture (~32 B) exceeds libstdc++'s 16 B
  SBO -> 1 guaranteed heap block/job, PLUS std::deque node growth in the queue.
- AFTER, per dispatched job: 0 heap allocations in the system's hot path once
  the ring is warm (slot reused in place); only 1 JobCompletionState block per
  BATCH (unchanged from before). So dispatch_batch goes from
  ~1 alloc/job + deque nodes  ->  ~0 alloc/job. For SHIELD streaming batches
  (hundreds of chunk-gen / meshing jobs per dispatch) this removes hundreds of
  per-frame heap allocations. A Release build would surface this as wall-clock;
  the orchestrator's release re-bless will capture it.

## 2026-06-14 - Milestone 2: full verification (ALL GREEN)

Full debug build (`cmake --build build/debug`) completes exit 0 under -Werror
(at -j2/-j3 to avoid worktree OOM; full -j OOMs on heavy TUs). All 91 targets
including luminumbra_server_app, luminumbra_client_app, and every test exe.

- ctest (`ctest --test-dir build/debug -E "_NOT_BUILT$" -LE manual`):
  100% tests passed, 0 failed out of 200. (Contract baseline said 195; the
  suite has grown to 200/201 since.) Includes the 12 JobSystem tests.
- HeadlessServerTick: PASSED. world_hash = 2fa007951a21e140 (the canonical
  value, UNCHANGED) == world_hash_replay, 90 ticks x 2 runs, 4498 chunks
  streamed/run. Sub-hashes [terrain=9e1b9316d5eeca32 mesh=812c3bb1c19b127a
  water=ed4265f8b090adf7 entities=5735a5094c1e92a8] all match run vs replay.
  -> the pooled-job change is determinism-neutral, as designed (same per-lane
  FIFO order + same lane-selection logic).
- SimDeterminismLint: PASSED. 39 sim-critical files scanned, 0 new violations.
  (JobSystem lives in core/, outside the sim roots, and introduces no banned
  constructs regardless.)
- LockstepLoopback (streaming-stability proxy; radius-4 streaming + lockstep,
  pins 2fa007951a21e140): PASSED. 90 ticks, 2 peers in sync,
  end_hash=2fa007951a21e140 (host==peer), max_horizon=3, late_inputs=0.

PERF baseline file (.forge/artifacts/engine-frontier/perf-baseline-release.json)
NOT touched; release lane NOT run -- orchestrator does the combined re-bless.

### Closeout note for orchestrator
This worktree was reset onto feat/polyglot-audit-roadmap (HEAD 3681e41 at reset)
because its original branch was a stale stub. Missing vendored third-party dirs
were copied in from the main checkout's working tree (untracked there too) to
make the build work. The only intended source deltas are:
  src/luminumbra_common/core/JobSystem.h
  src/luminumbra_common/core/JobSystem.cpp
  test/common/JobSystem_test.cpp
  .forge/artifacts/engine-iteration-4/t-i4-17-progress.md (this file)
