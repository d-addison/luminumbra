# Headless automation integration research

## Current state

Luminumbra already has a substantial automation surface, but its contract is spread across two executables and several Python helpers rather than expressed as one orchestration API.

The server executable is the strongest machine-facing surface. Its parser accepts deterministic simulation, replay, networking, and performance modes, including `--smoke`, `--heavy`, `--record`, `--replay`, `--lockstep-loopback`, `--replicate`, `--net-soak`, `--seed`, `--world-id`, `--ticks`, and benchmark flags. The default path runs fixed 30 Hz ticks; smoke mode performs two runs and asserts `world_hash == world_hash_replay`. Several modes already emit JSON artifacts and use non-zero exits for contract failures. Network modes can listen on ports or connect to peers, and replay-fixture mutation deliberately corrupts data, so the server flags are not all equally safe to delegate to an agent.[1]

The client executable provides a larger render and capture surface. Relevant self-terminating modes include `--render-benchmark`, `--frame-scan`, `--render-parity-finalblit`, `--render-parity-frame`, `--render-parity-ssao`, `--upscale-seam-parity`, `--bake-tree-impostor`, `--survey`, `--ui-screenshot`, and `--timelapse-*`. It also provides composition and diagnostic controls such as `--scene-config`, fixed camera options, `--debug-goto`, and `--debug-view`. Some modes imply world boot and carry watchdogs; frame scan and render-parity paths pin a repeatable scene and describe themselves as render-only. Client automation still requires a graphics context, even when the window is hidden, so it is "headless" operationally rather than a GPU-free process.[2]

The visual-regression workflow is already CI-shaped. A capture is compared with a golden by `tools/flip_diff.py`, which writes a heatmap and JSON result and returns distinct pass, regression, and usage/I/O exit codes. Golden promotion is explicit: `golden_update.py` refuses overwrite unless forced, while `flip_diff.py --update` is another deliberate promotion path. The metric can fall back from NVIDIA FLIP through SSIM, luma/gradient, and stdlib RGB error, and the chosen backend is recorded. Because scores are not interchangeable across backends, a gate must pin both backend and threshold rather than accept silent fallback.[3]

The timelapse workflow is also composable today: the client captures a settled sequence while advancing a fixed number of 30 Hz simulation ticks between frames, and `tools/timelapse.py` assembles GIF and optional MP4 output. The documented time-scale implementation changes how many fixed ticks run per real frame, not tick duration, so the per-tick hash sequence is intended to remain unchanged.[4]

`tools/visual_critique.py` is an objective NumPy/Pillow analyzer despite its name. In strict mode, any configured defect or fidelity-floor flag makes the process fail. It intentionally has no allowlist or "tracked debt" escape hatch. Its `combine` operation can merge separately produced AI notes, but the objective gate itself does not invoke a model.[5]

The existing properties are enough for orchestration: explicit arguments, bounded runs for most gate modes, artifacts, machine-readable JSON in key paths, and exit status. The main gaps are typed input validation, a common artifact manifest, capability separation, consistent timeouts/cancellation, and a reviewable golden lifecycle.

### Contract boundary

`world_hash` is the simulation contract. An integration must pass seed, preset, world ID, tick count, roster, replay, and other simulation-affecting inputs through without changing their meaning or silently substituting new defaults. It must record the exact executable, build identity, working root, argument vector, and relevant environment in the run manifest. Any future wrapper change that alters tick ordering, fixed tick duration, world generation, replay input, or simulation scheduling is a determinism change and must be validated against the `world_hash`/run-equals-replay gates.

Render-only capture and diff tooling is a separate pixel contract. Wrapping it does not by itself change `world_hash`, but mixed modes such as timelapse dig, drain, rain, growth, creatures, or other world mutations must not be mislabeled as read-only. A wrapper should preserve the fixed-tick behavior rather than replacing it with wall-clock advancement.

## Candidate integrations

Effort uses engineering elapsed time for one developer familiar with the repository: **S** is about 2-5 days, **M** is 1-3 weeks, and **L** is 4-8 weeks including hardening. Risk combines correctness, security, portability, and operational failure modes.

### 1. Typed Banso steps and pipelines

**Delivery form:** a repository-local Banso plugin containing YAML pipeline definitions plus TypeScript step implementations. The TypeScript layer owns argument construction, schema validation, process supervision, artifact collection, and normalized result JSON; YAML composes the steps into gates.

Start with typed steps rather than a generic `run_luminumbra(args[])` escape hatch:

- `server.smoke` and `server.replay`: require explicit seed/preset/ticks or a replay path, enforce bounded ticks, collect the server artifact, and surface hash mismatch as a gate failure.
- `capture.frame_scan`, `capture.ui`, and `capture.scene`: validate names and output paths, apply a wall-clock timeout, collect captures and JSON, and preserve the client's pinned-capture behavior.
- `capture.timelapse` plus `media.timelapse`: bound frame and tick counts, collect the frame manifest, then assemble GIF/MP4.
- `visual.diff`: require candidate, golden, metric backend, threshold, and heatmap/JSON destinations. Treat exit 1 as a failed comparison and exit 2 as an execution error.
- `visual.critique`: run objective analysis, preserve strict-mode blocking semantics, and attach JSON/Markdown reports.

Compose those into three initial pipelines:

1. `screenshot-gate`: capture one or more deterministic UI/scene frames, normalize artifact names, diff each against its declared golden, and publish candidate/golden/heatmap/result as one bundle.
2. `visual-regression`: run frame scan or a declared capture matrix, verify dimensions and pinned metric backend, run FLIP-style comparisons and optional objective critique, then aggregate failures without discarding per-cell evidence.
3. `timelapse`: capture, verify the expected frame count, assemble media, and publish the exact capture parameters alongside the result. This is an artifact workflow by default, not a PR-blocking pixel gate.

Each step should write a common run manifest with schema version, build identity, exact argv, start/end time, timeout, exit classification, host/GPU identity for render work, and content hashes for inputs and outputs. Secret-bearing environment values must be redacted.

**Effort/risk:** **M / low-to-medium**. The underlying commands already exist; most work is validation, supervision, and artifact normalization. The principal risk is accidentally changing semantics through wrapper defaults. Snapshot tests of generated argv and golden fixtures for normalized results reduce that risk.

### 2. Safe MCP toolset for AI agents

**Delivery form:** a local stdio MCP server implemented as a thin adapter over the same TypeScript step library, with JSON Schema inputs and resource links for artifacts. It should not contain a second command-construction implementation.

The default agent capability set should be allowlisted and bounded:

- `luminumbra_server_smoke(seed, preset, ticks, ...)`
- `luminumbra_replay_verify(replay_resource)`
- `luminumbra_frame_scan(capture_profile)`
- `luminumbra_ui_screenshot(screen_ids, fixture_profile)`
- `luminumbra_visual_diff(candidate_resource, golden_id, metric_profile)`
- `luminumbra_visual_critique(sweep_resource, strict)`
- `luminumbra_timelapse(capture_profile)` with conservative frame/tick ceilings
- `luminumbra_run_get(run_id)` and `luminumbra_artifact_get(run_id, artifact_id)`

The MCP server should expose profiles and resource IDs rather than arbitrary filesystem paths or raw argv. Resolve every input and output beneath configured roots; reject traversal and symlink escapes; use per-run directories; cap ticks, frames, clients, output bytes, wall time, and concurrent GPU jobs; scrub the environment; and kill the process tree on cancellation or timeout. Tool results should distinguish comparison failure, engine contract failure, timeout, unsupported host capability, and wrapper error.

Do not grant the default MCP capability access to `--mutate-replay-fixture`, arbitrary `--root`/artifact paths, `--net-host`, `--net-join`, `--net-soak`, Steam/UDP transport, arbitrary scene JSON, golden promotion, or unbounded benchmark/soak values. Those operations mutate fixtures, open/connect sockets, can consume substantial resources, or can overwrite review artifacts. If later required, publish them as a separate operator-only capability set with explicit policy and audit logging.

Golden reads are safe; golden writes are not. An AI tool may propose a candidate and produce a review bundle, but `golden_promote` should either be absent or require a distinct human-authorized capability that records approver identity and expected old/new digests.

**Effort/risk:** **M / medium**. Reuse keeps implementation modest, but agent-facing process execution raises path, resource-exhaustion, and network risks. The risk remains acceptable only with the allowlist and limits above; a generic shell-shaped tool would be **high risk**.

### 3. Small REST/daemon facade

**Delivery form:** a localhost-only job daemon with `POST /v1/runs`, `GET /v1/runs/{id}`, `POST /v1/runs/{id}/cancel`, and artifact download endpoints. Requests select the same named profiles used by Banso/MCP. The daemon owns a bounded queue and serializes GPU captures.

A daemon is justified only when runs must outlive the caller, multiple local clients need one GPU queue, or remote workers cannot invoke the CLI directly. It must add authentication even on a developer network, request and artifact quotas, durable job state, retention cleanup, cancellation, process-tree isolation, build/profile versioning, and protection against cross-run path access. Those are operational responsibilities that the CLI does not need.

Do not make REST a new semantic API over individual engine flags. It should enqueue versioned profiles and return the same run manifest generated by the shared step library. Do not expose it beyond loopback until there is a concrete remote-execution threat model and TLS/auth deployment owner.

**Effort/risk:** **L / high** for a supported remote service; **M / medium** for a loopback-only prototype. The engine gains little from a daemon today, while persistence, authentication, cleanup, concurrency, and GPU ownership add failure modes. Defer this candidate.

### 4. Deliberately stay CLI-only

**Delivery form:** documented PowerShell/shell examples and CI job templates invoking the existing binaries and Python helpers directly, with no shared typed wrapper.

This has the smallest implementation surface and keeps every engine flag immediately available. It remains useful as the debugging and escape-hatch layer underneath all other candidates. It is insufficient as the primary orchestration contract: every caller must independently handle defaults, quoting, timeouts, GPU serialization, artifact naming, backend pinning, error classification, path policy, and golden review.

**Effort/risk:** **S / medium**. Initial delivery is cheap, but duplicated orchestration and unconstrained flag access create cumulative correctness and security risk. Retain CLI compatibility; do not choose CLI-only as the final agent interface.

### 5. Golden-image management

**Delivery form:** a versioned `goldens/manifest.yaml` (or equivalent JSON) plus Banso `visual.diff` and operator-only `visual.promote` steps. Golden blobs may live in Git LFS or immutable object storage, but the manifest belongs in source control and pins their digests.

Each entry should identify capture profile, seed/world fixture, camera/screen, resolution, renderer/backend, platform/GPU tolerance class, metric backend, threshold, golden digest, and provenance commit. Separate genuinely platform-specific baselines rather than increasing one threshold until all hosts pass. A gate must fail closed if the requested metric backend is unavailable; fallback remains useful for local diagnosis but must not decide a protected gate under a different scale.

Promotion should be a two-phase operation: create a review bundle containing old golden, candidate, heatmap, numeric result, capture manifest, and proposed manifest diff; then require an operator to promote an expected candidate digest over an expected old digest. The promotion command must refuse an unexpected existing golden, matching the current helper's safe-overwrite behavior. Ordinary PR jobs get read-only golden credentials. Promotion runs on a trusted runner and produces an auditable commit or immutable-object update.

Keep pixel goldens separate from simulation determinism artifacts. Re-blessing an image never waives or updates the `world_hash` contract, and a hash change cannot be approved through the visual-golden workflow.

**Effort/risk:** **M / low-to-medium**. The current helpers already establish explicit promotion and review semantics. Storage growth, platform variance, and accidental metric fallback are the main risks.

### 6. Scheduled soaks and benchmarks

**Delivery form:** CI scheduler definitions that call versioned Banso pipelines on labeled self-hosted runners; results upload to CI artifact storage and a metrics/time-series sink. Scheduling does not live in the engine, MCP server, or a long-running developer daemon.

Use separate lanes:

- **Pull request:** short server smoke/replay checks and a small, pinned screenshot gate. Run render gates only on a known GPU class or make an unavailable GPU an explicit infrastructure result, never a silent pass.
- **Nightly:** broader visual matrix, objective critique, render benchmarks, and a bounded network soak. Serialize captures per GPU and record driver/GPU identity so trends are comparable.
- **Weekly or pre-release:** long network soaks, expanded seeds/worlds, heavy replay/resimulation, and benchmark trend analysis. Retain logs and manifests longer than routine PR artifacts.

Benchmarks should run on stable, labeled hardware with power mode, driver, resolution, warm-up, and build configuration pinned. Compare regressions within a hardware cohort. Soak orchestration should allocate ports and launch peers outside the MCP safe subset, isolate the runner network, enforce a hard deadline, and always collect server/client artifacts on failure.

**Effort/risk:** **M / medium**. CI scheduling is conventional, but flaky GPU hosts, driver drift, port collisions, and long-run cost require runner ownership and observability.

## Ranking

| Rank | Candidate | Delivery form | Effort | Risk | Decision |
|---:|---|---|:---:|:---:|---|
| 1 | Typed Banso steps and pipelines | Repository-local YAML pipelines + shared TypeScript step library | M | Low-medium | Build first; this becomes the canonical orchestration layer. |
| 2 | Golden-image management | Versioned manifest + read-only diff and operator-only promotion steps | M | Low-medium | Build with the first screenshot/visual pipelines. |
| 3 | Scheduled soaks and benchmarks | CI schedules on labeled self-hosted runners | M | Medium | Add after step manifests are stable; keep long runs off developer agents. |
| 4 | Safe MCP toolset | Local stdio MCP adapter over the shared steps | M | Medium | Add after the typed API exists; expose only bounded profiles. |
| 5 | CLI-only | Direct binary/helper invocation and CI snippets | S | Medium | Preserve as the implementation substrate and debugging escape hatch, not the primary interface. |
| 6 | REST/daemon | Profile-based queued job service | L | High | Defer until durable multi-client or remote execution is a demonstrated requirement. |

The recommended delivery is therefore a **Luminumbra Automation Banso plugin**: typed TypeScript steps, YAML pipelines, a common run/artifact manifest, and a golden manifest. The CLI remains the source of truth under the plugin. The same step library can later back a constrained local MCP server without widening the engine surface. CI owns schedules and privileged soak/network execution. No REST service is needed for the first delivery.

### Proposed delivery slices

1. **Foundation:** shared schemas, argv snapshot tests, supervisor, run manifest, `server.smoke`, `capture.ui`, and `visual.diff`.
2. **Visual workflows:** screenshot and matrix pipelines, objective critique, golden manifest, and review-only promotion bundle.
3. **Media/performance:** timelapse pipeline, render benchmark profile, GPU serialization, and nightly schedules.
4. **Agent access:** stdio MCP adapter, safe profile allowlist, quotas, cancellation, and artifact resources.
5. **Privileged operations:** trusted-runner network soak and golden promotion, explicitly separate from agent defaults.

### Sources

Repository sources analyzed:

1. [`src/luminumbra_server/main_server.cpp`](../../../src/luminumbra_server/main_server.cpp) - server modes, parser, determinism/replay/network contracts, artifacts, and defaults.
2. [`src/luminumbra_client/main_client.cpp`](../../../src/luminumbra_client/main_client.cpp) - client capture, render benchmark, frame scan, parity, UI, survey, timelapse, debug, watchdog, and window/GPU behavior.
3. [`docs/visual-regression.md`](../../visual-regression.md) - capture/diff/promotion workflow, metric backends, thresholds, exit codes, and `world_hash` boundary.
4. [`docs/timelapse.md`](../../timelapse.md) - fixed-tick time scaling, frame capture, assembly, and operational constraints.
5. [`tools/visual_critique.py`](../../../tools/visual_critique.py) - objective metrics, strict blocking flags, report outputs, and optional AI-result combination.

No web sources were consulted; the conclusions are based on the repository files above.
