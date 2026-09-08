# Engine/framework guide

This guide describes how Luminumbra fits together at runtime and where an
engine change belongs. It complements the generated C++ reference, which is
best used after choosing the relevant layer here.

## World format compatibility

> This world predates the v0.3.0 format and cannot be opened. Create a new world; migration is not supported.

This is the actionable refusal for recognized obsolete saved-world formats.
Create a new world with a current preset. There is no migration or legacy load
fallback, including when obsolete artifacts coexist with current region files.

| Layer | Current identity |
|---|---|
| World metadata | `world_info.json` declares `container_version: 2` at creation, before any chunk records exist; missing versions are obsolete |
| Region container | `chunks/region/r.<rx>.<rz>.lmr`: `LMR1` magic, little-endian `u16` container version `2`, `u16` record count, record headers and LZ4 payloads |
| World manifest | `chunks/region/world-manifest.json`: `schema: "luminumbra.persistence.world_manifest.v1"`, `container: "LMR1"`, `container_version: 2` |
| Far-LOD record payload | `FSD2` magic, payload version `3` inside the LMR1 container |
| Terrain preset | Explicit `schema_rev` must be the integer `6`; omitted revision remains accepted by the shared parser for in-memory callers |

The manifest schema is still v1; it is distinct from container v2, FSD2 payload
v3, and project version 0.3.0. Lod-0 records retain the canonical
`world_state_snapshot.v1` in-memory serialization. This does not restore support
for obsolete on-disk snapshots, their backups, LMR1 v1, or older far-LOD payloads.

`generation_params.terrain.shaping.enabled` and
`generation_params.features.cave_style` are retired selectors. Their **presence
is an error regardless of value**, including `false` or `null`; they are not
unknown-key warnings. Terrain shaping always runs, with default shaping parameters
when its optional tuning block is absent. When `features.caves_enabled` is true,
caves always use the noise router combining cheese, spaghetti, and Worley fields.
There is no selectable legacy cave algorithm. `caves_enabled` still controls
whether caves are generated.

Content tuning remains: base noise and shaping splines/warp, cave frequency and
carve settings, island masks, surface breaks, cliffs, rivers, lakes and structures.
Hydraulic/thermal relief remains opt-in through `terrain.hydro.enabled`; biome
tables, climate frequencies and biome relief retain their controls. The
[SDF contract](shield/sdf-contract.md) describes their sampling semantics.

Failures are deliberately distinct. Recognized older versions receive the refusal
above. Higher versions report `unsupported future LMR1 container version`,
`unsupported future world manifest schema`,
`unsupported future world manifest container version`, or
`unsupported future far-LOD payload version`, with the offending value. Malformed
magic, manifest identity, headers, compressed data and truncated payload streams
produce corruption/validation diagnostics, not obsolete-world advice. Loading
refuses invalid durable data rather than regenerating over it. The persistence,
far-LOD and terrain-preset tests cover these boundaries.

Metadata is checked before a missing chunk directory can count as a fresh save.
Unversioned metadata is refused even alongside current containers. A higher
metadata version reports `unsupported future world metadata container version`;
invalid JSON or invalid version types are corruption failures.

### Simulation clock metadata

The hashed `sim.active_regions` key is off by default. When enabled, every
`GameSession` save entry point writes two additional `world_info.json` keys
beside `waterSimCursor`, through `WorldSaveService`'s existing temporary-file
and atomic replacement path:

```json
"simulationTick": 317,
"calendar": { "dayLengthTicks": 36000, "daysPerYear": 8 }
```

`simulationTick` is the absolute completed host tick; there is no wall-clock
progression while a world is closed. Tick zero is midnight on day zero in
midwinter. At 30 Hz the default day lasts 20 minutes and the year lasts eight
days. Plants and circadian activity use the midnight phase directly; migration,
stimulus seasons and the sky's seasonal sine offset the year by three quarters
to their spring-equinoctial origin. Sky geometry offsets the day by half a cycle
because its existing zero is noon. An explicit render day-length override and
photo/scenario time pins remain presentation controls.

| Metadata condition | Open / write behavior |
|---|---|
| `simulationTick` absent | Tick zero |
| `calendar` absent | Pinned defaults: 36,000 ticks/day, 8 days/year |
| `simulationTick` present | JSON unsigned integer in `[0, 2^62)` |
| `calendar` present | Object containing exactly `dayLengthTicks` and `daysPerYear` |
| `dayLengthTicks` | JSON integer in `[1, 2^31)` |
| `daysPerYear` | JSON integer in `[1, 366]` |
| Invalid type, null, negative/out-of-range number, fractional/exponent number, incomplete calendar, unknown calendar member or arithmetic overflow | Corrupt world metadata; refuse before generation or writing |
| Either clock key present with `sim.active_regions` off | Incompatible configuration; refuse without modifying disk bytes |

Day × year and tick + the catch-up bound are checked before use. Validation
also applies to initialized saves with no chunk directory, catalog inspection,
explicit snapshot directories, and saves over existing metadata. The catalog
reports valid clock metadata without selecting a runtime configuration; opening
checks the feature requirement. Presence of either clock key marks the save as
requiring `sim.active_regions`; metadata writes cannot remove both keys from an
initialized clock save. Save calls inside a simulation catch-up batch are
refused; successful snapshots observe a completed tick boundary.

With the key off, saves omit both keys, loads restart at zero, and every legacy
period and hash byte stays unchanged. With it on, clock canonical bytes fold
into the existing ecology hash slot without changing the top-level hash order.
The canonical clock section is ASCII `world_clock:v1:` followed by little-endian
`u64 simulationTick`, `u32 dayLengthTicks`, and `u32 daysPerYear` (16 payload
bytes). It excludes frame accumulators and telemetry. Calendar and tick changes
therefore affect the hash even when the entity roster is empty.

C1 reconstructs wind and aether ambience at the restored tick and reconstructs
fixed-anchor weather over at most its 240-tick storm lifetime. In the key-on
path, storms move using base wind; their current gusts then perturb the
authoritative wind consumed by aether, plants and other systems. This removes
storm-to-wind-to-storm motion feedback from that path, so expired storms cannot
carry unbounded history into later storms. The key-off update order and feedback
are unchanged. This covers the current spawn-anchored ambient systems; regional storm records and activation
history belong to C4. The snapshot commit index, rollback journal, and multi-file
recovery belong to C2/C3. C1 uses the existing metadata write path and is not a
claim of atomic publication across chunk, entity, energy and metadata files.
The feature remains default-off until those persistence slices land. LMR1 v2,
world_manifest.v1, FSD2 v3, preset revision 6, and existing canonical chunk and
entity serialization are unchanged.

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

Instinct planning, perception/awareness, scent deposition and steering, creature
brains and plant growth already run in this sequence. They operate on entities
with the relevant components. The server can boot deterministic avatar, creature
and plant rosters; an empty default roster does not mean these systems are unwired.

Optional simulation features use an off-by-default, empty-neutral contract:
when disabled or without participants they allocate no authoritative state,
write no save records, and contribute no hash bytes. This keeps disabled/default
configurations stable within the current world format.

### Persistence, replay, and networking

`Luminumbra::Persistence::WorldSaveService` persists canonical world/chunk
records. Existing saved chunks are adopted before generation fills gaps, so
procedural regeneration cannot overwrite edits. Replay checkpoints capture the
top-level world hash and named subsystem hashes from one quiesced snapshot.

Networking and replication live under `net/` and `network/`. Transport delivery
is separated from simulation activation: commands and replicated state become
authoritative only at their assigned tick/order. Hashes localize a desync without
making logs or timing part of the state itself.

The shared `NetSocketsTransport` implements reliable and unreliable delivery for
both GNS and Steam sockets. Loopback and TCP always deliver reliably and ignore
`FrameDelivery`. CI builds the GNS implementation against upstream headers and
libraries; that compile coverage does not test a live GNS/Steam multi-client
connection. The owner-provided Steam SDK needs separate build validation.

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

OpenGL is the shipping renderer. The opt-in Diligent targets have a required CI
lane with software GL and Vulkan device bring-up and calibration-cube parity;
they do not provide a shipping second renderer. The G-buffer already supplies
camera reprojection and instanced foliage wind-sway motion to TAAU. Previous
rigid-object transforms and previous skinned bone poses remain missing.
Far-tree impostors are enabled at startup by default when atlas baking succeeds.

Directional-light inputs describe ray travel; surface lighting uses their
opposite. Each shadow cascade clears its own depth layer every frame. Aether
enhances authored emissive materials without making ordinary ground emit light.
The contrast curve preserves black and shaded material differences. Public
procedural tree leaves use a two-sided cutout with consistent reflectance across
mesh LODs. Ground-cover sizes in `data/common/foliage/scatter_set.json` are metres
before 20% variation; grass is 12–20 cm tall, with wind bend bounded by blade height.
Grass anchors use the triangles uploaded to the terrain renderer, including mesh
replacement and removal. Visibility follows the frustum and scene depth, without
an artificial screen-height cutoff. Live rain and snow use camera-relative spawn
volumes with particles that retain world-space positions; wind does not distort
the terrain image. Ground snow cover samples weather at the terrain elevation.
The weather fog layer activates for fog weather; clear-air haze belongs to the
aerial pass. Both layers account for how much of an elevated sightline passes
through air near the ground.

Underwater effects require the camera to be between a water bed and its surface.
Dry underground space below sea level remains air. Underground streaming retains
a bounded full-SDF neighbourhood around the player below the surface band,
up to 128 m horizontally and 64 m vertically within the active chunk budget.
Aerial haze and water depth reconstruction distinguish cleared sky depth from
all visible terrain depths. Screen-space god rays use opaque depth to mask land.
The current far terrain radius is approximately 3 km, with a 3200 m camera far
plane; it is a finite horizon and does not provide unlimited distant terrain.
Distant surface tiles remain resident beneath the live terrain ring. The camera-region
near-plane guard applies only near that region's vertical bounds, so an elevated view
does not lose a square of ground beneath it. Water tint and caustics use the water pass;
opaque cave floors do not become water-coloured simply because they are below sea level.

### UI, audio, and optional data

RmlUi documents and engine-owned component/listener lifetimes form the UI layer.
Audio banks in Git are metadata only; sound binaries remain optional and
external. `--no-audio` selects `Luminumbra::Client::NullAudioManager`. Normal
startup loads the metadata manifests, while absent external event files degrade
to silence without preventing the world from starting.

Audio coverage includes synthetic silent-WAV bank fixtures for manifest paths,
event references and species calls, mixer/environment math, volume readback math,
and physics-backed occlusion tests. These tests do not establish the fidelity of
external recordings, device playback or a complete reverb implementation.

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

Avatar state contributes to the `entities` sub-hash. Opt-in creature and plant
rosters exercise simulation and persistence through separate ecology and plant
sub-hashes in the composite world hash.

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

### Environment controls

These are the `LUMIN_*` controls read by the current client/render source. Set
them before starting the process. They affect presentation or diagnostics, not
authoritative simulation hashes. The similarly named `LUMINUMBRA_ENABLE_*`
variables in CMake are build options, not these runtime environment controls.

| Variable | Default and accepted behavior | Read timing |
|---|---|---|
| `LUMIN_RENDER_SCALE` | Uses `user.render_scale` (default `1.0`). A positive `strtof` value overrides it and is clamped to `[0.5, 1.0]`; empty, nonnumeric or nonpositive values leave the configured scale. | Each pipeline startup, after config seeding |
| `LUMIN_TREE_IMPOSTORS` | Unset enables atlas baking and far-tree impostors. Empty or any value beginning with `0` disables; every other nonempty value enables. A failed atlas bake leaves them disabled. | Each pipeline startup |
| `LUMIN_CLOUD_QUALITY` | Client default `2`: quarter resolution per axis. `0` = full, `1` = half, `2` = quarter. Parsed with `atoi`, clamped to `[0, 2]`; empty/nonnumeric becomes `0`. | Client startup |
| `LUMIN_SSAO_QUALITY` | Client default `3`: half-resolution GTAO High with depth-aware upsampling. `0` = 64-sample hemisphere SSAO, `1` = full-resolution GTAO Low (8 samples), `2` = full-resolution GTAO High (18 samples), `3` = half-resolution High. `atoi`, clamped to `[0, 3]`; empty/nonnumeric becomes `0`. | Client startup |
| `LUMIN_ATMOS` | `density,maxDistance,inscatterStrength,warmth`; all four floats must parse to override the current atmosphere. Initial defaults are `0.00045,3000,60,1`. | Once per process on first aerial-context use |
| `LUMIN_GRADE` | `exposure,saturation,contrast,warmR,warmG,warmB`; defaults `1.12,1.30,1.42,1.06,1.0,0.92`. A parsed prefix replaces those fields; remaining fields retain defaults. A positive render-context exposure takes precedence over the grade exposure. | Once per process on first lighting use |
| `LUMIN_MOON` | No forced illumination by default. A nonnegative float overrides moon illumination, clamped to `[0, 1]`. Negative or unparseable input falls back to the pipeline override or lunar cycle. | Once per process on first celestial update |
| `LUMIN_MOON_WRAP_FLOOR` | Float, default `0.25`; parse failure retains the default. The C++ environment parser does not clamp it. | Once per process on first lighting use |
| `LUMIN_CAVE_AO` | `enabled,maxDist,floor,steps,thickness`; defaults `0,24,0.06,8,1.5` (off). Parses a float prefix with an integer fourth field; unparsed fields retain defaults. This is screen-space cave/sky-visibility AO, independent of terrain cave generation. | Once per process on first lighting use |
| `LUMIN_SCENT_DECAL` | Off when absent. Presence enables the scent ground decal, including an empty value or `0`; requires scent data to display. | Cached on first scent-decal initialization |
| `LUMIN_FRAME_SCAN_SETTLE` | Frame-scan capture default `90` frames. A positive `atoi` result overrides the settle target; other values leave it unchanged. This diagnostic override is not set by the gates. | Client argument/capture setup |
| `LUMIN_GL_DEBUG` | Off if absent, empty or exactly `0`. Other values install synchronous GL debug output when KHR_debug entry points exist; exactly `verbose` also enables notification messages (low/medium warnings are already enabled). | Callback installation; subsequent calls return immediately once installed |
| `LUMIN_RHI` | Selector default `gl`; case-insensitive `gl`/`opengl`, `vulkan`/`vk`. Empty or unknown values select GL. Exposed by the RHI selector helper; the shipping client does not call it and it does not switch its renderer. | Each `SelectedBackendFromEnv()` call; no static cache |

Numeric parsers accept numeric prefixes rather than requiring the entire value
to match. Presence is preserved by `Core::ReadEnvironment`: an empty value and
an absent variable differ where noted above. Updating the environment after a
cached read does not retune a running process.

High-resolution water is instead the supported `sim.water_high_res` configuration
flag, default off: Medium uses 8×8 cells per 16 m chunk (2 m cells), High uses
16×16 (1 m cells). It changes hashed water state and must match on every peer.
`GameSession` applies the chosen resolution at world creation/load; the underlying
solver also supports amortized grid resizing. See the [water limits](known-limitations.md).

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
