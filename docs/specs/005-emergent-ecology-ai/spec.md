# Spec 005 — Emergent Ecology AI (completion)

> **STATUS: VALIDATED (2026-06-21).** The draft hypotheses were re-audited against the live code
> (`ai/*`, `world/GameSession.cpp::TickSimulation`, `ai/EcologyHash.h`, `test/ai/*`, `test/sim/*`).
> Two FRs were mis-stated by the draft and have been corrected below (FR-1 alignment, FR-5 grid).
> Companion parallelism/determinism plan: handoff `HANDOFF-2026-06-21-three-pillars.md`.

## Context (key finding)
This pillar is **~75–85% already built and LIVE** in `world/GameSession.cpp::TickSimulation` (the
draft's "85–90%" is defensible for infrastructure but optimistic for acceptance criteria) — this is a
**completion/integration** spec, NOT green-field. Do not re-implement what exists.

**Already exists (verify, don't rebuild):** IAUS arbiter (`ai/UtilityAI.h`), `ai/CreatureBrain.h` +
`ai/CreatureBrainSystem.h` (live per-tick brain), boids cohesion+separation (`ai/Flocking.h`, int64
fixed-point order-independent), `ai/SpatialGrid.h`, vision-cone + hearing audiogram
(`ai/Perception.h` + `ai/PerceptionSystem.h`), awareness (`ai/Awareness.h`), scent stigmergy
(`ai/ScentField.h` + deposit/steering systems, 128×128×4-channel field), GA + creature genome +
sexual reproduction (`ai/Evolution.h`, `ai/CreatureGenome.h`, `ai/CreatureReproductionSystem.h`),
herd-alarm / predator-pack / migration / territory / circadian / lifespan / decomposition /
scavenging systems, and the ecology sub-hash (`ai/EcologyHash.h`). The whole pipeline is wired in
`GameSession::TickSimulation` and folded into `world_hash` (append-only `chunk|wind|weather|aether|
scents|ecology`). Tests live in `test/ai/*` + `test/sim/*`.

## Goals
Close the real gaps so the emergent behaviours the owner wants actually arise: true flocking, scent on
the wind, ant-trail foraging, sensory divergence under selection, at the 20–48-agent scale.

## Non-Goals
Re-implementing the existing substrate; any new top-level `world_hash` term (ecology rides existing
sub-hashes); visual cues (size/alarm VFX are render-only → hand to the render agent, per
engine/game decoupling).

## Functional Requirements (each = an audited gap)
- **FR-1 Boids alignment in the grid-gather path (CORRECTED + RE-SCOPED).** There are TWO flocking
  paths: (a) `CreatureBrainSystem.h:212` calls `Flocking.h::ComputeFlockSteer` with a herd gathered
  via the spatial-grid query (`grid.QueryRadius`, unordered bucket order) — this is why `Flocking.h`
  reduces in int64 fixed-point (`Flocking.h:39-47`) — and it has cohesion+separation but **NO
  alignment** (its `Snap` struct stores position only, no heading); (b) `InstinctLocomotion`
  (`InstinctLocomotionSystem.cpp:199-252`) has its own inline **float** cohesion+separation+alignment,
  but gathers `neighbors` in **id-sorted order** (`:33-55`) so it is already run==replay-safe — NOT a
  determinism risk. So the real work is **add a fixed-point alignment term to
  `Flocking.h::ComputeFlockSteer`** (extend it to accept neighbour headings + `alignment_weight` in
  `FlockParams`, accumulate the heading sum in fixed-point) and **snapshot prior-tick headings
  (`CreatureComponent.wish_x/wish_z`) into `CreatureBrainSystem`'s `Snap`** to feed it.
  `alignment_weight`/`LocomotionProfile.alignment_strength` default 0 → byte-identical until tuned
  (canonical roster + 1v1 tests stay exact). Unifying InstinctLocomotion onto the shared helper is a
  DRY-only follow-up (it changes numerics → would move plant-free hash), kept OUT of the default path.
- **FR-2 Scent wind-advection:** add an optional semi-Lagrangian backtrace pre-pass to
  `ScentField::Step` (`ScentField.h:68-103` — currently no wind param) sampling the (already-hashed)
  `WindFieldSystem`, so scent travels downwind (enables hunt-upwind). Fixed-order/fixed-point.
  **Ordering constraint:** scent runs at tick slot ~196-212, wind updates at ~287 — read the
  **prior-tick** wind field to keep slot order (and the byte prefix) stable; do NOT reorder slots.
- **FR-3 Ant foraging double-trails:** `ForagerComponent` + NEW `ai/ForagingSystem.h` (file does not
  exist yet — largest item) — two-channel deposit-on-both-trips (to-food / to-home), follow the
  opposite channel via `GradientSteer` (Deneubourg double-bridge). Opt-in, appended as a discrete
  GameSession slot (never reflow existing slots). Deposit-only — no new RNG stream.
- **FR-4 Genome → senses:** extend `CreatureGenome` (currently 4 traits only — move_speed/vigilance/
  hunger_threshold/size_scale, `CreatureGenome.h:36-41`) with FOV / vision_range / hearing genes +
  bounds + mutation/crossover; `CreatureReproductionSystem::RunMatingResolveOnTick`
  (`CreatureReproductionSystem.h:208-232`, currently stamps transform+creature+genome only) must
  stamp `PerceptionComponent` from the child genome. Rides the existing reproduction RNG stream
  (offset 16). Enables predator/prey sensory divergence under selection.
- **FR-5 Scale + divergence (CORRECTED — partly done).** The herd query ALREADY uses `SpatialGrid`
  (`CreatureBrainSystem.h:79-91`); only the opposite-role *catch-target* scan is still O(N)
  (`CreatureBrainSystem.h:113-124`). Real work = move that scan to the existing opposite-role grid; add
  a deterministic evolution-divergence scenario fixture + per-generation trait telemetry; validate
  20–48 agents within tick + replication budgets.

## Non-Functional / Determinism
Integer/fixed-point, id-ordered, libm-free (`DeterministicMath`), unsigned hashing; run==replay
byte-exact. Re-pins ride the EXISTING `scents` + `ecology` sub-hashes (empty-roster stays
byte-identical → canonical baseline `d950a6afc12a5cdc` unchanged until a roster opts in). New sensory
genes ride the existing reproduction RNG stream (offset 16); foraging is deposit-only (no new RNG).
**Sim files only — zero overlap with the 600 fps render path** (the one contact point is creature
`TransformComponent` positions the render agent reads).

## Phasing (each landable + testable alone; batch the re-pin)
E2.1 alignment → E2.2 wind-advection → E3.1 foraging trails → E4.1 genome→senses → E5 scale+divergence.
Keep run==replay each commit; re-pin `scents`+`ecology` goldens ONCE at the end of E2–E4 (Batch A),
BEFORE foliage (006) appends its new top-level hash term.

## Acceptance Criteria (draft — re-derive)
- [ ] A herd converges to a common heading (alignment) without breaking existing flee/hunt tests.
- [ ] Scent measurably drifts downwind; a predator tracks prey upwind in a fixture.
- [ ] Foraging converges on the shorter path at evaporation ρ≈0.05, stagnates at ρ=0 (double-bridge).
- [ ] Predator FOV narrows / prey FOV widens across generations (divergence telemetry asserts it).
- [ ] 20–48 agents hold tick + replication budgets; run==replay byte-exact; gates green after re-pin.

## Sources / further reading
Reynolds boids (1987); Deneubourg double-bridge ant foraging; Dave Mark IAUS (GDC). (Re-cite during
the agent's own research pass.)
