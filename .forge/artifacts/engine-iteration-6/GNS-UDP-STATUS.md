# Standalone GameNetworkingSockets (UDP) transport — WORKING

## Result: real UDP replication, two processes, one PC ✅
The earlier protobuf wall was an artifact of probing the old GNS **v1.4.1** tag.
GNS **master** + the Windows-native **BCrypt** crypto backend + the ucrt64
**protobuf 31.1 / abseil** packages configures, builds (`libGameNetworkingSockets_s.a`),
links into the server, and runs:

```
gns-host: listening over UDP; peer connected over UDP; ran 60 ticks
gns-join: connected over UDP; mirrored host over UDP -- seq 60, 4 entities,
          controlled avatar (id 1) at x=12.57 m. Real UDP replication confirmed.
```

This is the path the Steam SDK could NOT provide on a single machine (Steam's
one-instance-per-app rule). GNS standalone is exactly what Valve ships it for:
local dev + Steam-independent dedicated servers.

## How to build + run it
```
# configure once with GNS on (FetchContent builds GNS master, ~2-3 min the first time)
cmake -S . -B build/debug -DLUMINUMBRA_ENABLE_GNS=ON
cmake --build build/debug --target luminumbra_server_app
# two terminals (ucrt64/bin must be on PATH for the protobuf/abseil runtime DLLs):
luminumbra_server_app --net-host --udp --port 27070 --avatars 4 --ticks 60
luminumbra_server_app --net-join --udp --host 127.0.0.1 --port 27070 --ticks 60
```
Default builds keep `LUMINUMBRA_ENABLE_GNS=OFF`; `--udp` without the flag errors
cleanly. GnsTransport shares the ISteamNetworkingSockets API with
SteamNetworkingTransport, so the same code ports to Steam (P2P/SDR) later.

## Notes
- Crypto: `USE_CRYPTO=BCrypt` (no OpenSSL/libsodium dependency).
- Runtime: the GNS-enabled exe needs libprotobuf + abseil DLLs (ucrt64/bin on
  PATH -- already standard for this toolchain). A redistributable build would
  bundle them; not needed for local dev.
- The replication logic over the wire is the SAME as the TCP `NetworkedReplication`
  gate; only the transport differs, and it's manually validated above. A ctest
  gate is intentionally NOT added (would force the multi-minute GNS FetchContent
  into every CI build).
