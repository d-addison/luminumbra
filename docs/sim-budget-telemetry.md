# Simulation budget telemetry

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

Regional foliage retains separate soil, irrigation, growth and living-world stage
measurements. Only admitted due participants and owned field cells contribute
work. A bounded resume can revisit a stage several times within one host tick;
those visits sum their work and measured intervals into one stage/tick sample.
Off-due and empty-window stages retain a zero-work sample. Telemetry counters
are evaluated only with collection enabled and never affect regional admission
or simulation state.

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
  --evidence-dir build/campaign-archives-20260907/slice-A3/before
```

Repeat with another named directory after simulation work. The default workload
is seed `1337`, 3,600 ticks per replay, surface/collision radius 1, on default,
mountains and archipelago, with the existing ecology and planted rosters and
`--no-audio`. The script fails on missing binaries, existing output directories,
server errors, invalid artifacts or vacuous plant/creature/scent/field work.
It retains partial evidence on failure and does not publish a success manifest.
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
The latter is a new UTF-8 JSON evidence record with schema
`luminumbra.sim_budget_capture.v1` and required keys `revision`, `source_dirty`,
`binary_sha256`, `fixture_hash`,
`seed`, `ticks`, `surface_radius`, `collision_radius`, and `runs`. Each run has
`preset`, relative `artifact`, and exact argument-array `command`. Absence means
incomplete/unqualified capture; corrupt or inconsistent manifests and unknown
schema versions must be refused by comparison consumers; unknown object keys
are ignored. `source_dirty` records whether the source tree had uncommitted
changes and `binary_sha256` identifies the measured executable bytes, so a
working-tree capture is not mislabeled as a pristine revision. The default
executable path includes `.exe` on Windows. Logs are optional diagnostics, not
authoritative records: missing
or damaged log text does not synthesize measurements. No evidence file is read
by world loading or hashed into simulation. Compare stage work traces/totals
separately from duration distributions. This slice proposes no limits or caps.

The capture and paired server comparison share the resolved-input recipe in
`tools/perf/resolved_fixture.py`, using the `luminumbra.sim_budget.inputs.v2` domain:
canonical headless `data/common` content, all three selected preset documents,
and the content of each resolved biome-table reference, including references
outside `data/common`. Common content includes simulation configuration, policy,
species and structure placement/piece documents. Directory links are followed;
cycles refuse capture. JSON comments and the existing render-only exclusions
remain ignored. Missing referenced tables are recorded explicitly because the
native generator permits a fallback; adding or removing one changes identity.
Root locations do not enter the digest. This deliberately replaces the earlier
common-only capture identity so differing generation inputs cannot compare as
the same workload. The committed simulation-budget fixture identity is unpinned;
no pinned identity changes. Controlled fixtures also include `.perf-controlled`
marker text because its presence enables runtime overrides. Unmarked shipping
fixture hashes remain unchanged. Other performance capture formats retain their
existing identity recipes. An unreadable or malformed preset refuses capture
before launching the executable or creating evidence output. Inputs are hashed
again before publication; changed input content refuses a success manifest.

## Executable checks

Seven `SimBudgetTelemetry` gtests cover complete production artifact byte equality
against the pinned devel baselines, disabled callbacks and append behavior,
water work after nonzero → empty and nonzero → paused updates, interpolation,
duration-free replay comparisons, artifact separation, and batched tick identities
and reset. `ActiveRegions/SimBudgetWorldParity` runs the real populated server
three times (telemetry on, on, off) for each active-regions setting, covering all
four switch combinations in six passes overall. Each run advances 3,600 ticks.
The two enabled runs (A and B) use the production telemetry API; the disabled run
(C) leaves all stage samples empty. All three runs check starting plant/creature
counts, successful tick completion and nonempty world hashes. A and B must match
every stage's tick/work identities through `WorkMatches`, excluding durations,
as well as final world hashes and save fingerprints. C must match A's final
hash and save fingerprints, checking world replay identity across the telemetry
toggle. Each enabled trace checks complete stage coverage, consecutive tick
identities, positive creature/plant/scent work, exact wind/weather totals, and
initial plant/creature work counts. Wind and weather
must each report 4,096 cells per tick on the separate path; the mutually exclusive
combined path must report 8,192. Physics must report one scene tick per tick.
These expected per-tick counts cannot hide compensating errors in the totals;
the independent A/B comparison also establishes replay equality of every dynamic
stage's work, including plants, events, streaming and water. The telemetry unit
test separately verifies that `WorkMatches` compares stage/tick/work identities
and ignores durations; smoke mode still compares complete traces between its
two enabled runs.
All three parity runs check ordinary save bytes (including plants, metadata and
clock) across a telemetry toggle at the same tick, covering both directions.
Save fingerprints also match across runs, normalizing only the fresh world's
creation time and its index digest, plus the transaction header already
normalized for save toggles; member generations remain checked. Active regions
intentionally changes the simulation; equality is required across telemetry
settings, not across active-regions settings. `SimBudgetCaptureContract` runs
seven Python command and refusal tests, including both field paths, refusal
of mixed coverage, and wrongly typed or malformed world hashes. Discovered totals, the measured delta against current devel,
execution results and qualification limitations are in the campaign receipt.

## Bounded regional observations

When **both** `sim.active_regions` and `--sim-budget` (or the session telemetry
switch) are enabled, `sim_budget` additionally contains `regions`, with schema
`luminumbra.sim_budget.regions.v1`. The enclosing v2 schema, stage identities,
order, units and existing artifact fields remain unchanged. With either switch
off this extension is absent. The exact `devel-{0,1,2}.json` production fixtures
and the existing v2 stage-only encoding remain the compatibility baselines.

`RegionTelemetry` owns observations independently of the ledger's completed-work
registers. It never writes those registers, schedules consumers, activates a
region, allocates an identity, changes RNG or feeds a persistence/hash codec.
Accepted creature/population counters come from the existing successful-operation
counters, relative to the start of collection. Attempted work and rolled-back
population commands receive no success credit. The scheduler's work observation
is the sum of accepted previous-boundary work used by that schedule; unknown-owner
work contributes only to `rejected_work`. A failed schedule publishes no scheduler
sample or state gauges. Accepted work preceding a later consumer fault still
stands and its activity counters are observed when the first fault latches.

The block has these required fields. All counts and ticks are unsigned 64-bit
integers; booleans and fractional counts are invalid. There are no row or tick
arrays. Each counter has one of three meanings: **gauge** (latest observation),
**high-water** (largest observed gauge), or **capture total** (sum since reset).

| Field | Meaning and classification |
|---|---|
| `samples` | Capture total of retained successful scheduler samples, at most 108,000 |
| `first_tick`, `last_tick` | First/last retained absolute scheduler tick; both null for an empty capture |
| `complete`, `dropped_samples` | Capture validity and capture total of observations lost to a cap, invalid timing/order, counter discontinuity or arithmetic overflow |
| `duration_ms` | Scheduler-duration summary `{p50,p95,p99,maximum}`, or null without samples |
| `scheduler.{ledger_records,active,reduced,frozen}` | Each `{current,maximum}` contains a gauge and its high-water; measured after successful scheduling and retention |
| `scheduler.due_total` | Capture total of surviving due-region decisions |
| `scheduler.accepted_work_total` | Capture total of accepted work consumed by successful schedules, including resumed retained work |
| `scheduler.work_limit` | Gauge of the configured scheduler work limit |
| `scheduler.pressure_max` | High-water of individual retained-record pressure |
| `scheduler.rejected_{anchors,work,capacity}` | Capture totals of deterministic scheduler rejections; frame-input rejections are excluded |
| `creatures.{full,coarse,frozen}` | Gauges from the existing const activity snapshot at artifact capture |
| `creatures.{promotions,demotions,full_updates,coarse_updates,births,deaths,removals,parked_travel}` | Capture totals from the corresponding accepted-operation counters; parked travel counts accepted travel updates that remain parked |
| `population.{successes,failures}` | Capture totals of committed population transactions and refused transactions, respectively |
| `population.pending_activations` | Gauge at artifact capture |
| `controllers.{created,removed,stale_handle_rejections}` | Capture totals of successful runtime controller operations and explicit stale-handle rejections |
| `persistence.parked_bytes` | `{current,maximum}` gauge/high-water, sampled at completed ticks and artifact capture |
| `persistence.{saves_total,failures_total}` | Capture totals of completed save-outcome callbacks and their failures; no-op paths without an outcome callback do not count as saves |
| `persistence.save_samples` | Capture total of retained save-outcome durations, at most 4,096 |
| `persistence.save_duration_ms` | Summary of successful and failed save-outcome durations, or null without samples |
| `water.{reserved,used,unused,unallocated}` | Capture totals of committed window grants: reserved/used include paired border windows; unused is allocated but unused interior work; unallocated is ungranted interior work. Reserved equals used plus unused |
| `water.completed_cells` | Capture total of committed preparation and solver cell work; reservations alone receive no completed-cell credit |
| `water.bound_known`, `water.bound` | Explicit boolean and nullable unsigned tick bound. The current dynamic-eligibility implementation always reports false/null; it establishes no fixed-cohort service theorem |
| `replay.trace_digest`, `replay.work_replay_match` | Deterministic work/transition digest and comparison verdict; incomplete captures never match |
| `fault` | Null, or first latched `{tick,message}`. Diagnostic text is at most 256 printable ASCII bytes; unsupported characters become `?` and excess text is truncated with an incomplete-capture indication |

Each scheduler sample times the existing schedule call only, including its
retention walk. When enabled, that walk observes each surviving record once;
it adds no record inventory or per-record allocation. Activity deltas are fixed
size, and water observations follow successful `UpdateRegional` publication.
Creature gauges use the existing const snapshot only at artifact capture, outside
the tick. Parked bytes use the owner-thread cached byte count without walking
parked chunks. With telemetry disabled there is no regional sample allocation, clock
read, activity snapshot or additional record scan. Observational storage is
cleared on world reset and toggles.

The collector allocates fixed arrays only when enabled: 108,000 scheduler doubles
and 4,096 save doubles, totaling **896,768 bytes** per live collector. Export
sorts the observational duration arrays in place and copies only scalar
summaries; it allocates no sample arrays or percentile scratch. Sorting timing
observations does not change tick coverage, counters or the deterministic trace,
and collection may continue after export. Two live sessions in Heavy therefore
retain **1,793,536 bytes** of regional samples at peak, including while both
exports and artifact serialization coexist, below 2 MiB. Scalar observations,
summary objects and the bounded logarithmic sorting stack also fit within the
remaining budget. Allocation accounting tracks live and peak sample-array
storage, and the production session/export/artifact regression fills both
collectors to both caps before checking that export never increases that peak.
Copies of `SimBudgetTelemetry` use the same bounded export path. The live
session supplies final gauges through a const export observer; exported values
retain neither that observer nor its owner, so copies remain valid after session
destruction. Explicit copies of a raw `RegionTelemetry` collector retain
independent arrays and are outside the capture path. Summary interpolation uses
`(n-1)*p`, matching the existing stage convention. Empty distributions are null;
a single duration supplies every percentile and maximum. The fixed block,
including escaped diagnostic text and whitespace, stays below **16 KiB**; the
serializer checks this independently. Unknown extension keys count toward the
reader's byte cap.

At either sample cap the collector checks the bound before writing, retains the
bounded prefix, sets `complete=false` and increments `dropped_samples`. Unsigned
counter overflow saturates observational totals and marks the capture incomplete.
Neither condition changes simulation, completed-work authority or the scheduler
fault latch. Capture qualification refuses incomplete evidence. Existing stage
storage retains its earlier lifetime/size behavior; these caps apply to the new
regional collector and block.

The regional digest uses FNV-1a with domain markers and little-endian fixed-width
values. In event order it folds scheduler ticks, accepted work, limits/rejections
and the existing schedule digest (which covers due state and transitions),
committed water work, and accepted simulation activity deltas. Durations,
persistence observations, runtime controller creation/removal/stale-handle
receipts, creature gauges, diagnostic text and presentation never enter this
comparison. A loaded session can reconstruct runtime controllers while preserving
the same durable work trace. No regional telemetry value enters world hashing, peer identity,
HashMsg, LREC1 or saves. Smoke requires both the existing stage work match and the
regional match. Enabled Heavy measures only the shared continuation window,
resetting observations in both sessions immediately before continuation; its
regional evidence uses the same writer and comparison.

The capture reader continues to accept legacy artifacts without `regions` and
validates every required group whenever the block is present. It refuses invalid
types, nonfinite or unordered durations, invalid gauges/accounting, duplicate JSON
keys, future schemas, excess bytes/samples, faults, incomplete evidence and a false
work verdict. Unknown object keys within the byte budget remain compatible.
For regional qualification, point the capture command at an existing fixture root
whose `data/common/systems.json` enables `sim.active_regions` and add `--regions`:

```sh
python3 tools/perf/capture_sim_budget.py --root build/regional-fixture \
  --regions --evidence-dir build/regional-evidence
```

The command validates both switches, records `regions_required` in the manifest,
and requires ticks 1 through the requested last tick. It never changes the fixture
configuration. The native trigger remains **p99 above 0.5 ms or allocation above
2 MB/s at 2,000 records**; native allocation measurements and long-run performance
qualification are pending. Python contract checks and source regressions do not
establish those measurement results.
