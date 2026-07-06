# Spec 019: Networking Scale-Out — Server-Authoritative Delta Replication as the 20-32 Player Framework

> Status: SPEC (created 2026-06-26). Derived from the engine-infrastructure devil's-advocate
> critique (`.forge/critique-engine-infrastructure-framework-20260626-200421.md`, finding F6).
> Subject: turn the networking layer from a useful set of seams (`ILockstepTransport`,
> `ReplicationDelta`, frame delivery) that is currently proven only at two-peer / local-loopback
> scope into a **proven 20-32 player networking framework**. Owner direction (locked):
> **(1) server-authoritative DELTA REPLICATION is THE scalable multiplayer path; (2) LOCKSTEP is
> demoted to a determinism oracle / replay tool / small-co-op mode; (3) validation is local
> MULTIPROCESS soak (single-PC constraint) over TCP + GameNetworkingSockets, with Steam SDR an
> explicit deferred-to-hardware item.** Anchors below were verified against the current tree this
> session (drifted citations corrected — see Verification).

## The framing insight (why this spec exists)

The networking layer has the *right seams* but its proven scope is still two-peer. The implementation
evidence is closer to local-loopback than to a 20-32 player session:

- **Lockstep is hard-coded to one remote.** `LockstepConfig` fixes `local_client_id=0` /
  `peer_client_id=1` and documents "v1 scope: <= 2" (`LockstepSession.h:227-228`); the class doc
  says it "Drives one end of a 2-peer (<=2 clients, ONE remote) ... session" (`LockstepSession.h:300`);
  the tick gate waits only for "the one remote" (`LockstepSession.cpp:600`). Lockstep at 20-32 is
  *especially* risky because the **slowest peer and the most divergent streamed state dominate the
  session** — one slow client stalls everyone, and a single hash mismatch HALTS the whole session
  (`TickOutcome::Desync`, `LockstepSession.h:251`).
- **All transports mirror the one-peer v1 shape.** TCP is "loopback + LAN scope, ONE remote ...
  Listen accepts a single connection" (`LockstepSession.h:185`); Steam "v1 scope mirrors TcpTransport:
  direct IP connect, ONE peer per transport (Listen accepts a single connection)"
  (`SteamNetworkingTransport.h:3-12`); GNS is the "locally-testable real-UDP path for dev ... two
  processes can connect on ONE machine" (`GnsTransport.h:3-7`).
- **The TCP send path busy-spins on backpressure.** `TcpTransport::SendFrame` loops on
  `WSAEWOULDBLOCK` with a bare `continue;` (`LockstepSession.cpp:378`) on a non-blocking socket
  (set at `:362`). Under a 32-client egress burst this is a CPU spin, not bounded backpressure.
- **Replication is *designed* for 20-32+ but proven only in-process.** `ReplicationDelta.h:3-6`
  states delta-vs-acked compression is "the bandwidth win that makes 20-32+ players affordable";
  `ReplicationEndpoint` already has multi-client fan-out (`AddClient`, `BroadcastSnapshot`), AOI
  (`SetAoiRadiusMm` / `SetAoiChunkRadius`, `ReplicationEndpoint.h:54-69`), delta-vs-acked baselines,
  and bandwidth telemetry (`last_broadcast_total_bytes` / `last_broadcast_max_client_bytes`,
  `:102-103`). Tests cover multi-client seq (`ReplicationEndpoint_test.cpp:78`), AOI scoping
  (`:142-145`), and a 64-player AOI-bound stress contract (`ReplicationScale` TEST,
  `ReplicationEndpoint_test.cpp:257`, doc block `:250-256`). **But these are all `LoopbackTransport`
  in one process — NOT a real Steam/GNS multiprocess soak.**

**The cost of inaction:** the network layer passes every local deterministic test today while
**failing the first real large session** through send-spin stalls, unbounded per-client backpressure,
desync HALTs (lockstep), or transport-specific behavior that loopback never exercises. This spec
promotes the replication path that is *built* for scale to the *proven* 20-32 path, kills the busy-spin,
makes backpressure and snapshot aging first-class metrics, and stands up a real transport test matrix
under the single-PC constraint.

**Determinism (honest framing — this is NOT render-only).** Networking touches the sim, so the
015-style "render-only, can't move the hash" claim does **not** apply. What holds: the sim stays
deterministic and replication is a **read-only observer** of authoritative sim state —
`BroadcastSnapshot` *consumes* the entity set and *never feeds* `world_hash`. The lockstep
determinism oracle stays green. So `luminumbra_server_app --smoke` must remain `6f008a9f637c40b7`,
run==replay, throughout this work, and the lockstep desync-oracle hash exchange is unchanged.
Cross-ref **spec 018 — determinism hardening** (`docs/specs/018-determinism-hardening/`), which owns
the determinism contract this spec must not regress.

## Goals

- **G-1** — Promote **server-authoritative delta replication** (`ReplicationEndpoint` + AOI) to THE
  documented, tested 20-32+ player path; size and gate it explicitly at 32 clients.
- **G-2** — **Demote lockstep** to a determinism oracle / replay tool / small (<=4) co-op mode; stop
  presenting it as the scale path. Keep its desync-oracle value intact for spec 018.
- **G-3** — Build a real **transport test matrix**: local multiprocess, packet loss, jitter,
  reordering, disconnect/reconnect, NAT/Steam-like behavior, **32 clients**, with bandwidth and CPU
  **p95** gates.
- **G-4** — **Remove the busy-spin send** behavior; replace it with a bounded per-client send queue +
  explicit backpressure signaling and policy.
- **G-5** — Make **per-client backpressure** (outbound queue depth, drop/throttle events) and
  **snapshot aging** (how stale a client's last-acked baseline is) **first-class, queryable metrics**.
- **G-6** — Stand up a **GNS local-multiprocess soak** (the locally-testable real-UDP path) as the
  scale gate *before* Steam; keep Steam SDR behind an explicit deferred-to-hardware item.
- **G-7** — Preserve determinism: sim stays deterministic; `world_hash` never moves; lockstep oracle
  stays green (`--smoke == 6f008a9f637c40b7`).

## Non-Goals

- **NG-1** — A from-scratch netcode rewrite. The seams (`ILockstepTransport`, `ReplicationProtocol`,
  `ReplicationDelta`, `FrameDelivery`) are kept; this spec hardens and proves them at scale.
- **NG-2** — Full lockstep at 20-32 players. Explicitly out of scope per owner decision; lockstep is
  oracle/replay/small-co-op only unless the product later *requires* full lockstep at scale.
- **NG-3** — Local validation of the **Steam SDR** transport. The single-PC constraint
  (one-instance-per-appid) makes this impossible locally; it is an Open Question / deferred to real
  two-box / Steam hardware (OQ-3).
- **NG-4** — Client-side prediction / rollback / interpolation polish (a follow-on gameplay-feel
  spec). This spec proves *transport + replication scale + backpressure*, not perceived smoothness.
- **NG-5** — Any worldgen / streaming / rendering change. Sim determinism contract (spec 018) is
  untouched; nothing here feeds `world_hash`.
- **NG-6** — Two-box LAN validation (no second PC). All scale validation is single-box multiprocess.

## Functional Requirements

IDs are grouped by thrust of finding F6. Each is testable and anchored to a verified file/line in the
current tree (see **Key files**). "Replace"/"retire" means the listed current behavior is superseded.

### Group A — Server-authoritative delta replication as THE scale path

- **FR-A-001 — Replication is the documented 20-32 path.** `ReplicationServer` /
  `ReplicationClient` (`ReplicationEndpoint.h:37,87`) shall be the canonical multiplayer session path
  for 20-32+ players; the engine docs + the spec-018 cross-ref shall state lockstep is NOT that path
  (paired with FR-B-001). The 64-player AOI contract (`ReplicationScale` TEST,
  `ReplicationEndpoint_test.cpp:257`) is promoted from "stress evidence" to a **required scale gate**.
- **FR-A-002 — Delta-vs-acked on by default at scale.** Delta-vs-acked compression
  (`ReplicationEndpoint::set delta`, `m_delta`, `ReplicationEndpoint.h:116`; `MakeSnapshotDelta` /
  `ApplySnapshotDelta`, `ReplicationDelta.h:26+`) shall be ENABLED on the 32-client scale path and
  the loss-tolerant re-delta-against-last-acked loop (`ReplicationDeltaLoop_test.cpp`) exercised under
  the loss matrix (FR-C-002). Today delta defaults OFF (`m_delta = false`, `:116`).
- **FR-A-003 — AOI bounds per-client egress at scale.** With chunk-AOI set
  (`SetAoiChunkRadius`, `ReplicationEndpoint.h:65`), each client's snapshot size shall be bounded by
  **local density, not headcount** at 32 clients — `last_broadcast_max_client_bytes` (`:103`) must
  stay flat as N grows while a full-set broadcast grows linearly (the property the `ReplicationScale`
  test asserts at 64; FR-A-001 makes it a gate).
- **FR-A-004 — Disconnect/reconnect is clean at scale.** `PruneDisconnectedClients` + the pending-
  despawn fold (`m_pending_removed_ids`, `ReplicationEndpoint.h:123-128`) shall correctly remove a
  leaver's avatar and a rejoining client shall re-baseline from a full snapshot — verified under the
  reconnect matrix (FR-C-005), not just the in-process despawn test.

### Group B — Demote lockstep to determinism oracle / replay / small co-op

- **FR-B-001 — Lockstep scope is documented as oracle/replay/small-co-op.** The `LockstepSession`
  doc + `LockstepConfig` shall state lockstep is the **determinism oracle / replay tool / small (<=4)
  co-op** path, NOT the 20-32 scale path, retiring the implication that the "v1 <= 2" scope
  (`LockstepSession.h:227,300`) is a step toward lockstep-at-scale.
- **FR-B-002 — Oracle value preserved.** The desync-oracle hash exchange (`HashMsg`, cadence
  `hash_cadence_ticks=30`, `LockstepSession.h:244`) and HALT-on-mismatch (`TickOutcome::Desync`,
  `:251`) shall remain unchanged and remain wired to spec 018's determinism gate. Demotion is
  documentation + routing, not deletion.
- **FR-B-003 — Small co-op cap is explicit.** If lockstep remains a co-op mode, the supported peer
  count shall be an explicit documented cap (<=4) rather than the implicit "<=2" hard-code; any
  count above the cap routes to the replication path.

### Group C — Transport test matrix (multiprocess, loss, jitter, reorder, reconnect, 32 clients)

- **FR-C-001 — Multiprocess soak harness.** A harness shall run **N separate client processes on one
  box** against one server process over a real transport (TCP first, then GNS), driven by the
  existing scenario harness `--replicated --avatars N` flags
  (`RuntimeScenarioHarness.cpp:202-203`, `--avatars` clamped 0..32). The current tests are
  single-process `LoopbackTransport` only (`ReplicationEndpoint_test.cpp`) — this adds the missing
  real-transport multiprocess dimension.
- **FR-C-002 — Packet loss.** The matrix shall inject configurable packet loss on the GNS/UDP path
  and assert the delta loop converges (no stranded client; FR-A-002) and snapshots remain correct.
- **FR-C-003 — Jitter + reordering.** The matrix shall inject latency jitter and out-of-order
  delivery; ack monotonicity (`AckedSnapshotSeq`, `ReplicationEndpoint.h:96`) and
  newest-wins usercmd handling (`LatestUsercmd`, `:95`) shall hold. (The `replication_hardening_test.cpp`
  suite already asserts the *in-process* contract for dropped/duplicate/out-of-order acks; this is
  the over-the-wire equivalent.)
- **FR-C-004 — 32-client scale soak.** The matrix shall run a **32-client** multiprocess session and
  assert per-client `last_broadcast_max_client_bytes` (`:103`) stays within the bandwidth budget and
  the session sustains its tick rate.
- **FR-C-005 — Disconnect / reconnect.** The matrix shall exercise abrupt disconnect (no Bye) and
  reconnect under load, asserting FR-A-004 (clean prune + re-baseline) and that no other client
  stalls (no shared-fate stall).
- **FR-C-006 — NAT / Steam-like behavior (documented, partially deferred).** The matrix shall
  document and, where locally possible (GNS), exercise NAT-like / relay behavior; the Steam SDR
  relay path is deferred to hardware (OQ-3) and explicitly marked unvalidated-locally.

### Group D — Remove busy-spin; bounded send queue + backpressure

- **FR-D-001 — Kill the busy-spin send.** The `WSAEWOULDBLOCK` bare `continue;` retry in
  `TcpTransport::SendFrame` (`LockstepSession.cpp:378`, socket non-blocking at `:362`, send loop
  `:372-378`) shall be replaced with a **bounded per-client outbound queue**: on would-block, enqueue
  the residual frame and drain on the next pump / writability signal — never spin the CPU.
- **FR-D-002 — Backpressure policy.** When a client's outbound queue exceeds a configured high-water
  mark, the server shall apply an explicit policy (throttle that client's snapshot cadence, drop to
  keyframe-on-next-ack, or disconnect a hopelessly-behind client) — chosen per OQ-4 — rather than
  blocking the broadcast for all clients (no shared-fate stall).
- **FR-D-003 — Backpressure applies to the GNS path too.** The same bounded-queue + policy shall hold
  on the GNS/UDP transport, using its native send-buffer / nReliable feedback rather than a spin.

### Group E — Backpressure + snapshot aging as first-class metrics

- **FR-E-001 — Per-client outbound queue depth metric.** `ReplicationServer` shall expose per-client
  **outbound queue depth** and cumulative throttle/drop counts. Today only aggregate send bytes exist
  (`last_broadcast_total_bytes` / `last_broadcast_max_client_bytes`, `ReplicationEndpoint.h:102-103`);
  `pending_inputs()` (`:233`) is **inbound** only — there is no outbound-queue or aging metric.
- **FR-E-002 — Snapshot aging metric.** The server shall expose, per client, the **age** of that
  client's last-acked baseline (current seq − last-acked seq, derivable from `AckedSnapshotSeq`,
  `:96`, vs `next_snapshot_seq`, `:109`) so a falling-behind client is observable before it desyncs
  or stalls.
- **FR-E-003 — Metrics gate at scale.** The 32-client soak (FR-C-004) shall report p95 of per-client
  queue depth and snapshot age, and the gate shall FAIL if either exceeds budget — the early-warning
  signal that a real large session is degrading.

### Group F — GNS local-multiprocess soak before Steam

- **FR-F-001 — GNS multiprocess is the scale gate.** The GNS transport (`GnsTransport.h`, two
  processes on one machine, real UDP) shall be the **primary locally-runnable scale gate** (TCP is
  the simpler first step; GNS is the realistic UDP/loss/jitter target). Build it out from the current
  one-peer v1 shape (`GnsTransport.h:3-7`) to accept N connections server-side.
- **FR-F-002 — Steam SDR deferred-to-hardware.** The Steam SDR transport
  (`SteamNetworkingTransport.h:3-12`) shall remain the ship transport but is explicitly
  **unvalidated locally** (one-instance-per-appid); its scale validation is deferred to real
  hardware / two-box (OQ-3) and tracked as a known gap, not a passing gate.
- **FR-F-003 — Multi-connection server accept.** The server side of TCP and GNS shall accept and
  fan out to **N concurrent connections** (today Listen "accepts a single connection",
  `LockstepSession.h:185`, `SteamNetworkingTransport.h:10-11`), wired to `ReplicationServer::AddClient`
  per accepted connection.

## Non-Functional Requirements

- **NFR-001 — Determinism (hard gate, sim-side).** `luminumbra_server_app --smoke` must stay
  `6f008a9f637c40b7`, run==replay, throughout. Replication is a read-only observer of sim state and
  must never feed `world_hash`; the lockstep desync oracle must stay green. (Cross-ref spec 018.)
  NOTE: this is explicitly NOT the 015 "render-only" exemption — networking touches the sim, so the
  guarantee is "observer-only + oracle-preserved", proven by the unchanged `--smoke` hash.
- **NFR-002 — Single-PC validation constraint.** All scale validation is **local multiprocess on one
  box** over TCP and GNS. No two-box LAN; Steam SDR cannot be validated locally and is deferred
  (OQ-3, FR-F-002). (Project memory: single-PC testing constraint.)
- **NFR-003 — No shared-fate stalls.** No single slow/behind/disconnecting client may stall the
  others. The broadcast loop must be non-blocking per client (FR-D-001/002); this is the core risk
  the spin and the one-remote lockstep shape introduce at scale.
- **NFR-004 — Bandwidth budget at 32 clients.** Per-client egress (`last_broadcast_max_client_bytes`)
  at 32 clients with chunk-AOI + delta must stay within the recorded bandwidth budget; the budget is
  the MEASURED `last_broadcast_max_client_bytes` baseline (the telemetry added in T-I6 P5,
  `ReplicationEndpoint.h:98-103`), re-recorded at 32 clients as the scale baseline.
- **NFR-005 — CPU p95 budget at 32 clients.** Server tick CPU p95 in the 32-client soak must stay
  within budget; removing the busy-spin (FR-D-001) is a prerequisite (the spin inflates CPU under
  backpressure).
- **NFR-006 — Seam stability.** Changes stay behind the existing seams (`ILockstepTransport`,
  `ReplicationProtocol`, `FrameDelivery`); the replication endpoints run unchanged over Loopback /
  TCP / GNS / Steam (`SteamNetworkingTransport.h:7-8` already asserts this invariant).
- **NFR-007 — Wire-format stability where possible.** Delta is a pure function over `SnapshotMsg`
  with "no wire-format change" (`ReplicationDelta.h:8`); new metrics (Group E) must not change the
  on-wire `ReplicationProtocol` frames unless an explicit versioned bump is taken.

## Acceptance Criteria

Each names a measurable signal / test command. Build the canonical preset tree
(`cmake --build --preset debug`) with `C:\msys64\ucrt64\bin` prepended (toolchain-PATH gotcha). Criteria are split into **runnable today** (regression guards) and
**new gates this spec introduces** (to-be-built; command named even where the harness is new).

### Cross-cutting (regression — runnable today)
- [ ] **AC-001** — `luminumbra_server_app --smoke` stays `6f008a9f637c40b7`, run==replay, after every
  group (sim determinism / observer-only invariant; NFR-001).
- [ ] **AC-002** — The full replication suite stays green: `ctest -R "Replication"` (covers
  `ReplicationEndpoint.*`, `ReplicationScale.*`, `ReplicationDelta*`, `ReplicationProtocol.*`,
  `replication_hardening` — registered via `gtest_discover_tests`, `test/CMakeLists.txt:124-138`).
- [ ] **AC-003** — `--replicated --avatars 32` runs in the scenario harness without clamping below 32
  (`RuntimeScenarioHarness.cpp:202`) and produces a per-client bandwidth report.

### Group A (scale path)
- [ ] **AC-A-001** — `ReplicationScale` is a REQUIRED gate (not evidence-only): `ctest -R
  ReplicationScale` green, and per-client snapshot bytes stay ~flat as N goes 8→32→64 while full-set
  bytes grow linearly (`last_broadcast_max_client_bytes`, FR-A-003).
- [ ] **AC-A-002** — With delta ON, a client that misses M consecutive deltas still converges to the
  correct full snapshot once an ack lands (`ReplicationDeltaLoop_test` extended over a lossy
  transport; FR-A-002 / FR-C-002).

### Group B (lockstep demotion)
- [ ] **AC-B-001** — Docs + `LockstepConfig`/`LockstepSession` comments state lockstep =
  oracle/replay/small-co-op, NOT the 20-32 path; grep shows the demotion language and the spec-018
  cross-ref (FR-B-001).
- [ ] **AC-B-002** — The desync oracle still HALTs on an injected hash mismatch and emits an LREC1
  dump (existing lockstep desync test stays green; FR-B-002).

### Group C (transport matrix) — new gates
- [ ] **AC-C-001** — Multiprocess soak: N client processes + 1 server process over TCP complete a
  session with correct final snapshots (new `ctest` target, e.g. `MultiprocessSoakTcp`; FR-C-001).
- [ ] **AC-C-002** — GNS soak under injected **loss** converges (new `MultiprocessSoakGnsLoss`;
  FR-C-002).
- [ ] **AC-C-003** — GNS soak under injected **jitter + reorder** preserves ack monotonicity +
  newest-wins (new `MultiprocessSoakGnsJitter`; FR-C-003).
- [ ] **AC-C-004** — 32-client multiprocess soak sustains tick rate and per-client bytes within
  budget (new `MultiprocessSoak32`; FR-C-004, NFR-004).
- [ ] **AC-C-005** — Abrupt disconnect + reconnect under load: leaver pruned, rejoiner re-baselines,
  no other client stalls (new `MultiprocessReconnect`; FR-C-005, NFR-003).

### Group D (backpressure) — new gates
- [ ] **AC-D-001** — `TcpTransport::SendFrame` no longer contains a `WSAEWOULDBLOCK`-spin `continue;`
  (grep `LockstepSession.cpp` shows the bounded-queue path replacing `:378`); a unit test forces
  would-block and asserts no busy-loop + eventual drain (FR-D-001).
- [ ] **AC-D-002** — Under a saturated client, the server applies the backpressure policy and the
  *other* clients keep receiving on-cadence snapshots (no shared-fate stall; FR-D-002, NFR-003).

### Group E (metrics) — new gates
- [ ] **AC-E-001** — `ReplicationServer` exposes per-client outbound queue depth + throttle/drop
  counts + snapshot age (new getters); a unit test reads them and they move as expected under
  backpressure (FR-E-001/002).
- [ ] **AC-E-002** — The 32-client soak reports p95 queue depth and p95 snapshot age, and the gate
  FAILs if either exceeds budget (FR-E-003, NFR-005).

### Group F (GNS-before-Steam) — new gates
- [ ] **AC-F-001** — GNS server accepts N concurrent connections and fans out via `AddClient` (new
  `GnsMultiConnect` test; FR-F-001 / FR-F-003).
- [ ] **AC-F-002** — Steam SDR scale validation is documented as deferred-to-hardware and tracked as
  a known gap (OQ-3); no Steam scale gate is claimed green locally (FR-F-002).

## Open Questions
- **OQ-1** — Does the product require full lockstep at 20-32 at all, ever? If yes, this spec's
  demotion (Group B) is interim and a lockstep-at-scale spec is needed; if no (assumed), lockstep
  stays oracle/replay/small-co-op permanently. Owner decision drives FR-B-003's cap.
- **OQ-2** — GNS dependency: is `LUMINUMBRA_ENABLE_GNS` (`GnsTransport.h:12`) buildable on the CI/dev
  box, or does GameNetworkingSockets need vendoring/FetchContent first? Blocks every Group C/F GNS gate.
- **OQ-3 (Steam, deferred)** — Steam SDR cannot be validated locally (one-instance-per-appid,
  single-PC constraint). What is the first real two-box / Steam-hardware validation milestone, and
  what stands in for it until then (GNS soak as proxy)? Blocks FR-C-006 / FR-F-002 closure.
- **OQ-4 (blocks FR-D-002 policy)** — Backpressure policy when a client falls behind: throttle
  cadence, drop-to-keyframe-on-next-ack, or disconnect-the-hopeless? Decide after the 32-client soak
  shows where queues actually grow.
- **OQ-5** — Bandwidth + CPU budgets at 32 clients: what are the actual numbers? The
  `last_broadcast_max_client_bytes` baseline must be re-recorded at 32 (NFR-004) before AC-C-004 /
  AC-E-002 can freeze a pass threshold. (No 32-client baseline number is recorded yet — only the P5
  telemetry mechanism exists.)
- **OQ-6** — Multiprocess harness shape: spawn N real `luminumbra_client_app --replicated` processes,
  or N in-process threads each with its own real socket? Real processes are the honest soak; threads
  are cheaper/CI-friendlier. Decide for FR-C-001.
- **OQ-7** — Does the scenario harness `--avatars` clamp of 32 (`RuntimeScenarioHarness.cpp:202`)
  need raising to test headroom beyond 32 (e.g. 48/64) so the AOI-bound property is proven past the
  product target, as the `ReplicationScale` test already does at 64?

## Phasing (sequenced: prove-the-scale-path first, then harden, then real-transport scale)

1. **Group B + A docs/routing** — demote lockstep, promote replication as THE path, make
   `ReplicationScale` a required gate. Cheap, no new transport; establishes the contract. *(cheap-high-clarity)*
2. **Group D — kill the busy-spin + bounded send queue.** Correctness fix that unblocks honest CPU
   measurement; unit-testable without a full soak. *(cheap-high-impact)*
3. **Group E — backpressure + aging metrics.** Needed to *observe* the soak; small additive getters.
4. **Group C (TCP) + F (multi-connection accept).** Stand up the multiprocess soak on TCP first
   (simplest), with N-connection server accept.
5. **Group C (GNS) — loss / jitter / reorder + 32-client soak.** The real-UDP scale gate; the
   payoff. Gated on OQ-2 (GNS buildable).
6. **Steam SDR (deferred).** Tracked as a known gap (OQ-3); validated only on real hardware.

## Blocking gates (per phase)
1. **Determinism (AC-001/002):** `--smoke == 6f008a9f637c40b7` run==replay + `ctest -R Replication`
   green after every phase (observer-only invariant; spec 018 contract intact).
2. **No busy-spin (AC-D-001):** the `WSAEWOULDBLOCK`-spin is gone before any scale soak runs (a soak
   over a spinning send path measures the wrong thing).
3. **No shared-fate stall (AC-D-002, AC-C-005, NFR-003):** one slow/behind/leaving client never
   stalls the others — the central F6 risk.
4. **Scale soak within budget (AC-C-004, AC-E-002, NFR-004/005):** 32-client multiprocess soak holds
   bandwidth + CPU p95 budgets; per-client bytes bounded by AOI, not headcount.

## Risks / unknowns
- **GNS not buildable locally** would gut the realistic scale gate (OQ-2); TCP soak is a weaker
  stand-in (no loss/jitter realism). Resolve OQ-2 early.
- **Steam stays unvalidated locally** (single-PC); the ship transport's scale behavior is proven only
  by analogy to GNS until real hardware (OQ-3). State this honestly; don't claim a green Steam gate.
- **Multiprocess soak is heavyweight** — N real client processes on one box may itself be CPU-bound
  and mask the server's true p95; pick the harness shape (OQ-6) to keep the *server* the bottleneck.
- **Backpressure policy changes session feel** — disconnecting a behind client vs throttling it is a
  product decision (OQ-4); measure before choosing.
- **Determinism is observer-only, not exempt** — unlike render specs, a careless replication change
  *could* perturb the sim (e.g. applying usercmds out of canonical order). The `--smoke` gate +
  lockstep oracle are the guard; treat any hash move as a real regression, not a re-bless.

## Key files
- `src/luminumbra_common/net/LockstepSession.h` — `LockstepConfig` v1 one-remote scope
  (`:227-228`, "v1 scope: <= 2"); class doc "2-peer (<=2 clients, ONE remote)" (`:300`);
  `TickOutcome::Desync` HALT (`:251`); `hash_cadence_ticks=30` oracle (`:244`); TCP transport
  "ONE remote ... single connection" (`:185`). Group B (demote) + F-003 (multi-connection accept).
- `src/luminumbra_common/net/LockstepSession.cpp` — TCP `SendFrame` busy-spin: socket non-blocking
  `:362`, send loop `:372-378`, `WSAEWOULDBLOCK) continue;` `:378` (Group D). Tick gate "the one
  remote" `:600` (Group B).
- `src/luminumbra_common/net/SteamNetworkingTransport.h` — Steam SDR ship transport; "v1 ... ONE peer
  per transport (Listen accepts a single connection)" (`:3-12`); endpoints run unchanged over it
  (`:7-8`). Deferred (FR-F-002, OQ-3).
- `src/luminumbra_common/net/GnsTransport.h` — locally-testable real-UDP dev path, "two processes on
  ONE machine" (`:3-7`); `LUMINUMBRA_ENABLE_GNS` guard (`:12`). The scale gate (Group C/F).
- `src/luminumbra_common/net/ReplicationEndpoint.h` — `ReplicationServer`/`ReplicationClient`,
  `AddClient` (`:37`), `BroadcastSnapshot` (`:87`), AOI `SetAoiRadiusMm`/`SetAoiChunkRadius`
  (`:54-69`), `AckedSnapshotSeq` (`:96`), bandwidth telemetry `last_broadcast_total_bytes` /
  `last_broadcast_max_client_bytes` (`:98-103`), `m_delta` default-off (`:116`), pending-despawn
  (`:123-128`), inbound `pending_inputs()` (`:233`). Groups A, E. Add outbound-queue + aging metrics.
- `src/luminumbra_common/net/ReplicationDelta.h` — "makes 20-32+ players affordable" (`:3-6`),
  no-wire-change pure delta (`:8`), `MakeSnapshotDelta`/`ApplySnapshotDelta` (`:26+`). Group A.
- `src/luminumbra_common/net/ReplicationProtocol.{h,cpp}` — `SnapshotMsg`/`UsercmdMsg`/`AckMsg` wire
  format; touch only behind a versioned bump (NFR-007).
- `test/common/ReplicationEndpoint_test.cpp` — multi-client seq (`:78`), AOI scoping (`:142-145`),
  per-client snapshot (`:243-248`), `ReplicationScale` 64-player AOI-bound TEST (`:257`, doc
  `:250-256`). Promote to required scale gate; extend over real transports.
- `test/common/replication_hardening_test.cpp` — in-process adversarial acks/AOI/despawn contract;
  the over-the-wire matrix (Group C) is its multiprocess counterpart.
- `test/common/ReplicationDeltaLoop_test.cpp` — loss-tolerant re-delta loop; extend over a lossy
  transport (AC-A-002).
- `src/luminumbra_client/core/RuntimeScenarioHarness.cpp` — `--avatars` clamp 0..32 (`:202`),
  `--replicated` flag (`:203`); the scale-soak driver (Group C, OQ-7).
- `test/CMakeLists.txt` — replication tests listed `:124-138`, registered via `gtest_discover_tests`;
  add the new multiprocess-soak `add_test` targets here.
- `docs/specs/018-determinism-hardening/` — the determinism contract this spec must not regress
  (NFR-001 cross-ref).

## Verification (end-to-end)
1. Build the canonical preset tree (`cmake --build --preset debug`, prepend
   `C:\msys64\ucrt64\bin`); `luminumbra_server_app --smoke` byte-identical
   (`6f008a9f637c40b7`), run==replay, after every group.
2. `ctest -R Replication` green (existing suite, regression); `ReplicationScale` promoted to required.
3. Group D: grep confirms the `WSAEWOULDBLOCK`-spin is gone; unit test forces would-block → no spin,
   eventual drain.
4. Group C/F: TCP multiprocess soak green; GNS soak under loss/jitter/reorder green (gated on OQ-2);
   32-client soak holds bandwidth (`last_broadcast_max_client_bytes`) + CPU p95 budgets.
5. Group E: per-client queue depth + snapshot age metrics readable; p95 reported and gated at scale.
6. Steam SDR: documented deferred-to-hardware (OQ-3); no local green gate claimed.

---
**Anchor drift corrected from the F6 brief** (verified against the tree this session):
- TCP "one remote": the brief cited `LockstepSession.cpp ~185`, which is actually `PeekMessageType`.
  Corrected to `LockstepSession.h:185` (TCP "ONE remote" comment) and `LockstepSession.cpp:600`
  ("the one remote" tick gate).
- TCP busy-retry: confirmed at `LockstepSession.cpp:378` (`WSAEWOULDBLOCK) continue;`), with the
  socket set non-blocking at `:362` and the send loop at `:372-378`.
- `ReplicationScale` stress TEST is at `ReplicationEndpoint_test.cpp:257` (the cited `:250` is the
  doc-comment block `:250-256`); AOI test `:142-145`; multi-client `:78` — all confirmed.
- Scenario flag is `--replicated` (not `--replicate`), paired with `--avatars` clamped 0..32 at
  `RuntimeScenarioHarness.cpp:202-203` — corrected in the ACs.
- `LockstepSession.h:227-228` (`local_client_id=0`/`peer_client_id=1`, "v1 scope: <= 2"), `:300`
  (one remote), `SteamNetworkingTransport.h:3-12`, `GnsTransport.h:3-7`, `ReplicationDelta.h:3-6`,
  and the `ReplicationEndpoint.h` telemetry (`:102-103`) all confirmed as cited.
