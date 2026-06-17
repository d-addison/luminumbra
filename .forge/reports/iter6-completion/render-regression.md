# Render Regression Completion

Task: T-I6-011-cloud-aurora-water-render-regression

## Summary

Fixed the cloud/aurora/water ordering regression in the render frame graph.

The skybox pass now draws after G-buffer depth is copied into the lighting FBO and before transparent water. This preserves terrain occlusion for sky/cloud/aurora while preventing the skybox from overwriting water pixels that leave depth at the far plane.

The weather overlay was split from the skybox timing and is now invoked after water, keeping rain and fog as full-scene screen-space effects.

## Changed Files

- `src/luminumbra_client/rendering/RenderPipeline.cpp`
- `src/luminumbra_client/rendering/passes/SkyboxPass.cpp`
- `src/luminumbra_client/rendering/passes/SkyboxPass.h`
- `.forge/specs/iter6/wave-b-volumetric-clouds-tier-2.md`
- `.forge/specs/iter6/wave-b-aurora-curtains.md`
- `.forge/specs/iter6/wave-b-ocean-water-waves.md`

## Result

- Sky/cloud/aurora render before transparent water.
- Water blends over completed sky+lighting color.
- Weather overlay remains after water.
- Render pass metadata reports skybox before water.
