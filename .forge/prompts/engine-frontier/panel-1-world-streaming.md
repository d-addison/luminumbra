You are Codex, running as the sole AI agent for the Luminumbra engine-frontier workflow.

Goal:
Research panel 1 of 7: World Generation, Streaming, and Job System. This is an
EMPHASIS panel — produce roughly twice the depth of a standard panel and bias
your Must-Fix and Deepening items toward Wave 1 implementation candidates.
Use only Codex reasoning. Do not invoke, recommend, or route to any non-Codex
AI agent.

Workspace:
D:\Coding\luminumbra

Read these files first:
- src/luminumbra_common/systems/SHIELD_WorldSystem.h
- src/luminumbra_common/systems/SHIELD_WorldSystem.cpp
- src/luminumbra_common/world/MarchingCubes.h
- src/luminumbra_common/world/MarchingCubes.cpp
- src/luminumbra_common/world/Chunk.h
- src/luminumbra_common/core/JobSystem.h
- src/luminumbra_common/core/JobSystem.cpp
- src/luminumbra_client/rendering/RenderPipeline.h (streaming/upload telemetry surface)
- test/shield/test_world_generation.cpp
- test/performance/initial_world_loading_perf_test.cpp
- .forge/artifacts/visual-performance-improvement/handoff.md
- .forge/artifacts/visual-performance-improvement/next-plan.md
- .forge/artifacts/engine-framework-roadmap/ultimate-plan.md

Research focus:
1. Streaming drain behavior: upload queue depth, deferred uploads, coalescing
   windows, distance-first priority. What pushes the engine toward the
   endurance-300 gate passing reliably, and what still backs up under load?
2. LOD correctness and hysteresis: LOD0/LOD1/LOD2 transitions, per-cell surface
   ownership in the coarse heightfield LOD, flicker or popping risk at ring
   boundaries.
3. Meshing throughput: marching cubes job batching, generation vs meshing
   budget split, opportunities for SIMD, table-driven fast paths, or
   incremental remeshing.
4. Job system frontier: work stealing, priority lanes, job affinity,
   instrumentation gaps in RuntimeStats.
5. Boundary-pushing proposals: what would take this subsystem beyond its
   current design (e.g. GPU-driven meshing, async readback, predictive
   prefetch along camera velocity)?

Required output:
Write exactly one file: .forge/artifacts/engine-frontier/panel-1-world-streaming.md
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
   exploratory and must be marked gate-first (the deterministic gate is built
   before the feature).
3. Every Proposed Gate must name the deterministic scenario, artifact JSON, or
   validator mode that would enforce it (follow the pattern of
   lod_ground_smoke / lod-ground-visual-analysis.json and
   .forge/scripts/validate-runtime-stability-phase-1.ps1 modes).
4. References must list every file you actually read.

Rules:
- Do not modify engine source, tests, build files, or other Forge artifacts in
  this run. Your only write target is the single panel file above.
- Keep the report concise and evidence-dense; no filler.
