# Pillar audit: Instinct / AI engine — utility AI, needs, perception, ecology (Spec 021, 2026-07-02)

**Verdict.** The Instinct pillar's substrate is remarkably complete and deterministically disciplined — a
libm-free IAUS arbiter, a live per-tick creature brain with fixed-point boids (alignment ON), scent
stigmergy with wind advection, Deneubourg double-bridge foraging, heritable sensory genes expressed into
perception, sexual reproduction with selection, and a full daily-life loop (thirst, circadian sleep,
scavenging) — all wired into `GameSession::TickSimulation` and guarded by the `PopulatedWorldReplay`
determinism gate and an `EcologyTickPerf` gate measured to 4k creatures. Spec 005 is **complete**; spec
011 is roughly two-thirds landed. The honest gap between the tree and the owner's emergent vision is not
missing systems but missing **live participants and integration**: the graze→deplete→regrow loop, the
alarm/pack/migration/territory/mortality systems, and vertebrate scent tracking all exist and are tested
but have zero participants in normal play (only timelapse demos and the gate roster stamp them), and the
engine still runs **two disjoint AI stacks** (GOAP `InstinctPlanner` for replicated server NPCs vs IAUS
`CreatureBrain` for ambient wildlife) that do not share perception, scent, or locomotion. Nothing in this
pillar shipped since the 2026-06-28 roadmap (last pillar commit `ba7bac3a`, 2026-06-26).

## Current state + evidence

### The decision core (IAUS, live and deterministic)

- **IAUS arbiter** — `src/luminumbra_common/ai/UtilityAI.h:81-92` (`SelectAction`, canonical
  lowest-id tie-break); response curves are libm-free by construction (`UtilityAI.h:39-55`, the
  "logistic" axis is a smoothstep, not `exp`). NaN-safe clamp at `UtilityAI.h:24`.
- **Creature brain** — `src/luminumbra_common/ai/CreatureBrain.h:34-41`: the action set is
  `{Wander, Graze, Flee, Hunt, Rest, Sleep}`. Sleep is circadian-gated and weighted just under Flee
  (`CreatureBrain.h:73-76`); `Drink` and `Forage` (spec 011 FR-B4/FR-F1) are **not** in the enum.
- **Live brain tick** — `src/luminumbra_common/ai/CreatureBrainSystem.h:92-342`: id-ordered, pre-tick
  snapshot, per-role spatial-grid herd gather (`CreatureBrainSystem.h:127-140`), predator catch + prey
  carcass (`:207-211`), Graze sates hunger (`:255-257`), full data-driven tuning via `EcologyTuning`
  (`:71-88`, defaults byte-identical, fed from SystemConfig `sim.ecology`).
- **Boids with alignment ON** — `CreatureBrainSystem.h:48` (`kAlignmentWeight = 0.5`, tuned on);
  the reduction is int64 fixed-point so it is order-independent (`src/luminumbra_common/ai/Flocking.h:48`,
  alignment term `Flocking.h:117-128`). Spec 005 FR-1 is done (commit `26d011d1`).
- **Wiring** — the whole pipeline runs in `src/luminumbra_common/world/GameSession.cpp`: perception
  `:201`, scent deposit + foraging + wind-advected field step (slot 2c) `:206-237`, instinct locomotion +
  scent steering (slot 2d) `:242-247`, creature brain (slot 2e) `:252-256`, mate seeking `:263`, steering
  consumer `:269`, thirst/scavenging + wish blend `:278-300`, Jolt physics bridge `:310-332`, mating
  resolve `:341-345`, wildlife-foliage grazing `:516-517`, lifespan `:519`, herd alarm `:524`,
  decomposition `:527`, circadian `:533`, territory `:537`, predator pack `:540`, migration `:546`.

### Perception, scent, foraging, evolution (the emergent-vision substrate)

- **Vision cones + hearing audiogram** — `src/luminumbra_common/ai/PerceptionSystem.h:35-75` (opt-in
  via `PerceptionComponent`+`AwarenessComponent`, id-ordered, world_hash-neutral).
- **Genome-gated targeting (spec 005 FR-5 follow-up, landed `8af4642f`)** — the brain's target scan is
  filtered by heritable hearing range and a vision cone faced along the prior-tick heading
  (`CreatureBrainSystem.h:163-199`); genome-less creatures keep the unfiltered scan (byte-identical).
- **Scent stigmergy + wind (FR-2, tuned ON)** — `src/luminumbra_common/ai/ScentField.h:78-97`
  (semi-Lagrangian upwind backtrace in `Step`), Weber-law `GradientSteer` (`ScentField.h:184-198`);
  advection strength `kScentWindAdvectionScale = 1.0` at `GameSession.cpp:88-93`, prior-tick wind
  sampled at `GameSession.cpp:226-235` to keep slot order stable.
- **Ant foraging (FR-3, landed `c02cede7`)** — `src/luminumbra_common/ai/ForagingSystem.h:51-153`:
  deposit-on-both-trips double trails (channels 2/3), fixed 4-neighbour scan, first-wins tie-break,
  grid clamp; live colony activated in the client world (below).
- **Heritable sensory genes (FR-4, landed `4e838d53`)** — `src/luminumbra_common/ai/CreatureGenome.h:47-49`
  (`vision_cos_half_fov`/`vision_range`/`hearing_range`), bred by draws taken AFTER the core breed + sex
  draw (`CreatureGenome.h:131-141`) and **expressed into a `PerceptionComponent` at birth**
  (`src/luminumbra_common/ai/CreatureReproductionSystem.h:264-267`). Divergence telemetry:
  `src/luminumbra_common/ai/CreatureTelemetry.h:26-45` (`ComputeSensoryMeans`), fixture
  `test/ai/creature_divergence_test.cpp`.
- **Reproduction + selection** — female-driven courtship, blend-crossover + Gaussian mutation, RNG
  seeded from offset 16 + parent ids + tick (`CreatureReproductionSystem.h:47`, `:203-207`); offspring
  inherit the mother's `species_id` (`:223`). **Note:** the live call passes `/*world_seed*/ 0ull`
  (`GameSession.cpp:341-342`), so offspring genomes do not vary by world seed (INSTINCT-11).
- **Life cycle** — `src/luminumbra_common/ai/LifespanSystem.h:57-58` (starvation death at hunger ≥ 1.0
  plus old-age death, opt-in `MortalComponent`); decomposition, scavenging, herd alarm, packs,
  migration, territory, circadian all exist as separate systems under `src/luminumbra_common/ai/`
  (36 headers) with matching tests in `test/ai/` (26 files) and `test/sim/` (43 files).
- **Grazing↔foliage coupling** — `src/luminumbra_common/ai/WildlifeFoliageSystem.h:11-16` (herbivores
  deplete `GrazeableComponent` biomass, plants regrow, grazing feeds the creature), wired at
  `GameSession.cpp:516-517`, opt-in gated (`WildlifeFoliageSystem.h:30-33`).

### Species identity + the living world (polish-roadmap Phase 0/1, spec 011 activation)

- **Species keystone** — `CreatureComponent.species_id` (`src/luminumbra_common/components/CreatureComponents.h:37`)
  keyed by FNV-1a (`CreatureComponents.h:22`); data-driven registry
  `src/luminumbra_common/ai/CreatureSpeciesRegistry.h:33-56` over 10 species JSONs
  (`data/common/creatures/species/ridgeback_stalker.json:1-17` et al: predator/nocturnal/rarity/
  base_color/biomes). Commits `10a0d9d0` (species identity), `4bd27b9b` (discovery moment),
  `e51b2c29` (biome-appropriate fauna).
- **Ambient living world (client)** — `src/luminumbra_client/main_client.cpp:6116-6217`: 12
  skinned, biome-selected, species-varied creatures with genome, thirst (`:6195-6196`), predator
  scavenging (`:6197-6198`), per-species circadian sleep (`:6199-6205`), Jolt avatars (`:6211-6215`),
  plus capped `WaterHoleComponent` drinking spots (`:6141-6147`).
- **Live forager colony (spec 011 Phase C, `7d92fd37`)** — nest + 24 ants + short/long food sources at
  `main_client.cpp:6221-6267`; ants render as markers (`main_client.cpp:714-723`) and the pheromone
  trails tint the albedo as a ground overlay (`src/luminumbra_client/rendering/RenderPipeline.cpp:2261`).
- **Sleep made visible/audible (Phase G partial)** — rest/sleep poses (`main_client.cpp:699-705`),
  sleeping-breath 3D audio (`:4211-4224`), colony chitter bed near an active nest (`:4262-4272`).
- **Determinism gates** — `PopulatedWorldReplay` (populated run==replay + pinned golden + non-vacuity:
  `tools/gates/validate-engine-frontier.ps1:4624-4712`) over the kinematic gate roster
  (`src/luminumbra_server/ServerWorldRunner.cpp:128-163`, which stamps the FULL deep-ecology component
  set: genome/alarm/mortal/decay/migratory/territory/pack); `EcologyTickPerf` measures median+p99 ms/tick
  at N ∈ {256, 1k, 4k} (`validate-engine-frontier.ps1:3012-3035`); the ecology sub-hash is the 6th
  world_hash term (`ServerWorldRunner.cpp:102-117`, `src/luminumbra_common/ai/EcologyHash.h:49-88`).

### Shipped since the 2026-06-28 roadmap

**Nothing in this pillar.** `git log --since=2026-06-27 -- src/luminumbra_common/ai test/ai ...` is
empty; the last Instinct commits are the 2026-06-26 config-drive block (`ba7bac3a`, `84b949ef`,
`dc778001`, `5b9401be`, `b593be84`). The roadmap's shipped-since items (017-A ring `ca2616d8`/`3bba2a52`,
moon channel `3aa9740d`, lake-preview fix `e1fee9ff`) do not touch this pillar.

**KNOWN-CONTEXT corrections (memory vs tree — the tree wins):**
- Memory said "ecology Batch A in progress; FR-2/3/4/5 next". Superseded: **all of spec 005 landed**
  (FR-2+alignment `26d011d1` 2026-06-25 "complete spec 005"; FR-3 `c02cede7`; FR-4 `4e838d53`;
  FR-5 `ea0f95bf` + the perception-gated targeting follow-up `8af4642f`). The spec header itself says
  VALIDATED (`docs/specs/005-emergent-ecology-ai/spec.md:3`).
- Memory said "scent deposit/sense was next". Landed: `GameSession.cpp:215` (deposit), `:244` (steer).
- The spec 011 header still says "**Next:** forager marker rendering"
  (`docs/specs/011-living-creatures-daily-life/spec.md:33`); that landed (`main_client.cpp:714-723`)
  — the spec doc is stale on this point.
- Memory said brain→tick+locomotion integration was open. Done: `GameSession.cpp:255` (brain in the
  tick) + the Jolt bridge (`GameSession.cpp:310-332`).

## Gaps / debt

1. **Feeding never touches the world (spec 011 Phase B).** The brain's Graze hard-codes
   `food_proximity = 0.6` (`CreatureBrainSystem.h:226`) and sates hunger without depleting anything
   (`:255-257`). The built graze→deplete→regrow system is fully dormant: **no code anywhere in `src/`
   emplaces `GrazeableComponent`** (the only registry reference is the gating view at
   `GameSession.cpp:516`), and `FoodSourceComponent` is only stamped for the ant colony
   (`main_client.cpp:6248-6254`).
2. **The arbiter is not the full needs arbiter (spec 011 Phase F).** Thirst and scavenging steer by
   additively blending their own wish into `CreatureComponent.wish` outside the IAUS decision
   (`GameSession.cpp:281-300`), so a creature can "decide" Flee while simultaneously being pulled to
   water; `Drink`/`Forage` are absent from `CreatureAction` (`CreatureBrain.h:34-41`).
3. **Needs have no consequences in live play (spec 011 FR-A3).** Starvation death exists
   (`LifespanSystem.h:57-58`) but there is no pre-death degradation (speed/perception), and the ambient
   spawn stamps **no** `MortalComponent`/`DecayComponent` (`main_client.cpp:6182-6215`) — ambient
   creatures cannot starve or age out, and eaten carcasses persist forever (decomposition has no
   participant).
4. **Deep-ecology systems have zero live participants.** `AlarmComponent`, `PackHunterComponent`,
   `MortalComponent`, `DecayComponent` are stamped only in the timelapse demo spawns
   (`main_client.cpp:6361`, `:6375`, `:6390-6395`) and the gate roster
   (`ServerWorldRunner.cpp:141-159`); `MigratoryComponent`/`TerritoryComponent` only in the gate roster
   (`:157-159`). In normal play there is no herd alarm, no pack flanking, no migration, no territory.
5. **No live vertebrate scent participants.** The ambient spawn stamps neither a scent-depositing
   `SensableComponent` nor a `ScentSenseComponent` (`main_client.cpp:6182-6215`;
   `GameSession::HasScentParticipants`, `GameSession.cpp:1077-1103`), so the wind-advected
   hunt-upwind loop — a core owner emergent-vision piece — never runs in play. Worse, scent steering
   structurally requires the *other* stack's components (`LocomotionIntentComponent` +
   `LocomotionProfile`, `src/luminumbra_common/ai/ScentSteeringSystem.h:56-57`), so it **cannot** bias
   `CreatureBrain` creatures at all today; only ants use the field (channels 2/3).
6. **Two disjoint AI stacks.** Replicated server NPC animals run the GOAP `InstinctPlanner` stack
   (`src/luminumbra_server/main_server.cpp:2045-2056`; validated by ReplicationSmoke's "GOAP NPCs
   approached water" assertion, `validate-engine-frontier.ps1:4898`) while ambient wildlife runs the
   IAUS `CreatureBrain` stack; they share neither perception (`PerceptionSystem` fusion vs the brain's
   inline genome-gated scan, `CreatureBrainSystem.h:163-199`) nor scent nor locomotion.
7. **The ecology sub-hash under-covers new state.** `EcologyHash.h:70-84` folds position/wish/hunger/
   stamina/eaten + genome(move_speed, generation, age_ticks) + alarm + pack — but **not** energy,
   circadian activity, thirst, sensory genes, or species_id. A silent divergence in any of those is
   invisible to `PopulatedWorldReplay` until it flips an action. (The FR-4 design comment acknowledges
   this: `CreatureReproductionSystem.h:210-212`.)
8. **Reproduction RNG ignores the world seed** — `GameSession.cpp:341-342` passes
   `/*world_seed*/ 0ull`, so two different worlds with identical entity ids/ticks breed byte-identical
   offspring. Deterministic, but seed-invariant — contrary to the "pure function of (seed, preset)"
   posture the roster itself follows (`ServerWorldRunner.cpp:119-127`).
9. **Species data is presentation-only.** The species JSON schema carries predator/nocturnal/rarity/
   color/biomes (`CreatureSpeciesRegistry.h:33-56`) — no per-species IAUS curve tuning (the seam
   explicitly called out at `CreatureBrain.h:9-10`), no genome ranges, no per-species senses.
10. **Stale determinism-critical comments.** `CreatureBrainSystem.h:54-55` claims "Energy is NOT yet
    read by DecideCreatureAction" (it is: `CreatureBrainSystem.h:217`, `CreatureBrain.h:75`) and
    `CreatureBrainSystem.h:302-304` claims "alignment_weight default 0" (it is 0.5 at `:48`). Both
    mislead future determinism/re-pin work. The spec 011 header's "Next" list is likewise stale
    (`docs/specs/011-living-creatures-daily-life/spec.md:33-35`).

## Risks

- **Hash-move clustering.** Items 1-3, 5, 7, 8 each move the `PopulatedWorldReplay` pinned golden
  (`validate-engine-frontier.ps1:4702-4707`). Landing them piecemeal means repeated re-pins; landing
  them carelessly means an undiagnosable populated-hash break. Mitigation: batch re-pins per the spec
  011 NFR-1 discipline (`docs/specs/011-living-creatures-daily-life/spec.md:211-214`), one deliberate
  transition per commit (TDD-LOCK scenario, `test/features/TDD-LOCK.md:28-32`). The canonical
  empty-roster `--smoke` (6f008a9f637c40b7) is unaffected by all of them (empty-neutral opt-ins).
- **Option A client/server fork.** Ambient creatures are client-only by design
  (`docs/specs/011-living-creatures-daily-life/spec.md:36`); every new live-world behaviour must keep
  the server roster's determinism separate (no client-only wish writers on replicated entities,
  FR-D2 `spec.md:165`). The dual-stack unification (gap 6) is the highest-risk item because it
  touches the replicated NPC path.
- **O(N²) opposite-role scan at scale.** The brain's catch-target scan is O(N) per creature
  (`CreatureBrainSystem.h:175-200`), deliberately kept for global-nearest semantics
  (`docs/specs/005-emergent-ecology-ai/spec.md:63-72`). Fine at 20-48 agents; `EcologyTickPerf`
  already measures 4k — but its budgets are not yet enforced ("fail-until-baselined" posture,
  `validate-engine-frontier.ps1:3016-3021`), so a regression would pass green today.
- **Perf-budget blind spot.** Same as above: until the `ecology_tick` budgets in
  `perf-floor-release.json` are blessed, the ecology tick has measurement but no enforcement.

## Opportunities

- **Cheap, high-visibility activation wins.** Most of the owner's emergent vision is one component
  stamp away: alarm/pack/mortal/decay on the ambient spawn (gap 4) makes herds bolt together, packs
  flank, populations self-bound — all already tested (`test/sim/herd_alarm_test.cpp`,
  `test/sim/predator_pack_test.cpp`, `test/sim/lifespan_test.cpp`, `test/sim/decomposition_test.cpp`).
- **Scent hunting as the flagship emergent behaviour.** The full chain (deposit → wind drift →
  gradient steer) exists and is byte-stable; giving prey a scent deposit and predators a scent sense
  on the brain path (gap 5) delivers the "predator tracks prey up-wind" acceptance the owner asked
  for (`docs/specs/005-emergent-ecology-ai/spec.md:90`).
- **The needs arbiter completes the daily-life story.** Folding thirst/energy/food into IAUS
  considerations (gap 2) plus feeding-with-consequences (gaps 1, 3) closes spec 011's remaining
  acceptance criteria (AC-1/2/4/5/6, `docs/specs/011-living-creatures-daily-life/spec.md:231-246`).
- **Per-species IAUS tuning** (gap 9) turns the 10 species from recolors into behaviourally distinct
  animals using the already-landed `EcologyTuning`/SystemConfig pattern — defaults byte-identical, so
  zero re-pin until a species opts in.
- **Ecology hash v2** (gap 7) hardens every future item in this pillar: divergence in energy/
  circadian/sensory state becomes immediately localizable via the gate's per-section sub-hash diff
  (`validate-engine-frontier.ps1:4669-4676`).

## Backlog items

| id | summary | spec | effort | risk | deps | status | proving_signal |
|---|---|---|---|---|---|---|---|
| INSTINCT-01 | Spec 005 emergent-ecology completion: fixed-point boids alignment ON, scent wind-advection ON, ant double-bridge foraging, heritable sensory genes expressed into PerceptionComponent, perception-gated targeting | 005 | L | low | [] | done | `PopulatedWorldReplay` gate (validate-engine-frontier.ps1 -Mode PopulatedWorldReplay) + `foraging_test` + `creature_divergence_test` |
| INSTINCT-02 | Spec 011 Phases A/C/E/G-partial: energy need, circadian-gated Sleep, live forager colony + trail render, rest poses + sleep/colony audio, full sim.ecology config-drive | 011 | L | low | [] | done | `circadian_test` + `thirst_test` + `foraging_test`; PopulatedWorldReplay stayed run==replay (byte-identical opt-ins) |
| INSTINCT-03 | Species identity keystone: real species_id (FNV-16) + data-driven 10-species registry + biome-appropriate ambient fauna, feeding the discovery/codex loop | new | M | low | [] | done | `species_identity_test` + `creature_species_registry_test` + `codex_view_test` |
| INSTINCT-04 | Activate the feeding loop: place GrazeableComponent on scatter/sim plants + FoodSourceComponents for large creatures so Graze actually depletes and WildlifeFoliageSystem runs live (today zero emplacements in src/) | 011 | M | medium | [] | todo | NEW: `EcologyPipeline.GrazeDepletesLiveBiomass` ctest — a grazing herd measurably draws down GrazeableComponent biomass and hunger falls; PopulatedWorldReplay re-pinned once with the extended roster |
| INSTINCT-05 | Complete the IAUS needs arbiter: add Drink + Forage actions to CreatureAction and fold thirst/energy/food-availability considerations into DecideCreatureAction, replacing the out-of-band additive wish blend | 011 | M | medium | ["INSTINCT-04"] | todo | `creature_brain_test` (new action-selection fixtures) + PopulatedWorldReplay re-pin (one deliberate hash transition) |
| INSTINCT-06 | Needs consequences (FR-A3): starvation/exhaustion degrade move_speed and perception before death; stamp MortalComponent + DecayComponent on the ambient living-world spawn so creatures age, starve, die, and decompose in normal play | 011 | M | medium | [] | todo | `lifespan_test` + NEW: `EcologyPipeline.StarvationDegradesThenKills` ctest — sustained max hunger reduces effective speed then marks dead; carcass decays |
| INSTINCT-07 | Stamp the deep-ecology components (Alarm, PackHunter, Migratory, Territory) on the ambient living-world spawn — herd alarm, pack flanking, migration, territory currently have zero participants outside timelapse demos and the gate roster | 011 | S | low | [] | todo | `herd_alarm_test` + `predator_pack_test` green; NEW: living-world spawn assertion in `ecology_demo_test` that ambient creatures carry the alarm/pack set (client-only, --smoke unchanged) |
| INSTINCT-08 | Live vertebrate scent tracking: prey deposit scent (SensableComponent), predators sense it on the CreatureBrain path (today ScentSteering requires the GOAP stack's LocomotionIntent/Profile and cannot bias brain creatures) — the hunt-upwind emergent loop | 005 | M | medium | [] | todo | NEW: `ScentHunt.PredatorTracksPreyUpwind` ctest — with wind ON, a predator outside direct perception closes on prey along the advected gradient; PopulatedWorldReplay re-pinned (scents sub-hash becomes non-empty) |
| INSTINCT-09 | Unify the two AI stacks: one perception substrate (PerceptionSystem fusion) feeding both the GOAP InstinctPlanner (replicated NPCs) and the IAUS CreatureBrain (ambient wildlife); retire the brain's inline genome-gated scan duplicate | new | L | high | ["INSTINCT-08"] | todo | `perception_system_test` + `creature_brain_system_test` + PopulatedWorldReplay run==replay + `ReplicationSmoke` (its GOAP-NPCs-approach-water assertion must stay green) |
| INSTINCT-10 | Ecology sub-hash v2: fold energy, circadian activity, thirst, sensory genes, and species_id into ComputeEcologySubHash so divergence in the new state is oracle-visible (currently unhashed); empty roster stays neutral | new | M | medium | [] | todo | `ecology_hash_test` (field-coverage assertions) + PopulatedWorldReplay golden re-pinned once + `--smoke` 6f008a9f637c40b7 unchanged (empty-neutral) |
| INSTINCT-11 | Wire the real world seed into RunMatingResolveOnTick (GameSession passes /*world_seed*/ 0ull) so offspring genetics vary per world; batch its re-pin with INSTINCT-10 | new | S | medium | ["INSTINCT-10"] | todo | `creature_reproduction_test` (same-seed reproducibility + cross-seed divergence case) + PopulatedWorldReplay re-pin |
| INSTINCT-12 | Per-species behaviour data: extend the species JSON schema with IAUS curve/tuning overrides and genome ranges (the seam CreatureBrain.h names), defaults byte-identical via the EcologyTuning pattern | new | M | low | ["INSTINCT-03"] | todo | `creature_species_registry_test` (schema round-trip) + `creature_brain_test` (default-species byte-identical) + PopulatedWorldReplay unchanged until a species opts in |
| INSTINCT-13 | Fix stale determinism-critical comments: CreatureBrainSystem.h:54-55 ("energy NOT yet read" — it is) and :302-304 ("alignment_weight default 0" — it is 0.5); refresh the spec 011 header's stale "Next" list | 011 | S | low | [] | todo | `creature_brain_system_test` green (doc-only change, zero behaviour delta) |
| INSTINCT-14 | Server-authoritative nests/homes + return-home steering (spec 011 FR-D2 Option-A deferral): promote nest anchors to replicated sim state for the multiplayer sprint | 011 | L | high | ["019"] | todo | `NetworkedReplication` gate (host+client over TCP mirror the nest-steered roster) + PopulatedWorldReplay re-pin |
| INSTINCT-15 | Bless the ecology_tick perf budgets: EcologyTickPerf measures median/p99 at N∈{256,1k,4k} but enforces nothing until perf-floor-release.json's ecology_tick block is baselined; includes deciding the O(N²) opposite-role scan's fate at 4k | new | S | low | [] | todo | `validate-engine-frontier.ps1 -Mode EcologyTickPerf` (enforced budgets RED on regression after blessing) |
