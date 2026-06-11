# Panel 1 — World Scale: Far-LOD (6x distance), Player-View Coverage, Terrain Shaping

Lead-stream research panel for Luminumbra engine iteration 3. All file references are absolute under `D:\Coding\luminumbra`.

## Current State

**Distance/chunk constants** (`include/luminumbra/core/Types.h:46-60`): chunks are 16^3 m; `RENDER_DISTANCE = 32` (512 m horizontal), `RENDER_DISTANCE_UP = 8`, `RENDER_DISTANCE_DOWN = 16`. `NEAR_FIELD_DISTANCE = 256`, `FAR_FIELD_DISTANCE = 8192` exist but are aspirational — nothing renders past the streamed chunk set.

**Streaming** (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp`):
- `update_chunk_activation` (651-804) walks a horizontal disc of radius `target_radius` and activates, per column, the chunk containing the column-center terrain height (`column_surface_chunk_y`, 206-219), plus `surface_y±1` inside `STREAMING_NEAR_VERTICAL_STACK_RADIUS = 4` and `STREAMING_MID_VERTICAL_STACK_RADIUS = 12` (lines 30-31, 713-719). Beyond ring 12: **one chunk per column**.
- `streaming_radius_for_pressure` (116-128) throttles the effective radius to 20-24 under load; full 32 only when calm. Budgets: `STREAMING_MAX_ACTIVE_CHUNKS_BUDGET = 8192`, `MAX_ACTIVE_CHUNKS = 20000` (27-28), generation/meshing batch `MAX_CHUNKS_TO_PROCESS_PER_FRAME` derived from `BASE_WORK_BUDGET_EQUIVALENT = 192` (15-18), backlog-scaled 2x meshing batches (536-543).
- Unload hysteresis (777-803): `UNLOAD_DISTANCE_XZ = 34`, `UP = 10`, `DOWN = 18` measured **camera-relative in chunk Y**.
- LOD table (`SHIELD_WorldSystem.h:222-226`): LOD0 step 1 to 192 m, LOD1 step 2 to 384 m, LOD2 step 4 beyond. Surface-band chunks select LOD by horizontal distance only (`get_required_lod_for_chunk`, cpp:231-257) to prevent vertical LOD seams.
- `EnsureSurfaceReadyNear` (810-981): synchronous spawn horizon (radius 12 typical), fixed `{-1, 0, +1}` vertical band per column (849), per-ring LOD via `horizon_lod_for_ring` (95-102), X/Z transition skirts.

**Meshing** (`src/luminumbra_common/world/MarchingCubes.cpp`): step 1 = full marching cubes over the 17^3 SDF lattice with a flat per-edge vertex cache (596-610). Step > 1 = `GenerateCoarseHeightfieldTerrain` (365-475): a pure 2D heightfield mesh sampled from `GetTerrainHeightAt`, with **per-cell surface ownership** — a cell is emitted only if its mean surface height falls inside this chunk's own Y span (417-420). `AddBoundaryTransitionSkirts` (748-857) closes X/Z LOD seams with dropped edge skirts + SDF-driven fallback face patches.

**Worldgen** (`SHIELD_WorldSystem.cpp`): `GetTerrainHeightAt` (983-1000) = single FBm simplex (freq/amplitude/offset) + optional island mask. `GenerateChunkData` (1235-1350) batch-generates 2D heightmap noise + 3D cave noise via `GenUniformGrid2D/3D` and **re-implements the same height math inline** (1319-1323); consistency is enforced by the `max_sdf_sample_error < 1e-4` snapshot gate. Caves are surface-capped (40-63). Every generated chunk stores `sdf_data` (17^3 f32 = 19.65 KB) + `heightmap_data` (17^2 f32 = 1.16 KB) — **even chunks that will only ever be meshed via the heightfield path which never reads the SDF**.

**Persistence** (`src/luminumbra_common/persistence/WorldSaveService.{h,cpp}`): single snapshot `<save>/chunks/world-state.json`, schema `luminumbra.persistence.world_state_snapshot.v1` (`WorldPersistenceRoundtrip.cpp:20`), `world_hash` = fnv1a-64 over canonical snapshot bytes, all writes through the private `write_snapshot()` seam (designed for a later per-chunk split).

**GPU SDF**: `res/shaders/sdf_generation.compute` exists but uses pre-baked noise *textures* (lines 50-91) that do **not** match FastNoise output; integration is hard-gated (`RenderPipeline.cpp:60 kEnableExperimentalGpuSdfIntegration = false`) and `test/shield/test_sdf_gpu_cpu_parity.cpp:110-126` asserts the gate text stays in place. Readback is synchronous (`RenderPipeline.cpp:2040`).

**Gating machinery**: rich runtime smoke harness (`src/luminumbra_client/core/RuntimeScenarioHarness.h`) with `lod_ground_smoke` pixel gates (`LodHolePixelStats`: dark-void ratio, background-blue ratio, near-black cluster detection, 135-152), camera-coverage stats (`get_camera_local_coverage_stats`, cpp:1142-1233), streaming telemetry, perf baseline scenarios (`.forge/artifacts/engine-frontier/perf-baseline.json`: idle_horizon p95 7.6 ms, streaming_walk p95 2.5 ms, chunk_churn p95 6.7 ms). Worldgen atlas/snapshot machinery in `test/shield/test_worldgen_layer_snapshots.cpp` (presets loaded from `worlds/atlas/presets/*.json`, per-layer metrics JSON + PPM images + HTML atlas). Meshing determinism FNV hashes in `test/shield/test_meshing_determinism.cpp` (asserts `sizeof(VoxelVertex) == 28`; hashes must stay unchanged). Endurance reference: ~4459 chunks / ~16 MB terrain mesh payload at radius 12 (~3.7 KB mesh per chunk average).

## Findings (file:line evidence)

**F1 — Column surface is sampled at one point.** `column_surface_chunk_y` (`SHIELD_WorldSystem.cpp:213-216`) samples `GetTerrainHeightAt` at the chunk-center only. With the mountains preset (`worlds/atlas/presets/mountains.json`: amplitude 120, 6 octaves, persistence 0.65) terrain height routinely varies by more than 16 m across one chunk footprint, so the true isosurface of a column spans several chunk-Ys while only the center one is classified "surface".

**F2 — One chunk per column beyond ring 12.** `update_chunk_activation` (cpp:711-719) activates `surface_y±1` only inside ring 12; beyond it, exactly one chunk per column. Combined with F1, any cell of a column whose surface lies in a different chunk-Y than the center sample has **no owner chunk streamed**. The coarse heightfield mesher's per-cell ownership test (`MarchingCubes.cpp:417-420`: skip cell when `cell_surface_y` outside `[0, CHUNK_SIZE_Y]`) then guarantees that cell is emitted by *nobody* — a permanent hole. This is the literal mechanism of "loads of unloaded chunks or faces" at eye level.

**F3 — Cliff faces between columns of different surface_y are never meshed at LOD0 either.** For adjacent columns with `surface_y` differing by k>3 chunk-Ys (a >48 m cliff), the marching-cubes isosurface of the cliff wall lives in chunks `y ∈ (surface_y_low, surface_y_high)` of the higher column; those Ys are outside the ±1 band (cpp:713-719), so the wall is sky. The "fill" the player sees through tall mountains is this.

**F4 — Tall peaks are vertically evicted relative to the camera.** Unload test (cpp:786-795): `d.y > UNLOAD_DISTANCE_UP (10)` unloads any chunk more than 10 chunk-Ys above the *camera*. A summit 200+ m above a player standing in a valley is `d.y >= 13` — its surface chunks are unloaded each activation pass and immediately re-added as candidates by the surface scan (711), wasting generation budget in a load/unload loop and leaving holes on big mountains. `RENDER_DISTANCE_UP/DOWN` were tuned for the old "load the whole vertical slab" model and are wrong for surface-band streaming.

**F5 — SDF is generated for chunks that never read it.** `EnsureSurfaceReadyNear` (cpp:883-893) and `dispatch_generation_jobs` (1352-1389) always run `GenerateChunkData` (17^3 SDF + 3D cave noise) before meshing, but step>1 chunks are meshed by `GenerateCoarseHeightfieldTerrain` which samples only `GetTerrainHeightAt`. At radius 32 the LOD2 annulus (ring 25-32) is ~1400 columns paying ~20 KB + a full 3D cave-noise grid each for nothing. This is the single biggest cheap win for 6x scaling of the live ring.

**F6 — Far field cannot be live chunks.** At radius 72 (1152 m), the disc is ~16,300 columns (~21,000 for the square scan the activation loop actually does, cpp:696-721); even at 1 chunk per column that is 2.5x `STREAMING_MAX_ACTIVE_CHUNKS_BUDGET` and >400 MB of SDF; with the F1/F2 span fix it is 3-5x worse. The per-frame O(active_chunks) scans in `update()` (355-374, 415-426, 455-514, 609-628) are also linear in this number — 4 full-map scans per frame. A decoupled far-LOD store is mandatory, exactly as the owner's Distant Horizons reference suggests.

**F7 — Worldgen has no low-frequency structure.** `GetTerrainHeightAt` (983-1000) is one FBm channel: amplitude is global, so "mountains" presets are uniformly jagged everywhere ("lack of normal land"). There is no continentalness/erosion control, no spline, no domain warp. The biome params in preset JSON (`temperature_frequency`, `humidity_frequency`, `rivers_enabled`, `structures_enabled`) are parsed by no code path (`GameSession.cpp:99-109` reads only terrain/caves keys; grep confirms zero consumers).

**F8 — Determinism/parity contracts that must not break silently:**
- `test/shield/test_meshing_determinism.cpp:5-6, 32`: FNV-1a-64 mesh hashes for fixed params + `sizeof(VoxelVertex)==28` static_assert.
- `test/shield/test_sdf_gpu_cpu_parity.cpp:115-125`: source-text assertions that the GPU gate stays closed and the shader keeps the CPU sign convention comment.
- `test/shield/test_worldgen_layer_snapshots.cpp:646,933`: `max_sdf_sample_error < 1e-4` couples `SampleWorldGenLayers` to `GenerateChunkData`'s inline math; line 767 hardcodes `25u*25u*3u` for the initial load list.
- `WorldPersistenceRoundtrip.cpp:20`: snapshot schema v1 + `world_hash` over canonical bytes.

## Far-LOD Architecture Proposal

**Recommendation: CPU heightfield region store as primary; GPU SDF stays an experimental near-field generator, not the far field.** Rationale: the far field only needs 2D surface height + material (terrain is a heightfield away from caves; caves are invisible at >400 m). `GetTerrainHeightAt` is a pure, deterministic, SIMD-batchable function — building far tiles from it is faster than a GPU round trip, is testable in headless CI (the GPU path is not — it is parity-gated for good reason, F8), and guarantees the far field matches the near field *exactly* at the seam because both sample the same function. The GPU SDF compute path (texture-noise, sync readback, no parity) would make the far field diverge visually from the near field.

### Tiering and data layout

Region = 32x32 chunks = 512x512 m, aligned to `region_coord = floor(chunk_coord / 32)`. Tiers (DH-style, each tier halves resolution):

| Tier | Sample step | Annulus (6x target) | Samples/region | Bytes/region (data) | Mesh verts/region | Mesh bytes/region |
|------|------------|----------------------|----------------|---------------------|-------------------|-------------------|
| Live | 1-4 m (existing LOD0-2) | 0-512 m (radius 32 chunks) | n/a (chunks) | n/a | n/a | n/a |
| F1   | 4 m  | 512-768 m  | 129x129 | ~100 KB | ~16.6 k | ~466 KB |
| F2   | 8 m  | 768-1536 m | 65x65   | ~25 KB  | ~4.2 k  | ~118 KB |
| F3   | 16 m | 1536-3072 m (stretch, "at least 6x") | 33x33 | ~6.5 KB | ~1.1 k | ~30 KB |

Per-sample record (6 B packed): `f32 height` + `u8 material_id` (from `classify_material` logic at depth 0) + `u8 flags` (bit0 water-at-or-above-sample → far water sheet, bit1 edited-by-player). 129x129 (=(512/4)+1, shared border row with the neighbor region so region meshes stitch without skirts).

**The 6x math (radius 72 chunks, 1152 m view):**
- Naive live chunks: ~21,000 columns x ~3 chunks (with the F1 span fix) x 19.65 KB SDF ≈ **1.2 GB SDF + ~180 MB mesh + 4 per-frame full-map scans over ~60 k entries → impossible** (budget is 8192 chunks).
- Proposed: live disc radius 32 stays ≈ today's cost (and gets *cheaper* via F5: LOD2 ring skips SDF). F1 annulus 512-768 m intersects ~12-16 regions → ~1.6 MB data + ~7 MB mesh; F2 annulus 768-1152 m intersects ~16-24 regions → ~0.6 MB data + ~2.8 MB mesh. **Far-field total: < 15 MB resident, ~30-40 draw calls (one per region), < 200 k far-field triangles.** Extending to 3 km (F3) adds ~80 regions ≈ +0.5 MB data +2.4 MB mesh +80 draws — still trivial. Column-walk cost: far field has *no per-column work per frame*; the scheduler walks regions (tens), not columns (tens of thousands).

### Disk format via WorldSaveService

New files `<save_dir>/farlod/t{tier}/r.{rx}.{rz}.bin`, binary, little-endian:
header `{ magic "LUMF", schema u32 = 1 ("luminumbra.farlod.region.v1"), tier u8, rx i32, rz i32, seed i32, params_hash u64 (fnv1a over the TerrainGenParams canonical bytes), payload fnv1a u64 }` + packed samples. On load, a `params_hash`/seed mismatch invalidates the tile (regenerate). Implementation mirrors `WorldSaveService::write_snapshot` (atomic-ish trunc write, create_directories, error vector); add `FarLodStore` beside `WorldSaveService` rather than widening it, so `world-state.json` schema v1 and `world_hash` are untouched (F8). Edited regions (flag bit1) are persisted on save; pristine pregenerated regions may be persisted lazily or treated as cache (regenerable — recommend cache semantics: only edited regions are authoritative saves).

### Build pipeline

1. **Background pregeneration (primary):** a `FarLodScheduler` ticked from `SHIELD_WorldSystem::update` computes the wanted region set per tier from camera position (Chebyshev rings, nearest-first), diffs against resident set, and dispatches build jobs on the **Normal** job lane (below hole-fill High lane). A build job = `GenUniformGrid2D` over the region grid (terrain + island channels — reuse the exact node graph from `reinitialize_noise`) + material classification + mesh build. Cost: 129^2 = 16.6 k samples ≈ ~2 ms/region single-threaded; the entire 6x far field pregenerates in well under a second of background work. No disk dependency for pristine terrain.
2. **From live chunks on visit/evict:** when a chunk with `is_voxel_data_dirty()` history (player edits) unloads, downsample its `heightmap_data` (already 17x17 f32, `Chunk.h:49`) into the covering F1 region tile, set edited flags, mark region mesh + coarser tiers dirty (F2/F3 rebuilt by downsampling F1 2:1). This is what keeps player-made mountain-carving visible at distance, DH-style.

### Merged-mesh rendering path

Reuse `GenerateCoarseHeightfieldTerrain`'s algorithm generalized to a region tile (it is already a pure heightfield mesher; lift it out of the chunk-Y ownership constraint — a region mesh owns its full vertical extent, so the per-cell ownership problem (F2) does not exist in the far field at all). Output `VoxelVertex` (keep 28-B layout; determinism static_assert stays valid) into one VBO/IBO per region, world-space-relative-to-region-origin, uploaded once. Render inside the existing G-buffer pass (`RenderPipeline.cpp:1041-1044`) **after** live chunks, same shader (`g_buffer.vert/frag` already consume VoxelVertex + material id), frustum-culled by region AABB through the existing `HierarchicalCuller` or a flat 40-entry loop (40 AABBs needs no hierarchy). Depth test handles live/far overlap; to avoid z-fighting and double-draw, the scheduler *skips* regions fully inside the live radius and the near/far boundary ring gets a 1-cell skirt dropped at the region edge facing the live field (reuse `AddBoundaryTransitionSkirts`' edge-skirt construction). Far water: regions with water flags emit a flat sea-level quad batch drawn in the water pass at far-LOD (no sim). Shadow pass excludes F2/F3 (beyond shadow range) — F1 optional.

### Eviction / budget policy

- Wanted set per tier = annulus ± 1-region hysteresis; resident regions outside wanted set are freed (GPU buffers deleted; edited tiles flushed to disk first).
- Byte budget: 64 MB resident far-LOD ceiling (≈ 4x the 6x requirement — headroom for 3 km), LRU-evict farthest-first when exceeded.
- Telemetry: extend `StreamingBudgetFrameStats` with `farlod_regions_resident/building/dirty/draws` so the existing telemetry artifact path (`WriteStreamingTelemetry`) gates it.

## Player-View Coverage Fix Design

### Root cause (concrete)

Three interacting defects, all evidenced above: (F1) center-point column sampling under-spans steep columns; (F2) one-chunk-per-column beyond ring 12 + coarse per-cell ownership (`MarchingCubes.cpp:417-420`) leaves cells with no owner → horizon holes at eye level even on moderate slopes; (F3) ±1 band cannot contain >48 m cliffs → see-through mountain walls at LOD0; (F4) camera-relative vertical unload (`cpp:786-795`, `UNLOAD_DISTANCE_UP=10`) actively evicts peak surface chunks >160 m above the player and re-candidates them every pass — churn plus permanent gaps on exactly the "really jagged mountains" the owner sees. A player at eye level doing a 360° turn in the mountains preset sees: missing cliff walls (F3), horizon cells dropped (F2), and flickering summits (F4).

### Smallest load-band fix that closes cliff faces

1. **Column surface span.** Replace the single cached `surface_y` with a cached `(surface_min_y, surface_max_y)`: sample `GetTerrainHeightAt` at the column's 4 corners + center (5 calls, cached per column for the seed/params lifetime like today, `m_column_surface_chunk_y_cache` → span cache). `span = [floor(min_h/16) - 1, floor(max_h/16) + 1]`.
2. **Activate the span at every ring** in `update_chunk_activation` (replace 711-719): surface candidates = all Ys in span (vertical_rank = distance from center surface_y so the sort still drains center-out); keep the extra ±1 stack inside ring 4/12 as today. Flat terrain stays 1-3 chunks/column (zero cost regression); only steep columns widen, which is precisely where the holes are. Also fixes F2 because every coarse cell's owner chunk-Y is inside the span by construction (cell surface heights lie between the corner samples ± noise variation absorbed by the ±1 margin).
3. **Same span in `EnsureSurfaceReadyNear`** (replace the fixed `{-1,0,1}` at cpp:849) and in `GetInitialChunkLoadList` (cpp:295-306) — note `test_worldgen_layer_snapshots.cpp:767` (`EXPECT_EQ(initial_chunks.size(), 25u*25u*3u)`) must be deliberately updated to a span-derived expectation.
4. **Surface-relative unload exemption** (cpp:786-795): a chunk whose Y is inside its column's span is exempt from the `d.y > UNLOAD_DISTANCE_UP / < -DOWN` test (XZ test unchanged). This kills the F4 eviction loop with a two-line condition.
5. `get_required_lod_for_chunk`'s surface-band test (cpp:249) widens from `|y - surface_y| <= 1` to "y inside span" so the whole cliff face keeps single-LOD-per-column behavior (preserving the vertical-seam invariant documented at cpp:236-245).

Cost estimate: in the mountains preset the mean span is ~2-4 chunks (amplitude 120 → ±2σ ≈ 5 chunk-Ys spread across the world, but per-column footprint spans are slope-bounded); expect active chunks at radius 12 to grow from ~4459 to ~6-8 k — inside the 8192 budget, and the F5 optimization (skip SDF for step>1) reclaims far more memory than the span adds.

### `player_view_smoke` gate (eye-level 360 sweep)

New scenario in `RuntimeScenarioHarness` (pattern-match `lod_ground_smoke`, registered in `main_client.cpp`'s scenario dispatch):
- Setup: world preset `mountains` (worst case; run `default` as second config), spawn at `GetTerrainHeightAt + 1.95` (eye height, `GameSession.cpp:25`), `EnsureSurfaceReadyNear(radius 12, collision 4)`, then let streaming settle (reuse readiness criteria: `near_field_renderable` + renderable floor).
- Sweep: 12 yaw stations (30° steps), ~1 s settle each; pitch 0 (eye-level horizon) plus one +25° station aimed at the highest visible peak (use the column-span cache to find it).
- **Metric A — frustum chunk coverage (sim-side):** extend `get_camera_local_coverage_stats` (or add `get_frustum_surface_coverage_stats(camera, frustum_planes, max_distance)`) to iterate columns within the live radius whose span-chunk AABBs intersect the frustum and count `missing / not-renderable` owner chunks. Gate: `missing_frustum_surface_chunks == 0` and `renderable_frustum_ratio >= 0.98` at every station after settle.
- **Metric B — sky-through-terrain (pixel-side):** per station capture backbuffer (existing `WriteBackbufferPpm`); detection = `background_blue` classifier from `LodHolePixelStats` (`RuntimeScenarioHarness.h:135-152`) restricted to the **below-horizon ROI**: bottom 45% of the frame at pitch 0 (at eye level over loaded terrain, everything below the horizon line must be geometry). Gate: `below_horizon_sky_ratio < 0.005` and `near_black_cluster_count == 0` per station. Sky legitimately visible between distant peaks sits above the horizon row and is excluded by construction.
- Artifact: `player_view_analysis.json` (per-station rows: yaw, coverage stats, pixel stats, screenshot file) + screenshot index, written like `WriteLodGroundVisualAnalysis`.
- This gate is the acceptance test for the span fix (run before/after: it must fail on current main against the mountains preset and pass after T1/T2).

## Terrain Shaping Design

### Control-noise stack grafted onto the existing pipeline

Minecraft-1.18-style three control channels, all 2D, all FastNoise nodes built in `reinitialize_noise()` with fixed seed offsets (current usage: seed = terrain, +1 = caves, +2 = island; extend deterministically):

| Channel | Node | Freq (default) | Seed |
|---|---|---|---|
| continentalness `c` | Simplex FBm 3 oct | 0.0008 | `m_seed + 3` |
| erosion `e` | Simplex FBm 3 oct | 0.0015 | `m_seed + 4` |
| peaks/valleys `pv` | Ridged simplex 2 oct | 0.004 | `m_seed + 5` |
| domain warp x/z | Simplex | `warp_frequency` | `m_seed + 6 / + 7` |

Height function (one shared helper used by *every* consumer — this is the determinism keystone):

```
warp   = warp_amplitude * (warp_x(p), warp_z(p))
q      = p + warp                       // warped sample point for detail + pv only
base   = spline_c(c(p))                 // continental elevation: ocean shelf / coast / inland plateau
amp    = spline_e(e(p))                 // erosion: 1.0 (rugged) .. 0.05 (flattened plains)
detail = fbm(q) * base_amplitude        // existing channel, unchanged node graph
ridge  = spline_pv(pv(q)) * peaks_amplitude * max(0, 1 - e_remapped)  // peaks only where erosion is low
h      = height_offset + base + amp * detail + ridge
```

Splines = monotone piecewise-linear control points evaluated with plain lerp (no `glm::smoothstep` divergence risk between CPU paths); points live in preset JSON. This produces exactly the owner's ask: large flat/rolling "normal land" where erosion is high, with mountains confined to low-erosion + high-PV bands instead of everywhere.

### Determinism + parity contract preservation

- **Single source of truth:** extract `ComputeShapedHeight(x, z)` (and a batched `ComputeShapedHeightGrid` using `GenUniformGrid2D` for the 3 control channels + 2 warp channels) and call it from `GetTerrainHeightAt` (cpp:983), `SampleWorldGenLayers` (cpp:1002), and `GenerateChunkData`'s batch loop (cpp:1319-1323, replacing the inline duplicate). The snapshot gate `max_sdf_sample_error < 1e-4` (`test_worldgen_layer_snapshots.cpp:646`) then enforces the consistency automatically. Caution: the batch path must call the *same* scalar combine code on pre-batched noise values, not a re-derivation.
- **`shaping_enabled` defaults to `false`** in `TerrainGenParams` (`SHIELD_WorldSystem.h:19-34`). With it off, the height function is bit-identical to today: meshing-determinism FNV hashes (`test_meshing_determinism.cpp`), parity-corpus params (`test_sdf_gpu_cpu_parity.cpp:32-46`), and existing layer-snapshot params all keep shaping off → **zero silent hash drift**. New worlds opt in via preset JSON.
- **GPU shader:** mirror the shaping math in `sdf_generation.compute` behind a `u_shapingEnabled` uniform in the same change that touches the parity test's source-text assertions (`test_sdf_gpu_cpu_parity.cpp:115-125`) — the gate stays closed (`kEnableExperimentalGpuSdfIntegration = false`), but the shader must not rot further from the CPU reference. Flag this task as parity-test-touching.
- **Persistence:** saved worlds carry voxel data in the snapshot, so existing saves render unchanged regardless of params; only *new* worlds with shaped presets differ. `world-state.json` schema is untouched. Far-LOD tiles embed `params_hash`, so shaping changes auto-invalidate stale far tiles.

### Preset JSON (game data) additions

Under `generation_params.terrain.shaping` (parsed in `GameSession.cpp::LoadTerrainParamsFromPreset` and `test_worldgen_layer_snapshots.cpp::LoadPresetParams`, both of which currently ignore unknown keys → backward compatible):

```json
"shaping": {
  "enabled": true,
  "continentalness_frequency": 0.0008,
  "erosion_frequency": 0.0015,
  "peaks_frequency": 0.004,
  "peaks_amplitude": 90.0,
  "domain_warp_amplitude": 30.0,
  "domain_warp_frequency": 0.006,
  "continental_spline": [[-1.0,-40],[-0.3,-12],[-0.1,2],[0.3,14],[1.0,42]],
  "erosion_spline":     [[-1.0,1.0],[0.0,0.55],[0.6,0.18],[1.0,0.05]],
  "peaks_spline":       [[-1.0,0.0],[0.4,0.05],[0.8,0.45],[1.0,1.0]]
}
```

Ship updated `mountains.json` (shaped: dramatic but *localized* ranges, plains between) and a new `highlands.json`; the unused `biomes`/`rivers_enabled`/`structures_enabled` keys stay documented-as-reserved (out of scope this iteration; note them in the preset README).

### Slope-histogram gate via the atlas machinery

Extend `test_worldgen_layer_snapshots.cpp::AuthoredPresetAtlasHasSaneSpawnAndCleanTopology` (904-953): per preset, sample a 256x256 grid at 4 m via `GetTerrainHeightAt`, compute per-cell slope `atan(|∇h|)`, write histogram + percentiles into the atlas JSON/HTML row, and gate:
- `flat_lands`: p95 slope < 15°; `default`: p95 < 35°, walkable fraction (<25°) > 0.60; `mountains` (shaped): cliff fraction (>60°) < 0.08 **and** "normal land" fraction (slope < 20° **and** height in [sea+2, sea+40]) > 0.25 — the direct, measurable encoding of "lack of normal land".
- Plus a relief-spectrum sanity check: height histogram of shaped `mountains` must be bimodal-ish (plains mode + peaks mode), approximated as `p50_height - p10_height < 0.35 * (p95 - p10)`.

## Task Breakdown

Order = dependency order. ⚠ = endangers a determinism/parity/persistence contract; the gate column is the proof.

| # | Task | Files | Depends | Gate proving it | Contract risk |
|---|------|-------|---------|-----------------|---------------|
| T1 | **Column surface-span streaming fix** (F1/F2/F3): span cache, span activation at all rings, `EnsureSurfaceReadyNear` + `GetInitialChunkLoadList` + `get_required_lod_for_chunk` span-aware | `SHIELD_WorldSystem.{h,cpp}` | — | New unit test (steep synthetic params: every coarse cell has a streamed owner chunk); `player_view_smoke` (T3) flips red→green; `SpawnReadyNeighborhoodMeshesBroadSurfaceArea` floors | ⚠ `test_worldgen_layer_snapshots.cpp:767` expects `25*25*3` — update **deliberately** to span-derived count. No hash risk (mesher untouched) |
| T2 | **Surface-relative vertical unload exemption** (F4) | `SHIELD_WorldSystem.cpp:777-803` | T1 (span) | Unit test: valley camera, peak chunk `d.y=+18` stays loaded; `chunk_churn` perf scenario p95 within baseline; telemetry `unloaded_chunks == 0` steady-state | none |
| T3 | **`player_view_smoke` scenario + gate** (360° yaw sweep, frustum coverage metric, below-horizon sky-leak pixel gate, artifacts) | `RuntimeScenarioHarness.{h,cpp}`, `main_client.cpp`, gating script under `test/tools` | T1,T2 | Itself: 12 stations x (missing_frustum_chunks==0, sky_ratio<0.005, near_black_clusters==0) on `mountains` + `default` | none |
| T4 | **Live-ring hyper-optimization**: skip SDF + 3D cave noise for chunks whose required LOD step>1 (F5); lift `RENDER_DISTANCE`/stack radii/budgets from `Types.h` consts into a `WorldStreamingConfig`; scale `streaming_radius_for_pressure` thresholds | `Types.h`, `SHIELD_WorldSystem.{h,cpp}`, `main_client.cpp` | T1 | Perf baseline: `streaming_walk`/`idle_horizon`/`chunk_churn` p95 within blessed baseline; runtime stats `terrain_payload_bytes` + process watermark reduced at radius 12; LOD0 promotion still generates SDF (covered by `LodRemeshKeepsPreviousMeshRenderableWhilePending`) | Step-1 mesher untouched → FNV hashes safe |
| T5 | **Far-LOD region store core**: `FarLodRegion` (tier/packed samples), pure builder from `GetTerrainHeightAt` grid, binary format `luminumbra.farlod.region.v1` (+seed/params_hash), `FarLodStore` save/load beside `WorldSaveService` | new `persistence/FarLodStore.{h,cpp}`, `world/FarLodRegion.{h,cpp}` | — | Unit: roundtrip + per-seed deterministic tile fnv1a hash; `world_hash`/`world-state.json` byte-identical before/after (persistence suite) | ⚠ new on-disk format — versioned from day 1; must NOT touch snapshot schema v1 |
| T6 | **Region mesher**: generalize `GenerateCoarseHeightfieldTerrain` to NxN region tiles (shared border rows, no per-cell chunk-Y ownership), near/far boundary skirt | `MarchingCubes.{h,cpp}` or new `world/FarLodMesher.cpp` | T5 | Unit: mesh metrics (0 degenerate/invalid/bad-normals via snapshot-test helpers); seam capture in T3 harness shows `near_black_cluster_count == 0` across the live/far boundary | ⚠ keep `VoxelVertex` 28-B layout (`test_meshing_determinism.cpp:32`); chunk mesher itself untouched |
| T7 | **Far-LOD render path**: per-region VBO/IBO, G-buffer pass draw after live chunks, region AABB frustum cull, far water sheet, shadow exclusion for F2/F3 | `RenderPipeline.{h,cpp}`, gbuffer/water pass files | T6 | `lod_ground_smoke` horizon capture extended (background_blue above-horizon only); `gbuffer_gpu_ms` delta < 1.5 ms vs baseline; new `farlod_draws` telemetry | none |
| T8 | **Far-LOD scheduler**: wanted-set ring diff per tier, background pregen jobs (Normal lane), build-from-live-heightmap on edited-chunk unload, eviction (hysteresis + 64 MB budget), telemetry in `StreamingBudgetFrameStats` | `SHIELD_WorldSystem.{h,cpp}` or new `systems/FarLodSystem.{h,cpp}` | T5,T6,T7,T4 | New endurance scenario at 6x horizon: after settle, 0 missing wanted regions per frame, memory watermark gate, `idle_horizon` p95 within budget | none |
| T9 | **Edit→far-LOD invalidation + persistence**: dirty-region flagging from voxel edits, edited-tile flush on save, coarser-tier cascade | T8 files + `FarLodStore` | T8 | `persistence_roundtrip_smoke` extension: edit a cliff, save, reload, far-LOD tile hash reflects edit; pristine regions remain cache (absent from save) | ⚠ save-dir contents grow — gate asserts `world-state.json` hash unchanged |
| T10 | **Terrain shaping core** (F7): control noises + splines + domain warp in shared `ComputeShapedHeight`, batch grid path, `TerrainGenParams` fields default-off | `SHIELD_WorldSystem.{h,cpp}` | — | Layer-snapshot test: new `06_shaping` snapshot layers + delta vs base; `max_sdf_sample_error < 1e-4` with shaping ON; `GenerationIsDeterministicWithSameSeed` with shaping ON | ⚠ FNV mesh hashes + parity corpus safe **only** because default-off — add an explicit regression test asserting legacy params produce the pre-change height at probe points |
| T11 | **Preset schema + shaped presets + slope-histogram gate** | `GameSession.cpp`, `worlds/atlas/presets/*.json`, `test_worldgen_layer_snapshots.cpp` | T10 | Atlas slope-histogram gates per preset (flat p95<15°, default walkable>0.60, mountains cliff<0.08 + normal-land>0.25); `ValidateWorldConfig` parse tests | ⚠ changes shipped `mountains.json` → new-world heights change (saved worlds unaffected: voxels persisted). Version-bump the preset (`"schema_rev": 2` in JSON) |
| T12 | **GPU SDF shaping mirror (kept gated)** + parity-test text update | `res/shaders/sdf_generation.compute`, `test_sdf_gpu_cpu_parity.cpp` | T10 | Parity test still passes with updated assertions; gate constant remains `false` | ⚠ touches the parity test's source-text assertions — must be its own reviewed task, never folded into T10 |

Suggested sequencing: T1→T2→T3 (player-view defect closed and gated first — it is the visible bug), T4 in parallel, then T5-T9 (far-LOD lead stream), T10-T12 (shaping) parallel to T5+.

## Proposed Gates

1. **player_view_smoke** (new, T3): mountains + default presets, eye-level 360° sweep, per-station `missing_frustum_surface_chunks == 0`, `below_horizon_sky_ratio < 0.005`, `near_black_cluster_count == 0`. Red on current main; the iteration's headline acceptance gate.
2. **farlod_horizon_smoke** (new, T8): 6x horizon scenario; 0 missing wanted far regions after settle; above-horizon-only sky; `farlod_resident_bytes < 64 MB`; `gbuffer_gpu_ms` delta < 1.5 ms.
3. **farlod determinism** (T5): per-(seed, params_hash, tier, region) tile fnv1a hash fixed across rebuilds and across build-paths (pregen vs from-live-heightmap on unedited chunks must agree exactly — they sample the same `heightmap_data`/`GetTerrainHeightAt` values).
4. **Perf-baseline non-regression** (T4, T8): `streaming_walk`/`idle_horizon`/`chunk_churn`/`enter_spawn` p50/p95 within the blessed `.forge/artifacts/engine-frontier/perf-baseline.json` envelope (existing bless flow).
5. **Slope-histogram atlas gate** (T11): per-preset percentile bounds + "normal land fraction" floor (the measurable encoding of the owner's terrain complaint), emitted into the atlas HTML.
6. **Determinism tripwires** (standing): meshing FNV hashes unchanged (T4/T6/T10), `sizeof(VoxelVertex)==28`, parity-test source assertions (only T12 may touch), `world-state.json` schema v1 + `world_hash` byte-stability (T5/T9), `max_sdf_sample_error < 1e-4` with shaping on and off (T10).
7. **Memory/endurance** (T8): endurance run at 6x horizon holds live chunks ≤ 8192 budget and process watermark within configured `memory_watermark_mb`.

## Risks

1. **Span fix inflates active chunks on extreme presets.** Mountains amplitude 120 could push spans to 5-8 chunks on cliff columns; if the radius-12 endurance count (~4459) grows past ~8 k, generation backlog telemetry (`max_deferred_age_frames`) regresses. Mitigation: T4's SDF-skip for coarse chunks lands with/before T1 in CI ordering; clamp span growth beyond ring 20 to the cell-owner minimum (only Ys that actually own coarse cells).
2. **Near/far boundary seam.** Live LOD2 chunks (step 4, per-cell ownership) meeting F1 regions (4 m grid) sample identical heights, but the live side drops cells near chunk-Y borders — the boundary skirt must cover the live side's dropped cells, not just the resolution change. The T3/T6 pixel gates (near-black clusters at the boundary ring) are the detection; budget a dedicated fix loop here, it is the classic DH failure mode.
3. **Far-LOD vs player edits divergence.** Pristine-region-as-cache means a params/seed hash mismatch silently regenerates terrain that an *old* far tile showed differently; edited tiles must never be regenerated. The edited-flag bit plus T9's roundtrip gate covers it, but cross-version saves (params change between sessions) need the `params_hash` check to refuse-and-rebuild *unedited* tiles only.
4. **Determinism drift in shaping batch path.** `GenUniformGrid2D` and `GenSingle2D` can differ in the last ulp on some FastNoise versions/SIMD levels; the existing `max_sdf_sample_error < 1e-4` gate tolerates it, but the meshing FNV hashes do not tolerate *any* drift on legacy params — hence the explicit legacy-height regression probe in T10 and default-off shaping.
5. **Frame-time scans.** Even with far-LOD, T1 grows the live map; `update()` does 4 full-map scans per frame (cpp:355, 415, 455, 609). If profiling shows regression, fold the four scans into one (mechanical change, no behavior delta) before raising any radius.
6. **Scope: 6x interpretation.** Radius 72 (6x the radius-12 spawn/endurance baseline) is delivered by F1+F2; if the owner means 6x `RENDER_DISTANCE` (3 km), only the F3 tier and its eviction budget change — the architecture is the same, but the gates' numeric budgets (draws, memory) should be re-derived from the table above before committing thresholds.
7. **Preset churn.** T11 changes shipped preset content; any tooling or doc that hardcodes current spawn heights for mountains-type worlds (e.g., scenario camera placements in `RuntimeScenarioHarness.cpp` tuned to `archipelago`/`default`) must be audited — visual smokes pin `default`/`archipelago` today (`main_client.cpp:1418-1421`), which T11 must not alter.
