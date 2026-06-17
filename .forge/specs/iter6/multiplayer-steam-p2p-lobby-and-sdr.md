# T-I6-044: Steam P2P Lobby and SDR Spec

## Status

Deferred. The Iteration 6 repo state available to this task contains only the common network fixture layer under `src/luminumbra_common/network`; it does not contain Steamworks, GameNetworkingSockets, or runtime transport implementation files to modify in this task scope.

## Goal

Define the production Steam multiplayer shape without claiming implementation:

- Host creates a Steam lobby and publishes the host identity needed by Steam Networking Sockets.
- Joiners discover or receive the lobby, read the host identity, and connect with Steam P2P over SDR.
- The transport presents the existing lockstep/replication contract to gameplay code.
- Steam-specific setup remains optional and gated so default builds do not require the non-redistributable Steamworks SDK.

## Non-goals

- No Steamworks SDK files are committed.
- No Steam App ID is hard-coded for production.
- No two-machine Steam validation is claimed from this worktree.
- No GameNetworkingSockets runtime transport status is promoted beyond what is present in the scoped source tree.

## Required Design

### Lobby Lifecycle

- Host path creates a Steam lobby in friends-only or invite-only mode for development.
- Lobby metadata includes protocol version, build compatibility marker, listen role, and host Steam Networking identity.
- Join path validates lobby metadata before attempting transport connection.
- Lobby leave/destruction is tied to host shutdown and explicit client disconnect.

### SDR Transport

- Steam P2P connections use `ISteamNetworkingSockets` P2P APIs and `SteamNetworkingIdentity`, not direct IP.
- Reliable and unreliable frame delivery map onto the existing lockstep transport semantics.
- Connection status callbacks must cover connect, accepted, closed, failed, timeout, and authentication failure states.
- Host/client logs must identify whether a connection is direct or relayed after Steam reports the route.

### Build Gating

- Steam support remains behind an opt-in build flag such as `LUMINUMBRA_ENABLE_STEAM`.
- Builds without that flag must compile without Steamworks headers or libraries.
- Runtime `--steam` options must fail with a clear message when the Steam build flag is disabled.
- `steam_appid.txt` may be generated for local development with App ID 480, but production App ID selection must remain external.

## Validation Plan

The Steam path requires a real Steam client session on two machines or two accounts. Local two-process validation on one machine is not sufficient for this layer because Steam associates both processes with the same app/user session.

Minimum validation evidence before marking complete:

- Host creates a lobby and publishes expected lobby metadata.
- Joiner finds or joins the lobby and validates metadata compatibility.
- Joiner establishes a Steam P2P/SDR connection to the host identity.
- Host accepts the connection and exchanges at least one reliable and one unreliable frame.
- Existing replication/lockstep payloads run over the Steam transport for a bounded tick count.
- Logs capture Steam route state and connection close reason.

## Deferred Acceptance

This task records the intended Steam P2P/lobby/SDR contract and explicitly defers implementation/verification until the Steam transport sources and two-machine Steam validation environment are available in scope.
