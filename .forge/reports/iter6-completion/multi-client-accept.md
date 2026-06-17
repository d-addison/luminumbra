# Multi-Client Accept Completion

## Summary

Implemented multi-client acceptance for the headless TCP and standalone GNS UDP
replication harness.

## Changes

- Added `--clients N` for host-side expected client count.
- Added `--player-id K` for join-side controlled avatar/client id.
- Added a shared `TryNetworkMultiClientAcceptPortForClient` helper in
  `src/luminumbra_common/network`.
- TCP host now accepts client ids `1..N` on deterministic ports and registers
  every accepted transport with one `ReplicationServer`.
- GNS UDP host follows the same client-id to port mapping and replication loop.
- TCP/GNS joiners connect to the mapped port and send user commands for their
  selected player id.

## Verification

- `git diff --check -- src/luminumbra_common/network/NetworkLoopbackAuthority.h src/luminumbra_common/network/NetworkLoopbackAuthority.cpp src/luminumbra_server/main_server.cpp` passed.
- `cmake --build build\debug --target luminumbra_server_app` could not run because
  this worktree has no configured CMake cache at `build\debug`.

## Manual Run Pattern

TCP:

```text
luminumbra_server_app --net-host --clients 2 --port 27070 --avatars 3 --ticks 60
luminumbra_server_app --net-join --player-id 1 --host 127.0.0.1 --port 27070 --ticks 60
luminumbra_server_app --net-join --player-id 2 --host 127.0.0.1 --port 27070 --ticks 60
```

GNS UDP:

```text
luminumbra_server_app --net-host --udp --clients 2 --port 27070 --avatars 3 --ticks 60
luminumbra_server_app --net-join --udp --player-id 1 --host 127.0.0.1 --port 27070 --ticks 60
luminumbra_server_app --net-join --udp --player-id 2 --host 127.0.0.1 --port 27070 --ticks 60
```
