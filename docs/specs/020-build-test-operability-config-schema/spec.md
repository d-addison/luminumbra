# Spec 020: Build & Test Operability + Config / Shader-ABI Schema

> Status: SPEC (created 2026-06-26). Derived from the engine-infrastructure devil's-advocate critique
> (`.forge/critique-engine-infrastructure-framework-20260626-200421.md`, findings F7 and the
> config-schema half of F8).

## The framing insight (why this spec exists)

The project already has serious validation machinery — an engine-frontier gate, a determinism oracle
(`--smoke`), a FLIP visual-parity harness, a render benchmark, and a CTest suite. Two pieces of
*operational* and *structural* debt undermine the trust those gates are supposed to buy:

1. **F7 — you can't always tell which artifact a gate read.** The repo carries **two configured CMake
   trees**: the preset trees under `build/${presetName}` (e.g. `build/debug`, the tree the
   engine-frontier gate actually builds and runs) AND a root `build/` tree with its own cache,
   binaries, CTest results, and committed test artifacts. A gate is only useful if the team always
   knows which executable, which shader cache, which screenshots, and which CTest results it is
   reading. Today that is ambiguous: a visual gate can bless an image produced by a stale binary in
   the other tree, and important GPU/perf tests are excluded or conditionally-registered (invisible
   rather than tiered), so a green gate can hide a red one.

2. **F8 (config half) — the manual config registry is fragile, and that fragility compounds during
   the RHI migration.** `SystemConfig` is a hand-maintained registry whose C++ defaults must be
   *manually kept byte-identical* to the constants in the owning systems, and whose hash behavior
   (which value affects `world_hash`, which is render-only) is enforced by **hand-written control
   flow**, not by the type system. That is tolerable in a GL-only renderer. It becomes a determinism
   and correctness hazard once spec 014 adds single-source HLSL with per-backend emission, spec 015
   adds exposure/froxel/OIT/colored-shadow constants plus shader hot-reload, and the same constants
   start living in three shader dialects at once.

This spec fixes the **operability** half (F7) and the **config typed-schema/codegen + hash-behavior**
half of F8. It deliberately does **not** own shader-resource reflection/layout validation — that
belongs to the new **spec 016 — render framework** (passes expressed against reflected resource
handles). The boundary is stated explicitly in NFR-007. F8's shader-ABI reflection half is referenced,
not owned, here.

**Determinism contract.** Everything in this spec is infrastructure, not new runtime behavior.
`luminumbra_server_app --smoke` must stay `6f008a9f637c40b7`, run==replay, before and after every
change. The config-schema codegen (Group B) is a *representation* change only: it must emit
**byte-identical** compiled defaults and a byte-identical `config:v1:` hash string, so no FR here is
allowed to bump `world_hash`. This is the structural enforcement of **spec 018 — determinism
hardening**'s render-vs-sim residency rule, which `SystemConfig.cpp:223` (`render.* never hashed`)
currently implements by hand.

**Environment realities (project memory).** The parent harness builds and tests `build/debug` via
`cmake --build --preset debug`; the engine-frontier gate (`tools/gates/validate-engine-frontier.ps1`)
builds and reads `build/$BuildPreset`. The Bash tool is sandboxed and cannot run the GPU client or the
compiler — all builds, the client, and the gates run through the PowerShell tool, prepending
`C:\msys64\ucrt64\bin` to PATH. Any preflight or manifest tooling this spec adds must run in that
PowerShell + preset-tree reality, not assume a Unix shell or a single ambient `build/`.

## Goals

- **G-1** — Make **exactly one canonical build root per preset** the source of truth, and make every
  gate REFUSE to read stale or wrong-tree artifacts (executables, shader cache, screenshots, CTest
  results).
- **G-2** — Give every gate-produced artifact a **manifest** that records preset + git SHA +
  executable hash + shader hash + scenario + timestamp, and make visual/perf gates compare that
  manifest against the *running* binary so you cannot bless an image from a stale build.
- **G-3** — Make manual GPU/perf tests **tiered and discoverable** (a named, enumerable tier), not
  invisible by exclusion.
- **G-4** — Replace the hand-maintained `SystemConfig` registry with **typed schemas that GENERATE**
  the config bindings, feature flags, defaults, and the config-hash serializer — so defaults can no
  longer silently drift from the constants they mirror.
- **G-5** — Make every sim-affecting value **declare its hash behavior structurally** (in the schema),
  so determinism residency is a property of the type, not of hand-written control flow in
  `ComputeConfigSubHash`.
- **G-6** — Do all of the above with **zero behavior change**: `world_hash`, `--smoke`, and the
  byte-identical default config string are preserved exactly.

## Non-Goals

- **NG-1** — Shader-resource reflection, constant-buffer layout generation, and reflected-layout CI
  validation. That is the F8 shader-ABI half and is owned by **spec 016 — render framework**. This
  spec generates the *C++/config* side and references 016 for the shader side (NFR-007).
- **NG-2** — The RHI / Vulkan / DX12 port itself (**spec 014**). This spec de-risks 014 by removing
  the two-build-tree ambiguity and by moving config off a manual registry, but performs no backend
  migration.
- **NG-3** — Any new render feature, sim system, or worldgen change. No FR here adds runtime behavior;
  this is operability + representation only.
- **NG-4** — Changing the determinism oracle, the FLIP algorithm, or the perf-budget thresholds. This
  spec changes *what artifacts gates trust*, not *what they assert*.
- **NG-5** — Deleting the committed `build/debug/test-artifacts/*` baselines. They stay; this spec adds
  provenance to how they are produced and validated, and (B-side) keeps their byte content identical.

## Functional Requirements

IDs are grouped. Each is anchored to a verified file/line in the current tree (see **Key files**).
"Refuse" means the gate must fail loudly (non-zero exit + named reason), never silently read the
wrong artifact.

### Group A — Build & test operability (F7)

- **FR-A-001 — One canonical build root per preset.** The preset tree `build/${presetName}`
  (`CMakePresets.json:13`) shall be the single source of truth for that preset's cache, binaries,
  CTest registry, shader cache, and gate artifacts. The configured root `build/` tree shall be
  **deprecated as a configured tree** (no engine source/build-file change required to begin
  deprecation — it is a documented + preflight-enforced policy; see FR-A-002).
- **FR-A-002 — Two-tree preflight refusal.** A preflight check shall **FAIL** when both a root
  `build/CMakeCache.txt` and a preset `build/${presetName}/CMakeCache.txt` exist concurrently,
  emitting which trees were found and which preset the caller intended. The engine-frontier gate
  (`tools/gates/validate-engine-frontier.ps1`, build at `:255`, client-exe resolution at `:309`)
  shall invoke this preflight before any build/test step, so the gate can never silently build one
  tree and read another.
- **FR-A-003 — Gate resolves the exact tree it builds.** Every gate step shall resolve its executable,
  shader cache, and artifact paths from the **same** `build/$BuildPreset` root it built
  (`validate-engine-frontier.ps1` `Get-ClientExe` `:308-314` already does this for the client exe —
  extend the same single-root resolution to shaders, screenshots, and the CTest manifest). No gate
  step may fall back to the root `build/` tree.
- **FR-A-004 — Artifact manifest schema.** Every gate-produced artifact (visual capture, perf JSON,
  CTest manifest) shall carry a sidecar/embedded **manifest** with: `build_preset`, `git_sha`,
  `exe_hash` (hash of the binary that produced it), `shader_hash` (hash of the shader cache/sources
  used), `scenario` (capture flags / test name), and `timestamp`. This generalizes the existing
  per-artifact `build_preset` labeling (`test/CMakeLists.txt:1510-1547`) into a full provenance record.
- **FR-A-005 — Visual/perf gates compare manifest to the running binary.** The visual-parity gate
  (`tools/flip_diff.py` + `tools/golden_update.py`, spec 015 NFR-003) and the perf gate
  (`--render-benchmark`) shall, before comparing or blessing, assert that the candidate artifact's
  `exe_hash` and `shader_hash` match the binary/shaders **currently being gated**. A mismatch shall
  **refuse the bless / fail the gate** with the offending hashes named. This is the structural fix for
  "bless wrong images / pass against stale binaries."
- **FR-A-006 — Tiered, enumerable manual GPU/perf tests.** The currently-excluded/conditional tests
  shall be a **named, discoverable tier**, not invisible. Concretely: `ForestPerfBudget`
  (`test/CMakeLists.txt:1404`, label `manual;perf;budget`), `ShieldRtSpike` (`:1416`,
  `manual;perf;spike`), `ShieldRtTracerProfileGpu` (`:1425`, `manual;perf;gpu`), the far-field GPU
  parity family (`:1432/:1439/:1446`, `manual;perf;gpu`), and the python-conditional `VisualCritiqueFlags`
  (`:1482`) / `TimelapseSelftest` (`:1502`) gates shall each be enumerable via a single documented tier
  command (e.g. `ctest --preset $BuildPreset -L manual --show-only`) and reported (count of
  registered-vs-skipped) by the gate, so a conditionally-unregistered test is **visibly missing**, not
  silently absent.
- **FR-A-007 — Spec 014/015 build instructions corrected.** The "build BOTH trees" instruction in
  `docs/specs/015-atmospheric-lighting-colored-glass/spec.md:344` (and any equivalent in spec 014)
  shall be corrected to "build the one canonical preset tree you test." (Doc-only change to those
  specs; this FR's deliverable is the corrected instruction text, verified by grep.)

### Group B — Config & shader-ABI schema (F8, config half)

- **FR-B-001 — Typed config schema is the single source of truth.** A typed schema (one declarative
  definition per feature flag and per tunable param) shall replace the hand-written parallel arrays
  `SysKey` (`SystemConfig.h:48-70`), `SysParam` (`SystemConfig.h:73+`), `kKeys`
  (`SystemConfig.cpp:34-51`), and `kParams` (`SystemConfig.cpp:53+`) as the authoritative source. The
  schema declares, per entry: section (sim/render), json name, type (scalar/vec3), default, and owning
  system.
- **FR-B-002 — Generated bindings + feature flags + defaults.** The `SysKey`/`SysParam` enums, the
  `kKeys`/`kParams` registries, the canonical ordering, and the accessor surface shall be **GENERATED**
  from the schema (codegen header(s)), eliminating the manual parallel-array maintenance that today
  requires reordering discipline ("Append-only (canonical order); never reorder",
  `SystemConfig.h:53-54`).
- **FR-B-003 — Defaults cannot drift from owning-system constants.** The schema entry for each sim
  param shall reference (or be cross-checked against) the constant it mirrors, replacing the
  hand-maintained "defaults MUST match …" contract repeated at `SystemConfig.cpp:57, 73, 78, 82, 86,
  94`. A drift between a schema default and the owning constant shall be a **compile-time or CI
  failure**, not a silent byte-difference that only surfaces as a `--smoke` flake.
- **FR-B-004 — Every value declares its hash behavior in the schema.** Each schema entry shall
  declare, structurally, whether it is sim-affecting (hashed) or render-only (never hashed). The
  config-hash serializer shall be **generated from that declaration**, replacing the hand-written
  residency branch `if (key_meta.section != Section::Sim) continue; // render.* never hashed`
  (`SystemConfig.cpp:223`) and the manual "emit only enabled, non-default sim params" logic
  (`SystemConfig.cpp:222-244`). Determinism residency becomes a property of the schema, not of
  control flow — the structural enforcement of spec 018's render-vs-sim rule.
- **FR-B-005 — Byte-identical hash output (no accidental bump).** The generated serializer shall
  produce a `config:v1:` byte string **character-for-character identical** to the current
  `ComputeConfigSubHash` (`SystemConfig.cpp:217-245`) for every config state: the all-default empty
  string (`:243`, `if (!any) return {}`), and every enabled-system combination. `--smoke` shall stay
  `6f008a9f637c40b7`.
- **FR-B-006 — Pilot scope = lighting/exposure constants first.** The schema migration shall **start**
  with the lighting/exposure-adjacent flags already in the registry (`RenderMoonlight` +
  `MoonlightStrength`/`MoonlightColor`, `SystemConfig.cpp:43, 55-56`) and the spec-015 exposure/froxel/
  OIT/colored-shadow constants (spec 015 NFR block, `:201-218`) as they land — proving the codegen on
  the smallest render-only surface before migrating the sim keys. (Per the F8 migration path: start
  with lighting/exposure, then replace duplicated shader/C++ constants with generated headers.)
- **FR-B-007 — Generated header replaces duplicated constants.** Where a config default is duplicated
  against the constant it must mirror, the schema shall emit a **single generated header** consumable
  by both sides, so the value has exactly one authored home. The **verified C++/C++ case** is the
  registry's "defaults MUST match …" contracts: e.g. `SystemConfig.cpp:55-56` moonlight defaults vs
  the owning render path, and the `Eco*`/`Wildlife*`/`Thirst*`/etc. defaults that must mirror their
  owning-system constants (`SystemConfig.cpp:57, 73, 78, 82, 86, 94`). The **C++/shader case** applies
  to any *config-driven* scalar/vec constant a shader also consumes. (NOTE: the shader's hardcoded
  `kMoonColor=(0.40,0.52,0.92)` / `kMoonKeyScale=1.3` at `res/shaders/lighting_pass.frag:461-462` are
  *not* currently config-driven — they differ from `SystemConfig MoonlightColor=(0.6,0.7,1.0)` and
  `RenderMoonlight` is render-only/default-OFF — so unifying them is a *follow-on* once moonlight is
  routed through config, not an existing mirror.) The shader-side *resource layout* reflection is spec
  016; this FR covers only scalar/vec constant de-duplication on the config side.
- **FR-B-008 — Hot-reload rollback test.** A failed shader/config hot-reload shall **preserve the
  previous pipeline state** (no half-applied pipeline). A test shall feed a deliberately-broken
  reload (bad shader or schema-invalid config) and assert the previously-valid pipeline state is
  retained and the engine does not crash — covering the hot-reload requirement spec 015 NFR-004
  (`:213-214`) introduces.

## Non-Functional Requirements

- **NFR-001 — Determinism (hard gate).** `luminumbra_server_app --smoke` stays `6f008a9f637c40b7`,
  run==replay, before and after every FR. No FR in this spec feeds `world_hash`.
- **NFR-002 — Byte-identical config representation.** Group B is a representation change only: compiled
  defaults and the generated `config:v1:` string are byte-identical to today's (FR-B-005). The
  config-schema sub-hash stays additive/sim-only and re-pins nothing (per project memory:
  config sub-hash is additive, zero re-pin).
- **NFR-003 — Single-root invariant.** After Group A, no gate, harness, or tool reads from the root
  `build/` tree; everything resolves under `build/$BuildPreset`. Verified by FR-A-002's preflight and
  by a grep that no gate path string references a bare `build/bin` / `build/` artifact root.
- **NFR-004 — PowerShell + preset-tree execution.** All preflight, manifest, and tier-enumeration
  tooling must run under the PowerShell tool with `C:\msys64\ucrt64\bin` prepended, against the preset
  tree (`cmake --build --preset $BuildPreset` / `ctest --preset $BuildPreset`). The Bash tool is
  sandboxed (no GPU client, no compiler) and must not be assumed for build/gate steps.
- **NFR-005 — No silent test loss.** The tiering (FR-A-006) must make a conditionally-unregistered
  test (e.g. `VisualCritiqueFlags` when no numpy is found, `test/CMakeLists.txt:1487`) **report as
  missing**, not vanish. The gate must surface registered-vs-expected tier counts.
- **NFR-006 — De-risks specs 014 and 015.** Both 014 (`docs/specs/014-rhi-vulkan-dx12-migration/spec.md`)
  and 015 (`:344`) currently instruct building both trees. This spec's Group A is a prerequisite that
  removes that ambiguity for them; FR-A-007 corrects their text. Cross-referenced, not duplicated.
- **NFR-007 — Shader-ABI boundary (explicit).** This spec owns the **config** typed-schema/codegen +
  hash-behavior-declaration half of F8 (Group B). Shader-resource **reflection and layout validation**
  (reflected resource handles, constant-buffer layout generation, reflected-layout CI) are owned by
  **spec 016 — render framework** and are out of scope here (NG-1). FR-B-007 touches only scalar/vec
  *constant de-duplication*, which sits on the config side of that boundary.
- **NFR-008 — Determinism residency is structural (spec 018 link).** FR-B-004's "every value declares
  its hash behavior" is the structural enforcement of spec 018 — determinism hardening's
  render-vs-sim residency rule, which `SystemConfig.cpp:223` does manually today. Spec 018 owns the
  broader determinism contract; this spec contributes the schema-level enforcement for config.

## Acceptance Criteria

Each names a measurable signal or command. Gate/build/test commands run via the PowerShell tool with
`C:\msys64\ucrt64\bin` prepended, against the preset tree.

### Cross-cutting
- [ ] **AC-001** — `luminumbra_server_app --smoke` stays `6f008a9f637c40b7` (run==replay) after
  **every** FR in both groups.
- [ ] **AC-002** — The engine-frontier gate (`tools/gates/validate-engine-frontier.ps1 -Mode
  Build` then the test/gate modes) passes end-to-end against `build/debug` with the new preflight in
  place.

### Group A — operability
- [ ] **AC-A-001** — With both a root `build/CMakeCache.txt` and `build/debug/CMakeCache.txt` present,
  the preflight (FR-A-002) **fails** and names both trees + the intended preset. With only the preset
  tree present, it passes.
- [ ] **AC-A-002** — Every gate step resolves exe/shader/screenshot/CTest paths under `build/$BuildPreset`
  only; a grep over the gate script and tools shows no read from a bare root `build/` artifact path
  (FR-A-003 / NFR-003).
- [ ] **AC-A-003** — A fresh visual capture and a `--render-benchmark` JSON each carry a manifest with
  all six fields (`build_preset`, `git_sha`, `exe_hash`, `shader_hash`, `scenario`, `timestamp`)
  (FR-A-004).
- [ ] **AC-A-004** — A bless/compare attempt where the candidate artifact's `exe_hash` or `shader_hash`
  does **not** match the running binary is **refused** with the mismatched hashes named (FR-A-005).
  Construct the mismatch by capturing with one binary and gating against a rebuilt one.
- [ ] **AC-A-005** — `ctest --preset $BuildPreset -L manual --show-only` enumerates the manual tier
  (ForestPerfBudget, ShieldRtSpike, the ShieldRt*Gpu family); the gate reports registered-vs-expected
  counts, so an unregistered conditional test (numpy/Pillow-gated) shows as missing (FR-A-006 / NFR-005).
- [ ] **AC-A-006** — `grep -n "both trees\|build both" docs/specs/015-*/spec.md docs/specs/014-*/spec.md`
  returns no "build both trees" instruction after FR-A-007 (replaced with single-preset-tree text).

### Group B — config schema
- [ ] **AC-B-001** — The `SysKey`/`SysParam` enums and `kKeys`/`kParams` registries are generated from
  the schema; the hand-maintained parallel arrays in `SystemConfig.h/.cpp` are gone (grep shows they
  are now `#include`d generated content, not literal arrays).
- [ ] **AC-B-002** — A unit test feeds every enabled-system combination + the all-default state and
  asserts the generated serializer's `config:v1:` string is **byte-identical** to a captured snapshot
  of the current `ComputeConfigSubHash` output, including the empty-string all-default case
  (FR-B-005). `--smoke` unchanged.
- [ ] **AC-B-003** — A deliberately-introduced drift between a schema default and its owning-system
  constant (e.g. change one `Eco*` default) fails at **compile time or CI**, not silently (FR-B-003).
- [ ] **AC-B-004** — Each schema entry declares sim/render residency; flipping one render entry to
  "sim/hashed" in the schema changes the generated hash output exactly as the manual branch would
  have, proving the residency is schema-driven (FR-B-004).
- [ ] **AC-B-005** — At least one verified mirrored-default pair (e.g. a `SimEcology` `Eco*` default
  vs its `CreatureBrainSystem.h` constant, `SystemConfig.cpp:57`) has a single generated home; grep
  shows the value authored once and the "defaults MUST match …" hand-contract removed (FR-B-007).
- [ ] **AC-B-006** — A broken hot-reload (bad shader / schema-invalid config) leaves the previous
  pipeline state intact and does not crash; the rollback test passes (FR-B-008).

## Phasing (sequenced by risk-to-trust and prerequisite order)

1. **Group A — operability first.** It is the cheapest trust win and a prerequisite that de-risks
   014/015. Order within A: FR-A-002 preflight + FR-A-003 single-root resolution (stop reading the
   wrong tree), then FR-A-004/FR-A-005 manifests + hash-match refusal (stop blessing stale images),
   then FR-A-006 tiering + FR-A-007 doc corrections.
2. **Group B — config schema, lighting/exposure pilot first.** Per the F8 migration path: schema +
   codegen proven on the render-only lighting/exposure surface (FR-B-006), with byte-identical output
   gated (FR-B-005) before touching the sim keys. Then migrate the remaining keys, add the
   drift-fails-CI check (FR-B-003) and the schema-driven residency (FR-B-004), then the generated
   shared header (FR-B-007) and the hot-reload rollback test (FR-B-008).

Group A and Group B are independent and may proceed in parallel, but A's preflight + manifest work
should land first because every B verification run depends on knowing which binary produced the
`--smoke` / config-hash result.

## Blocking gates (per phase)

1. **Determinism (AC-001):** `--smoke == 6f008a9f637c40b7`, run==replay. Group B additionally gates on
   the byte-identical config-string snapshot (AC-B-002).
2. **Engine-frontier gate (AC-002):** the existing `validate-engine-frontier.ps1` lifecycle passes
   against `build/debug` with the new preflight wired in.
3. **Single-root invariant (NFR-003 / AC-A-002):** no gate reads the root `build/` tree.
4. **No silent test loss (NFR-005 / AC-A-005):** manual tier enumerable; conditional tests report as
   missing, never vanish.

## Open Questions

- **OQ-1 (blocks FR-A-001 deprecation mechanics)** — Should the root `build/` tree be *deleted* from
  the repo, *gitignored*, or merely *fail the preflight when configured*? Deleting it disturbs the
  committed `build/debug/test-artifacts/*` baselines' sibling history; the safest first step is
  preflight-refusal + documentation, with physical removal a later cleanup. Owner call.
- **OQ-2 (drives FR-A-004 hashing choice)** — What is `exe_hash` / `shader_hash` computed over —
  the binary file bytes + the concatenated shader-source hashes, or a build-id stamped at link time?
  The former is tool-only (no engine change); the latter is more robust but touches the build. Pick
  the cheapest that survives a rebuild-with-no-source-change (so a clean rebuild doesn't falsely
  invalidate a bless).
- **OQ-3 (blocks FR-B-001 codegen toolchain)** — What generates the config headers: a build-time
  Python/CMake codegen step (matches the existing python-conditional CTest pattern,
  `test/CMakeLists.txt:1458-1508`) or `constexpr`/X-macro C++ metaprogramming (no external generator,
  but harder to emit the shared shader header from)? The shared-header requirement (FR-B-007) leans
  toward an explicit codegen step.
- **OQ-4 (FR-B-003 enforcement site)** — Is the schema-default-vs-owning-constant cross-check a
  `static_assert` (requires the constant be `constexpr` and visible at config-compile time) or a CI
  test that diffs the two? Some owning constants live in shaders (not C++-visible), so a hybrid may be
  needed.
- **OQ-5 (FR-A-005 vs temporal capture)** — Manifest hash-match refusal must not false-positive when
  a capture is intentionally re-run against the same binary; define the match as `exe_hash` equality
  only (ignore timestamp/scenario for the *refusal* decision), with the other fields informational.
- **OQ-6 (spec 016 sequencing)** — FR-B-007's generated shared header de-duplicates scalar/vec
  constants now; spec 016 will later own the reflected *resource layout*. Confirm 016 will consume
  (not re-author) the constant header so the two specs don't fork the constant's home again.

## Key files
- `CMakePresets.json:13` — `"binaryDir": "${sourceDir}/build/${presetName}"`. The canonical preset
  tree (FR-A-001). The root `build/` tree is the deprecation target.
- `tools/gates/validate-engine-frontier.ps1` — `Test-Build` runs `cmake --build --preset
  $BuildPreset` (`:255`); `Test-UnitTests` runs `ctest --preset $BuildPreset -E "_NOT_BUILT$"`
  (`:264`); `Get-ClientExe` resolves `build/$BuildPreset/bin/luminumbra_client_app.exe` (`:308-314`).
  Wire the two-tree preflight (FR-A-002) here; extend single-root resolution (FR-A-003) beyond the
  client exe to shaders/screenshots/CTest manifest.
- `test/CMakeLists.txt` — manual/conditional tier: `ForestPerfBudget` (`:892`, `manual;perf;budget`),
  `ShieldRtSpike` (`:904`, `manual;perf;spike`), `ShieldRtTracerProfileGpu` (`:913`), the far-field
  GPU parity family (`:920`/`:927`/`:934`, `manual;perf;gpu`), python-conditional `VisualCritiqueFlags`
  (`:970`, skipped at `:974-976` when no numpy) and `TimelapseSelftest` (`:990`); per-artifact
  `build_preset` labeling (`:998-1007`). Tiering (FR-A-006) + manifest generalization (FR-A-004).
- `src/luminumbra_common/core/SystemConfig.h` — `SysKey` enum (`:48-70`), `SysParam` enum (`:73+`),
  the broad render+sim feature-flag/tuning surface. Generated from the schema (FR-B-001/002).
  (NOTE: the critique cited `.../systems/SystemConfig.h`; the real path is `.../core/SystemConfig.h`.)
- `src/luminumbra_common/core/SystemConfig.cpp` — canonical registry `kKeys` (`:34-51`) / `kParams`
  (`:53+`); the hand-maintained "defaults MUST match …" contracts (`:57, 73, 78, 82, 86, 94`);
  `ComputeConfigSubHash` (`:217-245`) with the manual render-skip residency branch (`:223`) and the
  all-default empty-string baseline (`:243`). Schema-generate the registry, residency, and serializer
  (FR-B-002/004/005), with byte-identical output.
- `res/shaders/lighting_pass.frag:461-462` — hardcoded `kMoonColor=(0.40,0.52,0.92)` /
  `kMoonKeyScale=1.3`; these are NOT currently config-driven (they differ from `SystemConfig
  MoonlightColor=(0.6,0.7,1.0)`), so they are a *follow-on* unify target once moonlight routes through
  config — not an existing C++/shader mirror (FR-B-007 note). The verified existing duplication is the
  C++/C++ "defaults MUST match" contracts in `SystemConfig.cpp`.
- `docs/specs/014-rhi-vulkan-dx12-migration/spec.md` (`:24`, `:163` single-source HLSL toward
  reflection) and `docs/specs/015-atmospheric-lighting-colored-glass/spec.md` (`:344` "build both
  trees"; `:201-218` exposure/froxel/OIT/colored-shadow/hot-reload NFR block) — cross-referenced;
  FR-A-007 corrects the build-both-trees text.
- `tools/flip_diff.py`, `tools/golden_update.py`, `tools/ppm_to_png.py` — the visual-parity gate that
  must enforce manifest hash-match before blessing (FR-A-005).
- **Future siblings (referenced, not modified):** `spec 016 — render framework` (owns shader-resource
  reflection/layout, NFR-007); `spec 018 — determinism hardening` (owns the render-vs-sim residency
  rule this spec enforces structurally, NFR-008).

## Verification (end-to-end)
1. Build the one canonical preset tree (`cmake --build --preset debug`, PATH-prepend
   `C:\msys64\ucrt64\bin`); the two-tree preflight refuses if a root `build/` cache also exists.
2. `--smoke` byte-identical (`6f008a9f637c40b7`, run==replay) after every FR; Group B additionally
   gates the byte-identical `config:v1:` snapshot (AC-B-002).
3. Engine-frontier gate (`validate-engine-frontier.ps1`) green against `build/debug` with the
   preflight wired in.
4. Capture a visual + a `--render-benchmark` artifact; confirm the six-field manifest, then confirm a
   stale-binary bless is refused (AC-A-004).
5. `ctest --preset debug -L manual --show-only` enumerates the manual tier; the gate reports
   registered-vs-expected counts (AC-A-005).
6. Grep confirms the "build both trees" instruction is gone from specs 014/015 (AC-A-006) and that the
   `SysKey`/`kKeys` arrays are generated, not literal (AC-B-001).
