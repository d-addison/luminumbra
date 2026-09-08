# Distant world and distant simulation contract

Status: accepted contract, September 8, 2026. Implementation is planned in
bounded slices and is **not** present in the tree at the time of writing; the
current shipped behaviour is described under "Current behaviour" in each
section. Nothing in this document waives the acceptance evidence that each slice
must supply, and no format change listed here exists until its slice lands.

## Scope

- The view radius is **16 km from the camera** and the world is **unbounded**:
  terrain streams procedurally in every direction with no fixed outer edge.
- Distant caves are real: long interior sightlines from inside caverns and
  tunnels, coarse real interiors visible through cave mouths from outside, and
  openings, overhangs and arches kept as silhouette features to the full radius.
- Every terrain edit is carried into every coarse tier at that tier's sample
  spacing. Edits smaller than a tier's spacing vanish from that tier naturally.
- The far representation is volumetric coarse signed-distance tiers. Raising the
  camera far plane over the existing surface-only far tiles does not satisfy
  this contract.
- Simulation over distance runs only in **persistent active regions** and only
  while the world host runs. There is no wall-clock progression while a world is
  shut down.

## Three explicit sets

The engine currently uses one radius for everything: chunk residency, rendering
and simulation all follow the 32-chunk (512 m) streaming disc
(`include/luminumbra/core/Types.h`, `SHIELD_WorldSystem::update_chunk_activation`).
This contract separates three sets that may overlap but are never equated.

| Set | Meaning | Owner |
|---|---|---|
| Render visibility | What the client draws: the live chunk disc plus volumetric far tiers out to 16 km. Rendering never mutates or activates world state. | Client `FarLodSystem` and the render passes |
| Simulation activation | Which 512 m regions advance state: persistent active regions ranked and scheduled by the host. | Host `GameSession` through the active-region ledger |
| Storage residency | What is held in memory and on disk: full lattices in the live disc, regenerable far-tile caches, durable region records and the far authority overlay. | Persistence and streaming |

Camera position, noclip flight and coarse rendering never create or wake an
active region.

## Distant-world representation

### Tier ladder

Far terrain is a ladder of five nested, world-origin-aligned volumetric tiers.
Each tier stores sparse 5×5×5 signed-distance bricks and is polygonised per tile
with Marching Cubes; a brick exists only where the field crosses zero.

| Tier | Sample spacing | Brick edge | Tile edge | Drawn to |
|---|---|---|---|---|
| V1 | 4 m | 16 m | 512 m | 1,024 m |
| V2 | 8 m | 32 m | 1,024 m | 2,048 m |
| V3 | 16 m | 64 m | 2,048 m | 4,096 m |
| V4 | 32 m | 128 m | 4,096 m | 8,192 m |
| V5 | 64 m | 256 m | 8,192 m | 16,384 m |

Spacing over outer radius is constant (1/256 rad), so screen-space sample
density is uniform across the ladder. Each tier is resident over its whole disc
and drawn only in its band through the existing radial clip band; the coarser
tier always sits beneath the finer one, which is the mechanism the live ring
and far coverage already use. The live disc, its three live detail levels and
the bounded full-lattice cave neighbourhood are unchanged; long interior
sightlines come from V1 and V2 bricks, which sample the same cave field.

One tier table replaces the separate far-range constants and the differing
1,536 m and 3,000 m horizon figures in code, comments and gates.

Current behaviour: two surface-only heightfield tiers (4 m to 768 m, 8 m to
about 3,000 m) with a 3,200 m far plane; far tiles carry no caves; edits appear
in far tiles only while their chunk is resident because the far store is never
attached to a save directory at runtime.

### Caves and openings at each tier

Pristine bricks sample the same terrain and cave density composition the live
path uses. Below 8 m spacing the full cave router is sampled unchanged. At 16 m
and beyond, the noise-carved cave terms alias when point-sampled, so two
representations are qualified against each other before the ladder is fixed:
a band-limited field (noise caves to 4 km, analytic surface openings to 16 km)
and a filtered field (noise caves at every tier at higher tile cost). The
design review chooses from measured aliasing and cost on the cave-dense preset;
the band-limited field is the recorded fallback. Whatever is chosen, openings,
overhangs and arches remain silhouette features to the full radius.

### Edits

Edits are baked into every tier on the world host inside the save path, after
dirty chunks are written and before far authority is notified. A brick that
becomes homogeneous after an edit is removed. The far authority overlay stores
only edit-baked bricks; pristine bricks are a regenerable in-memory cache. A
client attaches the far store to the save directory at world entry and overlays
persisted bricks and the live resident snapshot.

### Depth, fog, shadows, horizon

- Depth uses reversed-Z with a 32-bit float attachment and a 17,408 m far plane.
  The flip lands first with the far plane held at 3,200 m so parity baselines
  attribute differences to the depth change alone.
- Aerial perspective extends to the ladder outer radius with the invariant
  that transmittance at the outer clip is at most 5%, so no horizon edge is
  visible.
- Shadow cascades still end at 250 m. Far tiles neither cast nor receive
  cascaded shadows; this is a contract, not a regression.
- There is no planetary curvature. The far water sheet is emitted per tile from
  column flags.

## Persistent active regions

Regions are the existing 512 m persistence regions. A region becomes
persistent-active when a replicated simulation anchor (a server avatar, or the
single-player host's player position) is inside its 512 m near disc, when any
voxel or ground-object edit touches it, or when the game pins it explicitly.

Each region carries flags (visited, edited, pinned, populated), tick stamps
(first active, last proximity, last edit, last ticked, frozen at, last save), a
state (active, reduced, frozen), a cadence shift and a water cursor. The host
ranks regions pinned > edited > visited, breaking ties by ticks since last
proximity. When measured budgets are exceeded the lowest-ranked regions first
tick at a reduced cadence with hysteresis and then freeze: state is persisted
with the freezing tick, nothing advances, and nothing catches up on resume
except a per-system opt-in bounded elapsed-time hook. Scheduling decisions are
functions of host tick state, so a replay reproduces them, and a diagnostic
panel shows the ledger, budgets and recent transitions.

Budgets are measured on representative worlds before any cap is enforced.

Dirty chunks leaving the live disc are parked and written by the next save
instead of being discarded, and the host autosaves on a tick interval.

Current behaviour: eviction erases dirty chunks without a flush, the client
saves only on quit, and no region ledger exists.

## Per-system distant policy

| System | Near (live disc) | Distant active region | Frozen region |
|---|---|---|---|
| Terrain edits | Full lattice | Persisted chunk records plus far authority bricks | Unchanged on disk |
| Plants, soil, irrigation, living-world ladder | Every tick | Same integer rules at a coarse cadence with scaled increments | No advance; bounded resume hook |
| Wildlife | Full utility AI, physics avatars | Persisted; coarse needs, lifespan, reproduction and region-to-region travel at cadence; promoted to full AI on approach | No advance |
| Water | Rotating cell window | Bounded per-region share of the cell budget; flux crosses a border only between two active regions | Millimetre arrays kept, no flow |
| Wind, weather, aether ambience | Stateful world-anchored pages | Pages tick only in active regions; storms are durable with absolute spawn ticks; a page entering activation is seeded from seed, tick and position | Page bytes kept |
| Ground objects | Settle, persist, despawn | Despawn when due | Despawn deferred |
| Scheduled game events | Not implemented | Deferred to a later milestone | — |

Authority is server-only on the deterministic fixed tick with per-region
seeded random streams; clients render coarse results only. Distant water
simulates only where its millimetre arrays are resident; a simulation-only
residency class is not part of this contract and would be a separate decision.

Current behaviour: every creature and plant in the registry ticks every tick
regardless of distance; creatures are not persisted; wind, weather, aether,
scent, soil and irrigation are fixed grids anchored on the spawn point.

## Simulation clock and calendar

One persisted absolute tick base continues across save and load. Day length is
36,000 ticks (20 minutes at 30 Hz) and a year is 8 days for every simulation
consumer and for the rendered sky; the shorter render day remains only as a
debug override. Wind, weather, aether, plant light and season, circadian,
migration and stimulus channels derive their phases from this clock.

Current behaviour: the tick counter restarts at zero on every load, so a loaded
world resumes tick-zero weather, and four different period constants coexist.

## Persistence and format contract

Unchanged: preset revision 6, LMR1 container v2, the
`luminumbra.persistence.world_manifest.v1` manifest, FSD2 far payload v3 (kept
decodable; no longer written by the runtime once the volumetric ladder lands),
canonical in-memory serialization, the lockstep hash message and protocol.
There is no obsolete-world migration.

New records, each declared before its slice lands:

| Record | Placement | Absent | Corrupt or truncated | Newer than supported |
|---|---|---|---|---|
| `simulationTick`, `calendar` keys in `world_info.json` | existing file | tick 0 and pinned defaults | corrupt-metadata refusal | not applicable |
| `active-regions.arl` | `region/` | empty ledger | refuse | refuse as future format |
| `field-pages.fpg` | `region/` | pages seeded on activation | refuse | refuse as future format |
| `creature-entities.json` | `region/` | no creatures | refuse | refuse as future format |
| `ground-objects.json` | `region/` | no objects | refuse | refuse as future format |
| `far/v<tier>.<tx>.<tz>.lmr` (FSV1 payload, edit-baked bricks) | save root | regenerate | refuse | refuse as future format |

Records under `region/` join the integrity scan. Engine builds that predate a
record refuse a save containing it as an unknown region file; that refusal is
accepted for the pre-release format. The far authority overlay at the save root
is derived render state: older builds ignore it and it never contributes to a
world hash.

World hash composition keeps its append-only order. New simulation state folds
into the existing slots the way the energy field already folds into the aether
slot, only when non-empty, so canonical hashes are unchanged while the features
are off or empty.

## Performance measurement contract

Two named profiles are qualified on the primary machine (RTX 5070 Ti) at native
output on both 3840×1600 and 3440×1440:

| Profile | Render scale | Gate |
|---|---|---|
| Quality | 1.0 | p99 frame time at most 16.67 ms on every workload |
| Performance | 0.67 | p50 and p99 reported against 8.33 ms (gate status decided in review) |

Workloads are settled fixed views (400 warm-up, 240 measured frames) and 60 s
scripted traversals (surface flight, cave walk, edited-world tour). Streaming
transients are included; any excluded window is listed explicitly with a
reason. Every frame records wall time from present to present, CPU submit,
present, a whole-frame GPU timestamp bracket and the per-pass sum (attribution
only), plus streaming and far-tier counters; reports carry p50, p95, p99,
maximum and median absolute deviation using the same percentile definition as
`tools/perf/perf.py`. The report records the GL vendor and renderer strings and
fails when the adapter is not the qualified GPU. The far horizon gate asserts
zero missing tiles per tier after settling, bounded below-horizon sky, no seam
bands at tier radii and no edge at the outer radius.

Current behaviour: the benchmark records means only, sums eleven pass timers
instead of bracketing the frame, pins the framebuffer to 3840×1600, and does not
record the renderer string.

## Deferred

Views beyond 16 km, far shadows and terrain occlusion beyond the cascade range,
depth handling on other rendering backends, hardware tiers beyond the primary
machine, replication of the far authority overlay to remote clients, scheduled
game events and Workshop content distribution are tracked separately and are
not part of this contract.
