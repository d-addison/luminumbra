# Plan: Path to 600 fps

> Authored directly (forge backlog planner does not have spec 004 registered; `forge plan
> 004-performance-600fps` → "not eligible"; spec.md/plan.md are the source of truth per the
> 2026-06-21 handoff §6). Companion: `spec.md`, `research-brief.md`.

## Overview
Invert where the frame's work happens. The frame is CPU/present-bound: a single-threaded 40–110k
static-prop loop in `GBufferPass::geometry_pass_static_meshes` gates submit, the GPU downclocks
(26–41 W / 200–1900 MHz), and GPU-pass timers are measured at idle clock — so the current
`--render-benchmark` (which sums those timers) is blind to the real bottleneck. We first make
measurement honest (Phase 0), then move prop cull/LOD/submit onto the GPU (Phases 1–2, the headline
lever) so the CPU stops gating and the card upclocks, then collapse foliage overdraw (Phase 4) and add
TAAU per-pixel headroom (Phase 5) to bring GPU work under budget, holding boost clock via a vsync-off
pacer (Phase 6). Beauty is held or raised (props cast shadows P3; impostor far-canopy P7/C2; temporal
AA P5). All render-side work is render-only (no `world_hash` change); the one worldgen item (A3
far-field voids/arches) is isolated to a final re-pin tier.

## Architecture

### Key Design Decisions
- **Stay OpenGL 4.5, reuse in-engine machinery.** MDI via `RenderPipeline::draw_chunks_mdi`
  (`RenderPipeline.cpp:1252-1352`, baseInstance-indexed — avoids `gl_DrawID`/ARB_shader_draw_parameters
  risk); persistent-mapped pools (`RenderPipeline.cpp:241-261`, `FoliagePass.cpp:106-117`);
  compute→count→indirect shape (`FoliagePass.cpp:615-636/743-745`). No greenfield renderer.
- **Upload-once SSBO + GPU compute cull.** Static props uploaded once at scatter
  (`main_client.cpp:4532-4776`) into an SSBO `{pos, scale, quat, tint, meshId, boundingSphere}`;
  `prop_cull.comp` does frustum + distance→LOD + atomic-append → indirect args. CPU issues 1–2
  `glMultiDrawElementsIndirect[Count]` per material, zero per-entity work.
- **Args stay GPU-resident.** Never read back indirect counts on the hot path — the FoliagePass
  `glGetBufferSubData` (`FoliagePass.cpp:641-642`) is the sync-stall anti-pattern. Use
  `glMultiDrawElementsIndirectCount`.
- **Determinism by call-order preservation.** The SSBO upload must walk the scatter in the same order
  the seeded `frand()` lambda (`main_client.cpp:4552`) produces — render-only, no `world_hash` change.
- **Measurement before optimization.** Wall = max(CPU_submit, GPU_work) + present; NVML power/clock;
  forest-dense pose; clock-lock A/B. Without this the headline win reads as a no-op.
- **Sequencing is load-bearing.** Depth-prepass (P4) MUST follow GPU-driving (P2) or it doubles CPU
  draws. A3 (worldgen) is LAST so it never entangles the render determinism story.

### Affected Modules
- `src/luminumbra_client/main_client.cpp` — benchmark (`:7242-7310`), scatter (`:4532-4776`); additive
  seams only (co-edited by spec 002).
- `src/luminumbra_client/rendering/passes/GBufferPass.{h,cpp}` — rewrite `geometry_pass_static_meshes`
  (`:292-534`), capacity (`:34`), buffers (`:62/:67`).
- `src/luminumbra_client/rendering/passes/ShadowPass.cpp` — prop shadows (`:74-97`).
- `src/luminumbra_client/rendering/passes/FoliagePass.{h,cpp}` — overdraw (alpha-test/A2C), readback removal.
- `src/luminumbra_client/rendering/RenderPipeline.{h,cpp}` — `RenderPassFrameStats` (CPU/present/NVML
  fields), pacer, TAAU resolve, history/MV attachments, render-scale.
- `res/shaders/` — `prop_cull.comp` (new), `instanced_mesh.vert` (baseInstance + MV/wind),
  `g_buffer.frag` (cheap paths), depth-prepass + TAAU resolve shaders, impostor bake/sample.
- `.forge/scripts/validate-engine-frontier.ps1` — `RenderBudget` gate (`:6429-6481`) updated for v2
  schema + dense pose; FR-W re-pin literals.
- New NVML wrapper (optional/guarded) under `src/luminumbra_client/core/` or `rendering/`.

### Data Model
- **`RenderPassFrameStats` (extend):** existing `*_gpu_ms` + `gpu_timers_supported`; ADD
  `cpu_submit_ms`, `present_ms`, `frame_wall_ms`, `gpu_power_w`, `gpu_clock_mhz`, `nvml_supported`.
- **Benchmark JSON schema `v2`:** `luminumbra.render_benchmark.v2` — keep `avg_ms.*`; add
  `avg.cpu_submit_ms/present_ms/frame_wall_ms/gpu_power_w/gpu_clock_mhz`, `pose: "forest_dense"`.
- **Prop instance SSBO:** `struct GpuPropInstance { vec4 pos_scale; vec4 quat; vec4 tint; uint meshId;
  vec4 boundingSphere; }` (std430-aligned), sized for the TOTAL prop set + growth.
- **Indirect command buffer:** `DrawElementsIndirectCommand[]` per material/LOD + a count buffer.

## Implementation Tasks (waves = phases; sequential, re-measure between)

### Wave 0 — Measurement (PREREQUISITE)
- [ ] T000: CPU-submit/present split + NVML power/clock in `RenderPassFrameStats` + benchmark; dense
  pose; schema v2; update `RenderBudget` gate in lockstep. Tests: benchmark emits new fields;
  clock-lock A/B harness. (FR-R0)

### Wave 1 — Persistent pool + MDI
- [ ] T010: Triple-buffered persistent instance pool + one MDI per material; delete per-group
  `glBufferSubData`/N draws; fix `kStaticInstanceCapacity` (size for total + grow). Test: capacity
  assertion, no props dropped; determinism unchanged. (FR-R1)

### Wave 2 — GPU compute cull (HEADLINE)
- [ ] T020: Upload-once SSBO at scatter (preserve frand order). (FR-R2)
- [ ] T021: `prop_cull.comp` frustum + distance→LOD + atomic-append → indirect args (GPU-resident).
- [ ] T022: Parity test (draw-count + visible-instance set vs CPU path on dense pose) BEFORE deletion.
- [ ] T023: Delete the CPU entity loop + FNV grouping; CPU issues MDI only. Clock-lock A/B flips bound.

### Wave 3 — Prop shadows
- [ ] T030: Feed prop indirect buffer into `ShadowPass` cascades. (FR-R3)

### Wave 4 — Overdraw collapse (after Wave 2)
- [ ] T040: Depth/Z-prepass + `GL_EQUAL`/`glDepthMask(FALSE)` for trees/bushes. (FR-R4)
- [ ] T041: Grass `GL_BLEND`→alpha-TEST + A2C/dither + mip-coverage alpha lift. Test: `gbuffer_ms` drops.

### Wave 5 — TAA → TAAU
- [ ] T050: Halton[2,3] jitter + motion-vector G-buffer attachment (cover wind) + history + clamp. (FR-R5)
- [ ] T051: TAAU Quality 1.5× + render-scale knob. Test: no ghosting/flicker beyond gate tolerance.

### Wave 6 — Pacing + cheap beauty
- [ ] T060: vsync-off sleep+spin pacer + shallow render-ahead (benchmark-driven, ship knob). NVML proof. (FR-R6)
- [ ] T061: Cheap beauty (contact shadows / per-texel roughness) only if budget allows.

### Wave 7 — C2 impostor atlas
- [ ] T070: Octahedral impostor atlas bake + sample for far LOD; slot into FR-R2 LOD selection. (FR-R7)

### Wave A3 — Worldgen (LAST, ONE re-pin)
- [ ] T080: Far-field-void/arches visibility; byte-identical when disabled; ONE re-pin + persistence
  fixtures regenerated. (FR-W1)

### Final
- [ ] T090: Full gate sweep + clock-lock A/B proof + re-bless visual goldens once + before/after PNGs +
  numbers to owner + closeout handoff. No push.

## Session Log / Findings

### Phase 0 — LANDED + VERIFIED (commit 205fda28)
Measurement substrate built and proven. On the forest-DENSE pose (3840×1581, release):
- **wall 16–29 ms, cpu_submit 16–29 ms, present 0.2–0.45 ms, gpu_pass_sum 3.9–6.9 ms,
  GPU 45–62 W / 1632–1854 MHz, bound=cpu.** The spec thesis is confirmed with hard numbers:
  the frame is overwhelmingly CPU-submit-bound and the GPU starves + downclocks (1.6–1.9 GHz / <65 W
  vs 3.09 GHz / 300 W boost). The old benchmark pose (pitch −12, looking down) culled most of the
  forest and hid this (~256 fps); the dense shallow pose exposes the true ~34–62 fps worst case.
- Added: CPU-submit/present/wall split + NVML power/clock (`NvmlSampler.h`, dynamic-load, guarded),
  forest-dense pose, schema `v2`, `--render-benchmark-screenshot`, RenderBudget gate parses v2 +
  prints honest metrics. RenderBudget stays intentionally RED on `total` (the spec-004 target).
- Determinism: untouched by construction (client render/measurement only; no sim/common/server/
  scatter/hash code). No re-pin.

### Phase 1+2 architectural realization (READ BEFORE STARTING)
MDI for props is NOT a drop-in reuse of `draw_chunks_mdi`. Props are **heterogeneous meshes**
(`GBufferPass.cpp:469-533`: each group has its own `mesh->vao`/index buffer + per-group uniforms:
`u_materialId`, `u_skinnedAlbedoLayer/NormalLayer`, `u_alphaTest`, `u_windStrength`, `u_forceFlat`).
A single `glMultiDrawElementsIndirect` draws from ONE bound VAO/index buffer with no uniform changes,
so the GPU-driven path requires:
1. **A shared static-prop mesh pool** — upload each unique prop mesh (incl. LOD variants) into a
   shared VBO+EBO once, record `{firstIndex, baseVertex, indexCount}` per mesh (mirrors the chunk pool
   at `RenderPipeline.cpp:241`). The cull/LOD compute then emits one `DrawElementsIndirectCommand` per
   (mesh-variant) with `baseInstance` into a shared instance buffer.
2. **Per-instance material attributes** — fold `materialId`, `albedoLayer`, `normalLayer`, `alphaTest`,
   `windStrength`, `forceFlat`, `tint` into the instance SSBO so `instanced_mesh.vert`/`g_buffer.frag`
   read them PER-INSTANCE (today they are per-group uniforms). All props then draw under ONE bound
   texture array (`static_model_texture_array`) with ~1–2 MDI calls. This is the load-bearing change
   that lets Phase 2's compute cull write indirect args with zero CPU per-entity work.
Because of this coupling, treat Phase 1 (pool + MDI of the already-CPU-grouped batches) and Phase 2
(compute cull feeding the same structure) as one focused GPU-driven rewrite. Keep the CPU path alive
behind a flag until the draw-count + visible-instance parity test passes on the dense pose.

### Phase 1 — LANDED + a SPEC-INVALIDATING finding (commit ba1c43b1)
Cached static-prop instance data (precomputed matrix+tint, reused group buffers) + fixed the
`kStaticInstanceCapacity` 16384 clamp (→131072). Render parity confirmed (identical forest). But the
CPU-submit did NOT move — so I added CPU per-phase profiling, which **overturns the spec's inherited
premise**. On the dense pose (release):

```
cpu_submit ~28.4 ms  =  render_frame ~3.1 ms   (static_prop submit only ~1.0 ms!)
                      +  sim_tick     ~0.19 ms  (well-decoupled — not a problem)
                      +  streaming    ~11 ms    (WorldSystem::update, even on a STATIC camera)
                      +  ~14 ms       other     (UI / foliage rebuild + readback stall / scenario / poll)
```

**The static-prop submit loop — spec 004's entire headline lever (Phases 1–3, 7) — is ~1 ms, not the
bottleneck.** Moving it to the GPU saves <1 ms. The real frame cost is **world streaming (~11 ms)** +
**~14 ms of other per-frame CPU**. The render pipeline itself is only ~3 ms. Spec 004 as written (from
spec 003 C2's premise) optimizes the wrong target. **Awaiting owner decision on re-scope** (see below).

#### Re-scope candidates (the actual ≤1.67 ms path)
1. **World streaming ~11 ms** — `gameSession->GetWorldSystem()->update()` costs ~11 ms PER FRAME on a
   static camera (no new chunks should be streaming). Likely re-scanning the full loaded-chunk ring /
   physics broadphase / snapshot rebuild every frame. Biggest single lever. Investigate + make
   incremental/threaded.
2. **~14 ms "other"** — localize next: foliage `rebuild_instances` + the known `glGetBufferSubData`
   sync stall (`FoliagePass.cpp:641`), ImGui overlay, `glfwPollEvents`, scenario-harness per-frame work.
3. The GPU-driven prop rewrite (old Phases 2–7) drops to LOW priority (≤1 ms); keep C2/A3 as fidelity
   items, not perf levers.

### Re-scope execution (commits 7b18150f …)
- **Foliage readback stall — LANDED (7b18150f).** Removed the synchronous `glGetBufferSubData`
  (FoliagePass.cpp:641) from the hot path (render-only; `set_readback_enabled`, default-on for the
  gate, off in play/benchmark; draws straight from the SSBO via `glDrawArraysIndirect`). Grass parity
  confirmed.
- **MEASUREMENT BLOCKER:** benchmark A/B is unreliable — the GPU clock floats 896–2851 MHz between
  runs (DVFS; frame is CPU-bound), swinging every metric ~3×. `nvidia-smi --lock-gpu-clocks` needs
  ADMIN (denied in the sandboxed shell). For reliable A/B going forward, the owner must lock the clock
  in an admin shell, OR we verify by WORK-COUNTS (rebuilds/re-meshes performed), which are
  clock-independent.
- **Root cause identified:** both streaming (~11–13 ms) and foliage rebuild (~5–9 ms) do FULL work
  every frame on a STATIC camera. Streaming `SHIELD_WorldSystem::update` (`:1933`) runs, per frame:
  `process_completed_meshing_jobs`, `PrefetchHydroRegions` (768 m/anchor), and an O(N) scan over ALL
  loaded chunks (`:1953`) — `update_chunk_activation` is correctly throttled, these are not. The
  foliage scatter cache fails to elide (sig thrashes on per-frame `set_wind` + streaming chunk-set
  churn). **Next: make this redundant per-frame work elide (verified by work-counts), starting with
  streaming (owner OK'd a batched re-pin if a fix touches world_hash).**

## Gate Criteria
The plan phase is complete when:
- [x] All tasks defined with clear acceptance criteria (above + spec ACs).
- [x] Dependencies mapped (phases sequential; P4 after P2; A3 last).
- [x] No open NEEDS CLARIFICATION (4 owner questions resolved 2026-06-21).

## Risks & Unknowns

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| GPU-cull readback re-adds a sync stall | Med | High | Keep args GPU-resident; `glMultiDrawElementsIndirectCount`; no `glGet*` on hot path |
| Measurement blindness hides the win | High (if skipped) | High | Phase 0 mandatory before any opt; clock-lock A/B |
| 16384 capacity clamp drops props | High (exists) | Med | Size SSBO for total + growth; capacity assertion test |
| Scatter-order determinism break | Med | High | Preserve `frand()` call order on SSBO upload; run==replay each phase |
| Depth-prepass before GPU-driving regresses | Med | Med | Strict ordering: P4 after P2 |
| TAA artifacts on thin foliage / wind MV | Med | Med | Reactive mask, correct wind MVs, Quality 1.5× default |
| Per-instance materials break MDI batching | Med | Med | Fold material/tint into SSBO; batch per material |
| vsync-off pacer thrash | Med | Low | sleep+spin hybrid; benchmark-only first, ship knob |
| A3 entangles render determinism | Med | Med | Isolated FR-W tier, sequenced last, single re-pin, byte-identical-when-disabled |
| NVML absent (non-NVIDIA/headless CI) | Med | Low | NVML optional/guarded; degrade to gpu_*=null |
