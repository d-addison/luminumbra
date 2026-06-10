## Subsystem State

- The compiled client audio stack is `AudioManagerFactory`, `AudioPropagationSystem`, `AudioSpatialCluster`, `EnvironmentalAudioSystem`, and `MiniaudioManager`; staged headers for streaming, advanced reverb, profiler, procedural sound, and variation logic are not in the client source manifest.
- The runtime has a device-free path: `--no-audio` selects `NullAudioManager`, while the scenario artifact system already writes `last-known-runtime.json` and `runtime-frames.json` for world/render/job telemetry.
- The working core is miniaudio playback, bank loading, listener transform, simple environmental volume scaling, ambient zones, spatial source bookkeeping, Jolt audio raycasts, propagation estimates, and physics batch query plumbing.
- The shipping-risk area is not "no audio code"; it is mismatched integration. Several advanced features are declared or documented, but their outputs are not applied to miniaudio, not serialized to deterministic artifacts, or not backed by material data.

## Findings

- Active build scope is narrower than the advanced header set: `src/luminumbra_client/sources.cmake:7` to `src/luminumbra_client/sources.cmake:11` compile only the core audio `.cpp` files, while `AdvancedReverbSystem.h`, `AudioStreamingManager.h`, `AudioPerformanceProfiler.h`, `SoundVariationSystem.h`, and `ProceduralSoundGenerator.h` have no corresponding built `.cpp` entries.
- The doppler/reverb TODO in clustering is real integration debt: `AudioSpatialCluster` requests full processing for Ultra/High detail at `src/luminumbra_client/audio/AudioSpatialCluster.cpp:287` to `src/luminumbra_client/audio/AudioSpatialCluster.cpp:295`, but `CalculateFullAttenuation` only applies distance plus optional occlusion and then stops at `src/luminumbra_client/audio/AudioSpatialCluster.cpp:327` to `src/luminumbra_client/audio/AudioSpatialCluster.cpp:334`.
- Cluster output is not connected back to playback. `MiniaudioManager::Update` calls `m_spatial_cluster->Update(...)` at `src/luminumbra_client/audio/MiniaudioManager.cpp:34` to `src/luminumbra_client/audio/MiniaudioManager.cpp:42`, and the cluster mutates `last_calculated_attenuation` at `src/luminumbra_client/audio/AudioSpatialCluster.cpp:283` to `src/luminumbra_client/audio/AudioSpatialCluster.cpp:311`, but `MiniaudioManager` never reads that value to set miniaudio volume, pitch, filter, or reverb send.
- Physics occlusion can be silently disabled even after `SetPhysicsSystem`. `MiniaudioManager::SetPhysicsSystem` only forwards the pointer at `src/luminumbra_client/audio/MiniaudioManager.cpp:495` to `src/luminumbra_client/audio/MiniaudioManager.cpp:498`, but `AudioSpatialCluster::Update` runs batched occlusion only when `m_raycast_callback` is set at `src/luminumbra_client/audio/AudioSpatialCluster.cpp:75` to `src/luminumbra_client/audio/AudioSpatialCluster.cpp:78`; the physics-system path inside `CalculateBatchedOcclusion` at `src/luminumbra_client/audio/AudioSpatialCluster.cpp:337` to `src/luminumbra_client/audio/AudioSpatialCluster.cpp:387` is therefore bypassed by the normal manager integration.
- Batched occlusion has lifetime and double-work risks. Queued callbacks capture raw `AudioSource*` at `src/luminumbra_client/audio/AudioSpatialCluster.cpp:357` to `src/luminumbra_client/audio/AudioSpatialCluster.cpp:374`, while inactive sources are erased on update at `src/luminumbra_client/audio/AudioSpatialCluster.cpp:29` to `src/luminumbra_client/audio/AudioSpatialCluster.cpp:36`; the synchronous LOD path can also calculate occlusion before batch queuing at `src/luminumbra_client/audio/AudioSpatialCluster.cpp:327` to `src/luminumbra_client/audio/AudioSpatialCluster.cpp:329`.
- Terrain-material absorption is hard-coded and mismatched with the material registry. `data/common/materials.json:5` to `data/common/materials.json:64` defines IDs for Air, Stone, Soil, Grass, Sand, Deepslate, LuminCrystal, and Water, but `PhysicsSystem::audio_raycast` maps hit height to material types 0/1/2 at `src/luminumbra_common/systems/PhysicsSystem.cpp:392` to `src/luminumbra_common/systems/PhysicsSystem.cpp:402`, and `get_material_audio_absorption` maps type 6 to Water even though registry ID 6 is LuminCrystal at `src/luminumbra_common/systems/PhysicsSystem.cpp:483` to `src/luminumbra_common/systems/PhysicsSystem.cpp:492`.
- Reflection correctness is provisional. Audio raycasts return a default upward normal at `src/luminumbra_common/systems/PhysicsSystem.cpp:388` to `src/luminumbra_common/systems/PhysicsSystem.cpp:390`, reflection rays use that normal at `src/luminumbra_common/systems/PhysicsSystem.cpp:462` to `src/luminumbra_common/systems/PhysicsSystem.cpp:474`, and propagation assigns fixed reflection loss rather than raycast material absorption at `src/luminumbra_client/audio/AudioPropagationSystem.cpp:175` to `src/luminumbra_client/audio/AudioPropagationSystem.cpp:183`.
- Reverb is mostly declarative. `MiniaudioManager::SetGlobalReverb` logs only at `src/luminumbra_client/audio/MiniaudioManager.cpp:391` to `src/luminumbra_client/audio/MiniaudioManager.cpp:394`, event reverb logs instead of processing at `src/luminumbra_client/audio/MiniaudioManager.cpp:456` to `src/luminumbra_client/audio/MiniaudioManager.cpp:461`, while the advanced convolution API is only declared at `src/luminumbra_client/audio/AdvancedReverbSystem.h:61` to `src/luminumbra_client/audio/AdvancedReverbSystem.h:96`.
- Headless smoke artifacts do not expose audio telemetry. `NullAudioManager` returns success/no-ops at `src/luminumbra_client/main_client.cpp:193` to `src/luminumbra_client/main_client.cpp:213`, `--no-audio` is parsed at `src/luminumbra_client/main_client.cpp:240` to `src/luminumbra_client/main_client.cpp:246`, but `RuntimeStateRecorder` serializes launch flags, memory, chunk, job, render, shader, GL debug, and readiness fields with no audio object at `src/luminumbra_client/main_client.cpp:642` to `src/luminumbra_client/main_client.cpp:675`.
- Current streaming behavior is miniaudio flags, not the staged streaming manager. Normal `PlayEvent` decodes into memory via `MA_SOUND_FLAG_DECODE` at `src/luminumbra_client/audio/MiniaudioManager.cpp:140` to `src/luminumbra_client/audio/MiniaudioManager.cpp:149`, music streams at `src/luminumbra_client/audio/MiniaudioManager.cpp:254` to `src/luminumbra_client/audio/MiniaudioManager.cpp:263`, and ambient loops stream at `src/luminumbra_client/audio/MiniaudioManager.cpp:406` to `src/luminumbra_client/audio/MiniaudioManager.cpp:416`; `AudioStreamingManager` declares cache/chunk policies at `src/luminumbra_client/audio/AudioStreamingManager.h:89` to `src/luminumbra_client/audio/AudioStreamingManager.h:123` but is not compiled.
- 3D one-shots have an object-lifetime defect. `PlayOneShot` allocates a local `ma_sound`, starts it, and returns without storing or uninitializing the sound at `src/luminumbra_client/audio/MiniaudioManager.cpp:213` to `src/luminumbra_client/audio/MiniaudioManager.cpp:239`.
- Bank/data capabilities exceed the loader. The event definition struct includes layers, procedural flags, adaptive properties, and variation controls at `src/luminumbra_client/audio/MiniaudioManager.h:33` to `src/luminumbra_client/audio/MiniaudioManager.h:78`, and complex banks use layered/procedural fields at `data/audio/complex_environmental_sfx.bank.json:6` to `data/audio/complex_environmental_sfx.bank.json:89`, but `LoadBank` parses only files, volume, pitch variation, 2D/looping, 3D distances, doppler factor, and basic reverb at `src/luminumbra_client/audio/MiniaudioManager.cpp:94` to `src/luminumbra_client/audio/MiniaudioManager.cpp:117`.
- Variation memory accounting is planned but unenforced. Variation data stores repeated file paths and layers at `data/audio/sound_variation_definitions.bank.json:27` to `data/audio/sound_variation_definitions.bank.json:82`, while `SoundVariationSystem` keeps full groups, a selection cache, learning data, and recent histories at `src/luminumbra_client/audio/SoundVariationSystem.h:188` to `src/luminumbra_client/audio/SoundVariationSystem.h:263`; its metrics expose `memory_usage_bytes` at `src/luminumbra_client/audio/SoundVariationSystem.h:161` to `src/luminumbra_client/audio/SoundVariationSystem.h:170`, but the system is not compiled.
- Profiler coverage exists on paper but is not a runtime contract. The profiler schema includes active sounds, occlusion/reflection counts, memory, streaming, reverb, and underruns at `src/luminumbra_client/audio/AudioPerformanceProfiler.h:35` to `src/luminumbra_client/audio/AudioPerformanceProfiler.h:78`, and integration hooks are declared at `src/luminumbra_client/audio/AudioPerformanceProfiler.h:287` to `src/luminumbra_client/audio/AudioPerformanceProfiler.h:316`, but no built source wires them into runtime artifacts.

## Must-Fix

- Close the spatial-cluster effect loop before shipping enhanced spatial audio: per-handle attenuation, occlusion, doppler pitch/velocity, and reverb-send outputs must be applied by `MiniaudioManager` or the cluster must stop claiming those responsibilities.
- Fix physics integration activation: `SetPhysicsSystem` must be sufficient for occlusion/reflection raycasts, without requiring an unrelated callback, and queued callbacks must not capture invalid `AudioSource*` across source removal.
- Replace height-threshold material absorption with the material registry. `materials.json` needs audio absorption/reflection coefficients, and Jolt audio raycasts need to return registry IDs plus real surface normals.
- Fix 3D one-shot lifetime by retaining active fire-and-forget sounds until completion or by using a miniaudio fire-and-forget path equivalent to `PlayOneShot2D`.
- Add deterministic no-device audio telemetry to the runtime artifact schema. Minimum counters: active voices, active 3D sources, cluster count and cluster sizes, propagation raycasts queued/processed/dropped, streaming cache bytes, decoded bytes, underruns, and last audio init mode.
- Validate or reject unsupported audio-bank fields. Layered, procedural, adaptive, filter, variation, and reverb fields must not be silently accepted by `LoadBank` unless they have runtime behavior and memory accounting.

## Deepening Opportunities

- Make doppler complete by tracking previous source/listener positions and velocities, using real frame delta instead of the fixed `1.0f/60.0f` cluster update input, and proving the pitch ratio against a deterministic source/listener sweep.
- Make reverb complete in two tiers: first, deterministic early-reflection/reverb-send parameters from `AudioPropagationSystem`; second, optional convolution through `AdvancedReverbSystem` once CPU/memory budgets and artifacts exist.
- Turn batched raycasts into an audio budgeted subsystem: expose queued/processed/deferred/dropped counts, batch priority distribution, and per-frame max query cost instead of relying only on `PhysicsSystem::BatchedPhysicsQueries` internals.
- Implement the streaming manager only after defining ring-buffer semantics: fill level, low-water refill, high-water backpressure, underrun count, decoder bytes, compressed cache bytes, resident decoded bytes, and eviction events.
- Add variation memory budgets before enabling variation banks broadly: intern repeated strings, cap selection cache/training data, and report per-group memory so rich contextual banks do not become unbounded content debt.
- Wire `AudioPerformanceProfiler` into the runtime recorder or replace it with a smaller `AudioTelemetrySnapshot` interface shared by real and null audio managers.

## Frontier Proposals

- Gate-first: convolution reverb from world geometry. Generate deterministic impulse-response metadata from propagation rays, material coefficients, and room analysis before enabling real-time convolution; only then route wet audio through `AdvancedReverbSystem`.
- Gate-first: procedural ambience from biome, water, and weather state. Drive `ProceduralSoundGenerator` and `EnvironmentalAudioSystem` from world biome/water telemetry, but first validate a parameter trace rather than subjective playback.
- Gate-first: audio golden-trace regression. Record event IDs, handles, positions, selected variations, cluster assignments, raycast counts, attenuation, doppler ratio, reverb send, stream fill, and underruns for fixed scenarios and diff the JSON trace in CI.

## Proposed Gates

- `audio_spatial_cluster_headless`: deterministic scenario with `--scenario audio_spatial_cluster_headless --no-audio`; emits `audio-telemetry.json` (`luminumbra.audio_telemetry.v1`) asserting active voices, active 3D sources, cluster count, cluster sizes, and zero underruns.
- `audio_occlusion_material_box`: deterministic physics fixture with Stone, Soil, Grass, Sand, Deepslate, LuminCrystal, and Water panels; emits `audio-physics.json` and runs `validate-audio-physics --mode occlusion-materials` to assert registry-backed absorption and raycast queue counts.
- `audio_reflection_room`: deterministic rectangular room scenario; emits `audio-propagation.json` with first reflection points, normals, path distances, material IDs, absorption, and delay_ms; validator fails if default up-normal reflection is used.
- `audio_doppler_sweep`: no-device scenario with fixed source/listener velocity curves; emits `audio-golden-trace.json` and validates expected doppler ratios, monotonic pitch changes, and stable per-frame attenuation.
- `audio_reverb_ir_box`: gate-first convolution scenario; emits `audio-ir.json` with RT60 estimate, early reflection delay set, IR length, CPU budget, and IR checksum before real-time convolution is enabled.
- `audio_streaming_memory_budget`: validator mode `validate-audio-bank --mode streaming-memory-budget`; emits `audio-memory.json` with decoded_bytes, streaming_cache_bytes, compressed_cache_bytes, ring_buffer_fill_min/max, evictions, and underruns.
- `audio_bank_schema_enhanced_v1`: validator mode `validate-audio-bank --mode enhanced-v1`; emits `audio-bank-validation.json` and fails if layered/procedural/adaptive/filter/reverb fields are accepted without supported runtime capability flags.
- `auto_world_smoke_audio_null`: extend the existing `auto_world_smoke --no-audio` lane; `last-known-runtime.json` and `runtime-frames.json` must include an `audio` object with deterministic null counters and `init_mode: "null"`.

## References

- src/luminumbra_client/audio/MiniaudioManager.h
- src/luminumbra_client/audio/MiniaudioManager.cpp
- src/luminumbra_client/audio/AudioSpatialCluster.h
- src/luminumbra_client/audio/AudioSpatialCluster.cpp
- src/luminumbra_client/audio/AudioPropagationSystem.h
- src/luminumbra_client/audio/AudioPropagationSystem.cpp
- src/luminumbra_client/audio/AdvancedReverbSystem.h
- src/luminumbra_client/audio/EnvironmentalAudioSystem.h
- src/luminumbra_client/audio/EnvironmentalAudioSystem.cpp
- src/luminumbra_client/audio/AudioStreamingManager.h
- src/luminumbra_client/audio/AudioPerformanceProfiler.h
- src/luminumbra_client/audio/SoundVariationSystem.h
- src/luminumbra_client/audio/ProceduralSoundGenerator.h
- src/luminumbra_client/audio/IAudioManager.h
- src/luminumbra_client/audio/AudioManagerFactory.h
- src/luminumbra_client/audio/AudioManagerFactory.cpp
- src/luminumbra_common/components/AudioComponents.h
- src/luminumbra_common/systems/PhysicsSystem.h
- src/luminumbra_common/systems/PhysicsSystem.cpp
- src/luminumbra_client/main_client.cpp
- src/luminumbra_client/sources.cmake
- src/luminumbra_common/sources.cmake
- data/common/materials.json
- data/audio/sound_variation_definitions.bank.json
- data/audio/complex_environmental_sfx.bank.json
- docs/ENHANCED_AUDIO_SYSTEM.md
- docs/ENHANCED_AUDIO_PHYSICS_INTEGRATION.md
- .forge/artifacts/engine-framework-roadmap/ultimate-plan.md
- .forge/specs/RUNTIME-STABILITY-PHASE-1-2026-06-08.md
- .forge/reports/polyglot-audit/tooling.md
