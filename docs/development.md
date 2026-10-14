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

Draft pull requests run the cheap routine checks: changed-file formatting, public
tree inventory, runner contracts, Doxygen, game assets, and the authoring mock
(subject to its existing paths filter). Linux/GCC, ASan, Windows/MSVC,
Windows/UCRT64, clang-tidy, GNS, Diligent RHI, preview-frame, and the paired server
comparison do no heavy work on draft pull requests. Linux/GCC, ASan, and
preview-frame start with a lightweight guard that fails drafts before checkout or
dependency installation; the other heavy jobs skip drafts. The separate
path-filtered UCRT package attribution diagnostic keeps its existing job routing.

Draft pull requests cannot merge. GitHub may report jobs skipped by a condition
as successful, so a draft skip alone does not block required checks. The guards
keep the existing `Linux / GCC`, `Linux / GCC ASan`, and
`Preview frame / software OpenGL` check names and report failure on drafts. Their
failure prevents a draft head from satisfying all required checks, including
immediately after marking it ready while the new runs are pending. For PR events,
they can turn green only after a non-draft run completes their full build/test or
measurement lane. No new required-check configuration is needed.

Marking a pull request ready for review triggers the full checks on its current
head. The CI, AddressSanitizer, and performance workflows listen for `opened`,
`synchronize`, `reopened`, `ready_for_review`, and `converted_to_draft`; existing
branch and path filters still apply. The campaign merge gate additionally
requires actual `SUCCESS` for every required check on the exact head and rejects
`SKIPPED` checks; routed skips are not evidence that a required lane passed.
Windows/UCRT64 keeps its existing branch/manual routing, and the paired server
comparison still runs only for main-bound ready PRs or eligible main/devel pushes.

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
