# Luminumbra Tooling Audit

Generated: 2026-06-07

## Scope

Build system, package management, generated assets, CI readiness, developer workflow, static analysis, formatting, and test execution. The goal is to identify the minimal toolchain structure needed for reliable local iteration and a credible CI gate.

Forge polyglot baseline lists 10 CMake files (316 LOC) and 197 JSON files (4,740 LOC) in scope, plus 36 GLSL, 8 markdown, 11 Lua, and 14 RML/RCSS files. The vendor tree adds the bulk of compile time, hidden from the polyglot scan.

## Executive Summary

The build system is **functional but fragile**: a hand-maintained `sources.cmake` already drifts from the file tree, vendor dependencies are pulled in as fully copied subdirectories instead of submodules or package-manager artifacts, generated assets are written into the source-controlled `data/` directory, and runtime data is copied to the build directory only at configure time. There is **no CI workflow, no warnings-as-errors gate, no formatter, no static-analysis configuration, and no presets file**, so every developer environment differs in compiler flags, build types, and lint behavior. The dev loop is `cmake -S . -B build && cmake --build build && ctest --test-dir build`, with one test binary and a perf check that asserts wall-clock thresholds. The minimum responsible toolchain delta is small and well-scoped: a `CMakePresets.json` with two presets (debug-asan, release), `.clang-format` + `.clang-tidy` at the repo root, a single GitHub Actions workflow that builds + tests on Windows and Linux, and one PowerShell script that drives the full local gate so contributors can match CI before pushing.

## Current State

### Build system

- **Generator:** CMake 3.20+. Top-level `CMakeLists.txt` orchestrates `vendor/`, `tools/`, `src/`, and `test/`.
- **Targets:** `luminumbra_common` (STATIC), `luminumbra_client_app` (WIN32 executable), `asset_processor` (tool), `process_assets` (custom `ALL` target), `world_generation_test` (GoogleTest binary).
- **C++ standard:** C++20, extensions off.
- **Output dirs:** `build/lib` for archives/libraries, `build/bin` for runtime.
- **Compile flags:** Manual `-O3 -DNDEBUG` for Release, `-g -Wall` for Debug. These are GCC/Clang flags; **MSVC ignores them**. There are no `/W4`, `/WX`, `-Werror`, or MSVC-equivalent overrides.
- **Build types:** Single-config (`CMAKE_BUILD_TYPE`) assumed; multi-config generators (VS, Ninja Multi-Config) silently lose `LUMINUMBRA_DEBUG`.
- **Sanitizers:** `add_link_options(-fsanitize=address)` is **commented out** in the Debug branch. No `-fsanitize=undefined` configuration.

### Source-list management

- `src/luminumbra_common/sources.cmake` lists 12 .cpp files.
- `src/luminumbra_client/sources.cmake` lists 11 internal .cpp files + 6 vendored ImGui sources.
- Drift confirmed: at least 9 .cpp files under `src/luminumbra_client/ui/`, `src/luminumbra_client/audio/`, and `src/luminumbra/core/ExternalImplementations.cpp` exist in-tree but are not in any source list.
- `src/luminumbra_client/CMakeLists.txt` defines `luminumbra_client_app` and is **not added as a subdirectory** by the active build (`src/CMakeLists.txt` does the wiring). The file is stale-but-still-present, which is a misleading-source trap.

### Package management

- **No vcpkg, no conan, no FetchContent.** `cmake/dependencies.cmake` is a one-line stub: *"all dependencies are handled locally in the vendor directory."*
- `vendor/` contains 16 fully-copied third-party trees: EnTT, fastnoise, glad, glfw, glm, imgui, Jolt, lua, miniaudio, nlohmann, rmlui, sol2, soil2, spdlog, googletest, meshoptimizer, plus a vendored `lz4`. Plus duplicates under `external/` (FastNoiseLite, glm-1.0.1).
- **Only googletest is a git submodule** (`.gitmodules`). The other 15 are flat copies — no upstream tracking, no easy CVE bumping, and the vendor folder is part of git history.
- `vendor/lz4` adds another implementation copy alongside whatever the system zlib/lz4 path provides. No version pinning visible.
- `.gitignore` is 8,958 lines / 425 KB — almost certainly a concatenation of every github/gitignore template. This is a `.gitignore` code smell: it suggests no curated ignore policy and makes diffing the file useless.

### Generated assets

- `assets/{models,audio,fonts,shaders,textures}/` is the source-of-truth. Top-level CMake globs `assets/*.glb` and invokes `asset_processor` to produce `data/.../*.lmesh` files. **The `.lmesh` outputs are written into the source-controlled `data/` directory**, mixing generated and authored content.
- `asset_processor` (`tools/asset_processor.cpp`) compiles meshoptimizer's `clusterizer.cpp`, `indexgenerator.cpp`, `overdrawoptimizer.cpp`, `vcacheoptimizer.cpp`, `vfetchoptimizer.cpp`, `vertexfilter.cpp` directly. It depends on `cgltf.h` and `cgltf_write.h` vendored in `tools/` (Forge excludes these from polyglot scans).
- `file(GLOB_RECURSE SOURCE_ASSETS "${ASSET_SOURCE_DIR}/*.glb")` is **not tracked by CMake's reconfigure scanner** — adding/removing a `.glb` does not trigger a CMake re-run. The user must manually `cmake -S . -B build` after editing the asset set.
- `file(COPY data DESTINATION ${CMAKE_BINARY_DIR})` runs at configure time only — JSON/UI/audio bank edits don't reach the build output during incremental builds.

### Runtime resources

- 197 JSON files for audio banks (8 in repo plus 7 new untracked banks per git status), material database, archetypes, world presets, and UI state.
- 7 RML documents, 4 RCSS stylesheets in `data/ui/`.
- 36 GLSL shaders in `res/shaders/`. Shaders are loaded by path at runtime; there is no build-time compilation or SPIR-V step.
- 11 Lua AI behavior files in `scripts/common/ai/`.

### Tests

- Single binary: `world_generation_test`. 15 GoogleTest cases.
- `gtest_discover_tests` is wired in `test/CMakeLists.txt`.
- Performance tests assert wall-clock `EXPECT_LT(avg_time_ms, X)` — flaky on slow hardware, breaks on Debug builds.
- No tests for: PhysicsSystem, JobSystem, WaterSystem (in isolation), GameSession save/load round trip, MarchingCubes water path, audio, UI, render abstractions, Lua bridge.

### CI

- **No `.github/workflows/`, no `.gitlab-ci.yml`, no `azure-pipelines.yml`, no buildkite config.**
- A `build_error.log` (1.4 KB) is committed in the repo root — a manual capture, not a tool output.
- No Forge dispatch or workflow gate runs in CI.

### Formatting and static analysis

- **No `.clang-format`, no `.clang-tidy`, no `.editorconfig`.**
- No `cmake --check-format` or `clang-tidy` integration. The project compiles with `-Wall` on GCC paths but never on MSVC.
- No `cppcheck`, `iwyu`, or `include-what-you-use` wiring.
- No `gdb`/`lldb` wrapper scripts, no `.vscode/launch.json` checked in beyond what's already in `.vscode/`.

### Forge integration

- `.forge/` is set up with `polyglot.json`, `architecture.toml`, `crate-manifest.toml`, `state.json`, `structure.md/json`, `symbol-index.bin`, `polyglot-audit-workflow.yaml`, and the in-progress reports under `polyglot-audit/`.
- `forge structure`, `forge index`, `forge verify --validation-policy warn --testing-policy warn` are the deterministic verification steps the workflow uses.
- The workflow's `audit-panels-phase` runs four AI panels in parallel; this audit was completed via Claude after Codex hit a quota window.

## Findings

### T0 — No CI; the test failures on `main` are not gated (confidence: high)

Three SHIELD tests are known-failing per the spec, and `main` shipped with them red. There is no GitHub Actions / GitLab CI / equivalent workflow that runs `ctest --test-dir build --output-on-failure` on every push, so the red state is invisible to reviewers and to the dispatch pipeline. **First tooling work item.** A single workflow YAML running `cmake --preset release && cmake --build --preset release && ctest --preset release` on `ubuntu-latest` and `windows-latest` would close this gap in <40 lines.

### T0 — No `CMakePresets.json`; build-flag behavior is generator-dependent (confidence: high)

`CMakeLists.txt:74-85` sets `-O3 -DNDEBUG`/`-g -Wall` only when `CMAKE_BUILD_TYPE` is one of `Release`/`Debug`. Multi-config generators (Visual Studio, Ninja Multi-Config) leave `CMAKE_BUILD_TYPE` empty at configure time, so neither block runs and `LUMINUMBRA_DEBUG` is never defined. The flags are also GCC syntax that MSVC silently drops. A `CMakePresets.json` with explicit `debug`, `debug-asan`, and `release` presets — each pinning generator, compiler-conditional flags, and warnings-as-errors — eliminates this whole class of "works on my machine" failure.

### T0 — `sources.cmake` drifts from the file tree (confidence: high)

(Restated from quality-risks panel for tooling completeness.) The hand-maintained source list is already wrong. Options:
1. Replace with `file(GLOB_RECURSE ...)` + `CONFIGURE_DEPENDS` so CMake re-scans on every build. Trades a small cost for correctness.
2. Keep manual lists and add a CI check that errors if an in-tree `.cpp` is not listed by any target. Trades a bit of CI complexity for explicit dead-code detection.

Either is fine. The current "manual, drifting, no check" state is not.

### T1 — Vendor copies prevent CVE response and inflate the repo (confidence: high)

15 of 16 vendor trees are direct copies. The repo carries (rough order of magnitude) >100 MB of vendored sources before counting `.git/`. Switching to git submodules or vcpkg manifest mode for stable libs (Jolt, EnTT, GLM, GLFW, spdlog, nlohmann, googletest) would:

- Make CVE bumps a `git submodule update` or `vcpkg upgrade`.
- Shrink the working tree and clone time.
- Make "what version is this?" answerable from one file.

The libraries that genuinely need to stay vendored are the small single-header ones (cgltf, FastNoiseLite if `external/` is the canonical home — and the dup with `vendor/fastnoise` should be resolved).

### T1 — Generated `.lmesh` outputs live in source-controlled `data/` (confidence: high)

`add_custom_command` writes `.lmesh` files into `${CMAKE_SOURCE_DIR}/data/`. Either:
- Move outputs to `${CMAKE_BINARY_DIR}/data/` and have the runtime path-resolution check both source-`data/` and build-`data/`, OR
- Keep outputs in source-`data/` and `.gitignore` them explicitly.

The current state means a clean checkout will rebuild assets *into* the working tree, dirty the git status, and every developer machine produces a slightly different binary representation of the "same" model.

### T1 — `file(GLOB ...)` for assets is undetected by CMake reconfigure (confidence: high)

Add `CONFIGURE_DEPENDS` to the asset glob so adding a new `.glb` triggers reconfigure on the next build:

```cmake
file(GLOB_RECURSE SOURCE_ASSETS CONFIGURE_DEPENDS "${ASSET_SOURCE_DIR}/*.glb")
```

This is a one-line fix and removes a recurring "did I forget to rerun CMake?" friction point.

### T1 — `file(COPY data ...)` runs only at configure time (confidence: high)

Replace with `add_custom_target(copy_data ALL COMMAND ${CMAKE_COMMAND} -E copy_directory ...)` so the copy is part of the build, not the configure. Or symlink `${CMAKE_BINARY_DIR}/data` to `${CMAKE_SOURCE_DIR}/data` on dev machines via `CMAKE_HOST_UNIX`/`CMAKE_HOST_WIN32` branch, which makes JSON edits live without any rebuild step.

### T1 — `.gitignore` is 8,958 lines, indicating no curated policy (confidence: high)

Replace with a project-specific ignore file (~30 lines max): `build/`, `out/`, `.cache/`, `.vs/`, `.vscode/*` exceptions if any are committed, `*.user`, `imgui.ini`, generated `.lmesh` (if T1 above lands), Forge transient state, plus a `.gitignore` per vendor subdir if needed. A 425 KB `.gitignore` is unreviewable.

### T1 — No formatter, no static analyzer (confidence: high)

Add at the repo root:
- `.clang-format` (project-specific; LLVM/Google/Mozilla base is a starting point).
- `.clang-tidy` with a small starting profile (`bugprone-*`, `clang-analyzer-*`, `cppcoreguidelines-pro-type-cstyle-cast`, `modernize-use-nullptr`, `performance-*`). Excluded paths: `vendor/`, `external/`, `build/`.
- `.editorconfig` for trivial formatting hygiene (final newline, trim trailing whitespace).
- A `scripts/lint.ps1` and `scripts/lint.sh` that run `clang-format --dry-run --Werror` and `clang-tidy -p build` on `src/` and `test/`. Same scripts hook into the CI workflow.

### T1 — No warnings-as-errors gate on the canonical Windows build (confidence: high)

Add to top-level CMake (after the generator detection):

```cmake
if(MSVC)
  add_compile_options(/W4 /WX /permissive- /Zc:__cplusplus)
else()
  add_compile_options(-Wall -Wextra -Werror -Wno-error=unused-parameter)
endif()
```

Apply only to first-party targets (`luminumbra_common`, `luminumbra_client_app`, `asset_processor`, `world_generation_test`) so vendor warnings don't poison the gate.

### T2 — One test binary covers one subsystem (confidence: high)

Split `test/` into per-module GoogleTest binaries: `test/common/JobSystem`, `test/common/Chunk`, `test/common/WaterSystem`, `test/common/GameSession`, `test/common/MarchingCubes`. Each binary stays under one minute of wall-clock so CI can fan them out. The existing perf tests move into a dedicated `world_generation_perf_test` binary gated by a CMake option `LUMINUMBRA_RUN_PERF_TESTS=OFF` by default.

### T2 — Asset processor has no test (confidence: high)

`tools/asset_processor.cpp` produces the binary format that the renderer consumes. There is no round-trip test (write a `.lmesh`, load it back, validate header + buffer integrity). A trivial Google Test against a tiny known-good `.glb` would catch format breakage before runtime.

### T2 — Lua bridge is empty but registered in CMake's intent (confidence: high)

`src/luminumbra_common/scripting/LuaState.cpp` is listed in `sources.cmake` but is empty. The "scripting" feature is plumbed into the build but doesn't exist. Either remove from `sources.cmake` (and remove from `.forge/architecture.toml` checks), or land a minimal `LuaState::startup/shutdown` + a smoke test that runs one of the existing `scripts/common/ai/actions/*.lua` files into a sol2 context.

### T2 — Shaders are not validated at build time (confidence: high)

GLSL files compile only at runtime via `glCompileShader`. A typo in a `#version 450` shader is detected when the player starts the game, not when CI runs. Adding `glslangValidator` (vendored or downloaded) as part of the build, gated by `LUMINUMBRA_VALIDATE_SHADERS`, would catch this on every build for ~0 ms / shader.

### T2 — Audio bank JSON has no validation (confidence: high)

8 audio bank JSON files (some new, untracked) parse via nlohmann. There is no JSON Schema and no `from_json` validator. A bank file with a missing field is detected at runtime as a silent skip. Adding `data/schemas/audio_bank.schema.json` and a CMake-level `validate_data` custom target running `ajv-cli` or a tiny C++ validator would catch authoring errors during the build.

### T3 — No `.editorconfig`, no consistent line endings (confidence: medium)

Windows-first repo with Linux-friendly C++. Without `.editorconfig`, CRLF/LF rules are per-developer. Add a five-line `.editorconfig` at the root.

### T3 — `imgui.ini` is committed (confidence: high)

`imgui.ini` is in the repo root. This is per-developer UI state and shouldn't be checked in. Add to `.gitignore` (when T1 ignore-file work happens).

### T3 — `build_error.log` is committed (confidence: high)

A historic build error capture in the root. Delete after preserving any useful content; logs do not belong in git.

### T3 — `external/` and `vendor/` overlap (confidence: high)

`external/FastNoiseLite/` and `vendor/fastnoise/` both exist. `external/glm-1.0.1/` and `vendor/glm/` both exist. Pick one. The `luminumbra_common` CMake hits `external/FastNoiseLite` via `target_include_directories` and `vendor/glm` via `target_link_libraries` — two unrelated dependency mechanisms for the same conceptual library.

### T3 — `.gitmodules` only tracks googletest (confidence: high)

If submodules are the chosen package management direction (T1), normalize: every external dep is a submodule, none are flat copies. If vcpkg is chosen, drop submodules entirely.

### T3 — `vendor/sol2/`, `vendor/spdlog/`, etc., bring in their own test suites (confidence: high)

`vendor/sol2/CMakeLists.txt:286 enable_testing()`, `vendor/spdlog/tests/CMakeLists.txt:62 enable_testing()`, etc. These compete with the project's own `enable_testing()` and may register dozens of unrelated tests under `ctest`. Set `CMAKE_DISABLE_FIND_PACKAGE_*`, `SOL2_BUILD_TESTS=OFF`, `SPDLOG_BUILD_TESTS=OFF`, and friends before each `add_subdirectory`. The current `vendor/CMakeLists.txt` (17 lines) is too thin to suppress these properly.

## Recommended Minimal Toolchain Structure

The minimum acceptable structure for reliable iteration. Each line is a deliverable, not a phase.

1. **`CMakePresets.json`** at the repo root with `debug`, `debug-asan`, and `release` presets. Each pins generator (Ninja for Linux, Visual Studio 17 2022 for Windows), compiler flags, `LUMINUMBRA_DEBUG`, and warnings-as-errors.
2. **`.clang-format`**, **`.clang-tidy`**, **`.editorconfig`** at the repo root, scoped to first-party paths via `--exclude` flags or in-file overrides.
3. **`scripts/lint.ps1`** and **`scripts/lint.sh`** that wrap `clang-format --dry-run`, `clang-tidy -p build`, and `cmake --build --target check-format`.
4. **`scripts/dev-loop.ps1`**: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug`. One invocation, one exit code.
5. **`.github/workflows/ci.yml`** running on push and PR: matrix over `windows-latest` and `ubuntu-latest`, builds the `release` preset, runs `ctest`, runs `forge verify --validation-policy fail --testing-policy fail`, runs `lint.sh`. Caches the vendor build via `actions/cache`.
6. **`.github/workflows/asan.yml`**: nightly job on `ubuntu-latest` with the `debug-asan` preset, runs `ctest`. Fails the workflow on any ASan report.
7. **Replace** `file(GLOB ...)` with `file(GLOB CONFIGURE_DEPENDS ...)` everywhere, OR replace with explicit lists + a CI guard that errors on drift.
8. **Move** `.lmesh` outputs to `${CMAKE_BINARY_DIR}/data` (and update runtime path resolution to fall back to the build directory).
9. **Replace** `.gitignore` (currently 8,958 lines) with a curated ~30-line file.
10. **Remove or pin** vendor test suites: `SOL2_BUILD_TESTS=OFF`, `SPDLOG_BUILD_TESTS=OFF`, etc.
11. **Decide submodules vs vcpkg.** Pick one. Document the choice in `docs/`.
12. **Add `forge run .forge/workflows/polyglot-audit-workflow.yaml`** as a periodic (cron or post-merge) job, so the audit panels stay current. This locks in the workflow that produced this report.

## Verification Workflow

After the above lands, the developer loop becomes:

```powershell
# One-time
cmake --preset debug
cmake --preset debug-asan
cmake --preset release

# Per-change
cmake --build --preset debug
ctest --preset debug --output-on-failure
.\scripts\lint.ps1
forge verify --validation-policy warn --testing-policy warn
```

And CI runs the same commands, with `--validation-policy fail --testing-policy fail`.

## Bottom Line

Nothing about this codebase's tooling is broken in a way that prevents builds — it builds, tests run, the workflow ships. The risk is **silent drift**: hand-maintained source lists already wrong, MSVC silently ignoring the Linux-style flags, no CI catching the red tests, vendor trees frozen forever, generated artifacts in source-controlled paths. The fixes are individually small, but each one closes off a category of "works on my machine" failure. The minimum responsible delta — presets + format/tidy + a 40-line CI YAML + asset path cleanup — is a single PR worth of work and unlocks every subsequent refactor with confidence.
