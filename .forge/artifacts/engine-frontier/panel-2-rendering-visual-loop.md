## Subsystem State

Luminumbra has a working deferred render path with explicit pass functions for shadow, G-buffer, SSAO, lighting, water, skybox, and final blit. The frame order is centralized in `RenderPipeline::render_frame`, and pass stats plus pass metadata already exist, so the next rendering work should be gate-first rather than exploratory.

The current visual loop has closed the largest LOD holes and basic water visibility. The remaining blocker is material appearance, especially Sand versus flat grey fallback. The first Wave 1 implementation task must be a deterministic material/texture-layer visual gate before shader refactors, pass extraction, or broader visual polish.

Water is visible and submitted, but quality is still mostly fallback-driven: the water pass binds neutral or black 1x1 textures for normal, flow, caustics, foam, and underwater inputs. The shader contains SSR, depth tint, foam, caustics, and underwater branches, but the current gate proves visibility, not that those feature paths are visually meaningful.

The shader tree contains several newer feature shaders that are not constructed by `RenderPipeline`. Some related ideas are embedded in the active lighting or water shaders, but the standalone feature shaders are currently dead assets unless a future pass explicitly wires and gates them.

## Findings

1. `RenderPipeline.cpp` is a single large owner for startup, pass execution, resources, texture arrays, water fallbacks, shader health, and GPU SDF. Startup initializes shaders, FBOs, G-buffer, shadows, SSAO, skybox, terrain textures, material LUT, water fallbacks, and GPU SDF in one sequence (`src/luminumbra_client/rendering/RenderPipeline.cpp:287-306`). The header also keeps pass declarations and resource fields in one class, including pass functions (`src/luminumbra_client/rendering/RenderPipeline.h:264-291`) and texture/LUT/water/SDF resources (`src/luminumbra_client/rendering/RenderPipeline.h:338-368`).

2. The frame path is already pass-shaped even though ownership is monolithic. `render_frame` executes shadow, G-buffer, SSAO, lighting, opaque copy/depth blit, water, skybox, and final blit in order (`src/luminumbra_client/rendering/RenderPipeline.cpp:734-781`). `refresh_render_pass_metadata` already emits pass names, inputs, outputs, dimensions, clear/load-store policy, and draw counts for shadow, gbuffer, ssao, ssao_blur, lighting, water, skybox, and final_blit (`src/luminumbra_client/rendering/RenderPipeline.cpp:544-585`).

3. The current material path can still hide grey fallback visually. The G-buffer shader defaults albedo to `vec3(0.7)` before switching known material IDs, including Sand at ID 4 (`res/shaders/g_buffer.frag:50-57`). The lighting shader replaces albedo with tri-planar terrain texture layers only when `MaterialID > 0` and `MaterialID <= terrainLayerCount` (`res/shaders/lighting_pass.frag:162-168`). The texture array hard-codes Sand as MaterialID 4 to texture layer index 3 (`src/luminumbra_client/rendering/RenderPipeline.cpp:1987-1995`), and `materials.json` defines Sand with ID 4 and the same sand albedo map (`data/common/materials.json:33-39`).

4. The material visual gate is not optional. `next-plan.md` says material appearance, especially sand/grey fallback, still lacks a deterministic heatmap or screenshot gate (`.forge/artifacts/visual-performance-improvement/next-plan.md:31-35`) and makes Loop 4 the immediate next task (`.forge/artifacts/visual-performance-improvement/next-plan.md:122-135`, `.forge/artifacts/visual-performance-improvement/next-plan.md:174-176`). The engine-frontier validator already defines the acceptance contract: `material-visual-analysis.json`, schema `luminumbra.material_visual_analysis.v1`, zero GL debug errors, nonempty materials array, mandatory Sand ROI, per-entry `pixels` and `thresholds`, `classified_pixels >= min_classified_pixels`, `grey_fallback_pixels <= max_grey_fallback_pixels`, `passed=true`, and existing screenshot plus heatmap paths (`.forge/scripts/validate-engine-frontier.ps1:168-210`).

5. Existing runtime scenarios are the right launch-flag family for the material gate. `RuntimeScenarioConfig` recognizes `lod_ground_smoke` and `water_visual_smoke` (`src/luminumbra_client/main_client.cpp:233-237`), assigns them a default 60-second timed run (`src/luminumbra_client/main_client.cpp:256-257`), and auto-creates/enters worlds for those scenarios (`src/luminumbra_client/main_client.cpp:259-262`). The screenshot/analysis loop already captures LOD screenshots and writes `lod-ground-visual-analysis.json` (`src/luminumbra_client/main_client.cpp:2389-2414`) and captures water screenshot plus `water-visual-analysis.json` once water draw stats and target selection are valid (`src/luminumbra_client/main_client.cpp:2417-2438`).

6. Runtime render health exists but does not cover the full shader suite. `RuntimeRenderStats` tracks only geometry, lighting, skybox, shadow, SSAO, SSAO blur, water, instanced static mesh, GPU SDF, terrain texture array, material LUT, and fallback layer count (`src/luminumbra_client/rendering/RenderPipeline.h:188-211`). `get_shader_health` publishes only geometry, lighting, skybox, shadow, SSAO, SSAO blur, water, and instanced static mesh (`src/luminumbra_client/rendering/RenderPipeline.cpp:439-460`), and `main_client` serializes the same narrow set (`src/luminumbra_client/main_client.cpp:565-587`).

7. The wired shader set is smaller than the shader directory. `init_shaders` constructs only `g_buffer`, `lighting_pass`, `skybox`, `shadow_map`, `ssao`, `ssao_blur`, and `water` (`src/luminumbra_client/rendering/RenderPipeline.cpp:1233-1248`); instanced static meshes are initialized separately with `instanced_mesh.vert` plus `g_buffer.frag` (`src/luminumbra_client/rendering/RenderPipeline.cpp:308-311`). There is no bloom, volumetric, weather, enhanced skybox, standalone caustics, magical particle, or standalone SSR program in the active render pipeline.

8. Several newer shaders are dead standalone assets today. `volumetric_lighting.frag` expects G-buffer, depth, shadow cascades, sun, and raymarch settings (`res/shaders/volumetric_lighting.frag:6-30`) and raymarches from camera to scene depth (`res/shaders/volumetric_lighting.frag:107-130`), but no volumetric pass appears in `init_shaders` or pass metadata. `weather_system.frag` expects scene color/depth, G-buffer, weather uniforms, and post-processes fog/rain/snow/lightning (`res/shaders/weather_system.frag:7-30`, `res/shaders/weather_system.frag:242-272`), but no weather pass exists. `enhanced_skybox.frag` has enhanced clouds, stars, and aurora (`res/shaders/enhanced_skybox.frag:59-94`, `res/shaders/enhanced_skybox.frag:119-143`), while the active skybox pass loads `skybox.frag` (`src/luminumbra_client/rendering/RenderPipeline.cpp:1236`).

9. Caustics and SSR are partially present but not feature-complete. `caustics_generator.frag` can generate animated caustic patterns (`res/shaders/caustics_generator.frag:133-154`), but lighting binds `m_water_black_texture` to `u_causticsTexture` (`src/luminumbra_client/rendering/RenderPipeline.cpp:1131-1142`). Water also binds black fallback to `u_caustics_texture` and `u_foam_texture`, neutral flow to `u_flow_map`, and flat normal to `u_normal_map` (`src/luminumbra_client/rendering/RenderPipeline.cpp:896-914`). `screen_space_reflections.frag` contains a higher-quality SSR trace with 64 max steps and binary refinement (`res/shaders/screen_space_reflections.frag:24-31`, `res/shaders/screen_space_reflections.frag:56-64`), but water uses its own reduced inline SSR loop (`res/shaders/water.frag:71-107`) and the standalone SSR shader is not loaded.

10. Magical particle shaders are dead, while magical crystal shading is embedded in lighting. The particle vertex, geometry, and fragment shaders define particle attributes, billboarding, and sparkle/ember/magic/crystal shapes (`res/shaders/magical_particles.vert:3-19`, `res/shaders/magical_particles.geom:3-24`, `res/shaders/magical_particles.frag:36-43`), but no particle shader program is constructed. By contrast, active lighting adds crystal glow for MaterialID 6 (`res/shaders/lighting_pass.frag:213-239`), and the material LUT marks ID 6 as luminous crystal (`src/luminumbra_client/rendering/RenderPipeline.cpp:2061-2064`).

11. Basic water visibility is gated, but water quality is not. The water analysis currently passes on target found, nonzero water draw submission, water-like pixel count and ratio, and zero GL errors (`src/luminumbra_client/main_client.cpp:1494-1545`). `next-plan.md` explicitly says the current water gate proves visibility, not beauty, and lower-angle quality validation remains open (`.forge/artifacts/visual-performance-improvement/next-plan.md:136-151`). The shader has depth absorption and minimum tint controls (`res/shaders/water.frag:113-180`), with uniforms set in the pass (`src/luminumbra_client/rendering/RenderPipeline.cpp:882-885`).

12. GPU SDF is wired separately from the visual shader suite. The render pipeline loads `sdf_generation.compute` into a compute program (`src/luminumbra_client/rendering/RenderPipeline.cpp:2137-2168`) and dispatch code sets chunk/worldgen uniforms (`src/luminumbra_client/rendering/RenderPipeline.cpp:2261-2295`). The compute shader uses the same solid-negative/empty-positive convention called out in shader comments (`res/shaders/sdf_generation.compute:104-115`), so future terrain visual gates should keep GPU/CPU SDF parity in the render-health envelope.

## Must-Fix

1. [Wave 1 candidate, FIRST task] Implement `material_visual_smoke` and the deterministic material/texture-layer visual gate before any pass extraction or shader polish. Design:
   - Add `RuntimeScenarioConfig::material_visual_smoke()` alongside `lod_ground_smoke` and `water_visual_smoke`; give it the same 60-second default timed run and auto-create/auto-enter behavior.
   - Default artifact dir: `build/debug/test-artifacts/runtime/material-visual`.
   - Capture `screenshots/material-visual.ppm` from the backbuffer after terrain is ready and the camera is locked to deterministic material ROIs. Sand must be one ROI. Prefer selecting ROI targets from actual chunk material data; fallback only to a deterministic authored terrain patch if sampling cannot find Sand.
   - Capture `screenshots/material-id-heatmap.ppm` in the same run. The heatmap should encode material ID or texture layer, not final lit color, so Sand ID 4/layer 3 can be diagnosed even when lighting changes.
   - Write `material-visual-analysis.json` with:

```json
{
  "schema": "luminumbra.material_visual_analysis.v1",
  "passed": true,
  "screenshot": "screenshots/material-visual.ppm",
  "heatmap_screenshot": "screenshots/material-id-heatmap.ppm",
  "gl_debug": { "messages": 0, "errors": 0, "warnings": 0, "notifications": 0 },
  "materials": [
    {
      "name": "Sand",
      "material_id": 4,
      "expected_texture_layer": 3,
      "roi": { "x": 0, "y": 0, "width": 0, "height": 0 },
      "pixels": {
        "roi_pixels": 0,
        "classified_pixels": 0,
        "grey_fallback_pixels": 0
      },
      "thresholds": {
        "min_classified_pixels": 2500,
        "max_grey_fallback_pixels": 0
      }
    }
  ]
}
```

   - Classifier contract: `classified_pixels` should require both material heatmap agreement and final-color plausibility for the expected material. `grey_fallback_pixels` should count low-saturation neutral grey pixels in the final screenshot inside the same ROI, for example small RGB channel deltas at mid luma. Sand is the primary failure target, but Stone, Soil, Grass, and Deepslate should be included once Sand is stable.
   - Validator: `.forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual` must pass without weakening its existing contract.

2. [Wave 1 candidate] Add shader-suite health that distinguishes `wired`, `compiled_only`, and `dead_asset`. The current runtime shader health reports only active core programs, so broken or obsolete shaders can ship unnoticed. The gate should compile/link every declared shader program pair or explicitly classify files as intentionally unused. Standalone post shaders need a known full-screen vertex harness; particle shaders need vert/geom/frag link validation.

3. [Wave 1 candidate] Freeze a render-health baseline before modularizing `RenderPipeline.cpp`. Required baseline: pass metadata, pass stats, shader health, GL debug counters, material gate result, `LodGround` result, `WaterVisual` result, and screenshots. This blocks safe extraction because the monolith already has subtle ordering dependencies around the opaque lighting copy, G-buffer depth blit, and water composition.

4. [Wave 1 candidate] Convert water quality claims into feature-specific gates before accepting SSR, caustics, foam, or depth tint as complete. Today the pass binds fallback textures for the same inputs that drive those effects. Shipping water as "quality complete" requires gates that can fail if SSR contributes nothing, caustics are black, or depth tint collapses into a flat blue overlay.

5. [Wave 1 candidate] Keep zero GL debug errors as a hard visual-gate requirement. MaterialVisual should copy the water analysis pattern and fail on `gl_debug.errors != 0`; render refactors should not be allowed to hide GL state errors behind successful draw counts.

## Deepening Opportunities

1. Staged no-behavior-change modularization:
   - Stage 0: capture `render-health-baseline.json` from `lod_ground_smoke`, `water_visual_smoke`, and `material_visual_smoke`.
   - Stage 1: extract shadow pass ownership into a small pass object using existing `ShadowMap`, cascade matrices, culling results, and counters. No shader or GL state behavior change.
   - Stage 2: extract G-buffer terrain/static mesh pass plus material texture/LUT bindings. Preserve G-buffer attachment formats and draw counters byte-for-byte.
   - Stage 3: extract SSAO and SSAO blur. Preserve kernel generation, noise texture, raw/blur attachments, and pass metadata.
   - Stage 4: extract lighting and water only after MaterialVisual and WaterVisual stay green. Keep opaque color copy and depth blit ordering fixed.
   - Stage 5: extract skybox/final blit/post shell. Bloom or weather should remain disabled until a gate proves them.

2. Make `materials.json` the material source of truth. `materials.json`, `init_terrain_textures`, `init_material_lut`, and `g_buffer.frag` currently duplicate IDs and properties. After the material gate exists, move texture layer paths and LUT values toward generated or loaded data so Sand ID 4/layer 3 cannot drift again.

3. Promote material-ID heatmap capture into a reusable debug capture mode. The same mechanism can gate terrain LOD holes, invalid material IDs, mixed-LOD seams, and future biome/material transitions without relying only on final lit pixels.

4. Replace water fallback inputs incrementally. First wire a generated caustics texture from `caustics_generator.frag`, then real normal/flow maps from water simulation or authored textures, then foam. Each step should preserve the existing visibility gate and add one feature-specific pixel or histogram assertion.

5. Add GL state assertions per pass during debug runs. The current pass sequence mutates cull face, depth mask, blend, framebuffer, viewport, and depth func across passes. A lightweight state snapshot in render-health artifacts would make pass extraction safer.

6. Add screenshot analysis helpers rather than more one-off classifiers. `main_client.cpp` already has water and LOD pixel classifiers; MaterialVisual should generalize ROI bounds, connected component counts, per-material thresholds, and heatmap correlation.

## Frontier Proposals

1. Gate-first render graph abstraction. Do not replace the pass order with a graph until `render-health-baseline.json` can prove the graph emits the same pass metadata, draw counts, resource dimensions, GL debug results, and screenshot classifications as the current imperative path.

2. Gate-first GPU timing telemetry per pass. Add timestamp queries around shadow, gbuffer, ssao, ssao_blur, lighting, water, skybox, and final_blit, but first gate query availability and ensure timing collection can be disabled for normal smoke runs.

3. Gate-first screenshot-diff regression harness. Build a deterministic corpus from `lod_ground_smoke`, `water_visual_smoke`, and `material_visual_smoke`; compare structured ROIs and material/heatmap classifications rather than whole-frame fragile diffs.

4. Gate-first shader feature matrix. Track every shader file as `active`, `inactive but compiling`, `inactive and exempt`, or `removed`. New feature shaders should not be wired until their matrix row names a deterministic visual gate.

5. Gate-first water quality ladder. Introduce separate quality profiles for visibility, low-angle reflection, shallow caustics, depth tint, and foam. Each profile should have its own artifact thresholds before the water shader grows further.

## Proposed Gates

1. Material visual gate: deterministic scenario `material_visual_smoke`; artifact `build/debug/test-artifacts/runtime/material-visual/material-visual-analysis.json`; validator `.forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual`.

2. Material heatmap gate: same `material_visual_smoke`; artifact `screenshots/material-id-heatmap.ppm` referenced by `material-visual-analysis.json`; validator `.forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual`.

3. LOD visual regression gate: deterministic scenario `lod_ground_smoke`; artifact `build/debug/test-artifacts/runtime/lod-ground-baseline/lod-ground-visual-analysis.json`; validator mode `LodGround` in the runtime stability validator.

4. Water visibility gate: deterministic scenario `water_visual_smoke`; artifact `build/debug/test-artifacts/runtime/water-visual/water-visual-analysis.json`; validator mode `WaterVisual` in the runtime stability validator.

5. Water quality gate: deterministic scenario `water_quality_smoke` or `water_visual_smoke --water-quality-profile low_angle_ssr`; artifact `build/debug/test-artifacts/runtime/water-quality/water-quality-analysis.json`; validator mode `WaterQuality`.

6. Shader suite health gate: deterministic validator mode `ShaderSuiteHealth`; artifact `build/debug/test-artifacts/render/shader-suite-health.json`; required fields `shader`, `stage_set`, `compile_ok`, `link_ok`, `wired_pass`, `visual_gate`, and `status`.

7. Render pass no-behavior-change gate: deterministic scenario set `lod_ground_smoke`, `water_visual_smoke`, and `material_visual_smoke`; artifact `build/debug/test-artifacts/render/render-health-analysis.json`; validator mode `RenderHealth`.

8. Pass timing telemetry gate: deterministic scenario `render_timing_smoke`; artifact `build/debug/test-artifacts/performance/render-pass-timing.json`; validator mode `RenderTiming`.

9. Screenshot regression gate: deterministic scenario `visual_regression_smoke`; artifact `build/debug/test-artifacts/runtime/visual-regression/visual-regression-analysis.json`; validator mode `VisualRegression`.

10. Dead shader inventory gate: deterministic validator mode `ShaderInventory`; artifact `build/debug/test-artifacts/render/shader-inventory.json`; it must fail if a shader file is neither wired, compile-validated, exempted with rationale, nor removed.

## References

- `src/luminumbra_client/rendering/RenderPipeline.h`
- `src/luminumbra_client/rendering/RenderPipeline.cpp`
- `src/luminumbra_client/rendering/Shader.h`
- `src/luminumbra_common/systems/WaterSystem.h`
- `src/luminumbra_client/main_client.cpp`
- `res/shaders/g_buffer.frag`
- `res/shaders/lighting_pass.frag`
- `res/shaders/water.frag`
- `res/shaders/ssao.frag`
- `res/shaders/bloom_composite.frag`
- `res/shaders/sdf_generation.compute`
- `res/shaders/volumetric_lighting.vert`
- `res/shaders/volumetric_lighting.frag`
- `res/shaders/weather_system.frag`
- `res/shaders/enhanced_skybox.frag`
- `res/shaders/caustics_generator.frag`
- `res/shaders/screen_space_reflections.frag`
- `res/shaders/magical_particles.vert`
- `res/shaders/magical_particles.geom`
- `res/shaders/magical_particles.frag`
- `data/common/materials.json`
- `.forge/artifacts/visual-performance-improvement/handoff.md`
- `.forge/artifacts/visual-performance-improvement/next-plan.md`
- `.forge/artifacts/engine-framework-roadmap/ultimate-plan.md`
- `.forge/scripts/validate-engine-frontier.ps1`
- `C:/Users/David/.codex/plugins/cache/personal/forge/0.17.0/skills/forge-cli/SKILL.md`
