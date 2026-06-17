# How to grab me the Steamworks SDK (owner action)

You only need this for **Layer 3** (the actual Steam transport: SDR relay, lobbies,
friend invites, auth). Real networking (TCP, done) and the GNS UDP transport
(in progress) need none of this.

## What I need from you
1. **A Steamworks partner account** (free to create; the $100 Steam Direct fee is
   only for *publishing* an app, not for downloading the SDK or dev testing).
   - Go to https://partner.steamgames.com/ and sign in / sign up.
2. **Download the Steamworks SDK**
   - https://partner.steamgames.com/downloads/list → "Steamworks SDK" (latest,
     e.g. `steamworks_sdk_161.zip`).
   - Unzip it. You'll get an `sdk/` folder containing:
     - `sdk/public/steam/` — the headers (`steam_api.h`, `isteamnetworkingsockets.h`, ...)
     - `sdk/redistributable_bin/win64/` — `steam_api64.lib` + `steam_api64.dll`
3. **Drop it in the repo at** `vendor/steamworks/` so the layout is:
   - `vendor/steamworks/public/steam/*.h`
   - `vendor/steamworks/redistributable_bin/win64/steam_api64.{lib,dll}`
   - (I'll add `vendor/steamworks/` to `.gitignore` — the SDK is **not
     redistributable**, so it must never be committed.)
4. **An App ID** — you do NOT need a paid app to test:
   - For development/testing, use **App ID 480** ("Spacewar", Valve's public test
     app). It works with the SDK for networking + SDR. I'll wire `steam_appid.txt`
     = `480` for dev runs.
   - A real shipping App ID needs the Steam Direct fee + a created app later; tell
     me the number when you have it and I'll switch dev→prod.
5. **Steam client installed + running** on this machine (the SDK talks to the
   running Steam client for auth/SDR). You're logged into Steam already, so that's
   covered.

## Tell me when done
Just say "steamworks sdk is in vendor/steamworks" (or wherever you put it) and I'll
wire `SteamNetworkingTransport : ILockstepTransport` + lobby create/join into the
existing seam. Until then I'm building the GNS UDP transport, which shares almost
all the same connection-handling code.

## Why this part is gated and the rest isn't
- TCP transport: open-source winsock — done, runs two processes over the wire now.
- GameNetworkingSockets (UDP): Valve open-source on GitHub — I can build it myself.
- Steamworks SDK: behind a partner **login**, **not redistributable** for me to
  fetch, and needs an **App ID** tied to your account. Only this layer needs you.
