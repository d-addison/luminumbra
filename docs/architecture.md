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

## Dependency boundaries

Third-party revisions are pinned in CMake dependency declarations or the vendor
build. First-party code includes third-party APIs at narrow adapter seams. Optional
integrations, such as Tracy or alternative rendering backends, default off and must
not change the normal deterministic build.
