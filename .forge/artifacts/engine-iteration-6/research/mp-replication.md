# Authoritative-Server State Replication — research brief

**Scope:** How a dedicated authoritative server replicates world/entity state to 20+ clients of a persistent
join/leave Luminumbra session, and the concrete encoding/rate/transport we should build on the existing
deterministic 30 Hz sim core. Grounds the P3 leg of `MULTIPLAYER-BLOCKER-SPEC.md` (v2 pivot).

---

## Recommended approach

**Adopt the Quake3/Source lineage: baseline + delta-compressed, interest-scoped entity snapshots over
unreliable UDP, with a small reliable channel for events/RPCs.** Concretely, for Luminumbra:

**What the server sends (downstream, per client, per snapshot):**
- A **per-client delta snapshot** of the ECS state the client is allowed to see, encoded as the *difference*
  between the current server tick state and the **most recent snapshot that client has acknowledged**
  (not necessarily the previous one). This is the Quake3 model exactly: the server keeps a ring of the last
  N snapshots it sent each client, and deltas the live state against the newest acked one. A client that has
  acked nothing (a fresh joiner, or one in deep packet loss) gets a **full snapshot delta'd against the zero
  baseline** — which is how *join* becomes trivial: connect → send a full baseline → resume delta snapshots.
- Replicated set is the **relevant set** for that client only: avatars + dynamic props within an
  **area-of-interest (AOI)** around the client's avatar, intersected with the chunks it has streamed. This is
  Unreal "network relevancy" / Tribes "scope" / Source PVS. A 20-player world must NOT broadcast all 20
  avatars + all props to all 20 clients; AOI is what keeps bandwidth bounded (see budget below).
- When AOI is saturated under a per-client byte budget, use a **priority accumulator** (Tribes/Fiedler): each
  replicated entity carries a float priority that *accumulates each tick it is not sent* (distance-weighted,
  recency-weighted), entities are sorted by accumulated priority, and we pack the highest until the packet
  byte budget is hit; unsent entities keep their accumulator so they are eventually covered. This degrades
  gracefully — "eventual consistency" of the far/low-priority entities, exactness for the near/important ones.

**Encoding (bit-packed, quantized — the load-bearing bandwidth lever):**
- Per-entity, a **changed-field bitmask** then only the changed fields (Quake3 "1 bit per field changed",
  Source SendTable delta). An unchanged entity costs ~1 bit.
- **Quantize** replicated fields, do not send raw floats:
  - Position: fixed-point at the world's voxel scale. Fiedler reaches ~50 bits absolute (18/18/14) at 512
    steps/m; delta-encoded moving positions average ~26 bits, stationary 1 bit. We bound to our chunk/world
    extents and pick step size at the sub-voxel level.
  - Orientation/facing: "smallest-three" quaternion = **29 bits absolute**, ~23 bits delta (Fiedler). Avatar
    facing for a photography game can be coarser still.
  - Velocity: optional; if we interpolate remote entities we can often omit velocity at 30 Hz (Fiedler drops
    it entirely at 60 Hz). Send it only where extrapolation matters (fast props).
- Reuse the existing `ILockstepTransport` wire discipline: explicit little-endian field shifts, length-prefixed
  blobs, **no struct padding on the wire** — but at the *bit* level, not byte level, for the entity payload.

**Rate (decouple snapshot rate from tick rate — the Source insight):**
- Server simulates at the canonical **30 Hz** fixed tick (unchanged `SimulationClock`).
- Snapshot send rate is a **separate, lower, configurable cadence**, e.g. **15–20 snapshots/s** per client
  (Source ships ~20–66 Hz updaterate; Quake3 ~20 Hz). Clients **interpolate** remote entities between the two
  most recent snapshots (a ~100 ms interpolation buffer ≈ 2 snapshots), so a 15–20 Hz snapshot stream renders
  smoothly. This halves downstream bandwidth versus snapshotting every tick for free.
- Upstream: each client sends a **quantized `usercmd` input** (movement axes fixed-point, action bits; look is
  render-side, never replicated) at tick cadence, with the **last-acked-snapshot sequence** piggybacked as the
  ack. The client **predicts** its own avatar locally from its own usercmds and **reconciles** when the
  authoritative snapshot for that tick arrives (replay un-acked inputs from the corrected state).

**Transport:** **Move state replication to UDP.** Snapshots are most-recent-wins: a dropped snapshot is simply
superseded by the next one delta'd against an older acked base — exactly why Quake3/Source/Tribes all run state
over UDP, never TCP. Keep a thin **reliable-ordered channel** (can stay TCP short-term, or a reliability layer
*over* the same UDP socket) for events/RPCs that must arrive (chat, spawn/despawn/join/leave, world edits,
inventory). See transport assessment below.

---

## Alternatives considered (and why rejected)

| Approach | Verdict | Why |
|---|---|---|
| **Delay-based peer lockstep** (current `LockstepSession`) | **Rejected for 20+ multiplayer** (kept for replay/loopback) | Stalls on slowest peer (one of 20 hiccups → freezes all); no mid-session join (every peer must run from tick 0); requires bit-exact *cross-machine* determinism — a perpetual tax that buys nothing a Garry's-Mod server needs. Already the v2 spec's conclusion. Fiedler: lockstep bandwidth is input-sized (great) but needs determinism "exact down to the bit-level… fragile" across compilers/OS/arch. |
| **TCP full-state streaming** | Rejected | TCP head-of-line blocking: one lost segment stalls *all* later snapshots until retransmit, injecting latency precisely when the network is bad. State is most-recent-wins, so retransmitting a stale snapshot is wasted work. This is the explicit Quake3 rationale ("reliable transmission introduces unacceptable latency"). |
| **Full snapshot every tick, no delta, no AOI** | Rejected | Does not scale. Fiedler's uncompressed 60 Hz full snapshot = **17.38 Mbps**; even our 901-object toy at 30 Hz blows any budget. Delta + AOI is what gets it to ~15 kbps–256 kbps. |
| **Pure eventual-consistency / CRDT replication** | Rejected as the primary model | Strong for unbounded sandbox object counts, but adds convergence complexity and weakens the "server is the single authority" guarantee we want for Garry's-Mod physics. We *borrow* its eventual-consistency idea only for low-priority far entities via the priority accumulator, under a hard authority. |
| **Tribes Networked Object Model wholesale** | Adopted in spirit, not verbatim | Its ghost-manager/scope/most-recent-state/priority design IS our model; we don't reimplement its 1998 modem-era stream layer (3-byte sliding-window ack overhead, moves-thrice) literally — we take the architecture and modern bit-packing. |

---

## Perf / bandwidth budget (20+ players)

All figures below are **ESTIMATES** derived from the cited primary-source per-entity costs; **none are measured
on Luminumbra yet** — P5 must instrument the real per-client byte counter. Flagged inline.

**Per-entity downstream cost (ESTIMATE, from Fiedler smallest-three + delta):**
- Moving avatar: ~26 bits position + ~23 bits orientation + ~6 bits action/state + id/bitmask overhead
  ≈ **~64 bits ≈ 8 bytes** per replicated moving entity per snapshot.
- Stationary/idle entity in AOI: ~1–8 bits (changed bitmask says "no change") ≈ **~1 byte**.

**Per-client downstream (ESTIMATE):** suppose AOI exposes ~15 other entities (avatars + props), ~half moving:
- ~8 entities × 8 B + ~7 × 1 B ≈ **~71 B** entity payload + ~20 B headers ≈ **~90 B/snapshot**.
- At **15 snapshots/s**: ~1.35 KB/s = **~11 kbps/client**. At 20 snapshots/s: ~14 kbps. This is in the right
  ballpark as Fiedler's stationary-scene **~15 kbps** and well under his **256 kbps** working target.

**Server upstream aggregate (ESTIMATE) at 20 players:** 20 × ~14 kbps ≈ **~280 kbps ≈ 35 KB/s out**. Trivial on
a wired/LAN host; comfortable on a modest VPS. The cost that grows is **per-client snapshot *assembly* CPU**
(delta + AOI cull per client per snapshot) — O(players × relevant-entities), which is why AOI/priority caps the
relevant set rather than the world.

**What keeps it bounded:** (1) **delta** — unchanged entities ≈ 1 bit; (2) **AOI/relevancy** — a client pays only
for entities near *it*, so cost scales with local density not world population; (3) **priority accumulator under
a per-packet byte budget** — a hard ceiling per client regardless of how many entities are in view, with far
entities degrading to lower refresh, never to a bandwidth blowout. Snapshot-rate < tick-rate halves it again.

**Hard rule to enforce in P5:** a fixed **per-client byte-per-snapshot budget** (e.g. 1100 B → ~17 kbps at
15 Hz). The priority accumulator fills up to that budget and stops. Bandwidth is therefore **O(budget × clients)
by construction**, independent of total entity count — the property we need for 20+.

---

## Determinism implications

**Authoritative replication does NOT require cross-client bit-exactness.** This is the single biggest
simplification of the pivot and it is a primary-source fact, not an opinion: clients never run the authoritative
world, so two clients diverging in floating-point is impossible-by-construction — they only *display* what the
server sends and *predict* their own avatar (a misprediction is corrected by reconciliation, not a desync).
Fiedler states the lockstep determinism bar is "exact down to the bit-level… fragile" across toolchains; dropping
that requirement removes the heaviest perpetual tax we currently carry.

**What the server still uses determinism for (keep all of it):**
- **`world_hash` / sub-hashes** remain the server's **replay, save-integrity, and debug** invariant — the same
  three-input contract `(seed, preset, ordered inputs)` still reconstructs a server session via LREC1. It is no
  longer a *transport* requirement, but it is still how we prove the server sim is reproducible and how we
  triage a server-side bug.
- **Reproducible avatar/physics state on the server** so a recorded input stream replays identically server-side
  (regression gates, crash repro). Per the spec, the moment player avatars + server Jolt physics enter the
  default lane, the `entities`/physics sub-hash stops being empty — that is **one deliberate `world_hash` bump**,
  landed in its own commit with the heavy-oracle + LREC1 + lockstep re-bless (spec §4 bump #5).
- **Client-side prediction** of the local avatar should use the *same* movement integration the server runs so
  prediction error stays tiny; it need not be bit-exact (reconciliation absorbs the residual), so it can use the
  client's normal float path — only the *server* needs the deterministic guarantee.

Net: we **keep `world_hash` as an internal tool, drop cross-machine determinism as a runtime requirement.**

---

## Integration notes (our files)

- **`ILockstepTransport` (`src/luminumbra_common/net/LockstepSession.h`)** — keep the *framing discipline*
  (length-prefixed, no wire padding, explicit LE shifts) as the encoding house-style for the new protocol. Add a
  sibling **`IReplicationTransport`** (or extend the seam) with an **unreliable datagram** send/recv for snapshots
  and a **reliable-ordered** channel for events. The existing `LoopbackTransport` pattern (in-process paired
  queues, no sockets) is the model for a `LoopbackReplicationTransport` so P3/P4 gates run with no real ports.
  The `TcpTransport` reliable path can back the reliable/event channel initially; a UDP datagram impl is the new
  additive work for the snapshot channel.
- **`LockstepSession`** — **park, do not delete** (spec §0). It stays the deterministic 2-peer replay/loopback
  tool. The new replication path is a *separate* class (e.g. `ReplicationServer` + `ReplicationClient`), not a
  generalization of `LockstepSession` — they are different models.
- **`ServerWorldRunner` (`src/luminumbra_server/ServerWorldRunner.h`)** — the snapshot producer. It already owns
  the authoritative `GameSession` + 30 Hz loop. Add: per-client snapshot ring (last N sent), per-client ack
  tracking, the AOI cull (it already takes avatar positions as the streaming-anchor vector — reuse those as AOI
  centers), the priority accumulator, and the delta encoder over the ECS snapshot. Snapshot send is a *separate
  cadence* from `RunFixedTicks` — drive it from a snapshot-tick divisor of the sim tick.
- **ECS / `entt::registry` (`GameSession::GetRegistry()`)** — the replicated set is a subset of registry
  components. Tag replicated components (a `Replicated` marker + a per-component field-quantization spec) so the
  delta encoder is data-driven (Source SendTable analogue). The `PlayerAvatar` entity from spec P1 is the first
  replicated type; props (P2) follow.
- **`GameSession::TickSimulation`** — unchanged authority. The `usercmd` upstream applies to each avatar inside
  the tick (replaces the empty `LockstepHooks` input blob); the server is the only place inputs are applied.
- **`NetworkStateHash` / `world_hash`** — repurposed to internal replay/save/debug only (see Determinism).

---

## Open risks

1. **UDP build-out cost.** We have no UDP transport today (winsock2 TCP only). The snapshot channel needs a UDP
   datagram socket + sequence/ack + a minimal reliability layer for events. Non-trivial but well-trodden;
   loopback-first keeps gates port-free.
2. **Per-client CPU at 20+.** Snapshot assembly is O(clients × relevant entities) per snapshot cadence. AOI caps
   relevant entities; still must be measured (P5) — a 20-client × full-AOI worst case could dominate the server
   tick budget. Mitigation: shared baseline computation, dirty-flagging unchanged entities once per tick.
3. **Prediction/reconciliation correctness** for server-authoritative Jolt physics (Garry's-Mod model). Player
   prediction of physics-driven motion (pushed by props, collisions) is the hard case — Source limits prediction
   to the local player's own movement and accepts visible correction for physics interactions. Same posture
   recommended; flag heavy reconciliation snapping as a tuning item.
4. **Interest-management correctness vs. streaming.** AOI must intersect with what the client has actually
   streamed (don't replicate an entity in a chunk the client lacks). Tie AOI to the multi-anchor streamed set.
5. **Budget figures are estimates.** Every kbps/byte number here is derived from cited per-entity costs, **not
   measured on Luminumbra**. P5 must add a per-client byte counter and re-derive the budget from real entity
   field counts and AOI densities before claiming a 20-player number.
6. **Event reliability semantics.** Deciding which messages are reliable-ordered (join/leave, world edits, chat)
   vs. unreliable (state) is a design surface; mis-classifying state as reliable reintroduces the TCP latency
   problem on UDP.

---

## Citations

1. **Quake 3 Network Model — Fabien Sanglard.** https://fabiensanglard.net/quake3/network.php — Canonical
   primary writeup: server keeps the **last 32 snapshots** per client in a ring and **delta-compresses the live
   state against the last *acked* snapshot** (not necessarily the previous), 1 bit/field-changed marker, all over
   **UDP** ("reliable transmission introduces unacceptable latency"), Huffman-compressed, fragmented to 1400 B.
2. **The TRIBES Engine Networking Model — Mark Frohnmayer & Tim Gift.**
   https://www.gamedevs.org/uploads/tribes-networking-model.pdf (full text:
   https://archive.org/stream/tribes-networking-model/tribes-networking-model_djvu.txt) — Defines the four
   delivery classes (**unguaranteed, guaranteed, guaranteed-quickest, most-recent-state**), the **ghost manager +
   scope** (only in-scope objects replicate, at a rate set by **priority + state mask** for partial updates), and
   packet-delivery-notification ack over UDP (~3 B/packet overhead). Example: **10 packets/s × 200 B ≈ 2 KB/s**
   on a 28.8k modem. This is our architecture's direct ancestor.
3. **Snapshot Compression & State Synchronization — Glenn Fiedler (Gaffer On Games).**
   https://gafferongames.com/post/snapshot_compression/ and
   https://gafferongames.com/post/state_synchronization/ — The concrete bit-packing/budget numbers we size
   against: **smallest-three quaternion = 29 bits** (~23 delta), position ~50 bits absolute / **~26 bits delta**
   (1 bit if unchanged), uncompressed 60 Hz full snapshot = **17.38 Mbps** → delta brings a stationary scene to
   **~15 kbps** under a **256 kbps** target; the **priority accumulator** packs the highest-priority entities into
   a fixed **per-packet byte budget** (e.g. 64 updates/packet) for graceful degradation.
4. **Source Multiplayer Networking & Networking Entities — Valve Developer Community.**
   https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking ·
   https://developer.valvesoftware.com/wiki/Networking_Entities — Tick rate **decoupled** from snapshot/update
   rate (`cl_updaterate`/`cl_cmdrate`); **baseline + delta** snapshots (only changes since last acked update;
   per-changed-property: name/type/SendTable index/**bits used**/value); **full snapshots only on connect or
   heavy packet loss**; client prediction + interpolation + lag compensation; UDP.
5. **Actor Relevancy and Priority / Replication Graph — Unreal Engine (Epic).**
   https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-priority-in-unreal-engine ·
   https://docs.unrealengine.com/en-US/Gameplay/Networking/Actors/Relevancy/index.html — The server replicates
   only each client's **relevant set**; under saturation a **distance- and recency-weighted float priority**
   (`GetNetPriority`, multiplied by time-since-last-replicated to avoid starvation) load-balances bandwidth — an
   actor at priority 2.0 updates twice as often as 1.0. This is the modern production form of our AOI + priority
   accumulator.
6. **Deterministic Lockstep — Glenn Fiedler.** https://gafferongames.com/post/deterministic_lockstep/ — Why we
   pivot away from lockstep as the *transport*: it needs determinism "exact down to the bit-level… fragile"
   across compilers/OS/architecture; state replication is preferable "when achieving bit-level determinism across
   platforms is impractical" — exactly our 20-player/persistent/physics case.
