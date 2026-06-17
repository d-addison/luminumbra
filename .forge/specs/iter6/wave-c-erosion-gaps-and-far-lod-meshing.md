# Wave C: Erosion Gaps and Far-LOD Meshing

## Objective

Repair the FarLodHorizon render path so the far-LOD mesh draw band covers the full authored F2 horizon range after the camera far plane was extended beyond 1536 m.

## Scope

- Keep far-LOD scheduling and rendering aligned at the same outer radius.
- Preserve the existing near clip, camera-region skip, water sheet pass, and live-terrain precedence.
- Add a validation guard that fails before scenario execution if the far mesh outer clip is narrowed below the F2 range.

## Acceptance Criteria

- `FarLodSystem::kFarClipOuterRadiusMeters` is tied to `kF2OuterRangeMeters`.
- The stale 950 m geometry clip no longer removes drawable terrain in the 950-1536 m horizon ring.
- `FarLodHorizon` validation checks the source guard before running the expensive capture sweep.

## Verification Commands

```powershell
cmake --build --preset debug --target luminumbra_client_app
powershell -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode FarLodHorizon -BuildPreset debug
```

## No-Deferral Rules

- Do not lower the far-LOD outer clip below the F2 outer range.
- Do not mask horizon gaps by weakening pixel thresholds.
- Do not disable the water-continuity, sky-sliver, or capture-pin assertions.
