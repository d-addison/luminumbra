# Simulation budget schema

`luminumbra.sim_budgets.v1` stores `identity.seed` and `identity.ticks` as
integers (booleans and strings are rejected). Null or omitted values are
reported as `unpinned` in the check report's `identity` and `summary`, and
make `--require-complete` exit with code 3. Capture manifests retain their
string seed format; the proposal emitter converts it to an integer.

A capture's `fixture_hash`, and any non-null budget `identity.fixture_hash`,
must contain exactly 64 lowercase hexadecimal characters. Null budget
fixture hashes are allowed, including under `--require-complete`; completeness
requires pinned seed, ticks, stage limits and total limits, not a fixture hash.

Each stage entry has `max_work_per_tick` (a non-negative integer or null)
and an optional boolean `expect_inactive`, defaulting to false. A stage
with zero samples retains its `inactive` verdict. The report also includes
its `expect_inactive` value and counts unexpected inactivity in
`summary.unexpected_inactive`. Under `--require-complete`, unexpected
inactivity exits with code 3; explicitly expected inactivity is accepted.
Without that flag, inactivity is reported without changing the exit code.
The proposal emitter records the observed inactivity in emitted budgets.

Any exceeded pinned work limit exits with code 1, even when identities or
other limits remain unpinned. Invalid input or mismatched pinned workload
identity exits with code 2. Durations remain advisory.

The committed limits enforce the shipping-default workload in CI with
`--require-complete`: seed 1337, 600 ticks, surface radius 1 and collision
radius 1, across default, mountains and archipelago. The capture script uses
its own checkout root and `data/common/systems.json`, with populated ecology
and plant rosters and autosaves every 30 ticks. The separate base-versus-head
comparison retains its gates-off `perf-common` fixture; its marker-guarded
source adapter does not apply to the shipping capture root.

Baseline `identity.fixture_hash` and `identity.revision` remain null so the
work limits can gate later content and code revisions. The capture manifest
and proposal retain the measured revision and fixture hash for evidence.
The hash uses `fixture_hash(..., scope="headless-server")` over `data/common`;
that scope still includes some client-only content, so pinning it would reject
unrelated changes before checking simulation work. Simulation work changes
still have to satisfy the pinned limits or receive a new capture and explicit
budget calibration. Seed, ticks and both radii remain pinned.

Positive stage maxima and the maximum of the summed per-tick traces receive
integer ceiling headroom of 3/2. All three presets measured a total maximum
of 147526 and pin a total limit of 221289. Stage maxima are not summed to
derive that total limit.

The proposal emitter deliberately leaves `idle` (samples > 0, maximum work 0)
limits null for calibration. The schema permits zero, and the checker treats
zero as pinned: any positive work exceeds it. The committed baseline pins
every zero-work stage to zero, symmetrically across stages and presets. This
includes animation, instinct, perception, locomotion, energy, irrigation,
disease, fire, circadian, events and server_streaming, each of which has 600
samples. They remain expected-active; their zero counts describe this fixed
workload's participants or submissions, not an absence of execution. New work
requires measurement and an explicit budget update, rather than an arbitrary
minimum allowance. Durations still do not gate.

Only the separate `wind` and `weather` stages set `expect_inactive: true`,
also with zero limits. With shipping `sim.active_regions` enabled and both
fields initialized, `GameSession::TickSimulation` directly invokes
`WeatherSystem::UpdateFromClock` every fixed tick; these fields have no
separate scheduler registration or delayed cadence. The server loads and
applies the shipping system configuration before creating the world and
drives that fixed tick. The combined call executes wind, weather, then wind
again, including storm processing. Telemetry intentionally attributes its
entire duration and 8192 input cells to `wind_weather`, with 600 samples and a
pinned limit of 12288. The alternative separate hooks are not reached, so
their zero samples are expected, not missing telemetry. See
[telemetry stage definitions](../../docs/sim-budget-telemetry.md) and
`GameSession.cpp`'s `Stage::WindWeather` branch. A switch to the separate path
would exceed its zero limits and leave `wind_weather` unexpectedly inactive.

## Re-pin the shipping-default budgets

Start from a clean checkout of the revision to measure (`git status --porcelain`
must be empty), with its matching server binary already built using the `perf`
preset. Run from the repository root. Use a new evidence directory each time;
the capture refuses an existing one. The ignored `build/` paths keep capture
outputs out of the source status.

```sh
python3 tools/perf/capture_sim_budget.py \
  --binary build/perf/bin/luminumbra_server_app \
  --evidence-dir build/sim-budget-capture \
  --ticks 600 --seed 1337 --radius 1 --collision-radius 1
python3 tools/perf/propose_sim_budgets.py build/sim-budget-capture \
  --headroom-num 3 --headroom-den 2 \
  --output build/sim-budget-proposal.json \
  --emit-budgets build/sim-budget-candidate.json
```

The capture covers default, mountains and archipelago with the shipping
configuration, both populated rosters and autosaves every 30 ticks. Its general
default is 3,600 ticks; the explicit `--ticks 600` above matches the CI baseline.
`--output` writes proposal evidence; `--emit-budgets` creates the candidate
baseline. Review the observations before applying it. Keep the proposed positive
stage and per-preset total limits with 3/2 headroom. For every preset, calibrate
each `idle` stage to limit 0 with `expect_inactive: false`. Only `wind` and
`weather` may be `inactive`; give those limit 0 and `expect_inactive: true` for
the combined path described above. Investigate any other inactivity instead of
accepting it. The following applies that policy and writes the baseline:

```sh
python3 - <<'PY'
import json
from pathlib import Path

proposal = json.loads(Path("build/sim-budget-proposal.json").read_text(encoding="utf-8"))
budgets = json.loads(Path("build/sim-budget-candidate.json").read_text(encoding="utf-8"))
for preset, data in proposal["presets"].items():
    inactive = {name for name, stage in data["stages"].items()
                if stage["status"] == "inactive"}
    if inactive != {"wind", "weather"}:
        raise SystemExit(f"Investigate unexpected inactive stages in {preset}: {inactive}")
    for name, stage in data["stages"].items():
        entry = budgets["presets"][preset]["stages"][name]
        entry["expect_inactive"] = name in inactive
        if stage["status"] in {"idle", "inactive"}:
            entry["max_work_per_tick"] = 0
budgets["identity"]["revision"] = None
budgets["identity"]["fixture_hash"] = None
Path("tools/perf/baselines/sim-budgets.json").write_text(
    json.dumps(budgets, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY
python3 tools/perf/check_sim_budgets.py build/sim-budget-capture \
  tools/perf/baselines/sim-budgets.json --require-complete \
  --output build/sim-budget-check.json
```

Keep the capture and proposal evidence unchanged, including their revision and
fixture hash. The final check must exit 0 with no failures, unpinned, unbudgeted,
missing or unexpected-inactive entries. All stage and total limits must be
non-null; seed 1337, ticks 600 and both radii 1 must stay pinned in the baseline.
