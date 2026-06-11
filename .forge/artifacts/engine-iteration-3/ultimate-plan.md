# Engine Iteration 3 — Ultimate Plan

Revision of the synthesis (`research.md`) applying every critique mitigation
(`critique.md`). Execution model: Claude Code agent teams implement; Forge
gates verify. The vision documents (current README TDD + the Project Capture
game README on `develop`) are adopted as decisions.

## Decisions (critique F2/F6 — binding)

- **Fixed simulation tick = 30 Hz** (TDD §3.2; WaterSystem concurs).
  `SimulationClock` is configurable but 30 Hz is canonical; every determinism
  gate keys off it.
- **Networking posture = input-based lockstep** (TDD §3.2): only inputs
  travel; per-tick 16-hex world-hash exchange is the desync oracle. No
  transport this iteration; the contracts pin it.
- **Far-field end-state = SHIELD-RT SDF raytracing** (TDD §2.1/§4.1). The
  far-LOD region store (DH model) is the deliberate waypoint: tiers F1 (4 m,
  512–768 m) + F2 (8 m, 768–1536 m) ONLY; F3 cut to keep the raymarch road
  open.
- **"6x" pinned**: baseline radius 12 = 192 m; requirement = **1152 m minimum
  complete visible horizon**; `farlod_horizon_smoke` asserts full coverage to
  **1536 m** (F2 outer edge), `farlod_resident_bytes < 64 MB`,
  `gbuffer_gpu_ms` delta < 1.5 ms.
- **Seed-offset registry**: shaping/warp = m_seed+3..+7;
  biomes/rivers (iteration 4) reserve +8..+10. Recorded in the Wave-0 design
  doc; collisions are a gate failure.
- **The game slice is a Project Capture slice** (critique F8): one MVP-roster
  creature, planner-driven, reacting to a light stimulus, observable in the
  archipelago world — the minimal photographable moment.

## Wave 0 — Design doc (1 task, inline)

- **T0 design-decisions doc** `.forge/artifacts/engine-iteration-3/design-decisions.md`:
  seed registry; 30 Hz; the region container spec (magic, addressing
  `r.<rx>.<rz>.lmr`, 32×32-chunk regions, record header WITH `lod_level`,
  LZ4 per-record, manifest, world-hash format-independence contract,
  durable-entity-id note for snapshots); 6x numbers; file-ownership map for
  the waves (critique F5). Gate: referenced by every Wave 1–3 task.

## Wave 1 — Enablers (file-ownership-partitioned)

Agent A (SHIELD owner, sequential): 
- **T1 SDF-skip with seam-fallback safety** (critique F3): step>1 chunks
  generate boundary-face-band SDF only (4 faces) OR the fallback patch
  emitter samples GetTerrainHeightAt — decided by evidence; LodSeamRisk +
  LodGround + meshing determinism hashes green with the skip on.
- **T2 column surface-span streaming fix + vertical-unload exemption**
  (critique F4): 5-point span cache replacing point sampling; ALL cache
  consumers enumerated and updated (T-I2-14 candidate collection, activation,
  LOD band, EnsureSurfaceReadyNear, GetInitialChunkLoadList); snapshot-test
  count expectation updated deliberately; same-PR perf re-bless with
  before/after telemetry; 8192-budget headroom proven (pairs with T1).
- **T3 player_view_smoke gate**: eye-level 360° (12 yaw + 1 peak-pitch
  stations), `missing_frustum_surface_chunks == 0`,
  `renderable_frustum_ratio >= 0.98`, `below_horizon_sky_ratio < 0.005`,
  `near_black_cluster_count == 0`; verified red on pre-T2 build; covers the
  seed-424242 degenerate chunk region. New `PlayerView` validator mode.

Agent B (parallel, disjoint files):
- **T4 SimulationClock @30 Hz** (engine, accumulator + max-tick clamp) +
  GameSession `TickSimulation()` hosting OrderedEventBus; main-loop wiring
  must not disturb existing visual-gate scenarios; eventbus order gate now
  runs hosted.
- **T5 TerrainPresetLoader**: one canonical parser (kills 4 duplicates),
  parses shaping/biomes/features/materials blocks (parsed-before-consumed),
  unknown-key warnings; all existing snapshot/determinism gates byte-stable.
- **T6 asset-manifest split**: ValidateWorldConfig simulation-only in common,
  client appends shaders/RML/fonts; headless CreateWorld gate (temp root with
  preset only); first split-enforcement act.

## Wave 2 — World scale & shaping

Agent A (far-LOD, sequential after T1–T3):
- **T7 persistence v2 region files** (against the T0 container spec): behind
  `write_snapshot()`, v1→v2 world-hash equality migration gate, `.bak`
  retention, `save_dirty_chunks` O(edited regions).
- **T8 FarLodStore + region mesher** (F1/F2 tiers, packed 6-B samples,
  pristine=cache / edited=authoritative, edited-chunk downsample on unload;
  generalized heightfield mesher per region tile, 28-B VoxelVertex).
- **T9 far render path + scheduler + seam gate**: regions drawn in G-buffer
  pass post-live-chunks, AABB culling, ring-diff scheduler on Normal lane,
  64 MB LRU; **dedicated live/far boundary seam gate** (the DH failure mode)
  + `farlod_horizon_smoke` (`FarLodHorizon` mode) at the pinned numbers; far
  tile determinism hashes (pregen == from-live on pristine terrain).

Agent C (worktree, parallel — worldgen files only):
- **T10 terrain shaping core**: ComputeShapedHeight (continentalness/erosion/
  peaks-valleys + domain warp + monotone splines), default-off,
  zero-hash-drift proof on legacy params, batch/sample parity under the
  1e-4 snapshot gate.
- **T11 shaping presets + slope-histogram gate**: preset `shaping` block
  (game data), mountains.json schema_rev 2 (deliberate golden/hash bumps),
  atlas slope-percentile gate incl. **normal-land fraction
  (slope<20°, habitable heights) > 0.25**, relief bimodality check.

## Wave 3 — Authority + characters (after Wave 1; parallel to late Wave 2 where files allow)

Agent D (server):
- **T12 server target in build** (links luminumbra_common only, hygiene gate:
  no gl/glfw/miniaudio/imgui/rmlui under src/luminumbra_server).
- **T13 ServerWorldRunner + headless_server_smoke**: boot from preset/save
  (LoadWorldState-before-generation contract), spawn-anchor streaming,
  collision-ready, 30 Hz fixed loop, autosave/shutdown save; never registers
  GPU SDF callback; `luminumbra.server_tick.v1` artifact with in-process
  determinism double-run (`world_hash == world_hash_replay`);
  `HeadlessServerTick` mode. Meshing stays ON (StreamingProfile deferred).

Agent E (characters, worktree):
- **T14 skeletal import**: `.lmesh` v2 under NEW `LMS2` magic (v1
  byte-identical), joints/weights u8x4, ≤256 joints; sibling `.lanim`
  (joint-name-hash keyed); cgltf skin/anim parsing + meshoptimizer remap;
  round-trip gtest.
- **T15 animation core + pose determinism gate**: Skeleton/Clip/Pose structs,
  pure SamplePose/BlendPoses, CPU sampling on the 30 Hz tick, palette SSBO
  GPU skinning; G1 checksum gate (debug==release, scalar math).
- **T16 skinned G-buffer stage** (`skinned_mesh.vert` + g_buffer.frag inside
  GBufferPass; non-instanced; fix the instanced rotation-drop and hardcoded
  MaterialID=3 bugs); `skinned_mesh_visual_smoke` capture gate + RenderHealth
  re-bless.
- **T17 planner runtime + relocations + split-lint** (atomic cluster):
  InstinctSystem over EnTT on the tick; NeedsComponent migration micro-task
  with EngineContracts updated atomically; grovestrider fixture →
  game-data JSON (with `expected` block), EntitySnapshot fixture → test
  support, AethericFieldDiffusion → fields/ScalarFieldDiffusion (alias +
  dual-schema gates, alias dropped at close); **split-lint gate** (path lint
  + game-noun grep on new src/ lines, allowlist); planner gate inverts:
  engine source contains no content strings.
- **T18 Project Capture creature slice**: one rigged MVP-roster creature +
  idle/walk clips in game data, archetype-spawned, planner-driven, reacting
  to an emissive/light stimulus (Glimmer-stone precursor via the materials
  LUT emission path if cheap, else a placed emissive block), observable via
  free camera in archipelago; capture artifact proves behavior change on
  stimulus.

Kept small extras (slotted where the owner-agent has slack):
- **T19 LOD hysteresis + gate ratchet** (the ratchet commit is the
  deliverable).
- **T20 release perf lane** (script + release-blessed baseline +
  preset-aware Test-PerfRegression).
- **T21 UI unsubscribe use-after-free fix** (token Subscribe/Unsubscribe +
  destroy-then-mutate test).
- **T22 closeout**: full sweep, Endurance300, handoff, forge verify,
  baseline re-bless log.

## Deferred to iteration 4 (recorded, critique F1/F10)

Biomes, rivers, structures, GPU SDF live-parity + enablement, Lua
bindings/hot-reload, multi-anchor streaming, StreamingProfile meshing-skip,
WorldList, audio reverb (trigger: biomes), F3 far tier, camera/photography
systems (lenses/DoF/shutter — the game's core loop, deliberately after
creatures exist to photograph), seasons/wind-grid (Atmospheric pillar — pairs
with instanced foliage).

## Success Definition

Iteration 3 is done when: `PlayerView` green (complete terrain in an
eye-level 360° at both presets); `FarLodHorizon` green at 1536 m / <64 MB /
<1.5 ms; slope-histogram normal-land floor green on shaped mountains;
`HeadlessServerTick` deterministic double-run green; pose-determinism +
skinned-capture green; the creature slice artifact shows stimulus-driven
behavior; split-lint active; all prior tests (82+) and validator modes
(22+new) green; PerfRegression holds (with the two deliberate re-blesses
logged). The far-field end-state remains SHIELD-RT raytracing — this
iteration ships its waypoint, not its replacement.
