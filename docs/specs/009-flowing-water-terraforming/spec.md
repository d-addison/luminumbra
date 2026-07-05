# Spec 009 â€” Flowing Water + Terraforming Coupling

Status: Landed (Phases 1-3 shipped; AC-5 dam half gated 2026-07-05 by WaterDeterminism.DamBedEditPoolsWaterDeterministically)
Owner: Lead Architect (water/sim)
Determinism class: HASHED SIM (lockstep host==peer + run==replay; `water_level_data` folded into `world_hash` / `NetworkStateHash`)

---

## Recommended Approach (decision)

**Adopt a fixed-point integer virtual-pipes (Meiâ€“Decaudinâ€“Hu 2007) shallow-water solver on the existing per-chunk 2.5D column grid, with one shared signed integer flux per edge applied with opposite sign to both endpoints, source/sink boundary conditions for rivers, the bed re-sampled on terrain edit, and a dynamic spill level replacing the static worldgen rest-clamp.**

Why this and not the alternatives, weighed against THIS engine's two hard constraints (lockstep determinism on a raw-float-bit hash; streamed chunk world within fixed per-tick budgets):

- **Why virtual-pipes over full SWE (Godunov/Riemann):** Full finite-volume SWE needs Roe/HLLC flux limiters, `sqrt` wave speeds, slope reconstruction and friction source terms â€” a transcendental-and-branch-heavy kernel that is materially harder to make bit-exact in fixed-point and whose stability must be defended per-scheme. The pipe model IS a simplified SWE that keeps the mass-conservation we need (the K outflow-scaling clamp) and drops exactly the parts that fight determinism. The engine already ships a pipe-model hydraulic-erosion kernel (memory "Erosion pillar D complete"), so the machinery is partly in-tree.
- **Why virtual-pipes over cellular-automata (Minecraft/DF levels):** Naive CA either leaks/creates mass (Minecraft infinite source) or needs so many ad-hoc pressure/equalization rules that it reinvents the pipe model worse. The pipe model is itself a principled flux automaton â€” it keeps CA's local-stencil simplicity while gaining structural mass conservation. (We borrow DF's *integer levels* and *evaporation cull* and Barnes priority-flood for instant lake-level/drain equilibrium as an optimization layer in Phase 3, not as the core.)
- **Why fixed-point integers, not floats:** `SHIELD_WorldSystem::debug_water_state_hash()` (`SHIELD_WorldSystem.cpp:2891`) FNV-1a's the **raw IEEE-754 bits** of every `water_level_data`/`water_flow_data` value. host==peer therefore requires *bit-identical* results, not "close enough." The current float sim (`FLOW_CONSTANT*diff`, `glm::mix`, `glm::length`) survives only because every peer runs the identical FP op order on the identical binary; the moment two peers run different builds (FMA/fast-math/`atan2f` divergence â€” Box2D 2024) it desyncs. A mass-conserving sim *accumulates* state every tick, so float rounding compounds. Fixed-point gives **exact** mass conservation (the same integer subtracted from one cell and added to its neighbour) and removes the float-order class of bugs structurally. This matches in-repo conventions: `ScentField.h` ("deterministic: fixed traversal order, no RNG/wall-clock/libm transcendentals"), HydraulicErosion int64 floor-div, `PlayerAvatar` mm quantization.
- **Why it satisfies terraforming for free:** Flux each tick reads only the *current* bed `b` and depth `d` via the surface gradient `Î”(b+d)`. Raise `b` (dam) â†’ `Î”` flips â†’ flow stops and water pools. Lower `b` (dig a channel) â†’ `Î”` drives flux down the new gradient and the body drains. No special-cased "drain" code path â€” draining IS the solver running on the edited bed, once we (a) re-sample the bed on edit and (b) drop the static rest-clamp that currently pins water at the worldgen surface.
- **Why it scales on the streamed world:** The stencil is 4-neighbour and local, so a chunk simulates from its own cells plus a 1-cell halo â€” the `WaterSimNeighbors` north/south/east/west plumbing (`WaterSystem.h:131-136`) already exists. Sleeping (`is_water_sleeping`), the deterministic rotating sim/init/resize budgets (`m_water_sim_cursor`, `MAX_WATER_*_PER_TICK`), and adaptive LOD (`WaterDetailLevel`) all carry over unchanged.

---

## Context

`src/luminumbra_common/systems/WaterSystem.cpp` is a per-chunk 2.5D height-field **equalizer**: each cell holds a water surface `h`; each tick it moves `diff*FLOW_CONSTANT` toward lower neighbours, clamped to `water_depth*0.2`, and post-sim **pins** every cell to its worldgen `water_rest_level` (`WaterSystem.cpp:418-427`). Cross-chunk reads use `sample_neighbor_edge` against a neighbour snapshot (`WaterSystem.cpp:453-481`). Rivers are a **worldgen carve only** (`RiverInfluenceAt`, `SHIELD_WorldSystem.cpp:1403-1431`): channels are cut into terrain but `WaterLevelAt` (`SHIELD_WorldSystem.cpp:913-922`) returns only `max(SEA_LEVEL, lake_surface)` and never integrates the carve, and no entity seeds water into the channels. Net effect:

- **Rivers render dry** â€” carved channels sit below sea level but water stays at sea level, and the rest-clamp forbids draining the channel below it.
- **No mass conservation / no true cross-chunk flux** â€” each cell writes its new height independently from a read-only snapshot; A's outflow toward a frozen neighbour B is never received by B, so mass leaks/duplicates at seams, and the rest-clamp silently *creates* mass.
- **No terraform coupling** â€” `water_sim_terrain_height` is sampled once at init/resize (`WaterSystem.cpp:194`) and never re-read after a runtime SDF edit; the bed is frozen at worldgen.
- **Determinism is incidental, not structural** â€” all-float on a raw-float-bit hash; the live `WaterDeterminism` gate (`test/common/WaterDeterminism_test.cpp`) only proves run==replay on one binary, never host==peer cross-build.

Persistence/hash contract (must be preserved): `water_level_data` / `water_flow_data` / `water_sim_terrain_height` are persisted fields folded into `WorldStreamingStateSubHashes.water` (`WorldPersistenceRoundtrip.h:106`) and into `NetworkStateHash` (`NetworkStateHash.cpp:29-37`). The water sim runs on **both** server and client via `SHIELD_WorldSystem::update() -> WaterSystem::update()`.

---

## Goals

1. Rivers **carry water**: deterministic springs/sources inject discharge at worldgen river/spring cells; the solver routes it downhill to sea/lake/edge **sinks**, conserving mass; carved channels fill instead of rendering dry; waterfalls connect to real flowing water (a plunge is just a large-`Î”h` edge).
2. Water **fills and drains**: a body rises to fill a basin and lowers when its outlet/spill changes.
3. **Terraform re-routes / drains / dams**: editing the voxel bed re-samples the bed for affected cells and wakes them; the next tick's flux re-routes (dig channel â†’ drain; raise wall â†’ pool) with no special drain path.
4. **Deterministic under lockstep**: host==peer AND run==replay hold with the hashed water state in **fixed-point integers**; exact integer mass conservation; no libm transcendentals in the hashed path.
5. **Scales across the streamed chunk world**: cross-chunk flux continuity, sleeping for dormant water, deterministic per-tick budgets, and LOD for far water â€” all within existing per-tick budgets.

---

## Non-Goals

- Full 3D voxel water (caves/overhangs, multi-Z stacking). Stays 2.5D height-field this spec; multi-layer is a follow-up.
- Full finite-volume SWE physics (hydraulic jumps, supercritical flow, true momentum advection). Pipe-model fidelity only.
- Pressure-projection / Poisson-solve 3D fluids. Rejected (determinism + budget).
- Sediment transport / live erosion of the bed by the water field (the Mei erosion coupling). The flux field is *designed* to later drive erosion, but erosion stays gated OFF and out of scope here.
- A general runtime terraforming UX/tooling surface. We add the **minimal** editâ†’bed-resampleâ†’wake notification path needed for the dig-to-drain acceptance test; full player terraforming tools are a separate spec.
- Changing the rendering/meshing of water beyond reading the new fixed-point height (existing `water_mesh_dirty_ticks` coalescing reused).

---

## Functional Requirements

- **FR-1 (sources):** Worldgen river/spring cells emit a deterministic fixed-point discharge (mm-volume/tick) into their cell each tick. Discharge is a pure function of position (and, where used, tick) â€” no RNG, no wall-clock. `WaterSourceComponent` emitters are quantized to integer discharge.
- **FR-2 (downhill flow + conservation):** Water injected at a source routes downhill cell-to-cell to a sink via the pipe solver, conserving total volume exactly between source inflow and sink outflow (integer mass invariant: `Î£ depth` changes only by `Î£(source âˆ’ sink)` per tick).
- **FR-3 (sinks):** Sea-level cells, map/loaded-region edge cells, and lake outlets act as sinks that absorb water down to their target level. Deterministic, integer.
- **FR-4 (fill + drain):** A basin fills to its spill level from inflow; when the spill/outlet lowers, the body drains toward the new level.
- **FR-5 (terraform re-route):** A runtime bed edit in cell(s) re-samples `water_sim_terrain_height` for the affected cells, marks water dirty, and wakes the chunk + neighbours; the next tick's flux re-routes over the edited bed (dig â†’ drain along the new channel; raise â†’ pool behind the new wall).
- **FR-6 (waterfalls connect):** A large surface drop across an edge produces flux into the lower cell (the existing `WaterfallDetect` visual triggers off live water, not static terrain), so a waterfall visually connects upstream flowing water â†’ sheet â†’ plunge pool.
- **FR-7 (lake vs river):** Perched closed basins retain a *dynamic* spill level (recomputed on rim edits) instead of the frozen worldgen rest-clamp; sourced river cells are not pinned and drain freely.

---

## Non-Functional Requirements

### NFR-DET â€” Determinism (hard constraint)

- **NFR-DET-1:** Hashed water state (depth + bed) carried as **fixed-point integers** â€” depth and bed in **millimetres, `int32`**; per-edge flux as **`int32` mm-volume/tick**; per-cell/per-edge sums widened to **`int64`**. Tuning constants are fixed-point integers. (mm matches `PlayerAvatar` quantization and is human-readable.)
- **NFR-DET-2:** **One shared signed flux per edge, computed once**, applied with opposite sign to both endpoints (`depth[P] -= q; depth[N] += q;`). Because it is the *same integer*, global mass is exact and **order-independent** regardless of iteration order. This is the single biggest determinism win over floats.
- **NFR-DET-3:** **Two-phase, double-buffered** update: phase 1 computes all edge fluxes from a read-only snapshot of tick-N state; phase 2 applies them. No in-place Gauss-Seidel. Order-independent and parallel-safe (we still run inline per `WaterSystem.cpp:412`).
- **NFR-DET-4:** **No libm transcendentals in the hashed path** â€” only `+ âˆ’ Ã— Ã·` and arithmetic shifts on integers; the only divides are the K outflow-scaling clamp and the dV/area term, with a **fixed rounding mode** (truncate toward zero), and any floor-divide remainder stays in the source cell (mass-conserving).
- **NFR-DET-5:** **Velocity / `water_flow_data` leaves the hash.** Velocity is render-only, derived at read time in `get_water_flow_at` from integer flux/depth, kept float, and **excluded** from `debug_water_state_hash`. (Removes a pure-desync-risk float field hashed today for zero gameplay reason.)
- **NFR-DET-6:** Fixed iteration order: cells row-major; chunks/edges in sorted-`ChunkID` order (reuse the existing chunk-id sort and `m_water_sim_cursor` window). Cross-chunk shared edge resolved by a deterministic owner (**lower `ChunkID` owns the shared edge**) so flux is computed once and applied to both sides â€” no double-integration, no snapshot staleness.
- **NFR-DET-7:** Sources/sinks are deterministic functions of position+tick; no RNG/wall-clock.

### NFR-PERF â€” Performance / streaming

- **NFR-PERF-1:** Stay within the existing per-tick budgets â€” `MAX_WATER_INITS_PER_TICK=6`, `MAX_WATER_SIMS_PER_TICK=64`, `MAX_WATER_RESIZES_PER_TICK=1` (`WaterSystem.cpp:29/36/42`) â€” via the deterministic rotating window. The pipe step is ~3 cheap passes/cell; no regression vs current equalizer.
- **NFR-PERF-2:** **Sleeping** with integers: a chunk sleeps when max `|Î”depth|` this tick `== 0` (exact, cleaner than a float epsilon) AND all 4 boundary fluxes are 0 AND no source/edit touched it; wakes on neighbour boundary flux, source emit, `apply_displacement`, or a terrain edit. Settled oceans/lakes sleep permanently until disturbed (ocean-scale water is free).
- **NFR-PERF-3:** Overflow discipline: depth/bed `int32` mm; per-edge and per-cell sums `int64`; never multiply two fixed-point values without widening to `int64` then shifting back. A 1024-cell chunk Ã— max depth stays well under 2^63.

### NFR-CONT â€” Cross-chunk continuity + streaming

- **NFR-CONT-1:** Cross-chunk flow uses the owner-edge rule (NFR-DET-6): the lower-`ChunkID` chunk computes the shared-edge flux into a per-edge buffer; both chunks apply it. Conservation holds across the seam.
- **NFR-CONT-2:** **Streaming frontier:** a loaded chunk whose neighbour is unloaded treats the missing edge as a fixed boundary at the worldgen rest/river level (or no-flux wall for closed beds), and **queues** flux destined for an unloaded chunk as a small persisted per-edge "pending inflow" applied when that chunk loads â€” a river crossing the load frontier loses/duplicates no water.
- **NFR-CONT-3:** LOD: far/calm water becomes a static settled level (reuse `WaterLevelAt`/dynamic spill) and only wakes on edit; when downsampling, conserve mass by integer area-weighted sum (power-of-two ratios â†’ exact integer division), and derive the coarse bed by averaging the fine voxel-derived heights (NOT re-sampling the analytic heightmap â€” per memory "Coarse LOD ignores SDF").

---

## Acceptance Criteria

- **AC-1 (river flows, testable):** In a river-bearing preset, after N ticks a sampled river-channel cell holds water depth `> 0` (it is dry today). A cell upstream and a cell downstream both carry water; flow vector points downhill.
- **AC-2 (mass conservation invariant):** Extend the `WaterDeterminism` gate with a **global integer mass invariant**: over a tick, `Î£ depth` over all chunks changes by exactly `Î£(source âˆ’ sink)` (bit-exact integer). Fails on any seam leak or non-deterministic rounding.
- **AC-3 (run==replay):** Existing live `WaterDeterminism_test.cpp` stays green: two runs with same seed + anchor path produce identical per-tick `debug_water_state_hash`, now over the fixed-point depth+bed (flow excluded).
- **AC-4 (host==peer, NEW):** Add a two-build / two-config host==peer hash-compare: the same scenario ticked under a determinism-stress build (e.g. fast-math/FMA-flag-flipped variant) must produce the **identical** water-state hash. (Catches the class the current gate cannot.)
- **AC-5 (dig-to-drain, NEW):** A scripted test fills a small basin, then issues a runtime bed edit carving a channel through its rim to a lower outlet; within a bounded tick count the basin's sampled depth drops toward the new outlet level (was impossible under the rest-clamp), and the global mass invariant (AC-2) holds throughout. A dam test (raise bed) shows water pooling behind the new wall.
- **AC-6 (waterfall connects):** A scenario with a flowing river over a cliff shows non-zero water depth on both the upper channel and the lower plunge-pool cell, with `WaterfallDetect` triggering off the live water surface.
- **AC-7 (budget):** Per-tick water cost stays within the existing init/sim/resize budgets; no new main-thread stall (verified against the moving-lag profiler scenario).
- **AC-8 (persistence roundtrip):** Fixed-point water state round-trips byte-identically through save/load (the integer encoding removes the last-ULP text-serialization risk noted for floats); world_hash re-pinned to the new baseline (local-dev bumps OK per memory).

---

## Phased Implementation Plan (vertical-slice first)

### Phase 1 â€” Slice 1: ONE river carries fixed-point flowing water deterministically

Smallest end-to-end that makes a single river flow sourceâ†’sink so a waterfall connects, in fixed-point, hash-deterministic.

- **Files to touch:**
  - `src/luminumbra_common/world/Chunk.h:99-118` â€” add fixed-point mirrors (`water_depth_mm`, `water_bed_mm`) alongside (or replacing, see Slice-1) the float arrays; keep float `water_flow_data` render-only.
  - `src/luminumbra_common/systems/WaterSystem.cpp:18-20` â€” replace float constants with fixed-point `K_ACCEL`, `MIN_FLOW_MM`, friction shift.
  - `src/luminumbra_common/systems/WaterSystem.cpp:182-196` â€” seed depth/bed in mm from `WaterLevelAt`/`GetTerrainHeightAt` (Ã— `MM_PER_M`, integer-rounded).
  - `src/luminumbra_common/systems/WaterSystem.cpp:203-228` â€” integer source discharge; add worldgen river-spring source seeding (sample `RiverInfluenceAt`, `SHIELD_WorldSystem.cpp:1403-1431`).
  - `src/luminumbra_common/systems/WaterSystem.cpp:440-544` â€” replace the equalizer body with the integer two-phase pipe step (flux â†’ K-clamp â†’ apply).
  - `src/luminumbra_common/systems/WaterSystem.cpp:416-437` â€” **remove the unconditional rest-clamp** for sourced/river cells (keep only for closed-basin spill, Phase 3).
  - `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:2891-2914` â€” hash fixed-point depth+bed (sorted chunk-id, FNV-1a over `int32` bits); **drop flow** from the hash.
  - `test/common/WaterDeterminism_test.cpp` â€” keep run==replay; add the mass invariant (AC-2).
- **Algorithm step:** Mei pipe model, single-chunk-internal first (4-neighbour, edges fully inside one chunk), no cross-chunk flux yet â€” neighbour edges fall back to a fixed boundary at rest level. Source cells `+= discharge`; sea/edge cells clamped to sink level.
- **Determinism approach:** all-integer, one shared edge flux, two-phase double-buffer, fixed rounding; flow out of hash. (Full NFR-DET set.)
- **Validation/gate:** AC-1 (river wet), AC-2 (mass invariant), AC-3 (run==replay green), AC-6 partial (waterfall over a single-chunk cliff). Re-pin world_hash baseline.

### Phase 2 â€” Terraform coupling (dig-to-drain / dam)

- **Files to touch:**
  - `src/luminumbra_common/world/Chunk.h:41-45` â€” reuse `mark_voxel_data_dirty`; add a water-bed-dirty signal.
  - `src/luminumbra_common/systems/WaterSystem.{h,cpp}` â€” add `NotifyTerrainEdit(chunk_id, bounds)` â†’ re-sample `water_sim_terrain_height`/`water_bed_mm` for affected cells (the dirty AABB + 1 ring), wake chunk + neighbours (extend `apply_displacement` wake path, `WaterSystem.cpp:635-696`).
  - Add a minimal `SetTerrainVoxelAt(world_pos, density)` edit API wrapping `sdf_data` edit + `mark_voxel_data_dirty()` + `NotifyTerrainEdit` (the dig-to-drain test driver; integer/quantized displacement from baseline for determinism).
  - Replace the static rest-clamp with a **dynamic spill level** per closed basin (lake cells only); river cells unpinned.
- **Algorithm step:** edit lowers/raises `b` â†’ next-tick flux re-routes (terraform coupling is intrinsic to the pipe model). Draining is the solver on the edited bed.
- **Determinism approach:** edits routed through the deterministic tick (not render thread); integer bed displacement; re-sample is a pure function of the edited SDF.
- **Validation/gate:** AC-5 (dig-to-drain + dam), mass invariant holds throughout the edit.

### Phase 3 â€” Cross-chunk scale + LOD + instant-equilibrium

- **Files to touch:**
  - `src/luminumbra_common/systems/WaterSystem.cpp:315-355` (snapshot build), `:453-481` (`sample_neighbor_edge`) â€” replace "drain toward frozen snapshot" with the **owner-edge shared-flux** exchange (NFR-DET-6/CONT-1).
  - `:700-816` (adaptive LOD) â€” integer area-weighted mass-conserving resample; coarse bed from averaged fine heights.
  - Streaming frontier: persisted per-edge pending-inflow (NFR-CONT-2); persist dynamic spill per basin.
  - Optional: local integer **priority-flood** (Barnes 2014) to set basin spill/drain equilibrium instantly on a rim breach, while the pipe solver animates the transient (hybrid).
- **Determinism approach:** sorted-id edge order; integer area-weighted downsample (power-of-two); pending-inflow persisted in `world_hash`.
- **Validation/gate:** AC-4 (host==peer two-build), AC-7 (budget), AC-8 (persistence roundtrip), continuity across a moving anchor over a river crossing chunk seams (extend the live `WaterDeterminism` river scenario).

---

## Slice-1 Design (implementation-ready)

### State fields (`Chunk.h`, alongside the existing water arrays)

```cpp
// --- Fixed-point flowing-water state (hashed; mm, deterministic) ---
std::vector<i32> water_depth_mm;   // water depth above bed, millimetres (>= 0). size = resolution^2
std::vector<i32> water_bed_mm;     // terrain bed height, millimetres. re-sampled on edit (Phase 2)
// water_flow_data stays float and RENDER-ONLY; NO LONGER hashed.
```

Surface height = `water_bed_mm[i] + water_depth_mm[i]` (mm). `MM_PER_M = 1000`. Keep `water_level_data` float only as a derived render mirror if the mesher needs it (regenerated from mm; not hashed).

### Constants (fixed-point, `WaterSystem.cpp`)

```cpp
constexpr i64 MM_PER_M     = 1000;
constexpr i32 MIN_FLOW_MM  = 1;        // sub-mm surface diffs produce no flux (kills limit-cycle jitter)
// K_ACCEL folds dt*A*g/L into one fixed-point gain with a shift. Tuned so worst-case
// wave stays sub-CFL at 30Hz on the coarsest cell; pick FLOW_SHIFT a power of two.
constexpr i64 K_ACCEL      = /* tuned */;   // applied as (K_ACCEL * dSurf_mm) >> FLOW_SHIFT
constexpr int FLOW_SHIFT   = 12;
constexpr i32 FRICTION_NUM = 250, FRICTION_SHIFT = 8;   // q = (q*FRICTION_NUM)>>FRICTION_SHIFT damping
```

### Per-tick update (integer two-phase, per chunk, internal edges only in Slice-1)

Snapshot of tick-N `water_depth_mm`/`water_bed_mm` is read-only (already the pattern). For each cell compute, then apply.

**Phase 0 â€” sources/sinks (deterministic):**
- For each river/spring source cell (from `RiverInfluenceAt` â‰¥ threshold, or a `WaterSourceComponent`): `depth_new[i] += discharge_mm` where `discharge_mm = (flow_rate_mm3_per_tick) / cell_area_mm2` (integer divide, remainder retained in an accumulator field for exactness â€” optional Slice-1, else floor).
- For each sink cell (sea/edge): after apply, clamp `depth` so surface â‰¤ sink level.

**Phase 1 â€” flux (compute from snapshot, never write depth here):** iterate only `+X` and `+Z` edges so each edge is visited once. For edge between `P` and neighbour `N`:
```
dSurf = (bed[P]+depth[P]) - (bed[N]+depth[N])          // i32 (mm)
q_prev = edge_flux[P,dir]                               // i32 (mm-vol/tick), persisted per edge
q = q_prev + (i32)((K_ACCEL * dSurf) >> FLOW_SHIFT)     // widen to i64 for the mul
q = (i32)(((i64)q * FRICTION_NUM) >> FRICTION_SHIFT)    // integer damping
if (abs(dSurf) < MIN_FLOW_MM) q = 0;
edge_flux[P,dir] = q;                                   // signed: +q drains P->N, -q drains N->P
```

**Phase 2 â€” K outflow-scaling clamp (per cell, integer):** for each cell sum its **outgoing** edge fluxes `Sigma_out` (edges where the signed flux drains this cell). If `Sigma_out > depth[cell]` (the available volume in mm-as-volume units), scale every outgoing flux of that cell by the same integer ratio:
```
q_scaled = (i32)(((i64)q * depth[cell]) / Sigma_out)   // floor; remainder stays in cell -> mass-exact
```
This guarantees `depth` never goes negative and total volume is conserved.

**Phase 3 â€” apply (symmetric, order-independent):** for each edge with final `q`:
```
depth[P] -= q;  depth[N] += q;     // same integer => exact conservation regardless of order
```
Clamp `depth[i] = max(depth[i], 0)` (should already hold after the K-clamp; defensive). Apply evaporation cull: `if depth[i] < EVAP_MM depth[i] = 0` only for non-source non-basin cells (kills thin films deterministically).

**Velocity (render-only, NOT hashed):** in `get_water_flow_at`, `v = net_edge_flux / (cell_width_mm * max(depth,1))` as float at read time.

### Seeding sources (Phase 0 wiring)

At init (`WaterSystem.cpp:182-196`), in addition to seeding `depth/bed` in mm: query `RiverInfluenceAt(world_x, world_z)` (`SHIELD_WorldSystem.cpp:1403-1431`); where influence â‰¥ threshold, register the cell as a deterministic source with `discharge_mm` proportional to a catchment estimate (integer). Sea/edge cells flagged as sinks. This is what finally makes carved channels fill.

### Integration point in `WaterSystem`

The two-phase step replaces `simulate_chunk_water` (`WaterSystem.cpp:440-544`) body; it still runs **inline** in the loop at `WaterSystem.cpp:412-414` (no JobSystem dispatch â€” preserves the head-of-line-blocking fix). The per-edge `edge_flux` buffer lives per chunk (a `std::vector<i32>` of size `2*resolution^2` for the +X/+Z edges) and is part of the snapshot/output so apply-order cannot matter. Remove the rest-clamp block (`WaterSystem.cpp:416-437`) for river/sourced cells (Phase 3 reintroduces a dynamic spill clamp for closed basins only).

### How it stays hash-deterministic

- `debug_water_state_hash` (`SHIELD_WorldSystem.cpp:2891-2914`) now FNV-1a's the `int32` bits of `water_depth_mm` and `water_bed_mm` (sorted chunk-id, row-major), and **no longer hashes** `water_flow_data`. Integer bits are identical across compilers/CPUs â†’ host==peer.
- Mass is exact because every edge flux is one shared `int32` subtracted from `P` and added to `N`; `Î£ depth` over all cells changes only by source/sink deltas. The AC-2 invariant asserts this each tick.
- No libm in the hashed path; the only divides (K-clamp, discharge/area) truncate toward zero with remainder retained in the source cell. Fixed row-major / sorted-chunk-id iteration. Sources/sinks pure functions of position+tick.

---

## Risks & Mitigations

- **CFL blow-up on tall waterfall drops:** bound `K_ACCEL` for the coarsest cell; if a single 30Hz step is too coarse, sub-step the water pass N times/tick (still deterministic). The K-clamp prevents negative-depth instability regardless.
- **4-neighbour grid anisotropy ("diamond front"):** acceptable for Slice-1; if visually blocky, add 8-neighbour pipes (still integer) in a later phase.
- **world_hash re-pin churn:** expected; local-dev bumps are sanctioned (memory "Local-dev world_hash bumps OK"). Re-pin baseline literals after Slice-1 and after each phase.
- **Coarse-LOD bed mismatch:** derive coarse bed from averaged fine voxel heights, never the analytic heightmap (memory "Coarse LOD ignores SDF").
