# Engine Frontier 2026-06-09

## Objective

Push the Luminumbra engine forward on all fronts with deterministic gates, prioritizing mature-system deepening in world streaming and rendering plus continuation of the visual-quality loop, especially the material/texture-layer gate, before enabling broader frontier systems such as persistence, GPU SDF runtime paths, Aetheric Field, GOAP/Instinct AI, Lua hot reload, or networking.

## Scope

In scope:

- Implement and validate the material/texture-layer visual gate, including Sand versus grey fallback, material-ID or texture-layer heatmap capture, final-color plausibility, and zero GL debug errors.
- Revalidate the mature visual/runtime loop with `LodGround`, `WaterVisual`, and endurance after the material gate exists.
- Keep world/streaming work focused on drain, upload priority, LOD-hole prevention, LOD churn, seam risk, and deterministic telemetry.
- Keep rendering work focused on visual gates, render-health baselines, shader inventory, and water-quality gate preparation rather than broad render graph replacement.
- Preserve and document gate-first entry points for physics/player, audio, UI/tooling/tests, simulation/Lua/Aetheric/Instinct, persistence, GPU SDF, and networking.
- Maintain Codex-only Forge workflow artifacts for this feature.

Non-goals for this iteration:

- Broad gameplay expansion, new creature/content breadth, or subjective visual polish without gates.
- Non-Codex AI dispatch, routing, recommendations, or agent execution.
- Render graph replacement, job-system replacement, sockets, prediction, multiplayer UI, runtime GPU SDF enablement, far-field SDF streaming, global Aetheric simulation, Lua hot reload in live multiplayer, or GOAP breadth before their named gates exist.
- Serializing mesh or collision as authoritative persistence data.
- Loosening existing visual thresholds to pass without an artifact-backed source fix.

## Acceptance Criteria

- `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual` passes, and the artifact proves Sand material ID/layer agreement, no grey fallback in the Sand ROI, valid screenshot and heatmap paths, and zero GL debug errors.
- The debug build for the engine-frontier validation targets succeeds without warnings-as-errors breaks: `luminumbra_client_app`, `world_generation_test`, `sdf_gpu_cpu_parity_test`, `render_smoke_test`, `render_capture_test`, `runtime_world_visual_validation_test`, and `ui_smoke_test`.
- The unit and smoke test lane covering world generation, water regressions, SDF parity, render smoke/capture, runtime visual validation, and UI smoke passes under the debug preset.
- `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode LodGround -SmokeSeconds 30` passes with no LOD holes, no GL debug errors, ready final state, drained uploads, and zero terrain/water upload-priority inversions.
- `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode WaterVisual -SmokeSeconds 30` passes with visible water pixels, nonzero water draw submission, ready final state, and zero GL debug errors.
- Endurance is revalidated after `MaterialVisual`, `LodGround`, and `WaterVisual` are green. The endurance artifact must complete without early visible-window close, report readiness true at the end, drain generation/meshing/job/upload tails, report zero upload-priority inversions, and preserve the visual gates.
- `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode All` passes for the feature documentation, source checks, build/unit lane, and material gate.
- `forge tasks validate .forge/tasks/engine-frontier/dispatch.json` passes once the engine-frontier dispatch file is present.
- Frontier systems remain disabled or explicitly gated. No persistence runtime path, GPU SDF runtime toggle, far-field SDF render path, networking authority path, Aetheric simulation, GOAP/Instinct planner, or Lua hot reload is accepted without its deterministic artifact and validator mode.

## Verification Commands

```powershell
cmake --build --preset debug --target luminumbra_client_app world_generation_test sdf_gpu_cpu_parity_test render_smoke_test render_capture_test runtime_world_visual_validation_test ui_smoke_test --parallel 1
ctest --preset debug --output-on-failure -R "WorldGenerationTest|WorldAndWaterTest|SdfGpuCpuParityTest|SDF|RenderSmokeTest|RenderCaptureTest|RuntimeWorldVisualValidationTest|UiSmokeTest"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode All
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode LodGround -SmokeSeconds 30
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode WaterVisual -SmokeSeconds 30
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode Endurance300
forge tasks validate .forge/tasks/engine-frontier/dispatch.json
```

## No-Deferral Rules

If sand renders as flat grey, a material ID maps to the wrong texture layer, or invalid terrain material IDs silently fall back to production grey, the issue must become a failing material/heatmap gate and be fixed before this feature closes.

If any new GL debug error appears during material, LOD, water, render, capture, or endurance validation, the affected lane must fail and the GL state/resource issue must be fixed before closure.

If LOD holes, background-blue triangles, dark voids, or near-ground missing terrain reappear in gated captures, the issue must become a failing `LodGround` or seam/LOD gate and be fixed before closure.

If terrain or water upload-priority inversions recur, or deferred upload tails do not drain after the scenario stabilizes, the inversion must become a failing gate and be fixed before closure.

If warnings-as-errors breaks, shader compile/link failures, missing required artifacts, stale visual thresholds, or missing validator modes block the feature checks, do not mark the feature complete by deferring them.
