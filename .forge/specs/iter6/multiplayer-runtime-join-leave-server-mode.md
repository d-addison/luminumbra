# T-I6-041 Runtime Join/Leave Server Mode

## Goal

Dedicated TCP host mode must keep the authoritative simulation running while clients join after startup or leave before shutdown.

## Scope

- `luminumbra_server_app --net-host --server-mode` starts ticking immediately instead of blocking until every configured client slot connects.
- Client slots still use the deterministic one-based port mapping from `TryNetworkMultiClientAcceptPortForClient`.
- Late clients are accepted on their assigned port and retain their stable player/avatar id.
- Disconnected clients do not fail the host; their movement input is zeroed and the server keeps ticking until `--ticks` completes.
- `--artifact <path>` on the host writes a `luminumbra.net_host_server_mode.v1` JSON lifecycle summary.

## Gate Contract

- `NetworkRuntimeJoinLeaveMeetsBaseline` validates the runtime lifecycle fixture.
- The fixture records empty-server ticks, late join, leave, continued host ticks, stable ids, and deterministic accept ports.
- The legacy `--net-host` path remains unchanged unless `--server-mode` is supplied.
