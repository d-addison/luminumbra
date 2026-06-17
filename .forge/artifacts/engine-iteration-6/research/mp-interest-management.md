# Interest Management / Area-of-Interest (AOI) for the Luminumbra dedicated server

**Slug:** `mp-interest-management` · **Side:** NET + SIM (engine-generic) · **Iteration:** 6, Wave C (P3/P5)
**Status:** RESEARCH BRIEF v1 (2026-06-17) · feeds the `MULTIPLAYER-BLOCKER-SPEC` v2 P3 replication layer

## Scope

The architecture pivot (MULTIPLAYER-BLOCKER-SPEC v2 §0) is an **authoritative dedicated server +
snapshot replication + client prediction** (Source/Quake lineage): the server runs the one true
30 Hz sim and sends each client a per-client entity-state snapshot. With 20+ players spatially
distributed across a large chunk-streamed world, the server **must not** broadcast every entity to
every client — that is O(N²) state and O(N²) bandwidth. **Interest management (IM) / area-of-interest
(AOI)** is the subsystem that decides, per client per tick, *which* entities and state each client
needs, so per-client bandwidth and server CPU stay bounded as N grows.

This brief is scoped to **entity-state interest** (which dynamic entities — avatars, props, future
creatures — each client's snapshot contains). World *voxel/chunk* streaming is a separate, already-
solved residency problem; the key insight below is that **the existing chunk residency index is
itself a spatial AOI index we should reuse**, not duplicate.

Out of scope: the snapshot wire format, delta/baseline compression, prediction/reconciliation
(those are the rest of P3); LOD of *terrain* (already done).

---

## Recommended approach

**Cell-based (grid) publish/subscribe interest, keyed on the existing 16 m chunk grid, with a
distance/radius refinement and update-rate tiering by distance. Reuse `SHIELD_WorldSystem`'s chunk
spatial index as the cell index; do not build a second one.**

This is the scheme the literature consistently identifies as the best accuracy-vs-cost trade-off for
MMO-scale games (Boulanger/Kienzle/Verbrugge 2006 [1][2]: tile/cell schemes "approximate ideal
visibility-based interest management at very low cost"), and it is what large persistent worlds use
in practice (EVE per-solar-system scoping [6][7]; Quake/Source per-client PVS-scoped snapshots
[8][9]). Concretely:

1. **Cells = chunk columns (or small chunk blocks).** We already have a 16 m chunk grid and a hash
   map from `ChunkID` → chunk. Our existing streaming `add_candidate`/union-wanted-set logic
   (`SHIELD_WorldSystem.cpp:1777-1867`) **already maintains, per tick, the set of cells each anchor
   cares about** (the union of each anchor's disc, deduped, with closest-anchor priority — T-I6 P0).
   AOI cells can be the same grid, optionally coarsened to NxN chunk "interest cells" (e.g. 4×4
   chunks = 64 m) so the subscriber bookkeeping is cheaper than per-chunk. This is the Benford/Fahlén
   **aura** made discrete: a player's *aura/focus* is the disc of interest cells around its avatar;
   an entity's *nimbus* is simply the cell it occupies [3][4].

2. **Per-cell entity buckets + a publish/subscribe map.** Maintain `cell → {entity ids in cell}`
   (rebuilt or incrementally updated each tick from avatar/prop positions) and, for each client, the
   set of cells it currently subscribes to (its AOI = the chunk discs around its avatar — *exactly
   the anchor's wanted-set the streamer already computes*). A client's snapshot entity set =
   ⋃ entities in its subscribed cells. This is O(players × cells-per-aura + entities) per tick, not
   O(N²).

3. **Distance/radius refinement inside the boundary cells (cheap, optional).** Cell membership
   over-includes at the rim (an entity in a corner of a far cell may be 90 m away). A single squared-
   distance test against the avatar (we already compute closest-anchor squared distance at
   `SHIELD_WorldSystem.cpp:1543-1546`) trims the rim to a true radius. This is the hybrid the
   literature recommends: grid for the cheap coarse pass, Euclidean for the precise edge.

4. **Hysteresis on the AOI boundary to stop subscribe/unsubscribe thrash.** Subscribe a cell at
   `radius_in` (e.g. 6 chunks / 96 m) but only unsubscribe at a larger `radius_out` (e.g. 8 chunks /
   128 m). An entity dithering on a border does not generate repeated enter/leave spawn/despawn
   churn. This mirrors the asymmetric LOD-demotion hysteresis the chunk LOD code *already* uses
   (`get_required_lod_for_chunk`, "asymmetric hysteresis margin so chunks dwelling on a band edge do
   not oscillate") — apply the identical pattern to AOI subscription.

5. **Enter/leave AOI events drive snapshot baselines.** When a cell enters a client's AOI, its
   entities are *spawned* into that client's replication set (full baseline next snapshot); when a
   cell leaves, they are *despawned* (a removal event, then dropped from deltas). Between those, the
   entity rides normal baseline+delta snapshots (the Quake 3 model: delta against the client's last
   acknowledged snapshot [9]).

6. **Update-rate tiering by distance (near = high rate, far = low rate).** Tier each subscribed
   entity by its cell ring distance: ring 0–1 (near) replicated every tick (30 Hz); mid rings every
   2nd–3rd tick (~10–15 Hz); rim every Nth tick (~5 Hz). Far entities are visually small / slow to
   matter, so a coarse rate plus client interpolation is indistinguishable. This is the standard MMO
   "primary vs secondary awareness" split EVE describes between in-system and out-of-system [6][7],
   and the explicit dynamic-update-rate technique in the IM middleware literature [5][10].

**Why this is the right fit for us specifically:** our world is *already* chunk-streamed around
multiple anchors with a fair per-anchor budget. The server's set of streaming anchors == the set of
player avatars (MULTIPLAYER-BLOCKER-SPEC P1). The residency disc the streamer computes per anchor
**is** that player's AOI. So IM is not a new spatial subsystem — it is a thin entity-bucketing +
subscription layer that *reads the same wanted-set the streamer already produces*. Lowest new code,
no second spatial index to keep coherent, and it inherits the multi-anchor fairness work already
landed.

---

## Alternatives considered (and why rejected)

- **Broadcast everything to everyone (no IM).** Per-client bandwidth and server serialization scale
  O(N): at 20 players each snapshot carries 20 avatars + all props; total server send is O(N²). Dies
  well before 20 players on a zen-co-op bandwidth budget, and pointless when players are spatially
  spread across a large world. Rejected — it is the exact thing the pivot spec calls out ("does not
  broadcast everything to everyone").

- **Per-pair Euclidean distance visibility (O(N²) every tick).** Test every entity against every
  client by true distance. Most accurate radius result, but O(N²) CPU and it ignores the cell index
  we already have. Boulanger et al. [1][2] show pure distance is *both* the least scalable bookkeeping
  *and* less accurate than visibility — worst of both. Rejected as the primary scheme; we keep a
  single distance test only as the cheap rim *refinement* inside the grid pass (step 3).

- **Ray / line-of-sight (occlusion-aware) visibility, or precomputed PVS (Quake/Source).** Most
  accurate (obstacle-aware IM cuts messages up to ~6× in [1][2]; Quake's BSP PVS is the canonical
  static form [8]). But PVS needs a static BSP partition with precomputed visibility — our world is a
  *procedurally generated, runtime-streamed, editable voxel field* with no static BSP, so a precomputed
  PVS is not available, and runtime ray-visibility per entity-pair is expensive. A zen photography
  game also has open sightlines (few hard occluders) where occlusion buys little. Rejected as
  overkill; revisit only if dense-occluder interiors become a bandwidth problem (then add coarse
  per-cell visibility links between adjacent cells, not per-pair rays).

- **Region/zone sharding with hard handoff (WoW-style zoning, EVE solar-system nodes [6][7]).** Split
  the world into authoritative regions, each on its own node/process; AOI is "everyone in your
  region." Excellent for 300k players, but it is a *server-partitioning* strategy, not a per-client
  scoping one, and it adds cross-region handoff/consistency complexity we do not need for a single-
  process 20+ player server. We borrow the *idea* (cell = lightweight region, primary vs secondary
  awareness) without the multi-node handoff. Rejected as premature; the grid cell *is* our "region"
  at single-process scale.

- **Second, dedicated AOI spatial index (separate quadtree/grid for entities).** A clean textbook IM
  layer, but it duplicates the spatial index `SHIELD_WorldSystem` already maintains and must be kept
  coherent with it. Rejected in favor of reusing the chunk index (lower code, no divergence risk).

---

## Perf / scale budget (concrete, vs N)

Assume N players, each with an AOI of ~A interest cells (e.g. a 12×12 chunk disc ≈ 110 chunks, or
~7 coarse 4×4 cells), E total dynamic entities, and a typical local density of d entities per AOI.

**Server CPU per tick:**
- Bucketing entities into cells: **O(E)** (one cell hash per entity; E here is avatars + props,
  tens to low hundreds, not the voxel count).
- Building each client's entity set from its subscribed cells: **O(N · A + N · d)** — linear in N,
  *independent of total E*. With cell hysteresis the per-tick subscription delta is near-zero when
  players move slowly (the zen case).
- Rim distance refinement: **O(N · d_rim)**, one squared-distance test per rim entity.
- Total: **≈ O(N · (A + d))**, i.e. linear in N for bounded local density — the defining property of
  IM (vs O(N²) broadcast). This is why the literature [1][2][5] treats cell IM as the scalability
  mechanism.

**Per-client bandwidth:** bounded by the entities in that client's AOI, **not** by total N. A client
near k others sends/receives ~k avatars + local props at full rate; distant players cost nothing
(out of AOI) or a trickle (rate-tiered rim). For a spread-out 20-player zen world the common case is
k ≈ 1–4 (small co-op clusters), so per-client downstream stays in the low-KB/s range even though the
server hosts 20. Worst case (all 20 in one spot, e.g. a group photo) degrades gracefully to ~O(local
density) — capped by a per-snapshot entity budget + rate-tiering, never O(total world).

**Memory:** `cell → entity-ids` map (O(E)) + per-client subscribed-cell set (O(N · A)). Both small
relative to the resident voxel data already in `m_streaming_state.chunks`.

**Tunables to size at P5 (20-player hardening):** `radius_in`/`radius_out` chunk radii, interest-cell
coarsening factor (1×1 vs 4×4 chunks), per-tier update intervals, and a hard per-snapshot entity cap
(spillover demoted to lower tiers). Measure at 20 anchors against the per-snapshot byte budget before
fixing values (the spec's "measure first" rule).

---

## Integration notes

- **Reuse the spatial index.** `SHIELD_WorldSystem::update(registry, std::vector<Vec3> anchors, …)`
  already computes, per anchor, the union wanted-set of chunks with closest-anchor squared distance
  and ring distance (`update_chunk_activation` / `add_candidate`, `SHIELD_WorldSystem.cpp:1777-1867`;
  budget `STREAMING_MAX_ACTIVE_CHUNKS_BUDGET = 8192` at `:32`). The AOI layer should consume the same
  per-anchor disc to derive each client's subscribed cell set — ideally `SHIELD_WorldSystem` exposes
  a small `const` query like `chunks_in_aoi(anchor, radius_in/out)` so IM never re-derives the grid
  math. Anchors == avatar positions (P1), so AOI and streaming share one source of truth.

- **Layering / ownership.** IM is a **server NET-side** component sitting between the SIM
  (`GameSession::TickSimulation`, the authoritative entity state) and the replication encoder (P3
  snapshot writer). Per tick: (1) SIM steps; (2) IM rebuilds cell buckets from ECS positions and each
  client's subscription set (applying hysteresis), emitting enter/leave events; (3) the snapshot
  encoder, for each client, serializes the client's in-AOI entities (baseline for newly-entered,
  delta against last-ack otherwise), at the per-entity tier rate. Keep IM **read-only over sim state**
  — it must not mutate `world_hash`-affecting state (it is a transport-scoping concern, exactly like
  render/stream residency is one-way and `world_hash`-neutral). This preserves the spec's invariant:
  IM decisions are a pure function of authoritative positions, never of socket/arrival order.

- **Determinism / hash.** IM is downstream of the authoritative sim and per-client — it does **not**
  enter `world_hash` (same rule as streaming residency, MULTIPLAYER-BLOCKER-SPEC §2). Two clients can
  receive different entity sets without any desync, because the server is the sole authority. Safe to
  build entirely in P3/P5 with no determinism bump.

- **Reuse the hysteresis pattern.** Lift the asymmetric-margin idea from `get_required_lod_for_chunk`
  (LOD demotion hysteresis) for AOI subscribe/unsubscribe so border entities don't thrash.

- **Join/leave (P4) falls out cleanly.** Join = add the avatar as a new anchor + start the snapshot
  stream; its AOI is computed on the first tick like any other. Leave = drop the anchor and the
  client's subscription set. No global recompute — IM is incremental per client.

- **Future entities.** Creatures/Codex fauna and dynamic props slot in as ordinary cell-bucketed
  entities; no IM change needed as content grows, only the per-snapshot entity cap may need raising.

---

## Open risks

1. **Crowd hotspot (all N in one cell — the "group photo" case).** Local density spikes to N; per-
   client cost there is O(N) and snapshot size can blow the byte budget. Mitigation: hard per-snapshot
   entity cap + aggressive rate-tiering of the overflow + (later) relevance prioritization (nearest /
   recently-changed first). Must be load-tested at 20-in-one-chunk, not just spread-out.
2. **Cell size vs entity speed.** Too-coarse cells over-include (wasted bandwidth); too-fine cells
   raise bookkeeping and increase boundary crossings (more enter/leave churn). Needs measurement
   against the 16 m chunk and avatar move speed; the hysteresis band absorbs some, but the
   coarsening factor is a real tuning knob.
3. **Fast-mover / teleport pop-in.** A client crossing many cells in one tick (or a teleport) must
   bulk-subscribe and receive many baselines at once — a bandwidth spike. Cap baseline spawns/tick and
   spread them over a few snapshots; acceptable for a zen game with mostly slow movement.
4. **Editable voxel world breaks any precomputed visibility.** Confirms PVS-style precomputation is
   off the table; if occlusion-aware IM is ever wanted it must be runtime and coarse (per-cell links),
   re-derived as chunks are edited/streamed.
5. **Coherence with streaming residency.** If AOI radius exceeds the streamed/resident radius, the
   server could try to replicate an entity in a cell whose chunk isn't resident. Keep AOI radius ≤
   the guaranteed per-anchor near-field floor (the P0 fairness work), or gate replication on chunk
   residency.

---

## Citations

[1] Boulanger, J.-S., Kienzle, J., Verbrugge, C. **"Comparing interest management algorithms for
massively multiplayer games."** Proc. 5th ACM SIGCOMM Workshop on Network and System Support for
Games (NetGames '06), 2006. https://dl.acm.org/doi/10.1145/1230040.1230069 — Compares distance,
tile/cell, ray-visibility, and obstacle-aware IM on a real MMO; obstacle-aware cuts messages up to
~6×, and **cheap tile/cell schemes approximate ideal visibility-based IM at very low cost** — the
primary justification for our grid approach.

[2] Boulanger, J.-S. **"Interest Management for Massively Multiplayer Games"** (M.Sc. thesis, McGill
Univ., 2006). https://www.cs.mcgill.ca/~jboula2/thesis.pdf — Full treatment: concludes **grid/cell
spatial partitioning is the best accuracy-vs-CPU balance**, cheaper than ray-casting and more
accurate than naive distance; basis for the hybrid grid + rim-distance scheme.

[3] Benford, S., Fahlén, L. **"A Spatial Model of Interaction in Large Virtual Environments."** Proc.
ECSCW 1993. https://www.lri.fr/~mbl/ENS/CSCW/2013/papers/Benford_CSCW1993.pdf — The canonical
**aura / focus / nimbus / adapter** model: an object's *aura* bounds where interaction is possible;
*focus* is what an observer attends to, *nimbus* is an object's projected presence. Our per-avatar
AOI disc = aura; cell occupancy = nimbus.

[4] Benford, S., Fahlén, L., et al. **"Awareness, Focus, and Aura: A Spatial Model of Interaction in
Virtual Worlds."** (Presence: Teleoperators and Virtual Environments, 1995.)
https://www.semanticscholar.org/paper/Awareness,-Focus,-and-Aura:-A-Spatial-Model-of-in-Benford-Fahl%C3%A9n/d27417e07fc96fd7741f632d1f13e8d986f4bbcc
— Journal formalization of focus/nimbus awareness and *adapters* that amplify/attenuate aura — the
theoretical grounding for distance-weighted relevance and update-rate tiering.

[5] Boulanger / IM middleware literature — **"Interest management middleware for networked games."**
https://www.researchgate.net/publication/220791986_Interest_management_middleware_for_networked_games
— Region-based publish/subscribe partitioning into regions/grid cells; dynamic update-rate
adjustment by context (basis for our pub/sub cell map + rate tiering).

[6] CCP Games. **"The Server Technology of EVE Online: How to Cope with 300,000 Players in One
World."** GDC. https://gdcvault.com/play/1030721/The-Server-Technology-of-EVE — Single-shard scaling;
solar-system partitioning gives **primary awareness within a system, secondary awareness across the
cohort** — the canonical real-MMO precedent for cell-scoped near/far interest tiering.

[7] **EVE Online Architecture** (High Scalability writeup of CCP's design).
https://highscalability.com/eve-online-architecture/ — Single cluster, per-solar-system SOL-blade
nodes; ~300K users / ~40K concurrent; concrete evidence that spatial partitioning (system = cell) is
how a persistent large world bounds per-area load.

[8] Quake/PVS background (Fabien Sanglard, Quake engine review).
https://fabiensanglard.net/quakeSource/quakeSourceRendition.php — **Potentially Visible Set**: per-
BSP-leaf precomputed visibility, delta-compressed; the static occlusion-aware IM we *reject* because
our voxel world has no static BSP, but the canonical reference point.

[9] **Quake 3 Network Model** (Fabien Sanglard). https://fabiensanglard.net/quake3/network.php —
Per-client snapshots, 32-snapshot history, **delta compression against the client's last acknowledged
snapshot**, server sends only entities the client needs (map-location scoped). The replication model
our IM layer feeds.

[10] **"Combat State-Aware Interest Management for Massively Multiplayer Online Games."**
https://www.researchgate.net/publication/323701106_Combat_State-Aware_Interest_Management_for_Massively_Multiplayer_Online_Games
— Context-driven dynamic update rates (raise rate where it matters, lower it elsewhere) — supports
our distance/state-based rate tiering and per-snapshot relevance prioritization for the hotspot case.
