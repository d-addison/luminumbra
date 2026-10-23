# Active-region ledger and scheduler

The host ledger implements the persistent-active scheduling foundation in
[distant-world.md](distant-world.md). It is gated by the existing
`sim.active_regions` switch, enabled in compiled and packaged defaults for
v0.3.0. Explicit false selects the legacy clock path. Budgets remain unlimited in
shipped configuration. No distant system consumes this schedule yet; field pages, creature records, water shares,
diagnostics and measured budget activation remain separate work.

Durable completed work and ground-object continuation, durable regional
fields/weather/scent/energy, regional foliage/water
continuation (issue #242), and strict distant policy and distant creatures (S8)
are planned for v0.3.1. Regional continuation performance/default enablement is planned for v0.4.0
as [issue #320](https://github.com/d-addison/luminumbra/issues/320). These consumers
are outside the shipping scheduler foundation. The dedicated server autosaves
every five minutes; this does not implement those continuation consumers.

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
region scheduling. The server publishes workload anchors and prepares collision
before advancing physics; its physics timer includes that preparation.
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
builds one set of current-tick work keys, lazily when a probe is needed. Anchor
visits refresh `last_proximity` before retention, using the replicated simulation
anchors or the saved walking anchor when no replicated list is supplied. An
explicitly empty replicated list has no fallback. Unpinned, unedited, unpopulated
records with no water cursor or current work are evicted after 108,000 ticks
without proximity. Camera position, render distance, chunk lifecycle and streamed
residency cannot retain scheduling history. Consumed work stops owning records
on the next tick. Later consumers must extend the probe using simulation ownership.

Invalid host input never throws out of the session. Non-finite or out-of-range
(outside -2^24 <= x,z < 2^24) walking anchors, ground-object edit positions and region pins are
ignored and counted (observational, never hashed or saved). Invalid replicated anchors, work for
regions the ledger does not know, and visit-time ledger exhaustion are skipped inside the scheduler
and counted in the schedule's `rejected` triple; the schedule digest appends that triple only when
it is non-zero, so valid input keeps its digest.
A scheduler exception, or an edit/pin that cannot be recorded because the ledger is full, is a
fail-stop fault: the failed schedule never mutates the ledger, the simulation clock is rewound to
the faulting tick (whose systems ran), pending region work is dropped, the validation ceiling is raised
to that tick, and every later tick returns zero. The server saves and exits with status 3, the
client saves and quits, and replay recording aborts without a schedule trace. A faulted session can
be saved and the save loads cleanly. A failed `load_active_regions` clears the world and restores the
previous ambient anchor.
Fallible collision/authority admission runs before committing the next world-clock
tick. If it fails, no systems for that tick run, only completed ticks are counted,
and both clocks and the fault retain the last completed tick. A later scheduler
failure still retains its faulting tick because that tick's systems already ran.

`BoundedRegionResumeHook` defines an opt-in interface receiving region key,
freeze tick, resume tick and bounded elapsed ticks. `resume_window` clamps elapsed
ticks to both the system's explicit cap and one WorldClock calendar day; it
refuses a resume before freeze. No callback consumer or catch-up is installed.

## File and compatibility

Live revision-4 captures bind the raw persisted water cursor as u64 little endian to both the water projection and chunk authority outside chunk caches in both active-region flag modes while terrain, mesh and frozen region digests retain their existing bytes.

Explicit legacy revision-3 identity and projection controls retain their original bytes, with incompatible current playback and peer admission refused before ticks without a legacy fallback.

The record is `<save>/chunks/region/active-regions.arl`, following the accepted
contract's placement in the region integrity scan. This resolves the slice
brief's conflicting save-root wording in favour of that contract. ARL1 does
not alter LMR1 v2, the world manifest, EFS1, plant payloads, or FSD2 v3;
active-region chunk snapshots retain explicit terrain and water ownership in optional
`authority` metadata, while legacy snapshot bytes remain unchanged. Presets now use revision 8 with revision-6 omission
compatibility and frozen packaged-preset fallback. An anchor-only ledger is not a chunk
snapshot: with active regions enabled, the first save with dirty chunks writes all
resident chunks, including clean chunks in other regions. The disabled path keeps
the legacy existing-save query: a plant-only save makes later saves incremental.
One synchronous save operation validates the existing durable snapshot once, then
writes chunk, plant and energy payloads before metadata and ledger acknowledgement.
Standalone writers retain full validation. A failed payload prevents later metadata
publication in that operation. Snapshot transaction publication is separate work; this slice uses the existing durable temporary-write and atomic
replacement primitive for the ledger, without claiming cross-file atomicity.

Clock/ledger pair saves are synchronous and serialize with all persistence writers in
the process. The client calls them during world creation, its main-thread pause-menu
quit callback, and shutdown after the main loop returns. Its explicit persistence
scenario also runs on that thread. The server calls them synchronously from
`RunFixedTicks` autosave, `SaveFullSnapshot` between runs, and `Shutdown` after the tick
loop. Job workers do not call the pair writer. Public persistence APIs permit
overlapping calls with stable caller-owned inputs. A process-wide recursive mutex guards
region read/merge/write, metadata pairs, shared durable writes and empty-record cleanup.
It serializes writers through reclamation and replacement; nested calls retain ownership
until the outer call returns. Independently launched processes sharing one save
directory are outside this ownership contract.

Incoming pairs are validated before either replacement. Forward saves replace metadata
first; rewinds replace the ledger first, preserving ledger tick <= clock tick at every
boundary. Ledger bytes are staged as unique
`<save>/active-regions.arl.tmp.<pid>.<sequence>` files, outside the strict `chunks/` and
`chunks/region/` scans on the save filesystem, then atomically renamed into place. A
separately mounted region directory causes replacement to fail across devices; no copy
fallback compromises atomicity. Termination can leave staging debris in the save root;
loading ignores it, and every later ledger save, including an empty-ledger save,
attempts to reclaim it. Reclamation holds the shared mutex through cleanup and
replacement, so existing staging cannot belong to another active in-process writer. It
is best effort: a file that cannot be removed is left alone and never blocks the save.
Unknown files inside the region directory are still refused. The POSIX subprocess
regression exits after the staging file is flushed and closed, verifies recovery while
that file remains, and then verifies the next successful save removes it.

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
They retain the version 1 envelope until every legacy historical digest has
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
`ext_len == 0` are corrupt. The currently supported extension is type 2,
`digest_projection`, with a one-byte value of 2. A value greater than 2 refuses
with `Unsupported frozen digest projection.`; a lower value is corrupt.
The encoder emits version 2 with this declaration whenever a record is frozen
or retains a nonzero freeze stamp or frozen digest, including after waking,
provided no legacy historical digest remains. A version 1 reload conservatively
treats all historical digests as legacy, including any refreshed before saving.
Otherwise the extension set is empty and the encoder emits version 1.
The declaration belongs only to the persistence envelope and adds no canonical
hash term.

The footer is one LE u64 FNV-1a-64 checksum over every preceding byte, including
the extension length and TLVs (offset basis 14695981039346656037, multiplier
1099511628211). Total length must be exactly 56 + count * 104 + 8 for version 1,
or 56 + count * 104 + 4 + ext_len + 8 for version 2. Files are capped at
109,051,979 bytes before allocation. No trailing bytes, padding, native structs,
section omission or alternate encodings are accepted. Stamps cannot exceed the
validation ceiling, counters cannot exceed the hold or both be nonzero, phases
must match the region key, and a frozen region's last
ticked stamp precedes its freeze stamp. The ledger tick cannot exceed the saved
WorldClock tick, and the canonical tick cannot exceed its validation ceiling.
Configuration is stored and hashed with the ledger rather than
read from an unrecorded machine-local budget.

Frozen content is supplied at the transition by merging authoritative chunks
by id, with complete resident authority taking precedence over durable
region records carrying the sticky authority flag. With active regions enabled,
render eviction atomically persists a complete chunk with edited terrain or
simulated water authority through
the existing LMR1 durable replacement path before releasing it. This includes
clean chunks whose dirty flags were acknowledged by an earlier save: later
water state must also be durable. Optional `water_authority` snapshot metadata
records generated or edited terrain provenance, and unknown values refuse loading.
Explicit `authority` metadata also distinguishes regenerable initialized water from
simulation owners, preserves terrain-only ownership, and rejects malformed or
contradictory ownership fields. Loaded and rehydrated records start clean.
Eviction reuses an unchanged durable owner only while its identity, array revision
and water bookkeeping match the saved stamp, with at most one stamp per resident chunk. A failed replacement keeps the complete
in-memory owner and latches the session's existing fail-stop fault at the next
tick boundary. Both streaming generation and synchronous surface loading restore
that durable authority before considering generation. Transient replay and
headless worlds use private temporary authority storage, removed at teardown.

Before horizon or edit admission, initialized authoritative water at nonzero Y migrates to the Y=0 column owner with its complete integer depth, bed, flux and solver bookkeeping.
Live same-id authority takes precedence over durable records; different Y layers have no historical precedence, so equal simulation states coalesce without summing water and conflicting or incomplete states refuse admission.
The destination and cleared source water records commit in one atomic region replacement before live ownership changes; source terrain authority remains independent.

Water initialization, depth, flux, wake/sleep state and threshold counters
advance once per fixed session tick. Natural water is admitted in a fixed 3x3
surface-column horizon around each simulation anchor, with one Y=0 water owner per column
and a separate terrain-containing chunk sampled at the column centre. The terrain chunk
provides collision independently of the water owner, including when submerged.
This one-chunk ring is simulation policy, independent of
camera, render radius, LOD and streaming completion. The set also includes all edited
authority in regions intersecting the ledger's 512 metre activation disc, plus the Y=0
owners of explicit ECS water sources in those regions. Every member is
admitted synchronously, restoring durable authority before considering canonical
full generation. Newly simulated water owners become durable authority; while
their region remains active they follow the same rule as edited chunks.

Replicated anchors take precedence over the saved walking anchor, including an
explicitly empty list. Natural-water initialization is a pure function of seed,
coordinates, generation and the fixed simulation resolution. Pending edited-water
initialization reuses the authoritative heightmap when its samples match the
simulation cell centres, and otherwise samples canonical terrain. Pristine Y=0 chunks
streamed outside the simulation set receive that same generated initial water state for
rendering, remain frozen, and never contribute to simulation hashes or freeze digests.
Both synchronous horizon meshing and asynchronous meshing render their local
water grids. Departing authority freezes without catch-up; returning authority
resumes from the exact durable integer water state. No distant water continuation
is introduced. Pending edited-water initialization and resolution changes select
chunk ids in order. Snapshot loads preserve the simulation-window cursor and
pending resizes, and discard derived activity scores.

Pristine terrain leaves the simulation set when it leaves the required horizon, even while its region remains active, so reload restores the same participants and colliders.

Before creature or server-avatar physics and client entry readiness checks, the
owner thread admits the simulation horizon around every advancing creature body
as well as each simulation anchor, and installs terrain colliders synchronously for chunks with
complete canonical heightmaps, in chunk-id order. Partial heightmaps do not install
terrain colliders. Active-mode rendering never allocates terrain bodies,
so body allocation and contacts follow only the simulation schedule. Admission
replaces stale render colliders immediately;
render remeshing cannot invalidate simulation collision readiness. Before physics,
the physics registry also removes every terrain body outside the deterministic
simulation set, even if its renderer chunk has already been evicted. Render-only
colliders therefore cannot affect authoritative creatures beyond their admitted horizons. Water
still advances in its existing fixed-tick slot after weather. The zero-avatar server
runner publishes its spawn, moving or wet-walk workload anchor before physics and
simulation; that anchor is a pure function of the workload tick.

When active regions are disabled, water keeps the historical first-six pending
initializations in map order and the rotating cursor over sorted pending resizes.
Sorted initialization/admission and first-pending resize selection apply only to
the deterministic authority path. The legacy sleep bookkeeping is preserved too.

Simulation and rendering share the existing 8,192 unique resident-chunk budget;
there is no unbounded parking map. Simulation ownership can displace a render
cache, persisting authoritative content first. Rehydration decodes at most one
additional chunk at a time, plus the existing bounded streaming job pipeline.
Container replacement keeps only the bounded LMR1 manifest and streams one
payload at a time; Windows and POSIX use the same flush/close/replace contract.
All LMR1 readers, including far-LOD workers, hold immutable opened file images.
Windows handles share deletion; POSIX descriptors retain the old inode. A reader
can finish its old image while atomic replacement publishes the new one. Extents
are measured from that opened image, never from a potentially replaced path.
Freeze digests also decode one durable chunk at a time and incrementally hash
the unchanged canonical JSON bytes. With active regions enabled, explicit snapshot loads retain at most the
resident budget while validating every record; overflow remains in the original
durable records for restoration. With active regions disabled, the legacy load
adopts every record without a cap. Required synchronous horizon loading protects
the entire requested horizon and displaces non-required, non-simulation chunks
in increasing chunk-id order through persist-before-release; changing the save destination
transfers evicted authority before installing that destination. Before any copy,
both metadata and full snapshot saves refuse destinations containing edited
chunk ids absent from the current durable authority. This includes extra siblings
in the same region file and entire destination-only regions; a save cannot import
another snapshot's edits into the live hash or later restoration.
Eviction candidates are scanned and sorted only when a missing admission needs space at the 8,192-chunk resident cap, with one prepared list reused within the admission batch.
Exhausting the budget with simulation authority is a fail-stop capacity fault.
The water activity cache is pruned to the same simulation set. Simulation-owned
chunks remain resident until their simulation ownership ends; camera movement
cannot choose which chunks receive a water tick. Render promotion cannot replace
authoritative SDF/heightmap contents with newly generated terrain; an edited
heightfield without full SDF keeps coarse rendering. Loaded and newly edited chunks
use the same rule.

The server's explicit full snapshot uses the session save operation to persist
clean resident chunks with their ownership, together with plants and energy,
before metadata and the ledger. A failed payload prevents metadata acknowledgement.

The server's three production hash captures and the client's lockstep chunk
capture use a shared authority oracle with active regions enabled. It merges
complete resident authority over durable terrain and water authority using
per-chunk terrain and water contributions. A cold scan decodes one durable chunk
at a time and caches only its two integer contributions; later captures replace
contributions from the bounded resident set without reading durable records.
Each capture compares a fixed-size key for water initialization, resolution, sleep,
quiet-tick count and maximum delta as well as the array revision, so bookkeeping
changes cannot reuse stale contributions.
Unsigned modular sums and the chunk count compose the same result as a full
recompute, with chunk identity included in every contribution. Render caches,
meshes and lifecycle flags cannot affect this oracle. The server availability
digest likewise uses sorted fixed-tick simulation participants in this mode.

The negotiated system configuration identity includes authority-rules revision 4
in its v4 encoding, so earlier executables are refused before simulation rather
than reaching a hash mismatch. This does not change the Hello protocol version.

Legacy active-region primitives retain explicit `simulation_authority_v4`, `simulation_terrain_v4` and `simulation_water_v4` scope prefixes beneath the revision-4 scheduler envelope. The water scope includes chunk identity,
integer bed/depth/flux, initialization, resolution, sleep and threshold fields.
Explicit legacy snapshot projections remain byte-exact with active regions disabled, while current live captures add the scheduler envelope. LMR1 stores
complete content, not these oracle values, and retains its container format;
water ownership uses optional chunk metadata with legacy omission compatibility. Replay headers/checkpoints do store oracle values;
older authority-profile captures remain parseable and are refused by configuration identity before current playback can advance a tick. Captures made
under the new scopes must be compared with captures under those same scopes.

Plants, creatures and living-world consumers run over authoritative ECS
participants on fixed session ticks, independently of the streamed chunk map.
Procedural vegetation is a rendering derivative, and ground-object continuation
is not installed. None contributes a streamed-only per-chunk field to the
content projection.

The scheduler captures only the requested region's complete resident authority,
so renderer residency cannot remove edits or expose a stale durable copy. This
content contributes even in `Loading` state during a render rebuild. Generic
incomplete live/parked inputs still exclude `Loading` chunks. All inputs exclude
generated caches before merging. Durable edited records contribute in every
serialized lifecycle state, including `Loading`. Durable pristine records are
regenerable caches and never contribute.
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
