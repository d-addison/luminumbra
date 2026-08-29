# Engine Caching Subsystem + Prioritized Worldgen-Sample Applications

Status: SPEC (ready to implement)
Branch context: `feat/polyglot-audit-roadmap`
Determinism oracle (must stay green): `luminumbra_server_app --smoke` -> `world_hash == world_hash_replay == b05b642e1299075f`

---

## 0. Problem & thesis

Profiling found the moving-frame cost is dominated by WATER: p50 ~10 ms, p90 ~31 ms while moving.
Root cause is per-cell WORLDGEN RE-SAMPLING in `WaterSystem` first-time chunk init: `seed_chunk_water`
(`src/luminumbra_common/systems/WaterSystem.cpp:352-403`) calls `SHIELD_WorldSystem::WaterLevelAt`
(`:377`) **and** `GetTerrainHeightAt` (`:378`) for all `WATER_SIM_RESOLUTION^2 = 64` cells of every
newly-resident chunk. `GetTerrainHeightAt` (`SHIELD_WorldSystem.cpp:2886-2889`) is
`ComputeShapedHeightSample(...).final_height` — a multi-octave FastNoise + shaping pipeline measured
in-tree at **~30 us/cell** (`WaterSystem.cpp:291` comment). The SAME world positions are re-sampled
across systems and frames.

**Thesis (grounded, verdict-confirmed):** the single biggest win does **not** need a cache — the
terrain height the water loop re-samples is **already stored byte-identically** in the chunk's own
`heightmap_data` at the exact coordinates the water cells use. The reusable cache facility is real
and worth building, but it is the **second** play, gated behind telemetry, and net-negative if applied
to the wrong callers. This spec ships the local read first, builds the facility second, and is explicit
about the DO-NOT-CACHE boundary.

### Verified ground truth (file:line)

1. `heightmap_data` is filled by `ComputeShapedHeightGrid`, asserted **byte-for-byte equal** to
   per-column `GetTerrainHeightAt` by a batch-vs-scalar parity gtest, on the **`shaping_enabled`**
   path (`SHIELD_WorldSystem.cpp:3641-3651` coarse, `:3770-3771` full). Write layout is **x-fastest**:
   `heightmap_write_idx = x + z*size_x` with `size_x = CHUNK_SIZE_X+1 = 17` (`:3619`, `:3756`, `:3797`).
   The stored value is `terrain_h` (pre-cave; caves touch only `sdf_data`) = `final_height`.
2. Coordinate alignment is EXACT. `CHUNK_SIZE_X = 16` (`Types.h:46`), `WATER_SIM_RESOLUTION = 8`
   (`WaterSystem.cpp:65`), cell width `= 16/8 = 2.0`. Water cell-center local offset
   `= (x+0.5)*2.0 = 2x+1` -> `{1,3,5,7,9,11,13,15}` (`WaterSystem.cpp:372-373`), an exact odd-integer
   subset of the `[0..16]` lattice. Water loop maps `world_x <- water x`, `world_z <- water z`
   (`:372-374`), so the byte-identical index is `(2*water_x+1) + (2*water_z+1)*size_x`. **NOT** the
   transposed `(2z+1)+(2x+1)*17` — terrain is anisotropic (independent warp_x/warp_z channels), so a
   swapped index reads a different lattice point and silently re-pins.
3. `water_bed_mm`/`water_depth_mm` (`Chunk.h:112-113`) are HASHED: folded into
   `debug_water_state_hash` (`SHIELD_WorldSystem.cpp:2909-2910`) -> `world_hash`. `water_bed_mm =
   lround(terrain*MM_PER_M)` (`WaterSystem.cpp:382`); identical float -> identical mm -> hash unchanged.
4. No general cache facility exists today (grep `*Cache`/`get_or_compute`/`memoiz` over
   `src/luminumbra_common` is empty). Two inline domain caches exist: `m_hydro_cache`
   (`SHIELD_WorldSystem.cpp:750-773`) — the CORRECT concurrent pure-fn pattern to generalize — and
   `m_column_surface_span_cache` (`:1766-1776`) — **main-thread-only, NOT mutex-guarded; do NOT copy**.
5. Water init dispatches one job/chunk to `JobSystem` workers and `wait()`s
   (`WaterSystem.cpp:391-398`). `WaterSystem::update` runs on the main thread BEFORE meshing dispatch
   (`SHIELD_WorldSystem.cpp:2012-2014`); off-thread meshing writes only `scratch.heightmap_data` /
   `pending_heightmap_data` (`Chunk.h:79`), never the live `heightmap_data`, and the LOD0 publish
   `heightmap_data = std::move(pending_heightmap_data)` is main-thread (`:4087`). So a worker reading
   `cp->heightmap_data` sees an immutable, fully-published array — **race-free, no lock**.

---

## 1. The cache facility

### 1.1 What it is

A header-only `WorldgenSampleCache<Value>` owned by `SHIELD_WorldSystem`, generalizing the **proven**
`m_hydro_cache` concurrency pattern (`SHIELD_WorldSystem.cpp:750-773`). Do NOT invent a new threading
model — that pattern is already validated under parallel chunk-gen. The single change vs the hydro
cache is **sharding**, because the hydro cache's single `shared_mutex` is correct for its rare/expensive
region bakes but would re-serialize the many cheap misses of the parallel water workers.

### 1.2 API — `get_or_compute` ONLY (no bare put/get)

A bare `put` invites a caller to store an interpolated/quantized value and silently re-pin. The only
public mutator is compute-on-miss:

```cpp
// Reusable, determinism-safe memo for PURE worldgen samplers.
// Value = the EXACT float bits the miss path returns. Never interpolated/quantized.
template <class Value>
class WorldgenSampleCache {
public:
    // key = packed exact coords (bit_cast of x,z) + seed + param-epoch + sampler-id.
    // compute() runs OUTSIDE the lock; concurrent double-compute is byte-identical.
    template <class Fn> Value get_or_compute(uint64_t key, Fn&& compute);
    void clear();              // on seed / worldgen-param change (mirror :366)
    CacheStats stats() const;  // hits / misses / size, summed from atomics

private:
    struct alignas(64) Shard {                 // cache-line padded -> no false sharing
        mutable std::shared_mutex mtx;
        std::unordered_map<uint64_t, Value> map;
        std::atomic<uint64_t> hits{0}, misses{0};
    };
    static constexpr size_t kShards = /* next_pow2(worker_count * 4) */;
    std::array<Shard, kShards> m_shards;       // shard = hash(key) & (kShards-1)
};
```

Read path: `shared_lock`, find, **copy the Value out under the lock** (sidesteps rehash / pointer
invalidation of `std::unordered_map`). Miss: compute OUTSIDE any lock; insert via `try_emplace` under
`unique_lock` (first writer wins; loser's identical bits dropped). For grid-shaped consumers `Value`
may be a fixed small array (e.g. the 8x8 water grid) so one miss serves a whole chunk — strictly fewer
lock ops than per-point.

### 1.3 Thread-safety model (verbatim from `m_hydro_cache`, + sharding)

- Pure fn over immutable generators (`SmartNode` set at init, `SHIELD_WorldSystem.h:858-873`;
  `m_seed`/`m_params` const for a world). Concurrent double-compute of one key yields **identical
  bits**, so a benign race on which writer wins is value-stable — determinism-safe by construction.
- Container access is still guarded (an unsynchronized non-atomic map read/write is UB even when
  logically idempotent — this repo has already been burned by release-only UB). "Racy" here means
  uncontended-by-design, NOT lock-free-by-omission.
- **Sharded + `alignas(64)`** so it does not re-serialize the parallel water-init workers
  (`WaterSystem.cpp:391-398`) and does not false-share adjacent shard heads.

### 1.4 Eviction (bounded memory — the memory verdict's hard requirement)

`m_hydro_cache` is unbounded but survives because it is keyed per 64x64-cell region (sparse). A
per-POINT cache keyed on exact float (x,z) has ~4096x higher key density and **grows monotonically as
a flying camera streams new XZ every frame**; `clear()`-on-seed-change does nothing within a session.
Therefore:

- **Preferred: per-chunk derived storage**, not a global point-memo. Store the derived grid on the
  `Chunk` (the `water_src_mm` precedent, `Chunk.h:117-121`: "pure function of cell position, computed
  ONCE lazily, NOT serialized, NOT hashed"). It is freed with the chunk by the existing
  `STREAMING_MAX_ACTIVE_CHUNKS_BUDGET` residency eviction — automatically window-bounded, no shared
  structure, no lock, re-stamped on edit naturally.
- **If a global shard map is used (App 3 only):** it MUST have a fixed per-shard capacity with a
  deterministic-irrelevant eviction (LRU/ring — eviction only forces a recompute that returns the same
  bits). Plus `clear()` on seed/param change (mirror `m_column_surface_span_cache.clear()` at `:366`).
  An unbounded `std::unordered_map<position,float>` is a memory leak with extra steps — rejected.

### 1.5 Telemetry (mandatory — proves net-positive)

Per-shard atomic `hits/misses`, summed into `stats()`, surfaced via the existing
`build/debug/test-artifacts` telemetry. A cache below ~50% hit-rate on this access pattern is pure
overhead; you cannot know without counters. Telemetry is the go/no-go gate for App 3.

### 1.6 WHAT-IS-SAFE-TO-CACHE contract (encode in the header)

| Rule | Why |
|---|---|
| Cache ONLY pure samplers over immutable generators: `GetTerrainHeightAt`, `ComputeShapedHeightSample`, `WaterLevelAt`, `RiverInfluenceAt`. | No per-frame/per-camera state enters; value is position-only and hash-stable. Camera only selects WHICH chunks, never the value. |
| Key = exact float bits of (x,z) + seed + param-epoch + **sampler-id**. Never quantize the key below the lattice the sampler is queried on. | Quantizing the key = a different real coordinate = different bits = re-pin (best) / host!=peer (worst). Sampler-id stops `WaterLevelAt` and `GetTerrainHeightAt` (same x,z,seed) colliding -> silent corruption. |
| Value = miss-path bits **verbatim**. NEVER interpolate / quantize / fp16 the value. | Interpolating the OUTPUT != evaluating the function (multi-octave noise + nonlinear shaping). Any approximation re-pins `b05b642e`. |
| The cache is a transparent accelerator — it MUST NOT enter `world_hash`. | It returns identical bits; sim-only/additive, consistent with the SystemConfig "additive/zero-re-pin" discipline. |
| `clear()` on seed/param change; prefer seed-in-key so a stale entry under a new seed is impossible. | Mirrors `m_column_surface_span_cache` invalidation at `:366`. |
| Invalidate on terrain EDIT for any height/bed derivation. | `water_bed_mm` is re-sampled on edit (`Chunk.h:113`). Per-chunk storage re-stamps naturally; a global point-memo has no edit signal -> stale beds break drain/dam logic. |

---

## 2. Applications — ordered, biggest-safe-win first

### App 1 — Water-init reads `heightmap_data` (no cache; **byte-identical**)

- **Site:** `WaterSystem.cpp:378`, `seed_chunk_water`.
- **Edit:** when the chunk's full heightmap is present, replace
  `m_shield_system->GetTerrainHeightAt(world_x, world_z)` with a direct read of the chunk's own array:
  ```cpp
  const int hm_stride = CHUNK_SIZE_X + 1;                 // 17
  const int hm_idx = (2*x + 1) + (2*z + 1) * hm_stride;   // x-fastest; matches :3756 layout
  const float terrain = cp->heightmap_data[hm_idx];
  ```
  Use `x`/`z` exactly as the water loop indexes them (NOT transposed). Keep `WaterLevelAt(:377)` — it
  is not in `heightmap_data` and is cheap (1 lake-noise read outside basins; route via App 3/App 4 later).
- **GUARDS (correctness gates, not re-pins) — fall back to `GetTerrainHeightAt` when any hold:**
  1. `cp->heightmap_data.size() != size_t(hm_stride) * hm_stride` (persistence-loaded chunks; surface-band -> LOD0 promotion with `pending_heightmap_data` still in flight, `Chunk.h:79`).
  2. `m_gpu_sdf_callback` is registered — that path derives `heightmap_data` by an integer-y SDF march (`SHIELD_WorldSystem.cpp:3676-3697`), NOT `GetTerrainHeightAt` -> different bits. It is client-only (`:4191`), so the headless oracle is always safe, but the read MUST fall back when set.
  3. `!m_params.shaping_enabled` — the non-shaping full path fills `heightmap_data` via `GenUniformGrid2D` (`:3738`), which the in-tree note flags as drifting from the scalar `GenSingle2D` ("1e-4 snapshot gate covers grid-vs-single drift") -> NOT byte-identical -> would re-pin. The shipped default preset has shaping ON; gate on it anyway.
- **Determinism class: BYTE-IDENTICAL** (on the headless CPU shaping path). Same world coordinate,
  value already produced by the parity-pinned `ComputeShapedHeightGrid`; `lround` of identical float ->
  identical `water_bed_mm` -> `world_hash` unchanged.
- **Expected ms:** removes 64 `ComputeShapedHeightSample` calls/chunk (~30 us each ~= ~1.9 ms/chunk) x
  up to `MAX_WATER_INITS_PER_TICK = 6` chunks/tick ~= **~11 ms/tick removed at the streaming burst** —
  the `GetTerrainHeightAt` half of the per-cell cost. Zero lookup/sync overhead.
- **Validation:** build, then `luminumbra_server_app --smoke`. **Gate = the hash LITERAL is stable:**
  `world_hash == world_hash_replay == b05b642e1299075f`. NOTE (memory verdict): run==replay alone is
  NOT the byte-identity proof — both runs read `heightmap_data` identically, so a uniform source-swap
  would pass run==replay even if it changed the value. The real proofs are (a) the existing
  batch-vs-scalar parity gtest (`ComputeShapedHeightGrid == GetTerrainHeightAt`) and (b) the hash
  literal staying `b05b642e1299075f`. Assert BOTH.

### App 2 — Per-chunk bed cache for resize (**byte-identical** via stored value)

- **Site:** `WaterSystem.cpp:799-841`, `ResizeSimulationGrid` (re-samples `GetTerrainHeightAt` per cell;
  still wired via the budgeted adaptive-resolution path `:283-329`). Runs on the main thread (the
  resize loop is not job-dispatched) -> single-threaded read, no lock.
- **Edit:** at `new_resolution == 8` the centers are the same odd-integer lattice -> read
  `heightmap_data` exactly as App 1. At 16/32 the centers go off-lattice; do NOT interpolate. Instead
  store the bed grid once at init (it already lives in `water_sim_terrain_height`, `Chunk.h:103`,
  assigned at `WaterSystem.cpp:381`) and READ it on resize — byte-identical at ANY resolution because
  it returns the same stored float. The bed at a chunk's fixed centers is invariant across resizes;
  re-sampling every resize is the redundant work the `:291` comment flags (the historical ~180 ms spike).
- **Determinism class: BYTE-IDENTICAL** (stored-value reuse).
- **Expected ms:** kills the resize re-sample burst. Lower marginal ROI than App 1 — resize is already
  `MAX_WATER_RESIZES_PER_TICK` budget-amortized. Implement only if a fixed-camera profile still shows it.
- **Validation:** `--smoke` hash literal stable; storing-then-reading the same float is byte-identical.

### App 3 — `WorldgenSampleCache` for scattered point-callers (the facility's real home)

- **Sites (callers with NO local heightmap):** `WaterLevelAt` in water init/resize; foliage scatter
  (`FoliagePass.cpp:639` + `main_client.cpp` scatter, `GetTerrainHeightAt`+`BiomeIdAt` per point);
  `WaterfallDetect.cpp:65-201` (5-point `GetTerrainHeightAt` stencil + `RiverInfluenceAt`);
  `SceneSurvey.cpp:42-104` (slope stencil). These hit overlapping XZ of freshly-streamed chunks within
  the same few ticks, each re-running the full pipeline.
- **Edit:** wrap these in `get_or_compute` from S1, keyed per S1.6. Add a unit test asserting
  `cache.get_or_compute(k) == GetTerrainHeightAt(...)` **bit-for-bit** over a key sweep (model = the
  existing batch-vs-scalar parity gate).
- **Determinism class: BYTE-IDENTICAL** iff key = exact bits and value = verbatim.
- **Expected ms:** medium, workload-dependent. Real ONLY where the SAME exact (x,z) is re-asked
  (clustered stencils, re-scatter on the same tile). Adopt **only after telemetry (S1.5) shows
  >~50% hit-rate** on the targeted callers. The cross-system mesher(integer corners) vs water(centers)
  point-sharing has near-zero hit-rate under a flying camera — do NOT route those through it.
- **Validation:** oracle unchanged (transparent accelerator); the bit-for-bit unit test is the gate.

### App 4 — Batched sampler (orthogonal; recompute-faster, not a cache)

- **Edit:** replace per-cell scalar `WaterLevelAt` / `GetTerrainHeightAt` loops with a
  `GenPositionArray2D`-backed batched call — the ~13x SIMD path the engine already trusts and parity-
  gates (`SHIELD_WorldSystem.cpp:1147`, `:3649`). The right fix for off-lattice resize (App 2 at 16/32)
  and for `WaterLevelAt` (which App 1 does not cover).
- **Determinism class: BYTE-IDENTICAL** (parity-gated batch path), zero re-pin.
- **Expected ms:** collapses the residual `WaterLevelAt` per-cell cost into a handful of SIMD sweeps.
- **Validation:** `--smoke` hash literal stable.

---

## 3. DO-NOT-CACHE list (net-negative / determinism-unsafe)

1. **Global per-POINT memo of `GetTerrainHeightAt` shared across systems.** Mesher (integer corners
   `[0..16]`) and water (centers `{1,3,..,15}`) request DIFFERENT coordinates, and a flying camera
   streams new XZ every frame -> cross-system/cross-frame hit-rate ~0 while you pay hash+lock per call.
   Per-chunk derived storage (App 1/2) beats it; the duplication is WITHIN a chunk and BETWEEN passes
   of the SAME chunk.
2. **Single-mutex global map under the parallel water workers** — re-serializes the parallelism
   `:391-398` just won (a regression vs no cache). Only a sharded, cache-line-padded map is acceptable,
   and only where telemetry shows reuse.
3. **Reading `heightmap_data` at OFF-lattice positions (res 16/32, or any bilinear-to-center scheme).**
   Interpolating corner heights to fractional centers != the multi-octave sampler's value -> changes
   `water_bed_mm`/`water_depth_mm` -> bumps `b05b642e`, and risks host!=peer if interp op-order differs.
   This is a deterministic RE-PIN, not a cache — reject as a primary path. Only do it as a deliberate,
   re-blessed re-pin if the owner explicitly accepts it (then pin the exact interp op order).
4. **Re-caching `RiverInfluenceAt`.** Already de-duplicated into `Chunk.water_src_mm`
   (`Chunk.h:117-121`, lazy, size-guarded, not hashed). Redundant; zero ROI.
5. **`m_column_surface_span_cache` pattern in any worker path.** It is main-thread-only and NOT
   mutex-guarded (`:1766-1776`) — copying it cross-worker is a real data race.
6. **Any value mixing camera/frame/LOD/dirty-generation state.** Leaks non-pure state into a hashed
   value -> instant desync. Only the four named pure samplers qualify.
7. **`thread_local` per-worker caches** for spatially-shared keys — zero cross-thread reuse + N-fold
   memory; chunks hop workers so it wastes the shared-region win. `thread_local` only for true scratch.

---

## 4. Honest expected effect on the water budget

- **Today:** water while-moving p50 ~10 ms, p90 ~31 ms; p90 is a BURST of LOD-boundary chunk
  inits/resizes (`WaterSystem.cpp:291-296`), not steady-state.
- **After App 1 (the headline, no cache):** removes the `GetTerrainHeightAt` half of the per-cell
  init cost — ~11 ms/tick at the burst. Expected p90 roughly **31 ms -> ~16-20 ms** (the `WaterLevelAt`
  half + edge-flux/assign remain), p50 ~10 ms -> **~5-6 ms**. Byte-identical, zero re-pin.
- **After App 4 (batch `WaterLevelAt`) + App 2 (resize):** collapses the remaining per-cell
  `WaterLevelAt` and the resize re-sample into SIMD sweeps / stored reads. Expected to take the burst
  p90 toward **~6-10 ms** (init then bounded by assigns + integer fill, not noise).
- **App 3:** does not move the water budget directly; it removes cross-system re-sampling
  (foliage/waterfall/scene-survey) elsewhere in the moving frame. Adopt only if telemetry justifies it.
- **200fps floor (5.0 ms/frame):** water init alone will NOT be the gate after Apps 1+4. The brief's
  separate streaming/UI/foliage/poll costs (~14 ms "other" in the 600fps progress note) remain the
  next ceiling. **Honest verdict:** Apps 1+2+4 remove water as the dominant moving-frame spike and
  make the worst-case water tick budget-shaped, but reaching a sustained 200fps floor requires the
  separate streaming/UI work — water caching gets us most of the way on the water axis only.

### Validation summary (every step)

| Step | Determinism class | Gate |
|---|---|---|
| App 1 | byte-identical | parity gtest green AND `--smoke` hash literal `b05b642e1299075f` (run==replay AND literal stable) |
| App 2 | byte-identical | `--smoke` hash literal stable |
| App 3 | byte-identical | bit-for-bit unit test vs scalar sampler; telemetry hit-rate >~50%; `--smoke` stable |
| App 4 | byte-identical | parity-gated batch path; `--smoke` stable |

No step is a planned re-pin. Re-pin `b05b642e` ONLY if the owner explicitly elects DO-NOT-CACHE item 3
(off-lattice interpolation); that is out of scope for this spec.

### Recommended order of operations

1. App 1 (read `heightmap_data`, gated on size + non-GPU + shaping) — validate `--smoke` literal stable.
2. App 4 (batch `WaterLevelAt`) — captures the other half of the init cost.
3. App 2 (per-chunk bed cache for resize) — only if a fixed-camera profile still shows resize.
4. Build the `WorldgenSampleCache` facility (S1) + App 3 with telemetry — prove net-positive first.

### Key files for the implementer

- `WaterSystem.cpp:352-403` (init), `:799-841` (resize), `:283-329` (resize dispatch), `:65`
  (`WATER_SIM_RESOLUTION=8`), `:391-398` (parallel dispatch).
- `SHIELD_WorldSystem.cpp:2886-2889` (`GetTerrainHeightAt`), `:913-922` (`WaterLevelAt`),
  `:3616-3804` (heightmap fill; GPU divergence `:3676-3697`, x-fastest layout `:3756`, parity note
  `:3645-3648`), `:750-773` (`m_hydro_cache` pattern to generalize), `:1766-1776`
  (`m_column_surface_span_cache` — anti-pattern), `:366` (`clear()` on param change),
  `:2909-2910` (water state hash), `:1147` (`ComputeShapedHeightGrid` batched).
- `Chunk.h:50` (`heightmap_data`), `:79` (`pending_heightmap_data`), `:103`
  (`water_sim_terrain_height`), `:112-113` (`water_bed_mm`/`water_depth_mm` HASHED, edit-resampled),
  `:117-121` (`water_src_mm` per-chunk-derived precedent).
- `include/luminumbra/core/Types.h:46` (`CHUNK_SIZE_X=16`).
- `src/luminumbra_common/core/JobSystem.h` (worker pool informing the sharded-mutex choice).
