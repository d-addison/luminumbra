# Development and testing

## Toolchain

The required tools are CMake 3.20 or newer, Ninja, Git, and a C++20 compiler. The
first configure downloads pinned dependencies. Subsequent configuration can reuse
the populated dependency sources without probing upstream repositories.

Windows builds with `LUMINUMBRA_ENABLE_GNS=ON` or `LUMINUMBRA_ENABLE_STEAM=ON`
require CMake 3.21 or newer for runtime DLL staging. Configuration rejects older
CMake versions for these builds; builds with both backends disabled still support
CMake 3.20.

Linux builds require OpenGL, X11/Wayland development headers, FreeType, and the
usual compiler and package-configuration tools. The exact Ubuntu package set is in
`.github/workflows/ci.yml`. Windows CI validates both Visual C++ and MSYS2 UCRT64.

## Presets

| Preset | Use |
|---|---|
| `debug` | Normal local development |
| `debug-asan` | Clang/GCC AddressSanitizer validation |
| `release` | Optimized builds, full CI, and measurements |
| `coverage` | GCC/Clang coverage instrumentation |
| `debug-simo0` | Simulation optimization-parity investigation |

Configure, build, and test a preset with matching names:

```sh
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release -LE manual --no-tests=error --output-on-failure
```

`NetworkingRuntimeContract` exercises runtime selection and staging refusals
with CMake scripts on all platforms. On Windows with CMake 3.21+, `NetworkingRuntimeBuildIntegration`
(labels `ci;tools;net`, timeout 90 seconds) configures and builds a small DLL
fixture through the real staging module and PE scanner. It tests the current
generator and Ninja Multi-Config, building both Debug and Release for the latter.
The fixture checks generated input files, configuration-specific stamps, and
staged DLL hashes, then deletes a scanned transitive DLL and rebuilds only the
application to require repair without relinking. Both Windows CI lanes run these
labels in their full test suite and provide the compiler and Ninja.

The earlier post-build-only wiring cannot repair a deleted DLL when the
application is already up to date. Its combined configuration inputs also admit
DLLs with equal basenames from different configurations: this fixture rejects
foreign configuration paths and checks Debug hashes again after building Release.
A staging target using a shared output directory instead of the configuration's
executable directory fails the staged-file checks before the application builds.

Use `ctest --preset <name> -N` to inspect discovery. Tests marked `manual` are not
part of the normal CI suite. Narrow local runs with `-R <expression>` only after a
full configure has proved that the expected executable exists.

The serialized cold-world latency diagnostic is intentionally host-specific and
is not a portable performance gate. Run it explicitly when investigating world
streaming latency:

```sh
ctest --preset release -L manual \
  -R '^WorldLoadBounded\.' --no-tests=error --output-on-failure
```

## Continuous integration

The Linux/GCC and ASan lanes use bounded ccache at 400M and 600M respectively.
Diligent RHI, preview-frame and paired performance use 250M each; UCRT64 and MSVC
use 500M each. Qualification captures bypass the cache. Immutable save keys include
the commit SHA; restores prefer the matching CMake configuration prefix where
available, then the lane prefix. Caches are trimmed with `ccache --cleanup`
before saving, and cache-save failures cannot
fail a job. Starting from the measured 1.55 GB devel generation, the two cap
increases add 0.15 + 0.35 GB and the newly seeded UCRT64 cache adds up to 0.5 GB:
about 2.55 GB per branch, or 5.1 GB across main/devel, leaving about 0.9 GB of the
6 GB budget for other caches. At all seven lanes' configured caps, one generation
per branch totals 2.75 GB, or 5.5 GB across both branches, leaving 0.5 GB for
package and analysis caches. These lanes use standard runners and the existing
cache actions, with no paid cache service.

All Actions caches, including performance, clang-tidy results, and MSYS2 packages,
are saved only on `push` events to `refs/heads/main` or `refs/heads/devel`; PRs,
scheduled runs, and manual dispatches never save caches. `cache-cleanup.yml` runs
on trusted maintainer pushes to devel/main, activating retention as soon as the
workflow lands on devel. Once installed on the default branch (main), it also runs
after pushed CI, AddressSanitizer or Performance observations completes. All
cleanup runs share one concurrency group and never cancel an active cleanup.
It retains the newest generation by creation time per branch and lane across
configuration changes and SHA keys, protecting both live branch heads' keys and
the triggering SHA (`github.sha` for push, `workflow_run.head_sha` for workflow_run)
even when cleanup is delayed until the branch advances. The named `RECENCY_GRACE`
constant in `cache-cleanup.yml` is six hours: caches whose creation or last access
(whichever is later) falls within that window are never deleted, preserving caches
for queued reruns and in-flight runs, including concurrent saves. It also
limits total repository cache storage, including package and other caches, to
6 GB, leaving room below GitHub's 10 GB cap for saves in progress. GitHub also
evicts old keys, but routine cleanup prevents relying on eviction at the cap.
Cleanup uses trusted inline workflow code with no checkout or PR triggers;
`contents: read` applies throughout and only the deletion job gets `actions: write`.
It rechecks branch heads before deletion and deletes by cache ID so concurrent
replacements of the same key survive. Saves in progress can temporarily add
storage until the next cleanup; protected and grace-window caches are never deleted
to make room. If those caches alone exceed 6 GB, cleanup warns with their size and
budget, skips deletion, and exits successfully. Cleanup is best-effort:
API/auth deletion errors warn and do not fail the run; malformed input is logged
clearly and remaining cleanup is skipped.

Pull requests first classify changed paths with a merge-base diff of the base and
head SHAs. Deleted paths and both sides of renames count. On PRs, the workflow
reads the classifier from the trusted base using `git show` against the history
already fetched by checkout, without an extra fetch. It passes the checkout root
explicitly and uses isolated Python imports. Changes under `tools/ci/`,
`.github/workflows/` or `scripts/` force all lanes before any classifier executes,
including when the base predates the classifier. A missing base script or failed
classifier also forces all lanes. The classifier runs on a standard
`ubuntu-latest` runner; fork PRs, missing repository identity, shallow or
missing history, an empty diff, unknown paths, or missing classifier outputs keep
the build lanes enabled. Pushes, schedules, and manual dispatches always enable
every path-gated lane, regardless of changed files.

Compiler and analysis caches can be restored on eligible PR and manual runs.
The MSYS2 package cache is an exception: its setup action enables both restore
and save together, so package caching is disabled outside main/devel pushes.

GCC, MSVC and UCRT64 each configure and build independently on two standard
hosted runners. `tools/ci/ctest_shards.py` partitions the built CTest discovery
roster after the existing `-LE manual` policy. No checked-in duration observations
exist yet, so placement uses stable SHA-256 name hashing. If timing observations
are supplied later, they affect placement only; unknown tests receive at least a
60-second weight or the largest known duration. Fixture setup, consumers and
cleanup, dependencies and resource locks stay together; CTest keeps its existing
`RUN_SERIAL` properties. GCC's prefab cases share a producer so installed-runtime
qualification still sees all required cases. Explicit serial NetSoak, packaging
checks and diagnostic artifacts remain enabled.

The shared helper also implements ASan's existing two-shard placement, including
its evenly divided heavy parity tests. ASan retains its original fixture grouping
and execution settings. Immediately before execution, every producer rechecks
the ordered discovery roster in the execution environment and refuses drift.
Indices address the filtered roster and support literal names with regex syntax.
The reports must match their assigned names exactly. The aggregate checks reject
duplicate execution, omissions, failures, errors and skips and preserve JUnit
output. Producers must agree on inventory and partition digests. The inventory
contract includes commands, ordered arguments, every CTest property and any
additional execution fields; only diagnostic backtrace IDs are excluded.
Source and build roots are recorded in partition evidence and included in the
inventory digest. Both producers must record byte-identical roots; different
roots refuse merging, independently of the aggregate's artifact download path.
Execution strings are compared byte for byte, including paths, case-sensitive
regexes, literal environment values and quoted or nested assignments containing
roots. The fresh execution discovery uses the same contract, so
command, filter, environment or working-directory drift refuses execution even
when CTest names stay unchanged. Evidence uses version 3; older partition evidence
must be regenerated by its producers before merging.
The merged floors remain 2280 for GCC and 2260 for MSVC, UCRT64 and ASan.
GCC/MSVC keep four CTest workers and a 180-second default; UCRT64 keeps four
workers and 360 seconds. Job limits remain 60, 90 and 85 minutes respectively.

The required `Linux / GCC`, `Windows / MSVC`, `Windows / MSYS2 UCRT64` and
`Linux / GCC ASan` checks are ordinary aggregates scheduled with `always()`.
They reject drafts, missing draft metadata, cancellations and missing or
unsuccessful expected producers. GCC and MSVC retain their Python-only route;
UCRT64 retains its path/event gates and also builds on devel pushes to seed its
trusted cache. Manual native qualification captures instead use a single full-suite
producer, explicitly distinguished from two-shard evidence. Their existing
binary-bound manifests, report bytes and qualification artifact paths remain
intact. Qualification validation checks both the full producer and its aggregate.

Native shard artifact names include lane, shard, SHA, run and attempt. Reports,
inventories and partition evidence are retained for 30 days. An aggregate-only
or failed-shard rerun selects the latest attempt of each producer within the same
run, retaining evidence from successful earlier attempts. ASan retains its
existing overwrite-per-shard artifact contract and the same 30-day retention.
Only shard 1 saves each lane's compiler cache; only UCRT64 shard 1 enables the
MSYS2 package cache. The cache caps and 6 GB cleanup budget described above apply
per lane, shared by its test shards.

The GCC aggregate checks acceptance-matrix execution after merging and validating
the complete JUnit union. It also consumes unfiltered discovery and serial NetSoak
evidence from shard 1, or shard 0 for manual full qualification. Those inputs and
the acceptance execution receipt are retained with the merged report for 30 days,
so aggregate reruns can reuse successful producers' evidence.

Native validation against each platform's unsharded inventory is still required
before rollout; offline regressions do not establish platform qualification.

Blocking clang-tidy analysis runs in three path-hash shards on standard Ubuntu
runners, each with a 60-minute timeout. Together they analyse every configured
first-party translation unit and its headers on every executed merged-tip run;
pushes use `writeonly` caching and never skip analysis. PRs restore only their
matching shard's cache. The required `Static analysis / clang-tidy` check aggregates the
shards and requires all three to succeed whenever native analysis is needed.
The shards use the same path gate as other heavy lanes. On ready PRs where
classification succeeds with `needs_cpp=false`, skipped shards produce a successful
aggregate with an explicit routed-skip summary. Missing or failed classification
keeps analysis required; skipped, failed, or cancelled shards then fail the
aggregate. Both the shards and the aggregate check skip draft pull requests.
Local `scripts/tidy.sh` still analyses the full scope by default;
`--shard INDEX/COUNT` selects one stable partition.

Draft pull requests run the cheap routine checks: changed-file formatting, public
tree inventory, runner contracts, Doxygen, game assets, and the authoring mock
(subject to its existing paths filter). Linux/GCC, ASan, Windows/MSVC,
Windows/UCRT64, clang-tidy, GNS, Diligent RHI, preview-frame, and the paired server
comparison do no heavy work on draft pull requests. Linux/GCC, ASan, and
preview-frame start with a lightweight guard that fails drafts before checkout or
dependency installation. The native test aggregates also reject drafts when
Windows producers skip; the other heavy jobs skip drafts. The separate
path-filtered UCRT package attribution diagnostic keeps its existing job routing.

C++ sources and headers, CMake inputs, `src/`, `include/`, `test/`, `cmake/`,
`res/` (including shaders and test data), `data/`, `config/`, `worlds/`, vendored
and third-party dependencies, vcpkg/dependency files, `.gitattributes`, and workflow
changes, all `tools/ci/**` and `scripts/**` changes, and scripts invoked by CI
(including executable tests and validation entrypoints) require builds. These
rules take precedence over the Python allowlist. Git attributes affect exact
fixture bytes on checkout.
Documentation and covered Python tooling can skip C++ lanes. The allowlist and
standalone test plan live together in `tools/ci/ci_paths.py`.
Unknown tooling remains on the build path.

Draft pull requests cannot merge. GitHub does not allow draft PRs to merge and
may report jobs skipped by a condition as successful, so a draft skip alone does
not block required checks. The guards keep the existing `Linux / GCC`,
`Linux / GCC ASan`, `Windows / MSVC`, `Windows / MSYS2 UCRT64`, and
`Preview frame / software OpenGL` check names and report
failure on drafts. Their failure prevents a draft head from satisfying all
required checks, including immediately after marking it ready while the new
runs are pending. For PR events, they can turn green only after a non-draft run
completes its routed validation. Linux/GCC runs Python contracts or native tests;
ASan and preview-frame run their full lanes when native validation is needed.
No new required-check configuration is needed.

Marking a pull request ready for review triggers the full checks on its current
head. The CI, AddressSanitizer, and performance workflows listen for `opened`,
`synchronize`, `reopened`, `ready_for_review`, and `converted_to_draft`; existing
branch and path filters still apply. Maintainers must not merge until the
`ready_for_review` run's required checks are green on the current head, and must
wait again after any head update. This repository does not implement a separate
merge gate. The external campaign gate (`campaign_control.py land`, outside this
repository) requires actual `SUCCESS` for required checks on the exact head,
with one routing exception: for non-draft engine PRs, it accepts `SKIPPED`
heavy checks only when every same-head `Classify changed paths` check succeeded.
Drafts and push/recovery validation remain `SUCCESS`-only. A routed skip is not
evidence that native validation ran. No additional job runs on draft pushes
to enforce this policy; the guards use existing required jobs. Windows/UCRT64
keeps its existing branch/manual routing, and the paired server comparison still runs only for
main-bound ready PRs or eligible main/devel pushes.

The required `Linux / GCC` and `Windows / MSVC` jobs always run on ready PRs.
When `needs_cpp=false`, each installs only numpy, Pillow and jsonschema for its OS
and runs `ci_paths.py python-tests`, skipping native dependencies, compiler caches,
configure/build/CTest, native qualification, packaging and native artifacts.
Both jobs log and summarize `route: python-only (native build skipped by Classify
changed paths)` and expose a `route` job output of `python-only`. When `needs_cpp`
is true or missing, they report and expose `native` and retain their full native
path. Consumers requiring native qualification must check that output alongside
the job conclusion; a successful Python route provides Python coverage.

The standalone plan also runs whenever `needs_python=true` or its output is
missing, including mixed Python/native PRs and full push/manual runs, with numpy,
Pillow and jsonschema installed. It includes `tools/ci/test_ci_paths.py` and cache
retention regression tests. Python failures block merging through the existing
required contexts on either route.

The standalone plan explicitly lists tests in `tools/ci`, `tools/perf`,
`tools/release`, `tools/assets`, `tools/audio`, `tools/persistence`, and
`tools/rendering/acceptance`, plus visual-critique, timelapse, SDF doc,
runtime-manifest, visual-montage, and Blender core/GLB contracts. This preserves
Windows coverage for CTest-registered Python suites. The POSIX shebang fixtures in
`test_tidy_cache.py` run only on Linux. Classification exemptions are explicit
reviewed files, including covered test-backed modules; new files require builds.
Native integrations such as `qualify_prefab_runtime.py`, its packaged service and
installed acceptance inputs, `test_render_runner.py`, its native fixture generator
and imports, and simulation capture tools stay on the build path.
Selected extension helpers and viewport fixtures also skip C++: the existing
authoring mock job covers them on Linux and Windows. Executable authoring test
entrypoints retain native builds under the CI script rule.
Authoring service implementation and native probes require builds.
The public-tree job also runs SDF documentation lint so doc-only edits retain that
CTest-registered check. Doxygen always runs because `docs/` feeds its input.

Required job names and workflow PR triggers remain present for every PR. Native
lanes depend directly on the always-running classifier. The clang-tidy aggregate
also depends on the shards and uses `always()` to report their result even when
they are skipped. Linux/GCC and Windows/MSVC select their native or Python
steps. Other unaffected heavy lanes report a terminal `skipped` conclusion,
which GitHub accepts for required checks, rather than leaving a missing status
pending. Public tree, Format, Doxygen, and runner contracts run independently
of the classifier. Assuming those checks pass,
the devel ruleset sees the following conclusions on ready PRs:

| Required check | Docs-only PR | Covered Python-only PR | C++ PR | Devel push |
|---|---|---|---|---|
| Linux / GCC | success (Python) | success (Python) | success | success |
| Windows / MSVC | success (Python) | success (Python) | success | success |
| Public tree / inventory honesty | success | success | success | success |
| Format changed files | success | success | success | success |
| Linux / GCC ASan | skipped | skipped | success | success |
| Build Doxygen reference | success | success | success | success |
| Runner contract / ubuntu-24.04 | success | success | success | success |
| Runner contract / windows-2025 | success | success | success | success |
| Runner contract / macos-15 | success | success | success | success |
| Preview frame / software OpenGL | skipped | skipped | success | success |
| Static analysis / clang-tidy | success (routed skip) | success (routed skip) | success | success |
| GNS transport / compile probe | skipped | skipped | success | success |
| Diligent RHI / software GL | skipped | skipped | success | success |

Python changes outside the covered allowlist use the C++ PR column. Failures still
report failure; `success` here describes an executed check that passes. Devel push
columns describe runs that reach execution, subject to batching below.
Windows/UCRT64 runs on ready PRs into main/devel, pushes to main/devel, and manual
qualification captures, with its existing draft and path gates. Ordinary manual
dispatches retain their main-only route. Devel pushes seed
the devel-scoped compiler cache restored by PRs into devel; the MSYS2 package cache
also saves on these trusted pushes. It shares the workflow-wide devel push
concurrency group described below. The paired server comparison retains its
main-bound ready PR and eligible main/devel push routing, with the same path gate.
Manual qualification captures retain full build/test coverage and cache bypass.

Devel pushes share one concurrency group per workflow and ref with
`cancel-in-progress: false`. A running run completes; GitHub's queue of one
supersedes a pending run with the newest devel push.
The campaign freeze audit covers a devel push run that ended cancelled
only with a newer devel push run of the same workflow path on a descendant
SHA that is pending or successful, or whose first attempt failed but has
an accepted automatic rerun request. Coverage is recomputed on every audit.
If the covering run later fails, or itself ends cancelled without its own
covering run, the freeze re-latches. Failed runs are never covered by
descendants; only their own successful rerun or an explicit resolution
can resolve them.
Scheduled runs use a separate group so their cancellation cannot interrupt devel
pushes. Main pushes and manual dispatches, including qualification captures, have isolated
non-cancelling groups, preserving their individual evidence. PR runs retain ref
batching and cancellation where previously configured, so `ready_for_review`
supersedes earlier draft runs and `converted_to_draft` cancels in-flight ready
runs. Authoring PR runs retain their independent, non-cancelling behaviour.
The authoring workflow keeps its existing path triggers. The diagnostic
`ucrt-attribution.yml` workflow is not a required check. Its devel PR triggers
cover the workflow itself, `tools/ci/ucrt_attribution.sh`,
`tools/ci/test_ucrt_attribution.py`, `tools/ci/test_ucrt_gcc_cohort.py`,
`tools/ci/check_worldgen_probe_smoke.py`,
`tools/ci/test_check_worldgen_probe_smoke.py`, and
`test/diagnostics/worldgen_probe.cpp`. Devel pushes cover those same paths plus
`tools/CMakeLists.txt` and `test/CMakeLists.txt`, catching probe build changes
after merge. Attribution also runs weekly on Sunday at 04:43 UTC on the default
branch and supports manual dispatch. Other scheduled and release routing stays
unchanged.

All runners remain standard hosted runners; no paid actions, larger/GPU runners,
or repository visibility changes are introduced.

CTest produces JUnit and runtime artifacts. CI verifies a minimum discovered-test
count so an empty or severely truncated suite cannot report success. Once a
non-draft test or sanitizer lane runs, its sanitizer and test artifacts are uploaded
even on failure to support diagnosis. The draft guard failures described above stop
before checkout and upload no artifacts.

`WorldgenProbeSmoke` validates the `luminumbra.worldgen_probe.v1` smoke report
with seed `424242` and spawn `(8, terrain height + 1.95, 8)`. Valid evidence requires
`selected_chunks == requested_chunks == 2` and `phases.polygonise.mesh_triangles >= 1`.
The report adds a top-level `terrain_height` number sampled at `(8, 8)`, preserving
the schema identifier and existing `spawn` fields for report consumers. Smoke
validation requires this finite height, exact `spawn.x == spawn.z == 8`, and
`spawn.y == terrain_height + 1.95` within an absolute tolerance of `1e-5` metres
(no relative tolerance), allowing C++ float rounding. Reports missing the height
or using a different spawn fail validation.
The probe scans a 25-by-25 initial surface neighborhood, including each column's
surface-height span and one vertical chunk of margin on either side, until it
finds the requested surface chunks. Reduced coverage or an empty mesh fails
validation. Timing values may be zero; the check imposes no speed threshold.

## UCRT64 package attribution setup

The dedicated attribution workflow explicitly provisions `libatomic` before
`tools/ci/ucrt_attribution.sh` records its package baseline. The pinned GCC
intervention includes `cc-libs=16.2.0-4`, whose package transaction requires
`libatomic=16.2.0-4`. Every changed package must already be installed when the
baseline is recorded; interventions do not install previously absent members.

For a direct script run, provision `mingw-w64-ucrt-x86_64-libatomic` in a disposable
UCRT64 environment before starting the script. Restart the complete run after any
provisioning change. The recorded baseline is the environment after that setup:
restoration returns `libatomic` and other changed members to their recorded
versions using staged archives. It does not undo the initial provisioning.
Baseline-only and CRT-only selections do not require this GCC prerequisite.

The script requires Python and validates each full-mode probe report before
recording `OK`. Full reports must cover all 24 requested chunks, contain a
non-empty mesh, satisfy the spawn contract above, and include every phase,
scaling entry, and both libm observations (eight shaped-height repetitions and
one million calls per libm function). Invalid JSON, missing reports, or incomplete
evidence produce `INVALID_REPORT` in the cell's `status.json` and summary row;
the validation reason is appended to `probe.txt`. Nonzero probe exits remain
`TEST_FAILED`, and probe or validation timeouts remain `TIMED_OUT`.

The actual installed baseline version is recorded rather than inferred from the
compiler version. Missing archives, package dependency refusals, and unsuccessful
restoration remain explicit failures; the script does not repair dependencies or
claim a measurement for an unavailable intervention. A run with a different setup
has a different provisioning profile, so its observations must retain that identity.

## Local validation

Run the preset that matches the affected platform and configuration. At minimum:

```sh
scripts/lint.sh --format-only
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug -LE manual --no-tests=error --output-on-failure
```

Use `debug-asan` for memory-ownership changes and `release` for deterministic hash
or performance comparisons. Software OpenGL can exercise shader and rendering
paths in automation, but a successful software run is not evidence of hardware GPU
performance or driver correctness.

## Generated and local-only data

Build products belong under `build/`. Test captures belong under the active build
tree unless a tool explicitly selects another local artifact directory. Do not
commit audio sources, generated captures, compiler caches, IDE state, or local task
records.
