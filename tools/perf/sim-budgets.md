# Simulation budget schema

`luminumbra.sim_budgets.v1` stores `identity.seed` and `identity.ticks` as
integers (booleans and strings are rejected). Null or omitted values are
reported as `unpinned` in the check report's `identity` and `summary`, and
make `--require-complete` exit with code 3. Capture manifests retain their
string seed format; the proposal emitter converts it to an integer.

A capture's `fixture_hash`, and any non-null budget `identity.fixture_hash`,
must contain exactly 64 lowercase hexadecimal characters. Null budget
fixture hashes remain allowed while the workload identity is being pinned.

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

The committed limits remain null until AC2p pins them from the first devel
push's `sim-budget-evidence` artifact. The CI capture step then switches to
`--require-complete`.
