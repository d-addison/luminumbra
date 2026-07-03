# Pillar audit: Foliage & farming (spec 006) (Spec 021, 2026-07-02)

The foliage pillar is in the strongest end-to-end state of the feature systems: the spec 006 farming
loop is closed through all five core phases (deterministic env-coupled growth, germination/lifecycle,
plant sub-hash + persistence, the unified sim→render plant bridge, and data-driven species + player
verbs + crop HUD), the vast-forest render stack (palette trees + 4-tier LOD + octahedral impostors,
now default-ON) is live, and the spec 017-A async blade readback rerouted the FoliagePass gate
readback off the hot path. Its one first-class defect is severe, however: the **FoliageInstancing
engine-frontier gate is RED** — the debug `flat_lands` scatter run produces **0 instances** in every
CPU-side probe (empty instance hash, zero coverage, zero sway) where ~70k in-ring instances are
expected — proven by a prior session NOT to be a 017-A regression, root cause still unknown. Secondary
debt: the ForestPerfBudget RED-by-design harness/gate was never re-tasked to green after impostors
landed, `octa_impostor_test` is built but never registered with ctest, and `scatter_set.json` carries
an uncommitted working-tree edit.

## Current state + evidence

### Sim farming loop (spec 006 Phases 1–5: all landed)

Spec 006 is a "wire-the-loop" spec over an already-complete deterministic substrate
(docs/specs/006-living-world-foliage-farming/spec.md:10-23). Verified against the tree, every phase
through 5 has shipped:

- **Phase 1 — env loop closed, tick order fixed.** `PlantGrowthSystem`'s `EnvSampler` now reads real
  atmospheric + player-driven inputs: weather precipitation via `PrecipitationAt`
  (src/luminumbra_common/world/GameSession.cpp:417), the irrigation grid via `MoistureAt`
  (GameSession.cpp:421-426), the soil-nutrient grid via `NutrientAt` (GameSession.cpp:450-459),
  a real day/night light curve replacing the 0.75 stub (GameSession.cpp:461-469), and a deterministic
  annual season swing (GameSession.cpp:475-483). The G4 tick-ordering bug is fixed: the soil/irrigation
  fields update at slot 5b BEFORE plant growth ("the soil-nutrient + irrigation FIELDS now update at
  slot 5b (BEFORE plant growth)", GameSession.cpp:489-495; grid ticks at GameSession.cpp:395-401).
  RED-first tests exist and are ctest-registered: `PlantGrowth.MonocultureStarvesSharedSoil`,
  `Farming.WateringBoostsGrowthInHarshEnv`, `PlantGrowth.BlightSlowsGrowth`,
  `PlantGrowth.IrrigationMoistensCellForGrowth` (test/ai/plant_growth_test.cpp:159,229,244,288, built
  into `frontier_gates_test`, test/CMakeLists.txt:795).
- **Phase 2 — lifecycle + germination.** `CropLifecycleSystem` with the corrected seed offsets
  `kGerminationSeedOffset = 36` / `kSeasonSeedOffset = 37` (reserved, no RNG)
  (src/luminumbra_common/systems/CropLifecycleSystem.h:36-37); tests
  `CropLifecycle.AnnualDiesAndReseeds` / `PerennialResetsAndRegrowsAndReseeds` /
  `GerminatesFromPollinationCross` / `DeterministicReplay` (test/sim/crop_lifecycle_test.cpp:61-102,
  in `common_tests`, test/CMakeLists.txt:180). The live spawn path wires the cross-pollination opt-in
  end-to-end (test/sim/foliage_drift_integration_test.cpp:1-6; FarmingSystem opt-in documented at
  src/luminumbra_common/systems/FarmingSystem.h:41-63).
- **Phase 3 — hash + persistence.** `GameSession::ComputePlantSubHash()` hashes the id-ordered integer
  growth state, empty-neutral (src/luminumbra_common/world/GameSession.cpp:1134-1153; note it has
  MOVED from the spec's cited `GameSession.cpp:968`) and is folded into the composite world hash by
  the server runner (src/luminumbra_server/ServerWorldRunner.cpp:614,684) and the smoke path
  (src/luminumbra_server/main_server.cpp:428). Persistence projects every PlantTag's sim truth into
  the generic EntitySnapshot framework, geometry excluded, empty roster byte-identical
  (src/luminumbra_common/persistence/PlantPersistence.h:3-13; test
  persistence/plant_persistence_test.cpp in `frontier_gates_test`, test/CMakeLists.txt:816).
- **Phase 4 — render bridge.** `RebakeAllPlants` composites the decoration scatter with SIM-tier
  PlantTag plants at their live `PlantGrowthComponent.stage` into the single `PlantProcgenPass`,
  sig-gated, visual-only (src/luminumbra_client/main_client.cpp:438-511). `PromoteNearestScatter`
  lets the player promote a decoration tree into a live, persisting sim plant
  (main_client.cpp:520-534). `render.plant_procgen` is enabled in data (data/common/systems.json:40).
  The sim-growth timelapse showcase seeds live species-driven plants (main_client.cpp:6291-6300).
- **Phase 5 — species data + player verbs + HUD.** Six species JSONs exist
  (data/common/foliage/species/wheat.json, oak/maize/frostberry/sungourd/moonpetal.json) loaded by
  `SpeciesRegistry` (src/luminumbra_common/foliage/SpeciesRegistry.h). Player farming state +
  species picker live in the client (main_client.cpp:365-372); plant/water/fertilize/harvest are
  input-bound with one-shot audio (main_client.cpp:6965-7007); the crop HUD panel shows the aimed
  crop's species/stage/quality + seed/harvest inventory (main_client.cpp:7196-7240). Ecology coupling
  exists too: `WildlifeFoliageSystem` grazing/regrowth, deterministic and opt-in
  (test/sim/wildlife_foliage_test.cpp:1-6).
- **Phase 6 — scale for sim-grown fields: NOT started** (see Gaps).

The load-bearing owner rule is enforced in code: procedural geometry is VISUAL-ONLY; the generator is
a deterministic pure function using libm-free `DeterministicMath`, never hashed
(src/luminumbra_common/systems/PlantProcgen.h:3-16). Note the generator is a **recursive
branch-skeleton** grower, not space colonization (PlantProcgen.h:5-9) — the memory's
"space-colonization" phrasing describes the research direction, not the shipped implementation.

### Render scatter — FoliagePass (grass/ground cover)

The instanced scatter pass is a deterministic pure-hash placement system (no RNG, no seed offset,
render-only one-way contract) with a 262,144-instance pool and 8,192 candidates/chunk
(src/luminumbra_client/rendering/passes/FoliagePass.h:50-72). It has a per-chunk camera-independent
record cache + build budget (FoliagePass.h:309-328), a scatter-cache elision signature
(FoliagePass.cpp:328-375), and a GPU compute scatter path (`grass_scatter.comp`, bit-exact splitmix64
port of the CPU hash, atomic append into a blade SSBO + indirect draw command;
res/shaders/grass_scatter.comp:74-92,205-220) with graceful CPU fallback on compile failure
(FoliagePass.cpp:216-229). Default density scale is 1.35 (FoliagePass.h:307). Archetypes are game
content from data/common/foliage/scatter_set.json:4-53 (6 archetypes; density from the biome table —
plains vegetation density 0.3, data/common/biomes.json:126-128).

**Spec 017-A blade readback reroute (verified landed):** the gate-only blade readback no longer uses
synchronous `glGetBufferSubData`. `rebuild_instances_gpu` submits the count + blade copy through the
`AsyncReadbackRing` and returns immediately (FoliagePass.cpp:820-835); `poll_foliage_readback()`
drains the most-recent COMPLETED result into `m_instances` every frame BEFORE the scatter-cache
elision, stale-safe and never re-emptied once primed (FoliagePass.cpp:317-326,853-873; ring member
FoliagePass.h:271-278). The ring's GL slots are freed under a live context in `destroy_compute`
(FoliagePass.cpp:255-267, commit 3bba2a52). FoliagePass has LEFT the FR-G-001 render-readback-ban
allowlist — only the RenderPipeline GPU-SDF site remains, gated on 017-B
(.forge/scripts/validate-engine-frontier.ps1:7026-7033); the gate dispatches as
`-Mode RenderReadbackAllowlist` (validate-engine-frontier.ps1:7306). The ring contract itself is
ctest-covered against a real headless GL context (test/rendering/async_readback_ring_test.cpp:1-9).

### Vast-forest stack — palette trees, LOD, impostors

- **Palette trees:** a genome-driven procedural tree palette is built at runtime and registered as
  `procgen://tree_N_{leaf,bark}[.lodN]` meshes into the instanced static-mesh path
  (src/luminumbra_client/main_client.cpp:855-929; registration hook
  src/luminumbra_client/rendering/passes/GBufferPass.h:73-76), with per-instance tint variation
  (GBufferPass.h:103).
- **LOD:** 4 buckets — LOD0 full mesh, LOD1/2 coarser, LOD3 far-field cross-billboard at 620 m —
  pure render-only selection, unit-tested (src/luminumbra_client/rendering/TreeLod.h:24,45-51;
  `tree_lod_test` registered, test/CMakeLists.txt:206,927). The FR-C2 note records that the forest
  G-buffer cost is fill/OVERDRAW-bound, not triangle-bound (TreeLod.h:39-44).
- **Octahedral impostors: landed and now DEFAULT-ON** — this contradicts the prior-session memory
  ("landed default-OFF ... validate before default-ON"): the flip was perf-validated
  (`--render-benchmark forest_dense`) and committed as ad3b093e (2026-06-25);
  `LUMIN_TREE_IMPOSTORS=0` is now the opt-OUT
  (src/luminumbra_client/rendering/RenderPipeline.cpp:665-671). The bake produces albedo + object-space
  normal atlas textures at 12x12 hemi-octahedral views with real bark/leaf textures
  (src/luminumbra_client/rendering/ImpostorBake.h:41-56; RenderPipeline.cpp:669-670; mapping math in
  src/luminumbra_client/rendering/OctaImpostor.h:41-58). The header comment at
  src/luminumbra_client/rendering/RenderPipeline.h:1168 still says "opt-in ... default OFF" — stale.
- **I8 tree-part bark/leaf textures** are loaded and registered (RenderPipeline.cpp:664, commit
  273e769a) — the tree-texture half of the remembered "I8 debt" has shipped.

### Gates and tests that guard this pillar

- **FoliageInstancing** is an engine-frontier gate (NOT a plain ctest, correcting the brief's
  phrasing): `validate-engine-frontier.ps1 -Mode FoliageInstancing`
  (.forge/scripts/validate-engine-frontier.ps1:3551,7271) runs
  `--scenario foliage_visual_smoke --world-preset flat_lands` (validate-engine-frontier.ps1:3571-3580)
  and asserts determinism (run==run instance hash), biome-band coverage density, distance fade, wind
  sway, and the GPU-timer budget (validate-engine-frontier.ps1:3600-3639).
- **ForestPerfBudget** ctest (test/CMakeLists.txt:967) + `-Mode FarFieldForestBudget` gate
  (validate-engine-frontier.ps1:3099) — see Gaps: still in the deliberate-RED posture.
- Sim tests: plant growth/genome (test/ai/plant_growth_test.cpp:70-288), crop lifecycle
  (test/sim/crop_lifecycle_test.cpp:45-102), drift integration, wildlife grazing, plant persistence —
  all inside ctest-registered targets (test/CMakeLists.txt:921-943).

### The mandated first-class finding: FoliageInstancing RED (0 blades, debug flat_lands)

Failure shape, from the most recent on-disk analysis artifact
(build/debug/test-artifacts/runtime/foldbg/foliage-instancing-analysis.json, timestamp 2026-06-29):

- `coverage_density.instances_total = 0`, `instances_within_ring = 0`, `measured_density = 0.0` →
  the coverage assertion fails first ("Foliage coverage density off-band",
  validate-engine-frontier.ps1:3610-3612), and `wind_sway` fails downstream (calm 0.0 → windy 0.0).
- `instance_hash_run_a == instance_hash_run_b == 1469598103934665603` — exactly the code's FNV-1a
  empty-set basis `fnv1a(nullptr, 0)` (FoliagePass.cpp:43,981-985): both probe snapshots hashed an
  EMPTY `m_instances`.
- Yet `render_pass.foliage_instances_drawn = 262144 == kMaxInstances` (FoliagePass.h:53) with
  `foliage_draws = 1`. In readback mode `m_frame_instance_count` tracks `m_instances.size()`
  (FoliagePass.cpp:847-848), so a 262,144 drawn-count can only come from the `!m_readback_enabled`
  play-mode marker branch — i.e. **this particular artifact was captured with the CPU readback
  disabled** (the single toggle site is
  `set_readback_enabled(scenario_config.active() && !g_play_paths)`, main_client.cpp:6637), which is
  itself notable: the analysis writer's `foliage_draws > 0 && foliage_instances_drawn > 0` guard
  (main_client.cpp:8103-8105) is satisfied by the play-mode marker, so a readback-disabled run writes
  a superficially-complete analysis with vacuously-empty probes instead of aborting.
- Expected healthy shape at this pose: ~70k in-ring instances on the dense flat_lands scatter
  (calibration note, main_client.cpp:8156-8162); a 2026-06-17 artifact recorded 60,501 instances
  (build/debug/test-artifacts/runtime/foliage-gpu-lush/foliage-instancing-analysis.json:6) — the
  zero-emission is a regression somewhere in the 2026-06-17 → 2026-06-29 window.
- Prior-session context (recorded here per FR-A-002; not re-derivable from the tree alone): the
  scatter compute was observed emitting 0 blades in debug flat_lands, and reverting the 017-A commits
  at HEAD did NOT fix it — so it is NOT a 017-A regression; root cause unknown.
- Zero-emission candidates to check first (all code-verified as plausible early-outs):
  `chunk.density <= 0` skips upload/dispatch (FoliagePass.cpp:677-679; grass_scatter.comp:147),
  the conservative all-4-corners `valid` test on the coarse surface grid rejects candidates
  (grass_scatter.comp:124-125), the cached per-chunk surface grid could serve stale/invalid entries
  (FoliagePass.cpp:687-713), and the sea-level/slope hard gates (grass_scatter.comp:171-172).
- The canonical gate artifact dir (`build/debug/test-artifacts/runtime/foliage-instancing`,
  validate-engine-frontier.ps1:3564) contains only runtime telemetry from a 2026-06-29 run
  (last-known-runtime.json, shutdown.json, streaming-telemetry.json) but NO
  foliage-instancing-analysis.json — the first fix step is a clean gate re-run to capture an
  uncontaminated, readback-enabled failure artifact. If that run wedges at world-load/IN_GAME, the
  upstream blocker is the RENDER-pillar headless IN_GAME capture hang, not this defect.

### Shipped since the 2026-06-28 roadmap

Verified via `git log` and the tree:

- **017-A foliage blade readback rerouted onto the AsyncReadbackRing** with the every-frame
  stale-safe drain that survives scatter-cache elision (commit ca2616d8, 2026-06-29;
  FoliagePass.cpp:317-326,820-835,853-873), plus FoliagePass leaving the FR-G-001 render-readback-ban
  allowlist (validate-engine-frontier.ps1:7029-7033).
- **Ring teardown fix under a live GL context** (commit 3bba2a52; FoliagePass.cpp:255-267).
- Nothing else foliage-owned landed in the window (git log --since=2026-06-27 over FoliagePass/
  AsyncReadbackRing/grass_scatter.comp/data/common/foliage shows only those two commits). The
  impostor default-ON flip (ad3b093e, 2026-06-25) and tree textures (273e769a, 2026-06-18) predate
  the roadmap but correct stale memory assumptions, so they are filed as `done` items below (note
  273e769a lands one day inside the 2026-06-17 → 2026-06-29 zero-emission regression window that
  FOLIAGE-01's bisect will search).

## Gaps / debt

1. **FoliageInstancing gate RED, root cause unknown** (mandated finding, above) — blocks re-blessing
   any foliage-visible change and leaves the grass determinism surface unverified
   (validate-engine-frontier.ps1:3610-3612; foldbg artifact lines 6-9,14-15,44-45).
2. **ForestPerfBudget / FarFieldForestBudget never re-tasked after impostors landed.** The model
   explicitly excludes the impostor atlas ("LOD3 here is the cross-billboard placeholder, NOT a
   shared impostor atlas", test/rendering/forest_perf_budget_test.cpp:156) and RED-fails by design
   (forest_perf_budget_test.cpp:260-271); the gate PASSES only while `over_budget.any == true` and
   documents "when impostors flip the load under budget ... the gate must be re-tasked to require
   GREEN" (validate-engine-frontier.ps1:3106-3111). Impostors are default-ON
   (RenderPipeline.cpp:665-668), so the budget certification is now stale-by-design: the suite carries
   a permanently RED manual ctest and no green budget proof of the impostor win.
3. **`octa_impostor_test` is built but never runs.** It is an `add_executable`
   (test/CMakeLists.txt:220) but is absent from `LUMINUMBRA_GTEST_TARGETS`
   (test/CMakeLists.txt:921-943) and has no `add_test` — the impostor direction<->tile mapping (the
   shared source of truth for bake + shader, OctaImpostor.h:13-18) has zero executing coverage.
4. **Uncommitted working-tree edit to data/common/foliage/scatter_set.json** (git status: modified;
   diff swaps card sizes/colors vs HEAD, e.g. grass_tuft 0.19/0.44 → 0.26/0.30). Untracked visual
   state that will silently ride the next commit or be lost.
5. **Spec 006 Phase 6 (scale for sim-grown fields) not started.** `RebakeAllPlants` re-tessellates
   EVERY plant into one combined mesh whenever any (id, stage) changes (main_client.cpp:438-511;
   "ONE combined CPU vertex/index buffer", PlantProcgenPass.h:25-32) — no instancing, no LOD tier for
   sim plants; a large farmed field or many promoted trees will hitch on every stage transition.
6. **Stale comments:** RenderPipeline.h:1168 still documents impostors as "default OFF";
   docs/specs/006 spec cites `ComputePlantSubHash` at GameSession.cpp:968 (now :1134).

## Risks

- **The RED gate masks new regressions:** while FoliageInstancing is red, any further scatter breakage
  (placement drift, coverage collapse, sway loss) lands silently; the WorldVisualSweep daytime cells
  do assert `foliage_draws > 0` (RuntimeScenarioHarness.cpp:8885-8894) but not coverage/determinism.
- **`GL_ARB_gpu_shader_int64` requirement** (res/shaders/grass_scatter.comp:2): on hardware/drivers
  without the extension the compute path silently falls back to the CPU loop
  (FoliagePass.cpp:216-229) — correct but a hidden perf cliff, and an int64-in-shader portability
  wall for the 014 RHI/Vulkan port of this shader.
- **Determinism posture is good:** the scatter is render-only one-way (FoliagePass.h:42-49), the sim
  half is integer/id-ordered with empty-neutral hashing (GameSession.cpp:1134-1153) — no world_hash
  exposure from this pillar's open items. The main execution risk is concentrated in the unknown-root
  0-blade defect.
- **Overdraw, not triangles, is the forest cost** (TreeLod.h:39-44): future foliage density/impostor
  tuning must be validated against fill-bound benchmarks, or wins will be illusory.

## Opportunities

- **Impostor budget certification** (Gaps #2) doubles as the first real "vast forest at scale"
  number: extending the model with the shared-atlas LOD3 path yields a defensible tri/draw-call
  budget green and a re-usable model for Phase 6 crops.
- **Phase 6 unified-plant scale**: per-(genome, stage) mesh caching + instanced draws would reuse the
  existing palette-tree instanced path (GBufferPass.h:73-76) rather than inventing a new one.
- **Space-colonization / richer tree morphology** as a follow-on to the recursion grower
  (PlantProcgen.h:5-9) — same deterministic pure-function contract, purely visual, high fidelity
  payoff for the BF4-floor mandate.
- **Farming loop is demo-ready**: promote-a-wild-tree (main_client.cpp:520-534) + species picker +
  HUD is a coherent player-facing slice worth a showcase timelapse once the IN_GAME capture hang
  (RENDER pillar) is fixed.

## Backlog items

| id | summary | spec | effort | risk | deps | status | proving_signal |
|---|---|---|---|---|---|---|---|
| FOLIAGE-01 | Diagnose + fix the FoliageInstancing gate RED: debug flat_lands scatter yields 0 CPU-probe instances (empty-basis hash, 0 coverage, 0 sway) vs ~70k expected; not a 017-A regression, root cause unknown; first step = clean readback-enabled gate re-run | new | M | medium | [] | todo | engine-frontier gate: validate-engine-frontier.ps1 -Mode FoliageInstancing (green) |
| FOLIAGE-02 | 017-A blade readback rerouted onto AsyncReadbackRing with every-frame stale-safe drain surviving scatter-cache elision; FoliagePass left the FR-G-001 ban allowlist; ring freed under live GL context | 017-A | M | low | [] | done | async_readback_ring_test ctest + validate-engine-frontier.ps1 -Mode RenderReadbackAllowlist |
| FOLIAGE-03 | Far-field octahedral tree impostors landed and flipped DEFAULT-ON (perf-validated, LUMIN_TREE_IMPOSTORS=0 is now the opt-out); albedo+normal 12x12 atlas from real textures | new | L | low | [] | done | engine-frontier gate: validate-engine-frontier.ps1 -Mode WorldVisualSweep |
| FOLIAGE-04 | Spec 006 farming loop Phases 1-5 landed: env-coupled growth (light/moisture/nutrient/season, slot-5b ordering fix), CropLifecycleSystem germination (+36/+37), plant sub-hash + EntitySnapshot persistence, unified RebakeAllPlants render bridge + promotion, species registry + player verbs + crop HUD | 006 | XL | low | [] | done | ctest PlantGrowth.*/Farming.*/CropLifecycle.* (frontier_gates_test, common_tests) + --smoke run==replay 6f008a9f637c40b7 |
| FOLIAGE-05 | Re-task the forest budget to GREEN posture: extend forest_perf_budget_test's model with the landed LOD3 shared-atlas impostor path, flip the RED EXPECTs to a green budget, and invert -Mode FarFieldForestBudget to assert over_budget.any == false | new | M | low | ["FOLIAGE-03"] | todo | NEW: ForestPerfBudget-green — the 16k-tree model WITH the impostor-atlas LOD3 path passes tri+draw budgets and -Mode FarFieldForestBudget asserts over_budget.any == false |
| FOLIAGE-06 | Register octa_impostor_test with ctest (it is built at test/CMakeLists.txt:220 but absent from LUMINUMBRA_GTEST_TARGETS, so the impostor mapping math never executes in the suite) | new | S | low | [] | todo | NEW: octa_impostor_test ctest registration — gtest_discover_tests(octa_impostor_test) runs the OctaImpostor encode/decode round-trip cases in the default ctest lane |
| FOLIAGE-07 | Spec 006 Phase 6: scale the unified sim-plant render — per-(genome,stage) mesh cache + instanced draws (+ LOD tier) instead of RebakeAllPlants re-tessellating every plant into one combined mesh on any stage change | 006 | L | medium | ["FOLIAGE-01"] | todo | NEW: PlantProcgenScale ctest — models a 10k-sim-plant field and asserts per-stage-transition rebake cost and draw submission stay within a pinned budget (byte-identical output for the small-roster baseline) |
| FOLIAGE-08 | Reconcile the uncommitted data/common/foliage/scatter_set.json working-tree edit (card sizes/colors differ from HEAD): commit deliberately with a visual re-bless or revert | new | S | low | ["FOLIAGE-01"] | todo | engine-frontier gate: validate-engine-frontier.ps1 -Mode WorldVisualSweep (no NEW flags vs baseline) |
| FOLIAGE-09 | Remove the GL_ARB_gpu_shader_int64 hard requirement from grass_scatter.comp (32-bit-pair splitmix64 kept bit-exact with the CPU hash) so the GPU scatter survives non-int64 drivers and the 014 RHI port | new | M | medium | [] | todo | engine-frontier gate: validate-engine-frontier.ps1 -Mode FoliageInstancing (instance-hash determinism + coverage band unchanged) |
| FOLIAGE-10 | Upgrade tree/plant morphology (space-colonization or improved recursion) under the same deterministic pure-function visual-only contract, palette + impostor pipeline unchanged | new | L | low | ["FOLIAGE-05"] | todo | engine-frontier gate: validate-engine-frontier.ps1 -Mode WorldVisualSweep + ForestPerfBudget ctest (budget still green) |
| FOLIAGE-11 | Harden the FoliageInstancing analysis writer: refuse to write an analysis when the readback is disabled (the play-mode kMaxInstances marker currently satisfies the foliage_draws>0 guard and produces vacuously-empty probe artifacts) | new | S | low | [] | todo | NEW: FoliageInstancing gate negative case — a readback-disabled scenario run produces NO analysis artifact and the gate fails with the explicit "readback disabled" diagnostic |
