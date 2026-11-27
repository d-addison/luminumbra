# Simulation budget telemetry

Regional resume can revisit a stage several times during one host tick. The
collector sums those visits into one stage sample for that tick, including their
work and measured durations. Integer work-item observations remain independent.
The parameterised 3,600-tick parity test retains three passes per region mode,
comparing telemetry work, world hashes and saves without adding another long test
to the sanitizer shards.

The additive time-free work-item traces and standard CI fixture ceilings for
issue #396 are documented in [Deterministic work budgets](deterministic-work-budgets.md).

`SimBudgetTelemetry` observes the existing fixed tick without scheduling work or
enforcing a budget. The session switch `SetSimBudgetTelemetryEnabled(true)` and
the headless CLI switch `--sim-budget` enable it. Both default to off; there is
no new entry in `data/common/systems.json`. `--sim-budget` implies `--smoke`.
The headless executable also accepts `--no-audio` as a no-op: it has no audio
subsystem. These switches do not change simulation configuration identity.

Each executed tick records an unsigned 64-bit tick identity, an unsigned 64-bit
work count, and a steady-clock duration in milliseconds for each ordered
`GameSession::TickSimulation` stage. A catch-up call records each constituent
tick separately; a call that advances no ticks records nothing. Counter queries
and sample insertion are outside the timed stage bodies. Enabling or
disabling clears the observations, as does creating/loading another world.
The collector retains samples in memory for the capture's lifetime, so memory
grows with ticks × stages. It is intended for finite measurement runs.

Counting uses const registry views, grid dimensions and existing integer
submission/solver counters. It never reads worker completion time to decide
work, advances an RNG, creates component storage, or changes a system call or
its order. Counts follow the same seed, world state and fixed-tick inputs as
the simulation. Smoke mode compares **every tick and stage's count** between
its two fresh-world runs and fails the run on a difference. The artifact retains
that integer trace for comparisons across platforms/builds. This diagnostic
trace does not extend LREC1 records or checkpoints.

Wall durations are nondeterministic, never drive simulation, and are excluded
from work comparisons, saves, world hashes and fixture hashes. The collector
does not contribute a world-hash slot or change the append-only
`chunk|wind|weather|aether|scents|ecology|plants` order. `sim.active_regions` owns the persisted clock and its save/hash contract;
telemetry adds no save fields or format changes. With collection off, no counter callback or new
clock read executes and no artifact member is added.

## Stage identity and integer units

The version below fixes this order and these definitions. Counts are units of
input work, not estimates of CPU instructions, successful actions or changed
bytes. An entity rejected by an inner eligibility check still counts as a
participant examined. A grid counts each spatial cell once (each channel-cell
for scent), rather than multiplying by diffusion passes. Pair searches and
reproduction are included in their owning stage's duration; their source
participant count is not an estimate of pair comparisons. Units must be
calibrated from measurements before any scheduler chooses weights or limits.

| Stage | Integer work unit |
|---|---|
| `server_physics` | One physics scene tick, including the existing avatar bridge |
| `animation` | Animation-player components examined |
| `instinct` | Entities with instinct-agent and needs components |
| `perception` | Entities with transform, perception and awareness components |
| `scent` | 128 × 128 × 4 channel-cells when scent or foraging participants activate the field; otherwise zero. Includes deposit, foraging, advection, diffusion and clamp duration |
| `locomotion` | Entities with action plan, transform and locomotion profile; includes the following scent-steering duration |
| `creatures` | Entities with creature and transform components at entry; includes brain, mate seeking, steering, thirst/scavenging, physics bridge and mating resolution in their existing order |
| `wind` | Separate path only: 64 × 64 grid cells when the field exists, otherwise zero |
| `weather` | Separate path only: 64 × 64 grid cells when the field exists, otherwise zero; includes storm processing |
| `wind_weather` | Combined clock path only: 64 × 64 wind cells + 64 × 64 weather cells = 8,192 input field cells; includes both wind rebuilds, storm processing and wind sampling |
| `aether` | 64 × 64 grid cells when the ambience field exists, otherwise zero |
| `energy` | Stored energy pages at entry (including pages outside the window); zero when disabled. A page-work proxy for anchor, emitter, deposit and cadence processing, not a claim that every page decays each tick |
| `irrigation` | 256 × 256 grid cells when a water-source participant enables the update, otherwise zero |
| `soil` | 256 × 256 grid cells when a soil-feeder participant enables the update, otherwise zero |
| `plants` | Entities with plant tag, growth, genome and transform |
| `pollination` | Entities with pollination tag, genome, growth, transform and pollination components |
| `disease` | Entities with plant tag, health and transform |
| `crops` | Entities with crop lifecycle, growth, genome and transform |
| `fire` | Entities with combustible and transform components |
| `grazing` | Entities with grazeable and transform components |
| `lifespan` | Mortal components examined |
| `alarm` | Entities with alarm, creature and transform components |
| `decay` | Decay components examined |
| `circadian` | Circadian components examined |
| `territory` | Entities with territory and transform components |
| `packs` | Entities with pack-hunter, creature and transform components |
| `migration` | Entities with migratory and transform components |
| `events` | Pending events examined by the ordered drain (including future events sorted but not delivered) |
| `server_streaming` | Chunk generation submissions in the current server update (`scheduled_generation`); zero on elided updates |
| `server_water` | Water cells stepped by that update (`cells_stepped_last_update`); zero for empty or paused updates, independent of the legacy debug counter |

With active regions enabled and both fields present, `wind_weather` measures
exactly one `WeatherSystem::UpdateFromClock` call with the absolute tick and
ambient anchor. It preserves the internal wind → weather → wind sequence.
Each input cell is counted once per field, not once per rebuild, following the
input-work convention above. Separate `wind` and `weather` then have zero
samples, zero totals, empty traces and null durations. Otherwise the existing
wind then weather calls retain their spawn-point inputs and separate attribution,
and `wind_weather` has no samples. No work or time is duplicated or estimated
by splitting the combined call. The artifact's distinct stage and trace coverage
make this choice visible. Telemetry never controls this branch.

The first and last two rows are server supplements. Physics precedes the
session tick. Streaming and water still run **outside** `TickSimulation`, in
the existing server update. When collection is enabled, streaming duration
brackets the complete update, including the final resident-chunk scan and queue
bookkeeping, subtracts the existing water-phase bracket, and adds the separate
due-activation wait. Water uses that water-phase bracket. The existing phase
timers and water-smoke debug outputs are preserved with collection disabled.
They do not double-count time; streaming is split around water in execution.
Boot generation, boot water settling, hashes, autosaves, shutdown and diagnostic
serialization are outside these stage distributions. No warm-up simulation
ticks are excluded after boot. The supplements have no samples in a session
driven directly without the server runner.

## Artifact extension and compatibility

The existing `luminumbra.server_tick.v1` JSON artifact gains exactly one
optional top-level object, `sim_budget`, only with collection enabled:

```json
{
  "sim_budget": {
    "schema": "luminumbra.sim_budget.v2",
    "work_replay_match": true,
    "stages": [
      {
        "name": "plants",
        "samples": 2,
        "work_total": 12,
        "work_trace": [{"tick": 1, "work": 6}, {"tick": 2, "work": 6}],
        "duration_ms": {"p50": 0.5, "p95": 0.95, "p99": 0.99, "maximum": 1.0}
      }
    ]
  }
}
```

The example shows one stage; a real block enumerates all rows above in order.
Totals, sample counts, traces and distributions describe the **first** smoke
run only. The second run supplies the work-equality verdict; neither run's
durations enter that verdict. JSON tick/count values are integers in
`[0, 2^64)`; consumers must preserve integer precision, not coerce them through
IEEE double. The trace is ordered by tick, with one sample per tick on the executed path.
`work_total` is its sum and `samples` its length. Duration values are finite,
nonnegative milliseconds. A stage with no samples has `duration_ms: null`;
an executed but inactive stage has zero work and a measured duration.

Percentiles match `tools/perf/perf.py`: sort samples, evaluate position
`(n - 1) × p`, and linearly interpolate the two enclosing samples. Fractions
are 0.50, 0.95 and 0.99; `maximum` is the largest sample. In particular this
does not change the older streaming-wait or water-smoke blocks, whose existing
nearest-rank calculations remain untouched.

Compatibility and refusal rules:

- An absent `sim_budget` means collection was off or the producer predates this
  extension. Ordinary smoke consumers continue accepting the existing artifact.
  A telemetry capture reader refuses absence as missing measurement evidence.
- Unknown object keys at any level are ignored, so existing consumers can
  ignore the entire extension. Within schema v2, required keys must be present;
  unknown, duplicate, missing or reordered **stage identities** are refused.
  Changed stage units/order require a new telemetry schema identity. Version 2
  adds `wind_weather` and mutually exclusive field-path coverage; the reader
  refuses version 1 instead of interpreting its different stage layout.
  Only the unexecuted field path may have no samples in a server capture; the
  executed field path and every other stage must cover all requested ticks.
- Corrupt JSON, wrong types (including booleans/fractions used as integers),
  out-of-range counts, inconsistent totals/ticks, missing timer coverage,
  nonfinite/negative/unordered percentiles, and false replay verdicts refuse a
  telemetry capture. They never cause a world-load refusal: artifacts are not
  world input and there is no telemetry save record.
  Both world hashes must be strings of exactly 16 lowercase hexadecimal digits,
  matching the producer's checksum format, before replay equality is evaluated.
- A missing or unrecognized telemetry schema, including a future version, is
  refused by the capture reader. It must not reinterpret unfamiliar evidence
  as an empty or zero-cost tick. Duration comparisons are observational only.

Independent devel smoke artifacts already contain nondeterministic wall times,
generated world IDs and render-mesh diagnostics, so raw-byte equality between
independent executions is not a valid existing guarantee. The disabled path
is checked separately by `SimBudgetTelemetry.DisabledProductionArtifactMatchesDevelBytes`.
It calls the complete production `WriteSmokeArtifact` writer with fixed legacy
run inputs and compares the file in binary mode against the checked-in
`test/fixtures/smoke_artifact/devel-{0,1,2}.json` baselines. These were generated
from devel commit `f2895388cd14fda8591ed571f87c0ffe026db15d`'s `Smoke.cpp`, replacing
only its simulation-input producer with the same fixed inputs. The cases cover
ordinary output, all optional legacy diagnostic blocks, and divergent replay
verdicts. Times, world IDs, mesh diagnostics, counts and hashes are pinned;
no output fields are removed or normalized. On Windows, expected baseline line
endings follow devel's existing text-stream CRLF expansion. Provenance hashes
and the explicit baseline generator live beside the fixtures.

`DisabledSkipsCounterAndArtifactExtension` is only a unit check that disabled
collection skips counter callbacks and the append helper leaves an existing
JSON object alone; it does not prove production artifact compatibility. Runtime
comparisons of independent executions check authoritative hash parity separately,
without demanding equality of their nondeterministic artifact fields.

## Repeatable capture

Build the existing `perf` preset, then run from the repository:

```sh
python3 tools/perf/capture_sim_budget.py \
  --binary build/perf/bin/luminumbra_server_app \
  --evidence-dir ../sim-budget-captures/before
```

Repeat with another named directory after simulation work. The default workload
is seed `1337`, 3,600 ticks per replay, surface/collision radius 1, on default,
mountains and archipelago, with the existing ecology and planted rosters and
`--no-audio`. The script fails on missing binaries, existing output directories,
server errors, invalid artifacts or vacuous plant/creature/scent/field work.
It retains partial evidence on failure and does not publish a success manifest.
Add `--check-neutrality` to also run a telemetry-off smoke counterpart immediately
after each preset's traced capture. This opt-in mode requires an evidence directory
outside the checkout. The counterpart uses the same executable, root, preset,
seed, rosters, tick count, radii and 30-tick autosave cadence; only `--sim-budget`
and the artifact output argument differ. Both counterpart world hashes must match
the traced hash, both artifacts must report exactly two complete smoke runs and
identical effective simulation gates, and the disabled artifact must omit
`sim_budget` entirely. Successful process exits and smoke verdicts are required.
The traced artifact still passes every population, field-path, per-tick work
and replay check. Fixture identities are checked again after all requested runs.
The default invocation continues to capture only telemetry-on evidence.
The committed shipping-default work-budget baseline and CI capture use 600 ticks
instead. To check or re-pin that baseline, pass `--ticks 600 --seed 1337 --radius 1
--collision-radius 1` explicitly and follow the
budget re-pin procedure in `tools/perf/sim-budgets.md`.
Omitting `--ticks` keeps the general 3,600-tick capture default, which will not
match the baseline's pinned tick identity.
The smoke artifact's `effective_simulation` object reports every `sim.*` gate from
the generated SystemConfig registry. Tuning-only gates report whether the config
override is enabled; runtime gates report their applied session state, including
actual energy-field allocation and water resolution. The existing `finite_hydrology`
and `weather_rain` fields remain present. Qualification derives required gate names
from `data/common/systems.json`, ignoring comment entries, and refuses any absent
or non-true gate. Adding a shipping gate therefore requires artifact coverage
automatically. Capture, comparison and budget consumers share this validation.
Use the same compiler, build preset, hardware, content and command arguments
for time comparisons. The roster evolves, including births and deaths: these
are populated starting worlds, not an enforced constant entity population.

Each directory contains `<preset>.json` (the smoke artifact), `<preset>.log`
(diagnostic stdout/stderr) and, only after all captures validate, `capture.json`.
With `--check-neutrality`, it also retains `<preset>-telemetry-off.json` and
`<preset>-telemetry-off.log`. Each manifest run then has an optional `telemetry_off`
object containing the counterpart's relative `artifact` and exact `command`,
using the existing manifest schema. Its presence in a successful manifest records
that the neutrality checks passed; absence does not establish neutrality.
Failures retain artifacts and diagnostics and prevent success-manifest publication.
The manifest is a UTF-8 JSON evidence record with schema
`luminumbra.sim_budget_capture.v1` and required keys `revision`, `source_dirty`,
`binary_sha256`, `fixture_hash`,
`seed`, `ticks`, `surface_radius`, `collision_radius`, and `runs`. Each run has
`preset`, relative `artifact`, and exact argument-array `command`. Absence means
incomplete/unqualified capture; corrupt or inconsistent manifests and unknown
schema versions must be refused by comparison consumers; unknown object keys
are ignored. `source_dirty` records whether the source tree had uncommitted
changes and `binary_sha256` identifies the measured executable bytes, so a
working-tree capture is not mislabeled as a pristine revision. New captures also
record `preset_hash`, binding the preset files and their referenced dependency
content. Historical captures may omit it; comparisons refuse a difference in
its presence or value. See the [fixture reference and hash rules](performance.md).
The default executable path includes `.exe` on Windows. Logs are optional diagnostics, not
authoritative records: missing
or damaged log text does not synthesize measurements. No evidence file is read
by world loading or hashed into simulation. Compare stage work traces/totals
separately from duration distributions. This slice proposes no limits or caps.

Capture and paired server comparison share the resolved-input recipe in
`tools/perf/resolved_fixture.py`. The `fixture_hash` uses the
`luminumbra.sim_budget.inputs.v2` domain over compact, sorted JSON containing
the canonical headless common-tree hash (`common`), preset-tree hash (`presets`),
and, when present, `.perf-controlled` marker text (`configuration`). Both tree
hashes retain their existing dependency and containment rules, including
transitive biome tables outside `data/common`. The preset tree includes all
three selected documents and any additional presets. Marker text uses universal
newline normalization; its presence enables runtime overrides, so controlled
and shipping fixtures have different identities even with identical data.
Clone locations and generated saves do not enter the digest. This replaces
the common-only capture identity and the comparison's separate recipe;
`preset_hash` remains unchanged and the committed fixture identity stays unpinned.
Missing, malformed or duplicate-key selected presets and non-string biome-table
paths refuse capture before launching the executable or creating evidence.
Inputs are hashed again before publication; changes refuse a success manifest.

## Regional cadence capture and comparison

The diagnostic capture tool for issue #138 uses the server's fixed
`--regional-cadence-workload`: eight distant anchored regions and a ninth
unanchored control, each with a plant, resource participant and water source.
The current server flag has no anchor-count option. The matrix varies presets,
seeds and repetitions of this fixed workload; other anchor counts await a
separately qualified workload. Use a stable executable and isolated runtime
content tree, with evidence outside both the repository and runtime tree:

```sh
python3 -I -B tools/perf/capture_regional_cadence.py capture \
  --root <runtime-directory> --binary <server-executable> \
  --evidence-dir <new-evidence-directory> \
  --presets default mountains archipelago --seeds 1337 --repeats 2 --ticks 3600
python3 -I -B tools/perf/capture_regional_cadence.py compare \
  <before-evidence-directory> <after-evidence-directory> > <comparison-file>
```

Each command runs standalone smoke twice internally; each matrix cell repeats
that smoke independently. Commands include no-audio, simulation telemetry,
30-tick autosaves and explicit surface/collision radii (both default to one).
Inherited `LUMINUMBRA_` overrides are removed from the child environment.
Capture-only admission bounds are 32 matrix cells, 36,000 ticks per run,
64 MiB per JSON artifact, 512 MiB total artifact input and a 512 MiB executable.
Each process has a 600-second deadline and retains at most 16 MiB of combined
stdout/stderr. Truncation or log I/O failure refuses publication; a per-cell
`cell-N.log.json` records retained bytes, the limit, truncation and log completeness,
including after process failure or timeout. Unmeasured status fields are null if
log setup fails. These bounds do not change shipping simulation limits. Larger workloads
require separate qualification.

Only a complete validated matrix publishes `capture.json`, schema
`luminumbra.regional_cadence_capture.v2`. It binds argument arrays with executable
and runtime paths replaced by role labels plus their SHA-256 digests, and artifact
paths relative to the evidence directory. The capture root is recorded as `.`.
Comparison also redacts these paths in legacy archives. Other bindings are source
revision and dirty flag, executable SHA-256, the shared resolved-content hash,
a canonical workload SHA-256, and canonical artifact digests. Source, executable
and content identities are checked again before publication. Partial artifacts
and logs remain diagnostic evidence after failure; existing evidence directories
are refused. Capture directories and input trees are operator-owned and stable
during capture; the tool does not provide concurrent-writer isolation or prove
that an executable was built from its recorded source revision.

Qualification checks both complete smoke runs, all shipping simulation gates,
all telemetry-v2 stages and integer work-item traces, matching simulation truth
hashes, and the optional `regional_cadence` diagnostic block from each run.
Every anchored owner has positive plant service, elapsed resource steps and
interior water service. Missing participants, incomplete observations, frozen
plant/water service, advancement of the unanchored control, divergent repeated
integer traces, and water work exceeding the existing 4,096-cell cap refuse
publication. Diagnostic water maxima include preparation and seam endpoint work;
per-tick `water_cells` traces are checked separately. Foliage replay cell/page
visits are last-pass and maximum observations, never summed as cumulative work.
Per-owner service gaps are simulation ticks; water service counts interior
windows, while border windows remain global observations. The tool retains
remaining replay steps and deferred water samples without interpreting them as
completed work or claiming native correctness.

Comparison revalidates complete matrices, commands, identities and artifact
digests. Workload, resolved content and effective gates agree across endpoints;
source revisions and executable digests may differ and remain in the report.
The `luminumbra.regional_cadence_compare.v2` report separates
`integer_work_and_service` (raw integer traces, counts, service gaps and truth
hashes for both endpoints, with equality) from `duration_ms` (per-stage
p50/p95/p99/maximum distributions). Duration changes and render-only mesh hashes
do not affect deterministic comparisons. Different integer trajectories across
revisions are reported; divergence within a repeated workload is refused.
There are no timing thresholds or new shipping-limit proposals. Representative
native runs, batching/save continuation correctness and release acceptance are
still separate evidence requirements. `RegionalCadenceCaptureContract` registers
synthetic regressions without launching the engine.

## Synthetic ledger and indexed-save measurements

The optional `LUMINUMBRA_BUILD_TOOLS` target `luminumbra_active_regions_bench`
measures bounded synthetic fixtures without procedural streaming. Collect its
raw samples and report outside the source tree with:

```sh
python3 -I -B tools/perf/active_region_scale.py \
  --binary build/perf/bin/luminumbra_active_regions_bench \
  --evidence-dir <new-evidence-directory>
```

Defaults are ledger sizes 8 and 32, four adopted durable chunks and three
independent repetitions. `--sizes`, `--chunks` and `--repeats` select the matrix;
the fixture caps are 524,288 ledger records, 4,096 chunks and 128 total runs
(at most 16 sizes and 32 repetitions). These are tool allocation bounds,
independent of shipping scheduler limits. Large runs require separate native
qualification; this documentation records no measured timing values.

Each run measures cold priority admission, unchanged warm priorities, an anchor
change, reduction, freezing and idle eviction. Alternating records are marked
populated and retained; other records are empty and unowned. Synthetic pressure
uses a fixture-local finite configuration and a deterministic digest provider.
Indexed saves include the requested number of pinned fixture ledger records and
measure first publication, an unchanged save and a one-chunk SDF edit. Pairs of
chunks share a region so sparse edits include a clean sibling;
each pair occupies a separate region. These saves use real indexed persistence,
with nonempty SDF and height payloads, without advancing simulation ticks.

`sample-<size>-<repeat>.json` retains the command, workload, source revision,
dirty flag, binary SHA-256, resolved fixture hash and fixture recipe version.
Each native sample has schema `luminumbra.active_region_scale_samples.v1`.
Only a complete, identity-consistent matrix publishes `report.json`, schema
`luminumbra.active_region_scale_report.v1`. Missing, duplicate, malformed or
mixed-identity samples refuse publication. Source, content and executable
identity are checked again before publishing. Failed captures retain partial
evidence and their synthetic saves; they never replace an existing directory.

The report preserves individual `SaveWorkCounts`, snapshot-copy and priority
comparison observations and interpolates elapsed p50/p95/p99/maximum using the
same percentile convention as stage telemetry. Memory observations are logical
record-value bytes, encoded scheduler/save ledger bytes and SDF/height value
bytes, reported separately from populated/empty record counts. They exclude container nodes,
scratch, allocator overhead and process RSS; they are not peak-memory estimates.
Encoding and memory accounting occur outside the scheduler timer. Cache-dependent
clean-sibling serialization counts remain separate raw observations.

The fast `ActiveRegionScale` gtests check schedule and encoded-byte parity with
and without observation, unchanged priority comparisons, and sparse-save work
using four chunks. `ActiveRegionScaleReportContract` checks the Python report
and capture refusal paths without launching the engine. Native execution and
large-world measurements remain pending. Continue using
`capture_sim_budget.py` for representative default, mountains and archipelago
per-system telemetry; this synthetic harness adds no replacement stage telemetry,
enforced limits, shipping simulation inputs, save fields or replay bytes.

## Representative comparison report

Capture both endpoints with `--check-neutrality`, then write the report outside
the checkout, retaining the original capture directories unchanged:

```sh
python3 -I -B tools/perf/report_sim_budget.py \
  ../sim-budget-captures/before ../sim-budget-captures/after \
  > ../sim-budget-captures/comparison.json
```

The JSON report retains all three presets and all 30 stage identities, including
inactive stages. Each endpoint includes its measured revision, dirty flag,
binary digest, resolved-input fixture hash, preset hash, seed, tick count and
radii. Per-stage rows retain the original p50/p95/p99 and maximum milliseconds,
sample count, total integer work and maximum per-tick work. Inactive durations
remain null. Per-preset neutrality results include both runs' world/replay
hashes, replay-work equality and effective simulation gates.

The report revalidates the original artifacts and requires complete telemetry-off
counterparts and the capture harness's populated roster, no-audio and 30-tick
autosave workload. Both endpoints must match the strict comparator's seed,
ticks, fixture hash, preset hash and radii, as well as effective gates. Different
revisions and binary digests are retained; world hashes may differ across
revisions. Missing or incompatible evidence refuses the report before writing
any JSON to stdout. Timing ratios are advisory after/before ratios (null for
inactive stages, a zero denominator or an unrepresentable ratio) and require
the same host and build conditions to interpret; the tool cannot verify those
conditions from the capture manifest.

The existing proposal emitter supplies the after endpoint's `proposal` and
`integer_proposal` with default integer headroom 3/2 (adjustable using
`--headroom-num` and `--headroom-den`). Only the proposal's advisory p99 fields
depend on durations; the integer proposal does not. Scheduler `RegionWork`
remains unevaluated. This report hands a proposal to issue #141 and does not
change enforced limits or establish native acceptance. `SimBudgetReportContract`
registers the two-tick synthetic regressions without launching the engine.

## Executable checks

Seven `SimBudgetTelemetry` gtests cover complete production artifact byte equality
against the pinned devel baselines, disabled callbacks and append behavior,
water work after nonzero → empty and nonzero → paused updates, interpolation,
duration-free replay comparisons, artifact separation, and batched tick identities
and reset. `ActiveRegions/SimBudgetWorldParity` runs the real populated server
three times (telemetry off, on, on) for each active-regions setting, covering all
four switch combinations. Each run advances 3,600 ticks. It checks world hash
equality within each active-regions setting, complete per-tick work replay,
mutually exclusive wind/weather stage coverage and exact field work totals,
starting plant/creature counts, and ordinary save bytes (including plants,
metadata and clock) across a telemetry toggle at the same tick. Active regions
intentionally changes the simulation; equality is required across telemetry
settings, not across active-regions settings. That parity test pins historical
terrain and disables other simulation gates. The opt-in shipping-workload capture
supplements it by exercising current shipping gates on all three presets; it does
not replace the historical test's two active-region modes.
`SimBudgetNeutralityContract` runs four mocked-launch Python tests on tiny synthetic
artifacts, covering command equality, hash/gate mismatch, disabled telemetry
refusal and failure publication ordering without an engine launch.
`SimBudgetCaptureContract` runs Python command and refusal tests, including both
field paths, refusal of mixed coverage, and wrongly typed or malformed world hashes.
Discovered totals, the measured delta against current devel,
execution results and qualification limitations are in the campaign receipt.
