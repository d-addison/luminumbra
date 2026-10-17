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
| Terrain preset | New presets use integer `schema_rev: 8` with sampling revision `2`; revision `6` and omitted revisions retain historical missing-field semantics |

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
Hydraulic/thermal relief defaults on in revision 7; explicit
`terrain.hydro.enabled: false` disables it. Revision 6 retains historical defaults.
Biome tables, climate frequencies and biome relief retain their controls. The
[SDF contract](shield/sdf-contract.md) describes their sampling semantics.

New saves embed resolved choices in `preset.json` and write `presetFormat: 1`
and `presetIdentity` in `world_info.json`. The identity is `StableChecksum` (FNV-1a)
of the canonical preset JSON (`nlohmann::json::parse(...).dump()`), independent of
whitespace and distinct from the runtime identity that includes dependencies.
A marked save refuses a missing embedded preset, a missing or mismatched identity,
or an unsupported preset format; it never substitutes packaged content. Refusal
does not modify files. Only legacy saves without the marker and embedded preset
may fall back: the eight packaged names resolve to frozen revision-6 copies in
`worlds/atlas/presets-rev6/`, and other names retain their original atlas path.
Any recorded legacy identity is still verified. Resaving an opened legacy world
embeds its exact preset and records the marker and identity. This is preset
fallback within the current container format, not obsolete-world migration.

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

### Active-region ledger

The enabled host persists `chunks/region/active-regions.arl` through the same
atomic replacement primitive as region containers. Its complete ARL1 version-1
byte layout, allocation limits, checksum, absent/corrupt/future refusal rules,
activation inputs, scheduler ordering, ecology projection and replay companion
are specified in [Active-region ledger and scheduler](active-regions.md).
No measured caps or distant system consumers are activated by this foundation.
The regional plants/water continuation stack (issue #242) is planned for v0.3.1;
performance/default enablement remains v0.4.0 (issue #320).

### Simulation clock metadata

The hashed `sim.active_regions` key is enabled by default for v0.3.0. When enabled, every
successful persistent `GameSession` save writes three additional keys in
`world_info.json` beside `waterSimCursor`, through `WorldSaveService`'s existing
temporary-file and atomic replacement path:

```json
"simulationTick": 317,
"calendar": { "dayLengthTicks": 216000, "daysPerYear": 8 },
"ambientFieldAnchor": { "x": 8.0, "y": 100.0, "z": 8.0 }
```

`simulationTick` is the absolute completed host tick; there is no wall-clock
progression while a world is closed. Tick zero is midnight on day zero in
midwinter. At 30 Hz the default day lasts two hours (about one hour of daylight) and
the eight-day year lasts 16 hours. Plants and circadian activity use the midnight
phase directly; migration, stimulus seasons and the sky's seasonal sine offset the year by three quarters
to their spring-equinoctial origin. Sky geometry offsets the day by half a cycle
because its existing zero is noon. An explicit render day-length override and
photo/scenario time pins remain presentation controls.

| Metadata condition | Open / write behavior |
|---|---|
| `simulationTick` absent | Tick zero |
| `calendar` absent | Pinned defaults: 216,000 ticks/day, 8 days/year |
| `simulationTick` present | JSON unsigned integer in `[0, 2^62)` |
| `calendar` present | Object containing exactly `dayLengthTicks` and `daysPerYear` |
| `dayLengthTicks` | JSON integer in `[1, 2^31)` |
| `daysPerYear` | JSON integer in `[1, 366]` |
| `ambientFieldAnchor` present in a clock save | Object containing finite numeric `x`, `y`, `z` coordinates |
| `ambientFieldAnchor` absent | Use the saved spawn point, or the initialized spawn if metadata has none |
| Invalid clock type, null, negative/out-of-range clock number, fractional/exponent clock number, incomplete calendar, unknown calendar member or arithmetic overflow | Corrupt world metadata; refuse before generation or writing |
| Either clock key present with `sim.active_regions` off | Incompatible configuration; refuse without modifying disk bytes |

Day × year and tick + the catch-up bound are checked before use. Validation
also applies to initialized saves with no chunk directory, catalog inspection,
explicit snapshot directories, and saves over existing metadata. The catalog
reports valid clock metadata without selecting a runtime configuration; opening
checks the feature requirement. Presence of either clock key marks the save as
requiring `sim.active_regions`; metadata writes cannot remove both keys from an
initialized clock save. Save calls inside a simulation catch-up batch are
refused; successful snapshots observe a completed tick boundary.
Enabled transient sessions can read explicit snapshots, but saving them is
refused before creating a directory or writing any member, just as their
canonical saves are refused.

Saves with an explicit calendar retain it, including older 36,000-tick days.
Saves without the calendar key adopt the 216,000-tick default; no migration is
needed to load them.

With the key off, saves omit all three keys and loads restart at zero; simulation
consumers retain their legacy periods and hash bytes. The renderer uses the shared
two-hour default day even on the legacy path. With it on, clock canonical bytes fold
into the existing ecology hash slot without changing the top-level hash order.
The canonical clock section is ASCII `world_clock:v1:` followed by little-endian
`u64 simulationTick`, `u32 dayLengthTicks`, and `u32 daysPerYear` (16 payload
bytes). It excludes frame accumulators and telemetry. Calendar and tick changes
therefore affect the hash even when the entity roster is empty.

C1 persists the ambient grid's anchor separately from the player respawn
`spawnPoint`. It is initialized from the spawn when creating or importing a
world and stays fixed for live wind, weather and aether updates. Client quit
and shutdown saves can therefore change `spawnPoint` without moving these
fields or their surviving storms. Loads reconstruct wind and aether ambience
at the restored tick and weather over at most its 240-tick storm lifetime,
using the persisted `ambientFieldAnchor`. In the key-on path, storms move using
base wind; their current gusts then perturb the
authoritative wind consumed by aether, plants and other systems. This removes
storm-to-wind-to-storm motion feedback from that path, so expired storms cannot
carry unbounded history into later storms. The key-off update order and feedback
are unchanged. This is a bounded C1 bridge: it does not provide ambient coverage
following the player at distance. C4 replaces the fixed grids with world-anchored
pages, regional storm records and activation history. Older clock snapshots
without an anchor use their saved spawn; a historical anchor already lost by
such a snapshot cannot be recovered. The snapshot commit index, rollback
journal, and multi-file recovery belong to C2/C3. C1 uses the existing metadata write path and is not a
claim of atomic publication across chunk, entity, energy and metadata files.
The shipping clock/ledger foundation is enabled by default. Indexed authority
durability/rehydration, durable completed work and ground-object continuation,
durable regional fields/weather/scent/energy, and regional foliage/water
continuation are planned for v0.3.1. Five-minute dedicated-server autosave does
not implement those persistence slices. LMR1 v2,
world_manifest.v1, FSD2 v3, and existing canonical chunk and entity serialization
are unchanged; preset revision 7 retains revision 6 omission compatibility.
The additive metadata contract `presetFormat: 1` requires an embedded preset
and its canonical-JSON checksum. It changes neither the container version nor
the existing `StableChecksum` algorithm or runtime dependency-content hash.

An absent energy record restores an empty layer with its next firing on the
next absolute eight-tick boundary. A legacy import may carry a different
cadence phase even after all its energy decays: enabled saves preserve that
phase in a header-only `EFS1` record. Empty layers with the aligned phase omit
the record and remove any stale prior record. This keeps the existing EFS1
format and empty-neutral hash bytes; C4 moves cadence ownership into the ledger
and restores the contract's all-zero record omission there.

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

Legacy far snapshots track world/region authority publication separately from
each captured chunk's voxel revision and provenance. A direct edit to a resident
chunk's SDF followed by `mark_voxel_data_dirty()` advances the chunk revision and
stales an earlier capture without advancing the publication counters. Shrinking
or growing the graphics horizon changes derived mesh admission, preserving
edited authority and rejection of stale captures.

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

Shipping simulation features are enabled by default. Explicitly disabled features
retain the empty-neutral contract:
when disabled or without participants they allocate no authoritative state,
write no save records, and contribute no hash bytes. This keeps disabled
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

TCP admission, lockstep and replay compare effective simulation configuration and
resolved preset-content identities before admitting state or stepping ticks. The
content identity binds canonical preset JSON and a sorted manifest of exact biome
and structure dependency bytes; generation uses that immutable snapshot. Runtime
and soak TCP slots release peers that fail to complete a matching Hello within
10 seconds (issue #315). The shared networking Hello is version 4; LREC1 replay
uses version 3 for complete streams and version 4 for diagnostic suffixes with
an elided prefix. Readers accept both; older Hello layouts are refused. See the
[identity contract](development/system-config-identity.md) for the effective
SystemConfig/presentation split, refusal rules and dependency encoding.

Lockstep is a direct two-peer protocol: Hello must match the configured remote
client slot, and a peer can submit only its own inputs. Hello exchanges start,
minimum and maximum horizons, checkpoint cadence, transport frame size, and
retention budget settings. Local and received settings must satisfy
`0 < horizon_min <= horizon_start <= horizon_max` before any input scheduling.
Both sides must use identical horizon, cadence and budget settings; incompatibility
fails before input scheduling with an explicit `handshake_error`. The frame size is the minimum of both transports and each side's
`max_frame_payload_bytes` setting; callbacks can inspect the result through the
const `AdmissionStatus().frame_limit` diagnostic.
Inputs must be newer than the consumed tick and no more than twice the maximum
configured horizon ahead. Hashes must be cadence checkpoints within that lookahead
and newer than the contiguous confirmed checkpoint prefix; delayed hashes for executed ticks still participate in desync detection.

Retention is derived, rather than dropping reliable messages when a container
fills. Let `L = 2 * horizon_max`, `C = ceil(L / cadence)` and
`W = kReceiveQueueFrameLimit + C + 1` (with no hash slots when cadence is zero).
The sender pauses simulation after `W` unconfirmed checkpoints while continuing
to receive; only the oldest unconfirmed tick advances the confirmation prefix.
The receiver reserves `L` input slots and `W + C` hash slots, including remote
lead and the next checkpoint needed for confirmation regardless of arrival order.
Multiplying these slots by the negotiated maximum frame size gives the encoded
byte bound; overflow is rejected before scheduling. The single remote's per-peer
and aggregate bounds are identical. Zero `peer_retained_byte_limit` and
`peer_retained_message_limit` select these derived bounds; explicit budgets must
fit all negotiated traffic or handshake fails. A tick within the negotiated
window is never discarded for capacity. Invalid slot, stale/future, malformed,
conflicting or oversized messages are dropped and counted in session status.
These rules apply above Loopback, TCP (Windows/POSIX), GNS and Steam transports.

Each `Handshake`, `PumpTick` or `Disconnect` call owns one receive-work budget,
shared with nested input/hash sends, retry refusals and closure drains. It permits
at most 1024 receive polls, including empty or malformed frames and unsuccessful
polls, and stops polling once delivered payload bytes reach 256 KiB. One final
frame can cross the byte threshold; its payload is bounded by the existing
64 MiB transport maximum. Transport queues and read-ahead retain their existing
bounds. Saturation leaves subsequent frames queued and allows an available
agreed tick to advance; it does not change input bytes, ordering or hashes.
`Finished` waits for an empty receive poll. A saturated local disconnect stops
simulation immediately and defers transport close and storage release; its owner
keeps the session and transport alive and calls `Disconnect` or `PumpTick` again
until status becomes terminal. A finite backlog drains over successive calls.
A continuously refilling peer can prolong draining, but cannot monopolize one
call or grow retained storage beyond the negotiated bounds.

Replication server/client public receive and send calls share the same 1024-poll
and 256 KiB threshold, including nested refusal and close drains. Admission
control frames consume that budget; a control-batch yield is distinguished from
an empty source. Closing links and transports stay alive until draining finishes,
so queued commands, ACKs and authoritative snapshots precede terminal close.
Server receive pumping rotates its first client after each call. Transport send
and receive capacities may differ; receive validation and snapshot reconstruction
use the receive capacity, and adapters forward both caps without raising either.
The loopback scenario driver uses fixed default horizons of 3–10 ticks and empty
input payloads, with no configurable backlog injection. Its clean-disconnect
artifact requires both session ends to be terminal and free of desync, including
checkpoints processed during disconnect. Registered regressions require native
execution before qualification is claimed.

On `Bye(T)`, the survivor compares queued hashes and drains buffered inputs only
through the sender's final executed tick T. Pre-scheduled inputs after T are
then discarded. Pending outbound retries cannot cut this drain short; closure
is reported before retained input/hash storage is released.

Consumed inputs and confirmed peer hashes are pruned. An out-of-order hash that
already matches its local checkpoint releases all peer strings and retains only
a confirmation marker. Local hashes retain only
unconfirmed checkpoints plus the latest `kReceiveQueueFrameLimit` diagnostic
ticks. Applied input diagnostics retain at most `kReceiveQueueFrameLimit` ticks
and `kMaxFramePayloadBytes` payload bytes, using a deque. Storage therefore depends
on the negotiated limits rather than session duration. The only internal history
consumer is desync export; save-game state does not consume this history.
Every `LockstepSession` automatically streams merged inputs and local checkpoints
to a `QueuedReplayRecorder` backed by `BoundedReplayRecorder`, including the
server lockstep mode, client network driver, and scenario/test drivers. No hook
installation is required. Handshake performs recorder setup and starts one
worker through the existing `JobSystem`. Each tick only enqueues shared references
to immutable, already-encoded merged inputs and local checkpoint strings; it
performs no recorder I/O, replay encoding, or digest scans. A single ordered
consumer performs those operations, including segment rotation and pruning.
Recording uses buffered disk segments beside the dump destination; no per-tick
flush is performed. Segments hold at most 1024 ticks or 64 MiB, with a larger individual
tick occupying its own segment. Confirmed segments rotate once the retained
suffix exceeds 1024 ticks or 64 MiB. All unconfirmed ticks remain pinned, so even
a delayed mismatch retains the divergent tick and its input bytes. Each segment
is closed before deletion, including on Windows. Destruction or completed clean
disconnection removes the working segments; an in-sync session emits no dump.

Outstanding recorder work, including the active tick, is limited to 4096 ticks
and `max(64 MiB, T)` bytes, plus count-bounded queue metadata. Byte accounting
charges retained input/string capacities as well as encoded sizes. Queue admission
reuses preallocated ring slots, takes constant time, and does not wait for the sink.
On overload it latches the first
failure and stops accepting further ticks. Accepted ticks still drain in order;
the recorder never silently drops or resumes a gapped recording. Desync
finalization returns failure and writes `<dump destination>.incomplete` with the
first failed tick and a clear reason. Sink failures use the same policy. Recorder
progress and failure never change simulation outcomes, input scheduling, hashes,
or session pacing. `LockstepStatus::replay_error` reports a detected recorder
failure without performing tick-path logging. `FlushReplayRecorder` is an explicit blocking persistence
barrier; it is not part of `PumpTick`. Desync finalization and shutdown drain all
accepted work before finalizing or removing working files. Complete artifacts
keep the existing paths, wire bytes, checkpoint cutoff, and incremental FNV-1a
prefix digest. Clean shutdown publishes no dump or incomplete companion.

The disk bound is checked at handshake with overflow-safe arithmetic. With
negotiated maximum frame size `F`, maximum horizon `H`, checkpoint cadence `D`,
and the checkpoint window `W` above, define `T = 3F + 46` (maximum encoded tick),
`U = WD + 2H + D` (zero when hashes are disabled), and
`M = max(64 MiB, T)`. Working segments retain at most `B = UT + 3M` bytes,
independent of session duration. Actual usage follows retained record sizes;
the bound is not preallocated. Finalization streams through a fixed 64 KiB
buffer. The final artifact is bounded by `B + 64 MiB + T + 53` bytes including
the header, prefix metadata, and trailer; temporary disk usage while finalizing
is at most `2B + 64 MiB + T + 53`. I/O failures are logged and never change
simulation or hash results; a failed artifact is never reported as successful.

On desync, the recorder finalizes LREC1 at the configured dump destination and
populates the existing `LockstepStatus::dump_path` (the server's peer destination
still appends `.peer`). It keeps every retained input and the local checkpoints
through the divergence. When the prefix has rotated away, the header is version 4 and an `ElidedPrefix`
record contains its last tick and a rolling FNV-1a digest of its exact encoded
record bytes. `ReadReplay` exposes `elided_through_tick` and
`elided_prefix_digest`; these artifacts support diagnostics, and `--replay`
explicitly refuses to replay them from the seed. Recordings with no elided
prefix keep the byte-identical version-3 LREC1 contents. The optional
`LockstepHooks::record_replay_tick` remains available for additional bounded,
caller-owned recording; the buffering `ReplayWriter` must not be used as an
unbounded full-session sink.

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

Scene depth uses `GL_DEPTH_COMPONENT32F` with reversed-Z and
`glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`: the 0.1 m near plane maps to 1,
the 3200 m far plane and cleared sky map to 0, and visible geometry wins with
`GL_GREATER` (or `GL_GEQUAL` for equal-depth overlays). Depth reconstruction
uses the sampled value directly as clip Z. Camera frustum extraction uses
`row3 - row2` for the near plane and `row2` for the far plane.
Shadow cascades retain conventional orthographic depth, clear to 1, and use
`GL_LESS`; ShadowPass temporarily selects `GL_NEGATIVE_ONE_TO_ONE` and restores
the scene convention afterward. Shadow-coordinate sampling remains unchanged.

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

The far-world requested mode defaults to Ladder, including when systems.json is
missing or the flag is absent. For an explicit Legacy opt-out, set
`render.far_world_ladder.enabled` to `false` in `data/common/systems.json` and
restart the client. The per-user settings overlay only reads `user.*`; this flag
has no environment or CLI override. Effective selection still falls back to
Legacy when Ladder cannot bind, with requested/effective mode and refusals in
the debug HUD and diagnostics. See [Far world horizon v2](far-world-horizon-v2.md)
for the config example and qualification gates. Saved-world far production
authority remains fixture-only until FW S8 (v0.3.1); the renderer reports an
explicit unavailable/refusal diagnostic.

### Developer console

The ordinary and QA clients provide the same developer console in Release and
other configurations. F1 opens its searchable widget hub by default; ordinary
Settings provides console access and the default-on performance HUD switch.
Visibility, resolved shortcuts and interactive panel layouts persist under
`user.developer` without changing simulation hashes. The Controls settings section
links to the hub for developer rebinding and hotkey help. Generated help includes
current developer and gameplay bindings. Interactive panels share input/cursor
ownership; passive HUDs do not capture gameplay. Opening the console does not
pause simulation.

| Default hotkey | Module / action |
|---|---|
| F1 | Open or close the developer hub |
| F3 | GPU profiler panel |
| F5 | Reload all shaders (`shader.reload`, no panel) |
| F6 | Cycle debug views (`render.debug-cycle`) |
| F7 | World generation preview panel |
| F8 | Client settings panel |
| F9 | Toggle wireframe effect (`render.wireframe-toggle`) |
| F10 | Shader panel |
| `[` | Slow time (`time.slower`) |
| `]` | Speed up time (`time.faster`) |
| `\` | Reset time scale (`time.reset`) |

The hub shows every module's description and current hotkeys, including unbound
panels and passive overlays. Debug-view, wireframe and time-control panel visibility
is separate from their effect hotkeys. Photo mode uses `[` and `]` for aperture.
Developer actions and rebound panel keys work while a panel owns input; focused
text fields block them. `--no-ui`, automated captures and scenario runs suppress
developer overlays and shortcuts without modifying saved preferences.

See [Developer console](developer-console.md) for the complete inventory,
registration API, typed commands, focus rules, save retries and capture overrides.
The generation inspector edits an independent preview; only its explicit
**Apply & Regenerate** button can apply changes to an ordinary local world.

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
| `LUMIN_CAVE_AO` | `enabled,maxDist,floor,steps,thickness`; defaults `1,24,0.06,8,1.5` (on). Parses a float prefix with an integer fourth field; unparsed fields retain defaults. This is screen-space cave/sky-visibility AO, independent of terrain cave generation. | Once per process on first lighting use |
| `LUMIN_SCENT_DECAL` | On when absent. Exactly `0` disables the scent ground decal; other values enable it. Requires scent data to display. | Cached on first scent-decal initialization |
| `LUMIN_FRAME_SCAN_SETTLE` | Frame-scan capture default `90` frames. A positive `atoi` result overrides the settle target; other values leave it unchanged. This diagnostic override is not set by the gates. | Client argument/capture setup |
| `LUMIN_GL_DEBUG` | Off if absent, empty or exactly `0`. Other values install synchronous GL debug output when KHR_debug entry points exist; exactly `verbose` also enables notification messages (low/medium warnings are already enabled). | Callback installation; subsequent calls return immediately once installed |
| `LUMIN_RHI` | Selector default `gl`; case-insensitive `gl`/`opengl`, `vulkan`/`vk`. Empty or unknown values select GL. Exposed by the RHI selector helper; the shipping client does not call it and it does not switch its renderer. | Each `SelectedBackendFromEnv()` call; no static cache |

Numeric parsers accept numeric prefixes rather than requiring the entire value
to match. Presence is preserved by `Core::ReadEnvironment`: an empty value and
an absent variable differ where noted above. Updating the environment after a
cached read does not retune a running process.

High-resolution water is instead the supported `sim.water_high_res` configuration
flag, default on: explicit false selects Medium, with 8×8 cells per 16 m chunk (2 m cells); High uses
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

## Shipping defaults

All registered `sim.*` and `render.*` SystemConfig gates, including
`render.far_world_ladder`, are enabled in both the compiled mask and
`data/common/systems.json`. Missing or malformed enabled fields
retain defaults; an explicit boolean false clears its bit. Simulation tuning
selectors use the packaged tuning. `sim.erosion`, `render.moonlight` and
`render.tree_wind` are declared gates without runtime activation consumers;
turning them on does not add a new erosion pass, moon strength or wind system.
The config:v1 serializer is unchanged: the default sim sub-hash is now nonempty,
and an explicitly all-disabled sim configuration still has an empty sub-hash.
`sim.*` is hashed when enabled; `render.*` is excluded from this diagnostic sub-hash.
The separate effective session identity includes `render.circadian` amplitude
because simulation consumes it, plus session options and effective scheduler
settings. Presentation-only gates and `user.*` settings remain excluded. See the
[identity contract](development/system-config-identity.md).

Client and server apply the same GameSession resolver before Create/Load, including
active regions/absolute clock, High water, weather rain/events, stateful aether and
all simulation tuning. Finite hydrology is on, with existing rain scale 25 mm and
zero constant rain/evaporation rates. Explicit false water resolution selects
Medium; lower-level session setters can retain disabled compatibility workloads.
Diagnostic rosters and telemetry remain opt-in.

TCP Hello admission, the lockstep handshake and replay playback compare the same
`GameSession::PresetContentHash()` alongside the effective configuration identity.
The content hash binds the resolved preset and its biome/structure dependencies,
plus every immediate regular `data/common/creatures/species/*.json` file, including
inline `brain` and `genome_ranges` overrides (issue #338). Species apply even with
biomes and structures disabled. All dependencies use one manifest: sorted generic
UTF-8 logical paths with `/` separators and exact-byte FNV-1a-64 file hashes, folded
with the canonical preset JSON. Absolute runtime roots and clocks are excluded.
The loader captures the species registry from those same bytes and GameSession
adopts it on create/load; it never rereads species after computing the hash.
Malformed JSON warns and skips the entry while its bytes remain bound; present
unreadable files/directories refuse resolution without replacing a running world.
Directory-open, entry-status and iteration failures are reported by the registry
without escaping to client metadata callers; a failed scan adds no species. Logical
dependency names must be well-formed UTF-8, and an invalid species filename refuses
the scan before any reads. Resolver exceptions, including JSON serialization
failures, return diagnostics without publishing an identity or dependency snapshot;
failed create/load leaves the live world unchanged.

Hello v4 carries negotiated lockstep admission limits; complete LREC1 streams use
version 3 and elided diagnostic suffixes use version 4. Binding species does not
change those layouts. A species mismatch refuses TCP admission, lockstep handshake
and replay verification before simulation advances. Existing content-mismatch
diagnostics apply; lockstep and replay name
`preset/species` content. Older v0.3.0-era preset-only replay hashes mismatch when
species records are present, in either engine direction, following the existing
preset dependency compatibility rule. With no species directory, no new records
are added. Current-format saves store no session content identity and adopt the
loading installation's species content; the optional historical `presetIdentity`
check still checks frozen preset JSON only. Older world-format refusals and save
container versions are unchanged. See the full
[identity contract](development/system-config-identity.md).

Playback, vsync, TAAU, tier-1 froxel volumetrics, cave AO, scent decals and aether
material gain 1 are on. `--no-audio`, `--volumetric-quality 0`, `LUMIN_CAVE_AO=0`,
`LUMIN_SCENT_DECAL=0`, explicit config values and renderer setters remain available.
User overlays contain only `user.*` plus `settings_schema_version` (currently 1,
independent of world, save and protocol formats). Saved false vsync and saved
volumes win over defaults. `user.graphics` owns presentation settings: a
`base_preset`, its `preset_revision`, explicit `overrides` and optional detection
metadata. A file without a schema version migrates to Ultra plus its explicit
values, including `user.video.render_scale`. A malformed, unreadable or future
version file is reported as Recovery: defaults apply for the session and saves
are refused so the original file is never overwritten. Locations are
`%APPDATA%/Luminumbra/settings.json`, `$XDG_CONFIG_HOME/luminumbra/settings.json`,
or `$HOME/.config/luminumbra/settings.json`.

Settings → Graphics offers Low, Medium, High and Ultra, with Custom displayed when
overrides differ from the retained base. Reset overrides restores that base;
selecting a preset clears overrides. Controls show session-only CLI/environment
values and identify and disable those overridden controls. They never save those
temporary values. Graphics edits apply together on the render thread, invalidate
temporal history, and save after a 500 ms debounce. Save failures remain visible;
Save graphics retries, and leaving settings or exiting flushes pending changes.

Detect & Apply recollects the active GPU, logical CPU threads and available
dedicated VRAM, displays the recommendation, and applies it with overrides cleared.
`--detect-and-apply-graphics` performs the same explicit action at startup. Only
a first launch automatically selects and saves a recommendation. Existing and
migrated selections remain intact across GPU/driver changes. Recovery continues
to protect malformed or unsupported overlays from writes, including explicit saves.

Replication defaults to acknowledged deltas and radius-3 chunk AOI with a 16,000 mm
chunk edge. Millimeter AOI is an alternative: disable chunk AOI before selecting it.
Locomotion defaults use separation radius 2 m/strength 0.6, flock radius 12 m,
cohesion 0.5 and alignment 0.4. Server autosaves default to every 9000 ticks:
five minutes at the canonical 30 Hz clock, to avoid saving every second during
normal play. Smoke fixtures explicitly use 30 ticks to exercise saves at ticks
30/60/90; explicit zero suppresses autosaves, including recorder fixtures.

Preset revisions 7 and 8 resolve omitted island masks, surface breaks, biomes,
biome relief, cliffs, rivers, lakes, structures and hydraulic/thermal erosion on.
The biome table is `common/biomes.json`; structures resolve under the data root.
Revision 6 and unversioned presets retain their historical omitted-field choices.
Explicit false and an empty biome table remain off. New saves embed resolved
choices; neutral knobs and UI controls use the same versioned resolver. Existing
saved presets are read without rewriting their choices.

Schema 8 gates `generation_params.terrain.surface_sampling_revision: 2`
(issue #453). New worlds and the shipping catalog select revision 2, so activation
height samples and vertex materials use the complete heightmap shaping sequence.
Older readers reject schema 8. Explicit sampling revision 1 is preserved;
historical presets without the selector keep revision 1 and their original
identity bytes. Its canonical identity remains at schema 7. Erosion continues to
bake its historical substrate, preserving generated terrain in existing worlds.

The ordinary hills world is `worlds/atlas/presets/default.json`: its island mask
is now on, alongside its already-enabled terrain features. Amplified, archipelago,
caverns, cave-dense, mountains and temperate forest also enable their shipping
features.
`flat_lands` keeps its minimal identity with explicit opt-outs and structures on.
Vulkan RHI, Steam and GNS defaults remain unchanged, as do developer diagnostics.

`cave_dense.json` follows the Caverns revision-7 migration: island mask, biome
relief and cliffs are on, and hydraulic/thermal erosion uses four iterations
instead of eight, matching the retuned Caverns terrain. Its denser cave tuning
is preserved: cave threshold `0.54`, surface-break density `0.55`, maximum feature
radius `52` m. Its canonical preset identity changes from `e70158a4777ee36b`
(frozen revision 6) to `a9296ce03a5d9f33` (shipping revision 8). The frozen copy
has SHA-256 `5eff891c8d9f654ada480abf27b9cf9781cc86798fec60844bf28b1a7a4ae5e0`;
the shipping copy has SHA-256
`416c5a8d847c31e240fe3d103ed73e769131e3488a9ef1096076f01235d67a18`.
Existing saves without an embedded preset use the frozen copy. The game
repository's cave-dense qualification fixture and identity sidecar must select
the intended revision: retaining the old fixture matches the frozen bytes;
using shipping revision 8 requires updating both the fixture and its pins.

Current live sessions use authority-rules revision 4 with active regions enabled or disabled, binding the persisted raw water scheduler cursor to water and world hashes while retaining explicit legacy projections and refusing mixed-profile admission before ticks.
