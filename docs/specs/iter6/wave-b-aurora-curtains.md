# Wave B - Aurora Curtains

Status: regression fixed

Task: T-I6-011-cloud-aurora-water-render-regression

## Scope

- Keep aurora rendering as deep-night-only curtain sheets in `enhanced_skybox.frag`.
- Preserve overcast gating so aurora does not show through storm cloud decks.
- Preserve the render-only path with no simulation or world hash feedback.
- Prevent aurora sky pixels from overwriting transparent water.

## Regression Resolution

Aurora remains part of the skybox shader, but the skybox pass now runs before transparent water. The lighting FBO already has copied G-buffer depth at that point, so aurora is still terrain-occluded while water blends over the finished sky color.

Weather overlay is invoked after water to keep precipitation and fog on top of both sky and water.

## Acceptance

- Aurora is gated by the CPU-provided deep-night strength and shader overcast gate.
- Aurora remains banded/curtain-like, not a screen-space blob or overlay.
- Water pixels are not replaced by aurora/sky output when depth remains at the far plane.
- Rain/fog overlay still composites as a full-scene effect.
