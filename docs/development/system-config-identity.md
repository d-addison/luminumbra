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
  simulation rule value. Worldgen content is bound separately by the preset identity below.

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

Runtime and soak TCP client slots allow 10 seconds from accept completion (when the
host begins awaiting Hello) for a complete matching Hello frame. A monotonic
`std::chrono::steady_clock` deadline closes peers that miss this window, logs the
client slot, and releases it for another connection. This transport deadline does
not use simulation ticks or affect simulation hashes or replay.

`GameSession::PresetContentHash()` captures the preset used at world creation or load.
Its canonical form first materializes revision-dependent defaults, then stamps the
current schema revision (the same operation used for embedded presets). It serializes
the resolved JSON with sorted object keys, no whitespace, ASCII escapes and nlohmann's
locale-independent binary64 number serialization. The identity envelope is compact ASCII
JSON with the shape `{"dependencies":[["logical/path","file_hash",...],...],"preset":{...}}`.
Entries for files carry their exact-byte hash and, for biome tables, the loader outcome.
Directory and absent-file entries carry explicit state markers instead of file hashes.
The manifest sorts entries by logical `data_root`-relative path, normalizes dot components
and uses UTF-8 with `/` separators. Each file hash covers the exact bytes read in binary mode,
including whitespace; absolute or escaping dependency paths are refused.
Runtime-resolved absolute asset paths and diagnostic provenance are excluded.
FNV-1a-64 for both the envelope and each dependency file uses offset
`14695981039346656037`, prime `1099511628211`, modulo-2^64 arithmetic and exactly 16
lowercase hex digits, encoded without locale-sensitive formatting. Equivalent key order
and preset whitespace produce identical bytes and hashes; a changed terrain override or
dependency file under the same preset name is incompatible. Identical inputs under
different runtime roots produce the same identity. Dependency-free presets use an empty
manifest; `TerrainPresetContentHash(json)` computes only that form, so runtime callers
must use the loader result to bind resolved files.

The manifest follows the generator's actual disk reads: the selected biome table when
biomes are enabled, and every immediate type directory under `common/structures` when
structures are enabled. Each type contributes its optional `placement.json`, if present,
and immediate regular `*.json` pieces actually read by `LoadStructureTemplatePool`.
A malformed placement rejects that pool before any pieces are read; a malformed piece
rejects the pool after the loader finishes reading its pieces. The manifest uses the
same biome/pool loaders as the generator and records their acceptance or fallback
decisions alongside the consumed bytes. The loader retains those objects and the
manifest in an immutable snapshot carried by `TerrainGenParams`, including through
GameSession's create/open adoption paths. Generation consumes that snapshot without
reading the dependencies again, so mutations during the handoff cannot change its
inputs. Structure types and pieces use the manifest's canonical UTF-8 logical-name
order; authored biome rows, boxes and sockets retain their JSON array order.
Direct generators constructed without a preset snapshot still load their biome table
and structure pools from disk. Nested files and other extensions are
not inputs. An absent selected biome table loads with the marker
`biome-table-absent:legacy-classification`; a present rejected/empty table binds its bytes
and `biome-table-rejected/empty:legacy-classification`. An absent structures directory
loads with `structures-directory-absent`; a present directory binds
`structures-directory-present`. Each type binds `pool-accepted` or `pool-rejected/empty`,
including types with no pieces. An absent placement binds
`placement-absent:compiled-defaults`. All of these states produce distinct identities.
A present unreadable dependency, directory enumeration failure or incomplete file read
refuses loading without publishing an identity; it never becomes an absence marker.
Biome material names resolve to compiled material IDs, and neither biome nor structure JSON recursively references other files.
Procedural shaping, hydro, caves, rivers, lakes, cliffs and surface breaks use preset
parameters and compiled algorithms, without additional disk inputs. Runtime species
definitions are loaded separately by GameSession for creature behavior, not terrain
generation; they are outside this worldgen manifest.

The same string continues through protocol-v4 Hello, replay-header-v3, server modes and
NetworkedSessionDriver. Hello v4 appends negotiated lockstep admission limits; the
configuration identity encoding and replay layout are unchanged. Earlier JSON-only identities
will mismatch the expanded identity and refuse admission/verification.
The older numeric replay `preset_hash` remains name
provenance only; admission and verification require `preset_content_hash` as well.

The GNS and Steam transports are optional and disabled by default. They do not yet
exchange or check the SystemConfig identity. Identity admission for these transports is
tracked for the next release. Their SDK declarations and mode implementations remain
guarded when their CMake options are off.

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
