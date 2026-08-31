# Performance measurement

Performance is evaluated per engine layer so a regression can be localized before
whole-frame profiling. Measurements compare a candidate revision with its base
using the same scenario, preset, compiler, operating system, CPU/GPU class, driver,
and runtime settings.

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

- `evaluated`: the scenario ran, required evidence exists, and comparison keys
  match.
- `unevaluated`: the platform, hardware, tool, or required evidence was unavailable.
- `failed`: the scenario attempted to run but produced invalid evidence or crossed
  an approved regression limit.

An unavailable profiler or missing GPU is never reported as a pass. The existing
software-OpenGL workflow is an observational render-path check; it is not a GPU
performance gate.

## Comparability

Machine-readable results include a schema version and a comparability key derived
from the benchmark name, scenario version, build preset, compiler and version,
operating system and architecture, CPU model, GPU and driver when applicable,
engine configuration, resolution, and sample policy. Base and candidate results
with different keys are reported as unevaluated rather than compared.

Thresholds are added only after at least 20 clean baseline runs on the intended
runner class. Initial lanes publish observations. A maintainer reviews the
distribution and then blesses an absolute floor, relative regression budget, or
both. No check may silently manufacture a zero baseline or treat absent evidence
as success.

## Automation model

Hosted pull-request lanes should cover deterministic CPU work on Windows, Linux,
and macOS when supported. They build base and candidate in the same job, run the
same scenario repeatedly, and upload raw JSON plus a Markdown comparison summary.
Hosted-runner noise warrants conservative thresholds and repeated samples.

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
