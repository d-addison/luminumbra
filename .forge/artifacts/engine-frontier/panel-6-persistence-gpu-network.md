## Subsystem State

- Persistence is metadata-only today. `GameSession::SaveWorld` writes `world_info.json` (`src/luminumbra_common/world/GameSession.cpp:303-328`), while `LoadWorld` rebuilds systems from metadata and preset only (`src/luminumbra_common/world/GameSession.cpp:243-300`). Chunk payloads, mutated terrain, water sim state, and ECS state are not saved.
- `Chunk` is the natural terrain persistence unit: it owns SDF, heightmap, mesh payloads, water sim arrays, LOD flags, and water activity state (`src/luminumbra_common/world/Chunk.h:38-76`). Mesh/collision should remain derived; SDF/heightmap/water and explicit edits are the initial durable payload.
- LZ4 is available to `luminumbra_common` (`src/luminumbra_common/CMakeLists.txt:14-22`), but there is no chunk serializer in `Chunk.h`/`Chunk.cpp`; `Chunk.cpp` only constructs, transitions state, and calculates IDs (`src/luminumbra_common/world/Chunk.cpp:6-73`).
- GPU SDF has a documented callback contract (`docs/shield/sdf-contract.md:119-156`), a compute shader (`res/shaders/sdf_generation.compute:1-140`), and renderer-side compute/readback code (`src/luminumbra_client/rendering/RenderPipeline.cpp:2137-2168`, `src/luminumbra_client/rendering/RenderPipeline.cpp:2261-2346`), but runtime integration is intentionally disabled (`src/luminumbra_client/rendering/RenderPipeline.cpp:54`, `src/luminumbra_client/rendering/RenderPipeline.cpp:341-344`).
- Networking/server are stubs. The requested `src/luminumbra_common/network/` path is absent; the actual build uses `src/luminumbra_common/net/NetworkManager.cpp` (`src/luminumbra_common/sources.cmake:13-14`). `NetworkManager` and `HostManager` only define constructors/destructors (`src/luminumbra_common/net/NetworkManager.h:5-9`, `src/luminumbra_common/net/NetworkManager.cpp:5-10`, `src/luminumbra_server/HostManager.h:6-10`, `src/luminumbra_server/HostManager.cpp:6-12`), and `main_server.cpp` only logs a startup message and returns (`src/luminumbra_server/main_server.cpp:3-7`).

## Findings

- Save/load currently loses identity and spawn state. `SaveWorld` writes `spawnPoint` and saves under `m_metadata.worldId` (`src/luminumbra_common/world/GameSession.cpp:303-323`), but `LoadWorld` restores only name, seed, worldType, and creationTime (`src/luminumbra_common/world/GameSession.cpp:257-266`), never `worldId` or `spawnPoint`.
- Streaming evicts chunks without a persistence hook: out-of-range chunks have collision removed and are erased from `m_streaming_state.chunks` (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:601-627`). The streaming map is private (`src/luminumbra_common/systems/SHIELD_WorldSystem.h:177-189`), so persistence also needs a deliberate snapshot/lookup API.
- The current chunk state is generation-oriented, not durable-state-oriented. States are `Unloaded`, `Loading`, `Idle`, `Meshing`, `Ready`, `Unloading` (`src/luminumbra_common/world/Chunk.h:11-13`), with no dirty bit, revision, persisted flag, or last-saved hash.
- Non-numeric world seeds use `std::hash<std::string>` (`src/luminumbra_common/world/GameSession.cpp:349-355`), which is not a good persistence/network determinism contract. Store a stable numeric seed and preset digest.
- GPU callback success is trusted too much. If the callback returns `true`, `GenerateChunkData` scans `chunk.sdf_data[sdf_idx]` to derive heightmap (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1058-1083`) without first validating size, finite values, or density coverage against the SDF contract (`docs/shield/sdf-contract.md:131-145`).
- GPU callback execution is unsafe for the current renderer implementation. `dispatch_generation_jobs` calls `GenerateChunkData` inside worker jobs (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1166-1177`), while `RenderPipeline::generate_chunk_sdf_gpu` performs GL calls and synchronous readback (`src/luminumbra_client/rendering/RenderPipeline.cpp:2271-2343`). That must move to a render-thread GPU job queue or stay disabled.
- The existing parity test does not run the GPU path. It generates only CPU SDF for known seeds (`test/shield/test_sdf_gpu_cpu_parity.cpp:75-80`, `test/shield/test_sdf_gpu_cpu_parity.cpp:130-147`) and asserts the renderer integration remains disabled (`test/shield/test_sdf_gpu_cpu_parity.cpp:110-125`).
- Far-field SDF rendering is not a runtime feature yet. Constants define near/far field distances (`include/luminumbra/core/Types.h:59-60`), but `render_frame` renders chunk mesh passes, water, skybox, and final blit (`src/luminumbra_client/rendering/RenderPipeline.cpp:676-770`), and pass metadata lists mesh-driven shadow/gbuffer/ssao/lighting/water without an SDF raymarch pass (`src/luminumbra_client/rendering/RenderPipeline.cpp:568-580`).
- The shader and CPU generator are not proven equivalent. CPU terrain/caves use FastNoise uniform grids with seed offsets (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1100-1107`), while the compute shader samples prebuilt textures with scaled coordinates and seed offsets (`res/shaders/sdf_generation.compute:49-90`). This may be acceptable, but only after an actual GPU-vs-CPU tolerance gate.
- Networking has no authority loop yet. The requested `src/luminumbra_common/network/` directory is absent, while the compiled `net` path points at a stub `NetworkManager` (`src/luminumbra_common/sources.cmake:13-14`, `src/luminumbra_common/net/NetworkManager.cpp:5-10`); the server executable only logs and exits with a future-work comment (`src/luminumbra_server/main_server.cpp:3-7`), and `HostManager` has no methods beyond construction/destruction (`src/luminumbra_server/HostManager.h:6-10`).
- The engine exposes a 30 Hz tick contract (`include/luminumbra/core/Types.h:42-43`) and world streaming has its own frame counter (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:307-312`), but the client player/physics path consumes variable `deltaTime` (`src/luminumbra_client/main_client.cpp:2216-2220`, `src/luminumbra_client/main_client.cpp:2351-2359`, `src/luminumbra_common/systems/PhysicsSystem.cpp:144-150`). Networking should start with a deterministic fixed-tick harness, not raw frame-time replay.

## Must-Fix

- Persist and reload complete world identity: write/read `worldId`, `spawnPoint`, stable numeric seed, world format version, preset digest, and schema version before relying on `SaveWorld`/`LoadWorld`.
- Add chunk persistence before shipping mutable worlds: versioned chunk header, coords, dimensions, section table, uncompressed sizes, LZ4-compressed SDF/heightmap/water sections, checksum, atomic write, and strict decode errors. Do not serialize mesh/collision as authoritative data.
- Add dirty tracking and a persistence API on the world system: dirty chunks, revision/hash, flush-on-evict, explicit save-all, and a deterministic snapshot of active chunks sorted by coordinate/id.
- Add minimal ECS snapshot support for the durable subset: stable entity id plus `TransformComponent`, `TagComponent`, `StaticMeshComponent`, and parent/child links from `CoreComponents.h:14-43`. `entt::entity` values cannot be the durable identity.
- Keep GPU SDF disabled until callback safety and GPU/CPU compute parity gates exist. Validate callback output before heightmap derivation, and never execute GL compute from worker generation jobs.
- Replace networking/server stubs with a deterministic loopback authority before sockets: fixed tick, ordered input frames, server-applied inputs, state hash broadcast, and disconnect-free local transport.

## Deepening Opportunities

- After per-chunk files pass the gate, move to region files with an index, tombstones, compact/repair mode, and region-level checksums. This improves IO locality but is not needed for the first persistence gate.
- Add compression telemetry: per-section raw bytes, compressed bytes, ratio, encode/decode time, and checksum failure counts in persistence artifacts.
- Promote a reusable `WorldHash` utility shared by persistence, networking, replay, and endurance soak. It should sort chunks/entities before hashing so insertion order cannot affect results.
- Add a GPU SDF debug overlay and artifact counters: callbacks attempted/succeeded/failed, CPU fallbacks, max GPU readback time, queue depth, and SDF parity deltas.
- Generalize runtime screenshot comparison helpers from existing visual gates into a CPU-vs-GPU render parity mode.

## Frontier Proposals

- Gate-first: `chunk_persistence_v1`. Implement per-chunk `.lchunk` files under `worlds/saves/<worldId>/chunks/`, one atomic file per chunk. Header: magic, version, coords, format flags, section count, section descriptors, payload checksum. Sections: SDF, heightmap, water levels, water flow, water terrain height, optional future edit/material section. Single-dispatch scope: serializer, deserializer, one save/load caller, one round-trip test.
- Gate-first: `world_entity_snapshot_v1`. Add a small JSON or binary entity snapshot for stable entity ids and the durable component subset only. Single-dispatch scope: serialize/deserialize two entity archetypes and compare canonical JSON/hash.
- Gate-first: `gpu_sdf_compute_parity_v1`. Extend `test_sdf_gpu_cpu_parity.cpp` from "integration remains guarded" to "headless GL compute matches CPU arrays" for the existing 9 seed/chunk cases. Keep the runtime flag off until this passes.
- Gate-first: `gpu_sdf_runtime_toggle_v1`. Replace direct callback GL calls with a render-thread GPU SDF queue and a world-system callback that can return completed GPU results or `false` for CPU fallback. Single-dispatch scope: no far-field raymarch yet; just safe callback plumbing plus counters.
- Gate-first: `far_field_sdf_slice_v1`. Add a narrow debug render mode that raymarches a deterministic far-field SDF horizon beyond mesh distance using the proven GPU SDF cache. Single-dispatch scope: one camera, one pass, one screenshot artifact; no general clipmap streaming yet.
- Gate-first: `loopback_authority_v1`. Implement in-memory `NetworkManager` loopback queues and `HostManager` fixed-tick authority for ordered input frames. Single-dispatch scope: two local clients, no sockets, no prediction, no NAT/session UI.
- Gate-first: `network_state_hash_v1`. Hash stable world state after each authoritative tick: tick number, seed/preset digest, sorted active persisted chunk hashes, and sorted durable entity snapshots. This is the gate primitive for later sockets.

## Proposed Gates

- `world_persistence_round_trip_v1`: deterministic scenario `persistence_round_trip_seed_4242`; unit test `WorldPersistenceRoundTripTest.GeneratedMutatedWorldReloadsIdentically`; artifact `build/<preset>/test-artifacts/persistence/world-persistence-roundtrip.json`; validator mode `validate-engine-frontier.ps1 -Mode PersistenceRoundTrip`. Flow: generate fixed chunks, mutate fixed SDF/water samples, create fixed durable entities, save, load in a fresh session, assert equal chunk section hashes and entity snapshot hash.
- `chunk_format_validator_v1`: unit test `ChunkPersistenceFormatTest.RejectsBadMagicVersionChecksumAndTruncation`; artifact `build/<preset>/test-artifacts/persistence/chunk-format-validation.json`; validator mode `validate-engine-frontier.ps1 -Mode ChunkFormat`. Enforces versioned header, coordinate match, section sizes, checksum, and LZ4 decode failure behavior.
- `deterministic_world_hash_v1`: unit test `WorldHashTest.SortedChunksAndEntitiesStableAcrossInsertionOrder`; artifact `build/<preset>/test-artifacts/persistence/world-hash.json`; validator mode `validate-engine-frontier.ps1 -Mode WorldHash`. Enforces identical hashes for equal state built in different insertion orders.
- `gpu_sdf_callback_safety_v1`: unit test `SdfGpuCallbackTest.InvalidCallbackResultsFallBackOrFailClosed`; artifact `build/<preset>/test-artifacts/render/gpu-sdf-callback-safety.json`; validator mode `validate-engine-frontier.ps1 -Mode GpuSdfCallbackSafety`. Scenarios: wrong vector size, NaN, infinity, false-with-partial-data, and valid data.
- `gpu_sdf_compute_parity_v1`: deterministic unit test `SdfGpuCpuParityTest.GpuComputeMatchesCpuKnownSeeds`; artifact `build/<preset>/test-artifacts/render/gpu-sdf-compute-parity.json`; validator mode `validate-engine-frontier.ps1 -Mode GpuSdfComputeParity`. Enforces max/mean SDF delta, sign-crossing agreement, finite values, and no GL debug errors.
- `gpu_sdf_runtime_visual_parity_v1`: deterministic scenario `gpu_sdf_runtime_parity`; artifacts `gpu-sdf-cpu.ppm`, `gpu-sdf-gpu.ppm`, and `gpu-sdf-runtime-parity.json`; validator mode `validate-engine-frontier.ps1 -Mode GpuSdfRuntimeParity`. Runs CPU path and GPU path from the same seed/camera path, compares world hash, callback counters, pass metadata, and ROI screenshot deltas.
- `far_field_sdf_visual_v1`: deterministic scenario `far_field_sdf_slice`; artifact `build/<preset>/test-artifacts/render/far-field-sdf.json` plus `far-field-sdf.ppm`; validator mode `validate-engine-frontier.ps1 -Mode FarFieldSdf`. Enforces nonblank far-field pixels, bounded CPU/GPU SDF delta for sampled rays, and no near-field mesh regression.
- `network_loopback_authority_v1`: deterministic scenario `network_loopback_two_clients_seed_9001`; unit test `LoopbackAuthorityTest.TwoSessionsConverge`; artifact `build/<preset>/test-artifacts/network/network-loopback-convergence.json`; validator mode `validate-engine-frontier.ps1 -Mode NetworkLoopback`. Feeds a fixed input trace to two local sessions through server authority and asserts equal per-tick and final world hashes.
- `reload_soak_v1`: deterministic scenario `persistence_reload_soak_100`; artifact `build/<preset>/test-artifacts/persistence/reload-soak.json`; validator mode `validate-engine-frontier.ps1 -Mode ReloadSoak`. Runs generate/mutate/save/reload for 100 cycles and asserts stable hashes, no dirty leak, and bounded artifact size.

## References

- `C:/Users/David/.codex/plugins/cache/personal/forge/0.17.0/skills/forge-cli/SKILL.md`
- `.forge/artifacts/engine-framework-roadmap/ultimate-plan.md`
- `docs/shield/sdf-contract.md`
- `include/luminumbra/core/Types.h`
- `res/shaders/sdf_generation.compute`
- `src/luminumbra_client/main_client.cpp`
- `src/luminumbra_client/player/PlayerController.cpp`
- `src/luminumbra_client/rendering/RenderPipeline.cpp`
- `src/luminumbra_client/rendering/RenderPipeline.h`
- `src/luminumbra_common/CMakeLists.txt`
- `src/luminumbra_common/components/CoreComponents.h`
- `src/luminumbra_common/net/NetworkManager.cpp`
- `src/luminumbra_common/net/NetworkManager.h`
- `src/luminumbra_common/sources.cmake`
- `src/luminumbra_common/systems/PhysicsSystem.cpp`
- `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp`
- `src/luminumbra_common/systems/SHIELD_WorldSystem.h`
- `src/luminumbra_common/world/Chunk.cpp`
- `src/luminumbra_common/world/Chunk.h`
- `src/luminumbra_common/world/GameSession.cpp`
- `src/luminumbra_common/world/GameSession.h`
- `src/luminumbra_common/world/WorldStreamingState.cpp`
- `src/luminumbra_common/world/WorldStreamingState.h`
- `src/luminumbra_server/CMakeLists.txt`
- `src/luminumbra_server/HostManager.cpp`
- `src/luminumbra_server/HostManager.h`
- `src/luminumbra_server/main_server.cpp`
- `test/performance/runtime_world_visual_validation_test.cpp`
- `test/shield/test_sdf_gpu_cpu_parity.cpp`
