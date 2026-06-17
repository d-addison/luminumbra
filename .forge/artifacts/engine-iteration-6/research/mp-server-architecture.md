# MMO / Multiplayer Server Architecture & Scalability — Cited Research Brief

**Project:** Luminumbra (zen co-op photography voxel game; C++ engine, deterministic 30 Hz fixed-tick
sim, Jolt physics, SHIELD voxel world streamed in 16 m chunks around multiple player anchors).
**Topic:** Server-side architecture & topology to scale a *persistent authoritative* world.
**Calibration:** "tens of players now (20+), designed to scale." Recommendations are STAGED — do not
over-engineer for thousands when the target is tens.
**Date:** 2026-06-17 · **Iteration:** 6, Wave C · Companion to `MULTIPLAYER-BLOCKER-SPEC.md` (v2 pivot).

---

## 1. Scope

How to structure the authoritative dedicated server so it (a) carries 20+ players at a deterministic
30 Hz with full server physics *now*, and (b) has the partitioning/handoff seams identified for later
scale. Covers: single-process vs zoning/sharding vs seamless distributed meshing; the canonical MMO
architectures (EVE single-shard + node partitioning + time dilation; WoW zone/shard; Star Citizen
server meshing); the authoritative server tick loop; persistence/lifecycle (join/leave, save/restore,
graceful degradation); and transport topology + threading model. It does **not** re-derive the
client-side replication protocol (snapshot/delta/prediction) — that is the companion netcode brief;
this brief is the *server topology and loop* that the protocol runs on top of.

---

## 2. Recommended approach (concrete + staged)

**Stage 0 (now → 20+): a single-process authoritative server. One world, one authoritative sim, run
by `ServerWorldRunner`.** This is the correct foundation and is *not* a stopgap — the industry data
below shows a single authoritative process comfortably carries tens of players with physics at our
tick rate; sharding/meshing is what you reach for at the *hundreds-in-one-place* boundary, which a
zen co-op photography game will not hit. Concretely:

1. **Keep the deterministic 30 Hz core as the server authority.** `SimulationClock` (fixed
   `fixed_dt`, catch-up clamp `kMaxCatchUpTicksPerFrame = 4`) + `GameSession::TickSimulation` is
   *already* the one true simulation `ServerWorldRunner` drives. This is exactly the Source-engine
   authoritative loop (intake user commands → physics step → game rules → snapshot) [S-Source], just
   at 30 Hz instead of 66 Hz. Do not rebuild it; wrap it.

2. **Server loop = the canonical authoritative tick** (per [S-Source], [S-Valorant]):
   `accumulate wall dt → SimulationClock.advance() → for each due tick: { drain per-client usercmd
   queue → GameSession::TickSimulation(fixed_dt) (physics + world systems) → build per-client
   snapshot } → send`. Today `RunFixedTicks()` runs `frame_dt == fixed_dt` for determinism; the
   *live* server replaces the fixed driver with a real wall-clock-fed accumulator (still one tick per
   `fixed_dt`), keeping `RunFixedTicks` for the replay/oracle path.

3. **Multi-anchor streaming = the player-position anchor vector.** The streaming engine already takes
   `std::vector<Vec3>` anchors (union wanted-set, evict-if-near-ANY, closest-anchor LOD; foundation
   `0113a60`). Feed every connected player's avatar position as one anchor. This is the natural
   server analogue of per-zone interest; it bounds *residency* (what voxels are CPU-resident) and is
   `world_hash`-neutral.

4. **Threading model: one sim thread, separate network IO threads, lock-free handoff.** The fixed-tick
   sim runs on a single logical thread (it already does — determinism + Jolt step ordering depend on
   it; internal parallelism stays inside the `JobSystem` fork/join *within* a tick, not across ticks).
   Network receive/send run on their own IO thread(s); inbound `usercmd`s land in a per-client queue
   the sim drains at tick start; outbound snapshots are handed to the IO thread to serialize/send. The
   sim thread never blocks on a socket. This is the standard authoritative-server split and is what
   lets one core saturate at the tick deadline without IO jitter [S-Source][S-Valorant].

5. **Graceful degradation = adaptive tick rate (a "time-dilation" analogue), NOT silent drop.** If a
   tick overruns its `fixed_dt` budget under load, prefer slowing the *simulated* rate so the world
   stays internally consistent for everyone — the EVE TiDi principle: "everyone is still getting an
   accurate view... players are still playing against an accurate game state" [E-HighScale][E-TiDi].
   For a zen game this will essentially never fire, but the `SimulationClock` already has the hook:
   its catch-up clamp drops *whole-tick* excess time (recorded in `dropped_time_seconds`) rather than
   spiraling. Expose that telemetry as the degradation signal; a TiDi-style deliberate slowdown is a
   later, opt-in refinement, not Stage-0 work.

**Stage 1 (identified seams, build later only if measured): partition by region, not by shard.**
The seam to cut along — if a single process ever saturates — is *spatial*, mirroring EVE's
"solar-system → node" mapping [E-HighScale] and the DVE partitioning literature [P-Object][P-Adaptive].
Our world is already chunked and anchor-driven, so the natural partition is a contiguous chunk-region
owned by one sim process, with **authority handoff at region borders** (the entity's authority swaps
as it crosses, the old owner becomes a receiver of updates — Star Citizen's model [SC-Mesh]). Identify
these seams now; do not implement them now:
- *Authority is per-region, keyed by chunk-region id* (not per-player), so the assignment is a pure
  function of position — the same determinism discipline the streaming anchors already follow.
- *Handoff is a state-baseline transfer* — exactly the same mechanism as a player JOIN (§4), so
  building join cleanly buys most of the handoff machinery.
- *Cross-region interest* (seeing/affecting entities just over a border) is the hard part the academic
  work warns about [P-Adaptive][NVE]; defer until the player density actually demands a second process.

**Why staged and not seamless-distributed now:** server meshing / seamless handoff is a multi-year,
multi-team effort (Star Citizen is still hardening it after years [SC-Mesh][SC-CitCon]) and solves a
problem — thousands of players in one contiguous space — we do not have. Building it now would violate
"don't over-engineer for thousands when the target is tens." But cutting the world along chunk-regions
and keeping authority position-derived means the *door* to Stage 1 stays open without a rewrite.

---

## 3. Alternatives considered (and why rejected for Stage 0)

| Architecture | What it is | Verdict for us |
|---|---|---|
| **Peer lockstep (the v1 plan)** | All peers run identical sim, advance only when *every* peer's input for a tick arrives. | **Rejected** (already retired in spec v2). Stalls on the slowest of N peers; cannot do mid-session join/leave; needs bit-exact cross-machine determinism. Incompatible with 20+ persistent join/leave + server physics. Keep only as a 2-player replay/loopback tool. |
| **Zone-server sharding (WoW)** | Each zone/map runs on a world-server; when a zone overcrowds, spawn a parallel *shard* (copy) and split players across copies; players on different shards can't see each other [W-Shard]. | **Rejected now.** Sharding deliberately *breaks the shared world* (separate NPC/node/object sets per copy) to dodge crowding — antithetical to a small co-op group that wants to photograph the *same* world together. It's a crowding tool for thousands, not a tens-of-players tool. |
| **Seamless distributed world / server meshing (Star Citizen)** | Many DGS processes simulate adjacent regions of one continuous world; a replication layer syncs entities and swaps authority at borders [SC-Mesh][SC-CitCon]. | **Deferred to Stage 1.** Correct *direction* for scale, but enormous cost and solves a density we won't hit. We adopt only its *seam* (position-keyed region authority + handoff-as-baseline-transfer) as the future-proofing, not the implementation. |
| **EVE single-shard, 1 Hz, per-node solar systems + TiDi** | One universe, partitioned into solar systems mapped to nodes; ~1 Hz tick; under overload the node *slows time* (down to 10%) instead of dropping state [E-HighScale][E-TiDi]. | **Adopted in principle, not in form.** We take the *graceful-degradation philosophy* (slow the clock, keep everyone consistent) and the *position→node partition seam*, but EVE's 1 Hz turn-based-ish cadence and per-node TiDi granularity are wrong for a 30 Hz physics game. |
| **Many tiny match servers (Valorant/Source CS model)** | N independent short-lived match instances, one core each, ~1000 players/host across ~108 games [S-Valorant]. | **Wrong shape.** That's *horizontal scale of isolated matches*, not one persistent shared world. But its per-process budget data (one core saturates a high-tick match) directly informs our single-process headroom estimate (§5). |

---

## 4. Persistence & lifecycle

**Continuous uptime.** The server runs the sim independent of any client (it already can — headless
`ServerWorldRunner` has no client coupling). Clients are anchors that come and go; the world clock
never stops. This is the EVE "world is always running" property [E-HighScale] and is the whole point
of the v2 pivot away from lockstep (which *requires* all peers from tick 0).

**Join = state-baseline handoff (trivial under authoritative model).** A connecting client gets: world
identity (seed/preset, validated by `GameSession::ValidateWorldConfig`), a deterministic avatar spawn
(P1: `PlayerAvatar` from `(seed, preset, player_id)`), and an initial *full snapshot baseline* of the
entities in its area-of-interest; thereafter it receives deltas. No cross-peer hash convergence is
needed — the server is the authority, the joiner simply adopts tick-T state [S-Source]. (Contrast: a
lockstep joiner *cannot* do this — the reason join was deferred in v1.)

**Leave = drop + despawn/freeze (trivial).** Disconnect removes the client's `usercmd` queue, despawns
or freezes its avatar, and removes its streaming anchor. No tick stall (unlike lockstep, where a
missing input freezes everyone).

**Save / restore.** Keep the existing `WorldSaveService` + `world_hash` machinery, **repurposed as the
server's replay + save-integrity + desync-DEBUG invariant** (not a transport requirement). Autosave on
the tick cadence (`autosave_interval_ticks` already exists on `ServerWorldRunnerConfig`); the
incremental contract (never-edited world writes nothing) and `world_hash` give a cheap integrity check
on restore. Player avatars become non-empty `entities` sub-hash state — one deliberate `world_hash`
bump, landed in its own commit with the heavy-oracle + replay re-bless (per the spec's bump ledger).

**Graceful degradation under load.** Two levers, in order of preference:
1. **Interest-management / AOI first** — shrink what each client is told about (PVS / per-anchor
   streaming floors) before touching the clock; bandwidth and snapshot-build cost scale with
   *observed* entities, not total. This is the cheapest lever and the one that actually matters at 20+.
2. **Adaptive tick rate (TiDi-analogue) last** — only if the *sim itself* can't hold `fixed_dt`. Slow
   the simulated rate uniformly so the world stays consistent [E-TiDi]; surface
   `SimulationClock::dropped_time_seconds` / `dropped_frame_count` as the trigger telemetry. Prefer
   this over silently dropping snapshot rate to a subset, which makes the world inconsistent.

---

## 5. Perf / scale budget (single authoritative process at 30 Hz with physics)

**Per-tick budget:** at 30 Hz, `fixed_dt ≈ 33.3 ms`. A single-game authoritative server can dedicate
~a full core to one match and still meet a *much* tighter deadline — Valorant meets **7.8125 ms**
(128-tick) on one core for a 10-player match [S-Valorant]. Our deadline is ~4.3× looser, so the raw
per-tick headroom for 20+ players + Jolt physics is comfortable *provided the per-tick work stays
roughly O(N), not O(N²)*.

**The real scaling constraints (flagged estimates, validate by measurement):**
- **Physics broadphase must be O(N), not O(N²).** Naive all-pairs is the classic killer: 30 players +
  50 props ≈ 6,400 pair checks vs spatial-partition O(N) [S-Scaling]. Jolt's broadphase already gives
  us this; the budget assumption is that *we keep prop/avatar counts in the hundreds, not thousands*.
- **Snapshot build + bandwidth scales with observed entities per client, not total.** Without interest
  management, downstream cost is ~O(players × visible-entities); with PVS/AOI it's bounded by what each
  player can actually see. For 20 players in a sparse zen world this is small; **AOI is what makes 50+
  feasible** [S-Source][P-Adaptive].
- **Streaming residency is the other CPU sink.** N far-apart anchors widen the union wanted-set; the
  shared 8192-active-chunk budget can starve a far player (spec risk #2). Per-anchor near-field floors
  (P0, building now) bound this.

**Realistic single-process holding (estimates — flag, then measure):**
- **20–32 players, sparse co-op world, full Jolt physics, 30 Hz, AOI on: comfortable on one modern
  core** for the sim, with IO on separate threads. This is the *design target* and should be the first
  load-test gate.
- **~50–100 players in one shared space:** plausible single-process *if* AOI + per-anchor floors are
  tuned and prop counts stay bounded; expect the 30 Hz sim deadline (physics + streaming residency),
  not bandwidth, to be the first wall. Treat as the "scale" stretch, gated on measurement.
- **Hundreds-in-one-place / thousands:** *out of scope for one process.* This is the EVE/Star-Citizen
  regime where per-node partitioning + TiDi or server meshing become mandatory [E-HighScale][SC-Mesh].
  This is the Stage-1 seam, not a Stage-0 promise.

These are estimates to be replaced by a real load test (synthetic 20→32→50 anchors driving distinct
avatars under physics, measuring 30 Hz tick-budget margin and per-anchor residency).

---

## 6. Integration notes (our types)

- **`ServerWorldRunner`** — the home of the live loop. Add a wall-clock-fed run mode beside
  `RunFixedTicks` (keep `RunFixedTicks` for replay/oracle determinism). It already owns `JobSystem` +
  `GameSession` + autosave; it gains a per-client `usercmd` intake (from the net IO thread) and a
  per-client snapshot emit. It already exposes `Session()`, `ComputeWorldHash`, and the streamed-chunk
  view the snapshot builder needs.
- **`GameSession::TickSimulation(fixed_dt)`** — unchanged as the authoritative sim step (physics →
  world/wind/weather/aether systems → ordered event-bus drain). Player avatars + server physics enter
  this path at P1/P2 (the deliberate `world_hash` entities/physics bump). The look axis stays
  render-only and never enters the hash (existing rule).
- **`SimulationClock`** — already the fixed-timestep authority. Its catch-up clamp +
  `dropped_time_seconds`/`dropped_frame_count` telemetry are the graceful-degradation signal; a
  deliberate TiDi-style slowdown is a thin layer on top, deferred.
- **`WorldSaveService` + `world_hash`/sub-hashes** — repurposed as server replay + save-integrity +
  desync-DEBUG (not a transport requirement). Autosave via the existing `autosave_interval_ticks`.
  Restore validates against the recorded hash. The `entities`/physics sub-hashes (currently empty
  headless) become the avatar/physics state hash after the P1/P2 bump.
- **Multi-anchor streaming (`SHIELD_WorldSystem::update_chunk_activation(std::vector<Vec3>)`,
  `0113a60`)** — the connected players' avatar positions ARE the anchor vector. Residency stays
  `world_hash`-neutral; per-anchor near-field floors (P0) prevent a far player starving. This is also
  the natural Stage-1 partition boundary (region = chunk-region; authority position-keyed).
- **Transport topology** — **star-through-dedicated-server** (clients ⟷ server only; no peer mesh).
  This is the authoritative-server topology by definition [S-Source]; it replaces the v1
  star-through-*host* lockstep relay. Threading: sim thread vs network IO thread(s), lock-free queues
  between them (§2.4).
- **`LockstepSession` / `ILockstepTransport`** — parked (not deleted). Its length-prefixed,
  no-struct-padding, hex-hash framing discipline is the wire-encoding *style* the replication protocol
  reuses; the session itself is retained only as a 2-player deterministic replay/loopback tool.

---

## 7. Open risks

1. **Sim-thread saturation under physics, not bandwidth, is the likely first wall.** Validate with a
   real 20→32→50-anchor load test before claiming any count; the §5 numbers are estimates.
2. **Per-anchor streaming starvation** at N far-apart anchors under the shared chunk budget — P0
   per-anchor floors must be measured at ≥3–4 distinct anchors (spec risk #2).
3. **AOI / interest management is load-bearing for 50+** and is *not* Stage-0-trivial; without it,
   snapshot cost grows with total entities. Scope it explicitly in the netcode brief.
4. **The `world_hash` entities/physics bump** (avatars + server physics) must land in its own commit
   with heavy-oracle + replay re-bless, sequenced after any remaining Wave C worldgen bump, to stay
   one-bump-per-commit attributable (spec §4).
5. **Stage-1 cross-region interest** (entities visible/interacting across a partition border) is the
   genuinely hard distributed-systems problem [P-Adaptive][NVE]; keep the seam position-keyed and
   handoff-as-baseline so we *can* cut it later, but do not under-estimate it if we ever do.
6. **TiDi-analogue is a refinement, not a crutch.** If it ever fires routinely, the real fix is AOI or
   a region split — slowing the clock is the last resort, not the load plan.

---

## 8. Citations

**Primary — real MMO / authoritative-server architectures**

- **[S-Source]** Valve, *Source Multiplayer Networking*, Valve Developer Community wiki.
  https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking — The canonical authoritative
  tick loop: server simulates in discrete ticks (default 15 ms / 66.7 Hz), each tick *processes user
  commands → runs a physics step → checks game rules → updates object states*, then snapshots world
  state for clients; client does prediction + interpolation + the server does lag compensation. This is
  exactly our intended server loop at 30 Hz.
- **[S-Valorant]** Riot Games, *VALORANT's 128-Tick Servers*.
  https://technology.riotgames.com/news/valorants-128-tick-servers — CPU is the binding constraint:
  128-tick means a 7.8125 ms per-tick deadline, ~one core per match; ~108 games (~1080 players) per
  36-core host. Doubling tick rate ~doubles CPU + bandwidth. Direct evidence one authoritative process
  easily meets a far tighter deadline than our 33.3 ms for a tens-of-players match.
- **[E-HighScale]** *7 Sensible and 1 Really Surprising Way EVE Online Scales*, High Scalability.
  https://highscalability.com/7-sensible-and-1-really-surprising-way-eve-online-scales-to/ — Single-shard
  universe; "games are sharded by solar system and multiple solar systems run on a node"; live node
  remapping; session changes throttled (≤1 per 10 s); time dilation "slows down time so the game can
  process more... clients kept in sync" while everyone keeps an accurate, consistent game state.
- **[E-TiDi]** *Time dilation*, EVE University Wiki.
  https://wiki.eveuniversity.org/Time_dilation — TiDi triggers at node-load threshold; slows simulation
  down to **10% speed** (1 s of game = 10 s real) at the extreme; *per-node* granularity (affects all
  systems on the overloaded node); does **not** touch EVE Server Time / persistent timers. The graceful-
  degradation principle we adopt (slow the clock, keep everyone consistent) — but per-node/1 Hz form is
  wrong for 30 Hz physics.
- **[SC-Mesh]** Cloud Imperium / community technical writeup, *Star Citizen server meshing /
  Replication Layer*. https://hangarbase.org/news/star-citizen-the-expanded-server-mesh-the-future-of-the-verse-revealed-at-citizencon-2025
  — Many dedicated game servers simulate adjacent regions of one continuous world; as entities cross
  borders, *authority swaps and the original server becomes a receiver of updates*; a Replication Layer
  syncs all entity state and feeds backend persistence. The seam (position-keyed region authority +
  handoff) we future-proof for; the full implementation we defer.
- **[W-Shard]** *Sharding (term)*, Warcraft Wiki.
  https://warcraft.wiki.gg/wiki/Sharding_(term) — WoW zone sharding: when an outdoor area overcrowds the
  game spawns a parallel copy ("shard"); players on different shards can't see each other and each shard
  has its own NPCs/nodes/objects. A crowding mitigation that *breaks the shared world* — rejected for a
  co-op group that wants the same world.

**Primary — academic distributed-virtual-environment scalability**

- **[P-Object]** *An Object Driven Partitioning Approach for Distributed Virtual Environments*, IEEE
  Conference. https://ieeexplore.ieee.org/document/4420156/ — The partitioning problem = efficient
  assignment of DVE workload to available server resources; basis for position-keyed region partitioning.
- **[P-Adaptive]** *Adaptive Partitioning for Multi-Server Distributed Virtual Environments*, Proc. ACM
  Multimedia. https://dl.acm.org/doi/10.1145/641007.641062 — Multi-server is the standard DVE scalability
  approach; load balance + consistency across servers (and the cross-border interest problem) are the
  hard open issues — the exact Stage-1 risks we flag.
- **[P-Split]** *Load Balancing for Virtual Worlds by Splitting and Merging Spatial Regions*.
  https://www.academia.edu/69613500/Load_Balancing_for_Virtual_Worlds_by_Splitting_and_Merging_Spatial_Regions
  — Dynamic split/merge of spatial regions to move workload off overloaded servers; the mechanism a
  Stage-1 region-authority scheme would use to rebalance.
- **[NVE]** *Key Technologies for Networked Virtual Environments*, arXiv:2102.09847.
  https://arxiv.org/pdf/2102.09847 — Survey of NVE scalability: spatial partitioning, interest
  management/AOI, dynamic zone repartitioning to balance server load; consolidates why AOI is the
  load-bearing lever at scale.

**Supporting — engineering practice on tick rate & physics scaling**

- **[S-Scaling]** *Game Server Tick Rate Explained* (Edgegap) and tick-rate/physics-scaling writeups
  surfaced alongside [S-Valorant]. https://edgegap.com/blog/game-server-tick-rate-explained-gameplay-precision-vs-infrastructure-cost
  — Tick rate × work-per-tick = CPU; naive O(N²) physics/hit-checks (e.g. 30 players + 50 props ≈ 6,400
  pair checks) is the classic scaling killer, fixed by spatial-partition broadphase to ~O(N) (which Jolt
  gives us). Grounds the §5 "keep it O(N)" budget caveat.
