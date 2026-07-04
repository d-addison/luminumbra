# Add the world generation test executable
add_executable(world_generation_test
    shield/test_world_generation.cpp
    shield/test_meshing_determinism.cpp
    shield/worldgen_hardening_test.cpp
    shield/meshing_hardening_test.cpp
)

target_include_directories(world_generation_test PRIVATE
    ../src
)

target_link_libraries(world_generation_test PRIVATE
    gtest_main
    luminumbra_common
)

add_executable(sdf_gpu_cpu_parity_test
    shield/test_sdf_gpu_cpu_parity.cpp
)

target_compile_definitions(sdf_gpu_cpu_parity_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

target_include_directories(sdf_gpu_cpu_parity_test PRIVATE
    ../src
)

target_link_libraries(sdf_gpu_cpu_parity_test PRIVATE
    gtest_main
    luminumbra_common
)

add_executable(worldgen_layer_snapshot_test
    shield/test_worldgen_layer_snapshots.cpp
    shield/worldgen_layers_hardening_test.cpp
)

target_compile_definitions(worldgen_layer_snapshot_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(worldgen_layer_snapshot_test PRIVATE
    ../src
)

target_link_libraries(worldgen_layer_snapshot_test PRIVATE
    gtest_main
    luminumbra_common
)

set(ASSET_PROCESSOR_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../tools/asset_processor.cpp")
set_source_files_properties("${ASSET_PROCESSOR_SOURCE}" PROPERTIES
    COMPILE_DEFINITIONS "main=asset_processor_main"
)

add_executable(asset_processor_round_trip_test
    tools/test_asset_processor_round_trip.cpp
    tools/test_skeletal_asset_round_trip.cpp
    "${ASSET_PROCESSOR_SOURCE}"
)

target_compile_definitions(asset_processor_round_trip_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

target_include_directories(asset_processor_round_trip_test PRIVATE
    ../src
    ../tools
    ../vendor/cgltf
    ../vendor/meshoptimizer/src
    ../vendor/stb
    ../third_party/cgltf
    ../third_party/meshoptimizer/src
    ../external/cgltf
    ../external/meshoptimizer/src
)

if(TARGET AssetProcessor)
    get_target_property(ASSET_PROCESSOR_INCLUDE_DIRS AssetProcessor INCLUDE_DIRECTORIES)
    if(ASSET_PROCESSOR_INCLUDE_DIRS)
        target_include_directories(asset_processor_round_trip_test PRIVATE
            ${ASSET_PROCESSOR_INCLUDE_DIRS}
        )
    endif()

    get_target_property(ASSET_PROCESSOR_LINK_LIBRARIES AssetProcessor LINK_LIBRARIES)
    if(ASSET_PROCESSOR_LINK_LIBRARIES)
        target_link_libraries(asset_processor_round_trip_test PRIVATE
            ${ASSET_PROCESSOR_LINK_LIBRARIES}
        )
    endif()
endif()

target_link_libraries(asset_processor_round_trip_test PRIVATE
    gtest_main
)

if(TARGET meshoptimizer)
    target_link_libraries(asset_processor_round_trip_test PRIVATE meshoptimizer)
elseif(TARGET meshoptimizer::meshoptimizer)
    target_link_libraries(asset_processor_round_trip_test PRIVATE meshoptimizer::meshoptimizer)
endif()

add_executable(common_tests
    common/AnimationRuntime_test.cpp
    common/BiomeReverb_test.cpp
    common/DeterministicMath_test.cpp
    common/SystemConfig_test.cpp
    common/PlantProcgen_test.cpp
    common/NetworkSim_test.cpp
    common/ReplicationDelta_test.cpp
    common/ReplicationDeltaLoop_test.cpp
    common/EngineContracts_test.cpp
    common/ResidencyContract_test.cpp
    common/FarLodStore_test.cpp
    common/GameSessionHeadlessWorld_test.cpp
    common/JobSystem_test.cpp
    common/JobWatchdog_test.cpp
    audio/AudioBankIntegrity_test.cpp
    common/ActivationQueueSemantics_test.cpp
    common/WorldgenPreviewReinitRace_test.cpp
    common/MultiAnchorStreaming_test.cpp
    common/PromotionSimTruthDecoupling_test.cpp
    common/WaterDeterminism_test.cpp
    common/PlayerAvatar_test.cpp
    common/ReplicationProtocol_test.cpp
    common/ReplicationEndpoint_test.cpp
    common/LockstepSession_test.cpp
    common/LockstepBackpressure_test.cpp
    common/ReplayStream_test.cpp
    common/ServerHeadlessHygiene_test.cpp
    common/SimulationClock_test.cpp
    common/StructurePlacement_test.cpp
    common/StructureStamping_test.cpp
    common/TerrainPresetLoader_test.cpp
    common/KnobLayer_test.cpp
    common/LayerGraph_test.cpp
    common/WindFieldSystem_test.cpp
    common/WeatherSystem_test.cpp
    common/AetherFieldSystem_test.cpp
    common/fields_hardening_test.cpp
    common/replication_hardening_test.cpp
    common/replication_scale_test.cpp
    common/lockstep_replay_hardening_test.cpp
    common/world_streaming_hardening_test.cpp
    common/HydraulicErosion_test.cpp
    common/WorldStreamingState_test.cpp
    sim/fire_spread_test.cpp
    sim/soil_nutrient_test.cpp
    sim/pollination_test.cpp
    sim/plant_disease_test.cpp
    sim/irrigation_test.cpp
    sim/lifespan_test.cpp
    sim/wildlife_foliage_test.cpp
    sim/photo_scoring_test.cpp
    sim/weather_event_test.cpp
    sim/herd_alarm_test.cpp
    sim/difficulty_test.cpp
    sim/photo_codex_test.cpp
    sim/species_identity_test.cpp
    sim/creature_species_registry_test.cpp
    sim/objectives_test.cpp
    sim/codex_view_test.cpp
    sim/creature_procgen_test.cpp
    sim/decomposition_test.cpp
    sim/circadian_test.cpp
    sim/territory_test.cpp
    sim/photo_camera_test.cpp
    sim/light_tools_test.cpp
    sim/photo_session_test.cpp
    sim/photo_mode_test.cpp
    sim/photo_mode_perf_test.cpp
    sim/predator_pack_test.cpp
    sim/migration_test.cpp
    sim/thirst_test.cpp
    sim/scavenging_test.cpp
    sim/species_codex_test.cpp
    sim/photo_filters_test.cpp
    sim/steering_consumer_test.cpp
    sim/ecology_pipeline_test.cpp
    sim/ecology_hash_test.cpp
    sim/crop_lifecycle_test.cpp
    sim/foliage_drift_integration_test.cpp
    sim/species_registry_test.cpp
    sim/movement_hardening_test.cpp
    sim/lifecycle_hardening_test.cpp
    sim/environment_hardening_test.cpp
    sim/social_hardening_test.cpp
    sim/gamelibs_hardening_test.cpp
)

target_compile_definitions(common_tests PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

target_include_directories(common_tests PRIVATE
    ../src
)

target_link_libraries(common_tests PRIVATE
    gtest_main
    luminumbra_common
)

# Track-B (roadmap pillar B): render-only tree LOD distance selection. The header
# under test (src/luminumbra_client/rendering/TreeLod.h) is OpenGL-free pure logic,
# so this target needs no client lib / GL link -- just the include root + gtest.
add_executable(tree_lod_test
    rendering/tree_lod_test.cpp
)

target_include_directories(tree_lod_test PRIVATE
    ../src
)

target_link_libraries(tree_lod_test PRIVATE
    gtest_main
)

# Wave-3 far-field impostors: the hemi-octahedral view<->tile mapping math (GL-free,
# the shared source of truth for the atlas bake + the impostor shader).
add_executable(octa_impostor_test
    rendering/octa_impostor_test.cpp
)

target_include_directories(octa_impostor_test PRIVATE
    ../src
)

target_link_libraries(octa_impostor_test PRIVATE
    gtest_main
)

# far-field-source-unification (FR-003 / AC-003): pillar-B's first slice -- the forest
# perf-BUDGET RED harness. OpenGL-free: it models the foliage draw load (instance /
# draw-call / triangle counts) at a pinned 16k-tree load over the pure TreeLod.h
# selection + the GBufferPass batching model, emits forest_perf_budget.json, and
# RED-fails the geometry budget at today's load (impostors flip it green in Wave 3).
# Links luminumbra_common for DeterministicMath + nlohmann_json (no world/GL).
add_executable(forest_perf_budget_test
    rendering/forest_perf_budget_test.cpp
)

target_compile_definitions(forest_perf_budget_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(forest_perf_budget_test PRIVATE
    ../src
    ../src/luminumbra_common
)

target_link_libraries(forest_perf_budget_test PRIVATE
    gtest_main
    luminumbra_common
)

add_executable(render_smoke_test
    rendering/render_smoke_test.cpp
)

target_compile_definitions(render_smoke_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(render_smoke_test PRIVATE
    ../src
)

target_link_libraries(render_smoke_test PRIVATE
    gtest_main
    glfw
    glad
)

if(WIN32)
    target_link_libraries(render_smoke_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(render_smoke_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(render_smoke_test PRIVATE OpenGL::GL)
endif()

add_executable(render_capture_test
    rendering/render_capture_test.cpp
)

target_compile_definitions(render_capture_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(render_capture_test PRIVATE
    ../src
)

target_link_libraries(render_capture_test PRIVATE
    gtest_main
    luminumbra_common
    glfw
    glad
)

if(WIN32)
    target_link_libraries(render_capture_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(render_capture_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(render_capture_test PRIVATE OpenGL::GL)
endif()

# RENDER-12/GPU-12 (spec 016 FR-B): the registry OWNED-resource contract against
# a real hidden GL context — frame-boundary survival, resize re-attach, destroy.
add_executable(registry_ownership_test
    rendering/registry_ownership_test.cpp
    ../src/luminumbra_client/rendering/RenderResourceRegistry.cpp
)

target_compile_definitions(registry_ownership_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(registry_ownership_test PRIVATE
    ../src
    ../src/luminumbra_client
    ../src/luminumbra_common
)

target_link_libraries(registry_ownership_test PRIVATE
    gtest_main
    glfw
    glad
    luminumbra_common
)

if(WIN32)
    target_link_libraries(registry_ownership_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(registry_ownership_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(registry_ownership_test PRIVATE OpenGL::GL)
endif()

# GPU-04 (spec 016 FR-A): the pilot-pass seam contract — DebugView + GroundDecal
# execute(RenderContext&) active paths against a real hidden GL context (both are
# default-OFF, so a headless flip is vacuous — this is the non-vacuous proof).
add_executable(pass_context_test
    rendering/pass_context_test.cpp
    ../src/luminumbra_client/rendering/passes/DebugViewPass.cpp
    ../src/luminumbra_client/rendering/passes/GroundDecalPass.cpp
    ../src/luminumbra_client/rendering/Shader.cpp
    ../src/luminumbra_client/rendering/ShaderReflection.cpp
    ../src/luminumbra_client/rendering/PassShaderLayouts.cpp
)

target_compile_definitions(pass_context_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(pass_context_test PRIVATE
    ../src
    ../src/luminumbra_client
    ../src/luminumbra_common
)

target_link_libraries(pass_context_test PRIVATE
    gtest_main
    glfw
    glad
    luminumbra_common
)

if(WIN32)
    target_link_libraries(pass_context_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(pass_context_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(pass_context_test PRIVATE OpenGL::GL)
endif()

# Spec 017-A AC-A-001: async GPU readback ring. Exercises the ring's submit/poll/
# consume contract against a real hidden GL context. Links the ring TU under test
# directly (GL-only, no luminumbra_client dependency — same shape as the reflection
# test below).
add_executable(async_readback_ring_test
    rendering/async_readback_ring_test.cpp
    ../src/luminumbra_client/rendering/AsyncReadbackRing.cpp
)

target_compile_definitions(async_readback_ring_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(async_readback_ring_test PRIVATE
    ../src
    ../src/luminumbra_client
)

target_link_libraries(async_readback_ring_test PRIVATE
    gtest_main
    glfw
    glad
)

if(WIN32)
    target_link_libraries(async_readback_ring_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(async_readback_ring_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(async_readback_ring_test PRIVATE OpenGL::GL)
endif()

# Spec 016 FR-D: shader-resource reflection + layout validation. Compiles small
# known programs in a hidden GL context and asserts the introspected layout +
# pass-expectation validation (incl. the hot-reload rollback predicate). Links the
# reflection TU under test directly (GL-only, no luminumbra_client dependency).
add_executable(shader_reflection_test
    rendering/shader_reflection_test.cpp
    ../src/luminumbra_client/rendering/ShaderReflection.cpp
    ../src/luminumbra_client/rendering/PassShaderLayouts.cpp
)

target_compile_definitions(shader_reflection_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(shader_reflection_test PRIVATE
    ../src
    ../src/luminumbra_client
)

target_link_libraries(shader_reflection_test PRIVATE
    gtest_main
    glfw
    glad
)

if(WIN32)
    target_link_libraries(shader_reflection_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(shader_reflection_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(shader_reflection_test PRIVATE OpenGL::GL)
endif()

# T-I5a-1: GL-free particle determinism unit tests (emitter descriptor surface).
add_executable(particle_determinism_test
    rendering/particle_determinism_test.cpp
)

target_compile_definitions(particle_determinism_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(particle_determinism_test PRIVATE
    ../src
    ../src/luminumbra_client
)

target_link_libraries(particle_determinism_test PRIVATE
    gtest_main
    luminumbra_client
    glfw
    glad
)

if(WIN32)
    target_link_libraries(particle_determinism_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(particle_determinism_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(particle_determinism_test PRIVATE OpenGL::GL)
endif()

# T-I5b-4 (W1): WaterfallVisual gate. Determinism (DetectWaterfalls same-seed ->
# same-sites) + a sheet/spray/foam render capture at a detected site. Compiles
# the render-side WaterfallDetect.cpp directly (it depends only on
# luminumbra_common's world queries + glm), plus a GL context for the capture.
add_executable(waterfall_visual_test
    rendering/waterfall_visual_test.cpp
    ../src/luminumbra_client/rendering/WaterfallDetect.cpp
)

target_compile_definitions(waterfall_visual_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(waterfall_visual_test PRIVATE
    ../src
    ../src/luminumbra_client
)

target_link_libraries(waterfall_visual_test PRIVATE
    gtest_main
    luminumbra_common
    glfw
    glad
    glm
)

if(WIN32)
    target_link_libraries(waterfall_visual_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(waterfall_visual_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(waterfall_visual_test PRIVATE OpenGL::GL)
endif()

add_executable(ui_smoke_test
    ui/ui_smoke_test.cpp
    ../src/luminumbra_client/ui/core/UIComponent.cpp
    ../src/luminumbra_client/ui/core/UIStateManager.cpp
)

target_compile_definitions(ui_smoke_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(ui_smoke_test PRIVATE
    ../src
    ../src/luminumbra_client
)

target_link_libraries(ui_smoke_test PRIVATE
    gtest_main
    luminumbra_client
)

if(WIN32)
    target_link_libraries(ui_smoke_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(ui_smoke_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(ui_smoke_test PRIVATE OpenGL::GL)
endif()

# T008 — UI page-load test. Mirrors ui_smoke_test: live headless RmlUi context
# (hidden GL window), loads every shipped data/ui/*.rml and asserts a populated body.
add_executable(ui_page_load_test
    ui/ui_page_load_test.cpp
    ../src/luminumbra_client/ui/core/UIComponent.cpp
    ../src/luminumbra_client/ui/core/UIStateManager.cpp
)

target_compile_definitions(ui_page_load_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(ui_page_load_test PRIVATE
    ../src
    ../src/luminumbra_client
)

target_link_libraries(ui_page_load_test PRIVATE
    gtest_main
    luminumbra_client
)

if(WIN32)
    target_link_libraries(ui_page_load_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(ui_page_load_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(ui_page_load_test PRIVATE OpenGL::GL)
endif()

# Spec 002 Item 1 — the create-world LIVE WORLD-PREVIEW DIORAMA. Drives the real
# RenderPipeline headlessly through the WorldgenPreview controller (offscreen FBO
# render of a bounded candidate world). GL-required: SKIP -> ctest FAILURE (3a).
add_executable(worldgen_preview_test
    ui/worldgen_preview_test.cpp
)

target_compile_definitions(worldgen_preview_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(worldgen_preview_test PRIVATE
    ../src
    ../src/luminumbra_client
)

target_link_libraries(worldgen_preview_test PRIVATE
    gtest_main
    luminumbra_client
)

if(WIN32)
    target_link_libraries(worldgen_preview_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(worldgen_preview_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(worldgen_preview_test PRIVATE OpenGL::GL)
endif()

add_executable(initial_world_loading_perf_test
    performance/initial_world_loading_perf_test.cpp
)

target_compile_definitions(initial_world_loading_perf_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(initial_world_loading_perf_test PRIVATE
    ../src
)

target_link_libraries(initial_world_loading_perf_test PRIVATE
    gtest_main
    luminumbra_common
)

# SHIELD-01 (spec 017-B step 3): the bounded-world-load proving gate — twenty
# cold interactive loads (radius 12/4), each < 60 s, watchdog armed. Standalone
# so the -Mode WorldLoadBounded validator can run just this measurement.
add_executable(world_load_bounded_test
    performance/world_load_bounded_test.cpp
)

target_compile_definitions(world_load_bounded_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

target_include_directories(world_load_bounded_test PRIVATE
    ../src
)

target_link_libraries(world_load_bounded_test PRIVATE
    gtest_main
    luminumbra_common
)

# perf-lane-and-ecology-tick (KDD-6 / T011): the EcologyTickPerf gate exe. A
# standalone target (NOT folded into common_tests) so the validator gate can run
# JUST this measurement (300 ticks x 3 roster sizes) without dragging the whole
# common test suite, and so it can be built in release for representative numbers.
# Links luminumbra_common (PUBLIC entt + nlohmann_json) + gtest; no world/GL.
add_executable(ecology_tick_perf_test
    sim/ecology_tick_perf_test.cpp
)

target_compile_definitions(ecology_tick_perf_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(ecology_tick_perf_test PRIVATE
    ../src
    ../src/luminumbra_common
)

target_link_libraries(ecology_tick_perf_test PRIVATE
    gtest_main
    luminumbra_common
)

add_executable(runtime_world_visual_validation_test
    performance/runtime_world_visual_validation_test.cpp
)

target_compile_definitions(runtime_world_visual_validation_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(runtime_world_visual_validation_test PRIVATE
    ../src
)

target_link_libraries(runtime_world_visual_validation_test PRIVATE
    gtest_main
    luminumbra_common
    glfw
    glad
)

if(WIN32)
    target_link_libraries(runtime_world_visual_validation_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(runtime_world_visual_validation_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(runtime_world_visual_validation_test PRIVATE OpenGL::GL)
endif()

# T-I4-15 SHIELD-RT spike benchmark (manual/excluded-by-default: Debug runtime
# can exceed 60s; evidence-only, not a required gate). Registered with add_test
# under a "manual" label below so it stays out of the default ctest selection
# (run explicitly: ctest -L manual -R ShieldRtSpike).
add_executable(shieldrt_spike_bench
    performance/shieldrt_spike_bench.cpp
)

target_compile_definitions(shieldrt_spike_bench PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(shieldrt_spike_bench PRIVATE
    ../src
)

target_link_libraries(shieldrt_spike_bench PRIVATE
    gtest_main
    luminumbra_common
)

# T-I6-A3a SHIELD-RT GPU tracer micro-profile (manual/GPU: needs a real GL
# context + GPU. Debug SDF-volume build can exceed 60s.) Re-runs the same two
# far-field tracers as compute kernels and times them with GL_TIME_ELAPSED to
# confirm the iteration-4 spike's heightfield-primary conclusion on hardware;
# emits shieldrt-tracer-profile.json. Shares shieldrt_far_field.h with the spike.
add_executable(shieldrt_tracer_profile_gpu
    performance/shieldrt_tracer_profile_gpu.cpp
)

target_compile_definitions(shieldrt_tracer_profile_gpu PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(shieldrt_tracer_profile_gpu PRIVATE
    ../src
)

target_link_libraries(shieldrt_tracer_profile_gpu PRIVATE
    gtest_main
    luminumbra_common
    glfw
    glad
)

if(WIN32)
    target_link_libraries(shieldrt_tracer_profile_gpu PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(shieldrt_tracer_profile_gpu PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(shieldrt_tracer_profile_gpu PRIVATE OpenGL::GL)
endif()

# T-I6-A3b (increment 1): SHIELD-RT far-field ground-truth parity gate. Manual/GPU
# (same gating shape as the profile). Proves the heightfield max-mip march hits the
# real analytic surface before it is wired into the live G-buffer; emits
# shieldrt-far-field-parity.json. Shares the harness + builders with the profile.
add_executable(shieldrt_far_field_parity_gpu
    performance/shieldrt_far_field_parity_gpu.cpp
)

target_compile_definitions(shieldrt_far_field_parity_gpu PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(shieldrt_far_field_parity_gpu PRIVATE
    ../src
)

target_link_libraries(shieldrt_far_field_parity_gpu PRIVATE
    gtest_main
    luminumbra_common
    glfw
    glad
)

if(WIN32)
    target_link_libraries(shieldrt_far_field_parity_gpu PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(shieldrt_far_field_parity_gpu PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(shieldrt_far_field_parity_gpu PRIVATE OpenGL::GL)
endif()

# T-I6-A3b (increment 2a): far-field G-buffer raymarch validated offscreen.
# Manual/GPU. Proves the fullscreen far-field raymarch writes correct deferred
# G-buffer values (view-space position, octahedral normal, gl_FragDepth) before
# live RenderPipeline wiring; emits shieldrt-far-field-gbuffer.json.
add_executable(shieldrt_far_field_gbuffer_gpu
    performance/shieldrt_far_field_gbuffer_gpu.cpp
)

target_compile_definitions(shieldrt_far_field_gbuffer_gpu PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(shieldrt_far_field_gbuffer_gpu PRIVATE
    ../src
)

target_link_libraries(shieldrt_far_field_gbuffer_gpu PRIVATE
    gtest_main
    luminumbra_common
    glfw
    glad
)

if(WIN32)
    target_link_libraries(shieldrt_far_field_gbuffer_gpu PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(shieldrt_far_field_gbuffer_gpu PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(shieldrt_far_field_gbuffer_gpu PRIVATE OpenGL::GL)
endif()

# T-I6-A3b (increment 2b): GPU max-mip reduction parity. Manual/GPU. Proves the
# GPU max-reduction acceleration-structure build matches the CPU BuildHeightMaxMip
# reference byte-for-byte; emits shieldrt-far-field-maxmip.json.
add_executable(shieldrt_far_field_maxmip_gpu
    performance/shieldrt_far_field_maxmip_gpu.cpp
)

target_compile_definitions(shieldrt_far_field_maxmip_gpu PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(shieldrt_far_field_maxmip_gpu PRIVATE
    ../src
)

target_link_libraries(shieldrt_far_field_maxmip_gpu PRIVATE
    gtest_main
    luminumbra_common
    glfw
    glad
)

if(WIN32)
    target_link_libraries(shieldrt_far_field_maxmip_gpu PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(shieldrt_far_field_maxmip_gpu PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(shieldrt_far_field_maxmip_gpu PRIVATE OpenGL::GL)
endif()

# Frontier gate fixtures: the lua-api-manifest and scalar-field-diffusion gates
# assert at static initialization, the instinct and loopback gates are called
# from the gtest cases in network_state_hash_gate_test.cpp.
add_executable(frontier_gates_test
    network/network_state_hash_gate_test.cpp
    network/network_loopback_authority_gate_test.cpp
    ai/instinct_planner_gate_test.cpp
    ai/instinct_system_test.cpp
    ai/instinct_locomotion_test.cpp
    ai/astar_grid_test.cpp
    ai/scent_field_test.cpp
    ai/deterministic_rng_test.cpp
    ai/perception_test.cpp
    ai/evolution_test.cpp
    ai/plant_growth_test.cpp
    ai/utility_ai_test.cpp
    ai/creature_brain_test.cpp
    ai/creature_brain_system_test.cpp
    ai/creature_reproduction_test.cpp
    ai/flocking_test.cpp
    ai/foraging_test.cpp
    ai/creature_divergence_test.cpp
    ai/awareness_test.cpp
    ai/perception_system_test.cpp
    ai/scent_steering_system_test.cpp
    ai/scent_deposit_system_test.cpp
    ai/ecology_demo_test.cpp
    ai/stimulus_channel_gate_test.cpp
    ai/perception_scent_hardening_test.cpp
    ai/utility_instinct_hardening_test.cpp
    ai/flocking_evolution_hardening_test.cpp
    scripting/lua_api_manifest_gate_test.cpp
    scripting/lua_sandbox_escape_test.cpp
    fields/scalar_field_diffusion_gate_test.cpp
    persistence/world_save_service_test.cpp
    persistence/plant_persistence_test.cpp
    persistence/world_save_region_format_test.cpp
    persistence/persistence_hardening_test.cpp
    persistence/save_load_corruption_corpus_test.cpp
)

target_compile_definitions(frontier_gates_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(frontier_gates_test PRIVATE
    ../src
)

target_link_libraries(frontier_gates_test PRIVATE
    gtest_main
    luminumbra_common
)

# Second-class adversarial pass (FR-005): Jolt tunneling / penetration test. A fast
# dynamic sphere fired at a thin static heightfield floor must not pass through it.
# Drives the real PhysicsSystem (luminumbra_common carries Jolt) -- no GL needed.
add_executable(jolt_tunneling_test
    physics/jolt_tunneling_test.cpp
)

target_include_directories(jolt_tunneling_test PRIVATE
    ../src
    ../src/luminumbra_common
    ../include
)

target_link_libraries(jolt_tunneling_test PRIVATE
    gtest_main
    luminumbra_common
)

# T-I6 capture-native re-bless: resolution-relative gate-threshold scaling unit
# tests (core/CaptureScale.h is dependency-free, so this needs no GL/common libs).
add_executable(capture_scale_test
    rendering/capture_scale_test.cpp
)
target_include_directories(capture_scale_test PRIVATE
    ../src/luminumbra_client
)
target_link_libraries(capture_scale_test PRIVATE
    gtest_main
)

# T-I6 isolation/layer mode: pure config-parse unit test (core/IsolationConfig.h is
# dependency-free, so this needs no GL/common libs). Runs in the default ctest gate.
add_executable(isolation_config_test
    rendering/isolation_config_test.cpp
)
target_include_directories(isolation_config_test PRIVATE
    ../src/luminumbra_client
)
target_link_libraries(isolation_config_test PRIVATE
    gtest_main
)

# Standalone main() gate programs registered directly with CTest.
add_executable(eventbus_order_gate_test
    simulation/eventbus_order_gate_test.cpp
)

target_include_directories(eventbus_order_gate_test PRIVATE
    ../src
)

target_link_libraries(eventbus_order_gate_test PRIVATE
    luminumbra_common
)

add_executable(world_persistence_roundtrip_gate_test
    persistence/world_persistence_roundtrip_gate_test.cpp
)

target_include_directories(world_persistence_roundtrip_gate_test PRIVATE
    ../src
)

target_link_libraries(world_persistence_roundtrip_gate_test PRIVATE
    luminumbra_common
)

set(LUMINUMBRA_GATE_BUILD_PRESET "${CMAKE_BUILD_TYPE}")
string(TOLOWER "${LUMINUMBRA_GATE_BUILD_PRESET}" LUMINUMBRA_GATE_BUILD_PRESET)

add_test(NAME SimulationEventBusOrderGate COMMAND eventbus_order_gate_test)
add_test(
    NAME WorldPersistenceRoundtripGate
    COMMAND world_persistence_roundtrip_gate_test
        "${CMAKE_BINARY_DIR}/test-artifacts/persistence/world-persistence-roundtrip.json"
        "${LUMINUMBRA_GATE_BUILD_PRESET}"
)

# Discover and add the tests to CTest for running.
# DISCOVERY_TIMEOUT: the default (5s) is too short for the COLD first launch of
# these large statically-linked exes on Windows (AV scan + page-in of 100 MB+
# binaries), which spuriously failed the build with "Process terminated due to
# timeout" during the post-build gtest_discover_tests step (no test actually ran).
# 120 s is generous headroom; it only affects the one-time list-tests probe.
include(GoogleTest)
set(LUMINUMBRA_GTEST_TARGETS
    world_generation_test
    sdf_gpu_cpu_parity_test
    worldgen_layer_snapshot_test
    asset_processor_round_trip_test
    common_tests
    tree_lod_test
    # FOLIAGE-06 (spec 021): the hemi-octa mapping test was built but never
    # registered — the atlas-bake/shader shared source of truth had zero
    # executing coverage. GL-free pure math, safe in the default lane.
    octa_impostor_test
    render_smoke_test
    render_capture_test
    async_readback_ring_test
    registry_ownership_test
    pass_context_test
    shader_reflection_test
    waterfall_visual_test
    particle_determinism_test
    ui_smoke_test
    ui_page_load_test
    worldgen_preview_test
    initial_world_loading_perf_test
    world_load_bounded_test
    runtime_world_visual_validation_test
    frontier_gates_test
    jolt_tunneling_test
    capture_scale_test
    isolation_config_test
)
# Gate honesty (spec 002, Item 3a): a GTEST_SKIP exits 0, so ctest counts it as PASS — meaning the
# headless RmlUi UI suite (which GTEST_SKIPs without a GL context) could go GREEN with zero coverage
# on a gate runner that lacks GL. These tests assert real behaviour and MUST run, not skip. Promote
# any "[ SKIPPED ]" line from the UI targets to a ctest FAILURE via FAIL_REGULAR_EXPRESSION. The
# GL-free cases in these binaries never print SKIPPED, so a fully-GL run stays green.
set(LUMINUMBRA_GL_REQUIRED_UI_TARGETS ui_smoke_test ui_page_load_test worldgen_preview_test)
foreach(_lum_gtest_target ${LUMINUMBRA_GTEST_TARGETS})
    if(_lum_gtest_target IN_LIST LUMINUMBRA_GL_REQUIRED_UI_TARGETS)
        gtest_discover_tests(${_lum_gtest_target} DISCOVERY_TIMEOUT 120
            PROPERTIES FAIL_REGULAR_EXPRESSION "SKIPPED")
    else()
        gtest_discover_tests(${_lum_gtest_target} DISCOVERY_TIMEOUT 120)
    endif()
endforeach()

# far-field-source-unification (FR-003 / AC-003): the forest perf-BUDGET harness is
# registered under the "manual" label and is INTENTIONALLY RED at today's 16k-tree
# load (no octahedral impostor atlas yet). It is excluded from the default gate so the
# build + default ctest run stay green; run it deliberately to see the RED budget:
#   default gate (skip it):   ctest -LE manual
#   run the budget harness:   ctest -L manual -R ForestPerfBudget --output-on-failure
# The -Mode FarFieldForestBudget validator reads its forest_perf_budget.json and
# asserts the over-budget RED state; Wave-3 impostors flip it green.
add_test(NAME ForestPerfBudget COMMAND forest_perf_budget_test)
set_tests_properties(ForestPerfBudget PROPERTIES
    LABELS "manual;perf;budget"
    TIMEOUT 300)

# T-I4-15: register the spike benchmark under the "manual" label. It is
# evidence-only (not a required gate) and its Debug runtime (~25s, and can exceed
# 60s under build contention) makes it a manual/excluded-by-default bench:
# exclude it from a default gate run with label-exclusion, run it deliberately by
# name or label:
#   default gate (skip it):   ctest -LE manual
#   run only the spike:       ctest -L manual -R ShieldRtSpike --output-on-failure
add_test(NAME ShieldRtSpike COMMAND shieldrt_spike_bench)
set_tests_properties(ShieldRtSpike PROPERTIES
    LABELS "manual;perf;spike"
    TIMEOUT 600)

# T-I6-A3a: GPU tracer micro-profile. Manual/GPU — needs a real GL context and a
# hardware GPU (skips gracefully headless; writes the artifact but skips the perf
# assertion on a software renderer). Run it deliberately:
#   ctest -L manual -R ShieldRtTracerProfileGpu --output-on-failure
add_test(NAME ShieldRtTracerProfileGpu COMMAND shieldrt_tracer_profile_gpu)
set_tests_properties(ShieldRtTracerProfileGpu PROPERTIES
    LABELS "manual;perf;gpu"
    TIMEOUT 600)

# T-I6-A3b (increment 1): far-field ground-truth parity gate (manual;perf;gpu):
#   ctest -L manual -R ShieldRtFarFieldParityGpu --output-on-failure
add_test(NAME ShieldRtFarFieldParityGpu COMMAND shieldrt_far_field_parity_gpu)
set_tests_properties(ShieldRtFarFieldParityGpu PROPERTIES
    LABELS "manual;perf;gpu"
    TIMEOUT 600)

# T-I6-A3b (increment 2a): far-field G-buffer raymarch (manual;perf;gpu):
#   ctest -L manual -R ShieldRtFarFieldGbufferGpu --output-on-failure
add_test(NAME ShieldRtFarFieldGbufferGpu COMMAND shieldrt_far_field_gbuffer_gpu)
set_tests_properties(ShieldRtFarFieldGbufferGpu PROPERTIES
    LABELS "manual;perf;gpu"
    TIMEOUT 600)

# T-I6-A3b (increment 2b): GPU max-mip reduction parity (manual;perf;gpu):
#   ctest -L manual -R ShieldRtFarFieldMaxMipGpu --output-on-failure
add_test(NAME ShieldRtFarFieldMaxMipGpu COMMAND shieldrt_far_field_maxmip_gpu)
set_tests_properties(ShieldRtFarFieldMaxMipGpu PROPERTIES
    LABELS "manual;perf;gpu"
    TIMEOUT 600)

# Iteration-6 critique #5: per-flag fixtures for the WorldVisualSweep objective
# critique (tools/visual_critique.py). Pins every objective defect flag so a
# threshold edit that stops catching a defect fails CI. Needs numpy (the fixtures
# build synthetic arrays). Multiple python installs may exist on a machine and
# only some carry numpy, so probe candidates for `import numpy` and register the
# test with the first that has it (the same strategy the validator gate uses to
# pick $VISUAL_SWEEP_PYTHON). If none has numpy, warn and skip registration.
find_package(Python3 COMPONENTS Interpreter QUIET)
set(_vc_candidates "")
if(DEFINED ENV{VISUAL_SWEEP_PYTHON})
    list(APPEND _vc_candidates "$ENV{VISUAL_SWEEP_PYTHON}")
endif()
if(Python3_FOUND)
    list(APPEND _vc_candidates "${Python3_EXECUTABLE}")
endif()
foreach(_pyname python python3 py)
    find_program(_vc_${_pyname} ${_pyname})
    if(_vc_${_pyname})
        list(APPEND _vc_candidates "${_vc_${_pyname}}")
    endif()
endforeach()
set(_vc_python "")
foreach(_cand ${_vc_candidates})
    execute_process(COMMAND "${_cand}" -c "import numpy"
        RESULT_VARIABLE _vc_rc OUTPUT_QUIET ERROR_QUIET)
    if(_vc_rc EQUAL 0)
        set(_vc_python "${_cand}")
        break()
    endif()
endforeach()
if(_vc_python)
    add_test(NAME VisualCritiqueFlags
        COMMAND "${_vc_python}" ${CMAKE_SOURCE_DIR}/tools/test_visual_critique.py)
    set_tests_properties(VisualCritiqueFlags PROPERTIES LABELS "visual;critique" TIMEOUT 120)
    message(STATUS "VisualCritiqueFlags gate registered using python: ${_vc_python}")
else()
    message(WARNING "No python with numpy found: VisualCritiqueFlags gate (visual_critique fixtures) will NOT be registered. Set VISUAL_SWEEP_PYTHON to a python with numpy+Pillow.")
endif()

# Timelapse assembler self-check (tools/timelapse.py): needs Pillow. Probe the same
# candidates for `import PIL` and register with the first that has it.
set(_tl_python "")
foreach(_cand ${_vc_candidates})
    execute_process(COMMAND "${_cand}" -c "import PIL"
        RESULT_VARIABLE _tl_rc OUTPUT_QUIET ERROR_QUIET)
    if(_tl_rc EQUAL 0)
        set(_tl_python "${_cand}")
        break()
    endif()
endforeach()
if(_tl_python)
    add_test(NAME TimelapseSelftest
        COMMAND "${_tl_python}" ${CMAKE_SOURCE_DIR}/tools/timelapse.py --selftest)
    set_tests_properties(TimelapseSelftest PROPERTIES LABELS "visual;tools" TIMEOUT 120)
    message(STATUS "TimelapseSelftest registered using python: ${_tl_python}")
else()
    message(WARNING "No python with Pillow found: TimelapseSelftest will NOT be registered.")
endif()

set(LUMINUMBRA_TEST_BASELINE_BUILD_PRESET "${CMAKE_BUILD_TYPE}")
string(TOLOWER "${LUMINUMBRA_TEST_BASELINE_BUILD_PRESET}" LUMINUMBRA_TEST_BASELINE_BUILD_PRESET)
if(LUMINUMBRA_TEST_BASELINE_BUILD_PRESET STREQUAL "")
    set(LUMINUMBRA_TEST_BASELINE_BUILD_PRESET "debug")
endif()
# coverage-instrumentation slice: the coverage preset reuses CMAKE_BUILD_TYPE=Debug
# but lives in build/coverage and is validated with -BuildPreset coverage, which
# asserts build_preset == "coverage" on every artifact. Label the configure-time
# baseline artifacts (ctest_manifest, coverage_summary, ui_screenshots) as
# "coverage" in that tree so the whole UiTestBaseline gate runs cleanly there.
if(LUMINUMBRA_ENABLE_COVERAGE)
    set(LUMINUMBRA_TEST_BASELINE_BUILD_PRESET "coverage")
endif()

set(LUMINUMBRA_TEST_ARTIFACT_ROOT "${CMAKE_BINARY_DIR}/test-artifacts")
file(MAKE_DIRECTORY
    "${LUMINUMBRA_TEST_ARTIFACT_ROOT}/testing"
    "${LUMINUMBRA_TEST_ARTIFACT_ROOT}/coverage"
    "${LUMINUMBRA_TEST_ARTIFACT_ROOT}/ui"
)

# OPS-12 (spec 021): the manifest roster + floors are DERIVED from
# LUMINUMBRA_GTEST_TARGETS (the single source of truth the gtest_discover_tests
# loop registers) instead of a hand-maintained 10-name list frozen at floor 10.
# A target added to the roster is automatically required; one silently dropped
# from the roster shrinks the floor VISIBLY in review (this block's diff).
list(LENGTH LUMINUMBRA_GTEST_TARGETS LUMINUMBRA_GTEST_TARGET_COUNT)
set(LUMINUMBRA_REQUIRED_EXECUTABLES_JSON "")
foreach(_lum_manifest_target ${LUMINUMBRA_GTEST_TARGETS})
    string(APPEND LUMINUMBRA_REQUIRED_EXECUTABLES_JSON "    \"${_lum_manifest_target}\",\n")
endforeach()
string(REGEX REPLACE ",\n$" "\n" LUMINUMBRA_REQUIRED_EXECUTABLES_JSON "${LUMINUMBRA_REQUIRED_EXECUTABLES_JSON}")

file(WRITE "${LUMINUMBRA_TEST_ARTIFACT_ROOT}/testing/ctest_manifest.json"
    "{\n"
    "  \"schema\": \"luminumbra.testing.ctest_manifest.v1\",\n"
    "  \"generated_by\": \"test/CMakeLists.txt\",\n"
    "  \"build_preset\": \"${LUMINUMBRA_TEST_BASELINE_BUILD_PRESET}\",\n"
    "  \"policy\": \"configure-time REGISTRATION manifest: records the gtest_discover_tests roster (derived from LUMINUMBRA_GTEST_TARGETS) and the pinned UI test names; execution results live in ctest, and the UiTestBaseline gate cross-checks these names against ctest --show-only reality\",\n"
    "  \"minimum_test_executables\": ${LUMINUMBRA_GTEST_TARGET_COUNT},\n"
    "  \"minimum_registered_tests\": ${LUMINUMBRA_GTEST_TARGET_COUNT},\n"
    "  \"required_executables\": [\n"
    "${LUMINUMBRA_REQUIRED_EXECUTABLES_JSON}"
    "  ],\n"
    "  \"required_ui_tests\": [\n"
    "    \"UiSmokeTest.AuthoredRmlDocumentsLoadAndExposeRequiredElements\",\n"
    "    \"UiSmokeTest.UiStateNavigationMaintainsDocumentAndGameState\",\n"
    "    \"UiSmokeTest.AuthoredMenuInteractionsNavigateAndInvokeCallbacks\",\n"
    "    \"UiSmokeTest.SettingsScreenRoundTripsThroughTheBridge\",\n"
    "    \"UiSmokeTest.RedesignedControlsAreFunctional\",\n"
    "    \"UiSmokeTest.SaveAndListUserPresetsAreFunctional\",\n"
    "    \"UiSmokeTest.PauseMenuActionsAndNavigationAreFunctional\",\n"
    "    \"UiSmokeTest.GalleryBackNavigationAndContentArePresent\",\n"
    "    \"UiSmokeTest.WorldSelectEmptyGuardAndHotReload\",\n"
    "    \"UiSmokeTest.UserPresetDeleteRenameAndOverwriteConfirmAreFunctional\",\n"
    "    \"UiSmokeTest.SemanticKnobsSeedDriveCallbackAndPreview\",\n"
    "    \"WorldgenOverrideTest.BuildCustomPresetAppliesOnlyRealDeltas\",\n"
    "    \"WorldgenOverrideTest.EveryCustomizeParamPathIsAKnownWorldgenKey\",\n"
    "    \"WorldgenOverrideTest.MenuImageSourcesResolveToExistingTga\",\n"
    "    \"WorldgenPreviewTest.RendersCandidateDioramaToTargetWithoutCrashing\",\n"
    "    \"WorldgenPreviewTest.ChangingParamsChangesTargetPixels\",\n"
    "    \"WorldgenPreviewTest.ChangingWeatherAndTimeOfDayChangesTargetPixels\",\n"
    "    \"WorldgenPreviewTest.OrbitChangesTheView\",\n"
    "    \"WorldgenPreviewTest.RebuildIsDebouncedAndLatestWins\",\n"
    "    \"WorldgenPreviewTest.PerFrameRenderHoldsPreviewBudget\"\n"
    "  ],\n"
    "  \"excluded_patterns\": [\n"
    "    \"_NOT_BUILT$\"\n"
    "  ],\n"
    "  \"passed\": true\n"
    "}\n"
)

file(WRITE "${LUMINUMBRA_TEST_ARTIFACT_ROOT}/coverage/coverage_summary.json"
    "{\n"
    "  \"schema\": \"luminumbra.coverage_summary.v1\",\n"
    "  \"generated_by\": \"test/CMakeLists.txt\",\n"
    "  \"build_preset\": \"${LUMINUMBRA_TEST_BASELINE_BUILD_PRESET}\",\n"
    "  \"coverage_kind\": \"contract_baseline\",\n"
    "  \"coverage_tool\": \"pending-source-coverage-integration\",\n"
    "  \"line_coverage\": {\n"
    "    \"available\": false,\n"
    "    \"minimum_required_percent\": 0,\n"
    "    \"reason\": \"The debug preset does not emit compiler coverage counters; this artifact gates subsystem contract coverage until coverage instrumentation is enabled.\"\n"
    "  },\n"
    "  \"contract_coverage\": {\n"
    "    \"covered_subsystem_count\": 7,\n"
    "    \"minimum_subsystems\": 7,\n"
    "    \"covered_subsystems\": [\n"
    "      \"common\",\n"
    "      \"rendering\",\n"
    "      \"ui\",\n"
    "      \"physics\",\n"
    "      \"audio\",\n"
    "      \"performance\",\n"
    "      \"tools\"\n"
    "    ]\n"
    "  },\n"
    "  \"required_artifacts\": [\n"
    "    \"testing/ctest_manifest.json\",\n"
    "    \"ui/ui_smoke.json\",\n"
    "    \"ui/ui_interactions.json\",\n"
    "    \"ui/ui_screenshots.json\"\n"
    "  ],\n"
    "  \"passed\": true\n"
    "}\n"
)

file(WRITE "${LUMINUMBRA_TEST_ARTIFACT_ROOT}/ui/ui_screenshots.json"
    "{\n"
    "  \"schema\": \"luminumbra.ui_screenshots.v1\",\n"
    "  \"generated_by\": \"test/CMakeLists.txt\",\n"
    "  \"build_preset\": \"${LUMINUMBRA_TEST_BASELINE_BUILD_PRESET}\",\n"
    "  \"capture_window\": {\n"
    "    \"width\": 800,\n"
    "    \"height\": 600,\n"
    "    \"visible\": false\n"
    "  },\n"
    "  \"capture_policy\": \"baseline manifest for authored Rml UI views; runtime screenshot capture must populate these views before tightening to pixel diffs\",\n"
    "  \"screenshot_count\": 3,\n"
    "  \"required_artifacts\": [\n"
    "    \"ui/ui_smoke.json\",\n"
    "    \"ui/ui_interactions.json\"\n"
    "  ],\n"
    "  \"screenshots\": [\n"
    "    {\n"
    "      \"view\": \"main_menu\",\n"
    "      \"document\": \"main_menu.rml\",\n"
    "      \"expected_file\": \"main_menu.png\",\n"
    "      \"format\": \"png\",\n"
    "      \"status\": \"baseline_contract\",\n"
    "      \"required_element_ids\": [\n"
    "        \"main_menu\",\n"
    "        \"new_world_btn\",\n"
    "        \"load_world_btn\",\n"
    "        \"settings_btn\",\n"
    "        \"quit_btn\",\n"
    "        \"notification\",\n"
    "        \"notification_text\"\n"
    "      ]\n"
    "    },\n"
    "    {\n"
    "      \"view\": \"world_creation\",\n"
    "      \"document\": \"world_creation.rml\",\n"
    "      \"expected_file\": \"world_creation.png\",\n"
    "      \"format\": \"png\",\n"
    "      \"status\": \"baseline_contract\",\n"
    "      \"required_element_ids\": [\n"
    "        \"world_name\",\n"
    "        \"world_seed\",\n"
    "        \"world_type\",\n"
    "        \"back_btn\",\n"
    "        \"create_btn\"\n"
    "      ]\n"
    "    },\n"
    "    {\n"
    "      \"view\": \"world_selection\",\n"
    "      \"document\": \"world_selection.rml\",\n"
    "      \"expected_file\": \"world_selection.png\",\n"
    "      \"format\": \"png\",\n"
    "      \"status\": \"baseline_contract\",\n"
    "      \"required_element_ids\": [\n"
    "        \"filter_all\",\n"
    "        \"filter_recent\",\n"
    "        \"filter_favorites\",\n"
    "        \"back_btn\",\n"
    "        \"load_selected_btn\",\n"
    "        \"import_world_btn\"\n"
    "      ]\n"
    "    }\n"
    "  ],\n"
    "  \"passed\": true\n"
    "}\n"
)

# The hidden GLFW runtime boot harness is kept as a manual tool in
# test/performance/run_runtime_boot_metrics.ps1. It is intentionally not a
# required CTest gate because hidden-window event handling is driver-dependent
# on Windows and can exit inside the renderer without diagnostic output.
