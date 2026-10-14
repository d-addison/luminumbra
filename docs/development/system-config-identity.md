# Effective simulation configuration identity

`GameSession::SystemConfigIdentity()` computes identity on demand from the session's
current simulation gates and tuning. It works without `ApplySystemConfig`, using the
compiled session defaults, and observes subsequent public tuning setters. Peers compare
this identity before TCP lockstep join or TCP remote replication admission; replay playback
compares the recorded identity with the live session before stepping any replay tick.
Missing identities on either side always fail compatibility, including two empty strings.

The fields are the simulation keys and owned parameters in `ConfigSchema.json`, plus
`render.circadian` amplitude, which reaches `RunCircadianOnTick`. The five session gates
are hydrology weather, weather events, aether state, water resolution and active regions.
Other key slots are normalized to enabled: their config flags select compiled fallback
tuning rather than disable the session system; erosion has no session gate. Disabling
such an override with default tuning therefore leaves the effective identity unchanged.
Parameters come from current session storage, including clamped plant mutation and the
integer reproduction counts resolved by `ApplySystemConfig`. Presentation and user
settings are excluded. This contract covers these schema fields, not all possible
simulation inputs, content or session options.

Canonical bytes start with ASCII `system-config:v2:`. In generated schema order, each
included key emits its `SysKey` id as u8, then its effective enabled flag as u8. Each owned
parameter emits its `SysParam` id as u8, then a scalar type tag and little-endian value:

- Tag 0: IEEE binary32 for ordinary tuning floats.
- Tag 1: three IEEE binary32 components for vector parameters.
- Tag 2: IEEE binary64 for the three foraging values.
- Tag 3: u32 for reproduction maturity, cooldown and courtship tick counts.

The field order and ordinary float encoding are preserved. Exact foraging doubles and
reproduction integers require schema v2: converting them to binary32 would hide adjacent
values set through the public API. Signed zeros remain distinct. FNV-1a uses offset
14695981039346656037 and prime 1099511628211, with unsigned 64-bit wraparound. The result
is 16 lowercase hex digits emitted without locale formatting. Shipped defaults serialize
to **279 bytes → `da24cea9dbc5159b`**. The independent stdlib computation in
`test/tools/test_system_config_identity.py` resolves JSON inputs through binary32 as
SystemConfig does, applies effective fallback/clamping/count conversion, encodes with
`struct.pack('<f' / '<d' / '<I', value)`, and folds each byte as
`h = ((h ^ byte) * 1099511628211) & ((1 << 64) - 1)`.

Lockstep protocol and LREC1 layout versions are 2 because Hello and replay headers now
carry an additional length-prefixed identity string. Version 1 replay headers are
rejected. Desync replay dumps include the identity captured at handshake. TCP (including
runtime/soak modes) hosts validate the same Hello compatibility fields before adding a
replication client. TCP joiners send Hello before user commands. TCP lockstep and replay
enforce the SystemConfig identity.

The GNS and Steam transports are optional and disabled by default. They do not yet
exchange or check the SystemConfig identity. Identity admission for these transports is
tracked for the next release. Their SDK declarations and mode implementations remain
guarded when their CMake options are off.

Saves carry no simulation identity. Loading applies the loading process's configuration;
identity adds no save load/overwrite refusal and changes no save, snapshot, fixture or
world-open diagnostic contract. Existing clock/configuration checks retain their behavior.

The per-tick world hash, replay start/checkpoint comparisons and every existing golden
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
