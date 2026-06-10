You are Codex, running as the sole AI agent for the Luminumbra engine-frontier workflow.

Goal:
Research panel 7 of 7: UI (RmlUi), tooling, and test infrastructure. Use only
Codex reasoning. Do not invoke, recommend, or route to any non-Codex AI agent.

Workspace:
D:\Coding\luminumbra

Read these files first:
- src/luminumbra_client/ui/core/UIComponent.h (known: 4 binding-unsubscribe TODOs — property listeners leak)
- src/luminumbra_client/ui/core/UIProperty.h
- src/luminumbra_client/ui/Rml_UIManager.h
- src/luminumbra_client/ui/Rml_UIManager.cpp
- src/luminumbra_client/ui/components/game/WorldList.cpp (known: loading state, favorites, recent filter, creation date all stubbed)
- src/luminumbra_client/ui/components/common/Button.cpp
- test/ui/ui_smoke_test.cpp
- test/CMakeLists.txt
- CMakePresets.json
- docs/TDD.md
- docs/UI_REFACTORING_SUMMARY.md
- .forge/artifacts/engine-framework-roadmap/ultimate-plan.md

Research focus:
1. UI binding lifecycle: the unsubscribe TODOs in UIComponent.h are a real
   leak risk. Propose the subscription-token pattern that fixes them and a
   unit-test gate proving listeners are released (bind, destroy component,
   assert zero live subscriptions).
2. WorldList completion: enumerate the stubbed features and which are
   ship-blocking vs cosmetic.
3. Test infrastructure: where are coverage holes relative to the TDD 95% gate
   ambition (per .forge/config.yaml)? What is cheap to add: ASan lane in CI
   cadence, UI screenshot smoke, worldgen snapshot breadth?
4. Tooling: asset processor round-trip coverage, shader compile gate breadth,
   developer loop friction (build times, test selection).
5. Boundary-pushing proposals: headless UI interaction harness (scripted
   clicks against RmlUi documents with screenshot assertions), test-artifact
   dashboard generation.

Required output:
Write exactly one file: .forge/artifacts/engine-frontier/panel-7-ui-tooling-tests.md
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
3. Every Proposed Gate must name the deterministic scenario, artifact JSON,
   unit test, or validator mode that would enforce it.
4. References must list every file you actually read.

Rules:
- Do not modify engine source, tests, build files, or other Forge artifacts in
  this run. Your only write target is the single panel file above.
- Keep the report concise and evidence-dense; no filler.
