# Iteration 6 — Wave A Spec (Aetheric · Erosion · SHIELD-RT substrate)

**Status:** spec / pre-dispatch. **Branch:** `feat/polyglot-audit-roadmap` (tip after
Wave 0 = `546ce6e`). **Inputs:** the 5 research briefs + `_synthesis.md` in this
directory; owner decisions 2026-06-15. **Determinism baseline:** `world_hash
d950a6afc12a5cdc`.

## 0. Owner constraints (pinned)
- **Target hardware:** RTX 5070 Ti, 16 GB. **Goal:** ~300 fps, heavily optimized +
  beautiful (Frostbite/Crysis caliber or stylistic). **Caps may be raised** to use
  this PC.
- **Render API:** stay **OpenGL 4.5**, build Wave A **Vulkan-aware** (SPIR-V-
  compilable GLSL, no GL-only intrinsics in the new substrate; abstraction
  boundaries that don't hardcode GL objects). Add **Nsight profiling now**. The
  Vulkan/DLSS/hardware-RT backend is a **deferred future render iteration** — see
  memory `render-api-and-target`.
- **Far-field quality:** go for the nicest path — SDF bricks for genuine 3D far
  content are welcome; GI/soft-shadows desirable (but SDF GI/soft-shadows are NOT
  in Wave A scope — recorded for a later wave; their existence changes the brick-cap
  decision, see §4 open Q3).

### Frame-budget honesty (IMPORTANT)
300 fps = **3.3 ms/frame total**. Native 6×-view-distance + full volumetrics on GL
**without DLSS** cannot hit that. So: **300 fps is the post-Vulkan+DLSS target**;
the Wave A/B **interim native target is ~120–144 fps at high settings on the 5070
Ti** (≈7–8 ms), with the architecture built so DLSS upscaling later closes the gap
to 300. The GPU pool below is sized to the interim budget and uses the 5070 Ti's
headroom; per-system ceilings stay honest so we can measure real cost on the card.

## 1. Wave order & the two deliberate world_hash bumps
Per `_synthesis.md` §2/§4. **Exactly two bumps, each in its OWN commit, never
together; replay (LREC1) + lockstep re-bless in the bump commit.**
1. **A1 Aetheric scalar field** → **bump #1** (sim, geometry-neutral). Lands first;
   cheap to bless; validates the `FieldGrid<float>` contract early.
2. **A2 Erosion height offset** → **bump #2** (worldgen, geometry-changing). Lands
   **before** any SHIELD-RT far-field parity baseline is blessed, so the parity
   corpus is *born eroded* and re-blessed once.
3. **A3 Shared volumetric substrate + SHIELD-RT primary path** (render-only, no
   bump). Its **parity baseline bless gates on A2 being merged.**

A1 ∥ A2 authoring can run in parallel worktrees (disjoint files); A3 serializes
after A2 merges. Inject the worktree base-check (memory `stale-main-worktree-hazard`):
`git rev-parse HEAD` == expected tip AND `src/luminumbra_common/core/DeterministicMath.h`
exists; copy vendored libs as REAL copies (no junctions); never `git worktree
remove --force`.

---

## A0. Nsight-now: per-pass GPU debug markers (small, do first)
**Render-only, no hash, no gate risk.** Enables legible Nsight Graphics / RenderDoc
captures so we can GPU-profile on the 5070 Ti (answers `_synthesis.md` open Q1).
- Add `push_debug_group(label)` / `pop_debug_group()` helpers to
  `src/luminumbra_client/rendering/passes/PassGlHelpers.h` next to the existing
  `label_gl_object` (guard on `GL_VERSION_4_3` + non-null `glPushDebugGroup`, same
  pattern as `glObjectLabel`).
- Bracket each pass in `RenderPipeline::render_frame` (shadow/gbuffer/ssao/lighting/
  water/skybox/aerial/foliage/particle/blit) with a named group. The debug context
  is already enabled under `LUMINUMBRA_DEBUG` (`main_client.cpp`).
- **Verify:** `RenderHealth` stays byte-stable (markers are no-ops to pixels); a
  capture shows named groups. Commit with A3 setup or standalone.

---

## A1. Aetheric scalar field (SIM — bump #1)
Engine knows only "emissive scalar fields"; LuminCrystal/Glimmer are game data
(`engine-game-decoupling`). New system mirroring `WindFieldSystem`.

### Data
- New `src/luminumbra_common/systems/AetherFieldSystem.{h,cpp}`. Holds **one
  `FieldGrid<float>` per channel**: `aether` (emissive energy) and `fire` (ignition
  scalar). Reuse the wind geometry (24 m cells, 64×64 = 1536 m extent) and the
  region-following origin. **Confirm 2.5D (single value per (lx,lz)) suffices — no
  true 3D need — before committing** (`_synthesis.md` open Q5); if 3D is needed the
  frozen `FieldGrid` API is violated and we stop and re-decide.
- Double-buffer + hoisted scratch, **zero per-tick heap allocation** (copy
  `WindFieldSystem`'s pattern exactly).

### Update (deterministic, on the 30 Hz tick, AFTER weather)
- Signature `void Update(std::uint64_t tick, const Vec3& region_anchor);`. Anchor
  via integer `set_origin_cells` (no FP drift), iterate **canonical z-major / x
  ascending**.
- **Diffusion:** fixed-iteration-count **Gauss-Seidel** (NOT exp-based) — there is
  **no `DeterministicMath::Exp`** (confirmed). Iteration count is **pinned** and
  baked into the sub-hash domain; changing it later is a world_hash bump (open Q6).
- **Advection by the wind field:** semi-Lagrangian backtrace sampling the wind
  `FieldGrid`. Keep deterministic: integer cell addressing + a **fixed bilinear
  interpolation** in a fixed operation order using only IEEE basic ops; NO libm.
- **Emission/absorption:** sources/sinks are deterministic scalars. Rates must be
  **polynomial/linear** (no exp decay) until a golden-tested `DeterministicMath`
  exp wrapper exists.
- **Lightning→fire hook:** seed the `fire` channel at the deterministic strike
  sites already produced by the weather lightning schedule (seed +13). Read-only
  consumption of strike state; no new RNG on the tick path beyond seed **+14**.

### Determinism / hashing
- Seed offset **+14** (next free; +11 wind, +12 weather, +13 lightning — append-only).
- Add `std::string ComputeAetherSubHash() const;` mirroring `ComputeWindSubHash`
  (`WindFieldSystem.cpp`): fnv1a-64 over IEEE bits via `MixFloat`/`BitsOf`, canonical
  cell order, mix (seed+14, tick, grid geometry, origin, then aether then fire cells).
- Fold into world_hash: extend `ComposeWorldHash` in
  `src/luminumbra_server/ServerWorldRunner.cpp` to append `"|aether:" + aether_hash`
  (append-only — the byte layout before it is unchanged, like the lightning slot).
- **SimDeterminismLint** must stay green: all tick math via `DeterministicMath::`,
  no unordered-container iteration, no wall-clock, seeded only. `AetherFieldSystem`
  is under a scanned root (`src/luminumbra_common/systems`).

### Render coupling (render-only, hash-neutral — can ride here or in Wave B)
- Pack `aether` to an RG16F (or R16F per channel) texture from `cells()` in
  canonical order; sample in the lighting pass and feed the **materials-LUT
  emissive path** (row 2, `kEmissiveLutScale=8.0`). **One-way bridge**: render
  never writes grid storage or influences sim/eviction order.

### A1 gates
- New `AetherFieldDeterminism` validator mode (mirror `WindFieldDeterminism`):
  field differs across fixtures, identical across a double-run; sub-hash stable.
- `HeadlessServerTick` world_hash advances to the new canonical **in the bump
  commit**; heavy oracle + LREC1 replay + lockstep re-bless asserted there.
- ctest green incl. a new `AetherFieldSystem` unit test (diffuse/advect golden).

---

## A2. Erosion height offset (WORLDGEN — bump #2, lands before SHIELD-RT parity)
Hydraulic + thermal erosion as an **additive `ErodedHeightOffset`** in the single
shared analytic height path. Closes the "smooth cones / blob-rocks / shoreline
seams" debt for the shape the far field renders.

### Computation
- Slot the offset in `ComputeShapedHeightSample()`
  (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:379`) **after shaping +
  island mask, before the river carve** (so rivers carve the eroded surface).
  Mirror it byte-for-byte in the SIMD batch path `ComputeShapedHeightGrid()`.
- Add fields to `TerrainGenParams` (`SHIELD_WorldSystem.h`): `erosion_enabled`,
  strength/iteration params. **Default OFF** (preserves byte-zero drift for legacy
  presets); the shipped archipelago/mountains presets opt IN.
- Erosion model: offline/generation-path bake (not on the 30 Hz tick). Thermal
  (talus) + hydraulic (Mei-style) over a region tile with an **overlapping halo** so
  region edges don't re-seam; the cropped interior must be **halo-width-independent**
  (acceptance test). **Fixed iteration/step count baked into the params hash.** The
  `C=Kc·sin(tilt)·|v|` sediment-capacity term uses `DeterministicMath::Sin` (exists);
  no `Exp` (use the linear/poly sediment forms).

### Determinism / hashing
- Mix the erosion params into `ComputeTerrainParamsHash()`
  (`src/luminumbra_common/world/FarLodStore.cpp:70`) — add a conditional block
  (marker e.g. `0x04`) like biomes/rivers/structures. **Also fix the latent gap:
  the continentalness/erosion/peaks shaping splines are currently NOT hashed** —
  fold them in under the same bump so the params hash honestly covers shaping.
- This re-keys `FarLodTile.params_hash` → pristine far-LOD tiles regenerate against
  eroded terrain automatically; edited tiles stay authoritative.
- Bump the shipped preset height-hash gate (the `…PresetHeightHash` /
  worldgen-atlas-snapshot machinery, `test/shield/test_worldgen_layer_snapshots.cpp`)
  with the new schema_rev; keep the legacy shaping-off fixture unchanged.

### A2 gates
- Worldgen atlas/snapshot deltas re-blessed; preset height-hash bumped (deliberate,
  logged). Slope-histogram still meets the normal-land floor; new acceptance: relief
  is erosion-shaped (drainage networks present), halo-width-independent.
- `HeadlessServerTick` world_hash advances **in this commit** (separate from A1);
  heavy oracle + LREC1 + lockstep re-bless. `FarLodHorizon` re-derived against eroded
  tiles. `WorldVisualSweep` re-run (terrain shape changed).

---

## A3. Shared volumetric/raymarch substrate + SHIELD-RT primary path (RENDER-only)
Build the substrate **once** and **freeze** it (Wave B clouds plug in as a pure
consumer). New module `src/luminumbra_client/rendering/volumetric/`.

### Substrate (frozen API — `_synthesis.md` §3)
1. Ray-from-G-buffer-depth reconstruction (world ray origin/dir per pixel).
2. Half-res RGBA16F march target + depth-aware bilateral upsample.
3. Temporal reproject + history + disocclusion reject (the affordability lever).
4. Shared blue-noise jitter LUT.
5. Empty-space skip + **conservative-mip** (lower-bound-safe) 3D-sample helper.
6. Froxel volume (frustum-aligned, ~160×90×64, exp depth slices, RGBA16F+history) —
   owned here; Wave B clouds/fog/god-rays composite into it. SHIELD-RT uses items
   1–5, not the froxel volume.
- **Vulkan-aware:** keep these shaders SPIR-V-compilable (no GL-only GLSL); the
  substrate interface takes opaque handles, not raw `GLuint` in its public surface
  where avoidable, so a Vulkan backend swaps the impl.

### SHIELD-RT far-field renderer
- **Primary tracer = heightfield max-mip march over `FarLodStore` tiles** (the iter-4
  spike conclusion, CONFIRMED by research). Build a render-side **conservative
  max-of-children mip pyramid** over `height_q`, keyed `(tier,rx,rz)`, as a derived
  structure — **NOT** added to the persisted tile format. F1 (4 m, 512–768 m) + F2
  (8 m, 768–1536 m).
- **Sparse SDF brick clipmap** for genuine 3D far content (overhangs/caves/
  structures) **behind** `kEnableExperimentalGpuSdfIntegration` using the existing
  `GPUSDFSystem` gating (`compile_time_enabled/runtime_requested/runtime_allowed/
  cpu_fallback_active`) and the `sdf_generation.compute` precedent. Hybrid per-ray:
  height-march tiles, sphere-trace brick segments where a clipmap cell flags 3D,
  resume (Lumen policy). **Brick pool hard-capped (start 96 MB; may rise on the 16 GB
  card after profiling — but slack returns to the shared pool, it does NOT auto-grow
  the cap).**
- **GPU-PROFILE BOTH TRACERS FIRST** on the 5070 Ti via the A0 markers + Nsight
  (`_synthesis.md` open Q1) before committing the primary path — a real GPU profile
  could invert the CPU-Debug step-count ranking.

### Integration
- Insert the raymarch pass in the **G-buffer region after live chunks**, the slot
  `FarLodSystem::draw_gbuffer` occupies (`GBufferPass.cpp:233`), same depth-bias
  discipline; writes material id to the G-buffer so Aetheric emissive far content
  lights identically (no LUT change). **Keep the FarLodSystem mesh path as fallback
  until the raymarcher passes its gates.**
- Add a `ShieldRT` entry to `GpuTimerPass` + `kGpuTimerPassNames[]` + a
  `shieldrt_gpu_ms` stat.
- Near↔far blend: depth-aware dither/dissolve band (Lumen-style) wired to the seam
  gate.

### A3 gates
- **Parity vs the FarLodSystem marching-cubes mesh at 1536 m** (A/B silhouette/depth
  diff; both consume the same `height_q`) — blessed **against eroded terrain (A2
  merged)**.
- Near↔far **seam/transition** gate (no cracks, no popping across the dither band).
- Perf: `shieldrt_gpu_ms` within the §4 ceiling on the 5070 Ti.
- `RenderHealth` + `WorldVisualSweep` (incl. the strict objective critique) green;
  `HeadlessServerTick` world_hash **unchanged** (proves render-only).

---

## §4. GPU budget (interim, 5070 Ti) — raised from the synthesis 60 fps figures
Interim frame target ~7–8 ms (≈120–144 fps); new-systems pool **raised to ~3.0 ms**
(headroom on the 16 GB card), DLSS later reclaims the rest toward 300 fps.

| Consumer | Allocated (typical) | Hard ceiling | Degrade lever |
|---|---|---|---|
| SHIELD-RT far-field | 1.2 ms | 1.6 ms | brick cap; coarser clipmap levels |
| Aetheric render tap | 0.1 ms | 0.2 ms | lighting-pass sample only |
| (Wave B: clouds) | — reserved 1.0 ms | 1.4 ms | temporal 1/16→1/32 |
| (Wave B: grass) | — reserved 0.7 ms | 1.0 ms | blade count / handoff distance |
- Re-measure on the per-pass GL timers + Nsight against the **quiet-machine perf
  baseline** (Phase 0.3 — still owed). Wave A ratifies the real split before B fans out.

## §5. Resolved open questions (from `_synthesis.md` §6)
- Q1 GPU-profile both SHIELD-RT tracers → **A3 task, before committing primary path**
  (A0 markers + Nsight enable it).
- Q4 target frame budget/hardware → **5070 Ti, interim 120–144 fps native, 300 fps
  post-DLSS** (§0).
- Q3 SDF soft-shadows/AO → **NOT Wave A**; recorded for a later wave. Brick cap
  decided for far-field-only for now.
- Q2 far-field 3D density, Q5 Aetheric 2.5D-sufficiency, Q6 Gauss-Seidel iteration
  count, Q7 erosion halo width, Q8 substrate freeze line, Q9 grass deterministic
  readback (Wave B), Q10 GL compute/SSBO/indirect capability → **pinned as named
  tasks/acceptance tests in the sections above**; Q2/Q5 are go/no-go checks the
  implementer must confirm before committing (heightfield-sufficient; 2.5D-sufficient).

## §6. Execution model
- **Agent teams (worktree-isolated Opus 4.8)** for A1 ∥ A2 authoring (disjoint files)
  with the mandatory base-check; A3 after A2 merges. **Workflow** for the
  per-leg verify pipeline (build → ctest → gate modes → `WorldVisualSweep`) and for
  the find→adversarially-verify visual critique.
- Each `world_hash` bump is its **own commit** with in-commit replay/lockstep re-bless.
- Self-contained dispatch specs (Fable unavailable; all tasks Opus 4.8).

## §7. Verification (Wave A exit)
ctest green (+ AetherField, erosion/worldgen-snapshot, SHIELD-RT parity/seam tests);
all engine-frontier gates incl. AetherFieldDeterminism, SimDeterminismLint,
FarLodHorizon (re-derived), WorldVisualSweep (strict), RenderHealth; HeadlessServerTick
shows exactly two new canonical hashes across the two bump commits and is unchanged
after A3; forge verify clean (modulo documented brace false-positives).
