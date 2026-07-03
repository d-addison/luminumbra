# Pillar audit: Render — deferred GL 4.5 pipeline, SHIELD-RT, 016 frame-graph seam, 015 lighting (Spec 021, 2026-07-02)

The render layer is structurally much further along than the 2026-06-28 roadmap snapshot assumed —
spec 016's pass contract is COMPLETE (the `RenderPipeline` friend list is empty, every pass executes
against `RenderContext`, FR-D shader reflection is landed and tested) and spec 017-A's async
readback ring is landed with the FR-G-001 ban gate enforcing it — but the pillar is held hostage by
one defect: the headless IN_GAME render-capture hang (`main_client.cpp:4349` doline-scan block,
charter FR-A-003), which blocks every headless scene/shader visual verification, the 015 Pillar-A
re-bless, AND (newly established here) the 016 in-process parity harness itself, because
`--render-parity-*` reuses the frame-scan IN_GAME boot (`main_client.cpp:2765-2790`). Remaining 016
work (FR-C declarative graph, FR-B resource ownership, FR-F decomposition) and the 015 tail
(A-T06/A-T07, C-1, B, C-2) are well-scoped and mostly dependency-clean; one new un-gated synchronous
readback was found escaping the FR-G-001 gate (`SkyAtmosphereLut.ipp:677`, `.ipp` not in the gate's
extension filter).

## Current state + evidence

### The pipeline core (the 016 god-object, mid-decomposition)

- `src/luminumbra_client/rendering/RenderPipeline.cpp` is **5,311 lines** and `RenderPipeline.h`
  **1,428 lines** (measured 2026-07-02; the 016 spec measured 4,751/1,464 at creation —
  `docs/specs/016-render-framework-frame-graph/spec.md:22` — so the .cpp has grown ~560 lines even
  as coupling shrank; state and policy still live on the pipeline).
- Frame order is still a **manually scripted call sequence** inside `render_frame`
  (`src/luminumbra_client/rendering/RenderPipeline.cpp:2034`): shadow `:2155` → gbuffer `:2186` →
  plant-procgen `:2215` → ssao `:2291-2296` → lighting `:2310` → skybox `:2341/:2363` → water
  `:2388` → weather overlay `:2478` → foliage `:2548` → particles `:2581` → lightning overlay
  `:2598` → final blit `:2628`. Spec 016 FR-C-001 (declarative graph,
  `docs/specs/016-render-framework-frame-graph/spec.md:110-117`) is NOT started — no
  FrameGraph/PassDescriptor type exists anywhere under `src/luminumbra_client/rendering/`.
- `update_time_of_day` is still a single multi-concern function
  (`src/luminumbra_client/rendering/RenderPipeline.cpp:4870-5138`, ~270 lines: sky LUT, sun, moon,
  ambient, exposure, cloud phase) — 016 FR-F-001 (`spec.md:140-146`) not started.

### Spec 016 pass contract — COMPLETE (further along than the roadmap snapshot)

- **AC-001 is achieved**: the `RenderPipeline` friend list is EMPTY —
  `src/luminumbra_client/rendering/RenderPipeline.h:730-738` records each removal (ShadowPass
  P2-T10, GBufferPass P2-T11, SsaoPass P2-T02, LightingPass P2-T12, WaterPass P3-T16, SkyboxPass
  P1-T04, ParticlePass/FoliagePass P3-T18/T17, PlantProcgenPass P3-T14) and states "the
  RenderPipeline friend list is EMPTY". No `passes/*.h` includes `RenderPipeline.h` and no pass
  `execute()` takes `RenderPipeline&` (grep-verified; e.g. `passes/LightingPass.h:36`,
  `passes/GBufferPass.h:59`, `passes/WaterPass.h:57`, `passes/ShadowPass.h:34`,
  `passes/SkyboxPass.h:36`, `passes/ParticlePass.h:240`, `passes/FoliagePass.h:209`,
  `passes/PlantProcgenPass.h:79`, `passes/SsaoPass.h:35-36`, `passes/FinalBlitPass.h:30` — all take
  `const RenderContext&`). Two minor passes take raw params rather than the context
  (`passes/GroundDecalPass.h:38` takes a `GLuint`; `passes/DebugViewPass.h:52` takes `GBuffer&`) but
  neither couples to the pipeline.
- The seam itself: `RenderContext` with additive field groups A-N
  (`src/luminumbra_client/rendering/RenderContext.h:33-163`), typed handles
  (`RenderResourceHandles.h:22-45` — TextureHandle/FboHandle/BufferHandle, FboHandle distinguishes
  "default framebuffer 0" from "unset"), and the registry
  (`RenderResourceRegistry.h:25-63`). The registry is **adopt-mode only** today — it wraps
  externally-owned GL names per frame; ownership/lifetime/load-store/history-buffers (FR-B-001/003,
  `spec.md:96-106`) have not moved off the pipeline (`RenderResourceRegistry.h:13-17` says so
  explicitly).
- **FR-D shader reflection LANDED** (commit `6bb83b17`, 2026-06-28):
  `src/luminumbra_client/rendering/ShaderReflection.h:9-28` (program introspection + pass-declared
  ExpectedLayout validation; hard-fails type/binding mismatches, soft-warns linker-stripped
  samplers, `ShaderReflection.h:93-99`), wired into `Shader::ValidateLayout`
  (`src/luminumbra_client/rendering/Shader.cpp:97-127`) including the hot-reload rollback that
  validates the candidate layout before swap (`Shader.cpp:125-127`, FR-D-003). 9/9 tests in
  `test/rendering/shader_reflection_test.cpp:138-228` (ctest target `shader_reflection_test`,
  `test/CMakeLists.txt:355`). Adoption is only started: **one** pass declares an ExpectedLayout so
  far (`passes/LightingPass.cpp:37`).
- Per-pass conversion proof harness: in-process same-frame A/B parity captures
  `capture_finalblit_parity` / `capture_ssao_parity` (`RenderPipeline.h:503/:509`) driven by
  `--render-parity-finalblit` / `--render-parity-ssao` (`main_client.cpp:2765-2790`) and diffed with
  `tools/flip_diff.py` (tiered FLIP/SSIM/luma backends, `tools/flip_diff.py:1-31`). NOTE: both
  parity flags set `g_frame_scan_active = true` and boot the auto-world to IN_GAME
  (`main_client.cpp:2770/:2784`) — so this harness rides the same IN_GAME capture path that
  currently hangs (see the blocker below).

### Spec 017-A async readback ring — LANDED (shipped since the roadmap)

- `src/luminumbra_client/rendering/AsyncReadbackRing.h:38-112`: backend-agnostic
  submit/poll/consume, no GL names in caller-visible signatures (`AsyncReadbackRing.h:17-21`, the
  RHI-shaped seam 014 backs later); fenced persistent-mapped SSBO slots, stale-safe consume.
  Implementation polls with `glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0)` — zero
  timeout, flush bit so fences signal on offscreen renders with no SwapBuffers
  (`AsyncReadbackRing.cpp:88-95, :150-155` — the documented gotcha, verified in-tree).
  5 GL unit tests: `test/rendering/async_readback_ring_test.cpp:76-259` (ctest target
  `async_readback_ring_test`, `test/CMakeLists.txt:320`).
- The FoliagePass blocking `glGetBufferSubData` gate readback is **rerouted onto the ring**
  (`passes/FoliagePass.cpp:814-834` submit path, `:861` consume; ring member
  `passes/FoliagePass.h:278`), freed under a live GL context (`FoliagePass.cpp:264`, commit
  `3bba2a52`).
- **FR-G-001 render-side readback ban gate** exists and is enforced:
  `Test-RenderReadbackAllowlist` (`.forge/scripts/validate-engine-frontier.ps1:7026-7076`, mode
  `RenderReadbackAllowlist` at `:7306`) scans `src/luminumbra_client/rendering` with narrow
  blocking-form matchers (`:7036-7038`: `glClientWaitSync(...GL_TIMEOUT_IGNORED`,
  `glMapBuffer(...GL_READ_ONLY`, `glGet[Named]BufferSubData`), deliberately NOT the broad sim-path
  regex. Exactly one allowlisted site remains: the GPU-SDF compute readback
  (`validate-engine-frontier.ps1:7031-7032`), to be retired after 017-B.
- The remaining synchronous site it allowlists: `RenderPipeline.cpp:4816`
  (`glClientWaitSync(m_gpu_sdf.compute_fence, ..., GL_TIMEOUT_IGNORED)`) + `:4823`
  (`glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_READ_ONLY)`) — the 016 FR-E / 017 FR-A-003 SDF
  readback, correctly deferred behind 017-B because the SDF is sim truth (the prior handoff cited
  `~:4321/:4328`; the file has grown — current lines are 4816/4823).
- **NEW FINDING — a synchronous readback ESCAPES the gate**:
  `src/luminumbra_client/rendering/SkyAtmosphereLut.ipp:677` does a blocking `glGetBufferSubData`
  (sky-view SSBO → CPU ambient reduction) on the `render.sky_lut_gpu` refresh path
  (`SkyAtmosphereLut.ipp:760-765`; the comment at `:672-674` concedes "the readback stall is
  acceptable ... an optimization target if ever enabled on release"). It is invisible to FR-G-001
  because the gate's file filter scans only `.h/.hpp/.cpp/.inl/.c`
  (`validate-engine-frontier.ps1:7046`) — `.ipp` is excluded. Default-OFF path, but the gate
  coverage hole is real.

### Spec 015 atmospheric lighting — Pillar A core landed; the tail is scoped

- **Moon radiance channel (Codex C5) landed** (commit `3aa9740d`): `m_moonRadiance`
  (`RenderPipeline.h:922-925`, C++-tunable) → `RenderContext.moon_radiance`
  (`RenderContext.h:112`, default == prior shader const) → `u_moonRadiance`
  (`passes/LightingPass.cpp:176`; consumed `res/shaders/lighting_pass.frag:80, :473-491` for moon
  diffuse + specular).
- **A-T04 lunar phase** landed: `RenderContext.moon_illumination` (`RenderContext.h:111`) + the
  scene-config `moon` knob for night captures (`main_client.cpp:258-260`).
- **A-T05 deterministic time-of-day exposure** landed: pure-function eye-adaptation curve
  (day 1.12 == prior static grade / night 1.75 / golden 1.02,
  `RenderPipeline.cpp:5113-5133`) feeding the exposure seam `RenderContext.exposure`
  (`RenderContext.h:150-155`, sentinel-0 falls back to static LUMIN_GRADE —
  `passes/LightingPass.cpp:194-196`). Render-only by construction; guarded by the DeterminismAudit
  gate (`validate-engine-frontier.ps1:7078+`, checks exposure/froxel stay render-only, `:7225`).
- **A-T06 GPU auto-exposure**: NOT started. Its two spine prerequisites have BOTH landed since the
  roadmap — 017-A (above) and the 018-E/F readback-discipline + determinism-audit gates
  (`Test-ReadbackDiscipline` `validate-engine-frontier.ps1:6983`, `Test-DeterminismAudit` `:7078`,
  commit `8c0dbf9d`) — so it is mechanically unblocked; only the capture-hang blocks its re-bless.
- **A-T07 photo manual EV**: metadata-only, exactly as suspected. Shutter/ISO/aperture nudges and
  clamps exist (`main_client.cpp:7261-7281`), the viewfinder shows a live EV meter computed from
  the lens (`main_client.cpp:7334`, elements `ro_shutter`/`ro_iso` in `data/ui/photo_mode.rml:32-41`),
  but **nothing writes the exposure seam** — grep confirms no photo-mode override of
  `m_pillarA_exposure`/`RenderContext.exposure`. (Discrepancy vs the brief: the EV code is at
  `main_client.cpp:7334`, not `~:7121` — `:7121` is codex-UI code.)
- **C-1 colored shadow maps**: not started (no colored-shadow attachment/tint symbol anywhere in
  `src/` — grep-verified). Per `docs/specs/015-atmospheric-lighting-colored-glass/spec.md:281-283`
  it must be built through 016's pass/resource contract; per `:283-285` Pillars B and C-2 are gated
  behind 016 AND the 014 RHI pilot.
- Moon calibration + true-midnight re-bless remain blocked on the capture hang
  (`docs/HANDOFF-2026-06-28-FINAL-land-everything.md:54-61`).

### SHIELD-RT software far-field raymarch — shipping, GPU-tested

- `src/luminumbra_client/rendering/passes/ShieldRtFarFieldPass.h:3-22`: camera-centered heightfield
  SSBO assembled from FarLod F1 tiles, GPU-built max-mip pyramid, overshoot-free hierarchical-DDA
  fullscreen raymarch writing the deferred G-buffer + `gl_FragDepth`; async heightfield rebuild on a
  JobSystem worker (`ShieldRtFarFieldPass.h:60-71`); scene-depth copy for the far-pixel early-out
  (`:73-78`). Gated compile-time + `--enable-far-field-gpu-raymarch` (`:17-18`). Proven by named
  ctests `ShieldRtFarFieldParityGpu` / `ShieldRtFarFieldGbufferGpu` / `ShieldRtFarFieldMaxMipGpu`
  (`test/CMakeLists.txt:995/:1002/:1009`). This is the software-RT asset the charter (OQ-3) keeps
  alongside future HW-RT — preserved, not replaced (GPU pillar owns the HW-RT track).

### Upscaling/TAA foundation (the DLSS seam — GPU pillar consumes it)

- `res/shaders/taau_resolve.frag:1-7`: TAA resolve + upscale hook, history AABB-clamp
  anti-ghosting; flag-gated `render.taau`, **default OFF → byte-identical**
  (`RenderPipeline.h:993`, `main_client.cpp:3101`). Halton[2,3] sub-pixel jitter computed in the
  pipeline (`RenderPipeline.cpp:2105-2117`); RG16F motion vectors at COLOR_ATTACHMENT4
  (`rendering/GBuffer.h:18-24`).

### Capture / verification tooling (render-owned)

- `--frame-scan` deterministic what's-in-frame report (`main_client.cpp:261-282`), self-implies the
  auto-world (`:2754-2763`) with a 4000-render-loop-frame watchdog (`:277-282`, checked `:6819-6825`).
- FrameHealth anomaly verdict with `"nominal"` rollup (`rendering/FrameHealth.cpp:190`,
  `FrameHealth.h:50-53`).
- Gates in `.forge/scripts/validate-engine-frontier.ps1`: `WorldVisualSweep` (`:4017`, TOD × angle ×
  weather × season presence/production matrix over `--scenario world_visual_sweep`), `RenderBudget`
  (`:6442`, runs `--render-benchmark` fixed-scenario per-pass GPU timings), `ReadbackDiscipline`
  (`:6983`), `RenderReadbackAllowlist` (`:7026`), `DeterminismAudit` (`:7078`).
- Per-run full-frame FLIP is not deterministic run-to-run (~0.057 noise, TAAU off) — `flip_diff` is
  the in-process same-frame tool; the gates use frame-health `nominal` + presence checks instead
  (consistent with `tools/flip_diff.py:8-16` positioning it as the GL↔Vulkan same-frame parity
  harness).

### THE BLOCKER (charter FR-A-003, mandated first-class): headless IN_GAME render-capture hang

- Symptom (4 confirmed runs, noon AND night): `--scene-config` (`main_client.cpp:2702`) and
  `--frame-scan` (`:2754`) load the world, render ONE ~2.4 s frame, start frame 2, log the spec-013
  doline scan — the block anchored at `main_client.cpp:4349-4362` (`FindLargestSurfaceBreak` at
  `:4354` is the last logged site) — then the main thread blocks dead 5+ minutes with zero further
  frames (`docs/HANDOFF-2026-06-28-FINAL-land-everything.md:29-39`; render thread 0.0 s vs ~2.4 s
  on the others). The `--ui-screenshot --preview-live` path (offscreen preview backbuffer) works.
- Current stopgap state: (a) the frame-scan watchdog (`main_client.cpp:281, :6819-6825`) counts
  render-loop iterations, so a mid-frame main-thread block DEFEATS it — it aborts wedge-in-menu
  loops, not this hang; (b) visual re-bless falls back to INTERACTIVE owner runs
  (`HANDOFF...:38-39`); (c) diagnosis tools staged: `LUMINUMBRA_JOB_WATCHDOG`, crash breadcrumbs,
  TOD bisect, or routing scene capture through an offscreen FBO like the working preview path
  (`HANDOFF...:36-39`).
- Blast radius (this audit's addition): beyond Pillar-A/015 re-bless and all `--scene-config` /
  `--frame-scan` shader verification, the 016 **in-process parity harness is also exposed** —
  `--render-parity-finalblit`/`--render-parity-ssao` set `g_frame_scan_active` and boot the same
  IN_GAME path (`main_client.cpp:2765-2790`), so future FR-C/FR-B conversion evidence is hostage to
  this fix too. Fixing it is the top-ranked render item (RENDER-01).

### Shipped since the 2026-06-28 roadmap

Verified via `git log`:

- **017-A AsyncReadbackRing + FR-G-001 render readback ban gate + FoliagePass reroute** — commits
  `ca2616d8` (2026-06-29) + `3bba2a52` (2026-06-29, ring freed under a live GL context). The
  roadmap scheduled 017-A in Wave 1; it is DONE.
- **015 Pillar A dedicated moon radiance channel** — `3aa9740d` (2026-06-28 18:15), byte-identical
  default.
- **016 FR-D shader reflection + layout validation** — `6bb83b17` (2026-06-28 15:28), 9/9 tests;
  removes one of the three 014-pilot gate legs (016 seam ✓ + FR-D ✓ + resource registry ✗).
- **Create-world preview render work** — far-field anchored to the diorama centre (`e79c1312`),
  `--preview-live` headless live-diorama capture (`860230b1`), full-SDF near-field preview LOD
  (`a21e088e`) — all 2026-06-28 (UI pillar owns the create-world punch list; listed because they are
  render-path changes).
- **DISCREPANCY vs the audit brief**: the create-world lake-preview null-water crash fix
  (`e1fee9ff`) is dated **2026-06-25 23:54** — it PRE-dates the 2026-06-28 roadmap rather than
  shipping after it.

## Gaps / debt

1. **The IN_GAME capture hang** (above) — every visual proving-signal in this pillar routes around
   it or waits on it; root cause unknown (candidates after the doline scan on the frame-2
   main-thread path, `main_client.cpp:4346-4362`).
2. **016 FR-C declarative frame graph not started** — the scripted `render_frame` sequence
   (`RenderPipeline.cpp:2034-2628`) still encodes ordering exceptions as comments/inline hacks;
   metadata is telemetry, not a contract (`spec.md:110-117`).
3. **016 FR-B resource ownership not migrated** — registry is adopt-only
   (`RenderResourceRegistry.h:13-21`); targets, lifetime, load/store, history buffers still
   pipeline-owned. This is the unfinished leg of the 014-pilot gate (016 seam + FR-D + registry).
4. **016 FR-E SDF readback still synchronous** (`RenderPipeline.cpp:4816/:4823`), correctly
   allowlisted (`validate-engine-frontier.ps1:7031-7032`) and correctly gated on 017-B (the SDF is
   sim truth; SHIELD owns 017-B).
5. **FR-G-001 gate coverage hole**: `.ipp` files escape the render readback scan
   (`validate-engine-frontier.ps1:7046`), hiding the blocking `glGetBufferSubData` at
   `SkyAtmosphereLut.ipp:677` (default-OFF `render.sky_lut_gpu` path).
6. **FR-D adoption is 1-of-N passes** — only LightingPass declares an ExpectedLayout
   (`passes/LightingPass.cpp:37`); the "renders garbage" tripwire protects one pass.
7. **016 FR-F**: `update_time_of_day` monolith (`RenderPipeline.cpp:4870-5138`); the .cpp grew to
   5,311 lines (AC-008 wants it dropping).
8. **015 tail**: A-T06 unstarted (now mechanically unblocked), A-T07 metadata-only
   (`main_client.cpp:7334`), C-1/B/C-2 unstarted, moon calibration + true-midnight re-bless pending.
9. **Visual-gate baselines stale** pending re-bless — blocked by (1)
   (`docs/HANDOFF-2026-06-28-FINAL-land-everything.md:29-39`).
10. Minor: GroundDecalPass/DebugViewPass take raw GL params instead of the RenderContext
    (`passes/GroundDecalPass.h:38`, `passes/DebugViewPass.h:52`) — cosmetic seam inconsistency, no
    pipeline coupling.

## Risks

- **The hang may be a player-facing engine defect**, not just a harness problem — a first/second
  IN_GAME-frame main-thread block after world-load is the same code interactive players run; the
  handoff's night `--scene-config` client also hangs on exit (`HANDOFF...:94`). Treating it as
  "capture tooling debt" underprices it.
- **014 pilot sequencing risk**: with FR-D done, pressure to start the 014 RHI pilot rises — but the
  registry-ownership leg (FR-B-003) is incomplete, which is precisely the "discover implicit GL
  state late" failure mode 016 exists to prevent (`spec.md:40-44`). The spine's gate (016 seam +
  FR-D + resource registry → 014 pilot) must hold.
- **Re-bless storm**: Pillar-A intensity/exposure changes move every lit-frame golden
  (`docs/specs/015-atmospheric-lighting-colored-glass/spec.md:317`); with per-run FLIP
  non-deterministic (~0.057 noise), only in-process A/B pairs + frame-health `nominal` +
  WorldVisualSweep presence checks can gate — and the first two are hostage to the hang.
- **Determinism risk of the readback consumers** (A-T06 metering, FR-E SDF): guarded by landed
  gates (`ReadbackDiscipline` `:6983`, `DeterminismAudit` `:7078`, `RenderReadbackAllowlist`
  `:7026`) — the risk is controlled as long as the allowlist shrinks rather than grows.
- **Pillar B/C-2 cost risk**: froxel volumetrics + OIT at 3840×1600 against a 300 fps target with a
  BF4-floor fidelity mandate; `RenderBudget` (`:6442`) is the only quantitative backstop. The
  render-opt defaults (half-res clouds + half-res GTAO) are already ON, owner-blessed 2026-06-20
  (`main_client.cpp:3068-3077`; `ssao_quality = 3` at `:3076`) — `LUMIN_SSAO_QUALITY` /
  `LUMIN_CLOUD_QUALITY` are legacy full-res fallback overrides for A/B, not opt-ins — so there is
  no cheap AO/cloud headroom left to reclaim before B/C-2 land.

## Opportunities

- **Fix the hang by rerouting capture through the working offscreen path**: `--preview-live`
  captures a live FBO diorama headlessly today (`860230b1`; `RenderContext` offscreen fields
  `RenderContext.h:43-49`) — routing `--scene-config`/`--frame-scan` through an offscreen FBO both
  bisects the defect (backbuffer/swapchain vs render-content) and, if it works, un-blocks re-bless
  immediately even before the root cause is found.
- **A-T06 is now the cheapest Pillar-A win**: both spine prerequisites landed; the ring's
  offscreen-safe poll (`AsyncReadbackRing.cpp:150-155`) was built for exactly this consumer.
- **A-T07 is a small, high-value photography-loop feature**: the EV math and UI already exist
  (`main_client.cpp:7334`); wiring EV → the exposure seam (`RenderContext.h:150-155`) is plumbing.
- **One-line gate hardening**: add `.ipp` to the FR-G-001 extension filter
  (`validate-engine-frontier.ps1:7046`) and either reroute or allowlist-with-reason
  `SkyAtmosphereLut.ipp:677`.
- **Half-res GTAO re-validation (already default-ON)**: half-res GTAO (mode 3 on the selector,
  `RenderContext.h:65-66`) has been the compiled-in DEFAULT since commit `39c06b3d` (2026-06-20)
  — `main_client.cpp:3076` sets `ssao_quality = 3` and `LUMIN_SSAO_QUALITY` is the legacy full-res
  fallback override, not an opt-in. The remaining opportunity is at most a `RenderBudget`
  re-validation at native 3840×1600, since the original AO-budget bless predates the Pillar-A
  lighting changes.
- **FR-D fan-out is mechanical**: replicating LightingPass's ExpectedLayout declaration across the
  other passes is additive, per-pass, and parallelizable — cheap tripwire coverage for every
  future shader change (and the substrate 014's SPIRV-Cross reflection plugs into,
  `ShaderReflection.h:21-24`).

## Backlog items

| id | summary | spec | effort | risk | deps | status | proving_signal |
|----|---------|------|--------|------|------|--------|----------------|
| RENDER-01 | Root-cause and fix the headless IN_GAME render-capture hang (frame-2 main-thread block after the doline scan, `main_client.cpp:4349`) that blocks all headless scene/shader verification, the 015 re-bless, and the 016 parity harness | new | M | medium | [] | todo | NEW: HeadlessInGameCapture gate — `--frame-scan` (noon) and `--scene-config` (night) reach report emission with frame-health verdict `nominal` inside the 4000-frame watchdog and exit 0 |
| RENDER-02 | 017-A AsyncReadbackRing (submit/poll/consume, fenced persistent-mapped SSBO, offscreen-safe flush-bit poll) + FR-G-001 render readback ban gate + FoliagePass blade readback rerouted onto the ring | 017-A | M | low | [] | done | ctest `async_readback_ring_test` (AsyncReadbackRing.*, 5 GL tests) + `validate-engine-frontier.ps1 -Mode RenderReadbackAllowlist` |
| RENDER-03 | 016 pass contract complete: RenderPipeline friend list EMPTY, all passes execute against RenderContext + typed handles (AC-001) | 016 | L | low | [] | done | in-process A/B parity via `--render-parity-finalblit` / `--render-parity-ssao` diffed with tools/flip_diff.py + `validate-engine-frontier.ps1 -Mode WorldVisualSweep` |
| RENDER-04 | 016 FR-D shader-resource reflection + layout validation + hot-reload rollback landed (Shader::ValidateLayout; candidate-layout check before swap) | 016 | M | low | [] | done | ctest `shader_reflection_test` (ReflectionFixture, 9 tests) |
| RENDER-05 | 015 Pillar A core landed: dedicated moon radiance channel (m_moonRadiance→u_moonRadiance), A-T04 lunar phase + scene-config moon knob, A-T05 deterministic TOD exposure via the RenderContext exposure seam | 015 | M | low | [] | done | `validate-engine-frontier.ps1 -Mode DeterminismAudit` (exposure render-only) + WorldVisualSweep rerun for the pending re-bless (see RENDER-08) |
| RENDER-06 | 016 FR-E: retire the GPU-SDF synchronous readback (`RenderPipeline.cpp:4816/:4823`) onto the 017-A ring and remove the last FR-G-001 allowlist entry | 016 | M | medium | ["017-B"] | todo | `validate-engine-frontier.ps1 -Mode RenderReadbackAllowlist` green with the RenderPipeline.cpp allowlist entry REMOVED + `--smoke` == 6f008a9f637c40b7 run==replay |
| RENDER-07 | 015 A-T06: GPU auto-exposure metering through the AsyncReadbackRing replacing the A-T05 analytic curve (both spine prerequisites 017-A and 018-E/F have landed) | 015 | M | medium | ["017-A", "018-E/F", "RENDER-01"] | todo | NEW: ExposureMeter ctest — ring-metered exposure converges on a known-luminance offscreen frame without blocking; plus `validate-engine-frontier.ps1 -Mode DeterminismAudit` + WorldVisualSweep rerun |
| RENDER-08 | 015 Pillar A re-bless: moon radiance calibration (taste pass on 0.40/0.52/0.92) + true-midnight TOD 0.5 night captures + refresh the stale visual-gate baselines | 015 | S | low | ["RENDER-01"] | todo | WorldVisualSweep rerun (night cells) + `--scene-config` night/midnight captures with frame-health `nominal`, goldens re-blessed via tools/flip_diff.py heatmap review |
| RENDER-09 | 015 A-T07: wire photo-mode manual EV (shutter/ISO/aperture already computed at `main_client.cpp:7334`) into the RenderContext exposure seam override | 015 | S | low | [] | todo | NEW: PhotoManualEv test — manual EV override produces a flip_diff-measurable exposure change on an in-process capture pair at fixed TOD, while `--smoke` stays 6f008a9f637c40b7 |
| RENDER-10 | Close the FR-G-001 coverage hole: add `.ipp` to the render readback scan and reroute or allowlist-with-reason the blocking `glGetBufferSubData` at `SkyAtmosphereLut.ipp:677` | 017-A | S | low | [] | todo | `validate-engine-frontier.ps1 -Mode RenderReadbackAllowlist` extended to `.ipp` — run flags SkyAtmosphereLut.ipp until the site is rerouted onto the ring or documented |
| RENDER-11 | 016 FR-C: declarative frame graph / pass-descriptor list replacing the scripted `render_frame` sequence; ordering exceptions become explicit edges | 016 | L | medium | ["RENDER-03", "RENDER-01"] | todo | in-process A/B parity via `--render-parity-finalblit`/`--render-parity-ssao` (tools/flip_diff.py) + `validate-engine-frontier.ps1 -Mode RenderBudget` (no perf regression) |
| RENDER-12 | 016 FR-B: migrate render-target ownership (G-buffer, shadow atlas, ssao, lighting accum, TAAU history) into the resource registry with lifetime/load-store/history semantics — the unfinished leg of the 014-pilot gate | 016 | L | medium | ["RENDER-01"] | todo | in-process A/B parity (tools/flip_diff.py on `--render-parity-*` pairs) + `validate-engine-frontier.ps1 -Mode RenderBudget` + WorldVisualSweep rerun |
| RENDER-13 | 016 FR-D adoption: declare ExpectedLayout for every remaining pass (only LightingPass validates today) so the type/binding tripwire covers the whole pipeline | 016 | M | low | ["RENDER-04"] | todo | NEW: startup layout-validation gate — every pass shader registers an ExpectedLayout and Shader::ValidateLayout returns ok at init (extend `shader_reflection_test` with per-pass fixtures) |
| RENDER-14 | 016 FR-F: decompose `update_time_of_day` (`RenderPipeline.cpp:4870-5138`) and shrink RenderPipeline toward assembler+policy (AC-008 line-count drop) | 016 | M | low | ["RENDER-11"] | todo | in-process A/B parity (tools/flip_diff.py) byte-identical intent + `--smoke` == 6f008a9f637c40b7 unchanged |
| RENDER-15 | 015 C-1: colored shadow maps (tinted-transmission attachment) built through the 016 pass/resource contract — the "light is the subject" quick win leading Pillar C | 015 | L | medium | ["RENDER-12", "RENDER-01"] | todo | WorldVisualSweep rerun + tools/flip_diff.py heatmap review (intentional tint change) + `validate-engine-frontier.ps1 -Mode DeterminismAudit` |
| RENDER-16 | Re-validate the already-default-ON half-res GTAO (`ssao_quality = 3`, default since `39c06b3d` 2026-06-20; `LUMIN_SSAO_QUALITY` is the legacy full-res fallback override, `main_client.cpp:3076`) under RenderBudget at native 3840x1600 — the original AO-budget bless predates the Pillar-A lighting changes | new | S | low | [] | todo | `validate-engine-frontier.ps1 -Mode RenderBudget` (per-pass GPU timings within budget on the fixed pose) + WorldVisualSweep rerun |
| RENDER-17 | 015 Pillar B: froxel volumetric participating media (extend the analytic aerial; C-1 tint compose for colored shafts) — gated behind 016 and the 014 RHI pilot | 015 | XL | high | ["RENDER-11", "RENDER-12", "014", "RENDER-15"] | todo | WorldVisualSweep rerun + `validate-engine-frontier.ps1 -Mode RenderBudget` + `-Mode DeterminismAudit` (froxel render-only) |
| RENDER-18 | 015 C-2: full OIT colored glass + screen-space refraction — lands last, gated behind 016 and the 014 RHI pilot | 015 | XL | high | ["RENDER-17", "014"] | todo | WorldVisualSweep rerun + `validate-engine-frontier.ps1 -Mode RenderBudget` (OIT variant decided by measurement per 015 OQ) |
