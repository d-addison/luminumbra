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

Pull requests run:

- optimized GCC builds and tests on Ubuntu;
- optimized MSVC builds and tests on Windows;
- optimized GCC/UCRT64 builds and tests on Windows;
- Clang AddressSanitizer builds and tests on Ubuntu;
- changed-file formatting checks;
- Doxygen parsing with warnings treated as errors; and
- an explicitly observational software-OpenGL performance capture.

CTest produces JUnit and runtime artifacts. CI verifies a minimum discovered-test
count so an empty or severely truncated suite cannot report success. Sanitizer and
test artifacts are uploaded even on failure to support diagnosis.

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
