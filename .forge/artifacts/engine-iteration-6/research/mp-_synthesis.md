# Multiplayer research synthesis — authoritative-server networking stack

**Slug:** `mp-_synthesis` · **Side:** SIM + NET · **Iteration:** 6, Wave C research
**Inputs:** the five cited briefs in this directory — `mp-replication.md`,
`mp-interest-management.md`, `mp-prediction-reconciliation.md`,
`mp-networked-physics.md`, `mp-server-architecture.md`.
**Owner directive (2026-06-17):** "research then spec then plan, don't go in blind"
+ the §0 answers (20+ players, persistent join/leave, full server physics).

This synthesis pins the unified architecture the five briefs converge on, resolves the
cross-cutting decisions, and feeds the concrete specifics into the
`MULTIPLAYER-BLOCKER-SPEC.md` v2 §0 phase plan (P3–P5). **All five briefs independently
validate the v2 pivot** (authoritative server over peer lockstep) and agree on the model
below; there were no contradictions to reconcile.

---

## The unified architecture (one picture)

```
 CLIENT (each)                         DEDICATED SERVER (single authoritative process)
 ───────────                           ──────────────────────────────────────────────
 sample input → usercmd  ──UDP(unrel)──▶  drain per-client usercmds (reliable-ish ordered)
 predict LOCAL avatar now                  │
 (replay unacked on ack)                   ▼  one true 30 Hz SimulationClock tick:
                                           - apply usercmds to player avatars
 render remote entities                    - step Jolt physics (players + props)
 at T-100ms (interp buffer)                - world/sim update + multi-anchor streaming
        ▲                                  │       (avatar positions = anchor vector)
        │                                  ▼
 reconcile vs snapshot ◀──UDP(unrel)────  per-client snapshot @ 15–20 Hz:
 (smooth error)                            baseline+delta vs client's last ACKED snapshot,
                                           scoped by AOI (cell-based on the 16 m chunk grid),
                                           quantized/bit-packed bodies, priority accumulator
                                           under a fixed per-client byte budget
 reliable channel (both ways): join/leave, world edits, chat, RPC events
```

Lineage: Quake3 / Source / Tribes replication + Gaffer-On-Games state-sync physics +
Gambetta/Valve prediction + Benford-Fahlén aura/nimbus AOI + EVE single-shard topology.

---

## Cross-cutting decisions (resolved across the briefs)

1. **Transport: add UDP for state; keep a reliable channel for events.** State replication
   MUST run over unreliable UDP with seq/ack + most-recent-wins — a dropped snapshot is
   superseded by the next, so TCP head-of-line blocking is exactly the wrong behaviour
   (`mp-replication`). Our current `TcpTransport` stays for reliable events (join/leave,
   world edits, chat) and as the loopback test path; **a new UDP transport behind the same
   `ILockstepTransport`-style seam is a P3 prerequisite.** (Brief flagged this as the one
   transport gap.)

2. **Snapshot rate 15–20 Hz, decoupled from the 30 Hz sim tick** (`mp-replication`,
   `mp-prediction`). Remote entities interpolated with a ~100 ms render-behind buffer
   (Source `cl_interp 0.1`). Bit-packing: position ~26-bit delta, smallest-three quaternion
   ~29 bits, ~1 bit for unchanged/at-rest.

3. **AOI reuses the EXISTING chunk index — no second spatial structure** (`mp-interest`).
   The multi-anchor streaming wanted-set (union of per-anchor discs, closest-anchor distance,
   T-I6 P0 fairness, `SHIELD_WorldSystem.cpp`) IS each player's area of interest. Interest
   management becomes a thin entity-bucketing + publish/subscribe layer over that index, with
   distance-tiered update rates (near 30 Hz → rim ~5 Hz) and the existing LOD-demotion
   hysteresis reused for subscribe/unsubscribe anti-thrash. This is a major simplification:
   the spatial work is already done and already fair.

4. **Predict the LOCAL avatar only; interpolate everything else** (`mp-prediction`). For a
   non-competitive zen co-op game, DO NOT build lag-compensation server-rewind, favor-the-
   shooter, or time-dilation-for-fairness in v1 — pure cost, no benefit. Our render-side
   camera already decouples look (look needs no prediction at all); only movement is
   predicted, and the 30 Hz tick id is the natural usercmd sequence number. Replay-on-
   correction is single-body, NOT the full-world rollback already rejected.

5. **Physics: state-sync replication of authoritative Jolt bodies** (`mp-networked-physics`).
   Server runs the one `JPH::PhysicsSystem`; ships per-body state (~10 B/body quantized),
   priority-accumulator-capped, sleeping props ~1 bit. Server is final arbiter; client
   "ownership" is a prediction hint, not a hand-off. Jolt's island-based parallel solver
   carries low-hundreds of active bodies inside the 33.3 ms tick. **Deterministic-lockstep
   physics rejected** (stalls at 20 peers, no join/leave, ~8% CROSS_PLATFORM_DETERMINISTIC tax
   we don't need under state-sync).

6. **Server topology: single authoritative process now; partitioning seams identified, not
   built** (`mp-server-architecture`). One process at 30 Hz holds the 20–32 player target
   comfortably on a core (VALORANT runs 128 Hz on one core as the headroom proof; EVE single-
   shard as the scale ceiling). Stage-1 chunk-region authority + handoff-as-baseline-transfer
   (EVE solar-system→node / Star Citizen authority-swap) is documented for later, built only
   if a load test demands it. Graceful degradation: AOI/rate-tiering first, EVE-style adaptive
   tick ("time dilation," surfaced via the clock's existing `dropped_time_seconds`) last.

7. **Determinism repurposed, not discarded** (all briefs). Authoritative + state-sync needs NO
   cross-machine bit-exactness → drops our heaviest tax. `world_hash`/`NetworkStateHash` stay
   as the server's single-binary replay + save-integrity + desync-DEBUG oracle, off the
   multiplayer hot path. The ONE deliberate `world_hash` bump remains P1/P2 (player avatars +
   server physics become non-empty sim state on the default lane).

---

## Bandwidth / scale budget (consolidated; all flagged ESTIMATE pending load test)

- Per-client downstream ~11–14 kbps at 20 players (AOI + delta + priority budget); aggregate
  ~280 kbps at 20 (`mp-replication`). Physics demo ceiling <256 kbit/s/player at 901 bodies
  (`mp-networked-physics`) — our prop counts are far lower.
- Server cost ≈ O(N · (cells-per-aura + local density)), linear in N, independent of total
  world entities (`mp-interest`). Physics step: low-hundreds active bodies < 33.3 ms on Jolt's
  parallel solver (`mp-networked-physics`).
- Single-process holding: **20–32 players comfortable, ~50–100 plausible with AOI tuned**,
  hundreds-in-one-place out of single-process scope (`mp-server-architecture`).
- **These are the numbers the P5 load test must confirm on the quiet-machine baseline.**

---

## What this changes in the spec (feeds MULTIPLAYER-BLOCKER-SPEC.md v2 §0)

- **P3 (replication) gains a P3.0 prerequisite:** a UDP transport behind the transport seam
  (state over unreliable UDP; reliable channel for events). Then baseline+delta snapshots,
  bit-packing, priority accumulator, client prediction + reconciliation, remote interpolation.
- **AOI is folded into P3** as a thin layer over the chunk index (was implied; now pinned as
  "reuse SHIELD_WorldSystem wanted-set, do not build a second index").
- **P2 physics** confirmed as state-sync (not deterministic lockstep); ownership = server-final.
- **P5 scale** = single-process + AOI tuning + the 20-in-one-chunk "group photo" hotspot load
  test (the top open risk across briefs), with adaptive-tick degradation as the safety valve.
- **Lag-compensation / time-dilation-for-fairness explicitly OUT of v1** (scope discipline).

---

## Top open risks (union across briefs — what the build must watch)

1. **The "group photo" hotspot:** all N players + props in one chunk defeats AOI culling →
   per-snapshot entity cap + rate-tiering, load-tested at 20-in-one-chunk (`mp-interest`,
   `mp-networked-physics`).
2. **UDP transport is net-new** (we only have TCP); reliability layer for events must be
   correct (`mp-replication`).
3. **Giant physics island serialization** under a big prop pile (Jolt LargeIslandSplitter
   limits) — profile (`mp-networked-physics`).
4. **Prediction misprediction smoothing** for full-physics avatars (defer inter-body collision
   to server, smooth corrections, hard-snap threshold for teleports) (`mp-prediction`).
5. **All perf/bandwidth numbers are estimates** — P5 load test on the quiet machine is the
   gate that turns them real.

---

## Citations (primary, consolidated — see each brief for the full list + findings)

- Sanglard, *Quake 3 Network Model* — https://fabiensanglard.net/quake3/network.php
- Frohnmayer & Gift, *The TRIBES Engine Networking Model* —
  https://www.gamedevs.org/uploads/tribes-networking-model.pdf
- Fiedler (Gaffer On Games), *State Synchronization* / *Snapshot Compression* /
  *Networked Physics in VR* — https://gafferongames.com/post/state_synchronization/
- Bernier (Valve), *Latency Compensating Methods* (GDC 2001) + *Source Multiplayer Networking*
  — https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking
- Gambetta, *Client-Side Prediction and Server Reconciliation* —
  https://www.gabrielgambetta.com/client-side-prediction-server-reconciliation.html
- Ford (Blizzard), *Overwatch Gameplay Architecture and Netcode*, GDC 2017 —
  https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and
- Boulanger, Kienzle, Verbrugge, *Comparing Interest Management Algorithms for MMGs*,
  NetGames '06 — https://dl.acm.org/doi/10.1145/1230040.1230069
- Benford & Fahlén, *A Spatial Model of Interaction in Large Virtual Environments*, ECSCW 1993
- CCP Games, *The Server Technology of EVE Online*, GDC — single-shard + time dilation
- Riot, *VALORANT's 128-Tick Servers* — single-core tick-deadline headroom proof
- Jolt Physics — architecture/determinism + island solver — https://github.com/jrouwe/JoltPhysics
