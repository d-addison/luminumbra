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

`GameSession` already ticks instinct planning, perception/awareness, scent and
creature-brain systems for participating entities, as well as plant growth and
ecology. Headless avatar, creature and plant rosters have hash/replay coverage;
their default opt-in behavior is not an absence of implementation.

`PhysicsSystem::audio_raycast` obtains the hit body's surface normal from Jolt
and orients it against the incoming ray. Attached terrain queries use the world's
voxel/biome material classifier; dynamic bodies use Stone. Height-band material
fallback applies only when no world system is attached.

Persistence uses LMR1 container v2, the v1 world-manifest schema identifying that
container version, and FSD2 payload v3. The
[world format contract](engine-guide.md#world-format-compatibility) gives the
v0.3.0 refusal message, preset revision 6 and separate obsolete/future/corrupt
failure behavior. Durable legacy worlds are not migrated.

Reliable and unreliable delivery are implemented in the shared GNS/Steam
`NetSocketsTransport`; Loopback/TCP remain reliable. The GNS CI build validates
the shared source against its real dependency. Live multi-client transport
testing and Steam SDK validation are separate from that build coverage.

## Rendering model

The client consumes snapshots of common state and stages work through rendering
passes. CPU resource ownership and render-resource handles remain engine-owned so
backend-specific objects do not escape into simulation code. Shader sources live
under `res/shaders/`; automated inventory and compilation checks guard against
orphaned or invalid shader files.

Visual results are validated separately from deterministic state. Structural tests
cover pass contracts and resource lifetimes, while captured frames and perceptual
diffs cover output that cannot be proven from source inspection.

OpenGL remains the shipping backend. A required Diligent CI lane builds opt-in
test targets and checks software GL/Vulkan device creation and calibration-cube
parity, without linking Diligent into shipping binaries. Current motion vectors
cover camera reprojection and instanced foliage sway; independent rigid-object
and skinned previous-frame history remain gaps. Tree impostors default on when
startup atlas baking succeeds.

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
