# Active-region ledger and scheduler

The host ledger implements the persistent-active scheduling foundation in
[distant-world.md](distant-world.md). It is gated by the existing
`sim.active_regions` switch, absent and false by default. No systems.json value
is added. Budgets remain unlimited in shipped configuration. No distant system
consumes this schedule yet; field pages, creature records, water shares,
diagnostics and measured budget activation remain separate work.

## Activation and ordering

Keys are signed persistence region coordinates: floor(world X / 512),
floor(world Z / 512), bounded to [-32768, 32768) by the existing packed chunk
identity. The fixed 512 metre XZ disc intersects the region's closed square;
edge tangencies count, corner squares outside the circle do not. Vertical
position and adaptive streaming radii do not affect membership. Regions remain
in the ledger after departure. The host's last walking feet position is stored
as an optional anchor and restored before ticking. Camera movement and noclip
updates cannot replace it. Replicated server avatar positions are supplied at
the host tick boundary; avatar persistence remains outside this slice. An absent
replicated list selects the single-player fallback; an explicitly empty server
list visits no regions, even when the ledger holds a local anchor.

Voxel edits notify each modified chunk's region. The game has explicit
`NotifyGroundObjectEdit` and `PinActiveRegion` entry points; ground-object
records and simulation are not introduced here. Edit and pin requests set a
persisted pending-wake bit, consumed on the next host tick. Proximity or a pending
wake directly selects full active cadence, including a thaw from frozen; that
is one transition, with no intermediate simulated step.

Priority order is pinned, edited, then visited, followed by ascending ticks since
last proximity, then signed X and Z. Budget reduction selects the lowest
priority eligible region, recovery selects the highest. At most one budget
transition is selected per tick; forced wakes and that transition are applied
in forward priority order. Work entries sum region ticks, entity updates, water
cells and page updates before any decisions, saturating unsigned 64-bit sums
rather than wrapping. Duplicate entries are additive and order-independent.
An unknown region in work input is an error. Work is supplied by the host, never
by timers or streaming telemetry. No consumer counters are invented here.

Pressure greater than the configured limit increments consecutive over-hold;
otherwise (including equality) it increments under-hold. The opposite counter
resets. Counters saturate at the configured hold. Near regions and pending wakes
reset both. After the hold, active becomes reduced, then reduced becomes frozen
if later pressure persists; recovery reverses those steps after the under-hold.
Queued work makes a host tick incomplete for snapshot purposes: metadata and
world saves refuse until the next simulation tick consumes it. The queue is not
persisted. Every state change resets both counters. Reduced cadence is 2^shift,
with shift in [1, 16]. Phase is the low `shift` bits of FNV-1a-64 over LE i32 X and Z.
A region is due when `(absoluteTick & (divisor - 1)) == phase`; active is always
due, frozen never is. All stamps use WorldClock, with no load-time restart.
Repeated/backward scheduler ticks refuse. Frozen scheduling metadata can change;
its simulation cursor, last-ticked stamp and frozen content digest do not advance.

`BoundedRegionResumeHook` defines an opt-in interface receiving region key,
freeze tick, resume tick and bounded elapsed ticks. `resume_window` clamps elapsed
ticks to both the system's explicit cap and one WorldClock calendar day; it
refuses a resume before freeze. No callback consumer or catch-up is installed.

## File and compatibility

The record is `<save>/chunks/region/active-regions.arl`, following the accepted
contract's placement in the region integrity scan. This resolves the slice
brief's conflicting save-root wording in favour of that contract. ARL1 does
not alter LMR1 v2, the world manifest, preset revision 6, EFS1, plant payloads,
FSD2 v3, or canonical chunk serialization. An anchor-only ledger is not a chunk
snapshot: with active regions enabled, the first save with dirty chunks writes all
resident chunks, including clean chunks in other regions. The disabled path keeps
the legacy existing-save query: a plant-only save makes later saves incremental.
Snapshot transaction publication is separate work; this slice uses the existing durable temporary-write and atomic
replacement primitive for the ledger, without claiming cross-file atomicity.

Clock/ledger pair saves are synchronous and non-reentrant on the process's host
thread. The client calls them during world creation, its main-thread pause-menu
quit callback, and shutdown after the main loop returns. Its explicit persistence
scenario also runs on that thread. The server calls them synchronously from
`RunFixedTicks` autosave, `SaveFullSnapshot` between runs, and `Shutdown` after
the tick loop. Job workers do not call the pair writer. No overlapping production
pair writers were found, including across sessions in one process. The call sites
state this ownership contract, and a debug-only atomic assertion guards the whole
pair writer against concurrent or reentrant calls. It does not serialize saves;
there is no production locking scheme. Independently launched processes sharing
one save directory are outside this ownership contract.

Incoming pairs are validated before either replacement. Forward saves replace
metadata first; rewinds replace the ledger first, preserving ledger tick <= clock
tick at every boundary. Ledger bytes are staged as unique
`<save>/active-regions.arl.tmp.<pid>.<sequence>` files, outside the strict
`chunks/` and `chunks/region/` scans on the save filesystem, then atomically renamed into place.
A separately mounted region directory causes replacement to fail across devices;
no copy fallback compromises atomicity. Termination can leave staging debris in
the save root, but loading and later saves ignore it. Unknown files inside the region
directory are still refused. The POSIX subprocess regression exits after the
staging file is flushed and closed and verifies recovery while that file remains.

An accepted partial pair can contain an older ledger than its saved clock. Session
load rebases only the in-memory ledger header to the restored clock. It preserves
every region record, hold counter, pressure, simulation cursor, frozen digest,
configuration and anchor. It executes no scheduler tick or catch-up. Immediate
save/edit stamps therefore fit under the header, and the next actual host tick
uses the restored absolute clock. The raw persistence reader still exposes the
durable older header for validation and inspection. Recovery tests reload rewind
metadata failures, forward ledger failures and terminated ledger writes into
fresh sessions and save immediately, both unchanged and after an edit.

Absent means an empty ledger, with unlimited scheduling defaults and no saved
local anchor; the host supplies its spawn feet position for a legacy save. Its
header tick starts at the restored WorldClock tick without executing a tick, so
an immediate edit or pin can be saved before simulation advances.
No old-world migration occurs. Corrupt, truncated, oversized, duplicate,
misordered or inconsistent records refuse with `Corrupt active-region ledger.`
A valid magic followed by a version greater than 1 refuses distinctly with
`Unsupported future active-region ledger version.` Version zero is corrupt.
A save containing the ledger requires `sim.active_regions`; opening or writing
with it off refuses as incompatible configuration. Older engine builds refuse
this file as an unknown region file. Decode failure never changes the destination
ledger. Unknown fields cannot be skipped: extensions require a new version.

All integers are little endian, signed integers use two's complement, floats
are IEEE binary32 (finite, with positive zero as the sole zero encoding).
The header is exactly 48 bytes:

| Offset | Bytes | Value |
|---|---:|---|
| 0 | 4 | ASCII ARL1 |
| 4 | 2 | version = 1 |
| 6 | 2 | reserved zero |
| 8 | 8 | scheduler clock floor (last scheduled or reconciled restored tick) |
| 16 | 8 | work limit; UINT64_MAX is unlimited |
| 24 | 4 | nonzero hold ticks |
| 28 | 1 | reduced cadence shift, 1 through 16 |
| 29 | 1 | local anchor present, 0 or 1 |
| 30 | 2 | reserved zero |
| 32 | 12 | local walking feet X, Y, Z; all zero if absent |
| 44 | 4 | record count, at most 1,048,576 |

Each record is exactly 104 bytes, sorted uniquely by signed X then Z:

| Offset | Bytes | Value |
|---|---:|---|
| 0 | 8 | i32 region X, i32 region Z |
| 8 | 1 | flags: visited bit 0, edited 1, pinned 2, populated 3; others zero |
| 9 | 1 | state: active 0, reduced 1, frozen 2 |
| 10 | 1 | cadence shift: zero when active, configured shift otherwise |
| 11 | 1 | pending wake, 0 or 1 |
| 12 | 4 | derived cadence phase |
| 16 | 40 | u64 first active, last proximity, last edit, last ticked, frozen at |
| 56 | 8 | u64 last save (observational) |
| 64 | 8 | u64 last decision tick |
| 72 | 8 | u64 per-region pressure |
| 80 | 8 | u32 over-hold, u32 under-hold |
| 88 | 8 | u64 water cursor |
| 96 | 8 | u64 frozen simulation content digest |

The footer is one LE u64 FNV-1a-64 checksum over every preceding byte (offset
basis 14695981039346656037, multiplier 1099511628211). Total length must be exactly
48 + count * 104 + 8, capped at 109,051,960 bytes before allocation. No trailing
bytes, padding, native structs, section omission or alternate encodings are
accepted. Stamps cannot exceed the header tick, counters cannot exceed the hold
or both be nonzero, phases must match the region key, and a frozen region's last
ticked stamp precedes its freeze stamp. The ledger tick cannot exceed the saved
WorldClock tick. Configuration is stored and hashed with the ledger rather than
read from an unrecorded machine-local budget.

Frozen content is supplied at the transition by merging live lod-0 chunks with
durable region records by id, with live records taking precedence. The provider
uses a dedicated `region_content_v1` projection in chunk-id order: identity,
SDF, heightmap, materials, integer water depth/bed/flux, water initialization,
resolution and solver sleep/threshold state. Lifecycle state/state_value,
collider flags, render meshes/bookkeeping, transient dirtiness, far render
records, compression and container envelopes are excluded. Legacy hashing and
serialization are unchanged. Later durable per-region systems must extend that provider
with a declared canonical projection before consuming the scheduler.

## Hash and replay

An empty ledger contributes no hash section. Otherwise ASCII
`active_regions:v1:` precedes the header bytes starting at offset 8 and ordered
record bytes, omitting each last-save u64. The file magic, version envelope and
footer are absent. This contribution follows the existing clock bytes within
the existing ecology fold. The outer composition remains
`chunk|wind|weather|aether|scents|ecology|plants`. Storage paths and transaction
generations are not ledger fields and never enter this projection.

The per-tick digest is FNV-1a-64 of ASCII `region_schedule:v1:`, LE u64 absolute
tick and summed work, LE u32 region count, then priority-ordered records of LE
i32 X/Z, u8 resulting state and u8 due, followed by LE u32 transition count and
ordered transitions of LE i32 X/Z and u8 previous/resulting state.
The enabled replay trace carries the digest outside unchanged LREC1 checkpoint
and container bytes. No trace file or artifact key is emitted with the switch
off. Schedule trace validation refuses missing, corrupt or future traces on an
enabled replay; details are specified alongside its implementation below.

The replay companion is `<recording>.regions.json`, with exact keys `schema`,
`ticks`, `checksum`. Schema is `luminumbra.region_schedule_trace.v1`. Ticks is an
array of exact `{tick, digest}` objects, consecutive unsigned ticks starting at
1, with a 16-character lowercase hexadecimal schedule digest. Checksum is the
existing StableChecksum of the compact sorted-key JSON object containing schema
and ticks alone. Writers append one LF and use binary streams. Readers limit the
file to 96 MiB and the roster to 1,000,000 ticks, require the exact replay tick
count, and reject unknown keys, invalid digests and checksum mismatches as
`Corrupt region schedule trace.` Absence is `Missing region schedule trace.`;
a higher numeric schema suffix is `Unsupported future region schedule trace
version.` An off replay with a companion refuses incompatible configuration.
Finalizing a disabled recording removes any previous companion at that path;
a filesystem removal error makes recording fail and reports the path and cause.
The sidecar is diagnostic replay evidence, never a save member or world-hash
input. Recording and playback emit and verify one digest after each host tick;
LREC1 payloads and checkpoint fields are unchanged.

## Verification fixtures

`test/fixtures/active-regions/devel-off/r.0.0.lmr` and its adjacent
`world-manifest.json` are exact bytes captured from origin/devel at
f2895388cd14fda8591ed571f87c0ffe026db15d. The fixture is a default-off seed-1337
session after seven ticks, with one adopted idle chunk at (0,0,0), height samples
8, 8.5 and 9, and a dirty voxel flag. The test compares its composed world hash
against an independently assembled session in the same build, without the disabled
region calls or the save operation under test. The metadata byte-layout oracle remains
`DefaultOffKeepsLegacyMetadataBytesAndTickZeroLoad`; creation timestamps are
observations rather than pinned fixture inputs. Missing or changed fixture bytes
fail the exact-byte test; these fixtures are never installed as runtime data.
The unchanged LMR1/manifest validators supply their corruption and future-version
refusals.

`test/fixtures/active-regions/devel-off-plant-edit/` is a second exact-byte
oracle captured from the same origin/devel commit in this worktree. A seed-1337
session first saves only a plant roster, then retains idle chunks at (0,0,0)
and (32,0,0), dirties only the former, and saves again without advancing a tick.
`DisabledPlantRosterThenMultiRegionEditMatchesDevelHashAndExactSaveBytes` checks
every output file against the fixture: plant roster, `r.0.0.lmr`, and manifest,
with no `r.1.0.lmr`. Independent expected sessions contain the plant roster and
respectively two live chunks or just the dirty saved chunk, without executing the
save/load sequence under test. Their hashes are computed in the same build.
`hashes.json` retains the historical capture values; it is not a save member or a
portable hash oracle. Missing or changed fixture bytes still fail the tests.

### Ambient hash portability investigation

The historical world-hash literals encode the ambient fields' selected FastNoise
instruction set. Terrain uses `NewWorldNoise` with an AVX2 cap; wind, weather and
aether instead use automatic CPU dispatch. With identical GCC ASan objects,
changing only the diagnostic dispatch from native AVX-512 to AVX2 reproduces every
reported CI value:

| State | AVX-512 | AVX2 |
| --- | --- | --- |
| Seven ticks | `a088c112ee5c428f` | `95c493661166ef9a` |
| Plant edit, live | `f4caef7e08157d8b` | `8570c9cd10372b20` |
| Plant edit, loaded | `993b7f0b151dc6c3` | `f3b4b51bad5cbbf8` |

Only `wind`, `weather` and `aether` move. `chunk`, `scents`, `ecology` and `plants`
are identical. The ambient implementations, their headers and vendored FastNoise
are byte-identical to the fixture's devel commit. A standalone probe compiled from
that commit's ambient sources reproduces the same component hashes for both
instruction sets. This is a pre-existing cross-CPU determinism gap in ambient
fields, outside terrain's pinned dispatch, rather than an ASan-induced region
regression. An identical runner image does not guarantee identical CPU features.
The dispatch-only reproduction establishes the cause of these failures without
relying on uninitialized memory or pointer ordering.

The ambient runtime behavior is unchanged here; cross-CPU ambient hash equality
remains unresolved. These persistence tests now use expected sessions computed in
the same build, retain the exact devel save bytes, and print all seven actual and
expected component hashes on mismatch. Diagnostic sources, component values,
compiler commands and predecessor source identities accompany the campaign receipt
under `ci-investigation/`.

`RegionRecordingOverwrite` runs the shipping recorder/player with
`--test-streaming-radius-cap 1` on every invocation. This diagnostic option applies
the existing streaming cap before boot; zero is the default. It is not serialized
in LREC1, so capped diagnostic recordings must be replayed with the same option.
The ordinary `--radius 1` only bounds the initial horizon: subsequent streaming
previously expanded the test to 4,459 chunks, exceeding even its explicit
600-second CI timeout. The cap bounds the ongoing wanted set while retaining both
tick-30 checkpoint roundtrips, all 30 enabled schedule entries, disabled companion
removal and the failed-removal assertion. Each subprocess has a 45-second limit;
the test registration allows 240 seconds for all five subprocesses and cleanup.

The campaign receipt is a local build artifact at
`build/campaign-archives-20260907/slice-C2/receipt.json`, containing branch, head
commit, changed files, added tests, both ctest totals, before/after fixture hashes,
and deviations. Its absence means the slice has not supplied completion evidence;
malformed or unknown receipt fields do not authorize acceptance. It is not read
by the engine and does not change save or performance artifact schemas.

For an enabled saved session that has scheduled ticks, client player construction
also restores the ledger's walking feet before accepting another walking update.
This prevents a saved noclip camera/spawn position from replacing the restored
simulation anchor on the first frame. Initial player construction and all
feature-off calls retain the existing spawn convention. Replay divergence uses
the existing artifact's `divergence_section` value `region_schedule` when a
companion digest differs; this adds no field or off-path bytes. The heavy oracle
compares canonical ledger bytes as part of its enabled authoritative equality.

If shared Git metadata is read-only, the receipt retains the actual assigned
branch's `head_commit`, separately identifies `prepared_commit`, and records
that the branch was not advanced. In that case `slice-C2.bundle` and
`slice-C2.patch` beside the receipt carry the prepared commit (one parent at the
recorded base, with a plain imperative message). These are standard Git bundle
and format-patch artifacts, not engine inputs. Absence supplies no commit;
corruption or an unsupported bundle version is refused by Git's bundle verifier,
and a patch with invalid context is refused by Git's patch application. Neither
artifact grants permission to alter another checkout.
