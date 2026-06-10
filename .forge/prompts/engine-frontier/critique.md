You are Codex, running as the sole AI agent for the Luminumbra engine-frontier workflow.

Goal:
Adversarially critique the engine-frontier research synthesis and spec before
any roadmap is finalized. Use only Codex reasoning. Do not invoke, recommend,
or route to any non-Codex AI agent.

Workspace:
D:\Coding\luminumbra

Read these files first:
- .forge/artifacts/engine-frontier/research.md
- .forge/specs/ENGINE-FRONTIER-2026-06-09.md
- .forge/artifacts/engine-frontier/panel-1-world-streaming.md
- .forge/artifacts/engine-frontier/panel-2-rendering-visual-loop.md
- .forge/artifacts/engine-frontier/panel-3-physics-player.md
- .forge/artifacts/engine-frontier/panel-4-audio.md
- .forge/artifacts/engine-frontier/panel-5-simulation-ai-scripting.md
- .forge/artifacts/engine-frontier/panel-6-persistence-gpu-network.md
- .forge/artifacts/engine-frontier/panel-7-ui-tooling-tests.md
- .forge/artifacts/visual-performance-improvement/handoff.md

Attack the synthesis on at least these axes:
1. Overreach: frontier proposals scoped too large for single Codex dispatch
   tasks, or attempted before their prerequisites.
2. Risky rewrites: changes to mature, proven systems (SHIELD streaming, job
   system, physics) that lack a no-behavior-change verification story. The
   RenderPipeline modularization is the highest-risk item — does every wave
   have a gate proving behavior is unchanged?
3. Hidden coupling: tasks that look independent but touch the same files
   (RenderPipeline.cpp, main_client.cpp, sources.cmake) and will collide in
   parallel dispatch.
4. Vague tasks: anything not executable by a single agent with a clear
   verification command.
5. Missing gates: proposals whose acceptance is subjective or unmeasured.
6. Sequencing errors: the material/texture gate must be Wave 1 task 1;
   endurance revalidation must come after visual gates stabilize; persistence
   before networking.
7. Budget realism: dispatch runs against a $30/day budget with max 4 parallel
   tasks; flag waves too large to integrate and verify in a day.

Required output:
Write .forge/artifacts/engine-frontier/critique.md (replace if it exists).
For each item use this structure:
### Finding N: <title>
- Finding: <what is wrong, with evidence>
- Mitigation: <the concrete change to the research, spec, or task breakdown>
- Verdict: <blocker | revise | accept-with-note>

End the file with a `## Verdict Summary` section listing blockers first.

Rules:
- Write only critique.md. Do not modify research.md, the spec, engine source,
  or other artifacts — the revision step applies your mitigations.
- Be genuinely adversarial; an empty critique is a failed critique.
