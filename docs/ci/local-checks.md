# Local compile and lint checks

These checks compile selected translation units and run clang-tidy. Never link
engine or test executables, build all targets, run those executables, or invoke
CTest. `BUILD_TESTING=ON` exposes test translation units in the databases so
their source can be compiled and linted without running tests. The only allowed
unit tests in this workflow are the stdlib Python suites for the helpers.

Use a local merge base. Direct `compile` and `tidy` selector invocations and the
shell recipe below use a nonempty `LUMINUMBRA_BASE_REF` instead of `origin/devel`;
an unset or empty variable defaults to `origin/devel`. An explicit `--base-ref`
takes priority over the environment, including an explicit empty value (which
Git refuses). A nonempty invalid or unavailable ref fails without falling back.
The selector never fetches. The owner must provision that ref and enough local
history for `git merge-base` to succeed. Committed, staged, unstaged and
untracked files are included by default. `--no-worktree` and
`--no-include-untracked` are available for callers checking committed inputs.

## Owner provisioning

Outside the sandbox, install GCC/G++, Clang/Clang++, clang-tidy (prefer version
18), CMake, Ninja, ccache, Git, Python 3.10 or newer, and the Linux development
packages used by `.github/workflows/ci.yml`: `build-essential`,
`extra-cmake-modules`, `libfreetype6-dev`, `libgl1-mesa-dev`, `libglu1-mesa-dev`,
`libwayland-dev`, `libxkbcommon-dev`, `pkg-config`, `python3-numpy`,
`python3-pil`, `wayland-protocols`, `xorg-dev`, `xauth`, and `xvfb`.
Workers cannot install missing packages.

The committed `tools/ci/deps-lock.json` records the declared revisions with
`status: unresolved`, null commits, null source digests, and null submodule
maps. It is a provisioning template, not a usable snapshot. Both `verify` and
`stage` refuse it. The owner must run the following **with network access,
outside any sandbox**, from this clone, using a fresh empty cache directory:

```bash
: "${LUMINUMBRA_DEPS_CACHE:?Set a fresh dependency snapshot directory}"
nice -n 10 python3 -I -B tools/ci/offline_deps.py snapshot \
  --cache "$LUMINUMBRA_DEPS_CACHE" --lock tools/ci/deps-lock.json
nice -n 10 python3 -I -B tools/ci/offline_deps.py verify \
  --cache "$LUMINUMBRA_DEPS_CACHE" --lock tools/ci/deps-lock.json
```

`snapshot` shallow-fetches each declared revision, resolves tags to commits,
checks out detached sources, and initializes submodules recursively with
shallow fetches. The lock includes every recursive submodule revision and a
SHA-256 source digest covering relative paths, file contents, executable bits
and safe relative symlinks. Git bookkeeping is excluded. GLAD's bundled Python
generator, `gl.xml` and `khrplatform.h` must be present. The entire source tree,
including generator resources and submodule contents, is retained. Publish the
resolved lock with its matching snapshot; keep the cache read-only while any
workers use it. Verification reads Git identities locally and never fetches.
It reads committed gitlinks with tree plumbing and checks every initialized
submodule HEAD, without running submodule status. All cache Git commands ignore
system/global configuration and inherited Git environment overrides, disable
filesystem monitors, hooks, SSH commands, credential helpers and automatic
submodule recursion, and deny transports. Only snapshot fetch/update commands
allow HTTPS. Metadata symlinks and submodule gitdir, common-directory and
alternate-object references must stay inside the cache; dangling metadata links
are refused. Redirections are checked recursively, including relative alternate
paths and alternates in common directories or other object stores. Blank and
`#` comment lines in alternate lists are ignored. Only LF separates alternate
paths; other separator bytes remain part of the pathname checked for containment.
Write these lists with LF endings on every platform. Redirection cycles and chains
deeper than 32 edges are refused, as are quoted alternate path encodings.
Inherited object-directory and alternate-directory environment overrides are
removed before Git runs. Top-level source `.git` directories cannot be symlinks.
Internal metadata links are checked before Git runs; a linked config receives the same
execution restrictions as an ordinary config.

All 18 declared dependencies must be present:

```text
entt glm glfw glad nlohmann_json joltphysics freetype rmlui soil2
miniaudio meshoptimizer lua_upstream sol2 spdlog imgui_src stb
googletest lz4_upstream
```

A vendor revision or dependency-set change requires a new snapshot and lock.
Never fill in a digest by guessing, or mark an unresolved lock resolved by
hand. A failed snapshot leaves a partial cache; retry into a new empty cache.

FastNoise and the RenderDoc header are committed locally. Keep Diligent, Tracy,
GNS and Steam disabled for this baseline. Changes to those configurations need
separately provisioned dependency profiles; Steam needs an owner-provided SDK.
The baseline database cannot validate disabled configurations. A changed TU
missing from that database fails explicitly.

## Worker recipe

Run from the clone after provisioning. All generated sources, temporary files,
objects, logs, dependency copies, build state and compiler-cache entries belong
under this clone's `scratch/`. Keep the shared dependency cache read-only.
Run `prepare` before creating temporary/cache directories or exporting their
paths. It checks the checkout and original scratch path components, fresh stage
and init paths, and every existing entry under `tmp`, `ccache`, `local-gcc` and
`local-tidy`. Symlinks, Windows reparse points, non-directory path components,
special files and escaped paths are refused before directory creation. Ordinary
nested directories and files in reused build/cache trees are allowed. Checks
are fresh on every invocation.
Staging uses ordinary copies, never writable hardlinks. Destinations and the
initial CMake cache must be fresh paths strictly inside `scratch/`; path
traversal outside that directory, a symlinked `scratch/` root, and any symlinked
component between the checkout root and either output path are rejected before
creating directories or files. Ordinary nested output paths are allowed. Reuse
a verified stage only with its matching lock; remove an old stage before staging
again.
The initial cache is written to a temporary file in its destination directory
and published with an atomic replacement. A failed write or publication removes
the temporary file and staged sources, so the same paths can be retried.

```bash
set -euo pipefail

: "${LUMINUMBRA_DEPS_CACHE:?Set the provisioned dependency snapshot path}"
base_ref="${LUMINUMBRA_BASE_REF:-origin/devel}"

nice -n 10 python3 -I -B tools/ci/offline_deps.py prepare \
  --root "$PWD" --cache "$LUMINUMBRA_DEPS_CACHE"
export TMPDIR="$PWD/scratch/tmp"
export CCACHE_DIR="$PWD/scratch/ccache"
export CCACHE_MAXSIZE=250M
export CCACHE_COMPILERCHECK=content
export PYTHONDONTWRITEBYTECODE=1
export GIT_OPTIONAL_LOCKS=0

nice -n 10 python3 -I -B tools/ci/offline_deps.py stage \
  --cache "$LUMINUMBRA_DEPS_CACHE" \
  --lock tools/ci/deps-lock.json \
  --dest scratch/offline-deps \
  --init scratch/offline-init.cmake

configure_local() {
  nice -n 10 cmake --preset release -B "$1" \
    -C "$PWD/scratch/offline-init.cmake" \
    -DFETCHCONTENT_BASE_DIR="$PWD/$1/_deps" \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DCMAKE_C_COMPILER="$2" \
    -DCMAKE_CXX_COMPILER="$3" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBUILD_TESTING=ON \
    -DLUMINUMBRA_ENABLE_PREFAB_RUNTIME=ON \
    -DLUMINUMBRA_ENABLE_DILIGENT=OFF \
    -DLUMINUMBRA_ENABLE_TRACY=OFF \
    -DLUMINUMBRA_ENABLE_GNS=OFF \
    -DLUMINUMBRA_ENABLE_STEAM=OFF \
    -DFASTNOISE2_NOISETOOL=OFF \
    -DFASTNOISE2_TESTS=OFF \
    -DGLAD_REPRODUCIBLE=ON
}

configure_local scratch/local-gcc gcc g++
configure_local scratch/local-tidy clang clang++

nice -n 10 cmake --build scratch/local-gcc --target glad --parallel 2
nice -n 10 cmake --build scratch/local-tidy --target glad --parallel 2

nice -n 10 python3 -I -B tools/ci/changed_tus.py compile \
  --root "$PWD" \
  --build-dir scratch/local-gcc \
  --base-ref "$base_ref" \
  --worktree --include-untracked \
  --ccache --jobs 2

nice -n 10 python3 -I -B tools/ci/changed_tus.py tidy \
  --root "$PWD" \
  --build-dir scratch/local-tidy \
  --base-ref "$base_ref" \
  --worktree --include-untracked \
  --config-file .clang-tidy \
  --also-ci-gate --jobs 2
```

The two `glad` target invocations prepare generated headers and the GL loader
only; verify these work with networking disabled and bundled inputs before
handing the snapshot to workers. No other targets are built. Reconfigure both
trees after build-system, dependency, compiler or flag changes. Keep configuration
and GLAD preparation output in `scratch/` if saving logs.

Budget **3–8 GiB additional scratch space per clone**, beyond approximately
445 MiB of inspected source/data and Git history. Reserve about **10 GiB** for
broad header edits. This is an unmeasured allowance: record actual peak disk
use during rollout. The allowance covers copied dependencies, two configured
trees, generated headers, selected Release objects, and a **250 MB ccache**.
Do not copy runtime assets. Use `nice -n 10` and `--jobs 2` on the shared
machine. Remove temporary directories after checks; remove only scratch trees
owned by the current checkout and no longer in use.

## Selection and diagnostics

First-party TUs are `.c`, `.cc`, `.cpp` and `.cxx` files below `src/`,
`include/`, `test/` and `tools/`, excluding the upstream RmlUi GL3 backend at
`src/luminumbra_client/ui/gl3/`. Changed sources select every database command
variant and the variants that include them. Changed sources and headers scan
every configured first-party variant using its actual compiler, defines, include
paths and working directory, both normally
and with `__clang_analyzer__`. Transitive, conditional and generated headers
are included. New headers are scanned too, so newly shadowing includes count.
Literal `configure_file`, `file(COPY_FILE)` and single-file `file(COPY)` outputs
are mapped back to their inputs, including chains of copies, only when every
inspected CMake script uses the supported literal assignment/copy syntax.
Control flow (`if`/`elseif`/`else`, `foreach`, `while`), functions, macros,
includes, other commands and unsupported syntax invalidate all mappings.
The reader does not interpret CMake or guess which branches execute. This
conservatively selects the full configured scope for header edits in the current
repository, including its engine-version generation script. Unresolved copy
relationships, generated dependencies with unknown inputs, and changed headers
without a proven consumer select the full configured scope, with the fallback
reason included in the report. Header edits never silently select no work.

Deleted sources/headers and build/flag/dependency or unknown native inputs
select the full configured scope. Scans which fail, omit the TU, name missing
dependencies, or cannot be parsed fail the check. Missing changed-TU database
entries fail even for optional backends, unless dependency scans prove that the
source is an included-only input consumed by configured translation units.
Unsupported wrappers, response files, plugins, modules and flags creating extra
outputs fail visibly. Compiler
options use an allowlist; unknown options are refused before execution. Exact
codegen flags include PIC, exception/RTTI controls, fast-math controls,
`-fno-stack-protector`, `-fno-fat-lto-objects`, `-fpch-instantiate-templates`,
`-flto`/`-fno-lto`, `-flto=auto`/`-flto=thin` and `-ffp-model=precise`.
The x86 ISA allowlist accepts enable/disable forms for avx, avx2, avx512dq,
avx512f, bmi, f16c, fma, lzcnt, popcnt, sse, sse2, sse3, ssse3, sse4.1 and
sse4.2; arbitrary `-m*` and `-f*` options remain refused. Bare `-W` is preserved.
Output options such as `-o`, `-MF` and `-MT` are consumed and removed;
include paths such as `-isystem` are consumed and preserved. Supported
compiler commands are GCC/Clang drivers, optionally wrapped by ccache/sccache;
commands are argument vectors, never evaluated by a shell. Option values are
consumed according to an explicit arity table and cannot count as source
operands. Every entry must have exactly one positional source matching its
normalized database `file`; mismatches, multiple
sources and missing sources fail before any compiler runs. CMake's paired
`-Xclang -include -Xclang HEADER` is normalized to a textual `-include` only
when the header exists and resolves inside the build tree. Binary
`-include-pch FILE` (including its paired `-Xclang` form) is dropped only when
FILE resolves inside the build tree and is the matching HEADER plus `.pch`.
This lets dependency scanning, compilation and tidy use the textual header
without requiring a prebuilt binary PCH; compile correctness must not depend
on that cache. GCC's `-include cmake_pch.h`/`cmake_pch.hxx` likewise requires
an existing header resolving inside the build tree. Other ordinary GCC/Clang
textual `-include` arguments are preserved and can supply the matching header.
Every forced include (`-include` or `-imacros`) must name an existing file
directly, relative to the command directory or by absolute path. Include-search
names must be replaced with absolute paths. An adjacent `.gch` or `.pch` file,
directory or link causes an actionable refusal; remove that cache before
replaying. Both the spelled header path and its resolved target are checked,
at database validation and again for dependency, compile and tidy replay.
PCH generation (`-x c++-header`, `-Xclang -emit-pch`) remains refused for
first-party sources; dependency-only generation entries are outside selection.
Other pass-through, unknown separate-value options and MSVC/clang-cl driver
syntax are refused, including plugins, specs, tool wrappers, profile-data paths
and response files.
There is no cached dependency graph. Each invocation reports selected files,
variant/scan counts, scope, selection time and check time. No-native selections
report success.

Compile commands retain configuration/determinism options and replace object
and dependency outputs with isolated scratch objects; objects are removed on
completion. They always use `-c`, never link. `--ccache` uses the provisioned
cache; the resulting compiler arguments, including warnings, distinguish
ccache entries. The report's `command_fingerprint` covers the selected variants'
resolved source paths, working directories and complete ordered compiler inputs.
With a warning profile it also covers the appended layer, resolved diagnostic
baseline content and clean prefixes. It describes commands and calibration,
not a saved check result; checks always execute again.

By default warning flags come exclusively from each variant in
`compile_commands.json`; no warnings or fatal categories are added, removed or
reordered. A database
configured with the recipe above carries CMake's strict `-Wall -Wextra
-Wpedantic -Werror -Wno-error=unused-parameter` policy on targets routed through
`luminumbra_configure_first_party_target`. The client and QA libraries retain
their documented exclusion from that policy. A source compiled by several
targets keeps each target's own flags and exceptions. Tests derive the strict
policy from the CMake helper and check both a strict target and the excluded
client, as well as ordered warning overrides on duplicate source variants.

For an optional MSVC `/W4` approximation, add `--warning-profile msvc-approx`
to the **compile** invocation. `--warning-profile none` is the default. Each
variant's complete ordered database flags are replayed first, followed by the
deduplicated flags from the ordered `MSVC_APPROX` table in the helper:

| MSVC warning | GCC approximation | Clang approximation | Rationale |
| --- | --- | --- | --- |
| C4456 | `-Wshadow` | `-Wshadow-all` | A local declaration hides another local declaration. |
| C4457 | `-Wshadow` | `-Wshadow-all` | A local declaration hides a function parameter. |
| C4458 | `-Wshadow` | `-Wshadow-all` | A declaration hides a class member, including inherited members. |
| C4459 | `-Wshadow` | `-Wshadow-all` | A declaration hides a global declaration. |
| C4244 | `-Wconversion -Wnarrowing` | `-Wconversion -Wnarrowing` | Detect possible loss of value or precision, including C++ list initialization. |
| C4267 | `-Wconversion -Wsign-conversion` | `-Wconversion -Wsign-conversion` | Detect loss of width or changes of signedness when converting `size_t`. |

`-Wnarrowing` is omitted for C; explicit `-x c`/`-x c++` and C++ drivers are
respected. The profile adds no `-Wall`, `-Wextra` or `-Werror`. Its diagnostics
inherit each target's fatal/nonfatal policy, so excluded client/QA targets
remain nonfatal and `-Wno-error=unused-parameter` remains unchanged. If the
database explicitly configures any shadow or conversion/narrowing subgroup,
the profile conservatively leaves that entire family unchanged. This preserves
target disables, demotions and narrower exceptions such as
`-Wno-error=sign-conversion`, `-Wno-shadow` and `-Wshadow=local` across compiler
differences in group precedence. `-w` and `-Wno-everything` disable all warning
additions. Generic `cc`/`c++` aliases are identified with a local `--version`
probe; unidentified drivers fail visibly. The profile sets plain diagnostic
formatting so GCC and Clang baseline records can be parsed without source-caret
snippets or terminal colors.

These flags cover more cases than the corresponding MSVC warnings. Calibrate
on a known clean revision accepted by native MSVC CI before using the profile
as a blocking check. The following opt-in capture records diagnostics only
from selected variants; selection and base-ref rules are the same as above.
Capture every scope for which exemptions are needed, separately for each
compiler/configuration. Use a fresh local baseline destination under `scratch/`:

```bash
nice -n 10 python3 -I -B tools/ci/changed_tus.py compile \
  --build-dir scratch/local-gcc --base-ref "$base_ref" --jobs 2 \
  --warning-profile msvc-approx \
  --write-warning-baseline scratch/gcc-warning-baseline.json

nice -n 10 python3 -I -B tools/ci/changed_tus.py compile \
  --build-dir scratch/local-gcc --base-ref "$base_ref" --jobs 2 \
  --warning-profile msvc-approx \
  --warning-baseline scratch/gcc-warning-baseline.json
```

Capture first verifies the target's original database policy, then records only
diagnostics in families added by the profile. Ordinary compiler errors and
native policy failures cannot be calibrated away. Publication occurs only
after all selected variants pass these checks; existing files are never
overwritten and failures remove temporary objects and baseline files. Capture
is an explicit acceptance operation: never capture a new baseline just to make
a failing change pass. Keep baseline files local and review them against the
known clean native result.

Replay also verifies the original target policy. An exemption must match the
complete ordered command variant, diagnostic path, line, column, warning option,
message and offending source-line content. A new diagnostic, changed offending
line, changed flags, unrecognized error, or partial diagnostic stream still
fails. GCC/Clang diagnostic formats are recognized separately, and diagnostics
remain visible with an accepted-baseline count. Calibration refuses errors
with effective `-Wfatal-errors`, since early termination could hide a new error;
compiler error-limit failures also remain blocking. Baselines are versioned
JSON, read on every invocation, with no result cache. Changes to compiler
versions or dependencies require fresh calibration, as command identities do
not hash compiler binaries or dependency contents.

Repeat `--conversion-clean-prefix src/some/directory` to require a known clean
conversion scope: matching conversion diagnostics cannot be exempted, either
in that path or from headers included by a TU in that path. Prefixes must be
existing checkout-relative directories; escaped paths are refused. Outside
these scopes only exact baseline diagnostics receive exemptions; new conversion
warnings retain the target's normal fatal policy. Prefixes do not suppress
shadow or ordinary compiler diagnostics. Calibration options require the
profile; warning profiles on `tidy` are refused before selection.

This approximation cannot validate MSVC `/W4 /WX`; native MSVC validation
remains required. Changing the target's shipping warning policy still requires
updating CMake and regenerating the database using the CI configuration.

Tidy runs the repository `.clang-tidy`, whose `WarningsAsErrors` is empty.
`--also-ci-gate` additionally reproduces the curated blocking configuration
from `scripts/tidy.sh` used by `.github/workflows/ci.yml`, including its
thresholds, header filter, fatal diagnostics, and existing per-TU global-state
exceptions. The helper reads the literal configuration without executing the
script, and fails if that format is unsupported. The additional gate reads the
script's file filter too: only TUs under `src/`, `test/`, and `tools/`, excluding
the GL3 backend. Other selected TUs still receive the repository configuration.
Each command variant gets a
separate temporary database so all target-specific configurations are analyzed.
Failed compiler or tidy processes return nonzero with their diagnostics.

## Fast hosted validation

`Fast compile / GCC + clang-tidy` runs on a standard `ubuntu-24.04` runner,
including draft pull requests, with a separate 20-minute timeout. It starts
as a non-required check while warning baselines and duration are calibrated.
Existing native lanes, full-tree tidy and draft merge guards remain authoritative.
A successful fast result never substitutes for them. Ordinary small changes
target ten minutes; shared-header changes can select the full configured tree.

The job uses `tools/ci/changed_tus.py route` against the locally fetched merge
base (pull-request base SHA or push predecessor). Only paths already proven
non-native by `tools/ci/ci_paths.py` qualify for an explicit
`No native work: verified documentation/Python-only changes` success. Python
CI infrastructure, generators and unknown files take the native path. Missing
history and empty diffs require conservative full selection; manual runs use
that path too. Routing does not fetch or change the existing lane classifier.

For native work the job installs Linux configuration dependencies plus GCC,
Clang 18, clang-tidy 18 and ccache. It configures separate
Release GCC and Clang compile databases with tests exposed and Diligent off.
Configuration generates engine/stb headers; the only build target is `glad`
for its generated headers. It does not link engine/test targets or run CTest.
The `ci` command shares the local dependency selector, scans ordinary and
analyzer contexts, unions both compiler selections and replays all variants
of every selected file. A changed or cross-selected TU missing from either
database fails visibly. Unconfigured sources and unsupported platforms/backends
are listed explicitly and retain their specialized full gates.

GCC compiles use isolated scratch object paths and the shared `msvc-approx`
profile. CI independently makes all/extra/pedantic, shadow=global, conversion,
sign-conversion, narrowing, unused and missing-field-initializers fatal, even
when a target already configures a warning family.
CI accepts the first-party CMake helper's `-Wno-error=unused-parameter`, then
appends `-Werror=unused-parameter`: that helper's MSVC `/W4 /WX` policy enforces
C4100 without an exception. Local calibration preserves the original demotion.
CI conservatively refuses other per-diagnostic disables and demotions because
subgroup precedence can defeat group enforcement. Entirely warning-disabled commands
are also refused. The fast lane never reads or writes
warning-calibration baselines and adds no diagnostic suppressions. MSVC remains
necessary to validate C4456–C4459 and native conversion diagnostics.

Each selected Clang command variant gets an isolated temporary database.
clang-tidy-18 (falling back to clang-tidy) first runs the repository `.clang-tidy`
configuration, whose additional findings remain advisory; parsing/tool failures
still fail the lane. It also runs the curated fatal configuration through
`scripts/tidy.sh --files0-from FILE`. That option accepts a strict NUL-terminated
list of existing first-party sources present in the database, preserves unusual
filenames, rejects malformed/missing/outside-checkout entries, and cannot be
combined with `--changed-from`. An explicitly empty list succeeds. The CI
orchestrator allows at most four workers and uses two; each per-variant shell
gate uses one worker. Temporary databases, manifests and objects are removed
on success and failure. The existing full-scope tidy invocation is unchanged.

There are no new Actions cache restores, saves or remote cache prefixes.
Runner-local ccache has a separate directory and a 250 MB maximum. Its namespace
includes compiler version/path identity, Release configuration, resolved ordered
arguments and working directories, the fatal warning profile, build/dependency
CMake content, the dependency lock and selector/tidy policy content. ccache's
own keys additionally cover compiler content and preprocessed dependencies.
No result cache can omit selection scans or tidy. This adds zero remote storage
to the existing at-most-10-GB Actions policy and requires no retention-prefix
registration. Selected files, every command variant (working directory and
command digest; full arguments in the log), unsupported configurations,
scan/compile/tidy timings and local cache statistics appear in the job summary,
including failure diagnostics when validation fails.

Hosted validation is still needed for dependency provisioning, generated headers,
real GCC/Clang warning baselines, both tidy configurations, duration on small
and shared-header changes, and actual ccache resource usage. Mocked helper tests
do not establish native success.

## Python helper checks

These checks use stdlib Python, local Git fixtures and mocked compiler/tidy/network execution. They
do not require configuration, native compilation, linking or a test runner.
Their fixtures live under `scratch/` and are removed on completion. Use a
fresh temporary directory and remove it after the suites:

```bash
mkdir -p scratch/helper-check-tmp
export TMPDIR="$PWD/scratch/helper-check-tmp"
nice -n 10 python3 -I -B tools/ci/test_changed_tus.py
nice -n 10 python3 -I -B tools/ci/test_offline_deps.py
nice -n 10 python3 -I -B tools/ci/test_tidy_shard.py
nice -n 10 python3 -I -B tools/ci/test_workflow_policy.py
rmdir scratch/helper-check-tmp
```

The selector and dependency suites are registered in `tools/CMakeLists.txt` and in the standalone
Python plan in `tools/ci/ci_paths.py`. Their CI infrastructure paths retain
native qualification routing.

The selector regressions also run in the fast lane; shell-selection and
workflow-policy regressions run in the public-tree job. Shell tests isolate PATH and record every analyzer
invocation, including tool fallback and failure propagation.
