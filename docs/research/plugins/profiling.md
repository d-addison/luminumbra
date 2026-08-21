# Profiling and performance tooling research

Research date: 2026-08-21

This report evaluates delivery options around the profiling and performance seams already described for Luminumbra. It deliberately separates facts verified in the permitted repository files from capabilities supplied by the task brief. It does not use README architecture claims as evidence.

## Current state

### Verified repository seams

- Tracy is isolated behind the engine-owned macros in [`Profiler.h`](../../../src/luminumbra_common/core/Profiler.h). The permitted seam exposes frame marks, scoped CPU zones, scalar plots, messages, and thread names. When `LUMINUMBRA_ENABLE_TRACY` is absent, every macro is a no-op.
- [`cmake/tracy.cmake`](../../../cmake/tracy.cmake) defines `luminumbra_profiler` in both modes, defaults `LUMINUMBRA_ENABLE_TRACY` to `OFF`, fetches Tracy at tag `v0.13.1` only when enabled, and sets `TRACY_ON_DEMAND=ON`. The file explicitly forbids enabling Tracy in determinism-gate and release builds.
- [`CaptureHooks.cpp`](../../../src/luminumbra_client/rendering/CaptureHooks.cpp) implements begin/end brackets for an already-injected RenderDoc API and an already-injected PIX capturer. It never loads those capture DLLs itself. Without an injected API it emits a sanitized `luminumbra.capture.ready:<scenario>:<backend>` marker and a diagnostic instead of pretending a capture succeeded.
- The RenderDoc path requests API 1.6.0, sets a capture-file template, and asks RenderDoc for the resulting capture path. The PIX path resolves `PIXBeginCapture2`/`PIXBeginCapture` and `PIXEndCapture`, requests a `.wpix` file, and records that requested path. PIX is Windows-only, and the source notes that a real GPU capture is expected to fail on the current OpenGL client because PIX GPU capture targets Direct3D; it becomes relevant with a D3D12 backend.
- The same capture source detects Nsight injection, but has no documented real begin/end trigger unless an NGFX Injection SDK API is supplied. That makes Nsight marker-only for production code in the reviewed seam.
- [`PERFORMANCE_ENHANCEMENT_ROADMAP.md`](../../PERFORMANCE_ENHANCEMENT_ROADMAP.md) proposes target metrics, benchmark scenarios, a hardware matrix, and regression monitoring. It is a planning document, not proof that its architecture descriptions, measurements, or projected gains match the current tree; those claims should be revalidated before being turned into budgets.

### Task-contract inventory

The task brief additionally states that the tree has an NVML sampler, FrameHealth/FrameScan, the client flags `--render-benchmark` and `--frame-scan`, and a `ForestPerfBudget` CTest. Those items are accepted as task inputs but were not independently inspected because they are outside this task's read scope. An implementation task should pin their actual command syntax, output schema, and failure semantics before automation depends on them.

### Non-negotiable determinism boundary

The `world_hash` contract is the main design constraint:

- Tracy must remain `OFF` in determinism-gate and release builds. A Tracy-enabled binary is a diagnostic artifact, never the binary used to certify determinism or release equivalence.
- Performance runs must fix and record every simulation input that can affect `world_hash`, including scenario/version, seed, tick count, and relevant flags. A result is comparable only when its `world_hash` matches the expected value for that scenario.
- Sampling and result emission must observe the simulation rather than change tick order, random-number consumption, scheduling rules, or world state. Any future benchmark feature that changes those behaviors requires an explicit `world_hash` contract review.
- GPU capture injection and profiling can perturb timing. Captures are diagnostic evidence, not baseline timing samples.

## Candidate integrations

### 1. Standard Tracy capture workflow

**Delivery form:** a Banso step, for example `perf.tracy-capture`, backed by a versioned PowerShell runner script so the same workflow can also be invoked locally.

**Effort:** Medium, approximately 3-5 engineering days after the profiling executable and scenario arguments are confirmed.

**Risk:** Low-Medium. The main risks are tool/client version mismatch, capturing the wrong window, unbounded artifacts, and accidental use of the profiling build in a gate or release path.

Tracy's upstream `v0.13.1` repository identifies it as a frame and sampling profiler and publishes matching viewer binaries and documentation. Its capture utility accepts an output file, address, port, duration, overwrite option, and memory limit; it also reports a protocol mismatch when the capture side does not match the client. The utility writes a compressed `.tracy` trace, while the matching `csvexport` utility can extract CPU zones, GPU-zone events, messages, plots, percentiles, and truncated means. These capabilities make a reproducible, non-interactive archive practical without inventing a custom trace format. See the [Tracy v0.13.1 README](https://github.com/wolfpld/tracy/blob/v0.13.1/README.md), [capture utility source](https://github.com/wolfpld/tracy/blob/v0.13.1/capture/src/capture.cpp), and [CSV exporter source](https://github.com/wolfpld/tracy/blob/v0.13.1/csvexport/src/csvexport.cpp).

The step should perform this fixed sequence:

1. Refuse gate/release configurations and configure a dedicated profiling build with `-DLUMINUMBRA_ENABLE_TRACY=ON`.
2. Resolve the Tracy capture and export tools from the same pinned `v0.13.1` package as the client.
3. Create a unique artifact directory from commit, UTC time, scenario, and attempt ID; never overwrite a previous capture silently.
4. Launch the profiled client with a fixed scenario, seed, resolution, warm-up, and bounded run duration. Use `--render-benchmark` and/or `--frame-scan` only after their exact syntax is verified.
5. Attach `tracy-capture` to the on-demand client for a bounded measured window. Fail if it cannot connect, detects a protocol mismatch, reaches its memory limit, or produces an empty trace.
6. Run `csvexport` for selected engine zones and plots. Treat CSV as a searchable summary, not as a replacement for the raw trace.
7. Archive the `.tracy` file, exported CSV, client/capture logs, and a JSON manifest containing schema version, commit, dirty-state flag, command line, Tracy version, compiler/build identity, scenario and seed, expected and observed `world_hash`, warm-up and sample durations, CPU, GPU, driver, OS, display resolution, and SHA-256 hashes of every artifact.

The current engine seam only proves CPU-style frame/zone/plot macros. Although upstream Tracy supports major GPU APIs, no engine-owned Tracy GPU-zone macro is visible in the permitted seam. The workflow should therefore promise CPU/timeline evidence first and add GPU-zone expectations only after the tree exposes and verifies such instrumentation.

**Acceptance signal:** one command produces a bounded, viewer-openable `.tracy` file and manifest; a second run with the same inputs has the expected `world_hash`; attempting the step with a gate/release configuration fails before building.

### 2. Automated frame-benchmark pipeline with trend tracking

**Delivery form:** a Banso pipeline named, for example, `perf-trend`, with a small benchmark runner and a versioned `perf-run/v1` JSON result contract.

**Effort:** Medium, approximately 5-8 engineering days for the initial scenario, schema, runner stabilization, and trend ingestion.

**Risk:** Medium. Performance noise, thermal state, driver updates, shared-runner contention, and changes in scene content can all look like regressions unless the environment and comparison cohort are controlled.

This pipeline should be the canonical producer of performance data. It should use the normal non-Tracy benchmark build, because Tracy and capture injection perturb the measurement. For each scenario it should:

- record scenario version, seed, fixed tick count, warm-up count, resolution, graphics settings, and expected `world_hash`;
- preflight power mode, CPU/GPU identity, driver, available memory, display mode, background-load policy, and NVML availability;
- run at least one warm-up followed by multiple independent measured repetitions;
- invoke `--render-benchmark` and `--frame-scan` and collect their raw outputs, FrameHealth/FrameScan summaries, NVML samples, process exit status, logs, and observed `world_hash`;
- calculate per-run and aggregate frame-time count, mean, median, p95, p99, maximum, standard deviation or median absolute deviation, slow-frame counts, CPU/GPU utilization and temperature summaries when available, and run duration;
- publish both raw observations and an immutable summary rather than only the final pass/fail decision.

The result schema should include at least:

```text
identity: run_id, commit, branch, attempt, timestamp_utc, schema_version
workload: scenario, scenario_version, seed, ticks, warmup_ticks, resolution, settings
correctness: expected_world_hash, observed_world_hash, frame_scan_status
environment: runner_id, os, cpu, gpu, driver, compiler, build_id, power_profile
metrics: frame_time_ms.{p50,p95,p99,max}, slow_frames, fps, nvml summaries
artifacts: benchmark_json, frame_scan, nvml, stdout, stderr, optional trace/capture URIs
```

Trend comparisons must stay inside a compatible cohort: the same scenario version, settings, operating system, GPU/driver class, compiler family, and runner pool. Store a rolling baseline from known-good main-branch runs, show the raw distribution, and use both an absolute budget and a relative regression threshold. A single noisy sample should request a bounded rerun rather than rewrite the baseline.

`world_hash` mismatch is a correctness failure, not a performance regression. The pipeline must stop comparison and report the mismatch separately; otherwise it risks celebrating faster execution of different work.

**Acceptance signal:** repeated runs on the pinned runner produce `perf-run/v1` records, comparable commits appear in one trend series, incompatible hardware/scenario runs are separated, and a hash mismatch cannot enter the performance baseline.

### 3. Performance-budget gates in CI

**Delivery form:** a reusable CI/Banso gate named, for example, `perf-budget`, consuming `ForestPerfBudget` plus `perf-run/v1` results from the trend pipeline.

**Effort:** Medium, approximately 3-6 engineering days after candidate 2 is stable.

**Risk:** Medium-High until dedicated runners and noise bounds are established; Low-Medium afterward. A flaky required check quickly teaches developers to ignore performance signals.

Use three gate tiers:

1. **Required, environment-independent:** execute the existing `ForestPerfBudget` CTest with its documented environment and surface its diagnostics. Its actual inputs and thresholds must be inspected in the implementation task before CI wiring.
2. **Required, pinned performance hardware:** run the canonical benchmark scenario on a labeled, thermally controlled runner. Require matching `world_hash`, valid FrameScan output, enough samples, and budgets that combine an absolute ceiling with a relative regression allowance larger than measured runner noise.
3. **Advisory, variable hardware:** collect and publish trends on ordinary agents but do not block changes on those numbers.

A failure should link to the exact run manifest, raw result, baseline commit and cohort, metric/budget calculation, and any bounded rerun. Baselines should be promoted only from reviewed main-branch runs; a pull request must never update the baseline it is being judged against. Budget changes should be reviewed as data/config changes with an explanation and before/after evidence.

Recommended rollout is observe-only, then soft warning, then required on a small stable scenario. Expand the scenario matrix only after each new case has a measured noise floor. Keep Tracy traces and GPU captures as on-failure diagnostics, not mandatory evidence on every gate run.

**Acceptance signal:** a synthetic over-budget fixture fails with an actionable comparison, a known-good fixture passes, shared-runner noise cannot block the branch, and baseline mutation requires a separate reviewed change.

### 4. GPU capture automation

**Delivery form:** an opt-in, hardware-labeled Banso job named, for example, `gpu-capture`, with separate RenderDoc and PIX adapters and strict artifact retention limits.

**Effort:** Medium-High, approximately 5-10 engineering days for RenderDoc; add 3-5 days for PIX after a D3D12 backend and compatible runner exist.

**Risk:** High. Capture injection is driver/API sensitive, files are large, captures perturb execution, and replay portability is limited. Microsoft notes that PIX captures are not generally portable across GPU hardware and driver versions. See [PIX GPU captures](https://devblogs.microsoft.com/pix/gpu-captures/).

RenderDoc is the first viable adapter for the current OpenGL client. RenderDoc supports OpenGL, Vulkan, D3D11, and D3D12, and its `renderdoccmd capture` path launches and injects into an executable with options for a capture template and waiting for exit. See the [RenderDoc project](https://github.com/baldurk/renderdoc) and [`renderdoccmd` capture implementation](https://github.com/baldurk/renderdoc/blob/v1.x/renderdoccmd/renderdoccmd.cpp). The job should use that external launcher because the engine intentionally recognizes only an already-injected module. It should then:

- run a fixed capture scenario that reaches the existing begin/end hook;
- require both the capture-ready marker and `capture_started=true`/successful completion diagnostics;
- archive the returned `.rdc` path, log, thumbnail if available, manifest, tool version, renderer/API, GPU, and driver;
- fail honestly when it receives the marker-only fallback or no capture file;
- keep the job opt-in or trigger it automatically only after a benchmark regression, FrameScan failure, or explicit label.

PIX should be deferred for GPU capture until the D3D12 backend is present. The reviewed source already documents the OpenGL incompatibility. When enabled, launch through PIX or `pixtool` so `WinPixGpuCapturer.dll` is injected before D3D12 device creation, then let the existing hook bracket the intended frame and write `.wpix`. Microsoft's programmatic-capture documentation confirms the injection/loading prerequisite and `.wpix` output, while `pixtool` can launch applications, take captures, export event-list/timing data to CSV, and extract images. See [PIX programmatic capture](https://devblogs.microsoft.com/pix/programmatic-capture/) and [`pixtool.exe`](https://devblogs.microsoft.com/pix/pixtool/).

GPU captures must never define CI timing budgets. They are diagnostic snapshots tied to the recorded tool, API, GPU, and driver. Retention should keep only failures or explicitly promoted captures, with checksums and size limits.

**Acceptance signal:** the RenderDoc job produces a replay-openable `.rdc` plus manifest on a labeled runner, marker-only fallback is reported as a failed capture rather than success, and PIX is skipped with a clear backend reason until D3D12 prerequisites are met.

### 5. Performance dashboard through Banso REST and MCP

**Delivery form:** a Banso performance-results service with a REST read/write API for ingestion and dashboard queries, plus a read-mostly MCP adapter for agent investigations.

**Effort:** High, approximately 2-3 engineering weeks after the `perf-run/v1` schema and baseline rules settle.

**Risk:** Medium. The risks are premature schema lock-in, high-cardinality metrics, artifact retention cost, authorization leaks, and misleading cross-hardware comparisons.

No permitted repository source or Banso skill documentation reviewed for this task establishes an existing Banso performance REST endpoint or MCP resource. The names below are therefore proposed contracts, not claims about current Banso behavior.

The canonical store should ingest the immutable `perf-run/v1` record and keep large `.tracy`, `.rdc`, `.wpix`, logs, and raw samples in artifact/object storage. Database rows hold typed summary metrics, cohort dimensions, checksums, and artifact URIs. An illustrative REST surface is:

```text
POST /api/perf/v1/runs                 ingest one validated run (pipeline identity only)
GET  /api/perf/v1/runs/{run_id}        run, environment, metrics, and artifact links
GET  /api/perf/v1/series               filtered cohort/time series with pagination
GET  /api/perf/v1/compare              candidate versus baseline calculation
GET  /api/perf/v1/budgets              effective budgets and provenance
GET  /api/perf/v1/regressions          open/acknowledged/resolved regressions
```

The browser dashboard should consume REST because chart pagination, caching, authentication, and drill-down are conventional application concerns. Its primary views should be scenario/cohort trends, commit comparison, percentile distributions, budget status, `world_hash`/FrameScan correctness, thermal/utilization context, and links to raw artifacts. Default filters must prevent comparison across incompatible hardware or scenario versions.

The MCP adapter should expose read-only or read-mostly data using resources such as `banso-perf://runs/{run_id}`, `banso-perf://series/{scenario}/{cohort}`, and `banso-perf://regressions/{id}`. MCP explicitly supports custom URI schemes and application-controlled resources. Narrow tools such as `perf_compare` and `perf_find_regressions` can perform server-side filtered queries, but ingestion and budget mutation should remain authenticated pipeline/REST operations rather than broadly exposed model-controlled tools. See the stable [MCP resources specification](https://modelcontextprotocol.io/specification/2025-11-25/server/resources) and [MCP tools specification](https://modelcontextprotocol.io/specification/2025-11-25/server/tools).

Start with artifacts plus a static trend report from candidate 2. Add the database/API only after real query patterns are known; add MCP last as a thin adapter over the same authorization and comparison logic, not as a second metrics store.

**Acceptance signal:** a pipeline identity can ingest a schema-valid run; dashboard/API and MCP views return the same baseline comparison; incompatible cohorts are rejected or clearly separated; artifact access is authorized and checksum-verifiable.

## Ranking

| Rank | Candidate | Delivery form | Effort | Risk | Rationale |
| ---: | --- | --- | --- | --- | --- |
| 1 | Automated frame-benchmark trend pipeline | Banso `perf-trend` pipeline + `perf-run/v1` | Medium (5-8 days) | Medium | Establishes the canonical data, correctness guard, and baseline needed by gates and dashboards. |
| 2 | Performance-budget CI gates | Reusable Banso/CI `perf-budget` gate | Medium (3-6 days after rank 1) | Medium-High initially | Converts stable measurements into regression prevention; value is high once pinned-runner noise is known. |
| 3 | Standard Tracy capture workflow | Banso step + PowerShell runner | Medium (3-5 days) | Low-Medium | Fast diagnostic value from an already-seamed dependency, with a clear determinism boundary. |
| 4 | Performance dashboard via REST/MCP | Results service, web dashboard, MCP adapter | High (2-3 weeks) | Medium | Valuable for history and agent-assisted diagnosis, but should consume the schema and comparison rules proven by ranks 1-2. |
| 5 | GPU capture automation | Opt-in labeled Banso RenderDoc/PIX jobs | Medium-High (5-10 days plus deferred PIX work) | High | Useful deep diagnostics, but hardware/driver sensitivity and the current PIX/OpenGL mismatch make it unsuitable as the first investment. |

Recommended delivery order differs slightly from priority ranking: implement the Tracy runner and benchmark schema/pipeline in parallel as small independent tasks, stabilize trend data, enable a narrow pinned-hardware gate, then add the dashboard read model. Add RenderDoc on-demand capture when the labeled runner is ready; keep PIX deferred until D3D12.

## Evidence and sources

Repository files reviewed:

- [`src/luminumbra_common/core/Profiler.h`](../../../src/luminumbra_common/core/Profiler.h): macro surface, default no-op behavior, release/gate prohibition, and `world_hash` rationale.
- [`cmake/tracy.cmake`](../../../cmake/tracy.cmake): Tracy `v0.13.1`, `TRACY_ON_DEMAND`, FetchContent behavior, interface target, and default-off configuration.
- [`src/luminumbra_client/rendering/CaptureHooks.cpp`](../../../src/luminumbra_client/rendering/CaptureHooks.cpp): injected-only RenderDoc/PIX behavior, capture paths and diagnostics, marker fallback, and PIX/OpenGL limitation.
- [`docs/PERFORMANCE_ENHANCEMENT_ROADMAP.md`](../../PERFORMANCE_ENHANCEMENT_ROADMAP.md): proposed targets and validation concepts, treated only as planning input.

Primary web sources consulted:

- Tracy upstream: [v0.13.1 release](https://github.com/wolfpld/tracy/releases/tag/v0.13.1), [v0.13.1 README](https://github.com/wolfpld/tracy/blob/v0.13.1/README.md), [capture utility](https://github.com/wolfpld/tracy/blob/v0.13.1/capture/src/capture.cpp), and [CSV exporter](https://github.com/wolfpld/tracy/blob/v0.13.1/csvexport/src/csvexport.cpp).
- RenderDoc upstream: [project/API support overview](https://github.com/baldurk/renderdoc) and [`renderdoccmd` capture/injection implementation](https://github.com/baldurk/renderdoc/blob/v1.x/renderdoccmd/renderdoccmd.cpp).
- Microsoft PIX: [programmatic capture](https://devblogs.microsoft.com/pix/programmatic-capture/), [GPU-capture scope and portability](https://devblogs.microsoft.com/pix/gpu-captures/), and [`pixtool.exe`](https://devblogs.microsoft.com/pix/pixtool/).
- Model Context Protocol: stable 2025-11-25 [resources](https://modelcontextprotocol.io/specification/2025-11-25/server/resources) and [tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools) specifications.
