# Spec 006 — Living-World Foliage Farming

> **STATUS: VALIDATED (2026-06-21).** Re-audited against the live code (`systems/*`,
> `world/GameSession.cpp`, `world/ServerWorldRunner.cpp`, `persistence/*`, `main_client.cpp`,
> `test/*`). Substrate is even MORE complete than the draft claimed (Irrigation/Disease systems +
> `ComputePlantSubHash()` already exist); two draft claims corrected below (the seed offsets, and
> Phase 3 = *wire* the existing sub-hash, not author it). A new tick-ordering bug was found (G4).
> Companion parallelism plan: handoff `HANDOFF-2026-06-21-three-pillars.md`.

## Context (key finding)
The deterministic plant SUBSTRATE largely exists (genome/GA `luminumbra::foliage`:
`RandomGenome`/`ExpressGenome`/`GeneratePlant`/`TessellatePlant`; sim systems `PlantGrowthSystem`/
`SoilNutrientSystem`/`PollinationSystem`/`PlantDiseaseSystem`/`IrrigationSystem`/`FarmingSystem` +
plant/health/disease components; plus `ComputePlantSubHash()` already implemented at
`GameSession.cpp:968`), but the **farming LOOP is open**: the sim→render bridge is disconnected,
there's no persistence, no player verbs, and the environment inputs are stubbed/mis-ordered. This is a
**wire-the-loop** spec.

**Already exists (verify, don't rebuild):** `luminumbra::foliage` genome/GA + `GeneratePlant`/
`TessellatePlant`; `PlantGrowthComponent`/`PlantGenomeComponent`/soil/disease/pollination components;
`PlantGrowthSystem`, `SoilNutrientSystem` (`NutrientAt`), `PollinationSystem`; the dedicated
`PlantProcgenPass` + `res/shaders/plant_procgen.{vert,frag}` (separate pass into the shared G-buffer).

## Gaps → Goals (close the loop)
- **G1 sim→render bridge disconnected:** `BakeProcgenPlants` reads CLIENT-side `g_procgenPlants` +
  a manual `g_procgenStageF` scrubber — NOT the sim `PlantTag` entities' `PlantGrowthComponent.stage`.
  Growth-over-time isn't driven by the deterministic tick. (Core deliverable.)
- **G2 no persistence:** plant components aren't serialized — saved worlds lose crops/growth/genetics.
- **G3 no player verbs:** `PlantSeed/Water/Fertilize/Harvest` exist as sim fns but no input binding,
  no seed/harvest inventory, no crop HUD.
- **G4 env loop half-wired + MIS-ORDERED (CORRECTED):** `EnvSampler` hardcodes `light=0.75`
  (`GameSession.cpp:347`); irrigation/nutrient grids are NOT read by `PlantEnvSample` at all, and
  disease/season aren't folded into growth `Suitability`, so the resource loop (monoculture starves,
  watering pays off, blight slows) isn't closed at the growth tick. **Tick-ordering bug:** plant
  growth ticks at slot 6 but the soil/irrigation grids update at slot 7 → growth reads 1-tick-stale
  env even once the reads are added. Phase 1 must (a) move the grid ticks BEFORE growth, (b) add
  `MoistureAt`/`NutrientAt` reads into `PlantEnvSample`. Pollination `next_genome` is computed but
  never re-seeds.
- **G5 no species data table; no LOD for vast sim-grown fields** (engine-generic data-driven content).

## Non-Goals
Re-implementing genome/GA; touching the 600 fps agent's `FoliagePass` grass path (the farming render
bridge is the separate `PlantProcgenPass`); procedural geometry in the sim (geometry is visual-only).

## Phasing (each landable + testable; re-pin batched)
1. **Phase 1 — close the env loop (sim):** real day/night light (`kTicksPerDay`) + self-shading;
   **REORDER** soil/irrigation/disease ticks to BEFORE the plant-growth slot (fixes the slot-6-reads-
   slot-7-grids staleness); add `MoistureAt`/`NutrientAt` reads into `PlantEnvSample`; fold
   `SoilNutrient`/irrigation moisture + `PlantHealth.infection` into `Suitability`; overcrowding/
   season stress. NOTE: reordering tick slots is hash-moving — keep it within the foliage-only range,
   append/move as discrete blocks, and re-pin once (below). RED-first tests: monoculture-starves,
   watered-beats-dry, blight-slows. Re-pin plant-bearing `world_hash`; no-plant baseline stays green.
2. **Phase 2 — lifecycle + germination (sim):** SIM season phase (`tick%year`) + `CropLifecycleSystem`
   (NEW; annual remove / perennial reset) + germination from `PollinationComponent.next_genome` via
   `PlantSeed`. **Seed offsets (CORRECTED):** the draft's +21/+22 are BOTH TAKEN (21=irrigation,
   22=lifespan, 23=wildlife-foliage) — the registry is allocated through +35. Claim the next free
   slots **+36 germination / +37 season** (re-grep `*SeedOffset` immediately before claiming). Test:
   N-season drift → germinated child genome matches the cross; run==replay.
3. **Phase 3 — persistence + hash (the ONLY new top-level hash term across all tracks):** serialize
   plant/genome/soil/disease/pollination in `WorldSaveService` + roundtrip; **wire the EXISTING
   `GameSession::ComputePlantSubHash()` (`GameSession.cpp:968`, already implemented) into
   `ServerWorldRunner::ComposeWorldHash`** (`ServerWorldRunner.cpp:96-106`, currently
   `chunk|wind|weather|aether|scents|ecology`) — appended LAST after `ecology`, gated so empty-roster/
   no-plant is byte-identical — plus the matching `NetworkStateHash` term. Test: save→load byte-exact
   for a planted field; no-plant baseline UNCHANGED.
4. **Phase 4 — render bridge (visual-only):** `BakeSimPlants` iterates the real `PlantTag` view →
   `GeneratePlant(genome, sim stage, env)` + `TessellatePlant` → `PlantProcgenPass`; demote
   `g_procgenStageF` to a debug override. Timelapse shows geometry advancing with sim stage;
   `render.plant_procgen` stays as-is (already `true` — do NOT regress).
5. **Phase 5 — content + UX:** data-driven `SpeciesRegistry` (`data/common/foliage/species/*.json`:
   genome ranges / structural bias / annual-perennial / lifespan / render archetype, engine-generic);
   player farming verbs (plant/water/fertilize/harvest) bound to input + seed/harvest inventory + crop HUD.
6. **Phase 6 — scale (optional, coordinate with render agent):** LOD/impostor tier for sim-grown crops.

## Determinism / re-pin
Sim half stays integer/fixed-point, id-ordered, milli-unit, seeded RNG from sorted ints+tick. Phase
1/2 re-pin plant-bearing `world_hash` only (no-plant baseline guard unchanged). Phase 3 is the single
NEW hashed top-level surface — append after ecology's re-pins so the byte prefix is stable; batch the
plant + `NetworkStateHash` + persistence-roundtrip golden re-pins in one pass after Phase 3.

## Acceptance Criteria (draft — re-derive)
- [ ] Growth-over-time is driven by the sim tick (not the scrubber); timelapse shows it.
- [ ] Monoculture starves / watering helps / blight slows — at the growth tick (RED-first tests pass).
- [ ] Save→load preserves crops/growth/genetics byte-exact; no-plant baseline unchanged.
- [ ] N-season pollination drift germinates children matching the cross; run==replay.
- [ ] Player can plant/water/fertilize/harvest with seed/harvest inventory + crop HUD.
- [ ] All gates green after the batched re-pin; engine stays generic (species are data).

## Sources / further reading
DST plant stress; L-system / space-colonization growth; Mendelian crossbreeding. (Re-cite in the
agent's own research pass.)
