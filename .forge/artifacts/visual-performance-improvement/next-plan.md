# Visual Performance Next Plan

Date: 2026-06-09

## Objective

Close the remaining user-visible visual blockers with reproducible gates before tuning broader performance:

1. Black/blue triangular terrain holes in the near-ground LOD path.
2. Residual dark water/terrain overlays and false water skins.
3. Material appearance uncertainty, especially sand/grey fallback.
4. Long-run readiness and stability after visual correctness is gated.

## Current Baseline

The LOD hole loop is now landed and gated:

- `LodGround` writes `lod-ground-visual-analysis.json` and passes with enforced mid/end captures.
- Latest enforced LOD captures have `0` dark-void pixels, `0` background-blue pixels, and near-black silhouette pixels below threshold (`mid=152`, `end=1216`) in the widened terrain ROI.
- Large black/blue triangular terrain holes are no longer present in the gated captures after per-cell coarse LOD ownership.
- Latest converted screenshots are under `build/debug/test-artifacts/runtime/lod-ground-baseline/png/`.

The water visibility loop is also landed and gated:

- `WaterVisual` now uses an archipelago/open-water target and passes with `water-visual-analysis.json`.
- Latest water visual artifact has `359424 / 359424` ROI pixels classified as water-like, ratio `1.0`.
- Open-water support is `25` samples.
- Water draw submission is nonzero: `water_draws=671`, `water_indices_drawn=155730`.
- GL debug errors were `0`.

Remaining visual work is material/lighting quality, not basic draw submission:

- Occasional small bright specks or dark cave/terrain-edge marks can still appear near the lower edge or horizon silhouettes; the large LOD defects are gated closed.
- The open-water screenshot is visibly blue-green and passes the water gate; a future beauty pass can improve lower-angle water lighting/tonemapping.
- Sand/grey material appearance still lacks a deterministic heatmap or screenshot gate.

## Loop 1: LOD Hole Pixel Gate

Goal: make black/blue triangular holes and pure black silhouettes fail deterministically.

Actions:

- Add `lod-ground-visual-analysis.json` beside `lod-ground-screenshots.json`.
- Analyze start/mid/end PPM screenshots for background-colored holes in terrain-heavy regions.
- Analyze widened lower-frame ROI for pure near-black silhouettes in addition to blue/black clear-color holes.
- Record per-screenshot:
  - ROI dimensions.
  - background-like pixel count.
  - dark-hole pixel count.
  - largest connected component estimate if cheap enough.
  - pass/fail threshold.
  - camera pose and coverage stats for the matching frame.
- Extend `LodGround` validation to fail if mid/end hole pixels exceed threshold.

Status: landed, then tightened for pure near-black silhouette pixels.

Acceptance now met:

- `lod-ground-visual-analysis.json` is produced.
- `LodGround` validation fails on excessive dark/background-blue/near-black pixels and passes on the fixed captures.
- Failure messages include pixel counts and screenshot path.

## Loop 2: Geometry Source Isolation

Goal: determine whether holes come from terrain mesh generation, transition skirts, culling, water overlay, or camera clipping.

Run focused toggles or temporary diagnostics one at a time:

- Disable water pass for LOD screenshots.
- Disable LOD transition skirts.
- Force all near-field terrain to LOD0.
- Render terrain wireframe or material-ID debug mode into capture.
- Emit per-chunk screen-space bounding boxes or chunk ID color capture if fast to add.

Candidate checks:

- If holes remain with water disabled, prioritize terrain mesh/LOD/culling.
- If holes vanish with water disabled, revisit water mesh depth, depth test, and blend ordering.
- If holes vanish with forced LOD0, prioritize transition geometry and LOD seam rules.
- If holes track chunk boundaries, inspect transition skirt winding and neighbor LOD selection.

Status: landed for the large LOD holes.

Finding:

- Coarse step 2/4 marching cubes under-sampled terrain and produced background-colored holes.
- Whole-chunk coarse heightfield ownership based only on chunk center could still drop edge/corner terrain cells.
- Isosurface material sampling also classified some full-detail terrain as `Air`.

## Loop 3: Fix Terrain Holes

Likely fix areas, depending on Loop 2 evidence:

- Transition skirt winding or degenerate triangle handling in `MarchingCubes.cpp`.
- Missing side faces/skirts at LOD boundaries or vertical chunk seams.
- Culling hierarchy bounds for near-ground views.
- Near-camera clipping through terrain exposing backfaces or missing underside geometry.
- Render order/depth state if non-terrain overlays are involved.

Implementation rules:

- Keep old renderable mesh until replacement LOD mesh and upload are ready.
- Do not increase LOD radius as a substitute for fixing holes unless the pixel gate proves radius is the root cause.
- Add the smallest targeted fix and rerun `LodGround`.

Status: landed for the gated LOD path.

Implemented:

- Coarse LOD terrain now uses a primary-surface heightfield mesh.
- Coarse heightfield LOD now emits cells per sampled cell surface height instead of accepting/rejecting an entire chunk from its center.
- Terrain material lookup samples slightly into solid terrain.
- Terrain material lookup falls back to renderable terrain materials instead of `Air`/`Water`.
- Cave surface cap is more conservative and mirrored in GPU SDF.
- LOD smoke uses a stable daylight inspection camera.

Acceptance now met:

- `LodGround` passes runtime coverage and the visual hole pixel gate.
- Latest mid/end screenshots no longer show the large background triangles that triggered this loop.

## Loop 4: Material Visual Gate

Goal: prove material appearance rather than draw submission.

Actions:

- Add a material/debug capture mode or heatmap artifact for material IDs/texture layers.
- Validate sand-like regions map to sand material/texture layer.
- Validate invalid material IDs render as diagnostic fallback in debug artifacts, not silent grey.

Acceptance:

- A deterministic material visual gate passes and can fail on grey fallback.

## Loop 5: Water Follow-Up

Goal: keep the new water visual gate useful while terrain work proceeds.

Actions:

- Keep `WaterVisual` in the smoke set for visual changes touching render, water, or shaders.
- Keep the raised deep tint/minimum tint/alpha path from the latest pass so water remains visible over dark terrain.
- Add a stricter lower-angle water quality gate after lighting/tonemapping improves; the current gate proves visibility, not beauty.
- Consider capturing a second lower camera angle once water lighting is less dark.

Acceptance:

- Water remains visibly tinted/translucent with GL errors `0`.

Status: landed for visibility; beauty/lower-angle validation remains open.

## Loop 6: Endurance Revalidation

Goal: only rerun long visible endurance after visual gates are meaningful.

Commands:

```powershell
cmake --build --preset debug --target luminumbra_client_app --parallel 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode WaterVisual -SmokeSeconds 20
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode LodGround -SmokeSeconds 30
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode Smoke -SmokeSeconds 5
```

After the LOD visual hole gate passes:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode Endurance300
```

Do not close the visible endurance window early.

## Immediate Next Task

Implement Loop 4: a deterministic material/texture-layer visual gate for terrain, especially sand versus grey fallback. Keep `LodGround` and `WaterVisual` in the verification set while doing it, and watch screenshots for any recurrence of the tiny black lower-edge specks or horizon artifacts.
