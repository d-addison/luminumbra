# Spec 004 — Path to 600 fps

> **Status: APPROVED (executable).** Promoted 2026-06-21 from the research-grounded PRD first draft
> (6-agent ultracode workflow). Companion research: `research-brief.md`. Owner decisions baked in
> (§Owner Decisions). Sibling pattern: `docs/specs/003-worldgen-water-render-deferred/spec.md`
> (gate/re-pin discipline). Shared files `main_client.cpp`, `TerrainPresetLoader.*` are co-edited by
> the create-world dev (spec 002) — **additive seams only**.

## ⚠ MEASURED REALITY (2026-06-21, Phase 0/1) — RE-SCOPE IN PROGRESS
Phase 0's honest profiling **invalidated the premise below** (inherited from spec 003 C2). On the
forest-dense pose the frame is CPU-bound, but the cost is NOT static-prop submission. Full breakdown
(release, dense pose; commits 205fda28 / ba1c43b1 / a45605fd):

```
cpu_submit ~22-29 ms  =  streaming 8-11 ms        (WorldSystem::update — deterministic)
                      +  foliage_rebuild ~5.3 ms  (FoliagePass readback stall @ FoliagePass.cpp:641 — render-only)
                      +  unattributed ~5 ms        (glfwPollEvents / scenario harness / chunk_scatter build)
                      +  render_frame ~2-3 ms      (static_prop submit only ~0.7-1.0 ms!)
                      +  sim 0.14 ms | ui_render 0.08 ms  (negligible)
```

**The GPU-driven static-prop rewrite (FR-R1..R3, R7 below) saves <1 ms — it is NOT the bottleneck.**
Real ≤1.67 ms path = streaming + foliage-readback + poll/scenario. Owner (2026-06-21): localize the
14 ms first (DONE), allow a batched re-pin for streaming if needed. **Re-scoped attack order:**
(1) foliage rebuild readback stall (render-only, safe); (2) streaming WorldSystem::update; (3) the
unattributed poll/scenario. FR-R0/R1 (measurement + prop cache + capacity fix) already LANDED and are
kept. The sections below are the ORIGINAL plan — retained for history; treat FR-R2/R3/R7 as low-priority
fidelity/scale items, not perf levers. See `plan.md` Session Log for the live re-scope.

## Context
luminumbra renders a vast Battlefield-1/Frostbite-floor voxel forest at ~256 fps (~3.9 ms) at
3840×1600 on the RTX 5070 Ti. Spec 003's C2 work measured the frame is **CPU/present-bound, not
GPU-bound**: ~40–110k static-prop entities are walked single-threaded every frame
(`GBufferPass::geometry_pass_static_meshes` — per-entity `glm::length` + `SelectTreeLod` + FNV64
grouping into `unordered_map`/`vector<mat4>` + per-group `glBufferSubData` + N
`glDrawElementsInstanced`), so the GPU starves and **downclocks** (26–41 W / 200–1900 MHz vs 300 W /
3090 MHz) and a 58 % triangle cut moved `gbuffer_ms` by **zero**. This spec **inverts where work
happens, without cutting beauty**: (1) move prop cull/LOD/submit onto the GPU so the CPU stops gating
and the card clocks back up; (2) collapse foliage overdraw; (3) add a TAA→TAAU temporal substrate.
The render path is **render-only** (sim is integer/fixed-point 30 Hz, never hashed), so determinism
is untouched — the single constraint is preserving the seeded `frand()` scatter call ORDER.

## Owner Decisions (resolved 2026-06-21)
1. **Target = "as high as possible while holding the BF1 floor"** — not a hard 1.67 ms cliff. Exit =
   measured improvement + no regression + a documented bound; still report the 600 fps / 1.67 ms
   number on the dense pose.
2. **Frame pacing = benchmark-only first, with a ship knob** — vsync-off + sleep+spin pacer drives the
   benchmark/RenderBudget gate; expose as a settings knob (default vsync-on for desktop); decide the
   shipping default later once measured.
3. **TAAU = Quality 1.5×, static, knob** — default Quality 1.5×/dim (~44 % pixels), fixed render-scale
   knob, **no dynamic resolution**; never ship Performance 2.0× as default.
4. **Include BOTH A3 (far-field-void/arches) AND C2 (octahedral impostor atlas).** C2 is render-only
   (joins FR-R). A3 is **worldgen → `world_hash`-affecting** → isolated FR-W tier with ONE re-pin,
   sequenced LAST so it never entangles the render determinism story.

## Goals
- Maximize fps on a forest-DENSE fixed pose at 3840×1600 with the GPU at boost clock; report the
  600 fps / 1.67 ms figure + the bound that remains.
- Hold or RAISE the BF1 fidelity floor (props cast shadows; TAAU temporal AA; impostor far-canopy).
- Honest CPU-vs-GPU attribution (wall = max(CPU_submit, GPU_work) + present; NVML power/clock).
- FR-R tier render-only (no `world_hash` re-pin); FR-W tier (A3) batched into ONE re-pin.

## Non-Goals
- A Vulkan backend (stay GL 4.5, Vulkan-aware); `GL_ARB_bindless_texture` (stay array-based);
  DLSS/hw-RT (Vulkan-gated, future); dynamic resolution; any sim/determinism change beyond the FR-W
  re-pin; any fidelity regression to win the budget; the create-world UI surface (spec 002).

## Budget Model
600 fps = **1.67 ms wall-clock = max(CPU_submit, GPU_work) + present** — NOT a sum of GPU pass timers
(the current benchmark's blind metric). Today ~3.9 ms is CPU-submit-gated (proven by the downclock +
the no-op triangle cut). Two moves: **(a)** CPU submit from a ~3.9 ms-gating 40–110k-entity loop →
**<1 ms** (a few compute dispatches + 1–2 MDI calls); **(b)** once CPU stops gating, the GPU
**upclocks ~1.5–3×** (same passes run far faster at boost), then **overdraw collapse** (depth-prepass:
3–8× canopy shading → 1×) + **TAAU Quality** (~44 % pixels → ~1.6–1.9× per-pixel headroom for ~0.6–1.0
ms reconstruct) bring GPU work under budget. **Pacing:** vsync ON idles the GPU → downclock; run
vsync-off + sleep+spin pacer + shallow render-ahead to hold boost clock.

## Functional Requirements

### FR-R · Render-only (no `world_hash` re-pin) — Phases 0–7
- **FR-R0 (measurement substrate, PREREQUISITE).** `--render-benchmark` today
  (`main_client.cpp:7242-7310`) sums per-pass GPU timers (`RenderPassFrameStats *_gpu_ms`) into
  `rb_total` under swap-interval 0 — **blind to CPU-submit/present** and emits schema
  `luminumbra.render_benchmark.v1`. It already pins a FIXED pose (`8,52,8`, Yaw 35, Pitch −12,
  near-noon) but that pose is a generic horizon view, **not forest-dense**. Add: (1) CPU-submit +
  present split (wall = max(CPU,GPU)+present); (2) **NVML** GPU power + clock sampling (does NOT exist
  in source today — add it, guarded/optional so non-NVIDIA + headless still build/run); (3) a
  forest-DENSE pose; (4) bump schema → `v2`. Update the `RenderBudget` gate
  (`validate-engine-frontier.ps1:6429-6481`) in lockstep (it hard-rejects unexpected schema, reads
  `avg_ms.total`, and currently uses `--auto-create-world`). 0 fps gain — un-blinds everything.
- **FR-R1 (persistent pool + MDI).** Replace per-group `glBufferSubData` (`GBufferPass.cpp:519`) + N
  `glDrawElementsInstanced` (:531) with a triple-buffered persistent-mapped pool + **one
  `glMultiDrawElementsIndirect` per material** across LOD batches (reuse `draw_chunks_mdi`,
  `RenderPipeline.cpp:1252-1352`, baseInstance-indexed — sidesteps `gl_DrawID`). **Fix the capacity
  bug:** `kStaticInstanceCapacity = 16384` (`GBufferPass.cpp:34`, clamp :516) silently drops below the
  ~110k prop set — size the pool for the TOTAL set and grow. First real upclock. Touch
  `instanced_mesh.vert` (baseInstance addressing).
- **FR-R2 (GPU compute cull + LOD select — HEADLINE).** Upload the full static-prop set **once** to an
  SSBO `{pos, scale, quat, tint, meshId, boundingSphere}` at world-scatter
  (`main_client.cpp:4532-4776`, **preserving the seeded `frand()` call ORDER** — load-bearing for
  determinism). A `prop_cull.comp` compute shader does frustum test + distance→LOD bucket +
  atomic-append survivors into per-LOD ranges and writes the indirect `instanceCount`s; the CPU issues
  one MDI per material with ZERO per-entity work. `SelectTreeLod`/`LodMeshPath` move into the compute
  shader; delete the per-frame `entt` loop + FNV grouping + per-group `glBufferSubData`. **Keep
  indirect args GPU-resident** — never read back the count on the hot path (the FoliagePass
  `glGetBufferSubData` at `FoliagePass.cpp:641-642` is the anti-pattern). Use
  `glMultiDrawElementsIndirectCount`. Ends the downclock.
- **FR-R3 (props cast shadows).** Feed the prop indirect buffer into `ShadowPass::execute`
  (`ShadowPass.cpp:54-100`; today draws chunks only, :74-97). Fidelity RAISE + stress-tests the
  GPU-driven path under the 4× cascade multiplier.
- **FR-R4 (overdraw collapse).** MUST come AFTER FR-R2 (else it doubles CPU draws). (a) Depth/Z
  pre-pass for alpha-tested trees/bushes, re-draw into the G-buffer with `glDepthFunc(GL_EQUAL)` +
  `glDepthMask(FALSE)` so each canopy pixel shades once. (b) Grass `GL_BLEND` → **alpha-TEST** (restore
  early-Z) + A2C / in-shader dither + **mandatory mip-coverage alpha lift** to hold distant density.
  (c) Extend the `g_buffer.frag` cheap paths. The lever that finally moves `gbuffer_ms`.
- **FR-R5 (TAA → TAAU).** Halton[2,3] projection jitter + a **motion-vector G-buffer attachment**
  (MUST cover shader-side wind in `instanced_mesh.vert` — a known MV hazard) + history buffer +
  neighborhood-clamp resolve (free edge AA first), then temporal **upsampling** with a render-scale
  knob, **default Quality 1.5×/dim** (Owner Decision 3). Exactly DLSS4's required inputs → cheap future
  Vulkan unlock.
- **FR-R6 (frame pacing / anti-downclock + cheap beauty).** vsync-off + sleep+spin pacer at a stable
  high cap + shallow render-ahead (hand-rolled Reflex-style just-in-time submit; no Reflex on GL).
  Benchmark-driven now; exposed as a settings knob (Owner Decision 2). Verify via NVML that power
  climbs toward 300 W / clock toward 3090 MHz. Cheap beauty (contact shadows, per-texel roughness) only
  if budget allows.
- **FR-R7 (C2 octahedral impostor atlas).** Replace the overdraw-heavy LOD3 cross-billboards for trees
  with an octahedral impostor atlas (bake inline). Render-only; fidelity-UP for far canopy + an
  overdraw cut. Slots into the LOD selection from FR-R2.

### FR-W · Worldgen tier (ONE re-pin, sequenced LAST) — Phase A3
- **FR-W1 (A3 far-field-void/arches follow-up).** Complete the far-field visibility of the
  surface-breaking features spec 003 added (sinkholes / arches / cave-mouths) so they read at distance
  (far-LOD SDF / impostor of the void). `world_hash`-affecting → isolated re-pin tier (spec-003
  pattern): all procgen hashing **unsigned**, guard normalize/division (release-only UB), and
  **byte-identical when the feature is disabled** (empty-case byte-zero hashing) so off-worlds don't
  move. Re-pin ONLY the gates whose preset enables the feature (verify each before touching).

## Non-Functional Requirements
- **NFR-1 (determinism):** `run==replay` byte-exact always holds. FR-R tier = **zero `world_hash`
  change** (only constraint: preserve `frand()` scatter order on SSBO upload). FR-W tier = ONE batched
  re-pin (literals ~4688/5213/5331/5435/5485 in `validate-engine-frontier.ps1` — verify each;
  re-pinning a gate whose preset doesn't enable the feature turns green→red). All procgen/GPU hashing
  unsigned; guard normalize/division. Re-run `HeadlessServerTick`/`PopulatedWorldReplay` each phase.
- **NFR-2 (memory):** instance SSBO sized for the TOTAL prop set + growth headroom (not clamped at
  16384); compute-cull scratch + indirect args sized per material/LOD; impostor atlas bounded.
- **NFR-3 (fidelity floor):** hold/raise BF1/Frostbite floor; perf opts must keep the floor (crisp
  TAAU upsample, lit/textured far field, seamless LOD); no ghosting/flicker beyond gate tolerance.
- **NFR-4 (concurrency):** `main_client.cpp`, `TerrainPresetLoader.*` co-edited by spec 002 — additive
  seams only. After a `.h` struct change misbehaves in release, `--clean-first` (stale-obj cross-TU).
- **NFR-5 (portability):** NVML + the GL 4.5 paths degrade gracefully (NVML optional/guarded; no hard
  dependency that breaks headless or non-NVIDIA CI).

## Phased Roadmap (each: lever → expected gain → touch points; re-measure between)
- **Phase 0 (FR-R0)** — measurement substrate. `main_client.cpp:7242-7310`, `RenderPassFrameStats`,
  `validate-engine-frontier.ps1:6429`. 0 fps; un-blinds.
- **Phase 1 (FR-R1)** — persistent pool + MDI + capacity fix. `GBufferPass.cpp:34/292-534`,
  `instanced_mesh.vert`. First upclock.
- **Phase 2 (FR-R2)** — GPU compute cull + LOD; delete the CPU loop. `prop_cull.comp` (new), scatter
  SSBO, `GBufferPass`. Ends the downclock (headline).
- **Phase 3 (FR-R3)** — props cast shadows via the indirect buffer. `ShadowPass.cpp:74-97`.
- **Phase 4 (FR-R4)** — overdraw collapse (depth-prepass/EQUAL + grass alpha-test+A2C). After Phase 2.
- **Phase 5 (FR-R5)** — TAA → TAAU Quality 1.5× + render-scale knob.
- **Phase 6 (FR-R6)** — frame pacing / anti-downclock + cheap beauty.
- **Phase 7 (FR-R7 / C2)** — octahedral impostor atlas for far LOD.
- **Phase A3 (FR-W1, LAST)** — far-field-void/arches; ONE re-pin; byte-identical when disabled.

## Determinism & Re-Pin Impact
FR-R (Phases 0–7) → no `world_hash` re-pin; pinned literals stay: PopulatedWorldReplay
`f314123daebb6cd1`, canonical `cf9c8cddf7156cd6`, NetworkedSession `ddfc228811d9f32b`. FR-W (A3) → ONE
re-pin of affected gates + regenerate persistence fixtures (local-dev re-pin is fine). Visual goldens
re-bless ONCE at the final TAAU output (parity-first); props-cast-shadows added.

## Risks & Mitigations
GPU-cull readback re-introduces a sync stall (keep args GPU-resident, `…IndirectCount`); measurement
blindness (Phase 0 mandatory); capacity clamp at 16384 (size for total + growth); scatter-order
determinism (preserve `frand()` order); depth-prepass before GPU-driving regresses (ordering); TAA
artifacts on thin foliage/wind MV (reactive mask, correct MVs, Quality default); per-instance materials
must fold into the SSBO or MDI batches break; vsync-off pacer thrash (sleep+spin hybrid); A3 entangling
render determinism (isolated FR-W tier, sequenced last, single re-pin); NVML absent on CI (guard/optional).

## GL-vs-Vulkan Decision
**STAY OpenGL 4.5** for the whole push; Vulkan-aware; defer the backend. Every 600 fps lever is
GL-native + half-built in-engine (MDI, persistent SSBOs, compute cull, FSR2-style TAAU). DLSS/hw-RT are
Vulkan-gated and NOT on the critical path; the MV-buffer + jitter + history (Phase 5) are exactly
DLSS4's inputs and the indirect path maps 1:1 to `VkDrawIndexedIndirectCommand`, so a future Vulkan
migration is cheap. Revisit Vulkan only when DLSS4/hw-RT becomes the next fidelity frontier.

## Acceptance Criteria & Exit Gates
- [ ] **AC-0:** benchmark reports CPU-submit/present split + NVML power/clock on a forest-DENSE pose
      (schema v2); `RenderBudget` gate updated in lockstep.
- [ ] **AC-1/2:** static-prop submit is GPU-driven (1–2 MDI/material, no per-frame entity loop);
      draw-count + visible-instance **parity** vs the old CPU path proven BEFORE deletion; capacity
      sized for the total set (no props dropped).
- [ ] **AC-bound:** clock-lock A/B (`nvidia-smi --lock-gpu-clocks`) shows the frame flips
      CPU-bound→GPU-bound after Phase 2; GPU power climbs toward TDP / clock toward boost.
- [ ] **AC-shadow:** props cast shadows via the indirect buffer.
- [ ] **AC-overdraw:** depth-prepass makes canopy shade ~1× (`gbuffer_ms` drops materially).
- [ ] **AC-taau:** TAAU Quality 1.5× holds the BF1 floor (no ghosting/flicker beyond tolerance) on the
      dense pose.
- [ ] **AC-fps:** fps maximized on the dense pose at 3840×1600 at boost clock; the 600 fps / 1.67 ms
      figure reported with the remaining bound documented (Owner Decision 1 — not a hard cliff).
- [ ] **AC-c2:** octahedral impostor atlas live for far LOD (fidelity-up vs cross-billboards).
- [ ] **AC-a3:** far-field-void/arches visible at distance; FR-W re-pin done; off-worlds byte-identical.
- [ ] **AC-beauty:** all FR-R determinism gates green (no re-pin); FR-W gates re-pinned once; visual
      gates green at the re-blessed output; no fidelity regression. Owner gets before/after PNGs +
      per-pass + CPU/clock numbers.
