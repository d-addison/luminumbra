# Spec 017: Engine Concurrency / Async Execution — Phase Ownership + Async Readback Rings

> Status: SPEC (created 2026-06-26). Derived from the engine-infrastructure devil's-advocate
> critique (`.forge/critique-engine-infrastructure-framework-20260626-200421.md`, finding F4).

## The framing insight (why this spec exists)

F4: **the concurrency model is budgeted and deterministic, but still fundamentally WAIT-HEAVY.**
Every scaling pressure — 60fps, 32 players, large forests, water-heavy terrain, GPU-heavy
atmosphere — surfaces not as worse *average* frame time but as **tail latency (p95/p99)**. The
current design answers blocking with *caps* (rotating per-tick windows in WaterSystem, a `>128`
batch threshold in SHIELD) rather than with *asynchronous ownership and staged completion*. A cap
bounds the worst single stall; it does not remove the main-thread `wait()` from the critical path,
and it leaves the renderer mapping GPU buffers synchronously inside the frame.

The audit found four concrete classes of main-thread blocking, each verified against the current
tree:

1. **Streaming "wait for all" barriers.** `wait_for_streaming_jobs()` (`SHIELD_WorldSystem.cpp:4287`)
   fans out to `wait_for_generation_jobs()` (`:339`) and `wait_for_meshing_jobs()` (`:347`), and is
   called **every tick** by the server loop (`ServerWorldRunner.cpp:489`). The meshing dispatch
   itself blocks on its own batch when the build set exceeds 128 chunks
   (`m_job_system->wait(m_job_system->dispatch_batch(build_jobs))`, `SHIELD_WorldSystem.cpp:2889-2890`;
   the lambda body that gets dispatched begins at `:2858`, `GenerateChunkData` at `:2875`).
2. **Water main-thread spikes.** The code itself documents the pathology: a 300ms+ streaming-init
   spike (`WaterSystem.cpp:27`), a ~450ms simulate-window block (`:34-35`), and an ~1300ms case
   where 18 *trivial* jobs were head-of-line blocked behind the streaming flood
   (`:439-445`). These were mitigated by rotating per-tick caps + an inline seed path — caps, not
   ownership.
3. **Synchronous GPU readback in render code.** The GPU SDF path dispatches a compute shader,
   inserts a fence, and then **blocks the CPU on it with `GL_TIMEOUT_IGNORED`**
   (`glClientWaitSync(m_gpu_sdf.compute_fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED)`,
   `RenderPipeline.cpp:4321`) before mapping the buffer (`glMapBuffer(... GL_READ_ONLY)`, `:4328`).
   The code even comments its own half-built intent: *"For async operation, create fence and return
   immediately… For now, do synchronous read"* (`:4314-4321`). The async path is **stubbed, not
   greenfield** — this spec finishes it.
4. **Blocking buffer readback in a pass.** `FoliagePass.cpp:793` reads the generated-blade count and
   buffer back with a synchronous `glGetBufferSubData` that *"blocks the CPU on compute completion
   (~5ms on the dense pose)"*. Today it is gated behind `m_readback_enabled` (the
   `FoliageInstancing` gate's `instance_hash` only; the indirect draw uses the GPU-resident count
   regardless) — so it is **not in the default hot path**, but it is the canonical anti-pattern this
   spec bans from spreading.

The fix F4 names — and this spec adopts — is **explicit phase ownership**: a simulation phase, a
render-submission phase, streaming workers, and **async GPU readback rings**. Main-thread waits are
replaced with **continuations** (built on the existing `JobHandle.completion` seam, `JobSystem.h:20`)
or **bounded result consumption**. The deterministic simulation advances from a **deterministic
availability set** — exactly the water-lockstep lesson (`docs/water-sim-lockstep-determinism.md`) —
while render residency is **best-effort**. Wall-clock job-completion timing must never become a sim
input.

**Determinism.** This is a concurrency rework, so determinism is the hard gate, not an afterthought.
`luminumbra_server_app --smoke` must stay `6f008a9f637c40b7`, run==replay, after every phase. The
goal is **hash-neutral**: the deterministic activation queue must reproduce the same chunk-availability
set per tick that the current per-tick barrier produces (the barrier guarantees arrival is a
deterministic function of position; the replacement must preserve that). Local-dev `world_hash` bumps
are acceptable, but a bump here means the availability contract was changed — it must be reviewed,
not waved through.

## Goals

- **G-1** — Replace main-thread streaming **"wait for all"** barriers with **per-tick completion
  budgets + a deterministic activation queue**, so sim advances from an availability set that is
  bit-reproducible independent of worker count or machine speed.
- **G-2** — Introduce a **frame-delayed async GPU readback API** (double-buffered / fenced PBO/SSBO
  ring) with **stale-safe consumers**, retiring the in-frame `glClientWaitSync(GL_TIMEOUT_IGNORED)`
  + `glMapBuffer(GL_READ_ONLY)` + `glGetBufferSubData` blocking pattern from render code.
- **G-3** — Establish **explicit phase ownership** (simulation / render-submission / streaming
  workers / async-readback) with the boundary rule: **deterministic sim residency vs. best-effort
  render residency.**
- **G-4** — Convert remaining main-thread `JobSystem::wait()` critical-path calls to **continuations
  or bounded consumption** built on the existing `JobHandle.completion` seam, so a slow worker
  produces *less residency this frame*, not *a longer stall*.
- **G-5** — Make tail latency a **first-class, gated metric**: instrument **p95/p99 main-thread wait
  time** and gate on it under movement (`--play-paths`, `--smoke-moving`), not just `--render-benchmark`
  average/p50.
- **G-6** — **Ban new synchronous GPU readbacks** in render code via a CI-checkable allowlist, so the
  anti-pattern cannot silently re-spread.
- **G-7** — Preserve determinism end-to-end: `world_hash` stays `6f008a9f637c40b7`, run==replay,
  after every phase; hash-neutral is the goal.

## Non-Goals

- **NG-1** — The RHI / Vulkan / DX12 backend port (that is **spec 014**). This spec authors the
  readback ring **compute-/RHI-shaped** so it ports cleanly behind 014's seam, but does not perform
  that migration. The ring's API is the abstraction; GL fences are today's *implementation*.
- **NG-2** — The render-framework pass graph and resource registry (that is **spec 016 — render
  framework**). The async readback ring is **shared infrastructure** owned jointly with 016; this
  spec defines the ring contract, 016 consumes it for pass scheduling.
- **NG-3** — The broader determinism-hardening audit checklist for render-adjacent features (that is
  **spec 018 — determinism hardening**). This spec consumes 018's **deterministic-availability-set**
  contract and supplies the streaming-activation reference implementation; it does not author 018's
  full cross-machine / cross-worker-count test matrix.
- **NG-4** — Auto-exposure metering itself (that is **spec 015 Pillar A, FR-A-004**). This spec
  *forbids* a synchronous exposure readback and *requires* metering to consume the async ring; it
  does not implement the metering math.
- **NG-5** — Networking transport scaling (F6 / replication); a separate spec. The 32-player tail
  pressure is in scope only as a **load condition** the perf gates must run under, not a transport
  change.
- **NG-6** — Removing the water rotating-window caps that are *already* deterministic and bounded.
  Those stay; this spec replaces the *barriers around them*, not the amortization itself.

## Phase ownership model (the architecture this spec lands)

Four phases with explicit residency rules:

| Phase | Owns | Residency rule | May block? |
|-------|------|----------------|------------|
| **Simulation** | sim tick @ 30Hz; `world_hash` inputs | reads **only** the deterministic availability set | never on wall-clock job completion |
| **Streaming workers** | generation + meshing jobs | produce into a **deterministic activation queue** | off the main thread |
| **Render submission** | per-frame GL command building | consumes **best-effort** render residency + **stale-safe** readback ring results | never `wait()` on GPU/CPU jobs |
| **Async readback** | fenced PBO/SSBO ring | results are **frame-delayed**; consumers tolerate N-frame staleness | never `glClientWaitSync(... GL_TIMEOUT_IGNORED)` in-frame |

The boundary invariant: **anything that can move `world_hash` reads only the availability set**;
**anything render-side reads best-effort residency and may be one or more frames stale.** GPU→CPU
data is render-only unless it is explicitly delayed, quantized, and hashed (the spec-018 contract).

## Functional Requirements

IDs are grouped. Each is anchored to a verified file/line in the current tree (see **Key files**).
"Replace" means the listed blocking call is retired by the new behavior; "ban" means a gate fails if
a new instance appears.

### Group A — Async GPU readback ring (shared with spec 016; required by spec 015 FR-A-004)

- **FR-A-001 — Ring API.** The engine shall expose an async GPU readback API — a double-buffered /
  N-buffered fenced ring (PBO for texture reads, persistent-mapped SSBO for buffer reads) — that
  (a) issues the GPU copy + fence on submit and **returns immediately**, and (b) yields a result only
  once its fence has signalled, **never** blocking the calling frame. This is the abstraction the
  `RenderPipeline.cpp:4314-4321` comment already anticipates ("create fence and return immediately").
- **FR-A-002 — Stale-safe consumers.** Every ring consumer shall be **stale-safe**: it consumes the
  most recently *completed* result (which may be N frames old) and never stalls waiting for the
  current frame's result. The API shall make "no completed result yet" an ordinary, non-blocking
  return state.
- **FR-A-003 — Retire the synchronous SDF readback.** The GPU SDF compute path shall consume the
  ring instead of the in-frame
  `glClientWaitSync(m_gpu_sdf.compute_fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED)`
  (`RenderPipeline.cpp:4321`) + `glMapBuffer(... GL_READ_ONLY)` (`:4328`). Because the SDF result
  feeds chunk *data* that can influence sim, its consumption must route through the deterministic
  activation queue (Group B), **not** be read inline on the render thread (see OQ-2).
- **FR-A-004 — Retire / quarantine the foliage blocking readback.** The foliage `glGetBufferSubData`
  count+blade readback (`FoliagePass.cpp:793`) shall, when enabled, route through the ring (frame-
  delayed). The default draw path already uses the GPU-resident count via indirect draw and must
  remain readback-free; the gate-only `m_readback_enabled` path must not reintroduce an in-frame
  block.
- **FR-A-005 — Exposure metering consumes the ring (spec 015 FR-A-004 coupling).** When spec 015
  Pillar A's auto-exposure metering lands, it **shall** read scene luminance through this ring. A
  **synchronous** exposure readback is forbidden (enforced by FR-G-001's allowlist). Auto-exposure
  is inherently temporal, so a one-frame-stale luminance is correct, not a compromise.
- **FR-A-006 — RHI-shaped API.** The ring's public interface shall be backend-agnostic (submit /
  poll / consume), so spec 014's RHI implements it with Vulkan timeline semaphores / DX12 fences
  without callers changing. GL fences are the *implementation*, not the *interface*.

### Group B — Deterministic streaming activation (replace "wait for all" barriers)

- **FR-B-001 — Deterministic activation queue.** Streaming workers shall publish completed
  generation/meshing results into a **deterministic activation queue**. The per-tick **availability
  set** consumed by sim shall be a pure function of (anchor position(s), seed, params, tick) — **not**
  of worker count or wall-clock completion order — preserving the guarantee the current per-tick
  barrier provides (`docs/water-sim-lockstep-determinism.md`: chunk arrival is a deterministic
  function of position because the barrier drains every tick).
- **FR-B-002 — Per-tick completion budget replaces the streaming barrier.** The every-tick
  `wait_for_streaming_jobs()` call in the server loop (`ServerWorldRunner.cpp:489`) shall be replaced
  by **draining the activation queue up to a per-tick budget**. Sim advances on whatever the
  availability set contains; pending chunks activate on subsequent ticks **in deterministic order**.
- **FR-B-003 — Remove the in-fan barriers from the hot path.** `wait_for_generation_jobs()`
  (`SHIELD_WorldSystem.cpp:339`) and `wait_for_meshing_jobs()` (`:347`) shall remain **only** for
  teardown/shutdown (the destructor at `:334-337`) and explicit barrier points
  (`ComputeWorldHash` at `ServerWorldRunner.cpp:516`, save/snapshot), **not** on the per-tick
  simulation critical path.
- **FR-B-004 — Bound the meshing dispatch.** The unconditional `m_job_system->wait(dispatch_batch(...))`
  for batches over 128 (`SHIELD_WorldSystem.cpp:2889-2890`) shall be replaced by a fire-and-publish
  dispatch whose results enter the activation queue (FR-B-001), so a large build set produces
  *staged activation*, not a single proportional stall.
- **FR-B-005 — Hash-neutral activation.** The activation set per tick shall reproduce, bit-for-bit,
  the chunk set the current barrier produces for the determinism scenarios, so `--smoke` stays
  `6f008a9f637c40b7` and run==replay (NFR-001). Any deviation is a reviewed `world_hash` bump, not a
  silent one.

### Group C — Continuations / bounded consumption (retire critical-path `wait()`)

- **FR-C-001 — Continuation seam.** `JobSystem` shall support **continuations** — a job-completion
  callback / dependency built on the existing `JobHandle.completion`
  (`std::shared_ptr<JobCompletionState>`, `JobSystem.h:20`) — so dependent work runs *when* its inputs
  complete instead of the caller spinning a `JobSystem::wait()` (`JobSystem.h:57`). Continuations
  respect the existing High/Normal lanes + starvation guard (`kNormalServiceInterval`, `JobSystem.h:36`).
- **FR-C-002 — Bounded result consumption.** Where a continuation does not fit, the main thread shall
  consume **at most a bounded count** of completed results per frame/tick (the water
  rotating-window pattern, generalized), never an unbounded `wait()` on the full set.
- **FR-C-003 — Water seeding off the barrier.** The water init/seed path (`WaterSystem.cpp:439-448`,
  already inlined to dodge the ~1300ms queue-wait) and the rotating sim/init/resize windows
  (`MAX_WATER_*_PER_TICK`, `:31/:38/:44`) shall consume from the deterministic activation queue
  (Group B) rather than re-implementing private amortization against a flooded job queue. The
  bit-identical per-chunk write guarantee (`:444-445`) must be preserved.

### Group D — Tail-latency instrumentation + readback ban (the gates)

- **FR-D-001 — Wait-time instrumentation.** The perf harness shall record **per-frame main-thread
  wait time** (sum of time spent in `JobSystem::wait`, GL fence waits, and buffer-map stalls) and
  emit **p50 / p95 / p99** of that metric in the `--render-benchmark` / `--play-paths` /
  `--smoke-moving` JSON. (This metric does **not** exist today — `--render-benchmark` reports only
  `frame_wall`/`gpu` p50 per spec 015 NFR-002 — so it is a deliverable, not an assumed signal.)
- **FR-D-002 — p99 wait gate.** A perf gate shall fail when **p99 main-thread wait** under
  `--play-paths` / `--smoke-moving` exceeds a budget (target TBD per OQ-4), under the load conditions
  that expose the tail: movement, dense forest, water-heavy terrain (and, where harness-supported,
  multi-avatar load via `--avatars`).
- **FR-G-001 — Synchronous-readback ban (allowlist gate).** A CI-checkable gate shall **fail when a
  new** in-frame synchronous GPU readback appears in render code — `glClientWaitSync(...,
  GL_TIMEOUT_IGNORED)`, `glMapBuffer(..., GL_READ_ONLY)`, or `glGetBufferSubData` — outside an
  explicit allowlist. The initial allowlist is exactly the two known sites
  (`RenderPipeline.cpp:4321/4328`, `FoliagePass.cpp:793`); each is removed from the allowlist as its
  FR (A-003 / A-004) lands. New code must use the ring (FR-A-001).

## Non-Functional Requirements

- **NFR-001 — Determinism (hard gate).** `luminumbra_server_app --smoke` must stay
  `6f008a9f637c40b7`, run==replay, after **every** phase. Sim advances from the **deterministic
  availability set**, NOT wall-clock job completion (the water-lockstep lesson,
  `docs/water-sim-lockstep-determinism.md`). The async/threading rework must not change sim results.
  Hash-neutral is the goal; a bump is a reviewed change to the availability contract, never silent.
- **NFR-002 — Availability set independent of worker count / machine speed.** The same availability
  set per tick must be produced for worker_count ∈ {1, N} and on fast vs. slow machines (the
  spec-018 cross-worker / fast-slow determinism contract). Completion *timing* must never be a sim
  input. (Validated via `--smoke` across worker counts; full matrix is spec 018.)
- **NFR-003 — Tail-latency budget, not average.** The success metric is **p95/p99** main-thread wait
  under load, not p50 frame time. A change that improves average while worsening the tail is a
  regression.
- **NFR-004 — Render residency is best-effort.** Missing render residency (a not-yet-streamed chunk,
  a not-yet-ready readback) degrades **visual** fidelity for a frame; it must **never** stall the
  frame and must **never** alter sim. Stale-safe is the default, not the exception.
- **NFR-005 — RHI-portable.** The readback ring (Group A) and continuation seam (Group C) shall be
  authored backend-agnostic so they port behind spec 014's RHI (Vulkan timeline semaphores / DX12
  fences) without caller changes. Shared with spec 016.
- **NFR-006 — No new global locks on the hot path.** Phase-ownership boundaries shall use the
  existing lock-light job seam (lanes + starvation guard, `JobSystem.h:36`) and lock-free / bounded
  queues; the rework must not introduce a coarse mutex that serializes the very phases it separates.
- **NFR-007 — Two-build-tree hygiene.** All gates must declare which tree they ran in
  (`build/` root vs. `build/debug` preset). Build the tree you test (the engine-frontier gate uses
  `build/debug`); a stale-tree wait-metric is worse than none.

## Acceptance Criteria

Each is verifiable via the existing harness; the measurable signal / command is named inline.

### Cross-cutting
- [ ] **AC-001** — `luminumbra_server_app --smoke` stays `6f008a9f637c40b7`, run==replay, after
  **every** phase (Groups A–D).
- [ ] **AC-002** — `--smoke` produces the **identical** `world_hash` for `worker_count = 1` and the
  default worker count (proves availability set is worker-count-independent, NFR-002).
- [ ] **AC-003** — `--smoke-moving` shows **zero desync flakes / ≥24 runs** after the barrier→queue
  replacement (the moving-water harness is the streaming-arrival oracle).

### Group A — readback ring
- [ ] **AC-A-001** — The async ring API exists, returns immediately on submit, and yields results
  only after fence signal (unit test: submit → poll-returns-pending → later poll-returns-result, no
  blocking wait on the submit frame).
- [ ] **AC-A-002** — `grep` shows `glClientWaitSync(... GL_TIMEOUT_IGNORED)` and `glMapBuffer(...
  GL_READ_ONLY)` removed from the SDF path (`RenderPipeline.cpp:~4321/4328`); the SDF compute path
  routes through the ring (FR-A-003).
- [ ] **AC-A-003** — The default foliage draw path performs **no** `glGetBufferSubData` in-frame
  (the indirect-draw count path is unchanged); the gate-only readback, when enabled, is frame-
  delayed (FR-A-004). FLIP parity off-tinted ~0 vs. pre-change foliage.
- [ ] **AC-A-004** — When spec 015 Pillar A lands, its exposure metering reads through the ring; a
  synchronous exposure readback would fail FR-G-001's gate (AC-D-003). (Forward-coupled to 015.)

### Group B — deterministic activation
- [ ] **AC-B-001** — The per-tick `wait_for_streaming_jobs()` on the sim critical path
  (`ServerWorldRunner.cpp:489`) is replaced by a budgeted activation-queue drain; the in-fan
  barriers (`SHIELD_WorldSystem.cpp:339/347`) remain only in teardown + explicit-barrier sites
  (`ComputeWorldHash` `:516`).
- [ ] **AC-B-002** — The meshing dispatch no longer unconditionally `wait()`s on the >128 batch
  (`SHIELD_WorldSystem.cpp:2889-2890`); large build sets activate in staged ticks, and `--smoke`
  hash is unchanged (AC-001).
- [ ] **AC-B-003** — Streaming-burst p99 main-thread wait under `--play-paths` (move into dense
  forest / water-heavy terrain) drops vs. the pre-change baseline (report both JSONs).

### Group C — continuations
- [ ] **AC-C-001** — `JobSystem` exposes a continuation built on `JobHandle.completion`
  (`JobSystem.h:20`); a unit test shows a dependent job runs on completion **without** the caller
  invoking `JobSystem::wait` (`JobSystem.h:57`).
- [ ] **AC-C-002** — Water init/sim/resize consume from the activation queue (FR-C-003); per-chunk
  writes remain bit-identical (`WaterSystem.cpp:444-445`) → `--smoke` hash unchanged.

### Group D — tail-latency gates + ban
- [ ] **AC-D-001** — `--render-benchmark` / `--play-paths` / `--smoke-moving` JSON now report
  **p95/p99 main-thread wait** (FR-D-001) in addition to existing p50 `frame_wall`/`gpu`.
- [ ] **AC-D-002** — The p99 main-thread-wait gate (FR-D-002) passes under the load scenario at the
  agreed budget (OQ-4); a deliberately reintroduced barrier fails it (negative test).
- [ ] **AC-D-003** — The synchronous-readback allowlist gate (FR-G-001) **fails** when a new
  `glClientWaitSync(... GL_TIMEOUT_IGNORED)` / `glMapBuffer(GL_READ_ONLY)` / `glGetBufferSubData` is
  added outside the allowlist (negative test), and **passes** on the current tree with the two known
  sites allowlisted.

## Phasing (migration path, sequenced by risk + dependency)

Mirrors F4's migration path: ban-new → ring → queue → gates.

1. **D (ban + instrument first).** Land FR-G-001 (allowlist gate, current two sites whitelisted) and
   FR-D-001 (p95/p99 wait instrumentation) **before** the rework, so every later phase is measured
   against a real tail baseline and no new sync readback sneaks in mid-migration. *(cheap, enabling)*
2. **A (async readback ring).** Build the ring (FR-A-001/002/006); retire the SDF
   (FR-A-003) and quarantine the foliage readback (FR-A-004); remove each from the allowlist as it
   lands. Shared with spec 016; unblocks spec 015 FR-A-004 (FR-A-005). *(render-only — no `world_hash`
   risk if SDF data routes through Group B)*
3. **B (deterministic activation queue).** Replace the per-tick streaming barrier
   (`ServerWorldRunner.cpp:489`) and the meshing batch-wait with the activation queue + per-tick
   budget. **The determinism-sensitive phase** — hash-neutral, validated by AC-001/002/003 every
   step. *(highest determinism risk; depends on spec 018's availability-set contract)*
4. **C (continuations + bounded consumption).** Convert remaining critical-path `wait()`s
   (water seeding, generic dependent jobs) to continuations / bounded consumption on the Group-B
   queue. *(cleanup + the tail-latency payoff lands here)*

## Blocking gates (per phase)

1. **Determinism (AC-001/002/003):** `--smoke == 6f008a9f637c40b7`, run==replay, identical across
   worker counts, zero `--smoke-moving` flakes ≥24 runs. Never moves silently.
2. **Tail latency (AC-D-001/002, AC-B-003):** p95/p99 main-thread wait under `--play-paths` /
   `--smoke-moving` within budget and improved vs. the pre-rework baseline; average frame time is
   *not* the gate.
3. **Readback ban (AC-D-003):** the allowlist gate is green and the two known sites shrink to zero as
   Group A lands.

## Risks / unknowns

- **Determinism regression (Group B) is the headline risk.** Replacing a per-tick barrier with a
  staged queue is exactly the class of change that caused the historic water desync. Mitigation: the
  availability set is a pure function of position/seed/params/tick; validate hash-neutrality every
  step; lean on `--smoke-moving` as the streaming-arrival oracle.
- **Stale-safe consumers can look wrong before they look slow.** A frame-delayed readback that a
  consumer treats as current produces a one-frame visual artifact. Mitigation: NFR-004 makes
  staleness the default contract, and consumers must declare their staleness tolerance.
- **Shared ownership with 016/018.** The ring (016) and availability-set contract (018) are
  co-owned; an interface drift between specs would cause double work. Mitigation: this spec defines
  the ring *interface* and the activation-queue *reference implementation*; 016/018 consume them.
- **GL is not the final backend.** Authoring the ring against GL fences risks baking GL assumptions.
  Mitigation: NFR-005 (backend-agnostic submit/poll/consume); 014 implements with timeline
  semaphores.
- **Budget calibration (OQ-4).** A p99 gate with the wrong budget either never fires or always
  fires. Pick the budget from the measured pre-rework baseline before turning the gate red.

## Open Questions

- **OQ-1 (drives FR-B-001/B-002 design)** — What is the right per-tick activation budget, and is it a
  fixed count or adaptive to frame headroom? It must stay a **deterministic** count (host==peer) — an
  adaptive budget keyed to wall-clock headroom would re-introduce timing as a sim input. Likely a
  fixed deterministic count, mirroring `MAX_WATER_*_PER_TICK`.
- **OQ-2 (blocks FR-A-003)** — The GPU SDF readback feeds chunk *data* that can influence sim, so it
  cannot simply become a best-effort render-side stale read. Decide: (a) route SDF generation
  entirely off the GPU readback into the deterministic activation queue, or (b) keep GPU SDF as a
  render-only acceleration with a CPU deterministic fallback as the sim source of truth. (a) is
  cleaner for determinism; (b) is less invasive.
- **OQ-3** — Ring depth (double vs. triple buffer) and acceptable staleness per consumer (exposure
  tolerates 1–2 frames; does anything need ≤1?). Sets the ring's memory + latency tradeoff.
- **OQ-4 (blocks FR-D-002 / AC-D-002)** — The p99 main-thread-wait budget number and the exact load
  scenario (forest density, water coverage, avatar count) the gate runs under. Pick from the measured
  baseline (phase 1) before the gate goes red.
- **OQ-5** — Does spec 016's pass-graph scheduler own the render-submission phase, or does this spec?
  Co-ownership boundary must be drawn so the render-submission phase isn't defined twice.
- **OQ-6** — Continuation execution context: do continuations run on the completing worker, a
  dedicated continuation lane, or the existing Normal lane? Affects the starvation guard
  (`kNormalServiceInterval`, `JobSystem.h:36`) and must not deadlock a continuation that re-dispatches.

## Key files

- `src/luminumbra_common/core/JobSystem.h` — the job seam. `JobHandle.completion`
  (`std::shared_ptr<JobCompletionState>`, `:20`) is the continuation hook (FR-C-001); `dispatch_batch`
  `:56`, `wait` `:57`; High/Normal lanes + starvation guard `kNormalServiceInterval` `:36` (NFR-006,
  OQ-6).
- `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp` — streaming barriers.
  `wait_for_generation_jobs` `:339`, `wait_for_meshing_jobs` `:347` (keep teardown-only, FR-B-003);
  `wait_for_streaming_jobs` `:4287`; meshing dispatch batch-wait `m_job_system->wait(dispatch_batch(...))`
  `:2889-2890` (dispatched lambda `:2858`, `GenerateChunkData` `:2875`) → staged activation (FR-B-004);
  destructor barriers `:334-337`.
- `src/luminumbra_server/ServerWorldRunner.cpp` — the per-tick barrier `wait_for_streaming_jobs()`
  `:489` (replace with budgeted activation drain, FR-B-002); explicit-barrier sites that **stay**:
  `ComputeWorldHash` `:516`, plus save/boot-settle calls `:392/:546/:591/:627`.
- `src/luminumbra_common/systems/WaterSystem.cpp` — documented spikes + amortization: 300ms init `:27`,
  450ms sim `:34-35`, 1300ms queue-wait note `:439-445`; rotating caps `MAX_WATER_INITS_PER_TICK` `:31`,
  `MAX_WATER_SIMS_PER_TICK` `:38`, `MAX_WATER_RESIZES_PER_TICK` `:44`; inline seed loop + bit-identical
  guarantee `:446-448`/`:444-445` → consume activation queue (FR-C-003).
- `src/luminumbra_client/rendering/RenderPipeline.cpp` — GPU SDF readback. Dispatch `:4309`, fence
  `:4318`, **self-documented async intent** `:4314-4321`, the in-frame block
  `glClientWaitSync(... GL_TIMEOUT_IGNORED)` `:4321`, `glMapBuffer(... GL_READ_ONLY)` `:4328` →
  retire via ring (FR-A-003, OQ-2).
- `src/luminumbra_client/rendering/passes/FoliagePass.cpp` — blocking `glGetBufferSubData` count+blade
  readback `:793`, gated behind `m_readback_enabled` (gate-only; indirect draw uses GPU count) →
  quarantine/frame-delay (FR-A-004).
- `docs/water-sim-lockstep-determinism.md` — the deterministic-arrival lesson the availability set
  must preserve (FR-B-001, NFR-001/002).
- new ring + queue + instrumentation: an `AsyncReadbackRing` (Group A, shared with **spec 016**), a
  `StreamingActivationQueue` (Group B, contract from **spec 018**), and the wait-time/p99 harness
  fields (Group D) in the `--render-benchmark` / `--play-paths` / `--smoke-moving` paths.

## Verification (end-to-end)

1. Build the correct tree (engine-frontier gate uses `build/debug`; prepend `C:\msys64\ucrt64\bin`);
   declare which tree each gate ran in (NFR-007).
2. **Determinism:** `--smoke` byte-identical (`6f008a9f637c40b7`, run==replay) after every phase;
   identical across worker counts (AC-002); `--smoke-moving` zero flakes ≥24 (AC-003).
3. **Tail latency:** `--play-paths` / `--smoke-moving` p95/p99 main-thread wait reported (AC-D-001)
   and within the OQ-4 budget (AC-D-002), improved vs. the phase-1 baseline (AC-B-003).
4. **Readback ban:** the allowlist gate is green on the current tree and red on a deliberately added
   sync readback (AC-D-003); the SDF/foliage sites leave the allowlist as Groups A land.
5. **Spec coupling:** confirm spec 015 Pillar A exposure metering consumes the ring (FR-A-005); the
   ring interface matches what spec 016 schedules and spec 018's availability-set contract.
