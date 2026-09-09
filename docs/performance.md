# Performance measurement

Performance is evaluated per engine layer so a regression can be localized before
whole-frame profiling. Measurements compare a candidate revision with its base
using the same scenario, preset, compiler, operating system, CPU/GPU class, driver,
and runtime settings.

## Local commands

Use the `perf` preset for comparable measurements and `profile` when collecting a
Tracy trace. Both retain debug symbols and frame pointers; only `profile` enables
instrumentation.

```sh
cmake --preset perf
cmake --build --preset perf --target luminumbra_server_app --parallel
python tools/perf/perf.py run \
  --workload server-smoke --layer end-to-end --preset perf \
  --build-manifest build/perf/performance-build.json --mode gating \
  --parameter ticks=30 --parameter surface_radius=1 \
  --parameter collision_radius=1 --parameter seed=1337 \
  --parameter world_preset=default --fixture-hash "$(git rev-parse HEAD:data)" \
  --evidence-contract server-smoke --evidence build/perf/server-smoke-evidence.json \
  --warmup 1 --samples 20 --output build/perf/server-smoke.json -- \
  build/perf/bin/luminumbra_server_app --smoke --preset default --seed 1337 \
    --ticks 30 --radius 1 --collision-radius 1 \
    --artifact build/perf/server-smoke-evidence.json
```

Compare results only when their generated comparability keys match:

```sh
python tools/perf/perf.py compare \
  --base build/perf/base.json --candidate build/perf/candidate.json \
  --output build/perf/comparison.json
```

`bisect-eval` accepts the same arguments and returns `0` for good, `1` for a
confirmed regression, and `125` when evidence is not comparable. The runner uses
raw samples, median, p95, p99, maximum, and median absolute deviation. Its default
relative verdict requires at least 20 ordered base/head observations, a median
change of at least 5%, a paired two-sided sign-test p-value no greater than 0.05,
and an effect larger than three pooled median absolute deviations. Smaller or
underpowered changes are reported as warnings.

For populated fixed-tick stage measurements on default, mountains and
archipelago, use the [simulation budget capture command](sim-budget-telemetry.md#repeatable-capture).
It reports deterministic work separately from wall-time distributions and
leaves the normal server smoke artifact unchanged when disabled.

## Measurement layers

| Layer | Representative evidence |
|---|---|
| Algorithms | Focused microbenchmarks for hashing, serialization, field updates, meshing, and allocators |
| Simulation | Fixed-tick phase timings, work counts, queue wait, and stable state hashes |
| World streaming | Chunk generation, activation latency, mesh/upload backlog, and residency counts |
| Networking | Encode/decode cost, snapshot size, fan-out, queue depth, and loopback latency |
| Rendering CPU | Frame and pass submission time, resource churn, draw/dispatch counts, and stalls |
| Rendering GPU | Timestamped pass durations, frame-time percentiles, occupancy, bandwidth, and captures |
| End to end | Deterministic scenarios covering startup, movement, populated worlds, and sustained play |

Every result records raw samples plus median, p95, p99, and median absolute
deviation. Averages alone are insufficient for frame-time or streaming analysis.
Warm-up samples are excluded explicitly and the retained sample count is recorded.

## Result states

A measurement has one of three outcomes:

- `evaluated`: the scenario ran and all evidence required by its workload
  contract exists. A comparison is evaluated separately only when both run keys
  match.
- `unevaluated`: the platform, hardware, tool, or required evidence was unavailable.
- `failed`: the scenario attempted to run but produced invalid evidence or crossed
  an approved regression limit.

An unavailable profiler or missing GPU is never reported as a pass. The
software-OpenGL workflow enforces a deliberately generous CPU/render-path ceiling
that catches gross regressions; it is not evidence of GPU performance.

## Comparability

Machine-readable results include a schema version and a comparability key derived
from the workload and version, allow-listed scenario parameters, fixture hash,
build preset, compiler identity and version, operating system and architecture,
CPU model, GPU and driver when applicable, renderer, metric schema, and sample
policy. Base and candidate results with different keys are reported as
unevaluated rather than compared. The key is recomputed during validation rather
than trusted from the file.

### Re-baselining after a deliberate fixture change

Refusing to compare across a changed key is correct: if the workload's inputs
moved, base and candidate did not measure the same thing. But a fixture
sometimes has to change for a real reason, and every comparison against the old
base is then unevaluated, so the required check can never pass on its own.

The escape hatch is a tracked declaration at
`tools/perf/baselines/rebaseline.json`:

```json
{
  "schema": "luminumbra.performance_rebaseline.v1",
  "retired_comparability_key": "<64 hex characters>",
  "replacement_comparability_key": "<64 hex characters>",
  "reason": "why the fixture had to change"
}
```

It is deliberately narrow. It waives the comparability mismatch and nothing
else: a comparison that does evaluate and finds a regression still fails, and no
environment variable, workflow input or pull-request label can trigger a waiver,
because none of those leave evidence in the repository. It is also single-use —
both keys must match the transition exactly, so once the stored base advances
past the retired key the declaration stops applying and cannot quietly disarm
the gate for some later change. A declaration that matches nothing is recorded
in the comparison document as unapplied rather than silently dropped; a
malformed one is an error, not an ignored file.

A waived comparison reports verdict `rebaseline`, carries the reason, and is
visible in the uploaded evidence artifact, so it never reads as a clean pass.

Review it as you would the fixture change itself: confirm both keys, confirm the
justification, and delete the declaration once the base has moved past it.

Thresholds are added only after at least 20 clean paired runs on the intended
runner class. The committed policy uses relative base/head comparisons so machine
speed is not mistaken for an engine regression. No check may silently manufacture
a zero baseline or treat absent evidence as success.

The reviewed stability observation used to select this policy is recorded in
[`tools/perf/baselines/relative-calibration.json`](https://github.com/d-addison/luminumbra/blob/main/tools/perf/baselines/relative-calibration.json).
It preserves the workload, runner class, summary statistics, and hashes of the
untracked raw evidence without committing machine-scale trace data.

`world_load_bounded_test` is an opt-in local latency diagnostic. It performs
twenty cold interactive loads, records each observation, and enables named
watchdog reporting, but it has no absolute pass/fail duration. It is labeled
`manual` in CTest because hosted toolchains have materially different machine
speeds; the paired comparison above is the release performance gate.

## Automation model

The hosted Linux pull-request lane builds base and candidate in the same job,
runs deterministic server batches in ABBA order, and uploads the engine evidence,
raw samples, and comparison. Missing or incomparable evidence fails the
measurement, while an underpowered result is an explicit warning. The workflow
enforces the reviewed relative policy. Windows and macOS currently exercise the portable runner contract
only; engine measurement on those systems remains non-blocking expansion work.

Hardware rendering requires a labeled, fixed Windows runner with a pinned driver,
power profile, display mode, and background-service policy. That lane runs nightly
and on demand, retains raw captures, and should become merge-blocking only after its
availability and baseline stability are proven.

Compiler and dependency caches may reduce setup time. Build trees, benchmark
results, and blessed baselines are not restored as caches; results are immutable CI
artifacts, while approved baselines are reviewed repository data.

## Profiling and triage

Use the lowest layer that reproduces the regression, then correlate upward:

- Tracy supplies cross-platform CPU zones and frame timelines when enabled in a
  dedicated profiling build.
- Windows Performance Recorder/Analyzer and GPUView diagnose scheduling, I/O, and
  CPU/GPU queue interaction; RenderDoc captures frame state.
- Linux `perf` supplies sampled call stacks and hardware counters where runner
  permissions allow it.
- macOS Instruments and `xctrace` provide time, allocation, and GPU traces.

Profiling builds and captures are diagnostic evidence, not comparable benchmark
numbers. Preserve the exact scenario metadata and raw trace with the associated
measurement so a change can be traced from end-to-end symptoms to a subsystem and
then to code.

## Render capture state

The default `--render-benchmark` camera is `(8,56,8)`, yaw `35`, pitch `-6`,
with render time of day `0.04`. Streaming and rendering use that pose after
player simulation. Explicit `--cam-pos` or scene camera settings take precedence;
an explicit scene also overrides time of day and FOV. This ordering corrects
[#120](https://github.com/d-addison/luminumbra/issues/120): older default captures
reported a camera/time pin applied after rendering, then overwritten by the next
player and simulation update. Those captures cannot qualify the intended forest view.

Benchmark JSON includes `render_uniforms`, the linked geometry/static/lighting
shader state read while emitting the artifact, and `capture_context`, including
controller presence and the last position passed to world streaming. The readback
runs outside the measured interval. It observes the report frame; it does not prove
that every shader drew or that every measured frame used identical state.

With the real controller active, validate a default capture and separate explicit
camera/time overrides using the file-only checker:

```sh
python3 tools/perf/validate_render_capture.py forest.json \
  --position 8 56 8 --yaw 35 --pitch -6 --tod 0.04 \
  --require-controller --require-distinct-controller --require-geometry --require-settled
```

The checker reconstructs camera matrices and sun phase independently of reported
camera metadata. It rejects absent or inconsistent shader values. Use the actual
requested pose/time for override captures, and `--fov` to check an explicit FOV.
Keep hot reload, frame scans and interactive camera/settings changes disabled during
these controlled checks. Zero pending queues and camera agreement are separate
requirements; an elevated camera in empty air need not have a resident camera chunk.
Read actual framebuffer dimensions from the artifact. Short ordering regressions
are not performance measurements, and estimated resource totals are not measured VRAM.

### Matched terrain coverage attachments

For a coverage diagnosis, add `--render-benchmark-aovs <fresh-directory>` to a
render benchmark. The directory's parent must exist. This opt-in mode records
far-region readiness and camera-neighbourhood draw decisions on each warmup,
measured and capture frame. It then writes one additional, unmeasured frame's
`color.ppm`, `depth.pfm`, `position.pfm`, `normal.pfm`, `albedo.pfm` and
`material.pgm`, together with `manifest.json`. Use `--no-ui` for scene-only color.

The manifest identifies the rendered frame, view and actual G-buffer projection,
internal/output dimensions, jitter, time of day, per-region bounds, authority
revisions and draw decisions. `missing_after_eviction` and `stale_after_eviction`
separate absent tiles from resident tiles awaiting a tier/authority rebuild.
Live-chunk rows retain prepared mesh versions, uploaded versions, pool residency,
and visibility from the actual G-buffer culler. They cover prepared renderable
snapshots, not every requested world chunk; truncation beyond 8192 rows is explicit.
Readiness and submission counts do not establish per-pixel coverage.

PFM attachments contain unscaled floating-point values, bottom row first, with
endianness declared by the PFM scale sign. Position and normal are view-space;
normal is decoded from the production octahedral encoding. TAAU state and jitter
are recorded; keep TAAU disabled for spatial coverage comparisons because its
color contains history. Depth is OpenGL window
depth with clear value 1. PPM and PGM use top-down rows; PGM stores the material ID,
including clear ID 255. Ignore position/normal/material values where depth is
clear. These are the deferred attachments from the same rendered frame as color;
they do not represent all transparent-surface contributions to final color.

For a controlled diagnostic A/B only,
`--render-benchmark-aovs-bypass-camera-region-guard` suppresses the whole camera
region's draw guard while retaining the production 176 m near clipping and 3 km
far range. It requires AOV capture and is recorded in every region receipt. This
is an experiment control, not a supported terrain-ownership policy or a cave/edit
correctness claim. Ordinary rendering is unchanged without diagnostics.

Existing output directories are refused. Successful publication requires every
attachment and a final complete manifest; a failed capture exits nonzero and
cannot replace earlier evidence. Limits are 8191 warmup plus measured frames and
16,777,216 pixels per internal/output image. Diagnostics add CPU work during the
run, so their timing observations are labelled instrumented and must not be used
as ordinary-play performance results.

See [Terrain coverage diagnostics](terrain-coverage-diagnostics.md) for a recorded
same-camera guard/bypass pair, original images, provenance joins and reproduction
commands. Its coverage findings do not qualify a production ownership policy or
ordinary-play performance.
