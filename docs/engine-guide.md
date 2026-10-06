# Engine/framework guide

This guide describes how Luminumbra fits together at runtime and where an
engine change belongs. It complements the generated C++ reference, which is
best used after choosing the relevant layer here.

## Runtime lifecycle

A normal process follows this sequence:

1. Resolve the runtime root and load generated/configured engine settings.
2. Start `Luminumbra::JobSystem`.
3. Construct `Luminumbra::world::GameSession`, attach the job system, and set
   the runtime root.
4. Create a world from a preset or load an existing world and its authoritative
   state.
5. Advance the session through
   `Luminumbra::world::GameSession::TickSimulation`. The client feeds frame time
   into the fixed-step clock; the server requests exact fixed ticks.
6. Let the client consume simulation state for presentation, or let the server
   compute hashes/checkpoints for replication and replay.
7. Quiesce streaming work, persist authoritative state, destroy the session,
   and shut down the job system.

The graphical and headless programs use the same `GameSession`. They differ in
hosting and presentation, not in the definition of authoritative world state.

## Common engine

The common engine lives in `src/luminumbra_common/` and is deliberately usable
without a window, graphics context, UI toolkit, or audio device.

### Session and fixed-tick simulation

`Luminumbra::world::GameSession` is the composition root for one world. It owns the EnTT registry,
world/physics/water systems, deterministic environmental fields, Lua state,
persistence hooks, `luminumbra::core::SimulationClock`, and
`luminumbra::simulation::OrderedEventBus`.

`Luminumbra::world::GameSession::TickSimulation(frame_dt)` converts elapsed time
into canonical 30 Hz ticks with a bounded catch-up policy. Each produced tick
executes these high-level stages in order:

| Stage | Work |
|---|---|
| Pose | Animation sampling |
| Creature simulation | Planning, perception, scent, locomotion, survival, reproduction, and physics-backed movement |
| Environment | Wind, weather, re-derivable aether, and optional stateful energy |
| Ecology fields | Soil nutrients and irrigation |
| Plants and living world | Growth, pollination, disease, fire, grazing, and lifespan |
| Publication | Ordered events for the completed tick |

Stable entity traversal, explicit random-stream seeds, fixed-point boundaries,
and canonical serialization make run/replay and host/peer comparisons meaningful.

Simulation changes must not depend on unordered container iteration, wall-clock
completion, render state, or device-specific results. If a new subsystem owns
authoritative state, it also needs deterministic ordering, persistence behavior,
a named diagnostic hash, and run/replay coverage.

### World generation and streaming

`Luminumbra::Systems::SHIELD_WorldSystem` owns procedural density/height
sampling, chunk generation, streaming, and terrain edits. Near terrain uses
resident chunk data; far terrain uses region tiles and reduced SDF authority.
The [SHIELD SDF contract](shield/sdf-contract.md) defines sign convention,
sample ownership, shared borders, and LOD behavior.

Generation and meshing may execute through `JobSystem`, which has high and
normal priority lanes plus starvation protection. Completion timing is not
publication timing: batches become visible in deterministic dispatch/tick order.
World-generator reconfiguration takes an exclusive epoch while sampling jobs
hold shared epochs, preventing a worker from observing a half-rebuilt generator
set.

### ECS and systems

The registry is owned by `GameSession`; component definitions live under
`src/luminumbra_common/components/`. Systems are grouped by responsibility under
`systems/`, `simulation/`, `ai/`, `fields/`, and `foliage/`. A system that
participates in authoritative simulation is called from the explicit fixed-tick
sequence rather than from a presentation frame callback.

Optional simulation features use an off-by-default, empty-neutral contract:
when disabled or without participants they allocate no authoritative state,
write no save records, and contribute no hash bytes. This keeps old worlds and
default configurations stable.

### Persistence, replay, and networking

`Luminumbra::Persistence::WorldSaveService` persists canonical world/chunk
records. Existing saved chunks are adopted before generation fills gaps, so
procedural regeneration cannot overwrite edits. Replay checkpoints capture the
top-level world hash and named subsystem hashes from one quiesced snapshot.

Networking and replication live under `net/` and `network/`. Transport delivery
is separated from simulation activation: commands and replicated state become
authoritative only at their assigned tick/order. Hashes localize a desync without
making logs or timing part of the state itself.

## Client framework

The client under `src/luminumbra_client/` owns windowing, input, UI, audio, and
rendering. It may read common state and maintain presentation caches, but it may
not mutate simulation based on frame rate, GPU visibility, capture output, or
audio availability.

### Frame and render graph

`main_client.cpp` hosts the frame loop. Fixed simulation ticks run independently
of presentation; render preparation then consumes the current world state.

`Luminumbra::Rendering::RenderGraph` declares the ordered GPU stages and their
named read/write resources.
`Luminumbra::Rendering::BuildLuminumbraFrameGraph()` is the canonical graph; the
runtime executor trace is tested against its schedule. A new pass therefore
needs both graph resource declarations and an executor implementation.
Structural tests catch read-before-write and ordering errors, while
software-OpenGL captures validate visible output.

Backend-specific handles remain behind the rendering/RHI boundary. Common engine
types do not include OpenGL or Diligent objects, which preserves the headless
server dependency boundary.

### UI, audio, and optional data

RmlUi documents and engine-owned component/listener lifetimes form the UI layer.
Audio banks in Git are metadata only; sound binaries remain optional and
external. `--no-audio` selects `Luminumbra::Client::NullAudioManager`. Normal
startup loads the metadata manifests, while absent external event files degrade
to silence without preventing the world from starting.

## Headless server

`Luminumbra::Server::ServerWorldRunner` is the supported headless host around
`GameSession`. `Boot()` starts jobs, creates or loads the world, adopts saved
state before streaming, and prepares the spawn region. `RunFixedTicks()` advances
exact ticks in the server order: physics, session simulation, anchor streaming,
then publication of batches due for that tick. Full streaming quiesces are
reserved for boot, hash/checkpoint, mutation, save, and teardown boundaries.
The runner exposes world/subsystem hashes, replay checkpoints, availability
traces, and full-snapshot persistence for verification and networking.

The server target links the common engine without OpenGL, GLFW, RmlUi, ImGui, or
audio backends. This link boundary is tested and is the practical check that a
new common feature remains genuinely headless.

## Data and configuration

Runtime inputs have explicit homes:

| Path | Contract |
|---|---|
| `src/luminumbra_common/core/ConfigSchema.json` | Typed system configuration schema and simulation/render residency source of truth |
| `data/common/systems.json` | Runtime configuration defaults |
| `src/luminumbra_common/core/*.gen.h` and `res/shaders/config_constants.gen.glsl` | Generated outputs; regenerate with `tools/config_codegen.py` |
| `config/public-asset-inventory.json` | Classification and hashes for published non-source assets |
| `data/` | Manifested runtime data copied into build trees |
| `res/shaders/` | Runtime shader sources and reflection inputs |
| `worlds/` | Authored world definitions and presets |
| Local `.glb` staging | Optional authored model inputs converted to `.lmesh` at configure/build time; not included in this source release |

Configuration keys are declared in the schema and generated into C++ rather
than duplicated by hand. Authoritative keys are distinguished from render-only
keys. Published non-source assets are classified and hashed in the public asset
inventory, with licensing recorded in `THIRD_PARTY_LICENSES.md`.

## Embed the common engine

The project does not yet install a stable SDK, but its in-tree hosts share this
ownership pattern:

```cpp
Luminumbra::JobSystem jobs;
jobs.startup();

{
    Luminumbra::world::GameSession session;
    session.SetJobSystem(&jobs);
    session.SetRootPath(runtime_root.string());

    if (!session.CreateWorld("Example", "1337", "default"))
        throw std::runtime_error("world creation failed");

    session.LoadWorldState();
    while (running)
        session.TickSimulation(frame_seconds);

    // This call quiesces streaming before it snapshots authoritative state.
    if (!session.SaveWorldState())
        throw std::runtime_error("world save failed");
} // Destroy the session before its worker pool.

jobs.shutdown();
```

For a load path, replace `CreateWorld(...)` and `LoadWorldState()` with
`LoadWorld(world_id)` followed by `LoadWorldState()`. A production host should
also configure required assets and feature tuning before world creation, as the
client and `Luminumbra::Server::ServerWorldRunner` do.

## Extending the engine

### Add an authoritative simulation system

1. Put state and logic in the common engine; keep presentation adapters in the
   client.
2. Define its position in `GameSession`'s fixed tick order.
3. Make entity/record iteration canonical and randomness seed-derived.
4. Define save/load and empty/disabled behavior.
5. Add a diagnostic sub-hash when the system owns persistent state.
6. Test deterministic replay, failure paths, and headless operation.

### Add a rendering pass

1. Declare graph reads, writes, and conditional behavior in
   `Luminumbra::Rendering::BuildLuminumbraFrameGraph()`.
2. Add the executor and keep resources behind the render/RHI boundary.
3. Add shader reflection/layout coverage.
4. Add structural render tests and a maintained visual observation when the
   result cannot be established from resource contracts alone.

### Add configuration or runtime content

1. Add configuration to the schema, assign simulation or render residency, and
   regenerate/check generated sources.
2. Add runtime files to the explicit runtime-data manifest.
3. Classify any publishable non-source asset in the public asset inventory and
   update third-party licensing when applicable.
4. Verify both client and headless behavior for optional content.

## Where to read next

- [Architecture](architecture.md) for dependency boundaries.
- [Development and testing](development.md) for builds and CI tiers.
- [Performance measurement](performance.md) for relative regression gates.
- [Visual regression](visual-regression.md) for capture evidence.
- [SHIELD SDF contract](shield/sdf-contract.md) for terrain invariants.
