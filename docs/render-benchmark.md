# Render benchmark measurement formats

The distribution-aware measurement implementation of the
[accepted distant-world contract](distant-world.md#performance-measurement-contract)
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

Each `point` is X Y Z yaw pitch. Positions interpolate at constant speed along
nonzero Euclidean segments; angles interpolate linearly along each segment (no
implicit wrap). Duration times tick rate must equal expected ticks. Only 30 Hz,
preset revision 6 and a fresh-process cold cache are supported. The
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
  paths. These cases fail rather than skip when rendering or required content is
  unavailable. The `RenderBenchmarkAssets` CTest fixture uses the existing pinned
  asset acquisition tool and verified archive cache; absent content is acquired,
  corrupt content or failed acquisition fails the fixture. No placeholder content
  or runtime defaults are installed. The intended CTest discovery delta is eleven
  over the original base (five added in this fix round).
- `build/campaign-archives-20260907/slice-A2/receipt.json` is generated campaign
  evidence, not runtime input. It records branch, head commit, changed files,
  added tests, both CTest totals, before/after fixture hashes and deviations.
  Unavailable verification results are explicit nulls, not passing totals. A
  missing, corrupt or incompatible receipt supplies no acceptance evidence and
  must not be treated as proof of completion.
