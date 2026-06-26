# Spec 014: Multi-Backend RHI — Vulkan + DX12 Migration with DLSS & Hardware Ray Tracing

> Status: IN PROGRESS (created 2026-06-26). Grounded in a deep-research brief (adversarially
> verified; run wf_whmuyyyvg) on cross-API render-hardware-interface libraries, DLSS integration,
> single-source shader pipelines, and voxel-mesh ray-tracing acceleration structures. Companion to
> the just-landed FLIP/debug-view/frame-health PARITY HARNESS, which is the load-bearing safety net
> for this entire migration.
>
> **Library choice DECIDED — ADOPT Diligent Engine (Apache-2.0) as the RHI (research finding [0],
> high confidence):** Diligent is the ONLY surveyed library that ships Vulkan + DX12 + **OpenGL**
> backends AND built-in cross-backend ray tracing (BLAS/TLAS/RT-pipeline) under a permissive license.
> The OpenGL backend is the **linchpin** of the whole plan: it lets the current GL renderer and the
> new Vulkan/DX12 backends coexist behind ONE RHI, so we can do **in-process FLIP per-pass parity
> validation** (render the same pass on GL and on the new backend in the same process, diff the
> framebuffers) while continuing to **ship on GL** until each pass is proven. NRI (MIT, built-in DLSS)
> is the runner-up but has **NO GL backend** — it cannot back the shipping path or the in-process FLIP
> harness, so it is rejected as the primary RHI (see OQ-3 for a possible later DLSS-only use).
>
> **DLSS DECIDED — NVIDIA Streamline (research finding [1]):** Diligent does not ship NGX/DLSS.
> Integrate DLSS via NVIDIA **Streamline** (the abstraction layer that owns NGX), which slots into the
> RHI command stream and consumes the motion vectors + depth + jitter we already produce for TAAU.
> DLSS **replaces** the existing TAAU pass on capable hardware; TAAU stays as the universal fallback.
>
> **Shaders DECIDED — single-source HLSL (research finding [2]):** port the existing GLSL passes to a
> single HLSL source set compiled per-backend: **DXC -> DXIL** for DX12, **DXC -> SPIR-V** for Vulkan,
> and **SPIRV-Cross** for reflection (and, where Diligent's GL backend needs it, GLSL emission). One
> shader source, three consumers — no per-backend shader forks.
>
> **Ray tracing DECIDED (research finding [3]):** use Diligent's BLAS/TLAS/RT-pipeline API. Build the
> **BLAS from the near-field marching-cubes mesh** (the geometry we already generate per chunk), with a
> rebuild cadence tied to streaming/voxel edits; keep the **SDF far-field analytic** (no BLAS out
> there — it ray-marches already). RT-GI/AO from the MC-mesh BLAS ALSO closes the known **cave-lighting
> gap** (coarse LOD currently ignores the SDF; see memory "Coarse LOD ignores SDF") because RT lighting
> samples the actual near-field geometry, not the heightmap.
>
> **This is render PARITY, not determinism.** The sim is render-AGNOSTIC: the backend NEVER feeds into
> `world_hash`. So the gate for every step is **FLIP parity of the rendered image** vs the GL baseline,
> not a hash. Any sim/worldgen touch (there should be none) must keep the legacy `default` preset
> byte-identical: `--smoke == 6f008a9f637c40b7`.

## Context

Luminumbra renders with a hand-rolled **OpenGL 4.5 deferred pipeline**: ~12 passes — shadow, gbuffer,
ssao, lighting, water, skybox, particle, foliage, plant-procgen, aerial, TAAU, blit — plus
GroundDecalPass and DebugViewPass. The world is an **SDF / marching-cubes voxel** world (near-field MC
mesh < 256m, SDF ray-march far-field), streamed in chunks. The sim is deterministic and
**render-agnostic**: nothing the renderer does can change `world_hash`.

The target hardware (RTX 5070 Ti, per memory "Render API & target") wants the NVIDIA RTX stack — **DLSS**
and **hardware ray tracing** — which the GL backend cannot reach. The owner's standing decision was to
stay GL + Vulkan-aware + Nsight now and **defer a Vulkan backend** until it could be done safely. The
two things that make it safe to do *now* are: (1) a library with a GL backend so GL and Vulkan/DX12 can
live behind one seam during the port, and (2) the **parity harness** just landed (FLIP image diff,
debug-view, frame-health). Together they let us migrate **one pass at a time, each FLIP-gated, while
the game keeps shipping on GL**.

This spec adopts Diligent Engine as the RHI, ports the ~12 passes behind it, brings up Vulkan then
DX12 backends pass-by-pass under FLIP parity, then unlocks the RTX features the GL path can't: **DLSS**
(via Streamline, replacing TAAU) and **hardware RT** (GI/AO + reflections from a BLAS built off the MC
near-field mesh). It is sequenced strictly by **risk**: cheapest, most-reversible step first.

## Goals

- **Decouple the renderer from OpenGL** behind a Diligent-backed RHI seam (device, swapchain, buffer,
  texture, pipeline, command list), with the ~12 deferred passes expressed against the seam, not raw GL.
- **Keep shipping on GL the entire time.** The GL-via-Diligent backend is a no-change baseline; Vulkan
  and DX12 are brought up behind it and only become the ship backend per-pass once FLIP-parity holds.
- **Stand up Vulkan, then DX12,** validated **pass-by-pass** against the GL baseline via the in-process
  FLIP harness — every ported pass is parity-gated before it counts as done.
- **Single-source shaders:** one HLSL set, compiled to DXIL (DX12) and SPIR-V (Vulkan), reflected via
  SPIRV-Cross — no per-backend shader forks.
- **DLSS via Streamline,** replacing TAAU on capable GPUs (reusing our motion vectors + depth + jitter),
  with TAAU as the fallback.
- **Hardware ray tracing:** RT-GI/AO and RT reflections from a BLAS built off the marching-cubes
  near-field mesh — and, as a bonus, **close the cave-lighting gap** that coarse-LOD heightmap lighting
  leaves open.
- **Render PARITY as the contract:** the backend never affects `world_hash`; correctness is FLIP image
  parity vs the GL baseline, pass by pass.

## Non-Goals

- **No sim/worldgen change.** The backend is render-only; `world_hash` is untouched. The legacy
  `default` preset stays byte-identical (`--smoke == 6f008a9f637c40b7`). This is not a determinism spec.
- **No big-bang rewrite.** We do not port all passes at once, and we do not flip the ship backend until
  each pass is parity-proven. GL stays the shipping path through every intermediate phase.
- **No bespoke RHI.** We adopt Diligent rather than hand-rolling a Vulkan/DX12 abstraction.
- **No far-field BLAS.** The SDF far-field already ray-marches; only the near-field MC mesh gets a BLAS.
- **Not a perf spec** in the optimization sense — but DLSS and RT are expected to *win* perf/quality on
  RTX; each new backend/pass must hold the existing render budget (`--render-benchmark`).
- **No AMD/Intel RT-vendor-specific tuning** this spec (RT is cross-backend via Diligent; DLSS is
  NVIDIA-only by design with TAAU fallback). FSR/XeSS are out of scope (OQ-4).

## Functional Requirements

### Group A — RHI seam (Diligent abstraction over the existing passes)

- **FR-A.1 (vendor + device).** Vendor Diligent Engine (Apache-2.0) via FetchContent (per the
  worktree/vendor hazard in memory — FetchContent, not a junction). Introduce an `IRenderDevice` /
  swapchain wrapper that creates a Diligent render device + swapchain and exposes our window surface.
- **FR-A.2 (resource seam).** Express the renderer's buffers, textures, samplers, and render targets as
  Diligent resources behind a thin RHI type set (`RhiBuffer`, `RhiTexture`, `RhiPipeline`, `RhiCmd`),
  so passes allocate/bind through the seam, not through GL calls.
- **FR-A.3 (pipeline + command seam).** Express pipeline state (shaders, blend/depth/raster, vertex
  layout) and command recording (begin pass, bind, draw/dispatch, barriers) through Diligent's PSO +
  command-list model. The ~12 passes record into RHI command lists.
- **FR-A.4 (G-Buffer + deferred targets).** The deferred G-Buffer (and all intermediate render targets:
  shadow atlas, ssao, lighting accum, water, taau history) become RHI textures with explicit layouts/
  barriers — Diligent's explicit-state model makes Vulkan/DX12 layout transitions correct by construction.
- **FR-A.5 (seam covers all 13 passes).** The seam must cover, by name: shadow, gbuffer, ssao, lighting,
  water, skybox, particle, foliage, plant-procgen, aerial, TAAU, blit, GroundDecalPass, DebugViewPass.
  No pass keeps a raw-GL escape hatch once ported (a pass is either GL-via-Diligent or backend-native).

### Group B — GL backend via Diligent (no-change baseline, keep shipping)

- **FR-B.1 (GL-via-Diligent baseline).** Bring up Diligent's **OpenGL** backend first and route the
  full pipeline through it. This is the **no-change baseline**: rendered output must FLIP-match the
  current raw-GL output within the parity harness's threshold.
- **FR-B.2 (shipping path unchanged).** GL-via-Diligent remains the **default ship backend** until a
  given pass's Vulkan/DX12 path is parity-proven. Backend selection is a runtime/env flag
  (e.g. `LUMIN_RHI=gl|vulkan|dx12`), defaulting to GL.
- **FR-B.3 (FLIP baseline registration).** The GL-via-Diligent output becomes the parity-harness
  REFERENCE for every subsequent backend/pass diff (the harness already produces FLIP + frame-health).

### Group C — Vulkan backend, first pass (DebugViewPass), FLIP-diffed vs GL

- **FR-C.1 (Vulkan device path).** Bring up Diligent's **Vulkan** backend behind the same RHI seam
  (device/swapchain) on the RTX target; no pass ported yet.
- **FR-C.2 (DebugViewPass on Vulkan first).** Port **DebugViewPass** (a single fullscreen pass, lowest
  risk, no G-Buffer dependency) to run on Vulkan. This is the **cheapest first real step** — one
  fullscreen triangle, one shader, one output.
- **FR-C.3 (in-process FLIP diff).** Render DebugViewPass on GL and on Vulkan **in the same process**
  (the GL backend is what makes this possible) and FLIP-diff the two framebuffers; the pass is DONE only
  when FLIP parity holds within threshold.
- **FR-C.4 (per-pass backend routing).** The pipeline can run a single pass on a different backend than
  the rest (or render a pass twice for the diff), so the port can proceed one pass at a time without a
  full-pipeline backend switch.

### Group D — Pass-by-pass port (each FLIP-parity-gated)

- **FR-D.1 (port order by risk).** Port the remaining passes Vulkan-native in increasing-coupling order:
  fullscreen/post first (blit, ssao, aerial, skybox), then lighting-deferred (gbuffer, lighting),
  shadow, then the dynamic/scatter passes (water, particle, foliage, plant-procgen), then GroundDecalPass.
  TAAU is ported last in this group (it is replaced by DLSS in Group G, so its Vulkan-native port is the
  fallback path).
- **FR-D.2 (FLIP gate per pass).** **Every** ported pass is gated by an in-process FLIP diff vs the
  GL-via-Diligent baseline (FR-B.3) before it is allowed to become the ship path for that pass. A pass
  that fails parity stays on GL.
- **FR-D.3 (frame-health gate).** Beyond FLIP, each ported pass must pass the frame-health check (no NaN/
  inf, no black/over-bright frame, expected coverage) from the parity harness.
- **FR-D.4 (full-Vulkan milestone).** When all passes are Vulkan-parity, Vulkan can become the default
  ship backend on the RTX target (GL remains the cross-vendor fallback).

### Group E — DX12 backend

- **FR-E.1 (DX12 device path).** Bring up Diligent's **DX12** backend behind the same seam. Because the
  shaders are already single-source HLSL (Group F) and the passes are already RHI-expressed (Group A),
  DX12 is mostly device-bring-up + per-pass FLIP validation, not new pass work.
- **FR-E.2 (DX12 pass-by-pass FLIP).** Validate each pass on DX12 with the same in-process FLIP gate
  (FR-D.2). DX12 reuses the DXIL compile of the single-source shaders.
- **FR-E.3 (DX12 optional ship).** DX12 is available as a selectable backend (`LUMIN_RHI=dx12`); whether
  it becomes a default on any SKU is an owner call (OQ-2). Its primary value is RT/Windows-native paths.

### Group F — Shaders: GLSL -> HLSL single-source

- **FR-F.1 (single-source HLSL).** Port every pass's GLSL to a single **HLSL** source. No per-backend
  shader forks — one source feeds all three backends.
- **FR-F.2 (DXC compile).** Compile HLSL with **DXC** to **DXIL** (DX12) and to **SPIR-V** (Vulkan).
- **FR-F.3 (SPIRV-Cross reflection).** Use **SPIRV-Cross** for reflection (resource binding/layout) and,
  where Diligent's GL backend needs GLSL, for GLSL emission — so the GL baseline runs the same authored
  shader logic, keeping the FLIP baseline honest.
- **FR-F.4 (offline shader build).** Shaders compile **offline** in the asset/build pipeline (DXIL +
  SPIR-V artifacts), with hot-reload preserved in dev (recompile-on-change) per the existing shader
  hot-reload workflow.
- **FR-F.5 (parity-preserving port).** Each shader port is validated by the FLIP gate (the ported HLSL
  pass on GL-via-Diligent must FLIP-match the original GLSL pass) BEFORE that shader feeds Vulkan/DX12 —
  isolating "shader port" defects from "backend" defects.

### Group G — DLSS via Streamline (replaces TAAU)

- **FR-G.1 (Streamline integration).** Integrate NVIDIA **Streamline** into the RHI command stream
  (Diligent does not ship NGX). Streamline owns the DLSS/NGX evaluation.
- **FR-G.2 (reuse TAAU inputs).** Feed DLSS the **motion vectors + depth + jitter** the engine already
  produces for TAAU — no new G-Buffer attachments required (motion + depth already exist).
- **FR-G.3 (DLSS replaces TAAU).** On DLSS-capable GPUs, **DLSS replaces the TAAU pass**; on incapable
  GPUs (or `LUMIN_DLSS=0`), TAAU (FR-D.1) is the fallback. Selection is runtime-detected + flag-overridable.
- **FR-G.4 (quality-mode plumbing).** Plumb DLSS quality presets (Quality/Balanced/Performance/Ultra-Perf)
  through the existing settings system (SystemConfig `user.*`, per memory "Settings & controls system").

### Group H — RT-GI / AO (BLAS from the MC mesh)

- **FR-H.1 (BLAS from near-field MC mesh).** Build a Diligent **BLAS** from the marching-cubes near-field
  mesh (the chunk geometry we already generate), with a **rebuild cadence** tied to chunk streaming /
  voxel edits (rebuild dirty chunks' BLAS, refit where possible, full rebuild on topology change).
- **FR-H.2 (TLAS over streamed chunks).** Maintain a **TLAS** over the active near-field chunk BLASes,
  updated as chunks stream in/out — bounded to the near-field (far-field stays SDF-analytic, FR-H.4).
- **FR-H.3 (RT-GI/AO pass).** Add an RT-GI/AO pass (Diligent RT-pipeline or inline RT) sampling the TLAS,
  composited into the deferred lighting. Default-OFF behind a flag (`LUMIN_RT=1`); GL path unaffected.
- **FR-H.4 (far-field stays analytic).** The SDF far-field is **not** added to the BLAS/TLAS; it keeps
  ray-marching. RT covers only the near-field MC mesh.
- **FR-H.5 (closes the cave-lighting gap).** Because RT-GI/AO samples the actual near-field MC geometry
  (caves, runtime edits) rather than the heightmap, it **fixes the coarse-LOD-ignores-SDF cave-lighting
  gap** (memory "Coarse LOD ignores SDF") for the RT lighting term — a substantive fidelity win, not just
  parity. (This is the one place RT output legitimately *differs* from GL: validate by frame-health +
  visual review, not strict FLIP — see NFR-4.)

### Group I — RT reflections

- **FR-I.1 (RT reflections).** Add RT reflections (TLAS-traced) for water/wet surfaces and reflective
  materials, replacing/augmenting the current screen-space approach on RT-capable backends.
- **FR-I.2 (default-OFF + fallback).** RT reflections are flag-gated (`LUMIN_RT_REFLECTIONS=1`) with the
  existing reflection path as fallback; GL path unchanged.
- **FR-I.3 (water-pass integration).** Integrate with the water pass so grottos/water surfaces reflect the
  near-field TLAS (pairs with spec 013's water-grotto POIs).

## Non-Functional Requirements

- **NFR-1 (render PARITY, not determinism).** The backend never affects `world_hash`. The contract is
  **FLIP image parity** vs the GL baseline, per pass. No sim/worldgen change ships in this spec; if any
  shared sim/worldgen file is touched incidentally, the legacy `default` preset stays byte-identical
  (`--smoke == 6f008a9f637c40b7`).
- **NFR-2 (keep-shipping-on-GL throughout).** GL-via-Diligent remains a valid, default-capable ship
  backend at **every** phase boundary. No phase leaves the game unshippable; backend selection is a flag.
- **NFR-3 (FLIP-parity gate per pass).** No pass becomes the ship path on a new backend until it passes
  the in-process FLIP diff (+ frame-health) vs the GL baseline. This is the universal done-gate (FR-D.2).
- **NFR-4 (intentional-divergence carve-out).** Exactly two features are *expected* to diverge from GL
  and so are validated by **frame-health + visual review** rather than strict FLIP: RT-GI/AO (FR-H.5,
  it fixes the cave gap) and RT reflections (Group I). Everything else is strict FLIP.
- **NFR-5 (small-team scope).** Sequenced for one developer + parent build/GPU verification: one library
  (Diligent), one shader source (HLSL), one parity gate (FLIP), risk-ordered phases, each independently
  shippable. No parallel multi-backend big-bang.
- **NFR-6 (perf budget held).** Each new backend/pass holds the existing render budget
  (`--render-benchmark`); DLSS/RT are expected net wins on RTX but must not regress the GL budget on the
  GL path (they are off there).
- **NFR-7 (visual-fidelity floor).** Per memory "Visual fidelity target", parity must hold the BF4/BF1
  fidelity floor; RT/DLSS may exceed it but never drop below on any backend.

## Acceptance Criteria

- [ ] **AC-A (RHI seam).** All 13 passes (shadow, gbuffer, ssao, lighting, water, skybox, particle,
      foliage, plant-procgen, aerial, TAAU, blit, GroundDecalPass, DebugViewPass) record through the
      Diligent RHI seam; no pass retains a raw-GL escape hatch. Client+server build clean.
- [ ] **AC-B (GL-via-Diligent baseline).** The full pipeline runs on Diligent's GL backend and
      **FLIP-matches** the prior raw-GL output within the harness threshold; this output is registered as
      the parity reference. `--smoke == 6f008a9f637c40b7` (sim untouched).
- [ ] **AC-C (DebugViewPass on Vulkan).** DebugViewPass renders on Vulkan and **FLIP-matches** the GL
      baseline in an in-process diff; backend is selectable via `LUMIN_RHI`.
- [ ] **AC-D (all passes Vulkan-parity).** Every pass passes the in-process FLIP gate + frame-health on
      Vulkan; Vulkan is selectable as the default ship backend on the RTX target; GL remains the fallback.
- [ ] **AC-E (DX12 backend).** Each pass passes the FLIP gate on DX12 (reusing the DXIL shader compile);
      `LUMIN_RHI=dx12` runs the full pipeline.
- [ ] **AC-F (single-source shaders).** Every pass runs from a single HLSL source compiled to DXIL +
      SPIR-V (DXC) with SPIRV-Cross reflection; no per-backend shader fork exists; hot-reload still works.
- [ ] **AC-G (DLSS).** On the RTX target, DLSS (via Streamline) replaces TAAU and renders within
      frame-health + budget; TAAU fallback engages when `LUMIN_DLSS=0` or on incapable HW; quality presets
      apply via settings.
- [ ] **AC-H (RT-GI/AO + cave gap).** RT-GI/AO from the MC-mesh BLAS composites into deferred lighting
      (`LUMIN_RT=1`), the near-field TLAS rebuilds correctly under streaming, the far-field stays SDF, and
      a `caverns`-style cave shows correct RT lighting where coarse-LOD heightmap lighting previously
      failed (frame-health + visual review). GL path byte-unaffected with RT off.
- [ ] **AC-I (RT reflections).** RT reflections render for water/reflective surfaces (`LUMIN_RT_REFLECTIONS=1`)
      with the screen-space path as fallback; GL path unaffected.
- [ ] **AC-J (no regressions).** `common_tests` green; client+server build clean; `--render-benchmark`
      within budget on the GL path; existing visual baselines unaffected on the GL path.

## Suggested phasing (risk order — cheapest, most-reversible first)

1. **Phase A — RHI seam + GL-via-Diligent baseline** (Groups A, B). Vendor Diligent, express the 13
   passes behind the seam, run on Diligent's GL backend, FLIP-match raw-GL, register the baseline. Most
   reversible: still 100% GL, just abstracted.
2. **Phase B — Vulkan device + DebugViewPass, FLIP-diffed** (Group C). The **cheapest first real step**:
   one fullscreen pass on Vulkan, diffed in-process against GL. Proves the whole pattern end-to-end.
3. **Phase C — Single-source HLSL shaders** (Group F). Port shaders to HLSL, FLIP-validated on
   GL-via-Diligent first (isolates shader-port defects from backend defects), unblocking native Vulkan/DX12.
4. **Phase D — Pass-by-pass Vulkan port** (Group D). Port remaining passes Vulkan-native in risk order,
   each FLIP+frame-health gated, until Vulkan is full-pipeline parity.
5. **Phase E — DX12 backend** (Group E). Device bring-up + per-pass FLIP, reusing the HLSL/DXIL compile.
6. **Phase F — DLSS via Streamline** (Group G). Replace TAAU on RTX; TAAU fallback retained.
7. **Phase G — RT-GI/AO** (Group H). BLAS from MC mesh + TLAS over streamed chunks; closes the cave gap.
8. **Phase H — RT reflections** (Group I). TLAS-traced reflections for water/reflective surfaces.

## Open Questions

- **OQ-1.** FLIP threshold for "parity": per-pass tuned, or one global threshold? Some passes
  (stochastic SSAO, dithered particles) are inherently noisy — lean per-pass thresholds with a
  documented default, frame-health as the backstop.
- **OQ-2.** Does DX12 ever become a *default* ship backend on any SKU, or stay a selectable/RT-only
  path? Lean: Vulkan default on RTX, DX12 selectable; revisit if a DX12-only RT/feature win appears.
- **OQ-3.** Could NRI (MIT, built-in DLSS) ever serve as a DLSS-only side path while Diligent owns the
  RHI? Probably not worth the second dependency given Streamline already covers DLSS cleanly — but note
  it as a fallback if Streamline+Diligent interop friction appears.
- **OQ-4.** Cross-vendor upscaling (FSR3 / XeSS) alongside DLSS for AMD/Intel? Out of scope here; TAAU
  is the universal fallback. Revisit once DLSS lands and the upscaler seam exists.
- **OQ-5.** BLAS rebuild cadence under heavy streaming: refit-vs-rebuild heuristic, and a per-frame BLAS
  build budget so RT doesn't stall streaming. Needs profiling once Group H lands.
- **OQ-6.** Does Diligent's GL backend reproduce our exact GL state closely enough for sub-threshold FLIP
  on every pass, or will a few passes need tolerance bumps / frame-health-only validation? Discover during
  Phase A (this is the key Phase-A risk).
- **OQ-7.** Offline shader artifacts: check DXIL+SPIR-V into the asset pipeline, or build them as a
  CMake step? Lean build-step with cache, to avoid binary churn in git.

## References (grounding — current code + research)

- Render pipeline + passes: `src/luminumbra_client/rendering/RenderPipeline.{h,cpp}` (the ~12 deferred
  passes + G-Buffer), `GroundDecalPass`, `DebugViewPass`.
- Parity harness (load-bearing): the just-landed FLIP image-diff + debug-view + frame-health harness
  (`build/debug/test-artifacts/render/render-health-analysis.json`, `gpu-sdf-runtime-parity.json`,
  `shader-inventory.json`).
- Voxel world: `src/luminumbra_common/systems/SHIELD_WorldSystem.*` (SDF + marching-cubes; near-field MC
  mesh < 256m, SDF far-field > 256m), `src/luminumbra_common/world/Chunk.*` (MC mesh gen).
- TAAU + motion/depth/jitter: TAAU pass in `RenderPipeline.cpp` (DLSS reuses these inputs).
- Settings plumbing for DLSS quality: SystemConfig `user.*` (memory "Settings & controls system").
- Cave-lighting gap RT closes: memory "Coarse LOD ignores SDF" (coarse LOD re-derives from heightmap, not
  the SDF — caves/runtime edits vanish; RT-GI/AO from the MC-mesh BLAS samples the real geometry).
- Vendoring constraint: FetchContent, not junctions (memory "Stale main / worktree hazard").
- API/target: GL 4.5 renderer, RTX 5070 Ti @ 300fps; owner's stay-GL-now + Vulkan-aware decision now
  acted on (memory "Render API & target").
- Build-tree gotcha: parent builds/tests `build/debug` via `cmake --build --preset debug` (memory
  "Build-tree gotcha"); Bash tool can't run GPU client — parent verifies on PowerShell (memory
  "Bash tool sandboxed").
- Research brief: deep-research run **wf_whmuyyyvg** (Diligent vs NRI vs bgfx/sokol survey; Streamline/NGX
  for DLSS; DXC/SPIRV-Cross single-source HLSL; Diligent BLAS/TLAS/RT-pipeline; voxel-mesh AS rebuild
  cadence).
