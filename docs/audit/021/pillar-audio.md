# Pillar audit: Audio — "everything maps to sound" (Spec 021, 2026-07-02)

The audio pillar is in good functional shape and the original "everything maps to sound" audit
genuinely closed: every current player verb, perceivable state change, and nearby world event is
wired through a clean `IAudioManager` seam (`src/luminumbra_client/audio/IAudioManager.h:14`),
the 3D one-shot use-after-free is fixed in the tree
(`src/luminumbra_client/audio/MiniaudioManager.cpp:257`), both loaded banks are ogg-free and all
67 referenced assets exist on disk (verified this audit run). The honest critique is threefold:
(1) **nothing enforces the standing rule** — the three audio gates are null-isolation /
handle-API / atmosphere-model checks, none validates bank asset integrity, the mp3-only
constraint, or event coverage; (2) a **large computed-but-inaudible layer** exists — biome/weather
reverb, wind parameters, thunder-distance cues and waterfall roar are implemented and even
gate-exercised, but `EnvironmentalAudioSystem` is never constructed in the game loop and
`SetGlobalReverb` is a log stub, so none of it reaches the player's ears; and (3) the **SFX
volume slider is a dead control** (persisted, never applied). No audio commits have landed since
the 2026-06-28 roadmap, so there is no drift to reconcile — only the pre-roadmap wave (creature
sleep/feed/drink/colony SFX, day/night music beds, live music bus) that the pillar doc itself has
not yet caught up to.

## Current state + evidence

### Architecture and seam

- **Interface seam**: `IAudioManager` (`src/luminumbra_client/audio/IAudioManager.h:14`) exposes
  bank load/unload, listener transform, `PlayEvent`/`PlayOneShot`/`PlayOneShot2D`, music
  (`PlayMusic`/`StopMusic`, `IAudioManager.h:32-33`), ambient loops with live volume scaling
  (`PlayAmbientLoop`/`StopAmbientLoop`/`SetAmbientVolume`, `IAudioManager.h:37-42`), and two
  buses: `SetMasterVolume` + `SetMusicVolume` (`IAudioManager.h:45-48`). There is **no SFX bus
  method** on the interface.
- **Concrete backend**: `MiniaudioManager`
  (`src/luminumbra_client/audio/MiniaudioManager.cpp:20-30` init;
  bank parsing at `:97-141` reading `is_2d`, looping, 3D attenuation, reverb fields). Sound file
  selection is uniform-random over the event's file list (`MiniaudioManager.cpp:161-162`); the
  bank keys `is_3d` and `strategy` present in `data/audio/sfx_main.bank.json:29-31` are **not read
  by the loader** (it keys off `is_2d`, default false → 3D; harmless but misleading authoring
  surface).
- **One-shot lifetime fix (the a970d89f UAF)** is present: 3D one-shots are parked in
  `m_oneShotSounds` with an explanatory comment (`MiniaudioManager.cpp:257-261`), reaped in
  `Update()` (`MiniaudioManager.cpp:60-67`) and uninit'd in `Shutdown()`
  (`MiniaudioManager.cpp:83-86`). KNOWN-CONTEXT fact verified against the tree.
- **Null path**: `--no-audio` selects `NullAudioManager`
  (`src/luminumbra_client/main_client.cpp:3025-3026`), which counts every call and writes the
  `luminumbra.audio.null_telemetry.v1` artifact
  (`src/luminumbra_client/audio/NullAudioManager.h:21-31`,
  `build/debug/test-artifacts/audio/audio-telemetry.json:67`, latest run `"passed": true` at
  `:59`). KNOWN-CONTEXT "--no-audio isolates audio crashes" verified.
- **Compiled audio sources** are exactly five (`src/luminumbra_client/sources.cmake:10-14`):
  AudioManagerFactory, AudioPropagationSystem, AudioSpatialCluster, EnvironmentalAudioSystem,
  MiniaudioManager. Five further headers in the audio directory
  (`AdvancedReverbSystem.h`, `ProceduralSoundGenerator.h`, `SoundVariationSystem.h`,
  `AudioStreamingManager.h`, `AudioPerformanceProfiler.h`) are referenced by **no other file**
  (grep across `src/` finds only self-references) — dead aspirational code.

### Banks, assets, generation pipeline

- Only two banks load (`main_client.cpp:3033-3034`): `data/audio/sfx_main.bank.json` and
  `data/audio/music.bank.json` — matching the pillar doc
  (`docs/AUDIO-everything-maps-to-sound.md:35-39`). Both are 100% mp3; a bank-wide grep for
  `\.ogg` matches **zero** entries in the loaded banks and **226 entries across the 7 unloaded
  banks** (`data/audio/weather_sfx.bank.json`, `creature_sfx.bank.json`, etc.).
- All 67 files referenced by the two loaded banks **exist on disk** (verified by script during
  this audit; note this is a manual check — no gate performs it, see Gaps).
- The mp3-only decoder constraint is documented (`docs/AUDIO-everything-maps-to-sound.md:26-28`)
  and the generation pipeline reads the ElevenLabs key from the environment only
  (`tools/audio/generate_sfx.ps1:12-13`); `tools/audio/sfx_manifest.json` contains no `.ogg`/`.wav`
  output targets (grep: zero matches). However the pipeline still *supports* transcoding to
  `.ogg` targets via ffmpeg (`generate_sfx.ps1:48-53`), and one `.ogg` path survives in engine
  code: `MiniaudioManager::SetWindParameters` hardcodes
  `assets/audio/sfx/weather/wind_loop.ogg` (`MiniaudioManager.cpp:433`) — currently unreachable
  in-game (see below) but a silent-failure landmine if ever wired.

### In-game wiring (the standing rule, as shipped)

Verified live wiring in `src/luminumbra_client/main_client.cpp` (all render/client-side,
scenario-guarded, determinism-neutral):

- Listener follows the player every IN_GAME frame (`main_client.cpp:4083-4085`).
- Material-keyed player footsteps: stone/deepslate/soil/sand/water/crystal/grass, stride-cadenced
  (`main_client.cpp:4108-4119`).
- Weather: rain bed with hysteresis tied to `PrecipitationAt` (`main_client.cpp:4139-4146`);
  thunder during heavy storms on a **flat 22-second timer** (`main_client.cpp:4149-4152`).
- Standing-water proximity stream bed (`main_client.cpp:4166`); wind bed swell from the live wind
  field via `SetAmbientVolume` (`main_client.cpp:4175`, impl `MiniaudioManager.cpp:513-520`).
- Per-species creature voice — `creature_<id>_call` picked by species id
  (`main_client.cpp:4200-4206`), all 10 species present in the bank
  (`data/audio/sfx_main.bank.json:157-165`).
- Creature per-action SFX: sleep breathing (`main_client.cpp:4230`), feed/drink
  (`main_client.cpp:4257`), colony chitter (`main_client.cpp:4283`), grounded footsteps/wingbeats
  by gait (`main_client.cpp:4323`).
- Day/night: `time_dawn`/`time_dusk` stings plus day/dusk **music bed swap**
  (`main_client.cpp:4337-4338`; beds in `data/audio/music.bank.json:11-22`).
- World-loaded chime + constant ambient beds (forest/birds/wind) started at world entry
  (`main_client.cpp:4610-4619`).
- Verbs: farming plant/water/fertilize/harvest (`main_client.cpp:6975-7005`), terraform
  place/dig-by-material + water-rush coupling (`main_client.cpp:7043-7067`), codex open/close
  (`main_client.cpp:7091`), objective-complete edge trigger (`main_client.cpp:7165`), camera
  shutter (`main_client.cpp:7358`), first-time discovery chime (`main_client.cpp:7430`), UI
  clicks (`main_client.cpp:6947`; RmlUi controls route via `src/luminumbra_client/ui/Rml_UIManager.cpp`).
- Settings: master and music volumes are applied **live**
  (`main_client.cpp:3508-3511`, `:3515-3518`, `:9466-9469`) and persisted via SystemConfig
  (`src/luminumbra_common/core/SystemConfig.h:42`). The **SFX slider is persisted but never
  applied** (`main_client.cpp:3512-3513` — setter writes config only; no audio-manager call).
  This *partially contradicts* the prior-session memory "sfx-music persisted-only": music now
  applies live (commit `606cdcb6`); SFX still does not. Recorded as a discrepancy.

### Gates and tests (what is actually enforced)

- `validate-engine-frontier.ps1` registers three audio modes
  (`tools/gates/validate-engine-frontier.ps1:7248-7250`):
  - **AudioNullTelemetry** (`:1255`; script `test/audio/audio-null-telemetry.ps1:34-55`) — static
    regex checks that `--no-audio` selects the null manager, playback routes through
    `IAudioManager`, and the telemetry schema exists.
  - **AudioHandleApplication** (`:1315`; script `test/audio/audio-handle-application.ps1:78-135`)
    — static regex checks over handle stop/position/volume/parameter application and sound
    lifetime (erase/uninit).
  - **AtmosphereAudio** (`:1399`) — drives the *real*
    `EnvironmentalAudioSystem::ComputeAtmosphere` + `AudioPropagationSystem` ambience beds
    headlessly via `RuntimeScenarioHarness`
    (`src/luminumbra_client/core/RuntimeScenarioHarness.cpp:1429-1479`, artifact written at
    `:1568`).
- **BiomeReverbTest** (ctest via `gtest_discover_tests`; source
  `test/common/BiomeReverb_test.cpp:26-73`, registered in `test/CMakeLists.txt:109`) proves the
  per-biome reverb *data* flow (biomes.json → BiomeTable → `reverb_for`).
- **None of these enforces the standing rule**: no gate checks that bank-referenced assets exist,
  that loaded banks are ogg-free, or that event ids referenced by client code resolve in a loaded
  bank. An unknown event id only logs at runtime (`MiniaudioManager.cpp:399`);
  a missing/undecodable file makes `PlayOneShot` return false silently
  (`MiniaudioManager.cpp:235-237`).

### The computed-but-inaudible layer

- `EnvironmentalAudioSystem` (biome reverb application `EnvironmentalAudioSystem.cpp:121-142`,
  pinned atmosphere model `:144-179`, backend push `:215-219`) is **never instantiated in the game
  loop** — its only constructions are inside the gate harness with a null manager
  (`RuntimeScenarioHarness.cpp:1461,1463`). Grep of `main_client.cpp` finds no reference.
- Even if wired, reverb dead-ends: `MiniaudioManager::SetGlobalReverb` is a log-only stub
  (`MiniaudioManager.cpp:465-469`), per-event reverb in `ApplyEnvironmentalEffects` is log-only
  (`MiniaudioManager.cpp:547-551`), and the `ma_delay` echo initialized by `SetEnvironment`
  (`MiniaudioManager.cpp:411-421`, member `MiniaudioManager.h:183-184`) is never attached to any
  sound chain.
- `AudioPropagationSystem::ComputeThunderCue` (`AudioPropagationSystem.cpp:121`) and
  `ComputeWaterfallRoar` (`AudioPropagationSystem.cpp:159`, decl `AudioPropagationSystem.h:92-98`)
  have **zero call sites** outside their own file/harness — in-game thunder uses the flat timer
  above, and waterfalls (which *are* rendered and dressed: `main_client.cpp:3224`
  `prepare_waterfalls`) make **no sound at all**, a standing-rule violation for a marquee world
  feature.
- Occlusion is a distance-only stub (`MiniaudioManager.cpp:571-583`);
  `MiniaudioManager::SetPhysicsSystem` (`MiniaudioManager.cpp:585-590`) is called from nowhere in
  `main_client.cpp` (grep: no matches), so the spatial cluster's physics hook is dormant.
  `UpdateWindEffect` applies a clamped random pitch walk to every active sound each Update
  (`MiniaudioManager.cpp:554-568`) — currently inert (nothing sets `m_windStrength` in-game) but
  a mix hazard the day wind parameters are wired.

### Shipped since the 2026-06-28 roadmap

**Nothing audio-touching has landed since 2026-06-28.** `git log --since=2026-06-25` over
`src/luminumbra_client/audio`, `data/audio`, `tools/audio`, `test/audio` shows the newest audio
commit is `0563bb42` (2026-06-26, per-action creature SFX — feed/drink/colony, spec 011 Phase G),
preceded by `71364abc` (sleep breathing), `606cdcb6` (live music-volume bus), `584531d7` (music
volume tuning) and `6d00f610` (day/dusk in-game music beds), all 2026-06-25. The roadmap-era
landings (017-A readback ring `ca2616d8`/`3bba2a52`, moon radiance channel `3aa9740d`, lake-preview
crash fix `e1fee9ff`) do not touch audio. Two audit-relevant consequences:

- The pillar doc's "Wired today" list (`docs/AUDIO-everything-maps-to-sound.md:49-78`) predates
  the sleep/feed/drink/colony SFX and the in-game music beds/live music bus — the doc is stale by
  three commits (filed as AUDIO-13).
- The moon/night visual work (`3aa9740d`) landed with **no matching night soundscape change**:
  the birdsong bed started at world load (`main_client.cpp:4617-4619`) plays 24/7 with no
  time-of-day gating (only the wind bed has a live volume driver, `main_client.cpp:4175`), so
  night currently sounds like day (filed as AUDIO-07; the dawn/dusk detector at
  `main_client.cpp:4330-4338` is the ready-made hook).

## Gaps / debt

1. **No enforcement of the standing rule** — the game-feel gate exists only as prose
   (`docs/AUDIO-everything-maps-to-sound.md:6-11`). No ctest/gate validates bank asset existence,
   the mp3/wav allowlist, or code↔bank event coverage; failures are silent at runtime
   (`MiniaudioManager.cpp:235-237`, `:399`). The 67-asset existence check I ran this audit is
   manual and unrepeatable. (AUDIO-04)
2. **SFX slider is a dead control** — `setting_audio_sfx` UI (`Rml_UIManager.cpp:1025`) persists
   to `SystemConfig` but is never applied; `IAudioManager` has no SFX bus
   (`main_client.cpp:3512-3513`, `IAudioManager.h:44-48`). (AUDIO-05)
3. **Waterfalls are silent** — rendered/dressed (`main_client.cpp:3224`) with a purpose-built,
   never-called roar model (`AudioPropagationSystem.cpp:159`). (AUDIO-06)
4. **Night sounds like day** — 24/7 birdsong, no night bed (`main_client.cpp:4617-4619`). (AUDIO-07)
5. **Thunder ignores physics** — flat 22 s cadence (`main_client.cpp:4149-4152`) instead of the
   existing distance-delay cue model (`AudioPropagationSystem.cpp:121`). (AUDIO-08)
6. **Reverb/atmosphere layer inaudible end-to-end** — `EnvironmentalAudioSystem` unconstructed in
   game; `SetGlobalReverb` stub; orphaned `ma_delay`; `.ogg` landmine at
   `MiniaudioManager.cpp:433`. (AUDIO-09)
7. **No mix architecture** — no ducking (stings vs beds), no category buses beyond master/music.
   (AUDIO-10)
8. **Occlusion/physics hook dormant** — `SetPhysicsSystem` uncalled; distance-only stub
   (`MiniaudioManager.cpp:571-590`). (AUDIO-11)
9. **Dead weight** — 7 unloaded banks referencing 226 `.ogg` files; 5 orphan header-only audio
   systems (`sources.cmake:10-14` compiles only five .cpp files). (AUDIO-12)
10. **Doc drift** — pillar doc missing the last three wiring commits; prior-session memory on
    volume buses half-wrong (music now live). (AUDIO-13)

## Risks

- **Silent regression risk (highest)**: because playback failure is silent by design (returns
  false, logs at most), an asset deleted/renamed, an ogg re-introduced by the pipeline's
  still-present transcode path (`generate_sfx.ps1:48-53`), or a typo'd event id would ship
  inaudibly with **no gate to catch it**. The static-regex gates
  (`test/audio/audio-null-telemetry.ps1:34-55`) verify code *shape*, not audible behavior, and
  are brittle against refactors (they regex `main_client.cpp` text).
- **Wiring the dormant layer is a mix-quality risk**: `UpdateWindEffect`'s per-frame random pitch
  walk over all active sounds (`MiniaudioManager.cpp:554-568`) and the un-attached echo delay
  (`MiniaudioManager.cpp:411-421`) will produce artifacts the moment `SetWindParameters` gets a
  real caller — AUDIO-09 must clean these paths, not just construct the system.
- **Determinism exposure is low and should stay that way**: all audio is render/client-only and
  scenario-guarded (`main_client.cpp:4088-4091`; `docs/AUDIO-everything-maps-to-sound.md:46-47`);
  the null-audio gates pin that isolation. Any future audio-driven *gameplay* coupling (e.g.
  creatures reacting to player noise) must go through the deterministic sim, not the client mixer.
- **Headless verification blocker (cross-pillar)**: live audible verification of IN_GAME wiring
  rides on the headless IN_GAME capture path, which currently hangs (the `--scene-config` /
  `--frame-scan` game-render capture path stalls on the first IN_GAME frame after world-load —
  see the RENDER pillar audit finding; root cause not yet localized) — audio coverage gates
  should therefore be built bank/static/model-level (as AUDIO-04 is), not gameplay-capture
  level, until RENDER clears that blocker.

## Opportunities

- **The propagation layer is already written and gate-tested** — wiring `ComputeWaterfallRoar` /
  `ComputeThunderCue` / the ambience beds into the IN_GAME loop is mostly plumbing
  (`AudioPropagationSystem.cpp:121,159`; harness proof at `RuntimeScenarioHarness.cpp:1474-1479`),
  a cheap and audible fidelity win consistent with the BF4-floor visual mandate.
- **Biome reverb data is authored and tested** (`test/common/BiomeReverb_test.cpp:26-73`) — a
  single real DSP send (miniaudio delay/LPF chain) turns an existing dataset into per-biome
  acoustic identity.
- **The dawn/dusk detector** (`main_client.cpp:4330-4338`) is a ready seam for a full diurnal
  ambience state machine (night bed, dawn chorus) — pairs naturally with the landed moon radiance
  channel (`3aa9740d`) and upcoming 015 Pillar-A night polish.
- **Photography loop (iter 7)** already has shutter + discovery + codex sounds
  (`main_client.cpp:7358,7430,7091`); a small lens/focus sound set completes the core loop's
  audio identity before that pillar's spec even starts.
- **ElevenLabs pipeline is turnkey** (`tools/audio/generate_sfx.ps1`) — every item above that
  needs new samples (night bed, waterfall roar, lens sounds) is a manifest entry + one script run.

## Backlog items

| id | summary | spec | effort | risk | deps | status | proving_signal |
|----|---------|------|--------|------|------|--------|----------------|
| AUDIO-01 | 3D one-shot use-after-free fixed: one-shots parked in m_oneShotSounds, reaped in Update, uninit in Shutdown | new | S | low | — | done | validate-engine-frontier.ps1 -Mode AudioHandleApplication |
| AUDIO-02 | Post-audit coverage wave shipped: per-species voices, creature footsteps/wingbeats, sleep/feed/drink/colony SFX, wind swell, dawn/dusk cues + day/night music beds + live music bus | 011 | L | low | — | done | validate-engine-frontier.ps1 -Mode AudioNullTelemetry |
| AUDIO-03 | Audio gate trio landed: AudioNullTelemetry, AudioHandleApplication, AtmosphereAudio (+ runtime null-telemetry artifact schema v1) | new | M | low | — | done | validate-engine-frontier.ps1 -Mode AtmosphereAudio |
| AUDIO-04 | Author the bank-integrity + ogg-guard gate: every loaded-bank file exists, extension allowlist {mp3,wav}, every event-id literal in client code resolves in a loaded bank | new | S | low | — | todo | NEW: AudioBankIntegrity ctest — asserts (a) all files referenced by sfx_main/music banks exist, (b) zero .ogg in loaded banks, (c) every PlayOneShot/PlayAmbientLoop/PlayMusic event literal in src/luminumbra_client resolves to a bank event |
| AUDIO-05 | Implement the SFX bus: add IAudioManager::SetSfxVolume, scale non-music playback, apply the persisted-but-dead setting_audio_sfx slider live | new | S | low | — | todo | NEW: sfx-bus check appended to test/audio/audio-handle-application.ps1 — asserts IAudioManager declares SetSfxVolume and MiniaudioManager scales one-shot/ambient gain by it |
| AUDIO-06 | Wire waterfall roar: feed render waterfall sites into ComputeWaterfallRoar, add a waterfall_roar bank event + ambient loop at the nearest site | new | M | low | AUDIO-04 | todo | NEW: WaterfallRoarWiring ctest — a waterfall crest site yields roar volume > 0 within range and 0 beyond max range, and the IN_GAME loop triggers the bed from waterfall_sites |
| AUDIO-07 | Night soundscape: gate the birdsong bed by sun elevation and add a night ambience bed (crickets/owls) off the existing dawn/dusk detector | new | M | low | AUDIO-04 | todo | NEW: night-audio assertion in validate-engine-frontier.ps1 -Mode AtmosphereAudio — bird-bed volume scales to ~0 when sun elevation < 0 and the night bed replaces it |
| AUDIO-08 | Replace the flat 22 s thunder timer with ComputeThunderCue distance-delayed strikes tied to storm cells | new | S | low | — | todo | NEW: ThunderCue wiring assertion in -Mode AtmosphereAudio — thunder gain/delay follows strike distance (speed-of-sound delay), not a fixed cadence |
| AUDIO-09 | Wire EnvironmentalAudioSystem into the IN_GAME loop with a real reverb/filter DSP (replace the SetGlobalReverb log stub, attach or delete the orphan ma_delay, fix the wind_loop.ogg landmine, tame UpdateWindEffect pitch jitter) | new | L | medium | AUDIO-04 | todo | NEW: AtmosphereAudio in-game wiring assertion — the IN_GAME loop constructs EnvironmentalAudioSystem and biome/weather reverb reaches a live miniaudio node, not a log stub |
| AUDIO-10 | Mix architecture: category buses + ducking (stings duck beds, thunder ducks ambience, music ducks under discovery/objective) | new | M | low | AUDIO-05 | todo | NEW: MixerBus ctest — pure bus-state unit test asserting a sting playback ducks bed gain and releases on completion |
| AUDIO-11 | Real audio occlusion: call SetPhysicsSystem from the client bootstrap and replace the distance-only stub with physics raycasts in AudioSpatialCluster | new | M | medium | — | todo | NEW: AudioOcclusion ctest — a source behind solid terrain (physics raycast blocked) computes lower gain than an equal-distance line-of-sight source |
| AUDIO-12 | Prune the dead weight: 7 unloaded banks referencing 226 .ogg files, and the 5 orphan header-only audio systems (AdvancedReverb/ProceduralSound/SoundVariation/AudioStreaming/AudioPerformanceProfiler) | new | S | low | AUDIO-04 | todo | NEW: all-banks extension of the AudioBankIntegrity ctest — every bank left under data/audio is loaded-and-valid or explicitly retired; orphan headers removed with the build/ctest suite green |
| AUDIO-13 | Refresh docs/AUDIO-everything-maps-to-sound.md (sleep/feed/drink/colony, music beds, live music bus) and record the volume-bus memory correction | new | S | low | AUDIO-04 | todo | NEW: doc-sync assertion in the AudioBankIntegrity ctest — every event id named in the pillar doc exists in a loaded bank |
| AUDIO-14 | Photography-loop sound set (lens zoom/focus tick, focus-lock confirm, photo-review UI) ahead of the iter-7 photography pillar; shutter/discovery/codex already wired | new | S | low | — | todo | NEW: photography-verbs coverage assertion in the AudioBankIntegrity ctest — every photography verb literal in the capture path triggers a wired bank event |
