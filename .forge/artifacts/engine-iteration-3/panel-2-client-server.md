# Panel 2 — Client/Server Decoupling (Headless Simulation Authority)

Engine iteration 3 planning. Scope: make `luminumbra_server` tick a real world with zero GL/GLFW/audio dependencies, ahead of any networking transport work.

## Current State

- **`luminumbra_server` is a dead stub and is not even built.** `src/luminumbra_server/main_server.cpp` is 8 lines (Log::Init + banner, exit 0); `HostManager.{h,cpp}` is an empty ctor/dtor class. `src/luminumbra_server/CMakeLists.txt` exists and links `luminumbra_common`, but `src/CMakeLists.txt` never calls `add_subdirectory(luminumbra_server)` and the first-party target list at `CMakeLists.txt:143-153` does not include it. The stub has never compiled against the current tree.
- **`luminumbra_common` is already link-clean for headless use.** A full grep of `src/luminumbra_common` for `glad|GLFW|gl[A-Z]|GLuint|miniaudio|OpenGL` returns zero hits. `src/luminumbra_common/CMakeLists.txt` links only `entt, Jolt, nlohmann_json, sol2, lua, spdlog, lz4, glm, FastNoise`. Proof in practice: `world_generation_test` links only `gtest_main + luminumbra_common` (`test/CMakeLists.txt:11-14`) and runs headless today, exercising SHIELD_WorldSystem, MarchingCubes, and WaterSystem.
- **The GPU SDF path is a clean seam, set FROM the client.** `SHIELD_WorldSystem::SetGPUSDFCallback` is a plain `std::function` (`src/luminumbra_common/systems/SHIELD_WorldSystem.h:183,198`); the only setter is `RenderPipeline.cpp:376/387/394`. When unset, generation falls back to CPU SDF (`SHIELD_WorldSystem.cpp:1244`). A server simply never registers it.
- **GameSession owns the full world lifecycle** — create/load/save metadata, system construction (Physics → SHIELD → Water, linked at `GameSession.cpp:214-222`), plus the recently added runtime persistence `SaveWorldState/LoadWorldState` (`GameSession.h:69-79`) backed by `WorldSaveService` (whole-world JSON snapshot + `world_hash()` fnv1a_64, `src/luminumbra_common/persistence/WorldSaveService.h:49`).
- **Networking fixtures exist but are synthetic.** `NetworkLoopbackAuthority` and `NetworkStateHash` build deterministic fixture reports (not real-world simulation); `EntitySnapshot.h` is a fixture-driven snapshot schema. No transport library is vendored: `vendor/` contains no Steam SDK, GameNetworkingSockets, enet, or asio.

## Findings

### Client-side coupling that blocks a headless server
1. **`GameSession::ValidateWorldConfig` hard-requires CLIENT assets** — `src/luminumbra_common/world/GameSession.cpp:138-144` demands `res/shaders/basic.vert`, `res/shaders/g_buffer.frag`, `res/shaders/sdf_generation.compute`, `data/ui/main_menu.rml`, and `data/fonts/Lora/.../Lora-Regular.ttf`. It is called from both `CreateWorld` (:171) and `LoadWorld` (:274), so a headless server **cannot create or load a world** unless shaders/UI/fonts ship with it. This is the single functional client leak into common — an asset-manifest coupling, not a link coupling.
2. **Server target absent from the build graph** — `src/CMakeLists.txt:1-65` builds only common + client; `luminumbra_server` is unreferenced. It also needs `luminumbra_configure_first_party_target` registration (`CMakeLists.txt:143-153`) and a `copy_data` dependency for `worlds/atlas/presets`.
3. **World boot/enter lifecycle lives in `main_client.cpp`** and must be ported, not shared: `CreateWorld` → (client-only `SetupGPUSDFIntegration`, :1319) → `LoadWorldState` **before any generation** (:1322-1334, ordering contract documented at `GameSession.h:74-78`) → `EnsureSurfaceReadyNear(spawnPoint, physics, horizon_radius, collision_radius)` (:1340-1345) → readiness evaluation. Shutdown save hook: `SaveWorldState()` then `clear_world()` (`main_client.cpp:2325-2332`).
4. **Tick order lives client-side**: player controller (:1771) → `physics->update(deltaTime)` (:1773) → `world_system->update(registry, streaming_position, physics)` (:1779-1783). WaterSystem is ticked *inside* the SHIELD update, after chunk activation and before meshing dispatch (`SHIELD_WorldSystem.cpp:383-387`).

### Simulation-internal couplings to the renderer's point of view
5. **Streaming is camera-anchored, single-anchor.** `SHIELD_WorldSystem::update` takes one `camera_position` (`SHIELD_WorldSystem.h:156`); LOD selection, activation radius, and budgets all derive from it. `WaterSystem` resolves a camera via `set_camera_entity` / `ActiveCameraComponent`, **falling back to `Vec3(0)`** when absent (`WaterSystem.cpp:66-79`) — a headless server would silently simulate water detail around the origin.
6. **Collision does NOT need render meshes — but the state machine pretends it does.** Collision bodies are Jolt heightfields built from `chunk.heightmap_data` (`PhysicsSystem.cpp:153-177`), never from `mesh_vertices`. Yet collision creation is gated on `ChunkState::Ready`, which is only reached after CPU marching-cubes meshing (`SHIELD_WorldSystem.cpp:591-600`; `EnsureSurfaceReadyNear` polygonises synchronously at :893 before creating collision at :963-975). A naive headless tick pays full render-meshing CPU cost for meshes nobody will draw. Caveat: persisted saves treat "Ready with mesh" as renderable (`GameSession.cpp:465-469`), and `Chunk` carries render/water mesh buffers inline (`Chunk.h:52-60`), so skipping meshing interacts with persistence and chunk state transitions.
7. **Water sim LOD is camera-distance-driven** (`WaterSystem.cpp:86-98`); headless needs a deterministic policy (fixed resolution, or distance from the nearest player anchor).
8. **Audio in common is types-only** (`src/luminumbra_common/components/audio/AudioTypes.h` — plain handles/IDs). `MiniaudioManager` lives entirely in the client. No leak.

### Networking & persistence substrate
9. **Authority contract** (`NetworkLoopbackAuthority.h:9-70`): inputs carry `clientId/tick/sequence`; ordering contract is tick-then-sequence-then-client-id; report tracks rejection of unauthorized authority claims and client prediction reconciliation (error before/after in mm).
10. **State-hash contract** (`NetworkStateHash.cpp:55-106`): canonical per-tick string = sorted field names + durable entity ids (entity-id-ascending order contract from `EntitySnapshot.h:35-37`) + persistence world hash, fnv1a_64 per tick, chained final hash, deterministic double-replay check. **All inputs are fixtures today** (`NetworkStateHash.cpp:124-138`); nothing hashes a really-ticked world yet.
11. **`WorldSaveService::world_hash()`** (fnv1a_64 over the canonical snapshot JSON) is directly reusable as the server-tick artifact's world hash. Note the on-disk format is a single whole-world JSON snapshot (`WorldSaveService.h:19-25`) — fine for a smoke gate, a scaling concern for a long-running server.
12. **Game/engine split status**: no `data/scripts/` exists; Lua machinery (`LuaState`, `LuaApiManifest`) is in common but no game content drives it yet. World presets (game content) live at `worlds/atlas/presets/<type>.json` (`GameSession.cpp:43-45`). The required-asset list inside `ValidateWorldConfig` is exactly the kind of game/client content knowledge that should move out of the engine-generic session into a caller-supplied manifest.

## Headless Server Design

**Boot (mirrors `main_client.cpp:1315-1401`, minus GL/UI):**
1. `Log::Init`, parse args (`--world-id | --world-type --seed`, `--ticks`, `--tick-rate`, `--artifact-dir`, `--root`), `JobSystem::startup`.
2. `GameSession` + `SetJobSystem` + `SetRootPath`; `LoadWorld(worldId)` if resuming, else `CreateWorld(name, seed, worldType)` — after the validation split (Task 2) this requires only the world preset, not shaders/fonts.
3. `LoadWorldState()` immediately after system init, before any generation (ordering contract, `GameSession.h:74-78`).
4. Establish streaming anchor(s): initially the spawn point; later one per connected player. `EnsureCollisionReadyNear(anchor, physics)` (`SHIELD_WorldSystem.h:173`) for spawn readiness.
5. **Never call `SetGPUSDFCallback`** — CPU SDF fallback is the authoritative generation path (also sidesteps GPU/CPU parity divergence in the authority).

**Fixed tick (30 Hz — matches WaterSystem's documented game tick, `WaterSystem.h:41`):**
```
for each tick: physics->update(fixed_dt)
              world->update(registry, anchor, physics)   // water ticks inside
              (periodic) SaveWorldState() autosave
```
Catch-up/accumulator loop; no vsync, no frame pacing from GLFW — `std::chrono` based.

**What a headless tick needs vs skips:**
- NEEDS: chunk generation (CPU SDF + heightmap), **collision** (heightfield from `heightmap_data` — already mesh-independent at the Jolt level), water simulation, physics step.
- SKIPS: render marching-cubes meshing, water render meshing, LOD transition skirts, GPU upload. Mechanism: add a `StreamingProfile { bool build_render_meshes; bool build_water_meshes; }` to `SHIELD_WorldSystem`, and re-gate collision creation on "generated + heightmap present" instead of `ChunkState::Ready` (`SHIELD_WorldSystem.cpp:591-600`). Recommended phasing: **Phase A ships with render meshing left ON** (correct, just wasteful — identical state machine, zero risk to the intricate LOD/seam logic) and Phase B introduces the profile behind the server flag with a chunk-state parity test.
- EMITS: per-tick `RuntimeChunkStats` (`SHIELD_WorldSystem.h:98-115` already exposes `collision_chunks`, queue depths), streaming telemetry (`StreamingTelemetryStats`), tick duration stats, and a final world hash via `WorldSaveService::world_hash(snapshot)`.

**Shutdown:** `SaveWorldState()` → `clear_world(physics)` → `gameSession.reset()` → `jobSystem.shutdown()` (same milestone order as `main_client.cpp:2317-2364`).

## Server Tick Gate Design

**Gate: `headless_server_smoke`** — modeled on the existing scenario-artifact pattern (`luminumbra.runtime_state.v1`, `luminumbra.persistence_runtime_roundtrip_phase.v1`).

- **Run mode:** `luminumbra_server --scenario server_tick_smoke --world-type default --seed 12345 --ticks 300 --tick-rate 30 --artifact-dir <dir>`. Boots a fresh world, ticks N fixed steps, saves, exits 0/1.
- **Artifact `luminumbra.server_tick.v1`** (JSON, written even on failure with `passed:false`):
  - `schema`, `build_preset`, `seed`, `world_type`, `tick_rate_hz`, `ticks_requested`, `ticks_completed`
  - `world_hash` (WorldSaveService canonical hash after final tick), `world_hash_replay` (second in-process run, same seed) and `deterministic: world_hash == world_hash_replay`
  - `chunk_stats` (final `RuntimeChunkStats`: total/ready/collision chunks; assert `collision_chunks > 0`)
  - `tick_ms` {mean, p95, max}, `streaming` (peak queue depth, scheduled/deferred counts)
  - `checks[]` in the existing `{name, passed}` style.
- **Validator mode:** new `Test-HeadlessServerTickGate` in `.forge/scripts/validate-engine-frontier.ps1` (same shape as `Test-NetworkStateHash` at :2273 and `Test-PersistenceRuntimeRoundtrip` at :1718): runs the binary, parses the artifact, asserts schema/determinism/collision-chunk/tick-budget checks, and **statically asserts target hygiene** — `luminumbra_server` links only `luminumbra_common` (parse `src/luminumbra_server/CMakeLists.txt`) and `src/luminumbra_server/**` contains no `glfw|glad|miniaudio|imgui|rmlui` includes.
- Headless by construction: no window, no GL context — runs on any CI agent.

## Transport Readiness

**What the existing contracts already pin down for a real transport:**
- Input frames must carry `(clientId, tick, sequence)` and the server applies them in `tick_then_sequence_then_client_id` order — so the transport needs per-client **ordered, reliable input delivery** (or sequence-gap rejection, which the loopback authority already models via rejected frames).
- Per-tick canonical state hashing (sorted fields + durable entity ids + world hash) gives a transport-independent **desync detector**: client and server exchange 16-hex-char hashes, not state diffs, to validate any future transport. This is the cheapest cross-machine correctness oracle and should be the first thing sent over a real socket.
- Reconciliation is server-authoritative snapshot + client replay (prediction error before/after in mm) — implies an unreliable-ok **snapshot channel** separate from the reliable input channel.

**Recommendation: plain sockets first, Steam later.** No Steam SDK or GameNetworkingSockets is vendored; the project vendors flat source libs, and GameNetworkingSockets drags protobuf/openssl-class dependencies that don't fit that model, while the closed Steamworks SDK adds account/licensing constraints irrelevant to a smoke-tested authority. Define a minimal `ITransport` (connect/poll/send on two channels) in common, implement (1) **in-process loopback** (replaces the synthetic fixture with the really-ticked world) and later (2) UDP datagram + tiny reliability layer for inputs. Steam Datagram Relay/GameNetworkingSockets becomes a third implementation behind the same interface if/when distribution needs it.

**What NOT to build yet:** client-side prediction/interpolation against the real world, interest management/area-of-interest filtering, encryption/auth, lobby/matchmaking, delta-compressed snapshots, and any entity replication beyond the durable-id hash exchange — none of it is needed until the headless authority ticks deterministically.

## Task Breakdown

| # | Task | Files | Depends on | Gate |
|---|------|-------|-----------|------|
| T1 | Wire `luminumbra_server` into the build: `add_subdirectory`, first-party warnings config, `copy_data` dependency, prune dead HostManager or keep as shell | `src/CMakeLists.txt`, `CMakeLists.txt:143-153`, `src/luminumbra_server/CMakeLists.txt` | — | server target builds in CI; links only `luminumbra_common`; runs and exits 0 |
| T2 | Split `ValidateWorldConfig` asset manifest: simulation requirements (preset only) in common; client appends shaders/UI/fonts via a caller-supplied required-assets list | `GameSession.{h,cpp}:123-163`, `main_client.cpp` call sites | — | new headless unit test: `CreateWorld` succeeds in a temp root containing only `worlds/atlas/presets/default.json`; existing client gates stay green |
| T3 | `ServerWorldRunner`: boot (create/load + `LoadWorldState`), fixed 30 Hz tick (physics → world update), spawn-anchor streaming, autosave + shutdown `SaveWorldState` | `src/luminumbra_server/main_server.cpp`, new `ServerWorldRunner.{h,cpp}` | T1, T2 | runs `--ticks 300` headless; `collision_chunks > 0`; world save written |
| T4 | `luminumbra.server_tick.v1` artifact: tick stats, chunk stats, `WorldSaveService::world_hash`, in-process determinism double-run | new `src/luminumbra_server/ServerTickRecorder.{h,cpp}` | T3 | artifact validates; `deterministic == true` |
| T5 | `Test-HeadlessServerTickGate` validator + CI hookup (incl. static no-GL/GLFW/audio include check on `luminumbra_server/**`) | `.forge/scripts/validate-engine-frontier.ps1`, CI config | T4 | gate red/green demonstrated both ways |
| T6 | `StreamingProfile`: skip render/water meshing server-side; re-gate collision on generated heightmap instead of `ChunkState::Ready`; deterministic water-detail policy without camera | `SHIELD_WorldSystem.{h,cpp}` (:591-600, :883-901), `WaterSystem.{h,cpp}` (:66-98) | T3 (ships after the gate is green with meshing ON) | parity test: collision chunk set identical with/without meshing; server tick p95 drops; client behavior unchanged |
| T7 | Multi-anchor streaming API (`update(registry, span<Vec3> anchors, physics)`) — pre-networking, also useful for split-screen/spectator | `SHIELD_WorldSystem.{h,cpp}`, call sites | T6 | unit test: chunks resident around two distant anchors |
| T8 | In-process loopback transport: feed `NetworkLoopbackInput` frames into the real `ServerWorldRunner` tick and hash the real world per tick (replaces synthetic fixture), behind a minimal `ITransport` | `src/luminumbra_common/network/`, server runner | T4 | extended `network_state_hash` gate over real ticks; deterministic replay holds |

## Risks

1. **Determinism is unproven for a real ticked world.** JobSystem thread scheduling, FastNoise SIMD codepaths (machine-dependent ISA dispatch), float accumulation, and Jolt's default non-deterministic mode can all break `world_hash` replay equality. Mitigation: T4's double-run runs in the same process/machine (catches order bugs, not cross-machine drift); treat cross-machine determinism as a separate, later gate.
2. **GPU vs CPU SDF divergence:** clients with the GPU callback registered generate different SDF bits than the CPU-authoritative server (the existing `sdf_gpu_cpu_parity_test` bounds but does not eliminate this). Authority hashing must come from CPU-generated or server-streamed chunk data, never client-local GPU generation.
3. **`ValidateWorldConfig` refactor (T2) touches a validation contract** used by client gates and the persistence roundtrip scenarios; regression risk if the client's asset list drifts from what the renderer actually loads.
4. **Meshing-optional refactor (T6) destabilizes a very intricate state machine** (LOD seams, transition skirts, pending-mesh persistence semantics in `GameSession.cpp:449-469`). Hence the deliberate phasing: gate first with meshing ON, optimize second.
5. **Whole-world JSON snapshot saves** (`WorldSaveService.h:22-25`) will not scale for a long-running server with autosave; `wait_for_streaming_jobs()` (`SHIELD_WorldSystem.h:188`) also stalls the tick during save. Acceptable for the smoke gate; needs per-chunk incremental I/O before real hosting.
6. **WaterSystem origin fallback** (`WaterSystem.cpp:78`) silently concentrates simulation detail at `Vec3(0)` headless — must be fixed in T6 or water behavior differs client vs server.
7. **No durable entity id system exists** in the live registry (durable ids exist only in the snapshot fixture); the real-world state hash (T8) needs one, which is its own design task.
8. **Single streaming anchor** caps the server at one player region until T7; networking work that assumes multiple players will block on it.
