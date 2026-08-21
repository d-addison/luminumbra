# Simulation and determinism tooling research

Status: research recommendation, 2026-08-21

This report evaluates tooling around the existing Luminumbra simulation. It does
not propose replacing the current deterministic simulation or its oracles. The
recommended shape is a thin Banso verification domain over the existing CTest and
headless-server entry points, plus scheduled soak, artifact, divergence, and
visualization tooling.

## Current state

### Verified in the assigned source slice

- The deterministic-math contract requires bit-identical simulation results,
  binary32 intermediates, fixed-order arithmetic, and `-ffp-contract=off`. Golden
  bit patterns and a lint guard protect the wrappers; changing existing call sites
  is explicitly described as something that can churn `world_hash`.
  ([`DeterministicMath.h`](../../../src/luminumbra_common/core/DeterministicMath.h#L15))
- The headless server implements 43 explicit CLI option branches, including
  `--smoke`, `--smoke-moving`, `--avail-trace`, `--water-hash-trace`,
  `--record`, `--replay`, `--lockstep-loopback`, fault injection, `--net-soak`,
  `--seed`, `--ticks`, and `--artifact`.
  ([`main_server.cpp`](../../../src/luminumbra_server/main_server.cpp#L225))
- `--smoke` performs two independent boots and runs, then requires the final
  `world_hash` and authoritative sub-hashes to match. Terrain, water, entities,
  wind, weather, aether, scents, ecology, and plants are simulation truth. The
  mesh sub-hash is retained for diagnosis but excluded from the pass condition
  because its worker-order representation is render-only.
  ([`main_server.cpp`](../../../src/luminumbra_server/main_server.cpp#L495))
- Smoke output already has a versioned JSON schema
  (`luminumbra.server_tick.v1`), run metadata, both hashes, sub-hashes, pass flags,
  streaming-wait percentiles, and optional availability/water traces. The moving
  availability trace is intentionally report-only because intermediate residency
  can differ while final deterministic state converges.
  ([`main_server.cpp`](../../../src/luminumbra_server/main_server.cpp#L558))
- LREC1 recording stores boot parameters, per-tick inputs, and hashes every 30
  ticks. Recording buffers data until finalization to keep I/O off the tick path.
  Replay rejects invalid/truncated data, checks tick 0, stops on the first checked
  mismatch, localizes it to a sub-hash when possible, and can emit
  `luminumbra.replay_divergence.v1` or `luminumbra.replay_roundtrip.v1` JSON.
  ([`main_server.cpp`](../../../src/luminumbra_server/main_server.cpp#L1415),
  [`main_server.cpp`](../../../src/luminumbra_server/main_server.cpp#L1564))
- The in-process lockstep gate owns two independently booted worlds, exchanges
  hashes every 30 ticks, supports delay and state-corruption fault injection,
  requires a deliberate divergence to halt at the requested tick, and emits an
  LREC1 dump plus `luminumbra.lockstep_loopback.v1` metadata.
  ([`main_server.cpp`](../../../src/luminumbra_server/main_server.cpp#L1793))
- The TCP network soak is already a real non-zero-exit gate over tick-rate,
  queue-depth p95, snapshot-age p95, bandwidth, disconnect, and reconnect behavior.
  It emits `luminumbra.net_soak.v1` when `--artifact` is supplied.
  ([`main_server.cpp`](../../../src/luminumbra_server/main_server.cpp#L2675))
- The residency contract separates deterministic `SimResidency` from
  nondeterministic `RenderResidency`. Simulation inputs must derive from seed,
  preset, deterministic config, tick, position, and the deterministic availability
  set, never wall clock, job-completion order, thread scheduling, or GPU readback.
  ([`determinism-residency-contract.md`](../../determinism-residency-contract.md#L36),
  [`determinism-residency-contract.md`](../../determinism-residency-contract.md#L84))
- The water incident documents why static smoke alone is insufficient: moving
  residency and cold boots must also be sampled. The final resolution relies on
  boot settling and a per-tick streaming barrier, with the historical heavier
  residency rewrite explicitly not planned.
  ([`water-sim-lockstep-determinism.md`](../../water-sim-lockstep-determinism.md#L6))
- EnTT usage is directly visible in the headless server. The assigned
  `PhysicsSystem.cpp`, however, is only a one-line include translation unit, so it
  does not expose the physics integration or debugging hooks.
  ([`main_server.cpp`](../../../src/luminumbra_server/main_server.cpp#L45),
  [`PhysicsSystem.cpp`](../../../src/luminumbra_common/physics/PhysicsSystem.cpp#L1))

### Brief-provided inventory that was not independently re-audited

The task brief identifies Jolt 5.3.0, shield/SDF world generation, foliage, GOAP
AI, `DeterministicRng`, `SimulationClock`, `NetworkStateHash`,
GameNetworkingSockets, and the existing `world_hash` CTest gate. These claims are
consistent with the allowed server and contract files, but the exact Jolt wiring,
version pin, GOAP implementation, build flags, CTest declaration, and Banso
configuration lie outside the permitted read scope. They must be confirmed before
implementing an integration that depends on their exact API or command name. No
README architecture claim was used as evidence.

### Gaps worth addressing

1. The existing oracles are strong but exposed as many independent commands and
   schemas; there is no single simulation-verification result or retention policy.
2. Thirty-tick replay checkpoints bound a divergence to a one-second interval but
   do not identify the first divergent tick or differing entity/component.
3. Seed breadth, cross-build coverage, cold-boot repetition, and network soak are
   manual or gate-specific rather than one scheduled, budgeted program.
4. LREC1 dumps and JSON artifacts have paths but no common manifest, digest,
   provenance, expiry, or promotion convention.
5. Headless diagnosis is textual. There is no established tick-aligned state-diff
   view, physics capture, or simulation timeline in the assigned sources.

## Candidate integrations

### 1. Banso `simulation` verification domain

Create one repo-local Banso verification domain named `simulation`; keep the
existing CTest target and server executable as the authorities. The Banso layer
should launch, time-bound, and parse them, not reimplement their pass logic.

Suggested steps:

| Step | Invocation class | Required result | Default lane |
| --- | --- | --- | --- |
| `world-hash-contract` | Existing targeted `world_hash` CTest gate | Existing CTest verdict unchanged | PR |
| `smoke-static` | `--smoke --artifact ...` | process success and JSON `passed=true` | PR |
| `smoke-moving` | `--smoke-moving --artifact ...` | process success and final hash/sub-hash match | PR or nightly |
| `replay-roundtrip` | `--record`, then `--replay --artifact ...` | valid LREC1 and roundtrip `passed=true` | PR |
| `lockstep-loopback` | normal, delayed-input, and corrupt-tick modes | normal sync; injected corruption halts and dumps at the requested tick | PR/nightly split |
| `network-soak` | host plus `--net-soak-client` processes | `luminumbra.net_soak.v1` `passed=true` | scheduled/manual |
| `diagnostics` | artifact normalizer and summary | never changes a gate verdict | failure-only |

Use steps rather than separate top-level domains because they share the same
build, seed/tick vocabulary, schema validation, timeout handling, and artifacts.
The step boundaries still permit fast PR selection and longer scheduled selection.
The exact Banso manifest syntax and installed CTest selector must be validated
against the repository configuration during implementation.

Important policy details:

- Process exit status is authoritative; JSON is additionally schema-validated.
- Do not fail moving smoke on `availability_trace_match=false`; the source marks
  that trace as report-only. Do fail on the final `passed` result.
- Do not promote render mesh mismatch to a simulation failure.
- A missing, truncated, wrong-schema, or internally inconsistent artifact is an
  infrastructure failure, distinct from deterministic divergence.
- Put hard timeouts and process-tree cleanup around multi-process network steps.

**Delivery form:** repo-local Banso verification plugin/domain with named steps,
JSON-schema checks, and a normalized `simulation-verification.v1.json` summary.

**Effort:** 3-5 engineer-days after the exact Banso and CTest interfaces are
confirmed.

**Risk:** Low-medium. The adapter is observer-only, but accidental reinterpretation
of mesh or availability-trace semantics could create false failures.

### 2. Scheduled determinism soak pipeline

Add a scheduled pipeline that invokes the `simulation` domain in a larger matrix.
Use an explicit seed manifest so every failure is reproducible. A date may choose a
partition of the manifest, but it must never be an unrecorded simulation input.

Recommended tiers:

- Every PR: canonical `world_hash`, static smoke, one record/replay, and normal
  loopback.
- Nightly: 50-200 committed seeds; static and moving smoke; Debug/Release comparison;
  replay roundtrip; delayed and corrupt lockstep cases; cold-boot repetitions under
  controlled load.
- Weekly: longer ticks, ecology/planted/avatar rosters, water trace cross-build
  comparison, and TCP/GNS network soak where the configured build supports it.
- Release: supported OS/compiler/build-mode matrix, with explicit FP flags and
  engine/build fingerprints recorded in every result.

Run shards independently and merge only normalized results. On failure, rerun the
same seed/build once for classification, while preserving both attempts. A passing
rerun does not erase the original failure; it classifies it as intermittent.
Scheduled CI can start late under service load, so schedule time is orchestration
metadata only, never part of the deterministic oracle.

**Delivery form:** CI schedule plus checked-in seed corpus/partition manifest and a
Banso scheduled profile such as `simulation-soak`.

**Effort:** 4-7 engineer-days for one OS and two build modes; add 2-4 days for
multi-OS or real-network runners.

**Risk:** Medium. Runtime cost, cold-cache variance, port collisions, and runner
contention can be mistaken for simulation divergence unless infrastructure and
oracle failures remain separate.

### 3. Divergence seed sweep and tick/state bisector

Build a failure-triggered CLI that consumes a failing seed or LREC1 stream and
narrows the mismatch in layers:

1. Sweep a declared seed range in parallel and emit one row per seed/build with
   first failing checkpoint and section.
2. Re-run only a failing seed and narrow the current 30-tick checkpoint interval.
   Prefer a diagnostic checkpoint cadence override or observer-only tick hashes;
   do not change simulation time or inputs.
3. At the first divergent tick, write canonical state dumps for the differing
   authoritative section and compare stable paths such as
   `entities/<stable-id>/<component>/<field>`.
4. For physics-only divergence, optionally layer Jolt `StateRecorder` validation or
   `JPH_ENABLE_DETERMINISM_LOG` over the world-level result. Jolt's native recorder
   does not cover Luminumbra's custom fields, AI, foliage, water, or networking and
   therefore cannot replace `world_hash`.

EnTT snapshots are useful as a capture mechanism, not a canonical diff format by
themselves. EnTT requires restoration in serialization order; registry/storage
iteration order must therefore not be assumed stable. The diagnostic dump should
sort stable entity IDs and component names, serialize integers explicitly, and
serialize floating-point values as both hexadecimal bits and human-readable values.
Unknown/unregistered components must be reported as coverage gaps.

The lowest-risk first version can repeatedly invoke the existing process and parse
artifacts. A faster second version can add diagnostic flags for per-tick combined
hashes and canonical section dumps at a selected tick.

**Delivery form:** standalone `simdiff` CLI plus a failure hook in the Banso
`diagnostics` step; outputs `seed-sweep.v1.json`, `tick-bisect.v1.json`, and a
canonical state-diff bundle.

**Effort:** 8-15 engineer-days for seed sweep, interval narrowing, three existing
authoritative sections, and readable diffs; more as component coverage expands.

**Risk:** Medium-high. Capturing at a different point, enumerating EnTT storage in
unstable order, or adding I/O to the tick path can manufacture or hide a divergence.
Capture only at the same settled snapshot boundary used by current hashing.

### 4. Replay and failure-artifact management

Standardize every run into an immutable bundle:

```text
simulation-artifact-v1/
  manifest.json
  result.json
  replay.lrec1                 # when produced
  divergence.json             # when produced
  state-diff.json              # when produced
  debug-draw.bin               # optional
  simtrace.json or trace.pftrace
  stdout.log
  stderr.log
```

`manifest.json` should include schema version, commit, dirty-state flag, engine
version, compiler, target triple, build mode, deterministic FP settings, command
line, seed, preset, ticks, shard, start/end time, source artifact schemas, and a
SHA-256 plus byte count for every member. Address bundles by digest and never
overwrite a same-name artifact.

Suggested retention:

- Passing PR bundles: 7-14 days, with replay bodies omitted unless sampled.
- Nightly passing summaries: 30 days; keep a small rotating sample of full bundles.
- Any divergence, flake, or release gate: 90 days in CI and promote selected cases
  to longer-lived object storage or a regression corpus.
- Canonical baselines and minimized repro replays: retain until deliberately
  superseded, with provenance linking old and new hashes.

Upload failure artifacts even when a gate exits non-zero. Validate that referenced
paths remain inside the run artifact root. The bundle is diagnostic evidence and
must never become a simulation input unless explicitly promoted into a reviewed
test fixture.

**Delivery form:** `simartifact` pack/verify CLI, manifest JSON schema, CI upload
step, and a small index rendered in Banso output.

**Effort:** 3-5 engineer-days for local bundles and one CI provider; 2-4 more for
external object-store promotion and garbage collection.

**Risk:** Low-medium. Main concerns are storage growth, silent omission on failed
jobs, leaking machine paths, and retaining a replay without its exact build
provenance.

### 5. Jolt physics debug capture and offline playback

Jolt 5.3.0 documents debug rendering of bodies and constraints, while Jolt's
debug-render recorder records line, triangle, text, and geometry calls for later
playback. Add an adapter only after confirming the actual Luminumbra Jolt wrapper,
because the assigned `PhysicsSystem.cpp` contains no implementation details.

Recommended shape:

- A diagnostic build enables Jolt debug rendering; Distribution remains unchanged.
- At a selected tick, capture bodies, bounds, constraints, contacts, broad-phase
  layers, body IDs, sleeping state, and collision filters.
- Store a tick/seed/world-hash header beside the native draw stream.
- Play it in a small standalone viewer or an existing development client, with
  color filters for host-only, peer-only, and differing bodies.
- Trigger automatically only after `simdiff` localizes a failure to physics/entities.

The capture is render/diagnostic residency. Draw order, camera state, timestamps,
and capture bytes must not feed `world_hash`. Enabling Jolt cross-platform
determinism flags, determinism logging, different vectorization, or different FMA
behavior is not an observer-only visualization change and requires the full
`world_hash` contract review below.

**Delivery form:** Jolt `DebugRenderer` adapter, versioned `debug-draw.bin` capture,
and offline viewer mode.

**Effort:** 5-10 engineer-days after integration discovery.

**Risk:** Medium-high. Jolt hookup is unverified in the assigned slice, captures can
be large, and diagnostic compile flags can accidentally change the physics path.

### 6. Tick-aligned simulation timeline

Convert existing JSON artifacts into a Perfetto-compatible trace before adding
in-process instrumentation. Perfetto can open legacy JSON traces and visualizes
slices, counters, and flows. Useful tracks include:

- host tick, peer tick, agreed tick, and lockstep horizon;
- checkpoint/world/sub-hash markers and the first divergent tick;
- availability and water trace hashes;
- streaming-wait latency, queue-depth p95, snapshot age, and client bandwidth;
- record/replay phases, reconnect events, and artifact links.

The first delivery should be an offline converter from existing artifacts, which
cannot perturb the simulation. If later C++ Perfetto SDK instrumentation is added,
keep it behind a diagnostic category and use wall time only for visualization.
Tick index remains the logical alignment key, and no trace timestamp or counter may
feed deterministic state.

**Delivery form:** `simtrace` artifact-to-Perfetto converter and a Banso summary link
to the generated trace.

**Effort:** 2-4 engineer-days for the offline converter; 4-7 additional days for
carefully validated in-process C++ trace events.

**Risk:** Low for offline conversion, medium for in-process instrumentation because
buffering and clock reads can alter scheduling even when they do not intentionally
alter state.

### World-hash contract guardrail for every candidate

All proposed capture, parsing, visualization, and orchestration work is intended to
be observer-only. Any implementation that changes one of the following is
determinism-affecting and must be treated as a `world_hash` contract change:

- compiler FP contraction/vectorization settings or Jolt determinism/vectorization
  flags;
- RNG consumption, tick order, component iteration order, job scheduling visible to
  simulation, streaming residency, or checkpoint placement that mutates state;
- authoritative serialization, sub-hash membership, or the Sim/Render residency
  partition;
- an engine or diagnostic flag that changes what a tick computes rather than only
  what is observed after the settled snapshot.

Such a change must run the existing targeted `world_hash` CTest gate, static and
moving smoke, replay roundtrip, lockstep loopback, relevant cross-build traces, and
cold-boot repetitions. A changed canonical hash is not auto-accepted: review the
semantic reason, update/re-pin deliberately, and preserve the old/new provenance.
Render meshes, GPU readbacks, profiler timestamps, debug draw order, and artifact
digests remain excluded from simulation truth.

## Ranking

| Rank | Candidate | Value | Effort | Risk | Recommendation |
| ---: | --- | --- | --- | --- | --- |
| 1 | Banso `simulation` verification domain | Very high | 3-5 days | Low-medium | Build first; it creates one trusted entry point without replacing current gates. |
| 2 | Replay/failure-artifact management | High | 3-5 days | Low-medium | Build with rank 1 so every later failure is reproducible and retained. |
| 3 | Scheduled determinism soak pipeline | Very high | 4-7 days | Medium | Add once normalized results and bundles exist; start nightly on one OS. |
| 4 | Divergence seed/tick/state bisector | Very high on failure | 8-15 days | Medium-high | Implement seed sweep and checkpoint narrowing first, then canonical component diffs. |
| 5 | Offline tick-aligned timeline | Medium-high | 2-4 days | Low | Cheap diagnostic multiplier using artifacts already emitted. |
| 6 | Jolt debug capture/viewer | Medium | 5-10 days | Medium-high | Defer until a failure localizes to physics or the Jolt wrapper is audited. |

Recommended delivery sequence: ranks 1 and 2 together, rank 3 next, then the
offline portion of rank 5. Build rank 4 from actual retained failure bundles. Add
rank 6 only after confirming the physics integration and a concrete visualization
need.

## Sources

### Repository files analyzed

- [`src/luminumbra_common/core/DeterministicMath.h`](../../../src/luminumbra_common/core/DeterministicMath.h)
- [`src/luminumbra_common/physics/PhysicsSystem.cpp`](../../../src/luminumbra_common/physics/PhysicsSystem.cpp)
- [`src/luminumbra_server/main_server.cpp`](../../../src/luminumbra_server/main_server.cpp)
- [`docs/determinism-residency-contract.md`](../../determinism-residency-contract.md)
- [`docs/water-sim-lockstep-determinism.md`](../../water-sim-lockstep-determinism.md)

### External primary sources consulted

- [Jolt Physics 5.3.0 documentation](https://jrouwe.github.io/JoltPhysicsDocs/5.3.0/)
  - deterministic simulation, rollback/state recording, and debug rendering.
- [Jolt build and feature flags](https://jrouwe.github.io/JoltPhysics/md__build_2_r_e_a_d_m_e.html)
  - `JPH_CROSS_PLATFORM_DETERMINISTIC`, `JPH_DEBUG_RENDERER`,
  `JPH_ENABLE_DETERMINISM_LOG`, profiling, and diagnostic build behavior.
- [Jolt `DebugRendererRecorder` source documentation](https://jrouwe.github.io/JoltPhysics/_debug_renderer_recorder_8h_source.html)
  - recordable lines, triangles, text, and geometry.
- [EnTT entity documentation: snapshots and loaders](https://skypjack.github.io/entt/md_docs_2md_2entity.html)
  - snapshot/loader behavior and serialization-order requirement. The API must be
    checked against Luminumbra's pinned EnTT version before use.
- [GitHub Actions scheduled workflow documentation](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#schedule)
  - cron schedules and possible start delays under high load; used as an
    illustrative CI implementation, not evidence that this repository uses GitHub
    Actions.
- [`actions/upload-artifact` documentation](https://github.com/actions/upload-artifact)
  - immutable artifacts, digests, IDs, and configurable retention.
- [Perfetto track-event documentation](https://perfetto.dev/docs/instrumentation/track-events)
  and [in-app trace guide](https://perfetto.dev/docs/getting-started/in-app-tracing)
  - C++ slices/counters/tracks and offline visualization.
