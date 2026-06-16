# SHIELD-RT far-field SDF raymarch + GPU-resident SDF storage

Research brief — luminumbra iteration-6 Wave 0. Topic slug: `shieldrt-gpu-sdf`.
Author: research subagent, 2026-06-15. Render-only topic (see Determinism section).

This brief takes a **POSITION** on the iteration-4 SHIELD-RT spike's central question
(do the FarLodStore F1/F2 heightfield tiles *become* the far raymarch source, or are
they retired into a general SDF brick volume?) and adversarially re-checks it against
the primary literature the spike cited.

---

## TL;DR position

**CONFIRM and harden the iteration-4 spike recommendation.** Keep the
`FarLodStore` F1 (4 m) / F2 (8 m) packed heightfield tiles as the **primary
far-field raymarch source** beyond the live chunk ring, traced with a
distance/quadtree heightfield march under a **conservative MAX-mip** pyramid.
Reserve GPU-resident **sparse 8³ SDF bricks in camera-centered clipmaps** *only*
for genuine 3D far content (overhangs, arches, cave mouths, large structures) that
a height function cannot represent. Do **not** retire the tiles into one general
brick SDF.

The primary sources do not flip this; they reinforce it. Every production system
that ships a global SDF (Claybook, UE5 Lumen) pays a large, relief-scaling memory
cost for 3D generality and carries the conservative-coarse-level correctness
burden — exactly the two costs the heightfield path sidesteps. UE5 Lumen's own
shipped policy is "trace the cheap detail representation for the first stretch of
the ray, the merged global SDF for the rest" — i.e. hybrid, not monolithic SDF.
**What would flip it is enumerated below** (pervasive 3D far content, a GPU profile
that inverts the step-cost ranking, or shared SDF consumers that make the global
field marginal-cost). None is demonstrated yet; the GPU-profile flip in particular
is *unverified* and is the single most important Wave A measurement to take.

---

## Recommended approach

### 1. Far primary rays: heightfield march of FarLodStore tiles (KEEP)

- Beyond the live chunk ring (≈ where F1 begins, 512 m), trace the existing
  `FarLodStore` packed heightfield tiles directly — Quilez-style terrain raymarch
  with distance-scaled steps, accelerated by a **maximum-of-children mip quadtree**
  over the quantized heights (`height_q`). A max-mip of heights is a *conservative
  upper bound by construction*: a ray above the local max-mip height cannot have hit
  the surface, so coarse levels can be skipped safely. This is the property the
  spike measured at **2.0 mean steps/ray on flat far field** and 7.3 on mountains.
- LOD falls out of the existing tier split: F1 (4 m / 512–768 m) and F2
  (8 m / 768–1536 m) are already distance-binned; the march selects the coarse mip
  level by ray-cone footprint (Claybook's mip-by-cone idea, but on a heightfield
  quadtree instead of a 3D volume).
- This is GPU-resident: upload the F1/F2 tile `height_q`/`material`/`flags` arrays
  (and their max-mip chains) as 2D array textures / SSBOs keyed by region, sized to
  the existing 64 MB `FarLodStore` residency budget. The march is a compute or
  fullscreen fragment pass.

### 2. Sparse 3D content: SDF bricks in camera-centered clipmaps (NEW, scoped small)

- For the *small* set of far locations that carry true 3D geometry (overhangs,
  arches, cave mouths, large structures), store **sparse 8³ voxel bricks** in
  **camera-centered clipmaps** — the storage pattern every production system
  converges on (UE5 Global Distance Field clipmaps; JCGT 2022 sparse brick sets;
  Claybook's mip volume). Clipmaps are concentric, **denser near the camera and
  sparser far** [Epic; UWA], with an indirection table from clipmap cell → brick
  pool slot; empty-air cells unallocated.
- Source the bricks by downsampling the chunk SDFs (`sdf_generation.compute` data
  path) for near-ish 3D content, and synthesize coarse far bricks analytically from
  the heightfield where a tile flags 3D features. **Incremental update only of
  newly-visible or scene-changed clipmap regions** [Epic Lumen doc: "only newly
  visible areas or those affected by the scene modification need to be updated, so
  composition doesn't cost much"].
- Trace bricks with grid-SDF sphere tracing under a **conservative MIN-magnitude
  mip** (see correctness criterion). Adopt the JCGT-2022 analytic ray-vs-trilinear
  intersection (cubic solver or repeated-lerp) at the *hit refinement* step to kill
  the residual base-grid tunneling the spike saw (1 332 grazing-ray misses on
  mountains_grazing came from coarse 8 m base voxels, not from mips).

### 3. The hybrid policy = copy Lumen's shape

Per ray: heightfield-march the FarLodStore tiles; where the ray enters a clipmap
cell flagged as having 3D-content bricks, sphere-trace the brick SDF for that
segment, then resume the heightfield march. This is structurally identical to
Lumen's verified shipped policy — **"trace against each mesh's distance field for
the first two meters for accuracy, and the merged Global Distance Field for the
rest of each ray"** [Epic Lumen Technical Details] — with "cheap representation
where it suffices, SDF where 3D detail exists" as the invariant.

### 4. Correctness criterion (PIN THIS)

**Coarse acceleration levels MUST be conservative bounds, or sphere tracing /
heightfield marching silently overshoots the surface and the artifact presents as
seam popping, not as an obvious bug.**

- Heightfield path: coarse levels are **MAX-of-children of `height_q`** (skip-safe
  upper bound). Conservative by construction.
- SDF brick path: coarse levels are **MIN-magnitude-filtered** distances with a
  sample-spacing correction (subtract ~half the cell diagonal), *never averaged*.
  Averaged mips are not a valid lower bound on true distance and overshoot — this
  is confirmed by both the literature [JCGT 2022: trilinear sampling, not averaging,
  is needed to avoid overshoot] and the spike's own numbers (**naive avg-filtered
  mips: 1 602/57 600 ray overshoots on the steepest view vs 33 for the conservative
  min-filtered chain — a 48× reduction**).
- The remaining 33 conservative-chain misses are the cheap-bound approximation gap;
  a JCGT-2022 correct trilinear grid-SDF interpretation closes it but costs more per
  step. For far-field quality this is acceptable; gate it.

---

## Alternatives considered (+ why rejected)

1. **Retire F1/F2 tiles into one general brick SDF (the "tiles retire" assumption).**
   *Rejected.* Pays for 3D generality terrain does not use. Spike measured the dense
   region SDF at **2.6× the heightfield+max-mip memory on mountains and growing with
   vertical relief** (SDF Y-extent scales with terrain range: ny=8 flat → ny=21
   mountains), 7–12× more steps/ray on common far views, plus the conservative-mip
   correctness burden a height max-mip avoids by construction. Claybook's dense world
   SDF — **1024×1024×512 @ 8-bit, ~586 MB across 5 mips** [secondary] — is the
   cautionary scale datapoint: a dense global SDF is hundreds of MB; our entire
   FarLodStore budget is 64 MB.

2. **Generic SDF sphere tracing as the *primary* far-field tracer.**
   *Rejected for terrain-dominated far field.* Sphere tracing's cost is governed by
   how close rays graze surfaces; on a heightfield the dedicated terrain march with a
   max-mip quadtree is strictly cheaper and trivially LOD'd [Quilez terrain marching;
   spike]. Keep sphere tracing only for the brick segments.

3. **Froxel/volumetric march for the far terrain** (e.g. extend the aerial-perspective
   path). *Rejected.* The aerial pass is analytic (Hillaire 2020, no froxel) and solves
   *atmosphere*, not opaque terrain visibility; it cannot resolve a hit surface for the
   G-buffer. Out of scope for the geometry tracer.

4. **Point-splatting from the SDF (Dreams/Media Molecule endgame).**
   *Rejected for now, noted as kill-criteria reference.* Media Molecule abandoned direct
   raymarching after three renderers and splatted points generated *from* the SDF
   [Evans, SIGGRAPH 2015]. Our scope is far narrower (far-field-only, terrain-dominated,
   no per-frame deformation), which is exactly why direct marching is viable where
   Dreams' general case was not. Keep splatting in the back pocket if the brick path's
   far-field overdraw/quality fails its gate.

5. **Hardware ray tracing (RTX) of a BVH over bricks** (JCGT's fastest combo used HW RT
   + BVH over non-empty voxels). *Deferred, not rejected.* luminumbra targets broad GL
   hardware; this is a software-trace stack like Lumen's default software path. Revisit
   only if a HW-RT backend is committed.

---

## Perf budget (concrete; GPU shares a contended budget with grass + clouds + Aetheric)

Targets are for the far-field geometry tracer at the 1536 m horizon, **render-side**,
on the shared GPU frame budget. The spike's absolute ms are **CPU Debug-build** and
are NOT a GPU budget — they establish the *cost-shape ratio*, not wall-clock. The
numbers below are engineering targets for Wave A to validate, derived from the
cost-shape (heightfield march ≈ 2–7 steps/ray) and Claybook/Lumen-class real-time
budgets.

| Item | GPU ms target (1080p, mid GPU) | VRAM |
|---|---|---|
| Far heightfield march (primary rays, F1+F2, max-mip) | **≤ 1.2 ms** | reuse existing **64 MB** FarLodStore residency budget; +max-mip chains ≈ **+15–20 MB** (the spike's max-mip accel was ~0.79 MB per region; ×~24 resident far regions) |
| Sparse 3D-content brick clipmaps (sphere trace segments) | **≤ 0.8 ms** (sparse coverage; most rays never enter a brick cell) | **brick pool cap 96 MB** (8³ bricks, R8/R16 distance; bounded by pool, not by relief). Cap explicitly so it cannot grow into the grass/cloud budget. |
| **SHIELD-RT far-field subtotal** | **≤ 2.0 ms** | **≤ ~180 MB** including tiles |

Budget-split note for Wave A: state the **total** SHIELD-RT+grass+clouds GPU-ms
ceiling first, then subtract. This topic asks for **≤ 2.0 ms / ≤ ~180 MB**. The
brick pool VRAM is the contended variable — it is hard-capped (96 MB) precisely so
volumetric clouds and GPU grass can claim the remainder deterministically. If the
GPU profile (see risks) shows the heightfield march itself is cheaper than 1.2 ms,
hand the slack back to the shared pool rather than enlarging the brick cap.

VRAM context datapoints: UE5 per-mesh DF caps at **8 MB @ 128³** [Epic]; Claybook's
*entire* dense world SDF is ~586 MB [secondary] — our sparse-brick + heightfield
hybrid must stay an order of magnitude under that, which the 64 MB tile budget +
96 MB brick cap achieves.

---

## Determinism implications (sim vs render; world_hash impact)

- **This topic is RENDER-ONLY. It does NOT touch `world_hash`.** The far-field
  raymarcher consumes already-authoritative data (FarLodStore tiles produced by sim
  worldgen; chunk SDFs) and produces *pixels*, not sim state. Per the iteration-4
  spike's scope discipline and research Area-3 takeaway 7, nothing here enters a sim
  TU, uses `DeterministicMath`, or advances the sim RNG.
- The `test_sdf_gpu_cpu_parity` gate continues to police only the **authoritative**
  SDF generation (`sdf_generation.compute` feeding worldgen), NOT the render bricks.
  Render bricks are a downsample of already-blessed data; they need visual-QA, not
  world_hash parity.
- **Caveat to enforce:** the heightfield march reads `FarLodStore.height_q`, which is
  sim-authored (quantized via `QuantizeFarLodHeight`). Reading it render-side is fine;
  the marcher must **never write back** into tile storage or influence tile
  generation/eviction order, or it would couple render into a world_hash-affecting
  path. Keep the marcher a pure consumer.
- If iteration-6 later builds a *global* SDF that sim also reads (e.g. for
  authoritative collision/queries), THAT crosses into sim and would be a deliberate
  world_hash bump in its own commit + re-bless. The far renderer here does not.

---

## Integration notes (luminumbra files/systems this touches)

- **`src/luminumbra_common/world/FarLodStore.h` / `FarLodSystem`** — primary data
  source. The marcher reads `FarLodTile.height_q/material/flags`, `samples_per_side`,
  tier step (4 m/8 m), region origin (`rx*512`, `rz*512`). **Freeze this as a
  read-only render contract** (parallels the FieldGrid API freeze for iter-6). Build
  the max-mip chains as a render-side derived structure keyed by (tier, rx, rz);
  do not add them to the persisted tile format.
- **`src/luminumbra_client/rendering/RenderPipeline.{h,cpp}`** — new far-field
  raymarch pass. Insert in the **G-buffer pass region, AFTER live chunks** (the
  current `FarLodSystem` mesh draw slot, with the same depth-bias-after-live-chunks
  discipline) so near/far compose into one G-buffer. The existing per-pass GL
  timestamp timers (`*_gpu_ms`) get a new `shieldrt_gpu_ms` field next to
  `aerial_gpu_ms`/`foliage_gpu_ms`.
- **`RenderPipeline::GPUSDFSystem` + `kEnableExperimentalGpuSdfIntegration` (=false)**
  — the existing disabled compute stubs (`generate_chunk_sdf_gpu`, noise textures,
  async `compute_fence`) are the natural home for the **brick generation** half. The
  feature flag pattern (`compile_time_enabled` + `runtime_requested` +
  `runtime_allowed`) is the right gating shape for shipping bricks behind a toggle
  while heightfield-march primary rays land first.
- **`res/shaders/sdf_generation.compute`** — reuse/extend for brick downsampling.
  New shaders: `far_heightfield_march` (compute or fullscreen frag) and
  `far_sdf_brick_trace`.
- **Seam / transition vs F1/F2 mesh path at 1536 m** — the marcher must reach
  *parity with the current `FarLodSystem` marching-cubes mesh* it replaces. Blend the
  near (live-chunk raster) ↔ far (raymarch) boundary with a **depth-aware
  dither/dissolve over a distance band**, NOT a hard plane [Lumen-style blend; Area-3
  takeaway 1], and wire it to the planned **near↔far seam gate** (already on the
  iter-6 list). Because both the new marcher and the old `FarLodSystem` mesh consume
  the *same* `FarLodStore` `height_q`, a parity test is straightforward: render both
  for the same view and diff depth/silhouette at 1536 m.
- **`MarchingCubes::GenerateFarLodRegionMesh`** — stays as the fallback far renderer
  during A/B parity bring-up; the raymarcher is gated behind a flag until it passes
  the seam gate and visual-QA.
- **Materials LUT (row 2, `kEmissiveLutScale=8.0`) + lighting pass** — the marcher
  writes `material` id into the G-buffer exactly as the far mesh does, so emissive
  Aetheric far content lights identically. No LUT change.

---

## Open risks

1. **[UNVERIFIED — highest priority] GPU profile may invert the step-cost ranking.**
   The spike is a CPU prototype. The SDF brick path's coherent, branchless inner loop
   could beat the heightfield march's bilinear-sample + quadtree-descent on real GPU
   hardware (divergence/cache effects the CPU does not surface). The *memory* and
   *conservative-correctness* arguments still favor heightfields, but a GPU profile
   that closes the ms gap weakens the per-ray-cost case. **Wave A MUST GPU-profile
   both paths before committing.** This is the one measurement that could flip the
   primary-tracer choice.
2. **Pervasive 3D far content flips the whole decision.** If iteration-5/6 worldgen
   puts overhangs/arches/floating islands/cave mouths across *most* of the far field
   (not a sparse set), a heightfield cannot represent it and the brick SDF becomes
   mandatory regardless of cost. The KEEP position assumes a terrain-dominated far
   field (current generation). Re-evaluate per worldgen feature.
3. **Shared SDF consumers could amortize a global field.** Iteration-6 commits SDF
   soft shadows/AO and volumetric clouds as additional SHIELD-RT consumers. If those
   need a global SDF anyway, building the far SDF for primary rays too becomes
   marginal-cost — possibly justifying a unified SDF even at a per-ray terrain loss.
   Decide the cloud/shadow SDF need *before* finalizing the brick pool cap.
4. **Brick clipmap memory is capped but unmeasured for real sparsity.** The 96 MB cap
   is an engineering target; actual sparse-brick allocation depends on how much 3D
   content exists. Measure real coverage in Wave A; if bricks blow the cap, the
   feature must degrade (fewer clipmap levels / coarser bricks), never steal from the
   grass/cloud budget.
5. **Conservative-mip approximation is cheap but not exact (33-miss gap).** The
   min-magnitude-minus-half-diagonal bound leaves residual overshoot on the steepest
   views. JCGT-2022 correct trilinear interpretation closes it at higher per-step
   cost. Gate the visual impact; do not assume zero misses.
6. **Seam/parity at 1536 m is the gate that can sink the feature.** If the raymarched
   far field cannot match the `FarLodSystem` mesh silhouette/depth within the seam
   gate's tolerance, ship the mesh and keep the raymarcher behind its flag. Treat the
   parity diff as a hard acceptance criterion, not a nice-to-have.
7. **Over-relaxation step-win figure is engine-internal, not externally pinned.** The
   spike cites a 25–40% step reduction from Keinert over-relaxation (ω≈1.2–1.6); the
   exact external percentage for our grid SDF was not verified from the primary paper
   in this pass. Treat as a tuning parameter to measure, not a guaranteed win.

---

## Citations (source + url + specific finding)

- **Epic Games — Lumen Technical Details (UE5 official docs)**
  https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine
  *Verified finding:* "By default, Lumen traces against each mesh's distance field for
  the first two meters for accuracy, and the merged Global Distance Field for the rest
  of each ray." Lumen Scene View Distance default 200 m, expandable to 800 m. Software
  ray tracing is the default. → Confirms the **hybrid detail-near / global-far policy**
  the KEEP recommendation mirrors.
- **Epic Games — Mesh Distance Fields in Unreal Engine (UE5 official docs)**
  https://dev.epicgames.com/documentation/unreal-engine/mesh-distance-fields-in-unreal-engine
  *Verified finding:* Global Distance Field = "a few volume textures centered around
  the camera, called clipmaps"; per-object Mesh DFs composited in; "only newly visible
  areas or those affected by the scene modification need to be updated, so composition
  doesn't cost much." Per-mesh DF caps at 8 MB @ 128³. → **Camera-centered clipmaps +
  incremental update + brick storage** pattern, and a concrete per-mesh VRAM datapoint.
- **UWA — UE5 Lumen Implementation Analysis**
  https://blog.en.uwa4d.com/2022/01/25/ue5-lumen-implementation-analysis/
  *Finding (secondary):* GDF clipmaps "smaller and denser closer to the camera; larger
  and sparser farther away," range cm-to-km; SDF stores nearest-surface distance so
  raymarch step length improves without penetrating the surface. → Confirms
  **denser-near/sparser-far clipmap** geometry.
- **Hansson-Söderlund, Evans, Akenine-Möller — "Ray Tracing of Signed Distance Function
  Grids," JCGT vol. 11 no. 3, 2022**
  https://jcgt.org/published/0011/03/06/ ;
  https://research.nvidia.com/publication/2022-09_ray-tracing-signed-distance-function-grids
  *Verified findings:* sparse SDF grids stored as bricks via indirection tables;
  optimized **analytic ray vs trilinear-interpolated isosurface intersection** (cubic
  solver or repeated linear interpolation); fastest combo = BVH over non-empty voxels +
  that intersection; novel continuous cross-voxel normals; **averaged/naive sampling
  cannot guarantee a valid distance lower bound (overshoots the trilinear surface)** —
  trilinear interpretation is required. → Pins the **conservative-coarse-level
  correctness criterion** and the brick-trace hit-refinement method.
- **Sebastian Aaltonen — "GPU-Based Clay Simulation and Ray-Tracing Tech in Claybook,"
  GDC 2018**
  Slides: https://media.gdcvault.com/gdc2018/presentations/Aaltonen_Sebastian_GPU_Based_Clay.pdf ;
  Video: https://www.youtube.com/watch?v=Xpf7Ua3UqOA
  *Findings:* shipped 60 fps console game ray-tracing a GPU-resident world SDF volume
  with a mip pyramid + trilinear filtering; coarse-mip-first traversal, cone footprint
  for mip selection. *Secondary-verified scale:* dense world SDF **1024×1024×512 @ 8-bit,
  ~586 MB across 5 mips**, distances [-4,+4] voxels at 1/32 precision. → Existence proof
  for GPU SDF marching AND the cautionary memory scale that favors keeping heightfields.
- **Keinert, Schäfer, Korndörfer, Niessner, Stamminger — "Enhanced Sphere Tracing,"
  STAG 2014**
  https://www.lgdv.tf.fau.de/publications/enhanced-sphere-tracing/
  *Finding:* over-relaxation step multiplier ω ∈ [1,2) with disjoint-sphere safe
  fallback; cheap step-count reduction. → Brick-trace acceleration (b) in adoption
  order. *Exact % win for our grid: UNVERIFIED — measure.*
- **Inigo Quilez — "Terrain Raymarching" / "Raymarching Distance Fields"**
  https://iquilezles.org/articles/terrainmarching/ ; https://iquilezles.org/articles/raymarchingdf/
  *Finding:* heightfield-specific march (distance-scaled stepping against y=f(x,z) with
  distance LOD) is a different, cheaper algorithm than generic SDF sphere tracing for
  terrain. → Basis of the KEEP heightfield-march primary path.
- **Alex Evans, Media Molecule — "Learning from Failure (Dreams)," SIGGRAPH 2015
  Advances in Real-Time Rendering**
  https://advances.realtimerendering.com/s2015/
  *Finding:* abandoned direct SDF raymarching after three renderers; shipped point
  splatting generated from the SDF. → Kill-criteria / fallback reference.
- **luminumbra internal — T-I4-15 SHIELD-RT Spike Memo**
  `.forge/artifacts/engine-iteration-4/shieldrt-spike-memo.md` (+ data
  `build/debug/test-artifacts/performance/shieldrt_spike.json`)
  *Findings (engine-internal, CPU Debug):* heightfield march **2.0 steps/ray flat,
  7.3 mountains**; SDF **7–12× more steps/ray** on common far views; SDF **2.6× memory
  on mountains, growing with relief**; naive avg mips **1 602/57 600 overshoots vs 33**
  conservative (**48×**). → The position this brief confirms; absolute ms are CPU and
  NOT a GPU budget.
- **luminumbra internal — Area 3 literature survey**
  `.forge/artifacts/engine-research/worldgen-lockstep-sdfrt.md`
  *Findings:* spike scope, hybrid-split policy, conservative-coarse-level success
  criterion, render-only/no-world_hash discipline, shared-consumer dividends.
