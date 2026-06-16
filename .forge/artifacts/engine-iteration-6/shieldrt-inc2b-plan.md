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
