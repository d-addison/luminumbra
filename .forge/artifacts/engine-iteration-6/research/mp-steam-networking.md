# Multiplayer Research: Steam Networking Integration

**Scope.** How to run Luminumbra's authoritative-dedicated-server multiplayer
over Steam, and whether our existing `ILockstepTransport` seam is sufficient to
host a Steam backend. We have a `LoopbackTransport` (tests), a `_WIN32`-guarded
`TcpTransport` (loopback/LAN dev), and a replication layer (Usercmd/Snapshot/Ack
with its own most-recent-wins seq/ack, plus a reliable event channel for
join/leave/world-edits/chat). We deferred a raw winsock UDP transport. The owner
asks: "make sure it'll work with Steam." This brief recommends the concrete
Steam path and the dev path that needs no Steam client.

Target: 20+ players, persistent join/leave, full server physics, Source/GMod-style
state replication.

---

## Recommended approach

Add a third transport implementation behind the existing seam:

```
class SteamNetworkingTransport final : public ILockstepTransport { ... };
```

built on **`ISteamNetworkingSockets`** — the modern, connection-handle-oriented,
**message-based** Steamworks networking API. This is the right primitive because:

1. **It is message-oriented, not a byte stream.** Per the official docs, message
   boundaries are preserved end-to-end: each `SendMessageToConnection` call
   matches one `ReceiveMessagesOnConnection` result on the remote host one-for-one
   (the docs state the sizes "WILL match … one-for-one"). This is a *better* fit
   for our seam than `TcpTransport` — `SendFrame`/`TryReceiveFrame` are already
   "one complete framed message" operations, so with Steam we drop the manual
   length-prefix reassembly that `TcpTransport::PumpRecv` has to do over a stream.

2. **Reliable AND unreliable on one connection.** A single connection carries
   both delivery modes, selected per-send by a flag:
   - `k_nSteamNetworkingSend_Unreliable` — our snapshot stream (most-recent-wins;
     drops are fine, the seq/ack layer already tolerates them).
   - `k_nSteamNetworkingSend_Reliable` — our event channel (join/leave, world
     edits, chat) and the handshake. Steam's reliable mode handles
     fragmentation, reassembly, retransmission, and in-order delivery internally.

3. **Polling model fits our per-tick pump.** We pull with
   `ReceiveMessagesOnConnection` (single connection) or, on the server,
   `ReceiveMessagesOnPollGroup` after putting every client connection into one
   poll group via `CreatePollGroup` + `SetConnectionPollGroup`. This is exactly
   our non-blocking `TryReceiveFrame` drain-loop shape — no blocking, no threads
   required in the tick loop.

**Shipping transport = Steam Datagram Relay (SDR).** For the released build the
dedicated server connects through SDR rather than exposing a raw UDP port. SDR is
Valve's relay backbone: it provides DDoS protection and IP hiding ("IP addresses
are never revealed"), authentication, encryption, rate-limiting, and often *lower*
ping via Valve's backbone routing. Two SDR deployment shapes apply to us:
  - **Relayed P2P / listen-server** (a player hosts): server side
    `CreateListenSocketP2P`, client side `ConnectP2P` with the host's identity —
    NAT punch-through and relay fallback are automatic.
  - **Hosted dedicated server** (a real server box in a known data center):
    server side `CreateHostedDedicatedServerListenSocket`; clients call
    `ConnectToHostedDedicatedServer` using a `SteamDatagramRelayAuthTicket` issued
    & signed by our game coordinator. There is also a **FakeIP** path
    (`BeginAsyncRequestFakeIP` / `CreateListenSocketP2PFakeIP`) for code that
    assumes IPv4 addressing.

**Dev/test transport = GameNetworkingSockets (GNS), no Steam client.** Valve's
open-source `GameNetworkingSockets` implements *the same API* (same symbol names:
`CreateListenSocketIP`, `ConnectByIPAddress`, `SendMessageToConnection`,
`ReceiveMessagesOnConnection`, the same send flags). License is BSD-3-Clause and
"Steam is not needed." So we write `SteamNetworkingTransport` once against the
shared API surface, develop and run CI against GNS over plain UDP
(`CreateListenSocketIP` / `ConnectByIPAddress`) with no Steam dependency, then at
ship-time link the Steamworks SDK build of the same API to gain SDR. **Caveat:**
SDR is *not* in the open-source GNS build — it is a Steam-hosted service. So GNS
gives us the API and direct-UDP transport for dev; the relay/anti-DDoS/IP-hiding
benefits arrive only when linked against Steamworks at ship.

**Is the seam sufficient? Yes — with one small additive tweak.** The current
`ILockstepTransport::SendFrame(frame)` has no way to say "this frame is reliable
vs unreliable." `TcpTransport` ignores that (everything is reliable in-order),
which is wrong for snapshots over Steam. Add an optional reliability argument with
a default so existing call sites and impls are unchanged:

```cpp
enum class Delivery { Unreliable, Reliable };

// add to ILockstepTransport (default keeps TCP/Loopback callers compiling):
virtual bool SendFrame(const std::vector<std::uint8_t>& frame,
                       Delivery delivery = Delivery::Reliable) = 0;
```

`LoopbackTransport` and `TcpTransport` ignore the flag (they are always reliable,
which is a safe superset). `SteamNetworkingTransport` maps `Reliable ->
k_nSteamNetworkingSend_Reliable`, `Unreliable -> k_nSteamNetworkingSend_Unreliable`.
The replication endpoint then sends snapshots `Unreliable` and events `Reliable`.
Everything else in the seam (`TryReceiveFrame`, `IsPeerConnected`, `Close`) maps
1:1 to Steam with no change.

One structural note: `ILockstepTransport` as written models **one peer link**
(the 1v1 lockstep heritage). The dedicated server with 20+ players wants
**one transport per client connection**, with the server owning a poll group
across them. That is already how the replication layer thinks (per-receiver
snapshots). So the server holds a `std::vector<SteamNetworkingTransport>` (one per
accepted connection) plus a shared poll group; each element is still a plain
`ILockstepTransport`. No interface change needed — just a per-connection instance
fed by Steam's `SteamNetConnectionStatusChangedCallback` on accept.

---

## How it maps to our transport seam

| Our seam | Steam (`ISteamNetworkingSockets`) | Notes |
|---|---|---|
| `SendFrame(frame, Reliable)` | `SendMessageToConnection(conn, data, len, k_nSteamNetworkingSend_Reliable, nullptr)` | Events, handshake, world edits, chat. Steam handles fragment/reassemble/retransmit/in-order. |
| `SendFrame(frame, Unreliable)` | `SendMessageToConnection(conn, data, len, k_nSteamNetworkingSend_Unreliable, nullptr)` | Snapshots. Drops tolerated; our seq/ack does most-recent-wins. |
| `TryReceiveFrame(out)` | `ReceiveMessagesOnConnection(conn, &pMsg, 1)` (client) / `ReceiveMessagesOnPollGroup(group, &pMsg, 1)` (server) | Returns 0 → `false` (nothing now); ≥1 → copy `pMsg->m_pData`/`m_cbSize` into `out`, `pMsg->Release()`, return `true`. Message boundaries preserved, so no reassembly buffer like `TcpTransport::m_recv_buffer`. |
| `IsPeerConnected()` | track state from `SteamNetConnectionStatusChangedCallback_t` (`k_ESteamNetworkingConnectionState_Connected` / `_ClosedByPeer` / `_ProblemDetectedLocally`) | Connection-status callback flips a member bool; mirrors how `TcpTransport` sets `m_peer_closed`. |
| `Close()` | `CloseConnection(conn, reason, debug, bEnableLinger)` | Clean close; peer sees a status-changed callback. |
| (per tick, required) | `RunCallbacks()` on the interface/`SteamNetworkingSockets()` | Must be pumped every tick to drive callbacks and the message pump — call it at the top of the session pump. |

**Reliable vs unreliable channel split.** Our design already separates two logical
streams: the **most-recent-wins snapshot stream** and the **reliable event
channel**. On Steam both ride *one* connection, distinguished only by the send
flag. Ordering interaction to be aware of:

- Steam's reliable channel is a single **in-order** reliable stream. All
  `Reliable` sends on a connection are delivered in order relative to each other —
  good for join/leave/world-edit/chat where order matters. Our reliable events
  should *not* assume independent ordering against the unreliable snapshots
  (the two flag classes are not ordered relative to each other).
- Our **most-recent-wins** logic stays in the replication layer, not the transport:
  even though Steam *could* deliver unreliable messages out of order, our snapshots
  carry `snapshot_seq`, and the receiver discards any snapshot older than the
  newest seen. So we keep our own seq guard; we do **not** rely on Steam ordering
  for snapshots. (Optionally `k_nSteamNetworkingSend_NoDelay` / `NoNagle` reduce
  latency for the snapshot send, but our per-tick flush already bounds delay.)

---

## Alternatives considered (and why rejected)

- **Raw winsock/POSIX UDP only (the deferred transport), no Steam.** Viable for
  LAN/dev and we still want a thin direct path, but for *shipping* it means we
  build NAT traversal, anti-DDoS, encryption, and IP-hiding ourselves — exactly
  the services SDR provides for free once on Steam. Rejected as the *ship*
  transport; kept only as the dev/LAN path, and even that is better served by
  GNS direct-UDP (same API as the ship path, so no second codebase). **Decision:
  do not invest further in a bespoke raw-UDP transport beyond what exists; route
  the deferred UDP work into the GNS/Steam transport instead.**
- **TCP for gameplay (extend `TcpTransport`).** Head-of-line blocking on a stream
  socket is fatal for a 30 Hz snapshot stream: one lost segment stalls *all*
  subsequent snapshots until retransmit. TCP also can't express "drop this stale
  snapshot." Keep `TcpTransport` for loopback/LAN dev convenience only; reject it
  for real-time replication.
- **`ISteamNetworkingMessages`** (the connectionless, UDP-like Steam interface).
  Simpler (no connection handles), supports reliable + large messages, and also
  relays over the Valve backbone. Rejected as the primary API for a *persistent
  dedicated server* because we want explicit per-client connection handles, poll
  groups, and connection-status callbacks for join/leave bookkeeping at 20+
  players — `ISteamNetworkingSockets` gives that directly. (Messages is a
  reasonable fallback for casual P2P, not our authoritative-server model.)
- **Third-party relays (Photon, Epic EOS relay, custom).** Add a vendor and cost,
  duplicate what SDR gives a Steam title for free, and don't hide IPs via Valve's
  backbone. Rejected — we are shipping on Steam.

---

## Integration notes

- **SDK vs GNS.**
  - *Dev/CI:* link **GameNetworkingSockets** (BSD-3-Clause, no Steam client/appid
    needed). Brings in OpenSSL/libsodium + protobuf as deps. Direct-UDP only (no
    SDR). Same symbols as the ship API, so the transport code is identical.
  - *Ship:* link the **Steamworks SDK** flavor of the same API to get SDR. The
    Steamworks SDK is free but requires a Steamworks partner account and an appid
    (480 "Spacewar" is the standard test appid during bring-up).
- **AppID / auth / lobbies (high level).**
  - *Find a session:* `ISteamMatchmaking` lobbies — `CreateLobby`, `RequestLobbyList`
    (+ string/numeric/distance filters), `JoinLobby`; lobby metadata via
    `SetLobbyData`/`GetLobbyData`. The lobby is just a coordination room (up to 250
    members); players read the host/server identity from lobby data, then *leave
    the lobby* and connect to the actual server via `ConnectP2P` /
    `ConnectToHostedDedicatedServer`.
  - *Authenticate:* client gets a ticket (`ISteamUser::GetAuthSessionTicket`,
    or `GetAuthTicketForWebApi` for backend) and sends it on connect; the server
    runs `ISteamGameServer::BeginAuthSessionForUser` (after `SteamGameServer_Init`
    + `LogOn`), which validates the ticket and confirms appid ownership
    (`k_EAuthSessionResponseNoLicenseOrExpired` if they don't own the game).
    `EndAuthSession` on disconnect. This is how the dedicated server gates who may
    join — it slots in at our handshake step, *before* the lockstep/replication
    Hello.
  - *SDR dedicated-server credentials* arrive via env (`SDR_LISTEN_PORT`, `SDR_IP`,
    `SDR_POPID`, `SDR_PRIVATE_KEY`, `SDR_CERT`, `SDR_NETWORK_CONFIG`) and the server
    publishes its `SteamDatagramHostedAddress` to the game coordinator; the
    coordinator mints `SteamDatagramRelayAuthTicket`s for clients. (Exact env var
    names per Valve's hosted-dedicated docs — treat the list as the documented set,
    confirm against the SDK headers at integration time.)
- **Build guarding for headless / non-Steam.** Mirror the existing
  `_WIN32` `TcpTransport` pattern. Gate the Steam transport behind a
  `LUMINUMBRA_WITH_STEAM` (and/or `LUMINUMBRA_WITH_GNS`) CMake option. When neither
  is set, compile `SteamNetworkingTransport` as **stubs** whose methods report no
  connection (`IsPeerConnected()==false`, `SendFrame` returns false), exactly like
  the non-`_WIN32` `TcpTransport` stub path, so headless CI and the loopback gates
  still build with zero Steam/GNS dependency. The `Delivery` enum and seam change
  are unconditional (header-only, no networking lib needed).
- **When to swap raw-UDP → Steam.** Do not build out the deferred bespoke
  winsock-UDP transport. Instead: (1) now — add the `Delivery` flag to the seam and
  thread it through the replication layer; (2) dev — implement
  `SteamNetworkingTransport` against **GNS** (direct UDP) and run the existing
  replication/lockstep gates over it; (3) ship — flip the CMake option to link the
  Steamworks SDK build and switch the server to `CreateHostedDedicatedServerListenSocket`
  + SDR tickets (or `CreateListenSocketP2P` for player-hosted). No replication-layer
  rewrite at any step — only the transport instance and the link target change.

---

## Open risks

- **Seam reliability flag is a real (small) change.** Adding `Delivery` to
  `SendFrame` touches the interface and all three impls plus the replication
  endpoint's send calls. Defaulted argument keeps it source-compatible, but the
  replication layer must be edited to send snapshots `Unreliable` (today it sends
  everything over a single reliable-style seam, which over Steam would needlessly
  retransmit stale snapshots).
- **One-peer seam vs many-client server.** The current `ILockstepTransport` and
  `LockstepSession` are 2-peer. The 20+ player dedicated server needs N
  per-connection transports + a poll group + a connection-accept path. This is a
  server-architecture change (already anticipated by the replication layer), not a
  transport-interface change — but it must be designed, not assumed.
- **GNS dependency weight in CI.** GNS pulls OpenSSL/libsodium + protobuf; budget
  build time and vendor it via FetchContent (per the project's
  no-junction-into-vendor rule) rather than a system install.
- **SDR is Steam-only.** All anti-DDoS/IP-hiding/relay benefits are absent in dev
  (GNS). Anything that *depends* on hidden IPs or relay routing can only be tested
  against a real Steam appid/partner setup late in the cycle. Plan a Steam-linked
  smoke test before ship; don't discover SDR-specific behavior at launch.
- **Exact SDR env var / coordinator symbol names.** The hosted-dedicated-server
  credential env vars and ticket-signing flow are quoted from Valve docs; confirm
  the precise spellings and the coordinator API against the shipped Steamworks SDK
  headers (`steamnetworkingsockets.h`, `isteamnetworkingsockets.h`,
  `steamdatagram_*`) at integration — do not hard-code from this brief alone.
- **Per-tick `RunCallbacks`.** Steam requires `RunCallbacks` to be pumped or
  messages/status callbacks stall. Easy to forget; wire it into the session pump
  and assert it's called.

---

## Citations

1. **ISteamNetworkingSockets API reference** —
   https://partner.steamgames.com/doc/api/ISteamNetworkingSockets
   *Confirms exact symbols: `CreateListenSocketIP`/`CreateListenSocketP2P`/
   `CreateHostedDedicatedServerListenSocket`/`CreateListenSocketP2PFakeIP`,
   `ConnectByIPAddress`/`ConnectP2P`/`ConnectToHostedDedicatedServer`,
   `SendMessageToConnection` with `k_nSteamNetworkingSend_Reliable`/`_Unreliable`,
   `ReceiveMessagesOnConnection`/`ReceiveMessagesOnPollGroup` +
   `CreatePollGroup`/`SetConnectionPollGroup`; API is message-oriented (boundaries
   preserved, sizes match one-for-one) and reliable+unreliable share one connection.*

2. **GameNetworkingSockets — ValveSoftware (GitHub)** —
   https://github.com/ValveSoftware/GameNetworkingSockets
   *Open-source implementation of the same-named Steamworks API; BSD-3-Clause;
   "Steam is not needed"; supports reliable + unreliable messages; SDR is the
   Steam-hosted build only ("you can access the additional services provided by the
   Steam Datagram Relay network" on Steam, not in the OSS build). This is the
   dev/CI path that needs no Steam client.*

3. **Steam Datagram Relay (Steamworks docs)** —
   https://partner.steamgames.com/doc/features/multiplayer/steamdatagramrelay
   *"Valve's virtual private gaming network"; "IP addresses are never revealed";
   authentication + encryption + rate-limiting + faster backbone routing;
   dedicated-server flow via `CreateHostedDedicatedServerListenSocket`,
   `SteamDatagramHostedAddress`, `SteamDatagramRelayAuthTicket`,
   `ConnectToHostedDedicatedServer`, and the FakeIP path
   (`BeginAsyncRequestFakeIP`).*

4. **Steam multiplayer networking overview** —
   https://partner.steamgames.com/doc/features/multiplayer/networking
   *Positions `ISteamNetworkingSockets` (connection-handle, lower-level) vs
   `ISteamNetworkingMessages` (connectionless, UDP-like); "All P2P connections are
   automatically relayed over the Valve backbone when appropriate"; SDR "prevents
   IP addresses from being revealed and in many cases improves ping times."*

5. **Steam multiplayer feature picker** —
   https://partner.steamgames.com/doc/features/multiplayer
   *Steam Game Servers recommended for "highly competitive games … or games with
   persistent servers that keep running even after all players leave" — our
   authoritative-dedicated-server model.*

6. **Steam matchmaking & lobbies** —
   https://partner.steamgames.com/doc/features/multiplayer/matchmaking
   *`ISteamMatchmaking`: `CreateLobby`/`RequestLobbyList`(+filters)/`JoinLobby`,
   `SetLobbyData`/`GetLobbyData`, lobby up to 250 members, then leave-and-connect to
   the real server. Lobby = discovery/coordination, not the gameplay transport.*

7. **Steam user authentication** —
   https://partner.steamgames.com/doc/features/auth
   *`ISteamUser::GetAuthSessionTicket`/`GetAuthTicketForWebApi`;
   `ISteamGameServer::BeginAuthSessionForUser` (after `SteamGameServer_Init`+`LogOn`)
   validates ticket + appid ownership (`k_EAuthSessionResponseNoLicenseOrExpired`);
   `EndAuthSession` on disconnect — how the dedicated server gates joins.*
