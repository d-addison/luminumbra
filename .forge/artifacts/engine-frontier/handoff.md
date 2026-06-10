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

## Success Definition

The handoff is complete when the three file/graph gates pass:

```powershell
forge tasks validate .forge/tasks/engine-frontier/dispatch.json
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Files
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Sections
```

Feature success requires Wave 1 visual gates green first, all later frontier streams gate-first, and terminal `Endurance300` revalidation after the visual gates.
