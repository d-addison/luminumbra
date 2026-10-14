# Effective simulation configuration identity

`GameSession::SystemConfigIdentity()` computes identity on demand from the session's
current simulation gates and tuning. It works without `ApplySystemConfig`, using the
compiled session defaults, and observes subsequent public tuning setters. Peers compare
this identity before TCP lockstep join or TCP remote replication admission; replay playback
compares the recorded identity with the live session before stepping any replay tick.
Missing identities on either side always fail compatibility, including two empty strings.

The fields are the simulation keys and owned parameters in `ConfigSchema.json`, plus
`render.circadian` amplitude, which reaches `RunCircadianOnTick`, and the session options
for finite hydrology, weather rain scale and region scheduler configuration. The five
registry gates are hydrology weather, weather events, aether state, water resolution and
active regions.
Other key slots are normalized to enabled: their config flags select compiled fallback
tuning rather than disable the session system; erosion has no session gate. Disabling
such an override with default tuning therefore leaves the effective identity unchanged.
Parameters come from current session storage, including clamped plant mutation and the
integer reproduction counts resolved by `ApplySystemConfig`. Scheduler values come
directly from the live active-region ledger's effective `config()`. Presentation and user
settings are excluded. This contract covers effective session rules, rather than world
content, initial rosters or per-tick inputs.

Canonical bytes start with ASCII `system-config:v3:`. In generated schema order, each
included key emits its `SysKey` id as u8, then its effective enabled flag as u8. Each owned
parameter emits its `SysParam` id as u8, then a scalar type tag and little-endian value:

- Tag 0: IEEE binary32 for ordinary tuning floats.
- Tag 1: three IEEE binary32 components for vector parameters.
- Tag 2: IEEE binary64 for the three foraging values.
- Tag 3: u32 for reproduction maturity, cooldown and courtship tick counts.

After the registry slots, v3 appends these fixed-width little-endian values without
padding: finite hydrology (u8, 0 or 1), rain scale (i32, modulo-2^32 representation), region
work limit (u64), hold ticks (u32) and reduced cadence shift (u8). All five values are
included even when their associated feature is disabled. No float conversion loses
adjacent integer values. The defaults append `0119000000ffffffffffffffff1e00000001`.
The domain version changes so older identities cannot be mistaken for this encoding.
This configuration encoding is independent of the resolved preset hash below.

The registry field order and scalar encoding from v2 are preserved. Exact foraging doubles
and reproduction integers would lose adjacent values if converted to binary32. Signed
zeros remain distinct. FNV-1a uses offset
14695981039346656037 and prime 1099511628211, with unsigned 64-bit wraparound. The result
is 16 lowercase hex digits emitted without locale formatting. Shipped defaults serialize
to **297 bytes → `60ab3c242d54ce89`**. The independent stdlib computation in
`test/tools/test_system_config_identity.py` resolves JSON inputs through binary32 as
SystemConfig does, applies effective fallback/clamping/count conversion, encodes with
`struct.pack('<f' / '<d' / '<I', value)`, and folds each byte as
`h = ((h ^ byte) * 1099511628211) & ((1 << 64) - 1)`.

## Setter audit and pins

`GameSession` supplies the current finite-hydrology boolean, signed rain scale, and all
three live ledger `RegionSchedulerConfig` fields to the core encoder. The scheduler's work limit,
hold ticks and reduced shift affect which regions tick or freeze; the server can override
them through `--sim-budget-profile`. Runtime Hello construction and replay boot apply the
same scheduler override as the runner. High-resolution water, weather rain/events,
active regions and aether state gates were already covered, as were every ecology,
foliage, thirst, scavenging, foraging, reproduction, plant mutation and circadian tuning
setter. Water resolution wiring selects fixed Medium/High solver settings from its hashed
gate. Erosion has no GameSession setter or effective SystemConfig gate; terrain erosion
parameters come from the world preset, outside this rules identity.

Deliberate exclusions from the setter audit:

- Moonlight, tree wind, plant rendering, TAAU, sky LUT, live-weather visuals, aether-tap
  visuals, snow cover, far-world ladder and user video/audio/controls are presentation.
  Circadian amplitude remains included because simulation consumes it.
- Creature/foraging spawn controls and timelapse rain/evaporation demos belong to client
  scenario setup/capture. They can create or mutate world state, but are not GameSession
  rule setters used by the admitted headless peers. They remain client-only workloads.
- Spawn points, local player positions, replicated anchors, edits and region pins are
  mutable world state or simulation inputs, not configuration; their state follows the
  existing world/region hash and input contracts.
- Budget telemetry and job-worker selection are observation/scheduling only. Root paths
  locate content, and required-client-asset lists validate client files; neither is a
  simulation rule value. Content identity remains outside this change.

The shipping pins in `SystemConfig_test.cpp`, `GameSessionHeadlessWorld_test.cpp`, this
document and the independent Python encoder change from `da24cea9dbc5159b` to
`60ab3c242d54ce89` because of the v3 domain and 18 appended bytes. Hardening tests' Hello
and desync-record identity literals use the same new shipping value; their codec and
record assertions are unchanged. `historical-defaults-off.json` contains configuration,
not an identity pin, and needs no edit: its effective identity is now
`74f285c5fa387d66`, with the independent session options still at their defaults.
The identity encoding change updates no world-state, terrain, water or replay checkpoint pins.

The scheduler load correction changes no identity pins: compiled/default schedulers
still encode the same bytes, and setter-only nondefault identities already encode their
effective values. Sessions loading a nondefault persisted ledger now identify that
ledger's settings instead of stale defaults; sessions with an absent ledger keep their
configured scheduler. These effective identities are compared against independently
configured sessions in regressions rather than assigned new hardcoded pins.
Preserving an override on an absent-ledger load can change nondefault scheduling and its
derived world/replay hashes compared with the previous default-substitution bug; no pins
for that incorrect behavior are retained or introduced.

Lockstep protocol and LREC1 layout versions are 3. Hello and replay headers carry
length-prefixed configuration identity and `preset_content_hash` strings. Versions 1
and 2 are explicitly rejected because they cannot bind resolved preset content.
Desync replay dumps include both identities captured at handshake. TCP (including
runtime/soak modes) hosts validate the same Hello compatibility fields before adding a
replication client. TCP joiners send Hello before user commands. TCP lockstep and replay
enforce both identities before simulation starts.

`GameSession::PresetContentHash()` captures the preset used at world creation or load.
Its canonical form first materializes revision-dependent defaults, then stamps the
current schema revision (the same operation used for embedded presets). It serializes
the resolved JSON with sorted object keys, no whitespace, ASCII escapes and nlohmann's
locale-independent binary64 number serialization. Runtime-resolved absolute asset paths
and diagnostic provenance are excluded. FNV-1a-64 over these ASCII bytes uses offset
`14695981039346656037`, prime `1099511628211`, modulo-2^64 arithmetic and exactly 16
lowercase hex digits, encoded without locale-sensitive formatting. Equivalent key order
and whitespace produce identical bytes and hashes; a changed terrain override under the
same preset name is incompatible. The older numeric replay `preset_hash` remains name
provenance only; admission and verification require `preset_content_hash` as well.

The GNS and Steam transports are optional and disabled by default. Both use the
unconditionally compiled `Luminumbra::Net` replication admission gate from issue #278.
A connected joiner sends the existing v3 Hello reliably. The host checks its assigned
client ID and `HelloMatches` compatibility (protocol, seed, preset, tick rate, nonempty
SystemConfig identity and resolved preset content hash). Host identities come from the
booted runner session, replacing the two identity fields constructed by
`RuntimeHelloFrom(options)`. GNS retains its port-to-client-slot mapping; Steam retains
one remote slot, client ID 1.

The host's reliable Hello with `client_id = 0` is the admission acknowledgement.
Successful enqueueing admits the host slot and permits `AddClient`; the joiner admits
only after validating the acknowledgement. Until then the adapter suppresses command
and snapshot-ack sends and discards snapshots without updating reconstruction history.
Commands arriving before Hello are discarded without disconnecting or extending the
deadline. Unreliable Hello or acknowledgement frames are rejected. Repeated compatible
Hello frames are consumed as control and may resend the acknowledgement without
registering another client. Malformed control, incompatible identity, send failure,
disconnect and deadline expiry close with specific diagnostic reasons. Received delivery
flags are retained before releasing each sockets message.

`kAdmissionTimeoutTicks = 150` uses elapsed `steady_clock` time at the local 30 Hz
handshake cadence: 150 ticks / approximately 5 seconds, independent of simulation
progress and callback or replication loop iteration counts. The host starts at
connection establishment; the joiner starts when its Hello is successfully enqueued.
Expiry is at elapsed tick `>= 150`. Each admission pump drains at most 32 frames, so
continuous traffic cannot postpone deadline checks. The connection-establishment limit
remains separately 30 seconds. The separate runtime TCP policy
`kRuntimeTcpHelloTimeoutTicks` introduced by issue #315 is also 150 ticks / approximately
5 seconds; the constants remain separately named. This optional-transport gate does not
alter TCP admission or add a TCP acknowledgement.

Hello and user commands share tag `0x01`. Admission validates the complete outer frame
length first: exactly `kUsercmdPayloadBytes = 19` payload bytes (24 total) identifies a
command, even when its tick starts with `LSTP1`. Other frames with this tag must carry
the Hello magic and satisfy `DecodeHello(frame, hello) && EncodeHello(hello) == frame`.
Failed Hello decoding never falls back to command decoding. The general Hello decoder
keeps its existing permissive TCP/lockstep behavior. `DecodeUsercmd` now rejects trailing
extensions, including those whose outer length was adjusted to include the extra bytes.
All valid encoder output is unchanged: lockstep protocol 3, replication protocol 1 and
LREC1 layout 3 remain unchanged, as do TCP wire bytes, replay and hash layouts. Hello
versions 1 and 2 remain rejected. Older optional joiners cannot admit through commands;
new joiners discard older hosts' snapshots until acknowledgement timeout.

The command-length compatibility audit found one production command encoder,
`EncodeUsercmd`, writing exactly `u64 tick + u32 player_id + 3 * i16 + u8 = 19` bytes,
with no extension path. `ReplicationClient::SendUsercmd` sends those bytes unchanged;
`ReplicationServer::PumpInbound` is the production replication decoder. TCP replication
uses these same endpoints. TCP lockstep instead encodes `InputMsg` through
`EncodeInput`/`DecodeInput` with a separately length-prefixed opaque blob. Desync dumps
write those merged blobs through `ReplayWriter::RecordInput`; replay streams use
`RecordInput`/`ReadReplay`, never `DecodeUsercmd`. Headless recordings use empty input
blobs. Replay regression fixtures generate empty or arbitrary opaque blobs with those
replay APIs; smoke artifact fixtures contain options and result/hash metadata, not encoded
replication commands. No checked-in binary replay/command stream or alternate command
encoder was found. Thus no existing valid command stream requires trailing bytes; opaque
lockstep/replay blobs keep their existing independent length contracts.

Admission regressions run in `common_tests` with both SDKs disabled, using explicit
ticks and scripted paired transports that preserve delivery flags and can suppress a
reliable acknowledgement. Ordinary `NetworkSim` packet loss does not drop reliable
frames and cannot exercise that timeout. The GNS compile probe builds both
`luminumbra_common` and `luminumbra_server_app`, and checks the shared sockets and GNS
mode objects. No CI job enables Steam; its guarded modes still require an SDK-equipped
Windows preview build. SDK declarations and mode implementations remain guarded when
their CMake options are off.

Saves carry no simulation identity. Loading applies the loading process's gates and
tuning; an active-region ledger also persists its own scheduler configuration and cadence
state. With no explicit scheduler override, a loaded ledger's settings become effective.
An explicit `SetRegionSchedulerConfig` override (including an explicit default, or
`--sim-budget-profile` scheduler settings) must match all three persisted fields; a
conflict refuses world loading with `Incompatible configuration: persisted region
scheduler differs from explicit override.` It does not install the conflicting ledger or write
the save. A matching override restores the ledger intact. Refusal preserves the saved
cadence/hold history rather than reinterpreting that state under different rules.

When the ledger file is absent, loading starts an empty ledger at the restored clock
with the currently configured scheduler, never compiled defaults substituted for an
override. Every session ledger replacement synchronizes the retained configuration;
creating another persistent or transient world clears region state while keeping those
effective settings. Identity always reads the live ledger after boot/load, so admission
and replay reject differing effective schedulers before advancing a tick. Existing
clock/configuration checks retain their behavior; save and replay layouts are unchanged.

The per-tick world hash, replay start/checkpoint comparisons and simulation-state goldens
remain unchanged. Replay wire bytes change only through the versioned header. Native
verification must exercise TCP lockstep, replay and effective setter tests, existing
save/open regressions, and the established determinism gates.

Both runtime executables accept `--systems-config <path>` for an explicit SystemConfig
JSON override and `--preset-json <path>` for a validated new-world terrain preset.
Paths are relative to the working directory. Missing or invalid overrides fail before
world creation. Server smoke, recording, replay, and lockstep share the resolved
configuration; the client networked smoke also supplies it to its host authority.
Replay and Hello identities still come from `GameSession::SystemConfigIdentity()`.
Record and replay must receive the same overrides.

Historical determinism pins use `test/fixtures/historical-defaults-off.json` together
with `worlds/atlas/presets-rev6/default.json`. The fixture explicitly disables every
simulation flag and retains the historical render settings. The ladder is render-only
and excluded from the world hash. The frozen preset also preserves the disabled island
mask; disabling simulation flags alone would not restore historical terrain. Shipping
defaults remain exercised by the unpinned headless, moving, and water trace workloads.
Capturing shipping-default absolute hashes still requires native runs.
