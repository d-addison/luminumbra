# Pillar audit: Build/Test Operability & the Gate Suite — spec 020 + determinism/visual gates (Spec 021, 2026-07-02)

**Verdict.** This pillar is in far better shape than the 2026-06-28 merged roadmap assumed: spec 020
Group A (operability/preflight/provenance) and Group B (config-schema codegen) have **both
substantially landed** (commits `3fb3724e`/`789788db` and `14dbdf1a`, all 2026-06-28), and the spec
018 gate wave (MovingResidency, ReadbackDiscipline, DeterminismAudit) plus the spec 017 FR-G-001
render-readback ban shipped the same window — meaning the 018-E/F "gates before readback consumers"
spine prerequisite is already satisfied. The gate suite is broad (80 named `-Mode` gates + a 31-exe
ctest inventory + an 18-flag (8 server + 10 client) headless harness). The remaining debt is operational stitching, not
architecture: the two-tree preflight exists but is **not wired into the frontier gate** (FR-A-002
unmet as written), provenance binding covers only 2 of the visual/perf gates, the root `build/` tree
is still WARN-only, three sibling specs still instruct "build both trees", 25 tracked
`build/debug/test-artifacts/*` baselines churn `git status` on every configure/run, `-Mode All` is a
partial suite with no scheduled full run, and the CI workflows that exist build the deprecated root
tree and never execute (local-only branch).

## Current state + evidence

### The engine-frontier gate suite (the proving-signal substrate for every other pillar)

- `tools/gates/validate-engine-frontier.ps1` exposes **80 named gates** via
  `-Mode` (`tools/gates/validate-engine-frontier.ps1:2` — the `ValidateSet` list, from
  `Build`/`UnitTests` through `WorldVisualSweep`, `HeadlessServerTick`, `ReplicationSmoke`,
  `LockstepLoopback`, `RenderBudget`, up to the newest `ArtifactManifest`, `ConfigSchemaCheck`,
  `MovingResidency`, `ReadbackDiscipline`, `RenderReadbackAllowlist`, `DeterminismAudit`).
  Every pillar in the 021 map has at least one gate here (SHIELD: `ChunkCollisionLifecycle`,
  `PersistenceRoundtripGate`, `HeadlessServerTick`; render: `RenderHealth`, `WorldVisualSweep`,
  `RenderBudget`; GPU-SDF: the `GpuSdf*` trio; audio: `AudioNullTelemetry`, `AtmosphereAudio`,
  `BiomeReverb`; UI: `UiTestBaseline`; net: `ReplicationSmoke`, `NetworkedReplication`,
  `LockstepLoopback`; foliage: `FoliageInstancing`, `FarFieldForestBudget`; water:
  `RiverPresence`, `WaterfallVisual`; instinct: `InstinctPlannerGate`, `CreatureSlice`;
  atmospheric: `WeatherVisual`, `TimeOfDaySweep`, `WindFieldDeterminism`; aetheric:
  `AetherFieldDeterminism`, `ScalarFieldDiffusionGate`; physics: `PhysicsReplay`). The only
  pillar with **no** gate is GPU-modernization (014) — its GL↔Vulkan FLIP parity gate is
  chartered by 021 FR-E-003 and owned by the GPU pillar.
- `-Mode All` composition (`validate-engine-frontier.ps1:7308-7340`): 30 fast gates. It
  deliberately **excludes** `Build`, `UnitTests`, `MaterialVisual`, every visual sweep, every
  perf gate, every replication/lockstep gate, `HeadlessServerTick`, and `MovingResidency` —
  so a green `All` is a lint+artifact pass, **not** full coverage (see Gaps).
- The gate does not auto-build: `Get-ClientExe` throws "Run -Mode Build first"
  (`validate-engine-frontier.ps1:313-319`); `Test-Build` runs `cmake --build --preset
  $BuildPreset` (`:260`) after a config-schema freshness preflight (`:251-255`).
- Capture pin: `RenderBudget` budgets are calibrated at 3840x1600 / RTX 5070 Ti
  (`validate-engine-frontier.ps1:6500`).

### Spec 020 Group A — operability (LANDED, with unwired edges)

- **FR-A-001/002/004/005 preflight + manifest**: `tools/gates/validate-build-tree.ps1`
  (11 KB, committed 2026-06-28) implements the canonical-preset-tree preflight: wrong-tree
  `CMAKE_CACHEFILE_DIR` refusal (`validate-build-tree.ps1:101-109`), two-tree detection
  (`:117-125`), opt-in stale-binary refusal via `-ExpectExeHash` (`:56`, `:138-140`), and a
  six-field provenance manifest (`build_preset`/`git_sha`/`exe_hash`/`shader_hash`/`scenario`/
  `timestamp`, `:194-213`). Per spec 020 OQ-1 (owner call), the concurrent root `build/` tree
  is a **WARNING by default** and hard-fails only under `-Strict` (`:27-32`, `:119-124`).
- **FR-A-004/005/006 in the gate**: `Test-ArtifactManifest`
  (`validate-engine-frontier.ps1:6715-6846`) emits the six-field manifest, proves the
  same-binary compare passes and a corrupted `exe_hash` is REFUSED with both hashes named
  (`:6744-6762`, `:6836-6841`), and enumerates the manual GPU/perf tier
  registered-vs-expected (`:6766-6789`). Shared plumbing: `New-ArtifactManifest` (`:6582-6598`),
  `Assert-ManifestMatchesBinary` (`:6600-6626`), and `Assert-ArtifactProvenance` (`:6656-6689`)
  with gitignored sidecars so a captured `exe_hash` can never be committed and false-refuse a
  fresh build (`:6641-6654`).
- **FR-A-005 wiring into real gates is PARTIAL**: provenance binding is live in
  `Test-MaterialVisual` (`validate-engine-frontier.ps1:341`) and `Test-RenderHealth` (`:488`)
  only — `RenderBudget` (`--render-benchmark` perf JSON) and the visual-sweep family are not
  yet bound (grep shows exactly those two call sites plus the E2E demo at `:6814-6831`).
- **FR-A-002 as written is UNMET**: the spec requires the engine-frontier gate to invoke the
  two-tree preflight before any build/test step
  (`docs/specs/020-build-test-operability-config-schema/spec.md:99-104`), but
  `validate-engine-frontier.ps1` never calls `validate-build-tree.ps1` (grep for
  `build-tree|CMakeCache` in the script: no matches; `Test-Build`'s only preflight is the
  config-schema check, `:251-255`).
- **FR-A-006 manual tier**: `ForestPerfBudget` (`test/CMakeLists.txt:967-969`,
  `manual;perf;budget`, intentionally RED pending impostor atlas per `:959-964`),
  `ShieldRtSpike` (`:979-981`), `ShieldRtTracerProfileGpu` (`:988-990`), the far-field GPU
  parity trio (`:995`, `:1002`, `:1009`), python-conditional `VisualCritiqueFlags`
  (`:1045-1050`) and `TimelapseSelftest` (`:1065-1070`). NOTE: spec 020's line citations
  (`spec.md:121-129`, citing `test/CMakeLists.txt:892/:904/:913`) have **drifted** — the file
  grew; the registrations now live at `:967+`.
- **FR-A-007 doc fix applied to 014/015 but NOT to siblings**: grep for "build both" finds no
  hit in specs 014/015, but the instruction survives in
  `docs/specs/016-render-framework-frame-graph/spec.md:239`,
  `docs/specs/018-determinism-hardening/spec.md:434`, and
  `docs/specs/019-networking-scale-out/spec.md:230` + `:386`. `docs/STANDARDS.md:175-177`
  likewise still teaches "Two build trees — build the one you test" rather than
  one-canonical-tree.

### Spec 020 Group B — config schema codegen (LANDED, full surface, two FRs open)

- **FR-B-001/002/004**: `src/luminumbra_common/core/ConfigSchema.json` (423 lines; 16 keys at
  `:4`, 45 params at `:102`; `_comment` at `:3` declares residency semantics) is the single
  source of truth. `SystemConfig.h:24` includes the generated
  `SystemConfigRegistry.gen.h`; the `SysKey`/`SysParam` enums expand from
  `LUMIN_CONFIG_KEY_TABLE`/`PARAM_TABLE` (`SystemConfig.h:57`, `:68`), and the `kKeys`/`kParams`
  registries expand the same tables (`SystemConfig.cpp:43`, `:50`). This is the **full
  registry**, not just the FR-B-006 lighting/exposure pilot. Residency (`hashed`/`excluded`)
  is schema-declared per key (`ConfigSchema.json:4+`; `tools/config_codegen.py:39-43`).
- **FR-B-003 (drift gate, schema↔header half)**: `tools/config_codegen.py --check` runs at
  configure time and hard-fails on drift (`src/luminumbra_common/CMakeLists.txt:16-48`), and
  again in the gate via `Assert-ConfigSchemaFresh` / `-Mode ConfigSchemaCheck`
  (`validate-engine-frontier.ps1:6691-6713`). The **owning-system-constant half is open**:
  schema params carry `owner` = the SysKey enum only, no reference to the mirrored constant
  (`ConfigSchema.json:102+`; `tools/config_codegen.py:115-127`), so a drift between e.g. an
  `Eco*` schema default and its `CreatureBrainSystem.h` constant is still silent. (The old
  hand-written "defaults MUST match …" comments are gone from `core/` — grep: no matches —
  the contract moved into the schema without a cross-check.)
- **FR-B-005 byte-identity**: `test/common/SystemConfig_test.cpp:129-185` reconstructs the
  canonical `config:v1:` byte string (`:138`) and asserts `ComputeConfigSubHash` equality for
  the all-default empty case (`:84`, `:185`) and enabled-system combinations (`:149-182`).
  `--smoke` baseline unchanged (`6f008a9f637c40b7` DEBUG / `ea9a0121d13bc3bd` RELEASE, pinned
  at `validate-determinism-matrix.ps1:39-40`).
- **FR-B-007 / FR-B-008 open**: no generated shared C++/shader constant header exists yet (the
  spec itself notes the moonlight shader constants are a follow-on, `spec.md:344-348`); the
  hot-reload rollback requirement has a shader-side rollback-predicate test
  (`test/rendering/shader_reflection_test.cpp:217-220`) but no config-side schema-invalid
  reload rollback test.

### Spec 018 gate infrastructure (determinism matrix + the E/F gates)

- **Determinism matrix** (`tools/gates/validate-determinism-matrix.ps1`): axes = worker
  count {1,2,4}, multiprocess one-box, record/replay, build-mode debug+release with
  **per-build baselines** (`:10-19`, `:39-40`), plus the `--smoke-moving` convergent oracle as
  a default axis (`:43-47`, `:127-136`). FR-D-002 (fast/slow-job throttle) is explicitly
  SKIPPED pending an engine hook (`:17-19`, `:165-166`).
- **MovingResidency** gate (`validate-engine-frontier.ps1:6848-6915`): `--smoke-moving
  --avail-trace --ticks 90`, gating on final-hash run==replay (convergent oracle; per-tick
  availability match is report-only by design, `:6900-6912`).
- **ReadbackDiscipline** (018 FR-E-001, `:6983-7010`): static scan of the 12 sim/hash-path
  roots (`:6924-6937`) for synchronous GL readback primitives (`:6962`); zero offenders,
  empty allowlist (`:6994-6999`).
- **RenderReadbackAllowlist** (017 FR-G-001, `:7026-7076`): narrow blocking-readback matchers
  for render code (`:7036-7038`), with exactly one allowlisted site — the RenderPipeline
  GPU-SDF readback, to retire after 017-B (`:7031-7033`).
- **DeterminismAudit** (018 FR-F, `:7078-7226`): six machine checks — ResidencyContract.h
  declared (`:7100-7119`), `render.*` never hashed (`:7121-7132`), no render term in
  `ComposeWorldHash` (`:7134-7152`), `.mesh` excluded from the determinism match
  (`:7154-7171`), no sim-path readback (`:7173-7179`), froxel/auto-exposure identifiers
  banned from the sim path (`:7181-7214`). This is the gate specs 015/016 pass through.

### ctest inventory

- 31 test executables (`test/CMakeLists.txt:2-891`, `add_executable` sites), 21 registered
  through `gtest_discover_tests` with a 120 s cold-launch discovery timeout
  (`test/CMakeLists.txt:920-957`); UI targets promote `GTEST_SKIP` to FAILURE so a GL-less
  runner cannot go green with zero coverage (`:944-953`).
- The configure-time `ctest_manifest.json` baseline floor is **10** executables / 10 tests
  (`test/CMakeLists.txt:1094-1101`) against the actual 31/21 — a two-thirds-loss blind spot.
  The `UiTestBaseline` gate consumes the manifest (`validate-engine-frontier.ps1:1561-1565`).

### Headless harness inventory (verified against the tree)

- Server (`src/luminumbra_server/main_server.cpp`): `--smoke` (`:242`), `--record`/`--replay`
  (`:248-250`), `--ticks` (`:270`), `--avatars` (`:278`), `--smoke-moving` (`:284`),
  `--avail-trace` (`:287`), `--replicate` (`:290`), `--artifact` (`:322`).
- Client (`src/luminumbra_client/main_client.cpp`): `--render-benchmark` (`:2678`),
  `--play-paths` SLOWFRAME profiler (`:2682`), `--scene-config` (`:2702`), `--frame-scan`
  (self-implies the auto-world, `:2754-2762`; hang watchdog `kFrameScanWatchdogFrames=4000`,
  `:281`, enforced at `:6816-6823`), `--survey` POI tour (`:2796`), `--ui-screenshot`
  (`:2807`), `--worldgen-graph` (`:3606`), `--timelapse` capture (`:192-228`), `--no-audio`
  (`src/luminumbra_client/core/RuntimeScenarioHarness.cpp:170`), `--crash-dir` (`:258`).
  There is **no single operator document** enumerating this surface; the knowledge lives in
  scattered comments and session memory.
- Visual diffing: `tools/flip_diff.py` (golden-image perceptual diff, tiered
  FLIP/SSIM/luma/stdlib backends, default `--threshold 0.05`, `tools/flip_diff.py:38-45`) —
  run-to-run full-frame FLIP is noisy (~0.057 observed on identical trees; use the in-process
  same-frame mode + frame-health `nominal` verdicts for gating, per project memory — the
  0.05 default is below the observed cross-run noise floor and only safe same-process).

### Crash diagnostics & hang observability (operability substrate)

- Persistent per-session log: `logs/luminumbra.log` truncated on boot so it always holds the
  crashed session (`src/luminumbra_common/core/Log.cpp:31-36`); symbolized crash report
  `crash-<ts>.txt` (`src/luminumbra_client/main_client.cpp:2028`), breadcrumbs flushed before
  symbolization (`:2136`); offline symbolizer `tools/gates/symbolize-crash.ps1` (committed
  2026-06-28).
- **DISCREPANCY vs KNOWN CONTEXT**: the brief listed `LUMINUMBRA_JOB_WATCHDOG` as *missing*.
  The tree contradicts this — it **exists** as an opt-in, hash-neutral wedge watchdog at
  `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:2964-2996` (names the wedged phase +
  chunk neighbourhood every 30 s). Its coverage is partial: it wraps only the collision-build
  batch wait (`:2972-2996`); the two earlier unbounded waits in `EnsureSurfaceReadyNear`
  (`wait_for_generation_jobs`/`wait_for_meshing_jobs`, `:2834-2836`) have one-shot breadcrumb
  logs only. The hang root-cause fix itself is SHIELD-owned (017-B).

### TDD discipline & CI

- The proving-signal law: `test/features/TDD-LOCK.md:41-49` (locked-gates table) and
  `docs/STANDARDS.md:155-169` (§7: test-first, SDD trace, mandatory sim determinism test,
  WorldVisualSweep for visual debt).
- CI **exists but is dormant and wrong-tree**: `.github/workflows/ci.yml` configures the
  deprecated root `build/` tree (`ci.yml:53-59`, `cmake -S . -B build`), has no
  msys64/preset awareness, and its `forge-verify` job invokes `forge` without installing it
  (`ci.yml:72-73`); `asan.yml:45-54` builds `build/asan` on Linux clang. Neither can have run
  against this work: the branch is local-only, never pushed (`docs/STANDARDS.md:189`). The
  KNOWN-CONTEXT claim "no CI" is therefore *effectively* true but literally false — workflows
  exist and would recreate the two-tree hazard if ever enabled.

### Shipped since the 2026-06-28 roadmap

The roadmap scheduled 020-A/B in Wave 0 and 018-E/F in Wave 2; all landed 2026-06-28/29
(verified `git log`):

- **020-A operability**: artifact provenance manifest + running-binary refusal + manual-tier
  enumeration (`3fb3724e`, 2026-06-28 14:07); provenance wired into MaterialVisual/RenderHealth
  (`789788db`, 15:03); `validate-build-tree.ps1` preflight (same window).
- **020-B config codegen**: full `ConfigSchema.json`-driven registry (`14dbdf1a`, 14:03);
  configure-time + gate drift checks.
- **018 C/E/F gate wave**: MovingResidency axis, ReadbackDiscipline, DeterminismAudit
  (`8c0dbf9d`, 14:01); per-build matrix baselines fix (`13f7ec5b`, 13:22).
- **017-A + FR-G-001**: AsyncReadbackRing + the render-side readback ban gate
  (`ca2616d8`/`3bba2a52`, 2026-06-29) — the gate half is ops-owned and live
  (`validate-engine-frontier.ps1:7026-7076`).

Consequence for the spine: the "018-E/F before readback consumers (015 A-T06, 016 FR-E)"
prerequisite is **already satisfied**; those consumers are unblocked from this pillar's side.

## Gaps / debt

1. **FR-A-002 unwired** — the two-tree preflight script exists but the frontier gate never
   invokes it (`validate-engine-frontier.ps1:251-264` runs only the config-schema preflight;
   spec requires gate wiring at `docs/specs/020-build-test-operability-config-schema/spec.md:99-104`).
2. **FR-A-005 partial rollout** — provenance binding covers MaterialVisual + RenderHealth only
   (`:341`, `:488`); `RenderBudget` perf JSON and the visual-sweep family are unbound.
3. **Root `build/` tree still live** — preflight WARN-only (`validate-build-tree.ps1:119-124`),
   `docs/STANDARDS.md:175-177` still documents the two-tree workflow, and `ci.yml:53` would
   rebuild the root tree. The stale-binary hazard the preflight was built for remains open in
   default operation.
4. **"Build both trees" survives in specs 016/018/019** (`016:239`, `018:434`, `019:230/:386`)
   — FR-A-007 fixed only 014/015.
5. **FR-B-003 owning-constant half + FR-B-007 shared header + FR-B-008 config-side rollback
   test open** (`ConfigSchema.json:102+` has no constant references;
   `shader_reflection_test.cpp:217-220` covers only the shader side).
6. **Tracked test-artifact churn** — 25 files under `build/debug/test-artifacts/` are tracked
   (`git ls-files build/` = 25) and are re-written in place by configure
   (`test/CMakeLists.txt:1094-1102`) and by gate runs, so `git status` is dirty after every
   local run (12 modified at audit time) and an accidental `git add -A` silently re-blesses
   baselines. Spec 020 NG-5 keeps the baselines; it does not require outputs to overwrite them.
7. **No full-suite run exists** — `-Mode All` excludes Build/UnitTests/visual/perf/replication/
   heavy-determinism gates (`validate-engine-frontier.ps1:7308-7340`); nothing schedules the
   heavies; CI is dormant. Suite health is only as good as the last ad-hoc invocation.
8. **ctest manifest floor stale-low** — minimum 10 vs the actual 31 executables / 21 gtest
   targets (`test/CMakeLists.txt:1094-1101`).
9. **FR-D-002 matrix axis skipped** — no engine job-throttle hook
   (`validate-determinism-matrix.ps1:17-19`, `:165-166`).
10. **No harness operator doc** — the 18-flag (8 server + 10 client) headless surface (see
    inventory above) is undocumented as a unit.
11. **Spec 020 line citations drifted** — e.g. `spec.md:121-129` cites `test/CMakeLists.txt:892+`
    for registrations now at `:967+` (cosmetic, but the spec is the operability contract).

## Risks

- **Stale-binary blesses remain possible by default** (gaps 1-3 combined): a gate mode invoked
  without `-Mode Build`, against a tree also carrying a root `build/` cache, still only warns.
  The provenance sidecars mitigate this for the two wired gates only. Severity: medium —
  this is exactly the F7 trust failure spec 020 was written against.
- **Baseline corruption via artifact churn** (gap 6): a routine commit sweeping `git status`
  can re-bless run-varying JSON/PPM baselines with no review signal. Severity: medium;
  probability grows with autonomous-agent commit cadence (STANDARDS §9 commit-per-slice).
- **Silent suite decay** (gaps 7-8): with no scheduled run and a floor of 10, losing whole
  test families (a CMake conditional, a Python-less box) is invisible until someone happens to
  run the right mode. `VisualCritiqueFlags` already demonstrates the conditional-registration
  failure shape (`test/CMakeLists.txt:1045-1050`).
- **Determinism gate blind spot on job timing** (gap 9): the one axis the matrix skips —
  fast/slow jobs — is precisely the axis the water-lockstep desync class lives in; the
  `--smoke-moving` oracle covers arrival order but not artificial starvation.
- **CI drift hazard**: if workflows are ever enabled as-is, `ci.yml:53` reintroduces the root
  tree on every runner and gates nothing that matters (no preset, no frontier modes).

## Opportunities

- The provenance substrate (`Assert-ArtifactProvenance`) is generic — extending it to
  `RenderBudget` + sweeps is a few call-sites, buying full FR-A-005 for ~zero new machinery.
- A single scheduled local "nightly" (Build → UnitTests → All → matrix `-Quick` →
  RenderBudget → one visual sweep) turns the existing suite into a regression net without
  building real CI; the report artifact doubles as the orchestrator's wave-boundary evidence
  for spec 021 Group F.
- `validate-build-tree.ps1 -Strict` is already written — retiring the root tree is mostly a
  docs + `ci.yml` + habit change, after which the preflight flips to hard-fail for free.
- The job watchdog pattern (30 s named-phase reporter, `SHIELD_WorldSystem.cpp:2964-2996`) is
  reusable for every unbounded wait the 017-B work will touch; generalizing it now gives the
  Wave-1 concurrency work (HIGH hash risk) its observability before the risky changes land.
- The schema codegen (`tools/config_codegen.py`) already parses C++ initializer tables — the
  FR-B-007 shared-header emitter is an incremental `--emit` target, and spec 015's
  exposure/froxel constants can onboard straight into the schema as they land.

## Backlog items

| id | summary | spec | effort | risk | deps | status | proving_signal |
|----|---------|------|--------|------|------|--------|----------------|
| OPS-01 | 020-A operability core landed: build-tree preflight script, six-field provenance manifests, stale-capture refusal (incl. E2E visual-gate binding), manual-tier enumeration | 020 | L | low | — | done | `validate-engine-frontier.ps1 -Mode ArtifactManifest` |
| OPS-02 | 020-B config codegen landed full-surface: ConfigSchema.json (16 keys/45 params) generates SysKey/SysParam + kKeys/kParams; configure-time + gate drift checks; byte-identical config:v1: tested | 020 | L | low | OPS-01 | done | `validate-engine-frontier.ps1 -Mode ConfigSchemaCheck` + ctest `common_tests` (SystemConfig_test byte-identity) |
| OPS-03 | 018-C/E/F + 017 FR-G-001 gate wave landed: MovingResidency, ReadbackDiscipline, DeterminismAudit, RenderReadbackAllowlist + per-build matrix baselines — unblocks 015 A-T06 / 016 FR-E from this pillar's side | 018 | L | low | — | done | `validate-engine-frontier.ps1 -Mode DeterminismAudit` (+ `-Mode MovingResidency`, `validate-determinism-matrix.ps1 -Quick`) |
| OPS-04 | Wire the FR-A-002 two-tree preflight into the frontier gate and extend FR-A-005 provenance binding to RenderBudget + the visual-sweep gates | 020 | S | low | OPS-01 | todo | NEW: engine-frontier Build-mode preflight — `-Mode Build` invokes validate-build-tree.ps1 and fails on a wrong-tree/copied CMakeCache; `-Mode RenderBudget` refuses a perf JSON whose exe_hash mismatches the gated binary |
| OPS-05 | Retire the root `build/` tree: flip the preflight to -Strict, rewrite STANDARDS.md §8 to one-canonical-tree, and fix/disable the root-tree build in ci.yml | 020 | M | medium | OPS-04 | todo | NEW: frontier BuildTreeStrict gate — validate-build-tree.ps1 -Strict exits 0 in the gate lifecycle (hard-fails on a concurrent root build/CMakeCache.txt) |
| OPS-06 | Extend the FR-A-007 "build both trees" correction to specs 016/018/019 (016:239, 018:434, 019:230/:386) and refresh spec 020's drifted test/CMakeLists.txt line citations | 020 | S | low | — | todo | NEW: engine-frontier Sections doc-lint — an Assert-Contains-style negative check fails when 'build both trees' text remains in docs/specs/016-,018-,019-*/spec.md |
| OPS-07 | FR-B-003 owning-constant half: schema params reference the constant they mirror and a CI check fails on drift (e.g. Eco* default vs its CreatureBrainSystem.h constant) | 020 | M | medium | OPS-02 | todo | NEW: ctest ConfigOwnerDriftCheck — deliberately drifting one Eco* schema default vs its owning-system constant turns the check RED; current tree GREEN |
| OPS-08 | FR-B-007 generated shared constant header (config-driven C++/shader scalar-vec constants, moonlight first) + FR-B-008 config-side hot-reload rollback test (shader-side predicate already exists) | 020 | M | medium | OPS-07 | todo | NEW: ctest ConfigHotReloadRollback — a schema-invalid config reload preserves the previous applied state without crash; grep shows the mirrored constant authored once in the generated header |
| OPS-09 | Charter a scheduled local full-gate run (no CI executes; -Mode All excludes Build/UnitTests/visual/perf/replication/heavy-determinism): nightly Build → UnitTests → All → matrix -Quick → RenderBudget with a dated report artifact | new | M | low | OPS-04 | todo | NEW: ScheduledGateRun report gate — a dated .forge/artifacts/nightly/<date>.json exists with all stages green; a missing/red report fails the check |
| OPS-10 | Fix tracked test-artifact churn: 25 committed baselines under build/debug/test-artifacts are overwritten in place by configure/gate runs, dirtying git status every run and risking silent re-bless; split run outputs (gitignored) from committed baselines | 020 | M | medium | — | todo | NEW: clean-tree gate — after `-Mode All` + a preset configure, `git status --porcelain build/debug/test-artifacts` is empty |
| OPS-11 | Job-watchdog observability: LUMINUMBRA_JOB_WATCHDOG landed (contradicts prior "missing" note) but covers only the collision-build wait; extend to the generation/meshing waits in EnsureSurfaceReadyNear and document it in the ops/harness doc (hang root-cause fix itself is SHIELD/017-B) | 017 | S | low | — | in-progress | NEW: ctest JobWatchdogNamesWedgedPhase — with LUMINUMBRA_JOB_WATCHDOG=1 and an injected stalled job, each of the three EnsureSurfaceReadyNear waits emits its named-phase warning within the report interval |
| OPS-12 | Raise the ctest_manifest baseline floor from 10 to the real roster (31 executables / 21 gtest targets) and derive required_executables from the CMake target list instead of a hand list | 020 | S | low | — | todo | NEW: re-pinned ctest-manifest floor — UiTestBaseline gate fails when registered executables < the derived roster count |
| OPS-13 | Implement the FR-D-002 fast/slow-job determinism axis: a test-only job-throttle hook (env-gated, sim-untouched) so the matrix can starve workers and still assert run==replay + baseline hash | 018 | M | medium | — | todo | NEW: determinism-matrix fast-slow axis — `--smoke` under LUMINUMBRA_JOB_THROTTLE reproduces 6f008a9f637c40b7 (DEBUG) run==replay; matrix SKIP note removed |
| OPS-14 | Crash-diagnostics substrate landed (persistent truncate-on-boot logs/luminumbra.log, symbolized crash-<ts>.txt, symbolize-crash.ps1) but has no regression test | new | S | low | — | done | NEW: ctest CrashDiagnosticsSelftest — an induced fault in a harness child process yields crash-<ts>.txt with ≥1 symbolized engine frame and a flushed luminumbra.log |
| OPS-15 | Author the harness operator doc: one page enumerating the 18-flag (8 server + 10 client) headless surface (server --smoke/--smoke-moving/--record/--replay/--ticks/--avatars/--avail-trace/--replicate/--artifact; client --frame-scan/--scene-config/--ui-screenshot/--render-benchmark/--play-paths/--survey/--worldgen-graph/--timelapse/--no-audio/--crash-dir) + the gate each feeds + the FLIP noise rule (in-process same-frame only; cross-run ~0.057 > the 0.05 default threshold) | new | S | low | — | todo | NEW: frontier Files-mode doc check — Assert-FileExists docs/operability/harness.md + Assert-Contains for every shipped harness flag |
