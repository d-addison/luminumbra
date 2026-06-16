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

## Next validatable increment (texture-backed path)
Before live wiring, prove the TEXTURE-backed render path (the live pass uses
textures, the benches used SSBOs): R32F clipmap texture + GPU max-mip as a texture
(adapt the validated reduction to imageLoad/imageStore) + the DDA raymarch sampling
the textures, validated offscreen vs analytic ground truth (mirrors inc2a). Then
inc2c wires `ShieldRtFarFieldPass` into the live G-buffer slot gated by the flag,
adds `GpuTimerPass::FarFieldRaymarching` (re-bless render_smoke_test pass list), and
the `FarLodHorizon` mesh-vs-raymarch parity leg.

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
