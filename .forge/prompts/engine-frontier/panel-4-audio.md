You are Codex, running as the sole AI agent for the Luminumbra engine-frontier workflow.

Goal:
Research panel 4 of 7: Audio stack deepening. Use only Codex reasoning. Do not
invoke, recommend, or route to any non-Codex AI agent.

Workspace:
D:\Coding\luminumbra

Read these files first:
- src/luminumbra_client/audio/MiniaudioManager.h
- src/luminumbra_client/audio/MiniaudioManager.cpp
- src/luminumbra_client/audio/AudioSpatialCluster.h
- src/luminumbra_client/audio/AudioSpatialCluster.cpp
- src/luminumbra_client/audio/AudioPropagationSystem.h
- src/luminumbra_client/audio/AdvancedReverbSystem.h
- src/luminumbra_client/audio/EnvironmentalAudioSystem.h
- src/luminumbra_client/audio/AudioStreamingManager.h
- src/luminumbra_client/audio/AudioPerformanceProfiler.h
- src/luminumbra_common/components/AudioComponents.h
- docs/ENHANCED_AUDIO_SYSTEM.md
- docs/ENHANCED_AUDIO_PHYSICS_INTEGRATION.md
- .forge/artifacts/engine-framework-roadmap/ultimate-plan.md

Research focus:
1. The doppler/reverb TODO in AudioSpatialCluster: what does completing it
   require, and what proves it works?
2. Physics integration: occlusion and reflection-point raycasts — correctness,
   batching cost, terrain-material absorption mapping vs materials.json.
3. Headless testability: the engine runs smokes with --no-audio; design an
   audio telemetry path (counts of active voices, cluster sizes, propagation
   raycasts, underruns) that can be asserted deterministically without a
   physical audio device.
4. Streaming and memory: ring buffer behavior, variation-system memory
   footprint, profiler coverage gaps.
5. Boundary-pushing proposals: convolution reverb from world geometry,
   procedural ambience driven by biome/water state, audio golden-trace
   regression gate.

Required output:
Write exactly one file: .forge/artifacts/engine-frontier/panel-4-audio.md
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
