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
the host tick boundary; avatar persistence remains outside this slice.

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
Every state change resets both counters. Reduced cadence is 2^shift, with
shift in [1, 16]. Phase is the low `shift` bits of FNV-1a-64 over LE i32 X and Z.
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
FSD2 v3, or canonical chunk serialization. Snapshot transaction publication is
separate work; this slice uses the existing durable temporary-write and atomic
replacement primitive for the ledger, without claiming cross-file atomicity.

Absent means an empty ledger, with unlimited scheduling defaults and no saved
local anchor; the host supplies its spawn feet position for a legacy save.
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
| 8 | 8 | last completed scheduler absolute tick |
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
| 96 | 8 | u64 frozen durable-record content digest |

The footer is one LE u64 FNV-1a-64 checksum over every preceding byte (offset
basis 14695981039346656037, multiplier 1099511628211). Total length must be exactly
48 + count * 104 + 8, capped at 109,051,960 bytes before allocation. No trailing
bytes, padding, native structs, section omission or alternate encodings are
accepted. Stamps cannot exceed the header tick, counters cannot exceed the hold
or both be nonzero, phases must match the region key, and a frozen region's last
ticked stamp precedes its freeze stamp. The ledger tick cannot exceed the saved
WorldClock tick. Configuration is stored and hashed with the ledger rather than
read from an unrecorded machine-local budget.

Frozen content is supplied by the durable-record owner at the transition. The
current persistence provider hashes canonical lod-0 record payloads in id order,
including their id and flags, excluding far render records, compression and
container envelopes. Later durable per-region systems must extend that provider
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
The sidecar is diagnostic replay evidence, never a save member or world-hash
input. Recording and playback emit and verify one digest after each host tick;
LREC1 payloads and checkpoint fields are unchanged.

## Verification fixtures

`test/fixtures/active-regions/devel-off/r.0.0.lmr` and its adjacent
`world-manifest.json` are exact bytes captured from origin/devel at
f2895388cd14fda8591ed571f87c0ffe026db15d. The fixture is a default-off seed-1337
session after seven ticks, with one adopted idle chunk at (0,0,0), height samples
8, 8.5 and 9, and a dirty voxel flag. Its composed world hash is
`a088c112ee5c428f`. The metadata byte-layout oracle remains
`DefaultOffKeepsLegacyMetadataBytesAndTickZeroLoad`; creation timestamps are
observations rather than pinned fixture inputs. Missing or changed fixture bytes
fail the exact-byte test; these fixtures are never installed as runtime data.
The unchanged LMR1/manifest validators supply their corruption and future-version
refusals.

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
