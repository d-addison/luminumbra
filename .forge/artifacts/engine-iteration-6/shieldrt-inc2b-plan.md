# A3b inc2b — GPU-resident far-field heightfield (design, scoped 2026-06-16)

Gateway from the validated CPU-bench raymarch (parity + G-buffer gates green) to a
LIVE far-field render pass. Goal: get the FarLodStore heightfield + a **max**-mip
pyramid onto the GPU, kept current as the player streams, so a pass can sample it.

## Decision: centered clipmap + GPU max-mip reduction
- **Heightfield clipmap:** one 2048×2048 **R16** 2D texture centered on the camera
  (~8 MB base, ~11 MB with mips), updated via `glTexSubImage2D` into the quadrants
  that changed as the player moves — mirrors the existing persistent-pool update
  pattern (`ChunkGeometryPool` RenderPipeline.cpp ~193-292; `ParticlePass` ring).
- **Max-mip pyramid:** built ON GPU by a new `res/shaders/far_field_mip_reduction.compute`
  (8×8 local, each thread = max of 2×2 from level L → L+1, dispatched 2048→1).
  **Critical:** `glGenerateMipmap` is a BOX filter (avg), unusable — the raymarch
  needs per-cell MAX (conservative ray-above test). ~22 MB flattened.
- **VRAM:** ~35 MB total (clipmap 11 + maxmip 22 + per-region meshes ~2) vs the
  128 MB FarLodSystem budget → ~3.6× headroom. Fine for 3840×1600 on the 16 GB 5070 Ti.

## CORRECTION — clipmap data source (verified 2026-06-16)
FarLodSystem retains ONLY meshes per resident region (`ResidentRegion` =
VAO/VBO/EBO + AABB, FarLodSystem.h:138-156); the raw `height_q` tile is consumed
during `BuildPristineFarLodTile` → `GenerateFarLodRegionMesh` in the worker and
NOT stored. So the clipmap CANNOT read heightfields from FarLodSystem's resident
state. **Design decision: `ShieldRtFarFieldPass` owns its OWN heightfield clipmap**,
assembled by calling `BuildPristineFarLodTile` for the regions around the camera
(exactly as the benches do via `BuildHeightFieldFromTiles`), keyed by camera region
+ `ComputeTerrainParamsHash`. This decouples the raymarch source from the mesh path
(and lets the raymarch eventually REPLACE the mesh path). No FarLodSystem change
needed; the `heightfield_generation` counter idea is dropped — the pass refreshes a
clipmap quadrant when the camera crosses into a new region (cache miss), not on a
mesh-residency signal.

## Scaffold landed (this increment)
- `kEnableExperimentalFarFieldGpuRaymarching = false` (RenderPipeline.cpp, by the
  GPU SDF flag) + `--enable-far-field-gpu-raymarch` CLI → `config.enable_far_field_gpu_raymarch`
  (inert/byte-stable until the pass reads them).

## Decision: SSBO-backed, no texture-path bench needed
inc2a already proved a fragment pass reading an SSBO heightfield + flattened SSBO
max-mip writes a correct deferred G-buffer; inc2b proved the GPU max-reduction.
So the live pass reuses the **SSBO** approach directly — no texture clipmap / no
extra texture-path bench. The heightfield is a camera-centered SSBO rebuilt on
region-crossing; the max-mip is the validated GPU reduction into a flattened SSBO.

## inc2c — TURNKEY live-wiring plan (decisions resolved)
Execute as ONE focused unit (gated by `kEnableExperimentalFarFieldGpuRaymarching`,
default false → render byte-stable except two re-blessable artifacts):

**Decision A — augment, not replace (v1):** keep the FarLodSystem mesh; the
raymarch fills only pixels the mesh/live geometry didn't (depth-tested). Writes
`gl_FragDepth` + the G-buffer MRT (inc2a encoding), `glDepthFunc(GL_LESS)` against
the existing G-buffer depth so it only fills sky/gaps beyond the mesh. (Replacing
the mesh slabs is a later refinement once parity holds.)

**Decision B — heightfield source:** the pass owns an SSBO heightfield assembled by
porting `BuildHeightFieldFromTiles` into production (it calls the production
`BuildPristineFarLodTile`); rebuild + re-run the GPU max-mip only when the camera
crosses into a new region (cache key = camera region + `ComputeTerrainParamsHash`).

**Files:**
- NEW `src/luminumbra_client/rendering/passes/ShieldRtFarFieldPass.{h,cpp}` — port
  the validated raymarch (inc2a frag), max-mip build (inc2b computes), and
  heightfield assembly; API `set_camera_region(world,camera)` (rebuild on miss) +
  `render(view,viewProj,invViewProj,normalView,eye,viewport,tmax)` into the bound
  G-buffer FBO. Embed the GLSL as string literals (matches the benches; avoids the
  ShaderInventory dir-scan ripple) OR add to res/shaders + PipelineProgramSpecs +
  re-bless `shader-inventory.json` — pick embedded for v1 to minimize ripple.
- `sources.cmake`: add ShieldRtFarFieldPass.cpp.
- `RenderPipeline.h/.cpp`: `m_shieldrt_far_pass` member (constructed only when the
  flag is on), `GpuTimerPass::FarFieldRaymarching` enum + `"shieldrt_far"` name
  (RenderHealth is PRESENCE-based — confirmed validate-engine-frontier.ps1:504/548
  — so an extra pass reporting 0 ms when flag-off is SAFE, no re-bless needed), and
  the execute call in `render_frame` after the G-buffer pass (gated), bracketed by
  begin/end_gpu_pass_timer.
- `main_client.cpp`: when `scenario_config.enable_far_field_gpu_raymarch`, the pass
  is active (the flag plumbing is already landed at 6b2c7df).

**Gates:** flag-off → default ctest 247/247 unchanged + RenderHealth green (extra
0 ms pass tolerated). Flag-on → a new `FarLodHorizon`-style scenario captures with
`--enable-far-field-gpu-raymarch` and asserts the raymarch G-buffer matches the
mesh path within the inc1 quantization tolerance (the MAJOR #9 mesh-vs-raymarch
parity leg) + a visual sweep for the seam. Temporal-stability gate is inc3.

This is a single coherent integration; all its components are proven (inc1/2a/2b)
and the flag-gating makes every intermediate commit byte-stable.

## inc2c-SCALE — make the far-field scalable under load (owner principle 2026-06-16)
Owner standing directive: build the most powerful, COMPOSABLE, SCALABLE-under-load
engine ([[engine-power-scalability-principle]]). The v1 pass RUNS but is NOT yet
scalable, so it stays dormant until this lands. Perf math: A3a measured the
heightfield march at ~0.07 ms for 320x180 (57,600 rays, ~2-4 steps/ray). Fullscreen
at the 3840x1600 target = 6.14 M rays ≈ **107x → ~7.5 ms** — far over the 300 fps /
~3.3 ms frame budget. The cost is RAY COUNT (fullscreen), not steps (the max-mip is
already efficient). Scale it down, in priority order:

1. **Far-pixel-only dispatch (biggest, cleanest win).** Only march pixels the mesh
   didn't cover (sky/far). Copy the G-buffer depth to a sampler texture BEFORE the
   pass (a blit — avoids the read-while-write feedback on the depth attachment the
   pass writes via gl_FragDepth), bind it, and `discard` immediately in the frag when
   sampled depth < ~1.0 (mesh closer). Typical views are ~2/3 near-terrain → skips
   most rays. Correctness-safe: only skips pixels the GL_LESS depth test would reject
   anyway. Est. fullscreen → ~2-3 ms.
2. **Half-resolution raymarch + depth-aware upsample.** March a half-res target
   (4x fewer rays → another ~4x), then NEAREST-DEPTH upsample into the G-buffer
   (naive bilinear bleeds across silhouettes — use the classic nearest-of-4 by depth
   delta). Est. → sub-ms. This is the spec's substrate (half-res/temporal/upsample).
3. **Async region-crossing rebuild.** The 49-tile BuildPristineFarLodTile rebuild is
   synchronous on the GL thread today → a stream hitch. Dispatch on JobSystem (mirror
   FarLodSystem's async build + main-thread integrate), render the prior field until
   ready. Removes the under-load hitch.
4. **Composability:** the half-res raymarch + upsample is the shared substrate Wave B
   clouds/aurora/ocean consume — freeze its API (output-target + field-sampler params)
   so those pass a froxel/volume sampler instead of the heightfield. (Substrate API
   freeze + Wave-B-consumer dry-run = MAJOR #10.)

**Gate:** add the FarFieldRaymarch GPU-timer budget to a perf gate (must be within
the far-field slice of the frame budget at 3840x1600 on the 5070 Ti) BEFORE flipping
the compile flag on. Then the parity/seam validation, then enable-by-default.

## VISUAL-FIDELITY constraint on the scalability work (owner principle 2026-06-16)
Don't sacrifice beauty for perf — hold a **Battlefield 4/BF1 (Frostbite) realistic
fidelity floor** ([[visual-fidelity-target]]). Every step above must pass the visual
bar, not just the perf bar:
- **Far-pixel-only dispatch:** quality-neutral (skips redundant pixels) — preferred.
- **Half-res + upsample:** MUST stay crisp — depth-aware nearest upsample (no
  silhouette bleed/aliasing) + temporal accumulation if needed; a blurry low-res far
  field FAILS the floor. If half-res can't hold the bar on silhouettes, keep edges
  full-res (edge-detect) or drop to 3/4-res rather than blur.
- **Shading parity (fidelity gap to close):** the far-field currently writes FLAT
  material ids + flat albedo. For the BF4/BF1 floor it must be TEXTURED + lit like the
  near terrain (it already shares the deferred lighting) — sample the same triplanar
  terrain material/albedo path (or a distance-faded approximation) so the far field
  reads as continuous realistic terrain, not flat slabs. Seamless near<->far LOD/fade
  (no popping/banding/void seam).
- **Guard:** the WorldVisualSweep / visual-critique objective gates (washed-out, flat,
  aliased, sparse, seam flags) are the floor's automated enforcement — the far-field,
  once enabled, must keep them GREEN at 3840x1600. Perf-gate AND visual-gate together
  before enable-by-default.

## New files
- `src/luminumbra_client/rendering/passes/ShieldRtFarFieldPass.{h,cpp}` — owns the
  clipmap texture + maxmip, update_clipmap(camera), dispatch mip reduction.
- `res/shaders/far_field_mip_reduction.compute` — max-reduce.
- (inc2c) `res/shaders/far_field_raymarch.*` — the live march writing the G-buffer
  (port of the validated `kHeightfieldMarchDepthCompute` / the gbuffer-frag logic).

## Modified files (surgical)
- `RenderPipeline.h/.cpp`: `GpuTimerPass::FarFieldRaymarching` enum + name (keep the
  `static_assert` count in sync — this changes the RenderHealth pass-name list, a
  small re-bless), `m_far_field_raymarch_pass` member + construct + execute (after
  GBuffer, before Lighting), gated by a new `constexpr bool
  kEnableExperimentalFarFieldGpuRaymarching = false;` (mirror the GPUSDF gating shape).
- `RuntimeScenarioHarness.cpp`: `--enable-far-field-gpu-raymarch` CLI flag.
- `FarLodSystem.{h,cpp}`: the generation counter.
- `CMakeLists.txt`: add ShieldRtFarFieldPass.cpp to the client sources.

## Sequencing
inc2b = data + acceleration structure resident on GPU, proven (a small ctest that
uploads a known region set, runs the mip reduction, and checks a few max-mip texels
vs the CPU `BuildHeightMaxMip`). inc2c = the live raymarch pass into the G-buffer +
near↔far dither blend + the `FarLodHorizon` mesh-vs-raymarch parity leg. inc3 =
temporal-stability gate (needs a minimal render-interpolated prev-view history).

Source: background scoping agent (read-only), 2026-06-16. Adding the `GpuTimerPass`
entry will require re-blessing the RenderHealth pass-name contract in render_smoke_test.
