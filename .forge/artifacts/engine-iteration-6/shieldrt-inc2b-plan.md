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

## Dirty tracking
FarLodSystem has no explicit dirty flag today; residency is implicit in the wanted-set
diff (`m_params_hash`/`m_epoch`, FarLodSystem.h:196/199). Add:
- `u64 m_heightfield_generation = 0;` + accessor `heightfield_generation()`;
- bump it in `integrate_completed_builds()` (FarLodSystem.cpp ~309) after the GPU
  upload loop. The pass re-uploads touched quadrants + rebuilds mips when it changes.

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
