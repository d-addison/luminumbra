# Spec — Emergent Ecology: Senses · Stigmergy · Evolution

Status: DRAFT (research-gated). Owner direction: see memory `emergent-ecology-ai-direction`.
Branch: `feat/polyglot-audit-roadmap`. Engine-generic; all creature traits are game data.

## Context

Move creature AI from scripted behavior to **emergent ecology**: flocking/herding,
scent-trail stigmergy (ant foraging), predator/prey **hunting & tracking** by sense, and
**evolution** of traits so predators and prey diverge under selection. The whole system
must obey the determinism contract (fixed 30 Hz, run==replay, seeded PRNG only, no
wall-clock/libm-transcendentals on the tick path) and scale to the 20–48 networked agents
the multiplayer stress target already validates.

This spec was preceded by a cited research pass (deep-research workflow, 2026-06-18; ACO/
Dorigo-Stützle, Deneubourg double-bridge, MAX-MIN AS, Weber-law chemotaxis steering, GA
practice) and a codebase integration map. Key sources are cited inline.

## Foundation already landed (this session, tested, world_hash-neutral)

All deterministic, data-flagged OFF by default → canonical roster byte-identical:
- **Boids** separation + cohesion + alignment; **flee** — `InstinctLocomotionSystem` / `LocomotionProfile`.
- **A\*** grid pathfinding (`ai/AStarGrid.h`) + **path-following** (`LocomotionPathComponent`).
- **ScentField** stigmergy (`ai/ScentField.h`): multi-channel deposit / diffuse / evaporate / gradient, on `fields::ScalarFieldDiffusion`.
- **DeterministicRng** (`core/DeterministicRng.h`): seeded splitmix64 + Irwin-Hall Gaussian (libm-free).
- **Perception** (`ai/Perception.h`): vision cone (cos-half-FOV + range), hearing **audiogram** (pitch + loudness sensitivity).

## Pillars & requirements

### P1 — Perception (vision / hearing / smell), fused
- **Vision**: forward cone (FOV angle + range), line-of-sight (terrain/obstacle occlusion),
  degraded by **light level** (existing day/night StimulusChannels) and **concealment**
  (foliage/cover). Gives exact bearing *now*.
- **Hearing**: omnidirectional **audiogram** — sounds carry `(loudness, pitch)`; listeners
  have `peak_pitch/bandwidth/gain/threshold/range`. Pitch carries info (low rumble vs high
  alarm); occlusion attenuates. Gives rough presence *now*, even around corners.
- **Smell**: `ScentField` — where a target *was* + trail direction; persistent, delayed,
  **wind-borne** (P2).
- **Fusion → awareness**: a graduated detection meter drives `unaware → suspicious → alert →
  searching → engaged` (NOT binary), plus **memory/last-known-position** so a creature
  *searches* after losing a target. Fused salience feeds the existing `InstinctPlanner`
  scoring (perception adds opportunities/threats; planner already ranks).
- **R-P1.1** All sense tests deterministic + libm-free on the tick path (FOV as cos-half;
  triangular audiogram). **R-P1.2** Awareness is a per-agent state machine with a decaying
  detection accumulator. **R-P1.3** Sensory params (FOV, vision range, `HearingProfile`,
  scent sensitivity) are evolvable genes (P5).

### P2 — Stigmergy (scent trails) + wind coupling
- **R-P2.1** Update is the canonical two-stage rule per cell: `τ ← (1−ρ)·τ` **then** deposit
  (ρ the EVAPORATION rate, `0 < ρ < 1`) [Dorigo-Stützle ACO book]. ScentField already does
  diffuse+evaporate; align the order to evaporate-before-deposit at the deposit/sense system.
- **R-P2.2** **Clamp** every cell to `[τ_min, τ_max]` with `τ_min > 0` (MAX-MIN AS
  anti-stagnation: keeps every cell's follow-probability above zero, prevents premature
  convergence). Add clamping to ScentField.Step.
- **R-P2.3** Evaporation regime: **ρ ≈ 0.02–0.1 for stable trails, up to ~0.5 for fast
  adaptation**; default ρ ≈ 0.05 (low-but-nonzero — S-ACO: ρ=0.2 converged suboptimal,
  ρ=0.01 found shortest, ρ=0 never converged). Tunable per scent channel.
- **R-P2.4** **Two foraging channels** (to-food, to-home) with deposit on **both** outbound
  and return trips — required for shortest-path trail convergence [Deneubourg double-bridge].
- **R-P2.5** **Wind coupling** (highest leverage — `WindFieldSystem` already exists): advect
  the ScentField by the wind each tick → scent travels downwind → **hunt-upwind emerges**
  (predator approaches downwind; prey smell predators on the wind).

### P3 — Hunting & tracking (predator/prey)
- **R-P3.1** Predator follows prey-scent **up-gradient**; prey flees **down** predator-scent.
  Gradient steering uses the Weber-law normalized rule `Δθ = k·(L−R)/(L+R)` (robust to
  evaporation down to a threshold floor; sensitivity `pheromone^a`, a≈2). ScentField.Gradient
  already provides the field gradient; add the Weber steering on the read side.
- **R-P3.2** Scent **decay+diffusion set the tracking window** — the predator can track only
  while signal > its detection threshold (a cold trail is lost → fall back to search/memory).
- **R-P3.3** Vision/hearing give the kill-confirmation / close-range engage; smell is the
  long-range / around-corner / over-time track. Fusion (P1) arbitrates.

### P4 — Flocking / herding + signaling (DONE + one bridge)
- Boids landed. **R-P4.1** **Alarm signaling**: a creature whose awareness hits `alert` on a
  predator emits an alarm (hearing call + optional alarm-scent channel) → nearby herd flees
  together (collective vigilance — the senses→flocking bridge).

### P5 — Evolution (deterministic GA)
- **R-P5.1** Genome = small real-valued vector of the existing trait knobs: `move_speed`,
  boids `separation/cohesion/alignment_strength` + radii, `flee` threshold, FOV/vision range,
  `HearingProfile` fields, scent sensitivity + deposit rate.
- **R-P5.2** **Real-valued GA**: **tournament selection** (k≈2–3), **Gaussian mutation**
  (per-gene σ as a fraction of the gene's range, e.g. 5–10%; `DeterministicRng.next_gaussian`),
  optional blend/arithmetic crossover, **elitism** (carry the best 1–2 unchanged). Modest
  population, many generations [GA practice]. Clamp genes to sane ranges.
- **R-P5.3** **Reproducibility**: the entire loop draws from a single SEEDED stream
  (`DeterministicRng.seeded(EVOLUTION_OFFSET, parent_id, generation)`); no wall-clock. This
  is what keeps run==replay and is exactly the regime the mean-field GA analysis assumes.
- **R-P5.4** Pitfalls: loss of diversity / premature convergence → mitigate with elitism +
  adequate mutation σ + tournament (not pure roulette) + keep population diverse.
- **R-P5.5** Keep agent counts **20–48** (below the ~2–3% density / N≥256 regime where
  attractive-trail benefits degrade per the coverage research).

## Determinism & world_hash plan

- Foundations are world_hash-neutral (off by default). Going LIVE moves the hash; budget the
  **two ordered bumps** rule:
  - **Bump 1 (scent field)**: append `"|scents:" + scent_hash` to
    `ServerWorldRunner::ComposeWorldHash` (append-only chain, mirrors wind/weather/aether;
    current baseline after aether = `f17726d44054d133`). scent_hash = fnv1a over the grid.
  - **Bump 2 (genomes + awareness)**: ride the **entities sub-hash** — a `GenomeComponent` +
    awareness state on creatures serialize into the ECS snapshot, already in the chain (no
    separate top-level salt).
- **Seed offsets** (extend the wind+11/weather+12,13/aether+14 convention): **scent = +15,
  evolution = +16** (record in design-decisions.md). `DeterministicRng` already exists.
- Sense/steer/evolve math stays scalar-float + `DeterministicMath::Sqrt`; no libm
  transcendentals on the tick path (audiogram triangular, Gaussian Irwin-Hall, FOV cos-half
  precomputed at spawn). SimDeterminismLint must stay clean.

## Architecture (integration map)

Tick order `GameSession::TickSimulation` (anim → instinct-plan → wind → weather → aether →
event-drain). New deterministic systems, in order, after instinct-plan:
1. **PerceptionSystem** — build each agent's sensed set (vision cone ∩ LOS ∩ light; hearing
   audiogram; scent sample) → update awareness meter + memory → emit threat/opportunity into
   the planner inputs.
2. **ScentDepositSystem** — agents deposit their species/role scent (and trail channels);
   then `ScentField.Step` (wind-advect → evaporate+clamp → diffuse).
3. **LocomotionSystem** (wire the existing executor live) — seek/flee/boids/path-follow +
   Weber scent-gradient steering, producing `wish_xz`.
4. **Reproduction/EvolutionSystem** (slot after event-drain) — on death/birth/age triggers,
   select parents (tournament) → crossover+Gaussian-mutate genome → spawn offspring
   (generation+1), all from the seeded evolution stream. (No lifecycle exists yet — new.)

Components (extend `InstinctComponents.h`): `VisionProfile`, `HearingProfile` (in
Perception.h), `AwarenessComponent` (state + detection meter + last-known pos),
`ScentEmitterComponent` (channel, loudness, pitch, deposit rate), `GenomeComponent`
(traits + generation + birth_tick). Archetype JSON (`data/common/archetypes/*.json`) gains an
optional `genome`/`senses` block; spawn path (`RuntimeScenarioHarness`) populates them.

## Phasing (forge-dogfood each: plan → tasks → dispatch → verify)

- **E0 (done)** foundation primitives (boids, flee, A*, ScentField, RNG, Perception).
- **E1** PerceptionSystem + AwarenessComponent (fusion, detection meter, memory) — CPU-tested.
- **E2** ScentDeposit/Step live + clamp + wind-advect; Weber gradient steering; wire the
  locomotion executor into the tick — **Bump 1** (scent_hash).
- **E3** Hunting/tracking + alarm signaling (predator/prey archetypes, scent channels).
- **E4** Evolution: GenomeComponent + ReproductionSystem (tournament/mutation/elitism,
  seeded) + lifecycle (birth/death) — **Bump 2** (entities sub-hash).
- **E5** Scale + tune: 20–48 agents over the replicated transport; tune ρ/σ/thresholds;
  visual + determinism gates; a "watch evolution diverge" scenario fixture.

## Acceptance criteria

- Each phase: build clean, `ctest` green, `world_hash` run==replay (bumps only at E2/E4, each
  ≤1 ordered, documented), SimDeterminismLint clean.
- Stigmergy: a double-bridge-style fixture converges to the shorter path under ρ≈0.05 and
  fails to (stagnates) at ρ=0 — proving the evaporation/clamp behavior matches the literature.
- Tracking: a predator fixture re-acquires a moving prey via scent up-gradient within the
  tracking window, and loses it when the trail evaporates (→ search).
- Evolution: a seeded multi-generation fixture is bit-reproducible (run==replay) AND shows
  measurable trait divergence (e.g. predator FOV narrows, prey FOV widens) over N generations.
- Scale: 20–48 agents hold the per-tick budget and the replication bandwidth baseline.

## Risks

- **Determinism regressions** from any libm slip or unordered iteration → guard with
  SimDeterminismLint + id-ordered traversal (as the boids/scent code already does).
- **world_hash churn** beyond two bumps → keep scent append-only + genomes via entities hash.
- **Premature convergence** (stigmergy and GA) → τ-clamp + nonzero ρ; elitism + mutation σ +
  tournament. **Diversity loss** at high density → cap agent count per R-P5.5.
- **Perf** of per-agent O(neighbors)/O(LOS) at 48 agents → chunk-index neighbor queries
  (the AOI chunk grid already exists) before the O(N²) naive scan.

## References (from the research pass)
- Dorigo & Stützle, *Ant Colony Optimization* (book): two-stage update, ρ defaults, stagnation.
- Stützle & Hoos, *MAX-MIN Ant System*: τ-clamp anti-stagnation, slow-ρ stable trails.
- Dorigo et al. 1996, *Ant System*: ρ=0.5 ant-cycle, stagnation example.
- Deneubourg double-bridge: two-channel forward+backward deposit → shortest-path convergence.
- Weber-law chemotaxis steering `Δθ = k(L−R)/(L+R)`, pheromone^a (a≈2).
- GA practice: tournament selection, Gaussian mutation, elitism, seeded reproducibility.
