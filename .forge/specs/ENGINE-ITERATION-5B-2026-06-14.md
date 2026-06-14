# Engine Iteration 5b Spec — Life & Water (SKELETON)

Status: SKELETON (authored 2026-06-14 alongside the 5a spec round). 5b is the
second half of iteration 5 (split 5a/5b, owner 2026-06-14). It is **dispatched
after 5a closes** so it inherits the post-5a `world_hash` (the 5a `wind` +
`weather` sub-hashes) and the re-blessed gate baselines. The full
`dispatch.json` + `design-decisions.md` for 5b are authored at 5a closeout / 5b
kickoff; this skeleton fixes the system list, the folded water backlog, and the
hard dependencies on 5a outputs so 5a's read-side APIs are designed for them.

Inherits (binding): `.forge/artifacts/engine-roadmap/long-range-roadmap.md`
§"Iteration 5" items 4–7; the iteration-4 determinism contract; the 5a
design-decisions sim/render line + APIs.

## Theme

5a made the world *behave* atmospherically. 5b makes it *alive and grounded*:
instanced ground cover and foliage that waves in the 5a wind, creatures that
react to weather/time/light through a stimulus channel registry, atmosphere
audio layered on weather, and waterfalls dressing the rivers — plus the
folded iteration-4 water backlog, re-sequenced now that 5a's aerial perspective
has reshaped the far-water look. Engine only; gameplay loop still deferred to
iteration 7.

## Systems

1. **Instanced ground cover & foliage with wind response** (engine path, game
   assets). One instanced scatter system: grass blades/tufts, pebble/gravel
   scatter, twigs/shells/clutter, canopy. Density from the biome table's
   vegetation/cover block (iter-4 hook, parsed-not-consumed today) modulated by
   slope/moisture. Vertex-level wind displacement reading the **5a wind grid**
   (`SampleWind`); pebbles don't wave, grass does. Distance-faded against the
   far-LOD horizon; budgeted via the release perf lane. The single biggest
   "real place" multiplier for photography. Render-only instancing; deterministic
   scatter placement is a pure function of seed + chunk (no `world_hash` growth
   unless placement feeds gameplay — it does not in 5b).
   **Depends on 5a:** wind-sampling API (A2).

2. **Ecology coupling** (Instinct pillar deepening). Engine-side **stimulus
   channel registry** — weather, temperature, time-of-day, light level — feeding
   the planner; extends the T-I3-18 single-channel stimulus pattern to a
   registry. Creature reactions (shelter-seeking in rain, dawn activity) stay
   game data. Channels that read sim-authoritative state (weather, season) must
   respect the sim/render line — the planner is sim-side, so channel *inputs* it
   consumes are the hashed weather/wind state, not render overlays.
   **Depends on 5a:** weather-state query API (B1), season time-scale (C2).
   Gate: stimulus-channel planner gate (behavior differs across weather/time
   fixtures, deterministically).

3. **Atmosphere audio**. Wind/rain ambience layers on the existing
   `AudioPropagationSystem`; reverb modulated by weather via
   `EnvironmentalAudioSystem`. Null-audio gates stay green; audio telemetry
   artifact extends. Render/client-side.
   **Depends on 5a:** weather-state query API (B1), wind field (A2).

4. **Waterfalls** (rendered phenomena, no new sim). Deterministic site detection
   where a river course / water body crosses a steep drop (waterline + height
   data already exist from iter-4 rivers + the heightfield). Dressing: animated
   falling sheet (flow-map shader), spray/mist via the **5a particle framework**,
   plunge-pool foam, distance-attenuated roar via `AudioPropagationSystem`. Same
   falls for every player/replay (site detection is deterministic; the dressing
   is render-only). Iteration-6 upgrade (shallow-water flow dynamics on the
   Aetheric field stack) explicitly out of 5b.
   **Depends on 5a:** particle framework (A1); iter-4 rivers.

## Folded water backlog (from the iteration-4 closeout, re-sequenced)

Sequenced **after** 5a aerial perspective so the FarLodHorizon far-water bands
are re-derived once, against the new sky/fog look:

- **Live-ring sea coverage** — who renders the 0–512 m sea surface beyond the
  water-sim radius (`FarLodSystem::kLiveRingRadiusMeters = 512`)? Bare sand
  seabed currently shows inside the live ring where the water sim doesn't reach
  and the far sheet (`<176 m` discard) correctly doesn't draw. Decide the owner of
  that annulus.
- **Seabed waterline terracing** — `kFarLodHeightQuantScale` (1/16 m) height
  quantization produces horizontal banding where the gently-sloping far seabed
  crosses the waterline. De-band or hide below water.
- **Sand-flat noon brightness** — near-sea-level dry sand flats render
  sun-bright (~234); albedo-calibration / preset-shaping territory.
- **FarLodHorizon boundary-band premise re-derivation** — the blue-dominance
  classifier (`b > r + 25 && g >= r && b > 150 && r < 205`, band 128–384 m,
  ratio ~0.0071) was tuned pre-aerial-perspective. Re-derive the bands now that
  scattering/aerial fog tints far water. This is the explicit reason the water
  backlog was folded into iteration 5 rather than fixed standalone.

## Gates (to be detailed at 5b kickoff)

- Foliage instancing perf budget (release lane) + distance-fade vs far-LOD.
- Stimulus-channel planner gate (deterministic behavior divergence across
  weather/time fixtures).
- Atmosphere-audio telemetry + null-audio green.
- Waterfall site-detection determinism + visual capture (sheet + spray + foam).
- Re-derived FarLodHorizon water bands green; sand-flat brightness band.

## Out of 5b (→ iteration 6+)

Shallow-water flow dynamics (Aetheric field), persisted snow/season terrain
accumulation, procedural/evolved creatures (own spec round), the photography
loop (iteration 7).
