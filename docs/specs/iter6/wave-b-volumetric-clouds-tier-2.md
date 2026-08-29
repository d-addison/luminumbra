# Wave B - Volumetric Clouds Tier 2

Status: regression fixed

Task: T-I6-011-cloud-aurora-water-render-regression

## Scope

- Keep volumetric cloud rendering render-only in `enhanced_skybox.frag`.
- Preserve the shared cloud coverage field used by the sky dome and projected cloud shadows.
- Preserve the cheap clear-sky reject and bounded cloud slab raymarch.
- Prevent the sky/cloud pass from overwriting transparent water pixels.

## Regression Resolution

The skybox pass now renders before transparent water, after the G-buffer depth has been copied into the lighting FBO. This keeps clouds depth-tested against terrain while allowing water to blend over the completed sky instead of being overwritten by sky pixels at far depth.

Weather overlay timing is deferred until after water so rain and fog remain a full-scene post composite.

## Acceptance

- Cloud rendering remains a pure function of render state, camera direction, time, and sky uniforms.
- Cloud shadows remain registered through the shared `cloudCoverageAt` field.
- Water over sky/clouds remains visible in far-depth/background samples.
- Weather overlay still composites after water.
