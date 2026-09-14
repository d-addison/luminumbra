# diagnostic visual scenarios

Generated from [the catalog](visual-catalog.json). [Roster](visual-catalog-roster.md) · [shared packet contract](visual-catalog.md).

## D01

**Five deterministic mesh scene fixtures**

Make diagnostic geometry/features obvious while preserving minimal deterministic fixtures.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Separate terrain_caves,water_submerged,sky_gradient,ui_overlay,loading_visualizer at256²; caption synthetic scope, preserve hashes/ROIs and attach production scenario links.

Source links:

- [test/rendering/render_capture_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_capture_test.cpp)

Required variants:

- terrain_caves
- water_submerged
- sky_gradient
- ui_overlay
- loading_visualizer

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S8: Diagnostic atlas/backend outputs with honest native versus supporting evidence labels.

Capture: **existing_subset**. Five 256 x 256 diagnostic originals, written to the build-defined artifact directory. No production scene or native GPU performance qualification.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<build>/bin/render_capture_test --gtest_filter=RenderCaptureTest.DeterministicMeshScenesProduceStableImages
```

Acceptance checks:

- Make diagnostic geometry/features obvious while preserving minimal deterministic fixtures.
- Separate terrain_caves,water_submerged,sky_gradient,ui_overlay,loading_visualizer at256²; caption synthetic scope, preserve hashes/ROIs and attach production scenario links.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Retain all five deterministic 256x256 fixture originals and assertions; improve labels/diagnostic readability and link real production scenes rather than prettifying synthetic truth.

## D02

**Material calibration plates**

Provide professionally readable neutral/lit material comparison with visible texture scale and shaded detail.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Every authored plate including soil,stone,grass,sand,deepslate and gray/white references; albedo/normal/light settings, clipping/contrast measures.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- soil
- stone
- grass
- sand
- deepslate
- gray-reference
- white-reference
- neutral
- lit

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **existing_subset**. Neutral/lit calibration PPMs and JSON; hardware context and complete variants must be verified. Retained files are not approval.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<build>/bin/render_smoke_test --gtest_filter=RenderSmokeTest.CalibrationPlateCloseRangeMaterialGate
```

Acceptance checks:

- Provide professionally readable neutral/lit material comparison with visible texture scale and shaded detail.
- Every authored plate including soil,stone,grass,sand,deepslate and gray/white references; albedo/normal/light settings, clipping/contrast measures.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture every neutral/lit material and gray/white plate with controlled settings, scales and color/contrast/clipping values; link R08 and G04 defects.

## D03

**Emissive and aether materials**

Show controlled emissive response against dark and lit surroundings without making ordinary terrain glow.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Actual sampled intensity sequence, emissive/nonemissive controls, monotonic data, matching clean scene and diagnostic plates.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- emissive-intensity-ladder
- nonemissive-controls
- lit
- dark

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show controlled emissive response against dark and lit surroundings without making ordinary terrain glow.
- Actual sampled intensity sequence, emissive/nonemissive controls, monotonic data, matching clean scene and diagnostic plates.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture emitter intensity ladder with nonemissive controls and scene context; retain actual monotonicity and material identity.

## D04

**Procedural leaf cutout and albedo**

Show clean leaf silhouette and retained color through alpha edges at readable scale.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Alpha/albedo ROIs, close-up on bright/dark backgrounds, mip/distance variants and fixture identity; connect to authored tree rather than substitute for it.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- bright-background
- dark-background
- mips
- distance

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S4: Ground cover, canopy, material/UV/alpha and LOD controls with accepted pack preservation.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show clean leaf silhouette and retained color through alpha edges at readable scale.
- Alpha/albedo ROIs, close-up on bright/dark backgrounds, mip/distance variants and fixture identity; connect to authored tree rather than substitute for it.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture procedural leaf alpha closeups on light/dark backgrounds and mips; attach silhouette/color measures and compare authored canopy separately.

## D05

**Shaded material contrast**

Preserve subtle material detail in shade while keeping balanced global contrast.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Matched shaded detail crop and full frame, luminance/contrast metrics, bright reference control.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- shade
- bright-reference
- full-frame

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Preserve subtle material detail in shade while keeping balanced global contrast.
- Matched shaded detail crop and full frame, luminance/contrast metrics, bright reference control.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture matched shaded/full-frame material detail and bright reference with luminance/contrast values; do not conceal current black-band geometry defect through grading.

## D06

**Grass/cloud shadow alignment**

Show one continuous shadow across grass and terrain.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Same projected shadow ROI in both surfaces, timed movement sequence, full scene with clear shadow edge.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- grass-shadow
- terrain-shadow
- ordered-motion

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show one continuous shadow across grass and terrain.
- Same projected shadow ROI in both surfaces, timed movement sequence, full scene with clear shadow edge.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture common shadow edge across grass/terrain with matched projection and ordered samples, reusing R12 scene where exact support holds.

## D07

**Water caustics and dry-ground rejection**

Make submerged light patterns readable and keep dry terrain free of false water tint.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Wet/dry/depth/far variants, normalized target readback and caustic structure metric, surface-context shot.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- wet
- dry
- depth
- far

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Make submerged light patterns readable and keep dry terrain free of false water tint.
- Wet/dry/depth/far variants, normalized target readback and caustic structure metric, surface-context shot.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture wet/dry/far-depth caustic controls with normalized target readback; add clear scene-level shoreline context from R06.

## D08

**Aerial haze and elevated fog**

Keep depth cues continuous across distant terrain while distinguishing sky and ground layers.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Ground/elevated matched views, reconstructed world/camera data, terrain/sky fog ROIs, no hidden empty-air shortcut.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- ground
- elevated
- terrain-roi
- sky-roi

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Keep depth cues continuous across distant terrain while distinguishing sky and ground layers.
- Ground/elevated matched views, reconstructed world/camera data, terrain/sky fog ROIs, no hidden empty-air shortcut.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture ground/elevated fog and aerial-haze comparisons with terrain/sky ROI classification and actual camera metadata.

## D09

**God rays and sky occlusion**

Show volumetric light shafts with plausible blockers and visibility in air.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Occluded/unoccluded pair, ray-source attachment and sky occlusion evidence, full composition plus targeted crop.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- occluded
- unoccluded
- full-context

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show volumetric light shafts with plausible blockers and visibility in air.
- Occluded/unoccluded pair, ray-source attachment and sky occlusion evidence, full composition plus targeted crop.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture occluded/unoccluded ray scenes with source attachment and blocker evidence, preserving full-frame context and measured ROI.

## D10

**Weather stationarity and storm grass edges**

Make weather atmospheric without shifting terrain or breaking soft grass edges.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Matched base/weather frames, stationary geometry projection, world normals and grass-edge composites.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- baseline
- weather
- grass-edge

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Make weather atmospheric without shifting terrain or breaking soft grass edges.
- Matched base/weather frames, stationary geometry projection, world normals and grass-edge composites.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture matched weather stationarity and storm soft-edge controls after ownership fixes; confirm projection and normal invariants.

## D11

**Grass silhouette, slope and wind shape**

Give blades a convincing tapered silhouette, correct sloped grounding and natural bounded wind motion.

Origin: original. Priority: P0. Execution: **not_captured**. Approval: **pending**.

Fixture: Upper-frame/rising-ground/wind phase fixtures, silhouette width and displacement measurements, actual ground-level scene sequence.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- upper-frame
- rising-ground
- wind-phases
- tip-width

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S4: Ground cover, canopy, material/UV/alpha and LOD controls with accepted pack preservation.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Give blades a convincing tapered silhouette, correct sloped grounding and natural bounded wind motion.
- Upper-frame/rising-ground/wind phase fixtures, silhouette width and displacement measurements, actual ground-level scene sequence.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Use current bounded-blade tests to distinguish raw wind from actual deformation; capture taper/slope/wind controls and connect to repaired R14.

## D12

**Rough reflection and light direction**

Make roughness/light response legible across a controlled material range.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Measured GGX integration and facing-surface controls, material sphere/plate strip only if rendered by actual path, real scene companion.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- roughness-range
- facing-light-controls
- real-scene

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Make roughness/light response legible across a controlled material range.
- Measured GGX integration and facing-surface controls, material sphere/plate strip only if rendered by actual path, real scene companion.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture actual roughness/light-direction controls with analytic evidence and material range, without invented off-path shading panels.

## D13

**Colored translucent shadows**

Show convincing colored transmitted sunlight with readable blocker/surface relationship.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Actual colored-shadow frames, Beer-Lambert controls, intensity/tint measurements and unclipped full context.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- transmitted-light
- blocker-context
- beer-lambert-controls

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show convincing colored transmitted sunlight with readable blocker/surface relationship.
- Actual colored-shadow frames, Beer-Lambert controls, intensity/tint measurements and unclipped full context.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture transmitted colored light with blocker/surface context and Beer-Lambert references, checking clip/color against measurements.

## D14

**Froxel transmittance**

Explain and show density/depth attenuation without passing a featureless fog frame as a finished scene.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Uniform-medium analytic comparison and plotted actual samples, plus real layered atmospheric scene; diagnostic explicitly labeled.

Source links:

- [test/rendering/render_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_smoke_test.cpp)

Required variants:

- analytic-medium
- sample-plot
- layered-scene

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Explain and show density/depth attenuation without passing a featureless fog frame as a finished scene.
- Uniform-medium analytic comparison and plotted actual samples, plus real layered atmospheric scene; diagnostic explicitly labeled.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Qualify enabled froxel path and actual transmittance output before plotting density/depth residuals; mark opt-in quality profile in screenshots and timings.

## D15

**Exposure response**

Show readable scene response over exposure changes with controllable highlight/shadow tradeoffs.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: At least two actual GPU exposures and luminance monotonicity; manual/auto precedence and exact lens values, full-frame sequence.

Source links:

- [test/rendering/render_capture_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/render_capture_test.cpp)

Required variants:

- gpu-exposure-low
- gpu-exposure-high
- manual-auto-precedence

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show readable scene response over exposure changes with controllable highlight/shadow tradeoffs.
- At least two actual GPU exposures and luminance monotonicity; manual/auto precedence and exact lens values, full-frame sequence.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture actual manual/auto exposure pair and settings with monotonic luminance proof, preserving shipped versus opt-in meter distinction.

## D16

**GL/Diligent/Vulkan pilot parity**

Make backend differences inspectable with crisp reference/current/heatmap presentation.

Origin: original. Priority: P3. Execution: **not_captured**. Approval: **pending**.

Fixture: Actual qualified pilot frames per backend, exact metric identity and calibration, hardware/software label; no whole-engine Vulkan claim from a pilot.

Source links:

- [test/rendering/rhi_pilot_flip_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/rhi_pilot_flip_test.cpp)
- [test/rendering/dual_backend_flip_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/dual_backend_flip_test.cpp)

Required variants:

- raw-gl
- diligent
- vulkan
- reference-current-heatmap

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S8: Diagnostic atlas/backend outputs with honest native versus supporting evidence labels.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Make backend differences inspectable with crisp reference/current/heatmap presentation.
- Actual qualified pilot frames per backend, exact metric identity and calibration, hardware/software label; no whole-engine Vulkan claim from a pilot.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Qualify each existing raw-GL/Diligent/Vulkan pilot on local host, retain reference/current/diff with exact metric version; never claim complete backend parity.

## D17

**Glass shared depth and resize**

Show correct glass occlusion and stable output after framebuffer recreation.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Before/after resize, front/behind opaque geometry views and attachment/depth evidence; add retained pixels where test currently only asserts.

Source links:

- [test/rendering/pass_context_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/pass_context_test.cpp)

Required variants:

- before-resize
- after-resize
- front-opaque
- behind-opaque

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show correct glass occlusion and stable output after framebuffer recreation.
- Before/after resize, front/behind opaque geometry views and attachment/depth evidence; add retained pixels where test currently only asserts.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Retain actual glass occlusion pixels before/after resize with shared-depth attachment proof; add output retention if assertions currently discard pixels.

## D18

**Debug albedo and ground decal**

Make diagnostic albedo truthful and scent/decal placement legible.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Separate albedo and ground-decal subcases with actual G-buffer/context identity, world position and sampled tint; diagnostic vs beauty captions.

Source links:

- [test/rendering/pass_context_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/pass_context_test.cpp)

Required variants:

- debug-albedo
- ground-decal

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Make diagnostic albedo truthful and scent/decal placement legible.
- Separate albedo and ground-decal subcases with actual G-buffer/context identity, world position and sampled tint; diagnostic vs beauty captions.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Produce separate actual albedo and decal/scent-placement diagnostic images with G-buffer/context and world-position proof.

## D19

**Shadow cascades and grass mesh lifecycle**

Show moving shadows without stale silhouettes and grass following replacement/removal geometry.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Frame pairs before/after blocker and mesh changes, depth/readback assertions; no orphan grass/previous-frame shadows.

Source links:

- [test/rendering/pass_context_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/rendering/pass_context_test.cpp)

Required variants:

- blocker-before-after
- grass-replacement
- grass-removal

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S4: Ground cover, canopy, material/UV/alpha and LOD controls with accepted pack preservation.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show moving shadows without stale silhouettes and grass following replacement/removal geometry.
- Frame pairs before/after blocker and mesh changes, depth/readback assertions; no orphan grass/previous-frame shadows.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Retain blocker/grass-mesh lifecycle before/after frames and depth/readback evidence to expose stale shadows or orphan grass.

## D20

**Runtime horizon and mixed LOD topology**

Turn a broad but small diagnostic horizon into an intelligible coverage proof linked to a composed world view.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Existing384²spawn_horizon plus mesh quadrant/collider/LOD counts, seam visualization and qualified high-resolution companion.

Source links:

- [test/performance/runtime_world_visual_validation_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/performance/runtime_world_visual_validation_test.cpp)

Required variants:

- spawn-horizon-384
- high-resolution-companion
- mixed-lod-seams

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S6: Terrain/streaming/spatial continuity, independent coverage, persistent world authority.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Turn a broad but small diagnostic horizon into an intelligible coverage proof linked to a composed world view.
- Existing384²spawn_horizon plus mesh quadrant/collider/LOD counts, seam visualization and qualified high-resolution companion.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Re-capture 384x384 runtime horizon alongside high-resolution companion and regional coverage, using black-band findings to strengthen per-region diagnosis.

## D21

**Worldgen layer, biome, river and preset atlases**

Make every layer/preset map easy to interpret and show how it appears in the actual world.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Named layer height/SDF slices; all authored presets/biome/river atlas; scales, legend, seed/hash, spawn/topology metrics and matched engine views. Maps are visualizations, not engine screenshots.

Source links:

- [test/shield/test_worldgen_layer_snapshots.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/shield/test_worldgen_layer_snapshots.cpp)

Required variants:

- all-named-layers
- sdf-slices
- all-presets
- biomes
- rivers
- matched-engine-views

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S6: Terrain/streaming/spatial continuity, independent coverage, persistent world authority.
- S8: Diagnostic atlas/backend outputs with honest native versus supporting evidence labels.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Make every layer/preset map easy to interpret and show how it appears in the actual world.
- Named layer height/SDF slices; all authored presets/biome/river atlas; scales, legend, seed/hash, spawn/topology metrics and matched engine views. Maps are visualizations, not engine screenshots.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Enumerate all layer/preset/biome/river atlas variants, regenerate maps with scales and hashes, then attach matched engine views for interpretation.

## D22

**Far-field GPU ground truth/G-buffer/max-mip**

Provide precise interpretable hit/depth/normal/material-error views with a real far-field companion.

Origin: original. Priority: P3. Execution: **not_captured**. Approval: **pending**.

Fixture: Actual hardware-qualified outputs vs analytic/CPU truth, residual images/scales and timings; missing GPU explicitly unevaluated.

Source links:

- [test/performance/shieldrt_far_field_parity_gpu.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/performance/shieldrt_far_field_parity_gpu.cpp)
- [test/performance/shieldrt_far_field_gbuffer_gpu.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/performance/shieldrt_far_field_gbuffer_gpu.cpp)
- [test/performance/shieldrt_far_field_maxmip_gpu.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/performance/shieldrt_far_field_maxmip_gpu.cpp)

Required variants:

- ground-truth
- gbuffer
- max-mip
- residuals

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S6: Terrain/streaming/spatial continuity, independent coverage, persistent world authority.
- S8: Diagnostic atlas/backend outputs with honest native versus supporting evidence labels.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Provide precise interpretable hit/depth/normal/material-error views with a real far-field companion.
- Actual hardware-qualified outputs vs analytic/CPU truth, residual images/scales and timings; missing GPU explicitly unevaluated.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Run existing far-field GPU truth/G-buffer/max-mip producers on qualified native backend, retain residual images and timer status; missing platform support stays blocked.
