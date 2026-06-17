# Steam Transport Status

## Current Status

Deferred. In the scoped Iteration 6 worktree for this task, Steam transport source files are not present under the allowed network source context. No Steam P2P/lobby/SDR implementation or two-machine validation is claimed by this status artifact.

## Intended Transport Shape

- Steam support stays optional behind an explicit build flag such as `LUMINUMBRA_ENABLE_STEAM`.
- The transport should implement the existing replication/lockstep transport contract without changing gameplay ownership semantics.
- Lobby create/join should be the shipping entry point.
- Lobby metadata should carry protocol/build compatibility and the host `SteamNetworkingIdentity`.
- Runtime connection should use Steam P2P over SDR through `ISteamNetworkingSockets`, not a direct-IP-only path.
- Reliable and unreliable payloads should map to Steam Networking Sockets send modes.

## Deferred Work

- Add Steamworks build integration with SDK paths excluded from source control.
- Add Steam API lifetime management and callback pumping.
- Add host lobby creation, metadata publication, lobby search/join, and invite-compatible join handling.
- Add P2P/SDR connect and accept handling using Steam identities.
- Add route diagnostics so logs report direct versus relayed state when Steam exposes it.
- Add bounded host/join smoke validation using two Steam identities.

## Validation Required Before Completion

- Steam-enabled build compiles and links locally with SDK files supplied out of tree.
- Host can create a Steam lobby and publish metadata.
- Joiner can resolve lobby metadata and connect to the host identity.
- Host and joiner exchange at least one reliable frame and one unreliable frame.
- Existing replication payloads run over the Steam transport for a bounded tick count.
- Logs capture connection lifecycle, route state, timeout/failure reasons, and clean shutdown.

## Localhost Limitation

A same-machine, same-account two-process test is not sufficient evidence for Steam P2P/SDR because it does not represent two distinct Steam identities. The acceptance run remains deferred to a two-machine or two-account setup; see `.forge/reports/iter6-completion/steam-two-machine-deferred.md`.
