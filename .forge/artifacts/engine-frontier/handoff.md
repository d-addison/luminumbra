# Engine Frontier Handoff

## Current Status (updated 2026-06-10, post-execution)

The dispatch has been executed. Of the 34-task graph: T-EF-1 (material visual
gate) was implemented directly after the pre-dispatch quality gate rejected it
as oversized; 27 tasks merged through the Codex dispatch onto
`feat/engine-frontier` and were merged back to the working branch; T-EF-32
(network state hash) was implemented directly after the Codex usage limit
exhausted mid-run; T-EF-33 and T-EF-34 were executed directly (wave-8 report,
endurance revalidation).

The full debug CTest lane passes 68/68 (`ctest --preset debug
--output-on-failure -E "_NOT_BUILT$"`), including the new frontier gate
executables. All 20 engine-frontier validator gate modes pass, plus
MaterialVisual, LodGround, WaterVisual, and Smoke.

Post-merge defects found and fixed during verification:
- GPU SDF callback-safety source needle vs the runtime-toggle-strengthened
  guard (cross-task interaction).
- `world/WorldStreamingState.cpp` missing from the common sources manifest
  (latent link break, hidden until the persistence fixtures were linked).
- Dispatched gate-test fixtures were never compiled by any CMake target; now
  built and registered with CTest (frontier_gates_test, eventbus and
  persistence gate programs).
- Endurance300 asserts visible water, but auto_world_smoke ran in the default
  preset which generates no water near spawn (height_offset 20, sea level 0);
  water-asserting scenarios now run in the archipelago world.

## Remaining Open Work (quality-gate-skipped tasks)

Three tasks were skipped by the pre-dispatch token-estimate ceiling (200k,
not configurable at the project layer) because their contracts read the large
engine translation units; the Codex usage limit (resets 2026-06-11 ~05:53)
prevented re-dispatching them via the shimmed runner:

- `T-EF-6-streaming-telemetry-schema` — EnduranceStreamDrain validator mode +
  no-behavior-change streaming telemetry.
- `T-EF-8-lod-boundary-hysteresis-gate` — lod_boundary_oscillation_smoke
  scenario + LodBoundaryHysteresis mode.
- `T-EF-9-lod-seam-arrival-gate` — lod_seam_arrival_smoke scenario +
  LodSeamRisk mode.

Run them via `.forge/scripts/run-codex-engine-frontier.ps1`-style direct
`codex exec` with their dispatch.json prompts once Codex credits reset, or
implement directly following the material_visual_smoke pattern.

## Immediate Next Step

Close the three skipped gate tasks above, then start the next iteration per
the Next Iteration Directives below (optimization pass, beautification pass)
and the staged roadmap in ultimate-plan.md (RenderHealth-gated render
extraction, persistence-first frontier sequencing).

Contract execution meters against a $30/day budget. Large waves, long runtime
gates, and `Endurance300` may need to span days rather than being forced into
one budget window.

## No-Deferral Rules

- Do not defer sand rendering as flat grey, invalid material IDs, wrong texture layers, missing material heatmaps, or missing visual screenshots.
- Do not accept draw counts as proof of final visual appearance.
- Do not start render extraction before `RenderHealth` exists and passes with `MaterialVisual`, `LodGround`, and `WaterVisual`.
- Do not start scheduler/LOD policy changes before streaming telemetry and boundary/seam gates exist.
- Do not enable persistence runtime paths, GPU SDF runtime integration, far-field SDF, Aetheric simulation, GOAP/Instinct planning, Lua hot reload, or networking unless the deterministic gate for that path exists and passes.
- Do not mark endurance closed until it runs after the visual gates and records the artifacts it depends on.
- Do not commit from task execution; integration is handled by the dispatch pipeline.

## Next Iteration Directives (owner, 2026-06-10)

Two additional work streams are mandated for the iteration after this dispatch
closes, both gate-first like everything else in this roadmap:

### Optimization pass

Goal: measured, regression-gated performance improvement — no optimization
lands without a baseline artifact proving the win and a gate preventing decay.

- Baseline first: capture frame-time P50/P95/P99, meshing throughput
  (cells/ms by step), upload drain rate, generation/meshing job latency, and
  peak memory across `auto_world_smoke 300`, `lod_ground_smoke`, and a
  camera-traversal scenario. Persist as
  `build/<preset>/test-artifacts/perf/perf-baseline.json` with schema and
  thresholds; add a `PerfRegression` validator mode that fails on >10%
  regression against the committed baseline.
- Candidate targets, in expected-leverage order: meshing hot path
  (MarchingCubes table walk and vertex cache), streaming drain and coalescing
  windows, RenderPipeline per-pass GPU timers (add timers first, optimize
  second), JobSystem priority lanes / work stealing, chunk SDF generation
  batching, water sim tick cost at distance.
- Every optimization task pairs with the gate run that proves end-state
  visuals unchanged (MaterialVisual, LodGround, WaterVisual stay green).

### Beautification pass

Goal: spend the renderer's existing-but-unwired feature set and tune the
world's look, with every visual claim backed by a screenshot-classification
or RenderHealth artifact.

- Wire and gate the dormant shader suite: volumetric_lighting,
  enhanced_skybox, weather_system, caustics_generator, magical_particles,
  screen_space_reflections. One feature per task; each adds a capture
  scenario plus pixel/structural assertions (e.g. god-ray luminance shafts
  present at dawn time-of-day; SSR reflections present on calm water ROI).
- Water beauty pass (explicitly deferred from Wave 1): depth-tint curve,
  caustics integration, SSR, shoreline foam; extends
  water-visual-analysis.json rather than replacing it.
- Terrain material richness: per-material texture layers validated by the
  material heatmap gate (extend the materials array beyond Sand: Grass,
  Stone, Soil ROI entries with their own thresholds).
- Atmosphere: time-of-day sweep capture (noon/dusk/night) with per-phase
  luminance and color-balance bands; LuminCrystal emission visible in night
  captures (ties to the Aetheric frontier stream).
- Sequencing: beautification tasks run AFTER RenderHealth exists and the
  optimization baseline is captured, so visual richness never silently buys
  frame-time regressions.

## Success Definition

The handoff is complete when the three file/graph gates pass:

```powershell
forge tasks validate .forge/tasks/engine-frontier/dispatch.json
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Files
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Sections
```

Feature success requires Wave 1 visual gates green first, all later frontier streams gate-first, and terminal `Endurance300` revalidation after the visual gates.

## Architectural Principle: Engine/Game Decoupling (owner, 2026-06-10)

Two decoupled deliverables: an optimized, powerful, generic ENGINE and a GAME
concept built on it. Game-world concepts must not be baked into engine code.

- LuminCrystal is game content; the engine feature is light-emitting material
  support (emissive materials in the registry + lighting integration). Gates
  assert "emissive material visible in night capture" generically; the
  LuminCrystal entry in data/common/materials.json is merely the fixture.
- Aetheric Field is a game concept; the engine piece is a generic scalar
  field diffusion system.
- Instinct/GOAP archetypes (grovestrider etc.) are game data; the engine
  planner API stays data-driven.
- New game concepts go in data/common/, worlds/, scripts/ (Lua) — never in
  src/luminumbra_common or src/luminumbra_client.
- Existing engine code carrying game names (AethericFieldDiffusion's naming,
  grovestrider fixture data inside InstinctPlanner.cpp) are rename/relocation
  candidates for a cleanup task — do not entrench further.

## Iteration 3 Directives (owner, 2026-06-10)

Mandated streams for the iteration after iteration 2, all gate-first:

0. **World scale & fidelity (LEAD STREAM)**:
   (a) 6x+ view distance, hyper-optimized — reference model: Minecraft
   Distant Horizons. Persistent far-LOD store (low-res region data built
   from live chunks / pre-generated, persisted via WorldSaveService)
   rendered as merged static meshes decoupled from live simulation; near
   field stays full-sim. GPU SDF far-field as complementary horizon path.
   Gated by PerfRegression/GPU timers/stream-drain/memory watermarks at the
   new distance.
   (b) Player-view coverage defect: unloaded chunks/faces visible in play —
   all current visual gates use elevated downward cameras (blind spot).
   New eye-level 360-degree player_view_smoke gate + vertical-band and
   horizon coverage fixes (neighboring columns' cliff faces, RENDER
   DISTANCE UP/DOWN audit). Includes the deterministic degenerate-geometry
   chunk near (-120..-160, 180..230) seed 424242 archipelago.
   (c) Terrain shaping for "normal land": continentalness/erosion control
   noises, domain warp, spline height remap so plains/hills/mountains
   coexist; slope-histogram gate (% area below slope threshold) in the
   worldgen atlas; preset params remain game data.

1. **Client/server decoupling**: simulation authority runs headless —
   luminumbra_server (currently a stub) must tick a real world with zero
   GL/GLFW/audio. Watch items: GameSession save/load hooks live in
   main_client; keep new sim features out of client code. Gate: headless
   server tick smoke (boot world, tick N frames, emit world hash). Precedes
   networking transport work.
2. **World generation improvements**: presets already declare unbuilt
   features (biome temperature/humidity params unused, rivers_enabled,
   structures_enabled). Stream: biome system driving material/vegetation
   variation, rivers, structures, richer 3D terrain beyond
   heightfield+capped-caves. Gated via the worldgen atlas/snapshot machinery.
   Biome/structure CONTENT is game data; generation systems are engine.
3. **Character models / animations**: engine side = skeletal mesh +
   animation sampling/blending through the extracted pass architecture, with
   the asset_processor/.lmesh pipeline as the import seam; game side = the
   actual models/clips. Gates: deterministic pose-sampling test +
   skinned-mesh render capture. Pairs with the GOAP runtime phase (brains +
   bodies).
4. (Carried) Engine/game split per the Architectural Principle above; cash
   in built gates (LOD hysteresis implementation, GPU SDF runtime
   enablement, release perf lane); gameplay systems runtime; vertical
   slice; networking last.

## Iteration 2 Closeout (2026-06-10)

All five phases complete on feat/polyglot-audit-roadmap. Final state: 82/82
ctest; all 22 engine-frontier validator modes pass; runtime-stability gates
(Smoke, LodGround, WaterVisual, LodSeamRisk, LodBoundaryHysteresis,
EnduranceStreamDrain, Endurance300) green; forge verify clean.

Landed: scenario harness extraction; EnduranceStreamDrain/
LodBoundaryHysteresis/LodSeamRisk gates (closing T-EF-6/8/9); PerfRegression
with blessed median-of-3 baseline (+50%/+25% noise-honest margins, 20ms
noise floor); per-pass GPU timers; WorldSaveService + chunk dirty tracking +
runtime save/load with PersistenceRuntimeRoundtrip gate; marching-cubes
flat-edge-cache optimization (byte-identical, determinism-hash locked);
six-pass RenderPipeline extraction under empty RenderHealth diffs; streaming
drain optimization (streaming_walk p99 66->11ms, chunk_churn 77->7ms, +82%
frame rate under load); JobSystem High/Normal priority lanes; beautification
(animated caustics, improved SSR, depth tint + shoreline foam, atmospheric
skybox, weather overlay behind set_weather, noon/dusk/night sweep gate,
Grass/Stone/Soil material ROIs); scenario windows no longer steal focus.

Defects found and fixed by gate-first work: vertical-column LOD mismatch
(black seam slivers + bottom band — surface-band column LOD); corrupted
skybox cube array (106/108 floats); backface-culled skybox; sign-flipped
sun/moon directions (no sun disc had ever rendered); water tangent normals
added raw (permanent 45-degree tilt); SSR far-plane grey blotches; shoreline
foam multiplied by a black fallback (never rendered); WorldStreamingState.cpp
missing from the sources manifest; dispatched gate fixtures never compiled.

Open (carried to iteration 3, see Directives above): degenerate-geometry
chunk seed 424242 near (-120..-160, 180..230); player-view coverage
(eye-level gate + vertical-band fixes); terrain shaping; 6x+ view distance
per the Distant Horizons model. NOTE: no git remote is configured — add one
to push.

## Iteration 2 Kickoff (2026-06-10)

Execution model: Claude Code agent teams execute all implementation (Agent + Workflow, worktree isolation for parallel phases); Forge provides gates, bookkeeping (.forge/tasks/engine-iteration-2/dispatch.json, 19 tasks / 7 waves), and forge verify. Codex dispatch retired this iteration. Phase order: S1 gates || S2a perf infra -> baseline capture -> GPU timers || persistence core || meshing opt -> pass extraction || runtime persistence -> streaming/jobs optimization -> beautification tracks -> closeout. render-health-baseline.json committed as the extraction diff anchor.

