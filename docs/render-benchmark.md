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
writer retain their existing behavior. All three benchmark schemas allocate a bounded
frame ring for far-mode evidence; v3 and v4 issue additional GL queries. Ordinary
play allocates no measurement ring. No system configuration entry is added.
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
An incomplete artifact write makes the client exit unsuccessfully for either
schema, including open, directory creation, partial write, flush and close errors.
Workload options accept both separate values and assignment form (for example,
`--render-benchmark-frames=4`) without becoming debug overrides. Assignment form
never consumes the following argument; other CLI options still require exact
override declarations.

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

## Far-mode reporting and Ladder baselines

V2, v3 and v4 include `far_world.frames`, one row for every measured frame,
and `far_world.summary`. Warmup and the report/screenshot frame are excluded.
Measured windows in all three schemas are bounded to 1..18000 frames; oversized
requests fail before allocating sample storage. Rendering selection, authority
binding, scheduling and submission are unchanged by this instrumentation (issue #392).

Each row records `frame`, `requested_mode`, `effective_mode` (`legacy` or
`ladder`), `refusal` (the exact selection enum name), `submission_refusal` (the
exact submission enum name), `suppressed`, and `coverage_settled`. Suppression
includes both the submission override and submission refusals such as
`LiveOwnershipIncomplete`; these can leave the effective mode as Ladder.
Coverage is settled only with a complete captured and advanced Ladder snapshot,
available water, a nonempty wanted set, current covering and settled counts equal
to wanted in every tier, no pending/building/ready work, rejected or evicted tiles,
horizon refusal, incomplete water coverage or draw refusal. Tile and tier wanted
counts must agree. This reports current far coverage, without
certifying the independent station expected-set or 90-second qualification gate.

Each row's `settings` records resolved `gns_compiled` (standalone
GameNetworkingSockets build availability, including both enabled and disabled
builds), `water_cells_per_side` (actual water solver resolution), `active_regions`
(the session value) and `grass_density_scale` (the foliage pass value, or `null`
when absent). Row `content_identity` is the immutable far authority's decimal
uint64 identity, with `"0"` meaning unavailable. `world_content_identity` records
the loaded terrain/content hash; v3 additionally retains its existing workload
preset revision, preset identity and world content identity. The summary contains
count maps for `requested_mode`, `effective_mode`, `refusal`,
`submission_refusal`, plus `suppressed_frames` and `unsettled_frames`. Each row
also includes cached `streaming` counts for `generation_pending`,
`meshing_pending` and `terrain_uploads_pending`, plus `generation_outstanding`
and `meshing_outstanding` booleans. The counts describe deferred work; the
booleans include dispatched batches awaiting publication and the promotion
pipeline. A frame is counted as unsettled if its far coverage is unsettled,
any streaming count is nonzero, or either outstanding flag is true. Terrain
isolation reports `suppressed=true` with `submission_refusal=IsolationSuppressed`
for both Legacy and Ladder, because the G-buffer gates their shared submission
path on the Terrain layer.

Startup logs the current requested/effective mode, selection refusal, suppression
and submission refusal. Subsequent mode or reason changes and submission
suppression/refusal transitions log once per transition. Coverage settling alone
does not generate a mode-transition log. SLOWFRAME lines include these far-mode
fields and the settled flag.

Any run submitted as a **Ladder baseline** must use `--ladder-baseline`:

```sh
python3 tools/perf/validate_render_capture.py capture.json \
  --qualified-renderer 'NVIDIA GeForce RTX 5070 Ti' --ladder-baseline
python3 tools/perf/perf.py validate capture.json --ladder-baseline
```

The first command performs full v3 measurement validation plus the Ladder rule.
The second checks only the measured far-mode evidence in either benchmark
schema; it does not establish timer, adapter or performance validity. With the
full v2 checker, also supply its existing pose/TOD arguments. The Ladder rule
rejects missing, malformed, noncontiguous or incomplete frame evidence,
inconsistent summary counts, and any measured frame requesting/effectively using
Legacy, reporting a refusal, suppressed submission, unsettled far coverage or
pending streaming work, or an unavailable (zero) far authority content identity.
It cannot be waived by override declarations or by a passing final report frame.
Older captures without these additive fields remain diagnostic inputs when the
Ladder-baseline rule is not requested; they cannot qualify as Ladder baselines.
The existing registered FarWorldDiagnostics C++ cases cover transition decisions,
coverage and serialization. The registered performance and render-contract Python
suites cover refusal controls and repair of individual measured frames.
`RenderBenchmarkMeasuredFrameBounds` registers strict v2/v3 frame-count refusals,
and `RenderBenchmarkIsolationSubmission` checks isolation reporting. The
existing native runner cases require far-mode rows and the dynamic startup log.

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
preset_revision 8
preset_identity 56ffbe1771cddfa6
content_identity 7583622726659997707
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
preset revisions 6, 7 and 8 and a fresh-process cold cache are supported. The
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
frames. The same frame decision gates physics and water initialization and stepping;
menus, warmup (including extra upload-settling frames), and report frames preserve
simulation state while streaming, meshing, uploads, and rendering continue. Extra
reload-settling passes also disable world simulation. Script ticks remain consecutive
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

**Cave workload status: awaiting activation.** This base refuses live Ladder activation without immutable committed authority
and dirty parking. Consequently the cave script can collect traversal evidence,
but cannot qualify its active cave policy on this base. The report preserves the
requested Ladder/effective Legacy distinction and the checker fails that capture.
Renderer activation must be supplied before an owner can obtain a qualifying cave
capture; the workload does not bypass that ownership gate.

Save/load calls the normal world-state/metadata saves and shared world-load entry
path. Entry completes its existing synchronous readiness work, restores the
camera to the scripted pose, and drains streaming and rendering before resuming
script ticks. The reload action keeps its presentation tick fixed and uses zero
render delta during these passes. Loaded water is paused for the extra residency
updates and resumes on every exit; these passes do not present additional frames.
Their work stays inside the request frame's CPU, wall and GPU interval. Each complete pass segment resolves its timestamps
before query reuse and contributes to the request frame's per-pass totals;
incomplete segments still invalidate the measurement. It waits through at least
one residency activation interval and terminates successfully only when every settle gate is satisfied.
A watchdog refuses 300 consecutive passes without a new high-water mark in
resident chunks, ready chunks, admitted meshes or foliage build generation;
shrinking or oscillating counters do not reset it. This bounds stalled work
without a total-frame deadline for a progressing restream.

Schema v4 also accepts `--render-benchmark-stream-radius` in separate or assignment
form: 0 preserves uncapped streaming, and 1..12 caps the wanted streaming radius
in chunks through the existing deterministic diagnostic cap. It is applied on
both world creation and reload, before surface preparation. Other schemas refuse
the option. It remains a debug override requiring an exact declaration and is
included in capture identity. The synchronous `--horizon-radius` and
`--collision-radius` options remain separate.

`RenderWorkloadSaveLoadIntegrated` and `RenderWorkloadNoAudioIntegrated` use radius
4 for all three controls and a 0.4-second, 12-tick fixture at 30 Hz. They save at
tick 1, reload at tick 2, hold the camera through tick 6 and resume within the same
chunk through tick 12. The 81 prepared columns retain the unchanged readiness
minima of 64 renderable chunks and nine collision chunks. The reload still drains every settle gate before advancing
the script; the short fixture removes the redundant 240-tick hold. Loader,
action, quiescence, resumed-camera and identity checks remain required. These
synthetic integration fixtures do not change the committed 60-second qualification
workloads. `RenderRunnerEvidencePython` checks the actual launch options and
declaration for both audio variants; `RenderBenchmarkStreamRadiusRefusal` and
`RuntimeSurfaceOptionsTest` cover schema and numeric admission. Native timing and
rendering verification remain pending.

`workload.reload` records the originating `request_frame`, the
`first_settled_frame`, the positive `settling_passes` count, and monotonic
`elapsed_ms` from the reload request through that frame's presentation endpoint. A settled frame requires gameplay, successful
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
| `settings.graphics_preset` | Nonempty selected base preset name, with `graphics_preset_status: "named"`; null presets and `"not_exposed"` status cannot qualify v4 captures. The effective graphics block records its revision and saved/session overrides |
| `settings.effective_render` | Effective dimensions, render scale, uncapped vsync, TAAU, SSAO/cloud/volumetric qualities, camera FOV, UI scale and window mode |
| `settings.effective_render.systems` | Every `render.*` flag and resolved scalar/vector parameter, enumerated from the shared SystemConfig registry, including sky LUT and live weather; disabled entries retain their resolved inputs |
| `settings.render_overrides` | Active render override declarations, also covered by the configuration identity |
| `settings.simulation_identity` | Existing session `SystemConfigIdentity()` for effective simulation gates and tuning, including water resolution and ecology |
| `settings.content_files` | Binary `fnv1a64:` digests, captured after any startup graphics settings save, keyed by portable relative paths for every file under `data/`, `res/`, `config/`, `game-assets/` and the optional `assets/audio/` tree, plus logical `inputs/` keys for explicit system-config and preset files and the user settings overlay when present. Each subdirectory, including empty ones, has a null entry with a trailing `/`; scan roots are not entries. Directories count toward the 65,536-entry bound, and their trailing slash counts toward the 4096-byte path bound; depth remains limited to 64. Enumeration order does not affect identity. An absent audio tree is recorded as `"assets/audio": null` |
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

The gallery keeps thumbnail content under the same validation. During capture it
omits optional personal ratings from `photos/*.photo.json` without reading those
sidecars or setting content refusal. Normal play still shows saved ratings.

V4 presentation time starts at zero before UI initialization and holds there
through startup. Warmup advances by one script tick per captured frame;
measurement uses `(warmup_frames + tick) / tick_rate` seconds. The same clock
drives shader time (water and foliage wind included), previous-frame animation
time, sky/cloud drift, loading visualization and RmlUi elapsed time. Reloads
preserve it, and menus continue to animate at the script rate while simulation
is paused. Every workload observation records `presentation_seconds`, which
the validator checks against its tick. Other captures and normal play retain
the existing wall-clock sources.

Frame, camera, action and region-transition observations use fixed records allocated
before warmup; JSON serialization runs after measurement. Storage is bounded to
18,000 frame records and camera poses, 64 actions, and 65,536 transitions across
the capture. Excess transitions refuse before that frame's transitions are copied.

Saved-world reloads copy at most 65,536 entries, 4,096 UTF-8 bytes per relative
path, 64 directory levels and 512 MiB of payload into an owned snapshot. Reads
use one regular-file handle per source file. Only a snapshot matching the recorded
save digest reaches the loader; subsequent live-save changes cannot change its
metadata, preset, clock, ledger, chunks, plants or energy input. A mismatch refuses
before the world swap, and capture refusal disables persistence on exit. The
snapshot is temporary; subsequent saves still target the configured save root.

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
adapter arguments to validate two captures and require equal render identities
and warmup frame counts. Warmup shifts the presentation clock, so different
counts produce different animation and cloud phases even with equal settings.
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
around the process call. Launch identity hashing and JSON receipt publication
are outside the recorded process interval. On Windows that interval includes
debug-event handling and any failure-only native evidence collection; it is
not a frame measurement. No capture-completion timestamp or
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

On Windows, both render process runners launch one child with
`DEBUG_ONLY_THIS_PROCESS`. The launching thread owns the debug loop; first-chance
application exceptions reach the application's handlers, and second-chance
exceptions are passed back to Windows after capture. A monitor/API failure fails
closed without relaunching. Timeout and monitor failure terminate, detach and reap
the owned child before fixture cleanup. The existing process deadlines and
executable identity guard remain in force. Expected refusal exits and successful
runs produce no dump or image copies. POSIX retains its existing launch behavior.

Failure evidence uses a fresh `native-failure-*` directory beside the process
receipt, under `test-artifacts` for integrated tests. The `native_diagnostics`
receipt field records exception code, instruction address, access type/address,
registers, loaded module paths/bases, retention status and image hashes. A normal
minidump retains exception context, thread stacks and modules; full process memory
is excluded. Capture requires the faulting thread's stack/context and rejects
partial or mismatched dumps. Timeout evidence explicitly reports no exception dump.
The collector holds loaded image file handles and copies the launch executable
only when its SHA-256 matches the launch receipt, plus relevant GL/Vulkan modules
including Mesa. Adjacent PDB and debug files are retained as symbol candidates;
their debugger build identity still needs validation. Missing or failed evidence
is an explicit error and cannot make a crash or timeout pass.

Collection caps are 512 live module handles, 32 image/symbol files, 512 MiB total
image/symbol bytes, 1 MiB copy blocks and a 64 MiB dump enforced before each dump
write. Image retention has a five-second budget after failure; dump callbacks
respect the original process deadline. Per-fixture output names and fresh native
evidence directories isolate parallel CTest cases and reject stale dumps. Both
Windows CI lanes preserve Release optimization, enable compiler debug information
and upload the entire `test-artifacts` tree even after test failure. The original
native access violation remains unresolved until matching CI evidence locates its
cause; these diagnostics claim no rendering fix.

Windows CTest also registers `RenderCrashCaptureWindows.test_success_and_expected_refusal`
and `RenderCrashCaptureWindows.test_unhandled_access_violation`. These use real
debug events with eight-second child deadlines: expected exits retain no images,
and an unhandled native-thread read fault requires a validated bounded dump,
registers and a matching retained executable. Their logs and receipts remain in
`test-artifacts/render-debugger` for both Windows uploads. Device-free mocks do
not establish native debugger correctness; native smoke execution and diagnosis
of the original render crash remain pending Windows evidence.

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
temporal path. Validators accept preset revisions 6, 7 and 8. The shipping hills traversal
now declares revision 8 and sampling revision 2. Its preset identity was regenerated
with the canonical checksum oracle; the terrain content identity is unchanged.
See the provenance beside `surface-flight.traversal`.

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
scatter input preparation, plant rebake, event polling, residual menu work and
residual UI work, together with the phases below. Rebuilds from preview and QA
callers also enter the foliage bucket; any nested rebuild time is subtracted
from its render, menu or poll parent. SLOWFRAME preserves its existing
field order and appends `foliage`, `menu` and `ui`; with attribution enabled,
`other` excludes all listed exclusive buckets. The legacy `foliage_inst` field
remains rebuild time only.

The CSV has 72 fields. Existing fields retain their zero-based indices;
`dirty_columns` remains at index 62, after all terrain, water and phase fields.
The nine water subzone fields below follow it at indices 63–71.
Within each range below, columns appear in the listed order:

| Indices | Columns |
|---|---|
| 0–12 | `frame`, `frame_ms`, `sim_ms`, `stream_ms`, `render_ms`, `present_ms`, `foliage_ms`, `scatter_ms`, `rebake_ms`, `poll_ms`, `menu_ms`, `ui_ms`, `other_ms` |
| 13–21 | `signature_ms`, `sort_ms`, `surface_ms`, `repack_ms`, `hash_ms`, `upload_ms`, `palette_scratch_ms`, `dispatch_ms`, `cpu_fallback_ms` |
| 22–29 | `rebuild_calls`, `reason_mask`, `admitted_columns`, `surface_misses`, `deferred_columns`, `copied_bytes`, `hashed_bytes`, `uploaded_bytes` |
| 30–36 | `scratch_bytes`, `backlog`, `path`, `fallback`, `instance_capacity`, `generated_count`, `count_generation` |
| 37–43 | `terrain_work_ms`, `terrain_overshoot_ms`, `terrain_attempts`, `terrain_adopted`, `terrain_work_bytes`, `terrain_byte_overshoot`, `terrain_pending` |
| 44–50 | `water_work_ms`, `water_overshoot_ms`, `water_attempts`, `water_adopted`, `water_work_bytes`, `water_byte_overshoot`, `water_pending` |
| 51–61 | The phase columns below |
| 62 | `dirty_columns` |
| 63–71 | `water_selection_ms`, `water_seed_inputs_ms`, `water_initialization_ms`, `water_sim_ms`, `water_seam_ms`, `water_bookkeeping_ms`, `water_precomputed_columns`, `water_fallback_columns`, `water_stale_columns` |

Issue #402's CPU wall-time phase columns follow the terrain and water fields,
at indices 51–61, in order:

| Column | Work measured |
|---|---|
| `ui_update_ms` | RmlUi layout/update |
| `ui_render_ms` | RmlUi rendering, excluding nested phases |
| `ui_filters_ms` | UI filter submission, including blur |
| `menu_scene_ms` | Scenic menu and preview scene rendering or cached-image presentation |
| `preview_adoption_ms` | Adopting a completed preview world |
| `document_load_ms` | Synchronous UI document loading |
| `texture_load_ms` | UI texture loading and upload |
| `shader_load_ms` | Shader source loading, compilation and linking |
| `shader_first_use_ms` | First program binding and first draw/dispatch submission, including raw compute programs |
| `world_create_callback_ms` | Synchronous world-creation callback work |
| `world_load_callback_ms` | Synchronous world-loading callback work |

These phases are exclusive: an enclosing phase excludes its nested phases and
direct foliage rebuild time. `render_ms`, `menu_ms`, `poll_ms` and `ui_ms` now
exclude the phases completed inside each measured parent. `foliage_ms` likewise
excludes shader load/first-use phases completed inside a rebuild. In particular,
`menu_ms` no longer includes attributed scenic/preview or RmlUi work, `ui_ms`
excludes the separate in-game RmlUi phases (and still includes final ImGui work),
and `poll_ms` excludes attributed synchronous world callbacks. `other_ms` also
excludes all appended phases. The corrected identity is:

```text
frame_ms = sim_ms + stream_ms + render_ms + present_ms + foliage_ms
         + scatter_ms + rebake_ms + poll_ms + menu_ms + ui_ms
         + ui_update_ms + ui_render_ms + ui_filters_ms + menu_scene_ms
         + preview_adoption_ms + document_load_ms + texture_load_ms
         + shader_load_ms + shader_first_use_ms
         + world_create_callback_ms + world_load_callback_ms + other_ms
```

CSV rounding can introduce a small difference. The remainder is not clamped:
a negative value exposes overlapping inputs. These are CPU submission costs;
first-use collection adds no GPU waits or readbacks.

Water subzones (issue #400) are nested inside `stream_ms`. At indices 63–71,
the CSV appends
`water_selection_ms`, `water_seed_inputs_ms`, `water_initialization_ms`,
`water_sim_ms`, `water_seam_ms` and `water_bookkeeping_ms`; the opt-in
`water-split` slow-frame log reports the same split. They aggregate every water
update in the frame and must not be added to the exclusive buckets.
Selection includes grid migration. Seed-input time covers cache adoption and
fallback sampling; at High, initialization then only fills the grids from those
exact inputs. Live initialization at other resolutions retains the existing
inline rest-level sampling. Boot prepares all session resolutions in batches of
at most 64 columns, then initializes them on the owner thread in selection order.
Input jobs retain a detached sampler and copied terrain inputs, never live chunk
storage. Rejected or incomplete jobs use the same inputs on the owner thread.

`water_precomputed_columns` counts ready High payloads used during initialization;
`water_fallback_columns` counts missing, unfinished or stale High payloads needing
fresh sampling, and `water_stale_columns` counts generation mismatches discarded within
those fallbacks. Existing terrain jobs prepare the half-cell terrain/rest arrays
under the world sampling scope. Terrain and edit generations guard their use;
readiness never changes initialization selection, its six-column live cap, or
simulation publication. These counters and inputs are neither hashed nor persisted.

Foliage stage times are CPU wall time, including GL submission calls, without
GPU waits or new readbacks. `signature_ms` includes polling existing evidence
and early-out handling. `surface_ms` covers complete uncached column builds
(including CPU placement in fallback), never individual candidate timers.
`repack_ms` covers GPU column ordering and slot preparation excluding surface builds.
`cpu_fallback_ms` is inclusive of its surface builds and mapped uploads; do not
add it to those sub-times. Palette/scratch time includes null-data storage
allocation; `scratch_bytes` records that allocation separately from actual
`uploaded_bytes`. The final `dirty_columns` field counts surface-slot uploads,
including reconstruction after storage growth or compute-resource recreation.
Unchanged columns transfer zero surface bytes. Copy bytes include the sorted
working copy, column metadata, dirty-slot batch scratch, completed readback
snapshots and CPU emitted/mapped records; hash bytes describe
the unchanged evidence input hash. Frozen qualification rebuilds are identified
as `frozen_gpu`; their submission time includes output clears and existing
readback submission, including the first rebuild started by qualification.

Surface storage uses deterministic free-list reuse and geometric growth up to
512 slots (64 MiB). Exceeding the admitted-column cap uses the existing CPU
fallback for that frame. Growth and compute-resource recreation rebuild slots
from the CPU surface cache. Palette and scratch storage persist between builds;
column and slot-order metadata share one bounded 18 KiB buffer and upload only
when changed. Adjacent dirty runs of three or more slots share a transfer using
bounded CPU scratch; runs of one or two keep direct per-slot transfers. Clean
slots are never copied or transferred. Evidence hashes retain
the logical full-build byte order independently of physical slot assignment.

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

Use the dedicated `profile` variant for Tracy captures (issue #395):

```sh
cmake --preset profile
cmake --build --preset profile --target luminumbra_client_app
```

This uses `RelWithDebInfo`, frame pointers, and `LUMINUMBRA_ENABLE_TRACY=ON`
in `build/profile`. Start a matching Tracy viewer and launch the client from that
build. Tracy runs on demand, recording when the viewer connects. Configuring this
variant fetches the upstream source; for offline use, supply an already
provisioned checkout with `-DFETCHCONTENT_SOURCE_DIR_TRACY=/path/to/tracy`.
The fetch settings in `cmake/tracy.cmake` pin Tracy v0.13.1 to an exact
upstream revision; check out that same revision for offline use. Tracy uses the BSD-3-Clause licence; its verbatim notice is in
[Third-party licenses](https://github.com/d-addison/luminumbra/blob/devel/THIRD_PARTY_LICENSES.md#tracy-v0131) and must accompany
redistributed Tracy-enabled binaries.

The `perf`, default, debug and release configurations disable Tracy. Their event
macros expand to nothing, discard arguments, and add no profiler symbols,
allocations, threads, dependency links or Tracy network fetch. Use `perf` for
uninstrumented benchmark results and determinism gates. The shared worker library
and client/server consumers inherit the same profiling definition in a profile
build.

Enabled-build qualification is pending a successful native CI run. The existing
`GNS transport / compile probe` job configures the `profile` preset and builds
the common library, server, shipping client and QA client against the pinned
Tracy source, plus compile-only fixtures for enabled and disabled macro forms.
This checks Linux/GCC compilation and linking, including loader/world jobs,
capture presentation and inline rendering headers. Source-contract checks alone
do not qualify the enabled build. No local profile configure or application
build has been run for this change. Windows/MSVC, Windows/GCC and macOS enabled
builds, profiler connection/capture behaviour and GPU execution remain
unqualified by this compile check. Release/perf determinism and measurement
lanes continue to use Tracy disabled.

Stable timeline names include `Client frame` (scope begin/end, including early
exit), the `Client` frame mark, `Worker job execution`, and `Job queue delay (ms)`.
Capture-only rendering uses `Client capture frame` and `Client capture` marks.
The queue plot measures from ring insertion to execution start for both priority
lanes and both single and batch dispatch. Its timestamp storage and clock reads
exist only in profile builds. Worker thread names identify the job pool, world
preview, audio propagation and saved-world catalog workers. Disposable TCP accept
threads retain their `TCP accept worker job` zone without explicit thread names,
which Tracy retains for the process lifetime.

`Job queue lock wait`, `Job completion lock wait`, `World chunks lock wait`,
`Chunk state lock wait`, `Worldgen epoch lock wait`, the hydrology and far-build
lock zones cover acquisition only. Condition waits, job completion waits, TCP
future waits and `Client present wait` have separate scopes. File zones cover
world/save reads, durable writes and replacement on Windows and POSIX, and both
preset loading paths. `Shader compile` and `Shader link` cover driver calls in
the material, compute, foliage, impostor and both UI shader paths.

`GL <PassClass>.execute*` zones measure CPU submission for each major pass,
including offscreen/preview consumers. Depth blits and opaque snapshots have
their own `GL` zones. These complement the existing foliage rebuild/upload zones
and frame-attribution buckets. GPU elapsed times still come from the existing
asynchronous GPU queries; CPU submission zones do not imply GPU completion.
All profiling values are observational: scheduling, wait predicates, simulation
ordering and authoritative decisions never consume them.

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

Terrain and water adoption (issue #390) uses independent presentation budgets.
The named, compile-time validated defaults in `rendering/UploadBudget.h` are:

| Queue | Payload bytes/frame | Attempts/frame | Progress floor | Time/frame |
| --- | ---: | ---: | ---: | ---: |
| Terrain | 4 MiB | 8 | 1 | 3 ms |
| Water | 1 MiB | 8 | 1 | 1 ms |

Valid ranges are 1 byte through 64 MiB, 1 through 64 attempts, a floor from 1
through the count limit, and 0.1 through 16 ms. The time interval starts before
candidate discovery, lifecycle cleanup and ordering. Terrain also charges the
shared snapshot preparation. Payload copying, foliage ground-query construction
and GPU submission are inside the same interval. The byte cost is the checked
vertex-plus-index payload size, charged once per attempt before copying, even
when an attempt fails. It is separate from driver storage capacity and the
existing counters for successful payload copies.

After the progress floor, another upload starts only if its entire payload fits
the remaining byte budget and the time budget has not expired. The count ceiling
is unconditional. An indivisible upload can exceed time; the progress floor can
also exceed bytes. Both overshoots are measured, including preparation that has
already exceeded the time limit before the first attempt. A valid pending upload
therefore always gets at least one attempt; a failed or superseded payload is
retried without blocking other pending work. New arrivals are ordered by distance,
new before replacement, then chunk ID. Existing pending work retains FIFO position,
including when refreshed to a newer version, so nearer arrivals cannot starve it.

Resident terrain and its foliage support stay available while replacements wait;
pool growth allocates before releasing the old slice. Water uses a staging slot
and publishes it only after both writes and attribute validation succeed, retaining
the old buffers on failure in both Legacy and Ladder modes. The presented water
version supplies both draw eligibility and far-water ownership while a replacement
waits, preventing overlapping live and far surfaces. Empty water replacements use
the same bounded adoption queue and remove the resident draw when adopted.
The spare-water-slot cache remains bounded at 1024 entries. These queues never gate
world publication, activation, collision readiness, water simulation or hashes.
Legacy and Ladder far-tile integration retain their separate existing policies.

The attribution CSV appends `terrain_` and `water_` fields for `work_ms`,
`overshoot_ms`, `attempts`, `adopted`, `work_bytes`, `byte_overshoot` and `pending`.
They aggregate every integration call in a presented frame, including menu and
preview calls. Pending is the sum of each call's remaining backlog observations,
not a global count of unique chunks. These are inclusive sub-measurements of the
existing render/menu buckets; do not add their times again to the exclusive frame
sum. Coverage frame observations expose the same work statistics per integration;
the benchmark's final streaming record includes its settled status and limits.

Benchmark warmup is a minimum: its final warmup frame waits for both upload queues
to settle. Scene captures, survey stations, world visual sweeps, frame scans, parity captures,
benchmark screenshots and terrain AOVs also wait for adoption. Traversal's measured
frames keep normal budgets and include backlog; its final capture waits again.
Use `--frame-attribution-csv` during fixed-view and traversal benchmarks to compare
settled zero-adoption frames against per-frame streaming work and overshoot.
Additional settling does not advance traversal ticks or measured frame IDs.
The benchmark records extra presented settling frames in `upload_settling_frames`
(`warmup` and `capture`), separately from the requested minimum `warmup_frames`.
The held final benchmark warmup and report frames, standalone capture waits and
synchronous survey/sweep upload drains advance both upload lanes with the same
limits, but defer scene preparation after adoption and omit GPU drawing while
either queue remains pending. The frame that reaches residency draws normally
before warmup completes or capture proceeds. This avoids repeating a full
software-rendered scene for each single-upload progress pass. Required settling
frames, every measured frame, interactive rendering and previews draw normally;
there is no software-renderer exemption from the upload budget.

Settling uses work counts, with no clock input to world state. The initial pending
terrain-plus-water backlog allows one render pass per item. Work published by a
render pass adds one pass per observed arrival, up to 8192 arrivals per wait;
continuous publication beyond this allowance fails explicitly. Successful adoptions
are counted when detecting arrivals, including when the backlog stays level or
shrinks. Persistent failures consume the remaining passes. The same policy covers
synchronous qualification drains and presented warmup/capture waits. Failure returns
an incomplete result, writes a failed capture record where applicable, and exits
standalone captures with a nonzero status. The existing frame-scan watchdog remains
in effect. AOV observations are capped at the requested warmup plus measured frames
and one capture frame (at most 8192 records); settling retries do not append records.
