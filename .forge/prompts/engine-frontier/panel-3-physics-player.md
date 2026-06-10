You are Codex, running as the sole AI agent for the Luminumbra engine-frontier workflow.

Goal:
Research panel 3 of 7: Physics (Jolt) and Player Controller deepening. Use
only Codex reasoning. Do not invoke, recommend, or route to any non-Codex AI
agent.

Workspace:
D:\Coding\luminumbra

Read these files first:
- src/luminumbra_common/systems/PhysicsSystem.h
- src/luminumbra_common/systems/PhysicsSystem.cpp
- src/luminumbra_client/player/PlayerController.h
- src/luminumbra_client/player/PlayerController.cpp
- src/luminumbra_common/world/GameSession.h
- src/luminumbra_common/world/GameSession.cpp
- .forge/artifacts/visual-performance-improvement/handoff.md
- .forge/artifacts/visual-performance-improvement/next-plan.md
- .forge/artifacts/engine-framework-roadmap/ultimate-plan.md

Research focus:
1. Determinism: fixed-tick contract, floating-point and iteration-order
   hazards, replay/lockstep readiness of the Jolt integration.
2. Chunk collision lifecycle: registration/removal vs streaming churn, stale
   body risk, collision LOD vs surface LOD divergence.
3. Character controller edge cases: coyote time, step-up/step-down on marching
   cubes terrain, swimming/water interaction, crouch collision adjustment.
4. Batched physics queries: the audio raycast batching path — priority
   handling, frame budget adherence, starvation risk.
5. Boundary-pushing proposals: a deterministic physics replay gate (record
   inputs, assert identical end-state hash), physics smoke scenario in the
   runtime harness, character-controller obstacle-course scenario.

Required output:
Write exactly one file: .forge/artifacts/engine-frontier/panel-3-physics-player.md
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
   validator mode that would enforce it (follow the pattern of
   .forge/scripts/validate-runtime-stability-phase-1.ps1 modes).
4. References must list every file you actually read.

Rules:
- Do not modify engine source, tests, build files, or other Forge artifacts in
  this run. Your only write target is the single panel file above.
- Keep the report concise and evidence-dense; no filler.
