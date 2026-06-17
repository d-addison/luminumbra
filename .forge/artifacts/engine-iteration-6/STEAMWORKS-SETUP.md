# Steamworks Setup

## Status

Deferred for Iteration 6. The scoped source tree for this task does not include Steamworks transport files, lobby code, or Steam SDK vendor files. This document records the expected owner setup and keeps the non-redistributable SDK out of the repository.

## Owner Setup

1. Sign in to a Steamworks partner account at `https://partner.steamgames.com/`.
2. Download the Steamworks SDK from the Steamworks downloads page.
3. Unzip the SDK outside source control.
4. If a future Steam-enabled branch expects local SDK files, place only the local working copy under `vendor/steamworks/` with this layout:
   - `vendor/steamworks/public/steam/*.h`
   - `vendor/steamworks/redistributable_bin/win64/steam_api64.lib`
   - `vendor/steamworks/redistributable_bin/win64/steam_api64.dll`
5. Keep `vendor/steamworks/` ignored. Steamworks SDK headers, libraries, and redistributables must not be committed.
6. Use App ID `480` only for local Spacewar-based development tests. A shipping App ID must come from the product's Steamworks app configuration.
7. Run the Steam client and sign in before starting a Steam-enabled build. Steam Networking Sockets and SDR depend on the client session for development authentication and relay access.

## Expected Build Contract

- Default builds must not require the Steamworks SDK.
- Steam code must be behind an explicit build option such as `LUMINUMBRA_ENABLE_STEAM`.
- `--steam` runtime options must report a clear disabled-feature error when the build option is off.
- Development builds may generate `steam_appid.txt` with `480`; production builds must not silently force that App ID.

## Validation Environment

Steam P2P lobby and SDR validation requires two Steam sessions. Use either two machines or two Steam accounts so the host and joiner are distinct Steam identities. A same-machine two-process run with one account is not acceptance evidence for Steam P2P/SDR.

## Handoff

When the SDK, App ID choice, and two-session validation environment are available, the implementation task can wire the Steam lobby lifecycle and SDR transport described in `.forge/specs/iter6/multiplayer-steam-p2p-lobby-and-sdr.md`.
