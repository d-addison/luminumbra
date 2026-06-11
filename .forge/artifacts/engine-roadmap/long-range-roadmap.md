# Luminumbra Long-Range Roadmap (iterations 4-7)

Owner-directed (2026-06-11). Binding priority frame: **engine first; the
photography/gameplay core loop is 100% last**. The Atmospheric pillar is
flagged IMPORTANT by the owner and leads iteration 5. Every iteration still
gets its own spec -> research -> critique -> ultimate-plan round before
execution; this document is the Wave-0 input to each of those rounds, not a
substitute for them. The four TDD pillars — SHIELD (world/SDF), Instinct
(creature minds), Atmospheric (weather/wind/seasons/time), Aetheric (scalar
fields/light) — each get a home below.

Standing cross-iteration spine (every iteration, non-negotiable):
- Gate-first discipline; determinism contracts only move via deliberate
  versioned bumps; PerfRegression + RenderHealth re-bless protocol.
- Engine/game split-lint stays green; pillar SYSTEMS are engine, pillar
  CONTENT (creatures, crystals, biome palettes, clips) is game data.
- 30 Hz SimulationClock is the determinism anchor for every new system.
- Claude Code agent teams execute; Forge gates/bookkeeping verify.

## Iteration 4 — World identity + authority transport (LOCKED, handoff §Iteration 4 Directives)

1. Wave A (LEAD): biomes (seed +8/+9), rivers (+10), surface structures;
   far-tile material variety; audio reverb (trigger satisfied).
2. Wave C: input-lockstep transport vs the headless server; world-hash
   desync oracle; loopback + LAN, one remote client. A before C (hash
   re-bless ordering).
3. SHIELD-RT spike: timeboxed raymarch prototype, evidence-only.
4. Slack extras: Lua bindings/hot-reload, StreamingProfile meshing-skip,
   GPU SDF enablement behind parity gate.

## Iteration 5 — ATMOSPHERIC PILLAR (LEAD) + Instinct/ecology coupling

The world starts *behaving*. Builds directly on iteration 4's biomes
(weather wants climate zones) and iteration 3's instancing fixes (foliage
wants instanced draws) and creature planner (ecology wants stimuli).

1. **Wind grid (engine)**: coarse vector field on the 30 Hz tick (the
   first production consumer of the generalized field architecture —
   design it so Aetheric scalar fields reuse the same container/budget in
   iteration 6). Deterministic, snapshot-gated, seed-registry offsets
   appended FIRST.
2. **Weather system (engine systems, game content)**: biome-aware fronts —
   rain/snow/fog volumes, wetness response in materials, storm cells
   advected by the wind grid. The dormant weather_system.frag work from
   iteration 2 graduates from beautification to simulation-driven.
   Weather state is part of world_hash (server-authoritative, replicated
   via lockstep inputs/seeded schedule — NOT client-local).
3. **Seasons + celestial model (engine)**: long-period time scale driving
   sun path, day length, biome material/foliage palettes; time-of-day
   sweep gates extend to season sweep gates.
4. **Instanced foliage with wind response (engine path, game assets)**:
   grass/canopy instancing budgeted via PerfRegression; vertex-level wind
   displacement sampling the wind grid; pairs with biome vegetation maps.
5. **Ecology coupling (Instinct pillar deepening)**: engine-side stimulus
   channels (weather, temperature, time, light level) feeding the planner;
   creature CONTENT reacts in game data (shelter-seeking in rain, dawn
   activity). Extends the T-I3-18 stimulus pattern from one channel to a
   channel registry.
6. **Atmosphere audio**: wind/rain ambience layers on the propagation
   system; reverb modulated by weather.
Gates: wind-field determinism hash; weather visual + state-hash gates;
foliage instancing perf budget; season snapshot sweep; stimulus-channel
planner gate (behavior differs across weather fixtures); Endurance300
under storm load.

## Iteration 6 — AETHERIC PILLAR + SHIELD-RT productionization + multiplayer scale

The world gains its strange light, and the far field becomes the real
SHIELD-RT renderer.

1. **Aetheric field system (engine)**: generalized scalar-field stack
   (diffusion exists as fields/ScalarFieldDiffusion post-T-I3-17) on the
   tick with the field-budget slot in TickSimulation; emission/absorption
   sources tied to the materials LUT emission path; field sampling API for
   planner stimuli and rendering. Game content: LuminCrystal/Glimmer
   ecosystems consume it — engine knows only "emissive scalar fields".
2. **SHIELD-RT far field (engine)**: if the iteration-4 spike's evidence
   holds, productionize the SDF raymarch as the far-field renderer; F1/F2
   region tiles retire to a fallback/LOD-blend role or are absorbed.
   Gates: parity vs the tile path at the 1536 m horizon, perf budget at
   greater-than-F2 distances, seam/transition gate near-to-far.
3. **GPU SDF live parity full enablement** (if not cashed in iteration 4)
   — the raymarch wants GPU-resident SDF anyway; these converge.
4. **Multi-anchor streaming + server scale (engine)**: multiple players on
   the lockstep session, per-anchor streaming budgets, server tick-rate
   holds under N anchors (HeadlessServerTick gains a multi-anchor mode).
5. Slack: WorldList/save-slot management; Lua surface area widening for
   game systems if hot-reload landed in 4.
Gates: field determinism (replay-stable), emission visual gate, raymarch
parity + perf, multi-anchor endurance.

## Iteration 7 — PROJECT CAPTURE: the photography loop (the game, LAST by design)

Every engine pillar now exists; the camera arrives to photograph them.

1. **Camera/lens system (game, thin engine hooks)**: focal lengths, DoF,
   shutter/exposure, focus — engine exposes render hooks (depth-aware blur,
   exposure control already half-exists via tonemapping); lenses/film are
   game data.
2. **Capture scoring + Codex**: subject detection vs the creature roster
   (planner state at capture time = behavior tags), composition/light
   scoring consuming Atmospheric (golden hour, weather mood) and Aetheric
   (crystal light) state — the pillars become the scoring inputs, which is
   WHY they were built first.
3. **Light/shadow tools (game)**: the player's light-manipulation toolkit
   driving Aetheric sources; creature reactions via the stimulus channels
   from iteration 5.
4. **Roster + habitats (game data)**: MVP creature roster across biomes,
   season/weather-dependent behaviors — content sprint on the iteration-5
   channel registry.
5. **Vertical slice + ship polish**: session flow (explore -> stalk ->
   capture -> Codex), save UX, the zen pacing pass.
Gates: capture-scoring determinism fixture, photo-mode perf budget,
slice playtest artifact (scripted session producing a scored Codex entry),
full-sweep + endurance as always.

## Explicitly unscheduled (revisit at iteration-7 close)

F3/3 km tier (superseded by SHIELD-RT), matchmaking/WAN networking, mod
tooling beyond Lua, console/platform ports, terrain editor tooling.

## Pillar coverage map

| Pillar | Built in | Consumed by |
|---|---|---|
| SHIELD | iters 1-4 (terrain, far-LOD), 6 (RT far field) | everything |
| Instinct | iter 3 (planner runtime), 5 (stimulus channels) | iter 7 scoring |
| Atmospheric | **iter 5 (LEAD)** | iter 7 light/mood scoring |
| Aetheric | iter 6 | iter 7 light tools |
