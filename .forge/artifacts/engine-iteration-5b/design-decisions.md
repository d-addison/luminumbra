# Engine Iteration 5b Design Decisions (binding)

Binding for all 5b waves. Inherits the iteration-4 determinism contract + the 5a
design-decisions sim/render line and APIs. 5b = "life & water": foliage, ecology
stimulus channels, atmosphere audio, waterfalls, folded water backlog. Base:
`feat/polyglot-audit-roadmap` tip `01e9789`, **world_hash `d950a6afc12a5cdc`**.

Sources: `.forge/specs/ENGINE-ITERATION-5B-2026-06-14.md`,
`.forge/artifacts/engine-roadmap/long-range-roadmap.md` §Iteration-5 items 4–7,
`.forge/artifacts/engine-iteration-5a/design-decisions.md` (the 5a APIs).

## 0. The headline determinism decision (NO world_hash bump #4)

5b is designed to **keep `world_hash` at `d950a6afc12a5cdc`** — no mega-bump. The
only sim-touching system is ecology (the planner). To avoid a bump:
- Foliage, audio, waterfalls, water-backlog are RENDER/CLIENT-only (never hashed).
- **Ecology adds the stimulus-channel registry + planner API, but the CANONICAL
  world (the HeadlessServerTick default seed) creatures do NOT subscribe to the
  new channels** — reactions are game-data opt-in. So the default entities
  sub-hash is byte-unchanged. The StimulusChannelGate uses a DEDICATED fixture
  whose creatures DO react; that fixture has its own hash, asserted self-
  consistent (run==replay), never folded into the canonical world_hash.
- If, during implementation, the registry cannot be added without perturbing the
  default entities sub-hash, STOP and report — a bump is an orchestrator decision,
  not a silent change.

## 1. Seed-offset registry (append-only)

Existing through 5a: …+10 rivers, +11 wind, +12 weather, +13 lightning. **+14
reserved for foliage scatter jitter** (F1) IF a global seed is needed; prefer
deriving scatter purely from `(chunk coords, biome id, instance index)` hashing
so no global seed is consumed (foliage is render-only and per-chunk). Pin the
choice in F1. No other 5b system takes a seed offset.

## 2. Foliage instancing + wind response (F1) — render-only

- **Mechanism:** instanced draw of scatter instances per visible chunk; reuse the
  T-I4-16 persistent-mapped pool / the A1 instance-buffer pattern. One scatter
  system covers grass blades/tufts, pebble/gravel, twigs/shells/clutter, canopy
  (per-archetype mesh + sway flag).
- **Placement (PINNED):** deterministic pure function of `(chunk coords, biome id,
  slope, moisture, instance index)` via a hash — NO global RNG, NO world_hash.
  Density driven by the biome table's vegetation/cover block (the iter-4 hook,
  parsed-not-consumed), modulated by slope/moisture.
- **Wind:** vertex-level displacement sampling the A2 `WindFieldSystem::SampleWind`
  (via the replicated render bridge, one-way — same pattern C3 clouds used);
  grass/canopy sway, pebbles/clutter do not (per-archetype sway flag).
- **Distance:** faded out against the far-LOD horizon (no foliage in far-LOD
  tiles); budgeted on the RELEASE perf lane (GPU-timer + instance-count caps).
- **Gate: FoliageInstancing** — coverage density matches the biome table within a
  band at fixed seeds; distance-fade present (no foliage beyond the live ring);
  sway responds to wind (calm vs windy instance displacement differs); release
  GPU-timer ≤ pinned budget. Keep PlayerView/RenderHealth green (foliage is a
  deliberate RenderHealth re-bless — it adds pixels everywhere there's ground;
  log it).

## 3. Ecology stimulus channels (E1) — sim/planner, canonical hash UNCHANGED

- **Engine piece:** a stimulus-channel REGISTRY feeding the planner — channels for
  weather (read B1 weather-state query), temperature, time-of-day + season (read
  C2), light level. Extends the T-I3-18 single-channel pattern to a registry +
  a query API the planner samples each tick. DeterministicMath; SimDeterminismLint.
- **Game data:** creature reactions (shelter-seeking in rain, dawn activity) are
  data — engine knows only "channel → scalar stimulus".
- **Canonical hash:** per §0, default-world creatures do NOT subscribe → default
  entities sub-hash unchanged → world_hash stays `d950a6afc12a5cdc`.
- **Gate: StimulusChannelGate** — a dedicated fixture with reactive creatures;
  assert behavior DIFFERS across weather/time fixtures (e.g. plan/state changes
  between a rain fixture and a clear fixture; between dawn and noon) and is
  DETERMINISTIC (run==replay within the fixture). Reuses the CreatureSlice
  capture machinery.

## 4. Atmosphere audio (AU1) — client-side

- Wind/rain AMBIENCE layers on the existing `AudioPropagationSystem`; reverb
  modulated by weather via `EnvironmentalAudioSystem` (read both headers first;
  extend, don't rebuild). Intensity driven by the replicated weather/wind state.
- **Gate: AtmosphereAudio** — extend the audio-telemetry artifact (ambience layer
  present + scales with weather intensity; reverb param shifts with weather);
  the **null-audio gates MUST stay green** (ambience is optional dressing). No
  world_hash, no visual-gate dependency on audio.

## 5. Waterfalls (W1) — rendered phenomena, NO new sim

- **Detection (deterministic, render-side):** scan where a river course / water
  body crosses a steep height drop using EXISTING waterline + heightfield data
  (iter-4 rivers). Same sites for every player/replay (pure function of the
  generated world) but computed render-side — NOT added to world_hash.
- **Dressing:** animated falling sheet (flow-map shader), spray/mist via the A1
  particle framework, plunge-pool foam, distance-attenuated roar via
  `AudioPropagationSystem`. Iteration-6 shallow-water flow (Aetheric) OUT.
- **Gate: WaterfallVisual** — site-detection determinism (same seed → same sites)
  + a capture asserting the sheet + spray + foam render at a detected site.

## 6. Folded water backlog (WB1) — render / far-LOD, sequenced AFTER 5a aerial

- **Live-ring sea coverage:** decide the owner of the 0–512 m sea surface beyond
  the water-sim radius (`kLiveRingRadiusMeters = 512`) where bare seabed shows;
  render a sea surface there (extend the far sheet inward to the live-water edge,
  or a dedicated live-ring sheet) WITHOUT breaking the FarLodHorizon boundary-band
  blue-dominance classifier (the trap that reverted the iter-4 attempt — re-derive
  the bands here).
- **Seabed waterline terracing:** the `kFarLodHeightQuantScale` (1/16 m) banding
  where the far seabed crosses the waterline — de-band (finer quant below water)
  or hide (depth-fade). Far-tile byte change = deliberate FarLodHorizon re-bless,
  NOT world_hash (far-LOD tiles are separate from the sim chunk hash).
- **Sand-flat noon brightness:** near-sea-level dry sand reads ~234 sun-bright —
  albedo calibration (material LUT) toward a natural sand tone.
- **FarLodHorizon band re-derivation:** the blue-dominance classifier
  (`b>r+25 && g>=r && b>150 && r<205`, band 128–384 m) was tuned pre-aerial-
  perspective; 5a scattering/aerial fog now tints far water — RE-DERIVE the bands
  once, here, and re-bless FarLodHorizon deliberately.
- **Gate:** FarLodHorizon green with re-derived water bands + a sand-flat
  brightness band assertion.

## 7. Gate list + budgets

| Gate | Kind | Assertion |
|---|---|---|
| **FoliageInstancing** | visual + perf | density vs biome table band; distance-fade; wind-sway differs calm/windy; release GPU-timer ≤ budget |
| **StimulusChannelGate** | determinism + behavior | reactive fixture: behavior differs across weather/time fixtures + run==replay |
| **AtmosphereAudio** | telemetry | ambience present + scales with weather; null-audio green |
| **WaterfallVisual** | determinism + visual | same seed → same sites; sheet+spray+foam render at a site |
| **FarLodHorizon (re-derived)** | visual | re-derived far-water bands green + sand-flat brightness band |
| **Closeout** | sweep | full engine-frontier + runtime-stability + Endurance300 + forge verify; world_hash stays d950a6afc12a5cdc |

## 8. Ownership + executor map (ALL Opus — Fable unavailable)

| Task | Executor | Primary files |
|---|---|---|
| T-I5b-0 design | opus (inline) | this doc |
| T-I5b-1 foliage | opus-agent | RenderPipeline, passes/ (new FoliagePass), Mesh, data/common/foliage/, RuntimeScenarioHarness, validator |
| T-I5b-2 ecology | opus-agent | ai/Instinct* (planner), systems/ (stimulus registry), RuntimeScenarioHarness, validator |
| T-I5b-3 audio | opus-agent | audio/AudioPropagationSystem, audio/EnvironmentalAudioSystem, RuntimeScenarioHarness, validator |
| T-I5b-4 waterfalls | opus-agent | rendering/ (detection + sheet), res/shaders/, passes/ParticlePass (spray), audio, RuntimeScenarioHarness, validator |
| T-I5b-5 water-backlog | opus-agent | rendering/FarLodSystem, world/FarLodStore, res/shaders/water+lighting, data/common/materials.json, RuntimeScenarioHarness, validator |
| T-I5b-6 closeout | opus (inline) | handoff.md, perf-baseline-release.json |

## 9. Operating rules

1. world_hash STAYS `d950a6afc12a5cdc` (§0); STOP+report if any task must bump it.
2. Render/client-only systems (foliage, audio, waterfalls, water-backlog) never
   write sim/world_hash inputs (one-way). Ecology is sim but canonical-neutral.
3. Perf: foliage gets a release GPU-timer budget; the quiet-machine perf re-bless
   (carried from 5a) is a 5b-closeout prerequisite for an honest foliage budget.
4. Worktree base hazard: reset to feat tip; COPY vendored fastnoise/imgui (real
   copies, NOT junctions) + `git submodule update --init vendor/googletest`;
   NEVER `git worktree remove --force` (deletes through junctions). Build via the
   PowerShell tool (`-j 2`), not Bash.
5. Foliage/waterfalls/water-backlog all deliberately re-bless RenderHealth /
   FarLodHorizon — log each.
