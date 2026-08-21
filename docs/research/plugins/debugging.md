# Debugging tooling research

Research date: 2026-08-21

This report evaluates debugging integrations for Luminumbra from the scoped source files listed under [Sources](#sources). The near-term recommendation is to make the evidence already produced by the engine and sanitizer job automatically triageable, then add a symbol-retention pipeline. Crashpad is the preferred production cross-platform crash-capture pilot, but it should follow those foundations rather than replace them.

## Current state

### Crash and runtime evidence

The checked-in source is ahead of the task brief in one important respect: the Windows client **does have a minidump writer**. `main_client.cpp` currently:

- creates timestamped `.dmp` files with `MiniDumpWriteDump(..., MiniDumpNormal, ...)`;
- writes a bounded, faulting-thread text trace with `StackWalk64`, DbgHelp symbol lookup, and `module+RVA` fallback addresses;
- flushes the logger, records an `unhandled_exception` phase in the last-known runtime-state JSON, and invokes both writers from a one-shot `SetUnhandledExceptionFilter`; and
- installs no signal, alternate-stack, core, or minidump handler in the non-Windows branch—the branch only retains the recorder pointer.

The source comment says the Windows minidump path has produced empty files, while the dump result is not recorded in an artifact. This makes dump existence an unreliable success signal. The handler also performs filesystem, C++ stream, logger, symbol-engine, and JSON work inside the faulting process. Microsoft recommends calling `MiniDumpWriteDump` from a separate process where possible because the target may be unstable, and notes that DbgHelp calls are single-threaded. The existing atomic one-shot guard prevents concurrent handler entry, but it cannot remove corruption, loader-lock, exhausted-stack, or re-entrancy risks.

`RuntimeStateRecorder` already emits structured JSON for last-known state, memory watermarks, and shutdown/job-drain state. That is a strong correlation surface for crash and hang reports and avoids requiring an immediate logging rewrite.

### Assertions and logging

`Debug.h` implements `LUMINUMBRA_ASSERT` by logging the failed expression and source location, invoking `Debug::Break` (`__debugbreak` on MSVC or `__builtin_trap` on Clang/GCC), and then aborting. `Log.h` exposes lazy-initialized spdlog loggers through level-specific macros. The allowed header does not expose formatter or sink configuration; `main_client.cpp` refers to a file-backed log and explicitly flushes it during Windows crash handling.

### Hang diagnostics

`JobWatchdog.h` supplies `WaitWithJobWatchdog`, enabled only by `LUMINUMBRA_JOB_WATCHDOG=1`. A reporter thread emits periodic `JOB_WATCHDOG` warnings containing the named wait phase and elapsed seconds. It deliberately neither ends nor changes the wait and is documented as disabled in determinism gates. It does not capture thread stacks, job-queue state, the runtime-state artifact, or a dump when a threshold is crossed.

### Sanitizers and interactive debuggers

The Ubuntu GitHub Actions job builds CMake code with Clang ASan+UBSan, debug information, and frame pointers, then runs CTest. ASan leak detection, abort-on-error, and online symbolization are enabled; UBSan halts and requests a stack trace. The job currently has no explicit `ASAN_SYMBOLIZER_PATH`, report parser, stable failure fingerprint, workflow annotation, job summary, or uploaded raw-log artifact.

The task brief reports an MSYS2 GDB launch configuration in `.vscode/launch.json`. That file was outside the permitted read set for this task, so the claim is recorded as supplied context rather than independently verified tree evidence. No LLDB integration is represented in the scoped files.

### Gaps

The principal gaps are Linux/POSIX crash capture, reliable out-of-process dumping, immutable symbol retention, repeatable offline symbolization, automated sanitizer/log triage, reusable engine-aware debugger commands, and a single report that correlates watchdog warnings with runtime and crash artifacts. The scoped files contain no Crashpad or Breakpad integration; repository-wide dependency absence was not re-scanned because this task limited reads to named files.

## Candidate integrations

### 1. Banso structured debug-triage step

**Delivery form:** a post-run Banso step/CLI command, tentatively `banso debug triage <artifact-dir>`, producing `debug-summary.json` plus a concise Markdown summary. Version 1 should consume existing logs, CTest output, crash text, last-known runtime JSON, memory-watermark JSON, and shutdown JSON without changing the engine logger.

The step should normalize timestamps, select the first fatal/assert/sanitizer event, collect the last bounded breadcrumb window, group repeated `JOB_WATCHDOG` messages by phase, attach runtime/job/readiness fields, and emit a stable fingerprint from diagnostic class plus the top symbolized project frames. It should retain links to raw evidence and mark missing/truncated inputs explicitly. Parsers should be fixture-tested against partial writes and interleaved worker output. Secrets and user paths need redaction before any upload; raw local artifacts remain authoritative.

**Effort:** 2–4 engineer-days. **Risk:** Low. Format drift and misleading regex matches are the main risks; schema versioning and fixture tests contain them.

**Determinism/world_hash:** Safe when strictly post-process. It must not launch, resume, attach to, or mutate the simulation.

### 2. ASan/UBSan triage automation

**Delivery form:** a small CI wrapper/parser around the existing sanitizer CTest invocation. Capture combined raw output, set `ASAN_SYMBOLIZER_PATH` to the installed `llvm-symbolizer`, parse ASan and UBSan blocks into `sanitizer-report.json`, emit GitHub error annotations and a job summary, and upload the raw plus normalized reports even on failure.

The normalized record should contain sanitizer kind, access kind/size, first project frame, allocation/free origins when present, test name, compiler version, commit, and a fingerprint that removes addresses and process IDs. Keep the raw report because ASan exits on the first detected error by design in this configuration. LLVM documents both online symbolization through `ASAN_SYMBOLIZER_PATH`/`PATH` and offline symbolization when an online symbolizer is unavailable. GitHub Actions artifacts are appropriate for preserving logs and core/dump-like outputs after a job ends.

**Effort:** 1–2 engineer-days. **Risk:** Low. Parser changes across LLVM versions are the main risk; pinning fixtures from the installed Clang and failing open to raw artifacts avoids hiding failures.

**Determinism/world_hash:** No production simulation change. Sanitizer runs remain diagnostic builds and should not be used as performance or timing baselines.

### 3. Immutable symbol bundle and offline symbolizer

**Delivery form:** a build/CI packaging command that publishes an immutable symbol manifest and a local `luminumbra-symbolize` CLI. Index exact executables, DLLs/shared objects, PDB/DWARF data, build ID or content digest, platform, architecture, compiler, configuration, and commit. The CLI should accept the existing Windows crash text and `.dmp` files and emit both human-readable and JSON stacks.

For current `module+RVA` text, resolve against the exact archived module with `llvm-symbolizer` or the platform-equivalent tool. `llvm-symbolizer` accepts object files or build IDs plus addresses. For minidumps, evaluate `minidump_stackwalk` with generated Breakpad symbols; Breakpad documents that minidumps need separately retained symbols and that its processor can consume Windows DbgHelp minidumps. Verify this against Luminumbra PDB and DWARF builds before standardizing. Never symbolize against a merely same-named binary.

**Effort:** 3–5 engineer-days. **Risk:** Medium. Exact build-to-symbol matching, PDB/DWARF conversion, storage retention, and tool-version compatibility are the material risks.

**Determinism/world_hash:** Build metadata and offline processing are hash-neutral. Do not embed timestamps or symbol-server identifiers into hashed simulation state.

### 4. Watchdog-to-report integration and opt-in hang capture

**Delivery form:** two layers: extend the Banso triage step to correlate existing `JOB_WATCHDOG` lines with last-known/shutdown JSON, and add an opt-in external CI/dev supervisor that, after a separately configured hard timeout, captures all-thread debugger stacks plus a runtime artifact bundle before terminating the wedged process.

The report schema should include phase, first/last warning time, report count, last-known frame/readiness/job-queue fields, whether orderly shutdown existed, debugger attach result, and paths to raw stacks/logs. Keep the in-engine watchdog warning-only. Prefer an external dumper/debugger for escalation so fault handling is not added to the waiting thread. Timeouts must distinguish “warning” from “capture” and “terminate,” and report attach failure rather than silently treating it as a successful capture.

**Effort:** 3–5 engineer-days. **Risk:** Medium. Attaching can fail under CI permissions or perturb thread timing; forced termination can truncate evidence.

**Determinism/world_hash:** Explicitly sensitive. Continue to keep the watchdog and supervisor disabled in determinism gates. Even observational reporter threads or debugger stops can alter scheduling, so no report produced with them enabled may establish or update a `world_hash` baseline.

### 5. GDB/LLDB engine-state command packs

**Delivery form:** versioned, debugger-specific Python modules loaded by launch configurations or manually: a GDB module for MSYS2/Linux and an LLDB module for Clang platforms. Initial read-only commands should provide bounded all-thread backtraces, summarize known job-wait/watchdog state, display available runtime recorder fields, and pretty-print high-value engine containers only after their layouts are verified. Include batch entry points that emit machine-readable text for the hang supervisor.

GDB exposes Python APIs for threads, frames, events, CLI commands, and pretty-printers; LLDB exposes its API through embedded Python and supports custom commands and stop hooks. Keep adapters thin around a shared output schema, but do not force identical debugger internals. Scripts must detect missing symbols/types and print an explicit unsupported result instead of calling engine methods inside a damaged inferior.

**Effort:** 3–6 engineer-days. **Risk:** Medium. Type-layout drift, optimized-out state, debugger Python packaging, and MSYS2/Windows path handling require fixtures and smoke tests.

**Determinism/world_hash:** Commands must be read-only: no inferior function calls, writes, tick advancement, or convenience commands that recalculate state. A paused/attached run is diagnostic evidence only and cannot be a timing or `world_hash` authority.

### 6. Crashpad cross-platform capture pilot

**Delivery form:** a Windows+Ubuntu proof-of-concept package containing the Crashpad handler executable, a small platform-neutral client wrapper, a local crash database, consent/upload disabled by default, and forced-crash smoke tests that verify a non-empty dump and metadata record. The pilot should preserve current last-known JSON and log breadcrumbs as attachments/annotations rather than duplicating engine state inside a signal handler.

Crashpad is the stronger production direction because its design separates the handler from the client: the handler snapshots, stores, and optionally uploads reports. Its current status page lists complete clients for Windows and Linux (as well as macOS and other targets). That directly addresses the reliability concern in the current in-process Windows DbgHelp path and fills the no-op Linux branch. Integration still needs a processor decision: Crashpad's status page says its own crash report processor remains future work, so test Breakpad-compatible stack processing or another supported backend before adoption.

Gate the pilot on packaging size, handler lifecycle, offline behavior, dump completeness for worker-thread faults and stack exhaustion, symbol lookup, privacy/redaction, retention, user consent, and crash-loop behavior. Keep uploading out of scope until local capture and deletion semantics are proven.

**Effort:** 8–15 engineer-days for a capture-and-symbolization pilot; production upload/operations is additional. **Risk:** Medium–High. GN/depot_tools integration, CMake packaging, helper-process lifecycle, symbol/backend compatibility, privacy, and crash-loop handling are substantial.

**Determinism/world_hash:** Capture registration must not write or sample hashed simulation state. Handler IPC and metadata collection can perturb scheduling, so disable the pilot in determinism gates until repeated run/replay tests demonstrate that the `world_hash` contract remains unchanged.

### 7. Breakpad or native POSIX capture as the fallback

**Delivery form:** a time-boxed alternative spike with one of two explicit outcomes: (a) integrate the Breakpad client on Linux and use its cross-platform minidump/symbol processor while retaining the current Windows DbgHelp writer, or (b) for developer/CI builds only, use OS core dumps plus an external GDB/LLDB batch collector. Deliver a platform adapter, forced-crash matrix, dump validation, and the same symbol manifest/report schema as candidates 3 and 4.

Breakpad provides client handlers, symbol dumpers, and a processor for Windows/Linux/macOS minidumps. Its documentation also emphasizes that in-process dump writing is unsafe and describes out-of-process approaches. A hand-written POSIX `sigaction` handler would create a third crash subsystem and would be constrained to async-signal-safe work; it should not call spdlog, JSON, filesystem abstractions, allocators, or debugger APIs. Native core dumps are useful in controlled CI/developer environments but are larger and operationally dependent on host configuration.

Use this candidate only if Crashpad packaging or backend compatibility fails. Avoid running both crash handlers in the same process because competing exception/signal ownership makes behavior and evidence precedence unclear.

**Effort:** 6–12 engineer-days. **Risk:** Medium–High for Breakpad integration and High for a custom POSIX handler. Signal safety, handler ownership, Linux host policy, dump size, and cross-platform parity dominate.

**Determinism/world_hash:** All capture paths are diagnostic and default-off in gates. No signal handler or reporter may mutate simulation state, acquire simulation locks, or change `world_hash` inputs.

## Ranking

| Rank | Candidate | Delivery form | Effort | Risk | Recommendation |
|---:|---|---|---|---|---|
| 1 | ASan/UBSan triage automation | CI wrapper, JSON report, annotations, artifacts | 1–2 days | Low | Implement first; immediate value from an existing required job. |
| 2 | Banso structured debug triage | Post-run Banso step producing JSON + Markdown | 2–4 days | Low | Implement alongside rank 1; establishes the common evidence schema. |
| 3 | Symbol bundle and offline symbolizer | Build artifact manifest + local CLI | 3–5 days | Medium | Implement before adding another dump producer. |
| 4 | Watchdog/report integration | Banso correlation + opt-in external supervisor | 3–5 days | Medium | Add after the report schema and symbols exist. |
| 5 | GDB/LLDB command packs | Read-only Python modules + batch output | 3–6 days | Medium | Build incrementally around the hang supervisor and verified engine types. |
| 6 | Crashpad pilot | Helper process, local database, wrapper, smoke matrix | 8–15 days | Medium–High | Preferred production capture direction; pilot after ranks 1–3. |
| 7 | Breakpad/native POSIX fallback | Alternative client/core adapter + validation matrix | 6–12 days | Medium–High/High | Time-box only if Crashpad fails its packaging or processing gates. |

The order optimizes diagnostic yield and reduces adoption risk: normalized evidence and exact symbols benefit the current Windows crash path immediately and are prerequisites for judging either Crashpad or Breakpad fairly.

## Proposed acceptance gates

1. A forced crash yields a non-empty raw artifact, last-known runtime state, raw logs, and a normalized report with a stable fingerprint.
2. The same archived build symbolizes identically offline; a mismatched build is rejected rather than producing plausible but wrong frames.
3. An ASan fixture produces a source annotation, JSON record, job summary, and retained raw output.
4. A synthetic watchdog wedge produces correlated phase/timing/runtime evidence and bounded all-thread stacks, including an explicit status when attach fails.
5. Windows and Ubuntu forced-crash smoke tests cover main-thread and worker-thread faults; stack-exhaustion and crash-during-startup cases are included before a helper-based capture system is called production-ready.
6. Diagnostic features are disabled in determinism gates, and dedicated run/replay checks confirm no change to the `world_hash` contract before any default is changed.

## Sources

### Repository evidence analyzed

- [`src/luminumbra_client/main_client.cpp`](../../../src/luminumbra_client/main_client.cpp): runtime JSON recorder (around lines 1468–1531), Windows minidump/stack/filter implementation and non-Windows no-op (around lines 2080–2258), and handler installation (around lines 2735–2742).
- [`src/luminumbra_common/core/Debug.h`](../../../src/luminumbra_common/core/Debug.h): assertion logging, debugger trap, and abort behavior.
- [`src/luminumbra_common/core/Log.h`](../../../src/luminumbra_common/core/Log.h): spdlog-backed logger interface and macros.
- [`src/luminumbra_common/core/JobWatchdog.h`](../../../src/luminumbra_common/core/JobWatchdog.h): environment gate, reporter thread, phase warning, and observability-only contract.
- [`.github/workflows/asan.yml`](../../../.github/workflows/asan.yml): Ubuntu Clang ASan+UBSan build and CTest execution.

### Web sources consulted

- [Crashpad overview design](https://chromium.googlesource.com/crashpad/crashpad/+/HEAD/doc/overview_design.md) and [Crashpad project status](https://chromium.googlesource.com/crashpad/crashpad/+/HEAD/doc/status.md): handler/client separation, storage/upload model, platform status, and processor status.
- [Breakpad getting started](https://chromium.googlesource.com/breakpad/breakpad/+/master/docs/getting_started_with_breakpad.md), [processor design](https://chromium.googlesource.com/breakpad/breakpad/+/master/docs/processor_design.md), and [symbol files](https://chromium.googlesource.com/breakpad/breakpad/+/master/docs/symbol_files.md): cross-platform minidump components, stack processing, and symbol requirements.
- [Microsoft `MiniDumpWriteDump` documentation](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump): separate-process guidance, calling-thread caveat, synchronization requirement, and API behavior.
- [Clang AddressSanitizer documentation](https://clang.llvm.org/docs/AddressSanitizer.html) and [`llvm-symbolizer` documentation](https://llvm.org/docs/CommandGuide/llvm-symbolizer.html): online/offline symbolization and object/build-ID address resolution.
- [GDB Python API](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Python-API.html) and [LLDB Python reference](https://lldb.llvm.org/use/python-reference.html): debugger commands, threads/frames/events, pretty-printers, and scripting surfaces.
- [GitHub Actions workflow artifacts](https://docs.github.com/en/actions/concepts/workflows-and-actions/workflow-artifacts) and [workflow commands](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-commands): retaining diagnostic outputs and emitting workflow annotations/summaries.
