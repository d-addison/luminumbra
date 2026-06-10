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

## Verification

Run:

```powershell
.forge/scripts/validate-engine-frontier.ps1 -Mode FrontierDisabled
.forge/scripts/validate-engine-frontier.ps1 -Mode All
```
