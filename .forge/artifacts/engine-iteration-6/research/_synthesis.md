# Iteration-6 Wave-0 Cross-Cutting Synthesis

Cross-cutting synthesizer output. Resolves the concerns no single research brief
owns: the shared GPU budget split, the world_hash bump sequencing, the
build-once volumetric/raymarch substrate, the within-iteration wave ordering,
the frozen FieldGrid contract both render and sim must consume, and the open
questions for the spec round.

Grounded against the same engine context the five briefs share: 30 Hz
deterministic sim; one shared GPU budget; `FieldGrid<T>` frozen for iter-6
(`src/luminumbra_common/fields/FieldGrid.h`, verified); `FarLodStore` F1/F2
far-field (64 MB residency); `RenderPipeline` ordered passes
(lighting -> water -> skybox -> aerial -> foliage -> particle -> HDR).

---

## 1. GPU budget split (ms)

**Assumed total: 16.6 ms/frame GPU at 60 fps, of which a hard 5.3 ms shared
"iter-6 new-systems pool" is carved out for the four contended consumers.** The
remaining ~11.3 ms stays with the already-shipped passes (G-buffer/terrain,
lighting incl. existing cloud cast-shadow, water, HDR/tonemap, particles) which
are NOT iter-6 new work and already fit the pre-iter-6 frame. The 6x-view-
distance target is what makes the far-field consumers (SHIELD-RT, far grass
handoff, cloud horizon) the dominant new cost, so the pool is sized to them.

The four briefs' own asks sum to ~6.5-7.3 ms if each takes its stated upper
bound, which **over-subscribes** a 5.3 ms pool. The split below resolves that by
holding each to its *typical* (not worst-case) figure and making the worst-case
headroom mutually exclusive — only one consumer spikes per frame, never all
four — backed by per-consumer degrade levers.

| Consumer | Brief ask | Allocated (typical) | Hard ceiling | Degrade lever |
|---|---|---|---|---|
| SHIELD-RT far-field raymarch | <= 2.0 ms | **2.0 ms** | 2.3 ms | brick cap 96 MB fixed; heightfield max-mip primary; if over, coarser brick clipmap levels (never steals others) |
| GPU grass | <= 2.0 ms | **1.4 ms** | 2.0 ms | lower live-ring blade count / earlier terrain-texture handoff |
| Volumetric clouds + froxel fog | ~2.0-2.5 ms | **1.6 ms** | 2.2 ms | coverage/step-count knob; 1/16 temporal amortization is the baseline, can drop to 1/32 |
| Aetheric-field render | <= 0.10 ms | **0.1 ms** | 0.2 ms | lighting-pass tap only; any volumetric glow pass budgeted separately later |
| **Pool total** | — | **5.1 ms** | **5.3 ms cap** | — |

Notes that make the split honest:

- **Clouds partly REPLACE, not add.** The tier-2 froxel path subsumes the
  existing analytic aerial pass (`aerial_gpu_ms`) and sharpens the existing
  cloud cast-shadow. Net new cost is the cloud raymarch (~1.2-1.8 ms) minus the
  retired aerial pass, so 1.6 ms allocated is conservative-realistic.
- **Clouds amortize on the SAME temporal history as SHIELD-RT** (see §3), so
  marginal cloud cost after the substrate exists is lower than standalone.
- **Grass REMOVES CPU cost** (up to 262144-iter scatter + ~9 MB/frame ring
  upload), so its GPU add is partly offset elsewhere in the frame.
- **0.2 ms of slack (5.1 -> 5.3)** is the explicit spike reserve; the contended
  variable is the SHIELD-RT brick path. Per the SHIELD-RT brief, if the
  heightfield march profiles cheaper than 1.2 ms, slack returns to the *shared
  pool*, it does NOT enlarge the brick cap.
- **All ms are engineering targets to re-measure** on the per-pass GL timestamp
  timers against the quiet-machine baseline. The SHIELD-RT spike numbers were
  CPU Debug (cost-shape only). Wave A ratifies the real split before fan-out.

---

## 2. world_hash bump sequencing — Aetheric FIRST, then shaping, then hydro, never same commit

Revised by the Wave-A spec and T-I6-020: Wave A has three deliberate
world_hash-affecting changes, each attributable to its own commit and verifier
surface. Required order:

**Bump #1 (commit A): Aetheric scalar field.** Sim-authoritative new sub-hash
slot `aether` (fnv1a-64 over IEEE bits, modeled on `ComputeWindSubHash`), ticked
after weather, folded into the runner-level world_hash as an append-only
`|aether:` term. T-I6-020 records the composite bump
`d950a6afc12a5cdc -> f17726d44054d133`. Re-bless: LREC1 replay + delay-based
lockstep. Cost: one re-bless pass; the Aetheric field does NOT change terrain
geometry, so it does not invalidate any far-field render parity corpus.

**Bump #2 (commit B): shaping-spline params-hash fold.** Worldgen-params
change only: `ComputeTerrainParamsHash` folds `shaping_enabled`, the shaping
frequencies, peaks controls, and the three spline control-point arrays. It is
gated on `shaping_enabled`, uses marker `0x05`, and lands before hydro so the
hydro commit has a single attributable params-hash delta.

**Bump #3 (commit C): hydraulic relief baked-grid lookup.** Sim-authoritative;
adds the baked hydro offset grid as the single terrain source for all
single-point callers, with deterministic bilinear lookup and marker `0x04` in
`ComputeTerrainParamsHash`. Re-bless: LREC1 replay + lockstep AND it changes the
terrain SHIELD-RT renders.

**Why this order minimizes re-bless cost:**

The expensive coupling is *hydro changes the terrain geometry that SHIELD-RT's
far-field parity baselines capture*. SHIELD-RT parity (the A/B silhouette/depth
diff at 1536 m vs the FarLodSystem marching-cubes mesh) must be blessed against
the FINAL terrain. If hydro lands AFTER SHIELD-RT parity is blessed, you
re-bless twice: once for the hydro sim bump (LREC1 + lockstep) AND a full
regeneration of the SHIELD-RT far-field parity corpus against hydro terrain.

Putting **hydro before the SHIELD-RT parity bless** collapses that into one
re-bless — the parity corpus is *born* hydro-shaped.

Aetheric goes first because it is geometry-neutral and cheap to bless, so it
clears out of the way and never entangles with the terrain corpus. The shaping
params fold goes second because shipped shaped presets already depend on those
fields and the correction must not be hidden inside the hydro bump. Hydro goes
third, immediately before Wave A.2 locks SHIELD-RT parity, so the heavy corpus is
captured exactly once against final terrain. The three are never in the same
commit because each is an independent deliberate bump with its own replay
re-bless protocol; merging them would make a replay failure ambiguous as to
which sub-hash diverged and would force re-blessing multiple systems on any
single-system regression.

**Ordering invariant for the scheduler:** `Aetheric bump` (commit A) ->
`shaping-spline params fold` (commit B) -> `hydro baked-grid bump` (commit C) ->
`SHIELD-RT parity baseline bless`. The hydro commit must precede the SHIELD-RT
parity bless even though SHIELD-RT is render-only and world_hash-neutral,
because its *parity capture* (a separate baseline, not world_hash) reads the
hydro-shaped geometry.

---

## 3. Shared froxel/raymarch infrastructure (build once)

SHIELD-RT far-field and volumetric clouds both raymarch and both want temporal
amortization. The build-it-ONCE mandate: a single
`src/luminumbra_client/rendering/volumetric/` module, **landed and FROZEN by
Wave A (SHIELD-RT)**, into which Wave B (clouds) plugs a second march kernel as a
pure consumer. Co-owned edit churn is the named risk; the freeze is the
mitigation (same discipline as the FieldGrid freeze).

The substrate exposes, as the common contract:

1. **Ray setup from G-buffer depth** — reconstruct world-space ray
   origin/direction per pixel from the depth buffer + inverse view-proj. Both
   the SHIELD-RT terrain/brick march and the cloud march start here.
2. **Half-res render target + bilateral depth-aware upsample** — render the
   march at half-res into RGBA16F, upsample with a depth-aware bilateral filter.
   Clouds need this for the <2 ms regime; SHIELD-RT brick segments can reuse it.
3. **Temporal reproject + history + disocclusion-reject** — reprojection from
   previous frame, history buffer, disocclusion rejection. This is the lever
   that makes BOTH affordable (clouds at 1/16 pixels/frame; SHIELD-RT can
   amortize brick-segment cost). Clouds explicitly amortize on the SAME history
   buffers, which is why their marginal cost is lower once SHIELD-RT lands it.
4. **Blue-noise jitter LUT** — shared spatiotemporal blue-noise for sub-pixel /
   sub-step jitter, consumed by both march kernels.
5. **Empty-space skip + conservative-mip 3D-texture sampling** — the
   skip-safe traversal primitive. SHIELD-RT uses it over the height max-mip
   pyramid and the sparse brick clipmap; clouds use it over the 128^3 base-shape
   texture. The conservative-mip *sampling* helper (lower-bound-safe) is shared;
   each consumer supplies its own field.
6. **Froxel volume** (frustum-aligned, ~160x90x64, exponential depth slices,
   RGBA16F + history) — owned here. Clouds composite into it; the froxel volume
   also serves aerial + ground fog + god-rays + cloud-shadow shafts. SHIELD-RT
   does not need the froxel volume itself but DOES share items 1-5.

What is NOT shared (consumer-specific, kept out of the frozen substrate):
SHIELD-RT's heightfield quadtree march + height max-mip + sparse SDF brick pool;
clouds' density model (Perlin-Worley base + Worley erosion), Beer-Powder /
dual-HG lighting, weather-cell coverage coupling. Each is a kernel/data plugged
into the substrate, not part of it.

Feature-flag both kernels until their gates pass; reuse the existing
`kEnableExperimentalGpuSdfIntegration` gating pattern
(compile_time_enabled / runtime_requested / runtime_allowed) as the home, so
both ship dark behind a toggle while their primary paths land.

---

## 4. Recommended within-iteration wave ordering

Driven by two hard dependencies: (a) the erosion->SHIELD-RT-parity sequencing
(§2), and (b) Wave A must land+freeze the shared volumetric substrate (§3)
before clouds can consume it.

**Wave 0 (this synthesis + spec round).** Lock the GPU split, the bump
sequencing, the FieldGrid contract (§5), and the substrate API surface. No code.

**Wave A — substrate + the two sim bumps, in this internal order:**
- A1. **Aetheric scalar field** (bump #1, commit A). Geometry-neutral, cheap
  re-bless, unblocks the FieldGrid<float> contract validation early.
- A2. **Erosion offset** (bump #2, commit B). Lands the terrain shape change
  BEFORE any far-field parity is blessed.
- A3. **Shared volumetric/raymarch substrate + SHIELD-RT primary path.** Build
  and FREEZE the substrate (§3). Land the heightfield max-mip primary tracer;
  brick clipmap behind its flag. **GPU-profile both paths first** (the
  top-priority UNVERIFIED that could flip the primary tracer) before committing.
  SHIELD-RT far-field parity baseline blesses here — against eroded terrain.

  A1/A2 are independent and can run in parallel worktrees; A3's parity bless
  gates on A2 being merged.

**Wave B — render-only consumers of the frozen substrate (parallelizable):**
- B1. **Volumetric clouds + froxel fog.** Pure consumer of the §3 substrate
  (second march kernel + froxel volume). Replaces the analytic aerial pass.
- B2. **GPU grass.** Independent of the substrate but contends for the GPU pool;
  reads scene depth + the analytic cloud state (must read the IDENTICAL cloud
  projection — coordinate with B1 on the CloudRenderState math, do not re-derive).
- B3. **Aetheric render coupling** (lighting-pass emissive tap). Trivial; can
  ride with A1 or land in B; render-only, no hash impact.

Wave B is fully render-only (no further world_hash bumps), so its only shared
constraint is the GPU pool split ratified in A3. Grass and clouds run in
parallel; the cloud-shadow registration is the cross-task seam between them.

**Closeout.** Confirm HeadlessServerTick world_hash unchanged after Wave B
(proves render-only), PerfRegression against the ratified split, visual-QA on
seams (near<->far SHIELD-RT dither band, grass LOD thinning, cloud silhouette
ghosting), erosion halo-independence acceptance test.

---

## 5. Frozen FieldGrid<T> contract (render sampling + sim solving)

`FieldGrid<T>` is frozen for iter-6 (verified header
`src/luminumbra_common/fields/FieldGrid.h`). It is **pure storage** — flat
`std::vector<T>` row-major, integer indexing, NO floating-point, NO hashed
containers, NO RNG, NO wall-clock. The canonical order is **z-major then x
ascending** (`index(lx,lz) = lz*stride + lx`), and that order IS the
snapshot/sub-hash order. This is the single fixed contract both consumers build
on. It must NOT change in iter-6; new behavior lives in the *systems* that own a
grid, never in the container.

What the frozen surface already exposes and both sides rely on (do not extend
the container — these are the load-bearing members):

- `FieldGrid(extent_cells, cell_size_m)` + `extent_cells()` / `cell_size_m()` —
  the pinned geometry. Aetheric reuses the wind geometry (24 m cells, 64x64 =
  1536 m). Sim solves on it; render samples it at the same geometry.
- `stride()`, `cell_count()`, `index(lx,lz)`, `in_bounds(lx,lz)` — the canonical
  integer addressing. The fixed iteration order for diffuse/advect (sim) AND for
  the sub-hash. Render sampling uses the same `index`/`in_bounds` so a sampled
  cell is byte-identically the cell the sim wrote.
- `origin_cell_x/z()` + `set_origin_cells(cx,cz)` — integer cell origin that
  follows the streamed region with NO FP drift. Both sides convert world-pos to
  local cell via the SAME integer floor-division `LocalCell` math
  (out-of-region clamps to base value). This is what lets the render texture
  upload and the sim solve agree on cell identity.
- `at(lx,lz)`, `cells()` (mutable + const), `reset(value)` — element + bulk
  access. Sim writes via `at` in canonical order (double-buffered, hoisted
  scratch, per WindFieldSystem). Render reads `cells()` in canonical order to
  pack the upload texture (e.g. Aetheric RG16F 64x64).

The contract the *systems* (not the container) must honor so both consumers stay
consistent — this is what the spec must pin:

- **One-way render bridge.** Render is a pure CONSUMER. Sampling/upload NEVER
  writes back to grid storage and never influences sim update or eviction order.
  (Same rule as the SHIELD-RT heightfield read of `FarLodStore.height_q`.)
- **Sim side owns world_hash; render side is hash-neutral.** Sim folds a
  `ComputeXxxSubHash()` (fnv1a-64 over raw IEEE bits, canonical order, copied
  from `ComputeWindSubHash`) into world_hash. Render sampling touches no hash.
- **Sim stays float32-deterministic via the canonical order + IEEE-basic-ops +
  DeterministicMath** (no libm transcendentals on the tick path); the fixed
  z-major order is *the* iteration order. Render may use GPU floats freely
  because it never feeds back.
- **2D-per-payload only.** The grid is 2.5D (one payload `T` per (lx,lz) cell).
  Aetheric stores one `FieldGrid<float>` PER channel (aether, fire) — confirm
  single-layer-per-channel suffices (no true 3D need) before committing, or it
  conflicts with the frozen API. Wind packs layers into the payload struct
  (`WindCell.layer[3]`); a scalar channel uses `FieldGrid<float>` directly.

---

## 6. Open questions for the spec round

1. **GPU profile of both SHIELD-RT paths (TOP PRIORITY).** The primary-tracer
   choice (heightfield max-mip vs SDF brick) rests on CPU Debug step-counts; a
   real GPU profile could invert the ranking. Must run in A3 before committing
   the primary tracer. If it flips, the 2.0 ms SHIELD-RT allocation and the
   96 MB brick cap both move.
2. **Is the far field terrain-dominated or pervasively 3D?** SHIELD-RT KEEP
   assumes terrain-dominated. If iter-6 worldgen (post-erosion) produces
   pervasive overhangs/arches/cave-mouths/floating islands across the far field,
   the heightfield is insufficient and the brick SDF becomes mandatory — which
   re-prices the GPU split. Spec must state the far-field 3D-content density.
3. **Do iter-6 SDF soft-shadows/AO get committed?** If shared SDF consumers
   (soft shadows, AO) land, a global SDF becomes marginal-cost and the 96 MB
   brick cap decision changes. Decide before finalizing the cap.
4. **Total frame budget + target hardware.** The 16.6 ms / 5.3 ms-pool split
   assumes 60 fps on a "mid GPU." Spec must state the actual target device and
   fps for the 6x-view-distance goal so the pool is sized to real silicon.
5. **Aetheric: single FieldGrid<float> per channel sufficient?** The frozen grid
   is 2.5D. Confirm no true volumetric (3D) Aetheric need before committing, or
   the freeze is violated.
6. **Fixed Gauss-Seidel iteration count for Aetheric diffuse.** Must be tuned in
   a Wave-A visual spike then PINNED (changing it later is a world_hash bump).
   Also: is semi-Lagrangian advection crisp enough for fire, or does it
   over-smooth? And no `DeterministicMath::Exp` exists — restrict to
   polynomial/linear rates until a golden-tested wrapper lands.
7. **Erosion halo width + step count.** Per-region bake needs an overlapping
   halo to avoid re-introducing region-edge seams; the cropped result must be
   provably halo-width-independent (acceptance test). Fixed step count baked into
   params_hash for reproducibility. Confirm `DeterministicMath` provides a
   deterministic `sin` for the `C=Kc*sin(tilt)*|v|` term.
8. **Substrate co-ownership freeze line.** Exactly which items (§3 list) are
   frozen by Wave A vs. which clouds may extend. Pin this in the spec so Wave B
   is a pure consumer and merge churn is bounded.
9. **Grass deterministic-readback gate.** `FoliageInstancing` asserts a CPU
   instance hash that won't exist once generation is GPU-side; spec must choose
   a new deterministic readback surface or a CPU shadow path, else the gate
   regresses to a no-op.
10. **GL capability baseline.** Confirm the shipped GL context exposes compute +
    SSBO + indirect draw (grass) — the GPUSDF compute stubs are gated off; is
    that a capability gate or a hardware gap? Same question gates the
    SHIELD-RT brick compute and the froxel compute passes.
