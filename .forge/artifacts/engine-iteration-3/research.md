# Engine Iteration 3 — Research Synthesis

Synthesis of the four iteration-3 research panels (2026-06-10), against the owner's
Iteration 3 Directives and Architectural Principle
(`.forge/artifacts/engine-frontier/handoff.md`). Panel sources:
`panel-1-world-scale.md` (LEAD), `panel-2-client-server.md`,
`panel-3-characters-gameplay.md`, `panel-4-split-worldgen-debt.md`.

## Consensus

The four panels were researched independently and converged on the same shared
infrastructure four separate times. These convergences are the spine of the
iteration — each is one system serving two streams, and must be designed once:

1. **Panel 1's control noises ARE panel 4's biome inputs.** Panel 1's terrain
   shaping adds continentalness/erosion/peaks-valleys 2D control channels with a
   single `ComputeShapedHeight` source of truth; panel 4's biome system
   explicitly refuses to invent parallel noises and consumes those channels via
   new named fields on `WorldGenLayerSample` (`continentalness`, `erosion`) plus
   batch buffers in `GenerateChunkData`. **Coordination required: the panels
   claim colliding seed offsets** — panel 1 assigns `m_seed+3..7`
   (continentalness/erosion/pv/warp x/z); panel 4 assigns `m_seed+3/+4/+5`
   (temperature/humidity/rivers). A single seed-offset registry must be fixed in
   the plan before either lands (suggested: shaping takes +3..+7 as the lead
   stream; biomes/rivers take +8/+9/+10). Both panels honor the same
   disjoint-seed-offset and batch-path/sample-path-parity conventions, enforced
   by the existing `max_sdf_sample_error < 1e-4` snapshot gate.

2. **Panel 4's persistence-v2 region format IS panel 1's far-LOD store format.**
   Panel 4's format v2 (`<save_dir>/chunks/region/r.<rx>.<rz>.lmr`, 32x32-chunk
   regions, per-chunk LZ4 records, manifest, behind the existing
   `write_snapshot()` seam) deliberately puts a `lod_level` field in the region
   record header "so the far-LOD store is a consumer, not a fork." Panel 1
   independently proposed region-aligned (32x32 chunks = 512 m) far-LOD tiles
   with seed/`params_hash` invalidation and cache-vs-authoritative (edited-flag)
   semantics. These must be co-designed: one region container/addressing scheme,
   two payload kinds (full chunk records at LOD live; packed
   height+material+flags samples at far tiers), one `world_hash` stability
   contract (hash stays over the canonical in-memory snapshot, format-independent
   — v1-vs-v2 hash equality is the migration gate).

3. **Panel 3's fixed tick IS panel 2's server tick.** Panel 3 needs
   `SimulationClock` (accumulator, fixed dt, max-ticks clamp) to host
   OrderedEventBus drain, InstinctSystem, FieldSystem, and Lua; panel 2's
   headless server needs the identical fixed-step loop (physics → world update →
   autosave) with no GLFW pacing. **Rate discrepancy to resolve at design time:**
   panel 3 proposes 1/60 (matches the boot-metrics capture constant,
   `main_client.cpp:1441`); panel 2 proposes 30 Hz (matches WaterSystem's
   documented game tick, `WaterSystem.h:41`). Build one engine-side
   `SimulationClock` with a configurable rate and pick the canonical sim rate
   once — the deterministic gates (eventbus order, pose-sampling checksum,
   server world-hash replay) all key off it. Physics stays variable-dt
   client-side this iteration (panel 3 risk 5); the server ticks physics at the
   fixed rate by construction.

4. **Panel 2's asset-manifest split IS panel 4's engine/game split, mechanized.**
   `GameSession::ValidateWorldConfig` hard-requiring shaders/RML/fonts
   (`GameSession.cpp:138-144`) is both the single functional blocker for a
   headless server (panel 2 finding 1) and exactly the class of client/game
   knowledge the Architectural Principle bans from the engine-generic session
   (panel 2 finding 12, panel 4 split mechanics). One refactor — simulation
   requirements (preset only) in common, caller-supplied required-asset list
   appended by the client — serves both streams, and panel 4's path-lint +
   game-noun grep gate is the ratchet that keeps it fixed.

Further cross-panel agreements:

- **Grovestrider/Aetheric relocations:** panels 3 and 4 independently specify the
  same three moves — `MakeGrovestriderHungerFixture` (and the baked
  `plan.passed` target-name assertion) out of `InstinctPlanner.cpp` into game
  data, `BuildEntitySnapshotFixture` out of `EntitySnapshot.h` into test
  support, and `AethericFieldDiffusion` → `fields/ScalarFieldDiffusion` with a
  transition alias + dual-schema gate acceptance. Both panels flag that the
  gate `.ps1` scripts string-assert engine source ("mossberry_grove") and must
  change atomically with the relocation. Merge into one task cluster.
- **GPU SDF stays gated, ported once after worldgen freeze.** Panel 1 rejects
  GPU SDF as the far-field primary (CPU heightfield store instead) but mirrors
  shaping math into the shader to stop rot (its T12); panel 4 sequences live
  parity + enablement strictly after shaping/biomes/rivers freeze (its T8/T9),
  fixing the GPU heightmap divergence (integer-marched vs analytic `terrain_h`)
  first. These compose into one stepwise GPU SDF plan with the parity test's
  source-needle assertions as the only sanctioned touch points.
- **materials.json becomes the single material source of truth.** Panel 3's T7
  (client material LUT built from `data/common/materials.json`, emission flowing
  JSON → LUT → shader) and panel 4's T4 (table-driven strata/veins replacing the
  duplicated `classify_material` / `GetTerrainMaterialAt` magic numbers)
  converge on the registry; LuminCrystal stays the sanctioned game fixture
  riding a generic emissive-material feature.
- **Determinism discipline is unanimous:** every panel carries explicit
  default-off flags, byte-identical legacy paths, versioned schema/magic bumps
  (`LMS2`, `farlod.region.v1`, `world_manifest.v2`), and same-commit gate
  updates. No panel proposes loosening any existing hash, parity, or snapshot
  contract.

## Emphasis

**World scale & fidelity leads** — owner mandate (Directive stream 0) and the
panel with the visible defect. Within it, the order is: player-view span fix +
`player_view_smoke` gate first (the bug players see; the gate must fail on
current main and flip green), with the SDF-skip optimization paired to protect
the 8192-chunk budget; then the far-LOD region store for 6x+ distance; terrain
shaping runs in parallel (independent of the streaming fixes).

The remaining streams are ordered by dependency leverage, not novelty:

1. **Enabler infrastructure** (highest leverage, smallest code): fixed tick /
   `SimulationClock` (unblocks panel 3's entire gameplay runtime AND panel 2's
   server), asset-manifest split (unblocks headless server AND starts the split
   ratchet), canonical `TerrainPresetLoader` (unblocks shaping presets, biomes,
   rivers; deletes 4-way parser duplication), persistence v2 region format
   (co-designed with — or strictly before — the far-LOD store).
2. **Client/server headless authority** (Directive stream 1): server target in
   the build, `ServerWorldRunner`, `headless_server_smoke` with deterministic
   double-run. Gate-first with meshing ON; the `StreamingProfile` meshing-skip
   optimization comes only after the gate is green.
3. **Characters & gameplay runtime** (Directive stream 3 + carried gameplay):
   skeletal pipeline + skinned G-buffer path, planner/eventbus/field/Lua from
   gate-only to runtime systems — brains + bodies, all hosted on the fixed tick.
4. **Worldgen features** (Directive stream 2): biomes → rivers → (deferrable)
   single-chunk structures, gated through the atlas machinery; sequenced after
   the preset loader and consuming panel 1's control noises.
5. **Split enforcement + gate cash-ins + debt** (carried stream 4): fixture
   relocations, split-lint gate, LOD hysteresis ratchet, release perf lane,
   UI unsubscribe UAF fix, WorldList stubs. GPU SDF enablement last, after
   worldgen freeze.

## World Scale and Fidelity

Distilled from panel 1 (LEAD). Root cause of the player-view defect is fully
evidenced: single-point column surface sampling (F1), one-chunk-per-column
beyond ring 12 + coarse per-cell ownership leaving unowned cells (F2),
±1 vertical band unable to contain >48 m cliffs (F3), and camera-relative
vertical unload evicting peaks >160 m above the player in a load/unload loop
(F4).

Must-fix / build, in panel order (T1–T12), gates carried:

- **Column surface-span streaming fix** (F1/F2/F3): per-column
  `(surface_min_y, surface_max_y)` span cache (5-point sampling), span
  activation at every ring, span-aware `EnsureSurfaceReadyNear`,
  `GetInitialChunkLoadList`, and `get_required_lod_for_chunk`. The
  `25u*25u*3u` expectation in `test_worldgen_layer_snapshots.cpp:767` is
  updated deliberately, never silently.
- **Surface-relative vertical unload exemption** (F4): chunks inside their
  column's span exempt from the `UNLOAD_DISTANCE_UP/DOWN` test; gated by a
  valley-camera/peak-chunk unit test + `chunk_churn` perf baseline.
- **`player_view_smoke` gate**: mountains + default presets, eye-level 360°
  sweep (12 yaw stations + one peak-aimed pitch station), sim-side frustum
  coverage metric (`missing_frustum_surface_chunks == 0`,
  `renderable_frustum_ratio >= 0.98`) and pixel-side below-horizon sky-leak
  (`below_horizon_sky_ratio < 0.005`, `near_black_cluster_count == 0`),
  artifacts per station. Red on current main; the iteration's headline gate.
  Must include the deterministic degenerate-geometry chunk near
  (-120..-160, 180..230), seed 424242 archipelago (owner directive 0b).
- **Live-ring hyper-optimization**: skip SDF + 3D cave noise for step>1 chunks
  (the LOD2 annulus pays ~20 KB + 3D noise per column for a mesher that never
  reads it); lift distance constants into `WorldStreamingConfig`. This pairs
  with the span fix to keep active chunks inside the 8192 budget.
- **Far-LOD region store (DH model, CPU heightfield primary)**: 32x32-chunk
  regions, tiers F1 (4 m, 512–768 m), F2 (8 m, 768–1536 m), F3 (16 m, to
  3072 m, stretch), 6 B packed samples (height + material + water/edited
  flags), shared border rows. `FarLodStore` beside `WorldSaveService`
  (`luminumbra.farlod.region.v1`, seed + `params_hash` invalidation); pristine
  regions are regenerable cache, edited regions are authoritative saves.
  Co-design the container with panel 4's persistence v2 (see Consensus 2).
- **Region mesher + render path + scheduler**: generalized heightfield mesher
  (no per-cell chunk-Y ownership — the F2 problem doesn't exist in the far
  field), 28-B `VoxelVertex` layout preserved, one VBO/IBO per region drawn in
  the existing G-buffer pass after live chunks, region-AABB culling, far water
  sheet, shadow exclusion for F2/F3; ring-diff scheduler on the Normal job
  lane, edited-chunk downsample on unload, 64 MB resident ceiling with LRU
  eviction, telemetry in `StreamingBudgetFrameStats`.
- **`farlod_horizon_smoke` gate**: 6x horizon scenario; 0 missing wanted
  regions after settle; `farlod_resident_bytes < 64 MB`; `gbuffer_gpu_ms`
  delta < 1.5 ms vs baseline; per-(seed, params_hash, tier, region)
  deterministic tile fnv1a hash, identical across pregen and from-live build
  paths on unedited terrain.
- **Terrain shaping** (F7): continentalness/erosion/peaks-valleys + domain
  warp + monotone piecewise-linear splines through a single
  `ComputeShapedHeight` used by `GetTerrainHeightAt`,
  `SampleWorldGenLayers`, and `GenerateChunkData`'s batch loop (the inline
  duplicate dies). `shaping_enabled` defaults false → zero hash drift on
  legacy params, with an explicit legacy-height regression probe. Preset
  `shaping` block is game data; shipped `mountains.json` is version-bumped
  (`schema_rev: 2`); GPU shader mirrored in a separate, parity-test-touching
  task with the gate still closed.
- **Slope-histogram atlas gate**: per-preset percentile bounds — flat p95<15°,
  default walkable(<25°)>0.60, shaped mountains cliff(>60°)<0.08 AND
  normal-land fraction (slope<20°, height in [sea+2, sea+40]) > 0.25 — the
  measurable encoding of "lack of normal land," plus a relief-spectrum
  bimodality check.

Carried risks: live/far boundary seam is the classic DH failure mode (dedicated
gate + fix loop budgeted); span growth on extreme presets vs the 8192 budget
(SDF-skip pairing + ring-20 span clamp); batch-vs-scalar noise ulp drift
(default-off shaping + regression probe); the four per-frame full-map scans in
`update()` are folded into one before any radius is raised; the "6x"
interpretation (radius-12 baseline vs RENDER_DISTANCE) re-derives gate budgets
from the tier table before thresholds are committed.

## Client/Server

Distilled from panel 2. `luminumbra_server` is a dead stub not in the build
graph; `luminumbra_common` is already link-clean headless (zero GL/GLFW/audio
hits; `world_generation_test` proves it daily). The single functional blocker
is the `ValidateWorldConfig` client-asset manifest.

Must-fix / build (panel T1–T8), gates carried:

- **Wire the server target into the build** (add_subdirectory, first-party
  warnings config, `copy_data` for presets); links only `luminumbra_common`.
- **Asset-manifest split**: simulation requirements (preset only) in common;
  client appends shaders/UI/fonts via caller-supplied list. Gate: headless
  `CreateWorld` succeeds in a temp root containing only
  `worlds/atlas/presets/default.json`; existing client gates stay green.
- **`ServerWorldRunner`**: boot mirrors `main_client.cpp` minus GL/UI —
  `LoadWorldState` before any generation (ordering contract,
  `GameSession.h:74-78`), spawn-anchor streaming,
  `EnsureCollisionReadyNear`, fixed-tick loop (physics → world update; water
  ticks inside), autosave + shutdown `SaveWorldState`. **Never registers the
  GPU SDF callback** — CPU SDF is the authoritative generation path.
- **`headless_server_smoke` gate**: `luminumbra.server_tick.v1` artifact
  (tick stats, final `RuntimeChunkStats` with `collision_chunks > 0`,
  `WorldSaveService::world_hash`, in-process determinism double-run with
  `world_hash == world_hash_replay`); new `Test-HeadlessServerTickGate`
  validator mode including static target hygiene (links only common; no
  glfw/glad/miniaudio/imgui/rmlui includes under `src/luminumbra_server/**`).
- **Phased meshing skip**: Phase A ships the gate with render meshing ON
  (correct, wasteful, zero risk to the LOD/seam state machine); Phase B adds
  `StreamingProfile` (skip render/water meshes), re-gates collision on
  generated-heightmap instead of `ChunkState::Ready` (collision is already
  Jolt heightfields from `heightmap_data`, mesh-independent), and fixes the
  WaterSystem `Vec3(0)` camera fallback with a deterministic headless
  water-detail policy. Gate: collision chunk set identical with/without
  meshing.
- **Multi-anchor streaming API** (pre-networking; also serves split-screen/
  spectator) and **in-process loopback transport**: feed
  `NetworkLoopbackInput` frames into the real runner and hash the really
  ticked world per tick behind a minimal `ITransport`, replacing the synthetic
  fixture. Requires a durable-entity-id design (open item — durable ids exist
  only in the snapshot fixture today).

Transport posture: plain sockets later, Steam never this iteration (see
Rejected Proposals). The existing authority/state-hash contracts already pin
the future transport: ordered-reliable input channel keyed
(clientId, tick, sequence); unreliable snapshot channel; 16-hex per-tick hash
exchange as the cheapest cross-machine desync oracle.

Carried risks: real-world determinism is unproven (JobSystem scheduling,
FastNoise SIMD dispatch, Jolt non-determinism) — the double-run is same-process
by design, cross-machine determinism is explicitly a later gate; whole-world
JSON autosave stalls the tick (`wait_for_streaming_jobs`) — acceptable for the
smoke gate, panel 4's persistence v2 is the fix; the meshing-skip refactor
touches pending-mesh persistence semantics, hence the deliberate phasing.

## Characters and Gameplay Runtime

Distilled from panel 3. Nothing skeletal exists anywhere in the pipeline; the
gameplay fixtures (planner, event bus, scalar field, Lua manifest) are
runtime-ready but unhosted — there is no fixed simulation tick.

Must-fix / build (panel T1–T10), gates carried:

- **Skeletal asset import**: `.lmesh` v2 under a NEW magic `LMS2` (the v1
  loader's exact-size check forbids in-place extension — v1 stays
  byte-identical, loader dispatches on magic), joints/weights u8x4 + embedded
  skeleton (≤256 joints); sibling `.lanim` clips keyed by joint name hashes
  (retargetable); `cgltf_skin`/`cgltf_animation` parsing + meshoptimizer remap
  widened for skinned attributes. Gate: skinned round-trip gtest; v1 asset
  still loads.
- **Animation runtime core**: Skeleton/Clip/Pose plain structs, pure
  `SamplePose`/`BlendPoses` free functions — **CPU sampling on the fixed tick,
  GPU skinning via palette SSBO** (GPU-side sampling rejected: breaks the
  determinism gate, needs compute plumbing the passes lack). Gate G1:
  pose-sampling determinism — procedural 4-joint skeleton, ticks 0..120 at the
  fixed rate, 1e-4-rounded palette serialization + fnv1a checksum, identical
  across runs and debug/release presets (scalar, non-SIMD math).
- **Skinned G-buffer rendering**: third stage inside `GBufferPass::execute`
  (`skinned_mesh.vert` + existing `g_buffer.frag`) — lighting/SSAO/shadows for
  free; non-instanced first (avoids the slot 3-6 instance-matrix collision);
  fix the instanced path's dropped rotation and hardcoded `MaterialID = 3u`
  while in the file. Gate G2: `skinned_mesh_visual_smoke` capture (deformation
  proven by differing captures at two clip times + RenderHealth clean).
- **Fixed tick + EventBus hosting**: `SimulationClock` (engine), GameSession
  owns `OrderedEventBus` + tick counter + `TickSimulation()` (Animation →
  Instinct → field budget → Lua → bus drain); main-loop accumulator must not
  disturb existing per-frame visual-gate scenarios. Shared with the server
  (Consensus 3).
- **Data-driven planner runtime**: `InstinctSystem` over EnTT views,
  `NeedsComponent` migrated to the named-need representation (resolving the
  two incompatible forms), `OpportunityComponent`, archetype JSON loader;
  grovestrider fixture relocated to game data with the `.ps1`/gtest/source
  change landing atomically (merged with panel 4's split tasks). Gate G3:
  planner scenario from data, checksum equivalence, AND the inverted
  assertion — engine source contains no content strings.
- **Per-chunk scalar field runtime**: `FieldSystem` over the renamed
  `ScalarFieldDiffusion`, per-streamed-chunk lattices, tick-budgeted
  round-robin diffusion, neighbor halo exchange with cross-chunk conservation.
  Gate G4: 3x3-chunk headless run, 600 fixed ticks, summed conservation error
  ≤ 1e-9 per tick, run-to-run checksum equality.
- **Emissive pipeline**: material LUT built from `materials.json` (emission
  flows JSON → LUT → shader, replacing the hardcoded LUT and the hardcoded
  crystal glow); field aggregate modulates the glow term. Gate: night-emissive
  capture extended to assert field-driven variation. Coordinated with panel
  4's table-driven material classification (Consensus, materials.json item).
- **Lua runtime + hot reload**: sol2-backed `LuaState` binding exactly the
  9 manifest entries against GameSession services (set_block queued to tick
  boundary for determinism); binding-parity check turns the manifest from
  documentation into a contract; `data/scripts/` content dir; reload tears
  down script subscriptions and replays a deterministic self-check. Gates:
  manifest-binding parity gtest + G5 hot-reload gate (v1/v2 payload checksums,
  no duplicate subscriptions). Watch: sol2 under `/W4 /WX` needs pragma
  wrapping early.
- **Character content slice**: one rigged model + idle/walk clips in game
  data, spawned via archetype, planner drives clip choice — the end-to-end
  brains-and-bodies proof consumed by G2.

Carried risks: cross-preset float determinism (mitigated by rounded
serialization + scalar math); attribute slot collision between instanced and
skinned VAOs; per-chunk field memory/CPU under the tick budget; materials.json
LUT unification may need material visual-gate threshold retuning.

## Split, Worldgen Features, and Debt

Distilled from panel 4.

**Split mechanics** (discipline + relocation, not a build rewrite; in-repo
sibling model kept):

- Relocate the grovestrider planner fixture and the baked `plan.passed`
  game-content assertion out of `InstinctPlanner.{h,cpp}` into game-data JSON
  with an `expected` block; move `BuildEntitySnapshotFixture` to test support;
  rename `AethericFieldDiffusion` → `fields/ScalarFieldDiffusion`
  (`luminumbra.fields.scalar_diffusion.v1`) with a transition alias +
  dual-schema gate acceptance, alias dropped at iteration close. All gate
  artifacts/checksums preserved by same-commit moves.
- New **split-lint gate**: (a) path lint failing PRs that touch both `src/**`
  and `{data,worlds,scripts}/**` without an `engine+game-interface` label;
  (b) game-noun grep (`grovestrider|aetheric|lumincrystal|shadowstalker`) on
  NEW lines under `src/` with a small allowlist. This is the owner's "do not
  entrench further" rule as a ratchet. Add `data/game/` for game-only
  tunables.

**Worldgen features** (engine = systems, game = content; all behind the
canonical preset loader):

- **Canonical `TerrainPresetLoader`**: dedupe the 4 copy-pasted preset
  parsers; parse biomes/features/materials blocks into extended
  `TerrainGenParams` (parsed before consumed); unknown-key warnings so dead
  preset keys can never silently reappear. Existing snapshot/determinism gates
  stay byte-identical.
- **Biome system**: temperature/humidity batch + sample noises (disjoint seed
  offsets per the registry in Consensus 1), data-driven `BiomeTable` with the
  `temperate_forest.json` strata/veins block as the schema seed; single
  implicit biome when `biomes` absent — exact backward compatibility;
  table-driven material classification replaces both hardcoded copies
  (`classify_material` + the MarchingCubes fallback). Gates: atlas
  temperature/humidity/biome_id layers; MaterialVisual per-biome ROI;
  no-biome presets byte-identical.
- **Rivers**: noise-guided carving as a HEIGHT modification (the cave-cap
  lesson applied — never a 3D density subtraction), pure function of
  (x, z, seed) with no inter-chunk communication; riverbed material via
  biome-table rule; water fill at/below sea level only. Gates: atlas
  river-mask layer + connectivity metric; `river_visual_smoke`; determinism
  hash.
- **Structures (deferrable wholesale)**: single-chunk micro-structures only,
  deterministic hash-jittered placement stamped through the existing
  post-generation edit path (rides persistence dirty tracking); gate is a
  placement-determinism artifact. Multi-chunk structures explicitly deferred
  to iteration 4 (panel self-fenced).
- **GPU SDF sequencing**: port once after worldgen freeze — fix the analytic
  heightmap divergence, make parity live (GPU dispatch over the 9-case corpus
  + seed-424242 degenerate chunk, max-abs-diff + isosurface sign-agreement),
  port frozen features, then flip `kEnableExperimentalGpuSdfIntegration` with
  needle tests updated in the same change, runtime still opt-in, plus a
  CPU-vs-GPU world-hash run before any default flips.

**Gate cash-ins**:

- **LOD hysteresis**: asymmetric promote/demote bands (h ≈ 0.05) reading
  `chunk->current_lod` in `get_required_lod_for_chunk`; fresh chunks get the
  unbiased mapping (first-arrival determinism preserved); **the ratchet commit
  is the deliverable** — tighten `lod-boundary-oscillation` baselines from
  (0.75 / 240 / 115.0) to measured-plus-margin in the same PR, LodSeamRisk +
  LodGround green.
- **Release perf lane**: `run-release-perf-lane.ps1`, release-blessed baseline
  via `capture-perf-baseline.ps1 -BuildPreset release` (own median-of-3 +
  noise-honest margins), preset-aware `Test-PerfRegression`. Manual-but-
  scripted cadence at iteration boundaries; debug lane stays the per-PR gate.

**Debt**:

- **UI unsubscribe UAF**: token-based `Property<T>::Subscribe/Unsubscribe`
  (monotonic ids, notify-safe iteration); all four `UIComponent` bind sites
  store real unbinders; lifetime caveat documented. Gate: destroy-then-mutate
  unit test, callback count returns to zero.
- **Persistence format v2**: region files + manifest behind `write_snapshot()`
  exactly as the header planned; public API unchanged; `world_hash` stays
  format-independent (v1-vs-v2 equality IS the migration gate); v1 file
  renamed `.bak`, never deleted; `save_dirty_chunks` finally O(edited regions)
  — the prerequisite both for long-running server autosave and the far-LOD
  store (Consensus 2).
- **WorldList stubs**: one bundled task (favorites/last-played persistence,
  filters, formatting, loading state); UiTestBaseline covers it.
- **Audio doppler/reverb**: explicitly re-deferred with a concrete trigger —
  biomes landed (reverb-preset-per-biome becomes a data-driven feature).

## Cross-Panel Dependencies

Ordered list for the planner. "A before B" means B must not start (or must not
merge) until A's gate is green, unless marked co-design.

1. **Fixed tick (`SimulationClock`) before the gameplay runtime AND before the
   server runner** — panel 3's T4 blocks its T5–T9; panel 2's T3 consumes the
   same clock. Resolve the 30 Hz vs 60 Hz rate decision at design time, before
   either consumer lands.
2. **Asset-manifest split (`ValidateWorldConfig`) before headless server boot**
   — panel 2 T2 before T3. Also the first enforcement act of the engine/game
   split.
3. **Player-view span fix (+ vertical unload exemption) before far-LOD store
   seam work** — the live ring must be complete and `player_view_smoke` green
   before the live/far boundary is built and gated; the far side's boundary
   skirt design depends on which live cells exist.
4. **SDF-skip for step>1 chunks lands with or before the span fix in CI
   ordering** — the span fix inflates active chunks; the skip reclaims the
   budget headroom (8192 cap).
5. **Persistence v2 region format before the far-LOD store, OR co-designed in
   one container spec** — region addressing, record header (`lod_level`
   field), and `world_hash` stability decided once; `FarLodStore` is a
   consumer, not a fork.
6. **Control noises (shaping core) before biomes** — biomes consume
   continentalness/erosion via `WorldGenLayerSample`; the seed-offset registry
   (Consensus 1 collision) is agreed before either noise lands.
7. **Canonical `TerrainPresetLoader` before shaping preset schema, biomes,
   rivers, and structures** — one parser gains the `shaping`, `biomes`,
   `features`, and `materials` blocks; the four duplicated parsers die first.
8. **Biomes before rivers' bed-material rule and before the (deferred) audio
   reverb trigger** — rivers can start on the flag/carving independently but
   the riverbed material override keys on the biome table.
9. **Worldgen freeze (shaping + biomes + rivers merged) before GPU SDF live
   parity and enablement** — port once; the source-needle tests keep the flag
   pinned until then. Panel 1's shader shaping mirror (gate still closed) is
   folded into this stream as its first step and remains the only sanctioned
   parity-test touch before enablement.
10. **Fixed tick + EventBus hosting before FieldSystem and Lua runtime**;
    FieldSystem before the emissive field-modulation hookup; materials.json
    LUT unification coordinated with (same design doc as) the table-driven
    material classification.
11. **`headless_server_smoke` green with meshing ON before the
    `StreamingProfile` meshing-skip, before multi-anchor streaming, before the
    loopback transport** — and the loopback transport additionally requires
    the durable-entity-id design (open item).
12. **Skeletal import + animation core before skinned rendering before the
    character content slice**; the content slice also needs the planner
    runtime (G2's capture uses planner-driven clip choice).
13. **Grovestrider/EntitySnapshot/Aetheric relocations before the split-lint
    grep gate turns on** — otherwise the ratchet fails on day one; the
    relocations and their gate-script rewrites land atomically.
14. **LOD hysteresis ratchet is independent but must precede any further LOD
    threshold churn from far-LOD work** — and no scheduler/LOD change lands
    without the streaming telemetry + boundary/seam gates green (standing
    rule).

## Rejected Proposals

Nothing is dropped silently. Items below were rejected or deferred — either by
a panel (recorded here so the decision is on the record) or by this synthesis.

| Proposal | Disposition | Reason |
|---|---|---|
| GPU SDF compute as the far-field primary | Rejected (panel 1) | Far field needs only 2D height+material; CPU heightfield from `GetTerrainHeightAt` is deterministic, headless-CI-testable, and seam-exact with the near field; the GPU path is texture-noise-divergent, sync-readback, parity-gated for good reason. GPU SDF remains a gated near-field experiment, ported once after worldgen freeze. |
| GPU-side pose sampling / compute skinning of poses | Rejected (panel 3) | Breaks the pose-sampling determinism gate; needs compute plumbing the pass architecture lacks. CPU-sample/GPU-skin chosen instead. |
| Separate `SkinnedMeshPass` | Rejected for now (panel 3) | Third stage inside GBufferPass reuses `g_buffer.frag` and gets lighting/SSAO/shadows for free; a separate pass only if forward/transparency needs diverge later. |
| Multi-chunk structures | Deferred to iteration 4 (panel 4, self-fenced) | Cross-chunk stamping + arrival-order independence is order-of-magnitude harder; single-chunk micro-structures with a placement-determinism gate are the iteration-3 fence, and the whole stream defers cleanly if the iteration runs hot. |
| Steam SDK / GameNetworkingSockets transport | Rejected this iteration (panel 2) | Nothing is vendored; GNS drags protobuf/openssl-class deps that don't fit the flat-source vendoring model; Steamworks adds licensing constraints irrelevant to a smoke-tested authority. `ITransport` with in-process loopback now; UDP later; Steam as a third impl if distribution needs it. |
| Real network transport features (prediction/interpolation vs real world, interest management, encryption/auth, lobby/matchmaking, delta-compressed snapshots, entity replication beyond hash exchange) | Deferred (panel 2) | None needed until the headless authority ticks deterministically; loopback contracts only this iteration. |
| Hydraulic river simulation / flow accumulation | Rejected (panel 4) | Breaks the chunk-local generation contract (inter-chunk communication, order dependence); noise-guided heightfield carving is deterministic per (x, z, seed). |
| Flowing above-sea-level river water | Deferred (panel 4) | No water-volume simulation exists for it; milestone is carved beds + fill at/below sea level. |
| Audio doppler/reverb TODO | Re-deferred with trigger (panel 4) | Doppler needs per-source velocity + miniaudio pitch control; reverb wants environment zones that key off biome data — concrete trigger: biomes landed. Not counted as iteration-3 debt. |
| Fixed-dt Jolt physics stepping (client) | Deferred (panel 3) | Changing Jolt stepping is out of scope; client physics stays variable-dt, sim systems tick fixed; the server ticks physics fixed by construction. Flagged as a known time-skew risk. |
| Vegetation from biomes | Deferred (panel 4) | `biome_id` on the layer sample is the hook; actual vegetation is a later iteration. |
| Field-maxima point-light injection | Stretch only (panel 3) | Minimal hookup is field-aggregate glow modulation; point lights only if the iteration has room. |
| Per-PR release perf lane | Rejected (panel 4) | Full-rebuild cost without CI (no git remote configured); manual-but-scripted at iteration boundaries; debug lane stays the per-PR gate. |
| Separate game repo for the split | Rejected (panel 4) | In-repo sibling-directory model with mechanical lint gates is sufficient this iteration. |
| Per-feature GPU shader porting with parity ratchet (alternative to port-once) | Rejected (panel 4 sequencing note) | Each worldgen feature widens divergence; porting per feature churns parity work. Port once after worldgen freeze. Exception: panel 1's shaping mirror lands early (gate closed) purely to stop shader rot, folded into the GPU stream as step one. |
| Widening `WorldSaveService` to host the far-LOD store | Rejected (panel 1) | `FarLodStore` lives beside it so `world-state.json` schema v1 and `world_hash` stay untouched; container unification happens through the co-designed persistence-v2 region format instead. |
| Far-LOD F3 tier to 3 km as a committed target | Kept as stretch (synthesis) | The 6x mandate is met by tiers F1+F2; F3 changes only budgets, not architecture. Gate thresholds are derived for 6x; F3 numbers re-derived if/when committed. |
| `RENDER_DISTANCE`/vertical constants raised directly to 6x via live chunks | Rejected (panel 1, F6) | Radius 72 live is 2.5x the chunk budget, >400 MB SDF, and 4 per-frame full-map scans over ~60 k entries — the far-LOD store is the only viable path. |
