# Distant world and distant simulation contract

Status: accepted contract, September 8, 2026, revised the same day after the
first independent review round. Implementation is planned in bounded slices and
is **not** present in the tree at the time of writing; each section states the
current shipped behaviour so nothing here reads as implemented. Nothing in this
document waives the acceptance evidence that each slice must supply, and no
format change listed here exists until its slice lands.

## Scope

- The view radius is **16 km from the camera** and the world has **no fixed
  edge**: terrain streams procedurally in every direction. The addressable
  extent is set by the packed chunk identity (`Chunk.h`, ±2^20 chunks of 16 m,
  about ±16,777 km on each horizontal axis; coordinates are masked, never range
  checked). World positions are 32-bit floats, so v0.3 qualifies traversal and
  rendering within 32 km of the world origin, where positions keep sub-centimetre
  precision; travel beyond that radius is not qualified and the origin-precision
  work it needs is tracked as residual scope.
- Distant caves are real: long interior sightlines from inside caverns and
  tunnels, coarse real interiors visible through cave mouths from outside, and
  openings, overhangs and arches kept as silhouette features to the full radius.
- Every terrain edit is carried into every coarse tier at that tier's sample
  spacing. Whether an edit smaller than the spacing appears at a tier depends on
  the sampling phase; it is neither guaranteed to vanish nor to appear, and a
  sampling-phase test matrix documents the behaviour per tier.
- The far representation is volumetric coarse signed-distance tiers. Raising the
  camera far plane over the existing surface-only far tiles does not satisfy
  this contract.
- Simulation over distance runs only in **persistent active regions** and only
  while the world host runs. There is no wall-clock progression while a world is
  shut down.
- Audio remains disabled for v0.3 and every qualification run launches with
  `--no-audio`; nothing in this contract depends on audio.

## Three explicit sets

Current behaviour: live chunk residency follows a streaming disc of at most 32
chunks (512 m) that adapts down to 24 or 20 chunks under generation pressure
(`SHIELD_WorldSystem::streaming_radius_for_pressure`); far rendering is a
separate camera-relative set of heightfield tiles to about 3 km; simulation
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
create or wake an active region; a differential test proves that host
activation, scheduling and authoritative hashes are identical with the camera
elsewhere or rendering disabled.

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
ownership are new machinery: each tier is resident over its whole disc and drawn
only inside its band through the radial clip band, the coarser tier is always
resident and drawn beneath the finer one with a one-brick overlap, and the
cross-tier transition rule (finer wins by depth bias; no stitching topology)
must be proven on walls, ceilings, arches and overhangs, not only on open
ground. The V1 inner clip follows the live disc's actual radius, so the adaptive
shrink under pressure never opens a gap. The live disc, its three live detail
levels and the bounded full-lattice cave neighbourhood are unchanged; long
interior sightlines come from V1 and V2 bricks, which sample the same cave
field.

One tier table replaces the separate far-range constants and the differing
1,536 m and 3,000 m horizon figures in code, comments and gates.

Current behaviour: two pristine heightfield tiers (4 m to 768 m, 8 m to about
3,000 m) with a 3,200 m far plane; edits reach far tiles only through
authoritative SDF brick overlays captured from chunks that are still resident,
because the far store is never attached to a save directory at runtime; full
lattices exist only for chunks generated or retained at the full live detail
level, while chunks first generated at a coarser level carry heightmaps only.

### Vertical coverage and the cave field

Bricks are discovered over the complete column span a tile can contain: from
the deepest depth the preset's cave field can carve below the lowest surface
height in the tile to the highest surface height, with the span recorded in the
tile record so deep camera positions and long sightlines never depend on an
unbounded scan. Pristine bricks sample the same terrain and cave density
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
dirty chunks are written and before far authority is notified. A baked
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
parked memory is bounded by that interval and reported in the diagnostic panel.

Current behaviour: eviction erases dirty chunks without a flush, the client
saves only on quit or shutdown, the server autosave interval defaults to zero,
and no region ledger exists.

## Per-system distant policy

| System | Near (live disc) | Distant active region | Frozen region |
|---|---|---|---|
| Terrain edits | Full lattice | Persisted chunk records plus far authority bricks | Unchanged on disk |
| Plants, soil, irrigation, living-world ladder | Every tick | Coarse cadence; each system's coarse rule is specified in its slice (rounding order, saturation, threshold crossings, event multiplicity) and either proven equal to k single ticks or given a documented tolerance band | No advance; bounded resume hook |
| Wildlife | Full utility AI, physics avatars | Persisted; coarse needs, lifespan, reproduction and region-to-region travel at cadence; promoted to full AI on approach; travel into a frozen region parks the creature at the border until the region thaws | No advance |
| Water | Rotating cell window | Deterministic integer share of the cell budget per region with remainder carried by rank order and a starvation guarantee; flux crosses a border only between two regions that are both due this tick, with border flux accounted on both sides | Millimetre arrays kept, no flow |
| Wind, weather, aether ambience | Stateful world-anchored pages | Pages tick only in due regions; a page is ranked by the region containing its centre and ticks if any intersecting region is due; storms carry an absolute spawn tick and an active-age accumulator, so lifetimes and strike deadlines advance only while active; a page entering activation is seeded from seed, tick and position | Page bytes kept; no catch-up (this deliberately replaces the energy field's catch-up rule) |
| Existing energy layer (`aether_state.efs`) | Unchanged | Under active regions its window policy is replaced by region activation with the same no-catch-up rule; file placement and bytes unchanged | Frozen pages kept |
| Scent and foraging | Spawn-anchored near facility, unchanged | Not simulated at distance; distant creatures do not use scent | — |
| Ground objects | Settle, persist, despawn | Despawn when due | Despawn deferred |
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

The clock metadata (`simulationTick`, `calendar`) is validated strictly: absent
keys mean tick zero and the pinned defaults; a non-integer, negative or zero day
length, a year of zero days, or a tick that is not an unsigned integer refuses
the world as corrupt metadata.

Current behaviour: the tick counter restarts at zero on every load, so a loaded
world resumes tick-zero weather, and the six durations above coexist.

## Persistence and format contract

Unchanged: preset revision 6, LMR1 container v2 for `chunks/region/*.lmr`
including its lod-level and flag validation, the
`luminumbra.persistence.world_manifest.v1` manifest, FSD2 far payload v3 (kept
decodable with its existing dimensions and validation; no longer written by the
runtime once the volumetric ladder lands), `aether_state.efs` placement and
bytes, canonical in-memory serialization, the LREC1 replay record layout and
checkpoint bytes, and the lockstep hash message and protocol. There is no
obsolete-world migration.

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

Records under `chunks/region/` (the directory returned by
`WorldSaveService::region_directory`) join the integrity scan. Engine builds
that predate a record refuse a save containing it as an unknown region file;
that refusal is accepted for the pre-release format and a fixture proves it.
The far authority overlay at the save root is derived render state: older
builds ignore it and it never contributes to a world hash. Entity records
declare record and component versions, required fields, duplicate-identity and
dangling-reference rules, numeric bounds and unknown-component refusal; the
persistent identity allocator and the species/content identity used by coarse
simulation are stored with the ledger and refused on mismatch.

A save is one committed snapshot. Every record written by a save carries the
same snapshot generation and tick, each file is written to a temporary path and
renamed, and the manifest records the committed generation last. A save with
zero dirty chunks still writes the clock, ledger, pages and entity records that
changed. On load, records whose generation disagrees with the manifest are
refused as an inconsistent snapshot; the prior complete generation remains
loadable because no file is replaced before it is fully written.

World hash composition keeps its append-only order and folds new state into
existing slots only when non-empty, following the energy-field precedent:
clock, calendar, scheduler configuration and ledger state, region random-stream
state, parked chunk records and creature/ground-object records fold into the
`ecology` slot as tagged canonical sections; page bytes fold into the `wind`,
`weather` and `aether` slots. Frozen regions contribute the content digest
stamped at freeze. Canonical hashes are therefore unchanged while the features
are off or empty. The replay checkpoint record keeps its existing fields; the
new sub-hashes are diagnostics outside the checkpoint. A lockstep session
admits a peer only when the calendar, simulation configuration and content
identities match, failing before the first tick; the wire layout is unchanged.

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
the adapter is not the qualified GPU. The far horizon gate requires complete
per-tier coverage at every qualified station within a bounded settle time,
bounded below-horizon sky, no seam band at any tier radius and no edge at the
outer radius, and a seam or a missing tile is a failure, not a warning. The
activation slice publishes a requirement-to-evidence matrix naming the
executable tests, native commands, fixtures, thresholds and artifact schemas
behind every statement in this document.

Current behaviour: the benchmark records means only, sums eleven pass timers
instead of bracketing the frame, pins the framebuffer to 3840×1600, and does not
record the renderer string; present-to-present wall timing already exists but
is not retained per frame.

## Deferred

Traversal beyond 32 km from the origin (origin rebasing), views beyond 16 km,
far shadows and terrain occlusion beyond the cascade range, depth handling on
other rendering backends, hardware tiers beyond the primary machine, delivery
of coarse state and the far authority overlay to remote clients, scent and
foraging at distance, a simulation-only residency class, scheduled game events
and Workshop content distribution are tracked separately and are not part of
this contract.
