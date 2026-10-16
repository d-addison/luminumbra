# Deterministic work budgets

Issue #396 gates work items on the existing standard CI lanes. The reviewed
ceilings are in `test/fixtures/deterministic-work-budgets.json`. Native fixtures
compare every named counter with its ceiling and report observed and limit
values. Missing or extra measurements fail. No duration, timestamp query, frame
rate or GPU timing enters the comparison. Changing a ceiling requires reviewing
that data file; the tests do not learn or rewrite limits.

`DeterministicWorkBudget` is registered through `common_tests` in
`test/CMakeLists.txt`. The foliage workload and stale-result regression are in
the existing `FoliageRebuildTest` fixture in `pass_context_test`. Both targets
already participate in standard CTest discovery on Linux, MSVC and UCRT64.
The hidden-window fixture uses the existing software GL setup and context lock.
There are no new runners, labels, timeouts or capacity requirements.

## Workloads and units

The foliage workload presents nine supported columns, moves the camera across
a signature cell, edits one support column, and then holds the camera still for
eight steps. It runs both CPU fallback and compute scatter. Readback and input
hash evidence are off, as in normal presentation. Nine columns require three
steps at the existing four-column admission rate. The test requires a nonempty
instance set, real upload work on movement/edit, one invalidated column on edit,
and a completely drained backlog before idle. Idle upload bytes and hash bytes
must each equal zero.

Foliage counters count dirty columns sampled, individual surface samples,
uploaded bytes, transfer operations, copied bytes, hashed bytes, deferred
columns and positive backlog growth per step. GPU input uploads count four
operations; a CPU mapped instance copy counts one nonempty transfer. Scratch
allocation is already a separate attribution counter and is not an upload.
Surface samples are accumulated once per complete candidate loop, avoiding
additional per-candidate diagnostic calls. The byte ceilings cover at most
nine times 8,192 packed 36-byte records, two CPU copies plus chunk metadata,
or the compute path's packed inputs. At most four columns are sampled in one
step. `drain_steps` measures deterministic calls, including the initial call.

The live subsystem fixture uses a flat synthetic world, seed 1337, one worker,
a one-column streaming radius, one dynamic physics body, one local character,
one server avatar, and one 8-by-8 fixed-point water tile. It moves at step 8 and
edits resident terrain at step 24. Both streaming update overloads run, with
tick-keyed activation. After the script ends, the fixture requires the backlog
to reach zero within the pinned 64-step bound. Each step checks work ceilings;
positive assertions prevent empty workloads from passing. The separate water
tile and standalone physics scene keep their input sizes fixed while streaming
changes. Streaming terrain collision is not installed in that physics scene.

The time-free `SimBudgetTelemetry::WorkItems()` trace extends simulation
diagnostics with these units:

| Counter | Work item |
|---|---|
| `water_cells` | Fixed-point cells stepped in the latest water update, including zero on empty/paused updates |
| `streaming_generation` | Generation submissions in the current streaming update |
| `streaming_meshing` | Meshing submissions in the current streaming update |
| `physics_bodies` | Input bodies in the latest physics scene update, including static bodies |
| `physics_characters` | Local-player and server-avatar character steps since that scene update |
| `active_region_records` | Retained scheduler records after this session tick |
| `active_region_due` | Regions due in this session tick |
| `snapshot_copies` | Pointer entries copied by streamed-chunk and renderable-chunk snapshots during the server tick |
| `activation_queries` | Surface columns enumerated by streaming activation in the current update, zero on an elided pass |

These units describe inputs/submissions, not solver iterations or all terrain
generation performed by synchronous editing/preparation APIs. Snapshot counts
cover pointer snapshots, not deep SDF captures or render mesh metadata. The
live fixture also bounds the resident loading/idle/meshing backlog and its
positive growth. The existing generation and water counters retain their units.

Collection adds an optional `work_items` object with schema
`luminumbra.sim_work.v1` to the enabled `sim_budget` artifact, with ordered named
counters and integer `{tick, work}` traces. It does not change the existing
`luminumbra.sim_budget.v2` timed-stage layout. Work replay comparison includes
the new traces and excludes all clock values. Session ticks own region samples;
the server runner owns the other samples. Collection disabled produces no trace
or artifact extension. These finite-run diagnostic vectors share the existing
collector lifetime and clear/reset behavior. They never drive scheduling,
simulation ordering, configuration identity, saves or hashes. Region canonical
bytes and schedules are checked across collection settings; the existing
populated server parity regression continues checking full world/save identity.

## Refusal and progress regressions

The same comparator used by the live fixtures receives an injected limit-plus-one
value for every counter in every section, including both progress bounds. A
separate test injects exactly one upload byte and one hash byte into idle.
Equality with a limit passes; missing/extra counters fail. Native failures print
all observed/limit pairs; successful comparisons retain them as test properties.

The stale-result fixture completes an actual asynchronous foliage result, edits
its support revision before adoption, and checks that the old generation is
discarded. A subsequent current-generation result must still be adopted. A GL
completion barrier establishes readiness for this regression; no timing is read.
Production consumption validates generation and payload size before copying or
allocating instance storage. Revision resolution precedes readback adoption,
including on cache-elided frames.

## Terrain upload follow-up

Terrain upload budget counters depend on the sibling terrain-upload work and
are deliberately absent here. After that work lands, add its actual upload
bytes/counts and queue observations to this reviewed budget file and exercise
the real terrain uploader on the same standard lane. Pointer snapshot counts
and foliage transfers do not establish a terrain-upload budget.
