# Pillar audit: Atmospheric — weather, seasons, wind, time-of-day, sky/atmosphere (Spec 021, 2026-07-02)

**Verdict.** The atmospheric pillar has a genuinely strong, deterministic sim substrate — a
bit-deterministic weather core (category field + advected storm cells + lightning schedule, all in
`world_hash`), a 3-layer wind grid, a tested-but-unwired world-event scheduler, and a Hillaire-2020
scattering LUT whose *hue* authority has now been partially extended to *magnitude* by the spec-015
Pillar A work that landed 2026-06-27/28 (ambient-magnitude coupling, lunar phase, deterministic TOD
exposure, dedicated moon radiance channel). The dominant defect is not missing systems but a missing
**live-play bridge**: in normal IN_GAME play the sim weather is *audible but invisible* (rain/thunder
audio reads `PrecipitationAt`, but the render overlay, cloud coverage, lightning bolts, and the
season clock are only driven by test scenarios, the menu backdrop, and photo/scene-config debug
paths), and time-of-day is a wall-clock 60-second render loop disconnected from the sim tick that
drives the season and lunar models. Remaining Pillar-A work (moon re-bless, photo manual EV, A-T06
GPU metering) is small and correctly sequenced behind the RENDER-owned IN_GAME capture-hang fix and
the OPS-owned 018-E/F gates.

## Current state + evidence

### Sim-authoritative weather core (T-I5a-3 B1) — landed, deterministic, hashed
- `src/luminumbra_common/systems/WeatherSystem.h:50-56` — five weather categories
  {Clear, Overcast, Rain, Snow, Fog}; enum order is the canonical hash order.
- `src/luminumbra_common/systems/WeatherSystem.h:64-71` — pinned geometry: 24 m cells x 64-cell
  extent (1536 m), <=16 storm cells; `:78` <=32 live lightning strikes.
- `src/luminumbra_common/systems/WeatherSystem.h:140` — `Update(tick, region_anchor, wind)` is a
  pure function of (seed, tick, anchor); budget <=0.20 ms/tick (WeatherVisual gate).
- Query API: `CategoryAt` / `PrecipitationAt` / `SampleAt`
  (`src/luminumbra_common/systems/WeatherSystem.h:147-153`; `PrecipitationAt` implemented at
  `src/luminumbra_common/systems/WeatherSystem.cpp:501`); lightning `StrikeSchedule()`
  (`WeatherSystem.h:167`) and `StrikesThisTick()` (`WeatherSystem.cpp:426`).
- Determinism surface: `ComputeWeatherSubHash` (`WeatherSystem.cpp:523`) feeds the `weather`
  world_hash slot via `src/luminumbra_server/ServerWorldRunner.cpp:58`; the wind sub-hash likewise
  at `:47`. Tick order: wind updates at slot 3, weather at slot 4 after wind
  (`src/luminumbra_common/world/GameSession.cpp:355`, `:365`).
- Tests/gates: `test/common/WeatherSystem_test.cpp:1-10` (two-instance sub-hash identity, bounded
  storms, 300-tick endurance horizon), `test/CMakeLists.txt:136-137` registers it in `common_tests`
  (gtest-discovered per `test/CMakeLists.txt:952`); engine-frontier modes `WeatherVisual`
  (`tools/gates/validate-engine-frontier.ps1:3271`), `Precipitation` (`:3727`), `CloudShadow`
  (`:3474`), `WindFieldDeterminism` (`:5014`), `SkyboxVisual` (`:3168`), `TimeOfDaySweep` (`:3841`),
  `AtmosphereAudio` (`:7250`).

### Wind field (T-I5a-2 A2) — landed, consumed by sim + render + audio
- `src/luminumbra_common/systems/WindFieldSystem.h:1-22` — deterministic 2.5D grid, 24 m cells,
  3 AGL layers, seed+11, hashed into world_hash; `SampleWind` at `:79`.
- Sim consumers: storm-cell advection (`src/luminumbra_common/systems/WeatherSystem.cpp:282`,
  `:335`), scent advection (`src/luminumbra_common/world/GameSession.cpp:228`), pollination + fire
  spread downwind (`GameSession.cpp:501-506`, `:515`), aether emission advection
  (`src/luminumbra_common/systems/AetherFieldSystem.cpp:141`).
- Render/audio consumers in live play: grass sway samples the live wind field every frame
  (`src/luminumbra_client/main_client.cpp:6638-6644` -> `foliage->set_wind`), wind-gust audio swell
  (`main_client.cpp:4171-4176`, `SetAmbientVolume("ambient_wind", ...)`).

### Hillaire atmosphere + spec-015 Pillar A: hue authority, now partially magnitude-coupled
- LUTs: `src/luminumbra_client/rendering/SkyAtmosphereLut.h:105` `sun_transmittance()`, `:109`
  `sky_ambient()`; **A-T01 magnitude getters landed** — `sun_irradiance_rgb` / `sky_unit_irradiance_rgb`
  with an explicit unit contract (`SkyAtmosphereLut.h:111-125`).
- **A-T03 / FR-A-002 landed:** daytime ambient *magnitude* is now LUT-coupled —
  `dayAmbient = m_sky_lut.sky_ambient() * kSkyAmbientRenderScale(8.49)` calibrated to preserve noon
  (`src/luminumbra_client/rendering/RenderPipeline.cpp:5048-5058`); ambient *hue* coupling raised
  from <=50% to 0.9 x sun intensity (`RenderPipeline.cpp:5083-5089`).
- **A-T04 landed:** lunar phase "two night modes" — deterministic lunar cycle from the tick
  (`RenderPipeline.cpp:5020-5040`, `kTicksPerLunarCycle=54000` at
  `src/luminumbra_client/rendering/RenderPipeline.h:926`), night skylight scales with phase above a
  starlight floor (`RenderPipeline.cpp:5066-5071`), `LUMIN_MOON` / `set_moon_illumination` override
  (`RenderPipeline.h:535-539`), scene-config `"moon"` knob (`main_client.cpp:2718`, `:6595`).
- **A-T05/A-T05b landed:** deterministic TOD exposure (eye adaptation) as a pure function of sun
  elevation (`RenderPipeline.cpp:5113-5133`) feeding the exposure seam
  (`src/luminumbra_client/rendering/RenderContext.h:150-153`;
  `src/luminumbra_client/rendering/passes/LightingPass.cpp:194-196` — sentinel-0 falls back to
  `LUMIN_GRADE`).
- **Moon radiance channel landed (Codex C5, commit 3aa9740d):** `RenderContext.h:112` ->
  `LightingPass.cpp:176` -> `res/shaders/lighting_pass.frag:80` (`u_moonRadiance`), default equals
  the prior shader const so night is byte-identical until re-calibrated
  (`RenderPipeline.h:921-925`).
- **Still authored (Pillar A remainder):** `m_sun.intensity` is the original smoothstep
  (`RenderPipeline.cpp:4921`); sun transmittance is *normalized* against the overhead reference so
  only hue moves (`RenderPipeline.cpp:4959-4970`); `nightAmbient` is an authored constant
  (`RenderPipeline.cpp:5070`); the moon key keeps hand constants `kMoonKeyScale=1.5`
  (`res/shaders/lighting_pass.frag:474`) and the wrap floor `NdotL*0.6+0.25` (`:483`); the aerial
  fog still vanishes at night via `u_skyDayFactor` (`res/shaders/volumetric_lighting.frag:131`).
- **KNOWN-CONTEXT discrepancies (tree wins):** (1) the memory claim "the atmosphere drives HUE but
  NOT brightness; intensity is authored ramps" is now *partially stale* — ambient magnitude is
  LUT-coupled (A-T03) and a deterministic exposure stage exists (A-T05); the sun smoothstep and
  night constants remain. (2) Spec 015's citation `kMoonKeyScale=1.3` /
  `lighting_pass.frag:462` (`docs/specs/015-atmospheric-lighting-colored-glass/spec.md:151-153`)
  is stale: the constant is now `1.5` at `lighting_pass.frag:474`. (3) `res/shaders/skybox.frag` is
  confirmed DEAD: the only skybox shader loaded is `enhanced_skybox.frag`
  (`src/luminumbra_client/rendering/passes/SkyboxPass.cpp:23`; no source references
  `shaders/skybox.frag`), yet the dead file still exists in `res/shaders/`.

### Season + celestial model (T-I5a-7 C2) — render-derived, gate-only
- Season phase is a pure function of an externally-fed tick (`RenderPipeline.cpp:4879-4915`;
  `kTicksPerSeasonCycle=432000` = 4 h at 30 Hz, `RenderPipeline.h:552`), driving a real ~23.5-deg
  seasonal sun declination (`RenderPipeline.cpp:4903-4915`) and a luminance-preserving seasonal
  palette tint on sun + ambient (`RenderPipeline.cpp:4984-4999`, `:5092-5111`).
- The `TimeOfDaySweep` gate asserts noon/dusk/night under BOTH seasons (6 phases,
  `tools/gates/validate-engine-frontier.ps1:3872-3881`).
- **But live play never feeds it:** `set_season_tick` is called only from the sweep scenario and the
  scenario harness (`main_client.cpp:6550`, `:6567`;
  `src/luminumbra_client/core/RuntimeScenarioHarness.cpp:8823`) — a live session stays frozen at
  season-neutral tick 0 (`RenderPipeline.cpp:4891-4894`). The separate procgen-foliage autumn
  palette `g_season` (`main_client.cpp:364`) is only advanced by timelapse capture
  (`main_client.cpp:9642`) and is not coupled to `get_season_phase()`.

### Time-of-day + timescale
- TOD advances by render-frame wall-clock delta over a hardcoded 60-second day
  (`RenderPipeline.cpp:4875`; `m_dayDurationSeconds = 60.0f` at `RenderPipeline.h:852` — no setter
  exists anywhere in `src/`). It is independent of the sim tick, of `g_timeScale`
  (`main_client.cpp:191`, keys at `:10023-10032`, F8 slider at `:9421`), and of the tick-pure
  season/lunar models — three different time authorities coexist.
- Photo mode holds/scrubs TOD (`main_client.cpp:7284-7295`, spec 013 FR-0.1) and cycles weather
  presets via the debug `set_weather` path (`main_client.cpp:7296-7305`).

### Weather rendering machinery — built, scenario-driven only
- Sim-driven overlay contract: `RenderPipeline::set_weather_state`
  (`RenderPipeline.cpp:5163-5197`) maps replicated precip/storm/fog to a weather type + uniforms;
  SkyboxPass consumes it (driven vs legacy-debug fallback,
  `src/luminumbra_client/rendering/passes/SkyboxPass.cpp:265-309`) via `weather_system.frag`
  (loaded at `SkyboxPass.cpp:30`); lightning overlay via `lightning_overlay.frag`
  (`src/luminumbra_client/rendering/passes/LightingPass.cpp:295`). Clouds: wind-advected coverage +
  cast shadow, scroll advanced deterministically (`RenderPipeline.cpp:5199-5235`); half-res cloud
  quality knob `LUMIN_CLOUD_QUALITY` (`main_client.cpp:3070-3073`).
- **Every `set_weather_state` caller is a scenario/gate/debug path**: WeatherVisual scenario
  (`main_client.cpp:4765`, `:4768`), CloudShadow scenario (`:4888`), Precipitation scenario
  (`:5089`), scenario harness (`RuntimeScenarioHarness.cpp:8815`); `set_cloud_state` callers are the
  menu backdrop (`main_client.cpp:3719`), scenarios (`:4899`, `:5170`), scene-config (`:6608`), and
  the worldgen preview (`src/luminumbra_client/world/WorldgenPreview.cpp:461`, UI-owned).
  `set_lightning_state` is scenario-only (`main_client.cpp:4867`, `:5280`;
  `RuntimeScenarioHarness.cpp:8817`).
- **Live-play consumers of sim weather are audio-only**: rain loop + thunder keyed on
  `PrecipitationAt` with hysteresis (`main_client.cpp:4141-4155`) and the wind-gust swell
  (`:4171-4176`). Live thunder is a random 22 s timer above a precip threshold — NOT the sim
  lightning schedule; `StrikesThisTick` is consumed only by server telemetry
  (`src/luminumbra_server/main_server.cpp:931`).

### Downstream sim consumers of weather — real and healthy
- Plant growth moisture is precip-driven (`src/luminumbra_common/world/GameSession.cpp:417-418`),
  AI stimulus channel reads precip (`src/luminumbra_common/ai/StimulusChannels.cpp:70`), and the
  adversarial environment-hardening suite pins wind-coupling signs + schedule validity
  (`test/sim/environment_hardening_test.cpp:1-9`).

### WeatherEventSystem — built + tested, ZERO consumers
- `src/luminumbra_common/systems/WeatherEventSystem.h:3-10` — a deterministic world-level
  {Clear, Storm, Drought, Snow} Markov window scheduler (seed offset +25, `:49`; pure entry point
  `WeatherEventAt` `:184`) intended to feed "moisture / fire-dryness / snow cover". Grep shows its
  only users are the header itself and `test/sim/weather_event_test.cpp:1-6`
  (`test/CMakeLists.txt:154`). Nothing in `GameSession` queries it.

### Spec 010 hydrology rain — API exists, weather not wired
- `WeatherSystem::PrecipitationAt` exists (`WeatherSystem.cpp:501`) but spec-010 rain is a *global
  uniform* `rain_mm_per_tick` (`src/luminumbra_common/systems/WaterSystem.h:105-106`;
  `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:3301-3302`) set only from timelapse/test
  paths (`main_client.cpp:5734`, `:9625`). Rain does not fall where the weather says it rains.
  (WATER owns the hydrology sink side; the weather-side wiring item is filed here.)

### Shipped since the 2026-06-28 roadmap
Verified via `git log`: after the roadmap approval late on 2026-06-28, only three non-docs commits
landed — 3aa9740d (06-28 18:15, moon radiance channel) and ca2616d8/3bba2a52 (06-29, 017-A readback
ring); the Pillar-A block (d22c0c81, bd8d519d, 6c8cf44b, 66ac5459) landed 2026-06-27 23:09 –
06-28 08:11 around approval. The many other 06-28 commits (worldgen-preview UAF fixes, crash
diagnostics, 016 FR-D reflection, 018/020 gates, create-world UI polish) belong to other pillars:
- **3aa9740d** (2026-06-28 18:15) — spec-015 Pillar A dedicated moon radiance channel (Codex C5):
  `RenderContext.moon_radiance` -> `u_moonRadiance`, byte-identical default. **Done** (ATMO-02).
- **66ac5459 / 6c8cf44b / bd8d519d / d22c0c81** (2026-06-27 23:09 - 06-28 08:11) — Pillar A
  A-T04 lunar phase, A-T03 ambient magnitude coupling, A-T05 TOD exposure + hue coupling,
  A-T01 LUT magnitude getters + A-T05b exposure seam. **Done** (ATMO-01).
- **ca2616d8 / 3bba2a52** (2026-06-29) — 017-A async readback ring + render readback ban gate
  (GPU-pillar-owned; unblocks this pillar's A-T06 GPU auto-exposure metering).

## Gaps / debt

1. **No live-play sim->render weather bridge (the pillar's headline gap).** The deterministic
   weather core is invisible in normal play: overlay, wetness, cloud coverage, and precipitation
   particles are driven only by scenarios/menu/photo/scene-config
   (`main_client.cpp:4765/4888/5089/3719/6599/7303`); live consumers are audio-only
   (`main_client.cpp:4141-4155`). A player standing in a sim rainstorm sees a clear sky.
2. **Sim lightning schedule unconsumed by the client.** `StrikesThisTick` feeds only server
   telemetry (`main_server.cpp:931`); live thunder is a random timer (`main_client.cpp:4148-4155`);
   bolts never render outside the WeatherVisual scenario (`main_client.cpp:4867`).
3. **Season frozen in live play.** `set_season_tick` never receives the sim tick outside sweep
   scenarios (`main_client.cpp:6550/6567`); the foliage autumn palette (`g_season`,
   `main_client.cpp:364`, `:9642`) is disconnected from the season phase.
4. **Fragmented time authority.** Wall-clock 60 s day (`RenderPipeline.cpp:4875`,
   `RenderPipeline.h:852`, no setter) vs tick-pure season/lunar models vs `g_timeScale` sim pacing
   (`main_client.cpp:191`): pausing the sim does not pause the sun; timescale does not speed the day.
5. **Pillar A remainder:** sun-intensity smoothstep + normalized transmittance + authored
   `nightAmbient` (`RenderPipeline.cpp:4921/4959-4970/5070`); moon hand constants
   (`lighting_pass.frag:474`, `:483`); moon calibration + true-midnight re-bless blocked by the
   RENDER-owned headless IN_GAME capture hang; A-T07 photo manual EV — lens shutter/ISO + a live EV
   meter exist (`main_client.cpp:7268-7282`, `:7331-7340`) but never drive `ctx.exposure`; A-T06
   GPU auto-exposure blocked behind 018-E/F (ring landed).
6. **WeatherEventSystem unwired** (`WeatherEventSystem.h:3-10` vs zero non-test consumers).
7. **Weather/wind grids are spawn-anchored** (`GameSession.cpp:355/365` pass
   `m_metadata.spawnPoint`): beyond the 1536 m extent (`WindFieldSystem.h:46-50`) precip degrades to
   the category base (`WeatherSystem.cpp:501` region check) and storms never follow the player.
8. **Dead shader file** `res/shaders/skybox.frag` still in-tree while only `enhanced_skybox.frag`
   is loaded (`SkyboxPass.cpp:23`) — a standing edit-the-wrong-file trap.
9. **Spec 010 rain is spatially uniform and unwired to weather** (`WaterSystem.h:105-106`,
   `main_client.cpp:5734/9625`).

## Risks

- **Re-bless storm on any further intensity coupling (ATMO-06):** completing FR-A-001 moves every
  lit frame; must be sequenced as one deliberate FLIP-reviewed re-bless (spec 015 NFR-003,
  `docs/specs/015-atmospheric-lighting-colored-glass/spec.md:316-318`), and is *currently
  unverifiable headlessly* while the IN_GAME capture hang stands (RENDER-owned).
- **Determinism risk on anchor-following weather (ATMO-13):** the weather/wind sub-hashes are pure
  functions of (seed, tick, anchor) (`WeatherSystem.h:25-30`); making the anchor follow the player
  makes the hash input player-motion-dependent — replay/lockstep must carry the anchor
  deterministically. HIGH hash risk; needs the heavy oracle + LREC1 + lockstep evidence.
- **World-hash coupling on weather->hydrology (ATMO-11):** rain injection touches the water
  sub-hash; safe only because hydrology is default-OFF (`WaterSystem.h:150`), so the all-off
  baseline stays byte-identical while the opt-in path re-pins.
- **A-T06 metering must stay on the async ring** — a sync readback regression is now mechanically
  banned by the render readback gates (`validate-engine-frontier.ps1:7305-7306`), which is the
  right order (018-E/F before consumers).
- **Live-bridge gates need the IN_GAME capture path**: any new live-play visual gate (ATMO-07/08)
  inherits the RENDER-owned headless IN_GAME render-capture hang as a hard dependency.

## Opportunities

- **The live weather bridge is cheap and transformative** — all machinery exists on both sides;
  one per-frame `SampleAt(player)` -> `set_weather_state`/`set_cloud_state` push (the exact pattern
  the scenarios already use) makes weather a *player-facing* feature.
- **Lightning as a world event**: the replayable strike schedule (`WeatherSystem.h:98-107`) is
  designed for bolt + light pulse + thunder + (future) fire ignition; consuming it in live play also
  future-proofs the ecology fire hook.
- **Season as a long-arc live feature**: the sun-declination + palette model is already
  deterministic and gate-covered; feeding it the sim tick gives 4-hour seasonal drift for free, and
  coupling `g_season` gives autumn foliage.
- **WeatherEventSystem gives drought/snow world states** that the farming/fire systems (moisture,
  dryness) can consume with zero new math.
- **Unifying TOD onto the sim tick** makes the photography game's light deterministic per replay —
  a photo of tick N is reproducible, which strengthens the photo-scoring loop and every visual gate.

## Backlog items

| id | summary | spec | effort | risk | deps | status | proving_signal |
|---|---|---|---|---|---|---|---|
| ATMO-01 | Pillar A intensity-coupling core landed: A-T01 LUT magnitude getters, A-T03 ambient-magnitude coupling, A-T04 lunar phase, A-T05/T05b deterministic TOD exposure + seam | 015 | L | low | — | done | TimeOfDaySweep gate (validate-engine-frontier.ps1 -Mode TimeOfDaySweep) + --smoke run==replay 6f008a9f637c40b7 |
| ATMO-02 | Dedicated moon radiance channel (RenderContext.moon_radiance -> u_moonRadiance), byte-identical default | 015 | S | low | — | done | WorldVisualSweep rerun (night frames byte-identical pre-calibration) + --smoke unchanged |
| ATMO-03 | Moon calibration + true-midnight (TOD 0.5) re-bless: tune m_moonRadiance, retire kMoonKeyScale=1.5 + wrap floor | 015 | M | low | RENDER(IN_GAME-capture-hang) | todo | TimeOfDaySweep night phases + WorldVisualSweep rerun + tools/flip_diff.py reviewed re-bless |
| ATMO-04 | A-T07: wire photo-mode manual EV (shutter/ISO/aperture ExposureValue) to override the A-T05 exposure seam | 015 | M | low | ATMO-01 | todo | NEW: PhotoManualEV engine-frontier gate — an EV nudge in photo mode moves captured frame mean-luma monotonically and overrides the TOD exposure curve |
| ATMO-05 | A-T06: GPU auto-exposure metering (last-frame average luminance) on the 017-A async readback ring, photo-EV override preserved | 015 | L | medium | 018-E/F, 017-A, ATMO-04 | todo | ReadbackDiscipline + RenderReadbackAllowlist gates green + TimeOfDaySweep + --render-benchmark p50 within budget |
| ATMO-06 | Close FR-A-001: replace the sun-intensity smoothstep + normalized transmittance + authored nightAmbient with LUT-derived magnitudes (or formally amend spec 015 to the exposure-curve architecture) | 015 | M | medium | ATMO-03, RENDER(IN_GAME-capture-hang) | todo | TimeOfDaySweep (golden hour reddens AND dims, AC-A-002) + WorldVisualSweep re-bless + tools/flip_diff.py heatmap review |
| ATMO-07 | Live-play weather bridge: per-frame SampleAt(player) -> set_weather_state + category-driven set_cloud_state + rain/snow particles in normal IN_GAME play (one-way, render-only) | new | M | low | RENDER(IN_GAME-capture-hang) | todo | NEW: LiveWeatherBridge gate — non-scenario IN_GAME run on a storm-forced seed asserts driven overlay uniforms (rain_intensity>0) + sky-luma drop vs a clear-seed control, reusing the WeatherVisual analyzers |
| ATMO-08 | Consume the sim lightning StrikeSchedule in the live client: bolt render + light pulse + distance-delayed thunder (replaces the random thunder timer) | new | M | low | ATMO-07 | todo | NEW: LiveLightning assertion in the LiveWeatherBridge gate — a sim-scheduled strike tick produces a bolt draw + frame-luma pulse in live play (WeatherVisual bolt/pulse analyzers) |
| ATMO-09 | Feed the authoritative sim tick to set_season_tick in live play and couple the procgen foliage autumn palette (g_season) to get_season_phase | new | S | low | — | todo | TimeOfDaySweep (6-phase season assertions stay green) + NEW: LiveSeasonTick ctest — after N live sim ticks, get_season_phase()==(N%432000)/432000 |
| ATMO-10 | Unify time authority: make TOD a pure function of the sim tick (like season/lunar), data-driven day length via SystemConfig (retire the hardcoded 60 s wall-clock day), honoring g_timeScale and photo-mode hold | new | M | medium | ATMO-09 | todo | --smoke run==replay 6f008a9f637c40b7 unchanged (render-only) + TimeOfDaySweep + NEW: TodTickPurity ctest — same tick -> same TOD independent of frame pacing |
| ATMO-11 | Drive spec-010 hydrology rain from WeatherSystem.PrecipitationAt: deterministic per-cell precip -> integer rain-mm injection (weather side; WATER owns the sink) | 010 | M | medium | 010, WATER(hydrology) | todo | Heavy oracle + LREC1 replay + --smoke run==replay 6f008a9f637c40b7 (hydrology default-OFF baseline byte-identical) + validate-determinism-matrix.ps1 |
| ATMO-12 | Wire WeatherEventSystem (drought/snow/storm Markov windows, seed+25) into the GameSession tick to modulate plant moisture + fire dryness | new | M | medium | ATMO-11 | todo | Existing ctest WeatherEvents.* (test/sim/weather_event_test.cpp) + heavy oracle + --smoke (no-participant worlds byte-identical) |
| ATMO-13 | Region-follow the weather/wind grids (streaming anchor instead of fixed spawnPoint) so weather exists beyond 1536 m and follows the player, deterministically | new | L | high | — | todo | WindFieldDeterminism gate + heavy oracle + LREC1 replay + lockstep evidence + MovingResidency gate + --smoke run==replay |
| ATMO-14 | Snow-cover ground response: accumulate/melt visual snow cover from the Snow category + WeatherEvent snow intensity (albedo/roughness shift, render-only) | new | L | medium | ATMO-07, ATMO-12 | todo | NEW: SnowCover gate — snow-forced capture asserts ground-ROI albedo/luma shift vs clear control + WorldVisualSweep no-new-flags |
| ATMO-15 | Delete (or tombstone) the dead res/shaders/skybox.frag — only enhanced_skybox.frag is loaded; the dead file is a standing wrong-file-edit trap | new | S | low | — | todo | ShaderInventory gate (validate-engine-frontier.ps1 -Mode ShaderInventory) green after removal + WorldVisualSweep unchanged |

**Ownership boundaries honored:** spec 015 Pillars B/C-1/C-2 (froxel volumetrics, colored shadows,
OIT glass) and the headless IN_GAME capture hang are RENDER-owned; the 017-A ring itself is
GPU-owned; the hydrology solver side of ATMO-11 is WATER-owned; wind-gust/rain audio is AUDIO-owned
(`AtmosphereAudio` gate); the worldgen-preview weather chips are UI-owned
(`WorldgenPreview.cpp:455-461`).
