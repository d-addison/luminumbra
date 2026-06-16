# Isolation / Layer render mode — SPEC (T-I6, owner-requested 2026-06-16)

> **POST-CRITIQUE REVISION (2026-06-16):** the 4-lens forge-critique
> (`.forge/critique-isolation-layer-spec-20260616.md`) found the render-side
> per-pass masking + transparent-alpha v1 below is BOTH over-engineered (risky
> deferred surgery: lighting overwrites the backdrop, fog smears, foliage/particle
> depth breaks, alpha doesn't survive the blit) AND under-scoped (no sim/studio
> context). **v1 is pivoted** to: (a) spawn-suppression in the scenario harness (don't
> create the non-selected content — orthogonal to the render pipeline, byte-stable),
> (b) a SkyboxPass flat-colour backdrop override (void/greenscreen/checker). Render-
> side pass masking, transparent RGBA, studio rig, sim debug-viz, and an interactive
> toggle are explicit v2 deferrals. The §Design/§TDD/§CI below are retained as the v2
> render-side reference; the critique report is the v1 plan of record. The pure
> `core/IsolationConfig.h` + parse unit test (`isolation_config_test`, in the default
> CI lane) is the landed foundation common to both.

## Goal
Blender-like **layer isolation**: render a selected subset of subsystems (terrain,
far-field, water, foliage, particles, skinned creatures, sim debug viz) ALONE
against a neutral **void / greenscreen / checker / transparent** backdrop — not
embedded in the full scene — for review, visual-critique, and debugging. Serves the
composability principle ([[engine-power-scalability-principle]]) and the visual-
fidelity work ([[visual-fidelity-target]]): each system judged in isolation at
native 3840x1600 ([[display-and-capture-resolution]]).

## Non-goals (v1)
- Not a full compositor / multi-layer EXR export (single isolated capture per run).
- No sim changes — RENDER-ONLY. Never touches world_hash / the determinism contract.
- Per-light / per-material isolation deferred; v1 isolates by PASS/subsystem.

## Design
`RenderPipeline` gains an `IsolationConfig { uint32 layer_mask; BackdropMode backdrop; }`,
default `{ALL, Scene}` (= today's behavior, byte-stable). Set from `RuntimeScenarioConfig`
via CLI `--isolate <csv>` (e.g. `foliage,particles`) + `--backdrop <mode>`.

**Layer mask bits** (a pass/draw renders only if its bit is set):
`Terrain` (gbuffer live chunks), `FarField` (gbuffer far-LOD mesh + the SHIELD-RT
pass), `Water`, `Foliage`, `Particles`, `Skinned` (creatures), `Skybox`, `Aerial`,
`Lightning`. `ALL` = every bit (no isolation).

**Backdrop modes** (fills the no-geometry / background pixels, replacing the skybox
when `Skybox` is masked out or backdrop != Scene):
- `Scene` — default skybox (no-op; byte-stable).
- `Void` — flat dark grey (0.02).
- `Greenscreen` — (0,1,0) for chroma-key compositing.
- `Checker` — magenta/grey checker (alignment/scale reference).
- `Transparent` — alpha 0 where no geometry (RGBA capture for external compositing).

**render_frame integration** (deferred): gate each pass's execute on its layer bit;
the GBufferPass gates its terrain-chunk vs far-LOD draws by `Terrain`/`FarField`.
The backdrop replaces the skybox: when backdrop != Scene, clear the lighting FBO
background to the backdrop colour (and skip the skybox pass) so no-geometry pixels
read the backdrop; lit selected geometry composites over it as normal.

**Transparent backdrop** needs an RGBA capture: the current capture path is RGB
(`WriteBackbufferPpm`/glReadPixels GL_RGB). v1 adds an RGBA PNG capture variant for
`--backdrop transparent`; the other backdrops work on the existing RGB path.

## TDD plan (tests FIRST / alongside)
1. **Unit — config parse** (pure, no GL; like `CaptureScale`): `ParseIsolationLayers(csv)`
   -> layer_mask; `ParseBackdropMode(str)` -> enum; round-trip + unknown-token handling.
   ctest `IsolationConfigParse`.
2. **Headless GL — isolation render** (`HiddenGlContext`, reuse `shieldrt_gl_harness`):
   render a synthetic scene (a known quad/geometry) with (a) a layer isolated +
   greenscreen backdrop -> assert background pixels == greenscreen and suppressed-
   layer pixels absent; (b) void/checker backdrops -> assert backdrop fill; (c)
   transparent -> assert alpha 0 where no geometry. ctest `IsolationRenderModeGpu`
   (manual;gpu, skips headless).
3. **Default byte-stability**: `{ALL, Scene}` leaves the existing render path
   unchanged — assert the full ctest suite + RenderHealth stay green (no regression).

## CI / testing integration
- New `isolation_config_test` (pure) added to `test/CMakeLists.txt` +
  `gtest_discover_tests` (runs in the default `ctest -LE manual` gate).
- `IsolationRenderModeGpu` registered manual;perf;gpu (like the SHIELD-RT benches).
- New `validate-engine-frontier.ps1 -Mode IsolationLayer`: drives a scenario with
  `--isolate`/`--backdrop`, captures, and asserts backdrop correctness + layer
  suppression via an objective check (background==backdrop colour ratio; suppressed-
  layer pixel count ~0). Added to the `ValidateSet` + the `All` sweep.
- Emits an `isolation-<layer>-<backdrop>.json` decision/QA artifact.

## Verification / exit
ctest green incl. the two new tests; default render byte-stable (RenderHealth +
WorldVisualSweep unaffected when isolation off); `-Mode IsolationLayer` green; a
demonstrated isolated capture (e.g. foliage on greenscreen, far-field on void) at
3840x1600. Then usable to review every system in isolation.
