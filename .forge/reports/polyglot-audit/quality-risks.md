# Luminumbra Quality & Risk Audit

Generated: 2026-06-07

## Scope

Defects, undefined-behavior risks, memory/ownership risks, concurrency hazards, error-handling, logging, test gaps, and build fragility. Findings ranked `severity {C0 critical, C1 high, C2 medium, C3 low} × confidence {high|med|low}`. Confidence reflects how directly the symptom is observable in repo code or test output, not the magnitude of impact.

Forge baseline: status `spec`, no active dispatch, 3 ship-blocker test failures, 36 polyglot risk markers (20 TODO, 8 `std::cout`, 6 raw new/delete, 2 `std::runtime_error`), single GoogleTest binary, no CI workflow committed, no static-analysis or formatter config.

## Executive Summary

The largest quality risk is **concurrency around shared chunk state**: `JobSystem` recycles 256 atomic-int counters in a ring buffer, `m_chunks` is an unsynchronized `unordered_map` read by render/physics threads while being mutated by main-thread streaming, and `dispatch_generation_jobs` blocks on `wait()` from inside the per-frame update path. The second-largest risk is **a known-failing SHIELD test contract** that has not been resolved before any architectural refactor — sign, normals, and cave-effect tests all fail, so any change to terrain code can't be validated. Logging is split between `LUMINUMBRA_CORE_*` macros and ad-hoc `std::cout`/`std::cerr` in the world session, which means real failures during world creation/load are invisible to the in-game console and to any future log capture. Build hygiene is brittle: `sources.cmake` is hand-maintained and already drifting from the file tree, `-O3 -DNDEBUG`/`-g -Wall` flags are wired in a way that silently does nothing on the MSVC default generator, and there is no warnings-as-errors gate. Test coverage is shallow — one binary, one fixture file, no tests for Physics, JobSystem, Water, GameSession, audio, or UI.

**This report has been revised after an independent review pass.** Several severity levels were adjusted, one cited line was corrected, and three additional findings were added. The severity roll-up at the bottom of the document reflects the post-review state.

## Findings (Ranked)

### C0 — SHIELD test contract is failing on `main` (confidence: high)

Three GoogleTest cases under `test/shield/test_world_generation.cpp` fail per the baseline spec:

- `SurfaceIsGeneratedAtCorrectHeight` expects `inside_density < 0` and `outside_density > 0`, but `get_density_at_from_precalculated()` returns `terrain_height - world_pos.y`, so points **below** the surface yield **positive** values (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:279`).
- `NormalsPointUpwardsOnHorizontalSurface` expects `+Y` normals. With the sign convention above, the marching-cubes inside-set condition `val < isolevel` (`world/MarchingCubes.cpp:128`) labels air as "inside," inverting the gradient and the resulting normals.
- `CavesChangeGeneratedMesh` expects different vertex counts with/without caves. With sign flipped, the carve term `std::min(terrain_density, cave_density)` still moves values toward the lower envelope but no longer crosses the isosurface where the test expects it, so the cave path is a no-op on the chunk under test.

These three failures are not independent — they all unwind from one decision about SDF sign. **Fix-or-redefine first; everything downstream (rendering, collision normals, marching-cubes step semantics, GPU SDF path) inherits whatever choice is made.** Until then, any refactor of terrain code is unverified.

### C0 — `JobSystem` counter ring is unsafe under load (confidence: high)

`src/luminumbra_common/core/JobSystem.h:39` declares `std::array<std::atomic<int>, 256> m_counters` and `JobSystem::dispatch_batch` chooses a slot via `m_next_counter_index.fetch_add(1) % m_counters.size()` (`JobSystem.cpp:41`). The selected slot is then *unconditionally overwritten* with `counter_ptr->store((int)jobs.size())`. Failure modes:

- A 257th in-flight batch overwrites batch 1's counter mid-execution. Any thread still holding the old `JobHandle` to batch 1 will see `wait()` return early (counter set to new batch's size) or never return (counter set to zero by old jobs finishing after new jobs are queued).
- `dispatch_meshing_jobs` discards the returned `JobHandle` (`SHIELD_WorldSystem.cpp:553-555`), so meshing batches cannot be waited on at shutdown. Worker threads are joined while jobs may still be in `m_job_queue` referencing `Chunk` shared_ptrs that have already been erased from `m_chunks`.
- `JobSystem::shutdown` sets `m_stop_threads` and joins, but the queue is not drained — jobs queued after `m_stop_threads.store(true)` and before the workers see it will sit in `m_job_queue` until destruction. Lambdas capture `shared_ptr<Chunk>` by value, so the chunks survive; the work just never runs.

This is the highest-impact concurrency issue in the codebase.

### C0 — `m_chunks` is mutated and read without synchronization (confidence: high)

`SHIELD_WorldSystem::m_chunks` is `std::unordered_map<ChunkID, shared_ptr<Chunk>>`. It is:

- written from `update_chunk_activation()` on the main thread (`SHIELD_WorldSystem.cpp:199`, `267`),
- iterated by `get_renderable_chunks()` from the render thread (`SHIELD_WorldSystem.cpp:305`),
- iterated by the water system from the simulation thread (`WaterSystem.cpp:41`),
- iterated again by `update()` itself to schedule meshing while it could still be holding state from the same tick.

`std::unordered_map` is not thread-safe; concurrent insert/erase invalidates iterators and (in many STL implementations) rebuilds buckets. The render thread receiving `Chunk*` raw pointers (`get_renderable_chunks()` returns `std::vector<Chunk*>`) can also outlive the `shared_ptr` if a chunk is unloaded the same frame.

### C1 — World generation blocks the update loop (confidence: high)

`SHIELD_WorldSystem::dispatch_generation_jobs()` immediately calls `m_job_system->wait(handle)` (`SHIELD_WorldSystem.cpp:476`). It is invoked from `update_chunk_activation()` which is invoked from `update()` — but `update()` only runs the expensive activation/wait branch once every 10 frames (`m_update_tick_counter >= 10` at `SHIELD_WorldSystem.cpp:94`). The advertised "background streaming" architecture is therefore synchronous batches stalling the main thread, plus a post-wait debug scan that re-walks every SDF. Frame stalls of multiple hundreds of ms for the initial load list are expected. Downgraded from C0 to C1: the throttle limits how often the stall fires, but every time it does fire it blocks the main thread.

### C1 — SDF sign + Marching Cubes inside-test must agree (confidence: high)

Even if you keep `terrain_density = terrain_height - y` (the current convention), `MarchingCubes::PolygoniseTerrain` sets `cube_index |= (1<<i)` when `val < isolevel` (`MarchingCubes.cpp:128`). With `isolevel = 0` and inside-positive, this code marks **air** corners as "inside" → all cell indices come out as the complement of what the standard MC tables expect → outward normals point inward. Pick one convention and propagate it: either flip the sign in `get_density_at_from_precalculated()` so inside is negative (matches tests, matches MC defaults), or flip the comparison in MC and the test expectations to match inside-positive. The former is the smaller and more conventional change.

### C1 — `Chunk` exposes mutable simulation/render buffers as public members (confidence: med)

`Chunk` (`world/Chunk.h:38-57`) exposes `sdf_data`, `heightmap_data`, `mesh_vertices`, `mesh_indices`, `water_mesh_vertices`, `water_mesh_indices`, `water_level_data`, `water_flow_data`, and `water_sim_terrain_height` as public `std::vector<...>`. `mesh_version` is atomic, but the buffers it claims to version are not protected by any ownership rule:

- Meshing job writes `mesh_vertices`/`mesh_indices` (`SHIELD_WorldSystem.cpp:515`).
- Renderer reads them on the GL thread (`RenderPipeline::geometry_pass_chunks`).
- Physics reads `mesh_vertices`/`mesh_indices` to build the Jolt mesh shape (`PhysicsSystem::add_chunk_collision`, called from main thread).
- The water sim writes `water_level_data` from its own jobs and a separate water mesh generator reads it.

**Mitigating factor**: `Chunk::get_state()`/`set_state()` ARE mutex-protected (`Chunk.h:28-36`), and every consumer gates on `ChunkState::Ready` before touching the buffers. So the system relies on a state-mutex barrier rather than no synchronization at all — the chunk-internal data race is real but conditioned on a "consumers always re-check state" invariant that's easy to violate during refactors. There is no double-buffer, no snapshot, no read/write fence. Visible-corruption risk if a future change drops a state check. Confidence dropped from high to med because the state-mutex partly protects today.

### C1 — Public headers leak vendor types and force common to advertise vendor link deps (confidence: high)

`luminumbra_common/CMakeLists.txt` declares EnTT, Jolt, sol2, lua, spdlog, lz4, glm, FastNoise, and nlohmann_json as **`PUBLIC`** link libraries. Public headers (`Types.h`, `PhysicsSystem.h`, `Chunk.h` via Log) include EnTT, Jolt, GLM, and FastNoise. Any future consumer of `luminumbra_common` (server, tools, tests) inherits the full vendor surface, so even a CLI debug tool linking common pulls Jolt's compile-heavy headers and its global factory state.

### C1 — `GameSession` save/load asymmetry, no schema, no `worldId` persisted (confidence: high)

(Already detailed in the data-models panel; restated here because it is a quality-correctness issue, not only modeling.) `SaveWorld()` writes `name/seed/worldType/creationTime/spawnPoint`. `LoadWorld()` reads only `name/seed/worldType/creationTime` — **`spawnPoint` is silently dropped on load** (`GameSession.cpp:151-159`). `worldId` is computed from the directory name on create, but never written back, so any rename invalidates references and load can't verify the directory actually corresponds to the manifest. There is no `schema_version`, so a future format change can't be detected.

### C1 — Asset processing pipeline ignores generated outputs in source-of-truth dir (confidence: high)

Top-level `CMakeLists.txt:30-57` globs `assets/*.glb`, processes each into `data/**/*.lmesh` *inside the source tree*, and declares `process_assets` an `ALL` target. Two consequences:

- `data/` mixes source-controlled JSON/UI assets and tool-generated `.lmesh` binaries. There is no `.gitignore` rule on generated outputs visible in the repo skeleton; the engine ships with `.lmesh` checked in next to the JSON authoring data.
- `file(COPY data DESTINATION ${CMAKE_BINARY_DIR})` runs only at CMake configure time (`CMakeLists.txt:72`). Changes to JSON, UI, audio banks, or worlds during a build session are not reflected in the runtime data directory until the user reconfigures CMake. This is a frequent "why didn't my change show up" trap.

### C2 — `RenderPipeline::get_light_space_matrices` throws `std::runtime_error` from a render-loop ordering guard (confidence: high)

`RenderPipeline.cpp:1500` throws `std::runtime_error("Shadow map cascade splits not initialized!")` from `get_light_space_matrices()` — a defensive guard against calling shadow-matrix calculation before shadow init has populated cascade splits. **Earlier draft of this report misattributed the location to `RenderPipeline::startup`; the actual call site is the render-loop helper.** There is still no top-level `try/catch` in `main_client::main()` covering the render loop, so if the guard ever fires it aborts the client without unwinding GLFW or the JobSystem cleanly (worker threads will be detached on `std::terminate`). Downgraded from C1 to C2: the guard isn't on a hot startup path; it only fires under an internal-invariant violation that current code paths don't exercise. Fix: replace with a logged fatal + return-error, or catch in `main` and shut down systems in reverse order.

### C2 — `WaterSystem` recovers camera position from "first entity with TransformComponent" (confidence: high)

`WaterSystem::update()` picks `transform_view.front()` as the camera position (`WaterSystem.cpp:36-37`) with a TODO admitting the design is wrong. In any registry with multiple transforms (NPCs, scenery, lights), this picks an arbitrary entity. Water LOD will pulse with whichever entity happens to be first in the registry. Track this as a feature gap, not a fast fix — the right answer is the AppState/`PlayerHandle` model proposed by the data-models panel.

### C2 — Inconsistent logging: `LUMINUMBRA_CORE_*` macros vs raw `std::cout`/`std::cerr` (confidence: high)

`GameSession.cpp` uses `std::cerr` for at least 8 distinct error sites (file open failures, JSON parse failures, missing world IDs) and `std::cout` for "World loaded successfully" (`GameSession.cpp:200`). `AssetManager.h:17` uses `std::cout` for texture failures. `main_server.cpp:4` uses `std::cout` as the only output. The engine has a working `Log::Init()` / `LUMINUMBRA_CORE_*` macro family. The raw streams skip the in-game console hook and won't be visible if `stdout` is redirected (Win32 `WIN32` subsystem builds detach stdout by default; `main_client.cpp:2` builds with `WIN32`, so on Windows release these messages are dropped on the floor).

### C2 — Raw `new`/`delete` for RmlUi listeners and Jolt factory (confidence: high)

- `Rml_UIManager.cpp:272,276`: `element->AddEventListener("click", new LambdaEventListener(...))`. RmlUi owns the listener and deletes it when the element is destroyed, so this is not a leak per se, but every fresh subscription path needs a code review to confirm RmlUi will free it; the present pattern is fragile if a future call path detaches the listener without notifying RmlUi.
- `Rml_Interfaces.cpp:124/171`: `new CompiledGeometry()` / `delete geometry`. RAII would eliminate the leak surface entirely.
- `PhysicsSystem.cpp:97`: `Factory::sInstance = new Factory();` paired with `delete Factory::sInstance;` in `shutdown()` (`:121`). If `shutdown()` is not called (uncaught exception during world init, see C1 above), the factory leaks and registers types that the next instance attempts to re-register.

### C2 — `dispatch_generation_jobs` mutates `m_chunks` from the calling thread, then issues jobs that write the chunk in parallel (confidence: medium)

The flow at `SHIELD_WorldSystem.cpp:447-501`:
1. Main thread iterates `chunks_to_generate`, calls `m_chunks[id] = chunk` immediately.
2. Sets `chunk->set_state(ChunkState::Loading)`.
3. Dispatches a job that calls `GenerateChunkData(*chunk)`.
4. Calls `wait(handle)` immediately.
5. After wait, walks `chunks_to_generate` again and re-reads `chunk->sdf_data` from the main thread.

Step 5 is fine only because of step 4. Without step 4, the post-walk would race the jobs. But the data flow also means `m_chunks` gets a `chunk` with empty `sdf_data` published to the map *before* the job has populated it — `get_renderable_chunks()` (render thread) can grab the entry between steps 1 and 4 and dereference the empty mesh (which currently is gated by `ChunkState::Ready`, so it skips, but the protocol is fragile and depends on every consumer always re-checking state).

### C2 — `update()` performs unbounded debug logging in formerly-hot paths (confidence: high)

`SHIELD_WorldSystem::update()` and its callees have comments like "Chunk state logging removed from hot path - too expensive" (`:109`), "SDF analysis logging removed from hot path" (`:496`), "Mesh generation complete" (`MarchingCubes.cpp:197`) — these aren't actually free. The conditional fmt-formatting macros still incur the argument capture and lookup work on every call. `SHIELD_WorldSystem::GetInitialChunkLoadList()` issues `LUMINUMBRA_CORE_WARN("Loading {} chunks ...")` every world load. Wire `spdlog` levels to compile-time filter for hot paths, or move chatty diagnostics behind a `LUMINUMBRA_DEBUG`-only macro.

### C3 — Marching cubes uses an `unordered_map<u64, u32>` per-chunk vertex cache (confidence: medium)

`MarchingCubes.cpp:92` creates a fresh `std::unordered_map<u64, u32>` per `PolygoniseTerrain` call. For a fully meshed chunk this is hundreds-of-thousands of hash inserts on the hot mesher loop. A direct-indexed array (3 edges × chunk dimensions, sized at compile time) would replace this with O(1) lookups and no allocator pressure. Risk is performance and meshing latency, not correctness — but meshing latency directly drives chunk-pop visibility. Downgraded from C2 to C3: no measurements show meshing as the dominant frame cost, and the optimization is independent of every other roadmap item.

### C2 — `RenderPipeline::SetupGPUSDFIntegration` is a runtime override of CPU generation that bypasses the failing tests (confidence: high)

`SHIELD_WorldSystem::GenerateChunkData()` (`:340`) tries the GPU SDF callback first and returns early with a heightmap reconstructed from the GPU buffer. None of the GoogleTest fixtures exercise this path. If the runtime uses GPU SDF in the main client and CPU SDF in tests, runtime and tests have different sign/normal behavior. This is the most expensive divergence to debug: green tests can hide a broken runtime.

### C2 — `LUMINUMBRA_ASSERT` collapses to `assert()` (assumed) and is compiled out in Release (confidence: low — needs `Debug.h` confirmation)

`main_client.cpp:90, 96` use `LUMINUMBRA_ASSERT(window, ...)`. If `LUMINUMBRA_ASSERT` is the standard "do nothing in Release" pattern, a failed GLFW or GLAD init silently dereferences a null pointer. Read `core/Debug.h` and either route asserts to a logged fatal + exit, or pair them with explicit `if (!window) return -1` checks for non-recoverable startup conditions.

### C3 — Build flags do not apply to the default MSVC generator on Windows (confidence: high)

`CMakeLists.txt:74-85` sets `-O3 -DNDEBUG` and `-g -Wall`. On the default MSVC generator these are GCC-syntax flags that MSVC ignores; the project relies on CMake's default Release config to set `/O2 /DNDEBUG`. On a GCC/Clang MinGW build they apply. The result: no warnings on the canonical Windows build, no warnings-as-errors anywhere, and `LUMINUMBRA_DEBUG` is only set when `CMAKE_BUILD_TYPE=Debug` is explicitly given (it is not by default in multi-config generators).

### C3 — `add_compile_options(-fsanitize=address)` is commented out in Debug (confidence: high)

`CMakeLists.txt:82-84` leaves ASan disabled. Combined with the raw `new`/`delete` sites and the chunk-buffer ownership issues, the project is forfeiting the cheapest UB-catch surface. Enable in a dedicated `Debug+ASan` CMake preset, not in default Debug, so a single `cmake --preset debug-asan && ctest` flow exists.

### C2 — `process_assets` target is empty (no `.glb` files in `assets/`) (confidence: high)

`CMakeLists.txt:30` uses `file(GLOB_RECURSE SOURCE_ASSETS "${ASSET_SOURCE_DIR}/*.glb")`. **Verified:** `assets/models/{creatures,environment,shield_tiles}/` are empty — zero `.glb` files anywhere under `assets/`. `SOURCE_ASSETS` is empty, `process_assets` is a no-op `ALL` target, and the `add_dependencies(luminumbra_client_app process_assets)` line is silently meaningless. `file(GLOB ...)` is also not tracked by CMake's dependency scanner — adding a new asset doesn't trigger a reconfigure, so the new file is invisible until the user re-runs CMake. Upgraded from C3 to C2: this is actively misleading build hygiene — the build pretends to have an asset pipeline that does nothing today.

### C3 — Test suite covers one module, asserts wall-clock perf bounds (confidence: high)

`test/CMakeLists.txt:2-13` registers exactly one binary: `world_generation_test`. There are no tests for PhysicsSystem, JobSystem, WaterSystem, GameSession, MarchingCubes water path, Chunk lifecycle, audio, UI, or render abstractions. The existing perf tests (`Performance_GenerateChunkData`, `Performance_PolygoniseTerrain`) use `EXPECT_LT(avg_time_ms, 50.0)` and `EXPECT_LT(avg_time_ms, 30.0)` — these will flake on slow CI hardware and on debug builds. Move them behind a `LUMINUMBRA_RUN_PERF_TESTS` switch or convert to non-asserting microbenchmarks.

### C3 — `sources.cmake` drifts from the file tree (confidence: high)

`src/luminumbra_client/sources.cmake` omits files that exist in the tree:
- `ui/UIIntegration.cpp` (defines `EnhancedUIManager` integration, holds the only `throw std::runtime_error` in UI)
- `ui/core/UIComponent.cpp`, `ui/core/UIManager.cpp`, `ui/core/UIStateManager.cpp` (the new component UI system)
- `ui/components/common/Button.cpp`, `Input.cpp`, `ui/components/common/Panel.cpp`, `ui/components/game/WorldList.cpp`, plus `ui/core/UIHotReload.cpp`, `ui/core/UIDataStream.cpp` (Quantum UI components — broader than the original undercount)
- `audio/AudioPropagationSystem.cpp`, `audio/EnvironmentalAudioSystem.cpp`

Most of these were the subject of TODO entries the polyglot risk scanner flagged. The drift means dead-or-active classification is implicit. Either delete the inactive files (the owner-decision recommendation is to delete all of `ui/core/` and `ui/components/`) or wire them in with their own tests.

**Note on `src/luminumbra/core/ExternalImplementations.cpp`**: it is also omitted from `sources.cmake` but the omission is **intentional** — the file defines `MINIAUDIO_IMPLEMENTATION` and `FNL_IMPL` which can only be defined in a single translation unit. The earlier draft of this report framed this as "drift"; the correct framing is "intentionally inactive, must remain inactive if reintroduced elsewhere." Whether it's compiled by any current target is a separate audit question.

### C3 — `LUMINUMBRA_DEBUG` is the only feature flag (confidence: high)

There is no separation between "GL debug callbacks," "verbose logging," "in-game profiler," and "developer console" — `LUMINUMBRA_DEBUG` gates everything together. This blocks shipping a release build with logging but without `glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS)` (which costs measurable frame time).

### C3 — `GameSession.h:27` contains a dead nested namespace declaration (confidence: high)

`GameSession.h:27` declares `namespace Luminumbra::Systems { ... }` *inside* `namespace Luminumbra::world`, creating the dead nested namespace `Luminumbra::world::Luminumbra::Systems`. The forward declaration is intended to introduce `Luminumbra::Systems::PhysicsSystem` and friends but, due to the surrounding `namespace world` scope, declares nothing usable. The code compiles only because `GameSession.cpp` includes the concrete `Systems` headers directly. Confusing dead code that misleads future readers.

### C3 — `luminumbra_common` exposes the entire `vendor/` tree as PUBLIC include directories (confidence: high)

`src/luminumbra_common/CMakeLists.txt:9-15` exposes `${CMAKE_SOURCE_DIR}/vendor/spdlog/include`, `vendor/soil2/src`, `external/FastNoiseLite`, **and `${CMAKE_SOURCE_DIR}/vendor` itself** as `target_include_directories(luminumbra_common PUBLIC ...)`. Combined with the wide `PUBLIC` link surface (EnTT/Jolt/sol2/lua/spdlog/lz4/glm/FastNoise), every consumer of `luminumbra_common` — the client, the server, the tests, future tools — inherits the *full vendor tree* in its include path. This is broader than the architecture panel's "many PUBLIC links" framing and is the root cause of the long header dependency surface across the project.

### C3 — `SHIELD_WorldSystem::update` throttle uses a raw frame-count constant (confidence: high)

`SHIELD_WorldSystem::update()` at `SHIELD_WorldSystem.cpp:91-97` runs the expensive chunk activation branch only when `m_update_tick_counter >= 10` — a magic-number framerate-dependent throttle. At 60 FPS the activation runs ~6x/sec; at 144 FPS, ~14x/sec; at 30 FPS, 3x/sec. The documented design contract is a fixed-rate (30 Hz) simulation but this counter is in frame ticks, not simulation ticks. Either move to a wall-clock or fixed-tick interval, or name the constant and document the tradeoff.

## Cross-Cutting Concerns

- **Determinism.** `GenerationIsDeterministicWithSameSeed` passes (good). But `WorldSystem` stores the noise generators as `SmartNode<Generator>` rebuilt from seed; the GPU SDF callback is *not* seed-verified against the CPU path. A determinism property test that pins GPU output to CPU output at every committed seed would be cheap.

- **No fuzz / property tests anywhere.** Marching cubes, water flow, and the SDF gradient are all naturally fuzzable. Even a single rapidcheck-style property — *"meshing a chunk with random SDF in [-1,1] never crashes and produces winding-consistent triangles"* — would catch a category of edge-case bugs the current asserts can't.

- **No memory tooling in the loop.** ASan, Valgrind, Dr. Memory: all off. The leak-prone sites listed above don't get caught.

- **Logging side-effects in const methods.** `GetInitialChunkLoadList()` is `const` but calls `LUMINUMBRA_CORE_WARN`. Not a bug, but worth noting that this prevents future `-fno-rtti`/`constexpr` exploration on the world system.

## Severity Roll-Up (after post-review recalibration)

| Severity | Count |
|----------|-------|
| C0       | 3     |
| C1       | 6     |
| C2       | 8     |
| C3       | 10    |

**Recalibrations applied:**
- "World gen blocks update loop" C0 → C1 (throttle limits how often the stall fires).
- "RenderPipeline throws" C1 → C2 + location corrected (it's in `get_light_space_matrices`, not `startup`).
- "Marching cubes unordered_map" C2 → C3 (not a measured hot path).
- "process_assets target empty" C3 → C2 (actively misleading).
- "Chunk exposes mutable buffers" confidence high → med (state-mutex partly protects today).
- "sources.cmake drifts" framing fixed: `ExternalImplementations.cpp` exclusion is intentional, not drift.
- Added: `GameSession.h:27` dead namespace, common PUBLIC vendor include surface, frame-count tick throttle.

The C0 block (SHIELD test contract, JobSystem counter ring, `m_chunks` unsync) must be addressed before any architectural refactor. The C1/C2 blocks are the work backlog the synthesis roadmap will sequence. C3 items are hygiene gates that should land alongside CI work, not in their own commits.

## Recommended Verification Commands

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build --output-on-failure
forge structure
forge index
forge verify --validation-policy warn --testing-policy warn
```

Add (not yet present):

```powershell
cmake --preset debug-asan
ctest --preset debug-asan --output-on-failure
clang-tidy --warnings-as-errors=* -p build src/luminumbra_common
```

## Bottom Line

Three SHIELD tests are red on `main`, and the engine ships in spite of the failure because no CI gate exists to enforce green. Behind those failures sits a job system that can corrupt batch-completion signaling under load, a chunk store that the renderer reads while the streamer mutates it, and a save/load path that silently loses spawn-point and worldId fields. The fix order is: lock SHIELD contracts → harden JobSystem + chunk ownership → fix save/load round trip → wire CI + ASan + warnings-as-errors → split logging/feature flags. Everything in the C2/C3 list is much cheaper to do once the C0 block is gone.
