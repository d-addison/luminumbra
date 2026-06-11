# Panel 4 — Engine/Game Split, Worldgen Features, Gate Cash-Ins, Debt

Research panel 4 of 4 for engine iteration 3 planning. Scope: split mechanics,
worldgen FEATURES beyond shaping (panel 1 owns shaping), built-gate cash-ins,
and debt. Evidence gathered read-only from the working tree on
`feat/polyglot-audit-roadmap`, 2026-06-10.

## Current State

**Engine/game split.** The owner's Architectural Principle
(`.forge/artifacts/engine-frontier/handoff.md`, "Architectural Principle"
section) mandates two decoupled deliverables: generic engine in
`src/luminumbra_{common,client,server}`, game content in `data/`, `worlds/`,
`scripts/` (Lua/JSON). The split is already mostly honored: GroveStrider the
*creature* lives entirely in data (`scripts/common/archetypes/grovestrider.json`,
`scripts/common/ai/agents/grovestrider.lua`, `data/audio/*.bank.json`), and the
Instinct planner API itself is data-driven. Violations are narrow and known:
hardcoded grovestrider fixture data inside engine translation units
(`InstinctPlanner.cpp`, `EntitySnapshot.h`) and the game-named
`AethericFieldDiffusion` class for what is a generic conservative scalar-field
diffusion system. `scripts/client/` and `scripts/server/` exist but are empty;
`data/common/` contains only `materials.json`. There is no `data/game/`
directory yet and no mechanical enforcement (no path-based PR lint) of the
split.

**Worldgen features.** Presets declare features the engine never reads.
`TerrainGenParams` (`src/luminumbra_common/systems/SHIELD_WorldSystem.h:19-34`)
carries only terrain shape + caves + island mask. The `biomes` block
(temperature/humidity frequencies), `features.rivers_enabled`,
`features.structures_enabled` (`worlds/atlas/presets/default.json`), and the
entire `materials.strata`/`materials.veins` block in
`worlds/atlas/presets/temperate_forest.json` are parsed by NO code path —
neither the runtime loader (`GameSession.cpp:99-108`) nor the three duplicated
test-side `LoadPresetParams` copies. Material selection is hardcoded twice
(`classify_material` in `SHIELD_WorldSystem.cpp:65-83`, duplicated as a
fallback in `MarchingCubes.cpp:51-70 GetTerrainMaterialAt`) with magic numbers
(sand below y<34, grass depth<1, soil depth<5, stone otherwise).

**Built gates awaiting cash-in.** Three gates exist that lock in
"no-worse-than-today" so implementations can land safely: (1)
`lod_boundary_oscillation` gate — no hysteresis exists in
`get_lod_level_for_distance`; (2) GPU SDF integration — compile-time disabled
behind `kEnableExperimentalGpuSdfIntegration = false` with three frontier
validator gate modes plus a source-needle parity test; (3) perf budgets — the
PerfRegression gate and blessed baseline are debug-preset-only
(`capture-perf-baseline.ps1` defaults `BuildPreset = "debug"`), and
`run_runtime_boot_metrics.ps1` is a manual lane.

**Debt.** Four UI binding-unsubscribe TODO leaks (use-after-free hazard, not
just leak), six WorldList stubs, one audio doppler/reverb TODO, and the
whole-world single-JSON persistence layout (observed 57 MB
`world-state.json` artifact) that blocks the iteration-3 far-LOD store and big
worlds.

## Findings

### Worldgen: declared-but-unbuilt features

- `src/luminumbra_common/systems/SHIELD_WorldSystem.h:19-34` —
  `TerrainGenParams` has no biome, river, structure, or material-strata
  fields. Adding them here is the single seam: this struct flows to
  generation, the GPU SDF callback signature
  (`SHIELD_WorldSystem.h:183`), the WorldGenViewer, and all preset loaders.
- `src/luminumbra_common/world/GameSession.cpp:66-108` — runtime preset
  parsing validates/reads only `generation_params.terrain` and
  `generation_params.features` (caves only). `biomes`, `rivers_enabled`,
  `structures_enabled`, `materials` are silently ignored.
- `test/shield/test_worldgen_layer_snapshots.cpp:158-181`,
  `test/performance/runtime_world_visual_validation_test.cpp:178`,
  `test/performance/initial_world_loading_perf_test.cpp:130` — three
  copy-pasted `LoadPresetParams` implementations, all also ignoring the
  feature blocks. Four total preset parsers to converge.
- `worlds/atlas/presets/default.json` — declares
  `biomes.temperature_frequency: 0.005`, `humidity_frequency: 0.005`,
  `rivers_enabled: true`, `structures_enabled: true`. All dead.
- `worlds/atlas/presets/temperate_forest.json` — declares a full
  data-driven material model (`strata`: Soil/Stone/Deepslate by depth;
  `veins`: LuminCrystal in Stone/Deepslate at noise threshold 0.85, Sand
  with `max_altitude`). `grep strata|veins|host_materials` over `src/`
  returns zero files — completely unimplemented, but it is the right schema
  shape to implement against.
- `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:65-83`
  (`classify_material`) and `src/luminumbra_common/world/MarchingCubes.cpp:51-70`
  (`GetTerrainMaterialAt`) — duplicated hardcoded material logic, including
  duplicate magic constants (y<34/h<36 sand rule). `GetTerrainMaterialAt`
  calls `SampleWorldGenLayers` then re-derives the same rules as a fallback
  when the sample returns Air/Water.
- `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1235-1350`
  (`GenerateChunkData`) — the graft point for features. CPU path: batch 2D
  heightmap noise (line 1289), batch 3D cave noise (1294), then a per-voxel
  combine loop (1298-1345). Biome control noises slot in as additional
  `GenUniformGrid2D` batches; river carving slots into the combine loop next
  to `apply_cave_field` (1333); structures need a post-pass after the SDF
  loop. CRITICAL: the GPU path (1244-1272) bypasses the entire CPU combine —
  any feature added only to the CPU side silently widens the GPU/CPU parity
  gap (see Gate Cash-Ins).
- `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1002-1045`
  (`SampleWorldGenLayers`) — the deterministic single-point mirror of
  `GenerateChunkData` used by meshing materials, the WorldGenViewer
  (`WorldGenViewer.cpp:133`), and the worldgen atlas snapshot tests. Every
  feature must be added in BOTH the batch path and this sample path or the
  layer-snapshot gate diverges from runtime.
- `data/common/materials.json` — `luminumbra.material_registry.v1`, 8
  materials (Air, Stone, Soil, Grass, Sand, Deepslate, LuminCrystal with
  `emission`, Water). Registry is data; LuminCrystal entry is the sanctioned
  game fixture per the owner's principle.
- Determinism context honored by existing code: noise seeds are offset per
  layer (`m_seed` terrain, `m_seed + 1` caves, `m_seed + 2` island mask) —
  new biome/river noises must continue the disjoint-seed-offset convention,
  and caves are surface-capped (`surface_capped_cave_density`,
  `SHIELD_WorldSystem.cpp:1038`) — the cave-cap lesson rivers must honor.

### Built gates

- LOD hysteresis: `SHIELD_WorldSystem.cpp:221-229`
  (`get_lod_level_for_distance`) is a bare threshold walk over
  `m_lod_levels` (192/384/640 m, `SHIELD_WorldSystem.h:222-226`); no
  hysteresis state anywhere (the only "hysteresis" hit in engine code is a
  render-radius comment at `SHIELD_WorldSystem.cpp:780`).
  `get_required_lod_for_chunk` (231-257) adds the surface-band
  horizontal-distance rule (vertical seam fix) but feeds the same raw
  threshold function. The gate
  (`RuntimeScenarioHarness.cpp:2892-2960`, schema
  `luminumbra.lod_boundary_oscillation.v1`) locks baselines explicitly
  documented as "no LOD hysteresis yet ... a future hysteresis fix can
  tighten these numbers": 0.75 transitions/chunk/s max, 240 oscillating
  chunks, 115 total transitions/s. Validator:
  `validate-runtime-stability-phase-1.ps1:531` (`Test-LodBoundaryHysteresis`).
- GPU SDF: `RenderPipeline.cpp:60`
  (`constexpr bool kEnableExperimentalGpuSdfIntegration = false`), runtime
  opt-in `set_gpu_sdf_runtime_enabled` (357), guarded setup
  (`SetupGPUSDFIntegration`, 374-402). The parity test
  (`test/shield/test_sdf_gpu_cpu_parity.cpp:110-126`) asserts BY SOURCE
  NEEDLE that the flag stays false and currently validates only CPU SDF
  invariants over a 9-case seed/chunk corpus — there is no live GPU dispatch
  in the parity test yet. Three frontier gate modes exist
  (`validate-engine-frontier.ps1`: GpuSdfCallbackSafetyGate,
  GpuSdfComputeParityGate, GpuSdfRuntimeToggleGate). The GPU heightmap
  derivation in `GenerateChunkData` (1250-1265) reconstructs heightmap from
  SDF by marching down — quantized to integer y, NOT equal to the CPU's
  analytic `terrain_h`; physics-relevant divergence to resolve before
  enablement.
- Release perf lane: `test/performance/run_runtime_boot_metrics.ps1` is a
  manual parameterized script; `.forge/scripts/capture-perf-baseline.ps1`
  defaults to the debug preset; `PerfRegression` gate mode
  (`validate-engine-frontier.ps1:2338`) compares against the
  debug-blessed baseline only. A `release` CMake preset exists
  (`CMakePresets.json:39,59,79`) with warnings-on, so the lane is buildable
  today — there is simply no release baseline or gate wiring.

### Debt

- `src/luminumbra_client/ui/core/UIComponent.h:125,151,189,209` — four
  `/* TODO: Implement unsubscribe */` no-op lambdas pushed into
  `m_bindings`. Root cause is upstream:
  `src/luminumbra_client/ui/core/UIProperty.h:48-50` —
  `Subscribe(PropertyCallback<T>)` appends to a plain
  `std::vector<PropertyCallback<T>>` and returns void; no token, no
  removal API. Bindings capture `this`/`m_element` raw, so a destroyed
  UIComponent leaves a dangling callback in the Property — use-after-free
  on next `NotifyObservers`, not merely a leak.
- `src/luminumbra_client/ui/components/game/WorldList.cpp:369,392,411,416,436,469`
  — loading-state, favorite-toggle, favorite/recent filters, last-played
  formatting, creation-date sort all stubbed (filters return `true`
  placeholders).
- `src/luminumbra_client/audio/AudioSpatialCluster.cpp:332` —
  `// TODO: Add doppler and reverb processing when needed` in the
  attenuation path (occlusion already implemented at 329).
- Persistence: `WorldSaveService.h:21-25` documents the single
  `<save_dir>/chunks/world-state.json` layout and that "all writes funnel
  through the private write_snapshot() seam so the layout can later split
  into per-chunk files without changing the public API". Schema
  `luminumbra.persistence.world_state_snapshot.v1`
  (`WorldPersistenceRoundtrip.cpp:20`). `save_dirty_chunks`
  (`WorldSaveService.h:51-58`) already computes per-chunk dirtiness
  (`WorldStreamingState::dirty_chunk_ids`,
  `WorldStreamingState.h:37-40`) but rewrites the whole snapshot when any
  chunk is dirty. Observed artifact:
  `build/debug/test-artifacts/persistence/runtime-roundtrip/session/chunks/world-state.json`
  = 57 MB. Gates already in place to protect a format change:
  PersistenceRoundtripGate, PersistenceRuntimeRoundtrip,
  ChunkFormatValidationGate, WorldHashEntitySnapshotGate
  (world hash is fnv1a over canonical snapshot bytes —
  `WorldSaveService.h:47-49` — so a format v2 must preserve a canonical
  hashable serialization).

### Game-named engine code

- `src/luminumbra_common/ai/InstinctPlanner.cpp:126-143`
  (`MakeGrovestriderHungerFixture`, declared `InstinctPlanner.h:53`) —
  hardcoded grovestrider needs/opportunities ("mossberry-cache",
  "thunder-hollow"...) inside the engine planner TU. Worse,
  `PlanInstincts` itself (`InstinctPlanner.cpp:190-194`) bakes the
  fixture's expected answer into the engine result:
  `plan.passed = ... candidates.front().target == "mossberry_grove"` —
  game-fixture assertions living inside the engine API. Consumed by
  `test/ai/instinct_planner_gate_test.cpp:7` and
  `test/network/network_state_hash_gate_test.cpp:57`.
- `src/luminumbra_common/ecs/EntitySnapshot.h:51-79+`
  (`BuildEntitySnapshotFixture`) — "grovestrider_alpha" / "lantern_wisp"
  fixture entities in an engine header.
- `src/luminumbra_common/aetheric/AethericFieldDiffusion.{h,cpp}` — generic
  conservative 2D scalar diffusion (energy, permeability, sealed cells)
  under the game name, namespace `luminumbra::aetheric`, schema
  `luminumbra.aetheric.field_diffusion.v1`; listed in
  `src/luminumbra_common/sources.cmake`.

### Game vision anchors (for what the game module must hold)

`README.md` (sections 2.2, 2.4): Instinct Engine archetypes/needs/actions are
game data over a deterministic GOAP engine API; the Aetheric Field
(Lumin/Umbra) is a game concept over a generic diffusion field; emissive
materials glow by field strength. Existing game data tree:
`scripts/common/{ai,archetypes,directives,systems}` (agents, actions, JSON
archetypes including `grovestrider.json` and `shadowstalker.json`,
`system_aetheric_feedback.lua`, scenario directives).

## Split Mechanics

The split is a discipline + relocation problem, not a build-system rewrite.
Engine = `src/`, `res/shaders/`, `include/`; Game = `data/`, `worlds/`,
`scripts/`, `assets/`. Recommendation: keep the in-repo sibling-directory
model (no separate repo this iteration) and make the boundary mechanical.

Concrete moves:

1. **Grovestrider planner fixture out of engine.** Replace
   `MakeGrovestriderHungerFixture` with a generic
   `LoadInstinctPlanRequestFromJson(const nlohmann::json&)` in
   `InstinctPlanner.{h,cpp}`; move the fixture values to
   `scripts/common/ai/fixtures/grovestrider_hunger.json` (game data) plus a
   mirrored test asset path; tests load the JSON. Remove the baked
   `plan.passed` target-name check from `PlanInstincts` — move
   expected-answer assertions into the gate test/fixture (`expected` block in
   the fixture JSON). Keep the serialized plan schema stable so
   `NetworkStateHash` and `InstinctPlannerGate` artifacts stay comparable;
   the gate tests change in the same commit.
2. **EntitySnapshot fixture out of the engine header.** Move
   `BuildEntitySnapshotFixture` from `EntitySnapshot.h` to a test-support
   file under `test/` (it is only fixture data); engine header keeps the
   snapshot types and normalization only.
3. **Rename AethericFieldDiffusion.** Engine class becomes
   `ScalarFieldDiffusion` in `src/luminumbra_common/fields/` (namespace
   `luminumbra::fields`), schema
   `luminumbra.fields.scalar_diffusion.v1`. Provide a temporary
   `using AethericFieldDiffusion = ScalarFieldDiffusion;` alias +
   dual-schema acceptance in the AethericDiffusionGate during the
   transition, then drop the alias at iteration close. The *Aetheric* name
   survives only in game data/Lua (`system_aetheric_feedback.lua`) and in the
   gate's fixture name.
4. **Game module layout.** Adopt:
   - `data/common/` — engine-consumed registries (materials.json stays; it is
     the registry; LuminCrystal entry is game content riding a generic
     emissive-material feature).
   - `data/game/` — NEW: game-only tunables that no engine system requires to
     boot (e.g. aetheric source definitions, creature spawn tables when they
     arrive).
   - `worlds/atlas/presets/` — game data (preset params already declared
     "game data" by the Iteration 3 Directives).
   - `scripts/{common,client,server}/` — game Lua/JSON, already structured.
5. **PR separability.** Two mechanisms, both cheap: (a) a path-lint gate
   (extend an existing validator script) that fails any PR touching both
   `src/**` and `{data,worlds,scripts}/**` unless labeled
   `engine+game-interface` — keeps engine PRs and game PRs separable by
   default while allowing schema-bump PRs explicitly; (b) grep-gate that
   fails on game nouns (`grovestrider|aetheric|lumincrystal|shadowstalker`,
   case-insensitive) appearing in NEW lines under `src/` (allowlist:
   the transition alias and data-path string literals). This encodes the
   owner's "do not entrench further" rule as a ratchet.

## Worldgen Features Design

Panel 1 owns shaping (continentalness/erosion/domain-warp/spline remap).
Those control noises are SHARED infrastructure: this panel's biome system
must consume panel 1's continentalness/erosion fields rather than inventing
parallel noises. Concretely: panel 1's control-noise outputs should land as
named fields on `WorldGenLayerSample` (e.g. `continentalness`, `erosion`)
and as batch buffers inside `GenerateChunkData` — biomes then derive from
(temperature, humidity, continentalness, erosion) without any extra noise
fetches beyond the two new 2D grids.

**Biome system (engine = systems, game = content).**
- Extend `TerrainGenParams` with `biomes_enabled`,
  `temperature_frequency`, `humidity_frequency` and add a data-driven
  `BiomeTable` loaded from the preset: each biome entry = name + selector
  ranges over (temperature, humidity, optionally panel-1 controls) +
  material strata/veins block + future vegetation hooks. The
  `temperate_forest.json` `materials` block is the schema seed; promote it
  under per-biome entries (a single implicit biome when `biomes` absent —
  exact backward compatibility for existing presets and snapshot gates).
- One canonical preset parser: move `LoadPresetParams` into the engine
  (e.g. `worldgen/TerrainPresetLoader.{h,cpp}`), used by `GameSession`
  and all three test TUs (deletes the 4-way duplication; unknown-key
  warnings so dead preset keys can never silently reappear).
- Generation: two new `GenUniformGrid2D` batches (seeds `m_seed + 3`
  temperature, `m_seed + 4` humidity) in `GenerateChunkData`, mirrored in
  `SampleWorldGenLayers` (new `WorldGenLayerSample` fields
  `temperature`, `humidity`, `biome_id`). `classify_material` becomes
  table-driven (strata by depth-below-surface, veins by 3D noise threshold
  with host-material and altitude constraints, per the declared schema);
  `GetTerrainMaterialAt` in MarchingCubes drops its duplicated fallback
  constants and trusts the sample (single source of truth).
- Vegetation/preset modulation is a consumer, not part of this stream:
  biome_id exposed on the sample is the hook; actual vegetation is a later
  iteration.
- Gates: extend the worldgen atlas layer-snapshot machinery
  (`test_worldgen_layer_snapshots.cpp`) with temperature/humidity/biome_id
  layers per preset; extend the MaterialVisual heatmap gate with a
  per-biome material-distribution assertion (e.g. temperate_forest shows
  Soil-over-Stone strata, LuminCrystal vein frequency within band).
  Determinism: meshing-determinism hash test must stay byte-identical for
  presets without a `biomes` block.

**Rivers (carving, determinism, cave-cap lessons).**
- Approach: noise-guided carving, not hydraulic simulation — a 2D river
  field `r(x,z)` = |ridged noise| (seed `m_seed + 5`) thresholded near its
  zero-crossings produces connected winding channels; carve depth scales
  with distance below the threshold and with panel-1 erosion where
  available. Carving is applied as a HEIGHT modification (lower `terrain_h`
  before density computation), NOT a 3D density subtraction — this is the
  cave-cap lesson applied: caves needed `surface_capped_cave_density` to
  stop carving from puncturing the surface unpredictably; rivers avoid the
  problem class entirely by staying in the heightfield domain, which also
  keeps `GetTerrainHeightAt`, the heightmap physics path, and
  `column_surface_chunk_y` automatically consistent.
- Riverbed water: carve below the water table and let the existing
  WaterSystem/sea-level machinery fill; riverbed material override (Sand)
  via a biome-table rule keyed on `river_mask > 0`. First milestone is
  explicitly "carved riverbeds + filled water at/below sea level";
  flowing above-sea-level water is out of scope (no water-volume
  simulation exists for it).
- Determinism: pure function of (x, z, seed) — no inter-chunk
  communication, no flow accumulation pass; identical on any chunk
  generation order, satisfying the chunk-local generation contract that
  both the CPU batch and `SampleWorldGenLayers` paths require.
- Gates: atlas river-mask layer snapshot; a `river_visual_smoke` capture
  scenario (water ROI present along a known seed's river course) following
  the water_visual pattern; slope/connectivity metric in the atlas
  (river cells form ≥1 connected component longer than N cells).

**Structures (data-defined placement — DEFER if heavy).**
- Engine piece: a deterministic placement pass — per-chunk, derive
  candidate sites from (seed, chunk coords, biome_id, slope/height
  constraints) via hash-based jittered grid; stamp small SDF/material
  edits through the existing post-generation edit path so structure edits
  ride the persistence dirty-chunk machinery rather than the generator.
  Structure DEFINITIONS (what to stamp) are game data
  (`data/game/structures/*.json`).
- Honest sizing: multi-chunk structures need cross-chunk stamping and
  arrival-order independence — that is real engineering. Recommendation:
  implement only the placement-determinism core with single-chunk
  micro-structures (boulder/crystal-cluster scale) behind
  `structures_enabled`, gate with a placement-determinism test (same
  seed ⇒ identical site list, artifact-locked), and DEFER multi-chunk
  structures to iteration 4. If the iteration runs hot, the whole stream
  defers cleanly: the flag stays parsed-but-false.

**Sequencing note (GPU parity coupling).** Every feature above widens the
CPU/GPU SDF divergence (`sdf_generation.compute` knows nothing of biomes or
rivers). Decide explicitly: enable GPU SDF for the CURRENT feature set first
and extend the shader per feature with the parity gate as ratchet, or keep
GPU SDF disabled until iteration-3 worldgen features freeze, then port once.
Recommendation: the latter (port once after WG freeze) — cheaper, and the
far-field GPU SDF horizon path (iteration-3 stream 0) needs only the frozen
result. Material/biome classification stays CPU-side either way (GPU path
produces only SDF + heightmap).

## Gate Cash-Ins

**LOD hysteresis (`T-EF-8` cash-in).**
- Design: asymmetric promote/demote bands per chunk. Replace the raw
  threshold in the LOD decision with: a chunk at LOD `n` only moves to
  `n+1` when `dist > boundary_n * (1 + h)` and only returns to `n` when
  `dist < boundary_n * (1 - h)`, `h ≈ 0.05` (≈10 m at the 192 m
  boundary, comfortably above per-frame camera drift). State: the
  hysteresis needs the chunk's current LOD — implement in
  `get_required_lod_for_chunk` (which already receives the chunk and is
  non-const) by reading `chunk->current_lod` and biasing the boundary,
  leaving `get_lod_level_for_distance` pure for first-assignment of fresh
  chunks. Both the horizontal surface-band path and the 3D path get the
  same treatment (the surface-band rule composes: it changes the distance
  metric, hysteresis changes the comparison).
- Invariants to protect: `EnsureSurfaceReadyNear` per-ring LOD and the
  initial-load path use the raw mapping — fresh chunks get the unbiased
  LOD so save/load and first-arrival remain deterministic; hysteresis only
  suppresses *re*-transitions. LodSeamRisk and LodGround gates must stay
  green (transition skirts already handle adjacent-LOD seams; hysteresis
  can make neighbors disagree by at most the same one level as today).
- Gate ratchet plan: land implementation behind the existing gate, capture
  a fresh `lod-boundary-oscillation.json`, then tighten
  `RuntimeScenarioHarness.cpp:2905-2908` baselines in the SAME PR from
  (0.75 / 240 / 115.0) to measured-plus-margin — expected order
  (≤0.15 trans/chunk/s, ≤30 oscillating chunks, ≤15 total/s; exact
  numbers from the run, with the same noise-honest margin discipline as
  the PerfRegression baseline). The ratchet commit is the deliverable; an
  implementation without tightened numbers does not count as cashed in.

**GPU SDF runtime enablement.**
Stepwise, each step gated:
1. Fix the GPU heightmap divergence: compute the analytic `terrain_h`
   heightmap on CPU (cheap 2D pass) even when the GPU produced the SDF,
   instead of marching the SDF down at integer resolution
   (`RenderPipeline.cpp`/`GenerateChunkData` GPU branch) — removes a
   physics-visible difference before parity is even measured.
2. Make the parity test real: add a live-GPU lane to
   `test_sdf_gpu_cpu_parity.cpp` (GL context via the existing render-test
   scaffolding) dispatching `sdf_generation.compute` for the 9-case corpus
   and comparing against CPU within an epsilon band (float-order
   differences make byte-identity unrealistic; lock max-abs-diff and
   sign-agreement at the isosurface: zero sign flips within |sdf| > eps).
   Extend the corpus with the seed-424242 degenerate-geometry chunk from
   the iteration-3 directives.
3. Port any frozen iteration-3 worldgen features into the compute shader
   (see sequencing note) and re-pass parity.
4. Flip `kEnableExperimentalGpuSdfIntegration` to true and update the
   source-needle assertions (`test_sdf_gpu_cpu_parity.cpp:115`,
   `render_smoke_test.cpp:1047-1219`) in the same change to assert the NEW
   contract: compile-time on, runtime opt-in still default-off via
   `--enable-gpu-sdf-runtime`, GpuSdfRuntimeToggleGate asserting the
   toggle state matrix, GpuSdfCallbackSafetyGate unchanged.
5. Enable runtime-on in one scenario lane (e.g. `auto_world_smoke
   --enable-gpu-sdf-runtime`) with MaterialVisual/LodGround/WaterVisual
   green, plus a world-hash comparison CPU-vs-GPU run before making it a
   default anywhere. Per the No-Deferral Rules, none of this lands without
   its deterministic gate passing first.

**Release perf lane.**
- Build: `cmake --preset release` already exists. Add
  `.forge/scripts/run-release-perf-lane.ps1`: build release, run
  `run_runtime_boot_metrics.ps1` against the release client exe, run the
  PerfRegression scenario set (`auto_world_smoke 300`, `lod_ground_smoke`,
  streaming/traversal scenario) and emit
  `build/release/test-artifacts/perf/perf-baseline.json`.
- Bless a release baseline via `capture-perf-baseline.ps1 -BuildPreset
  release` (median-of-3, same +50%/+25% noise-honest margins and 20 ms
  noise floor as the debug baseline) and teach `Test-PerfRegression` /
  the validator to take the preset parameter so `-Mode PerfRegression`
  runs per-preset against the matching blessed baseline.
- Cadence: keep it a manual-but-scripted lane (one command) run at
  iteration boundaries and before/after each optimization task; debug lane
  stays the per-PR gate (release adds ~full-rebuild cost; making it per-PR
  is not worth it until CI exists — no git remote is even configured).
  Release-only failure classes this catches: NDEBUG-path timing,
  optimizer-dependent ordering, debug-only assert side effects.

## Debt Plan

**UI unsubscribe (use-after-free, fix design).**
- `UIProperty.h`: give `Property<T>` token-based subscriptions —
  `using SubscriptionId = std::uint64_t; SubscriptionId
  Subscribe(PropertyCallback<T>); void Unsubscribe(SubscriptionId);`
  backed by `std::vector<std::pair<SubscriptionId, PropertyCallback<T>>>`
  + monotonic counter (no ABA; erase-by-id; `NotifyObservers` iterates a
  copy or index-based loop so unsubscribe-during-notify is safe).
- `UIComponent.h`: all four bind sites store
  `m_bindings.emplace_back([&property, id]{ property.Unsubscribe(id); });`
  and `UIComponent::~UIComponent` (or `Shutdown`) runs and clears
  `m_bindings`. Lifetime caveat to document: this fixes
  component-dies-before-property (the dangling-`this` UAF); if a Property
  can die before the component, the binding lambda's `&property` capture
  is itself dangling — acceptable now because Properties live on
  long-lived view models, but note it in the header.
- Gate: a unit test creating/destroying a component bound to a property,
  then mutating the property — must not touch freed memory (ASan lane
  catches regression; also assert callback count returns to zero).

**Per-chunk persistence format v2 (versioned migration).**
- Layout: `<save_dir>/chunks/manifest.json`
  (`luminumbra.persistence.world_manifest.v2`: format version, seed,
  preset, chunk index with per-chunk content hash) +
  `<save_dir>/chunks/region/r.<rx>.<rz>.lmr` region files (32x32 chunk
  columns, binary, per-chunk LZ4-compressed records with a small TOC —
  the README already promises LZ4+bit-packing; JSON-per-chunk would be
  death by file count at far-LOD scale). Implementation lives behind the
  existing `write_snapshot()` seam exactly as
  `WorldSaveService.h:21-25` planned; public API (`save_world`,
  `load_world`, `save_dirty_chunks`, `world_hash`) unchanged.
- `world_hash` stability: keep hashing the canonical in-memory snapshot
  serialization (already format-independent —
  `SerializeWorldStreamingStateSnapshotJson` of state, not of the file),
  so v1-vs-v2 hash equality IS the migration gate.
- Versioned migration: `load_world` detects layout — v2 manifest present
  ⇒ v2 reader; legacy `world-state.json` present ⇒ v1 reader, then
  one-time rewrite to v2 + rename v1 file to `world-state.json.v1.bak`
  (no silent delete). Unknown version ⇒ hard error with diagnostics.
- Dirty-save win: `save_dirty_chunks` rewrites only regions containing
  dirty chunks (finally honoring its doc comment), making runtime
  autosave O(edited regions) instead of O(world) — prerequisite for the
  far-LOD store (stream 0), which should read/write the SAME region
  format at coarse LOD (design the region record header with a
  `lod_level` field now so the far-LOD store is a consumer, not a fork).
- Gates: PersistenceRuntimeRoundtrip and ChunkFormatValidationGate
  extended to v2; new migration test: write v1 fixture, load, assert
  `world_hash` identical pre/post migration and on v2 re-load.

**WorldList stubs.** Small, self-contained: persist favorites + last-played
in a client-local JSON (`worlds/saves/ui-state.json` or per-world
`world_info.json` fields), implement the two filters, timestamp formatting,
creation date in `WorldInfo`, loading-state wiring
(`WorldList.cpp:369-470`). Bundle as one task; UiTestBaseline gate covers
regressions.

**Audio doppler/reverb (`AudioSpatialCluster.cpp:332`).** Lowest priority;
recommend explicitly RE-DEFERRING with a one-line scope note instead of
half-building: doppler needs per-source velocity tracking and miniaudio
pitch control; reverb needs environment zones (which want biome data —
natural follow-up AFTER the biome system lands, when "reverb preset per
biome" becomes a data-driven game-content feature). Do not count it as
iteration-3 debt to clear; carry it with a concrete trigger (biomes
landed).

## Task Breakdown

| # | Task | Files (primary) | Deps | Gate |
|---|------|-----------------|------|------|
| T-I3-P4-1 | Engine/game split: planner + snapshot fixtures to data; remove baked `plan.passed` fixture assertion | `ai/InstinctPlanner.{h,cpp}`, `ecs/EntitySnapshot.h`, new `scripts/common/ai/fixtures/grovestrider_hunger.json`, `test/ai/instinct_planner_gate_test.cpp`, `test/network/network_state_hash_gate_test.cpp` | — | InstinctPlannerGate, NetworkStateHash green with relocated fixture |
| T-I3-P4-2 | Rename `AethericFieldDiffusion` → `ScalarFieldDiffusion` (fields/ dir, transition alias); add `data/game/` + path-lint/game-noun grep gate | `aetheric/` → `fields/`, `sources.cmake`, validator script | T-I3-P4-1 | AethericDiffusionGate dual-schema; new split-lint gate |
| T-I3-P4-3 | Canonical preset loader: engine `TerrainPresetLoader`, dedupe 4 parsers, parse biomes/features/materials blocks into extended `TerrainGenParams` (parsed, not yet consumed); unknown-key warnings | `worldgen/TerrainPresetLoader.{h,cpp}` (new), `GameSession.cpp`, 3 test TUs, `SHIELD_WorldSystem.h` | — | Existing snapshot/determinism gates byte-identical; loader unit tests |
| T-I3-P4-4 | Biome system: temperature/humidity batch+sample noises, BiomeTable, data-driven strata/vein material classification replacing `classify_material` + MarchingCubes duplicate | `SHIELD_WorldSystem.{h,cpp}`, `MarchingCubes.cpp`, presets | T-I3-P4-3; consumes panel-1 control noises if landed | Atlas biome layers; MaterialVisual per-biome ROI; no-biome presets byte-identical |
| T-I3-P4-5 | Rivers: heightfield carving via ridged-noise river field, riverbed material rule, water fill at/below sea level | `SHIELD_WorldSystem.cpp` (+ sample path), presets | T-I3-P4-3 (flag), T-I3-P4-4 (bed material) | Atlas river-mask layer; `river_visual_smoke` capture; determinism hash |
| T-I3-P4-6 | Structures core (single-chunk micro-structures only): deterministic placement pass + data-defined stamps; DEFERRABLE | `SHIELD_WorldSystem.cpp`, `data/game/structures/` | T-I3-P4-4 | Placement-determinism artifact gate |
| T-I3-P4-7 | LOD hysteresis implementation + gate ratchet (tighten oscillation baselines in same PR) | `SHIELD_WorldSystem.{h,cpp}`, `RuntimeScenarioHarness.cpp:2905-2908` | — | LodBoundaryHysteresis (tightened), LodSeamRisk, LodGround |
| T-I3-P4-8 | GPU SDF live parity: analytic heightmap on GPU path, real GPU-dispatch parity lane + seed-424242 case | `RenderPipeline.cpp`, `test_sdf_gpu_cpu_parity.cpp`, `sdf_generation.compute` | After WG freeze (T-I3-P4-4/5) per sequencing note | GpuSdfComputeParityGate (live) |
| T-I3-P4-9 | GPU SDF enablement: port frozen features to compute shader, flip flag, update needle tests, runtime-opt-in scenario lane | `RenderPipeline.cpp:60`, `render_smoke_test.cpp`, validators | T-I3-P4-8 | All 3 GPU gates + visual gates + CPU-vs-GPU world hash |
| T-I3-P4-10 | Release perf lane: script + release blessed baseline + preset-aware PerfRegression | `.forge/scripts/run-release-perf-lane.ps1` (new), `capture-perf-baseline.ps1`, `validate-engine-frontier.ps1` | — | PerfRegression(release) |
| T-I3-P4-11 | UIProperty subscription tokens + UIComponent unsubscribe + lifetime test; WorldList stub completion | `ui/core/UIProperty.h`, `ui/core/UIComponent.h`, `ui/components/game/WorldList.cpp` | — | UiTestBaseline + new unsubscribe unit test |
| T-I3-P4-12 | Persistence format v2: region files + manifest, versioned v1→v2 migration, dirty-region incremental save; far-LOD-ready record header | `persistence/WorldSaveService.cpp`, `WorldPersistenceRoundtrip.{h,cpp}` | — (precedes/parallel to stream-0 far-LOD store) | PersistenceRuntimeRoundtrip, ChunkFormatValidationGate v2, migration hash-equality test |

Suggested waves: (1) P4-1/2/3/7/10/11 are independent and parallelizable;
(2) P4-4 → P4-5 → P4-6, P4-12; (3) P4-8 → P4-9 last in the worldgen stream.
Audio doppler/reverb: explicitly re-deferred, trigger = biomes landed.

## Risks

1. **GPU parity vs worldgen velocity (highest).** Each worldgen feature
   widens CPU/GPU divergence; if sequencing isn't enforced (GPU port after
   WG freeze), parity work churns. Mitigation: T-I3-P4-8/9 hard-gated
   behind WG-freeze; the source-needle tests keep the flag pinned
   meanwhile.
2. **Determinism regressions.** Biomes/rivers touch `GenerateChunkData`
   AND `SampleWorldGenLayers`; any drift between the batch and sample
   paths breaks meshing materials and atlas gates subtly. Mitigation: the
   no-feature-flags path must stay byte-identical (locked by the existing
   determinism hash test); every new layer gets an atlas snapshot in the
   same PR.
3. **Persistence migration data loss.** v2 rewrite of live saves; a bug
   eats worlds (hundreds of save dirs exist in `worlds/saves/`).
   Mitigation: v1 file renamed to `.bak`, never deleted; hash-equality
   migration gate; `world_hash` stays format-independent.
4. **Hysteresis vs streaming invariants.** LOD biasing interacting with
   the surface-band rule, `EnsureSurfaceReadyNear`, and budget scheduling
   can starve remeshes or reopen seams. Mitigation: hysteresis only
   suppresses re-transitions (fresh chunks unbiased); LodSeamRisk +
   LodGround must pass in the ratchet PR.
5. **Fixture relocation breaks gate artifact contracts.** Instinct/
   network-hash gates compare serialized artifacts; moving fixture data
   changes checksums if not done atomically. Mitigation: same-commit test
   + fixture moves; keep plan serialization schema unchanged.
6. **Structures scope creep.** Multi-chunk structures are an
   order-of-magnitude harder than single-chunk stamps. Mitigation: scope
   fence in T-I3-P4-6 (single-chunk only, deferrable wholesale).
7. **Split-lint false positives** annoying schema-bump PRs. Mitigation:
   explicit `engine+game-interface` label escape hatch; grep-gate only on
   added lines.
8. **Release lane noise.** Release timings differ enough that debug-derived
   margins may flap. Mitigation: separate blessed release baseline with
   its own median-of-3 + margins; manual cadence until CI exists.
