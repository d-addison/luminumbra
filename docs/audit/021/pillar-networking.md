# Pillar audit: Networking & replication — spec 019 scale-out (Spec 021, 2026-07-02)

**Verdict.** The networking pillar is in the best shape of the feature sub-pillars: spec 019's
phases 1-4 (docs/routing, busy-spin kill, backpressure/aging metrics, 32-client soak + required
`ReplicationScale` gate) have ALL landed — including the Wave-3 019-C1 soak the 2026-06-28 roadmap
still lists as future work. Server-authoritative delta replication is the documented, tested
20-32+ player path (`docs/networking-scale-architecture.md:1`); lockstep is demoted to
determinism-oracle/replay/small-co-op with the oracle preserved verbatim. What remains is the
honest tail of the spec: delta compression is still never enabled on any real session or soak path
(`SetDeltaCompression` has zero non-test callers), the live over-the-wire soak has only been run
at N=4 (32 clients were proven in-process only), GNS is code-complete but compiled OUT of both
build trees (OQ-2 unresolved — the loss/jitter over-the-wire matrix does not exist), the
FR-D-002 backpressure *policy* is a placeholder metric, and Steam SDR remains explicitly
unvalidated-locally per the single-PC constraint. Determinism exposure is low: replication is a
read-only observer of sim state and none of the remaining work touches `world_hash`.

## Current state + evidence

### The two paths, correctly routed (spec 019 Groups A + B — DONE)

- **Delta replication is THE documented 20-32+ path.** The canonical architecture note exists
  (`docs/networking-scale-architecture.md:1`, created 2026-06-27) and states
  "`ReplicationServer` / `ReplicationClient` delta replication — not lockstep — is the 20-32+
  player session path" (`docs/networking-scale-architecture.md:6-7`), with a two-path routing
  table (`docs/networking-scale-architecture.md:221-224`). The code carries the same marker:
  "SCALE PATH (spec 019 FR-A): server-authoritative delta replication here -- NOT lockstep -- is
  THE 20-32+ player session path" (`src/luminumbra_common/net/ReplicationEndpoint.h:23-27`).
- **Lockstep demoted to oracle/replay/small (<=4) co-op**, oracle preserved: the demotion +
  explicit co-op cap and "current implementation is strictly 2-peer" statement are at
  `docs/networking-scale-architecture.md:250-261`; the desync-oracle hash exchange
  (`HashMsg`, `src/luminumbra_common/net/LockstepSession.h:92-98`; cadence
  `hash_cadence_ticks = 30`, `LockstepSession.h:293`; `TickOutcome::Desync` HALT,
  `LockstepSession.h:300`) is unchanged. The lockstep implementation itself is still hard-wired
  to one remote: "v1 scope: <= 2" (`LockstepSession.h:276-277`), class doc "2-peer (<=2 clients,
  ONE remote)" (`LockstepSession.h:349`), tick gate waits on "the one remote"
  (`src/luminumbra_common/net/LockstepSession.cpp:634`).

### The replication core (pre-roadmap, verified present)

- `ReplicationServer` (`ReplicationEndpoint.h:41`): multi-client fan-out via `AddClient`
  (`:44`), deterministic ordered-map roster (`m_clients`, `:162`), `BroadcastSnapshot` (`:97`),
  non-blocking `PumpInbound` (`:102`), drain-then-prune leave handling
  (`PruneDisconnectedClients`, `:54`) with despawn repeated across `kRemovalRepeatBroadcasts = 3`
  unreliable snapshots (`:176-177`).
- **AOI bounds egress by density, not headcount**: mm-radius (`SetAoiRadiusMm`, `:61`) and the
  scale-mode chunk-index AOI (`SetAoiChunkRadius`, `:72`), asserted at 64 players by
  `TEST(ReplicationScale, ChunkAoiBoundsPerClientBandwidthAsPlayersScale)`
  (`test/common/ReplicationEndpoint_test.cpp:269`).
- **Delta-vs-acked codec + ack-driven re-delta loop LANDED** (corrects the stale project-memory
  claim that the re-delta loop was "remaining integration"): pure-function delta
  (`src/luminumbra_common/net/ReplicationDelta.h:3-13`, `MakeSnapshotDelta` `:26`), server
  dispatch behind `SetDeltaCompression` (`ReplicationEndpoint.h:90`, default-off `m_delta = false`
  `:163`), loss-tolerant loop proven over injected loss in
  `test/common/ReplicationDeltaLoop_test.cpp:71` (registered `test/CMakeLists.txt:115`) and the
  adversarial ack/AOI/despawn contract in `test/common/replication_hardening_test.cpp:504`.
- Client-side feel layer: `SnapshotInterpolator` (render-behind lerp, no extrapolation,
  `ReplicationEndpoint.h:216`) and `LocalPlayerPredictor` (predict + snap-and-replay reconcile,
  `ReplicationEndpoint.h:255`).
- **Deterministic network-condition sim**: `NetworkSim` injects deterministic latency/jitter/loss
  over the `ILockstepTransport` seam, in ticks, seeded (run==replay)
  (`src/luminumbra_common/net/NetworkSim.h:3-13`, `NetworkConditions` `:24-29`) — the single-PC
  unblocker for in-process adverse-condition tests.

### Transports behind the seam

- The seam: `ILockstepTransport` + per-frame `FrameDelivery` selector
  (`LockstepSession.h:133`, `:135-156`); endpoints run unchanged over Loopback/TCP/GNS/Steam
  (`SteamNetworkingTransport.h:7-8`).
- **TCP** (`LockstepSession.h:240-268`): still "ONE remote ... accept ONE client" per transport
  (`:232`, `:249`); non-_WIN32 build is a stub (`LockstepSession.cpp:441-449`).
- **GNS** (dev real-UDP, two processes on one machine): `src/luminumbra_common/net/GnsTransport.h:3-15`;
  single listen + single connection (`m_listen`/`m_conn`, `GnsTransport.h:59-60`). Compiled out
  unless `LUMINUMBRA_ENABLE_GNS` (`GnsTransport.h:15`); the CMake option + FetchContent path
  exists (`src/luminumbra_common/CMakeLists.txt:131-146`) but is **OFF in both build trees**
  (`build/debug/CMakeCache.txt:766`, `build/CMakeCache.txt` same flag) — spec 019 OQ-2 is
  unresolved in practice.
- **Steam SDR** (ship transport): `SteamNetworkingTransport.h:3-14`, one peer per transport
  (`:10-11`), compiled out unless `LUMINUMBRA_ENABLE_STEAM` (`:18`), OFF in both trees
  (`build/debug/CMakeCache.txt:769`). Local validation impossible (one-instance-per-appid); the
  split is a locked TDD scenario (`test/features/TDD-LOCK.md:34-38`).
- **Multi-connection accept = per-client-port scheme**, not single-port N-accept: client K maps to
  `base_port + K - 1` (`src/luminumbra_common/network/NetworkLoopbackAuthority.cpp:487-501`, API
  `NetworkLoopbackAuthority.h:199-202`), documented at `src/luminumbra_server/main_server.cpp:109-111`
  and used by every multi-client server mode via `ResolveNetworkClientPort`
  (`main_server.cpp:357-362`). This satisfied 019-C1's FR-F-003 precondition for TCP.

### Proving signals that exist today (verified)

- **`ReplicationScale`** — ctest: the deterministic in-process 32-client soak suite
  (`test/common/replication_scale_test.cpp:106`, `:168`, `:248`; registered
  `test/CMakeLists.txt:141`) plus the 64-player AOI-bound test
  (`ReplicationEndpoint_test.cpp:269`).
- **`ReplicationSmoke`** — engine-frontier gate (`tools/gates/validate-engine-frontier.ps1:4845`,
  dispatch `:7281`): drives `--replicate --avatars 4 --npcs 3 --arrow` (`:4867`) and asserts
  mirroring, acks, input-moved-avatar, GOAP NPC replication, and arrow lifecycle.
- **`NetworkedReplication`** — engine-frontier gate (`:4904`, dispatch `:7282`): two real
  processes (`--net-host` / `--net-join`) over TCP (`:4926-4933`). Note: the pillar brief called
  these two "ctests"; they are engine-frontier gate modes, `ReplicationScale` is the ctest
  (per `test/features/TDD-LOCK.md:47`).
- Additional gates: `NetworkedSession` (`:7290`), `NetworkLoopbackAuthorityGate` (`:7260`),
  `NetworkStateHash` (`:7261`).
- Scenario harness scale driver: `--avatars` clamped 0..32 + `--replicated`
  (`src/luminumbra_client/core/RuntimeScenarioHarness.cpp:202-203`).

### Shipped since the 2026-06-28 roadmap

Verified via `git log -- src/luminumbra_common/net test/common/...`:

- **019-C1 — 32-client replication soak harness + `ReplicationScale` gate** (commit `45e6963f`,
  2026-06-28 14:02). The merged roadmap schedules 019-C1 in **Wave 3**; it in fact landed the same
  day the roadmap was approved — **reality is ahead of the roadmap here**. Server side:
  `--net-soak` (`main_server.cpp:2542-2558`, `RunNetSoak` `:2559`) accepts up to `--clients` TCP
  peers on per-client ports, re-arms accept slots on clean leave for reconnect-under-load
  (`:2647-2659`), and FAILs (non-zero exit) on tick-rate/QueueDepthP95/SnapshotAgeP95/bytes budget
  breach (`:2688-2693`, `:2758`), emitting a `luminumbra.net_soak.v1` artifact (`:2714-2750`).
  Client side: `--net-soak-client` disconnect/reconnect cycles (`:2761-2764`). The deterministic
  in-process 32-client equivalent asserts the same budgets over LoopbackTransport
  (`replication_scale_test.cpp:39-44`). **Caveat recorded in the commit itself: the live
  over-the-wire run was N=4** (400/400 ticks, q-p95=0, age-p95=7, 152 B/client); N=32 is proven
  in-process only.
- Immediately pre-roadmap (2026-06-27), roadmap already assumed done — verified true:
  **019-D1** busy-spin kill (commit `9ef61c40`): `TcpTransport::SendFrame` now appends to a
  bounded `OutboundByteQueue` (`LockstepSession.h:194-230`), drains via `FlushSendNonBlocking`
  which STOPS on would-block (`LockstepSession.cpp:369-385`, `:382`), and applies a bounded
  select-on-writability wait only past the 4 MiB high-water mark (`:406-414`) — never a spin,
  never a drop. Proven by the five `OutboundBackpressure` tests
  (`test/common/LockstepBackpressure_test.cpp:28-109`, registered `test/CMakeLists.txt:127`).
  **019-E1** metrics (commit `27273e5a`): per-client `OutboundQueueDepth` / `SnapshotAge` /
  `PeakOutboundQueueDepth` / `DroppedFrames` / `ThrottledFrames` + across-client
  `QueueDepthP95` / `SnapshotAgeP95` (`ReplicationEndpoint.h:130-136`; queue-and-drain broadcast
  path `ReplicationEndpoint.cpp:167-183`; p95s `:274-284`), proven by the three
  `ReplicationMetrics` tests (`ReplicationEndpoint_test.cpp:529-582`).

## Gaps / debt

1. **Delta compression is never enabled outside tests (FR-A-002 open).** `SetDeltaCompression`
   has exactly zero non-test callers — grep hits only `ReplicationDeltaLoop_test.cpp:71` and
   `replication_hardening_test.cpp:504` plus the setter itself (`ReplicationEndpoint.h:90`).
   Neither `RunNetSoak` (`main_server.cpp:2593-2594` — AOI only) nor the `ReplicationScale`
   soak tests (`replication_scale_test.cpp:108`, `:170` — AOI only) turn delta on. The "bandwidth
   win that makes 20-32+ players affordable" (`ReplicationDelta.h:5-6`) is therefore NOT what the
   scale gates exercise: they validate full snapshots.
2. **The 32-client over-the-wire soak has not actually run at 32.** Commit `45e6963f` records the
   live TCP run at N=4; the 32-client budget assertions ran in-process only
   (`replication_scale_test.cpp:39-44`). AC-C-004's "32-client multiprocess soak" is
   half-closed, and no frozen measured 32-client bandwidth/CPU baseline exists (OQ-5): the budget
   constants are hand-set (16 KiB in-process `replication_scale_test.cpp:43`; 64 KiB + 40%
   wall-clock tolerance over TCP, `main_server.cpp:2570-2573`).
3. **GNS is compiled out of both build trees** (`build/debug/CMakeCache.txt:766`,
   `LUMINUMBRA_ENABLE_GNS:BOOL=OFF`) — OQ-2 ("is GNS buildable on the dev box?") is unresolved,
   so the entire FR-C-002/003 over-the-wire loss/jitter/reorder matrix and the FR-F-001 "GNS is
   the primary locally-runnable scale gate" promotion do not exist. Loss/jitter coverage today is
   in-process `NetworkSim` only (`NetworkSim.h:3-13`).
4. **FR-D-002 backpressure policy is a placeholder.** `ThrottledFrames` exists as a metric but the
   field comment states it is "0 until that policy lands" (`ReplicationEndpoint.h:157-160`;
   getter returns the always-zero field, `ReplicationEndpoint.cpp:269`). The endpoint has
   drop-oldest bounding (`kOutboundQueueCap = 256`, `ReplicationEndpoint.h:165`) but no
   throttle-cadence / drop-to-keyframe / disconnect-the-hopeless policy (OQ-4), and FR-D-003
   (backpressure on the GNS path) is untouched because GNS isn't built.
5. **Lockstep demotion language is missing from the lockstep code itself** (AC-B-001 half-met).
   `ReplicationEndpoint.h:23-27` and the architecture doc carry it, but
   `LockstepSession.h` contains no oracle/replay/small-co-op scope statement — its only spec-019
   references are the FR-D queue comments (`LockstepSession.h:186`, `:267`); the config comment
   still reads as a plain "v1 scope: <= 2" (`:276`).
6. **Doc line-drift**: `docs/networking-scale-architecture.md:255-259` cites
   `LockstepSession.h:227-228` / `:300` (and `:244`/`:251` above) — the 019-D1 `OutboundByteQueue`
   insertion shifted these to `:276-277` / `:349` / `:293` / `:300`. Same drift class as the
   spec's own corrected anchors (`docs/specs/019-networking-scale-out/spec.md:396-409`).
7. **`--net-soak` is not wired into any automated gate.** No `NetSoak` mode exists in
   `validate-engine-frontier.ps1` (grep: no match); the soak is a manually-invoked harness. The
   in-process `ReplicationScale` ctest covers regression, but the over-the-wire path has no
   repeatable gate entry (deliberate per the flake note at `replication_scale_test.cpp:5-8` —
   still, a manual-tier gate entry per spec 020-A conventions is the missing operability hook).
8. **Single-port multi-accept does not exist.** The per-client-port scheme
   (`NetworkLoopbackAuthority.cpp:487-501`) burns one listen port per client and is not how a real
   dedicated server accepts; TCP/GNS `Listen` still accept exactly one connection
   (`LockstepSession.h:249`; `GnsTransport.h:44`, `:59-60`).
9. **POSIX TcpTransport is a stub** (`LockstepSession.cpp:441-449`) — a Linux dedicated server
   cannot network at all today.

## Risks

- **The ship transport is proven only by analogy.** Steam SDR cannot be validated on one PC
  (`SteamNetworkingTransport.h:10-14`; `test/features/TDD-LOCK.md:34-38`); until second-machine
  validation happens, its scale behavior is inferred from TCP (and eventually GNS). This is an
  accepted, documented gap (spec 019 FR-F-002/OQ-3) — the risk is forgetting it is open when
  shipping is discussed.
- **Delta-at-scale is unmeasured.** Because the scale gates run full snapshots (gap 1), enabling
  delta for a real session flips the 32-client path onto a code route whose budget behavior at
  N=32 was never asserted; the delta loop is proven correct under loss but not proven cheap at
  scale. Low correctness risk (codec is pure + loss-tolerant, `ReplicationDelta.h:8-13`), real
  measurement risk.
- **Determinism exposure is low but not zero.** Replication is an observer (`ReplicationEndpoint.h:19-21`)
  and 019's remaining work is transport-side, but spec 019's own warning stands: applying usercmds
  out of canonical order *could* perturb the sim (`docs/specs/019-networking-scale-out/spec.md:345-347`).
  Every remaining item below keeps `--smoke == 6f008a9f637c40b7` as a hard gate.
- **Multiprocess soak flakiness**: the harness is wall-clock + OS-port bound
  (`replication_scale_test.cpp:5-8`); wiring it as a *blocking* CI gate would import flake. It
  should land as a manual-tier / artifact-verified gate, not a per-commit one.
- **GNS FetchContent from `master`** (`src/luminumbra_common/CMakeLists.txt:139-143`) is an
  unpinned upstream; first enablement should pin a tag to keep builds reproducible.

## Opportunities

- The **NetworkSim seam makes the GNS loss/jitter matrix cheap to specify**: the in-process
  contracts (`ReplicationDeltaLoop_test.cpp`, `replication_hardening_test.cpp`) are exactly the
  assertions to replay over GNS UDP once OQ-2 is resolved — the tests are already written in
  transport-agnostic terms against `ILockstepTransport`.
- The **net_soak.v1 artifact** (`main_server.cpp:2714-2750`) already carries everything a
  budget-freezing baseline needs (worst p95s, per-client bytes, accept/leave counts); recording
  the N=32 run and checking the artifact into the baseline set closes OQ-5 with near-zero new code.
- **`ThrottledFrames` is pre-plumbed** (`ReplicationEndpoint.h:134`, `:157-160`): the FR-D-002
  policy can land as a small, unit-testable endpoint change whose metric surface already exists.
- The soak's reconnect re-arm loop (`main_server.cpp:2647-2659`) is the natural substrate for a
  future join-in-progress/late-join product feature — the hard transport part is already gated.

## Backlog items

| id | summary | spec | effort | risk | deps | status | proving_signal |
|----|---------|------|--------|------|------|--------|----------------|
| NET-01 | Replication core: endpoints, AOI, delta codec + ack-driven re-delta loop, prediction/interpolation, hardening tests (corrects stale memory: the re-delta loop LANDED 2026-06-19) | 019 | L | low | — | done | ReplicationScale (ctest, test/CMakeLists.txt:141) + ReplicationSmoke gate (validate-engine-frontier.ps1:4845) |
| NET-02 | 019 Group A/B docs + routing: delta replication promoted to THE 20-32 path, lockstep demoted to oracle/replay/small-co-op (docs/networking-scale-architecture.md) | 019 | S | low | — | done | ReplicationScale (ctest; promoted to required gate per FR-A-001) |
| NET-03 | 019-D1: WSAEWOULDBLOCK busy-spin killed; bounded no-drop OutboundByteQueue + high-water bounded wait (commit 9ef61c40) | 019 | M | low | — | done | named ctest OutboundBackpressure.* (test/common/LockstepBackpressure_test.cpp:28-109) |
| NET-04 | 019-E1: per-client outbound queue-depth + snapshot-age metrics with across-client p95s (commit 27273e5a) | 019 | S | low | — | done | named ctest ReplicationMetrics.* (test/common/ReplicationEndpoint_test.cpp:529-582) |
| NET-05 | 019-C1: 32-client soak harness (--net-soak/--net-soak-client, per-client-port TCP accept, reconnect-under-load, budget-FAIL artifact) + deterministic in-process ReplicationScale soak gate (commit 45e6963f; roadmap Wave-3 item landed early) | 019 | L | medium | NET-03, NET-04 | done | ReplicationScale (ctest, replication_scale_test.cpp:106/168/248) |
| NET-06 | Enable delta-vs-acked on the scale paths (FR-A-002): SetDeltaCompression(true) in RunNetSoak + a delta-ON ReplicationScale variant asserting budgets hold at N=32 | 019 | S | low | NET-05 | todo | ReplicationScale (extended: delta-ON 32-client run stays within QueueDepthP95/SnapshotAgeP95/bytes budgets and converges) |
| NET-07 | Run the over-the-wire soak at N=32 (live run was N=4), freeze the measured bandwidth/CPU baseline into the net_soak.v1 artifact (closes OQ-5/NFR-004), and wire a manual-tier NetSoak gate mode | 019 | M | medium | NET-05, 020 | todo | NEW: NetSoak gate — validate-engine-frontier.ps1 -Mode NetSoak asserts a luminumbra.net_soak.v1 artifact with expected_clients=32 and passed=true |
| NET-08 | Resolve OQ-2: build GNS locally (pin the FetchContent rev), add N-connection GNS accept, and stand up the over-the-wire loss/jitter/reorder matrix (FR-C-002/003, FR-F-001) | 019 | L | medium | NET-05 | todo | NEW: MultiprocessSoakGnsLoss ctest — GNS multiprocess session under injected packet loss converges with no stranded client and ack monotonicity held |
| NET-09 | Implement the FR-D-002/003 backpressure policy (throttle cadence / drop-to-keyframe / disconnect per OQ-4, decided from NET-07 soak data) and make ThrottledFrames real (today hard-0, ReplicationEndpoint.h:157-160) | 019 | M | medium | NET-07 | todo | NEW: ReplicationBackpressurePolicy ctest — a saturated client is throttled (ThrottledFrames>0) while other clients keep receiving on-cadence snapshots |
| NET-10 | Finish AC-B-001: add the oracle/replay/small-co-op demotion language to LockstepSession.h/LockstepConfig comments and fix the stale line citations in docs/networking-scale-architecture.md:255-259 | 019 | S | low | — | todo | NEW: NetDemotionDocGrep check — grep asserts LockstepSession.h states the oracle/replay/small-co-op scope + the arch-doc line anchors match the tree |
| NET-11 | True single-port multi-connection accept for TCP (and GNS under NET-08): one listen socket fanning N connections into ReplicationServer::AddClient, replacing the port-per-client scheme as the real dedicated-server shape (FR-F-003 full form) | 019 | M | medium | NET-08 | todo | NEW: GnsMultiConnect ctest — one listen port accepts N concurrent connections and each fans out via AddClient (spec 019 AC-F-001) |
| NET-12 | Steam SDR second-machine validation (FR-F-002/OQ-3): the ship transport's scale behavior over the wire; blocked on hardware, GNS soak stands in as the local proxy | 019 | M | high | NET-08 | todo | Second-machine validation (test/features/TDD-LOCK.md:34-38 locked scenario; local substitute: GNS UDP) |
| NET-13 | POSIX TcpTransport implementation (non-_WIN32 stub today, LockstepSession.cpp:441-449) so a Linux dedicated server can network | new | M | low | — | todo | NEW: TcpTransportPosixLoopback ctest — Listen/Connect/SendFrame/TryReceiveFrame roundtrip passes on a non-_WIN32 build |

All items keep `luminumbra_server_app --smoke == 6f008a9f637c40b7` (run==replay) as a standing
cross-cutting gate (spec 019 NFR-001/AC-001); replication stays a read-only observer of the sim.
