# Developer-experience tooling research

Research date: 2026-08-21

This report is deliberately limited to developer tooling. It does not infer engine architecture from `README.md`, and it proposes no simulation or content change. Where a recommendation could change test scheduling around deterministic code, the `world_hash` contract is called out explicitly.

Effort estimates use **S** (1–2 engineer-days), **M** (3–5 engineer-days), and **L** (1–2 engineer-weeks). Risk estimates describe adoption risk after implementation, not implementation difficulty.

## Current state

- `CMakePresets.json` defines a hidden `base` configure preset with Ninja, `binaryDir` set to `build/${presetName}`, and `CMAKE_EXPORT_COMPILE_COMMANDS=ON`. Therefore a debug configure produces the authoritative database at `build/debug/compile_commands.json`; debug-ASan, release, and coverage have their own databases. The task inventory reports a stale `build/compile_commands.json`. That root copy is especially hazardous because clangd normally searches source ancestors and a direct `build/` child, but not an arbitrary nested preset directory such as `build/debug/`; without an explicit database directory it can select the stale copy or fall back to incomplete flags.
- `scripts/lint.sh` and `scripts/lint.ps1` already provide equivalent cross-platform format and tidy entry points. They scan first-party C/C++ files below `src`, `include`, and `test`, exclude common vendored/build directories, run `clang-format --dry-run --Werror`, and run clang-tidy for translation units. Both default to `build`, while the preset convention puts the usable database in `build/debug`; callers must currently supply `--build-dir build/debug` or `-BuildDir build/debug`.
- `.clang-tidy` enables bug-prone, Clang analyzer, misc, performance, and portability families plus a small set of guideline/modernization checks. `WarningsAsErrors` is empty, so merely invoking the current tidy driver is not yet a blocking warning gate. The toolchain version is also not pinned in the inspected files; version drift can change enabled wildcard checks and diagnostics.
- `docs/CLAUDE.md` documents the preset tree as canonical, says the retired root `build` tree is rejected by a `BuildTreeStrict` preflight, and instructs developers to run CTest serially because `ctest -j` causes false failures. The task inventory quantifies the suite at roughly 1,800 cases registered by a roughly 1,700-line `test/CMakeLists.txt`; those two figures were supplied to this task rather than independently re-read because that file is outside the allowed read scope.
- `CMakePresets.json` provides a separate coverage tree and explicitly says it is never used for determinism/`world_hash` gates. The task inventory also identifies `tools/coverage_summary.py`; its implementation was outside this task's allowed read scope. Coverage is therefore an existing reporting lane, not a substitute for static diagnostics or deterministic test gates.
- The task inventory states that Banso can enrich dispatched work with clangd-backed `lsp_diagnostics` and diagnostic-delta baselines. No repository-local Banso configuration was in the permitted read set, so the wiring below is a concrete integration design based on that declared capability, not a claim that it is already enabled.

The upstream behavior behind these conclusions is well-defined: clang-tidy consumes a compilation database and its official parallel runner can analyze the database corpus; clangd supports an explicit compilation-database directory; CTest can select numbered tests with `-I`; and pre-commit supports local hooks, file filtering, and multiple Git stages. See [Sources](#sources).

## Candidate integrations

### 1. Authoritative clang-tidy CI job

**Delivery form:** a dedicated CI job (for example, `devx-clang-tidy`) plus a small extension to both lint drivers so CI can select a preset database, pin a supported LLVM major, emit a machine-readable log/artifact, and choose whether warnings are fatal.

Configure with `cmake --preset debug`, then analyze against `build/debug/compile_commands.json`. Start with a non-blocking full-corpus baseline using the repository `.clang-tidy`; triage or suppress existing findings; then make new diagnostics blocking and run a scheduled full-corpus gate. For pull requests, analyze every changed translation unit and translation units affected by changed first-party headers. Do not use `clang-tidy-diff.py` as the only gate: upstream documents that it filters diagnostics to changed lines after analyzing the full file and can miss effects reported elsewhere. A periodic `run-clang-tidy.py -p build/debug` job closes that gap and can use bounded parallelism.

Keep clang-format as a separate fast CI step so format failure is obvious and does not wait behind analysis. Pinning LLVM is necessary because `.clang-tidy` uses wildcard families whose membership evolves.

**Effort:** M. **Risk:** M — the main risks are initial diagnostic volume, platform/compiler-command incompatibilities, and tool-version churn. Roll out report-only first and promote a frozen baseline only after representative Linux and Windows commands parse correctly.

### 2. Banso clang-tidy verify domain

**Delivery form:** a Banso `verify` domain named `cpp-static` that wraps the same preset-aware lint entry point and records diagnostic deltas for dispatched C++ tasks.

Use this as the low-latency task gate, not as a replacement for CI. The domain should preflight `build/debug/compile_commands.json`, reject the stale root database, select touched translation units plus dependents of touched headers, and compare normalized diagnostics against a baseline keyed by LLVM major and configure preset. It should never apply `-fix` in verification. Missing tools, an absent/failing configure, or an unusable database must be reported as infrastructure failure rather than “zero diagnostics.” CI remains authoritative for full-corpus and cross-platform analysis.

**Effort:** M. **Risk:** M — delta normalization and header-to-translation-unit expansion can create false additions or omissions. Share tool version, path normalization, and suppression policy with the CI job to prevent two competing definitions of clean.

### 3. clangd LSP enrichment for Banso dispatch

**Delivery form:** a repository Banso dispatch profile/enrichment block plus a cross-platform preflight/launcher that starts clangd with `--compile-commands-dir=build/debug` (or the equivalent LSP initialization option) and attaches `lsp_diagnostics` to dispatched C++ tasks.

Recommended wiring:

1. Before dispatch, ensure `cmake --preset debug` has succeeded in that worktree.
2. Resolve and validate the absolute `build/debug/compile_commands.json`; require JSON parseability and reject `build/compile_commands.json` as an input. Do not copy or symlink a preset database into `build/`, because that recreates ambiguous/stale ownership across presets.
3. Start a workspace-scoped clangd process with the explicit database directory and a pinned LLVM major. Capture its log and selected database path in enrichment metadata.
4. Request diagnostics for touched C/C++ files. For headers, also open a bounded set of associated translation units from the database because compilation databases generally have entries for source files, not headers.
5. Normalize workspace-absolute paths and compare `(file, range, severity, diagnostic code, message)` against the task's baseline. Attach added/resolved/unchanged counts; fail only on policy-defined added errors after an observation period.
6. If configuration or database validation fails, mark enrichment unavailable with the reason. Never silently fall back to the stale root database or clangd's generic `clang some_file.cc` command.

This makes dispatch diagnostics fast and local to the change while preserving the clang-tidy CI job as the comprehensive gate. clangd and clang-tidy should use the same `.clang-tidy` and LLVM major, but their baselines should remain distinct because their diagnostic surfaces and file-opening behavior differ.

**Effort:** M. **Risk:** M — the largest risks are database freshness, header inference, and differences between the database's compiler driver and clangd. Explicit selection and provenance metadata make those failures visible.

### 4. Isolated, time-balanced CTest sharding

**Delivery form:** a CI shard-planning script and matrix job that generate CTest `-I` selection files from the freshly configured test inventory and historical durations, then run every shard serially with `ctest --preset debug -j1` in a fully isolated checkout/build/runtime directory.

Do not run multiple shards against one worktree or one build tree. `RUN_SERIAL` only constrains scheduling inside one CTest process; separate CTest processes can still overlap. Sharding is safe to trial only when each CI job also has isolated temp/data directories, ports, user state, and external service namespaces. If that isolation cannot be demonstrated, retain one serial lane.

Use a longest-processing-time allocation over recent test durations rather than equal contiguous ranges: place the currently slowest remaining test into the shard with the lowest assigned duration. Generate selections after configure so numeric IDs match the exact `CTestTestfile.cmake`; CTest's long-standing `-I` syntax accepts explicit test numbers and an input file, avoiding fragile name regexes and command-line length limits. The planner must prove that the union contains every discovered test exactly once and no unknown IDs. Each shard still uses `-j1`, uploads its log, and preserves the normal failure exit code. Retain a scheduled unsharded serial run while confidence is built, because shard concurrency may reveal hidden cross-checkout dependencies.

**`world_hash` contract:** this candidate must not change seeds, fixtures, compiler options, simulation inputs, or expected hashes. Any attempt to make tests shardable by changing ordering, shared fixtures, or deterministic state requires explicit before/after `world_hash` parity evidence and must leave the existing determinism gates intact. The coverage preset remains separate and must not feed `world_hash` decisions.

**Effort:** L. **Risk:** H — useful speedup depends on proving isolation behind the current serial-only rule. Start with two shards in non-blocking CI, compare results with the unsharded lane, and expand only after repeated parity.

### 5. Build-tree hygiene automation

**Delivery form:** extend the existing `BuildTreeStrict` preflight with a read-only “doctor” mode and add a small CI/Banso preflight that validates the selected preset tree and compile database.

The check should fail with remediation when it finds configured artifacts directly under retired `build/`, especially `build/CMakeCache.txt` or `build/compile_commands.json`; print the owning preset path and expected configure command; verify that the chosen database exists beneath exactly one selected preset tree; and inspect its `directory`/`file` entries for paths outside the current worktree or active build tree. Provide a separate, explicit cleanup command that lists exact targets and asks for confirmation locally. Do not make hooks or dispatch delete build trees automatically.

Run the check before clangd enrichment, clang-tidy verification, and local pre-push tidy. That turns the current stale-database problem into one actionable error and establishes a shared prerequisite for candidates 1–3.

**Effort:** S. **Risk:** L — detection is read-only and builds on an existing invariant. The main risk is rejecting legitimate multi-preset trees, so distinguish “multiple preset directories exist” from “ambiguous active database” and encode the actual `BuildTreeStrict` policy in one shared implementation.

### 6. Tiered pre-commit hooks

**Delivery form:** a pinned `.pre-commit-config.yaml` with repository-local cross-platform hooks that call the existing lint drivers (after adding changed-file support), plus documented `pre-commit` and `pre-push` stages.

At `pre-commit`, run clang-format in dry-run mode only on staged first-party C/C++ files and run the cheap build-tree hygiene check. At `pre-push`, optionally run clang-tidy for affected translation units using `build/debug`, but fail with a clear configure instruction if the database is absent or invalid. Do not run the entire roughly 1,800-test serial suite in a commit hook; provide an opt-in targeted test hook and leave comprehensive testing to CI/Banso. Hooks are convenience feedback and are bypassable, so every blocking rule must also run in CI.

Use local wrapper hooks rather than duplicating shell syntax in YAML, keep Bash/PowerShell behavior equivalent, pass filenames safely on Windows, and pin the pre-commit framework/tool versions used by CI. Upstream supports file filters and stage selection, which fits this two-tier design.

**Effort:** S. **Risk:** M — installation is voluntary, system LLVM availability differs by platform, and quoting/path behavior can diverge. A bootstrap check and CI execution of `pre-commit run --all-files` reduce drift.

## Ranking

| Rank | Candidate | Delivery form | Effort / risk | Recommendation |
|---:|---|---|---|---|
| 1 | Build-tree hygiene automation | `BuildTreeStrict` doctor + shared CI/Banso preflight | S / L | Implement first; it removes the known stale-database ambiguity and is a prerequisite for reliable Clang tooling. |
| 2 | clangd LSP enrichment | Banso dispatch profile + explicit `build/debug` launcher | M / M | Pilot on dispatched C++ tasks in report-only mode, then gate added errors once baseline stability is demonstrated. |
| 3 | Authoritative clang-tidy CI | Dedicated pinned-LLVM job + preset-aware lint driver | M / M | Establish a full-corpus baseline, then block new diagnostics; keep a scheduled comprehensive run. |
| 4 | Banso clang-tidy verify domain | `cpp-static` diagnostic-delta domain | M / M | Add after CI policy is defined so it reuses one toolchain and baseline policy instead of creating a second standard. |
| 5 | Tiered pre-commit hooks | Pinned local hooks for format/hygiene and optional pre-push tidy | S / M | Add as fast feedback after the shared wrappers exist; never treat hook success as the merge gate. |
| 6 | Isolated CTest sharding | Duration-balanced CI matrix using generated CTest `-I` files | L / H | Prototype last and retain the serial control lane until cross-job isolation and result parity are proven. |

The recommended delivery sequence is therefore hygiene → clangd observation → clang-tidy CI baseline/gate → Banso tidy delta → hooks → experimental isolated test shards. Coverage stays in its existing isolated preset and can publish alongside these checks, but it should not be coupled to LSP or `world_hash` decisions.

## Sources

Repository files analyzed:

- `scripts/lint.sh` and `scripts/lint.ps1` — current file discovery, format/tidy behavior, tool checks, and compile-database parameter.
- `.clang-tidy` — enabled check families, header filter, options, and non-fatal warning policy.
- `CMakePresets.json` — preset build directories, compile-command export, test presets, and coverage isolation statement.
- `docs/CLAUDE.md` — documented canonical-tree workflow, `BuildTreeStrict` warning, and serial CTest instruction. Its architecture sections were not used as evidence.
- The task brief — approximate test count/CMake registration size, stale root database, coverage-summary helper, and declared Banso `lsp_diagnostics` capability. These were not independently verified where their source files fell outside the allowed read scope.

Official upstream sources consulted:

- [Clang-Tidy documentation](https://clang.llvm.org/extra/clang-tidy/) — compilation databases, parallel `run-clang-tidy.py`, and limitations of changed-line-only `clang-tidy-diff.py` reporting.
- [clangd project setup](https://clangd.llvm.org/installation#project-setup) and [clangd configuration](https://clangd.llvm.org/config#compilationdatabase) — database discovery and explicit `CompilationDatabase` selection.
- [clangd protocol extensions](https://clangd.llvm.org/extensions#compilation-commands) — `initializationOptions.compilationDatabasePath` as an alternative explicit wiring mechanism.
- [CTest command-line reference](https://cmake.org/cmake/help/latest/manual/ctest.1.html) — serial/parallel execution and `-I` numbered test selection.
- [CTest `RUN_SERIAL`](https://cmake.org/cmake/help/latest/prop_test/RUN_SERIAL.html) — serialization applies within CTest scheduling, motivating process/worktree isolation for shards.
- [CTest `RESOURCE_LOCK`](https://cmake.org/cmake/help/latest/prop_test/RESOURCE_LOCK.html) — a possible future replacement for blanket serialization only after shared resources are understood; it is not assumed safe here.
- [pre-commit documentation](https://pre-commit.com/) — local hooks, file filtering, supported stages, and cross-platform environment behavior.
