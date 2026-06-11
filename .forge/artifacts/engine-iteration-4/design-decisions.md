# Iteration 4 — Binding Design Decisions (T-I4-0)

Referenced by every iteration-4 task. Sources: spec
`ENGINE-ITERATION-4-2026-06-11.md` (FINAL), `critique.md` (all resolutions
binding), the two research surveys in `.forge/artifacts/engine-research/`.

## 1. Seed-offset registry (activations; append-only, collisions = defect)
| Offset | Owner | Use |
|---|---|---|
| +0..+7 | existing | terrain base/cave/island/shaping/warp (iteration 3) |
| +8 | T-I4-1 | biome temperature 2D |
| +9 | T-I4-1 | biome humidity 2D |
| +10 | T-I4-3 | river PV-band noise 2D |
| +11..+13 | reserved (iter 5) | wind grid / weather cells / season phase |

## 2. Biome architecture (T-I4-1/2)
- Selection: `biome_id = BiomeTable.lookup(continentalness, erosion, pv,
  temperature, humidity)` — the five dimensions REUSE +3/+4/+5 control
  noises and add +8/+9. Engine knows only the lookup; the table is GAME
  DATA at `data/common/biomes.json` (per-biome: climate ranges, surface
  material palette {top, filler, depth}, underwater material, reverb
  params (T-I4-5), future-proof `vegetation` block parsed-not-consumed).
- Per-column biome id cached in the surface-span cache (u8, 255 = none).
- Presets opt in via `"biomes": {"table": "common/biomes.json"}`; absent →
  legacy single-material behavior, byte-zero drift (existing fixtures are
  the proof).
- **ComputeTerrainParamsHash MUST mix in the biome-table content hash**
  (fnv1a64 of the canonicalized JSON) when biomes are enabled — pristine
  far-LOD tiles then self-invalidate on table change (critique-verified
  requirement; FarLodSystem.cpp:103 / FarLodStore.cpp:314).

## 3. Materials LUT schema (COMPLETE — decided here so A and B never fight)
`data/common/materials.json` per-material columns:
- existing: id, name, base color, emission (iteration ≤3 fields untouched)
- `texture_layer` (int, -1 = untextured), `normal_layer` (int, -1 = flat),
  `tiling` (float, world-units per repeat) — written by T-I4-7 ONLY
- `roughness` (0..1, default 0.85) — written by T-I4-10 ONLY
- `emissive_intensity` (calibrated scalar; calibration table maps it to
  on-screen bloom response) — written by T-I4-9 ONLY
- biome palettes live in biomes.json (NOT here) and reference material ids
  — written by T-I4-2 ONLY
Merge order: T-I4-2 merges before any B-task materials.json edit; B agents
rebase. Unknown columns are warnings, never errors (loader already does
this).

## 4. Rivers (T-I4-3)
- Chunk-local carve where the folded PV value falls in the valleys band;
  exact band ranges per the research capture
  (worldgen-lockstep-sdfrt.md Area 1, Minecraft 1.18 PV-band trick), river
  noise on +10 modulates width/wobble.
- River waterline: carve floor sits below a per-river-cell water level;
  water fill uses the EXISTING fill machinery (same path as archipelago
  sea — critique F5). No new WaterSystem scope; no new WaterVisual scope.
- Bank material from the biome palette (`filler` at banks).
- Global drainage/erosion rivers explicitly OUT (iteration-6 erosion
  precompute).

## 5. Structures (T-I4-4)
- Placement: spacing/separation/salt grid per structure type (O(1),
  deterministic, supports `locate(type, near)` query — gate-tested).
- Assembly: jigsaw template pools, templates as game data under
  `data/common/structures/<type>/` (JSON: voxel-box pieces with material
  ids + socket joints; small hand-authored fixtures this iteration).
- Structures write through the normal edit path → persist as edited
  chunks/regions (existing machinery; no new persistence format).
- WFC explicitly OUT.

## 6. FP determinism contract (T-I4-11; precedes ALL Wave C work)
- Compiler flags pinned on luminumbra_common + luminumbra_server targets:
  `-ffp-contract=off`; fast-math family banned (none present today).
  Critique-verified expectation: HASH-NEUTRAL (no -march/-mfma in the
  build → no FMA codegen today). Proof in the same commit: full ctest +
  HeadlessServerTick hash unchanged. If NOT neutral: STOP, report — the
  mega-bump is an orchestrator (Fable) decision, not the agent's.
- `core/DeterministicMath.h`: sin/cos/atan2/sqrt wrappers for sim code
  (table/poly implementations, exactly reproducible); libm transcendentals
  banned from sim paths by the new determinism lint (also bans unordered-
  container iteration order dependence, wall-clock, and non-seeded RNG in
  sim code — the Factorio std::sort comparator lesson).
- world_hash gains per-system sub-hashes {terrain, entities, water,
  fields} surfaced in the server_tick artifact; heavy-mode oracle
  (save → load → resimulate N ticks → compare) wired into
  HeadlessServerTick as `-Heavy`.

## 7. Replay format LREC1 (T-I4-12)
- Header: magic `LREC1` u32 | u16 version=1 | u64 seed | u64 preset_hash |
  u64 start_world_hash | u16 tick_rate (30) | u64 tick_count (patched on
  close).
- Records: TICK-indexed (never frame-indexed; SimulationClock catch-up/
  drop telemetry excluded — critique F4): per tick, the input set applied;
  every 30 ticks a world_hash checkpoint record.
- Playback drives ServerWorldRunner tick-by-tick; divergence = first
  checkpoint mismatch, reported with tick index + sub-hash breakdown.
- Recording is ON for every server run (cost is negligible; it is the
  desync-repro tool).

## 8. Lockstep transport (T-I4-13/14)
- Delay-based lockstep; rollback REJECTED (rationale: zen co-op, no
  competitive latency demands; research Area 2). Server-paced, adaptive
  input horizon (start 3 ticks, grow on late inputs, max 10 → beyond that
  the session pauses visibly rather than desyncing).
- TCP, length-prefixed frames, inputs + hash exchanges only. Loopback +
  LAN scope. Any disconnect ends the session cleanly (no rejoin v1 —
  critique F3). Desync → halt + both sides dump LREC1 replays.
- Camera look stays render-side in PlayerController (hides perceived
  latency); only movement/action inputs travel.

## 9. Close-range material gate (T-I4-7; calibration-plate pattern)
Scenario teleports to authored per-material test patches (world-edited at
scenario start at fixed coordinates), captures at 2-8 m under two sun
angles (existing time-of-day control): per-material albedo bands + normal-
response check (shading delta between sun angles exceeds flat-surface
bound). This task OWNS the MaterialVisual re-home (iteration-3 deferral
diagnosis in handoff.md is its starting input).

## 10. Texture pipeline (T-I4-6)
- GL texture arrays keyed by LUT layer indices; bindless REJECTED
  (research: AMD-fragile/Intel-absent on GL).
- asset_processor imports PNG → mip chain in `.ltex` (simple header +
  raw mips; KTX2 deferred); texture-resident-bytes surfaced in render
  telemetry with a gate budget: **96 MB** textures resident max this
  iteration.

## 11. SHIELD-RT spike (T-I4-15)
Heightfield ray-march of FarLodStore tiles VS sphere-traced mip SDF;
min-filtered conservative mips are a SUCCESS CRITERION. One benchmark
round, memo, stop (critique F6). Final timing capture on a quiet machine
(coordinate with orchestrator if other agents are building).

## 12. File-ownership map + executors (merges serialized by orchestrator)
- Agent WA1 (opus, main checkout, serial): T-I4-1 → 2 → 3. Owns
  SHIELD_WorldSystem, TerrainPresetLoader, MarchingCubes (A2 materials
  paths), FarLodStore, biomes.json, presets, worldgen snapshot tests,
  materials.json (palette refs ONLY via biomes.json), validator appends.
- Agent WA2 (opus, main checkout, after WA1): T-I4-4 → 5. Owns
  StructurePlacement (new), structures data, EnvironmentalAudioSystem.
- Agent WB1 (opus, worktree, parallel with WA1): T-I4-6 ONLY. Owns
  asset_processor, RenderPipeline texture-array residency, .ltex tests.
  NO materials.json edits.
- Agent WB2 (opus, worktree, after WA1+WB1 merge): T-I4-7 → 8 → 9 → 10.
  Owns g_buffer.frag, lighting_pass.frag, bloom_composite.frag,
  skinned_mesh.vert, GBufferPass, materials.json texture/roughness/
  emissive columns, harness + validator appends, render-health baseline
  re-bless.
- Agent WC (opus, main checkout, after WA2 merge + hash re-bless):
  T-I4-11 → 12 → 13 → 14. Owns build flags, DeterministicMath, replay/,
  net/, server runner, main_client transport wiring.
- Agent WS (opus, worktree, anytime): T-I4-15 spike (test/performance +
  memo only).
- Agent WO (opus, main checkout, after WC): T-I4-16 → 17 → 18. Owns
  render passes (MDI), JobSystem, MarchingCubes allocators, release
  baseline re-blesses.
- RuntimeScenarioHarness.cpp, main_client.cpp registrations, validator
  scripts: APPEND-ONLY for everyone.

## 13. Determinism/compatibility discipline (restated)
Contracts move only via deliberate versioned bumps in the same commit as
their gate update: meshing FNV hashes, worldgen goldens, far-tile hashes,
world_hash semantics, perf baselines (re-bless with previous block, GPU
provenance recorded), render-health baseline (re-bless protocol),
LMS2/.lanim layouts (additive version bump), replay LREC1 (versioned).
