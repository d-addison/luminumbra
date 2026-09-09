# Distant world and distant simulation contract

Status: accepted contract, September 8, 2026, revised the same day after two
independent review rounds. Implementation is planned in bounded slices and
is **not** present in the tree at the time of writing; each section states the
current shipped behaviour so nothing here reads as implemented. Nothing in this
document waives the acceptance evidence that each slice must supply, and no
format change listed here exists until its slice lands.

## Scope

- The view radius is **16 km from the camera** and the world has **no fixed
  edge**: terrain streams procedurally in every direction. The addressable
  extent is set by the packed chunk identity (`Chunk.h`, ±2^20 chunks of 16 m,
  about ±16,777 km on each horizontal axis; coordinates are masked, never range
  checked). World positions are 32-bit floats. By owner decision (September 8,
  2026) v0.3 qualifies traversal and rendering within 32 km of the world origin,
  where positions keep sub-centimetre precision; the camera-relative
  (floating-origin) precision work that would qualify the full addressable extent
  is deliberately residual scope, not an open contract gap. Chunk identity,
  region identity, persistence and hashing are integer-addressed and unaffected.
- Distant caves are real: long interior sightlines from inside caverns and
  tunnels, coarse real interiors visible through cave mouths from outside, and
  openings, overhangs and arches kept as silhouette features to the full radius.
- Every terrain edit is carried into every coarse tier at that tier's sample
  spacing. Whether an edit smaller than the spacing appears at a tier depends on
  the sampling phase; it is neither guaranteed to vanish nor to appear, and a
  sampling-phase test matrix documents the behaviour per tier.
- The far representation is volumetric coarse signed-distance tiers. Raising the
  camera far plane over the existing heightfield far tiles does not satisfy
  this contract.
- Simulation over distance runs only in **persistent active regions** and only
  while the world host runs. There is no wall-clock progression while a world is
  shut down.
- Audio remains disabled for v0.3 and every qualification run launches with
  `--no-audio`; nothing in this contract depends on audio.

## Three explicit sets

Current behaviour: live chunk residency is the union of one adaptive disc per
anchor (at most 32 chunks, 512 m, reduced to 24 or 20 chunks under generation
pressure and further when several anchors share the budget,
`SHIELD_WorldSystem::streaming_radius_for_pressure`); far rendering is a
separate camera-relative set of pristine heightfield tiles to about 3 km with
authoritative volumetric overlays captured from resident chunks; simulation
systems tick every registry participant regardless of residency; wind, weather,
aether ambience, scent, soil and irrigation are fixed grids anchored on the
spawn point. There is no notion of an active region.

This contract separates three sets that may overlap but are never equated.

| Set | Meaning | Owner |
|---|---|---|
| Render visibility | What the client draws: the live chunk disc plus volumetric far tiers out to 16 km. Rendering never mutates or activates world state. | Client `FarLodSystem` and the render passes |
| Simulation activation | Which 512 m regions advance state: persistent active regions ranked and scheduled by the host. | Host `GameSession` through the active-region ledger |
| Storage residency | What is held in memory and on disk: chunk lattices in the live disc, regenerable far-tile caches, durable region records and the far authority overlay. | Persistence and streaming |

Camera position, noclip flight, disabled rendering and coarse rendering never
create or wake an active region. The host's simulation anchor for the local
player is the last walking feet position; noclip and debug-camera movement do
not move it, and it is persisted with the ledger and restored before the first
tick. A differential test proves that host activation, scheduling and
authoritative hashes are identical with the camera elsewhere, in noclip, or
with rendering disabled.

## Distant-world representation

### Tier ladder

Far terrain is a ladder of five nested, world-origin-aligned volumetric tiers.
Each tier stores sparse 5×5×5 signed-distance bricks and is polygonised per tile
with Marching Cubes; a brick record exists where the field crosses zero.

| Tier | Sample spacing | Brick edge | Tile edge | Drawn to (horizontal) |
|---|---|---|---|---|
| V1 | 4 m | 16 m | 512 m | 1,024 m |
| V2 | 8 m | 32 m | 1,024 m | 2,048 m |
| V3 | 16 m | 64 m | 2,048 m | 4,096 m |
| V4 | 32 m | 128 m | 4,096 m | 8,192 m |
| V5 | 64 m | 256 m | 8,192 m | 16,384 m |

Distances are horizontal (XZ) distances from the camera to the nearest point of
a tile. Spacing over outer radius is constant (1/256 rad), so screen-space
sample density is uniform across the ladder. Multi-tier residency and draw
ownership are new machinery with an explicit arrival-state rule: the coarser
tier is always resident and drawn beneath the finer one with a one-brick
overlap; a finer tile replaces the coarser surface inside its band only once it
is built and uploaded, and until then the coarser surface stays visible, so a
missing or late tile is never a hole; ownership at the live edge is decided
against the live chunks that are actually meshed and uploaded (including cave
ceilings and floors), replacing today's camera-region skip; the transition rule
(finer wins by depth bias; no stitching topology) must be proven on walls,
ceilings, arches and overhangs, not only on open ground. The live disc, its three live detail
levels and the bounded full-lattice cave neighbourhood are unchanged; long
interior sightlines come from V1 and V2 bricks, which sample the same cave
field.

One tier table is the authority for the volumetric ranges; adopting it in
runtime streaming and gates belongs to the later implementation slices.

Current behaviour: two pristine heightfield tiers (4 m to 768 m, 8 m to about
3,000 m) with a 3,200 m far plane; edits reach far tiles only through
authoritative SDF brick overlays captured from chunks that are still resident,
because the far store is never attached to a save directory at runtime; full
lattices exist only for chunks generated or retained at the full live detail
level, while chunks first generated at a coarser level carry heightmaps only.

### Far-range declaration and compatibility

[`FarTierTable.h`](../src/luminumbra_common/world/FarTierTable.h) declares the
ladder above as a header-only `constexpr` table in `Luminumbra::World`. New
volumetric far code reads this table instead of introducing local range
constants. For tier number `t` in 1 through 5, sample spacing is
`4 * 2^(t-1)` metres, brick edge is `4 * spacing`, tile edge is
`128 * spacing`, and horizontal outer radius is `256 * spacing`.

The source API exposes `kFarTierCount`, `kFarTierTable`, the one-based
`FarTierAt(t)` accessor, and `kFarOuterRadiusMeters`. Each `FarTierDimensions`
entry has integer metre fields `sample_spacing_meters`, `brick_edge_meters`,
`tile_edge_meters`, and `outer_radius_meters`. `FarTierForHorizontalDistance`
returns the one-based nominal band owner for an XZ camera-to-nearest-tile
distance: V1 owns `[0, 1024]`, then each tier owns
`(previous outer radius, own outer radius]`. Exact radii belong to the finer
tier, including V5's outer edge. Actual live coverage, arrival fallback and
one-brick overlap remain obligations of later slices.

This is a source API addition, with no serialized file, record, field, artifact
key or feature switch added to the engine. An absent header fails compilation;
invalid table dimensions fail its compile-time invariants. `FarTierAt` refuses
zero and unsupported or future tier numbers with `std::nullopt`, without
clamping or indexing out of bounds. The distance accessor returns
`std::nullopt` for negative, non-finite or beyond-horizon values. There is no
runtime table loader or format version to accept, migrate or reinterpret.
[`FarTierTable_test.cpp`](../test/common/FarTierTable_test.cpp) adds five cases
to `common_tests` covering the published values, nesting, angular density,
exact boundaries and invalid inputs; a missing source fails configuration or
build, and a broken contract fails its tests.

The legacy two-tier heightfield path remains pinned to F1 = 768 m,
F2 = 3,000 m, fragment clips = 176 m and 3,050 m, and camera far plane =
3,200 m. Removing the unreferenced `NEAR_FIELD_DISTANCE` and
`FAR_FIELD_DISTANCE` declarations changes no consumer. The frontier gate
continues to assert the reported missing-region count for the runtime wanted
set, without independently computing a coverage radius. Its existing
`luminumbra.farlod_horizon.v1` artifact still writes the historical
`thresholds.f2_outer_range_m` value 1,536; the gate never reads that field.
That value is retained solely to preserve artifact bytes and is not the
runtime horizon. No frontier capture or wider-radius qualification is claimed
by this declaration. Runtime constants, gate assertions, payloads, world
hashes and configuration remain unchanged.

The declaration's evidence receipt is
`build/campaign-archives-20260907/slice-B2/receipt.json`. It is evidence only,
never engine input. Its `schema_version` is 1; it records `branch`,
`head_commit`, `files_changed`, `tests_added`, `ctest`, `fixture_hash`,
`verification`, `runtime_constants`, `dead_constants`, `frontier_gate`,
`deviations` and `out_of_scope`. An absent receipt means missing evidence;
invalid JSON or missing required fields means corrupt evidence; an
unsupported schema version must be refused by an evidence reader. None of
these conditions changes world loading or supported bytes.

### Vertical coverage and the cave field

The cave field carves without a lower bound, so pristine coverage is bounded by
requests rather than by any preset or live-streaming property: a tile's
pristine span is initialised to its surface band plus a contract default of
256 m below its lowest surface height (a number chosen for this contract, not
a property of today's streaming), and extended, upward and downward, by every
anchor, camera and sightline depth request that reaches the tile and by any
durable chunk record above or below it. Requests are bounded per frame, the
encoded span bounds are stored in the tile record, and discovery walks only
the span, so a camera descending a deep cave extends the spans of the tiles
around it without any unbounded column scan, and deep interior sightlines
beyond the requested span appear as the request grows. Pristine bricks sample the same terrain and cave density
composition the live path uses. At spacings of 8 m or less (V1 and V2) the full
cave router is sampled unchanged. At 16 m and beyond, the noise-carved cave
terms alias when point-sampled, so two representations are qualified against
each other before the ladder is fixed: a band-limited field (noise caves to
4 km, analytic surface openings to 16 km) and a filtered field (noise caves at
every tier at higher tile cost). The design review chooses from measured
aliasing and cost on the cave-dense preset; the band-limited field is the
recorded fallback. Whatever is chosen, openings, overhangs and arches remain
silhouette features to the full radius, and the acceptance stations include
deep interiors and distant mouths beyond the noise-cave cutoff, not only within
it.

### Edits and the far authority overlay

Edits are baked into every tier on the world host inside the save path, after
dirty chunks are written and before far authority is notified. Bake
obligations are derived, never remembered: on every save and load, any column
whose committed chunk generation exceeds its overlay generation is due for a
bake, so an interrupted or failed bake is retried by the next save without
blocking the chunk write that preceded it, and a snapshot commits successfully
with bakes still due. A baked
authoritative brick record carries one of three states: present (sampled
signed-distance values), homogeneous air, or homogeneous solid; the two
homogeneous states are tombstones, so a brick that an edit emptied or filled is
never confused with an absent pristine brick. Absence of an overlay record means
"no authoritative override"; if an overlay record is absent for a column whose
lod-0 chunk records are marked edited, the tier is rebuilt from those durable
chunk records, never from the pristine field. Each bake writes the far authority
generation it was produced from; a client that finds an overlay older than the
durable chunk generation treats it as stale and rebuilds from the chunk records.
An interrupted bake leaves the previous complete generation in place. The far
authority overlay stores only edit-baked bricks; pristine bricks are a
regenerable in-memory cache.

### Depth, fog, shadows, horizon

- Depth uses reversed-Z with a 32-bit float attachment and a 17,408 m far plane.
  The flip lands first with the far plane held at 3,200 m so parity baselines
  attribute differences to the depth change alone. Frustum-culling planes and
  shadow coordinate extraction are audited for the changed clip convention as
  part of that slice.
- Aerial perspective extends to the ladder outer radius. The invariant is
  enforced on the composited result, including the altitude fog path: measured
  transmittance at the outer clip along any qualified sightline is at most 5%,
  with an explicit outer-distance fade if the analytic density alone cannot
  guarantee it.
- Shadow cascades still end at 250 m. Far tiles neither cast nor receive
  cascaded shadows; not receiving is implementation work (the lighting path
  needs a reliable far-surface discriminator), and the overlap between the
  cascade range and the V1 inner band is tested.
- There is no planetary curvature. The far water sheet is emitted per tile from
  column flags.

## Persistent active regions

Regions are the existing 512 m persistence regions. A region becomes
persistent-active when a replicated simulation anchor (a server avatar, or the
single-player host's player position) is inside its 512 m near disc, when any
voxel or ground-object edit touches it, or when the game pins it explicitly.

Each region carries flags (visited, edited, pinned, populated), tick stamps
(first active, last proximity, last edit, last ticked, frozen at, last save), a
state (active, reduced, frozen), a cadence shift, a water cursor, and the
content digest of its durable records at the time it froze. Regions inside any
anchor's near disc are always active at full cadence.

Scheduling is a deterministic function of host tick state only. Pressure is
measured in integer work units counted per tick (region ticks, entity updates,
water cells, page updates), never in wall-clock time; the limits are calibrated
from measured telemetry and recorded in the world's configuration identity.
Every transition-driving value (per-region pressure and hold counters, cadence
shift and phase, the last decision tick) is persisted in the ledger record and
hashed, so reduction and recovery reproduce after reload; observational
bookkeeping (last-save stamps, transaction generations, storage identities) is
persisted but excluded from the hash projection. Comparisons are strict
greater-than against the limit, work units are summed per tick before any
decision, at most one state step is applied per region per tick, transitions
are applied in the total region order, and counters reset when a region
changes state.
Regions are totally ordered by rank (pinned before edited before visited), then
by ticks since last proximity, then by region coordinates. When the counted work
exceeds the limit, the lowest-ordered regions first drop to a reduced cadence
(a power-of-two divisor with a per-region phase derived from the region key)
after a hold of N consecutive over-budget ticks, and recover only after N
consecutive under-budget ticks; when reduction is insufficient they freeze:
state is persisted with the freezing tick, nothing advances, and nothing
catches up on resume except a per-system opt-in bounded elapsed-time hook. A
frozen region thaws when it regains proximity, is edited, is pinned, or when
budget is available again in order. The per-tick schedule digest is part of the
replay trace, and a diagnostic panel shows the ledger, budgets and recent
transitions.

Budgets are measured on representative worlds before any cap is enforced.

Dirty state leaving the live disc is parked and written by the next save
instead of being discarded. Dirtiness is defined per record, not only by voxel
edits: modified water depths, fluxes, sleep and scheduling state, page bytes,
entity records and ledger entries each carry their own dirty flag. The host
autosaves on a tick interval short enough to cover a traversal capture, and
parked memory is bounded by a byte limit: when parking would exceed it the host
performs a blocking save first, and while a save is failing eviction of dirty
chunks stops (they stay resident) and the failure is reported; dirty state is
never dropped. Parked bytes, save latency and failures are shown in the
diagnostic panel and qualified on the historical 5,433-chunk edited save.

Current behaviour: eviction erases dirty chunks without a flush, the client
saves only on quit or shutdown, the server autosave interval defaults to zero,
and no region ledger exists.

## Per-system distant policy

| System | Near (live disc) | Distant active region | Frozen region |
|---|---|---|---|
| Terrain edits | Full lattice | Persisted chunk records plus far authority bricks | Unchanged on disk |
| Plants, soil, irrigation, living-world ladder | Every tick | Coarse cadence; each system's coarse rule is specified in its slice (rounding order, saturation, threshold crossings, environmental sampling, event multiplicity); integer-linear paths must equal k single ticks exactly, and every other path is qualified to at most one stage transition or one event of divergence per cadence period against the full-rate trajectory, with resume hooks capped at one calendar day | No advance; bounded resume hook |
| Wildlife | Full utility AI, physics avatars | Persisted; coarse needs, lifespan, reproduction and region-to-region travel at cadence; promoted to full AI on approach; travel into a frozen region parks the creature at the border until the region thaws | No advance |
| Water | Rotating cell window | Authoritative water steps on the host fixed tick (today the client host steps it per rendered frame), with eligibility from resident simulation arrays and the ledger, independent of render streaming. The cell budget is shared in indivisible units of one chunk window: shares are proportional to awake chunks, rounded down, with the remainder carried as persisted service debt; service is starvation-free in the bounded sense that every due region receives at least one chunk window within a number of its due ticks no greater than the total awake chunks divided by the per-tick window budget, a bound the diagnostic panel publishes; reduced cadences use power-of-two divisors with a common phase so neighbouring due ticks coincide, a border steps only on ticks when both regions are due (at the coarser cadence) with a paired reservation and paired accounting, and a border to a frozen region is sealed | Millimetre arrays kept, no flow |
| Wind, weather, aether ambience | Stateful world-anchored pages | A 24 m cell is owned by the region containing its centre; a page update is masked to owned cells whose region is due this tick, a partially active page seeds only its owned cells on first activation, and exchange across a cell boundary follows the same shared-boundary schedule as water (both owners due, at the coarser cadence, sealed toward frozen regions), so a frozen region's cells and digest never change; storms are owned by the region containing their centre, carry an absolute spawn tick and an active-age accumulator, advance and schedule strikes only while their region is due, and transfer ownership across a border only on a tick when the destination is due, otherwise waiting at the border; a page entering activation is seeded from seed, tick and position | Owned cells immutable; no catch-up (this deliberately replaces the energy field's catch-up rule) |
| Existing energy layer (`aether_state.efs`) | Unchanged | Under active regions its window policy is replaced by region activation with the same no-catch-up rule; its serializer never mutates state, the per-owner cadence and active-age metadata live in the ledger record (the EFS1 payload is unchanged), frozen pages hash as their stored bytes, and an all-zero layer writes no record while the snapshot index records the record as intentionally absent | Frozen pages kept |
| Scent and foraging | Spawn-anchored near facility; the scent grid is historical state and is persisted with the page records so near consumers resume continuously | Not simulated at distance; distant creatures do not use scent | — |
| Ground objects | Settle, persist, despawn | Despawn deadlines are active-age counters, advancing only while the owning region is due | Nothing advances |
| Scheduled game events | Not implemented | Deferred to a later milestone | — |

Authority is server-only on the deterministic fixed tick with per-region
seeded random streams; clients render coarse results only, and delivering
coarse state to remote clients is outside v0.3 (host-local evidence). Distant
water simulates only where its millimetre arrays are resident; a
simulation-only residency class is not part of this contract and would be a
separate decision.

Current behaviour: every creature and plant in the registry ticks every tick
regardless of distance; creatures are not persisted; wind, weather, aether,
scent, soil and irrigation are fixed grids anchored on the spawn point.

## Simulation clock and calendar

One persisted absolute tick base continues across save and load. Day length is
36,000 ticks (20 minutes at 30 Hz) and a year is 8 days for every consumer
below and for the rendered sky; the shorter render day remains only as a debug
override.

| Consumer today | Period today | Under this contract |
|---|---|---|
| Plant light (`GameSession.cpp`) | 36,000-tick day | Calendar day |
| Plant season (`GameSession.cpp`) | 288,000-tick year (8 days) | Calendar year |
| Circadian slot (`GameSession.cpp`) | 36,000-tick day | Calendar day |
| Migration (`GameSession.cpp`) | 108,000-tick year | Calendar year |
| Stimulus channels (`StimulusChannels.h`) | 7,200-tick day, 432,000-tick season | Calendar day and year |
| Rendered sky (`TimeOfDayModel.h`, `RenderPipeline.h`) | 1,800-tick day, 432,000-tick season | Calendar day and year |

One physical origin governs every consumer: tick zero is midnight of day zero
in midwinter, which is the origin the plant and circadian formulas use today;
the rendered sky, migration and stimulus channels adopt the same origin with
explicit per-consumer offsets, and a test asserts that sky, plant and
circadian phases agree at a set of pinned ticks.

The clock metadata (`simulationTick`, `calendar`) is validated strictly: absent
keys mean tick zero and the pinned defaults; `dayLengthTicks` must be an integer
in [1, 2^31), `daysPerYear` an integer in [1, 366], `simulationTick` an
unsigned integer below 2^62, and the products day × year and tick + catch-up
clamp are overflow-checked; anything else refuses the world as corrupt metadata.
Calendar phase origins equal today's formulas (phase = tick modulo period from
tick zero), so the plant season still starts at its cold minimum. Snapshots are
taken only on tick boundaries, never inside a batched client catch-up.

Current behaviour: the tick counter restarts at zero on every load, so a loaded
world resumes tick-zero weather, and the six durations above coexist.

## Persistence and format contract

Unchanged: preset revision 6, LMR1 container v2 for `chunks/region/*.lmr`
including its lod-level and flag validation, the
`luminumbra.persistence.world_manifest.v1` manifest, FSD2 far payload v3 (kept
decodable with its existing dimensions and validation; no longer written by the
runtime once the volumetric ladder lands), the `aether_state.efs` payload bytes
and placement, the plant record payload, canonical in-memory serialization, the
LREC1 replay record layout and checkpoint bytes, and the lockstep hash message
and protocol. There is no obsolete-world migration.

New records, each declared before its slice lands with exact version identity,
byte order, lengths, checksums, allocation limits and section-evolution rules
in the engine guide:

| Record | Placement | Absent | Corrupt, truncated or inconsistent | Newer than supported |
|---|---|---|---|---|
| `simulationTick`, `calendar` keys in `world_info.json` | existing file | tick 0 and pinned defaults | corrupt-metadata refusal | not applicable |
| `active-regions.arl` | `<save>/chunks/region/` | empty ledger | refuse | refuse as future format |
| `field-pages.fpg` | `<save>/chunks/region/` | pages seeded on activation | refuse | refuse as future format |
| `creature-entities.json` | `<save>/chunks/region/` | no creatures | refuse | refuse as future format |
| `ground-objects.json` | `<save>/chunks/region/` | no objects | refuse | refuse as future format |
| `far/v<tier>.<tx>.<tz>.lmr` (FSV1 payload, edit-baked bricks with tombstones and generation) | `<save>/far/` | regenerate or rebuild from edited chunk records | refuse | refuse as future format |
| `snapshot.json` commit index (members, content identities, generation, tick, anchors) | `<save>/chunks/region/` | legacy save without simulation records: load with defaults | refuse as inconsistent snapshot | refuse as future format |
| `aether_state.efs` (existing) | `<save>/` unchanged | none | refuse | refuse as future format |
| `plant-entities.json` (existing; persistent identities are recorded in a versioned sibling identity map, the plant payload bytes are unchanged) | `<save>/chunks/region/` unchanged | no plants | refuse | refuse as future format |
| scent grid section of `field-pages.fpg` (the near scent field is historical state; it is persisted so near creatures that consume it resume continuously) | `<save>/chunks/region/` | re-seeded empty | refuse | refuse as future format |

Persisted creature records inventory every evolution-relevant component and
reference (creature, genome, needs, mortality and decay, thirst, circadian,
alarm, pack, territory and bias, migration, scavenging, sensing, reproduction
cooldown and courtship, coarse tier and promotion state) and the slice proves
completeness with a projection test: after every system has run, a save and
reload must reproduce the ecology hash. Records under `chunks/region/` (the
directory returned by `WorldSaveService::region_directory`) join the integrity
scan. Engine builds
that predate a record refuse a save containing it as an unknown region file;
that refusal is accepted for the pre-release format and a fixture proves it.
The far authority overlay at the save root is derived render state: older
builds ignore it and it never contributes to a world hash. Entity records
declare record and component versions, required fields, duplicate-identity and
dangling-reference rules, numeric bounds and unknown-component refusal; the
persistent identity allocator and the species/content identity used by coarse
simulation are stored with the ledger and refused on mismatch.

A save is one committed snapshot published through the commit index. The
index enumerates every authoritative member the snapshot requires (chunk
region files, the energy record, plant, creature, ground-object, ledger and
page records, the clock metadata), each with its content identity and the
generation in which it last changed, records intentionally empty or deleted
members explicitly, and lists derived members (far overlay files) separately:
a missing or stale derived member is rebuilt from the authoritative records,
while a corrupt one is refused; unchanged members are reused, not rewritten.
Catalog inspection is read-only and reports a save whose journal is pending
recovery as such; recovery and snapshot selection happen when the world is
opened, before any member (including clock metadata) is read. Existing
payload bytes carry no transaction metadata; the index does. Publication is
recoverable through a journal carrying the transaction identity and the
generation it replaces: before any member is replaced its previous file is
copied into the journal with an undo record (created members get a delete
record, deleted members a restore record), every changed member is written to
a temporary path and renamed, the previous index is retained under its
generation, the new index is renamed last, and the journal is deleted only
after the new index is durable. On load, a journal whose transaction is not
the committed index is rolled back in reverse order before the index is read;
a journal whose transaction matches the committed index is stale and deleted.
Members whose content identity disagrees with the index are refused as an
inconsistent snapshot and a required member that is missing is refused. A save
is legacy only when it has no index and no new-format record; any new-format
record without an index is refused as inconsistent. A save with zero dirty chunks
still publishes the clock, ledger, pages and entity records that changed.
Interruption is tested after every rename and during the bake.

Everything in this section is gated by `sim.active_regions`: with the key off,
clock restoration, the calendar unification, identity-dependent random
streams, the chunk projection, the water host-tick change and every new hash
section are inactive, so the existing determinism fixtures (canonical debug
and release hashes, populated goldens, replay and lockstep gates) stay
byte-identical; a save written with the key on and opened with it off refuses
as incompatible configuration rather than silently degrading. With the key on, world hash composition
keeps its append-only order and folds new state into existing slots as tagged
canonical sections, each present only when its state is non-empty: clock, calendar, scheduler configuration and ledger state, region
random-stream state, persistent-identity allocator, creature and ground-object
records fold into the `ecology` slot; page bytes fold into the `wind`, `weather`
and `aether` slots. The host-owned authoritative chunk domain is the set of chunks with an edited
or simulated record (live, parked or durable) plus the simulation arrays of
regions that are due this tick; it is decided by the host tick, never by
render or storage arrivals, and pristine chunks are hash-neutral; each member contributes exactly one canonical simulation-only
projection (voxel lattice, materials and water millimetre state; never meshes,
storage envelopes or compression) with the precedence live over parked over
durable, and pristine space contributes through the seed and generation
parameters, so the hash is independent of residency and of the camera; frozen
regions contribute the digest stamped at freeze, computed from the same
projection. Identity, ordering and random-stream rules apply to plants,
creatures, avatars and ground objects alike through their persistent
identities. The replay checkpoint record keeps its existing fields; the new
sub-hashes are diagnostics outside the checkpoint. Replay determinism gates in
v0.3 start from fresh worlds; replaying a resumed snapshot needs a replay boot
reference to a snapshot and absolute tick, which is tracked separately. In v0.3
lockstep evidence is produced with identical builds and content; pre-tick
admission by calendar, configuration and content identity belongs to the v0.4
admission work. The existing periodic hash exchange is state-divergence
detection, not admission: qualified sessions must run with checkpoint
exchange enabled, and a mismatch ends the session at the first exchange after
it arises.

## Performance measurement contract

Two named profiles are qualified on the primary machine (RTX 5070 Ti) at native
output on both 3840×1600 and 3440×1440. Both profiles must meet the release
floor on every workload; the Performance profile exists to report the 120 fps
target.

| Profile | Render scale | Floor | Reported target |
|---|---|---|---|
| Quality | 1.0 | p99 frame time at most 16.67 ms | p50 against 8.33 ms |
| Performance | 0.67 | p99 frame time at most 16.67 ms | p50 and p99 against 8.33 ms |

Workloads are settled fixed views (400 warm-up, 240 measured frames) and 60 s
scripted traversals (surface flight, cave walk, edited-world tour) with pinned
seeds, preset and content revisions, camera paths and speeds, expected
simulation ticks and cold-cache state. Streaming transients are included; any
excluded window is listed explicitly with a reason. Every frame records wall
time from present to present, CPU submit, present, a whole-frame GPU timestamp
bracket and the per-pass sum (attribution only), each tagged with its
originating frame so late GPU results attach to the right sample and
outstanding queries are drained before the report; unavailable timers are
recorded as unavailable, never as zero. Reports carry p50, p95, p99, maximum and
median absolute deviation using the same percentile definition as
`tools/perf/perf.py`, record the GL vendor and renderer strings, and fail when
the adapter is not the qualified GPU. A capture is invalid, independently of
any threshold, when timer coverage is incomplete, an undeclared calendar or
render debug override is active, the workload manifest does not match, or the
far ladder did not settle within 90 s; invalid captures fail. The far horizon
gate requires complete per-tier coverage against an independently computed
expected wanted set at every qualified station within the settle limit,
below-horizon sky at most 2%, no seam band at any tier radius and no edge at
the outer radius, with fixtures for valid empty tiles and cave openings; a seam
or a missing tile is a failure, not a warning. The activation slice publishes a
requirement-to-evidence matrix naming the executable tests, native commands,
fixtures, thresholds and artifact schemas behind every statement in this
document, and its acceptance includes an executable oracle that a world saved
at tick N, loaded and advanced K ticks equals an uninterrupted N+K run over
every persisted subsystem, the clock, the scheduler and the identity allocator,
with negative controls per subsystem. The traversal workloads include an
identified edit, a successful snapshot commit and the corresponding all-tier
bake inside the measured window, with artifact timestamps proving inclusion,
and a construction above a tile's pristine maximum height is part of the
edited-world tour.

Current behaviour: the benchmark records means only, sums eleven pass timers
instead of bracketing the frame, pins the framebuffer to 3840×1600, and does not
record the renderer string; present-to-present wall timing already exists but
is not retained per frame.

## Evidence map

| Contract area | Executable evidence |
|---|---|
| Far ladder, caves, seams | `far_volume_horizon_smoke` scenario (v2 artifact) through `validate-engine-frontier.ps1 -Mode FarLodHorizon` on default, mountains, archipelago and the cave-dense fixture; `FarVolumeTile`, mesher and store gtests in the default ctest lane |
| Edits and far authority | `edit_bake_all_tiers_test`, `far_volume_store_test`, `world_open_refusal_test` cases, the `PersistenceRoundtrip` gate with the edited-world tour save |
| Snapshot commit and recovery | `snapshot_commit_test` (interruption after every rename and during the bake), catalog read-only inspection test, refusal matrix in `world_open_refusal_test` |
| Clock, ledger, pages, creatures, water | Unit gtests per record, `HeadlessServerTickHeavy` oracle extended to N-save-load-K versus N+K over every persisted subsystem with negative controls, `MovingResidency` 20 km drift, `PopulatedWorldReplay` |
| Hash and determinism | `validate-determinism-matrix.ps1` pins, `ecology_hash_test` additivity guards, `ReplayRoundtrip`, `LockstepLoopback` with checkpoint exchange enabled |
| Performance | Benchmark schema v3 artifacts for six fixed views and three traversals per profile per display from the native command set, checked by `tools/perf/render_contract.py` and `validate_render_capture.py` |
| Client hang | `hang_watchdog_test`, the reproduction matrix records and hang reports archived with the campaign evidence |

Names of new tests are binding once their slices land; the activation slice
fills in fixture identifiers, thresholds and artifact schemas per row.

## Deferred

Traversal beyond 32 km from the origin (origin rebasing), views beyond 16 km,
far shadows and terrain occlusion beyond the cascade range, depth handling on
other rendering backends, hardware tiers beyond the primary machine, delivery
of coarse state and the far authority overlay to remote clients, scent and
foraging at distance, a simulation-only residency class, scheduled game events
and Workshop content distribution are tracked separately and are not part of
this contract.
