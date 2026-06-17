# Steamworks transport — status

## What's done (committed, gated OFF by default)
- **SDK wired**: `vendor/steamworks/` (SDK 164, gitignored — non-redistributable).
  CMake option `LUMINUMBRA_ENABLE_STEAM` (OFF by default) adds the headers, links
  `steam_api64.lib`, defines `LUMINUMBRA_ENABLE_STEAM`, and copies `steam_api64.dll`
  next to the server exe.
- **`SteamNetworkingTransport : ILockstepTransport`** (`net/SteamNetworkingTransport.{h,cpp}`)
  over `ISteamNetworkingSockets`: `Listen`/`Connect` (direct IP), reliable +
  unreliable `SendFrame` (maps to `k_nSteamNetworkingSend_Reliable/Unreliable`),
  message-framed `TryReceiveFrame`, async accept/connect via the global
  connection-status callback. `SteamLink::Init/RunCallbacks/Shutdown` wraps
  `SteamAPI_Init` (+ writes `steam_appid.txt`=480 for dev) + `InitRelayNetworkAccess`.
- **`--net-host --steam` / `--net-join --steam`** run the SAME authoritative-server
  replication over the Steam transport.

## Verified
- ✅ **Compiles against the Steam headers** on msys2/ucrt64.
- ✅ **Links `steam_api64.lib` with GCC/MinGW** — the main risk (MSVC import lib +
  GCC) is cleared.
- ✅ **`SteamAPI_Init` succeeds** against the running Steam client (app id 480):
  `"SteamLink: initialized (Steamworks SDK, app id 480)."`
- ✅ Host creates the listen socket; client attempts `ConnectByIPAddress`.

## Known limitation: localhost two-process test does NOT connect
Running `--net-host --steam` and `--net-join --steam` as two processes on ONE
machine with the same app id (480) does not establish the connection (both time
out). This is **Steam's single-instance-per-app / local-loopback model**, not a
code defect — Steam treats both processes as the same app+user, and its
IP/relay networking is not meant for two same-app instances on one box.

### How to actually validate the Steam path
- **Two machines** (or two Steam accounts), host on one, join on the other by IP —
  the direct-IP path should connect.
- **P2P + lobby + SDR** (the real shipping shape): create/join a Steam lobby,
  connect via `ConnectP2P` to the host's `SteamNetworkingIdentity` over the SDR
  relay. This is the next layer (needs lobby create/join + identity exchange) and
  is the recommended production path. Tracked as a follow-up.
- Meanwhile, the **TCP transport + NetworkedReplication gate** already prove the
  replication logic over the wire locally (two processes), so the Steam work is
  isolated to the transport handshake, not the replication stack.

## Default build unaffected
`LUMINUMBRA_ENABLE_STEAM` defaults OFF; the transport body is `#ifdef`-guarded and
`--steam` without the build flag prints a clear error. No SDK dependency in the
normal build; world_hash untouched (transport-side only).
