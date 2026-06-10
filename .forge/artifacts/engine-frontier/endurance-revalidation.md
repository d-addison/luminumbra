# Endurance Revalidation Report

Task: T-EF-34-endurance-revalidation (executed directly; agent dispatch was
blocked by the Codex usage limit). Date: 2026-06-10.

## Terminal Gate Result

`validate-runtime-stability-phase-1.ps1 -Mode Endurance300` — **PASS**

Visible 300-second `auto_world_smoke` run in the archipelago world:

- elapsed_seconds: 301.55 (timed_run_seconds=300, visible window)
- frame_count: 18016
- readiness.ready: true
- render_pass.water_draws: 979, water_indices_drawn: 715686
- render_pass.terrain_draws: 734
- gl_debug.errors: 0
- terrain/water uploads_deferred at end: 0 / 0
- upload priority inversions (terrain/water deferred nearer than selected): 0 / 0

Note: the first revalidation attempt failed because `auto_world_smoke` ran in
the default preset, which generates no water near spawn (height_offset 20
against sea level 0) — the gate's visible-water assertion was unsatisfiable.
Water-asserting scenarios (endurance, water visual, material visual) now run
in the archipelago world; `lod_ground_smoke` keeps the default world its
pixel thresholds were tuned on.

## Visual Gate Provenance (prior green runs preserved)

| Gate | Artifact | passed | timestamp_utc | sha256[0..16] |
| --- | --- | --- | --- | --- |
| MaterialVisual | runtime/material-visual/material-visual-analysis.json | true | 2026-06-10T16:09:57Z | AEF7CA5D1D9DCCED |
| LodGround | runtime/lod-ground-baseline/lod-ground-visual-analysis.json | true | 2026-06-10T16:09:08Z | 1A0FF8A46F5BF078 |
| WaterVisual | runtime/water-visual/water-visual-analysis.json | true | 2026-06-10T16:09:25Z | 88236890C0BD89FE |

## Test Lane

`ctest --preset debug --output-on-failure -E "_NOT_BUILT$"` — 100% passed,
0 failed out of 68 (includes the frontier gate executables added in wave 8).

## Status

The endurance gate now runs after the visual gates and records the artifacts
it depends on, closing Loop 6 of the visual-performance-improvement plan.
