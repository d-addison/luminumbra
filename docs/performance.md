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
