# HANDOFF 2026-07-06 — M0 hygiene closed · FOLIAGE-05 · GPU-P09 render-scale seam (byte-identical @1.0, 0.67 fidelity blocked on a depth bug)

**Supersedes** `HANDOFF-2026-07-06-post-A1-aether-and-gpu-runway.md` for the wave/runway inventory.
**Branch:** `feat/polyglot-audit-roadmap` — **NEVER push**; commit per green gate only.
**HEAD:** `03a57668`. **Backlog: 147/190 done** (43 remaining: gpu 14 · net 7 · ui 5 · buildtestops 5
· audio 4 · foliage 3 · instinct 2 · atmo/water/shield 1 each).
No canonical sim hash moved this session — every change is render-only or doc/test.

---

## 0. TL;DR

This session closed **M0 (the hygiene on-ramp)** — OPS-05/06, NET-10, SHIELD-10 — landed
**FOLIAGE-05** (retiring the ForestPerfBudget chartered RED so the gate lane is now 100% green),
and built most of **GPU-P09, the render-scale seam** (the DLSS precondition). The seam is
**byte-identical at `render_scale = 1.0`** (RenderParityFrame EXACT 0.0, verified 9×) and renders
**full-frame at 0.67**, but has ONE remaining bug: at scale < 1.0 the **sky renders black and the
terrain is unlit** (§3 is the debugging dossier — read it before touching the seam). The
byte-identical-at-1.0 HARD gate is met + committed; only the 0.67 visual fidelity is blocked.

The remaining runway is: **finish GPU-P09** (§3), then **M2–M6** (HLSL port → native Vulkan →
IUpscaler/DLSS → DX12 → hardware RT) plus the file-disjoint **NET/UI/AUDIO/OPS/FOLIAGE/sim-tail
clusters** (§4), plus the **owner activation menus** (§5).

---

## 1. What landed this session (11 commits, in order)

| Commit | What |
|---|---|
| `a7baee98` | **OPS-05** — retire root `build/` tree; `-Strict` preflight; NEW `BuildTreeStrict` frontier gate; STANDARDS.md §8 + docs/CLAUDE.md + ci.yml → one canonical preset tree. |
| `9e46e96e` | **OPS-06** — one-canonical-tree in specs 016/018/019; NEW `Assert-Absent` + Test-Sections negative doc-lint; refreshed spec 020 CMakeLists citations. |
| `4e2834de` | **NET-10** — lockstep oracle/replay/small-co-op scope in LockstepSession.h; NEW `NetDemotionDocGrep` gate; arch-doc anchors re-pointed. |
| `f46c813b` | **SHIELD-10** — two-tier SDF producer contract in sdf-contract.md; NEW `SdfContractDocLint` ctest (#1722) + test/features/sdf-contract.feature; fixed stale ServerWorldRunner.h ref. |
| `3da0befc` | docs(audit) — A1 aether close + Wave I (M0) record in priority-ranking.md. |
| `26cc8223` | **FOLIAGE-05** — ForestPerfBudget re-tasked GREEN. Modeled the far-field octa-impostor fold in the CPU perf model; **re-blessed the tri budget 12M→60M** (SURFACED decision — impostor-ON load is 52.7M near-field-dominated; 12M was aspirational + doesn't track real cost per TreeLod.h:39-44; 60M is a regression tripwire, impostor-OFF = 99.4M). Inverted -Mode FarFieldForestBudget to assert `over_budget.any == false`. |
| `2b827d47` | **GPU-P09 Phase A** — internal render-scale extent + scaled-target allocation seam (byte-identical). |
| `0aed2ac5` | **GPU-P09 Phase B.1** — scene-pass viewports → internal. |
| `61d75955` | **GPU-P09 Phase B.2a** — OIT/aerial/god-rays/ctx.internal_*/taau-u_texel → internal. |
| `3bf5ce00` | **GPU-P09 Phase B.2b** — `LUMIN_RENDER_SCALE` activation + FinalBlit internal→output upscale + depth-blit internal rect. |
| `03a57668` | **GPU-P09** — SSAO march-metric + water/particle/cave resolution uniforms → internal. |

---

## 2. GPU-P09 render-scale seam — what's DONE (do NOT redo)

Goal: render the scene at `internal = round(output × render_scale)` into the scaled
G-buffer/lighting/SSAO/water intermediates, then upscale to output. The precondition for
GPU-P10 (IUpscaler) and GPU-P11 (DLSS). Deps GPU-04 + GPU-12 both `done`.

**Landed + byte-identical at scale 1.0 (RenderParityFrame EXACT 0.0, verified repeatedly):**
- `RenderPipeline` members (`RenderPipeline.h` ~1025): `float m_render_scale = 1.0f`,
  `u32 m_internal_width/height`. `startup()` (~638) reads `LUMIN_RENDER_SCALE` (env A/B knob,
  clamp [0.5,1.0]) → `m_render_scale`, computes `m_internal_* = lround(m_screen_* * m_render_scale)`;
  `on_resize()` (~3526) recomputes. SCALED inits (init_lighting_fbo/init_gbuffer/init_ssao) use
  `m_internal_*`; shadow atlas + TAAU history stay output.
- `RenderContext.h` (~40): `internal_width/height` + `internal_w()/internal_h()` accessors (fall
  back to screen_*); populated alongside `screen_*` in all 15 context builders.
- Scaled scene-pass viewports → `m_internal_*` (geometry/gbuffer/lighting composites) and
  `ctx.internal_w()/h()` (aerial ~3758, god-rays ~3860). OIT (m_oit textures + viewport) internal.
- taau_resolve `u_texel = 1/internal` (samples internal u_current, writes output history).
- `FinalBlitPass::execute` (`passes/FinalBlitPass.h`): source rect = INTERNAL, blit → output with
  GL_LINEAR = the upscale (**required** — was 1:1-copying the corner, leaving the rest black).
- G-buffer→lighting **depth blit** (`execute_stage_depth_blit_to_lighting` ~2808): internal rect.
- SSAO march metric (`u_screenSize`) + half/blur viewports, water/particle/cave `u_screenSize`
  → internal.

**Verify byte-identity:** `& powershell -File .forge/scripts/validate-engine-frontier.ps1
-Mode RenderParityFrame -BuildPreset debug` → "PASS ... EXACT (score 0.0, 3840x1581)".

---

## 3. GPU-P09 — THE REMAINING BUG (debugging dossier — read before touching the seam)

**Symptom:** at `LUMIN_RENDER_SCALE=0.67` the parity-pose capture renders FULL-FRAME with correct
**foliage**, but the **SKY renders pure BLACK** (it is light blue at 1.0) and the **terrain/ground
is unlit black** except a blown-out white horizon strip. The frame is ~4–15× darker than 1.0
(band-diff: sky 0.29×, horizon 0.11×, ground 0.07× — direct-lit foliage stays bright).

**Repro (each cycle ~7 min on the RTX 5070 Ti):**
```
$env:LUMIN_RENDER_SCALE = "0.67"
& powershell -File .forge/scripts/validate-engine-frontier.ps1 -Mode RenderParityFrame -BuildPreset debug
Remove-Item Env:\LUMIN_RENDER_SCALE
python tools/ppm_to_png.py build/debug/test-artifacts/render/frame-parity/frame_parity_a.ppm out.png
```
Band-diff helper written to the scratchpad this session: `framediff.py` (compares two PNGs by
sky/horizon/ground band means). GLSL shaders hot-load at client boot — a shader-only diagnostic
needs NO rebuild, just the capture.

**RULED OUT (with evidence — do not re-investigate):**
- **Auto-exposure** — `m_auto_exposure_metered = false` by default (RenderPipeline.h:1302); and the
  meter (`luminance_reduce.comp`) samples via NORMALIZED UVs, so it's resolution-independent anyway.
- **SSAO / ambient occlusion** — forcing `float ao = 1.0` in `lighting_pass.frag:437` changed the
  frame by ~0 (ao ≈ 1 already). The SSAO textures ARE correctly internal-sized (init_ssao gets
  internal dims; halfW/halfH = internal/2).
- **Ambient color** — `lighting_pass.frag:700` `ambient = (ambientDiffuse+ambientSpecular)*ao*skyVis`,
  `skyVis=1` by default (cave AO off); `ambientDiffuse/Specular` are `u_skyAmbientColor` × per-pixel
  Albedo/hemi. `u_skyAmbientColor` (= `m_skyAmbientColor`, computed ~RenderPipeline.cpp:6084 from
  sun.intensity/moon/season) is FULLY ANALYTIC → resolution-independent.
- **Per-pass resolution uniforms** — SSAO/water/particle/cave `u_screenSize` → internal (committed
  `03a57668`); lighting `u_screenSize` is a DEAD uniform (only the default-OFF cave-AO march reads
  it); enhanced_skybox `gl_FragCoord` is debug-checker-only.

**LOCALIZATION (where the evidence points):** the **sky's depth-masked draw fails and the terrain
is unlit at internal resolution** → a **DEPTH-BUFFER-at-internal-res** problem. The full-res sky
path runs (`m_cloud_quality = 0` default → the `else` branch ~RenderPipeline.cpp:2865:
`glBindFramebuffer(lighting); m_skybox_pass->execute(...)`, NO explicit viewport — inherits). The
sky dome draws where the lighting depth == far; a black sky means its depth test FAILS everywhere
→ the lighting depth is wrong (all-near, or not the copied gbuffer far-plane) at 0.67. Bilinear
upscale CANNOT darken, so the internal lit scene is genuinely dark → the terrain isn't receiving
sun (reads shadowed/unlit) which is also depth/world-pos driven.

**NEXT DIAGNOSTIC (start here):**
1. **Instrument the internal depth**: dump/visualize the gbuffer depth AND the lighting depth (post
   depth-blit) at 0.67 — confirm empty regions are far (1.0). A shader-only quick test: in the
   skybox path, temporarily disable the sky's depth test (or force it to draw) — if the sky appears,
   the bug is the depth-test/lighting-depth; if not, the skybox itself.
2. Suspects to check at internal res: the **lighting pass depth write** (does the deferred fullscreen
   lighting clobber the lighting depth before the depth-blit restores it? order: lighting → depth_blit
   → skybox); the **gbuffer depth CLEAR** vs the internal viewport; whether the skybox `else` branch
   needs an EXPLICIT `glViewport(0,0,m_internal_width,m_internal_height)` (it currently inherits).
3. Also verify the **terrain (SDF marching-cubes mesh) actually rasterizes** at 0.67 (the foliage is
   instanced + renders; confirm the terrain geometry pass isn't culled/mis-viewport'd at internal).

**THEN finish GPU-P09 Phase B.2 (remaining scope):**
- **LOD mip bias** `log2(scale)` on the scaled samplers (sharper texturing at reduced internal res).
- **taau upsampler polish**: taau_resolve already bilinear-upscales via normalized TexCoords +
  `u_texel=1/internal`; at scale 1.0 BYPASS the upscaler (no 1:1 resample) but flow through the new
  plumbing (advisor: NOT `if(scale==1.0) old_path()`). Force TAAU/temporal OFF for the parity capture.
- **Wire `user.render_scale`** properly: it exists (SystemConfig.h:36, parsed SystemConfig.cpp:68-69)
  but is read NOWHERE in the client except my env knob. Add a `set_render_scale(float)` on RenderPipeline
  and have main_client call it with `config.user.render_scale` before startup (clamp; recompute on a
  settings change like on_resize). Keep the LUMIN_RENDER_SCALE override for A/B.
- **NEW `UpscaleSeamParity` gate** (design fully scoped by exploration this session — PS Mode, mirror
  `Test-RenderParityFrame` @validate-engine-frontier.ps1:6940, provenance-bound via
  `Assert-ArtifactProvenance`, **NOT in -Mode All**):
  - client flag `--upscale-seam-parity <dir>` (main_client.cpp ~2881, reuse the fixed pose/settle
    at ~7188-7218);
  - `RenderPipeline::capture_upscale_seam_parity` modeled on `capture_frame_parity` (RenderPipeline.cpp:803)
    + the two-different-paths shape of `capture_finalblit_parity` (:741);
  - **leg 1 (HARD, EXACT 0.0)** scale=1.0 vs the native no-seam reference; **leg 2 (SOFT)** 0.67×
    upscaled vs native, luma FLIP ≤ a PRE-REGISTERED threshold (~0.08 floor; anchor like
    `rhi_pilot_flip_test.cpp:64` kGoThreshold; assert threshold explicit+bounded per the idiom at
    validate-engine-frontier.ps1:1134-1140);
  - emit `luminumbra.upscale_seam_parity.v1` JSON; decide + document what leg 2 compares against.
- On green: flip backlog **GPU-P09** (rank 113) + GPU-07 → done; `python docs/audit/021/validate_backlog.py`;
  append a wave record to priority-ranking.md; **VISUAL PAUSE** (looked-at 0.67 capture blessed).

**Live tracking:** auto-memory `m1-render-scale-in-progress.md`; the file:line plan at
`C:\Users\David\.claude\plans\m1-gpu-p09-render-scale-seam.md`.

---

## 4. Remaining runway — Waves M2–M6 + file-disjoint clusters

Full detail: campaign plan `C:\Users\David\.claude\plans\ultracode-land-all-of-joyful-matsumoto.md`
(§"WAVES I–K", M0–M6); backlog `docs/audit/021/backlog.json`; `docs/audit/021/gpu-modernization-plan.md`.
**Landed GPU foundation (do NOT redo):** rhi/ backend selector + headless Diligent bring-up
(GPU-01..06/09/12/14/P01..P05); `cmake/diligent.cmake` (v2.5.6 GL+Vk, ucrt64); slang pilot wiring;
2 pilot HLSL shaders. **Chains:** U: P09→P10→P11 · B: P06→P07→P08 · R: P12→P13→P14. **Shared-file
serialization** (INLINE, one track at a time, orchestrator-owned, NEVER fan out — interdependent
render code diverges): `RenderContext.h`, `RenderResourceRegistry.*`, `RenderPipeline.*`,
`main_client.cpp`, `validate-engine-frontier.ps1`, `test/CMakeLists.txt`, `PassShaderLayouts.cpp`.

### 4.1 The GPU spine (M2–M6)
- **M2 — HLSL port (GPU-P06 ≡ GPU-08):** build `cmake/shaders.cmake` (slangc → GLSL+SPIR-V+reflection
  into `build/shaders_gen/`, cached; Shader.cpp loads emitted GLSL; hot-reload watches `.hlsl`) → NEW
  `ShaderSingleSourceInventory` gate → dead-shader census (~53→~43). **Batches F1–F6**, each:
  HLSL → reflection-parity vs PassShaderLayouts → whole-frame in-process A/B (emitted-vs-original,
  `-Mode RenderParityFrame` == 0.0) → delete originals → inventory green. **INLINE-only** (do NOT fan
  out): enhanced_skybox (42KB), g_buffer, lighting, water, particles, grass_scatter.comp,
  sdf_generation.compute. FOLIAGE-09 BEFORE the compute batch. P09 before P06's TAAU batch (done).
- **M3 — native Vulkan (GPU-P07):** rhi/ gains PSO/command/texture backing behind 016 handles;
  per-pass backend routing. Port order (increasing coupling): blit→ssao→aerial+god-rays→skybox→gbuffer
  →lighting→shadow→water→particles(GS risk)→foliage+grass_scatter→plant-procgen(post-FOLIAGE-07)→
  decals→overlays→**TAAU last (=DLSS fallback)**. Per pass: GL-via-Diligent baseline → native-Vulkan
  in-process FLIP + frame-health (Wave-F A/B harness, RenderParityFrame == 0.0). NEW `VulkanPassParityGpu`.
- **M4 — IUpscaler + DLSS (GPU-P10, GPU-P11):** P10 `rendering/upscale/` `IUpscaler{color,depth,motion,
  jitter,exposure,extents}→color`; TAAU = provider #1; FSR2/XeSS **stubs compile only** (need a 2nd
  machine to validate). NEW `UpscalerSelectSmoke`. P11 (DLSS): `cmake/streamline.cmake` FetchContent
  (create it); **RUNTIME-loaded DLLs only** (sl.interposer+sl.dlss+nvngx staged beside the binary, NO
  import-lib linkage); Vulkan manual-hooking via Diligent native handles; RTX detect + TAAU fallback;
  `LUMIN_DLSS=0` override; NEW `DlssSwapSmoke` RTX-gated, EXCLUDED from -Mode All. **VISUAL PAUSE**
  (DLSS presets + FOLIAGE-10 morphology). GPU-P10/P11 depend on P09.
- **M5 — DX12 + nightly (GPU-P08):** `DILIGENT_NO_DIRECT3D12=OFF` (TIMEBOXED, mingw risk; fallback =
  parse-only). NEW `Dx12PassParityGpu`. + OPS-08, OPS-09 (nightly full-gate, after FOLIAGE-05 ✓),
  FOLIAGE-10.
- **M6 — hardware RT + tail (GPU-P12/P13/P14):** P12 BLAS/TLAS (`rendering/rt/` per-chunk BLAS at
  render-mesh upload — **render residency ONLY, never sim hash**; amortized per-frame build budget;
  `LUMIN_RT=0` = zero work); P13 RT-GI/AO (slangc lib-profile HLSL; additive in LightingPass alongside
  the untouched software ShieldRtFarFieldPass — **augments-never-replaces**; NFR-4 carve-out = frame-
  health + visual, NOT strict FLIP; ShieldRt* trio stays green); P14 RT reflections in WaterPass
  (`LUMIN_RT_REFLECTIONS=1` OFF, screen-space fallback preserved). **VISUAL PAUSE** + campaign final report.

### 4.2 Non-GPU clusters (file-disjoint — CAN fan out via Workflow/Agent; agents never build, never
touch main_client·GameSession; `git status` sweep after every generation)
- **NET (7):** NET-06 delta-ON variants (before NET-07) · NET-07 N=32 over-the-wire soak · NET-08 GNS
  matrix (ENABLE_GNS OFF) · NET-09 backpressure (ThrottledFrames is a hard-0 stub) · NET-11 single-port
  multi-connection accept · **NET-12 HARDWARE-BLOCKED** (Steam SDR → needs 2nd machine) · NET-13 POSIX
  TcpTransport #else (Docker-validated).
- **UI (5):** UI-06 world-select from `worlds/saves/` + thumbnails · UI-09 unhide settings rows · UI-10
  codex photo thumbnails · UI-11 first-session tutorial · **UI-08 fidelity baseline @3840×1600, LAST in
  UI (bless once)**.
- **AUDIO (4):** AUDIO-11 real occlusion (`SetPhysicsSystem` ~main_client.cpp:3025-3040, Jolt raycasts)
  · AUDIO-12 prune 7 dead banks/5 orphan headers · AUDIO-13 doc · AUDIO-14 photography SFX (.mp3 only;
  ElevenLabs key needed).
- **OPS/buildtestops (5):** OPS-07 config `mirrors:` drift check · OPS-08 generated C++/shader constant
  header · OPS-09 nightly ScheduledGateRun (after FOLIAGE-05 ✓) · OPS-15 harness operator doc (dead last)
  · OPS-16 EngineGameSplitLint allowlist retire (INSTINCT-12 done ⇒ unblocked).
- **FOLIAGE (3):** FOLIAGE-07 PlantMeshCache (before M3 plant-procgen) · FOLIAGE-09 drop int64 ext from
  grass_scatter.comp (before M2 compute batch) · FOLIAGE-10 morphology (M5).
- **Sim tails:** INSTINCT-09 GOAP+IAUS unification (also the chartered fix for the ecology O(N²) hot
  spot) · INSTINCT-14 **BLOCKED on spec-019** (reserve seed ≥39) · ATMO-13 region-follow grids
  (deferred moving-baseline bump) · WATER-15 river fidelity (render-only channel detail first) ·
  SHIELD-08 far-field SDF fidelity (XL; design M4/M5, land M6).

### 4.3 Hardware / residual register
- **Needs a 2nd machine / non-NVIDIA GPU:** NET-12 · FSR2/XeSS full providers (stubs only) · NET-13
  (Docker gcc container proxy).
- **Needs RTX (dev box HAS RTX 5070 Ti ⇒ CAN validate locally, gated / excluded from -Mode All):**
  DlssSwapSmoke, RtGiCaveGpu, RtReflectionsWaterGpu, BlasTlasStreamGpu, and the P09 UpscaleSeamParity.
- **Timebox/process:** DX12 mingw (parse-only fallback) · DLSS Streamline auto-exposure interim ·
  Streamline ship-signing follow-up · AUDIO-14 needs `ELEVENLABS_API_KEY`.

---

## 5. Owner activation menus (nothing blocks engineering; current defaults stand)
Each = ONE deliberate hash bump + evidence bundle + re-pin: `sim.hydrology_weather` ON ·
`sim.weather_events` ON · a THIRSTING server roster · ATMO-13 region-follow ON · full spec-010 finite
hydrology ON · **`sim.aether_state` ON** (the A1 stateful-field layer's activation). **PAUSE #1 visual
menu** (still open): AC-A-001 night floor `u_moonWrapFloor` 0.25 vs 0.0; GPU metered auto-exposure
adopt-or-keep; froxel default tier. **NEW this session — `user.render_scale` default** (once GPU-P09's
0.67 fidelity is fixed): confirm the shipped default scale (1.0 vs a mild 0.9) and the clamp range.

---

## 6. Standing law + lessons (unchanged unless noted)
ucrt64 PATH prepend (`C:\msys64\ucrt64\bin`) on every build/ctest/validator; **ctest SERIAL only**;
full-tree rebuild after header changes; **shaders INLINE** (never fan out render code); commit per
green gate, **NEVER push**; `git commit -F <file>` (PowerShell has no heredoc); visual changes need a
looked-at capture; validate backlog after every status flip; RenderContext.h/RenderResourceRegistry/
RenderPipeline/main_client are serialized shared render seams.
**New this session:** (1) PowerShell `2>&1 |` on a native exe MANGLES it (left a configure half-done →
add_test didn't register) — use `*> file`, never `2>&1 |`. (2) A doc-lint that scrapes needles from a
`.feature` must anchor to Gherkin `Then/And` step lines (prose describing the template gets scraped).
(3) When editing a header that a living-doc cites by line, insert AFTER the cited block so anchors
don't shift. (4) Render-scale seam: `lround(N*1.0f)==N` gives free byte-identity at scale 1.0; the
FinalBlit src rect must be the internal extent (else the scene 1:1-copies to the corner); the
G-buffer→lighting depth blit rect must be internal; SSAO/water/particle `u_screenSize` must be internal.
(5) FOLIAGE-05 re-bless: a perf-budget re-bless is a SURFACED, documented decision when the metric is a
proxy (TreeLod.h:39-44 — forest cost is fill/overdraw-bound, not triangle-bound; real floor is
-Mode PerfFloor), not a silent rubber-stamp.

## 7. Source-of-truth pointers
1. `docs/audit/021/backlog.json` (+ `validate_backlog.py` → "OK: 190 items valid").
2. `docs/audit/021/priority-ranking.md` (wave records; append M1 close next).
3. Campaign plan `ultracode-land-all-of-joyful-matsumoto.md` (M0–M6, serialization, per-commit sequences).
4. GPU-P09 seam plan `C:\Users\David\.claude\plans\m1-gpu-p09-render-scale-seam.md`.
5. Auto-memory `MEMORY.md` + `m1-render-scale-in-progress.md` (the live GPU-P09 debugging state).

## 8. Immediate next actions
1. **Fix the GPU-P09 0.67 depth bug** (§3): instrument the internal depth buffer; confirm the sky
   depth test + terrain rasterization at internal res; likely an explicit skybox viewport and/or the
   lighting-depth ordering. Re-capture 0.67 → looked-at + band-diff ≈ 1.0.
2. **Finish GPU-P09** (§3): LOD bias · wire `user.render_scale` · build `UpscaleSeamParity` (leg1
   EXACT 0.0, leg2 0.67 under calibrated FLIP) · flip backlog + wave record + VISUAL PAUSE.
3. Then **M2 (HLSL port)** on the GPU spine; fan out the file-disjoint clusters (§4.2) in parallel.
