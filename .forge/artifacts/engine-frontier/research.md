## Consensus

Luminumbra is no longer a paper engine. It has working chunk generation, streaming, LOD meshing, deferred rendering, water submission and visibility gates, a miniaudio-backed core, RmlUi menu flows, useful worldgen/render/UI tests, and Forge runtime artifacts that can explain readiness, uploads, jobs, GL debug state, and screenshots. The strongest areas are the mature world/rendering loop and the recent visual-performance work: `LodGround` and `WaterVisual` now prove large LOD holes and basic visible water are gated closed in short deterministic runs.

The cross-panel consensus is that the engine still cannot be called shippable because the remaining blockers are gate and contract gaps, not raw feature absence. Ordered by severity:

1. Material appearance is still ungated. Sand versus grey fallback, invalid material IDs, and texture-layer agreement can still mask or invalidate conclusions from world streaming and rendering captures.
2. Long-run runtime stability is not closed. Short `LodGround` and `WaterVisual` runs drain cleanly, but visible endurance still needs revalidation after the material gate, with rolling upload/job/LOD drain budgets.
3. World streaming is still batch-oriented and partly FIFO-driven. It needs endurance-tail contracts, LOD boundary churn gates, seam-arrival gates, lane telemetry, and backlog budgets before larger scheduling changes.
4. Rendering has a useful pass shape but monolithic ownership. Pass extraction, water quality, shader inventory, and render graph work are unsafe until `MaterialVisual`, `LodGround`, `WaterVisual`, and render-health baselines all pass.
5. Physics/player determinism and collision correctness are not contracts yet. Variable-dt stepping, unordered collision creation, stale collider replacement, and heightfield-vs-mesh divergence block replay, lockstep, and reliable traversal claims.
6. Audio has a working core but incomplete integration. Spatial-cluster outputs, occlusion activation, material absorption, one-shot lifetime, no-device telemetry, and enhanced bank fields must be made truthful before advanced audio is claimed.
7. Simulation, Lua, Aetheric Field, Instinct AI, persistence, GPU SDF, and networking are frontier systems. They are mostly stubs, disconnected hooks, or unsafe paths and must enter only behind small deterministic gates.
8. UI/tooling/tests contain lifecycle and enforcement gaps. RmlUi is the active UI despite stale ImGui documentation, property/listener lifetimes leak, visible WorldList controls lie or are stubbed, coverage is configured but not enforced, and test selection metadata is under-specified.

No panel proposal is rejected in this synthesis. Proposals that are too broad for the immediate iteration are staged behind their named gates. The only rejected claim is the stale documentation claim that a Dear ImGui replacement is complete; source and tests show RmlUi is still the active UI path.

## Emphasis

Ranked work streams:

1. Material/texture-layer visual gate, first. Implement and pass `MaterialVisual` before accepting further visual claims, endurance conclusions, shader polish, or render extraction. Sand ROI, material-ID or texture-layer heatmap, final-color plausibility, and zero GL debug errors are mandatory.
2. Mature world/streaming deepening. Keep extending the already-working chunk/LOD/upload loop with endurance drain, LOD hysteresis gates, seam-arrival gates, lane telemetry, and backlog budgets.
3. Mature rendering and visual loop continuation. Preserve `LodGround` and `WaterVisual`, freeze render-health baselines, inventory shaders, and add water quality ladders only after the material gate is stable.
4. Physics/player correctness. Fix fixed-tick stepping, stale collider replacement, authoritative collision policy, crouch state synchronization, water traversal contract, and audio-query fairness before replay or lockstep claims.
5. Audio integration and telemetry. Make the active miniaudio path truthful, connect spatial outputs to playback, gate no-device telemetry, and reject unsupported bank fields.
6. UI/tooling/test reliability. Fix UI binding/listener lifetimes, truthful WorldList controls, screenshot smoke, shader/asset manifests, labels, sanitizers, and coverage enforcement.
7. Simulation contracts. EventBus ordering, Lua API manifests, Aetheric diffusion, and Instinct planning are Wave 2-style contracts, not gameplay breadth.
8. Persistence, GPU SDF, and networking. Start with world/chunk round-trip, GPU callback safety/parity, deterministic world hashes, and loopback authority. Do not enable runtime GPU SDF, far-field SDF, sockets, prediction, or global field streaming until the gates pass.

The emphasis is conservative: deepen mature systems first, continue the visual-quality loop immediately, then stage frontier systems gate-first.

## World Generation and Streaming

Must-fix list:

- Close the endurance drain gap. `Endurance300` must prove rolling and final readiness, zero active generation/meshing, drained job queue, zero terrain/water upload priority inversions, and bounded or zero deferred upload tails after the camera path stabilizes.
- Gate LOD boundary oscillation before retuning LOD thresholds. Runtime LOD currently changes directly at thresholds, so the gate must measure mesh-version churn, pending LOD count, upload backlog, coverage, and screenshots while hovering around LOD0/LOD1 and LOD1/LOD2 boundaries.
- Gate transition-skirt and seam correctness under asynchronous neighbor arrival. Skirts are created from neighbor state visible at dispatch time; late finer neighbors must be detectable by transition-face masks and screenshot/pixel checks.
- Add real job telemetry. Current stats cannot distinguish generation, meshing, water, upload-copy pressure, wait time, worker occupancy, or starvation.
- Make sustained generation/meshing backlog a failure, not only final drain. Long camera paths need rolling-window budgets.
- Finish the material/texture-layer visual gate first because grey fallback can invalidate world-streaming visual conclusions.

Deepening list:

- Split runtime scheduling into priority lanes for surface generation, collision-near generation, LOD replacement meshing, water-only remesh, and far-horizon fill.
- Add LOD hysteresis with a no-hole replacement rule that keeps the old renderable mesh until the replacement CPU mesh and GPU upload are complete.
- Make water upload budgets adaptive using the existing water candidate/deferred telemetry.
- Add screen-space size or camera-velocity upload priority as a tiebreaker after distance.
- Reduce LOD0 meshing cost with reusable worker scratch, flat edge-cache arrays, SIMD sign masks, and allocation-light material queries.
- Add incremental remeshing hooks for localized edits, caves, erosion, and water-only changes.
- Extend terrain mesh stats into p50/p95/p99 by LOD, active-cell ratio, empty/full chunk counts, and payload bytes.
- Treat `EnsureSurfaceReadyNear` as a deterministic boot/join builder, not a proxy for long-run runtime streaming.

Frontier list and carried gates:

- Gate-first GPU terrain meshing: `gpu_meshing_parity_smoke`, `gpu-meshing-parity.json`, validator mode `GpuMeshingParity`.
- Gate-first async SDF readback: `async_sdf_readback_smoke`, `async-sdf-readback.json`, validator mode `AsyncSdfReadback`.
- Gate-first predictive prefetch: `predictive_prefetch_replay`, `predictive-prefetch-analysis.json`, validator mode `PredictivePrefetch`.
- Gate-first job-system priority lanes plus work stealing: `job_lane_contention_smoke`, `job-lane-contention.json`, validator mode `JobLaneContention`.
- Gate-first meshlet/indirect terrain streaming: `terrain_meshlet_stream_smoke`, `terrain-meshlet-stream.json`, validator mode `TerrainMeshletStream`.

Panel proposed gates retained:

- `endurance_stream_drain`: writes `build/debug/test-artifacts/runtime/endurance-stream-drain/stream-drain-analysis.json`; validator `.forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode EnduranceStreamDrain`.
- `streaming_upload_stress_smoke`: writes `build/debug/test-artifacts/runtime/streaming-upload-stress/streaming-upload-drain.json`; validator `-Mode StreamingUploadDrain`.
- `lod_boundary_oscillation_smoke`: writes `build/debug/test-artifacts/runtime/lod-boundary-oscillation/lod-boundary-hysteresis.json`; validator `-Mode LodBoundaryHysteresis`.
- `lod_seam_arrival_smoke`: writes `build/debug/test-artifacts/runtime/lod-seam-arrival/lod-seam-risk.json`; validator `-Mode LodSeamRisk`.
- `meshing_throughput_budget_smoke`: writes `build/debug/test-artifacts/performance/meshing-throughput-budget.json`; validator `.forge/scripts/validate-performance-framework-phase-5.ps1 -Mode MeshingThroughputBudget`.
- `job_contention_smoke`: writes `build/debug/test-artifacts/performance/job-contention-runtime-stats.json`; validator `validate-performance-framework-phase-5.ps1 -Mode JobContention`.
- `material_terrain_heatmap_smoke`: writes `build/debug/test-artifacts/runtime/material-terrain/material-terrain-heatmap-analysis.json`; validator `.forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode MaterialTerrainHeatmap`.
- `async_sdf_readback_smoke`: writes `build/debug/test-artifacts/runtime/async-sdf-readback/async-sdf-readback.json`; validator `-Mode AsyncSdfReadback`.

## Rendering and Visual Quality

Must-fix list:

- Implement `material_visual_smoke` as the first Wave 1 task. It must produce `build/debug/test-artifacts/runtime/material-visual/material-visual-analysis.json`, `screenshots/material-visual.ppm`, and `screenshots/material-id-heatmap.ppm`.
- Require Sand as a mandatory ROI with material ID 4 and expected texture layer 3. The classifier must require material heatmap agreement plus final-color plausibility and must fail on grey fallback pixels.
- Add shader-suite health that classifies every shader as `wired`, `compiled_only`, `dead_asset`, or explicitly exempted, instead of only reporting the active core programs.
- Freeze a render-health baseline before modularizing `RenderPipeline.cpp`: pass metadata, pass stats, shader health, GL debug counters, MaterialVisual, LodGround, WaterVisual, and screenshots.
- Convert water quality claims into feature-specific gates before accepting SSR, caustics, foam, or depth tint as complete.
- Keep zero GL debug errors as a hard visual-gate requirement.

Deepening list:

- Extract render passes in stages only after baselines: shadow, G-buffer/material bindings, SSAO, lighting/water, skybox/final blit/post shell.
- Make `materials.json` the material source of truth for IDs, texture layer paths, and LUT values.
- Promote material-ID heatmap capture into a reusable debug mode for material IDs, LOD holes, seams, and biome/material transitions.
- Replace water fallback inputs incrementally: generated caustics, real normal/flow maps, then foam, each with a feature-specific gate.
- Add per-pass GL state assertions during debug runs.
- Generalize screenshot analysis helpers for ROI bounds, connected components, thresholds, and heatmap correlation.

Frontier list and carried gates:

- Gate-first render graph abstraction. It must prove identical pass metadata, draw counts, resource dimensions, GL debug results, and screenshot classifications before replacing the imperative order.
- Gate-first GPU timing telemetry per pass, with query availability and disablement validated.
- Gate-first screenshot-diff regression harness using structured ROIs and heatmap classifications rather than fragile whole-frame diffs.
- Gate-first shader feature matrix. New feature shaders need deterministic visual gates or explicit inactive/exempt status.
- Gate-first water quality ladder for visibility, low-angle reflection, shallow caustics, depth tint, and foam.

Panel proposed gates retained:

- Material visual gate: scenario `material_visual_smoke`; artifact `build/debug/test-artifacts/runtime/material-visual/material-visual-analysis.json`; validator `.forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual`.
- Material heatmap gate: same scenario; artifact `screenshots/material-id-heatmap.ppm`; validator `.forge/scripts/validate-engine-frontier.ps1 -Mode MaterialVisual`.
- LOD visual regression gate: scenario `lod_ground_smoke`; artifact `build/debug/test-artifacts/runtime/lod-ground-baseline/lod-ground-visual-analysis.json`; validator mode `LodGround`.
- Water visibility gate: scenario `water_visual_smoke`; artifact `build/debug/test-artifacts/runtime/water-visual/water-visual-analysis.json`; validator mode `WaterVisual`.
- Water quality gate: scenario `water_quality_smoke` or `water_visual_smoke --water-quality-profile low_angle_ssr`; artifact `build/debug/test-artifacts/runtime/water-quality/water-quality-analysis.json`; validator mode `WaterQuality`.
- Shader suite health gate: validator mode `ShaderSuiteHealth`; artifact `build/debug/test-artifacts/render/shader-suite-health.json`.
- Render pass no-behavior-change gate: scenarios `lod_ground_smoke`, `water_visual_smoke`, and `material_visual_smoke`; artifact `build/debug/test-artifacts/render/render-health-analysis.json`; validator mode `RenderHealth`.
- Pass timing telemetry gate: scenario `render_timing_smoke`; artifact `build/debug/test-artifacts/performance/render-pass-timing.json`; validator mode `RenderTiming`.
- Screenshot regression gate: scenario `visual_regression_smoke`; artifact `build/debug/test-artifacts/runtime/visual-regression/visual-regression-analysis.json`; validator mode `VisualRegression`.
- Dead shader inventory gate: validator mode `ShaderInventory`; artifact `build/debug/test-artifacts/render/shader-inventory.json`.

## Physics and Player

Must-fix list:

- Add a fixed-tick physics contract before replay, lockstep, or deterministic runtime claims. Player movement and `PhysicsSystem::update` need an accumulator, tick index, fixed substep count, max catch-up policy, and deterministic state hash.
- Fix stale collider replacement. A terrain mesh/LOD version change must rebuild or remove the existing Jolt body instead of returning early and marking stale collision as current.
- Define authoritative collision geometry. If caves, overhangs, skirts, and marching-cubes repairs are traversable, LOD0 collision must come from the same authoritative surface; otherwise content must gate those surfaces as non-traversable.
- Synchronize crouch shape and state. The player should update crouch state and eye height only after the physics shape transition succeeds.
- Define the player-water contract: swim/wade state using water level/flow data, or validator/content rules proving required routes do not depend on swimming.
- Add audio physics query fairness: max queue age, deadline/aging priority, queue length telemetry, and drop/expire policy.

Deepening list:

- Add coyote time and jump buffering after fixed tick exists, expressed in ticks.
- Define step-up, step-down, slope, and ground-snap expectations for terrain.
- Extend runtime artifacts with collision readiness, body counts, stale body counts, attached mesh version, near-field collision coverage, and collision build time.
- Add deterministic ordering for audio queries with sequence/tick tie-breakers.
- Add a water-aware controller mode with buoyancy, drag, current influence, and transition thresholds.

Frontier list and carried gates:

- Gate-first deterministic physics replay with input events, fixed ticks, transforms, velocities, collision add/remove events, and hashes.
- Gate-first runtime physics smoke across slopes, ledges, chunk boundaries, water, and crouch tunnel.
- Gate-first character-controller obstacle course for coyote time, jump buffer, step-up/down, crouch, stand-up, and water transitions.
- Gate-first collision churn stress across LOD boundaries and forced remeshes.
- Gate-first audio physics query budget under mixed-priority raycast overload.

Panel proposed gates retained:

- `-Mode PhysicsReplay`: scenario `physics_replay_gate`; artifact `build/<preset>/test-artifacts/runtime/physics-replay/physics-replay-endstate.json`.
- `-Mode PhysicsSmoke`: scenario `physics_smoke`; artifact `build/<preset>/test-artifacts/runtime/physics-smoke/physics-smoke.json`.
- `-Mode PlayerObstacleCourse`: scenario `player_obstacle_course`; artifact `build/<preset>/test-artifacts/runtime/player-obstacle-course/player-obstacle-course.json`.
- `-Mode ChunkCollisionLifecycle`: scenario `chunk_collision_churn`; artifact `build/<preset>/test-artifacts/runtime/chunk-collision-lifecycle/chunk-collision-lifecycle.json`.
- `-Mode AudioPhysicsQueries`: scenario `audio_physics_query_budget`; artifact `build/<preset>/test-artifacts/runtime/audio-physics-query-budget/audio-physics-query-budget.json`.
- `-Mode WaterPlayer`: scenario `water_player_interaction`; artifact `build/<preset>/test-artifacts/runtime/water-player/water-player-interaction.json`.

## Audio

Must-fix list:

- Close the spatial-cluster effect loop. Attenuation, occlusion, doppler pitch/velocity, and reverb-send outputs must affect playback handles or stop being claimed.
- Make `SetPhysicsSystem` sufficient for occlusion/reflection raycasts and remove invalid raw `AudioSource*` callback captures.
- Replace height-threshold material absorption with registry-backed material audio coefficients and real surface normals.
- Fix 3D one-shot lifetime by retaining sounds until completion or using a valid fire-and-forget path.
- Add deterministic no-device audio telemetry to runtime artifacts: active voices, 3D sources, clusters, raycasts queued/processed/dropped, streaming/decoded bytes, underruns, and init mode.
- Validate or reject unsupported audio-bank fields. Layered, procedural, adaptive, filter, variation, and reverb fields must not be silently accepted without runtime behavior and memory accounting.

Deepening list:

- Complete doppler with previous source/listener positions, velocities, real frame delta, and deterministic sweep validation.
- Complete reverb in tiers: deterministic send/early reflection parameters first, optional convolution later.
- Turn batched raycasts into an audio-budgeted subsystem with queued/processed/deferred/dropped counts and max frame cost.
- Implement the streaming manager only after ring-buffer semantics, backpressure, underrun, decoder, and cache telemetry exist.
- Add variation memory budgets with interned strings, cache caps, training-data caps, and per-group memory reporting.
- Wire `AudioPerformanceProfiler` into runtime recording or replace it with a small shared `AudioTelemetrySnapshot`.

Frontier list and carried gates:

- Gate-first convolution reverb from world geometry.
- Gate-first procedural ambience from biome, water, and weather state.
- Gate-first audio golden-trace regression.

Panel proposed gates retained:

- `audio_spatial_cluster_headless`: scenario `--scenario audio_spatial_cluster_headless --no-audio`; emits `audio-telemetry.json`.
- `audio_occlusion_material_box`: physics fixture with Stone, Soil, Grass, Sand, Deepslate, LuminCrystal, and Water panels; emits `audio-physics.json`; validator `validate-audio-physics --mode occlusion-materials`.
- `audio_reflection_room`: emits `audio-propagation.json` with reflection points, normals, distances, material IDs, absorption, and delay.
- `audio_doppler_sweep`: emits `audio-golden-trace.json` validating doppler ratios, pitch monotonicity, and attenuation.
- `audio_reverb_ir_box`: emits `audio-ir.json` with RT60, early reflections, IR length, CPU budget, and checksum.
- `audio_streaming_memory_budget`: validator `validate-audio-bank --mode streaming-memory-budget`; emits `audio-memory.json`.
- `audio_bank_schema_enhanced_v1`: validator `validate-audio-bank --mode enhanced-v1`; emits `audio-bank-validation.json`.
- `auto_world_smoke_audio_null`: extends `auto_world_smoke --no-audio`; `last-known-runtime.json` and `runtime-frames.json` include deterministic `audio` counters and `init_mode: "null"`.

## Simulation, AI, and Scripting

Must-fix list:

- Implement deterministic EventBus ordering plus replay recording before Aetheric, Instinct, or Lua publish cross-system state.
- Add a minimal Lua API manifest and sandboxed loader before running gameplay scripts.
- Repair sample-script contract defects before promoting scripts to gates: `action_find_water.lua` must represent water/thirst, and random movement must use engine deterministic RNG or precomputed commands.
- Add the smallest Aetheric C++ model before tying gameplay to shader output: chunk-local fixed-point Lumin/Umbra scalar grid, source injection, diffusion, sampling, and checksum.
- Add the smallest Instinct planner before claiming GOAP behavior: needs to goal selection to bounded action plan with deterministic tie-breaks and one need-satisfaction scenario.

Deepening list:

- Extend `materials.json` with validated `aetheric_emission` metadata for `LuminCrystal` and future flora.
- Drive `crystal_field_effect.frag` from deterministic field/source artifacts only after the unit field exists; until then it remains visual-only.
- Build a script-contract validator for `scripts/common` that rejects banned APIs and unknown engine calls.
- Use job batches only for read-only planner/diffusion work after single-thread deterministic tests pass; commit results sorted by tick, phase, entity ID, and sequence.
- Add AI/event trace artifacts for goal choice, action starts/completions/failures, script digest, and field sample values before adding more behavior.

Frontier list and carried gates:

- EventBus v1: typed simulation event queue with `{tick, phase, sequence, source_entity, event_type, payload}`, stable subscription ordering, and replay JSON.
- Aetheric Field v1: chunk-local fixed-point two-channel diffusion with deterministic source injection and checksums. No global streaming, player redirection tools, GPU simulation, Radiance Cascade integration, or cross-chunk conservation in this iteration.
- Instinct planner v1: deterministic `NeedsComponent` to goal scores to action graph search; commit sorted by entity ID.
- Lua v1: deterministic gameplay iteration APIs only; exclude `os`, `io`, `debug`, wall-clock time, uncontrolled `math.random`, threads, coroutines, and direct registry iteration.
- Hot reload v1: validate in isolation, swap only at tick boundaries, emit `LuaScriptReloaded`, and retain old digest on failure.
- Gate-first sequencing: `simulation_eventbus_order_replay_v1`, `lua_api_manifest_common_v1`, `aetheric_chunk_diffusion_single_crystal_v1`, then `instinct_grovestrider_hunger_v1`.

Panel proposed gates retained:

- `simulation_eventbus_order_replay_v1`: unit test `EventBusDeterministicOrderTest`; emits `eventbus-replay.json`; validator `.forge/scripts/validate-engine-frontier.ps1 -Mode SimulationEventBus`.
- `aetheric_chunk_diffusion_single_crystal_v1`: unit test `AethericFieldDiffusionTest.SingleLuminCrystal16Ticks`; emits `aetheric-field-diffusion.json`.
- `aetheric_lumincrystal_emission_visual_v1`: scenario `--scenario aetheric_lumincrystal_emission_smoke`; emits `aetheric-visual.json` and `aetheric-lumincrystal.ppm`; validator `-Mode AethericVisual`.
- `instinct_grovestrider_hunger_v1`: test `InstinctPlannerNeedSatisfactionTest.GroveStriderHunger`; emits `instinct-grovestrider-hunger.json`.
- `lua_api_manifest_common_v1`: validator `.forge/scripts/validate-engine-frontier.ps1 -Mode LuaApiManifest`; emits `lua-api-manifest.json`.
- `lua_hot_reload_tick_boundary_v1`: unit test `LuaHotReloadGateTest.TickBoundaryRollback`; emits `lua-hot-reload.json`.
- `simulation_event_trace_cross_run_v1`: validator `.forge/scripts/validate-engine-frontier.ps1 -Mode SimulationTrace`; emits `simulation-event-trace.json`.

## Persistence, GPU SDF, and Networking

Must-fix list:

- Persist and reload complete world identity: `worldId`, `spawnPoint`, stable numeric seed, world format version, preset digest, and schema version.
- Add chunk persistence for mutable worlds: versioned header, coordinates, dimensions, section table, uncompressed sizes, LZ4-compressed SDF/heightmap/water sections, checksum, atomic write, and strict decode errors. Mesh and collision stay derived.
- Add dirty tracking and world-system persistence APIs for dirty chunks, revision/hash, flush-on-evict, explicit save-all, and sorted active-chunk snapshots.
- Add minimal ECS snapshot support for stable entity IDs plus durable `TransformComponent`, `TagComponent`, `StaticMeshComponent`, and parent/child links.
- Keep GPU SDF disabled until callback safety and GPU/CPU parity gates exist. Validate callback output and never call GL compute from worker generation jobs.
- Replace networking/server stubs with deterministic loopback authority before sockets: fixed tick, ordered inputs, server-applied inputs, state hash broadcast, and disconnect-free local transport.

Deepening list:

- Move from per-chunk files to region files only after chunk files pass gates; add index, tombstones, compact/repair, and region checksums later.
- Add compression telemetry for raw bytes, compressed bytes, ratios, encode/decode time, and checksum failures.
- Promote a reusable sorted `WorldHash` utility for persistence, networking, replay, and endurance soak.
- Add GPU SDF overlay and counters: attempted/succeeded/failed callbacks, CPU fallbacks, readback time, queue depth, and parity deltas.
- Reuse screenshot comparison helpers for CPU-vs-GPU render parity.

Frontier list and carried gates:

- `chunk_persistence_v1`: per-chunk `.lchunk` files under `worlds/saves/<worldId>/chunks/`.
- `world_entity_snapshot_v1`: durable entity snapshot for stable IDs and limited component subset.
- `gpu_sdf_compute_parity_v1`: actual headless GL compute parity for the known seed/chunk cases.
- `gpu_sdf_runtime_toggle_v1`: render-thread GPU SDF queue plus safe callback/counters, no far-field raymarch.
- `far_field_sdf_slice_v1`: one narrow debug render mode, one camera, one screenshot artifact.
- `loopback_authority_v1`: in-memory local authority for two clients, no sockets, prediction, NAT, or session UI.
- `network_state_hash_v1`: hash authoritative state per tick.

Panel proposed gates retained:

- `world_persistence_round_trip_v1`: scenario `persistence_round_trip_seed_4242`; test `WorldPersistenceRoundTripTest.GeneratedMutatedWorldReloadsIdentically`; artifact `build/<preset>/test-artifacts/persistence/world-persistence-roundtrip.json`; validator `.forge/scripts/validate-engine-frontier.ps1 -Mode PersistenceRoundTrip`.
- `chunk_format_validator_v1`: test `ChunkPersistenceFormatTest.RejectsBadMagicVersionChecksumAndTruncation`; artifact `build/<preset>/test-artifacts/persistence/chunk-format-validation.json`; validator `-Mode ChunkFormat`.
- `deterministic_world_hash_v1`: test `WorldHashTest.SortedChunksAndEntitiesStableAcrossInsertionOrder`; artifact `build/<preset>/test-artifacts/persistence/world-hash.json`; validator `-Mode WorldHash`.
- `gpu_sdf_callback_safety_v1`: test `SdfGpuCallbackTest.InvalidCallbackResultsFallBackOrFailClosed`; artifact `build/<preset>/test-artifacts/render/gpu-sdf-callback-safety.json`; validator `-Mode GpuSdfCallbackSafety`.
- `gpu_sdf_compute_parity_v1`: test `SdfGpuCpuParityTest.GpuComputeMatchesCpuKnownSeeds`; artifact `build/<preset>/test-artifacts/render/gpu-sdf-compute-parity.json`; validator `-Mode GpuSdfComputeParity`.
- `gpu_sdf_runtime_visual_parity_v1`: scenario `gpu_sdf_runtime_parity`; artifacts `gpu-sdf-cpu.ppm`, `gpu-sdf-gpu.ppm`, and `gpu-sdf-runtime-parity.json`; validator `-Mode GpuSdfRuntimeParity`.
- `far_field_sdf_visual_v1`: scenario `far_field_sdf_slice`; artifact `build/<preset>/test-artifacts/render/far-field-sdf.json` and `far-field-sdf.ppm`; validator `-Mode FarFieldSdf`.
- `network_loopback_authority_v1`: scenario `network_loopback_two_clients_seed_9001`; test `LoopbackAuthorityTest.TwoSessionsConverge`; artifact `build/<preset>/test-artifacts/network/network-loopback-convergence.json`; validator `-Mode NetworkLoopback`.
- `reload_soak_v1`: scenario `persistence_reload_soak_100`; artifact `build/<preset>/test-artifacts/persistence/reload-soak.json`; validator `-Mode ReloadSoak`.

## UI, Tooling, and Tests

Must-fix list:

- Implement subscription tokens for `Property<T>` and store them in a component-owned bag so component destruction releases callbacks deterministically.
- Convert all component property binds to tokens: base binds, Button binds, and WorldList binds.
- Add Rml event-listener tokens that call `RemoveEventListener` before deleting listeners, used by `UIComponent` and `Rml_UIManager`.
- Complete or hide visible broken WorldList commands. Loading state, zero-world empty state, generated favorite/delete hookup, and truthful recent/favorite filters must not lie to users. Favorite or creation-date controls should be hidden until save metadata supports them.

Deepening list:

- Add UI screenshot smoke for `main_menu.rml`, `world_creation.rml`, and `world_selection.rml` with nonblank, occupied tile, and clipped-text-risk metrics.
- Expand asset processor corpus: external buffers, node transforms, optional attribute fallback, larger index types, malformed input, and one real asset.
- Make shader gates manifest-driven so runtime declarations, link specs, shader health, and `res/shaders` cannot drift.
- Broaden worldgen snapshots across authored presets and seed corpus `{424242, 1337, 9001}`.
- Add CTest labels and presets for `fast`, `ui`, `tooling`, and `frontier-artifacts`.
- Turn the configured 95 percent coverage ambition into an actual first-party fast-target coverage lane.

Frontier list and carried gates:

- Gate-first headless UI interaction harness from `data/ui/tests/main_menu_flow.json`, with screenshots and pixel metrics.
- Gate-first static test-artifact dashboard indexing existing JSON/PPM outputs without deciding pass/fail.
- Gate-first test-only lifecycle telemetry for live property subscriptions, Rml event listeners, loaded documents, and destroyed component counts.

Panel proposed gates retained:

- `UiBindingLifecycleTest.ComponentDestroyReleasesPropertySubscriptions`: writes `test-artifacts/ui/ui_binding_lifecycle.json`.
- `UiEventListenerLifecycleTest.DocumentReloadDoesNotGrowLiveListeners`: writes `test-artifacts/ui/ui_event_lifecycle.json`.
- `WorldListComponentTest.FiltersStatesAndSortsAreTruthful`: writes `test-artifacts/ui/world_list_state_matrix.json`.
- `UiScreenshotSmokeTest.AuthoredMenusRenderStablePixels`: writes PPM snapshots and `test-artifacts/ui/ui_screenshots.json`; validator `validate_ui_artifacts.ps1 -Mode screenshots`.
- `AssetProcessorRoundTrip.CorpusCasesWriteManifest`: writes `test-artifacts/tooling/asset_processor_round_trip.json`; validator `validate_tool_artifacts.ps1 -Mode asset_processor`.
- `RenderSmokeTest.ShaderManifestMatchesRuntimePrograms`: writes `test-artifacts/render_framework/shader_manifest_validation.json`; validator `validate_render_artifacts.ps1 -Mode shaders`.
- `WorldGenLayerSnapshotTest.SeedPresetMatrixExportsSnapshots`: writes `test-artifacts/worldgen/worldgen_layers_matrix.json`; validator `validate_worldgen_artifacts.ps1 -Mode matrix`.
- `CoverageGate.FirstPartyFastTargetsMeetThreshold`: writes `test-artifacts/coverage/coverage_summary.json`; validator `validate_coverage.ps1 -Threshold 95 -Mode first-party-fast`.
- `SanitizerGate.DebugAsanPresetCoversAllFirstPartyTests`: writes `test-artifacts/sanitizers/asan_ctest.json`; validator `validate_sanitizers.ps1 -Mode asan`.
- `CTestMetadataGate.AllTestsHaveLabelsAndPresets`: writes `test-artifacts/testing/ctest_manifest.json` and fails missing labels or presets.
