# Engine Frontier Handoff

## Current Status

The research and adversarial critique have been reconciled into a gate-first roadmap and dispatch graph. The immediate blocker remains material appearance: sand versus grey fallback and material/texture-layer agreement are not yet gated.

The current debug CTest lane was run during handoff authoring with:

```powershell
ctest --preset debug --output-on-failure -E "_NOT_BUILT$"
```

It passed 58/58 tests, including `WorldGenLayerSnapshotTest.ExportsLayerMetricsAndImages`, `WorldGenLayerSnapshotTest.LodRemeshKeepsPreviousMeshRenderableWhilePending`, and `RuntimeWorldVisualValidationTest.MixedLodBoundariesAreContinuousAndReported`. No current Wave 1 repair task is required for failing CTests.

## Immediate Next Step

Execute Wave 1 through the approval-gated dispatch: start with `T-EF-1-material-visual-gate`, then run `T-EF-2-material-visual-gate-test`, then preserve the full CTest baseline. The dispatch is Codex-only and must not route work to any other AI agent.

Contract execution meters against a $30/day budget. Large waves, long runtime gates, and `Endurance300` may need to span days rather than being forced into one budget window.

## No-Deferral Rules

- Do not defer sand rendering as flat grey, invalid material IDs, wrong texture layers, missing material heatmaps, or missing visual screenshots.
- Do not accept draw counts as proof of final visual appearance.
- Do not start render extraction before `RenderHealth` exists and passes with `MaterialVisual`, `LodGround`, and `WaterVisual`.
- Do not start scheduler/LOD policy changes before streaming telemetry and boundary/seam gates exist.
- Do not enable persistence runtime paths, GPU SDF runtime integration, far-field SDF, Aetheric simulation, GOAP/Instinct planning, Lua hot reload, or networking unless the deterministic gate for that path exists and passes.
- Do not mark endurance closed until it runs after the visual gates and records the artifacts it depends on.
- Do not commit from task execution; integration is handled by the dispatch pipeline.

## Next Iteration Directives (owner, 2026-06-10)

Two additional work streams are mandated for the iteration after this dispatch
closes, both gate-first like everything else in this roadmap:

### Optimization pass

Goal: measured, regression-gated performance improvement — no optimization
lands without a baseline artifact proving the win and a gate preventing decay.

- Baseline first: capture frame-time P50/P95/P99, meshing throughput
  (cells/ms by step), upload drain rate, generation/meshing job latency, and
  peak memory across `auto_world_smoke 300`, `lod_ground_smoke`, and a
  camera-traversal scenario. Persist as
  `build/<preset>/test-artifacts/perf/perf-baseline.json` with schema and
  thresholds; add a `PerfRegression` validator mode that fails on >10%
  regression against the committed baseline.
- Candidate targets, in expected-leverage order: meshing hot path
  (MarchingCubes table walk and vertex cache), streaming drain and coalescing
  windows, RenderPipeline per-pass GPU timers (add timers first, optimize
  second), JobSystem priority lanes / work stealing, chunk SDF generation
  batching, water sim tick cost at distance.
- Every optimization task pairs with the gate run that proves end-state
  visuals unchanged (MaterialVisual, LodGround, WaterVisual stay green).

### Beautification pass

Goal: spend the renderer's existing-but-unwired feature set and tune the
world's look, with every visual claim backed by a screenshot-classification
or RenderHealth artifact.

- Wire and gate the dormant shader suite: volumetric_lighting,
  enhanced_skybox, weather_system, caustics_generator, magical_particles,
  screen_space_reflections. One feature per task; each adds a capture
  scenario plus pixel/structural assertions (e.g. god-ray luminance shafts
  present at dawn time-of-day; SSR reflections present on calm water ROI).
- Water beauty pass (explicitly deferred from Wave 1): depth-tint curve,
  caustics integration, SSR, shoreline foam; extends
  water-visual-analysis.json rather than replacing it.
- Terrain material richness: per-material texture layers validated by the
  material heatmap gate (extend the materials array beyond Sand: Grass,
  Stone, Soil ROI entries with their own thresholds).
- Atmosphere: time-of-day sweep capture (noon/dusk/night) with per-phase
  luminance and color-balance bands; LuminCrystal emission visible in night
  captures (ties to the Aetheric frontier stream).
- Sequencing: beautification tasks run AFTER RenderHealth exists and the
  optimization baseline is captured, so visual richness never silently buys
  frame-time regressions.

## Success Definition

The handoff is complete when the three file/graph gates pass:

```powershell
forge tasks validate .forge/tasks/engine-frontier/dispatch.json
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Files
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Sections
```

Feature success requires Wave 1 visual gates green first, all later frontier streams gate-first, and terminal `Endurance300` revalidation after the visual gates.
