# Pillar audit: Water — specs 009 flowing water/terraforming + 010 finite hydrology (Spec 021, 2026-07-02)

**Verdict:** The water pillar is in the strongest shape of its life: spec 009's fixed-point integer
virtual-pipes solver is fully landed through Phase 3 (cross-chunk owner-edge flux, terraform
coupling, camera-independent grid), spec 010 finite hydrology (rain fills / evaporation empties /
drained-stays-drained) is landed and test-gated, the 2026-06-25 lockstep desync is resolved with a
0-flake record, and the old ~450 ms/~1500 ms water main-thread block is dead and locked in by a
regression gate. What remains is wiring and hygiene, not core engineering: rain is still driven by
hardcoded demo values instead of `WeatherSystem::PrecipitationAt`; two float→sim feedback edges keep
the redundant float mirrors inside `world_hash`; the water sub-hash localization group omits the
authoritative mm arrays; waterfalls are still a static-worldgen phenomenon that cannot react to live
water or terraform; a night-lighting shader fix sits **uncommitted** in the working tree with its
C++ half already at HEAD; and spec 010 shipped without a spec document (the docs/specs listing jumps
009→011). Several KNOWN-CONTEXT facts from prior sessions are stale and are corrected below against
the tree.

## Current state + evidence

### The spec 009 fixed-point solver (landed, Phases 1–3)

- The live sim is the integer Mei virtual-pipes kernel `StepChunkWaterFixed`
  (`src/luminumbra_common/systems/WaterSystem.cpp:112-257`): Phase 0 deterministic river sources via
  a cached per-cell mask (`WaterSystem.cpp:146-162`), Phase 1 `+X/+Z` edge flux from a read-only
  surface snapshot (`WaterSystem.cpp:171-190`), Phase 2 the K outflow-clamp (order-independent,
  mass-exact, `WaterSystem.cpp:192-216`), Phase 3 symmetric apply (`WaterSystem.cpp:218-223`), then
  sea sink / evaporation / float render-mirror regen (`WaterSystem.cpp:225-248`). Constants are all
  fixed-point (`WaterSystem.cpp:51-58`).
- **Cross-chunk continuity (Phase 3)** is a sequential owner-edge shared-flux seam pass after the
  per-chunk step: each chunk owns its `+X`/`+Z` boundary edges, computes the same pipe flux against
  the neighbour, clamps to the source cell's depth and applies symmetrically
  (`WaterSystem.cpp:557-607`), with a `seam_wet_pairs` continuity proof counter
  (`WaterSystem.cpp:564`, exposed at `WaterSystem.h:167`).
- **Hashed state is integer-only**: `debug_water_state_hash` FNV-1a's the raw `int32` bits of
  `water_depth_mm` + `water_bed_mm` in sorted-chunk-id order and explicitly no longer hashes the
  float arrays (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:3088-3111`). The mm arrays
  live on the chunk (`src/luminumbra_common/world/Chunk.h:112-116`) alongside the transient
  `water_edge_flux` and the non-persisted source mask `water_src_mm` (`Chunk.h:116,121`).
- **The sim grid is camera-independent and uniform** — `CalculateRequiredDetail` is hardwired to the
  single fixed `WATER_SIM_RESOLUTION` (Medium 8×8, 4 m cells) because cross-chunk flux requires
  cell-aligned seams and host==peer forbids camera-driven hashed state (`WaterSystem.cpp:60-69`,
  `WaterSystem.cpp:834-842`).
- **Mass invariant (AC-2)** is asserted live: `Σdepth` change must equal `Σsource − Σsink` each tick
  (`WaterSystem.cpp:547-616`), surfaced via `dbg_mass_ok` (`WaterSystem.h:159-172`) and checked
  every tick by the gate test (`test/common/WaterDeterminism_test.cpp:87-90`).
- **Terraform coupling (Phase 2)** is landed twice over: the bed-level edit `EditTerrainBed`
  (`WaterSystem.cpp:796-830`, delegated at `SHIELD_WorldSystem.cpp:3296-3298`) and the player-facing
  voxel dig/fill `EditTerrainVoxel`, which carves `sdf_data`, remeshes, and couples the water bed
  (dig drains / fill dams) (`SHIELD_WorldSystem.cpp:3320-3386`, water couple at
  `SHIELD_WorldSystem.cpp:3379-3383`).
- `WaterSystem::update` runs inside the deterministic tick from `SHIELD_WorldSystem::update`
  (`SHIELD_WorldSystem.cpp:2135-2140`), with deterministic rotating per-tick budgets:
  `MAX_WATER_INITS_PER_TICK=6`, `MAX_WATER_SIMS_PER_TICK=64`, `MAX_WATER_RESIZES_PER_TICK=1`
  (`WaterSystem.cpp:31-44`), id-sorted rotating sim window (`WaterSystem.cpp:518-536`).
- **Spec doc drift:** `docs/specs/009-flowing-water-terraforming/spec.md:3` still says
  `Status: Draft` although Phases 1–3 shipped (commit `7a780279`).

### Spec 010 finite hydrology (landed, default-OFF, demo-wired only)

- `SetHydrology(finite, rain_mm_per_tick, evap_mm_per_tick)` on the WaterSystem
  (`WaterSystem.h:101-109`), exposed as `SHIELD_WorldSystem::SetWaterHydrology`
  (`SHIELD_WorldSystem.cpp:3300-3303`). Defaults `(false,0,0)` preserve classic spec-009 behaviour so
  all baselines stay green (`WaterSystem.h:150-152`).
- `finite==true` removes the perpetual river source (`WaterSystem.cpp:137-162`), rain is a uniform
  deterministic integer input carried to low spots by the flux (`WaterSystem.cpp:163-169`), and
  above-sea standing water evaporates `evap_mm`/tick (`WaterSystem.cpp:234-243`). Rain keeps
  otherwise-sleeping chunks awake so puddles form on dry land (`WaterSystem.cpp:508-516`).
- The **land-water probe** (KNOWN CONTEXT verified): `debug_land_water_volume_mm` sums depth only
  over cells whose bed sits >0.5 m above sea, isolating rain-fed water from the sea-pinned max-depth
  probe (`SHIELD_WorldSystem.cpp:3155-3168`); the gate compares a rain run against a dry run on the
  identical world (`test/common/WaterDeterminism_test.cpp:286-298`).
- **Wiring gap (KNOWN CONTEXT confirmed still open):** the only `SetWaterHydrology` call sites are
  the rain/drain timelapse demo paths with hardcoded values
  (`src/luminumbra_client/main_client.cpp:5734`, `main_client.cpp:9625`). Rain is NOT driven from
  `WeatherSystem::PrecipitationAt` (`src/luminumbra_common/systems/WeatherSystem.cpp:501`), there is
  no SystemConfig entry, and the hydrology config is not persisted with the world.
- **Docs defect:** there is no `docs/specs/010-*` directory — the specs listing jumps
  `009-flowing-water-terraforming` → `011-living-creatures-daily-life` (verified by directory
  listing; the audit charter itself notes "010 skipped",
  `docs/specs/021-engine-framework-audit-charter/spec.md:17`) even though the feature shipped
  (commit `524a5557`).

### Determinism record (lockstep desync RESOLVED — KNOWN CONTEXT verified)

- `docs/water-sim-lockstep-determinism.md:3` records the resolution: bed-value `current_lod==0`
  gate (`7d605399`, live at `WaterSystem.cpp:371-381`), boot water-settle interim C (`254cec1f`,
  0 flakes/24, canonical hash moved `b05b642e → 6f008a9f`,
  `docs/water-sim-lockstep-determinism.md:20-27`), and the B′ streaming rewrite **verified
  unnecessary** by the `--smoke-moving` harness (0 flakes/22,
  `docs/water-sim-lockstep-determinism.md:9-16`; flag at
  `src/luminumbra_server/main_server.cpp:284`).
- Water is the named template for the 018-B residency contract ("streamed-system residency pattern
  (water is the template)", `docs/determinism-residency-contract.md:150-155`), with finite hydrology
  listed as an inheritor of the pattern.
- The live gate is 4 ctests in `common_tests` (`test/CMakeLists.txt:122`, discovered at
  `test/CMakeLists.txt:952`): run==replay + AC-1 river-fills + AC-2 mass + Phase-3 seam continuity
  (`test/common/WaterDeterminism_test.cpp:110-138`), terraform bed-edit dig
  (`WaterDeterminism_test.cpp:178-192`), player voxel dig
  (`WaterDeterminism_test.cpp:235-247`), finite-hydrology rain
  (`WaterDeterminism_test.cpp:286-298`).

### Performance record (water-perf-200fps — KNOWN CONTEXT **stale**, corrected)

- The memory claim "#1 remaining moving-lag killer = water-sim main-thread wait() (~450 ms)" is
  **stale**. The tree shows: the dead float-mirror job sim was deleted (`WaterSystem.h:132-134`
  removal note; commit `9a7360f2`), the sim runs INLINE over a deterministic rotating 64-chunk
  window (`WaterSystem.cpp:32-38`, `WaterSystem.cpp:518-536`), init seeding is inline (no
  dispatch+wait; `WaterSystem.cpp:439-448`) and reuses the mesher heightmap byte-identically at LOD0
  (`WaterSystem.cpp:352-393`), and the river-source mask is cached (`WaterSystem.cpp:146-157`).
- The win is **locked in by a regression gate**: `.forge/scripts/validate-moving-hitch.ps1:3-7`
  ("water-sim head-of-line block (was ~1500ms)") with a steady-state water ceiling of 220 ms
  (`validate-moving-hitch.ps1:33`) — the pillar's moving-lag item is DONE, not open.
- `docs/specs/water-perf-200fps.md` records the honest ground truth: Steps 1–3 landed (commit
  `9a7360f2`), the init follow-on landed (`43e3c7bf`, `07cc9f5f`, `896bd45c`), Step 6 catch-up cap
  landed (`43567963`). **Step 7 (sever float→sim edges, exclude float mirrors from world_hash) has
  NOT landed** — see Gaps.

### Waterfalls (KNOWN CONTEXT **stale**, corrected)

- The memory claim "WaterfallDetect does river+drop but misses lake outlets + the connection
  guarantee" is **stale**: commit `eaaf106b` (2026-06-21) added perched-lake/tarn rim-spill outlet
  detection (`src/luminumbra_client/rendering/WaterfallDetect.cpp:145-242`, params at
  `WaterfallDetect.h:80-89`) and the upstream/downstream surface-connection pass that pins the crest
  to the upstream water surface and raises the foot to the downstream surface
  (`WaterfallDetect.cpp:248-265`).
- What remains deferred, by explicit in-code note: creating a plunge POOL where the foot is dry is a
  sim/world_hash-affecting change and was deliberately excluded (`WaterfallDetect.cpp:255-257`).
- Sites are a pure function of static worldgen (`RiverInfluenceAt`/`GetTerrainHeightAt`/
  `WaterLevelAt`, `WaterfallDetect.cpp:65-70,162`) cached per (seed, window)
  (`WaterfallDetect.h:125-140`) — so waterfalls can never respond to LIVE water or terraform (spec
  009 AC-6's "WaterfallDetect triggering off the live water surface",
  `docs/specs/009-flowing-water-terraforming/spec.md:102`, is unmet).
- Gate: `waterfall_visual_test` (`test/rendering/waterfall_visual_test.cpp:1-18`), run as
  `WaterfallVisualTest` (`.forge/scripts/verify-waveb-ocean.ps1:15`).
- **Stranded uncommitted work:** the night-lighting fix is split across a commit boundary. The C++
  half is at HEAD (committed in `7a780279`): the pipeline sets `u_scene_light` from sun intensity
  (`src/luminumbra_client/rendering/RenderPipeline.cpp:2424-2429`). The shader half — the
  `uniform vec3 u_scene_light = vec3(1.0)` declaration and its use — exists ONLY as an uncommitted
  working-tree diff (`res/shaders/waterfall.frag:35,99`; `git status` shows `M
  res/shaders/waterfall.frag`, +14/−6). At the committed HEAD the uniform set is a silent GL no-op,
  so waterfalls still emit near-white at night. Visual confirmation of the fix is blocked by the
  headless IN_GAME render-capture hang (RENDER pillar owns that blocker).

### Render side

- `WaterPass` (spec 016 pass-contract seam, caustics generation, SSR sky fallback):
  `src/luminumbra_client/rendering/passes/WaterPass.h:29-71`, executed with GPU timing from
  `RenderPipeline.cpp:2387-2391`; waterfall sheets draw after water into the lit FBO
  (`RenderPipeline.cpp:2395-2449`).

### Persistence / hash scope

- All five water arrays (float `water_level_data`/`water_flow_data`/`water_sim_terrain_height` +
  integer `water_depth_mm`/`water_bed_mm`) are serialized by `ChunkToJson`
  (`src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp:259-273`) and none of them are
  in `kRenderMeshHashExcludedFields` (`WorldPersistenceRoundtrip.cpp:767-775`) — i.e. the float
  mirrors are still redundantly inside `world_hash` (water-perf Step 7 not landed).
- `water_edge_flux` is transient by design — not persisted, cleared on load
  (`WorldPersistenceRoundtrip.cpp:302-308`) — so flow momentum resets across save/load.
- The per-subsystem `water` sub-hash group includes the float arrays and flags but **omits
  `water_depth_mm`/`water_bed_mm`** (`WorldPersistenceRoundtrip.cpp:870-883`); the authoritative
  integer state is in no localization group at all (terrain group: `:842-853`; mesh: `:855-868`).

### Shipped since the 2026-06-28 roadmap

**Nothing water-pillar-specific has landed since 2026-06-28** — verified:
`git log --since=2026-06-28` over `WaterSystem.*`, `WaterfallDetect.*`, `WaterPass.cpp`,
`res/shaders/waterfall.frag`, spec 009, and `WaterDeterminism_test.cpp` returns empty. The pillar's
most recent landed work is the 2026-06-25 lockstep-desync resolution block (`7d605399`, `254cec1f`,
`7592f84c`) and the 2026-06-23/24 perf block (`9a7360f2`, `43e3c7bf`, `07cc9f5f`, `896bd45c`,
`43567963`). The roadmap-era ships (017-A ring `ca2616d8`/`3bba2a52`, moon channel `3aa9740d`,
lake-preview crash fix `e1fee9ff`) belong to other pillars; `3aa9740d` touched `RenderPipeline.cpp`
but not the water paths. The only post-roadmap water artifact in the tree is the **uncommitted**
`res/shaders/waterfall.frag` scene-light diff (WATER-06).

## Gaps / debt

1. **Rain is not weather-driven (spec 010's declared NEXT).** `SetWaterHydrology` is only called
   from timelapse demos with hardcoded mm values (`main_client.cpp:5734`, `main_client.cpp:9625`);
   `WeatherSystem::PrecipitationAt` (`WeatherSystem.cpp:501`) exists and is already consumed
   deterministically elsewhere (`GameSession.cpp:417`), but never feeds the hydrology. No
   SystemConfig flag, no persistence of the hydrology config.
2. **Two live float→sim feedback edges keep the float mirrors load-bearing** (water-perf Step 7,
   parts 1+2 unlanded): `WaterSourceComponent` inject writes only the float mirror — which
   `StepChunkWaterFixed` overwrites for simmed chunks, making entity water sources effectively inert
   (`WaterSystem.cpp:466-473`; spec 009 FR-1's "WaterSourceComponent emitters quantized to integer
   discharge", `spec.md:59`, unmet); `apply_displacement` likewise writes float-only
   (`WaterSystem.cpp:776-784`); and `ResizeSimulationGrid` reconstructs the HASHED mm arrays FROM
   the float mirror — dead in steady state but reachable on a persisted-resolution mismatch
   (`WaterSystem.cpp:929-948`). Until severed, the floats cannot leave `world_hash`
   (`WorldPersistenceRoundtrip.cpp:767-775`).
3. **Water sub-hash localization blind spot:** a divergence in `water_depth_mm`/`water_bed_mm`
   flips `world_hash` but attributes to NO sub-hash section (`WorldPersistenceRoundtrip.cpp:870-883`)
   — exactly the localization the lockstep `desync_section:"water"` flow depends on
   (`docs/water-sim-lockstep-determinism.md:38-46`).
4. **Spec 009 AC-4 (host==peer cross-build) has no gate.** The integer path makes it structurally
   safe, but no two-build/two-config water-hash compare exists (grep of `test/` finds only comments;
   `.forge/scripts/validate-determinism-matrix.ps1` has no water mode). Debug vs Release canonical
   world hashes already differ (`6f008a9f…` vs `ea9a0121…` — float state elsewhere), which makes a
   *water-scoped* cross-build gate the only practical way to prove the water claim.
5. **Spec 009 AC-5's dam half is untested** — the terraform gate digs (`delta_mm=-4000`,
   `WaterDeterminism_test.cpp:167`) but never raises a bed and asserts pooling
   (`spec.md:101` requires both).
6. **Waterfalls are static worldgen** — no live-water/terraform response and no plunge-pool
   creation (`WaterfallDetect.h:125-140`, `WaterfallDetect.cpp:255-257`); AC-6's live-water clause
   (`spec.md:102`) unmet.
7. **Uncommitted split-brain shader fix** (`res/shaders/waterfall.frag` working-tree diff vs the
   committed `RenderPipeline.cpp:2424-2429`) — at HEAD the fix silently no-ops.
8. **Docs debt:** spec 009 `Status: Draft` (`spec.md:3`); spec 010 has no spec directory; the
   resize-path comment still claims "water resolution is camera-driven … TRUE host==peer needs a
   camera-independent sim resolution" (`WaterSystem.cpp:933-936`) — contradicting the landed Phase-3
   fix three screens above it (`WaterSystem.cpp:834-842`).

## Risks

- **Persistence-load resize path can corrupt hashed state** (medium): the only reachable trigger of
  `ResizeSimulationGrid`'s float→mm re-seed is a persisted chunk whose `current_water_resolution`
  differs from `WATER_SIM_RESOLUTION` (`WaterSystem.cpp:844-953`) — e.g. any future retune of the
  constant against old saves. Gap 2 removes the hazard.
- **Turning finite hydrology ON is a trajectory change** (medium): rain wakes every water chunk
  (`WaterSystem.cpp:508-516`) and under sustained rain the 64-chunk rotating window means each chunk
  sims every ⌈N/64⌉ ticks — hydrology slows with world size, and enabling it flips `world_hash`
  (acceptable local-dev, needs a deliberate re-pin + the settle-boot interaction re-checked).
- **Save/load flow-momentum reset** (low): `water_edge_flux` clears on load
  (`WorldPersistenceRoundtrip.cpp:308`), so a loaded world's water re-settles along a different
  transient than the run-through world. Both lockstep peers load the same save, so no desync — but
  replay-across-save and AC-8's "round-trips byte-identically" (`spec.md:104`) hold only for
  depth+bed, not flow.
- **Waterfall/water visual verification is blocked** (high, external): the headless IN_GAME
  render-capture hang (RENDER pillar; `src/luminumbra_client/main_client.cpp:4349` per the charter,
  `docs/specs/021-engine-framework-audit-charter/spec.md:40-43`) blocks confirming the night
  waterfall fix, and any water-visual re-bless.
- **KNOWN-CONTEXT staleness itself** (low): two of the pillar's memory entries (moving-lag killer,
  WaterfallDetect gaps) were overturned by the tree during this audit — recorded here so downstream
  planning stops re-scoping solved work.

## Opportunities

- **Weather-coupled water cycle** (WATER-07): `PrecipitationAt` is deterministic, already hashed sim
  state, and already consumed by plant growth (`GameSession.cpp:404-418`) — the same pattern gives
  storms that visibly fill ponds and dry spells that recede shorelines, a marquee living-world
  feature with the substrate already proven.
- **Hash-scope cleanup unlocks render amortization** (WATER-08): once the floats leave `world_hash`,
  the float-mirror regen can be skipped for off-screen/distant chunks without re-pins
  (`docs/specs/water-perf-200fps.md:171-173`).
- **Waterfalls as live phenomena** (WATER-11): gate each site's dressing on the live upstream water
  surface (render-only read of `get_water_level_at`) so damming a river actually stops the fall —
  connects terraforming to the most dramatic water visual, no hash impact if read-only.
- **River channel fidelity** (WATER-15): the 8×8/4 m uniform grid is the floor, not the ceiling —
  seam-aligned higher resolution on river chunks is explicitly sketched in-code
  (`WaterSystem.cpp:66-69`) once a seam-resampling scheme exists.

## Backlog items

| id | summary | spec | effort | risk | deps | status | proving_signal |
|---|---|---|---|---|---|---|---|
| WATER-01 | Spec 009 Phases 1–3 landed: fixed-point virtual-pipes solver + cross-chunk owner-edge continuity + terraform coupling (bed edit + player voxel dig) on a camera-independent uniform grid | 009 | XL | low | — | done | ctest `WaterDeterminism.*` (4 tests, test/common/WaterDeterminism_test.cpp) |
| WATER-02 | Spec 010 finite hydrology landed: SetHydrology(finite,rain,evap), rain fills land (land-water probe, not max-depth), evaporation recedes, default-OFF keeps baselines | 010 | L | low | — | done | ctest `WaterDeterminism.FiniteHydrologyRainFillsLandDeterministically` |
| WATER-03 | Water lockstep desync resolved: bed current_lod==0 gate + boot water-settle (0 flakes/24) + --smoke-moving proof that B′ is unnecessary | 018 | M | low | — | done | `--smoke` run==replay `6f008a9f637c40b7` + ctest `WaterDeterminism.LiveWaterSimIsRunReplayDeterministic` |
| WATER-04 | Water main-thread block (~450–1500 ms) killed: rotating 64-chunk sim window + inline init seeding + heightmap reuse + cached river-source mask; locked by the moving-hitch gate (220 ms water ceiling) | water-perf-200fps | M | low | — | done | engine gate `.forge/scripts/validate-moving-hitch.ps1` (WaterCeil=220) |
| WATER-05 | Waterfalls-from-connections landed: lake/tarn rim-outlet detection + crest/foot pinned to upstream/downstream water surfaces (KNOWN CONTEXT was stale) | 003 | M | low | — | done | ctest `WaterfallVisualTest` (.forge/scripts/verify-waveb-ocean.ps1:15) |
| WATER-06 | Commit the stranded waterfall night-lighting shader fix: HEAD's RenderPipeline sets `u_scene_light` (silent GL no-op) while the uniform exists only in the uncommitted res/shaders/waterfall.frag diff; then visually verify | 009 | S | low | RENDER: headless IN_GAME capture-hang fix (visual verify only) | in-progress | ctest `WaterfallVisualTest` stays green (shader default 1.0 = byte-identical) + WorldVisualSweep rerun on a night waterfall cell |
| WATER-07 | Drive spec-010 rain/evap from WeatherSystem::PrecipitationAt behind a SystemConfig flag (default-OFF), persist the hydrology config with the world, and quantize precip→rain_mm deterministically | 010 | M | medium | — | todo | NEW: ctest `WaterDeterminism.WeatherDrivenRainIsDeterministic` — weather-coupled rain run==replay + land-water volume rises during a storm epoch and recedes after; `--smoke` unchanged with the flag off |
| WATER-08 | Sever the float→sim feedback edges (route WaterSourceComponent + apply_displacement through water_depth_mm; make ResizeSimulationGrid interpolate the mm arrays, not the float mirror), then exclude the three float water arrays from world_hash (one deliberate re-pin) | water-perf-200fps | M | medium | — | todo | `--smoke` run==replay with ONE re-blessed `world_hash` bump + ctest `WaterDeterminism.*` + persistence gate `test/persistence/world-persistence-roundtrip.ps1` |
| WATER-09 | Add water_depth_mm/water_bed_mm to the `water` sub-hash localization group so an integer-state desync attributes to a section instead of vanishing | new | S | low | — | todo | NEW: ctest `WaterSubHashCoversFixedPointState` — flipping one water_depth_mm bit changes sub_hashes.water (and only that section) |
| WATER-10 | Spec 009 AC-4 cross-build water-hash gate: compare debug vs release `debug_water_state_hash` sequences on the same scenario (water-scoped, since full world_hash legitimately differs across configs) | 018 | M | low | — | todo | NEW: `validate-determinism-matrix.ps1 -Mode WaterCrossBuild` — per-tick water-state hash sequence identical between debug and release server builds |
| WATER-11 | Make waterfalls respond to live water/terraform: gate each site's sheet/spray dressing on the live upstream surface (render-only), and revisit the deferred plunge-pool-on-dry-foot decision | 009 | L | medium | WATER-08 | todo | NEW: ctest `WaterfallLiveWaterGate` — damming the upstream channel (EditTerrainBed) extinguishes the site's dressing deterministically; `WaterfallVisualTest` stays green |
| WATER-12 | Add the AC-5 dam half: raise-bed terraform test asserting deterministic pooling behind the new wall (only dig is tested today) | 009 | S | low | — | todo | NEW: ctest `WaterDeterminism.DamBedEditPoolsWaterDeterministically` — post-dam upstream depth rises, mass invariant holds, run==replay |
| WATER-13 | Prove and document the save/load flow-momentum contract: water_edge_flux resets on load by design — assert the loaded world re-settles to the same fixed point as run-through | 009 | S | low | — | todo | NEW: ctest `WaterPersistenceSettleParity` — save→load→N settle ticks reaches the identical water-state hash as the uninterrupted run |
| WATER-14 | Water docs debt: flip spec 009 Status Draft→Landed(P1–3), author the missing docs/specs/010 finite-hydrology spec doc, fix the stale camera-driven-resolution comment in ResizeSimulationGrid | new | S | low | — | todo | NEW: `SpecStatusAudit` check in an ops gate — every shipped spec has a dir + a Status matching the tree (negative: a Draft spec with landed ACs fails) |
| WATER-15 | River-channel sim fidelity beyond the uniform 8×8 grid: seam-aligned higher resolution on river chunks (needs a deterministic seam-resampling scheme) or render-side channel detail | new | XL | high | WATER-08 | todo | ctest `WaterDeterminism.*` green + `--smoke` re-pin + WorldVisualSweep water cells (river channel reads as a channel at 2 m) |
