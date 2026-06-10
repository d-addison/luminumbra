You are Codex, running as the sole AI agent for the Luminumbra engine-frontier workflow.

Goal:
Apply the adversarial critique to produce the final engine-frontier roadmap,
handoff, and executable dispatch task graph. Use only Codex reasoning. Do not
invoke, recommend, or route to any non-Codex AI agent.

Workspace:
D:\Coding\luminumbra

Read these files first:
- .forge/artifacts/engine-frontier/research.md
- .forge/artifacts/engine-frontier/critique.md
- .forge/specs/ENGINE-FRONTIER-2026-06-09.md
- .forge/artifacts/visual-performance-improvement/handoff.md
- .forge/artifacts/visual-performance-improvement/next-plan.md
- .forge/scripts/validate-engine-frontier.ps1 (Test-MaterialVisual defines the JSON contract task T-EF-1 must satisfy)
- .forge/tasks/runtime-stability-phase-1/dispatch.json (dispatch shape reference)

Required output 1: .forge/artifacts/engine-frontier/ultimate-plan.md
Replace if it exists. A staged, waved roadmap with H2 sections `Wave 1`
through `Wave N` and a final `## Success Definition`. Requirements:
- Wave 1 starts with the deterministic Material/texture-layer visual gate
  (sand vs grey fallback) and contains only emphasis-stream work
  (world/streaming deepening, rendering/visual loop).
- Frontier streams (persistence, GPU SDF runtime integration, Aetheric Field,
  GOAP, networking) appear in later waves, each gate-first.
- Every wave lists its verification commands and what must be true before the
  next wave starts.
- Address every critique mitigation explicitly; if you reject one, justify it
  inline.

Required output 2: .forge/artifacts/engine-frontier/handoff.md
Replace if it exists. H2 sections, in order: `## Current Status`,
`## Immediate Next Step`, `## No-Deferral Rules`, `## Success Definition`.
The Immediate Next Step is executing Wave 1 via the approval-gated dispatch.
Note that contract execution meters against a $30/day budget and large waves
may need to span days.

Required output 3: .forge/tasks/engine-frontier/dispatch.json
Replace if it exists. Create the directory if needed. EXACT contract:
- Top level: "schema_kind": "forge.task_graph", "schema_version": "2",
  "feature": "engine-frontier", "description" containing the literal string
  "Codex-only".
- Each task: "id" of the form T-EF-<N>-<slug>, "title", "type" one of
  plan|code|test, "tier" (standard, or deep for genuinely complex tasks),
  "depends_on" array of task ids, "files" array, "agent_contract" object with
  "reads", "modifies", "creates" arrays, and "prompt" beginning with the
  literal string "Codex-only execution."
- The dispatch file must NOT mention any non-Codex AI agent by name anywhere;
  the CodexOnly routing gate regex-fails the workflow otherwise.

Mandatory seed tasks (include regardless of panel/critique outcomes):
1. "T-EF-1-material-visual-gate" (type: code, Wave 1, no dependencies):
   implement the `material_visual_smoke` runtime scenario producing
   build/debug/test-artifacts/runtime/material-visual/material-visual-analysis.json
   with schema "luminumbra.material_visual_analysis.v1", per-material ROI
   entries (Sand mandatory) with classified vs grey-fallback pixel counts and
   thresholds, `gl_debug` error count, a normal screenshot and a material-ID
   heatmap screenshot. Acceptance: `powershell.exe -NoProfile
   -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1
   -Mode MaterialVisual` passes. Likely files: src/luminumbra_client/main_client.cpp,
   src/luminumbra_client/rendering/RenderPipeline.cpp.
2. "T-EF-2-material-visual-gate-test" (type: test, depends_on T-EF-1):
   run the MaterialVisual, LodGround, and WaterVisual gates and record results.
3. Final task "T-EF-<last>-endurance-revalidation" (type: test, depends on all
   visual-gate tasks): rerun `powershell.exe -NoProfile -ExecutionPolicy
   Bypass -File .forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode
   Endurance300` and record the result.
4. Run `ctest --preset debug --output-on-failure -E "_NOT_BUILT$"` and add a
   Wave 1 fix task for every test that currently fails (the post-execution
   verify gate runs this exact command and will fail the workflow otherwise).
   Known failures at authoring time, suspected fallout of the cave-surface-cap
   raise and per-cell LOD ownership changes:
   WorldGenLayerSnapshotTest.ExportsLayerMetricsAndImages,
   WorldGenLayerSnapshotTest.LodRemeshKeepsPreviousMeshRenderableWhilePending,
   RuntimeWorldVisualValidationTest.MixedLodBoundariesAreContinuousAndReported.

Task prompt template requirements (apply to every task prompt):
- Begin with "Codex-only execution."
- State the exact verification command(s) the task must leave passing.
- Warn: the build uses warnings-as-errors (cmake --build --preset debug); any
  new source file must be added to the matching sources.cmake manifest.
- Forbid committing; integration is handled by the dispatch pipeline.
- Keep each task completable by one agent in one session; split anything
  larger.

Verification to run before finishing:
- forge tasks validate .forge/tasks/engine-frontier/dispatch.json
- powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Files
- powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Sections
Fix issues until all three pass.

Rules:
- Write only the three files named above. Do not modify engine source, tests,
  panel files, research.md, critique.md, or the spec. Do not commit.
