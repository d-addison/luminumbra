# T-I6-030 Erosion Worldgen Hash Bump

## Summary

- Added `kHydraulicErosionWorldgenVersion = 2` as the explicit generation salt for the enabled hydraulic/thermal erosion path.
- Mixed that version into `ComputeTerrainParamsHash` before the pinned hydro params so erosion kernel behavior changes invalidate pristine far-LOD/worldgen cache keys.
- Kept disabled hydro worlds byte-stable by leaving the version salt inside the existing `params.hydro_enabled` block.
- Updated the Wave C spec and erosion research brief to require a version bump plus re-bless for future shape-affecting erosion kernel changes.

## Verification

- `cmake --build --preset debug --target luminumbra_client_app` failed before compile: `Error: could not load cache`.
