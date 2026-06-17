# UDP transport (GameNetworkingSockets) — viability findings

## What I tried
Isolated FetchContent probe (separate build dir, main tree untouched) of
GameNetworkingSockets (GNS) v1.4.1 with `USE_CRYPTO=OpenSSL` on the msys2/ucrt64
toolchain.

## Result: blocked on protobuf, by design not by accident
- ✅ GNS fetched + configured through crypto: **OpenSSL 3.5.1 found** (BCrypt + EVP
  probes pass). Crypto backend is a non-issue.
- ❌ GNS requires **Protobuf**, and the only version packaged for ucrt64 is
  **protobuf 31.1** (the new versioning, ex-5.x — abseil-based, 2024+). GNS v1.4.1
  targets **protobuf 3.x**; the generated-code API and CMake integration changed
  substantially between them. Installing 31.1 and pointing GNS at it would almost
  certainly fail in GNS's generated `.pb.cc` against the old expectations — the
  classic protobuf-major-version fight, on MinGW.

## The important realisation
**The Steamworks SDK *is* GameNetworkingSockets** — `ISteamNetworkingSockets` is
literally the same Valve codebase, shipped **prebuilt** (`steam_api64.dll`) with
its **own bundled protobuf**. So if the owner is grabbing the Steamworks SDK
anyway (Layer 3), building standalone GNS from source is **largely redundant
work** AND the painful path. The Steam SDK gives the UDP transport (plus the SDR
relay) with **zero build fight**.

## Three real paths to UDP (recommendation: #1)
1. **Use the Steamworks SDK's bundled networking (RECOMMENDED).** Wait for the
   owner's SDK drop (STEAMWORKS-SETUP.md), then implement ONE
   `SteamNetworkingTransport : ILockstepTransport` against `ISteamNetworkingSockets`
   — that gives UDP + reliability + encryption + NAT/SDR, prebuilt, no protobuf
   build. The transport code is identical whether the lib is standalone GNS or the
   Steam SDK (same API). This is the fastest path to real UDP.
2. **Standalone GNS from source, properly.** Use GNS `master` (supports
   protobuf 4/5 + abseil) and let it fetch/build its OWN protobuf+abseil (vendored)
   so it doesn't collide with the ucrt64 protobuf 31. Real but multi-hour MinGW
   integration work; only worth it if we need a Steam-independent dedicated server
   binary (no Steam client required).
3. **Pin protobuf 3.21 for GNS.** Build protobuf 3.21 from source just for GNS
   v1.4.1. Also multi-hour; brittle.

## Status
- The `ILockstepTransport` seam + the real **TCP transport** (`--net-host` /
  `--net-join`, NetworkedReplication gate) already prove over-the-wire replication.
- Any of the three UDP paths drops into that same seam. Recommend #1 (Steam SDK),
  which makes the UDP work a single transport class with no dependency build.
- Did NOT install protobuf 31 or commit any speculative/untested transport code
  (would be dead code biased toward the wrong path).
