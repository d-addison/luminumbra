# Engine Iteration 3 2026-06-10

## Objective

Make Luminumbra an engine someone can SEE the difference in: 6x+ view distance
via a Distant-Horizons-model far-LOD region store (merged static region meshes
decoupled from live simulation, persisted through a far-LOD-ready region
format), a complete player view (the eye-level missing-chunk/missing-face
defect fixed and pinned by a new 360-degree `player_view_smoke` gate, including
the seed-424242 degenerate-geometry chunk), "normal land" terrain via
continentalness/erosion/domain-warp shaping with a slope-histogram gate, a
headless `luminumbra_server` that ticks a real world deterministically with
zero GL/GLFW/audio as the simulation authority, characters on screen (skeletal
mesh import, deterministic CPU pose sampling, GPU-skinned G-buffer rendering),
the gameplay fixtures promoted to runtime systems on a fixed simulation tick
(planner, event bus, scalar field, Lua bindings with hot reload), and the
engine/game split enforced mechanically — all gate-first, with every existing
determinism, parity, persistence, and perf contract preserved or versioned
deliberately.

## Scope

In scope:

- **World scale & fidelity (LEAD stream):** column surface-span streaming fix +
  surface-relative vertical unload exemption; `player_view_smoke` eye-level
  360-degree gate (mountains + default presets, seed-424242 degenerate chunk);
  live-ring SDF-skip optimization for step>1 chunks paired with the span fix;
  far-LOD region store (CPU heightfield primary, tiers to at least 6x view
  distance), region mesher, G-buffer far render path, ring-diff scheduler with
  eviction budget, edit-driven invalidation and persistence;
  `farlod_horizon_smoke` and far-LOD tile-determinism gates; terrain shaping
  control noises + splines behind default-off `shaping_enabled` with
  slope-histogram atlas gates and shaped presets as game data.
- **Client/server decoupling:** server target wired into the build;
  `ValidateWorldConfig` asset-manifest split (simulation needs preset only;
  client appends shaders/UI/fonts); `ServerWorldRunner` fixed-tick boot/tick/
  save loop; `headless_server_smoke` gate with deterministic double-run and
  static no-GL/GLFW/audio target hygiene; then `StreamingProfile` meshing skip,
  multi-anchor streaming, and in-process loopback transport hashing the real
  ticked world.
- **Characters & gameplay runtime:** `.lmesh` v2 (`LMS2`) skinned format +
  `.lanim` clips through the asset processor; animation runtime core with
  deterministic CPU pose sampling and GPU skinning; skinned stage in
  GBufferPass; `SimulationClock` fixed tick hosting OrderedEventBus,
  data-driven `InstinctSystem` over EnTT, per-chunk `FieldSystem` over the
  renamed `ScalarFieldDiffusion`, materials.json-driven LUT + field-modulated
  emissive hookup, sol2 `LuaState` binding the 9-entry manifest with hot
  reload; one rigged character content slice end-to-end.
- **Split, worldgen features, gate cash-ins, debt:** grovestrider/
  EntitySnapshot fixture relocations to game data and the
  `AethericFieldDiffusion` -> `ScalarFieldDiffusion` rename; split-lint gate
  (path lint + game-noun grep ratchet); canonical `TerrainPresetLoader`
  replacing four duplicated parsers; biome system with data-driven
  strata/veins material classification; rivers via deterministic heightfield
  carving with water fill at/below sea level; single-chunk micro-structures
  (deferrable wholesale); LOD hysteresis implementation with the oscillation
  baseline ratchet tightened in the same PR; GPU SDF live parity + enablement
  strictly after worldgen freeze; release perf lane with its own blessed
  baseline; UI subscription-token unsubscribe fix; persistence format v2
  (region files + manifest, versioned v1->v2 migration, dirty-region
  incremental save, far-LOD-ready record header); WorldList stub completion.

Non-goals for this iteration:

- Multi-chunk structures (single-chunk micro-structures only; the structures
  stream defers wholesale if the iteration runs hot).
- Real network transport beyond in-process loopback contracts: no sockets, no
  client prediction/interpolation against the real world, no interest
  management, encryption/auth, lobby/matchmaking, delta-compressed snapshots,
  or entity replication beyond the durable-id hash exchange.
- Steam SDK / GameNetworkingSockets integration in any form.
- Hydraulic river simulation, flowing above-sea-level water, vegetation
  placement, audio doppler/reverb (re-deferred; trigger = biomes landed).
- Fixed-dt Jolt physics stepping on the client (sim systems tick fixed;
  client physics stays variable-dt).
- GPU SDF as the far-field primary path; GPU-side pose sampling.
- Loosening any existing visual, determinism, parity, or perf threshold to
  pass without an artifact-backed source fix.

## Acceptance Criteria

- `player_view_smoke` passes on the `mountains` and `default` presets: at
  every station of the eye-level 360-degree sweep,
  `missing_frustum_surface_chunks == 0`, `renderable_frustum_ratio >= 0.98`,
  `below_horizon_sky_ratio < 0.005`, and `near_black_cluster_count == 0`, with
  per-station screenshots and a `player_view_analysis.json` artifact. The gate
  demonstrably fails on pre-fix main and passes after the span/unload fixes.
  The seed-424242 archipelago degenerate-geometry chunk near
  (-120..-160, 180..230) is covered and clean.
- `farlod_horizon_smoke` passes at 6x view distance: zero missing wanted far
  regions per frame after settle, `farlod_resident_bytes < 64 MB`,
  `gbuffer_gpu_ms` delta < 1.5 ms versus the blessed baseline, above-horizon-
  only sky in the horizon capture, and live chunks within the 8192 budget.
  Far-LOD tile hashes are deterministic per (seed, params_hash, tier, region)
  and identical between pregenerated and from-live-heightmap build paths on
  unedited terrain.
- The slope-histogram atlas gate enforces the normal-land floor per preset:
  `flat_lands` p95 slope < 15 degrees; `default` p95 < 35 degrees with
  walkable (<25 degrees) fraction > 0.60; shaped `mountains` cliff
  (>60 degrees) fraction < 0.08 AND normal-land fraction (slope < 20 degrees,
  height in [sea+2, sea+40]) > 0.25, emitted into the worldgen atlas
  JSON/HTML.
- `headless_server_smoke` passes: `luminumbra_server` builds, links only
  `luminumbra_common`, contains no glfw/glad/miniaudio/imgui/rmlui includes,
  boots a world from a preset-only root, ticks 300 fixed steps headless with
  `collision_chunks > 0`, writes a save, and emits
  `luminumbra.server_tick.v1` with `deterministic == true`
  (`world_hash == world_hash_replay` across an in-process double-run).
- Pose-sampling determinism (G1) passes: the procedural 4-joint fixture
  sampled over ticks 0..120 produces an identical fnv1a checksum of
  1e-4-rounded palettes across repeated runs and across debug/release presets.
- The skinned render capture (G2), planner scenario gate (G3, fixture loaded
  from game data with engine source free of content strings), field-
  conservation runtime gate (G4, cross-chunk error <= 1e-9/tick), Lua
  manifest-binding parity test, and Lua hot-reload gate (G5) all pass.
- The split-lint gate is live and green: no game nouns
  (grovestrider/aetheric/lumincrystal/shadowstalker) in new lines under
  `src/`, and engine/game PR path separation enforced with the
  `engine+game-interface` escape hatch.
- LOD hysteresis is cashed in: implementation landed AND the
  `lod_boundary_oscillation` baselines tightened from (0.75 / 240 / 115.0) to
  measured-plus-margin in the same PR, with LodSeamRisk and LodGround green.
- Persistence format v2 passes PersistenceRuntimeRoundtrip and
  ChunkFormatValidationGate, and the v1->v2 migration test proves
  `world_hash` equality pre/post migration with the v1 file preserved as
  `.bak`.
- All existing tests stay green: the full debug CTest lane (82+ tests) and all
  22 engine-frontier validator modes pass; meshing-determinism FNV hashes,
  `sizeof(VoxelVertex) == 28`, `max_sdf_sample_error < 1e-4` (shaping on and
  off), GPU parity source-needle assertions (touched only by the sanctioned
  GPU tasks), and the `world-state.json` schema-v1 hash contract (until the
  versioned v2 migration) are preserved.
- PerfRegression holds: `streaming_walk`, `idle_horizon`, `chunk_churn`, and
  `enter_spawn` p50/p95 stay within the blessed
  `.forge/artifacts/engine-frontier/perf-baseline.json` envelope on debug, and
  a release baseline is blessed with `-Mode PerfRegression` runnable per
  preset.

## Verification Commands

```powershell
cmake --build --preset debug --parallel 1
ctest --preset debug --output-on-failure -E "_NOT_BUILT$"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode All
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode PlayerView -SmokeSeconds 60
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode FarLodHorizon -SmokeSeconds 60
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode HeadlessServerTick
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode PerfRegression
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode SplitLint
ctest --preset debug --output-on-failure -R "WorldgenLayerSnapshots|MeshingDeterminism|SdfGpuCpuParityTest|PersistenceRoundtrip|PoseSampling|InstinctPlanner|FieldConservation|LuaManifestBinding"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode LodGround -SmokeSeconds 30
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode WaterVisual -SmokeSeconds 30
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode LodBoundaryHysteresis
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode Endurance300
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/run-release-perf-lane.ps1
forge tasks validate .forge/tasks/engine-iteration-3/dispatch.json
```

(New validator mode names — `PlayerView`, `FarLodHorizon`,
`HeadlessServerTick`, `SplitLint` — and new ctest regexes are binding on the
plan: the implementing tasks must register these exact entry points or update
this spec in the same change.)

## No-Deferral Rules

- No silent determinism-hash, parity, or golden drift — ever. Meshing FNV
  hashes, `sizeof(VoxelVertex) == 28`, `max_sdf_sample_error < 1e-4`, GPU
  parity source needles, persistence `world_hash`, gate artifact checksums,
  and snapshot expectations (including the `25*25*3` initial-load count)
  change only via deliberate, reviewed, versioned bumps in the same commit as
  the change that requires them, with the reason recorded.
- Gate-first for every new system: no far-LOD store, headless server path,
  skinned rendering, planner/field/Lua runtime, biome/river generation,
  structure placement, persistence v2, or GPU SDF enablement lands without its
  deterministic gate existing and passing first. A gate that has never been
  red against the defect it pins does not count.
- The engine/game split is enforced by the new split-lint gate: no game nouns
  in new engine lines, no mixed engine+game PRs without the explicit
  interface label, and the fixture relocations land before the ratchet turns
  on — do not entrench further.
- No scheduler or LOD policy changes without their gates: streaming telemetry,
  LodBoundaryHysteresis, LodSeamRisk, and LodGround must exist and stay green
  through any change to activation, eviction, LOD selection, or budgets; the
  hysteresis cash-in is not complete until its baselines are tightened in the
  same PR.
- The live/far seam must have a dedicated gate before the far-LOD store ships:
  the near/far boundary ring gets explicit pixel-level coverage (near-black
  cluster and sky-leak detection across the boundary) and a region/chunk seam
  determinism check; no far-LOD render path merges while that gate is missing
  or red.
- The vertical-band (column surface-span) fix must not regress the 8192-chunk
  budget: it lands paired with the step>1 SDF-skip optimization in CI
  ordering, and the endurance/memory-watermark gates at the new spans must
  pass before any view-distance constant is raised.
- Carry-forward frontier rules remain in force: no flat-grey sand, wrong
  texture layers, missing heatmaps/screenshots, or draw-count-as-visual-proof;
  any new GL debug error fails the affected lane; LOD holes, dark voids, or
  upload-priority inversions reappearing in gated captures must become failing
  gates and be fixed before closure; warnings-as-errors breaks, shader
  compile/link failures, missing artifacts, and missing validator modes block
  completion and must not be deferred around.
