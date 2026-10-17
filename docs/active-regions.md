# Active-region ledger and scheduler

The host ledger implements the persistent-active scheduling foundation in
[distant-world.md](distant-world.md). It is gated by the existing
`sim.active_regions` switch, enabled in compiled and packaged defaults since
v0.3.0. Explicit false selects the legacy clock path. Budgets remain unlimited in
shipped configuration. Enabled host sessions consume this schedule for field pages,
ground objects, weather, energy, foliage and resident water. Resident water uses the
checked [regional continuation contract](regional-water-continuation.md). Diagnostics
and measured budget activation remain separate qualification work.

Strict distant policy and distant creatures are planned for v0.3.1. Regional
continuation performance/default enablement is planned for v0.4.0 as
[issue #320](https://github.com/d-addison/luminumbra/issues/320). The dedicated
server autosaves every five minutes by default.

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
The telemetry v2 `events` timer ends after the ordered event drain, before
region scheduling. The server's physics timer ends before anchor submission.
Scheduler measurement remains part of the later diagnostics work; it does not
change the existing telemetry stage identities or units.

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

Idle retention probes ownership only for otherwise evictable records. The host
builds one set of pending work keys and all resident chunk region keys per tick,
including pristine and `Loading` chunks. These residents protect scheduling
history even when they contribute no frozen content. Consumed work stops owning
records on the next tick.

Invalid host input never throws out of the session. Non-finite or out-of-range
(outside -2^24 <= x,z < 2^24) walking anchors and ground-object edit positions
are ignored and counted (observational, never hashed or saved). Region pins outside
-32768 <= x,z < 32768 are ignored and counted with the same observational rules.
Invalid replicated anchors, work for regions the ledger does not know, and visit-time ledger exhaustion are skipped inside the scheduler
and counted in the schedule's `rejected` triple; the schedule digest appends that triple only when
it is non-zero, so valid input keeps its digest.
A scheduler exception, or an edit/pin that cannot be recorded because the ledger is full, is a
fail-stop fault: the failed schedule never mutates the ledger, the simulation clock is rewound to
the faulting tick (whose systems ran), pending region work is dropped, the validation ceiling is raised
to that tick, and every later tick returns zero. The server saves and exits with status 3, the
client saves and quits, and replay recording aborts without a schedule trace. A faulted session can
be saved and the save loads cleanly. A failed `load_active_regions` clears the world and restores the
previous ambient anchor.

`BoundedRegionResumeHook` defines an opt-in interface receiving region key,
freeze tick, resume tick and bounded elapsed ticks. `resume_window` clamps elapsed
ticks to both the system's explicit cap and one WorldClock calendar day; it
refuses a resume before freeze. No callback consumer or catch-up is installed.

## File and compatibility

The record is `<save>/chunks/region/active-regions.arl`, following the accepted
contract's placement in the region integrity scan. This resolves the slice
brief's conflicting save-root wording in favour of that contract. ARL1 does
not alter LMR1 v2, the world manifest, EFS1, plant payloads, FSD2 v3, or canonical
chunk serialization. Presets now use revision 7 with revision-6 omission
compatibility and frozen packaged-preset fallback. An anchor-only ledger is not a chunk
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
the save root; loading and later saves ignore it, and the next successful ledger
save reclaims it, so repeated interruptions cannot accumulate ledger copies until
the volume is exhausted. Reclamation is safe because saves are serialized per
destination by construction, so a staging file that already exists was abandoned
by a process that is no longer writing it. It is best effort: a file that cannot
be removed is left alone and never blocks the save. Unknown files inside the region
directory are still refused. The POSIX subprocess regression exits after the
staging file is flushed and closed, verifies recovery while that file remains, and
then verifies the next successful save removes it.

An accepted partial pair can contain an older ledger than its saved clock. The
ledger therefore carries two tick values. The canonical scheduler tick advances
only when the scheduler runs and is part of the hash projection. The validation
ceiling is the latest world time the ledger knows about, bounds every persisted
record stamp, and is observational bookkeeping excluded from the hash.

Session load raises only the ceiling to the restored clock. The canonical tick,
every region record, hold counter, pressure, simulation cursor, frozen digest,
configuration and anchor are preserved, and no scheduler tick or catch-up runs.
Recovery therefore cannot move the hash projection of a state nobody simulated.
Immediate save and edit stamps fit under the ceiling, scheduling refuses any tick
at or below it because those ticks have already elapsed, and the next actual host
tick uses the restored absolute clock. Saving does not raise the ceiling: a save
whose clock runs ahead of everything the ledger knows about stamps a last-save
value above the ceiling and fails its own decode check, which is how an
inconsistent metadata and ledger pair is refused rather than written. The raw persistence reader still exposes the
durable older header for validation and inspection. Recovery tests reload rewind
metadata failures, forward ledger failures and terminated ledger writes into
fresh sessions and save immediately, both unchanged and after an edit.

Absent means an empty ledger, with unlimited scheduling defaults and no saved
local anchor; the host supplies its spawn feet position for a legacy save. Its
ceiling starts at the restored WorldClock tick without executing a tick, so
an immediate edit or pin can be saved before simulation advances.
No old-world migration occurs. Corrupt, truncated, oversized, duplicate,
misordered or inconsistent records refuse with `Corrupt active-region ledger.`
A valid magic followed by a version greater than 2 refuses distinctly with
`Unsupported future active-region ledger version.` Version zero is corrupt.
A save containing the ledger requires `sim.active_regions`; opening or writing
with it off refuses as incompatible configuration. Older engine builds refuse
this file as an unknown region file. Decode failure never changes the destination
ledger. Unknown extension types cannot be skipped and are corrupt.
Version 1 ledgers load with or without frozen history. Their historical digest
values, freeze stamps and canonical bytes are preserved without recomputation.
Their legacy digest projection remains undeclared until every legacy historical digest has
been refreshed at a later freeze or its record evicted; waking alone does not
refresh a digest. This avoids declaring legacy digests as projection 2.
Flag-off legacy saves without a ledger retain their existing load, save and hash
behaviour, with no new files, bytes or hash terms.

All integers are little endian, signed integers use two's complement, floats
are IEEE binary32 (finite, with positive zero as the sole zero encoding).
The header is exactly 56 bytes:

| Offset | Bytes | Value |
|---|---:|---|
| 0 | 4 | ASCII ARL1 |
| 4 | 2 | version = 1 or 2 |
| 6 | 2 | reserved zero |
| 8 | 8 | canonical scheduler tick; advances only when scheduling runs |
| 16 | 8 | validation ceiling; observational restored clock bound |
| 24 | 8 | work limit; UINT64_MAX is unlimited |
| 32 | 4 | nonzero hold ticks |
| 36 | 1 | reduced cadence shift, 1 through 16 |
| 37 | 1 | local anchor present, 0 or 1 |
| 38 | 2 | reserved zero |
| 40 | 12 | local walking feet X, Y, Z; all zero if absent |
| 52 | 4 | record count, at most 1,048,576 |

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

Version 1 has no extensions. Version 2 uses the same header and records, then
a LE u32 `ext_len` followed by exactly that many extension bytes. Each extension
is a TLV: LE u16 type, LE u32 value length, then the value bytes. Types must be
strictly ascending, with no duplicates; unknown types, truncated values and
`ext_len == 0` are corrupt. Supported extension values are:

| Type | Value |
| --- | --- |
| 1 | Persistent allocator: one LE u64 next ID in [2, 2^63]; emitted when advanced from 1. |
| 2 | `digest_projection`: one byte equal to 2. |
| 3 | Water debt: LE u32 count, then sorted (LE i32 x, LE i32 z, LE u64 positive Q32 debt) entries, at most one per owned region. |
| 4 | Energy ages: LE u32 count, then sorted (LE i32 x, LE i32 z, LE u64 absolute tick) entries, at most one per owned region. |
| 5 | Simulation content identity: 40 bytes containing LE u16 encoding, defaults and algorithm versions, LE u16 zero reserved, then LE u64 policy, creature definitions, plant definitions and population digests. |
| 6 | Completed work: LE u32 count, then sorted (LE i32 x, LE i32 z, LE u64 positive work) entries, at most one per owned region. |

Map extensions are emitted only when nonempty.
For type 2, a value greater than 2 refuses
with `Unsupported frozen digest projection.`; a lower value is corrupt.
The digest declaration is emitted whenever a record is frozen
or retains a nonzero freeze stamp or frozen digest, including after waking,
provided no legacy historical digest remains. A version 1 reload conservatively
treats all historical digests as legacy, including any refreshed before saving.
The encoder emits version 2 whenever any supported extension is present; otherwise it emits version 1.
The digest declaration belongs only to the persistence envelope and adds no canonical
hash term.

The footer is one LE u64 FNV-1a-64 checksum over every preceding byte, including
the extension length and TLVs (offset basis 14695981039346656037, multiplier
1099511628211). Total length must be exactly 56 + count * 104 + 8 for version 1,
or 56 + count * 104 + 4 + ext_len + 8 for version 2. Files are capped at
159,383,717 bytes before allocation. No trailing bytes, padding, native structs,
section omission or alternate encodings are accepted. Stamps cannot exceed the
validation ceiling, counters cannot exceed the hold or both be nonzero, phases
must match the region key, and a frozen region's last
ticked stamp precedes its freeze stamp. The ledger tick cannot exceed the saved
WorldClock tick, and the canonical tick cannot exceed its validation ceiling.
Configuration is stored and hashed with the ledger rather than
read from an unrecorded machine-local budget.

Persistence reads and hashes each use a verified opened regular-file handle, refuse symlinks and non-regular files, enforce a format-specific byte bound during reading, and refuse growth or truncation.
Plant admission and both snapshot writer lanes support at most 65,536 plants, at most 8,192 bytes per canonical plant record, and 65,536 bytes of envelope margin, giving a read and write bound of 536,936,448 bytes.
Legacy energy accepts 1 through 64 channels and at most 67,108,864 decoded value bytes; the text bound is 406,847,552 bytes (12 bytes per uint16 value, 32 bytes per page, and 64 header bytes).
A missing legacy energy record is neutral; unreadable, oversized or invalid records fail loading with a distinct error and never silently erase authoritative state.
Region containers have a shared read and write bound of 268,435,456 bytes, checked before writer allocation.

Frozen content is supplied at the transition by merging authoritative lod-0
chunks by id, with live chunks taking precedence over parked chunks, then durable
region records carrying the sticky edited flag. Live and parked inputs must have
`LoadedOrEdited` provenance and must not be in `Loading` state; exclusions apply
before merging, so these inputs cannot shadow authoritative content. Durable
edited records contribute in every serialized lifecycle state, including
`Loading`. Durable pristine records are regenerable caches and never contribute.
Residency and save history cannot change the result. The provider uses a dedicated
`region_content_v2` projection in chunk-id order: identity,
SDF, heightmap, materials, integer water depth/bed/flux, water initialization,
resolution and solver sleep/threshold state. Lifecycle state/state_value,
collider flags, render meshes/bookkeeping, transient dirtiness, far render
records, compression and container envelopes are excluded. Legacy hashing and
serialization are unchanged. Later durable per-region systems must extend that provider
with a declared canonical projection before consuming the scheduler.

## Hash and replay

An empty ledger contributes no hash section. Otherwise ASCII
`active_regions:v1:` precedes the header bytes starting at offset 8 and ordered
record bytes, omitting the validation ceiling u64 at header offset 16 and each
last-save u64. The file magic, version envelope and footer are absent. This
contribution follows the existing clock bytes within
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

### Historical ambient hash portability investigation

The historical world-hash literals encode the ambient fields' selected FastNoise
instruction set. At the time of the C2 investigation, terrain used `NewWorldNoise`
with an AVX2 cap; wind, weather and aether used automatic CPU dispatch. With identical GCC ASan objects,
changing only the diagnostic dispatch from native AVX-512 to AVX2 reproduces every
reported CI value:

| State | AVX-512 | AVX2 |
| --- | --- | --- |
| Seven ticks | `a088c112ee5c428f` | `95c493661166ef9a` |
| Plant edit, live | `f4caef7e08157d8b` | `8570c9cd10372b20` |
| Plant edit, loaded | `993b7f0b151dc6c3` | `f3b4b51bad5cbbf8` |

Only `wind`, `weather` and `aether` move. `chunk`, `scents`, `ecology` and `plants`
were identical. The investigated ambient implementations, their headers and vendored FastNoise
were byte-identical to the fixture's devel commit. A standalone probe compiled from
that commit's ambient sources reproduces the same component hashes for both
instruction sets. This is a pre-existing cross-CPU determinism gap in ambient
fields, outside terrain's pinned dispatch, rather than an ASan-induced region
regression. An identical runner image does not guarantee identical CPU features.
The dispatch-only reproduction establishes the cause of these failures without
relying on uninitialized memory or pointer ordering.

That C2 change left ambient runtime behavior unchanged and did not resolve
cross-CPU ambient hash equality. These persistence tests use expected sessions computed in
the same build, retain the exact devel save bytes, and print all seven actual and
expected component hashes on mismatch. Diagnostic sources, component values,
compiler commands and predecessor source identities accompany the campaign receipt
under `ci-investigation/`.

The subsequent ambient dispatch repair
caps both FBm and Simplex nodes in all three ambient systems at terrain's x86
AVX2 ceiling, preserving lower-ISA fallback and native non-x86 selection. Its
Debug/Release controls and native qualification retain their own source identities;
the historical table above is not relabelled as post-repair evidence. Integrated
and native acceptance remain tracked by [issue #163](https://github.com/d-addison/luminumbra/issues/163).
The wind input-padding defect tracked by [issue #173](https://github.com/d-addison/luminumbra/issues/173) was fixed afterwards on devel (df8a6716, e9b122e5); the evidence above is unchanged and pre-dates that fix.

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

## Distant policy adoption and simulation content identity

With `sim.active_regions` enabled, create and load capture and validate both required species
inventories before constructing world systems. Each inventory must contain 1–256 JSON definitions;
each document is capped at 1 MiB and the captured inventories together at 16 MiB. Names are nonempty
UTF-8 strings of at most 256 bytes without NUL. Duplicate names or folded 16-bit IDs refuse within
each species domain, including the zero-to-one ID remap. Creature IDs use `CreatureSpeciesId16`
with its existing offset basis, folding and constants; plant IDs use `SpeciesId16`. Policy names
must resolve in the matching inventory. Species filenames and paths do not identify definitions.
Captured definitions and both registries are ordered by declared name using unsigned UTF-8 bytes,
so enumeration and biome selection remain stable across filename changes.

`data/common/sim/distant_policy.json` is optional. A missing leaf in an existing readable parent
directory selects the compiled table. A missing parent, status, permission, open, read and
change-during-read errors refuse; directories, symlinks and reparse points cannot substitute for
content files or parents. Present policy documents require UTF-8 without BOM, LF line endings and a final LF.
Species documents accept ordinary JSON whitespace. Raw NUL bytes refuse before parsing in both
domains, including after an otherwise valid document. Both use duplicate-key detection before
adoption, a nesting cap of 32, strings of at most 256 bytes, at most 256 entries in any collection,
and at most 65,536 parser callback events. Unknown keys, wrong types, narrowing and non-finite
simulation numbers refuse. Adoption is transactional and the session exposes only const content
and registries. Later file edits cannot change an open world's policy.

Biome tables, terrain presets, structure placement and piece documents, and species registry text
also refuse raw NUL before parsing. Biome and preset loads report an error without adopting a table
or preset; structure loads retain their existing per-file error handling. Malformed systems
configuration, including raw NUL, selects defaults; malformed operator budget profiles refuse.
Population configuration is caller-supplied identity framing, not a parsed document.

The policy root has exactly `schema`, `creature_species` and `plant_species`;
`schema` is `luminumbra.distant_policy.v1`. Every authored creature entry has exactly
`coarse_need_rate_ppm` (u32), `coarse_travel_m_per_cadence` (u32), `promote_distance_m` (u16),
`lifespan_cadences` (u32) and `reproduce_cadences` (u32). Every authored plant entry has exactly
`max_cadence_shift` (integer 1–8) and `resume_cap_ticks` (integer 0–36,000). Boolean, floating,
exponent, negative and negative-zero tokens cannot stand in for these unsigned integers.

Compiled defaults table version 1 resolves missing creature entries to need rate 0, travel 0,
promotion distance 512 m, lifespan limit 0 and reproduction limit 0. Zero need/travel rates do
nothing; zero lifecycle limits disable their mechanisms; explicit zero promotion disables
promotion. Plant defaults are maximum shift 8 and resume cap 36,000 ticks; explicit zero resume
cap discards catch-up. A coarse cadence means one accepted owner due step, rather than a separate
fixed tick interval. Algorithm version 1 seals this interpretation. Consumer enforcement of these
policies belongs to the corresponding simulation systems.

TLV 5 is always present in newly created enabled worlds, even with no regions or creatures.
Its value is exactly 40 bytes, without padding:

| Offset | Bytes | Value |
|---|---|---|
| 0 | 2 | LE u16 identity encoding version, 1 |
| 2 | 2 | LE u16 compiled defaults version, 1 |
| 4 | 2 | LE u16 simulation algorithm version, 1 |
| 6 | 2 | LE u16 reserved, zero |
| 8 | 8 | LE u64 resolved policy digest |
| 16 | 8 | LE u64 creature definitions digest |
| 24 | 8 | LE u64 plant definitions digest |
| 32 | 8 | LE u64 populator identity/configuration digest |

Each digest uses FNV-1a-64 over the canonical bytes below, with offset basis
14695981039346656037 and multiplier 1099511628211, wrapping modulo 2^64. Integer fields are
little endian; strings/byte sections are framed with a LE u32 byte length. Inventories are ordered
by unsigned UTF-8 name bytes. No filesystem order or path is hashed.

The resolved policy projection begins with the unframed ASCII tag `ds-effective-policy:v1`,
then u16 defaults version, u16 algorithm version, the five compiled creature fields in the order
listed above (4, 4, 2, 4, 4 bytes), and the two compiled plant fields (1, 4 bytes). Next comes u32
creature count and each resolved entry as framed name, u16 species ID and the five fields, followed
by u32 plant count and each resolved entry as framed name, u16 species ID and the two fields.
The compiled table is included even when all species have authored overrides. Missing, empty and
explicit-default policy documents therefore produce the same identity. Override-only canonical
JSON remains a separate parser round-trip representation.

Creature and plant definition projections begin with `ds-creature-definitions:v1` and
`ds-plant-definitions:v1` respectively, then u32 count and each definition's framed name,
u16 species ID and framed resolved simulation payload. Creature payloads contain u8 predator,
u8 nocturnal, u32 biome-name count with framed names in sorted order, then IEEE-754 u32 float bits
for brain weights in order wander, rest, sleep, hunt, flee, graze, flee logistic m, flee logistic c,
drink; then each core and sensory genome range as low/high float bits. Range order is move speed,
vigilance, hunger threshold, size scale, vision cosine half-FOV, vision range and hearing range.
Plant payloads contain u8 perennial, u32 lifespan ticks, then low/high float bits for each gene
in `PlantGene` enum order. Omitted fields are resolved through the registry's compiled defaults.
Display names, colours, discovery rarity, render archetypes, JSON key order and whitespace are
excluded. All definitions contribute, including species not mentioned by authored policy.

The population projection begins with `ds-population:v1`, then framed populator contract and
framed canonical configuration bytes. The current unregistered contract is `none:v1` with empty
configuration. `GameSession::SetSimulationPopulationIdentity` supplies these inputs before
create/load; admission requires a nonempty UTF-8 contract of at most 256 bytes and caps canonical
configuration at 1 MiB. This identity seam does not implement a population callback or adopt game
population files. Registered population content must be supplied before world adoption and
versioned when its behavior changes.

The ledger canonical projection appends `|content_identity:` and the exact identity record before
`|completed_work:`. This feeds the existing enabled ecology/world hash at boot, including an empty
creature world, without changing network or replay layouts. TLV 5 precedes existing TLV 6 in the
persistence envelope; completed-work encoding and next-decision behavior are unchanged. Identity
adoption is immutable; repeated identical adoption is allowed, changing it refuses.

Opening an enabled world without TLV 5 adopts the current resolved content, including historical
enabled or indexed state. Adoption must succeed; the next save writes TLV 5. A present identity
must match newly captured content exactly or open refuses with `Simulation content identity mismatch.`
Unsupported identity/default/algorithm versions, nonzero reserved fields, wrong lengths and duplicate
or misordered TLVs are corrupt. Load compares identity before adopting loaded ledger/consumer state;
a refusal prevents saving. A legacy flag-off world may opt in and adopt current content for the
first time. With the flag disabled, strict policy/species adoption is bypassed,
identity is absent, and the existing lenient species, save, hash and artifact paths remain unchanged.
