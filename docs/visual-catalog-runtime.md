# runtime visual scenarios

Generated from [the catalog](visual-catalog.json). [Roster](visual-catalog-roster.md) · [shared packet contract](visual-catalog.md).

## R01

**Whole-world seasonal/time/weather visual sweep**

Make every required cell readable and visually intentional: coherent horizon composition, legible foreground, distinguishable lighting/weather, no blank or hidden cells.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: 48 summer cells;96 with winter: dawn/noon/dusk/night × yaw000/yaw120/yaw240/down35/up25/water × clear/storm. Original frames, exact per-cell pins and feature metrics; missing cells displayed as failures.

Source links:

- [src/luminumbra_client/core/scenarios/WorldVisualSweep.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/WorldVisualSweep.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- summer/dawn/yaw000/clear
- summer/dawn/yaw000/storm
- summer/dawn/yaw120/clear
- summer/dawn/yaw120/storm
- summer/dawn/yaw240/clear
- summer/dawn/yaw240/storm
- summer/dawn/down35/clear
- summer/dawn/down35/storm
- summer/dawn/up25/clear
- summer/dawn/up25/storm
- summer/dawn/water/clear
- summer/dawn/water/storm
- summer/noon/yaw000/clear
- summer/noon/yaw000/storm
- summer/noon/yaw120/clear
- summer/noon/yaw120/storm
- summer/noon/yaw240/clear
- summer/noon/yaw240/storm
- summer/noon/down35/clear
- summer/noon/down35/storm
- summer/noon/up25/clear
- summer/noon/up25/storm
- summer/noon/water/clear
- summer/noon/water/storm
- summer/dusk/yaw000/clear
- summer/dusk/yaw000/storm
- summer/dusk/yaw120/clear
- summer/dusk/yaw120/storm
- summer/dusk/yaw240/clear
- summer/dusk/yaw240/storm
- summer/dusk/down35/clear
- summer/dusk/down35/storm
- summer/dusk/up25/clear
- summer/dusk/up25/storm
- summer/dusk/water/clear
- summer/dusk/water/storm
- summer/night/yaw000/clear
- summer/night/yaw000/storm
- summer/night/yaw120/clear
- summer/night/yaw120/storm
- summer/night/yaw240/clear
- summer/night/yaw240/storm
- summer/night/down35/clear
- summer/night/down35/storm
- summer/night/up25/clear
- summer/night/up25/storm
- summer/night/water/clear
- summer/night/water/storm
- winter/dawn/yaw000/clear
- winter/dawn/yaw000/storm
- winter/dawn/yaw120/clear
- winter/dawn/yaw120/storm
- winter/dawn/yaw240/clear
- winter/dawn/yaw240/storm
- winter/dawn/down35/clear
- winter/dawn/down35/storm
- winter/dawn/up25/clear
- winter/dawn/up25/storm
- winter/dawn/water/clear
- winter/dawn/water/storm
- winter/noon/yaw000/clear
- winter/noon/yaw000/storm
- winter/noon/yaw120/clear
- winter/noon/yaw120/storm
- winter/noon/yaw240/clear
- winter/noon/yaw240/storm
- winter/noon/down35/clear
- winter/noon/down35/storm
- winter/noon/up25/clear
- winter/noon/up25/storm
- winter/noon/water/clear
- winter/noon/water/storm
- winter/dusk/yaw000/clear
- winter/dusk/yaw000/storm
- winter/dusk/yaw120/clear
- winter/dusk/yaw120/storm
- winter/dusk/yaw240/clear
- winter/dusk/yaw240/storm
- winter/dusk/down35/clear
- winter/dusk/down35/storm
- winter/dusk/up25/clear
- winter/dusk/up25/storm
- winter/dusk/water/clear
- winter/dusk/water/storm
- winter/night/yaw000/clear
- winter/night/yaw000/storm
- winter/night/yaw120/clear
- winter/night/yaw120/storm
- winter/night/yaw240/clear
- winter/night/yaw240/storm
- winter/night/down35/clear
- winter/night/down35/storm
- winter/night/up25/clear
- winter/night/up25/storm
- winter/night/water/clear
- winter/night/water/storm

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.
- S8: Diagnostic atlas/backend outputs with honest native versus supporting evidence labels.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario world_visual_sweep --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Required environment: `{"LUMINUMBRA_VISUAL_SWEEP_WINTER": "1"}`.

Acceptance checks:

- Make every required cell readable and visually intentional: coherent horizon composition, legible foreground, distinguishable lighting/weather, no blank or hidden cells.
- 48 summer cells;96 with winter: dawn/noon/dusk/night × yaw000/yaw120/yaw240/down35/up25/water × clear/storm. Original frames, exact per-cell pins and feature metrics; missing cells displayed as failures.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Summer/winter generation and missing-cell montage slots exist. Standing gate defaults to 48 summer cells; the full campaign requires the winter environment flag and all 96 cells. No current qualifying capture.

Retained evidence:

- None qualified for this catalog row.

Next action: Run 48-cell summer sweep after camera/foliage fixes; explicitly expand winter to 96 cells and validate every slot; prepare four time-of-day contact sheets with full originals.

## R02

**Initial world / auto_world_smoke**

Present a convincing completed world entry with a visible grounded player view and understandable loading transition.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Capture creation/loading/ready sequence and ready-state frame; tie world hash, collider and streaming readiness to frame. Verify exact entry orchestration before scripting.

Source links:

- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- creation
- loading
- grounded-ready

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Present a convincing completed world entry with a visible grounded player view and understandable loading transition.
- Capture creation/loading/ready sequence and ready-state frame; tie world hash, collider and streaming readiness to frame. Verify exact entry orchestration before scripting.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Selector exists. A dedicated retained creation/loading/ready image sequence is not established; do not equate process success to visible readiness.

Retained evidence:

- None qualified for this catalog row.

Next action: Bind loading/ready stills to actual world entry and readiness event; preserve transient progress and final grounded state instead of only process success.

## R03

**Ground LOD camera path / lod_ground_smoke**

Keep ground detail and silhouette coherent through the existing near/mid/far path.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Existing lod-ground named frames plus continuous path samples, camera positions, LOD selection/triangle counts and image difference around transitions.

Source links:

- [src/luminumbra_client/core/scenarios/LodGround.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/LodGround.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- near
- mid
- far
- transition-path

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S6: Terrain/streaming/spatial continuity, independent coverage, persistent world authority.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario lod_ground_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Keep ground detail and silhouette coherent through the existing near/mid/far path.
- Existing lod-ground named frames plus continuous path samples, camera positions, LOD selection/triangle counts and image difference around transitions.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Record each named ground-LOD station and intervening transition frames; present labeled near/mid/far sheet with seam metrics.

## R04

**LOD boundary oscillation**

Cross the same boundary repeatedly without flicker, holes or unstable geometry.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Frame sequence plus transition recorder, queue/residency state and repeated crossing timestamps; still alone cannot qualify.

Source links:

- [src/luminumbra_client/core/scenarios/StreamingLodBoundary.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/StreamingLodBoundary.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- repeated-crossings
- worst-transition-frame

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S6: Terrain/streaming/spatial continuity, independent coverage, persistent world authority.

Capture: **missing**. The scenario selector and numeric/transition receipts exist, but retained original pixels for this complete visual claim are not wired in the current capture orchestrator.

Acceptance checks:

- Cross the same boundary repeatedly without flicker, holes or unstable geometry.
- Frame sequence plus transition recorder, queue/residency state and repeated crossing timestamps; still alone cannot qualify.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Transition recording exists without retained repeated-crossing image output; implement bounded original-frame retention before claiming a visual packet.

Retained evidence:

- None qualified for this catalog row.

Next action: Add retained frame IDs around repeated boundary crossings and aggregate worst seam/flicker events; select stills from measured intervals.

## R05

**LOD seam arrival**

Show newly arriving neighboring geometry joining without exposed cracks or transient voids.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Existing lod-seam captures, arrival order/frame IDs and screen-space seam evidence; retain worst transition frame.

Source links:

- [src/luminumbra_client/core/scenarios/StreamingLodBoundary.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/StreamingLodBoundary.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- before-arrival
- arrival
- joined
- worst-seam

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S6: Terrain/streaming/spatial continuity, independent coverage, persistent world authority.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario lod_seam_arrival_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Show newly arriving neighboring geometry joining without exposed cracks or transient voids.
- Existing lod-seam captures, arrival order/frame IDs and screen-space seam evidence; retain worst transition frame.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture incoming neighbor states and geometry continuity with arrival order; include worst transient frame in review sheet.

## R06

**Water surface, shore and reflection**

Make the shoreline, depth and reflected scene legible and materially distinct without hiding dry-ground errors.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: water-visual.ppm and water-reflection.ppm plus camera/depth/shore ROI, underwater/surface variants and short motion path for reflection stability.

Source links:

- [src/luminumbra_client/core/scenarios/WaterVisual.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/WaterVisual.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- surface
- underwater
- shore
- reflection-motion

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario water_visual_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Make the shoreline, depth and reflected scene legible and materially distinct without hiding dry-ground errors.
- water-visual.ppm and water-reflection.ppm plus camera/depth/shore ROI, underwater/surface variants and short motion path for reflection stability.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Run existing water/reflection scene on verified archipelago inputs; frame shore and depth clearly, then expose wet/dry/reference pairs.

## R07

**Waterfall sheet/spray/foam and dam response**

Show a coherent waterfall connected to actual upstream water, with readable sheet, spray, foam and surrounding scale.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Link test/rendering/waterfall_visual_test.cpp detected-site fixture and dam-extinguish assertions; capture active/blocked/recovered site and particle/state counts. Verify runtime trigger wiring; producer helper alone is not a runnable scenario.

Source links:

- [src/luminumbra_client/core/scenarios/WaterfallVisual.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/WaterfallVisual.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- active
- dammed
- recovered

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show a coherent waterfall connected to actual upstream water, with readable sheet, spray, foam and surrounding scale.
- Link test/rendering/waterfall_visual_test.cpp detected-site fixture and dam-extinguish assertions; capture active/blocked/recovered site and particle/state counts. Verify runtime trigger wiring; producer helper alone is not a runnable scenario.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: WaterfallVisual gate runs WaterfallVisualTest.SiteDetectionDeterministicAndDressed. There is no waterfall runtime selector. Dam/recovery assertions and diagnostic dressing do not supply a composed active/blocked/recovered packet.

Retained evidence:

- None qualified for this catalog row.

Next action: Verify runtime WaterfallVisual helper caller and actual site framing, map active/dammed/recovery states to existing water tests, and retain corresponding stills.

## R08

**Terrain material vista and ID heatmap**

Demonstrate identifiable sand/grass/stone/soil with natural scale, shaded detail and transitions in a well-framed real scene.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: material-visual.ppm plus material-id-heatmap.ppm; material ROI identities, camera and color/light settings, comparable close/mid/distant views.

Source links:

- [src/luminumbra_client/core/scenarios/MaterialVisual.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/MaterialVisual.cpp)
- [src/luminumbra_client/core/scenarios/MaterialLodAnalysis.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/MaterialLodAnalysis.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- close
- mid
- distant
- material-id-heatmap

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario material_visual_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Demonstrate identifiable sand/grass/stone/soil with natural scale, shaded detail and transitions in a well-framed real scene.
- material-visual.ppm plus material-id-heatmap.ppm; material ROI identities, camera and color/light settings, comparable close/mid/distant views.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture real material vista and ID heatmap at matched camera; compare close/mid/far material readability against calibration plates.

## R09

**Skybox and celestial view**

Compose a convincing sky that retains horizon integration and identifiable celestial features across intended views.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: skybox-visual.ppm, sun/moon projection and visibility measures; dawn/day/dusk/night variants linked to R15.

Source links:

- [src/luminumbra_client/core/scenarios/SkyboxVisual.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/SkyboxVisual.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- dawn
- day
- dusk
- night
- sun
- moon

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario skybox_visual_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Compose a convincing sky that retains horizon integration and identifiable celestial features across intended views.
- skybox-visual.ppm, sun/moon projection and visibility measures; dawn/day/dusk/night variants linked to R15.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Run skybox capture at qualified sun/moon projections, attach actual pass times and compare horizon context with R15 daylight/night variants.

## R10

**Clear/weather comparison**

Show mood and depth changes with readable terrain and stationary geometry, not an indiscriminate full-frame tint.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: weather-baseline.ppm and weather-visual.ppm; actual weather bridge state, matched camera and ROI changes; composited grass/terrain edges.

Source links:

- [src/luminumbra_client/core/scenarios/WeatherVisual.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/WeatherVisual.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- matched-clear
- matched-weather

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario weather_visual_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Show mood and depth changes with readable terrain and stationary geometry, not an indiscriminate full-frame tint.
- weather-baseline.ppm and weather-visual.ppm; actual weather bridge state, matched camera and ROI changes; composited grass/terrain edges.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture clear/weather pair with actual shared camera and stable world projections; review terrain and grass-edge readability.

## R11

**Lightning strike**

Make the bolt and localized illumination clearly visible without clipping the whole scene.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: lightning-neighbor.ppm/strike.ppm plus timed sequence and active-strike metadata, luminance regions; verify runtime trigger wiring.

Source links:

- [src/luminumbra_client/core/scenarios/LightningStrike.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/LightningStrike.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- neighbor
- strike
- recovery

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario weather_visual_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Make the bolt and localized illumination clearly visible without clipping the whole scene.
- lightning-neighbor.ppm/strike.ppm plus timed sequence and active-strike metadata, luminance regions; verify runtime trigger wiring.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Lightning capture is wired by captureLightningStrike to weather_visual_smoke. Retain neighbor, strike and recovery identities; it is not a standalone selector.

Retained evidence:

- None qualified for this catalog row.

Next action: Verify strike capture path through weather scenario; retain neighbor/strike/recovery stills and event tick, pulse intensity and clipping evidence.

## R12

**Moving cloud shadows**

Show coherent shadow movement across terrain and foliage beneath readable sky clouds.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: cloud-shadow-t0/t1.ppm plus path sequence, cloud/wind/time pins, terrain/grass matched projected shadow ROIs.

Source links:

- [src/luminumbra_client/core/scenarios/CloudShadow.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/CloudShadow.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- t0
- t1
- moving-shadow-path

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario cloud_shadow_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Show coherent shadow movement across terrain and foliage beneath readable sky clouds.
- cloud-shadow-t0/t1.ppm plus path sequence, cloud/wind/time pins, terrain/grass matched projected shadow ROIs.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Retain time-indexed moving-shadow frames and matched terrain/grass ROIs; compare observed added cost only to applicable 0.4ms target.

## R13

**Deterministic particle emitter**

Display the actual effect at a useful scale and contrast, with stable motion rather than barely detectable particles.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: particle-determinism.ppm and frame sequence; descriptor hashes, seed, particle presence/count and actual render stage.

Source links:

- [src/luminumbra_client/core/scenarios/ParticleEmitterDeterminism.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/ParticleEmitterDeterminism.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- visible-emitter
- ordered-particle-samples

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario particle_emitter_determinism_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Display the actual effect at a useful scale and contrast, with stable motion rather than barely detectable particles.
- particle-determinism.ppm and frame sequence; descriptor hashes, seed, particle presence/count and actual render stage.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Run actual emitter fixture, record visible particle ROI and deterministic descriptors; retain ordered samples beyond single presence image.

## R14

**Instanced foliage calm/wind**

Create rich but readable ground vegetation with grounded bases, plausible density and bounded wind sway.

Origin: original. Priority: P0. Execution: **deficient_baseline**. Approval: **pending**.

Fixture: foliage-instancing.ppm, calm/wind sequence, biome density and instance counts, ground contact, live-ring fade, GPU pass timings where available.

Source links:

- [src/luminumbra_client/core/scenarios/FoliageInstancing.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/FoliageInstancing.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- calm
- windy
- independent-rebuild-a
- independent-rebuild-b
- vertex-displacement
- correlated-gpu-samples

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario foliage_visual_smoke --auto-create-world --auto-enter-world --world-preset flat_lands --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Create rich but readable ground vegetation with grounded bases, plausible density and bounded wind sway.
- foliage-instancing.ppm, calm/wind sequence, biome density and instance counts, ground contact, live-ring fade, GPU pass timings where available.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Required performance scope: `foliage_draw`; 1 distinct frozen profile(s).

- `foliage_draw_ms` (profile-declared statistic) <= 0.6.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Controlled calm/wind v2 capture code and a deliberately incomplete full gate are landed. Independent rebuilds, actual vertex movement and correlated GPU samples still require recovered implementation and native qualification. Historical night-sky baseline is unchanged.

Retained evidence:

- deficient_baseline at executed source `17c70af79c96d600bde11b0fb13c59b7af15a105`; [receipt](assets/visual-baselines/20260908/receipts.json).
- [foliage-functional.png](assets/visual-baselines/20260908/foliage-functional.png) — SHA-256 `6d864644ab5f40b4fdedd4125163bcdc16049af49dbddf9b42dc98bc1b226e6f`.
- [foliage-functional.json](assets/visual-baselines/20260908/foliage-functional.json) — SHA-256 `4c4df138b8cdb3eb668e2aeead13beda3463656cfaf2cbbd23d177f613cae7ac`.

Next action: Consume terrain/foliage ownership/camera/time/wind fixes; recapture calm/windy diagnostic with final pre-draw state, independent placement evidence and truthful motion measurement.

## R15

**Time of day and season sweep**

Deliver distinct noon/dusk/night and summer/winter moods while preserving material readability and believable emissive response.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Six existing timeofday frames plus night-emissive frame; canonical sim tick/time pins, luminance/hue assertions, automated progression samples with captioned still contact sheets.

Source links:

- [src/luminumbra_client/core/scenarios/TimeOfDaySeasonSweep.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/TimeOfDaySeasonSweep.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- summer-noon
- summer-dusk
- summer-night
- winter-noon
- winter-dusk
- winter-night
- night-emissive
- progression

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario timeofday_sweep_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Deliver distinct noon/dusk/night and summer/winter moods while preserving material readability and believable emissive response.
- Six existing timeofday frames plus night-emissive frame; canonical sim tick/time pins, luminance/hue assertions, automated progression samples with captioned still contact sheets.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture six summer/winter noon/dusk/night states plus emissive control with exact existing per-state pins and actual shader time proof.

## R16

**Precipitation calm/crosswind/start-stop**

Show readable rain/slant and snow where supported, integrated into scene depth without screen-space artifacts.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: precip-calm/windy.ppm plus start/stop sequence, wind vectors and streak metrics; snow requires qualified runtime variant, not inference from a rain fixture.

Source links:

- [src/luminumbra_client/core/scenarios/Precipitation.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/Precipitation.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- calm-rain
- crosswind-rain
- start
- stop
- snow

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S3: Current lighting, material, water and weather paths; diagnose existing defects first.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario precipitation_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Show readable rain/slant and snow where supported, integrated into scene depth without screen-space artifacts.
- precip-calm/windy.ppm plus start/stop sequence, wind vectors and streak metrics; snow requires qualified runtime variant, not inference from a rain fixture.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Verify precipitation state ownership, capture calm/windy/start-stop stills, and measure slant plus scoped storm timing; qualify snow separately.

## R17

**Grounded player-view stations**

Make each existing station a compelling human-height scene showing useful near ground and horizon.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Every named player-view station capture, controller/feet/eye pose, real terrain ray support, renderable near field and water coverage; small/large relief presets.

Source links:

- [src/luminumbra_client/core/scenarios/PlayerViewSmoke.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/PlayerViewSmoke.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- all-named-stations-small-relief
- all-named-stations-large-relief

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S6: Terrain/streaming/spatial continuity, independent coverage, persistent world authority.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified. Repeat independently for default, mountains and archipelago with a fresh output directory for each preset; these are the current standing gate presets.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario player_view_smoke --auto-create-world --auto-enter-world --world-preset default --timed-run 45 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Make each existing station a compelling human-height scene showing useful near ground and horizon.
- Every named player-view station capture, controller/feet/eye pose, real terrain ray support, renderable near field and water coverage; small/large relief presets.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture every existing player-view station at grounded pose across supported presets, checking current terrain support and near-field coverage.

## R18

**Far terrain horizon and water seam**

Show a coherent long-distance world whose near/far terrain, fog and water join convincingly.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Existing far-horizon station captures and boundary-band water analysis, camera/coverage, elevated and ground views, transition path.

Source links:

- [src/luminumbra_client/core/scenarios/FarLodHorizonSmoke.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/FarLodHorizonSmoke.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- ground
- elevated
- water-boundary
- transition-path

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S6: Terrain/streaming/spatial continuity, independent coverage, persistent world authority.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified. Repeat independently for default, mountains and archipelago with a fresh output directory for each preset; these are the current standing gate presets.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario farlod_horizon_smoke --auto-create-world --auto-enter-world --world-preset default --timed-run 50 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Show a coherent long-distance world whose near/far terrain, fog and water join convincingly.
- Existing far-horizon station captures and boundary-band water analysis, camera/coverage, elevated and ground views, transition path.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Pair far-horizon and boundary-water views with depth/material and residency evidence; add transition samples after the independent coverage diagnosis.

## R19

**Skinned mesh two-pose visual**

Make silhouette, UVs/normals and actual deformation unambiguous under good lighting.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: skinned-mesh-a/b.ppm, fixed poses and animation samples, mesh/texture identities, world placement, animation palette assertions; synthetic fixture labeled diagnostic.

Source links:

- [src/luminumbra_client/core/scenarios/SkinnedMeshVisualSmoke.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/SkinnedMeshVisualSmoke.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- pose-a
- pose-b
- intermediate-poses

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario skinned_mesh_visual_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Make silhouette, UVs/normals and actual deformation unambiguous under good lighting.
- skinned-mesh-a/b.ppm, fixed poses and animation samples, mesh/texture identities, world placement, animation palette assertions; synthetic fixture labeled diagnostic.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture synthetic skinned A/B poses and intermediate deformation samples with texture identity; keep distinct from production character acceptance.

## R20

**Creature gameplay slice**

Present an actual recognizable game creature reacting visibly to the implemented stimulus in a composed environment.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: creature-slice-before/after.ppm and existing motion sequence, archetype/content hashes, stimulus/AI-state/pose/position timeline; no claim of unimplemented game-code extraction.

Source links:

- [src/luminumbra_client/core/scenarios/CreatureSliceSmoke.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/CreatureSliceSmoke.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- before
- stimulus
- reaction
- state-trajectory

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified. First compile the shipped grovestrider and glow_bloom glTF inputs into the matching runtime lmesh files, using the pinned asset_processor; identify the output hashes.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario creature_slice_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output> --creature-archetype data/common/archetypes/grovestrider.json
```

Acceptance checks:

- Present an actual recognizable game creature reacting visibly to the implemented stimulus in a composed environment.
- creature-slice-before/after.ppm and existing motion sequence, archetype/content hashes, stimulus/AI-state/pose/position timeline; no claim of unimplemented game-code extraction.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Verify currently supported archetype/stimulus assets, capture before/after plus selected measured reaction poses; do not imply game-code extraction or new behavior system.

## R21

**Persistence visible roundtrip**

Show a clearly recognizable terrain edit surviving a real restart without confusing camera changes.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Save/load phase frames with identical framing, file hashes and world identity; UI sequence from campaign separately qualified; corruption refusal UI captured in U03.

Source links:

- [src/luminumbra_client/core/scenarios/PersistenceRoundtrip.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/PersistenceRoundtrip.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- edited-before-save
- restart
- loaded-matched-camera

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **missing**. The scenario selector and numeric/transition receipts exist, but retained original pixels for this complete visual claim are not wired in the current capture orchestrator.

Acceptance checks:

- Show a clearly recognizable terrain edit surviving a real restart without confusing camera changes.
- Save/load phase frames with identical framing, file hashes and world identity; UI sequence from campaign separately qualified; corruption refusal UI captured in U03.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: The runtime roundtrip requires separate save and load processes with a shared disposable persistence-session directory. Numeric phase receipts exist; matching before/restart/after pixels need capture wiring.

Retained evidence:

- None qualified for this catalog row.

Next action: Use disposable saved-world fixture and identical before/restart/after camera, attach file/world digests and actual user-flow stills.

## R22

**Rendered network session**

Show the same server-owned world/state visibly in participating clients with clear identities.

Origin: original. Priority: P3. Execution: **not_captured**. Approval: **pending**.

Fixture: Matched tick client frames and movement/action sequence, server/client hashes and transport proof. Driver presence is not confirmation of an existing image producer; add capture wiring if absent.

Source links:

- [src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- client-a-matched-tick
- client-b-matched-tick
- replicated-action

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S6: Terrain/streaming/spatial continuity, independent coverage, persistent world authority.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show the same server-owned world/state visibly in participating clients with clear identities.
- Matched tick client frames and movement/action sequence, server/client hashes and transport proof. Driver presence is not confirmation of an existing image producer; add capture wiring if absent.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Network driver and state/transport checks exist; matching-tick retained client image capture and a complete motion packet remain unqualified.

Retained evidence:

- None qualified for this catalog row.

Next action: Confirm actual client image capture wiring for existing network driver; retain matching-tick client views with server/transport state, add capture plumbing if absent.

## R23

**Window-mode transition**

Keep UI/world crisp and correctly laid out through supported window arrangements and resize.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: window-mode-stress-final.ppm plus before/during/after framebuffer/window dimensions and layout captures; prove interactive resolution separately because scenario capture pins framebuffer.

Source links:

- [src/luminumbra_client/core/scenarios/WindowModeStressSmoke.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/scenarios/WindowModeStressSmoke.cpp)
- [src/luminumbra_client/core/ScenarioRunnerCapture.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/src/luminumbra_client/core/ScenarioRunnerCapture.cpp)

Required variants:

- before
- during
- after
- interactive-resolution

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **existing_subset**. Source-wired still producer only; command has not been rerun for this catalog. Review source pins and workload before capture; variants, motion evidence and performance remain unqualified.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<native-runtime>/luminumbra_client_qa_app.exe --scenario window_mode_stress_smoke --auto-create-world --auto-enter-world --timed-run 30 --no-audio --no-ui --no-menu-backdrop --hidden-window --runtime-artifact-dir <fresh-output>
```

Acceptance checks:

- Keep UI/world crisp and correctly laid out through supported window arrangements and resize.
- window-mode-stress-final.ppm plus before/during/after framebuffer/window dimensions and layout captures; prove interactive resolution separately because scenario capture pins framebuffer.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Source fixture or supporting assertions are present. Current native screenshots, all required variants and performance have not been qualified for this catalog.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture before/after actual supported window-mode changes and UI layout, recording both native window and pinned framebuffer sizes.

## R24

**Expanded distant world and edit authority**

Implement and qualify volumetric generation, meshing, scheduling and independent coverage through 16,384 m, then durable edit authority and residency/upload budgets.

Origin: added. Parents: R03, R04, R05, R18, D20, D22. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Implement and qualify volumetric generation, meshing, scheduling and independent coverage through 16,384 m, then durable edit authority and residency/upload budgets. Author and version the fixture with GLB plus versioned sidecars and immutable generations.

Source links:

- Missing: this fixture still needs implementation and a reproducible producer.

Required variants:

- five-tiers-through-16384m
- arrival-fallback
- caves
- edit-propagation
- durable-overlays
- tombstones
- residency-budget
- upload-budget

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S6: Terrain/streaming/spatial continuity, independent coverage, persistent world authority.

Capture: **missing**. Implementation and a reproducible installed/native capture producer remain required.

Acceptance checks:

- Implement and qualify volumetric generation, meshing, scheduling and independent coverage through 16,384 m, then durable edit authority and residency/upload budgets.
- Meet the shared packet contract for every required variant and parent mapping.

Required performance scope: `accepted_expanded_world`; 2 distinct frozen profile(s).

- `frame_ms` (p99) <= 16.67; also report target 8.33.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Authorized September 13 completion scope. This row adds explicit coverage without replacing any original group; implementation and acceptance remain unfinished.

Retained evidence:

- None qualified for this catalog row.

Next action: Identify the concrete missing feature and dependency, implement in dependency order, then prepare an individually reviewable native packet.

## R25

**Persistent coarse simulation continuation**

Reconcile the coarse simulation integration without losing landed telemetry; prove game-owned activation data and deterministic persistent continuation using the existing clock.

Origin: added. Parents: R20, R21, R22, G07. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Reconcile the coarse simulation integration without losing landed telemetry; prove game-owned activation data and deterministic persistent continuation using the existing clock. Author and version the fixture with GLB plus versioned sidecars and immutable generations.

Source links:

- Missing: this fixture still needs implementation and a reproducible producer.

Required variants:

- region-field-pages
- persistent-wildlife
- plant-water-scheduling
- ground-objects
- complete-hashes
- save-N-load-K
- uninterrupted-N-plus-K
- negative-controls

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S6: Terrain/streaming/spatial continuity, independent coverage, persistent world authority.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.

Capture: **missing**. Implementation and a reproducible installed/native capture producer remain required.

Acceptance checks:

- Reconcile the coarse simulation integration without losing landed telemetry; prove game-owned activation data and deterministic persistent continuation using the existing clock.
- Meet the shared packet contract for every required variant and parent mapping.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Authorized September 13 completion scope. This row adds explicit coverage without replacing any original group; implementation and acceptance remain unfinished.

Retained evidence:

- None qualified for this catalog row.

Next action: Identify the concrete missing feature and dependency, implement in dependency order, then prepare an individually reviewable native packet.
