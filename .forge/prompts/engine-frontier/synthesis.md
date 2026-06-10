You are Codex, running as the sole AI agent for the Luminumbra engine-frontier workflow.

Goal:
Synthesize the seven engine-frontier research panels into a consensus research
document and a feature spec. Use only Codex reasoning. Do not invoke,
recommend, or route to any non-Codex AI agent.

Workspace:
D:\Coding\luminumbra

Read these files first:
- .forge/artifacts/engine-frontier/panel-1-world-streaming.md
- .forge/artifacts/engine-frontier/panel-2-rendering-visual-loop.md
- .forge/artifacts/engine-frontier/panel-3-physics-player.md
- .forge/artifacts/engine-frontier/panel-4-audio.md
- .forge/artifacts/engine-frontier/panel-5-simulation-ai-scripting.md
- .forge/artifacts/engine-frontier/panel-6-persistence-gpu-network.md
- .forge/artifacts/engine-frontier/panel-7-ui-tooling-tests.md
- .forge/artifacts/visual-performance-improvement/handoff.md
- .forge/artifacts/visual-performance-improvement/next-plan.md
- .forge/specs/RUNTIME-STABILITY-PHASE-1-2026-06-08.md (spec skeleton reference)

Required output 1:
Write .forge/artifacts/engine-frontier/research.md (replace if it exists) with
these H2 sections, in order:
## Consensus
## Emphasis
## World Generation and Streaming
## Rendering and Visual Quality
## Physics and Player
## Audio
## Simulation, AI, and Scripting
## Persistence, GPU SDF, and Networking
## UI, Tooling, and Tests

Content requirements for research.md:
1. Consensus: the cross-panel view — what the engine is, where it is strong,
   what blocks shipping, ordered by severity.
2. Emphasis: rank work streams. Mature-system deepening (world/streaming,
   rendering) and the visual-quality loop continuation (the material/texture
   gate) come FIRST; frontier systems (persistence, GPU SDF, Aetheric Field,
   GOAP, networking) are staged behind them, gate-first.
3. Each subsystem section: distilled must-fix list, deepening list, frontier
   list, with the panel's proposed gates carried through. Drop nothing
   silently — if you reject a panel proposal, say so and why.

Required output 2:
Write .forge/specs/ENGINE-FRONTIER-2026-06-09.md (replace if it exists)
following the skeleton of RUNTIME-STABILITY-PHASE-1-2026-06-08.md, with these
H2 sections, in order:
## Objective
## Scope
## Acceptance Criteria
## Verification Commands
## No-Deferral Rules

Content requirements for the spec:
1. Objective: one paragraph — push the engine forward on all fronts with
   deterministic gates, emphasis on mature-system deepening and the visual
   loop.
2. Scope: in-scope work streams and explicit non-goals for this iteration.
3. Acceptance Criteria: concrete and checkable; the first criterion is the
   material/texture-layer visual gate passing
   (.forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual); include
   build, unit test, LodGround, WaterVisual, and endurance revalidation
   criteria.
4. Verification Commands: exact commands, including:
   - powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode All
   - powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual
   - powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode LodGround -SmokeSeconds 30
   - powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode WaterVisual -SmokeSeconds 30
   - forge tasks validate .forge/tasks/engine-frontier/dispatch.json
5. No-Deferral Rules: hard blockers that must become failing gates plus fixes
   before this feature closes (sand-vs-grey material fallback, any new GL
   debug errors, LOD holes, upload-priority inversions, warnings-as-errors
   breaks).

Rules:
- Write only the two files named above. Do not modify engine source, tests, or
  other Forge artifacts.
- If a panel file is missing, stop and report the gap instead of inventing its
  content.
