# Iteration 3 — Binding Design Decisions (T-I3-0)

Referenced by every Wave 1-3 task. Sources: ultimate-plan.md, critique.md,
the engine TDD (README), and the Project Capture game README (develop branch).

## 1. Simulation tick
- Canonical fixed simulation rate: **30 Hz** (TDD §3.2; WaterSystem concurs).
- `SimulationClock` (engine, `src/luminumbra_common/core/`): accumulator over
  variable frame dt, fixed dt = 1/30, max 4 catch-up ticks per frame (clamp,
  drop excess time with a telemetry counter). Configurable rate parameter
  exists for tests; 30 Hz is the only shipped value.
- Hosted by GameSession::TickSimulation(); order per tick:
  Animation pose sampling -> Instinct planning -> field budget (iteration 4)
  -> queued world edits -> OrderedEventBus drain.
- Client render loop stays variable-dt; physics stays variable-dt client-side
  this iteration; the headless server ticks physics at the fixed rate.

## 2. Seed-offset registry (collision-free, append-only)
| Offset | Owner | Use |
|---|---|---|
| +0 | existing | terrain base 2D |
| +1 | existing | cave 3D |
| +2 | existing | island mask 2D |
| +3 | shaping | continentalness 2D |
| +4 | shaping | erosion 2D |
| +5 | shaping | peaks/valleys 2D |
| +6 | shaping | domain warp X 2D |
| +7 | shaping | domain warp Z 2D |
| +8 | reserved (iter 4) | biome temperature |
| +9 | reserved (iter 4) | biome humidity |
| +10 | reserved (iter 4) | rivers |
New consumers append here FIRST; a duplicate offset is a review-blocking
defect.

## 3. Region container spec (persistence v2 AND far-LOD store)
- Path: `<save_dir>/chunks/region/r.<rx>.<rz>.lmr`; region = 32x32 chunks
  (512 m); `rx = floor(chunk_x/32)`, `rz = floor(chunk_z/32)`.
- File: magic `LMR1` (u32) | u16 version=1 | u16 record_count | manifest of
  records | LZ4-compressed record payloads.
- Record header: u64 chunk_id (or tile_id) | u8 lod_level (0 = full live
  chunk record; 1/2 = far tiers F1/F2) | u8 flags (bit0 edited/authoritative,
  bit1 water-present) | u32 uncompressed_size | u32 compressed_size.
- Payload kinds: lod_level 0 = the v1 chunk snapshot record (sdf/heightmap/
  materials as today, LZ4); lod_level 1/2 = packed far samples (per sample:
  u16 height_q (1/16 m), u8 material, u8 flags) + shared border row/column.
- `world_hash` remains computed over the canonical IN-MEMORY snapshot,
  format-independent: v1-file vs v2-file load of the same world MUST hash
  equal (this equality IS the migration gate).
- Pristine far tiles are regenerable cache keyed (seed, params_hash, tier,
  region) and carry a deterministic fnv1a64 tile hash; edited tiles are
  authoritative (built by downsampling edited chunks on unload).
- Durable entity ids: u64, allocated monotonically per world, persisted in
  the world manifest; EntitySnapshot fixtures already use u64 ids — the
  manifest allocator is the source of truth from T-I3-7 onward.

## 4. View distance numbers (pinned)
- Baseline: radius 12 chunks = 192 m. Requirement: **>= 1152 m (6x) complete
  visible horizon**. Far-LOD tiers: F1 4 m samples covering 512-768 m; F2 8 m
  samples covering 768-1536 m. Gate `FarLodHorizon` asserts: zero missing
  wanted regions to 1536 m after settle; farlod_resident_bytes < 64 MB;
  gbuffer_gpu_ms delta < 1.5 ms vs baseline. F3 (3 km) is cut; the far-field
  end-state is SHIELD-RT SDF raytracing (TDD) — do not gold-plate the
  waypoint.

## 5. File-ownership map (Wave 1-3 contention control)
- Agent A (main checkout, serial): SHIELD_WorldSystem.{h,cpp},
  MarchingCubes.{h,cpp}, RuntimeScenarioHarness (T3 additions),
  main_client.cpp scenario registration for T3, engine-frontier validator
  (PlayerView).
- Agent B (worktree, serial): GameSession.{h,cpp}, SimulationClock (new),
  TerrainPresetLoader (new), main_client.cpp clock/manifest wiring,
  common sources.cmake. Merge after Agent A's T3 lands; harness/validator
  edits are append-only.
- Wave 2: far-LOD agent owns MarchingCubes/FarLodStore/RenderPipeline;
  shaping agent (worktree) owns SHIELD height paths + presets + snapshot
  tests. Serialize their SHIELD merges.
- Wave 3: server agent owns src/luminumbra_server + validator appends;
  characters agent (worktree) owns tools/asset_processor, animation/ (new),
  GBufferPass, ai/InstinctPlanner, ecs/EntitySnapshot.
- validate-engine-frontier.ps1 and RuntimeScenarioHarness are append-only
  everywhere; merges serialized by the orchestrator.

## 6. Determinism / compatibility discipline (unchanged, restated)
Contracts that may only change with a deliberate versioned bump in the same
commit as their gate update: meshing FNV hashes, GPU/CPU parity corpus,
worldgen snapshot goldens, persistence world_hash semantics, perf baselines
(re-bless with previous block), render-health baseline (re-bless protocol).
The SDF-skip (T-I3-1) must keep LodSeamRisk green — fallback patches either
read face-band SDF or sample GetTerrainHeightAt; proven before merge.
