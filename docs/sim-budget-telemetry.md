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
`chunk|wind|weather|aether|scents|ecology|plants` order. Preset revision 6, LMR1
container v2, the existing world manifest, FSD2 payload v3 and canonical
serialization are unchanged. With collection off, no counter callback or new
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
| `wind` | 64 × 64 grid cells when the field exists, otherwise zero |
| `weather` | 64 × 64 grid cells when the field exists, otherwise zero; includes storm processing |
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
| `server_water` | Water cells actually stepped by that update (`dbg_cells_simmed`) |

The first and last two rows are server supplements. Physics precedes the
session tick. Streaming and water still run **outside** `TickSimulation`, in
the existing server update. Streaming duration sums its existing disjoint
process-completed, telemetry, activation, meshing and collision timers plus
the existing due-activation wait. Water uses the existing water-phase bracket.
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
    "schema": "luminumbra.sim_budget.v1",
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
IEEE double. The trace is ordered by tick, with one sample per executed tick.
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
  ignore the entire extension. Within schema v1, required keys must be present;
  unknown, duplicate, missing or reordered **stage identities** are refused.
  Changed stage units/order require a new telemetry schema identity.
- Corrupt JSON, wrong types (including booleans/fractions used as integers),
  out-of-range counts, inconsistent totals/ticks, missing timer coverage,
  nonfinite/negative/unordered percentiles, and false replay verdicts refuse a
  telemetry capture. They never cause a world-load refusal: artifacts are not
  world input and there is no telemetry save record.
- A missing or unrecognized telemetry schema, including a future version, is
  refused by the capture reader. It must not reinterpret unfamiliar evidence
  as an empty or zero-cost tick. Duration comparisons are observational only.

Independent devel smoke artifacts already contain nondeterministic wall times,
generated world IDs and render-mesh diagnostics, so raw-byte equality between
independent executions is not a valid existing guarantee. The disabled path
preserves the existing serializer and its exact output for identical inputs;
`DisabledSkipsCounterAndPreservesArtifactBytes` checks this property. Runtime
comparison must also report authoritative hash parity rather than treating
those existing nondeterministic fields as simulation changes.

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

Five `SimBudgetTelemetry` gtests cover disabled byte preservation, interpolation,
duration-free replay comparisons, artifact separation, and batched tick identities
and reset. `SimBudgetWorldParity.Populated3600TicksPreserveWorldHashAndReplayWork`
runs the real populated server three times (off, on, on), checks world hash
equality and complete per-tick work replay, pins starting plant/creature and
wind-cell counts, and checks binary save bytes across a telemetry toggle at
the same settled tick. `SimBudgetCaptureContract` runs six Python command and
refusal tests. Both build lanes discover 2,017 tests with manual tests excluded, exactly seven
more than the 2,010 entries discovered from the devel source archive. Execution
results and any qualification deviations are recorded in the campaign receipt.
