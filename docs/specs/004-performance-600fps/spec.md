# Spec 004 — Path to 600 fps (Performance PRD, first draft)

> **Status: PRD FIRST DRAFT (research-grounded, not yet scoped for execution).** Produced from a
> 6-agent ultracode research workflow (2026-06-21) grounded in the actual render code + AAA
> technique sources. Detailed facet findings + citations: `research-brief.md` (companion).
> This draft is for owner review/iteration before it becomes an executable spec.

## 1. Summary & Vision
luminumbra runs a vast, **Battlefield-1/Frostbite-floor** voxel forest at **600 fps (1.67 ms/frame)**
on the RTX 5070 Ti at 3840×1600 — by **inverting where work happens, not by cutting beauty**. The
measured truth (spec 003 C2) is that the frame is **CPU/present-bound**: ~40–110k static-prop
entities are walked single-threaded every frame (cull + `SelectTreeLod` + FNV grouping + per-group
`glBufferSubData` + `glDrawElementsInstanced`), so the GPU **starves and downclocks** (26–41 W /
200–1900 MHz vs 300 W / 3090 MHz) and a 58 % triangle cut moved `gbuffer_ms` by **zero**. North star:
(1) move static-prop cull/LOD/submit **onto the GPU** so the CPU stops gating and the card clocks
back up; (2) **collapse foliage overdraw**; (3) add a **TAA→TAAU** temporal substrate for per-pixel
headroom and DLSS-ready groundwork. Beauty is held or **raised** throughout. Everything stays
**render-only** (sim is integer/fixed-point 30 Hz, never hashed), so determinism is untouched.

## 2. Measured Baseline & Problem Statement
- Current: ~256 fps (≈3.9 ms) at 3840×1600; RenderBudget total 3.83–3.89 ms (skybox/ssao within budget).
- **CPU/present-bound, not GPU-bound:** GPU idles at 26–41 W / 200–1900 MHz during the benchmark; a
  58 % forest-triangle cut left `gbuffer_ms` unchanged. Per-pass GPU timers measured at 200 MHz are
  meaningless — clock must be reported alongside ms.
- **Foliage cost is overdraw-bound**, separate from triangle count.
- **Latent bug:** `kStaticInstanceCapacity = 16384` (`GBufferPass.cpp:515`) silently clamps below the
  ~110k true prop set (≈28k tree-parts + 14k rocks + 40k bushes) — props may be dropped per group.
- **Measurement is blind today:** `--render-benchmark` sums per-pass GPU timers only; it will show a
  FLAT result for a CPU-submit win and under-reports overdraw. Fixing this is a prerequisite.

## 3. Goals / Non-Goals
**Goals:** 1.67 ms/frame on a forest-DENSE fixed pose with the GPU at boost clock; hold/raise the BF1
fidelity floor; render-only (no sim/world_hash change); honest CPU-vs-GPU attribution.
**Non-Goals (this PRD):** a Vulkan backend (stay GL 4.5, Vulkan-aware); `GL_ARB_bindless_texture`
(stay array-based per design §10); DLSS/hw-RT (Vulkan-gated, future); any sim/determinism change; any
fidelity regression to win the budget.

## 4. Budget Model
600 fps = **1.67 ms wall-clock = max(CPU_submit, GPU_work) + present** — NOT a sum of GPU pass timers.
Today ~3.9 ms is gated by CPU submit (proven by the downclock + the no-op triangle cut). We buy the
budget in two moves: **(a)** CPU submit from a ~3.9 ms-gating 40–110k-entity loop → **<1 ms** (a few
compute dispatches + 1–2 MDI calls); **(b)** once the CPU stops gating, the GPU **upclocks ~1.5–3×**
(the hidden multiplier — the same passes run far faster at boost), then **overdraw collapse** (depth
pre-pass: 3–8× canopy shading → 1×) + **TAAU Quality** (~44 % pixels → ~1.6–1.9× per-pixel headroom
for ~0.6–1.0 ms reconstruct) bring GPU work under 1.67 ms. **Pacing:** vsync ON leaves the GPU idle →
downclock; run vsync-off with a sleep+spin pacer + shallow render-ahead to hold boost clock.

## 5. Architecture — GPU-Driven Static-Prop Submission (the headline lever)
Upload the full static-prop set **once** to an SSBO `{pos, scale, quat, tint, meshId, boundingSphere}`
at world-scatter (preserving the seeded `frand()` call ORDER — load-bearing for determinism). A
compute shader does frustum test + distance→LOD bucket + atomic-append survivors into per-LOD ranges
and writes the indirect-command `instanceCount`s; the CPU issues **one `glMultiDrawElementsIndirect`
per material** with ZERO per-entity work. `SelectTreeLod`/`LodMeshPath` move into the compute shader
(deleting the per-frame `entt` loop, FNV grouping, `resolveMemo`, and per-group `glBufferSubData`
syncs). **Reuse, not greenfield:** the engine already has `RenderPipeline::draw_chunks_mdi`
(`RenderPipeline.cpp:1252`, persistent command buffer, baseInstance-indexed — sidesteps the
`gl_DrawID` ext risk), persistent-mapped pools (chunk/foliage/particle), and the
compute→count_ssbo→indirect shape (`FoliagePass.cpp:614-657`). **Keep indirect args GPU-resident** —
never read back the count on the hot path (the FoliagePass readback at `:640-651` is the anti-pattern).

## 6. Architecture — Overdraw Collapse
(a) **Depth/Z pre-pass** for alpha-tested trees/bushes, then re-draw into the G-buffer with
`glDepthFunc(GL_EQUAL)` + `glDepthMask(FALSE)` so each canopy pixel shades exactly once (MUST come
AFTER GPU-driving, else it doubles CPU draws). (b) Grass `GL_BLEND` → **alpha-TEST** to restore
early-Z, softened with A2C / in-shader dither + **mip-coverage alpha lift** to hold distant density.
(c) Extend the existing `g_buffer.frag` `u_forceFlat`/`u_macroRockOverlay`-off cheap paths. This is
the lever that finally moves `gbuffer_ms`.

## 7. Architecture — Temporal Substrate (TAA → TAAU)
Halton[2,3] projection jitter + a **motion-vector G-buffer attachment** (MUST cover shader-side wind
in `instanced_mesh.vert` — a known MV hazard) + history buffer + neighborhood-clamp resolve (free
edge AA first), then upgrade to temporal **upsampling** (hand-rolled TAAU or FSR2 GL path) with a
**render-scale knob**, default **Quality 1.5×/dim** (never ship Performance 2.0× as default). These
are exactly DLSS4's required inputs → cheap future Vulkan unlock.

## 8. Frame Pacing & Anti-Downclock
vsync-off + sleep+spin pacer at a stable high cap, shallow render-ahead queue, continuous GPU work to
hold boost clock (hand-rolled Reflex-style just-in-time submit; no Reflex on GL). Verify via NVML that
power climbs toward 300 W and clock toward 3090 MHz.

## 9. Phased Roadmap (each: lever → expected gain → touch points; re-measure between)
- **Phase 0 — Honest measurement substrate (PREREQUISITE).** CPU-submit/present split + NVML
  power/clock sampling + a forest-DENSE fixed pose in `--render-benchmark`. 0 fps but un-blinds
  everything. `main_client.cpp:~7242`, `RenderPassFrameStats`.
- **Phase 1 — Persistent instance pool + MDI (do-now, low risk).** Replace per-group
  `glBufferSubData` + N `glDrawElementsInstanced` with a triple-buffered persistent pool + one
  `glMultiDrawElementsIndirect` per material (reuse `draw_chunks_mdi`). First real upclock.
  `GBufferPass::geometry_pass_static_meshes` (330-533), `instanced_mesh.vert`, bump capacity.
- **Phase 2 — GPU compute cull + LOD select (the headline lever).** Upload-once SSBO + `prop_cull.comp`
  → indirect; delete the 40–110k CPU loop. Ends the downclock.
- **Phase 3 — Props cast shadows via the same indirect buffer.** BF1-floor RAISE + stress-tests the
  GPU-driven path under the 4× cascade multiplier. `ShadowPass::execute`.
- **Phase 4 — Foliage overdraw collapse** (after GPU-driving). Depth-prepass/EQUAL + grass alpha-test+A2C.
- **Phase 5 — TAA → TAAU** (per-pixel headroom + DLSS groundwork).
- **Phase 6 — Frame pacing / anti-downclock + cheap beauty** (contact shadows, per-texel roughness).
- **Phase 7 — DEFER: two-phase Hi-Z occlusion cull + octahedral impostor atlas + dithered LOD cross-fade.**
  Re-measure first; only if cull/far-canopy is the new ceiling.

## 10. Beauty-Floor Plan
Hold-and-raise, never trade down. Phases 1–3 are pixel-identical by construction (same
meshes/tints/LODs — validate draw-count + visible-instance **parity** vs the CPU path before deleting
it); Phase 3 ADDS prop shadows. Phase 4 depth-prepass is visually identical; grass alpha-test needs
the mandatory mip-coverage alpha lift to hold density. Phase 5 TAAU RAISES baseline quality (temporal
AA), gated by render-scale (Quality default). Phase 7 impostors give true far-canopy silhouettes vs
the current overdraw-heavy cross-billboards. Re-bless visual goldens ONCE at the final TAAU output.

## 11. Measurement & Attribution Plan
Fix attribution BEFORE Phase 1: (1) CPU-submit/present split (wall = max(CPU, GPU)); (2) NVML
power+clock per frame (success = power→300 W, clock→3090 MHz); (3) forest-DENSE fixed pose; (4)
clock-lock A/B (`nvidia-smi --lock-gpu-clocks`) to prove CPU-vs-GPU bound; (5) draw-count + visible-
instance parity assertion before deleting the CPU path; (6) ≥60-frame warmup so boost settles.

## 12. GL-vs-Vulkan Decision
**STAY OpenGL 4.5** for the whole 600 fps push; keep Vulkan-aware; defer the backend. Every lever
that buys 600 fps is GL-native and half-built in-engine (MDI, persistent SSBOs, compute cull, FSR2
TAAU). DLSS/hw-RT are Vulkan-gated and NOT on the 600 fps critical path. The MV-buffer + jitter +
history built in Phase 5 are exactly DLSS4's inputs and the indirect path maps 1:1 to
`VkDrawIndexedIndirectCommand`, so the future Vulkan migration is cheap. Revisit Vulkan only when
DLSS4/hw-RT becomes the next fidelity frontier.

## 13. Risks & Mitigations
GPU-cull readback re-introduces a sync stall (keep args GPU-resident, `…IndirectCount`); measurement
blindness (Phase 0 mandatory); capacity clamp at 16384 (size SSBO for the total + growth); scatter-
order determinism (preserve `frand()` order); depth-prepass before GPU-driving regresses (ordering);
TAA artifacts on thin foliage/wind MV (reactive mask, correct MVs, Quality default); per-instance
materials must fold into the SSBO or MDI batches break; vsync-off pacer thrash (sleep+spin hybrid).

## 14. Determinism & Re-Pin Impact
All changes are **render-only** → no `world_hash` re-pin. The single constraint: the SSBO upload must
preserve the seeded `frand()` scatter call ORDER. Visual goldens re-bless ONCE at the final output.

## 15. Engine-Fit / Reuse Inventory
MDI: `RenderPipeline::draw_chunks_mdi` (`RenderPipeline.cpp:1252-1345`). Persistent pools:
`RenderPipeline.cpp:241`, `FoliagePass.cpp:106-117`. Compute→indirect: `FoliagePass.cpp:521-657,
741-746`. Target to rewrite: `GBufferPass::geometry_pass_static_meshes` (`GBufferPass.cpp:330-533`),
buffers (`:51-69`). Scatter site: `main_client.cpp:~483-700, ~4532-4776`. Shadows: `ShadowPass.cpp:74-97`.

## 16. Acceptance Criteria & Exit Gates (for the eventual executable spec)
- [ ] AC-0: benchmark reports CPU-submit/present split + NVML power/clock on a forest-dense pose.
- [ ] AC-1/2: static-prop submit is GPU-driven (1–2 MDI calls/material, no per-frame entity loop);
      draw-count + visible-instance parity vs the old CPU path proven before deletion.
- [ ] AC-bound: clock-lock A/B shows the frame flips from CPU-bound to GPU-bound after Phase 2; GPU
      power climbs toward TDP / clock toward boost.
- [ ] AC-overdraw: depth-prepass makes canopy shade ~1× (gbuffer_ms drops materially).
- [ ] AC-taau: TAAU Quality holds the BF1 floor (no ghosting/flicker beyond gate tolerance) on the dense pose.
- [ ] AC-600: **600 fps (≤1.67 ms)** on the forest-dense pose at 3840×1600 with the GPU at boost clock.
- [ ] AC-beauty: all visual gates green at the re-blessed TAAU output; props cast shadows; no fidelity regression.

## Open Questions (for owner)
- Accept vsync-off + a custom frame pacer as the shipping default, or only in benchmark mode?
- TAAU default quality tier (1.5× Quality recommended) and is dynamic-resolution acceptable?
- Is 600 fps the firm target, or "as high as possible while holding the floor" (the levers are the same)?
- Priority vs. the deferred A3 far-field-void / arches and the C2 impostor-atlas follow-up.
