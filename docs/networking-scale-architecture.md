# Networking Scale Architecture — Server-Authoritative Delta Replication as the 20-32 Player Path

> Status: ARCHITECTURE NOTE (created 2026-06-27). Implements spec 019 FR-A (Group A —
> "server-authoritative delta replication as THE scale path"), see
> `docs/specs/019-networking-scale-out/spec.md`. This document is the canonical statement
> that **`ReplicationServer` / `ReplicationClient` delta replication — not lockstep — is the
> 20-32+ player session path**. Every claim below is anchored to a real symbol in
> `src/luminumbra_common/net/`, verified against the tree this session (line numbers are the
> post-edit final-tree values, including the two scale-path comment markers added with this note).

## TL;DR

For a 20-32+ player session, the engine runs **one authoritative `ReplicationServer`**
(`src/luminumbra_common/net/ReplicationEndpoint.h:40`) that fans out per-client snapshots to
many `ReplicationClient`s (`:141`). Three properties make this the scale path:

1. **Delta-vs-acked compression** — each client is sent only what changed since the snapshot it
   last ACKed (`ReplicationDelta.h:26` `MakeSnapshotDelta` / `:65` `ApplySnapshotDelta`), the
   "bandwidth win that makes 20-32+ players affordable" (`ReplicationDelta.h:3-6`).
2. **AOI-bounded egress** — chunk-index area-of-interest (`SetAoiChunkRadius`,
   `ReplicationEndpoint.h:71`) bounds each client's snapshot by **local entity density, not
   headcount**, so per-connection bytes stay flat as N grows.
3. **Clean disconnect/reconnect** — `PruneDisconnectedClients`
   (`ReplicationEndpoint.h:53`, impl `ReplicationEndpoint.cpp:23`) drains-then-prunes a leaver
   and folds its avatar despawn into the next broadcasts; a rejoiner re-baselines from a full
   snapshot.

**Lockstep is NOT this path.** `LockstepSession` is retained as a determinism oracle / replay
tool / small (<=4) co-op mode only (spec 019 Group B; cross-ref spec 018 determinism). It is
hard-coded to one remote ("v1 scope: <= 2", `LockstepSession.h`) and HALTs the whole session on
a single hash mismatch — both fatal at 20-32. Delta replication is unreliable-snapshot,
most-recent-wins, and never shared-fate-stalls on one slow peer.

## Determinism contract (observer-only, NOT render-exempt)

Networking touches the sim, so the "render-only, can't move the hash" exemption used by spec 015
does **not** apply here. What holds instead:

- Replication is a **read-only observer** of authoritative sim state. `BroadcastSnapshot`
  (`ReplicationEndpoint.h:96`) *consumes* the entity set the sim hands it and **never feeds**
  `world_hash`. The header comment states this invariant: "Engine-generic + world_hash-neutral:
  this is render/transport-side glue" (`ReplicationEndpoint.h:19`).
- The lockstep desync oracle stays green. `luminumbra_server_app --smoke` must remain
  `6f008a9f637c40b7`, run==replay. The doc/comment edits that accompany this note are
  comment-only and behavior-neutral.

Cross-ref: `docs/specs/018-determinism-hardening/spec.md` owns the determinism contract this path
must not regress.

## The server: `ReplicationServer`

`src/luminumbra_common/net/ReplicationEndpoint.h:40` — one per dedicated server.

- **Multi-client fan-out.** `AddClient(client_id, transport)` (`:43`, impl
  `ReplicationEndpoint.cpp:13`) registers each connected player on its own
  `ILockstepTransport`. Clients live in an **ordered** `std::map` (`m_clients`, `:124`) so the
  broadcast order is deterministic. `client_count()` (`:45`) / `has_client()` (`:46`) expose the
  roster.
- **One broadcast per tick.** `BroadcastSnapshot(server_tick, entities, removed_ids)` (`:96`,
  impl `ReplicationEndpoint.cpp:41`) builds a `SnapshotMsg` per client, each with its own
  monotonically increasing `snapshot_seq` (`ClientLink::next_snapshot_seq`, `:118`) and
  `acked_usercmd_tick`.
- **Inbound drain.** `PumpInbound()` (`:101`, impl `ReplicationEndpoint.cpp:183`) drains Usercmd
  (newest-wins) and Ack (monotonic) frames non-blocking from every client.
  `LatestUsercmd(client_id)` (`:104`) feeds the sim; `AckedSnapshotSeq(client_id)` (`:105`)
  drives the delta baseline.

## Property 1 — Delta-vs-acked is the default at scale

`ReplicationDelta.h:3-6` states delta-vs-acked compression is "the bandwidth win that makes
20-32+ players affordable". The mechanism (`ReplicationEndpoint.cpp:132-159`):

- When delta is ON (`SetDeltaCompression(true)`, `ReplicationEndpoint.h:89`; `m_delta`, `:125`),
  the server looks up the baseline the client last ACKed (`link.inbound.acked_snapshot_seq()`,
  used at `ReplicationEndpoint.cpp:140`) in `ClientLink::sent_history` (`ReplicationEndpoint.h:122`)
  and sends `MakeSnapshotDelta(baseline, current)` (`ReplicationDelta.h:26`) tagged with
  `delta_from_seq = acked` (`ReplicationEndpoint.cpp:145`).
- With no usable acked baseline yet, the server sends a **full** snapshot
  (`delta_from_seq == 0`, `ReplicationEndpoint.cpp:148`) — this is how a fresh or rejoining client
  re-baselines.
- **Loss-tolerant by construction.** The server keeps deltaing against the *last-acked* baseline
  until a newer ack arrives (`ReplicationDelta.h:8-13`), so a dropped delta is recovered by the
  next one — no stranded client. The client reconstructs via `ApplySnapshotDelta`
  (`ReplicationDelta.h:65`, applied at `ReplicationEndpoint.cpp:307`); a frame whose baseline was
  pruned under heavy loss is simply dropped and re-converged by a later frame
  (`ReplicationEndpoint.cpp:305-307`).
- **No wire-format change.** Delta is a pure function over `SnapshotMsg` (`ReplicationDelta.h:8`);
  the on-wire encoder `EncodeSnapshot` (`ReplicationProtocol.cpp:103`) carries `delta_from_seq`
  as a regular `u32` field (`ReplicationProtocol.cpp:107`), so full and delta frames share one
  format. Baseline retention is bounded by `kServerHistoryCap = 256` (`ReplicationEndpoint.h:126`)
  on the server and `kClientHistoryCap = 256` (`:166`) on the client.

**Scale-path default.** Today the field defaults OFF (`m_delta = false`,
`ReplicationEndpoint.h:125`) so the canonical P3.0 full-snapshot baselines hold bit-exact. The
20-32 scale path **enables delta** (`SetDeltaCompression(true)`) at session setup; the
loss-tolerant re-delta loop is the `ReplicationDeltaLoop_test` contract
(`test/common/ReplicationDeltaLoop_test.cpp`, registered in `test/CMakeLists.txt`; spec 019
FR-A-002 / AC-A-002).

## Property 2 — AOI bounds per-client egress

Two AOI modes scope each client's snapshot to its own area of interest (the entity whose
`entity_id == client_id` is the centre, and is **always** included):

- **Chunk-index AOI (the scale mode).** `SetAoiChunkRadius(chunk_radius, chunk_size_mm)`
  (`ReplicationEndpoint.h:71`) buckets every entity by its horizontal (X/Z) streaming chunk
  **once** per broadcast (`ReplicationEndpoint.cpp:67-75`), then each client gathers only the
  `(2r+1)^2` chunk neighbourhood of its own avatar's chunk (`ReplicationEndpoint.cpp:91-110`).
  Cost is `O(E + clients * (2r+1)^2)` instead of the mm-radius path's `O(clients * E)` — the
  comment at `ReplicationEndpoint.h:63-70` documents this. Takes precedence over the mm radius.
- **mm-radius AOI (fallback).** `SetAoiRadiusMm(radius_mm)` (`ReplicationEndpoint.h:60`) box-culls
  then does the squared int64 distance compare (`ReplicationEndpoint.cpp:111-126`).

**The bounded-egress property.** With chunk-AOI on, `last_broadcast_max_client_bytes`
(`ReplicationEndpoint.h:112`) stays ~flat as N grows while a full-set broadcast grows linearly.
This is exactly what the `ReplicationScale` test asserts at 64 players
(`test/common/ReplicationEndpoint_test.cpp:257`, doc block `:250-256`): 64 players on an 8x8 grid
100 m apart with a 16 m chunk + radius 1 → each client's snapshot stays ~1 entity regardless of
population (`EXPECT_LT(aoi_max * 8, full_max)` and `aoi_total * 8 < full_total`,
`ReplicationEndpoint_test.cpp:286-287`), and re-broadcasting identical state is byte-identical
(`:289-293`). Spec 019 FR-A-001 promotes this from "stress evidence" to a **required scale gate**
(`ctest -R ReplicationScale`).

Telemetry: `last_broadcast_total_bytes()` (`ReplicationEndpoint.h:111`) and
`last_broadcast_max_client_bytes()` (`:112`) report the MEASURED per-broadcast egress (the T-I6
P5 mechanism); the 32-client baseline is re-recorded against these (spec 019 NFR-004).

## Property 3 — Clean disconnect / reconnect at scale

- **Drain-then-prune.** `PruneDisconnectedClients()` (`ReplicationEndpoint.h:53`, impl
  `ReplicationEndpoint.cpp:23`) removes any client whose transport reports
  `IsPeerConnected() == false` (`ReplicationEndpoint.cpp:26`) and returns the removed ids so the
  caller can despawn those avatars. The header comment is explicit: call it *after* `PumpInbound`
  so a peer's queued frames are drained first — "Drain-then-prune = a clean leave, not a desync"
  (`ReplicationEndpoint.h:48-53`).
- **Prune-into-tick despawn.** Pruning enqueues the leaver's avatar id (id == client_id) into
  `m_pending_removed_ids` with a repeat count `kRemovalRepeatBroadcasts = 3`
  (`ReplicationEndpoint.cpp:32`; field `ReplicationEndpoint.h:137`, constant `:138`). The next
  `BroadcastSnapshot` folds those ids into `removed_ids` (`ReplicationEndpoint.cpp:47-57`) and
  repeats them across a few unreliable snapshots so a dropped despawn can't leave a ghost
  (decay loop `ReplicationEndpoint.cpp:174-180`). Verified by
  `ReplicationLifecycle.PruneFoldsDespawnIntoNextSnapshot`
  (`test/common/ReplicationEndpoint_test.cpp:298`).
- **Rejoin re-baselines.** A reconnecting client is re-added via `AddClient`, starting with an
  empty `sent_history`; its first snapshot is therefore full (`delta_from_seq == 0`,
  `ReplicationEndpoint.cpp:147-148`), so it re-baselines cleanly without any stale delta.
- **No shared-fate stall.** Each client is an independent `ClientLink` over its own transport;
  one leaver/slow peer being pruned or behind never blocks the others' broadcasts (spec 019
  NFR-003). The over-the-wire reconnect-under-load matrix is spec 019 FR-C-005 / AC-C-005.

## Why lockstep is NOT the scale path

`LockstepSession` (`src/luminumbra_common/net/LockstepSession.h`) stays as a **determinism oracle
/ replay tool / small (<=4) co-op** path, not the 20-32 path (spec 019 Group B):

- It is hard-coded to one remote (`LockstepConfig` "v1 scope: <= 2"); every transport
  (TCP / GNS / Steam) "accepts a single connection".
- A single hash mismatch HALTs the whole session (`TickOutcome::Desync`) — and the slowest peer
  gates everyone. At 20-32 that is a guaranteed stall/HALT.
- Its value is the desync oracle (periodic hash exchange) wired into spec 018's determinism gate;
  that is preserved unchanged. Demotion is documentation + routing, not deletion.

Delta replication has none of these properties: snapshots are unreliable + most-recent-wins, each
client is independent, and a behind client is recovered by the next delta rather than halting the
session.

## Transport seam (unchanged)

The replication endpoints run **unchanged** over any `ILockstepTransport`
(`LoopbackTransport` for tests; TCP / GNS / Steam SDR for real sessions) — the seam declared in
`LockstepSession.h` and pulled in at `ReplicationEndpoint.h:34`. Snapshots are sent
`FrameDelivery::Unreliable` (`ReplicationEndpoint.cpp:168`), usercmds and acks likewise
(`ReplicationEndpoint.cpp:218`, `:323`), because most-recent-wins makes a dropped frame
self-healing. Scale validation over real transports (multiprocess soak, loss/jitter/reorder,
32-client) is spec 019 Groups C/F; this note covers the in-process-proven replication core that
those gates exercise over the wire.

## Verification

- **Presence:** this file exists and references the real symbols `ReplicationServer`,
  `ReplicationClient`, `BroadcastSnapshot`, `MakeSnapshotDelta`, `ApplySnapshotDelta`,
  `SetAoiChunkRadius`, `last_broadcast_max_client_bytes`, `PruneDisconnectedClients`.
- **Behavior (existing gate):** `ctest -R ReplicationScale` — the 64-player AOI-bound contract
  (`test/common/ReplicationEndpoint_test.cpp:257`) that this note documents as the scale property.
- **Determinism:** `luminumbra_server_app --smoke == 6f008a9f637c40b7`, run==replay (unchanged;
  this note + the comment edits are observer-only / comment-only).

## Key files

- `src/luminumbra_common/net/ReplicationEndpoint.h` / `.cpp` — `ReplicationServer` /
  `ReplicationClient`, AOI, delta dispatch, prune/despawn, telemetry.
- `src/luminumbra_common/net/ReplicationDelta.h` — `MakeSnapshotDelta` / `ApplySnapshotDelta`
  pure delta functions.
- `src/luminumbra_common/net/ReplicationProtocol.{h,cpp}` — `SnapshotMsg` / `UsercmdMsg` /
  `AckMsg` wire format (`delta_from_seq` carried at `ReplicationProtocol.cpp:107`).
- `test/common/ReplicationEndpoint_test.cpp` — `ReplicationScale` (`:257`), lifecycle/AOI tests.
- `test/common/ReplicationDeltaLoop_test.cpp` — loss-tolerant re-delta loop (spec 019 AC-A-002).
- `docs/specs/019-networking-scale-out/spec.md` — the parent spec (FR-A this note implements).
- `docs/specs/018-determinism-hardening/spec.md` — the determinism contract this path preserves.


# Networking Scale-Out Architecture

> Status: living architecture note (created 2026-06-27). Owner of the multiplayer-scale
> contract is **spec 019** (`docs/specs/019-networking-scale-out/spec.md`). The
> determinism contract this layer must never regress is **spec 018**
> (`docs/specs/018-determinism-hardening/`): `luminumbra_server_app --smoke` must stay
> `6f008a9f637c40b7`, run==replay.
>
> **Scope of this revision (track 019-B1 / FR-B).** This commit creates the doc and adds
> the **Lockstep** section only. The **server-authoritative delta replication** scale-path
> content (spec 019 Group A — `ReplicationServer`/`ReplicationClient`, AOI, delta-vs-acked,
> the 32-client soak) is owned by the Group-A track and must be merged in as an *additional*
> section here, NOT as a competing rewrite of this file.

## The two networking paths (and which is which)

Luminumbra has two distinct networking subsystems that are easy to conflate but serve
opposite purposes:

| Path | Role | Scale target |
|------|------|--------------|
| **Server-authoritative delta replication** (`ReplicationEndpoint.h`) | THE multiplayer session path | 20-32+ players |
| **Lockstep** (`LockstepSession.h`) | Determinism oracle / replay tool / small co-op | <=4 (documented cap); 2-peer today |

The replication path is the scalable one. Lockstep is **not** the scale path — see below.
(The replication scale-path section is authored by the Group-A track; see the scope note.)

## Lockstep: Determinism Oracle / Replay / Small Co-op — NOT the scale path

Lockstep (`src/luminumbra_common/net/LockstepSession.h`) is a **delay-based** (1500-Archers
model, not rollback) transport that drives every peer through the *same* ordered per-tick
input stream on the 30 Hz `SimulationClock`. Per owner decision (spec 019, FR-B), lockstep is
**demoted** from any implied "path to multiplayer at scale" to three explicit roles:

1. **Determinism oracle** — peers exchange authoritative world hashes and HALT on divergence
   (the load-bearing value; preserved verbatim, see below).
2. **Replay tool** — the desync dump is an LREC1 stream identical in shape to the `--replay`
   path, so existing replay tooling re-runs it.
3. **Small co-op mode** — a low-headcount, fully-deterministic shared session.

**Why lockstep is NOT the 20-32 path.** At scale the *slowest peer and the most divergent
streamed state dominate the whole session*: the tick gate does not advance until every peer's
input for the wanted tick has arrived (one slow client stalls everyone), and a *single* hash
mismatch HALTs the entire session (`TickOutcome::Desync`, `LockstepSession.h:251`). Our world
tick includes chunk streaming, water, and physics, so the shared-fate stall and the all-or-
nothing HALT make lockstep-at-scale a non-goal (spec 019, NG-2). Headcounts above the co-op
cap route to the replication path.

### Supported co-op peer cap (explicit)

- **Documented small-co-op cap: `<= 4` peers** (spec 019, FR-B-003). This is the *intended*
  cap and is owner-gated by Open Question OQ-1 (does the product ever need full lockstep at
  20-32? assumed no → lockstep stays oracle/replay/small-co-op permanently).
- **Current implementation is strictly 2-peer / ONE remote.** `LockstepConfig` hard-codes
  `local_client_id = 0` / `peer_client_id = 1` with the comment "v1 scope: <= 2"
  (`LockstepSession.h:340-341`); the class doc says it "Drives one end of a 2-peer (<=2
  clients, ONE remote) ... session" and states the oracle/replay/small-co-op scope
  (`:413-416`). Raising the cap from 2 to 4 is future work under OQ-1, not a present capability.
- **Any count above the documented cap routes to server-authoritative delta replication**, not
  to a wider lockstep session.

### Determinism-oracle value — PRESERVED, unchanged

The demotion is **documentation + routing only, not deletion** (spec 019, FR-B-002). The
desync-oracle hash exchange is untouched and stays wired to spec 018's determinism gate:

- **What is exchanged.** `HashMsg` carries the checkpoint `tick`, the combined `world_hash`,
  and the authoritative sub-hashes `terrain` / `water` / `entities` (`LockstepSession.h:91-97`).
  The render **mesh is deliberately excluded** — it is a derived render artifact, not
  authoritative state (`:88-90`), matching the `world_hash` exclusion elsewhere in the engine.
- **Cadence.** Hashes are captured, sent, and compared at the 30-tick checkpoint cadence
  (`hash_cadence_ticks = 30`, `LockstepSession.h:244`), the same cadence the LREC1 replay uses,
  so the dump's checkpoints line up with a recorded replay's
  (`LockstepSession.cpp:681-701`).
- **Compare + localization.** On receiving a peer hash the session compares immediately if it
  already holds the local hash for that tick; on a `world_hash` mismatch it localizes the
  divergence to a section — `terrain`, then `water`, then `entities`, falling back to
  `world_hash` (mesh / non-authoritative) — before halting (`LockstepSession.cpp:544-560`).
- **HALT + repro artifact.** A mismatch yields `TickOutcome::Desync` (`LockstepSession.h:251`);
  the session HALTs and `EmitDesyncDump` writes an LREC1 stream of the local session up to the
  divergence tick, carrying the local checkpoint hashes and the divergent tick + section
  (`LockstepSession.cpp:712`). This is the desync-repro artifact, re-runnable by the existing
  `--replay` tooling.

Because none of the adaptive-horizon / latency machinery feeds the world hash (the simulation
depends only on `seed`, `preset`, and the ordered per-tick inputs — `LockstepSession.h:20-27`),
the oracle remains a sound cross-peer determinism check regardless of timing. Keeping it green
is a hard gate for spec 018; treat any move of `--smoke` (`6f008a9f637c40b7`) as a real
regression, never a re-bless.
