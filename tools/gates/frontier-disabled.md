# Engine Frontier Disabled Gate

## Status

- Gate: disabled by default.
- Owner: T-EF-4-frontier-disabled-gate.
- Default state: documentation and validation only.

## Gate

The engine-frontier work remains a planning and validation track until a later task explicitly changes this artifact and its validator. It must not enable experimental frontier behavior in default builds, default test runs, or default runtime paths.

## Disabled by Default

- Default builds must not require frontier execution.
- Default validation must treat this file as a policy gate, not as a feature toggle.
- Any frontier runtime behavior requires explicit opt-in configuration.

## Allowed Activation

Temporary activation is allowed only for local investigation or for a named Forge task that owns the change. Activation must be documented with its switch, scope, rollback path, and verification commands.

### Gate-Backed Activations

- Persistence runtime save/load (T-I2-12, T-I2-13): `GameSession::SaveWorldState` runs on the world-exit/shutdown path and `GameSession::LoadWorldState` runs on world enter. The path is inert without an existing world snapshot (load is a clean miss) and writes nothing unless a chunk carries unsaved voxel edits, so default builds and save-less worlds stay byte-for-byte on the fresh-world path. Scope: `src/luminumbra_common/world/GameSession.{h,cpp}`, the generation-skip contract in `SHIELD_WorldSystem`, and the wiring in `src/luminumbra_client/main_client.cpp`. Verification: `validate-engine-frontier.ps1 -Mode PersistenceRuntimeRoundtrip`, which drives the `persistence_roundtrip_smoke` scenario through `--persistence-phase save` and `--persistence-phase load` against a shared `--persistence-session-dir`. Rollback: revert the GameSession save/load wiring in `main_client.cpp`.

## Verification

Run:

```powershell
tools/gates/validate-engine-frontier.ps1 -Mode FrontierDisabled
tools/gates/validate-engine-frontier.ps1 -Mode All
```
