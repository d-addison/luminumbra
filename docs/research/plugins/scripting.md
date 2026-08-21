# Scripting tooling research

Research date: 2026-08-21

This report treats the inspected source files as authoritative and uses the task brief only for facts outside the permitted read scope. In particular, no README architecture claim is used as evidence.

Effort bands used below are **S** (1-3 engineering days), **M** (4-8 days), **L** (2-4 weeks), and **XL** (more than 4 weeks), including targeted tests and documentation but excluding broad engine redesign.

## Current state

### Runtime surface

`src/luminumbra_common/scripting/LuaState.cpp` owns a `sol::state`, identifies sol2 3.3.0 and Lua 5.4.8, and deliberately never calls `open_libraries()`. That is a meaningful sandbox boundary: Lua exposes standard libraries only when the embedding host opens them, either together with `luaL_openlibs` or individually ([Lua 5.4 manual](https://www.lua.org/manual/5.4/manual.html#luaL_openlibs)).

The only live host function registered by `LuaState` is the read-only energy sampler. The same lambda is exposed as both the bare global `sample_energy_field` and `world.sample_energy_field`. It reads a const `EnergyFieldState`, returns zero when no field is attached, and does not feed its floating-point result back into hashed state. `EvalNumber` executes a string with `safe_script(..., sol::script_pass_on_error)` and reports invalid execution or a non-number result as `false`.

The live surface and declared surface are not equivalent:

| Surface | Observed contents |
| --- | --- |
| Live `LuaState` bindings | `sample_energy_field`; `world.sample_energy_field` |
| Manifest entries | Ten entries across `core`, `entity`, `simulation`, `time`, and `world` |
| Standard libraries | None opened by the host |

`src/luminumbra_common/scripting/LuaApiManifest.cpp` declares schema `luminumbra.scripting.lua_api_manifest.v1`, manifest version `1.0.0`, deterministic `module_then_name` ordering, JSON serialization, and a baseline validator. Its ten entries include the live sampler, but also nine design-contract entries that are not registered in `LuaState`. Several are mutating capabilities (`entity.destroy`, `entity.spawn`, `simulation.emit_event`, and `world.set_block`) and therefore conflict with the task's target policy that scripts read simulation state and never write it. No generator should treat the present manifest as permission to bind every entry until the schema distinguishes at least **planned versus live** and **read-only versus state-affecting** capabilities.

The task brief records an empty `bindings/` directory and 12 design-stage GOAP action/agent/directive scripts under `scripts/common/` that are not loaded. Those locations were outside this task's permitted read set, so this report does not independently infer their interfaces or readiness.

### Existing gates and gaps

`test/scripting/lua-api-manifest-gate.ps1` performs static source checks, duplicates the ten required names in PowerShell, checks build/test wiring textually, and writes `build/<preset>/test-artifacts/scripting/lua-api-manifest.json`. It does not compile the manifest, export the serializer's actual output, or compare live globals against declared live entries. The duplicated required-name list can drift from the C++ initializer.

`test/scripting/lua_sandbox_escape_test.cpp` has a useful negative corpus covering filesystem, process, dynamic loading, raw globals, reflection, network, wall-clock, and random APIs. Today its assertions inspect only the manifest. Its header still says `LuaState` has no interpreter, which is stale relative to the inspected `LuaState.cpp`; none of the authored attack chunks is actually executed. Positive controls also accept manifest-only write APIs such as `world.set_block`, so this test is evidence of the declared whitelist, not of the live read-only sandbox.

The immediate contract gap is therefore more important than adding another binding: there is a live interpreter, a broader design manifest, and a manifest-only escape test, but no automated equivalence check among the intended read-only policy, declared live API, generated editor API, and runtime globals.

## Candidate integrations

### 1. LuaLS with manifest-generated LuaCATS definitions

**Delivery form:** repository-local developer tooling: a generated `luminumbra-api.d.lua`, a checked-in `.luarc.json`, and a CI `lua-language-server --check` job over authored scripts.

Generate a LuaLS definition file from only manifest entries marked `exposure: live`. LuaLS recommends `*.d.lua` definition files with `---@meta`; adding their path to `workspace.library` supplies completion and diagnostics without executing the file ([LuaLS definition files](https://luals.github.io/wiki/definition-files/), [LuaLS `@meta`](https://luals.github.io/wiki/annotations/#meta)). Convert structured parameter and result types into `---@param` and `---@return`, create one table per module, and emit the bare sampler alias explicitly.

Configure `runtime.version` as `Lua 5.4`, add the generated directory through `workspace.library`, and set every unavailable `runtime.builtin` library to `disable`. LuaLS exposes per-library enable/disable controls, which is essential here because its normal Lua 5.4 model would otherwise suggest basic, package, math, I/O, OS, and debug APIs that the host never opens ([LuaLS settings](https://luals.github.io/wiki/settings/#runtimebuiltin)). Do not solve those false positives by merely listing custom globals; the generated definition plus disabled built-ins should model the actual sandbox.

Acceptance criteria:

- the generated stub is byte-stable for a stable manifest;
- LuaLS accepts all 12 design scripts syntactically;
- completion offers only live manifest symbols and language syntax, not standard-library APIs;
- generation fails on an unparseable or underspecified signature;
- `lua-api-gen --check` detects a stale checked-in stub.

**Effort:** M initially, S after the declarative schema in candidate 2 exists. **Risk:** Low operational risk; medium contract risk until planned/live and effect metadata are added. The current free-form signature strings are parseable for simple functions but are a brittle long-term type source.

### 2. Declarative manifest and binding code generation

**Delivery form:** a repository CLI named `lua-api-gen`, consuming a canonical versioned `lua-api.json` and producing normalized manifest JSON, LuaCATS definitions, and C++ registration `.inc` files.

Make the manifest data—not a C++ initializer and a second PowerShell array—the source of truth. Each entry should carry structured fields for module, name, kind, parameters, returns, description, aliases, exposure (`planned` or `live`), effects (`read_only` or `writes_sim`), determinism classification, and the name of a reviewed C++ thunk. The generator should emit registration glue that refers only to hand-written thunks; it should not synthesize host behavior or expose arbitrary C++ objects. This keeps policy review visible and gives the currently empty `bindings/` area a narrow purpose: reviewed host adapters, not generated business logic.

Generation must reject duplicate qualified names, nondeterministic ordering, an alias collision, a live entry without a thunk, and any `writes_sim` entry under the read-only scripting profile. It should also produce a compile-time descriptor table used by `GetLuaApiManifest`, eliminating the gate script's copied name list. A runtime test can then enumerate generated live descriptors and assert that each symbol exists and no extra global/module field is present.

The world-hash contract is a hard boundary. Turning any planned mutator into a live binding, allowing a script result to influence simulation, or changing numeric conversion/ordering can alter `world_hash`. Such a change requires explicit determinism review, replay/network compatibility analysis, and hash-contract tests; code generation must never make it automatic merely because an entry exists.

**Effort:** L. **Risk:** Medium-high because schema migration and C++ glue touch a security boundary. Risk falls materially if generated code is limited to tables and registration calls while implementations remain reviewed, const-correct thunks.

### 3. Production-faithful script test harness

**Delivery form:** a dedicated CTest executable/fixture using the production `LuaState` construction path, plus table-driven `.lua` fixtures and machine-readable per-script results.

Use the embedded runtime rather than the standalone `lua` executable: the standalone host opens the standard libraries, while luminumbra intentionally does not. Extend the existing corpus so every stored `attempt` is executed in a fresh production-configured state and must fail or observe `nil`, as appropriate. Then load each authored script without activating it in gameplay and validate syntax, top-level evaluation, required entrypoints, and returned value shape against a small contract.

The harness should include:

- a fresh state per case, with only generated live read-only bindings;
- a fake/fixture energy field for boundary, negative-coordinate, absent-layer, and repeatability cases;
- manifest-to-runtime equivalence and unexpected-global checks;
- the existing escape corpus executed as code, not only searched in descriptors;
- deterministic instruction and memory limits enforced by the host without exposing the debug library;
- two identical runs with identical inputs and ordered result serialization;
- an assertion that simulation state and `world_hash` are unchanged by every direct script call.

sol2 documents that `safe_script` on a state is not isolated and recommends a new state or a sandbox environment when isolation is needed ([sol2 `state` documentation](https://sol2.readthedocs.io/en/latest/api/state.html)). Fresh states are preferable for this harness because they also reveal accidental dependence on globals left by a previous test.

**Effort:** M. **Risk:** Medium. The main risk is a harness-only host that drifts from production; constructing the real `LuaState` and sharing generated registration code avoids that split.

### 4. Determinism-safe development hot reload

**Delivery form:** an opt-in, development-only `LuaReloadService` with a file watcher, scratch-state validator, tick-boundary activation queue, and reload audit log.

On a file change, read the complete file, compute a content digest, and compile/evaluate it in a new `LuaState` with the exact production sandbox. If validation succeeds, queue replacement of the whole script state at an explicit tick boundary; if it fails, keep the old state and report the protected error. Never patch functions into the active state's global tables and never migrate arbitrary Lua globals. Whole-state replacement prevents stale closures and cross-version state from surviving a reload.

Read-only bindings do not make reload timing deterministic. A script's returned decision can still affect later hashed simulation state, and filesystem notification timing differs across machines. Therefore:

- disable hot reload during replay, multiplayer, verification, and any run governed by the `world_hash` contract by default;
- for a future authoritative mode, represent reload as an ordered simulation input containing activation tick, manifest version, and script digest, distribute identical bytes, and reject peers without the digest;
- include the active script digest and manifest version in determinism diagnostics, and evaluate whether they must become inputs to `world_hash` before enabling script-driven decisions;
- permit no direct simulation mutation from Lua, before or after reload.

**Effort:** L for safe local-development reload; XL for authoritative synchronized reload. **Risk:** High. File watching is straightforward, but activation semantics, state lifetime, replay compatibility, and the world-hash contract are not.

### 5. Manifest gate as a Banso verify check

**Delivery form:** a first-class Banso project verification check named `lua-api-contract`, producing the existing JSON artifact shape and failing on any nonzero subcheck.

Initially, the check can invoke `test/scripting/lua-api-manifest-gate.ps1` with the selected build preset and publish its artifact. In the target form it should run `lua-api-gen --check`, the compiled manifest baseline/equivalence test, the live escape subset, and LuaLS checking when the tool is available. The result should report separate subchecks for schema validity, generated-file freshness, read-only policy, live binding equivalence, sandbox escapes, and script diagnostics so a Banso failure is actionable.

Avoid teaching the Banso adapter a third copy of the API list. It should orchestrate authoritative checks and consume their structured result. The manifest/codegen check is fast enough for every verification; the compiled harness can be a targeted gate selected whenever scripting inputs or bindings change.

**Effort:** S for wrapping the current gate, M for the target structured check after candidates 1-3. **Risk:** Low for the wrapper and medium for rollout if developer machines do not yet have LuaLS; treat LuaLS absence explicitly rather than silently passing.

## Ranking

| Rank | Candidate | Why now | Dependency / release gate |
| ---: | --- | --- | --- |
| 1 | Production-faithful script test harness | Converts the existing manifest-only security claims into evidence against the live interpreter before any design script is activated. | Must prove sandbox denial, read-only behavior, repeatability, and unchanged `world_hash`. |
| 2 | Banso `lua-api-contract` verify check | Low-cost path to make the existing gate visible and consistently enforced, then absorb stronger checks as they land. | Must not duplicate API policy; nonzero status and artifact publication are mandatory. |
| 3 | LuaLS plus generated definitions | Immediate authoring value for the 12 design scripts and catches API misuse before runtime. | Block generation from the current undifferentiated manifest; emit only reconciled `live` entries and disable unavailable built-ins. |
| 4 | Declarative manifest and binding codegen | Removes three-way drift among C++, PowerShell, and editor definitions and scales the surface safely. | Requires schema/effect review and generated-output checks; live mutators remain forbidden under the read-only profile. |
| 5 | Development hot reload | Valuable only after loading, contracts, and isolation are tested. | Local-only first; authoritative use is blocked on an explicit replay/network/`world_hash` design. |

Recommended sequence: reconcile the manifest's planned/live and effect semantics; land the production-faithful harness; wrap it and the current manifest gate in `lua-api-contract`; generate LuaLS stubs from the reconciled data; then introduce narrow registration codegen. Defer hot reload until the 12 design scripts have a stable load/entrypoint contract.

## Sources

Repository files analyzed:

- `src/luminumbra_common/scripting/LuaState.cpp` — live state construction, binding registration, energy sampling, and protected evaluation.
- `src/luminumbra_common/scripting/LuaApiManifest.cpp` — manifest contents, JSON serialization, ordering, and baseline validation.
- `test/scripting/lua_sandbox_escape_test.cpp` — manifest-only escape corpus and whitelist assertions.
- `test/scripting/lua-api-manifest-gate.ps1` — current static gate checks and JSON artifact.

Web sources consulted (accessed 2026-08-21):

- [Lua 5.4 reference manual: standard-library loading](https://www.lua.org/manual/5.4/manual.html#luaL_openlibs).
- [LuaLS definition files](https://luals.github.io/wiki/definition-files/).
- [LuaLS annotations: `@meta`](https://luals.github.io/wiki/annotations/#meta).
- [LuaLS settings: runtime version, built-ins, and workspace library](https://luals.github.io/wiki/settings/).
- [sol2 state and safe-script behavior](https://sol2.readthedocs.io/en/latest/api/state.html).
