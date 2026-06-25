# Handoff: every feature maps to a sound

**Status:** standing rule + audit · **Owner:** audio/game-feel · **Created:** 2026-06-25

## The rule

**Every player-perceivable feature, action, state change, and world event MUST have an
associated sound — or a deliberate, written decision that it is silent.** When you add or
change a feature, the PR is not done until you have answered: *"What does this sound like,
and is it wired?"* A silent action reads as broken; a world without reactive sound feels
dead. This is a game-feel gate, not a nice-to-have.

### Checklist for any new feature / PR
- [ ] Does the player **do** something here (a verb / button / key)? → it needs a one-shot.
- [ ] Does a **state** change the player can perceive (discovery, level-up, objective done,
      weather shift, day/night)? → it needs a sting or an ambience change.
- [ ] Does something **happen in the world** near the player (a creature moves/calls, water,
      wind, an impact)? → it needs a spatial (3D) sound.
- [ ] If it is intentionally silent, say so in the PR and (ideally) in a code comment.
- [ ] Add the event to a **loaded** bank (`data/audio/sfx_main.bank.json`) and wire the
      trigger in the owning system. Generate the sample via `tools/audio/` (below).

## Audio architecture (how it actually works here)

- **Engine:** `src/luminumbra_client/audio/MiniaudioManager.cpp` (miniaudio).
  - ⚠️ **FORMAT GOTCHA (cost a real bug):** this build has **no Vorbis decoder** — `.ogg`
    files fail to load with `ma_result -10` (`MA_INVALID_FILE`) and play **silently**. Use
    **`.mp3`** (or wav). The generation pipeline outputs mp3.
  - API (on `IAudioManager`, so call via the `audioManager` pointer):
    `PlayOneShot(id, pos)` (3D), `PlayOneShot2D(id)`, `PlayAmbientLoop(id, pos, radius)`
    (decoded-in-full, loops; use a huge radius for a constant bed), `StopAmbientLoop(id)`,
    `PlayMusic(id)`, `StopMusic()`, `SetListenerTransform(...)`.
  - The **3D listener follows the player** every frame (`SetListenerTransform` in the
    IN_GAME loop in `main_client.cpp`). Without it, positional audio is near-silent.
- **Banks:** `data/audio/*.bank.json` map an event id → file(s) + volume/pitch/strategy/3D.
  **Only `sfx_main.bank.json` and `music.bank.json` are loaded** (`main_client.cpp`
  `LoadBank`). The other banks (creatures/environment/weather/…) reference hundreds of
  not-yet-generated files and are **not loaded** — add new events to `sfx_main` (or load a
  new focused bank) rather than relying on those.
- **Generation pipeline (ElevenLabs):** `tools/audio/sfx_manifest.json` (the source of
  truth for prompts + output paths) + `tools/audio/generate_sfx.ps1`. Run with the key in
  the environment ONLY (never commit it):
  `\$env:ELEVENLABS_API_KEY = "<key>"; pwsh tools/audio/generate_sfx.ps1` (`-Force` to
  regenerate). It calls `/v1/sound-generation`, saves mp3, skips existing files.
  `duration_seconds` must be **≥ 0.5**.
- **Determinism:** all audio is render/client-only — it reads sim state, never mutates it,
  and is guarded off in scenario/gate runs, so `world_hash` and the gates are untouched.

## Wired today (commits on `feat/polyglot-audit-roadmap`)
Footsteps (grass/stone/**soil/sand/water/crystal** by surface material), **camera shutter**
(on capture), UI click + world-loaded chime, **menu music**, constant ambient bed (**forest
rustle + birdsong + wind**), **rain** (reactive to `WeatherSystem::PrecipitationAt`),
**thunder** (during heavy storms), **water** (when standing water is within ~14 m), and
**creature calls** (nearest live creature, ~every 11 s). **Farming verbs** (plant/water/
fertilize/harvest, at the aim point on success). **Terraform** (dig sample picked by the
material being cut — soil/stone/sand — and a place/thud on fill). **Discovery chime** (first-
time codex fill at capture). **Objective-complete chime** (edge-triggered on the goal count).

## The audit — features that SHOULD make sound but DON'T yet
These are the gaps to close (each is a wire-up + usually one generated sample):
1. **Codex open/close** (C), **species picker** (V) — no UI tick/page sound.
2. **Creature FOOTSTEPS** — creatures are brain-driven and move **silently**, even though a
   `creature_grovestrider_footstep` event exists. Wire it to creature locomotion.
3. **Per-species creature voices** — only grovestrider has a call; the other 9 species reuse
   nothing. Each species should get (or share a tagged pool of) calls.
4. **Wind gusts** — wind is a constant bed; it should swell with the wind-field strength
   (needs a runtime ambient-volume setter — not yet exposed).
5. **Menu/UI** — RmlUi button hover/click are not wired to `ui_button_click`/`ui_button_hover`.
6. **Plant promotion**, water terraform **drain/dam**, day/night transition, low-health /
    danger cues, etc.

## Process going forward
Treat this doc + `sfx_manifest.json` as the audio backlog. When you touch a system, wire its
sounds in the same PR. Generate missing samples through the pipeline (mp3!), add the event to
`sfx_main`, trigger it in the owning system, and verify the bank loads (`--frame-scan` logs
`Loaded sound bank: N events` and `Started ambient loop: …`, with no `failed to load`).
