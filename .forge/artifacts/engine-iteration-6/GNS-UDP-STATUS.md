# GameNetworkingSockets UDP Status

## Current Status

Deferred in this worktree. The scoped source context for this task contains common network fixtures only and does not include a standalone GameNetworkingSockets transport implementation to verify or update.

## Relationship To Steam

Standalone GameNetworkingSockets can still be useful as a Steam-independent UDP development transport because its socket API is close to Steam Networking Sockets. It does not replace the production Steam P2P/lobby/SDR path:

- GNS UDP can support local two-process transport testing without Steam identity constraints.
- Steam P2P/SDR still needs Steamworks SDK setup, lobby metadata, Steam identities, and two-session validation.
- Passing GNS UDP validation must not be used as proof that Steam lobby/SDR works.

## Expected Future GNS Contract

- Keep GNS behind an explicit build flag such as `LUMINUMBRA_ENABLE_GNS`.
- Keep default builds independent of GNS, protobuf, abseil, or other GNS runtime dependencies.
- Map reliable/unreliable frame sends to the same replication/lockstep transport contract used by other network paths.
- Record bounded host/join logs separately from Steam validation artifacts.

## Validation Required Before Completion

- GNS-enabled build configures and links in the selected toolchain.
- Host listens on UDP and accepts a joiner from a separate process.
- Host/joiner exchange reliable and unreliable frames.
- Existing replication payloads run for a bounded tick count.
- Runtime dependency requirements are documented for local and redistributable builds.

## Iteration 6 Note

This artifact intentionally avoids claiming a working GNS UDP transport from the current scoped source tree. It remains a future or external validation path adjacent to the Steam P2P/lobby/SDR work.
