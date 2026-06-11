# Engine Iteration 4 Spec — World Identity, Surface Fidelity, Lockstep+Replay, SHIELD-RT Spike

Status: FINAL (critique applied — see engine-iteration-4/critique.md; both spec risks 1 and 3 resolved against codebase facts). Inherits (binding, do not restate here):
- Priority order + wave definitions: `.forge/artifacts/engine-roadmap/long-range-roadmap.md`
  (owner-mandated 2026-06-11) + handoff §"Iteration 4 Directives" research
  amendments.
- Research: `.forge/artifacts/engine-research/worldgen-lockstep-sdfrt.md`,
  `.forge/artifacts/engine-research/opengl-cpp-architecture.md`.
- Execution: Claude Code agent teams; Opus 4.8 implementation agents
  (`executor: opus-agent` per task), Fable planning/merges only. Forge
  gates/bookkeeping verify.
- Iteration-3 closeout state: 148/148 ctest; all validator modes green
  except MaterialVisual (deferred re-homing — owned by Wave B here).

## Theme

The engine after iteration 3 renders a complete, deterministic, headlessly-
simulated world to 1536 m. Iteration 4 makes that world *somewhere* (biomes,
rivers, structures), makes it *look real up close* (surface detail), and
makes it *shared and replayable* (lockstep transport + session replay),
while keeping the SHIELD-RT road open (spike). Engine only; zero gameplay-
loop work.

## Wave 0 — Design decisions doc (inline, Fable)

`.forge/artifacts/engine-iteration-4/design-decisions.md`, binding for all
waves: biome architecture (multi-noise selection REUSING +3/+4/+5 climate
noises, adding temperature +8 / humidity +9 ONLY — per research; biome =
f(continentalness, erosion, PV, temp, humidity) lookup table in game data);
river spec (chunk-local PV-band carve on +10, exact band ranges; NO global
drainage this iteration); structure spec (spacing/separation/salt grid +
jigsaw template pools, game-data templates); FP determinism contract
(compiler flags pinned for luminumbra_common+server: precise FP, FMA
contraction off; deterministic trig wrapper table; lint bans: libm
transcendentals + unordered-container iteration in sim code); replay format
(`LREC1`: header {schema u32, world seed, preset hash, start world_hash,
tick_rate}, then per-tick input records + periodic world_hash checkpoints
every 30 ticks; divergence = first checkpoint mismatch); transport spec
(delay-based lockstep, adaptive input horizon, server-paced; TCP loopback +
LAN; camera look render-side); texture asset pipeline decision (KTX2 or raw
mip chains via asset_processor; texture arrays NOT bindless — per research);
sub-hash registry (world_hash = fnv(terrain, entities, water, fields, rng)
exposed per-system); file-ownership map + executor assignment per task.

## Wave A — World identity (LEAD; worldgen files)

- A1 biome selection core (engine): climate sampling (+8/+9 added to the
  registry), data-driven biome table lookup (game data declares biomes;
  engine knows only the selection function), per-column biome id in the
  span cache; zero-drift proof on biome-table-absent legacy presets.
- A2 biome surface expression: per-biome material palettes + surface depth
  (game data), wired through TerrainSurfaceMaterialAt + far-tile material
  bytes (FarLodStore tile hash bump = deliberate, same commit). Gate:
  biome-coverage atlas gate (each authored biome present + material
  distribution bands at fixed seeds); FarLodHorizon stays green.
- A3 rivers: PV-band carve on +10 with bank material + waterline fill;
  chunk-local, deterministic. Gate: river-presence atlas gate (river pixels
  within PV band, connectivity within atlas window, waterline continuity);
  meshing determinism fixtures bumped deliberately if signatures move.
- A4 structures: grid placement (spacing/separation/salt) + jigsaw
  assembly from game-data template pools; placement is generation-time
  chunk-local; structures persist as edited chunks (existing machinery).
  Gate: placement determinism (same seed = same sites, locate query test) +
  structure-integrity snapshot (assembled template fixture hash).
- A5 biome audio reverb: reverb parameter per biome (game data), wired
  through EnvironmentalAudioSystem; gate extends audio telemetry artifact.
- Wave gates: full worldgen snapshot suite (deliberate bumps logged),
  slope-walkability still green on all shaped presets, PlayerView +
  FarLodHorizon + HeadlessServerTick green on a biome-enabled world,
  PerfRegression (gen cost budget: biome lookup adds < 10% to chunk gen
  p99).

## Wave B — Surface detail & material fidelity (render files; ∥ Wave A)

- B1 texture asset path: asset_processor texture import (mips, channel
  packing), GL texture-array atlas keyed by materials LUT (research: arrays
  not bindless); RenderHealth resource registry entries.
- B2 triplanar terrain texturing + normal/detail maps in g_buffer.frag
  driven by material id (LUT gains texture/normal layer indices + tiling
  params; game data). Gate: close-range material capture gate (2-8 m,
  MaterialVisual pattern: per-material albedo bands + normal-response check
  via two-light-angle captures), replaces/re-homes the deferred
  MaterialVisual scan; RenderHealth re-bless; PerfRegression (gbuffer
  budget delta < 1.0 ms at baseline view).
- B3 skinned-mesh texturing: UV + texture refs through LMS2 (additive,
  versioned bump if layout moves), normal-mapped skinned G-buffer path;
  SkinnedMeshVisual extends to assert textured response.
- B4 emissive calibration: materials LUT emission → bloom chain audit so
  authored emissive intensities map predictably to on-screen glow
  (calibration table in the artifact); CreatureSlice glow ROI assertion
  tightens.
- B5 roughness/specular variation per material (LUT column + lighting
  path); TimeOfDaySweep bands re-blessed deliberately.

## Wave C — Lockstep transport + replay (AFTER Wave A hash re-bless)

- C1 determinism-prevention contract (FIRST): pinned FP flags on
  common/server targets (build-system enforced), deterministic trig
  wrappers replacing libm in sim code, split-lint-style determinism lint
  (bans: transcendental libm calls, unordered iteration, time/random in
  sim paths); per-system sub-hashes in world_hash artifact; heavy-mode
  oracle (save/load/resim/compare) wired into HeadlessServerTick.
- C2 replay record/playback: LREC1 record on every server run; playback
  drives ServerWorldRunner to identical end-hash; divergence detection at
  checkpoints. Gate: record→replay hash equality + deliberate-divergence
  detection (mutated input file caught at first checkpoint).
- C3 transport: TCP lockstep session (server + N clients, inputs only),
  adaptive input horizon, per-tick hash exchange + desync halt with replay
  dump. Gate: loopback 2-client session, M ticks, all hashes equal;
  fault-injection test (delayed/dropped input handled by horizon).
- C4 client-over-transport: client renders a server-owned world in
  loopback; input round-trip; camera render-side. Gate: scenario smoke +
  existing visual gates unaffected.

## SHIELD-RT spike (timeboxed; isolated prototype dir + bench)

Benchmark heightfield ray-march of FarLodStore tiles VS sphere-traced
mip SDF (min-filtered conservative mips MANDATORY — success criterion) on
3-4 representative far-field views; artifact with ms/frame, ray counts,
memory; recommendation memo (tiles-as-raymarch-source vs brick SDF
clipmaps). No production wiring; evidence feeds iteration 6.

## Closing optimization wave (standing; after C merges)

From research, in expected-leverage order, each gate-ratcheted on the
RELEASE lane: O1 MDI chunk submission (persistent-mapped pooled vertex
storage + glMultiDrawElementsIndirect for live chunks + far regions, one
coherent work item incl. shadow pass) — target ≥ 1.5x render-submission
improvement release-lane; O2 JobSystem pooled POD jobs + aligned counters
(kills std::function allocation); O3 meshing per-worker arena allocators.
Each: PerfRegression + release-lane re-bless with previous block,
RenderHealth byte-stable (O1 changes HOW we draw, not WHAT).

## Closeout

Full sweep + Endurance300 + forge verify + handoff section + MaterialVisual
re-home confirmation + baseline re-bless log + iteration-5 planning-round
kickoff inputs (Atmospheric pillar lead, per roadmap).

## Risks / critique seeds

1. Wave A far-tile material bump invalidates FarLodStore pristine caches —
   migration or cache-clear policy needed (tile hash key includes
   params_hash — verify biome table hash participates).
2. B2 texture memory: array layers × authored materials vs the 64 MB far
   budget precedent — set an explicit texture-resident budget gate.
3. C1 FP flag pinning may shift existing world hashes (FMA contraction was
   previously on?) — if so EVERY golden bumps at once; must land as one
   deliberate mega-bump commit with before/after, BEFORE C2/C3 build on
   the hashes. Verify current flags first; if already precise/no-FMA, the
   contract is free.
4. Wave A ∥ B share materials.json + LUT (A2 palettes, B2 texture layers)
   — schema must be decided in Wave 0 and edits serialized (A2 first).
5. Replay tick-rate coupling: SimulationClock catch-up (max 4) means wall
   frames ≠ ticks; replay must be tick-indexed, never frame-indexed.
6. MDI (O1) vs FarLodSystem's own draw path — far regions already batch;
   O1 scope covers live chunks first, far regions only if release lane
   shows win.
