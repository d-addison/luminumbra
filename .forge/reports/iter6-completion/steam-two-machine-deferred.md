# Steam Two-Machine Validation Deferred

## Task

`T-I6-044-steam-p2p-lobby-sdr`

## Result

Deferred. Steam P2P lobby and SDR validation was not completed in this worktree because the scoped source context does not include Steam transport implementation files and the required two-session Steam validation environment is not available to this task.

## Why Deferred

- Steam P2P/SDR acceptance requires two distinct Steam identities, using either two machines or two Steam accounts.
- A same-machine same-account two-process run is not representative acceptance evidence for Steam lobby/SDR.
- The Steamworks SDK is non-redistributable and must be supplied outside source control.
- The current scoped source context contains common network fixtures, not the Steam lobby/transport implementation needed to run the validation.

## Required Evidence To Close Later

- Steam-enabled build compiles with the SDK supplied from an ignored local path.
- Host creates a lobby and publishes protocol/build metadata plus host Steam identity.
- Joiner joins the lobby, validates metadata, and connects to the host identity over Steam P2P/SDR.
- Host accepts the joiner and both sides exchange reliable and unreliable frames.
- Existing replication payloads run over the Steam transport for a bounded tick count.
- Logs include lobby ID, host/join roles, connection lifecycle, Steam route state, close reason, and tick/frame counts.

## Linked Artifacts

- `.forge/specs/iter6/multiplayer-steam-p2p-lobby-and-sdr.md`
- `.forge/artifacts/engine-iteration-6/STEAMWORKS-SETUP.md`
- `.forge/artifacts/engine-iteration-6/STEAM-TRANSPORT-STATUS.md`
- `.forge/artifacts/engine-iteration-6/GNS-UDP-STATUS.md`
