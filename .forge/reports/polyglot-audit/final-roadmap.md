# Luminumbra Final Roadmap (Post-Critique + Post-Review)

Generated: 2026-06-07
Revised: 2026-06-07 (post-review)

## Revision History

- **v1** — Absorbed the 15 concrete changes from `critique.md`.
- **v2 (this document)** — Absorbed the findings of an independent multi-agent review pass:
  - **Owner decisions recorded** (5 of 5; see "Owner Decisions" section below). The roadmap no longer hand-waves them as "open."
  - **H5 / H6 hand-waves resolved** — Forge cache invalidation is now a concrete S1.10 deliverable; vcpkg is explicitly *rejected* with a documented rationale (stay-vendored, delete `external/` and `vendor/test/`).
  - **Coupling risks surfaced** that the prior revision missed: S1.7 ↔ S7d (asset path move vs. LMesh V1 reprocess), S2.4 ↔ S7a (chunk-domain refactor overlap), S5.1 takes deps on mid-flight refactors.
  - **Dropped panel items reinstated**: WaterSystem first-transform fix, asset-processor round-trip test, `LUMINUMBRA_ASSERT`-in-Release fix, strong-typed IDs.
  - **`docs/build/dependency-policy.md`** is now an explicit deliverable of S1.0 (was orphaned before).
  - **Dispatch realism bugs caught and fixed** — see `.forge/tasks/polyglot-audit-roadmap/dispatch.json` v2: T-S0-1 now covers SHIELD_WorldSystem.cpp:354 (GPU-path surface detection); T-S0-3 covers both cave call sites at :287 and :430; T-S0-6 links GLFW into the test binary; T-S2-1 modifies main_client.cpp + WaterSystem.cpp.

## How This Differs From `roadmap.md`

This revision absorbs the 15 concrete changes from `critique.md` and the follow-on review pass. Headline shifts:

- **Stage 0 grows** to include a CPU/GPU SDF parity test as the first sub-step and acknowledges the cave-fix may require parameter tuning, not just a sign flip.
- **Stage 2 is rescoped** so the chunk-internal data race is explicitly handled (REF-CHUNK-DOMAIN's snapshot work pulled forward), not deferred behind a fix that masks the bigger race. **Stage 2.4 is now split into 4 sub-tasks** in the dispatch (4a–4d) to avoid the 9-file/6-dep coordination bomb.
- **Stage 4 (UI decision) is decoupled** from Stage 5 (runtime coordinator) so the engineering work doesn't stall on an owner decision. **Owner has decided**: keep legacy `Rml_UIManager` behind `IUIManager`, delete the component UI scaffolding.
- **Stage 5 gains S5.5** — a headless render-snapshot harness — as a prerequisite to Stage 6.
- **Stage 7 is decomposed** into S7a–S7f with explicit ordering and dependencies.
- **Forge schema concerns** (dispatch shape, cache invalidation) are pulled into S0/S1.
- **Day estimates are removed in favor of effort tags** (XS/S/M/L/XL). Estimates were unbacked guesses; effort tags signal relative scale without lying about precision.

## Effort Sizing

- **XS** — single sitting (<1 day equivalent), one file, one test.
- **S** — small focused change with a handful of files and an existing test surface.
- **M** — a few files, may need new tests, requires reading more code than written.
- **L** — multi-stage refactor with new interfaces, requires new tests, touches many call sites.
- **XL** — design + implementation + test infra at once. Reserved for the renderer split and the chunk-domain extraction.

## Staged Roadmap

### Stage 0 — Lock the SHIELD Contract

**Goal.** Three failing tests pass on `main`; SDF + Marching Cubes contract is written down; CPU and GPU paths produce identical SDFs for known seeds.

- **S0.0 (XS, blocks everything in S0)** — Add `test/shield/test_sdf_gpu_cpu_parity.cpp`. Compares `SHIELD_WorldSystem::GenerateChunkData()` (CPU) output to the GPU SDF compute shader output for three known seeds and three chunks. Test is initially marked `GTEST_SKIP()` until the GPU path is invoked in the test build (a follow-up). The point is to have the harness in place so the sign decision is validated against both paths immediately.
- **S0.1 (S)** — Fix `BUG-SHIELD-SDF`. Choose inside-negative (matches the test expectations and standard MC convention). Flip the sign in `SHIELD_WorldSystem::get_density_at_from_precalculated()` (`SHIELD_WorldSystem.cpp:279`) and in the precalculated SDF write inside `GenerateChunkData()` (`SHIELD_WorldSystem.cpp:420, 440`). Also flip in the GPU compute shader under `res/shaders/` that produces SDF — find via `Grep` for "terrain_height" and "density" in `*.compute` files.
- **S0.2 (S)** — Verify `BUG-SHIELD-NORMALS` passes after S0.1. If `MarchingCubesTables.inl` was authored against inside-positive (suspected risk per critique C1), the triangle winding will still be inverted. The fix is to either invert winding in the table-consuming loop (`MarchingCubes.cpp:164`) or flip face-normal sign in pass 2. Pick the smaller change. Document the table convention in `docs/shield/sdf-contract.md`.
- **S0.3 (M)** — Fix `BUG-SHIELD-CAVES`. After the sign flip, audit whether `cave_density` term still crosses the isosurface in the test fixture. If not, this is a parameter-tuning + test-update task: the carve term has to actually produce sub-isolevel values where the test points are. The roadmap explicitly authorizes updating `test_world_generation.cpp` to match the corrected cave contract if the existing assertion is unsalvageable.
- **S0.4 (S)** — Write `docs/shield/sdf-contract.md`: sign convention, isolevel, step semantics, normal direction, cave-carve formula, valid `cave_threshold`/`cave_carve_value` ranges. Link from `README.md`. This is the deliverable that locks the contract forever.
- **S0.5 (S)** — Add a `MeshNormalWindingIsConsistentUnderLodStep` regression test that locks the winding for cells with mixed `step` values (`step=1, 2, 4`).
- **S0.6 (XS)** — Make the GPU/CPU parity test from S0.0 active (un-skip) once S0.1 is in. Run in `debug-asan` CI.

**Acceptance.** `ctest --test-dir build --output-on-failure` is green. `docs/shield/sdf-contract.md` exists and is linked from `README.md`. GPU and CPU SDF agree to within `1e-5` for the parity test fixtures.

### Stage 1 — Make the Build Trustworthy

**Goal.** Reproducible builds with one local + two CI workflows, lints + format gates, no source-list drift, no source-tree generated assets, curated `.gitignore`.

- **S1.0 (XS)** — **Owner decision recorded: stay-vendored.** Deliverable: write `docs/build/dependency-policy.md` documenting (1) the stay-vendored decision, (2) the criteria for revisiting (CVE pressure, repo size friction, frequent upstream churn), (3) the cleanup of `external/` and `vendor/test/` duplicates landing in S1.9. Implemented as dispatch task `T-S1-0-dependency-policy-decision`.
- **S1.1 (S)** — Land `CMakePresets.json` with `debug`, `debug-asan`, `release` presets. Each pins generator (Ninja for Linux, Visual Studio 17 2022 for Windows), compiler-conditional flags, `LUMINUMBRA_DEBUG` as a target compile-definition (not a `CMAKE_BUILD_TYPE` string check), and `LUMINUMBRA_RUN_PERF_TESTS=OFF`.
- **S1.2 (S)** — Add `.clang-format`, `.clang-tidy`, `.editorconfig`. Scoped to first-party paths via `--exclude` or `.clang-tidy` `HeaderFilterRegex`. Land `scripts/lint.ps1` + `scripts/lint.sh`. Run the format pass once and commit as its own change (so the diff is reviewable separately).
- **S1.3 (M)** — Add `.github/workflows/ci.yml`: matrix over `windows-latest` and `ubuntu-latest`, build the `release` preset, run `ctest`, run lint, run `forge verify --validation-policy fail --testing-policy fail`. Cache vendor build via `actions/cache` keyed on `vendor/**/CMakeLists.txt` hash.
- **S1.4 (S)** — Add `.github/workflows/asan.yml`: nightly on `ubuntu-latest` with the `debug-asan` preset. Runs `ctest`, fails on any ASan report.
- **S1.5 (S)** — Reconcile `BUG-SOURCES-DRIFT`. Replace manual `sources.cmake` lists with `file(GLOB_RECURSE CONFIGURE_DEPENDS ...)`. Remove the stale `src/luminumbra_client/CMakeLists.txt` (`BUG-CMAKE-STALE`). **Acceptance note:** post-pull, developers must `rm -rf build/` once; document in `CONTRIBUTING.md`.
- **S1.6 (S)** — Fix `BUG-FLAGS-MSVC`. Apply `/W4 /WX /permissive- /Zc:__cplusplus` for MSVC and `-Wall -Wextra -Werror` for GCC/Clang to first-party targets only (`luminumbra_common`, `luminumbra_client_app`, `asset_processor`, test binaries). Vendor warnings are suppressed via `target_compile_options(... PRIVATE ...)` or by setting `CMAKE_DISABLE_FIND_PACKAGE_*` on vendor `add_subdirectory` calls.
- **S1.7 (S)** — Fix `BUG-GLOB-NO-DEPS` (`CONFIGURE_DEPENDS` on the asset glob) and `BUG-FILE-COPY-DATA`. Replace `file(COPY data ...)` with an `add_custom_target(copy_data ALL COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/data)` build-time target. Move `.lmesh` outputs to `${CMAKE_BINARY_DIR}/data/` and update runtime path resolution in `MeshLoader::Load` to check both source-`data/` and build-`data/`.
- **S1.8 (XS)** — Replace the 8,958-line `.gitignore` with a curated file (~30 lines). **Owner confirmation required** before deleting `imgui.ini` from the index — confirm whether the committed debug-viewer layout is intentional or accidental.
- **S1.9 (S)** — Reconcile `external/` vs `vendor/` duplicates (FastNoise, GLM). Pick one source per dependency per the S1.0 policy. Update CMake includes/links accordingly.
- **S1.10 (S)** — Suppress vendor test suites. Set `BUILD_TESTING=OFF`, `SOL2_BUILD_TESTS=OFF`, `SPDLOG_BUILD_TESTS=OFF`, `JSON_BuildTests=OFF`, `GTEST_*` overrides, etc., **before** each `add_subdirectory` in `vendor/CMakeLists.txt`. (Renumbered from S1.11 — the prior S1.10 was hand-waved Forge-cache-policy work and is now resolved at the cron+config level: the CI workflow gates panel re-runs on `.forge/structure.md` content-hash change via `actions/cache` keyed on the hash, no separate Forge-side feature needed.)

**Acceptance.** Clean clone → `cmake --preset debug-asan && cmake --build --preset debug-asan && ctest --preset debug-asan` green. CI workflow green on a no-op PR. `ctest` lists only first-party tests (no `sol2_*`, `nlohmann_*`, etc.). Format gate fails the CI build on a deliberate mis-formatted commit.

### Stage 2 — Fix Concurrency + Chunk Ownership

**Goal.** The runtime cannot tear chunk state under load. Both the map-level race and the chunk-internal race are addressed in this stage, not deferred.

- **S2.1 (M)** — `BUG-JOBSYS-COUNTERS`. Replace the 256-slot ring with a per-handle owned counter: `JobHandle` holds `std::shared_ptr<std::atomic<int>>` instead of a raw pointer. Lambdas capture the shared_ptr by value. The counter lives until every reference is dropped. **Migration callout:** audit every existing `JobHandle` consumer (search `JobHandle` across `src/`) and confirm capture-by-value semantics.
- **S2.2 (S)** — `BUG-JOBSYS-DRAIN`. `JobSystem::shutdown` drains the queue (joins pending jobs) before signaling workers to exit. Reject new `dispatch` calls after `m_stop_threads` is set with a logged error.
- **S2.3 (S)** — `BUG-MESHING-HANDLE`. `SHIELD_WorldSystem::dispatch_meshing_jobs` stores the returned `JobHandle` in a member `m_meshing_handle`. `clear_world()` and the destructor wait on it before erasing chunks.
- **S2.4 (L)** — `BUG-CHUNKS-RACE` + `BUG-CHUNKS-PUBLIC`. Introduce `WorldStreamingState` that owns `m_chunks` with `std::shared_mutex`. Reads obtain shared snapshots: `get_renderable_chunks()` returns `std::vector<std::shared_ptr<const ChunkSnapshot>>`, where `ChunkSnapshot` is an immutable view (coords, current_lod, mesh_version, and a `shared_ptr<MeshBuffers>` to the current mesh). Meshing jobs publish a new `MeshBuffers` shared_ptr via `chunk->publish_mesh(...)` — the chunk's internal `current_mesh` shared_ptr is atomic. This eliminates both the map-level and the chunk-internal race.
- **S2.5 (S)** — `BUG-GENERATE-SYNC`. Remove the inline `wait()` from `dispatch_generation_jobs`. The post-wait debug scan moves into a `forge`-style diagnostic command behind `LUMINUMBRA_DEBUG_WORLDGEN`. **Acceptance note (H2):** verify `ChunkState::Loading` blocks meshing eligibility. Add a regression test that proves the meshing pass skips `Loading` chunks.
- **S2.6 (S)** — `BUG-RENDER-THROW`. Replace the `throw std::runtime_error("Shadow map cascade splits not initialized!")` in `RenderPipeline::startup` with a logged fatal + return-error. Wrap pipeline startup in `main_client::main()` with explicit error handling and ordered shutdown.

**Acceptance.** New tests under `test/common/JobSystem_test.cpp` and `test/common/WorldStreamingState_test.cpp` cover: 1000-batch stress with intermixed waits, shutdown-with-pending-jobs, snapshot reads during concurrent inserts/erases, meshing-skips-Loading-chunks. ASan workflow passes. JobSystem branch coverage ≥80%.

### Stage 3 — Fix Persistence + Logging

**Goal.** World creation/load round-trips; all error paths surface through the engine logger.

- **S3.1 (M)** — `BUG-LOGGING-MIX`. Replace every `std::cout`/`std::cerr` in `GameSession.cpp`, `AssetManager.h`, `main_server.cpp` with `LUMINUMBRA_CORE_*` macros. Verify `Log::Init()` is called before any logging in each binary entry point. **Open question (H3):** if `Log::Init()` has dependencies that don't fit a future server-only binary, factor it into a `Log::InitMinimal()` that initializes only the `spdlog` sink without GL or filesystem dependencies.
- **S3.2 (M)** — Land `WorldManifestV1` per the data-models panel. Required fields: `schema_version`, `world_id`, `display_name`, `seed_text`, `seed_u32`, `preset_id`, `created_at`, `last_played_at`, `spawn`, `engine_version`, `content_hashes`. `LoadWorld` restores `spawnPoint` and `worldId`; rejects mismatched `schema_version` with a clear error. **Open question (M7):** `engine_version` source — pick git SHA injected by CMake at build time (`git rev-parse --short HEAD`). Document the choice in the manifest spec.
- **S3.3 (S)** — Add `from_json` validators for world preset JSON. Reject unknown top-level fields with a logged warning (not a hard error — content authors should be able to add fields ahead of code support).

**Acceptance.** Save-then-load preserves spawn point and world ID. New tests under `test/common/GameSession_test.cpp` cover round-trip, unknown-field handling, version-mismatch rejection. No `std::cout`/`std::cerr` in `src/luminumbra_*/` (lint-enforced via a `grep` step in CI).

### Stage 4 — UI Direction (decoupled from S5)

**Goal.** There is one UI manager symbol in the active build. **Decoupled per critique C4:** S5 (runtime coordinator) consumes UI through an `IUIManager` interface, so the runtime can be extracted before the legacy/component decision is made. The interface lets either implementation plug in.

- **S4.0 (XS)** — Add `IUIManager` interface in `src/luminumbra_client/ui/IUIManager.h`. `Rml_UIManager` becomes the first implementation. The runtime coordinator (S5) consumes through this interface.
- **S4.1** — **Owner decision** (not Claude's call): keep legacy `Rml_UIManager`, OR migrate to the component UI system, OR commit to a parallel-bridge with a deadline. This decision **does not block S5** thanks to S4.0.
- **S4.2 (M, gated on S4.1)** — Whichever path: delete the others' source files (or move them under `incubate/`) and update build configs. Update `.forge/architecture.toml`.
- **S4.3 (S, gated on S4.1)** — Convert UI TODOs surfaced by the polyglot audit into either implementation tasks or removed-dead-path PRs. No naked TODOs remain in the active source tree.

**Acceptance.** Exactly one `IUIManager` implementation in the active build (post-S4.2). Polyglot risk-marker count for UI TODOs is zero.

### Stage 5 — Extract the Runtime Coordinator

**Goal.** `main_client.cpp` is a thin entry point; runtime state lives in named classes.

- **S5.1 (L)** — Introduce `Client::ClientApplication` that owns: GLFW window, ImGui context, `JobSystem`, `GameSession`, audio manager, `IUIManager`, `RenderPipeline`, `WorldLoadingVisualizer`, `Camera`, `PlayerController`, `WorldGenViewer`. Exposes `startup() / run() / shutdown()` and consumes `AppState` events.
- **S5.2 (M)** — Remove the global pointers in `main_client.cpp`. GLFW callbacks become member functions invoked via `glfwSetWindowUserPointer(window, this)` + a static thunk that dispatches to `ClientApplication::on_key()` etc. **Migration callout (H4):** verify GLFW callback signatures match the user-pointer pattern; some callbacks (error callback) are window-independent and need a different approach.
- **S5.3 (M)** — Wire `AppState` per data-models panel: typed commands for world creation/load/play, replacing raw-string UI callbacks.
- **S5.5 (L, prerequisite to Stage 6)** — Establish a **headless render-snapshot harness**. Headless GL context (EGL on Linux, ANGLE or osmesa on Windows CI), framebuffer-to-PNG, deterministic-seed scene loader, hash comparison with a per-test tolerance for floating-point determinism. Add one initial snapshot test (g-buffer of a flat-terrain chunk) to lock the baseline.

**Acceptance.** `main_client.cpp` is <100 lines. `test/client/ClientApplication_smoke_test.cpp` exercises a no-window run with a stubbed `IUIManager`. The snapshot harness runs in CI on the `release` preset.

### Stage 6 — Split RenderPipeline

**Goal.** No single rendering file exceeds 600 LOC. Snapshot tests gate each pass.

- **S6.1 (M)** — Introduce `RenderGraph` orchestrating: G-buffer, shadow, SSAO, lighting, water, skybox, hierarchical culling.
- **S6.2 (L)** — Move GL resource lifetime into per-pass owners. Each pass owns its FBOs, programs, uniform locations.
- **S6.3 (M)** — Move chunk/water upload caches into `ChunkGpuResourceCache`.
- **S6.4 (S, verify-then-do)** — **M4 from critique:** verify static mesh instancing actually exists before scoping `StaticMeshRenderer`. If `geometry_pass_static_meshes` is a stub, drop this sub-step.
- **S6.5 (M)** — Move GPU SDF generation into `GpuSdfGenerator`. The world system depends on it as a strategy.
- **S6.6 (M)** — Add a snapshot test for each pass (g-buffer, shadow, SSAO, lighting composite, water, skybox). Lock the framebuffer hash. The harness lands in S5.5.

**Acceptance.** No file in `src/luminumbra_client/rendering/` exceeds 600 LOC. Snapshot tests for all six passes run in CI. Renderer branch coverage ≥60% (lower than other modules because GL-dependent code is harder to cover; raise the bar later).

### Stage 7 — Domain Types and Per-Module Tests (decomposed per critique C6)

Sequenced sub-stages, each gated on prior dependencies.

- **S7a (M, gated on S0)** — `SdfGrid`, `HeightField`, `ChunkLodSelection`, `MeshBuffers`, `WaterGrid` as types. `Chunk` becomes a thin owner. Tests cover construction, invariants, indexing helpers.
- **S7b (M, gated on S6.2)** — `MaterialRegistry` as single source for ID/name/LUT/textures. Renderer derives terrain texture array from it.
- **S7c (M, independent)** — `SoundBankV1` validator. `MiniaudioManager` loads only the validated subset.
- **S7d (M, gated on S1.7)** — `LMeshHeaderV1` versioned format. Asset processor writes V1; loader validates `magic`, `version`, `endian_tag`, `vertex_stride`, `index_size`, counts, offsets, `crc32`. **Format change callout (M5):** existing `.lmesh` files must be reprocessed; document in `docs/build/asset-pipeline.md`.
- **S7e (M, gated on S1.3)** — Split `test/` into per-module GoogleTest binaries. Convert perf tests into a `LUMINUMBRA_RUN_PERF_TESTS`-gated binary.
- **S7f (M, spec-first, gated on S8.1)** — `ArchetypeDefinition` + `ComponentFactoryRegistry`. Implementation only after the `INSTINCT-ENGINE.md` spec is signed off.

**Acceptance per sub-stage.** Each new type has tests covering construction, invariants, and (where applicable) serialization. Branch coverage target ≥80% for new domain types.

### Stage 8 — Specs Catch Up With Code (parallel, late)

- **S8.1 (M, gated on S5 completion per M2)** — Write `.forge/specs/INSTINCT-ENGINE.md`: scope, components, owner, decision points, deterministic simulation rules. Do not implement until signed off.
- **S8.2 (M, gated on S5)** — Write `.forge/specs/AETHERIC-FIELD.md` with the same shape.
- **S8.3 (S)** — Update `README.md`: move premature claims for "Quantum UI complete," "Instinct Engine implemented," "Atmospheric Engine" into a "planned" section. Replace with a list of currently-shipping features.

## Must-Fix vs Optional

**Must-fix before any architecture refactor:** S0, S1, S2, S3.1.

**Strongly recommended (unlocks downstream work):** S3.2, S3.3, S4.0, S5, S7e.

**Owner-decision implementations** (now scheduled in dispatch as `T-OD*`): delete dead UI scaffolding (S4 → `T-OD5`), embed git-SHA `engine_version` (S3.2 → `T-OD4`), delete `external/` + `vendor/test/` duplicates (S1.9, prompt updated), gitignore `imgui.ini` (S1.8, prompt updated).

**Newly-reinstated panel items** (were silently dropped from v1; now scheduled): WaterSystem first-transform → `T-DR-watersystem-camera-handle-fix`; asset-processor round-trip test → `T-DR-asset-processor-round-trip-test`; `LUMINUMBRA_ASSERT` Release behavior → `T-DR-luminumbra-assert-release-behavior`; `GameSession.h:27` dead namespace → `T-DR-fix-broken-game-session-fwd-decl`. Strong-typed IDs (`WorldId`, `MaterialId`, `ChunkCoord`) remain in S7 (S7a/S7b), not promoted to must-fix.

**Optional (do as bandwidth allows):** S6, S7a–S7f, S8.

## Coupling Risks Surfaced by Review

The review pass identified three coupling risks the original critique missed. Mitigations:

- **S1.7 ↔ S7d (asset path move vs. LMesh V1 reprocess):** S1.7 moves `.lmesh` output to `${CMAKE_BINARY_DIR}/data/` and updates `MeshLoader::Load`. S7d ships `LMeshHeaderV1` which forces reprocessing all existing `.lmesh` files. If S1.7 lands first, devs hit broken-asset state until they wipe `build/` *and* the asset processor runs. **Mitigation**: T-S1-7's prompt documents both the source-to-build move and the fallback in `MeshLoader::Load`. S7d must consume `T-DR-asset-processor-round-trip-test` (now wired) before format change.
- **S2.4 ↔ S7a (chunk-domain refactor overlap):** S2.4 introduces `WorldStreamingState`, `ChunkSnapshot`, atomic `shared_ptr<MeshBuffers>`. S7a then re-shapes `MeshBuffers` into `SdfGrid` + `HeightField` + proper domain types. **Mitigation**: The dispatch sequences T-S2-4a (introduce types) → T-S2-4b (migrate SHIELD) → T-S2-4c (migrate Render) → T-S2-4d (migrate Water) so the API surface is stable when S7a lands. S7a's new domain types compose with (don't replace) `MeshBuffers`.
- **S5.1 takes deps on mid-flight refactors of its own constituents:** `GameSession` (S3.2 in flight), `RenderPipeline` (S6 in flight). **Mitigation**: S5.1 must consume these through narrow interfaces (`IGameSession`, future render facade), not via direct calls into mid-flight implementations. This is an architectural rule for whoever picks up S5; no dispatch task yet because S5 itself is out of the must-fix block.

## Owner Decisions (Resolved by Review)

All five owner-gated questions were researched by an independent review agent against the actual code. Outcomes:

1. **SDF sign convention** — **Inside-negative** (confirmed, high confidence). `MarchingCubesTables.inl` is Paul Bourke's verbatim table authored for inside-negative; flipping the tables would have huge blast radius. Flipping three lines of density math (plus the GPU shader and `SHIELD_WorldSystem.cpp:354` surface-detection) achieves the convention switch.
2. **Dependency policy** — **Stay-vendored** (confirmed, high confidence). `vendor/` is 174 MB across 18 trees; `external/FastNoiseLite/`, `external/glm-1.0.1/`, and `vendor/test/` (31 MB) are dead duplicates and **will be deleted in S1.9**. vcpkg/CPM are explicitly **rejected** for now — the vendored copies were never being updated, so a migration would not solve the actual problem and would add CI bootstrap latency.
3. **`imgui.ini`** — **Accidental** (confirmed, high confidence). The file is 8 lines of default layout, touched by 6 unrelated gameplay commits per `git log`. S1.8 will `git rm --cached imgui.ini` and add it to `.gitignore`.
4. **`engine_version` source** — **Git short SHA + dirty flag** at build time via CMake `configure_file` (confirmed, high confidence). Implemented as a new dispatch task `T-OD4-embed-git-sha-engine-version` producing `cmake/engine_version.cmake` + `EngineVersion.h.in`.
5. **UI direction** — **Keep legacy `Rml_UIManager` behind `IUIManager`** (confirmed, high confidence). The component UI system has *never been in the active build* (sources.cmake confirms); `EnhancedUIManager` is a 27-line empty stub; the "Quantum" label is RCSS theming, not architecture. Implemented as a new dispatch task `T-OD5-delete-dead-ui-scaffolding` that removes `EnhancedUIManager`, `SimpleUIManager`, `UIIntegration.cpp`, the entire `ui/core/` and `ui/components/` trees, and updates `.forge/architecture.toml` to drop the orphaned check.

## Verification Commands (Post-Stage)

```powershell
cmake --preset debug-asan
cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure
.\scripts\lint.ps1
forge verify --validation-policy fail --testing-policy fail
forge run .forge\workflows\polyglot-audit-workflow.yaml
```

## Bottom Line

The revision keeps the original "stabilize-then-extract" framing but plugs the three load-bearing holes the critique surfaced: (1) GPU/CPU parity is verified before the SDF sign flips; (2) Stage 2 actually fixes the chunk-internal race rather than just the map-level race; (3) Stage 5 doesn't stall on the UI decision. Effort tags replace fantasy day estimates. Each stage has a clearly testable acceptance bar. The dispatch.json that follows operationalizes the must-fix block (S0, S1, S2, S3.1) into Forge tasks; the optional stages are left for follow-on dispatch generation after the must-fix block lands.

The post-review revision adds: (a) all 5 owner decisions are now resolved and translated into dispatch tasks (`T-OD4`, `T-OD5`, plus updates to existing S1.8/S1.9 prompts); (b) the H5/H6 hand-waves are replaced with concrete decisions (Forge cache via CI's `actions/cache`, vcpkg explicitly rejected); (c) three coupling risks the original critique missed are surfaced with mitigations; (d) four panel items that were silently dropped are reinstated as `T-DR-*` tasks; (e) four concrete dispatch bugs identified by the review (T-S0-1:354, T-S0-3 dual call sites, T-S0-6 GLFW link, T-S2-1 consumer set) are fixed in dispatch.json v2.

The dispatch is `[task_graph v2] valid — 35 tasks in 10 waves, zero warnings, exit 0`. **Safe to execute** after the user-interaction approval gate, subject to the must-fix scoping. The old `CORE-AUDIT-2026-06-07/dispatch.json` has been annotated to mark T-001/T-002/T-006 as superseded so the two dispatches do not collide on shared files if both are run.
