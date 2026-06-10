You are Codex, running as the sole AI agent for the Luminumbra engine-frontier workflow.

Goal:
Research panel 6 of 7: Persistence, GPU SDF compute integration, and
Networking/Server. This is a FRONTIER panel: these systems are partially built
or stubs. Mark every proposal gate-first (the deterministic gate is built
before or alongside the feature) and keep scopes small enough for
single-dispatch tasks. Use only Codex reasoning. Do not invoke, recommend, or
route to any non-Codex AI agent.

Workspace:
D:\Coding\luminumbra

Read these files first:
- src/luminumbra_common/world/Chunk.h (voxel storage, LZ4 compression — the natural serialization unit)
- src/luminumbra_common/systems/SHIELD_WorldSystem.h (GPU SDF callback interface, streaming state)
- res/shaders/sdf_generation.compute
- test/shield/test_sdf_gpu_cpu_parity.cpp
- src/luminumbra_common/network/ (whole directory — expect stubs)
- src/luminumbra_server/ (whole directory — expect a skeleton)
- docs/shield/ (SDF contract docs if present)
- .forge/artifacts/engine-framework-roadmap/ultimate-plan.md

Research focus:
1. Persistence: no chunk save/load exists. Propose the minimal world
   persistence format (chunk serialization with LZ4 already in place,
   versioned header, region files or per-chunk files, dirty tracking) and a
   round-trip gate: generate -> mutate -> save -> reload -> assert identical
   voxel and entity state.
2. GPU SDF: sdf_generation.compute and the parity test exist; the runtime
   callback integration is incomplete. What is missing to use the GPU path for
   far-field rendering at runtime, and how does the existing parity test
   extend into a runtime gate (GPU path on, screenshots compared against CPU
   path)?
3. Networking/server: luminumbra_server and NetworkManager are stubs. Propose
   the smallest credible authority loop (deterministic input relay over the
   existing fixed-tick sim) and a loopback gate: two local sessions converge
   to identical world hashes.
4. Sequencing: persistence almost certainly precedes networking; justify or
   refute, and identify which pieces unblock other panels (e.g. persistence
   unblocks endurance soak with reload).

Required output:
Write exactly one file: .forge/artifacts/engine-frontier/panel-6-persistence-gpu-network.md
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
1. Findings must cite concrete file:line evidence (including evidence of
   absence — empty or stub files).
2. Must-Fix items are defects or gaps that block shipping; Deepening
   Opportunities improve an already-working system; Frontier Proposals are
   exploratory and must be marked gate-first.
3. Every Proposed Gate must name the deterministic scenario, artifact JSON,
   unit test, or validator mode that would enforce it.
4. References must list every file you actually read.

Rules:
- Do not modify engine source, tests, build files, or other Forge artifacts in
  this run. Your only write target is the single panel file above.
- Keep the report concise and evidence-dense; no filler.
