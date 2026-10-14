# Development and testing

## Toolchain

The required tools are CMake 3.20 or newer, Ninja, Git, and a C++20 compiler. The
first configure downloads pinned dependencies. Subsequent configuration can reuse
the populated dependency sources without probing upstream repositories.

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

Draft pull requests run the cheap routine checks: changed-file formatting, public
tree inventory, runner contracts, Doxygen, game assets, and the authoring mock
(subject to its existing paths filter). Linux/GCC, ASan, Windows/MSVC,
Windows/UCRT64, clang-tidy, GNS, Diligent RHI, preview-frame, and the paired server
comparison skip draft pull requests. The separate path-filtered UCRT package
attribution diagnostic keeps its existing job routing.

Marking a pull request ready for review triggers CI on its current head, running
the full required set. PR workflows listen for `opened`, `synchronize`, `reopened`,
and `ready_for_review`; existing branch and path filters still apply. Heavy
required checks report skipped on drafts, and GitHub can treat skipped checks as
successful. GitHub does not allow draft PRs to merge. Marking the PR ready reruns
every required job on the same head; the new check runs supersede the skipped
ones. Maintainers must not merge until the `ready_for_review` run's required
checks are green on the current head, and must wait again after any head update.
This repository does not implement a separate merge gate. The external campaign
gate (`campaign_control.py land`, outside this repository) accepts only actual
`SUCCESS` for required checks on the exact head. No additional job runs on draft
pushes to enforce this policy. Windows/UCRT64 keeps its existing branch/manual
routing, and the paired server comparison still runs only for main-bound ready
PRs or eligible main/devel pushes.

Workflows with concurrency groups key PR runs by ref and cancel superseded runs,
so a `ready_for_review` run cancels an earlier draft run on the same PR ref. Devel
pushes keep separate SHA-keyed groups with cancellation disabled. Push, schedule,
manual dispatch (including qualification capture), and release behavior is
unchanged.

Ready pull requests run:

- optimized GCC builds and tests on Ubuntu;
- optimized MSVC builds and tests on Windows;
- optimized GCC/UCRT64 builds and tests on Windows;
- Clang AddressSanitizer builds and tests on Ubuntu;
- blocking clang-tidy analysis and GNS and Diligent RHI probes;
- changed-file formatting checks;
- Doxygen parsing with warnings treated as errors; and
- an explicitly observational software-OpenGL performance capture.

CTest produces JUnit and runtime artifacts. CI verifies a minimum discovered-test
count so an empty or severely truncated suite cannot report success. Sanitizer and
test artifacts are uploaded even on failure to support diagnosis.

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
