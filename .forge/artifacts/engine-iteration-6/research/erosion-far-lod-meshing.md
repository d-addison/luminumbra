# Research brief: Hydraulic/thermal erosion + far-LOD meshing

Slug: `erosion-far-lod-meshing`
Iteration: 6, Wave 0 (research-gated)
Date: 2026-06-15

## TL;DR

Two separable concerns are bundled in this topic; treat them as two tasks with
different determinism classes:

1. **Erosion (worldgen, world_hash-AFFECTING).** Adopt a **grid-based virtual-pipe
   hydraulic model (Mei et al. 2007)** + a **Musgrave 1989 thermal/talus relaxation
   pass**, baked **offline into a deterministic per-region height-offset field on the
   CPU** (or a fixed-iteration-order CPU pass), NOT a live GPU sim. The eroded height
   becomes an additive term in the single shared analytic height path
   (`ComputeShapedHeight`), so near chunks and far-LOD tiles erode identically at the
   seam, exactly as the existing river carve does today.
2. **Far-LOD seam debt (RENDER-only, world_hash-NEUTRAL).** Keep the current
   heightfield approach; if/when full 3D voxel LOD (overhangs/caves) is meshed, adopt
   **Transvoxel transition cells (Lengyel 2010)** rather than Dual Contouring. The
   current Marching-Cubes far tiles already share border vertices + drop perimeter
   skirts; Transvoxel is the principled crack-free upgrade and is patent-free.

**Critical sequencing answer (the question this brief exists to settle): land the
shape-affecting erosion BEFORE Wave A's SHIELD-RT parity baselines bless.** Erosion
changes the terrain SHAPE that SHIELD-RT renders and the far-field parity corpus is
built against. Blessing the parity baseline first and then landing erosion forces a
guaranteed second re-bless of the entire far-field corpus. Sequence erosion as a
deliberate world_hash bump in Wave 0/early Wave A, re-bless replay+lockstep once, THEN
let SHIELD-RT capture its parity baselines against the eroded terrain. If schedule
forces erosion after parity bless, budget it explicitly as a second corpus re-bless
(both the LREC1 replay re-bless AND the SHIELD-RT far-field parity corpus regen).

---

## Recommended approach

### Erosion model

**Grid virtual-pipe hydraulic erosion (Mei/Decaudin/Hu 2007).** Each grid cell carries
terrain height `b`, water height `d`, suspended sediment `s`, an outflow flux vector
`(fL,fR,fT,fB)`, and a velocity `(u,v)`. One step is a fixed sequence of passes:

1. Water increment from rain/source: `d1 = d + dt * rain`.
2. Flux update from hydrostatic head: `fX = max(0, fX + dt * A * g * dh / l)` for each
   of 4 neighbours, then scale all four by `K = min(1, d1*l*l / ((fL+fR+fT+fB)*dt))` so
   you never drain a cell below zero (this scaling factor is the documented stability
   guard; it is what keeps the explicit scheme non-blowing).
3. Water height update from net flux divergence -> `d2`.
4. Velocity field from average flux through each axis.
5. Sediment transport capacity `C = Kc * sin(local_tilt) * |velocity|`. If `C > s`:
   dissolve `Ks*(C-s)` into water (lower terrain); else deposit `Kd*(s-C)` (raise
   terrain). (A documented practical deviation: multiply `C` by `clamp(d,0,1)` so thin
   films don't dissolve unrealistically — verified failure mode + fix.)
6. Sediment advection (semi-Lagrangian backtrace of `s` along velocity).
7. Evaporation `d = d2 * (1 - Ke*dt)`.

**Thermal erosion / talus (Musgrave 1989).** Interleave a relaxation pass: where the
slope between a cell and a neighbour exceeds the talus angle `T` (tan ~= angle of
repose, 30–35 deg), move a fraction of the excess material downhill. This is what kills
"smooth cones / blob rocks": ridges get talus slopes and scree, and it directly
addresses the "smooth cone" complaint that pure FBM produces. Thermal is cheap and
unconditionally stable for small per-step transfer fractions.

**Why this fixes the three named defects:**
- *Smooth cones*: thermal talus carves angle-of-repose facets; hydraulic incises
  drainage networks into otherwise-radial FBM mounds.
- *Blob rocks*: thermal relaxation + hydraulic channel incision break radial symmetry.
- *Shoreline seams*: hydraulic deposition builds graded beaches/deltas where channels
  meet `SEA_LEVEL`; combined with item below the shore stops being a hard noise contour.

### How it lands deterministically (the load-bearing design)

Do NOT run this as a live sim system on the 30 Hz tick, and do NOT run it as a live GPU
compute pass that feeds world_hash. Instead **precompute a per-region eroded-height
offset field offline (or in a deterministic generation-time pass) and bake it into the
shared analytic height path** as an additive term, exactly the way the river carve is an
additive term in `ComputeShapedHeightSample` today (`pre_carve_height` ->
`river carve` -> `final_height` in `SHIELD_WorldSystem.cpp`). The offset is keyed by
`(seed, params_hash, region)` and stored/streamed like a far-LOD tile.

Rationale, pinned to the engine determinism contract:
- The whole terrain today flows through ONE shared height implementation
  (`SHIELD_WorldSystem.h` "the ONE shared height implementation", `ComputeShapedHeight`)
  so the scalar path, the SIMD batch path, `GetTerrainHeightAt`, far-LOD tiles, biome
  climate sampling, and `GetTerrainHeightAtCoarse` cannot diverge. Erosion MUST enter
  through this same chokepoint or near/far and scalar/batch will desync.
- Erosion is iterative and stateful (a relaxation over thousands of steps); you cannot
  recompute it per-column analytically. So it must be precomputed into a sampled offset
  field that the analytic path then bilinearly samples — same shape as `FarLodTile` and
  `FieldGrid<float>`.
- Determinism: the offset bake must use integer iteration order and the project's
  `DeterministicMath` wrappers (precise FP, contraction off, no libm transcendentals)
  so the baked field is byte-identical across machines. Run it on the CPU in the
  generation TUs (NOT sim TUs subject to `SimDeterminismLint`, but still under the same
  FP discipline since the OUTPUT becomes world_hash input).

### Far-LOD meshing

**Recommendation: stay on the current shared-border + skirt heightfield mesher for the
heightfield far field; reserve Transvoxel for the day the far field becomes true 3D
voxels.** The current `FarLodStore`/`MarchingCubes::GenerateFarLodRegionMesh` already
(a) shares edge vertices between adjacent regions (`samples_per_side = 512/step + 1`,
border row/col shared) and (b) drops a one-sample-step perimeter skirt to hide
tier-boundary cracks (F1<->F2 and far<->near). That is a correct, cheap crack-mask for
heightfields and does not need Transvoxel.

If iteration 6+ moves the far field to a genuine 3D voxel density (needed for caves,
overhangs, arches that hydraulic-with-overhangs erosion can produce), adopt
**Transvoxel transition cells**: a transition cell samples 13 points (9 on the
high-res face, 4 on the low-res face), bridging a block to its exactly-half-resolution
neighbour with a 512-case (73 equivalence-class) lookup table alongside the standard
256-case regular cell table. Hard constraint to design around: **adjacent blocks may
differ by at most one LOD level** — the F1/F2/near tiering must enforce a one-level
step at any boundary. Transvoxel is patent-free (Terathon) and was shipped in the C4
Engine, so it is safe to vendor the tables.

---

## Alternatives considered (and why rejected)

- **Live GPU virtual-pipe erosion feeding world_hash (Št'ava 2008 / Jako 2011 style).**
  Rejected for the world_hash path. GPU floating-point is not reproducible across
  vendors/drivers: parallel reductions and atomic-add execute in thread-arrival order,
  and FP addition is non-associative, so results differ run-to-run and machine-to-machine
  even under IEEE-754. This directly violates the desync-oracle contract. (GPU erosion is
  fine for a NON-authoritative preview/visualizer, but its output must never enter
  world_hash.) The fast perf numbers (RTX 3070: ~6 ms/step at 2048², ~23 ms/step at
  4096²) are attractive but only usable for a render-only preview.
- **Particle/droplet erosion (Job Talle, "snowballs").** Simpler and faster than the
  grid model and produces nice channels, but (a) it is RNG-ordered (droplet spawn order
  matters) which is hostile to determinism unless the spawn schedule is fully pinned,
  and (b) it produces dendritic channels but weaker graded shorelines/deltas than the
  pipe model's explicit deposition. Viable as a cheaper deterministic CPU alternative IF
  the droplet schedule is a fixed integer sequence; keep as fallback if grid-pipe bake
  cost is too high.
- **Dual Contouring (Ju et al. 2002) for far-LOD.** Rejected as the primary mesher. DC
  reproduces sharp features via per-cell QEF-minimized vertices and naturally supports
  octree LOD, but: (a) it can produce non-manifold geometry and self-intersections
  needing extra handling (Manifold Dual Contouring), (b) it requires Hermite data
  (edge-intersection points + normals) we don't currently store, and (c) it loses sharp
  features exactly where Hermite normals are inaccurate (near the features). For a mostly
  smooth eroded terrain the sharp-feature advantage is marginal and the manifold/seam
  complexity is a net loss vs Transvoxel's table-driven crack-free transitions.
- **Bake erosion into raw FBM offline as a static heightmap asset.** Rejected: breaks
  the "pure function of (seed, params)" regenerable-cache property of pristine tiles and
  the infinite/streamed extent; you'd lose procedural regen and the params_hash cache key.

---

## Perf budget (concrete)

**Generation-time (offline / CPU bake) — NOT in the frame budget:**
- Grid-pipe + thermal is ~12–15 float ops/cell/step over a handful of arrays. As a
  reference scale, a tuned GPU implementation reports ~6 ms/step at 2048² and ~23 ms/step
  at 4096² on an RTX 3070; a single-thread CPU bake is ~1–2 orders slower per step, so
  budget the CPU bake per region, not for the whole world at once.
- A 512 m region at the F1 4 m lattice is 129² ~= 16.6k cells; at a 2 m erosion working
  lattice ~257² ~= 66k cells. Convergence for visible drainage networks is typically
  ~200–1000 hydraulic steps interleaved with thermal. Estimate: **tens of ms to low
  hundreds of ms of CPU per region bake, multi-threaded across regions off the sim
  thread.** This is a streaming/prefetch cost amortized like pristine far-LOD tile
  generation, not a per-frame cost.
- VRAM: zero at runtime for the bake. The baked offset is stored exactly like a far-LOD
  tile: u16 height-offset per sample. One F1 region offset tile ~= 129² * 2 B ~= 33 KB;
  fits comfortably inside the existing 64 MB `FarLodStore` residency budget. Recommend
  carrying erosion as additional packed bytes in the existing `FarLodTile` rather than a
  parallel store.

**Runtime GPU (this topic's claim on the contended Wave-A GPU budget):**
- Erosion bake: **0 ms GPU, 0 MB VRAM** at runtime (it is generation-time CPU). This is
  the recommended path's headline property and the reason to prefer it.
- Far-LOD meshing (Transvoxel or current MC): meshing is CPU/async, not per-frame GPU.
  The far G-buffer draw cost is unchanged (same vertex count class). Reserve **< 0.5 ms
  GPU** for the far G-buffer pass; no additional VRAM beyond the existing 64 MB far store
  + the mesh VBOs already accounted for.
- Net: **this topic should reserve ~0 ms of the shared SHIELD-RT/grass/clouds GPU
  budget.** That is deliberate — it keeps the whole erosion realism win off the contended
  GPU so Wave A can spend its ms on raymarch/grass/clouds.

---

## Determinism implications

- **Erosion = SIM-side / world_hash-AFFECTING.** It changes terrain shape, which is a
  per-system sub-hash input. It MUST land as a deliberate world_hash bump in its own
  commit, with LREC1 replay re-bless and delay-based lockstep re-bless. The bake itself
  must be deterministic: integer iteration order, `DeterministicMath` wrappers, pinned FP
  flags (precise, contraction off), no libm transcendentals (`sin` for the tilt term must
  go through the project's deterministic approximation), no unordered-container iteration.
- **The bake output (the offset field) is the world_hash input, so its byte layout must
  be canonicalized** like `FieldGrid` (z-major, integer-indexed) and `FarLodTile`
  (row-major) snapshot order. Quantize the offset (u16, fixed scale) before it enters the
  hash so float noise can't flap the hash.
- **GPU is forbidden in the authoritative path** (see Alternatives): any GPU erosion is
  render/preview-only and never touches world_hash.
- **Far-LOD meshing (Transvoxel/MC) = RENDER-only / world_hash-NEUTRAL.** It consumes the
  (already-hashed) height/offset field and emits triangles; it never feeds world_hash.
  Changing the mesher does not bump world_hash. (It DOES change SHIELD-RT parity captures,
  which is a separate baseline, not world_hash.)

### Sequencing vs SHIELD-RT Wave A parity (explicit)

- SHIELD-RT builds far-field parity baselines against the terrain SHAPE. Erosion changes
  that shape. **Order: erosion world_hash bump + re-bless FIRST, then SHIELD-RT captures
  parity baselines against eroded terrain.** One re-bless total.
- Reverse order costs TWO re-blesses: bless parity on un-eroded terrain, then erosion
  invalidates both the world_hash (LREC1 + lockstep re-bless) AND the entire SHIELD-RT
  far-field parity corpus (regen `.forge/parity-corpus.jsonl` baselines). If schedule
  forces this, budget the double re-bless explicitly and gate it.

---

## Integration notes (luminumbra files/systems touched)

- `src/luminumbra_common/systems/SHIELD_WorldSystem.{h,cpp}` — erosion offset enters the
  ONE shared height path (`ComputeShapedHeight` / `ComputeShapedHeightSample`), as an
  additive term alongside the existing river carve (`pre_carve_height` ->
  `final_height`). `GetTerrainHeightAt`, `GetTerrainHeightAtCoarse`, `SampleWorldGenLayers`,
  and the SIMD batch loop all then inherit it for free. Add the erosion offset to
  `TerrainGenParams` (and thus to `ComputeTerrainParamsHash`) so it participates in the
  params_hash cache key. NOTE: the existing `erosion`/`m_erosion_generator` channel is a
  NOISE amplitude-multiplier spline, NOT real erosion — name the new system distinctly
  (e.g. `ErodedHeightOffset`) to avoid confusion.
- `src/luminumbra_common/world/FarLodStore.{h,cpp}` — extend `FarLodTile` with a packed
  per-sample erosion-offset stream (or carry the post-erosion height directly), included
  in `ComputeFarLodTileHash` and the LMR1 record. Pristine tile rebuild
  (`BuildPristineFarLodTile`) must apply erosion so far tiles match near chunks at the seam.
- `src/luminumbra_common/fields/FieldGrid.h` — the erosion working/offset field is a
  natural `FieldGrid<float>` consumer (the same plumbing iter-6 Aetheric reuses), giving
  canonical z-major snapshot order for free. Respect the iter-6 API freeze.
- `src/luminumbra_common/world/MarchingCubes.{h,cpp}` — `GenerateFarLodRegionMesh` +
  `AddBoundaryTransitionSkirts` are the current crack handling; Transvoxel tables would be
  vendored here ONLY if/when the far field becomes 3D voxel density.
- `src/luminumbra_client/rendering/FarLodSystem.cpp` + `RenderPipeline.cpp` — far G-buffer
  draw; unchanged by erosion (consumes the same tile mesh). SHIELD-RT parity capture path
  re-baselines here.
- New generation-time module (e.g. `src/luminumbra_common/world/TerrainErosion.{h,cpp}`),
  built off the sim thread; output keyed `(seed, params_hash, region)`.

---

## Open risks

- **Streaming boundary consistency.** Erosion is non-local (water/sediment flow across
  region edges). A per-region bake with no halo will produce seams at region borders —
  the very class of defect we're fixing. Mitigation: bake with a generous overlapping
  halo (e.g. +1 region or +N cells) and crop to the region interior; verify the cropped
  result is independent of halo width (adversarial check). This is the single biggest
  correctness risk and must be in the acceptance test.
- **Determinism of the tilt term.** `C = Kc * sin(tilt) * |v|` uses `sin`; the bake must
  route this through the deterministic math approximation, not libm, or cross-machine
  world_hash will diverge. Verify byte-identical offset fields on two machines before bless.
- **Convergence/iteration-count tuning vs bake-time budget.** Too few steps = no visible
  drainage; too many = streaming hitches. Needs a tuned fixed step count baked into params
  (and into params_hash) so it is reproducible.
- **Double re-bless if sequencing slips** (see Determinism section) — schedule risk, not a
  technical one.
- **Transvoxel one-LOD-level constraint** — if adopted, the F1/F2/near tier scheduler must
  guarantee no boundary skips a level; current 3-tier scheme already steps F1->F2->near,
  but verify no diagonal/corner 2-level jumps.

## Unverified / could not confirm

- Exact numeric Kc/Ks/Kd/Ke and dt values from the Mei 2007 PDF (HAL/Inria mirror behind
  an anti-bot wall; IEEE Xplore paywalled). The algorithm STRUCTURE and the K-scaling
  stability guard are confirmed across multiple secondary implementations; the specific
  constants must be pulled from the paper PDF or a reference implementation during
  implementation and are tuning params anyway.
- The RTX-3070 6 ms/2048², 23 ms/4096² figures come from a single secondary
  source (search summary of a virtual-pipe GPU implementation); treat as order-of-magnitude
  for the GPU-preview alternative, not a committed budget (the recommended path is CPU bake,
  0 ms GPU).
- Whether luminumbra already has a deterministic `sin` approximation in `DeterministicMath`
  — assumed present per the contract; confirm before relying on it.
