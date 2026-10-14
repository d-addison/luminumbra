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

The Linux/GCC, Diligent RHI, ASan, preview-frame and paired performance lanes use
bounded ccache (250M each); UCRT64 and MSVC use 500M each. Qualification captures
bypass the cache. Keys include the configuration hash where applicable and the
candidate SHA, with stable restore prefixes, so each new candidate can save
updated entries. Caches are trimmed before saving. Main/devel native and
performance caches together retain about 4.5 GB across both branches.

All Actions caches, including performance, clang-tidy results, and MSYS2 packages,
are saved only on `push` events to `refs/heads/main` or `refs/heads/devel`; PRs,
scheduled runs, and manual dispatches never save caches. After CI, AddressSanitizer
or Performance observations completes, `cache-cleanup.yml` retains the newest
cache per branch and lane across configuration changes and SHA keys. It also
limits total repository cache storage, including package and other caches, to
6 GB, leaving room below GitHub's 10 GB cap for saves in progress. GitHub also
evicts old keys, but routine cleanup prevents relying on eviction at the cap.
Cleanup uses trusted inline workflow code with no checkout. When a PR closes,
it deletes caches scoped to that PR's `refs/pull/<n>/merge` ref.

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

Draft pull requests retain the previous draft routing: heavy lanes skip while
formatting, public-tree inventory, runner contracts, Doxygen, game assets, and the
authoring mock run (subject to the mock's existing paths filter). The required
Linux/GCC and Windows/MSVC jobs still skip drafts. Marking a PR ready for review
triggers its current head; those two jobs run Python or native tests,
and other heavy lanes run when the paths require them. PR workflows
listen for `opened`, `synchronize`, `reopened`, and `ready_for_review`.

Required job names and workflow PR triggers remain present for every PR. Native
lanes depend directly on the always-running classifier; none depends on a skipped
heavy job. Linux/GCC and Windows/MSVC select their native or Python steps. Other
unaffected heavy lanes report a terminal `skipped` conclusion, which GitHub
accepts for required checks, rather than leaving a missing status pending. Public tree, Format, Doxygen, and
runner contracts run independently of the classifier. Assuming those checks pass,
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
| Static analysis / clang-tidy | skipped | skipped | success | success |
| GNS transport / compile probe | skipped | skipped | success | success |
| Diligent RHI / software GL | skipped | skipped | success | success |

Python changes outside the covered allowlist use the C++ PR column. Failures still
report failure; `success` here describes an executed check that passes. Devel push
columns describe runs that reach execution, subject to batching below.
Windows/UCRT64 retains its existing branch/manual and draft routing in addition
to path gating. The paired server comparison retains its main-bound ready PR and
eligible main/devel push routing, with the same path gate. Manual qualification
captures retain full build/test coverage and cache bypass.

Devel pushes share one concurrency group per workflow and ref with
`cancel-in-progress: false`. A running run completes; GitHub's queue of one
supersedes a pending run with the newest devel push. The campaign freeze audit
accepts a superseded devel run when a descendant devel SHA's run of the same
workflow is green or pending. A failed completed run remains a failure. Scheduled
runs use a separate group so their cancellation cannot interrupt devel pushes. Main
pushes and manual dispatches, including qualification captures, have isolated
non-cancelling groups, preserving their individual evidence. PR runs retain ref
batching and cancellation where previously configured, so `ready_for_review`
supersedes earlier draft runs. Authoring PR runs retain their independent,
non-cancelling behaviour.
The authoring workflow keeps its existing path triggers; UCRT attribution keeps
its PR/manual-only triggers. Scheduled and release routing stays unchanged.

All runners remain standard hosted runners; no paid actions, larger/GPU runners,
repository visibility changes, or cache-cap increases are introduced.

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
