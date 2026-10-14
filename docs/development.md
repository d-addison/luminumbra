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

The Linux/GCC, Diligent RHI, and ASan lanes use bounded ccache (500M each), as do
UCRT64 and MSVC (1G each); qualification captures in these lanes bypass the cache.
Performance lanes are bounded at 250M each. A complete compiler-cache generation
is bounded at about 4 GB, including preview-frame and paired-server caches.
Immutable save keys include the commit SHA; restores prefer the matching CMake
configuration prefix where available, then the lane prefix. Cleanup runs before
saving, and cache-save failures cannot fail a job. Per-SHA entries accumulate;
GitHub evicts older entries within the repository's 10 GB cache cap. These lanes
use standard runners and the existing cache actions, with no paid cache service.
All Actions caches, including performance, clang-tidy results, and MSYS2 packages,
are saved only on `push` events to `refs/heads/main` or `refs/heads/devel`; PRs,
scheduled runs, and manual dispatches never save caches. When a PR closes,
`cache-cleanup.yml` deletes caches scoped to its `refs/pull/<n>/merge` ref without
checking out PR code.

Blocking clang-tidy analysis runs in three path-hash shards on standard Ubuntu
runners, each with a 60-minute timeout. Together they analyse every configured
first-party translation unit and its headers on every merged tip; pushes use
`writeonly` caching and never skip analysis. PRs restore only their matching
shard's cache. The required `Static analysis / clang-tidy` check aggregates the
shards and passes only when all three succeed. Both the shards and the aggregate
check skip draft pull requests. Local `scripts/tidy.sh` still analyses the full
scope by default; `--shard INDEX/COUNT` selects one stable partition.

Draft pull requests run the cheap routine checks: changed-file formatting, public
tree inventory, runner contracts, Doxygen, game assets, and the authoring mock
(subject to its existing paths filter). Linux/GCC, ASan, Windows/MSVC,
Windows/UCRT64, clang-tidy, GNS, Diligent RHI, preview-frame, and the paired server
comparison do no heavy work on draft pull requests. Linux/GCC, ASan, and
preview-frame start with a lightweight guard that fails drafts before checkout or
dependency installation; the other heavy jobs skip drafts. The separate
path-filtered UCRT package attribution diagnostic keeps its existing job routing.

Draft pull requests cannot merge. GitHub does not allow draft PRs to merge and
may report jobs skipped by a condition as successful, so a draft skip alone does
not block required checks. The guards keep the existing `Linux / GCC`,
`Linux / GCC ASan`, and `Preview frame / software OpenGL` check names and report
failure on drafts. Their failure prevents a draft head from satisfying all
required checks, including immediately after marking it ready while the new
runs are pending. For PR events, they can turn green only after a non-draft run
completes their full build/test or measurement lane. No new required-check
configuration is needed.

Marking a pull request ready for review triggers the full checks on its current
head. The CI, AddressSanitizer, and performance workflows listen for `opened`,
`synchronize`, `reopened`, `ready_for_review`, and `converted_to_draft`; existing
branch and path filters still apply. Maintainers must not merge until the
`ready_for_review` run's required checks are green on the current head, and must
wait again after any head update. This repository does not implement a separate
merge gate. The external campaign gate (`campaign_control.py land`, outside this
repository) additionally requires actual `SUCCESS` for every required check on
the exact head and rejects `SKIPPED` checks; routed skips are not evidence that a
required lane passed. No additional job runs on draft pushes to enforce this
policy; the guards use existing required jobs. Windows/UCRT64 keeps its existing
branch/manual routing, and the paired server comparison still runs only for
main-bound ready PRs or eligible main/devel pushes.

Workflows with concurrency groups key PR runs by ref and cancel superseded runs,
so a `ready_for_review` run cancels an earlier draft run on the same PR ref, and
`converted_to_draft` cancels an in-flight ready run and prevents new heavy work.
Devel pushes keep separate SHA-keyed groups with cancellation disabled. Push,
schedule, manual dispatch (including qualification capture), and release behavior
is unchanged.

Ready pull requests run:

- optimized GCC builds and tests on Ubuntu;
- optimized MSVC builds and tests on Windows;
- optimized GCC/UCRT64 builds and tests on Windows;
- Clang AddressSanitizer builds and tests on Ubuntu;
- blocking clang-tidy shards with an aggregated required check, and GNS and
  Diligent RHI probes;
- changed-file formatting checks;
- Doxygen parsing with warnings treated as errors; and
- an explicitly observational software-OpenGL performance capture.

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
