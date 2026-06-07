# Luminumbra Architecture Audit

Generated: 2026-06-07

## Scope

This audit uses the available Forge project context plus direct repository inspection. Forge status is `spec` with no active dispatch. The relevant Forge baselines are `.forge/polyglot.json`, `.forge/architecture.toml`, `.forge/reports/polyglot-audit.md`, `.forge/crate-manifest.toml`, and `.forge/specs/CORE-AUDIT-2026-06-07.md`.

The Forge polyglot baseline scanned 375 files and 22,447 LOC across C++, CMake, GLSL, Lua, RmlUi markup/style, JSON, and Markdown. The largest code module is `luminumbra_client` at 112 files / 12,285 LOC, followed by `content_and_scripts` and `luminumbra_common`.

## Current Architecture

- `luminumbra_common` is the shared simulation layer: ECS components, job system, logging, world generation, chunks, marching cubes, physics, water, scripting, and networking skeletons.
- `luminumbra_client` is the presentation/runtime layer: GLFW/GLAD/OpenGL, render pipeline, RmlUi/ImGui UI, audio, player controller, debug viewer, and client entry point.
- `luminumbra_server` is only an incubating skeleton with a placeholder entry point and `HostManager`.
- `tools` contains the asset processor and build-time meshoptimizer integration.
- `scripts`, `data`, `res/shaders`, and `worlds` hold runtime content, UI, audio banks, shaders, Lua behavior, and world presets.

The intended dependency direction is mostly preserved: client depends on common; server depends on common; common does not include client. A lightweight include scan found `client -> common` edges and no reverse `common -> client` edges. The main boundary problems are not reversed dependencies, but wide public dependency exposure and large concrete coordinators.

## Module Boundaries And Dependency Direction

Strong points:

- The target graph has the right high-level shape: `luminumbra_common` as a static library, `luminumbra_client` as a static library plus `luminumbra_client_app`, and `luminumbra_server` depending on common.
- Common owns deterministic systems such as `SHIELD_WorldSystem`, `GameSession`, `PhysicsSystem`, `WaterSystem`, `Chunk`, and `MarchingCubes`.
- Client-only concerns mostly stay in `src/luminumbra_client`: GL resources, RmlUi interfaces, audio playback, camera/input, and debug UI.

Boundary issues:

- Public headers expose concrete vendor/framework types directly. Examples: `PhysicsSystem.h` includes Jolt headers, `RenderPipeline.h` includes GLAD/OpenGL types, UI headers include RmlUi types, and `Types.h` exposes EnTT/GLM aliases.
- `luminumbra_common` links many dependencies as `PUBLIC`, including dependencies that appear implementation-only. This leaks vendor requirements through every consumer and makes later target separation harder.
- `GameSession.h` includes concrete `SHIELD_WorldSystem`, `PhysicsSystem`, and `WaterSystem` headers instead of forward declarations plus private implementation includes. This makes the session facade compile-heavy and tightly coupled.
- `PhysicsSystem` contains audio-specific raycast/query concepts. Client audio depending on common physics is correct, but audio terminology inside common physics blurs domain ownership.
- Several headers reach the public include tree with relative paths such as `../../../include/luminumbra/core/Types.h`. This bypasses the intended include root and makes files sensitive to layout moves.

## Runtime Lifecycle

`src/luminumbra_client/main_client.cpp` is the real composition root. It initializes logging, path discovery, GLFW, GLAD, ImGui, `JobSystem`, `GameSession`, audio, `Rml_UIManager`, `RenderPipeline`, and `WorldLoadingVisualizer`. It then owns the state loop for main menu, world loading, and in-game runtime, and it handles shutdown in reverse-ish order.

Key lifecycle risks:

- `main_client.cpp` owns too much: platform windowing, input callbacks, game state transitions, UI callbacks, audio updates, chunk loading, physics ticking, rendering, and debug tools. The file is effectively an application framework.
- Runtime state uses global pointers for camera, player controller, UI manager, and loading visualizer. This makes ownership and callback lifetime harder to reason about.
- World generation is visually batched through loading state, but `SHIELD_WorldSystem::dispatch_generation_jobs` immediately waits on each batch. Initial generation is therefore synchronous per batch, despite using the job API.
- Meshing jobs are dispatched without returned handles. Shutdown likely drains via `JobSystem::shutdown`, but there is no explicit world-streaming lifecycle object that owns generation, meshing, collision upload, and render upload as one pipeline.
- `JobSystem` uses a fixed circular array of 256 atomic counters for batch handles. Any future workload with many outstanding batches can reuse a counter before old work completes unless ownership rules are tightened.
- The documented fixed 30 Hz simulation contract is not yet visible in the client loop. Physics and world update consume frame `deltaTime`, while `SHIELD_WorldSystem` throttles chunk activation by frame count.

## Layering And Framework Fit

The chosen frameworks fit the prototype: CMake/C++20, EnTT, Jolt, FastNoise, OpenGL/GLFW/GLAD, RmlUi, ImGui, miniaudio, Lua/sol2, nlohmann-json, and GoogleTest are reasonable for a local engine. The issue is integration shape, not library choice.

Framework-fit concerns:

- CMake target usage is inconsistent. The active build is defined in `src/CMakeLists.txt`, but `src/luminumbra_client/CMakeLists.txt` also defines `luminumbra_client_app` and is not added as a subdirectory. This stale file can mislead future changes.
- `src/luminumbra_client/sources.cmake` omits active-looking `.cpp` files under `ui/core`, `ui/components`, `UIIntegration.cpp`, `AudioPropagationSystem.cpp`, and `EnvironmentalAudioSystem.cpp`. Those files exist in the tree but do not participate in the active client target.
- UI direction is ambiguous. Runtime constructs `Rml_UIManager`; `EnhancedUIManager` is only a thin `SimpleUIManager` wrapper; docs claim a completed "Quantum UI" migration; active CMake only compiles the legacy RmlUi path plus the wrapper.
- `RenderPipeline.cpp` is over-consolidated. It owns pass orchestration, GL resource lifetime, chunk/water upload caches, static mesh instancing, lighting, shadows, SSAO, skybox, texture/material setup, hierarchical culling, and GPU SDF generation.
- `RenderSystem` is an empty lower-case namespace placeholder (`Luminumbra::rendering`) while the real renderer uses `Luminumbra::Rendering`. This is stale scaffolding and a namespace consistency smell.
- `src/luminumbra/core/ExternalImplementations.cpp` is not in the active target graph, but defines `MINIAUDIO_IMPLEMENTATION` and `FNL_IMPL`. If reintroduced without checking vendor targets, it can create duplicate implementation risk.

## File Layout

The intended layout is understandable: `src/luminumbra_common`, `src/luminumbra_client`, `src/luminumbra_server`, `tools`, `data`, `scripts`, `res/shaders`, and `worlds`. Forge correctly excludes `build`, `vendor`, and `external` from polyglot audit scans.

Layout issues:

- The public include tree is almost empty (`include/luminumbra/core/Types.h` only). Most engine headers live under `src`, so consumers rely on source-directory include paths rather than a stable public/private split.
- Manual source lists are already drifting from the file tree. This is the main build hygiene risk.
- Documentation is ahead of implementation for Instinct Engine, Atmospheric Engine, Aetheric Field, and Quantum UI.
- Server, networking, event bus, scripting, and `RenderSystem` are placeholders while README/TDD describe mature systems.
- Runtime data is copied wholesale from `data` by CMake, while asset processing writes generated `.lmesh` files into source `data`. That is convenient but mixes generated output with source-controlled content.

## Refactor Candidates

1. Stabilize SHIELD contracts first. Forge marks failing tests around SDF sign convention, cave effect, and normal orientation as ship blockers. Do not do broad architecture refactors until those contracts are explicit.
2. Clean CMake truth. Remove or wire `src/luminumbra_client/CMakeLists.txt`; reconcile `sources.cmake` with actual source files; decide whether UI/audio files are active, staged, or dead.
3. Extract an application/runtime layer from `main_client.cpp`. Introduce a small `ClientApplication` or `RuntimeCoordinator` that owns systems, state transitions, callbacks, and shutdown order.
4. Split `RenderPipeline` by responsibility: render graph/pass orchestration, GL resource owners, chunk mesh upload cache, water renderer, lighting/shadow/SSAO passes, culling, static mesh renderer, and GPU SDF generation.
5. Decide the UI path. Either keep legacy RmlUi and delete/park incomplete Quantum UI files, or make the component UI manager the active target with a migration plan and tests.
6. Tighten common/client interfaces. Move implementation-only includes out of headers, reduce `PUBLIC` CMake dependencies, and replace relative public includes with `#include <luminumbra/core/Types.h>`.
7. Define README-only pillars as specs before implementation. Instinct and Aetheric currently have components/scripts/data hints, not first-class C++ system boundaries.
8. Replace runtime `std::cout`, review raw allocation sites, and convert TODO clusters into tracked tasks. Forge currently reports 36 risk markers, mostly UI TODOs, direct console output, raw allocation, and runtime exceptions.

## Bottom Line

The repository has a sensible prototype architecture: common simulation below client presentation, with content/scripts/data kept outside C++. The immediate architecture risk is drift: docs ahead of code, CMake source lists behind the file tree, stale target files, inactive new systems, and oversized runtime/rendering coordinators. The safest path is to make the build graph truthful, lock SHIELD behavior with passing tests, then refactor the client runtime and render/UI stacks in narrow slices.
