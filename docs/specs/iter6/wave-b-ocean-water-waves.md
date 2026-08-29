# Wave B - Ocean Water Waves

Status: regression fixed

Task: T-I6-011-cloud-aurora-water-render-regression

## Scope

- Keep water waves render-only in `water.vert` and `water.frag`.
- Preserve gentle Gerstner vertex swells and analytic fragment ripple normals.
- Preserve transparent water blending and depth-tested placement against the G-buffer.
- Ensure sky, cloud, and aurora are available behind water before water blends.

## Regression Resolution

The frame graph now draws sky/cloud/aurora before transparent water and snapshots that completed sky+lighting color into `lighting.opaque_color_copy`. Water still reads the copied color/depth inputs, keeps depth writes disabled, and blends into the lighting target after the sky exists behind it.

The weather overlay remains deferred until after water, preserving rain/fog composition over the final water surface.

## Acceptance

- Water surface motion remains render-only and does not alter simulation water height.
- Refraction reads a stable pre-water color source.
- Sky/cloud/aurora no longer overdraw water where the G-buffer depth is background/far depth.
- Rain/fog remain visible over water.
