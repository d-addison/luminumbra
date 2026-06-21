# 600 fps Research Brief (companion to spec 004 PRD)

> 6-agent ultracode research workflow (2026-06-21): 5 facets (GPU-driven submission, overdraw/foliage/
> impostors, culling/visibility, beauty-at-600fps, engine-grounding) → synthesis. Findings are grounded
> in the actual render code + cited AAA sources. This brief backs the PRD's roadmap.

## The measured truth (from spec 003 C2) that frames everything
The frame is **CPU/present-bound**, not GPU-bound: ~40–110k static-prop entities are walked single-
threaded every frame (`GBufferPass::geometry_pass_static_meshes`, per-entity `glm::length` +
`SelectTreeLod` + FNV grouping + `unordered_map`/`vector<mat4>` churn + per-group `glBufferSubData`
implicit syncs + N `glDrawElementsInstanced`). The GPU idles → downclocks (26–41 W / 200–1900 MHz vs
300 W / 3090 MHz), and a 58 % triangle cut moved `gbuffer_ms` by zero. So per-pass GPU timers are
measured at idle clock and are misleading. **The headline lever is moving prop submit onto the GPU**
(unblocks the clock), then overdraw + temporal upsampling buy GPU-ms.

## Facet 1 — GPU-driven submission (DO-NOW, the headline lever)
- Persistent-mapped triple-buffered instance pool (`glBufferStorage` MAP_WRITE|PERSISTENT|COHERENT)
  written ONCE at scatter, replacing per-group `glBufferSubData`.
- `glMultiDrawElementsIndirect` (MDI): one call per material across LOD batches, baseInstance-indexed.
- Compute cull writing indirect args (AC Unity / AnvilNext model): per-instance frustum + distance→LOD
  in a compute shader, atomic-append survivors, CPU issues ~0 per-entity work.
- **Engine already ships every primitive:** `RenderPipeline::draw_chunks_mdi` (`RenderPipeline.cpp:1252`,
  persistent cmd buffer, baseInstance — sidesteps the `gl_DrawID`/ARB_shader_draw_parameters risk);
  persistent pools (`RenderPipeline.cpp:241`, `FoliagePass.cpp:106`); compute→count_ssbo→indirect
  (`FoliagePass.cpp:614-657`). **Keep args GPU-resident** — the FoliagePass readback (`:640-651`) is the
  sync-stall anti-pattern to avoid.
- Sources: AZDO (Everitt/Sellers/McDonald/Foley, GDC 2014); GPU-Driven Rendering Pipelines
  (Haar/Aaltonen, SIGGRAPH 2015); AnvilNext 10× objects / 1–2 orders fewer drawcalls; Persistent Mapped
  Buffers (Filipek); vkguide GPU-driven; NV_command_list / NV_bindless_multi_draw_indirect; Aokana
  GPU-driven voxel framework (arXiv 2505.02017, 2025).

## Facet 2 — Overdraw collapse (DO-NOW after GPU-driving)
- Depth/Z pre-pass for alpha-test trees/bushes + `GL_EQUAL` main pass → 3–8× canopy overdraw to ~1×
  (the lever that finally moves `gbuffer_ms`). MUST come after GPU-driving or it doubles CPU draws.
- Grass `GL_BLEND`→alpha-TEST (restore early-Z) + A2C / dither + mip-coverage alpha lift for density.
- Octahedral impostor atlases replace the overdraw-heavy LOD3 cross-billboards (also a fidelity UP).
- Sources: Ghost of Tsushima procedural grass (GDC 2021); Octahedral Impostors (Brucks/shaderbits);
  Alpha-to-Coverage (Golus); z-prepass YMMV (Interplay of Light); Early-Z (MJP); UE5 Nanite Foliage.

## Facet 3 — Culling / visibility (DO-NOW cull; DEFER Hi-Z/visibility-buffer)
- GPU cull + compaction + per-instance LOD via `glMultiDrawElementsIndirectCount` deletes the CPU loop.
- Two-phase Hi-Z occlusion (reuse the G-buffer depth as HZB source): 20–60 % fewer drawn in occluded
  hilly terrain — DEFER until cull/far-canopy is the proven new ceiling.
- Visibility-buffer / clustered shading: bigger rewrites, deferred (note for the Vulkan era).
- Sources: Interplay of Light GPU occlusion; Rastergrid Hi-Z; Turitzin hierarchical depth; two-pass
  occlusion (mil_kru); SSBO indirect drawing (lingtorp); Nanite macro-view (elopezr); Olsson clustered.

## Facet 4 — Beauty at 600 fps (TAA→TAAU; stay-GL)
- TAA substrate first (Halton[2,3] jitter + motion-vector G-buffer attachment covering shader-side
  WIND + history + neighborhood clamp) → free edge AA + DLSS-ready inputs.
- TAAU (hand-rolled or FSR2 GL path), render-scale knob, default **Quality 1.5×** (~44 % px → ~1.6–1.9×
  per-pixel headroom for ~0.6–1.0 ms reconstruct). Never default Performance 2.0×.
- DLSS/DLSS-RR/Frame-Gen are **Vulkan/DX-only** (no GL NGX) → not on the 600 fps path; the TAA inputs
  are exactly what a future Vulkan+DLSS port needs.
- Reflex-style just-in-time submit (hand-rolled on GL) + vsync-off pacing keep the boost clock.
- Sources: FSR2 manual (cost table, render ratios, required inputs) + GDC slides; DLSS NGX (GL
  unsupported); NVIDIA Reflex; Frostbite stochastic SSR; TAA (elopezr, Wronski); UE5 TSR; potato3d/azdo.

## Facet 5 — Engine-grounding (the concrete map)
- Rewrite target: `GBufferPass::geometry_pass_static_meshes` (`GBufferPass.cpp:330-533`), buffers
  (`:51-69`, `kStaticInstanceCapacity=16384` **silently clamps** below the ~110k set → real bug).
- Scatter (upload-once source, render-only, never hashed): `main_client.cpp:~483-700, ~4532-4776`
  (preserve seeded `frand()` order for determinism).
- Shadows draw only chunks today (`ShadowPass.cpp:74-97`) → feed the prop indirect buffer for Phase 3.
- Benchmark sums GPU timers only (`main_client.cpp:~7242`) → blind to CPU-submit wins (Phase 0 fix).
- Sources: nvpro gl_occlusion_culling; vkguide; logdahl GPU-driven; Imagination GPU-controlled
  rendering; NVIDIA MDI sample.

## Cross-cutting decisions
- **Stay OpenGL 4.5**, Vulkan-aware, defer the backend (every 600 fps lever is GL-native + half-built).
- **Render-only** throughout → no `world_hash` re-pin; only constraint is preserving scatter call order.
- **Measurement first** (Phase 0): CPU-submit/present split + NVML power/clock + forest-dense pose +
  clock-lock A/B — without it the GPU-timer-sum benchmark shows flat and the headline win looks like a no-op.
