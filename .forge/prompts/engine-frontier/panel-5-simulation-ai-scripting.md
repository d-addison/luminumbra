You are Codex, running as the sole AI agent for the Luminumbra engine-frontier workflow.

Goal:
Research panel 5 of 7: Simulation frontier — Aetheric Field, Instinct/GOAP AI,
Lua scripting, and the event bus. This is a FRONTIER panel: most of these
systems exist only as components, shaders, or stubs. Mark every proposal
gate-first (the deterministic gate is built before or alongside the feature)
and keep scopes small enough for single-dispatch tasks. Use only Codex
reasoning. Do not invoke, recommend, or route to any non-Codex AI agent.

Workspace:
D:\Coding\luminumbra

Read these files first:
- src/luminumbra_common/components/InstinctComponents.h
- src/luminumbra_common/core/EventBus.h
- src/luminumbra_common/scripting/ (whole directory)
- scripts/ (sample Lua under client/common/server)
- res/shaders/crystal_field_effect.frag
- data/common/materials.json (LuminCrystal emission ties to the Aetheric Field)
- README.md (Aetheric Field and Instinct Engine vision)
- .forge/artifacts/engine-framework-roadmap/ultimate-plan.md

Research focus:
1. Aetheric Field: shaders and the LuminCrystal material exist but there is no
   C++ simulation core. Propose the smallest field model (e.g. chunk-local
   scalar diffusion sampled by lighting and gameplay) with a deterministic
   unit-test gate and a visual gate tied to LuminCrystal emission.
2. Instinct/GOAP AI: components exist without a planner. Propose the minimal
   planner contract (needs -> goals -> actions), where it runs (tick phase,
   job system), and a deterministic scenario gate (an agent provably satisfies
   a need in N ticks from a fixed seed).
3. Lua scripting: current sol2 surface is minimal. What binding surface
   unlocks gameplay iteration without destabilizing determinism, and how is
   hot-reload gated?
4. EventBus: the header is near-empty. Propose the event contract the above
   systems need (typed events, deterministic ordering, recording for replay).
5. Sequencing: which of these is the cheapest credible Wave 2 entry point,
   and what must NOT be attempted yet.

Required output:
Write exactly one file: .forge/artifacts/engine-frontier/panel-5-simulation-ai-scripting.md
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
