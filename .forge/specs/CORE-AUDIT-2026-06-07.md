---
id: CORE-AUDIT-2026-06-07
title: "Luminumbra Core Audit and Polyglot Forge Baseline"
status: draft
owner: daddison
updated: 2026-06-07
---

# Luminumbra Core Audit and Polyglot Forge Baseline

## Goal

Instantiate Forge as a useful C/C++ engine audit surface for Luminumbra, then use it to drive the first stabilization roadmap.

## Current Baseline

- Forge project-local config exists in `.forge/config.yaml`.
- C/C++ architecture checks exist in `.forge/architecture.toml`.
- Module/ship-blocker manifest exists in `.forge/crate-manifest.toml`.
- Polyglot audit manifest exists in `.forge/polyglot.json`.
- Polyglot audit script exists in `.forge/scripts/polyglot-audit.mjs`.
- Latest polyglot report is `.forge/reports/polyglot-audit.md`.
- Symbol index has been built with `forge index`.

## Audit Findings

### P0: Test Baseline Is Not Green

`cmake --build build` succeeds, but `ctest --test-dir build --output-on-failure` fails 3 of 14 tests:

- `WorldGenerationTest.SurfaceIsGeneratedAtCorrectHeight`: SDF sign convention disagrees with the test contract.
- `WorldGenerationTest.CavesChangeGeneratedMesh`: cave-enabled and cave-disabled terrain produce identical mesh vertex counts.
- `WorldGenerationTest.NormalsPointUpwardsOnHorizontalSurface`: generated terrain normals point down instead of up.

These block confident refactoring because the core terrain contract is ambiguous.

### P1: Forge Needs Polyglot Project Knowledge

Forge's stock `audit report` can consume the architecture and blocker manifests, but its crate LOC and generic verify paths are Rust-biased. The local polyglot layer fixes the immediate repo need by scanning:

- C/C++ headers and source.
- CMake.
- GLSL shaders.
- Lua scripts.
- RmlUi markup and RCSS.
- JSON data and world presets.
- Markdown docs.

It excludes `build/`, `vendor/`, `external/`, and vendored CGLTF headers in `tools/`.

### P1: Architecture Vision Is Ahead of Implementation

The README describes four engine pillars:

- SHIELD Engine.
- Instinct Engine.
- Atmospheric Engine.
- Aetheric Field.

Only SHIELD/world generation, rendering, audio, water, physics, UI, and scaffolding are presently concrete enough for code audit. Instinct and Aetheric need explicit C++ module specs before major implementation.

### P2: Technical Debt Clusters

The calibrated polyglot report currently finds:

- 20 TODO/FIXME/HACK/WIP markers.
- 9 direct `std::cout` sites in C++ paths.
- 7 raw `new`/`delete` sites.
- 2 `std::runtime_error` throw sites.

The biggest debt cluster is the UI refactor, followed by renderer TODOs and engine logging/RAII cleanup.

## Roadmap

### Phase 1: Make the Baseline Trustworthy

1. Fix or explicitly redefine the SHIELD SDF sign convention.
2. Fix terrain normal winding or normal accumulation.
3. Make cave carving measurably affect SDF/mesh output, or replace the test with a better cave contract.
4. Re-run `cmake --build build` and `ctest --test-dir build --output-on-failure`.

### Phase 2: Keep Forge Useful for This Repo

1. Keep `.forge/polyglot.json` as the source of truth for language roots, exclusions, modules, test commands, and risk patterns.
2. Run `forge script run polyglot-audit` before roadmap reviews.
3. Treat `.forge/reports/polyglot-audit.md` as the local polyglot audit baseline.
4. Upstream C/C++ support into `D:/Coding/forge-new` when we are ready to edit Forge core.

### Phase 3: Stabilize Architecture Boundaries

1. Define SHIELD terrain contracts: SDF sign, units, chunk coordinate layout, cave semantics, material selection, mesh winding.
2. Split large renderer responsibilities out of `RenderPipeline.cpp`.
3. Choose the UI architecture path: legacy `Rml_UIManager`, new component UI manager, or an explicit migration bridge with milestones.
4. Write specs for Instinct Engine and Aetheric Field before implementing them.

### Phase 4: Debt Burn-Down

1. Replace direct `std::cout` in engine/runtime code with logging abstractions.
2. Review raw allocation sites for ownership and RmlUi API constraints.
3. Convert UI TODOs into feature tasks or remove dead paths.
4. Add CI or a local scripted gate for CMake build, CTest, and polyglot audit.

## Acceptance Criteria

- `ctest --test-dir build --output-on-failure` passes or remaining failures are documented as intentional with replacement tests.
- `forge script run polyglot-audit` runs successfully and writes both JSON and Markdown reports.
- `forge audit architecture` has no drift caused by stale paths or patterns.
- The first dispatch plan validates with `forge tasks validate`.
- The roadmap identifies concrete next tasks with file scopes and verification commands.

## Verification Commands

```powershell
forge doctor
forge audit architecture
forge audit report
forge script run polyglot-audit
cmake --build build
ctest --test-dir build --output-on-failure
forge tasks validate .forge/tasks/CORE-AUDIT-2026-06-07/dispatch.json
```
