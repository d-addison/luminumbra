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

- `--render-benchmark-schema v2|v3|v4` selects the artifact format; absent means v2.
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
surface flight only. The scripted workloads below use traversal v2 and artifact
v4; edited-world bake qualification remains separate.

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

## Scripted workloads (issue #403)

The same traversal parser, camera sampler, bounded frame ring and report checker
also support `luminumbra.traversal.v2` with
`luminumbra.render_benchmark.v4`. Select both explicitly; v2/v3 artifacts and
traversal v1 retain their existing contracts. V4 requires one of these scripts:

| Script in `tools/perf/fixtures` | Workload | Fixed schedule at 30 ticks/second |
|---|---|---|
| `cave.traversal` | `cave` | Prepare a connected shaft/tunnel with tick-zero digs, descend from Y=80 to Y=-8 by tick 600, traverse to X=56 by tick 1200, hold through 1800 |
| `edit.traversal` | `edit` | Dig at tick 120 and fill at tick 300, both at (8,12,8), radius 8 m; hold the camera throughout |
| `region-crossing.traversal` | `region_crossing` | Travel from X=8 to X=2056 by tick 600, return during ticks 900–1500, hold through 1800 |
| `save-load.traversal` | `save_load` | Save at tick 240, reload at 300, hold through 1200, resume the path through 1800 |
| `menu-entry.traversal` | `menu_entry` | Open pause at 120, settings at 240, return to pause at 360, resume at 480, save/leave to main menu at 600, enter that world at 900 |

All five pin seed 424242 and the same default preset/content identities as the
surface traversal. These identities are requirements, never inferred from the
machine running a capture. A changed preset or content must have its identities
recaptured and reviewed before the script can run. These scripts and the
synthetic Python test reports are definitions and contract inputs, not native
performance evidence.

V2 adds `workload <name>` immediately after the version line. The remaining
header fields retain v1 order and bounds. Each point becomes
`point <tick> <x> <y> <z> <yaw> <pitch>`, with strictly increasing ticks starting
at zero and ending at `expected_ticks`. Poses interpolate by these ticks;
identical endpoints hold exactly. `speed_mps` remains a positive descriptive
header field for compatibility; v2 key ticks determine motion, including holds.
The native parser expands at most 18001 poses once, giving constant-time lookup
per frame. A script tick advances once per measured frame regardless of wall
time. Gameplay simulation advances one fixed 30 Hz tick on unpaused gameplay
frames and stays paused during menus and warmup. Script ticks remain consecutive
across save/load and menus; `actual_ticks` counts script ticks in v4.

V4 starts from a newly created world. It refuses `--load-world` and loads of
pre-existing worlds with exit code 2 because the capture identity does not cover
initial saved state. Save/load and menu-entry actions may reload only the world
created during that capture. Each successful scripted save records a digest of
every saved file, with sorted relative paths and exact binary contents. Reload
and menu-entry actions recheck that digest before opening the world; replaced,
added, removed or renamed files refuse with exit code 2 even when the world ID
is unchanged. Presentation time holds zero during startup,
advances at 30 Hz through warmup and measurement, then holds the final script
time through report, screenshot and AOV frames. ImGui uses a fixed 1/30-second
delta throughout the capture, including startup and reloads.

After the points, v2 requires `actions <count>` and exactly that many records:
`action <tick> <name> <x> <y> <z> <radius>`. There are at most 64 actions. Dig/fill
use finite positions within ±32000 m and radii in (0,16]; other actions require
zero position/radius. Tick-zero digs are cave preparation before warmup. Other
actions have distinct, increasing measured ticks below the final tick. Each
workload permits only the action sequence in the table; unsupported versions,
workloads, malformed/truncated fields and trailing tokens refuse. Both parsers
accept LF and CRLF; scripts use ASCII tokens and whitespace, within the existing
1 MiB file bound. Files that change size during native reading refuse.

Cave preparation uses the normal terrain edit/remesh path to establish a fixed
route, rather than searching for a different entrance at runtime. Qualification
requires observed underground frames with requested/effective ladder mode,
matching active cave policies, and a positive live voxel-air sample at the camera.
An unavailable lattice or solid sample fails qualification. A legacy fallback
fails cave qualification. The region workload sets the scheduler to work limit 0,
hold 30 ticks and reduced shift 1 before world creation. It requires active
regions, drives their local anchor from the scripted camera, and records real
scheduler transitions. Validation requires both departure from Active and
reactivation, plus camera positions on different 512 m regions.

This base refuses live Ladder activation without immutable committed authority
and dirty parking. Consequently the cave script can collect traversal evidence,
but cannot qualify its active cave policy on this base. The report preserves the
requested Ladder/effective Legacy distinction and the checker fails that capture.
Renderer activation must be supplied before an owner can obtain a qualifying cave
capture; the workload does not bypass that ownership gate.

Save/load calls the normal world-state/metadata saves and shared world-load entry
path. Entry completes its existing synchronous readiness work, restores the
camera to the scripted pose, and continues normal rendering and streaming.
`workload.reload` records the originating `request_frame`, the
`first_settled_frame`, and monotonic `elapsed_ms` from the reload request through
that frame's presentation endpoint. A settled frame requires gameplay, successful
world-entry readiness, no scheduler fault, zero streaming queue depth, no
unpublished generation or meshing batches, no pending LOD chunks or promotion work,
zero terrain/water upload candidates and no foliage build backlog. Every measured
frame records `stream_generation_batches`, `stream_meshing_batches`,
`stream_pending_lod_chunks`, `stream_promotion_pending` and
`region_scheduler_fault` alongside the existing queue and readiness evidence. Validation
recomputes the settled predicate and requires the first such frame after the
request, duration within measured wall coverage, and subsequent camera motion.
The duration must include every complete frame interval strictly between the
request and settled frames (with a 0.000001 ms numerical tolerance). The two
boundary frame intervals may be covered only partially.
No timing threshold selects a camera pose or stops a script. A missing settled
frame remains null and cannot qualify.

Menu actions use the gameplay pause function, `settings.rml`, `pause.rml`, the
main-menu game state, and the normal load callback. Menu-only frames stay in the
measurement ring and whole-frame GPU interval, even when no scene pass runs.
Observed gameplay/main-menu and pause states, and the visible UI document, must
match the script. A queued document request alone is insufficient. No UI input
or filesystem save outside the requested benchmark world is synthesized.

V4 retains every v3 distribution, sample, adapter, profile and override check. Its
additional required evidence is:

| Field | Meaning |
|---|---|
| `settings.gns_enabled` | Actual compiled GNS networking setting, including false on builds without GNS |
| `settings.active_regions` | Effective enabled flag, uint64 work limit, hold ticks, reduced shift |
| `settings.far_mode` | Requested/effective far mode and requested/effective cave policy at the measured window end; each frame also records these values |
| `settings.graphics_preset` | Clearly reserved for a future named settings preset; null on this base, with `graphics_preset_status: "not_exposed"` |
| `settings.effective_render` | Effective dimensions, render scale, uncapped vsync, TAAU, SSAO/cloud/volumetric qualities, camera FOV, UI scale and window mode |
| `settings.effective_render.systems` | Every `render.*` flag and resolved scalar/vector parameter, enumerated from the shared SystemConfig registry, including sky LUT and live weather; disabled entries retain their resolved inputs |
| `settings.render_overrides` | Active render override declarations, also covered by the configuration identity |
| `settings.simulation_identity` | Existing session `SystemConfigIdentity()` for effective simulation gates and tuning, including water resolution and ecology |
| `settings.content_files` | Binary `fnv1a64:` digests keyed by portable relative paths for every file under `data/`, `res/`, `config/`, `game-assets/` and the optional `assets/audio/` tree, plus logical `inputs/` keys for explicit system-config and preset files and the user settings overlay when present. An absent audio tree is recorded as `"assets/audio": null` |
| `render_identity` | Stable `fnv1a64:` identity of the complete resolved `settings` object; recomputed by the validator |
| `windows.startup` | Monotonic startup `elapsed_ms` through world identity checking and tick-zero preparation; its foliage observation precedes rendering |
| `windows.warmup` | Every warmup `frame_wall_samples_ms`, plus distribution with p50/p95/p99/max/MAD, sample and unavailable counts |
| `windows.measured` | Full measured frame-wall distribution, identical to the top-level distribution |
| Each window's `foliage` | Actual `instances`, density-admitted `candidates`, submitted `candidate_slots`, `backend` (CPU/GPU), `observation: "window_end"` |
| `workload.actions` | Observed tick, action name, success and modified chunk count; dig/fill in the edit workload must change terrain |
| `workload.observations` | Consecutive tick/frame IDs, menu/pause state and visible document, region key/transitions, far/cave policies, underground and camera-in-air flags, terrain uploads, foliage build generation, queue counters and settled flag |

Startup is elapsed time for initialization, not a rendered frame distribution.
V4 native qualification and the performance floor require a visible window mode
(`windowed`, `borderless` or `fullscreen`); headless captures are rejected.

The content inventory deliberately includes indirect and unused assets so changes
to scatter sets, tuning, models, textures, shaders, UI styles and fonts cannot
silently compare as identical workloads. Content is streamed before runtime
initialization and checked again before writing the report; a change refuses
the capture. Inventory reads are outside warmup and measured frame intervals.
The audio tree may be absent, including in public packages used with `--no-audio`;
its absence participates in the inventory and render identity. When present,
every audio file is hashed like the other content files.
Referenced content must resolve inside the four required trees or `assets/audio/`.
The shared loader check refuses external absolute paths, parent-relative escapes
and symlinks leaving a tree, including nested JSON dependencies, shader includes, models, textures,
scatter sets, UI resources and audio banks. Audio paths escaping their allowed
tree are refused even when that tree is absent. A loader's optional-asset fallback
cannot clear this refusal. Capture mesh loading also disables build/source-tree
fallbacks.

V4 presentation time starts at zero before UI initialization and holds there
through startup. Warmup advances by one script tick per captured frame;
measurement uses `(warmup_frames + tick) / tick_rate` seconds. The same clock
drives shader time (water and foliage wind included), previous-frame animation
time, sky/cloud drift, loading visualization and RmlUi elapsed time. Reloads
preserve it, and menus continue to animate at the script rate while simulation
is paused. Every workload observation records `presentation_seconds`, which
the validator checks against its tick. Other captures and normal play retain
the existing wall-clock sources.

Warmup and measurement contain all their frame-wall samples, independently of
slow-frame logging. The first warmup wall interval starts after preparation;
measured intervals exclude startup, warmup and the report frame. Foliage counts
are explicit window-end snapshots, not totals summed across frames or capacity
markers. CPU fallback and GPU scatter collect candidate counts at admission. The
GPU diagnostic reads only the current indirect draw count at window boundaries;
it may wait there, outside measured frame intervals. The first measured interval
starts after the warmup boundary observation, so that readback wait cannot leak
into it. Ordinary play and legacy captures do not request these observations.

Owner-machine example, with `BENCHMARK_OUTPUT` set to a directory outside the
checkout:

```sh
build/debug/bin/luminumbra_client_app --no-audio \
  --render-benchmark "$BENCHMARK_OUTPUT/edit.json" --render-benchmark-schema v4 \
  --capture-size 3440x1440 --perf-profile quality \
  --render-benchmark-warmup 400 --traversal tools/perf/fixtures/edit.traversal
python3 tools/perf/validate_render_capture.py "$BENCHMARK_OUTPUT/edit.json" \
  --qualified-renderer 'NVIDIA GeForce RTX 5070 Ti' \
  --traversal tools/perf/fixtures/edit.traversal
```

Use each of the other scripts identically, subject to the cave activation gate
above. Native captures remain owner
machine work. The existing `RenderContractPython` suite and standard performance
workflow run all new validator and deterministic script tests without a GPU.
`RenderWorkloadDefinitions` registers the matching native parser regression in
the existing CPU-only executable. No GPU timing case is added to CI.

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
camera representation (relative tolerance 1e-7). Expected region IDs use an
IEEE single-precision coordinate round-trip before division and floor, matching
the native camera even at region boundaries. Future traversal versions are
refused even inside a known v3 artifact. This checker does not certify the future
far-volume ladder or its 90-second settle obligation.

Use `--reference-capture reference.json` with the same expected traversal and
adapter arguments to validate two captures and require equal render identities.
Changes to default `systems.json` flags or parameters change this identity even
without a CLI or environment override. The canonical hash uses sorted objects,
length-prefixed UTF-8 strings, typed values and little-endian IEEE double bytes;
it is independent of JSON formatting and platform byte order. Both v4 captures
must contain the complete registry and a matching recomputed identity. V2 and v3
reports remain accepted individually; they lack the complete configuration
needed for this comparison.

The registered native CPU regressions cover script dispatch order and ticks,
reload request through first-settled present boundaries, visible menu state,
CPU/GPU foliage count selection, and registry/configuration identity. They need
no GPU timing or graphics context.

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
binary path and SHA-256, the 180-second limit, monotonic process elapsed seconds,
and outcome `returned`, `timed_out` or `spawn_error`. `return_code` is null when no
exit status was returned; exception type/message describe timeout or launch
failure. Binary identity is null if the executable could not be read, and elapsed
time is null if execution was never attempted. Hashing and evidence finalization
are outside the recorded process interval. No capture-completion timestamp or
teardown duration is inferred from these receipts.

`capture_status` is `retained`, `missing`, `invalid_json`, `read_error` or
`write_error`; retained bytes have a path and SHA-256. A retained document with an
unsupported schema remains diagnostic evidence and still fails normal validation.
`evidence_errors` records persistence failures where a receipt can be written;
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
continues to force vsync off. Spatial parity, upscale seam and terrain AOV captures
explicitly disable TAAU and record zero jitter; normal-default captures retain the
temporal path. Validators accept preset revisions 6 and 7. The shipping hills
traversal now declares revision 7; its pinned identities need recapture from CI
after the terrain and preset changes.
