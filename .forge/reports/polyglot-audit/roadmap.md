# Luminumbra Consensus Roadmap

Generated: 2026-06-07

## Inputs

Synthesized from the four audit panels under `.forge/reports/polyglot-audit/`:

- `architecture.md` — module boundaries, runtime lifecycle, layering, refactor candidates.
- `data-models.md` — types, schemas, contracts, persistence shape.
- `quality-risks.md` — defects, concurrency, ownership, error-handling, test gaps.
- `tooling.md` — build, CI, formatter/lint, asset pipeline, dev loop.

Repo signal at synthesis time: status `spec`, no active dispatch, 3 ship-blocker tests failing on `main`, 36 polyglot risk markers, single test binary, no CI workflow, no formatter or static-analysis config.

## Consensus Direction

The four panels agree on three things:

1. **The SHIELD test contract must be locked before anything else.** SDF sign, mesh normal winding, and cave-effect tests are red on `main`. Architecture and data-model panels both call this a P0 / refactor-blocker. Quality-risks ranks it C0. Until those three tests pass, every change to terrain code is unverified.
2. **The runtime layer is mis-shaped, not mis-implemented.** `main_client.cpp` is the de-facto application framework. `RenderPipeline` owns nine responsibilities. `Chunk` exposes mutable simulation/render buffers as public members. `GameSession` mixes JSON parsing, world creation, save/load asymmetry, and direct console I/O. None of the individual systems are wrong; the composition is.
3. **The build and tools surface should be small, explicit, and gated.** Manual source lists drifting from the file tree, no CI, no formatter, generated assets in source paths, 8.9 KLOC `.gitignore`. Each item is a small fix; together they're a credibility step that unlocks everything downstream.

The consensus direction is *not* a rewrite. It is **stabilize-then-extract**: lock the failing contracts, fix the concurrency hazards that block refactors, then peel responsibilities off the oversized coordinators in narrow slices.

## Ideal Repository Structure (Target State)

This is the destination, not the next step. It informs sequencing.

```
luminumbra/
  CMakeLists.txt
  CMakePresets.json                 # debug / debug-asan / release
  .clang-format / .clang-tidy / .editorconfig
  .gitignore                        # ~30 lines, curated
  .github/workflows/                # ci.yml, asan.yml
  cmake/                            # toolchain helpers, no dependency stubs
  include/luminumbra/               # ONLY public headers (Types, IDs, errors)
  src/
    luminumbra_common/              # simulation, deterministic
      core/                         # Job, Log, EventBus, AppState
      domain/                       # NEW: SdfGrid, HeightField, ChunkLodSelection,
                                    # WorldManifestV1, MaterialRegistry, SoundBankV1
      systems/                      # SHIELD_WorldSystem (slimmer), PhysicsSystem,
                                    # WaterSystem
      world/                        # Chunk (private internals), MarchingCubes
      net/ scripting/               # incubate; spec-first
    luminumbra_client/              # presentation
      app/                          # NEW: ClientApplication / RuntimeCoordinator
      rendering/                    # RenderGraph + per-pass owners + ResourceCache
      ui/                           # ONE UI path; legacy parked or removed
      audio/                        # MiniaudioManager + SoundBank loader; advanced
                                    # systems behind feature flags
      player/ debug/
    luminumbra_server/              # spec-first; stays incubate
  tools/                            # asset_processor + tiny utilities
  test/                             # per-module GoogleTest binaries
    common/ client/ tools/
  data/                             # authored content only
  res/shaders/                      # GLSL with build-time validation
  scripts/                          # Lua behavior + dev shell scripts
  worlds/
  assets/                           # source assets; processed outputs go to build/
  docs/                             # specs catch up with code
  .forge/                           # Forge state; reports under polyglot-audit/
```

The biggest deltas vs. today: a real `include/luminumbra/` public header tree, a `src/luminumbra_common/domain/` layer of first-class types, a `src/luminumbra_client/app/` runtime coordinator that takes responsibilities away from `main_client.cpp` and `RenderPipeline`, and per-module test binaries.

## Data Model Direction

Land these types as first-class, in this order:

1. **`SdfGrid` / `ChunkLodSelection` / `HeightField`** — fixes the sign + step ambiguity that anchors the C0 test failures.
2. **`WorldManifestV1`** with `schema_version`, `world_id`, `seed_text`, `seed_u32`, `preset_id`, `created_at`, `last_played_at`, `spawn`, `engine_version`, `content_hashes`. Round-trippable save/load.
3. **`MaterialRegistry`** as the single source of `MaterialType`/`materials.json`/renderer LUT alignment.
4. **`SoundBankV1`** with capability flags, validated against the audio bank JSON.
5. **`AppState`** with typed commands (`CreateWorldRequest`, `LoadWorldRequest`, `WorldLoadingProgress`, `EnterWorld`) replacing duplicated game-state enums and raw-string UI callbacks.
6. **`LMeshHeaderV1`** with version, endian tag, stride, offsets, CRC. Asset processor and runtime loader share the definition.
7. **`ArchetypeDefinition` + `ComponentFactoryRegistry`** for JSON/Lua-driven entity spawning. (Spec phase; implementation behind a flag until the architecture spec is signed off.)

## Framework / Tooling Direction

- **CMake** stays. Add `CMakePresets.json`. Pin warnings-as-errors on MSVC (`/W4 /WX`) and GCC/Clang (`-Wall -Wextra -Werror`).
- **vcpkg** for vendored libraries with clear upstream releases (Jolt, EnTT, GLM, GLFW, spdlog, nlohmann, googletest, FastNoise). Keep small single-header vendor copies (cgltf, miniaudio if still consumed) under `vendor/`.
- **clang-format + clang-tidy** at the repo root. Scope to first-party paths.
- **GitHub Actions** with two workflows: `ci.yml` (push/PR, Windows + Linux, build + ctest + lint) and `asan.yml` (nightly, Linux, debug-asan).
- **Forge** stays as the audit + dispatch surface. The polyglot-audit workflow runs on a cron and writes reports into `.forge/reports/polyglot-audit/`.
- **glslangValidator** for build-time shader validation behind a `LUMINUMBRA_VALIDATE_SHADERS` option.

## Bug List (Must-Fix)

Tagged with the panel that surfaced each item.

| Tag | Description | Source |
|-----|-------------|--------|
| BUG-SHIELD-SDF | SDF sign convention disagrees with tests and Marching Cubes inside-test | quality, data-models |
| BUG-SHIELD-NORMALS | Mesh normals point down on horizontal surface | quality, architecture |
| BUG-SHIELD-CAVES | Cave-enabled and cave-disabled chunks produce identical vertex counts | quality |
| BUG-JOBSYS-COUNTERS | `JobSystem` ring-buffer of 256 atomic counters is reused without ownership rules | quality |
| BUG-JOBSYS-DRAIN | `JobSystem::shutdown` does not drain the pending queue | quality |
| BUG-MESHING-HANDLE | `dispatch_meshing_jobs` discards its `JobHandle` (no shutdown wait) | quality |
| BUG-CHUNKS-RACE | `m_chunks` is mutated/read without synchronization across threads | quality |
| BUG-CHUNKS-PUBLIC | `Chunk` exposes mutable buffers with no ownership protocol | quality, data-models |
| BUG-WORLDSAVE-ASYM | `LoadWorld` drops `spawnPoint`; `worldId` not persisted; no schema version | data-models, quality |
| BUG-LOGGING-MIX | Direct `std::cout`/`std::cerr` in `GameSession`, `AssetManager`, `main_server` | architecture, quality |
| BUG-RENDER-THROW | `RenderPipeline::startup` throws `runtime_error` past main with no clean shutdown | quality |
| BUG-GENERATE-SYNC | `dispatch_generation_jobs` blocks the update path with `wait()` | quality, architecture |
| BUG-FLAGS-MSVC | `-O3 -DNDEBUG`/`-g -Wall` ignored by MSVC; `LUMINUMBRA_DEBUG` not set on multi-config | tooling |
| BUG-SOURCES-DRIFT | `sources.cmake` omits real in-tree `.cpp` files (UI, audio, ExternalImplementations) | architecture, tooling |
| BUG-FILE-COPY-DATA | `file(COPY data ...)` runs only at configure time; runtime data goes stale | tooling |
| BUG-GLOB-NO-DEPS | `file(GLOB assets/*.glb)` lacks `CONFIGURE_DEPENDS` | tooling |
| BUG-GPU-CPU-DIVERGE | GPU SDF path is not exercised by any test | quality |
| BUG-UI-AMBIG | UI manager direction is ambiguous; legacy + new + wrapper coexist | architecture |
| BUG-CMAKE-STALE | `src/luminumbra_client/CMakeLists.txt` is stale but present | architecture, tooling |
| BUG-PERF-FLAKY | Perf tests assert wall-clock thresholds; flaky on slow hardware | quality, tooling |

## Refactor Priorities (Optional, after Must-Fix)

| Tag | Description | Source |
|-----|-------------|--------|
| REF-APP-EXTRACT | Extract `ClientApplication` / `RuntimeCoordinator` from `main_client.cpp` | architecture |
| REF-RENDER-SPLIT | Split `RenderPipeline` into render graph + per-pass owners + resource cache | architecture |
| REF-COMMON-PRIVATE | Move vendor includes out of public common headers; downgrade `PUBLIC` links | architecture |
| REF-INCLUDE-ROOT | Replace `../../../include/luminumbra/...` with `#include <luminumbra/...>` | architecture |
| REF-CHUNK-DOMAIN | Split `Chunk` into `SdfGrid`, `HeightField`, `MeshBuffers`, `WaterGrid` | data-models, quality |
| REF-MATERIAL-REGISTRY | Make `MaterialRegistry` canonical for ID / LUT / textures | data-models |
| REF-APPSTATE-COMMANDS | Centralize `AppState`; replace raw-string UI callbacks with typed commands | data-models |
| REF-MESHING-CACHE | Replace per-call `unordered_map<u64, u32>` in `PolygoniseTerrain` with direct-indexed array | quality |
| REF-PHYSICS-DOMAIN | Move audio-specific raycast/query concepts out of `PhysicsSystem` | architecture |
| REF-SOURCES-AUTO | Replace manual source lists with `file(GLOB CONFIGURE_DEPENDS ...)` or a CI drift check | tooling |
| REF-VENDOR-VCPKG | Move stable deps to vcpkg manifest mode; keep single-header copies vendored | tooling |
| REF-PER-MODULE-TESTS | Split test/ into per-module GoogleTest binaries | tooling, quality |
| REF-SHADER-VALIDATE | Wire `glslangValidator` into the build behind a flag | tooling |
| REF-AUDIO-BANK-VALIDATE | Add JSON Schema + `from_json` validator for audio banks | data-models, tooling |
| REF-ARCHETYPE-WIRE | Land `ArchetypeDefinition` + `ComponentFactoryRegistry` (spec first) | data-models |
| REF-LMESH-V1 | Versioned `.lmesh` header with CRC + offsets; loader validates strictly | data-models |
| REF-LUA-MIN | Either flesh out `LuaState` + smoke test, or remove from build entirely | architecture, tooling |
| REF-UI-DECIDE | Either keep legacy RmlUi (delete Quantum stubs), or migrate (with spec + tests) | architecture |
| REF-INSTINCT-SPEC | Write specs for Instinct Engine and Aetheric Field before any implementation | architecture |
| REF-LOG-FEATURE-FLAGS | Separate `LUMINUMBRA_DEBUG` from "GL debug callbacks" and "verbose logging" | quality |

## Staged Roadmap

Sequencing constraint: tooling/CI work must land before refactors so each refactor is gated. SHIELD bug fixes must land before any terrain refactor so the contract is locked. UI direction must be decided before extracting the runtime coordinator so the coordinator knows what to coordinate.

### Stage 0 — Lock the Contract (1–2 days, blocks everything)

Goal: green ctest on `main`, with the SDF convention written down.

- **S0.1** Fix `BUG-SHIELD-SDF`: choose inside-negative (matches tests, matches MC default). Flip the sign in `get_density_at_from_precalculated()` and the precalculated SDF write in `GenerateChunkData()`.
- **S0.2** Verify `BUG-SHIELD-NORMALS` passes after S0.1 (it should follow from the sign fix). If not, audit Marching Cubes winding and corner ordering.
- **S0.3** Fix `BUG-SHIELD-CAVES`: with the new sign convention, confirm the cave term `std::min(terrain_density, cave_density)` still crosses the isosurface. Adjust `cave_carve_value` semantics if needed; update the test if the new semantics make the original assertion obsolete.
- **S0.4** Document the SDF + MC contract in `docs/shield/sdf-contract.md`: sign convention, isolevel, step semantics, normal direction.
- **S0.5** Add a `LOD_MeshingProducesContiguousNormals` regression test that locks the winding for cells with mixed `step` values.

**Acceptance.** `ctest --test-dir build --output-on-failure` is green; docs link from `README.md`.

### Stage 1 — Make the Build Trustworthy (2–3 days, blocks all later refactors)

Goal: builds reproducibly, lints, fails CI on test breakage.

- **S1.1** Land `CMakePresets.json` with `debug`, `debug-asan`, `release` presets.
- **S1.2** Add `.clang-format`, `.clang-tidy`, `.editorconfig`. Land `scripts/lint.ps1`, `scripts/lint.sh`. Run once on first-party paths and commit the format pass as its own change.
- **S1.3** Add `.github/workflows/ci.yml`: Windows + Linux, build the `release` preset, run `ctest`, run lint, run `forge verify`. Cache vendor build.
- **S1.4** Add `.github/workflows/asan.yml`: nightly, Linux, `debug-asan`.
- **S1.5** Reconcile `BUG-SOURCES-DRIFT`: either move to `file(GLOB CONFIGURE_DEPENDS ...)` or add a CI guard. Remove stale `src/luminumbra_client/CMakeLists.txt` (`BUG-CMAKE-STALE`).
- **S1.6** Fix `BUG-FLAGS-MSVC`: pin `/W4 /WX` and `-Wall -Wextra -Werror` per compiler. Define `LUMINUMBRA_DEBUG` via target `compile_definitions` for the debug preset, not via `CMAKE_BUILD_TYPE` string comparison.
- **S1.7** Fix `BUG-GLOB-NO-DEPS` (`CONFIGURE_DEPENDS`) and `BUG-FILE-COPY-DATA` (move to a `copy_data` build-time target).
- **S1.8** Replace the 425 KB `.gitignore` with a curated file. Delete `build_error.log` and `imgui.ini` from the index; add them to ignore.
- **S1.9** Reconcile `external/` vs `vendor/` duplicates (FastNoise, GLM). Pick one path per dependency.

**Acceptance.** A clean clone runs `cmake --preset debug-asan && cmake --build --preset debug-asan && ctest --preset debug-asan` and the CI workflow shows green on a no-op PR.

### Stage 2 — Fix Concurrency Hazards (3–5 days)

Goal: the runtime cannot tear chunk state under load.

- **S2.1** `BUG-JOBSYS-COUNTERS`: replace the 256-slot ring with `shared_ptr<atomic<int>>` per batch (or per-handle owned counter). `JobHandle` owns the counter; reuse is impossible by construction.
- **S2.2** `BUG-JOBSYS-DRAIN`: on `shutdown`, drain the queue (run remaining jobs to completion) before joining workers. Reject new `dispatch` after `m_stop_threads` is set.
- **S2.3** `BUG-MESHING-HANDLE`: store the meshing batch handle and `wait()` on it during `SHIELD_WorldSystem::clear_world()` and at app shutdown.
- **S2.4** `BUG-CHUNKS-RACE`: wrap `m_chunks` with an `std::shared_mutex`. Reads (`get_renderable_chunks`, water update) take a shared lock; writes (`update_chunk_activation`) take a unique lock. As a follow-up (REF-CHUNK-DOMAIN), introduce a `WorldStreamingState` that owns the map and exposes typed snapshots.
- **S2.5** `BUG-GENERATE-SYNC`: remove the inline `wait()` from `dispatch_generation_jobs`. Let chunk activation be async; the post-wait debug scan moves into a `forge`-style diagnostic command or behind `LUMINUMBRA_DEBUG_WORLDGEN`.
- **S2.6** `BUG-RENDER-THROW`: catch the `runtime_error` in `main_client.cpp::main()` and shut down systems in reverse order. Or remove the throw in favor of a logged fatal + `return -1`.

**Acceptance.** New tests (`test/common/JobSystem_test.cpp`) cover: 1000-batch stress with intermixed waits, shutdown-with-pending-jobs, recovery after worker exit. ASan workflow passes.

### Stage 3 — Fix Persistence + Logging (2–3 days)

Goal: world creation/load can round-trip; failures surface through the engine logger.

- **S3.1** `BUG-LOGGING-MIX`: replace every `std::cout`/`std::cerr` in `GameSession.cpp`, `AssetManager.h`, `main_server.cpp` with `LUMINUMBRA_CORE_*` macros. Run `Log::Init()` from `main_server.cpp` if it exists; otherwise add the init.
- **S3.2** Land `WorldManifestV1` per data-models panel. Implement `LoadWorld` to restore `spawnPoint`, persist `worldId`, and reject mismatched `schema_version`. Add a save/load round-trip test (`test/common/GameSession_test.cpp`).
- **S3.3** Add `from_json` validators for world preset JSON; reject unknown top-level fields with a clear error.

**Acceptance.** Save-then-load preserves spawn point. New manifest tests cover round-trip, unknown-field rejection, version-mismatch rejection.

### Stage 4 — Decide UI Path (1–3 days, depends on owner decision)

Goal: there is one UI manager, and the build only compiles it.

- **S4.1** **Owner decision** (not Claude's call): keep legacy `Rml_UIManager`, OR migrate to the component UI system, OR commit to a parallel-bridge with a deadline.
- **S4.2** Whichever path: delete the others' source files (or move them under `incubate/`) and update `sources.cmake`. Update `.forge/architecture.toml` accordingly.
- **S4.3** Convert the UI TODOs surfaced by the polyglot audit into either implementation tasks or removed-dead-path PRs. No naked TODOs remain in the active source tree.

**Acceptance.** Exactly one UI manager symbol in the active build. Polyglot risk-marker count for UI TODOs drops to zero.

### Stage 5 — Extract the Runtime Coordinator (3–5 days)

Goal: `main_client.cpp` becomes a thin entry point; runtime state lives in named classes.

- **S5.1** Introduce `Client::ClientApplication` (or `Client::RuntimeCoordinator`) that owns: GLFW window, ImGui context, `JobSystem`, `GameSession`, audio manager, UI manager, `RenderPipeline`, `WorldLoadingVisualizer`, `Camera`, `PlayerController`, `WorldGenViewer`. It exposes `startup() / run() / shutdown()` and consumes `AppState` events.
- **S5.2** Remove the global pointers in `main_client.cpp`. Callbacks become member functions that capture `this` via `glfwSetWindowUserPointer`.
- **S5.3** Wire `AppState` per data-models panel: typed commands for world creation/load/play, replacing raw-string UI callbacks.

**Acceptance.** `main_client.cpp` is <100 lines. The runtime coordinator has its own smoke test (`test/client/ClientApplication_smoke_test.cpp`) that fakes GLFW.

### Stage 6 — Split RenderPipeline (5–8 days)

Goal: `RenderPipeline.cpp` is not 1,184 lines.

- **S6.1** Introduce a `RenderGraph` (or named pass list) that orchestrates: G-buffer, shadow, SSAO, lighting, water, skybox, hierarchical culling.
- **S6.2** Move GL resource lifetime into per-pass owners (each pass owns its FBOs, programs, uniform locations).
- **S6.3** Move chunk/water upload caches into a `ChunkGpuResourceCache` consumed by relevant passes.
- **S6.4** Move static mesh instancing into a `StaticMeshRenderer`.
- **S6.5** Move GPU SDF generation into a `GpuSdfGenerator` that the world system depends on as a strategy.
- **S6.6** Add a snapshot test for each pass (render a known scene, hash the framebuffer, compare).

**Acceptance.** No single file in `src/luminumbra_client/rendering/` exceeds 600 LOC. Snapshot tests run in CI.

### Stage 7 — Domain Types and Per-Module Tests (rolling)

Goal: first-class domain types and one test binary per module.

- **S7.1** `SdfGrid`, `HeightField`, `ChunkLodSelection`, `MeshBuffers`, `WaterGrid` as types. `Chunk` becomes a thin owner.
- **S7.2** `MaterialRegistry` as the single source for ID/name/LUT/textures. Renderer derives terrain texture array from it.
- **S7.3** `SoundBankV1` validator. `MiniaudioManager` loads only the validated subset; unsupported strategies are rejected with a logged warning + fallthrough.
- **S7.4** `ArchetypeDefinition` + `ComponentFactoryRegistry`. Spec-first (write the spec in `.forge/specs/` before implementation).
- **S7.5** `LMeshHeaderV1` versioned format. Asset processor writes V1; loader validates `magic`, `version`, `endian_tag`, `vertex_stride`, `index_size`, counts, offsets, `crc32`.
- **S7.6** Split `test/` into per-module binaries. Convert perf tests into a `LUMINUMBRA_RUN_PERF_TESTS`-gated binary.

**Acceptance.** Each domain type has tests covering construction, invariants, serialization. Per-module test binaries run in parallel in CI.

### Stage 8 — Specs Catch Up With Code (parallel)

Goal: documentation matches implementation tier.

- **S8.1** Write `.forge/specs/INSTINCT-ENGINE.md`: scope, components, owner, decision points. Do not implement until the spec is signed off.
- **S8.2** Write `.forge/specs/AETHERIC-FIELD.md` with the same shape.
- **S8.3** Update `README.md`: move premature claims for "Quantum UI complete," "Instinct Engine implemented," and "Atmospheric Engine" into a "planned" section. Replace with a list of currently-shipping features.

## Must-Fix vs Optional

**Must-fix before any architecture refactor:** S0 (lock contract), S1 (tooling baseline), S2 (concurrency hazards), S3.1 (logging unification).

**Optional improvements** (do as time allows, but they unlock major productivity gains): S3.2–S3.3, S4, S5, S6, S7, S8.

## Risks and Open Questions

- **GPU SDF parity.** The runtime can use a GPU SDF path that is not under test. Adding a CI test for parity (CPU vs GPU SDF for known seeds) is on the critical path before S2, otherwise S2 fixes the wrong layer.
- **UI direction is an owner call.** S4 is the only stage gated on a human decision, not on engineering work. If the decision delays, the rest of S5/S6 can proceed with the legacy UI as the assumed target.
- **Asset processing in source paths.** Switching to `${CMAKE_BINARY_DIR}/data` means every consumer of `.lmesh` files needs path-resolution updates. This is a Stage 1 work item but has downstream implications worth flagging now.
- **Vendor migration to vcpkg.** This is in REF-VENDOR-VCPKG but not strictly required for CI. Scheduling it is an owner call; lower priority than the stage-ordered work above.

## Verification Commands

After the staged work lands:

```powershell
# Local
cmake --preset debug-asan
cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure
.\scripts\lint.ps1
forge verify --validation-policy fail --testing-policy fail

# CI runs the same with `release` and `debug-asan` presets.
```

## Bottom Line

Lock SHIELD → trust the build → fix the concurrency → fix persistence + logging → decide UI → extract the runtime → split the renderer → harden the domain. Eight stages, the first four are non-negotiable, the last four reward the investment. Nothing in this roadmap is a rewrite; everything is "do the thing the code already wants to do, in the order the contracts demand."
