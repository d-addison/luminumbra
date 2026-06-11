# Luminumbra Long-Range Roadmap (iterations 4-7)

Owner-directed (2026-06-11). Binding priority frame: **engine first; the
photography/gameplay core loop is 100% last**. The Atmospheric pillar is
flagged IMPORTANT by the owner and leads iteration 5. Every iteration still
gets its own spec -> research -> critique -> ultimate-plan round before
execution; this document is the Wave-0 input to each of those rounds, not a
substitute for them. The four TDD pillars — SHIELD (world/SDF), Instinct
(creature minds), Atmospheric (weather/wind/seasons/time), Aetheric (scalar
fields/light) — each get a home below.

## Execution-model tiering (owner-mandated 2026-06-11)

Fable (the frontier model) is reserved for PLANNING-TIER work only:
spec/research/critique/ultimate-plan rounds, dispatch authoring, merge
adjudication, gate-failure diagnosis, recovery from stuck executors.
IMPLEMENTATION is delegated down-tier: Opus 4.8 subagents (Agent tool
`model: "opus"`) for well-specified coding tasks; Codex dispatch (the
original forge executor path — infrastructure retained in .forge/) may be
revived for mechanical, fully-gate-defined tasks. The enabler is dispatch
quality: every task record must be self-contained (binding design-doc
refs, file-ownership map, inherited state, operating rules, verification
ladder, exact gate commands) so a lesser executor cannot go wrong in an
unbounded way — the gates, not the executor's judgment, define done.
Tasks whose dispatch cannot be written that tightly are planning-tier
work that hasn't finished; tighten the spec rather than upgrading the
executor. dispatch.json task records gain an `executor` field
(codex | opus-agent | fable) from iteration 4 on.

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
4. **Wave B — Surface detail & material fidelity (engine, owner-flagged
   2026-06-11 after the T-I3-18 capture review)**: the engine renders flat
   LUT colors only — no textures, normal maps, or detail variation; at
   photography distance (the game's core distance) there is nothing on
   screen but lighting. Scope: triplanar terrain texturing + normal/detail
   maps wired through the materials LUT; textured + normal-mapped skinned
   meshes; roughness variation; emissive->bloom calibration (the T-I3-18
   glow prop does not visibly glow). Gates: close-range material capture
   gate (the MaterialVisual pattern at 2-8 m), RenderHealth re-bless,
   PerfRegression. Precursor evidence: T-I3-18 creature captures.
   Related now-item at T-I3-22 closeout: shape the archipelago preset
   (left on legacy params by T-I3-11), re-frame creature capture
   eye-level, high-contrast creature material, composition check in the
   CreatureSlice gate.
5. Slack extras: Lua bindings/hot-reload, StreamingProfile meshing-skip,
   GPU SDF enablement behind parity gate.

## Iteration 5 — ATMOSPHERIC PILLAR (LEAD) + Instinct/ecology coupling

The world starts *behaving*. Builds directly on iteration 4's biomes
(weather wants climate zones) and iteration 3's instancing fixes (foliage
wants instanced draws) and creature planner (ecology wants stimuli).

0. **GPU particle framework (engine enabler, owner-mandated 2026-06-11 —
   precedes the weather wave, which consumes it)**: instanced/compute
   pools; emitters as game data (rate/lifetime/velocity/size/color curves,
   textures from the .ltex arrays); depth-buffer collision; soft
   particles; lit by the existing lighting path. magical_particles
   re-homes onto it. Gates: deterministic emitter fixture on the 30 Hz
   tick, visual capture, hard release-lane perf budget. Consumers in this
   iteration: rain/snow (wind-advected — rain slants in storms), water
   splash/spray, biome leaves/pollen, fire/smoke VISUALS (emissive sprite
   + smoke column emitters). Fire as a SPREADING SIMULATION is iteration
   6: an Aetheric scalar field (ignition/fuel/propagation) whose render
   side is these emitters — the field stack's first dramatic consumer.
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
   **Lightning (owner-mandated 2026-06-11)**: deterministic strike events
   scheduled by storm-cell state (sim-side, in world_hash — strikes are
   world events, not client effects); render side: branching bolt flash +
   a one-to-few-frame full-scene light pulse through the lighting pass;
   thunder via the audio propagation system with distance-correct delay
   (sound arrives seconds after the flash — free realism from machinery
   we already have); strike scorch/impact via emitters. Iteration-6 hook:
   strikes write ignition into the fire scalar field (storm starts a
   wildfire). Photography: THE timing shot — gate asserts a captured
   strike frame shows the luminance pulse + bolt pixels.
3. **Seasons + celestial model (engine)**: long-period time scale driving
   sun path, day length, biome material/foliage palettes; time-of-day
   sweep gates extend to season sweep gates.
   **Owner-mandated additions (2026-06-11)**: (a) physically-based
   atmospheric scattering sky (Bruneton/Hillaire precompute) replacing the
   authored gradient — sunrise/sunset palettes (pinks/purples/oranges)
   emerge from Rayleigh/Mie at low sun angles and drive sky + sun + ambient
   + fog coherently; TimeOfDaySweep gains dawn/dusk hue-band assertions.
   (b) Cloud layer TIER 1: wind-advected 2.5D coverage clouds
   (weather/biome-aware) with REAL cast shadows — cloud-shadow projection
   modulating sunlight in the lighting pass (crawling terrain shadows);
   fluffy imposter/sky visuals at landscape distance. TIER 2 (iteration 6,
   unchanged): full Nubis-style volumetric raymarch upgrade sharing
   SHIELD-RT infrastructure.
4. **Instanced ground cover & foliage with wind response (engine path,
   game assets; owner-expanded 2026-06-11)**: grass BLADES/tufts, pebble
   and gravel scatter, twigs/shells/clutter, canopy — one instanced
   scatter system, density driven by the biome table's vegetation/cover
   block (iter-4 hook, parsed-not-consumed today) modulated by slope/
   moisture; vertex-level wind displacement (grass waves, pebbles don't);
   distance-faded against the far-LOD horizon; budgeted via the release
   perf lane. The single biggest "real place" multiplier for photography.
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

## Recorded concept — procedural/evolved creatures (owner interest, 2026-06-11)

Blender-authored rigs are fully served by the LMS2/.lanim pipeline. The
evolution concept ("evolution type things") needs two ENGINE systems:
(1) parametric body generation — skeleton + mesh + skin weights from a
body-plan parameter vector; strong native fit: compose bodies from SDF
primitives and mesh via the existing marching-cubes stack (capsule limbs,
blended torsos; weights derive from per-primitive joint ownership);
(2) procedural locomotion — IK chains + deterministic gait synthesis as a
pose source beside clip sampling in AnimationRuntime (30 Hz, lockstep-
safe). GAME side: genomes/mutation/species/selection as data + systems;
iteration-5 ecology stimulus channels supply selection pressure. Synergy:
evolved fauna makes every world's Codex photographically unique. Earliest
sensible seed: IK/gait after iteration 5; parametric bodies as an
iteration 6/7 stretch or first post-photography feature. Gets its own
spec/critique round before any scheduling.

## Committed expansions (owner-mandated 2026-06-11 — "all great and must
## be added"; each still gets its spec/critique round for sizing/sequence,
## but inclusion is decided; listed by natural home + why it is cheap here)

Lockstep dividends (iter 4+): **session replay** (seed + input stream =
perfect replay; re-photograph missed moments, ghosts, trailers, desync
repro — consider inside the iter-4 transport wave, same machinery);
trap/remote cameras (headless sim + deferred render of a recorded tick);
time-lapse photo mode (SimulationClock fast-forward under authority).

Atmospheric (iter 5): microclimates (valley fog/rain shadows from wind
grid + heightfield); snow/season accumulation as persisted terrain state
(dirty-chunk machinery); deterministic celestial rare events per seed
(auroras/meteors/eclipses — rare-event photography pull).

Instinct (iter 5/6): creature memory/wariness (startled creatures flee
earlier next time — THE zen stalking mechanic); herds/flocking/predator-
prey; nests/territories/lifecycles + mating displays (rare Codex shots).

Aetheric/SHIELD (iter 6): underground biome volumes w/ bioluminescent
cavern ecosystems (night/cave photography x light fields); volumetric
clouds as a second SHIELD-RT raymarch consumer; deterministic erosion
detail pass (thermal+hydraulic) feeding river valleys.

Game loop (iter 7+): field-guide commissions (gentle goals, no quests);
real photo export to disk from the capture path; hides/camouflage tools
(pairs with wariness).

Owner-flagged highest leverage: replay, creature memory, celestial
events. Also recorded: procedural/evolved creatures (section above).

## Standing optimization discipline + framework-hardening iteration
## (owner-mandated 2026-06-11)

1. **Per-iteration optimization wave (STANDING, every iteration close)**:
   profile-driven, gate-ratcheted (the iteration-2 model — every win locks
   in via PerfRegression baseline re-bless + ratcheted thresholds, never
   silent). Rotating focus chosen by that iteration's profiling evidence:
   streaming/meshing hot paths, JobSystem scheduling/lane balance, memory
   (pooling/arenas, resident watermarks), render submission (batching,
   state-change reduction, bindless candidates), ECS iteration layout,
   far-LOD/SDF pipeline costs. Release-lane numbers (T-I3-20) become the
   canonical optimization currency from iteration 4 on.
2. **Framework-hardening iteration (between 6 and 7, before the game
   loop lands)**: deliberate refactor pass over the engine AS A FRAMEWORK —
   formalize the engine public API surface (engine as a reusable library;
   game links against it, never reaches inside); module dependency audit
   (common/client/server layering enforced by build, not convention);
   subsystem lifecycle unification (init/tick/shutdown contracts);
   allocator strategy unification; error/logging policy; asset pipeline
   throughput; build-time reduction; dead-code/dormant-path sweep; gate
   suite consolidation (validator modes as a versioned contract). Exit
   criterion: a new game module can be built against the engine using only
   public headers + data, proven by the iteration-7 photography loop doing
   exactly that.

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
