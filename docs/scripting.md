# Lua host capabilities

The current Lua host supports a read-only energy-field query through two entry
points: `sample_energy_field(x, y, z)` and
`world.sample_energy_field(x, y, z)`. Both use the same implementation. They return
channel zero in gameplay units, or zero when no field is connected. Sampling does
not write simulation state.

`LuaState::api_manifest()` describes these initially installed functions. Manifest
version `1.1.0` retains the `luminumbra.scripting.lua_api_manifest.v1` JSON shape;
an empty `module` names a bare global, without exposing a Lua `_G` table. This
version corrects the earlier manifest's unimplemented entries. Entity creation,
block access/writes, event subscription, logging and tick-time APIs are not bound.
Consumers must check the actual manifest rather than assuming those operations
exist because an older descriptor listed them.

`LuaState::Evaluate()` reports execution status separately from an optional numeric
first return value. Successful chunks returning nothing, nil or a nonnumeric value
report `Succeeded` with an empty number. Syntax and runtime failures have distinct
statuses and a Lua diagnostic; other VM failures report `OtherError`. Diagnostics
are runtime text, not stable rule IDs or graph source maps.

`EvalNumber()` remains available for existing callers. It returns false, leaving
its output unchanged, for both execution failures and successful chunks without a
numeric result. Consequently, false alone does not prove that an operation was
denied or that a chunk made no changes.

No standard libraries are opened. The host does not yet provide execution/memory
budgets, isolated globals per chunk, typed component registration or visual
behavior execution. Globals persist between evaluations, and errors do not roll
back earlier Lua writes. This remains a test/development evaluation seam; it is
not an entry point for untrusted behavior programs.

The C++ `LuaSandboxEscape` tests check that forbidden bindings are absent before
attempting calls, verify actual runtime failure, and invoke every advertised
sampler entry point. `LuaEvaluation` tests distinguish success, conversion failure
and execution errors. Existing `AetherScriptBinding` tests exercise populated
fields and read-only sampling. The PowerShell manifest gate checks source
structure only; its receipt is not evidence of live execution. On hosts with
PowerShell, CTest registers `LuaApiManifestSourceContract` to exercise both the
source report producer and the frontier gate consumer in a clean process.
