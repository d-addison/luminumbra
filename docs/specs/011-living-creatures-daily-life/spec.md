# Spec 011: Living Creatures — Needs, Foraging Colonies & Daily Life

> Status: DRAFT (created 2026-06-25). Builds on spec 005 (emergent ecology AI),
> spec 006 (foliage/farming — the plants creatures eat), and the existing but
> dormant foraging substrate. Companion to the standing
> `docs/AUDIO-everything-maps-to-sound.md` rule.

## Context

The creature simulation has a strong skeleton but most of the *life* is dormant or
unwired:

- **Needs exist but aren't fully arbitrated.** `CreatureComponent` carries `hunger`
  (0 sated…1 starving) and `stamina` (0 exhausted…1 fresh). **Thirst** is a whole
  system already (`ai/ThirstSystem.h` + `components/ThirstComponents.h`). But the
  brain only knows 5 actions and there is **no energy/fatigue need** and **no link
  from circadian state to behaviour**.
- **The IAUS brain is shallow.** `ai/CreatureBrain.h` `CreatureAction` = {Wander,
  Graze, Flee, Hunt, Rest}. There is no Forage, no Drink, no Sleep. `DecideCreatureAction`
  is a clean Infinite-Axis-Utility-System arbiter — the right place to add needs.
- **Circadian is half-built.** `components/CircadianComponents.h` `CircadianComponent`
  (diurnal vs `nocturnal`) + its system scale a creature's activity by the day phase,
  but **`CreatureBrain` never reads it** — creatures never actually sleep.
- **Foraging is fully built and 100% dormant.** `ai/ForagingSystem.h` implements the
  Deneubourg double-bridge (shorter pheromone path wins), `components/ForagingComponents.h`
  defines `ForagerComponent` (home/nest cell + `carrying_food`) and `FoodSourceComponent`
  (grid cell + depletable `amount`), and `GameSession` ticks `RunForagingOnTick` at slot
  2c — but **nothing anywhere stamps `ForagerComponent` or `FoodSourceComponent`**, so the
  ant-trail behaviour never runs in a live world.
- **No nests/homes for normal creatures.** Only the forager's `home_x/home_z` cell models
  a home today. Creatures spawn, wander, and never return anywhere.

This spec turns that skeleton into a **needs-driven daily-life loop**: creatures get
hungry, thirsty, and tired; they eat (graze / hunt / forage), drink at water, and **sleep
at nests** on a real day/night cycle; **forager colonies** lay visible pheromone trails
between nest and food, and **large creatures also forage** opportunistically; and **night
visibly changes the world** — most diurnal creatures bed down, the world quiets, dawn
stirs it awake — with a sound for every new action (per the standing audio rule).

Scope decisions confirmed with the owner (2026-06-25): foraging = **colony insects AND
large creatures**; needs = **full arbiter** (hunger+thirst+energy+circadian); atmosphere =
**full** (sleep visuals + night quiet + per-action audio).

## Goals

- A single **needs substrate** (hunger, thirst, energy) that all decay/recover
  deterministically and feed one IAUS arbiter.
- **Eating actually works**: grazing depletes a food source / plant and reduces hunger;
  predation reduces hunger; foraging delivers food to a nest. Hunger has consequences.
- **Forager colonies are alive**: a colony species nests, sends foragers out, and **visible
  pheromone trails** converge on the shortest nest→food path (the double-bridge, in play).
- **Large creatures forage too**: opportunistic carry-food-home behaviour layered on the
  existing species, distinct from colony swarms.
- **Nests/homes** for creatures: a home anchor placed at spawn; creatures return to it to
  rest and sleep; colonies nest as a unit.
- **Sleep**: a Sleep action gated by circadian phase + energy; sleeping recovers energy,
  dampens perception, and increases vulnerability. Diurnal sleep at night, nocturnal by day.
- **Daily routine emerges**: dawn wake → forage/graze/drink by day → return to nest →
  sleep at dusk (inverted for nocturnal), all from utility arbitration, not scripting.
- **Full night atmosphere**: most creatures asleep at night, the world quiets, dawn stirs
  it; feeding / drinking / sleeping / colony activity each have a wired sound and visible rest.
- **Determinism preserved**: every new sim term is integer/fixed-point, default-OFF until
  participants exist, empty-roster-neutral, folded into `world_hash` via a `Compute<X>SubHash`
  with a re-pin, run==replay byte-exact.

## Non-Goals

- No new **rendering pipeline** work beyond rest poses + a sleep/feeding particle or marker
  (reuse existing creature draw + animation hooks; no skeletal-rig overhaul).
- No **player-facing taming/feeding** mechanic — this is ambient world life, not a pet system.
- No **multi-tile colony construction** (tunnels, chambers). A nest is a point anchor with a
  radius, not a built structure (defer to a future spec if desired).
- No change to the **finite-hydrology** water model (spec 010); Drink consumes no water mass,
  it only reads `WaterLevelAt` to find a drinking spot.
- No **weather-driven** behaviour changes (sheltering from rain) — possible follow-on.
- Not a perf spec: targets are "stays within the existing ecology tick budget at scale,"
  not a new perf frontier.

## Functional Requirements

### A. Needs substrate (SIM — new sub-hash)

- **FR-A1 (energy need).** Add an `energy` field (0 exhausted…1 rested) to the creature
  needs. Decays each tick while active (faster when fleeing/hunting/foraging), recovers
  while Rest/Sleep. Mirror the existing `hunger`/`stamina` integer-fixed-point treatment;
  do **not** reuse `stamina` (stamina = short-term exertion; energy = long-term sleep need).
  Decide at design time whether energy lives on `CreatureComponent` or a new
  `CreatureNeedsComponent` (see Open Questions).
- **FR-A2 (unified decay).** Hunger rises, thirst rises (existing `ThirstSystem`), energy
  falls — all deterministic, fixed-point, per-tick, scaled by genome where a gene exists
  (e.g. metabolism). No wall-clock.
- **FR-A3 (consequences).** Sustained max hunger or zero energy degrades the creature
  (reduced `move_speed` / perception, eventually contributes to death) — bounded and
  deterministic. Starvation already partially modeled via reproduction gating; extend it.
- **FR-A4 (telemetry).** Needs are observable for tests/codex (per-creature snapshot)
  without breaking determinism (read-only).

### B. Food & feeding (SIM)

- **FR-B1 (food sources).** Place `FoodSourceComponent`s in the world deterministically:
  herbivore forage tied to **foliage** (graze near `PlantTag`/scatter plants), and colony
  forage as standalone food piles. Sources deplete (`amount`) and regenerate slowly.
- **FR-B2 (grazing feeds).** `CreatureAction::Graze` must actually **reduce hunger** and
  **deplete** the nearest food source/plant it grazes (today Graze only steers). Tie graze
  success to proximity + source amount.
- **FR-B3 (predation feeds).** A successful Hunt that sets a prey `eaten` must **reduce the
  predator's hunger** (carcass = food); scavengers may feed on existing carcasses.
- **FR-B4 (drink action).** Add `CreatureAction::Drink`: when thirsty, steer to the nearest
  standing water (`WorldSystem::WaterLevelAt` ring-probe, the same the audio uses) and reduce
  thirst on arrival. Wire the existing `ThirstSystem` need into this action.

### C. Foraging — colonies AND large creatures (SIM — activates the dormant system)

- **FR-C1 (colony archetype).** Add a colony forager species (data-driven, e.g.
  `data/common/creatures/species/*.json` with a `forager`/`colony` role) that spawns a
  **nest** + a population of small foragers carrying `ForagerComponent` (home = nest cell).
- **FR-C2 (live trails).** With colony foragers + `FoodSourceComponent`s present, the
  existing `RunForagingOnTick` must produce the **double-bridge result in play**: trails
  strengthen on the shorter nest→food path, foragers alternate outbound/laden, `deliveries`
  accrues. Validate with a live (not just unit) world.
- **FR-C3 (large-creature foraging).** Layer opportunistic foraging on existing creatures:
  when hungry and a food source is in range, a large creature may carry food toward its
  home/nest (FR-D) and feed there. Distinct from swarm trails (no dense pheromone road);
  reuses the needs arbiter, not necessarily the ant pheromone channel.
- **FR-C4 (scent integration).** Colony trails use the existing `ScentField` channels
  (deposit on the laden path, follow on the outbound) — confirm the deposit/sense wiring is
  active for foragers and order-invariant.

### D. Nests & homes (SIM)

- **FR-D1 (home anchor).** Every creature that sleeps/returns gets a deterministic **home
  anchor** (a world cell), assigned at spawn (near the spawn point / biome-appropriate).
  Colonies share one nest; solitary creatures each get a home.
- **FR-D2 (return-home).** A `ReturnHome` steering target (or a Sleep precondition) routes a
  creature back toward its anchor when it intends to rest/sleep, reusing the existing
  locomotion/pathing (A*-grid fallback already present).
- **FR-D3 (nest as origin).** The nest is the forage origin (colony) and the sleep site
  (all). Nests are deterministic, bounded in number, and empty-roster-neutral.

### E. Sleep & circadian (SIM)

- **FR-E1 (Sleep action).** Add `CreatureAction::Sleep`. Precondition: at/near home anchor
  AND in the creature's inactive circadian phase AND low energy. While sleeping: `wish`
  velocity = 0, energy recovers fastest, perception range is **dampened** (more vulnerable).
- **FR-E2 (circadian gates the brain).** `DecideCreatureAction` reads the `CircadianComponent`
  day phase: Sleep utility is high in the inactive phase, near-zero in the active phase;
  diurnal creatures sleep at night, nocturnal by day. This is the missing brain↔circadian link.
- **FR-E3 (wake/stir).** Dawn (active-phase onset) suppresses Sleep utility → creatures wake
  and resume needs-driven behaviour. Transitions are smooth (hysteresis), not a hard snap.
- **FR-E4 (vulnerability).** A sleeping prey is easier to catch (predator Hunt utility up,
  prey perception down) — emergent risk to sleeping in the open vs at a nest.

### F. Brain integration — the full needs-arbiter (SIM)

- **FR-F1 (arbiter).** `DecideCreatureAction` weighs all needs into the IAUS action set:
  Wander, Graze, **Forage**, Hunt, Flee, **Drink**, Rest, **Sleep**. Highest-need wins
  (hungry→eat/forage, thirsty→drink, tired+night→sleep, threatened→flee always trumps).
- **FR-F2 (order-invariance preserved).** All new considerations keep the brain a pure
  function of sensed state (no iteration-order dependence); the grid-gather path stays
  run==replay (the property spec 005 FR-1 established).
- **FR-F3 (genome hooks).** Where sensible, gate need weights on existing/added genes
  (metabolism, vigilance, boldness) so daily routines diverge under selection — reuse the
  `CreatureGenomeComponent` inheritance path (draws AFTER core breed, like FR-4 senses).

### G. Atmosphere — night/sleep made felt (RENDER/UI/AUDIO)

- **FR-G1 (rest poses).** Sleeping/resting creatures show a visibly different pose/state
  (crouched/still, eyes-closed marker, or a small "Zzz"/breathing marker) — reuse existing
  creature animation/draw hooks; no new rig.
- **FR-G2 (night quiet).** With most diurnal creatures asleep, the world is audibly/visually
  calmer at night; dawn stirs it (the ambient/music dusk↔dawn swap + the `time_dawn`/
  `time_dusk` cues already exist — hook creature wake/sleep to that moment).
- **FR-G3 (per-action audio — everything maps to sound).** Wire a sound for each NEW
  perceivable action: feeding/grazing (soft chew), drinking (lap/sip), sleeping (slow
  breathing / soft snore, 3D at the nest), colony activity (a faint chittering bed near an
  active nest). Generate via the `tools/audio` pipeline (mp3), add to `sfx_main`, follow the
  audit doc. Update `docs/AUDIO-everything-maps-to-sound.md`.

## Non-Functional Requirements

- **NFR-1 (determinism, sacred).** Every sim term integer/fixed-point; new state folded into
  `world_hash` via a `Compute<X>SubHash` appended to `ComposeWorldHash` (append-only,
  empty-neutral); `--smoke` run==replay byte-exact before re-pinning the baseline literals;
  ≤2 ordered writes per sub-hash. Re-pin once per workstream that moves the hash.
- **NFR-2 (seed-offset registry).** This spec claims the next sequential offsets after +37
  (germination/season). Reserve and document, e.g. **+38 needs, +39 food placement, +40
  forager/colony spawn, +41 nest placement, +42 sleep** (re-grep `*SeedOffset` immediately
  before claiming; append-only, never reuse +1..+37).
- **NFR-3 (empty-roster neutrality).** With no creatures / no foragers / no food sources,
  every new sub-hash returns the neutral value and all-off baselines stay byte-identical
  (the established opt-in-component pattern).
- **NFR-4 (perf/scale).** New per-tick work stays within the existing ecology tick budget at
  the standard roster and scales to colony swarms (bounded forager counts; the foraging grid
  is already coarse). No new main-thread stalls; validate with the moving-frame profiler.
- **NFR-5 (audio).** No `.ogg` (miniaudio has no Vorbis decoder); 3D one-shots use the
  reaped `PlayOneShot` path (the use-after-free is fixed); listener follows the player.

## Acceptance Criteria

- [ ] **AC-1 (needs loop).** A headless world shows creatures' hunger/thirst/energy cycling:
      a hungry creature seeks food and hunger drops after grazing; a thirsty one drinks;
      a tired one sleeps and energy recovers. Asserted via a needs fixture + telemetry.
- [ ] **AC-2 (eating works).** Grazing measurably reduces hunger AND depletes a food
      source; a successful hunt reduces the predator's hunger. RED-first tests.
- [ ] **AC-3 (live ant trails).** In a live world with a colony + two food sources at
      unequal distances, the pheromone trail converges on the **shorter** path and
      `deliveries` accrues (the double-bridge, end-to-end, not just the unit test).
- [ ] **AC-4 (large-creature foraging).** A hungry large creature carries food toward its
      home and feeds — distinct from colony swarming.
- [ ] **AC-5 (sleep cycle).** Diurnal creatures sleep at night at/near their nest and wake
      at dawn; nocturnal inverted; sleeping recovers energy and dampens perception. Fixture
      over a full simulated day.
- [ ] **AC-6 (vulnerability).** A sleeping prey in the open is caught measurably more often
      than an awake one (emergent risk), without breaking determinism.
- [ ] **AC-7 (atmosphere).** At night most diurnal creatures are at rest (visible pose) and
      the world is calmer; dawn stirs them; feeding/drinking/sleeping/colony each emit their
      wired sound. Verified by a frame-scan + the bank loading the new events with no errors.
- [ ] **AC-8 (determinism).** `--smoke` run==replay byte-exact with the full system active;
      each workstream's sub-hash re-pinned once; all-off baselines byte-identical
      (empty-roster neutrality); ≤2 ordered writes per sub-hash.
- [ ] **AC-9 (no regressions).** `common_tests` green (existing flocking/scent/forag/herd/
      perception fixtures unchanged); the engine-frontier gates green after re-pin.

## Suggested phasing (each independently shippable, re-pin per phase)

1. **Phase A — Needs substrate + brain hooks** (energy need, unified decay, telemetry; no
   behaviour change yet beyond Rest). Smallest hash move.
2. **Phase B — Feeding** (graze/hunt feed + deplete; Drink action wired to `ThirstSystem`).
3. **Phase D — Nests/homes** (home anchor at spawn + ReturnHome target). Needed by sleep+foraging.
4. **Phase E/F — Sleep + full arbiter** (Sleep action, circadian gate, vulnerability; the
   complete IAUS arbiter).
5. **Phase C — Foraging colonies + large-creature foraging** (activate the dormant system:
   colony archetype + nest + food sources + live double-bridge; opportunistic large foraging).
6. **Phase G — Atmosphere** (rest poses, night quiet hook, per-action audio). Render/UI/audio
   only — no hash move.

## Open Questions

- **OQ-1.** Energy on `CreatureComponent` (cache-friendly, but grows the hot struct +
  re-pins the creature hash) vs a new opt-in `CreatureNeedsComponent` (empty-neutral, but a
  second lookup)? Lean opt-in component for determinism cleanliness.
- **OQ-2.** Colony foragers as **full entities** (one entt entity per ant — simple, reuses
  everything, but N can be large) vs an **aggregate colony** (one entity carries a forager
  population integer + the grid does the work)? Aggregate scales better; decide by perf.
- **OQ-3.** Do herbivore food sources **derive from foliage** (graze the actual `PlantTag`/
  scatter plants — ties ecology to farming) or are they **standalone** `FoodSourceComponent`
  piles? Deriving from foliage is richer but couples two sub-systems' hashes.
- **OQ-4.** Carcasses: does a hunted prey become a timed `FoodSourceComponent` (scavenger
  loop) or just vanish on `eaten`? Scavenging is a nice emergent layer but more state.
- **OQ-5.** Nest placement: purely procedural at spawn, or biome-weighted (colonies in
  meadows, dens near rock)? Procedural-first; biome-weight as a follow-on.
- **OQ-6.** How visible is sleep without a skeletal rig — a pose swap, a ground marker, or a
  particle? Pick the cheapest that reads clearly at gameplay distance.

## References (grounding — current code)

- Brain/arbiter: `src/luminumbra_common/ai/CreatureBrain.h` (`CreatureAction`,
  `DecideCreatureAction`), `ai/CreatureBrainSystem.h`.
- Needs: `components/CreatureComponents.h` (`hunger`/`stamina`), `ai/ThirstSystem.h` +
  `components/ThirstComponents.h`.
- Circadian: `components/CircadianComponents.h` (+ its system).
- Foraging (dormant): `ai/ForagingSystem.h` (`RunForagingOnTick`),
  `components/ForagingComponents.h` (`ForagerComponent`, `FoodSourceComponent`),
  `ai/ScentField.h`.
- Spawn/roster: `ai/CreatureSpeciesRegistry.h`, `data/common/creatures/species/*.json`,
  creature spawn in `world/GameSession.cpp` / the client spawn path.
- Determinism: `world/GameSession.cpp` (`ComputePlantSubHash`/tick slots),
  `ServerWorldRunner` `ComposeWorldHash`, `docs/STANDARDS.md` (seed-offset registry),
  `--smoke` oracle.
- Audio: `docs/AUDIO-everything-maps-to-sound.md`, `tools/audio/`, `data/audio/sfx_main.bank.json`.
