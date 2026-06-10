You are Codex, running as the sole AI agent for the Luminumbra engine-frontier workflow.

Goal:
Research panel 2 of 7: Rendering Pipeline, Shaders, Water, and the Visual
Quality Loop. This is an EMPHASIS panel — produce roughly twice the depth of a
standard panel and bias your Must-Fix and Deepening items toward Wave 1
implementation candidates. Use only Codex reasoning. Do not invoke, recommend,
or route to any non-Codex AI agent.

Workspace:
D:\Coding\luminumbra

Read these files first:
- src/luminumbra_client/rendering/RenderPipeline.h
- src/luminumbra_client/rendering/RenderPipeline.cpp
- src/luminumbra_client/rendering/Shader.h
- src/luminumbra_common/systems/WaterSystem.h
- src/luminumbra_client/main_client.cpp (runtime scenarios: lod_ground_smoke, water_visual_smoke, screenshot analysis)
- res/shaders/g_buffer.frag
- res/shaders/lighting_pass.frag
- res/shaders/water.frag
- res/shaders/ssao.frag
- res/shaders/bloom_composite.frag
- res/shaders/sdf_generation.compute
- data/common/materials.json
- .forge/artifacts/visual-performance-improvement/handoff.md
- .forge/artifacts/visual-performance-improvement/next-plan.md
- .forge/artifacts/engine-framework-roadmap/ultimate-plan.md
- .forge/scripts/validate-engine-frontier.ps1 (the MaterialVisual mode defines the JSON contract you must design toward)

Research focus:
1. HARD REQUIREMENT: the deterministic material/texture-layer visual gate from
   next-plan.md Loop 4 is the FIRST implementation task of this workflow. It
   MUST appear in your Must-Fix list with a concrete design:
   - a `material_visual_smoke` runtime scenario (same launch-flag family as
     lod_ground_smoke / water_visual_smoke),
   - a `material-visual-analysis.json` artifact with schema
     `luminumbra.material_visual_analysis.v1` containing `passed`,
     `gl_debug.errors`, `screenshot`, `heatmap_screenshot`, and a `materials`
     array of per-material ROI entries with `pixels.classified_pixels`,
     `pixels.grey_fallback_pixels` and matching `thresholds` — a Sand entry is
     mandatory (sand vs grey fallback is the primary failure being gated),
   - a material-ID heatmap capture for diagnosis.
   Read Test-MaterialVisual in .forge/scripts/validate-engine-frontier.ps1 and
   design so that gate passes.
2. RenderPipeline.cpp is a very large monolith. Propose a staged
   modularization (pass extraction: shadow, gbuffer, ssao, lighting, water,
   post) with strict no-behavior-change waves and the render-health artifacts
   that prove it.
3. Shader suite health: the newer shaders (volumetric_lighting, weather_system,
   enhanced_skybox, caustics_generator, magical_particles,
   screen_space_reflections) — which are wired into the pipeline, which are
   dead, and what gates would prove each visually.
4. Water rendering: the visual loop proved water draws and pixel ratios; what
   remains for water quality (SSR integration, caustics, depth tint curve) and
   how would each be gated?
5. Boundary-pushing proposals: render graph abstraction, GPU timing telemetry
   per pass, screenshot-diff regression harness over the existing capture
   scenarios.

Required output:
Write exactly one file: .forge/artifacts/engine-frontier/panel-2-rendering-visual-loop.md
If the file already exists, replace its content entirely; do not append.

The file must contain these H2 sections, in order:
## Subsystem State
## Findings
## Must-Fix
## Deepening Opportunities
## Frontier Proposals
## Proposed Gates
## References

Content requirements:
1. Findings must cite concrete file:line evidence.
2. Must-Fix items are defects or gaps that block shipping; Deepening
   Opportunities improve an already-working system; Frontier Proposals are
   exploratory and must be marked gate-first.
3. Every Proposed Gate must name the deterministic scenario, artifact JSON, or
   validator mode that would enforce it.
4. References must list every file you actually read.

Rules:
- Do not modify engine source, tests, build files, or other Forge artifacts in
  this run. Your only write target is the single panel file above.
- Keep the report concise and evidence-dense; no filler.
