# Spec: 200fps worst-case floor (<5ms/frame) — water + sim + render-submit

Status: ready to implement
Branch: feat/polyglot-audit-roadmap
Determinism oracle: `build/bin/luminumbra_server_app.exe --smoke` -> `world_hash == world_hash_replay == b05b642e1299075f`
Profiler: `build/release/bin/luminumbra_client_app.exe --profile-fly 16 --auto-create-world --auto-enter-world --no-audio`

## Ground truth (verified against code, this is what the spec is built on)

Findings that OVERTURN parts of the brief — all confirmed by reading the source:

1. **The float render-mirror sim is DEAD CODE.** `WaterSystem::dispatch_simulation_jobs`
   (WaterSystem.cpp:564-711) and `simulate_chunk_water` (713-817) have ZERO call sites.
   `WaterSystem::update` (265-562) runs ONLY the integer Spec 009 path. The brief's
   "stream-split water=12-30ms is the parallelized float mirror" is a misattribution.
   The 12-30ms is `WaterSystem::update` in full (SHIELD_WorldSystem.cpp:2014-2016), dominated by
   first-time INIT seeding (WaterSystem.cpp:316-375, capped 6/tick) + `StepChunkWaterFixed`
   (112-239) + the cross-chunk seam pass (492-534).

2. **The live integer kernel's only per-cell libm is `RiverInfluenceAt`** (WaterSystem.cpp:142),
   a single `GenSingle2D` per cell (RiverInfluenceFromNoise, SHIELD_WorldSystem.cpp:1403-1427) —
   a pure function of (x,z,seed), recomputed every tick, gated by `!finite_hydrology` (default ON).
   NOT "multi-octave 4096/tick"; grid is Medium = 8x8 = 64 cells/chunk, capped at
   MAX_WATER_SIMS_PER_TICK=64 chunks.

3. **ALL FIVE water arrays are in world_hash today.** kRenderMeshHashExcludedFields
   (WorldPersistenceRoundtrip.cpp:767-775) excludes only mesh/lod/version fields. It does NOT
   contain `water_level_data`, `water_flow_data`, `water_sim_terrain_height`, `water_depth_mm`,
   or `water_bed_mm` — all are emitted by ChunkToJson (259-265) and survive into the hashed
   projection. The in-code comment (262-263) calling the floats "render-only mirrors" never
   updated the hash scope.

4. **The float arrays are NOT pure stateless derivatives** of the integer state, because of two
   live float-only feedback edges:
   - WaterSource inject (WaterSystem.cpp:397): `water_level_data[i] += water_added` writes the
     float ONLY, never `water_depth_mm`.
   - ResizeSimulationGrid (WaterSystem.cpp:1119-1130, esp. 1127): RECONSTRUCTS the hashed
     `water_depth_mm` FROM `water_level_data`. This path is DEAD IN STEADY STATE
     (CalculateRequiredDetail is hardwired to a fixed resolution, 1022-1023) but is REACHABLE on
     save/load when a persisted chunk's `current_water_resolution` differs.

5. **The catch-up clamp never fires on the oracle.** ServerWorldRunner feeds exactly `fixed_dt`
   per frame, so `advance()` returns 1 every tick and the cap is dead code under `--smoke`.
   `m_simulationClock` is default-constructed (GameSession.h:205), cap=4.

## OPEN QUESTION — is the float water mirror render-only and removable from world_hash?

**Position: PARTIALLY. The floats are a render mirror in steady state, but two live float->sim
feedback edges (findings 4) mean they are NOT freely removable until those edges are severed.**

- `water_level_data` IS a render derivative of `water_bed_mm`+`water_depth_mm` for the mesher,
  regenerated every tick at WaterSystem.cpp:229.
- BUT excluding it from the hash and THEN amortizing/skipping the float write (the "unlock" the
  brief sells) is UNSAFE while line 1127 can re-seed hashed `water_depth_mm` from a stale float on
  a save/load resize, and while line 397 injects into the float only.
- **Recommendation:** treat hash-exclusion of the floats as a TWO-PART change gated on first
  killing the feedback edges (Step 7 below). Until then, the authoritative hashed truth is
  `water_depth_mm`/`water_bed_mm` and the floats stay hashed (harmless, just redundant). Do NOT
  ship a float-only amortization that relies on exclusion alone.

---

## Ordered implementation plan (impact-per-risk, biggest-safe-win first)

Baseline worst-frame budget (brief): water 12-30ms, occasional sim 15.9ms, occasional render
21-25ms. Target: <5ms worst-case. The single largest lever is the **streaming-activation gate**
(render path, byte-identical), but the dominant SUSTAINED slow-frame cost is water. Steps are
ordered: prove-safe byte-identical wins first, then deterministic re-pins, then render-only.

### Step 1 — Delete the dead float-mirror sim (hygiene; unblocks reasoning)
- **File/fn:** WaterSystem.cpp:564-817 (`dispatch_simulation_jobs`, `simulate_chunk_water`);
  WaterSystem.h:154-155 + the `WaterChunkSnapshot`/`WaterSimNeighbors`/`WaterChunkSimulationOutput`/
  `WaterSimulationTask` struct decls.
- **Approach:** pure dead-code removal. Grep-confirmed zero callers. Keep
  `GetWaterResolution`/`HasCompleteWaterGrid`/`GetWaterCellCount` (live integer-path callers).
- **Determinism class:** byte-identical (never executed).
- **Validation:** build server + `--smoke` stays `b05b642e1299075f` run==replay. No re-pin.
- **Expected ms:** 0 (frame), removes ~250 LOC + the misleading narrative.

### Step 2 — Cache the static river-source mask per chunk (kill per-tick noise)
- **File/fn:** WaterSystem.cpp:138-147 (consume), 351-369 (init build), 1063-1130
  (ResizeSimulationGrid rebuild); Chunk.h (add `std::vector<std::int32_t> water_src_mm`, NON-serialized).
- **Approach:** precompute per-cell `water_src_mm[i] = (RiverInfluenceAt(wx,wz) >= RIVER_SOURCE_THRESHOLD) ? RIVER_DISCHARGE_MM : 0`
  once at init and on resize. Phase-0 becomes `depth[i] += water_src_mm[i]; out_src += water_src_mm[i];`.
  Same cell-center formula, same threshold/discharge, same row order -> byte-identical.
  **REQUIRED robustness:** add a size-guard rebuild at the top of `StepChunkWaterFixed`
  (`if (water_src_mm.size() != n) rebuild from RiverInfluenceAt`) so persistence-LOADED chunks
  (which skip the init loop, WorldPersistenceRoundtrip.cpp:299-308) don't read an empty buffer
  (OOB / silent dead rivers). Do NOT add `water_src_mm` to any persistence/hash field list.
- **Determinism class:** byte-identical. No re-pin.
- **Validation:** `--smoke` stays `b05b642e1299075f`. Re-measure gain (see note).
- **Expected ms:** the only per-cell libm in the integer hot loop; realistic ~1-4ms off a full
  64-chunk window (NOT the 6-15ms the brief claims — grid is 64 cells, not 1024; ZERO under
  finite hydrology, which skips Phase-0 entirely).

### Step 3 — Hoist the two per-call scratch allocations (surf + depth_before)
- **File/fn:** WaterSystem.cpp:131 (`depth_before` copy), 157 (`surf` alloc), 227 (max_delta).
- **Approach:** make `surf` and `depth_before` `thread_local` resize-without-shrink scratch (keep
  the FULL `depth_before` copy — reuse storage only; do NOT "track max_delta incrementally", that
  changes the hashed `max_water_delta_last_tick`/`water_mesh_dirty_ticks` and is a correctness
  trap). thread_local satisfies the future parallel case (Step 4).
- **Determinism class:** byte-identical. No re-pin.
- **Validation:** `--smoke` stays green.
- **Expected ms:** small (~0.05-0.2ms; allocator pressure). Honest: minor.

### Step 4 — Parallelize the integer `StepChunkWaterFixed` internal loop
- **File/fn:** WaterSystem.cpp:478-482 (parallelize), per-job src/sink reduce; reuse
  JobSystem `dispatch_batch(JobPriority::High)` + `wait(handle)`.
- **Approach:** each chunk's internal step writes only its own buffers; the sole external read
  (`RiverInfluenceAt`, or `water_src_mm` after Step 2) is pure/stateless. Dispatch the 64 calls as
  a High batch; make `out_src`/`out_sink` PER-JOB locals and sum them in id-sorted order after
  `wait()` (integer add is associative -> bit-exact; they feed only the non-hashed mass-debug
  invariant). Cross-chunk seam pass (492-534) STAYS sequential. `wait()` before the seam pass.
- **Determinism class:** byte-identical. No re-pin.
- **Validation:** `--smoke` + WaterDeterminism gate stay green.
- **Expected ms:** REALISM CAVEAT — the integer kernel at 64x64 cells is small (tens-to-low-hundreds
  of µs). Two adversarial verdicts judged this near-zero/possibly net-negative (job dispatch
  overhead vs <1ms work). **Adopt ONLY if Step 2's re-measurement shows the kernel is actually
  multi-ms; otherwise SKIP (low ROI).** Do not assume the brief's 3-6ms.

### Step 5 — Chunk-quantized anchor gate for the streaming meshing pass (LARGEST render win)
- **File/fn:** SHIELD_WorldSystem.cpp:2042-2048 (gate); add `m_last_anchor_chunks` to
  SHIELD_WorldSystem.h.
- **Approach:** the Step-2/3 meshing-candidate gate uses an EXACT-float anchor inequality (2042),
  so it NEVER elides while moving -> two O(N) walks (2064-2075, 2104-2201) run every frame. Replace
  the float compare with per-anchor CHUNK-coord compare, ORed with the existing dirty signals.
  **CORRECTNESS CONSTRAINT (the contested part):** `get_required_lod_for_chunk` keys on CONTINUOUS
  distance and only surface-band chunks have >=1-chunk hysteresis; AIR/DEEP chunks use full 3D
  distance with NO promotion hysteresis (SHIELD_WorldSystem.cpp:1788-1844), so sub-chunk anchor
  motion CAN flip an off-band chunk's LOD. Therefore this is a meshing-CADENCE change, not strictly
  scan-identical. It is safe ONLY because: (a) mesh + current_lod are hash-EXCLUDED (so no re-pin),
  and (b) `activation_ran_this_tick` re-opens the gate every STREAMING_ACTIVATION_INTERVAL_FRAMES=4,
  bounding LOD-promotion lag to <=4 frames. Accept the bounded visual lag; it does not touch
  collision (heightmap-gated, separate `m_collision_pass_dirty`).
- **Determinism class:** byte-identical for world_hash (render-only outputs). No re-pin.
- **Validation:** `--smoke` stays `b05b642e1299075f`. Eyeball LOD pop on the fly path.
- **Expected ms:** REALISM CAVEAT — the brief claims 20-26ms, but the brief's OWN profiler line
  says stream non-water meshing is ~0.4ms when moving smoothly, and activation already forces the
  scan every 4th frame. Realistic: collapses two O(N=4096) walks from every-frame to once per
  ~16m chunk crossing -> a few ms off the meshing-spike frames, ~sub-1ms steady. NOT 20ms. Adopt
  (cheap, safe), but budget conservatively.

### Step 6 — Cap catch-up ticks to 2 (kills the sim 4x spike multiplier)
- **File/fn:** GameSession ctor construction of `m_simulationClock` (GameSession.h:205 / .cpp:137) —
  pass `max_catch_up=2`. DO NOT edit the SimulationClock.h:19 header constant (broad blast radius +
  breaks SimulationClock_test.cpp:24 which asserts ==4).
- **Approach:** scope the cap to the game clock only. Over-cap ticks are already dropped
  (not replayed), sub-tick remainder preserved.
- **Determinism class:** byte-identical for the oracle (clamp never fires under fixed-dt `--smoke`).
  No re-pin.
- **Validation:** `--smoke` stays green (structurally unaffected).
- **Expected ms:** ~7ms off a genuine 4-tick sim spike (15.9 -> ~8). Zero on the common
  water-bound frames (water is in stream, outside TickSimulation). Targets the OCCASIONAL sim spike.

### Step 7 — Sever float->sim feedback, THEN exclude floats from world_hash (the OPEN-QUESTION fix)
- **File/fn:** WaterSystem.cpp:1119-1130 (resize re-seed), 397 (WaterSource inject);
  WorldPersistenceRoundtrip.cpp:767-775 (exclusion list).
- **Approach (two parts, in order):**
  1. **Sever the edges (byte-identical):** make ResizeSimulationGrid reconstruct
     `water_depth_mm`/`water_bed_mm` by interpolating the prior MM arrays directly (not
     `water_level_data`); route WaterSource discharge through `water_depth_mm` (mm) so the float is
     a pure one-way derivative. (Resize is dead in steady state, so part 1 is byte-identical there;
     validate the save/load resize fixture separately.)
  2. **Exclude (deterministic re-pin):** add `water_level_data`, `water_flow_data`,
     `water_sim_terrain_height` to kRenderMeshHashExcludedFields. Keep
     `water_depth_mm`/`water_bed_mm`/`has_water_sim`/`current_water_resolution`/`is_water_sleeping`/
     sleep counters/`max_water_delta_last_tick`/`water_mesh_dirty_ticks` IN the hash.
- **Determinism class:** part 1 byte-identical; part 2 deterministic-rehash (one-time literal bump).
- **Validation:** build server + `--smoke`; run==replay MUST stay green; **re-pin the
  `b05b642e1299075f` literal once** (local-dev hash bump is allowed per standing note).
- **Expected ms:** ~0 direct. UNLOCKS future render-only float amortization (skip the line-229
  divide for distant/off-screen chunks) WITHOUT re-pins. Lower priority than 1-6; do it to
  correct the hash scope and de-risk, not for frame time.

---

## Cumulative worst-frame budget (conservative, post-measurement)

| After step | Change | Class | Worst-frame effect | Running worst-case |
|---|---|---|---|---|
| 1 | delete dead sim | byte-identical | 0 | 12-30ms (water) / 15.9 (sim) / 21-25 (render) |
| 2 | cache river mask | byte-identical | water -1..4ms | water ~8-26ms |
| 3 | hoist scratch | byte-identical | water -~0.1ms | water ~8-26ms |
| 4 | parallelize kernel | byte-identical | water -0..? (MEASURE; may skip) | water ~6-24ms |
| 5 | anchor gate | byte-identical (render) | meshing-spike -few ms | render ~18-22ms; stream non-water sub-1ms |
| 6 | catch-up cap 2 | byte-identical (oracle) | sim -~7ms on 4-tick | sim spike ~8ms |
| 7 | float hash-exclude | rehash (re-pin) | 0 direct; unlocks amortize | — |

**Does the SAFE set reach <5ms? Honest answer: NO, not on the dominant water-bound frame.**
The verified hot cost in `water=12-30ms` is **water INIT seeding** (per-cell `WaterLevelAt` +
`GetTerrainHeightAt`, WaterSystem.cpp:316-375) plus the integer kernel — and Steps 2-4 only attack
the kernel, not the init burst. To reach <5ms worst-case the REMAINING (post-safe-set) work must
target the init/seed path:

- **Follow-on (recommended next spec):** parallelize/budget the per-cell `WaterLevelAt`/
  `GetTerrainHeightAt` resampling in the init loop (316-375) and cache the seed like Step 2 caches
  the source mask (both are pure seed/param functions). MAX_WATER_INITS_PER_TICK=6 already
  time-slices it, so the spike is the per-cell worldgen sampling cost, not the count. This is where
  the last ~10ms lives.

With Steps 1-6 the OCCASIONAL sim (->~8ms) and render (->~20ms) spikes shrink and the steady water
frame drops a few ms, but the worst water-init frame remains >5ms until the init follow-on lands.
The safe set gets the MOVING-SMOOTH frame well under budget; it does not by itself guarantee the
200fps FLOOR on a water-region-entry frame.

## DO NOT ATTEMPT (rejected / low-ROI / unsafe)

- **Distance-cull the awake water set / camera-gated water sim** (brief water-scheduling lever).
  REJECTED: `chunks_to_sim` feeds the HASHED integer sim; gating on a float camera anchor makes
  hashed `water_depth_mm` camera-dependent -> host!=peer desync (the exact bug Spec 009 Phase 3
  removed, WaterSystem.cpp:1016-1021). `--smoke` (fixed anchor) would NOT catch it. Also breaks
  mass conservation at the cull seam and Spec 010 drainage.
- **Raise/lower MAX_WATER_SIMS_PER_TICK as a perf lever.** Tuning only; raising re-inflates the ms
  it bounds; lowering is a deterministic re-pin that slows hydrology under sustained rain. Not a win.
- **Skip float-mirror regen for off-screen chunks BEFORE Step 7 part 1.** The dirty-tick fields
  (water_mesh_dirty_ticks/water_mesh_generated, lines 232-237) ARE hashed and not in the exclusion
  list; gating them on visibility breaks run==replay. Even after exclusion, the resize re-seed
  (1127) makes a stale float silently corrupt hashed depth. Only safe after Step 7 part 1 severs
  the edge.
- **Halve Aether diffuse sweeps 8->4.** REJECTED: `diffuse_iterations` is a HARD-PINNED gate value
  (validate-engine-frontier.ps1 throws if !=8) and an explicit iteration-6 non-goal; forces a
  re-pin + gate edit + lockstep re-bless for ~0.2ms release in the wrong cost center.
- **Amortize Wind/Weather/Aether on a tick-phase (run 1 of 3).** REJECTED as written: Weather is a
  STATEFUL integrator with exact modular spawn/strike epochs (tick%45, tick%18, both multiples of
  3) — phasing weather off tick%3==0 makes storms NEVER spawn (silent content kill). Wind/Aether
  alone could phase, but the gain (sub-1ms) is not worth the re-pin + the Weather aliasing hazard.
- **Skip field updates when no subscribers.** REJECTED: creates a flag-dependent hash fork
  (subscriber-bearing worlds diverge); the fields are unconditionally hashed; gain targets a
  non-bottleneck.
- **Parallelize the per-cell Wind/Weather/Aether loops.** Byte-identical and safe, but the loops
  are ~10-60µs each; dispatch overhead on the same High lane water uses makes it break-even or
  net-negative. SKIP (low ROI).
- **Incremental wanted-set delta / spatial eviction index / build-once MDI across cascades.**
  REJECTED: target sub-ms, motion-gated, every-4th-tick scans (not worst-frame drivers); the MDI
  "draw the union" variant is a GPU shadow regression (depth-split CSM cascades are disjoint slabs,
  not nested supersets). The anchor gate (Step 5) already captures the real streaming win.
- **`std::move` out of `chunk->mesh_vertices` in the upload path.** UB-adjacent: the snapshot holds
  a const Chunk* to the LIVE buffer re-read every frame. Only the direct-copy-into-pool variant is
  valid (and it's a sub-ms render-submit hygiene win, optional).
- **Static-prop group caching / unordered_map->vector / bind coalescing / water-frustum-cull.**
  All render-only and safe, but each is ~0.2-0.5ms on a ~1ms sub-phase that is NOT the worst-frame
  driver (spec-004 memory: "Don't pursue prop-MDI as a perf lever"). Optional hygiene; do not
  count them toward the floor.
