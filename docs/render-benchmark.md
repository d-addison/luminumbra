# Render benchmark measurement formats

The distribution-aware measurement implementation of the
[accepted distant-world contract](distant-world.md)
is opt-in. It establishes measurement validity, without setting or enforcing
performance budgets or qualifying the far horizon. Native Windows captures and
activation evidence remain separate work.

## Compatibility and commands

`--render-benchmark output.json` continues to write
`luminumbra.render_benchmark.v2`. The default frame count (120), warmup (60),
3840×1600 capture size, configured render scale, legacy GPU timer ring and v2
writer remain the legacy path. Omitting every new option allocates no measurement
ring and issues no additional GL queries. No system configuration entry is added.
World generation, hash order, saves, preset revision 6, LMR1 container v2, world
manifest v1 and FSD2 payload v3 are unaffected.

New options are:

- `--render-benchmark-schema v2|v3` selects the artifact format; absent means v2.
  Unknown versions are refused with exit 2 before world creation.
- `--capture-size WxH` requests an undecorated framebuffer of that pixel size,
  with framebuffer scaling disabled. Each dimension must be in 1..16384 and the
  actual framebuffer must match; otherwise exit 2. Absent preserves the legacy
  pinned size. Scenario captures cannot use this option.
- `--perf-profile quality|performance` seeds the existing render-scale path with
  1.0 or 0.67. Absent preserves the user setting and existing environment override
  precedence. A conflicting environment override is observable, and cannot pass
  named-profile validation even if declared.
- `--traversal path` selects the strict script format below and requires v3.
  Its duration owns the measured window; `--render-benchmark-frames` is refused
  because the script tick count owns the measured frame count. A script owns the camera and declares the world seed/preset;
  conflicting scene, fixed-camera, scenario, debug-goto, profile-fly
  and timelapse modes are refused. Missing or corrupt scripts are fatal (exit 2).
- `--declare-render-overrides path.json` requires v3 and reads an object of exact
  override-name/string-value pairs. Absent means an empty declaration. Unreadable,
  malformed or non-string declarations are refused with exit 2. Unknown names do
  not activate features: validation fails unless declarations equal observations.

Capture size and profiles also work with explicitly selected v2. They are inert
when absent. Other new options require a benchmark output path. V3 automatically
creates a fresh world unless `--load-world` is supplied, and refuses scenario
commands. A loaded traversal world must match its script seed, preset and content identities before
measurement starts; disagreement is refused with exit 2. V3 needs
1..18000 measured frames and 1..18000 warmup frames. It does not silently
truncate a window, drop overflow samples, or substitute a missing timer with zero.
V3 writes JSON in binary stream mode; the v2 writer retains its existing mode.
An incomplete v3 artifact write makes the client exit unsuccessfully.

Example fixed view (the shader-state checks remain optional for v3):

```sh
build/debug/bin/luminumbra_client_app --no-audio \
  --render-benchmark build/quality.json --render-benchmark-schema v3 \
  --capture-size 3440x1440 --perf-profile quality \
  --render-benchmark-warmup 400 --render-benchmark-frames 240
python3 tools/perf/validate_render_capture.py build/quality.json \
  --qualified-renderer 'NVIDIA GeForce RTX 5070 Ti'
```

Add `--traversal tools/perf/fixtures/surface-flight.traversal` to the client
command (omit the fixed-view frame count), and supply that same `--traversal`
path to the checker. This example advances 1800 host ticks (60 seconds of
simulation at 30 Hz), one tick per measured frame. The camera is selected from
the simulation tick count before streaming and rendering. The measured samples
cover ticks 1 through 1800 in order; warmup holds the tick-zero pose, and neither
warmup nor the report frame advances traversal simulation. Repeating a script
therefore produces identical emitted camera samples and tick counts. Achieved
wall duration is an observation and may be shorter or longer than simulation
duration; it does not select camera poses or end the traversal. The measured
frame count equals the expected tick count, within the 18000-frame capacity.
Streaming transients inside the measured window are included. The example is
surface flight only; cave-walk and edited-world save/bake qualification are not
supplied.

## Artifact v3

V3 keeps the existing v2 report fields, including `avg` and `avg_ms`. In v3 their
five measured metric means and per-pass means use the correctly attributed,
drained samples. Incomplete means are `null`; the other legacy CPU attribution
and optional NVML means keep their existing meanings. `avg.gpu_frame_ms` is a new
whole-frame mean. Per-pass sums and legacy `avg_ms.total` remain attribution,
not the whole-frame GPU measurement. The old `bound` label is only a legacy
heuristic. The additive keys have these meanings:

| Key | Required contents and units |
|---|---|
| `frame_samples` | Ordered measured rows; exactly `frames` entries, IDs `warmup_frames` through `warmup_frames + frames - 1` |
| Row `frame`, `gpu_frame` | Integer originating frame IDs, equal even when queries resolve late |
| Row `frame_wall_ms`, `cpu_submit_ms`, `present_ms`, `gpu_frame_ms`, `gpu_pass_sum_ms` | Finite nonnegative milliseconds, or JSON `null` for unavailable measurements |
| Row `passes` | Objects for `shadow`, `gbuffer`, `ssao`, `ssao_blur`, `lighting`, `water`, `skybox`, `particle`, `foliage`, `aerial`, `final_blit`; each contains boolean `issued` and number-or-null `ms` |
| `distribution` | One object for each of the five row metrics, with `unit: "ms"`, `sample_count`, `unavailable_count`, `p50`, `p95`, `p99`, `max`, `mad` |
| `adapter` | Nonempty `vendor`, `renderer`, `version`, read from `GL_VENDOR`, `GL_RENDERER`, `GL_VERSION`; unavailable strings are `null` |
| `profile` | `name` (`legacy`, `quality`, `performance`), actual `render_scale`, output `width`, `height`; consistent with legacy dimensions, scale, and rounded internal extent |
| `excluded_windows` | Inclusive integer `first_frame`, `last_frame`, nonempty `reason`; exactly warmup and the one report/screenshot frame, no overlapping or hidden measured exclusions |
| `debug_overrides` | `active` and `declared` maps; exact string values for CLI, environment and observed runtime overrides |
| `workload` | `kind` (`fixed_view` or `traversal`), actual numeric world `seed`, `preset` provenance name, resolved `preset_revision`, `preset_identity`, loaded `content_identity`, `expected_frames` (completed frame count; equal to expected ticks for traversal) |
| Traversal workload additions | Verbatim UTF-8 `script`, diagnostic `script_path`, `duration_seconds`, `tick_rate`, `speed_mps`, `expected_ticks`, observed `actual_ticks`, `measured_duration_seconds`, `cold_cache`, observed `camera_samples` |
| Camera sample | Simulation `tick` (consecutive, first 1, last expected tick), three-component `position` in metres, `yaw` and `pitch` in degrees, recorded from the rendered camera |

The whole-frame GPU timestamp pair encloses frame submission from the main-loop
measurement start through all scene and UI GL work before swap. CPU submit ends
immediately before swap; present ends immediately after swap; wall is measured
between those successive present endpoints. Legacy slow-frame logging is outside
the v3 present interval and inside the following present-to-present wall interval.

The measured sample ring has an explicit capacity equal to the requested window
(maximum 18000). The additional eight-slot GPU query ring polls completed results
and attaches them by originating frame ID. Reusing a still-pending query slot
blocks to drain it rather than discarding the result; this wait is included in
CPU and wall time. Every pending query is drained before serialization and before
query destruction. An unsupported timer or reversed timestamp remains `null`.
An unissued optional pass has `issued: false, ms: null`; its known absence of work
contributes nothing to attribution. An issued pass without a valid result makes
the pass sum unavailable and the capture invalid. Reissuing the same pass timer
within one measured frame is likewise invalid; it cannot replace an earlier
interval silently.

Percentiles sort samples, use position `(n - 1) * fraction`, and linearly
interpolate the floor and ceiling samples exactly as `tools/perf/perf.py` does.
MAD is the median of absolute deviations from p50. Statistics over available
samples are reported even for incomplete windows, with explicit unavailable
counts; an entirely unavailable distribution has all five statistics `null`.
Such captures are diagnostic artifacts, never valid performance evidence.

All unrecognized CLI options are recorded as `cli:<option>` with the following
argument, or `"true"` for a flag. Benchmark/workload, camera-pose, window-mode,
auto-create/enter, load-world, no-audio and no-menu-backdrop options are not debug overrides.
The renderer's environment knobs are recorded as `env:<name>` (including render
scale, synchronous GL debug output (`LUMIN_GL_DEBUG`), atmosphere, moon, grading, cave AO, cloud/SSAO quality, tree impostors,
scent decals, backend, visual sweep and job throttle). Runtime wireframe, debug
view and time scaling observed during measurement are also recorded. Any active
entry lacking the same declared value invalidates the capture. Declarations
cannot waive missing timers, a wrong GPU, a mismatched profile or workload.

Windows executables export the read-only DWORD `NvOptimusEnablement = 1` and
integer `AmdPowerXpressRequestHighPerformance = 1` with C linkage. These are
adapter-selection hints; actual adapter qualification still checks GL identity.
The exports do not exist on non-Windows builds.

## Traversal script v1

`tools/perf/fixtures/surface-flight.traversal` is the committed example. The ordered
whitespace-delimited UTF-8 format is:

```text
luminumbra.traversal.v1
duration_seconds 60
tick_rate 30
speed_mps 8
seed 424242
preset default
preset_revision 6
preset_identity 026f24207a8ffc7e
content_identity 16636010052247870746
expected_ticks 1800
cold_cache true
points 3
point 8 80 8 0 -6
point 248 80 8 0 -6
point 488 80 8 0 -6
```

Each `point` is X Y Z yaw pitch. The workload advances **one fixed simulation tick
per measured frame**, with path spacing of `speed_mps / tick_rate` metres per tick
along nonzero Euclidean segments. The declared speed sets path spacing per tick,
not a wall-clock speed. Angles interpolate linearly along each segment (no
implicit wrap). `duration_seconds` is simulation duration; duration times tick
rate must equal expected ticks. A 60-second script at 30 Hz therefore measures
1800 frames and does **not** necessarily take 60 seconds of wall time. The achieved
wall duration is recorded as an observation in `measured_duration_seconds`, not
used to pace the workload. Only 30 Hz,
preset revisions 6 and 7 and a fresh-process cold cache are supported. The
cache declaration concerns engine world/tile caches at launch, not driver shader
caches or OS caches. Warmup subsequently populates the engine caches.
Seed is a uint32; the preset is a nonempty lowercase identifier using letters,
digits, underscore or hyphen. There must be 2..1024 points, a path at least as
long as duration times speed, finite coordinates within ±32000 m, finite yaw,
and pitch strictly between -90 and 90 degrees. Speed and duration are positive.
`preset_identity` is the 16-digit lowercase FNV-1a checksum (the existing
`StableChecksum` algorithm) of the resolved preset's compact, key-sorted
`nlohmann::json::dump()` UTF-8 representation. `content_identity` is the decimal
uint64 returned by the existing `ComputeTerrainParamsHash` over the loaded world
seed and terrain parameters, including loaded biome and structure content hashes.
The artifact records both identities and the resolved preset's actual `schema_rev`
as `preset_revision`, for fixed views as well as traversals. A saved world's
embedded `preset.json` takes precedence exactly as in world loading; its metadata
preset name is only provenance. The script pins both expected identities, and the
client refuses any mismatch with exit 2 before measurement. Missing, malformed or
future-revision identities are refused; no identities are inferred for old scripts
or artifacts. This adds no world/save fields and changes no existing hash algorithm.

The script is limited to 1 MiB. Unknown trailing fields, incomplete records,
unsupported versions, invalid values and missing files are refused. Nothing is
migrated or inferred from another script version.

## Checker, refusal and evidence

`tools/perf/validate_render_capture.py` continues to accept existing v2 captures
with the existing requested-position/angles/TOD and optional report-frame checks.
Omitting any of `--position`, `--yaw`, `--pitch` or `--tod` for v2 fails explicitly
with the missing argument names. When v3 report-frame checks are requested, the
checker returns their numeric `max_absolute_errors` alongside a separate top-level
`measurement` result; measurement-only validation keeps its existing result shape.
V3 additionally calls `tools/perf/render_contract.py`. A missing, malformed or
future artifact schema is refused. For v3, absent required keys, corrupt types,
nonfinite values, incomplete frame/timer coverage, incorrect statistics or means,
misattributed queries, invalid profiles, undeclared overrides, malformed exclusions,
or inconsistent workloads fail with exit 1. Unknown additive keys within a known
artifact version are ignored; unsupported schema versions are never downgraded.

V3 requires `--qualified-renderer` with the **exact** qualified GL renderer string;
`--qualified-vendor` optionally requires the exact vendor too. Use the qualified
driver's complete renderer string, including any suffix. Neither the fixture
nor an artifact can declare itself qualified. Requesting adapter qualification of
v2 fails because v2 has no identity evidence. Whole-frame GPU time must be at least
the same frame's pass sum minus 0.05 ms, a timestamp precision allowance rather
than a performance budget. No frame-time or simulation budget is introduced.

`--workload-manifest expected.json` is an optional object containing expected
workload fields; supplied fields must match exactly. A missing/corrupt file is
refused; absent means only self-consistency checks for a fixed view. Traversals
also require `--traversal` pointing to the expected script: exact script bytes,
manifest fields, content identities, tick count and every observed camera sample must match. Camera
comparison allows 0.002 m/degrees absolute tolerance for the engine's float
camera representation (relative tolerance 1e-7). Future traversal versions are
refused even inside a known v3 artifact. This checker does not certify the future
far-volume ladder or its 90-second settle obligation.

Committed and generated evidence has these compatibility rules:

- `tools/perf/fixtures/adapter-mismatch.v3.json` is a synthetic v3 artifact with
  an Intel renderer and otherwise valid measurement data. Against the qualified
  RTX 5070 Ti it must fail; repairing only the renderer is a positive control.
  It is not a native performance capture. Missing/corrupt fixture data fails tests.
- `tools/perf/fixtures/distribution.txt` contains a sample count, that many
  numeric samples, then p50/p95/p99/max/MAD. Both C++ and Python tests consume this
  shared oracle; missing or inconsistent fixtures fail. It is test input with no
  runtime serialization role or supported future versions.
- `RenderMeasurement.h`, `RenderBenchmarkReport.h` and `BenchmarkGpuQueries.h`
  define process-local measurement records only. Absent instrumentation selects
  the unchanged legacy path; no record is written into worlds or other artifacts.
- `test/rendering/render_measurement_test.cpp` supplies CTest cases
  `RenderMeasurementStatistics`, `RenderTraversalDeterministic`,
  `RenderTraversalRefusal`, `RenderMeasurementFrameRing`,
  `RenderMeasurementGpuQueries` (delayed readback, drain-before-reuse, missing and
  unsupported timers).
  `tools/perf/test_render_contract.py` is registered as `RenderContractPython`
  and runs in the existing Python CI lane. Its test inputs never become runtime
  defaults.
- `tools/perf/test_render_runner.py` launches the shipping client in isolated runtime
  directories. `RenderTraversalIntegrated` compares two actual captures;
  `RenderBenchmarkImplicitCreationFailure`, `RenderBenchmarkOverrideCollection`
  and `RenderTraversalEmbeddedPresetRefusal` exercise the real failure and collection
  paths. These cases run in the ordinary lanes and fail rather than skip when
  rendering or their synthetic inputs are unavailable. The `RenderBenchmarkAssets`
  CTest fixture runs `tools/perf/synthetic_render_content.py` without acquisition,
  network access or an archive cache. It generates all 18 required files and a
  matching size/SHA-256 manifest in a build-local content root: nine valid 136-byte
  LMSH triangle meshes (three parts, three LODs) and nine solid-color LTEX textures.
  The material loader requires 512×512 RGBA with ten mip levels, so even these
  synthetic textures need complete mip chains (1,398,117 bytes each). The runner
  copies the synthetic manifest and inputs into each isolated runtime directory;
  successful captures also assert mesh loading and all three material registrations
  to rule out fallback-only success. These are explicit generic renderer test
  inputs, not authored-art or performance qualification evidence, and never change
  the shipping manifest or runtime defaults. `RenderRunnerEvidencePython` exercises
  the runner's evidence handling with tiny Python child processes and requires no
  graphics device. Including this case, the CTest discovery delta is twelve over
  the original measurement base.
- `build/campaign-archives-20260907/slice-A2/receipt.json` is generated campaign
  evidence, not runtime input. It records branch, head commit, changed files,
  added tests, both CTest totals, before/after fixture hashes and deviations.
  Unavailable verification results are explicit nulls, not passing totals. A
  missing, corrupt or incompatible receipt supplies no acceptance evidence and
  must not be treated as proof of completion.

## Qualification execution record

`tools/perf/run_qualification.py --client <native-client> --out-dir <output>
--build-manifest <performance-build.json>` writes
`qualification.json` with schema `luminumbra.render_qualification.v1`.
The output directory is held exclusively with `.qualification.lock`, created with
`O_CREAT | O_EXCL`, after the output directory is validated/created and before any
run files or settings change, through aggregate publication. Directory creation
and lock creation failures without an existing lock report `cannot write` and
exit 2. A held lock refuses the run and names the lock file; it is never
automatically reclaimed. Normal returns and exceptions release the lock. After
an abrupt termination, the operator must establish that the owner has stopped
before removing its lock. Aggregate publication uses a sibling temporary file
and atomic replacement. Prior aggregates and selected cells' captures, verdicts,
retained captures and receipts are set aside as `.prev-<n>`, including for cells
that have become unready. An unevaluated cell reports no capture or adapter.

`--host-record <staged-host.json>` validates Windows OS, PowerShell and GPU driver
identity before execution, then publishes `host.json` with the aggregate under
the same lock. A run without this option removes any previous `host.json` under
the lock. If aggregate publication fails, the previous host is restored. Both
staged and previous host files must be regular files, with no symlinks, and fit
the shared 16 MiB input cap. Staged JSON must be UTF-8 (an optional BOM is accepted)
and contain at most 64 object/array levels; invalid inputs refuse with exit 2.
Dry runs preserve the current pair and write supplied host evidence separately
as `host-dryrun.json`.

Cell `artifact` paths, and retained `capture_path` values in process receipts, are
relative to the output directory, such as `cells/surface-flight.quality.3840x1600.json`.
The private process receipt still records absolute command, cwd and client paths.
Its `binary_sha256` identifies the launched executable. The qualification runner
adds `client_path` and a `client` object with the actual client code's `sha256`,
`kind`, `sha256_verified`, `native_build`, manifest hashes and refusal `reason`.
For `.py` clients, the executable hash identifies Python and the client hash
identifies the script. Both script and native client hashes are checked before
and after execution; a changed or unverifiable client makes the cell fail.
Existing client paths, including bare `.py` filenames, resolve from the caller's
directory before the child enters its per-cell working directory.

Every launched cell receives `--systems-config` with the absolute path to the
selected runtime's `data/common/systems.json`. The client parses this option in
`src/luminumbra_client/core/RuntimeScenarioConfig.cpp` and strictly loads the
override in `src/luminumbra_client/main_client.cpp`. Each process receipt records
`systems_config.path`, `systems_config.sha256` and `systems_config.sha256_verified`.
A missing/unreadable configuration prevents launch; changed or unreadable bytes
after execution fail the cell before the checker runs.
The runner writes a per-cell `render-overrides.json` declaring the exact
`cli:--systems-config` argument. Runtime and save roots select file locations and
are excluded from render overrides.

Every launched cell receives an absolute `--runtime-root`. The runner resolves
the selected client executable before launch and searches only its directory and
ancestors for `res/shaders/g_buffer.vert`, `data/audio/music.bank.json` and
`worlds/atlas/presets/default.json`. For other layouts, supply the runner's
`--runtime-root <absolute-directory>` argument. An incomplete explicit root fails
without falling back to discovery. The output directory and per-cell working
directory never select runtime assets, including when output is inside a checkout.

Each cell also receives its own absolute `--save-root` at
`<out-dir>/work/<cell-id>/storage`, with world saves and the world catalog beneath
`worlds/saves` there. Its working directory remains `<out-dir>/work/<cell-id>`.
Process receipts record `runtime_root` and `save_root` alongside the exact command.
The client refuses relative, empty, missing-value or repeated root options,
runtime roots missing required files, and save roots that cannot be created.
`GameSession` uses the save-root override for world creation, loading, saving and
catalog inspection; runtime assets still come from the runtime root. Ordinary
client launches without either option retain discovery and runtime-root storage.
Embedded presets resolve biome and structure dependencies from the runtime's
`data/` directory. Legacy saves without an embedded preset use the runtime's
frozen or named atlas preset, even when storage is separate.

Traversal qualification binds both the script path and its expected SHA-256.
Every traversal entry in `tools/perf/qualification/matrix.json` requires
`script_sha256`: 64 lowercase hexadecimal digits for the exact script bytes,
or null for a blocked traversal whose `script` is null. A missing or malformed
digest is a matrix input error (exit 2). Before launching a traversal, the runner
compares the fixture bytes with that expected digest; a mismatch records an
invalid cell and exits 1 without launching the client or checker.
The runner snapshots each manifest into `work/<cell-id>/workload.traversal`
(at most 64 KiB) and passes that same file to the client and checker. It verifies
the snapshot after each process and records its digest and verification status
in the cell's `traversal` object. Editing the source fixture changes its digest
and prevents qualification; edits after snapshot creation cannot change that run.

The supplied manifest uses the existing `luminumbra.performance_build.v1` schema:
compiler (`id`, `version`, `target`), configuration (the existing build options
and `effective_flags_sha256`), and system (`name`, `processor`). For qualification
it must additionally contain `"client": {"sha256": "<64 lowercase hex digits>"}`,
recorded for the final native client binary after the build. The CMake-generated
manifest currently has no client hash, so it must be extended with that evidence
before use here. Missing/invalid fields in a supplied manifest are input errors.
Omitting the manifest or supplying a different executable hash permits diagnostic
captures but prevents qualification (`build_manifest_missing` or
`client_build_hash_mismatch`). Executable format checks reject interpreter
scripts, even if renamed or accompanied by a matching manifest, with
`client_not_native_build`. Batch and command files are rejected by suffix even
when they begin with an executable signature. PE images must have a valid DOS,
PE, optional and section header layout with an executable entry point.
The manifest is operator-supplied build evidence;
synthetic controls do not establish a native GPU measurement.

The checker `--verdict-out` contract is the complete
`luminumbra.render_verdict.v1` envelope returned by `render_budget.evaluate`, with
workload kind, profile, adapter, frames, `floor`, and `target`. The checker calls
this envelope `floor` internally; it does not write only its nested floor block.
The runner reconciles the envelope with the capture and selected cell. A real
checker success with a passing floor produces a passing cell, even when aggregate
qualification prerequisites remain unmet.

The child writes to a private per-invocation capture directory. The runner
publishes and checks that capture only after the process receipt records a
successful return, successful retention, the exact spawned capture path and a
SHA-256 matching both the private capture and retained bytes. The published
artifact must match those bytes. An existing public artifact without this
ownership evidence fails the cell before the checker runs.

The aggregate records `matrix.sha256`, `matrix.canonical_sha256` and
`matrix.canonical`. These are SHA-256 hashes of UTF-8 JSON with sorted object keys
and compact separators, covering every matrix field, including poses and their
provenance. The canonical hash is computed from the committed repository matrix.
A different `--matrix` path makes every cell's `matrix_canonical` false, including
a byte-identical copy; changed content is also noncanonical. Such runs include
`non_canonical_matrix` in `render_floor_qualified_reasons` and cannot qualify.

`render_floor_qualified` additionally requires all 36 canonical cells, the
canonical checker, matching RTX 5070 Ti adapter identities, passing capture/floor
checks, declared display refresh and manifest-bound native clients.
`expanded_qualified` remains false until the far-world evidence is available.
Unqualified records retain `qualification_status: "NEEDS-NATIVE-RUN"` and explicit
`render_floor_qualified_reasons`. The committed matrix still has unrecorded fixed
poses and blocked traversal workloads; completing diagnostic tests cannot
qualify it. Missing acceptance evidence is never inferred from passing tests.

## Process failure and shutdown evidence

Each `test_render_runner.py` child keeps its original 180-second process deadline,
which includes startup, capture and shutdown. A complete capture does not establish
that the process exited or that validation passed. Before its temporary runtime
root is removed, the harness retains parseable capture JSON verbatim as
`<test-method>-<case>.json`, including when the process times out or returns an
unexpected exit code. Existing exit/refusal, loader and capture checks still run;
missing or incomplete JSON is not promoted to a complete capture. Only that case's
old outputs are retired before another invocation, preventing stale evidence.

The adjacent `<test-method>-<case>.process.json` has schema
`luminumbra.render_runner_process.v1`. It records the exact command array, cwd,
binary path and SHA-256, the 180-second limit, high-resolution process elapsed seconds,
and outcome `returned`, `timed_out` or `spawn_error`. `return_code` is null when no
exit status was returned; exception type/message describe timeout or launch
failure. Binary identity is null if the executable could not be read, and elapsed
time is null if execution was never attempted. Elapsed time uses `perf_counter()`
around the process call. Hashing and evidence finalization
are outside the recorded process interval. No capture-completion timestamp or
teardown duration is inferred from these receipts.

Both receipt producers resolve the executable to its real path and verify its
device/file identifier, size, nanosecond modification/change times and SHA-256
before and after execution. The original directory preserves runtime library
discovery. `binary_identity_status` is `verified` after a matching final check;
a changed or unreadable executable at the final check records `changed`, clears `binary_sha256`,
adds an evidence error and fails the invocation. Captures remain diagnostic
evidence on this failure. An existing process exception retains priority.
Bare executable names search only absolute, nonempty PATH entries.

The parameterized runner holds output locks through executable verification,
capture retention and receipt publication. Lock identities resolve directory
aliases and, on Windows, long destination names and case aliases.
Generated qualification checker commands include `--enforce-floor` for the
16.67 ms frame-wall p99 limit. Requested poses are rounded to float32 before
comparison with client camera and streaming positions; the absolute comparison
tolerance stays unchanged.

`capture_status` is `retained`, `missing`, `invalid_json`, `read_error` or
`write_error`; retained bytes have a path and SHA-256. A retained document with an
unsupported schema remains diagnostic evidence and still fails normal validation.
`evidence_errors` records verification or persistence failures where a receipt can be written;
receipt-write failures are reported to stderr. Evidence errors never replace the
original timeout, launch error or validation assertion. A normally returning
process cannot silently succeed when required evidence persistence failed.
Capture and receipt writes use sibling temporary files followed by replacement.
The receipt describes a process outcome, not a passing test or qualified benchmark.

Terminal shutdown now emits seven timestamped `Shutdown stage:` breadcrumbs:
`query_cleanup_started`, `query_cleanup_finished`, `world_readers_drain_started`,
`far_lod_drained`, `world_readers_drained`, `world_save_started` and
`world_save_finished`. They expose query cleanup, the first far-LOD drain, other
world-reader drains and the save stage without changing their order. A finished
stage means its call returned, including a save refusal. The existing
`world_state_saved` milestone retains its successful-save condition.

These strings also join the existing `luminumbra.shutdown.v1` milestone list.
With the existing hang watchdog enabled, the incomplete record begins before
query cleanup and updates as stages advance; otherwise the record is still
written only at final shutdown. Its existing final `jobs_drained` and optional
`complete` fields keep their meanings. No runtime artifact format, watchdog
default, workload, timeout, queue policy or save behavior changes. These
diagnostics localize a future wait; they do not establish the cause of the UCRT
timeouts or a fix for the original `0xCFFFFFFF` incident.

Shipping rendering enables vsync, TAAU and tier-1 volumetrics. Benchmark startup
forces vsync off and the frame cap to unlimited, regardless of saved settings.
The same presentation override applies throughout `--scenario` and `--profile-fly`
runs, including live settings edits; it does not modify the saved choices.
Players can set `user.video.vsync` (boolean, existing default on) and
`user.video.frame_cap` (integer: 0 for unlimited, 30, 60, 120, 144, 165 or 240; default 0)
through the settings screen. Invalid fields retain their previous/default values.
The client limiter uses `steady_clock` and sleeps before spinning to its deadline.
Windows prefers a high-resolution waitable timer. If unavailable or a wait fails,
the fallback requests 1 millisecond timer resolution and releases the request at
shutdown. If that request fails, it uses the queried system timer tick (or a
15.625 millisecond default). It sleeps in whole coarse quanta that fit before the
deadline and yields for the final sub-tick remainder. A failed high-resolution
wait switches to this policy in the same frame. Other platforms and the
high-resolution timer reserve 200 microseconds. Deterministic virtual-clock tests
qualify the pacing policy only; native `SteadyFrameTiming` timer and sleep behavior
remains unqualified until measured on Windows. Its wait runs
before the frame work timers and is excluded from CPU/work buckets; benchmark
wall timing has no cap wait because measurement always runs uncapped.

Spatial parity, upscale seam and terrain AOV captures
explicitly disable TAAU and record zero jitter; normal-default captures retain the
temporal path. Validators accept preset revisions 6 and 7. The shipping hills
traversal now declares revision 7; its pinned identities need recapture from CI
after the terrain and preset changes.

## Continuous CPU attribution (issue #387)

Detailed CPU attribution is off by default. `--frame-attribution` enables the
frame collector, foliage stage timings, work counters and the `foliage-split`
slow-frame log. Disabled collectors and stage timers take a branch without clock
reads. This applies to gameplay, preview/menu and qualification callers alike.

`--frame-attribution-csv` also enables that collection and writes every presented
frame to `logs/frame-attribution.csv` in the process working directory, independently of
benchmark warmup, the 12 ms slow-frame threshold and the 400-line log window.
It is off by default, replaces the previous session's CSV, streams through a
bounded buffer and closes on normal exit. It does not change vsync or the frame
cap; “uncapped” refers to the recording duration and number of rows.

The frame interval runs from loop entry through buffer swap. Its exclusive
buckets are sim, stream, render submission, present, foliage rebuilds,
scatter input preparation, plant rebake, event polling, menu rendering and UI
rendering. `other_ms` is the remainder; these buckets plus the remainder sum to
`frame_ms`. Rebuilds from preview and QA callers also enter the foliage bucket;
any nested rebuild time is subtracted from its render or menu parent. Menu
rendering includes the menu's UI; `ui_ms` covers the separate
in-game RmlUi and final ImGui render calls. SLOWFRAME preserves its existing
field order and appends `foliage`, `menu` and `ui`; with attribution enabled,
`other` excludes all listed exclusive buckets. The legacy `foliage_inst` field
remains rebuild time only.

Water subzones (issue #400) are nested inside `stream_ms`. The CSV appends
`water_selection_ms`, `water_seed_inputs_ms`, `water_initialization_ms`,
`water_sim_ms`, `water_seam_ms` and `water_bookkeeping_ms`; the opt-in
`water-split` slow-frame log reports the same split. They aggregate every water
update in the frame and must not be added to the exclusive buckets.
Selection includes grid migration. Seed-input time covers cache adoption and
inline fallback sampling; at High, initialization then only fills the grids
from those exact inputs. At other resolutions, initialization retains the
existing inline rest-level sampling.

`water_precomputed_columns` counts ready High payloads used during initialization;
`water_fallback_columns` counts missing, unfinished or stale payloads sampled
inline, and `water_stale_columns` counts generation mismatches discarded within
those fallbacks. Existing terrain jobs prepare the half-cell terrain/rest arrays
under the world sampling scope. Terrain and edit generations guard their use;
readiness never changes initialization selection, its six-column live cap, or
simulation publication. These counters and inputs are neither hashed nor persisted.

Foliage stage times are CPU wall time, including GL submission calls, without
GPU waits or new readbacks. `signature_ms` includes polling existing evidence
and early-out handling. `surface_ms` covers complete uncached column builds
(including CPU placement in fallback), never individual candidate timers.
`repack_ms` covers aggregate GPU input assembly excluding surface builds.
`cpu_fallback_ms` is inclusive of its surface builds and mapped uploads; do not
add it to those sub-times. Palette/scratch time includes null-data storage
allocation; `scratch_bytes` records that allocation separately from actual
`uploaded_bytes`. Copy bytes include the sorted working copy, aggregate inputs,
completed readback snapshots and CPU emitted/mapped records; hash bytes describe
the unchanged evidence input hash. Frozen qualification rebuilds are identified
as `frozen_gpu`; their submission time includes output clears and existing
readback submission, including the first rebuild started by qualification.

`reason_mask` combines: 1 initial build, 2 config (enabled, evidence phase,
quantized gate wind, fade or density), 4 camera cell, 8 ground revision,
16 scatter chunks, 32 pending backlog. Components mirror the existing signature;
they do not introduce invalidation. `admitted_columns` counts columns included
in the selected build after culling and budgeting; `surface_misses` counts
uncached column builds. `deferred_columns` counts budget deferrals encountered
before the existing capacity stop; `backlog` is the existing pending-work flag.
Early-outs report zero rebuild work and `reused_gpu` or `reused_cpu`; an existing
readback consumed before the early-out still contributes its copy bytes.
`fallback` distinguishes GPU disabled/unavailable, missing resources and the
GPU column capacity limit. No readback is enabled by recording.

`instance_capacity` is storage capacity, never a generated count. The CSV's
`generated_count` and `count_generation` are empty when no count is known.
The per-frame stage times and work counters aggregate all calls on the render
thread; `rebuild_calls` reports the number of invocations, reason bits and backlog
are ORed, and differing path/fallback tokens become `mixed`. The instance
snapshot belongs to the last invocation in that frame. With no invocation,
capacity is zero and the generated count is empty. Each pass also exposes its
last collected invocation's diagnostic without affecting its rebuild decision.

The optional profiling build uses `-DLUMINUMBRA_ENABLE_TRACY=ON` in a separate
build directory. The client links the profiler interface in all variants; with
the default `OFF` setting it adds no profiling definitions or dependencies.

Completed GPU counts are only exposed with readback active and a matching build
generation. Ordinary and frozen rebuilds clear the known count until that
generation's readback completes; empty builds have a known zero count. Retained
blade snapshots keep their own source generation for qualification. CPU fallback
counts are available directly. Disabling readback retires its submission marker;
re-enabling retries the snapshot even when inputs are unchanged. Frozen rebuilds
also obey the disabled readback policy. Frame scans use `luminumbra.frame_scan.v2`:
`foliage.instances` is a nonnegative integer when known (including zero), or
`null` when unavailable. Readers must preserve unknown counts rather than
coercing them to zero. `foliage.instance_capacity` is storage capacity, never a
substitute count. The overlay labels capacity explicitly.
