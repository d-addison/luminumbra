# Far-LOD Horizon Render Repair

## Summary

The far-LOD render path had a stale outer geometry clip at 950 m while the camera far plane now extends beyond the 1536 m F2 horizon. That clipped drawable far terrain out of the 950-1536 m band and could leave the horizon ring uncovered.

## Changes

- Updated `FarLodSystem::kFarClipOuterRadiusMeters` to use `kF2OuterRangeMeters`.
- Added a `FarLodHorizon` validator source guard so the gate fails if the far outer clip is narrowed away from the F2 range.
- Restored the missing iter6 Wave C spec artifact for this repair task.

## Verification

- `cmake --build --preset debug --target luminumbra_client_app` failed before build because the debug cache was not configured.
- `cmake --preset debug` failed during configure because `vendor/fastnoise` and `vendor/googletest` are missing `CMakeLists.txt`.
- `powershell -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode FarLodHorizon -BuildPreset debug` reached the client executable check after the new source guard, then failed because `build/debug/bin/luminumbra_client_app.exe` is absent.
