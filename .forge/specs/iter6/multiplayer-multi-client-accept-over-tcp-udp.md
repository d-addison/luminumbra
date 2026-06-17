# Multiplayer Multi-Client Accept Over TCP/UDP

## Task

Extend the headless multiplayer harness so one host process can accept more than
one remote replication client over the existing TCP transport and the standalone
GameNetworkingSockets UDP transport.

## Contract

- Host accepts client ids `1..N` with `--clients N`.
- Joiners select their controlled avatar with `--player-id K`.
- TCP and GNS UDP use the same deterministic port mapping:
  `accept_port = base_port + player_id - 1`.
- Host spawns at least `N + 1` avatars so remote clients can control avatar ids
  `1..N` while avatar `0` remains available.
- Host registers each accepted transport with one `ReplicationServer` and
  broadcasts every snapshot to all accepted clients.
- Each joiner uses its `--player-id` in `ReplicationClient` and `UsercmdMsg`.

## Commands

TCP, two clients:

```text
luminumbra_server_app --net-host --clients 2 --port 27070 --avatars 3 --ticks 60
luminumbra_server_app --net-join --player-id 1 --host 127.0.0.1 --port 27070 --ticks 60
luminumbra_server_app --net-join --player-id 2 --host 127.0.0.1 --port 27070 --ticks 60
```

GNS UDP, two clients:

```text
luminumbra_server_app --net-host --udp --clients 2 --port 27070 --avatars 3 --ticks 60
luminumbra_server_app --net-join --udp --player-id 1 --host 127.0.0.1 --port 27070 --ticks 60
luminumbra_server_app --net-join --udp --player-id 2 --host 127.0.0.1 --port 27070 --ticks 60
```

## Acceptance Criteria

- Existing single-client TCP and GNS UDP commands continue to work with default
  `--clients 1` and `--player-id 1`.
- `--clients 2` accepts client `1` on the base port and client `2` on
  `base_port + 1`.
- Host applies user commands from every accepted client id.
- Join success validates a snapshot containing the selected player id.
- The shared port-mapping helper rejects client id `0` and port overflow.
