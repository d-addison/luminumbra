# Architecture

Luminumbra separates authoritative world state from presentation so the same
simulation can serve a local client, a replay, or a headless network session.

## Runtime layers

| Layer | Ownership | Key locations |
|---|---|---|
| Common engine | ECS state, fixed-tick simulation, world generation, physics, persistence, replay, scripting, and networking | `src/luminumbra_common/` |
| Client | Windowing, input, audio, UI, render graph, shaders, and visual resource residency | `src/luminumbra_client/`, `res/shaders/` |
| Server | Headless authoritative process and server orchestration | `src/luminumbra_server/` |
| Tools | Asset conversion, captures, validation gates, and CI evidence checks | `tools/` |

`luminumbra_common` must not depend on client libraries. The server executable
links the common engine without OpenGL, GLFW, RmlUi, ImGui, or audio backends.
Client code may consume common state, but presentation timing and GPU results must
not feed authoritative hashes.

## Simulation model

The simulation advances at a fixed cadence. Canonical entity ordering, pinned
numeric behavior, deterministic random streams, and explicit serialization make
run, replay, and network comparisons meaningful. Work may execute concurrently,
but availability and activation of authoritative state must be derived from
deterministic inputs rather than wall-clock completion order.

The common engine groups functionality by responsibility:

- `world/` owns spatial data: chunks, meshing, biome/erosion tables, far-LOD
  storage, and the `GameSession` lifecycle. Terrain generation and streaming are
  driven by the SHIELD world system in `systems/`.
- `systems/`, `simulation/`, `ai/`, `fields/`, and `foliage/` advance world
  state. `systems/` also integrates the physics backend behind engine-owned
  types (`PhysicsSystem`) and hosts the water, weather, wind, and aether solvers.
- `persistence/` and `replay/` encode and verify durable state.
- `net/` and `network/` contain transport, lockstep, and replication paths.
- `scripting/` exposes the supported Lua surface.

## Rendering model

The client consumes snapshots of common state and stages work through rendering
passes. CPU resource ownership and render-resource handles remain engine-owned so
backend-specific objects do not escape into simulation code. Shader sources live
under `res/shaders/`; automated inventory and compilation checks guard against
orphaned or invalid shader files.

Visual results are validated separately from deterministic state. Structural tests
cover pass contracts and resource lifetimes, while captured frames and perceptual
diffs cover output that cannot be proven from source inspection.

## Data flow

Authored models under `assets/` are converted by `asset_processor`. Authored runtime
configuration and content under `data/` are copied into each build tree from an
explicit manifest. Shader sources under `res/` remain runtime inputs. World
definitions live under `worlds/`. Large local audio sources and generated captures
are intentionally outside version control.

## On-disk world format

If a world created before Luminumbra 0.3.0 will not load, that is the expected
compatibility boundary, not evidence that the save is merely missing. There is no
legacy-format fallback: create a new world with 0.3.0 or later. In particular, a
diagnostic such as `region file uses retired pre-v0.3.0 LMR1 container version 1`
means the loader deliberately refused the old region format.

Persistent chunk and far-LOD records live under
`worlds/saves/<world-id>/chunks/region/`. Each `r.<rx>.<rz>.lmr` file owns a
32-by-32 chunk region, where `rx = floor(chunk_x / 32)` and likewise for `rz`.
All multibyte container header fields are little-endian. The container layout is:

1. Four-byte magic `LMR1`, followed by a `u16` container version and a `u16`
   record count. The only accepted container version is 2. Version 2 is the
   v0.3.0 format; version 1 and every other version are refused rather than
   migrated.
2. One 18-byte header per record: `u64 id`, `u8 lod_level`, `u8 flags`, `u32`
   uncompressed size, and `u32` compressed size. Headers are ordered by
   `(lod_level, id)`. Flag bit 0 marks edited or authoritative data and bit 1
   records water presence.
3. The corresponding LZ4-compressed payloads in header order. LOD 0 records are
   full chunk snapshots; LOD 1 and 2 records are far-LOD tiles.

The adjacent `world-manifest.json` is written with this exact schema:

```json
{
    "schema": "luminumbra.persistence.world_manifest.v1",
    "container": "LMR1",
    "container_version": 2
}
```

This persistence manifest identifies the region-container contract; it is
separate from the world's gameplay metadata in `world_info.json`. The manifest
schema remains `world_manifest.v1` while `container_version` identifies LMR1
version 2.

Far-LOD records use only the `FSD2` payload. The payload begins with four-byte
magic `FSD2` and `u16` payload version 2, followed in order by the `u8` tier,
`i32 rx`, `i32 rz`, `u32 samples_per_side`, `u64 params_hash`, and a `u8` edited
marker. Count-and-byte-length fields frame row-major `u16` quantized heights,
`u8` materials, and `u8` sample flags. A `u32` descriptor count then introduces
sorted 16-byte SDF brick descriptors containing local chunk X/Z, source kind,
a reserved byte, chunk Y, revision, and payload CRC32. Counted `i16` quantized
density and `u8` SDF-material streams follow. The loader rejects an unsupported
header, inconsistent counts, invalid descriptors, CRC failures, and trailing or
truncated bytes. The retired height-only far-LOD payload is not accepted.

World presets still provide the seed and numeric terrain tuning, but they no
longer select a per-world generation feature set. The SHIELD generator always
uses the current shaping, cave-router, biome, river, lake, cliff, structure, and
erosion paths; the legacy feature-disable branches and cave style are gone. A
pre-0.3.0 preset therefore cannot be used to recover the terrain behavior needed
by an old save.

## Dependency boundaries

Third-party revisions are pinned in CMake dependency declarations or the vendor
build. First-party code includes third-party APIs at narrow adapter seams. Optional
integrations, such as Tracy or alternative rendering backends, default off and must
not change the normal deterministic build.
