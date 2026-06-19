# Spec — SystemConfig: data-driven feature-flag + per-system tuning registry (§1)

Status: DRAFT. Owner direction: HANDOFF-2026-06-18-forge-roadmap-driver §1 + §0.5 critique
revisions (report `.forge/critique-handoff-roadmap-20260618-221947.md`).
Branch: `feat/polyglot-audit-roadmap` (LOCAL-ONLY). Engine-generic; flag *values* are game data.
Tier: **T1 (full lifecycle)** — this is the cross-cutting substrate every pillar plugs into.

## Context

Today's toggles are ad-hoc and scattered: `PlantTag` per-entity opt-in
(`components/PlantComponents.h:70`), `LUMIN_GRADE` env var (`LightingPass.cpp:151`),
`LUMIN_ATMOS` env var (`RenderPipeline.cpp:2014`), hardcoded tree-scatter constants
(`main_client.cpp:3209-3217`), and moon/wind/palette constants in render passes. The owner
wants **every system configurable + toggleable on/off from data, no recompile**. This spec
defines `SystemConfig` — one registry (`data/common/systems.json`) queried by every system —
so each pillar plugs into a consistent on/off + tuning seam.

**This is §1a (the minimal mechanism) only.** §1b (migrating the existing ad-hoc toggles into
the registry, one subsystem at a time with a before/after behavior diff) is a SEPARATE,
deferred, opportunistic effort and is NOT in this spec's scope (critique Objection 4). The one
exception is the **moonlight** render flag, migrated here as the first real consumer to prove
the §1b pattern (R-7).

### Determinism architecture (verified against the codebase — corrects the handoff)

The handoff said sim flags "fold a `ComputeConfigSubHash` **into** world_hash (deliberate
bump)." The actual architecture is cleaner and this spec adopts it:

- The **top-level** `world_hash` (`ComputeWorldStreamingStateHash`, canonical baseline
  `d950a6afc12a5cdc`) is computed from world/streaming STATE. Per-system sub-hashes
  (`WorldStreamingStateSubHashes` in `persistence/WorldPersistenceRoundtrip.h:100`) are
  **ADDITIVE and SEPARATE** — they exist for desync localization, and the header states the
  top-level hash is "unchanged byte-for-byte" by them.
- Each existing sub-hash (`wind`, `weather`, `aether`, and GameSession's `scent`, `plant`)
  returns **empty `{}` when its system has no participants** (e.g. `ComputeScentSubHash`
  `GameSession.cpp:721`, `ComputePlantSubHash:750`).

Therefore a `config` sub-hash modeled on these means **zero re-pin** of `world_hash` from its
introduction, AND an empty config sub-hash at all-defaults keeps the sub-hash SET byte-identical
too. A `sim.*` flag only ever moves the top-level `world_hash` when it actually changes sim
STATE (e.g. erosion enabled → terrain heightfield changes → terrain sub-hash + top-level hash
move) — that is the intrinsic, deliberate, per-feature bump, re-pinned then per
`AC-I6-TDD-LOCK-004`. SystemConfig itself introduces **no** bump.

## Pillars & requirements

### P1 — The registry & query API (§1a core)
- **R-1.1** New files `src/luminumbra_common/core/SystemConfig.{h,cpp}` + data file
  `data/common/systems.json`. JSON parsed via the vendored `nlohmann/json` (`vendor/nlohmann/`).
- **R-1.2** Schema: top-level `sim` and `render` objects, each a map of
  `"<key>": { "enabled": bool, "params": { "<name>": <number | [x,y,z]> } }`. Two sections so
  the determinism boundary is explicit in the data: `sim.*` keys may affect sim state; `render.*`
  keys are render-only.
- **R-1.3** API (header):
  - `bool enabled(Key) const` — O(1) bit test on a resolved snapshot (NOT a map lookup).
  - `float param(Key, ParamId, float fallback) const`
  - `Vec3 param3(Key, ParamId, Vec3 fallback) const`
  - Keys and param ids are a **compile-time enum** (`enum class SysKey`, `enum class SysParam`);
    the JSON string ↔ enum mapping is resolved once at load. Unknown JSON keys are ignored with
    a warning (forward-compat); unknown enum queries return the fallback.
- **R-1.4 (load)** `SystemConfig::Load(path)` parses the file once at startup into an immutable
  resolved snapshot. **Defaults table:** every `SysKey`/`SysParam` has a compiled-in default
  (`enabled=false`, documented param defaults); the JSON overrides only what it names.
- **R-1.5 (perf)** The resolved snapshot stores flags as a packed `std::bitset`/`uint64_t` and
  params as a flat `std::array` indexed by the enum. `enabled()` does a single bit test; no
  allocation, no locking, no string compare on any hot path. Budget: callable per-entity across
  the 36k-entity / 30 Hz tick with zero measurable overhead vs. today's hardcoded `if`.

### P2 — Determinism: the config sub-hash (additive, sim-only)
- **R-2.1** Add a `config` field to `WorldStreamingStateSubHashes`
  (`persistence/WorldPersistenceRoundtrip.h`), supplied by
  `SystemConfig::ComputeConfigSubHash()`, exactly mirroring the `wind`/`weather`/`aether` slots.
  The top-level `world_hash` is UNCHANGED by this addition.
  - **CRITICAL (verified 2026-06-18):** the top-level `world_hash` is
    `ServerWorldRunner::ComposeWorldHash`, a LITERAL append-chain
    `StableChecksum(chunk + "|wind:" + … + "|scents:" + scent)`. Adding a `"|config:"` term
    THERE would change the checksum **even when config is empty** (a deliberate bump). So
    R-2.1 must populate ONLY the **additive struct field** `out_sub.config` in
    `ServerWorldRunner::ComputeWorldSubHashes` (and `ComputeWorldHashAndSubHashes`) — and must
    **NOT** touch `ComposeWorldHash`. That keeps the top-level hash byte-identical (zero re-pin)
    while still surfacing config drift to the desync-localization oracle.
  - **Ownership:** the server needs its own `SystemConfig` (loaded from
    `data/common/systems.json` only — NO per-user overlay; `user.*` is client-only). At
    defaults `ComputeConfigSubHash()` is empty → `out_sub.config == ""`, identical to today.
  - **Baseline-break check before landing:** confirm no gate serializes the WHOLE
    `WorldStreamingStateSubHashes` struct (all fields) against a pinned literal — if one does,
    adding an empty `config` field changes that JSON and needs a re-pin (mechanical, not a
    world_hash bump). Verify with the determinism/NetworkStateHash gate.
- **R-2.2** `ComputeConfigSubHash()` returns empty `{}` when **every `sim.*` flag and param is
  at its compiled default**. Otherwise it is `Persistence::StableChecksum("config:v1:" + <the
  sorted, canonical serialization of NON-DEFAULT sim entries only>)`. Order-independent:
  iterate `SysKey` in enum order (canonical), emit only non-default `sim.*` entries.
- **R-2.3** `render.*` flags/params NEVER appear in `ComputeConfigSubHash()` and never touch any
  hash (render-only, one-way — same discipline as `LUMIN_GRADE`/`LUMIN_ATMOS`).
- **R-2.4** `SystemConfig` reads no wall-clock, draws no RNG, and is libm-free on any path the
  sim consumes (it only stores parsed data).

### P3 — Default-off discipline & the game profile
- **R-3.1** Every `sim.*` and `render.*` key defaults to `enabled=false`. A missing or empty
  `systems.json` yields all-defaults and MUST NOT crash (graceful: log + use defaults).
- **R-3.2** Ship TWO tracked profiles: `data/common/systems.json` (the **default/all-off**
  baseline) and `data/common/systems.game.json` (the **game profile** — the intended-ON set;
  seed it with today's known-green behavior: nothing that changes sim state yet, i.e. it starts
  equal to all-off and grows as pillars land). Both are gated baselines kept green every slice
  (critique Objection 5). A `LUMIN_SYSTEMS` env var (or `--systems <path>`) selects the profile;
  default load path is the all-off file.

### P4 — Migrate moonlight as the first §1b consumer (proof of pattern)
- **R-4.1** Replace the hardcoded moonlight constants (render path) with
  `render.moonlight.{enabled, color (param3), strength (param)}` read through `SystemConfig`.
  Defaults reproduce today's look EXACTLY (the resolved values equal the current constants), so
  the visual sweep is unchanged with the default profile.
- **R-4.2** Render-only: moonlight never touches `world_hash`/`ComputeConfigSubHash`.

## Acceptance Criteria (each maps to a RED test, written first)

### AC-SC-001 — Missing/empty config → all-defaults, no crash
Given no `systems.json` (or an empty `{}`)
When `SystemConfig::Load` runs
Then every `enabled(k)` is false, every `param` returns its fallback, and no exception/crash occurs.
Proving signal: `test/core/system_config_test.cpp` (ctest `SystemConfigDefaults`).

### AC-SC-002 — Param parse + defaults round-trip
Given a `systems.json` setting some flags ON and some params
When loaded
Then `enabled`/`param`/`param3` return exactly the file's values for named keys and the compiled
default for unnamed ones; unknown JSON keys are ignored (warned, no throw).
Proving signal: `test/core/system_config_test.cpp` (ctest `SystemConfigRoundtrip`).

### AC-SC-003 — Flag OFF is a no-op AND baselines hold (top-level + sub-hash set)
Given the default (all-off) profile
When the canonical headless world is built and hashed
Then the top-level `world_hash` equals `d950a6afc12a5cdc` (byte-identical) AND
`ComputeConfigSubHash()` is empty `{}` (sub-hash set byte-identical to today).
Proving signal: engine-frontier determinism gate + `test/core/system_config_hash_test.cpp`
(ctest `SystemConfigHashNeutralAtDefaults`).

### AC-SC-004 — A sim non-default flag moves the config sub-hash; render flags never do
Given the config sub-hash is empty at defaults
When a `sim.*` flag/param is set to a non-default value
Then `ComputeConfigSubHash()` becomes non-empty and is deterministic + order-independent
(same value regardless of JSON key order); and setting ANY `render.*` flag/param leaves
`ComputeConfigSubHash()` empty and `world_hash` unchanged.
Proving signal: `test/core/system_config_hash_test.cpp` (ctest `SystemConfigSimVsRenderHash`).

### AC-SC-005 — `enabled()` is O(1) on the hot path, zero alloc
Given a loaded snapshot
When `enabled()` is called N×10^6 times
Then it performs a bit test only (no heap allocation, no lock, no string compare), verified by a
no-alloc assertion / micro-budget in the test.
Proving signal: `test/core/system_config_test.cpp` (ctest `SystemConfigHotPathBudget`).

### AC-SC-006 — Moonlight migration is visually identical at defaults
Given `render.moonlight` resolves to today's constant values by default
When the `world_visual_sweep` scenario runs with the default profile
Then `visual_critique.py --strict` reports no new flags vs. the current baseline (0/48 holds).
Proving signal: WorldVisualSweep rerun (per `AC-I6-TDD-LOCK-003`).

## Out of scope (recorded non-goals)
- Migrating LUMIN_GRADE/ATMOS, wind, palette, tree-scatter, LOD (→ §1b, lazy, later).
- Hot-reload / live in-game flag editing (→ optional `render.debug_overlays` later).
- Any sim flag that changes sim STATE (those land with their owning pillar, A/D/E/§4).

## Verify With
`cmake --build --preset debug` (msys64 PATH first) then
`ctest --test-dir build/debug -R "SystemConfig" --output-on-failure` and
`.\.forge\scripts\validate-engine-frontier.ps1 -Mode UnitTests` +
`-Mode WorldVisualSweep` for AC-SC-006.

---

## Addendum A (2026-06-18) — `user.*` settings extension + per-user overlay persistence

Owner decision: SystemConfig is the SINGLE config registry — it also carries player-facing
**settings** (no second config system). See `docs/STANDARDS.md` §5. This addendum extends the
registry; it is the spec for task #9. Status: DRAFT (build pending, TDD).

### Pillars
- **P5 — `user.*` section (client-only, NEVER hashed).** A third top-level section alongside
  `sim`/`render`, holding player settings in sub-groups:
  - `user.video.*`: `resolution` (e.g. "3840x1600"/[w,h]), `window_mode` (windowed|borderless|
    fullscreen), `vsync` (bool), `fov` (deg), `render_scale` (float), `mouse_sensitivity` (float).
  - `user.audio.*`: `master`, `sfx`, `music` (0..1).
  - `user.controls.*`: a keybind map `action -> key code` (int), keyed by `InputAction` name.
- **P6 — Value types.** Extend params to support **int** (key codes, enum-as-int like window_mode)
  and **string** (resolution) in addition to float/vec3. Keybinds are an action→int map.
- **P7 — Per-user overlay + Save.** Load layering: compiled defaults → `data/common/systems.json`
  (dev/game) → **per-user overlay** `%APPDATA%/Luminumbra/settings.json` (or `$XDG_CONFIG_HOME`/
  `~/.config/luminumbra/` on POSIX) → env/CLI (dev override). `SystemConfig::SaveUserOverlay()`
  writes **only** the `user.*` section back to that path (atomic write; create dirs). Missing
  overlay → defaults (no crash).
- **P8 — Ownership.** A single loaded `SystemConfig` instance is owned at the client/session entry
  (resolves task #5's ownership question) and threaded to consumers; the `config` sub-hash is
  supplied to `WorldStreamingStateSubHashes` from it (task #5).

### Acceptance Criteria (RED-first)
- **AC-SC-101** `user.*` round-trips (video/audio/controls incl. int keycodes + string resolution);
  unnamed → fallback; malformed overlay → defaults, no crash.
- **AC-SC-102** Setting ANY `user.*` value leaves `ComputeConfigSubHash()` empty AND `world_hash`
  unchanged (`d950a6afc12a5cdc`) — user settings are never hashed.
- **AC-SC-103** `SaveUserOverlay()` then reload reproduces the same `user.*` values byte-for-byte,
  and the written file contains ONLY `user.*` (no sim/render leakage).
- **AC-SC-104** Overlay merge precedence: a `user.*` value in the overlay wins over `systems.json`;
  `sim.*`/`render.*` come only from `systems.json` (overlay ignored for them).
- **AC-SC-105** Keybind map resolves `InputAction -> key`, with a compiled default binding when the
  overlay omits an action.

### Verify With (addendum)
`ctest --test-dir build/debug -R "SystemConfig" --output-on-failure` (extends the existing suite).
