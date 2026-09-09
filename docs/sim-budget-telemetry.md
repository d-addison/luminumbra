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
settings, not across active-regions settings. `SimBudgetCaptureContract` runs
seven Python command and refusal tests, including both field paths, refusal
of mixed coverage, and wrongly typed or malformed world hashes. Discovered totals, the measured delta against current devel,
execution results and qualification limitations are in the campaign receipt.
