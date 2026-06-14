# Engine Iteration 5a Spec — Atmospheric Core (particles, wind, weather, sky, clouds, lightning)

Status: FINAL (critique applied — see engine-iteration-5a/critique.md). Iteration
5 is split 5a/5b by owner decision (2026-06-14); this is **5a, the atmospheric
core and owner-flagged LEAD**. 5b (foliage, ecology, atmosphere audio,
waterfalls, folded water backlog) is a separate dispatch authored after 5a's
`world_hash`/gate re-bless lands. Inherits (binding, do not restate here):
- Priority + system definitions: `.forge/artifacts/engine-roadmap/long-range-roadmap.md`
  §"Iteration 5 — ATMOSPHERIC PILLAR (LEAD)" (owner-mandated 2026-06-11).
- Research: `.forge/artifacts/engine-research/atmospheric-particles-wind-weather-sky.md`,
  and (inherited) `.forge/artifacts/engine-research/opengl-cpp-architecture.md`,
  `.forge/artifacts/engine-research/worldgen-lockstep-sdfrt.md`.
- Determinism contract (iteration 4): `src/luminumbra_common/core/DeterministicMath.h`,
  `SimDeterminismLint`, pinned FP flags on `luminumbra_common`+`luminumbra_server`,
  per-system `world_hash` sub-hashes, LREC1 replay, delay-based lockstep.
- Execution: Claude Code agent teams; Opus 4.8 for ALL tasks
  (`executor: opus-agent` per task) — Fable is UNAVAILABLE (owner 2026-06-14).
  Planning/closeout tasks run as orchestrator-driven inline Opus turns;
  implementation tasks run as Opus worktree agents. Forge gates/bookkeeping
  verify.
- Iteration-4 closeout state: 200/200 ctest; all engine-frontier +
  runtime-stability gates green at `726ab8f`; `world_hash` `2fa007951a21e140`.

## Theme

The engine after iteration 4 renders a complete, deterministic, headlessly-
simulated, biome'd world that is shared and replayable. Iteration 5a makes that
world *behave*: wind blows, weather moves through, storms throw deterministic
lightning, and the sky becomes physically-based so sunrise/sunset/golden-hour
palettes emerge from scattering rather than an authored ramp — with clouds that
drift on the wind and cast crawling shadows. Engine only; zero gameplay-loop
work. The governing discipline is the **sim/render line**: sim-authoritative,
deterministic, hashed state (wind field, weather state, lightning schedule, the
season time-scale) obeys the iteration-4 determinism contract; all pixels
(particles, sky scattering, cloud fields, bolt geometry, overlays) are
render-only and never enter `world_hash`.

## Wave 0 — Design decisions doc (inline, Opus)

`.forge/artifacts/engine-iteration-5a/design-decisions.md`, binding for all
waves: seed-registry offsets appended FIRST (+11 wind, +12 weather/storm, +13
lightning schedule; append-only, collision-free); GPU particle emitter format
(game-data curves rate/lifetime/velocity/size/color + `.ltex` array refs + blend
mode; instanced pool, not compute, for v1); generalized wind-grid `Field`
container/budget (vector field; shared with the Aetheric scalar stack in
iteration 6 — reuse/refactor `fields/ScalarFieldDiffusion`); weather state schema
that enters `world_hash` (region category + storm cells + precip field + strike
schedule) vs the render-only overlay; sky model (Hillaire 2020 LUTs:
transmittance ~256×64, multi-scatter ~32×32, sky-view ~200×100; analytic aerial-
perspective term in the lighting pass, not froxel); cloud tier-1 approach (2.5D
wind-advected coverage + projected cast-shadow in the lighting pass); the full
gate list + thresholds; the sub-hash registry update + the deliberate
`world_hash` mega-bump + re-bless plan; the file-ownership map + executor
assignment per task; the determinism + perf operating rules.

**Prerequisite (carried debt, Wave 0):** capture the post-iteration-4-trio
quiet-machine release perf baseline (`.forge/scripts/run-release-perf-lane.ps1
-Bless`) so all 5a perf budgets gate against an honest baseline.

## Wave A — Engine enablers (render ∥ sim; 2 agents parallel)

- A1 **GPU particle framework** (render): instanced fixed-capacity pool
  (persistent-mapped, reusing the T-I4-16 pattern), depth-buffer soft-particle
  fade + collision against the G-buffer, lit by the existing path; new
  `ParticlePass` slotted AFTER `SkyboxPass` (transparent composite into the lit
  HDR target). `magical_particles.{vert,frag,geom}` re-homes onto it. Emitter
  **schedule** is sim-deterministic (descriptor = id/type/region/spawn-rate/seed/
  enable); particle **motion** is render-only (NOT hashed). Gate:
  **ParticleEmitterDeterminism** (sim-tick emitter-descriptor snapshot byte-equal
  across runs) + a visual capture + a hard release-lane perf budget.
- A2 **Wind grid** (sim): coarse 2.5D vector field on the 30 Hz tick in a
  generalized `Field` container (iteration-6 Aetheric reuse mandated); base
  large-scale direction from `seed+11` low-freq noise + storm perturbations
  (consumed in Wave B); DeterministicMath only; snapshot-gated; `wind` sub-hash
  added to `world_hash`. Gate: **WindFieldDeterminism** (seed → N-tick field hash
  equal across runs). Seed offset `+11` appended FIRST.

## Wave B — Weather (depends on A1 + A2)

- B1 **Weather core** (sim): server-authoritative storm-cell model over a
  biome-aware base (`seed+12` pressure field × biome temp/humidity); storm cells
  spawned on a seeded schedule, advected by the wind grid; precipitation intensity
  field; render-side wetness material modulation. `weather_system.frag` graduates
  from the `set_weather` debug overlay to sim-driven uniforms. Weather **state**
  enters `world_hash` (`weather` sub-hash: region category + storm cells + precip
  field + strike schedule); overlay/streaks are render-only. Seed offset `+12`
  appended FIRST. Gate: **WeatherVisual** extends to a state-hash assertion +
  real-state baseline-vs-weather luminance/streak checks.
- B2 **Precipitation particles** (render+sim seam): rain/snow emitted through the
  A1 framework, wind-advected (rain slants in storms via the A2 field), splash/
  spray emitters on surface impact (depth-buffer). Depends on A1 + A2 + B1. Gate:
  precipitation present + slant-responds-to-wind in a weather capture; emitter
  descriptors deterministic.
- B3 **Lightning** (sim+render): deterministic strike events `(tick, pos,
  magnitude)` scheduled by storm-cell state from `seed+13` (sim-side, in
  `world_hash` strike schedule); render: seeded branching bolt + a 1-to-few-frame
  full-scene light pulse through the **lighting pass**; thunder via the existing
  `AudioPropagationSystem` with distance-correct delay (distance/343); scorch via
  emitters. Iteration-6 fire-ignition hook NOTED, not built. Seed offset `+13`
  appended FIRST. Gate: **WeatherVisual** asserts a captured strike frame shows
  the luminance pulse + bolt pixels (the photography timing shot).

## Wave C — Sky / celestial (render-led; partly ∥ Wave B)

- C1 **PBR atmospheric scattering sky** (render): Hillaire 2020 LUTs replace the
  authored gradient in `enhanced_skybox.frag`; sun/sky/ambient/fog read the same
  transmittance for coherent low-sun palettes; the `u_skyDayFactor` seam stays as
  the night-darkening envelope; analytic aerial-perspective term in the lighting
  pass (wire `volumetric_lighting.frag` / extend `lighting_pass.frag`). Gate:
  **TimeOfDaySweep** gains dawn/dusk **hue-band** assertions; **SkyboxVisual**
  asserts low-angle scattering palette emergence; noon luminance held inside the
  existing band where achievable (else deliberate logged re-bless).
- C2 **Seasons + celestial model** (sim time + render): long-period **tick-
  derived** time scale (never wall-clock) driving sun path, day length, biome
  material/foliage palettes. Gate: **TimeOfDaySweep** extends to a **season
  sweep** (per-season sun-path + palette bands). The season time-scale is a
  deterministic function of tick count; if any season state is read sim-side it is
  hashed (prefer render-derived from tick to avoid hash growth — design-doc pin).
- C3 **Cloud layer tier 1** (render): wind-advected 2.5D coverage clouds
  (weather/biome-aware) + landscape-distance imposters; **real cast shadows** via
  projected coverage sampled in the lighting pass (crawling terrain shadows).
  Depends on A2 (wind) + C1 (sky) + lighting pass. Render-only (pure function of
  replicated weather state + tick + wind; nothing reads clouds back into the
  hash). Gate: **CloudShadow** (moving cast-shadow signature on terrain) + cloud
  layer present in SkyboxVisual. TIER 2 volumetric raymarch is iteration 6 —
  out of scope.

## Wave D — Closeout (inline, Opus)

`T-I5a-9-closeout`: full gate sweep incl. all new modes
(ParticleEmitterDeterminism, WindFieldDeterminism, extended WeatherVisual /
TimeOfDaySweep / SkyboxVisual, CloudShadow), **Endurance300Storm** (300 ticks of
active storm, no perf cliff / state-growth leak), the existing engine-frontier +
runtime-stability sweep, `forge verify`, the deliberate `world_hash` mega-bump
re-bless + LREC1 replay + lockstep loopback + heavy-mode oracle re-validation in
the bump commit, release-lane re-bless log, the handoff section, and the **5b
planning inputs** (water-backlog premise re-derivation notes now that aerial
perspective exists).

## New / extended gates (validate-engine-frontier.ps1 + RuntimeScenarioHarness)

Follow the existing harness pattern (capture config → metrics struct → analyzer →
JSON/PNG writer → `main_client.cpp` scenario dispatch), as TimeOfDaySweep /
SkyboxVisual / WeatherVisual already do in
`src/luminumbra_client/core/RuntimeScenarioHarness.{h,cpp}`.

- **ParticleEmitterDeterminism** — sim-tick emitter-descriptor snapshot byte-equal
  across two runs; + visual capture; + hard release-lane perf budget.
- **WindFieldDeterminism** — seed → N-tick wind-field hash equal across runs;
  `wind` sub-hash in `world_hash`.
- **WeatherVisual (extended)** — `weather` state-hash stable across resim/replay +
  baseline-vs-weather luminance/streak (real state) + strike-frame luminance
  pulse + bolt-pixel assertion.
- **TimeOfDaySweep (extended)** — dawn/dusk hue-band assertions (scattering) +
  season-sweep phases.
- **SkyboxVisual (extended)** — low-angle scattering palette emergence.
- **CloudShadow** — moving cast-shadow presence on terrain (luminance delta in a
  fixed ROI as a shadow edge crosses).
- **Endurance300Storm** — 300 ticks under an active storm without a performance
  cliff or unbounded state growth.
- **PerfRegression (release-lane budgets)** — particle draw/update, wind-tick,
  weather-update, cloud render, sky precompute (startup), cloud-shadow lighting
  sample each get a hard threshold.

## Operating rules (binding, carried into every task prompt)

1. **Sim/render line.** In `world_hash`: wind field, weather state (region
   category + storm cells + precip field + strike schedule), the season time-
   scale if read sim-side. OUT of `world_hash`: particle motion, sky scattering,
   cloud fields, bolt geometry, all `*_system.frag` overlays, wetness modulation.
2. **Determinism (sim-side).** DeterministicMath wrappers only; pass
   SimDeterminismLint (no libm transcendentals / unordered-container iteration /
   wall-clock / `std::random` in sim paths); seed offsets `+11/+12/+13` appended
   FIRST; add `wind` + `weather` sub-hashes. The `world_hash` change is a
   deliberate mega-bump executed in ONE commit with the heavy-mode oracle + LREC1
   replay + lockstep loopback re-validated. If any sim addition is unexpectedly
   NOT hash-load-bearing or breaks replay, STOP and report — the bump is an
   orchestrator decision.
3. **Perf.** Every render addition pairs with a release-lane budget + threshold;
   RenderHealth re-bless is deliberate and logged; the quiet-machine baseline is
   captured in Wave 0 first.
4. **Scope.** Out of 5a: tier-2 volumetric clouds, fire-as-simulation, froxel
   volumetrics, compute particle pools, persisted snow/season terrain, 3D fluids,
   and ALL of 5b (foliage/ecology/audio/waterfalls/water backlog). No render
   feature lands without its visual gate; no sim feature without its determinism
   gate.
5. **Worktree base hazard.** Inject `git rev-parse HEAD` + scope-file existence
   check before any agent worktree work; prefer the main tree — `main`/
   `origin/HEAD` still points at the stale project-capture `972c133`.

## Success definition

5a is complete when:
- Wave 0 design doc pins all decisions with concrete values (no placeholders).
- A1/A2 land with ParticleEmitterDeterminism + WindFieldDeterminism green and the
  `wind` sub-hash in `world_hash`.
- B1/B2/B3 land with extended WeatherVisual (state-hash + strike timing shot)
  green and the `weather` sub-hash in `world_hash`.
- C1/C2/C3 land with extended TimeOfDaySweep (hue + season) + SkyboxVisual +
  CloudShadow green; sky/sun/ambient/fog coherent from scattering.
- The deliberate `world_hash` mega-bump is committed with LREC1 replay + lockstep
  loopback + heavy-mode oracle re-validated and the new world_hash logged.
- Endurance300Storm green; full ctest + engine-frontier + runtime-stability +
  `forge verify` green; release-lane re-blessed with budgets recorded.
- Handoff updated; 5b planning inputs written.
