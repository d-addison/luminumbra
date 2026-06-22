# Streaming-elision instrumentation map (ultracode audit, synthesized)

> From the 5-agent mutation audit (workflow wf_0c0f7b00-b40; its synthesis agent 529'd, so this is
> hand-synthesized from the cached audit + my own reasoning). Line numbers are vs the CURRENT clean
> tree (streaming attempt #1 reverted). This is the complete site list so the determinism-safe impl
> instruments ALL of them — a missed collision push = player fall-through; a missed dirty bump = a
> skipped remesh (stale LOD / seam crack).

## Design (determinism-safe, per attempt-#1 failure + red-team)
Two deterministic signals; the candidate build elides, collision NEVER elides (drains every tick).

### A. `m_pending_collision` (std::vector<ChunkID>) — PUSH at 4 sites; DRAIN every tick
Eligibility predicate (must match today's Step-4 scan): `state==Ready && !has_collision && current_lod==0
&& !mesh_vertices.empty() && !mesh_indices.empty()`.
PUSH sites (where a chunk becomes collision-eligible — every `has_collision.store(false)` at Ready/LOD0):
- `GameSession.cpp:921` — chunk adoption after world load
- `SHIELD_WorldSystem.cpp:2701` — EnsureSurfaceReadyNear synchronous mesh
- `SHIELD_WorldSystem.cpp:3697` — process_completed_meshing_jobs after mesh published
- `RuntimeScenarioHarness.cpp:~4808-4814` — CarveSphereIntoChunk (push only if current_lod==0)
DRAIN (replaces the O(N) Step-4 scan, `SHIELD_WorldSystem.cpp:2247-2260`): drain `m_pending_collision`
every tick under the existing `MAX_COLLISION_MESHES_PER_FRAME` cap, **RE-CHECKING the predicate** per
entry (so a stale/duplicate entry is a harmless skip), re-deferring uncreated entries. **Determinism:
must run EVERY tick (NOT gated) so it converges to the same has_collision SET as the baseline scan by
the hash point — that is the exact bug that broke attempt #1.** Keep a **full O(N) backstop scan on
activation ticks** (every STREAMING_ACTIVATION_INTERVAL_FRAMES) so a missed push self-heals within one
interval (no permanent fall-through).

### B. `dirty_generation` (uint64 on StreamingState) — bump at main-thread-observed transitions; gates Step 2+3
Bump (function-level — once per call when work happened — is enough; the audit's ~30 field-level sites
all live inside these):
- `process_completed_meshing_jobs` (publishes mesh/LOD/state/sdf/material, :3685-:3719): ++ if it
  published ≥1 chunk this call.
- `EnsureSurfaceReadyNear` (synchronous rebuild, :2694-:2761): ++ if it rebuilt ≥1 chunk.
- chunk insert/erase into `m_streaming_state.chunks` (`:2596` erase, `:2651` insert, `:3451` gen insert,
  `:3816` adopt) — covered generically by a `count_changed` (size vs `m_last_chunk_count`) check, plus
  the persistence-load inserts (`WorldPersistenceRoundtrip.cpp:712`, `WorldSaveService.cpp:531`,
  `GameSession.cpp:939`).
- generation-job completion edge: generation sets Idle on a worker thread (`:3458`); the MAIN thread
  observes the falling edge at `clear_completed_job_handle(generation_job_handle)` (`:1934`) — bump there.
- carve/mining (`RuntimeScenarioHarness.cpp:4808`): ++ directly (pre-EnsureSurfaceReadyNear).
- persistence load / ApplyChunkJson (`WorldPersistenceRoundtrip.cpp:271-285`): ++ on load.

GATE (wrap Step 2 meshed_columns build + Step 3 candidate loop/sort/dispatch):
`streaming_dirty = (dirty_generation != m_last_serviced_generation) || anchor_changed (EXACT vector
equality — LOD keys on continuous distance) || count_changed || activation_due || !m_last_pass_drained`.
Advance `m_last_serviced_generation = dirty_generation` **only when fully drained**
(`deferred_meshing==0 && !meshing_jobs_active()`) so budget-deferred remeshes aren't abandoned
(red-team must-fix). **Do NOT gate on job-activity polling** (that broke attempt #1).

Telemetry scans (passes 1+5) + water update + activation tick stay OUTSIDE the gate (unchanged cadence).
Preserve `terrain_meshing_backlog` across elided frames (member) so queue-depth telemetry doesn't collapse.

## Verify regimen (needs the build/gate loop — currently blocked by an API/classifier outage)
1. Rebuild server+client release (both link luminumbra_common). 2. All 6 determinism gates GREEN
   (hashes match pinned: PopulatedWorldReplay f314123daebb6cd1, canonical cf9c8cddf7156cd6,
   NetworkedSession ddfc228811d9f32b) — this IS the byte-compare. 3. Visual gates: PlayerView,
   FarLodHorizon, WorldVisualSweep, FoliageInstancing (warmup at fixed anchor must still stream → the
   count_changed/activation/dirty signals must fire during warmup). 4. Benchmark: streaming_ms drop +
   streaming_pass_elided true on the settled static pose (clock-independent work-count). Implement in 2
   increments (collision pending-set first — determinism-neutral; then the gate) and run the determinism
   gates after EACH. Pre-built warm baseline catches a break in one run; revert if it does (attempt #1
   precedent).

## Audit gaps to resolve during impl
- Confirm CarveSphereIntoChunk is the ONLY runtime in-place voxel-edit path (no other player mining/sculpt API).
- Confirm `run==replay` suite exercises a carve/mining scenario; if not, the carve dirty bump is untested.
- Drain MUST re-check the predicate (not consume blindly) so a missed push degrades to "handled by the
  activation-tick backstop" rather than a hard miss.
- Ready→Meshing without an intervening has_collision.store(false): drain re-check + backstop cover it.
