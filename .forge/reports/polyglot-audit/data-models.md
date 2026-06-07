# Luminumbra Data Models Audit

Generated: 2026-06-07

## Scope

Audited first-party C++, Lua, JSON, RmlUi markup/state code, and Forge audit context. Vendor, external, and build outputs were excluded. Forge status at audit time: `spec`, no active dispatch.

## Executive Summary

Luminumbra has useful prototype models for SHIELD world generation, chunks, EnTT components, RmlUi callbacks, audio events, and binary meshes, but most contracts are implicit. The highest-risk areas are ambiguous SDF/LOD invariants, asymmetric world metadata persistence, public mutable chunk buffers, fractured material identity, stringly typed content/API boundaries, and JSON formats without schema validation.

The target direction should be a small set of first-class domain types: `WorldManifest`, `GenerationPreset`, `ChunkCoord`, `SdfGrid`, `ChunkLodSelection`, `WaterGrid`, `MaterialRegistry`, `SoundBank`, `ArchetypeDefinition`, `AppState`, and versioned asset/save formats.

## Current Model Surface

- Core aliases live in `include/luminumbra/core/Types.h`: EnTT entity IDs, numeric aliases, GLM vector/matrix aliases, chunk/world constants, and `MaterialType`.
- World/session state lives in `GameSession`: `WorldMetadata`, `TerrainGenParams`, EnTT registry, and owned world/physics/water systems.
- Chunk state is `Chunk`: coordinates, packed `ChunkID`, lifecycle enum, SDF/heightmap vectors, terrain/water mesh vectors, collision/LOD/mesh-version atomics, water simulation vectors, and water sleep counters.
- ECS components are plain structs: transform/tag/hierarchy/static mesh, needs/senses/action plan, point light, water source/interactor, and audio source.
- Runtime state is split between `GameStateManager`, globals in `main_client.cpp`, and the newer `Client::UI::UIStateManager`.
- Content formats include world preset JSON, audio bank JSON, material JSON, archetype JSON, Lua behavior scripts, RML/RCSS UI, GLSL shaders, and `.lmesh` binary meshes.

## Findings

### P0: SHIELD Invariants Are Ambiguous

- SDF sign is inconsistent with tests and meshing semantics. Tests expect below-terrain density to be negative and above-terrain density positive, while `get_density_at_from_precalculated()` returns `terrain_height - y`. Marching Cubes treats values `< isolevel` as inside. This affects normals, cave carving, and all terrain consumers.
- LOD mixes `level` and `step`. `ChunkLOD` defines both, but `get_lod_level_for_distance()` returns `level` and meshing treats that value as the Marching Cubes step. Near chunks can therefore pass `0` into loops that increment by `step`.

Target shape: add `SignedDistance`/`SdfGrid` documentation and tests for sign, indexing, and surface convention. Replace `int required_lod` with `ChunkLodSelection { int level; int step; float max_distance; }`.

### P0: World Persistence Is Metadata-Only And Asymmetric

- `SaveWorld()` writes `name`, `seed`, `worldType`, `creationTime`, and `spawnPoint`, but `LoadWorld()` does not restore `spawnPoint`.
- `worldId` is path-derived but not saved in metadata.
- There is no schema version, engine/content version, generator snapshot, chunk persistence, player/session state, checksum, or atomic save protocol.
- README/TDD describe binary chunk data, compression, integrity checks, and `.lworld` files, but current code regenerates the world from seed and preset.

Target shape: `WorldManifestV1 { schema_version, world_id, display_name, seed_text, seed_u32, preset_id, generator_snapshot, created_at, last_played_at, spawn, engine_version, content_hashes }`, plus separate versioned chunk/player save records.

### P1: JSON Formats Are Ad Hoc

- World presets assume `generation_params.terrain` and `.features` exist; `biomes`, `rivers_enabled`, and `structures_enabled` are present but ignored.
- Audio banks contain `strategy`, `layers`, procedural/adaptive fields, `is_3d`, triggered events, and chorus settings, but `MiniaudioManager::LoadBank()` only loads a basic subset and ignores or silently drops the rest.
- `data/common/materials.json` exists but rendering uses hard-coded texture/LUT data.
- Archetype JSON refers to components that do not exist or have no loader: `PhysicsComponent`, `DietComponent`, `AethericFieldComponent`, `FoodSourceComponent`, `RenderMeshComponent`, `ScriptedAgentComponent`, combat/stealth components.

Target shape: add JSON Schema files and C++ `from_json` validators for each format. Unsupported fields should either fail validation or be explicitly recorded as ignored with a version gate.

### P1: Chunk Data Has Weak Ownership And Shape Rules

- `Chunk` exposes simulation and render vectors publicly, while background jobs, water simulation, collision upload, and renderer upload all consume them.
- `mesh_version` is atomic, but the buffers it versions are not protected by an ownership protocol.
- Adaptive water resolution stores `current_water_resolution`, but many queries and updates still index with fixed `WATER_SIM_RESOLUTION_X/Z`. `WaterDetailLevel::Off` is also never applied cleanly.

Target shape: split chunk internals into `SdfGrid`, `HeightField`, `MeshBuffers`, and `WaterGrid`, each with dimensions, indexing helpers, and single-writer rules. Publish immutable snapshots or versioned handles to render/physics consumers.

### P1: Material Identity Is Fractured

- `MaterialType` defines `Air=0`, `Stone=1`, `Soil=2`, `Grass=3`, `Sand=4`, `Deepslate=5`, `LuminCrystal=6`, `Water=7`.
- `data/common/materials.json` assigns different IDs/names and omits `Air`, `Grass`, and `Water`.
- `RenderPipeline` builds terrain textures and material LUTs from hard-coded arrays that must match material IDs by convention.

Target shape: make `MaterialRegistry` the canonical source of material ID, name, render properties, texture paths, physics/audio tags, and serialization version. Generate or validate enum/LUT alignment from it.

### P1: Runtime State Models Are Duplicated

- `src/luminumbra_client/core/GameState.h` and `src/luminumbra_client/ui/core/UIStateManager.h` define different game-state enums.
- World loading state is held in globals (`g_initial_chunks_to_load`, `g_generation_dispatch_index`, etc.) rather than a lifecycle model.
- UI callbacks use raw strings for world name, seed, world type, and world IDs.

Target shape: centralize `AppState` and expose typed commands/events such as `CreateWorldRequest`, `LoadWorldRequest`, `WorldLoadingProgress`, and `EnterWorld`.

### P1: Audio Contracts Are Overdeclared And Underimplemented

- `AudioEventDefinition` declares layered, procedural, adaptive, interrupt, crossfade, and chorus fields, but the active loader/playback path only implements basic random file selection plus simple 2D/3D flags and distances.
- `UnloadBank()`, `SetEventVolume()`, and `SetEventParameter()` are API promises but stubs.
- Several advanced audio systems are header-heavy and not wired into active source lists.

Target shape: introduce `SoundBankV1` with capability flags. Parse the full supported subset, reject unsupported strategies, and separate authored event definitions from backend runtime sound instances.

### P2: IDs And API Boundaries Are Stringly Typed

- `AudioEventID`, `AudioParamID`, world type, component type, material names, script paths, and document paths are all unvalidated strings.
- `ChunkID`, audio handles, mesh IDs, material IDs, and entity IDs are numeric aliases rather than strong IDs.

Target shape: use small strong types (`WorldId`, `PresetId`, `AudioEventId`, `MaterialId`, `ChunkCoord`, `ChunkId`) with constructors/validators at file and UI boundaries.

### P2: Binary Asset Format Needs Versioning

- `.lmesh` uses a raw `LMeshHeader` with magic, counts, and bounding sphere only.
- There is no version, endian tag, vertex stride, index format, bounds type, offsets, checksum, or read-count validation.

Target shape: `LMeshHeaderV1 { magic, version, endian_tag, vertex_stride, index_size, vertex_count, index_count, bounds, vertex_offset, index_offset, crc32 }`, with strict loader validation.

### P2: Scripting And Archetypes Are Not A Runtime Model Yet

- `LuaState` is empty.
- Archetype JSON exists, but there is no component factory registry, schema, or spawn path connecting JSON/Lua to EnTT.
- Instinct components are data-only and do not match authored archetype fields such as hunger/thirst rates or senses radii.

Target shape: `ArchetypeDefinition { id, components[] }` plus `ComponentFactoryRegistry`, component-specific DTOs, and tests that spawn a known archetype into an EnTT registry.

## Recommended Target Shapes

1. `GenerationPreset`: nested `TerrainSettings`, `CaveSettings`, `BiomeSettings`, and `FeatureFlags`; loaded from presets with validation and defaults.
2. `WorldManifestV1`: versioned, round-trippable world metadata; restore all saved fields; include generator snapshot and content hashes.
3. `ChunkDomain`: `ChunkCoord`, `ChunkId`, `ChunkLifecycle`, `SdfGrid`, `HeightField`, `MeshBuffers`, `WaterGrid`, and explicit job ownership.
4. `MaterialRegistry`: canonical material data consumed by terrain generation, renderer LUTs, physics, audio, and save files.
5. `SoundBankV1`: fully validated authored sound event definitions, separate from runtime `PlayingSound`.
6. `AppState` plus typed UI/runtime commands instead of duplicate enums and raw string callbacks.
7. `ArchetypeDefinition` and component factory loaders for JSON/Lua-driven entity creation.
8. `LMeshHeaderV1` with version, stride, offsets, endian marker, and integrity checks.

## Near-Term Actions

1. Fix or explicitly redefine SDF sign and `ChunkLOD` step semantics before further SHIELD refactors.
2. Make `WorldMetadata` save/load round-trip and add `schema_version` plus `worldId`.
3. Add schemas or strict C++ validators for world presets, sound banks, materials, and archetypes.
4. Consolidate material IDs into one registry and make renderer data derive from it.
5. Replace duplicated game-state enums with one app-state model and typed world creation/load requests.
6. Add chunk/water grid dimension helpers and remove fixed-resolution indexing from adaptive water paths.
