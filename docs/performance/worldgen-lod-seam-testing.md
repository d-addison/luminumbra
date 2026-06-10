# Worldgen LOD Seam Testing

Date: 2026-06-08

## Purpose

The runtime world visual harness verifies that world creation produces a broad visible horizon, near-field collision, and covered mixed-LOD terrain boundaries. It exists to catch failures where the player spawns on a small rendered patch, sees through terrain, or crosses chunk/LOD seams with missing mesh.

## Current Gate

`RuntimeWorldVisualValidationTest` builds the default spawn horizon through `SHIELD_WorldSystem::EnsureSurfaceReadyNear(spawn, physics, 12, 4)`.

It writes:

- `runtime_world_visual.json`: horizon mesh counts, LOD distribution, collision count, image coverage.
- `spawn_horizon.ppm`: offscreen capture of the generated terrain horizon.
- `lod_seams.json`: adjacent seam counts, density continuity, mixed-LOD transition coverage.
- `physics_water_budget.json`: near collision scope versus broad render horizon.

The seam gate now fails when:

- Adjacent boundary density has discontinuities.
- Mixed-LOD pairs are not reported.
- A mixed-LOD transition-risk sample is not covered by transition geometry.
- A mixed-LOD pair lacks transition coverage.

## Transition Geometry Prototype

The current implementation uses a conservative prototype rather than full Transvoxel cells:

- LOD0 terrain remains raw Marching Cubes.
- LOD1/LOD2 chunks add transition geometry only on faces adjacent to finer LOD or vertically offset mixed-LOD neighbors.
- The first pass adds downward skirts from boundary mesh edges.
- If a requested transition face has no boundary mesh edge, a fallback fills near-surface SDF cells on that face with outward patches.
- Vertically offset mixed-LOD pairs accept coverage from either side when that side can actually bridge the vertical seam.

This keeps the fix scoped and testable while avoiding broad, always-on chunk boundary walls.

## Reading `lod_seams.json`

Important fields:

- `mixed_lod_pairs`: number of adjacent pairs with different LOD levels.
- `transition_risk_samples`: near-surface samples that can expose fine/coarse topology gaps.
- `mitigated_transition_risk_samples`: risk samples covered by transition geometry.
- `unmitigated_transition_risk_samples`: must remain `0`.
- `mixed_lod_pairs_without_transition_skirt`: must remain `0`.
- `transition_skirt_pair_type_counts`: coverage by LOD pair and face direction.
- `missing_transition_pair_details`: exact coarse/fine chunk coordinates if coverage fails.

## Long-Term Direction

This prototype is a blocker closure step, not the final terrain LOD algorithm. The long-term target is still Transvoxel-style transition cells or an equivalent constrained stitching method that produces mathematically clean transition topology with less overdraw than fallback face patches.

Until that lands, the automated gate keeps the known visual seam class from silently returning.
