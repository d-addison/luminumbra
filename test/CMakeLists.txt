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
    stb_headers
)

if(TARGET meshoptimizer)
    target_link_libraries(asset_processor_round_trip_test PRIVATE meshoptimizer)
elseif(TARGET meshoptimizer::meshoptimizer)
    target_link_libraries(asset_processor_round_trip_test PRIVATE meshoptimizer::meshoptimizer)
endif()

add_executable(common_tests
    common/Crc32_test.cpp
    rendering/shader_compositing_contract_test.cpp
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
    common/config_constant_drift_test.cpp
    common/config_hot_reload_rollback_test.cpp
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
    net/single_port_accept_test.cpp
    net/single_port_host_client_integration_test.cpp
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

# tree rendering (tree rendering): render-only tree LOD distance selection. The header
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

#  far-field impostors: the hemi-octahedral view<->tile mapping math (GL-free,
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

# the mixer/ducking envelope math. The header under test
# (src/luminumbra_client/audio/MixerModel.h) is a pure dependency-free header
# (no miniaudio, no audio device, no GL) -- same shape as tree_lod_test: just
# the include roots + gtest, safe in the default CPU lane.
add_executable(mixer_model_test
    audio/mixer_model_test.cpp
)

target_include_directories(mixer_model_test PRIVATE
../src
../src/luminumbra_client
)

target_link_libraries(mixer_model_test PRIVATE
    gtest_main
)

# the day/night-soundscape gating/crossfade and
# the biome->reverb + delay-proxy mapping math. The header under test
# (src/luminumbra_client/audio/EnvironmentalAudioModel.h) is pure std (no
# miniaudio, no audio device, no GL) -- same shape as tree_lod_test: just the
# include root + gtest, safe in the default CPU lane.
add_executable(environmental_audio_model_test
    audio/environmental_audio_model_test.cpp
)

target_include_directories(environmental_audio_model_test PRIVATE
../src
)

target_link_libraries(environmental_audio_model_test PRIVATE
    gtest_main
)

# world-dressing placement determinism. Compiles the
# extracted client-side WorldDressing.cpp directly (it depends only on
# luminumbra_common's DeterministicRng / plant-genome / creature-build headers +
# glm — no GL, no entt storage, no world system) and drives it with synthetic
# callbacks: same inputs must produce a byte-identical placement stream, and the
# tree/rock/bush loops must share ONE sequential RNG stream (the legacy layout
# contract that keeps the async extraction byte-equal to the old inline loops).
add_executable(world_dressing_test
    client/world_dressing_test.cpp
../src/luminumbra_client/WorldDressing.cpp
)

target_include_directories(world_dressing_test PRIVATE
../src
../src/luminumbra_client
)

target_link_libraries(world_dressing_test PRIVATE
    gtest_main
    luminumbra_common
)

# far-field source unification: tree rendering's first slice -- the forest
# perf-BUDGET RED harness. OpenGL-free: it models the foliage draw load (instance /
# draw-call / triangle counts) at a pinned 16k-tree load over the pure TreeLod.h
# selection + the GBufferPass batching model, emits forest_perf_budget.json, and
# RED-fails the geometry budget at today's load (impostors flip it green in ).
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

# GL-free regression coverage for the foliage surface-grid cache prune: the
# helper is a free template in FoliagePass.h precisely so it can be exercised
# without a GL context.
add_executable(foliage_cache_prune_test
    rendering/foliage_cache_prune_test.cpp
)

target_include_directories(foliage_cache_prune_test PRIVATE
../src
../src/luminumbra_client
)

target_link_libraries(foliage_cache_prune_test PRIVATE
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
    # the capture-hooks TU under test (GL-free; direct source, same as
    # shader_reflection_test's ShaderReflection.cpp -- render_capture_test links
    # luminumbra_common, not luminumbra_client, so no duplicate symbols).
../src/luminumbra_client/rendering/CaptureHooks.cpp
)

target_compile_definitions(render_capture_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(render_capture_test PRIVATE
../src
../src/luminumbra_client
../vendor
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

# the PIX + Nsight trigger legs, driven through the
# CaptureHooks test seams against injected api doubles. CaptureHooks.cpp is
# GL-free (module detection + function pointers only), so unlike
# render_capture_test this target needs no glfw/glad/OpenGL link.
add_executable(capture_sdk_trigger_test
    rendering/capture_sdk_trigger_test.cpp
../src/luminumbra_client/rendering/CaptureHooks.cpp
)

target_compile_definitions(capture_sdk_trigger_test PRIVATE
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(capture_sdk_trigger_test PRIVATE
../src
../src/luminumbra_client
../vendor
)

target_link_libraries(capture_sdk_trigger_test PRIVATE
    gtest_main
    luminumbra_common
)

# the registry OWNED-resource contract against
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

# the pilot-pass seam contract — DebugView + GroundDecal
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

# async GPU readback ring. Exercises the ring's submit/poll/
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

#  ( ;   / ): the in-process dual-backend
# FLIP parity harness. Renders a temporally-deterministic pass (basic.vert/frag)
# into an in-memory framebuffer and diffs two framebuffers produced in ONE process
# with the same perceptual metric as tools/flip_diff.py (rendering/InProcessFlip.h,
# header-only + GL-free). GL-only, self-contained (no luminumbra_common); the
# RhiDiligent second-backend leg is wired and loudly skipped until.
add_executable(dual_backend_flip_test
    rendering/dual_backend_flip_test.cpp
)

target_compile_definitions(dual_backend_flip_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(dual_backend_flip_test PRIVATE
../src
../src/luminumbra_client
)

target_link_libraries(dual_backend_flip_test PRIVATE
    gtest_main
    glfw
    glad
    glm
)

if(WIN32)
    target_link_libraries(dual_backend_flip_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(dual_backend_flip_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(dual_backend_flip_test PRIVATE OpenGL::GL)
endif()

# shader-resource reflection + layout validation. Compiles small
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

#   /   + /F.3: PilotShaderReflectionParity.
# A SINGLE slangc invocation both compiles the pilot pair's single-source HLSL to
# SPIR-V (-target spirv) AND emits its reflection (-reflection-json); the ctest
# asserts that reflection == the GL-introspected GLSL layout == the declared
# ExpectedLayout. Slang replaces the former dxc + spirv-cross two-tool chain (one
# tool that compiles for every backend and emits reflection data. slangc is
# a STANDALONE binary (no mingw linkage), bundled with the Vulkan SDK since 1.3.296,
# so gate the target on finding it -- an SDK-less configure just skips this GPU ctest.
find_program(LUMINUMBRA_SLANGC slangc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(LUMINUMBRA_SLANGC)
    set(_pilot_reflect_dir "${CMAKE_BINARY_DIR}/pilot_shaders")
    file(MAKE_DIRECTORY "${_pilot_reflect_dir}")
    set(_pilot_reflect_jsons "")
    foreach(_p ssao debug_view)
        set(_hlsl "${CMAKE_SOURCE_DIR}/res/shaders/pilot/${_p}.frag.hlsl")
        set(_spv "${_pilot_reflect_dir}/${_p}.frag.spv")
        set(_json "${_pilot_reflect_dir}/${_p}.frag.reflect.json")
        # -profile ps_6_0 selects the HLSL pixel stage (do NOT also pass -stage: it
        # double-specifies the stage and warns). -emit-spirv-directly uses Slang's
        # direct SPIR-V backend (1.3+; its 1.0-1.2 emission is experimental, but the
        # RTX/Vulkan target supports 1.3 trivially).
        add_custom_command(
            OUTPUT "${_json}" "${_spv}"
            COMMAND "${LUMINUMBRA_SLANGC}" "${_hlsl}"
                    -profile ps_6_0 -entry main
                    -target spirv -emit-spirv-directly -O0
                    -o "${_spv}" -reflection-json "${_json}"
            DEPENDS "${_hlsl}"
            COMMENT ": slangc ${_p}.frag.hlsl -> SPIR-V + reflection JSON"
            VERBATIM
        )
        list(APPEND _pilot_reflect_jsons "${_json}")
    endforeach()
    add_custom_target(pilot_shader_reflect DEPENDS ${_pilot_reflect_jsons})

    add_executable(pilot_shader_reflection_test
        rendering/pilot_shader_reflection_test.cpp
../src/luminumbra_client/rendering/ShaderReflection.cpp
../src/luminumbra_client/rendering/ShaderReflectionSpirv.cpp
../src/luminumbra_client/rendering/PassShaderLayouts.cpp
    )
    add_dependencies(pilot_shader_reflection_test pilot_shader_reflect)
    target_compile_definitions(pilot_shader_reflection_test PRIVATE
        LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
        PILOT_SHADER_REFLECT_DIR="${_pilot_reflect_dir}"
    )
    target_include_directories(pilot_shader_reflection_test PRIVATE
../src
../src/luminumbra_client
    )
    target_link_libraries(pilot_shader_reflection_test PRIVATE
        gtest_main
        glfw
        glad
        nlohmann_json::nlohmann_json
    )
    if(WIN32)
        target_link_libraries(pilot_shader_reflection_test PRIVATE opengl32)
    elseif(APPLE)
        find_package(OpenGL REQUIRED)
        target_link_libraries(pilot_shader_reflection_test PRIVATE ${OPENGL_LIBRARIES})
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(pilot_shader_reflection_test PRIVATE OpenGL::GL)
    endif()
    set(LUMINUMBRA_PILOT_SHADER_TEST_AVAILABLE ON)
    message(STATUS " PilotShaderReflectionParity enabled (slangc=${LUMINUMBRA_SLANGC})")
else()
    message(STATUS " PilotShaderReflectionParity SKIPPED (slangc not found in VULKAN_SDK)")
endif()

# the LUMIN_RHI backend-selector parse gate.
# Pure string logic -- no GL, no Diligent, no GPU. Compiles the RhiBackend TU
# directly (like shader_reflection_test) so it needs no client/GL link. This is
# the toolchain-independent half of the RhiDeviceBringupGpu proving signal; the
# device/swapchain-creation half lives in the GPU ctest added by the pilot.
add_executable(rhi_backend_test
    rendering/rhi_backend_test.cpp
../src/luminumbra_client/rendering/rhi/RhiBackend.cpp
)

target_include_directories(rhi_backend_test PRIVATE
../src
../src/luminumbra_client
)

target_link_libraries(rhi_backend_test PRIVATE gtest_main)

# the device-creation leg of RhiDeviceBringupGpu. This is the
# ONLY target in the whole build that links Diligent (GL + Vk static engines) --
# proof by construction that no shipping binary pulls it. GLFW backs a headless GL
# context; Device.cpp + RhiBackend.cpp are compiled directly (Diligent stays out of
# luminumbra_client). Guarded by LUMINUMBRA_ENABLE_DILIGENT so a Diligent-less
# configure still builds the rest of the suite.
if(LUMINUMBRA_ENABLE_DILIGENT)
    add_executable(rhi_device_bringup_test
        rendering/rhi_device_bringup_test.cpp
../src/luminumbra_client/rendering/rhi/Device.cpp
../src/luminumbra_client/rendering/rhi/RhiBackend.cpp
    )
    target_include_directories(rhi_device_bringup_test PRIVATE
../src
../src/luminumbra_client
    )
    target_link_libraries(rhi_device_bringup_test PRIVATE
        gtest_main
        glfw
        Diligent-GraphicsEngineOpenGL-static
        Diligent-GraphicsEngineVk-static
    )
    if(WIN32)
        target_link_libraries(rhi_device_bringup_test PRIVATE opengl32 gdi32 user32)
    endif()

    #   + leg B: RhiPilotParityGpu -- the RHI pilot's
    # backend-fidelity go/no-go. Renders the  calibration cube raw-GL (glad) and
    # GL-via-Diligent (dual_backend_diligent.cpp -- the only render TU that includes
    # Diligent) in one process and FLIP-compares with the SAME in-process metric
    # calibrated. Links Diligent (GL + Vk) like the bringup test; guarded by the same
    # LUMINUMBRA_ENABLE_DILIGENT so a Diligent-less configure still builds the suite.
    add_executable(rhi_pilot_flip_test
        rendering/rhi_pilot_flip_test.cpp
        rendering/dual_backend_diligent.cpp
    )
    target_compile_definitions(rhi_pilot_flip_test PRIVATE
        LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
        LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
    )
    target_include_directories(rhi_pilot_flip_test PRIVATE
../src
../src/luminumbra_client
        # The GL/Vk engine targets propagate only their own interface/ dirs; the
        # shared public headers (RefCntAutoPtr.hpp etc.) live under Common/Platforms,
        # reached via the FetchContent source dir set by cmake/diligent.cmake.
        "${diligentcore_SOURCE_DIR}/Common/interface"
        "${diligentcore_SOURCE_DIR}/Platforms/interface"
        "${diligentcore_SOURCE_DIR}/Platforms/Basic/interface"
    )
    target_link_libraries(rhi_pilot_flip_test PRIVATE
        gtest_main
        glfw
        glad
        glm
        Diligent-GraphicsEngineOpenGL-static
        Diligent-GraphicsEngineVk-static
    )
    if(WIN32)
        target_link_libraries(rhi_pilot_flip_test PRIVATE opengl32 gdi32 user32)
    elseif(APPLE)
        find_package(OpenGL REQUIRED)
        target_link_libraries(rhi_pilot_flip_test PRIVATE ${OPENGL_LIBRARIES})
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(rhi_pilot_flip_test PRIVATE OpenGL::GL)
    endif()
endif()

#  GL-free particle determinism unit tests (emitter descriptor surface).
add_executable(particle_determinism_test
    rendering/particle_determinism_test.cpp
)

add_executable(far_lod_worker_test
    rendering/far_lod_worker_test.cpp
)

target_include_directories(far_lod_worker_test PRIVATE
../src
../src/luminumbra_client
)

target_link_libraries(far_lod_worker_test PRIVATE
    gtest_main
    luminumbra_client
    glfw
    glad
)

if(WIN32)
    target_link_libraries(far_lod_worker_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(far_lod_worker_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(far_lod_worker_test PRIVATE OpenGL::GL)
endif()

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

# WaterfallVisual gate. Determinism (DetectWaterfalls same-seed ->
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

# UI page-load test. Mirrors ui_smoke_test: live headless RmlUi context
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

# the create-world LIVE WORLD-PREVIEW DIORAMA. Drives the real
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

# Manual cold-load latency diagnostic. It exercises twenty interactive loads at
# radius 12/4 with the watchdog armed, but deliberately makes no machine-speed
# claim. The required performance workflow uses paired relative observations.
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

# per-species behaviour data (IAUS "brain" overrides +
# "genome_ranges" in the species JSON schema). Pure CPU (registry parse + brain
# arbiter + breeding operators; no GL, no world) — pins the byte-identical-defaults
# law: absent/default JSON resolves the compiled constants exactly, an override
# changes the loaded table. Cloned from the world_load_bounded_test block above.
add_executable(species_behavior_overrides_test
    sim/species_behavior_overrides_test.cpp
)

target_compile_definitions(species_behavior_overrides_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

target_include_directories(species_behavior_overrides_test PRIVATE
../src
../src/luminumbra_common
)

target_link_libraries(species_behavior_overrides_test PRIVATE
    gtest_main
    luminumbra_common
)

# perf-lane-and-ecology-tick ( / ): the EcologyTickPerf gate exe. A
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

#   spike benchmark (manual/excluded-by-default: Debug runtime
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

#   GPU tracer micro-profile (manual/GPU: needs a real GL
# context + GPU. Debug SDF-volume build can exceed 60s.) Re-runs the same two
# far-field tracers as compute kernels and times them with GL_TIME_ELAPSED to
# confirm the  spike's heightfield-primary conclusion on hardware;
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

# Standalone far-field ground-truth parity gate (manual/GPU). It proves the
# heightfield max-mip march hits the real analytic surface and emits
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

# far-field G-buffer raymarch validated offscreen.
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

# GPU max-mip reduction parity. Manual/GPU. Proves the
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
    ai/perception_substrate_test.cpp
    ai/creature_brain_substrate_test.cpp
    ai/scent_steering_system_test.cpp
    ai/scent_deposit_system_test.cpp
    ai/ecology_demo_test.cpp
    ai/stimulus_channel_gate_test.cpp
    ai/aether_stimulus_test.cpp
    ai/perception_scent_hardening_test.cpp
    ai/utility_instinct_hardening_test.cpp
    ai/flocking_evolution_hardening_test.cpp
    scripting/lua_api_manifest_gate_test.cpp
    scripting/lua_sandbox_escape_test.cpp
    scripting/aether_script_binding_test.cpp
    fields/scalar_field_diffusion_gate_test.cpp
    fields/energy_field_state_test.cpp
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

# Second-class adversarial pass: Jolt tunneling / penetration test. A fast
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

# Physics surface queries for audio raycasts: real Jolt surface normals
# (flat/ramp/from-below opposition flip, dynamic-sphere radial) + the worldgen
# material seam (SurfaceVertexMaterial vs the legacy Y-band fallback). Drives
# the real PhysicsSystem + a headless SHIELD_WorldSystem -- no GL needed.
add_executable(surface_query_test
    physics/surface_query_test.cpp
)

target_include_directories(surface_query_test PRIVATE
../src
../src/luminumbra_common
../include
)

target_link_libraries(surface_query_test PRIVATE
    gtest_main
    luminumbra_common
)

# real physics-raycast audio occlusion. Compiles AudioSpatialCluster.cpp
# directly (now decoupled from miniaudio) + links luminumbra_common for the Jolt
# PhysicsSystem::calculate_audio_occlusion raycast; no client/GL link needed.
add_executable(audio_occlusion_test
    audio/audio_occlusion_test.cpp
../src/luminumbra_client/audio/AudioSpatialCluster.cpp
)
target_include_directories(audio_occlusion_test PRIVATE
../src
../src/luminumbra_client
../src/luminumbra_common
../include
)
target_link_libraries(audio_occlusion_test PRIVATE
    gtest_main
    luminumbra_common
)

# prune guard — compile-sentinel that the SURVIVING audio headers still build
# (fails if anything leans on a deleted orphan) + asserts the 7 retired bank manifests are
# gone. Includes MiniaudioManager.h so needs the miniaudio + nlohmann_json link interface.
add_executable(audio_prune_guard_test
    audio/audio_prune_guard_test.cpp
)
target_compile_definitions(audio_prune_guard_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
target_include_directories(audio_prune_guard_test PRIVATE
../src
../src/luminumbra_client
)
target_link_libraries(audio_prune_guard_test PRIVATE
    gtest_main
    luminumbra_common
    miniaudio
    nlohmann_json
    glm
)

#  implementation note: occlusion read-back onto ma_sound volume. Pure test of the
# ComputeFinalVolume helper; includes MiniaudioManager.h so needs miniaudio + nlohmann_json.
add_executable(audio_volume_readback_test
    audio/audio_volume_readback_test.cpp
)
target_include_directories(audio_volume_readback_test PRIVATE
../src
../src/luminumbra_client
)
target_link_libraries(audio_volume_readback_test PRIVATE
    gtest_main
    luminumbra_common
    miniaudio
    nlohmann_json
    glm
)

#  capture-native update the baseline: resolution-relative gate-threshold scaling unit
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

#  isolation/layer mode: pure config-parse unit test (core/IsolationConfig.h is
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

# Standalone main gate programs registered directly with CTest.
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
    worldgen_layer_snapshot_test
    asset_processor_round_trip_test
    common_tests
    tree_lod_test
    # the hemi-octa mapping test was built but never
    # registered — the atlas-bake/shader shared source of truth had zero
    # executing coverage. GL-free pure math, safe in the default lane.
    octa_impostor_test
    # mixer/ducking envelope math (CPU-only, GL-free).
    mixer_model_test
    # day/night soundscape + reverb mapping math
    # (CPU-only, GL-free, no audio device).
    environmental_audio_model_test
    # world-dressing placement determinism (CPU-only).
    world_dressing_test
    foliage_cache_prune_test
    render_smoke_test
    render_capture_test
    capture_sdk_trigger_test
    async_readback_ring_test
    dual_backend_flip_test
    registry_ownership_test
    pass_context_test
    shader_reflection_test
    rhi_backend_test
    waterfall_visual_test
    particle_determinism_test
    far_lod_worker_test
    ui_smoke_test
    ui_page_load_test
    worldgen_preview_test
    initial_world_loading_perf_test
    world_load_bounded_test
    # species JSON behaviour overrides — byte-identical
    # defaults + override round-trip (pure CPU, default lane).
    species_behavior_overrides_test
    runtime_world_visual_validation_test
    frontier_gates_test
    jolt_tunneling_test
    surface_query_test
    audio_occlusion_test
    audio_prune_guard_test
    audio_volume_readback_test
    capture_scale_test
    isolation_config_test
)

# CTest discovers each GoogleTest case as a separate process. On headless Linux,
# several of the binaries below would otherwise initialize GLFW/Mesa concurrently
# against the same Xvfb server. That can make an otherwise healthy context creation
# fail transiently and turn a required test into a skip. Keep CPU-only tests parallel,
# while admitting one hidden-GL test at a time.
set(LUMINUMBRA_GL_GTEST_TARGETS
    render_smoke_test
    render_capture_test
    async_readback_ring_test
    dual_backend_flip_test
    registry_ownership_test
    pass_context_test
    shader_reflection_test
    waterfall_visual_test
    particle_determinism_test
    far_lod_worker_test
    ui_smoke_test
    ui_page_load_test
    worldgen_preview_test
    runtime_world_visual_validation_test
    pilot_shader_reflection_test
    rhi_device_bringup_test
    rhi_pilot_flip_test
)
# register the RHI device-bringup GPU ctest only when Diligent is
# vendored in (the target does not exist otherwise).
if(LUMINUMBRA_ENABLE_DILIGENT)
    list(APPEND LUMINUMBRA_GTEST_TARGETS rhi_device_bringup_test)
    list(APPEND LUMINUMBRA_GTEST_TARGETS rhi_pilot_flip_test)
endif()
# register the pilot reflection-parity GPU ctest only when the
# dxc + spirv-cross toolchain was found at configure time.
if(LUMINUMBRA_PILOT_SHADER_TEST_AVAILABLE)
    list(APPEND LUMINUMBRA_GTEST_TARGETS pilot_shader_reflection_test)
endif()
# Gate honesty: a GTEST_SKIP exits 0, so ctest counts it as PASS — meaning a suite that
# skips without a GL context could go GREEN with zero coverage on a gate runner that lacks
# GL. Skip honesty is NOT enforced here in CMake: CI post-processes the JUnit output with
# tools/ci/check_ctest_junit.py, which fails the lane on any skipped or disabled test not
# present in the script's hard-coded ALLOWED_SKIPS set.
foreach(_lum_gtest_target ${LUMINUMBRA_GTEST_TARGETS})
    # common_tests is parallel-safe by construction: its temp-dir fixtures use
    # per-instance unique roots and its socket tests bind ephemeral ports
    # (Listen(0)), so no RESOURCE_LOCK is needed. New tests in the aggregate
    # must keep it that way.
    if(_lum_gtest_target STREQUAL world_load_bounded_test)
        # This machine-sensitive latency observation is opt-in. Keep it out of
        # normal correctness lanes; the paired relative workflow is the gate.
        gtest_discover_tests(${_lum_gtest_target} DISCOVERY_TIMEOUT 120
            PROPERTIES LABELS manual TIMEOUT 3600 RUN_SERIAL TRUE)
    elseif(_lum_gtest_target IN_LIST LUMINUMBRA_GL_GTEST_TARGETS)
        gtest_discover_tests(${_lum_gtest_target} DISCOVERY_TIMEOUT 120
            PROPERTIES RESOURCE_LOCK luminumbra_gl_context)
    else()
        gtest_discover_tests(${_lum_gtest_target} DISCOVERY_TIMEOUT 120)
    endif()
endforeach()

# The deterministic forest geometry harness is manual because it validates a
# pinned 16k-tree workload rather than ordinary unit behavior. It guards the
# production impostor fold with draw-call and triangle-count ceilings; it makes
# no machine-specific timing claim.
add_test(NAME ForestPerfBudget COMMAND forest_perf_budget_test)
set_tests_properties(ForestPerfBudget PROPERTIES
    LABELS "manual;perf;budget"
    TIMEOUT 300)

# Over-the-wire N=32 replication soak (real TCP loopback, 32 client threads,
# ~10s wall-clock, freezes net_soak.v1). Keep the manual label so this remains
# excluded from the parallel default suite; Linux CI gates it explicitly by name
# in a serial step.
#   run: ctest -R NetSoak --output-on-failure
add_executable(net_soak_over_the_wire_test
    net/net_soak_over_the_wire_test.cpp
)
target_compile_definitions(net_soak_over_the_wire_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts")
target_include_directories(net_soak_over_the_wire_test PRIVATE
../src
../src/luminumbra_common)
target_link_libraries(net_soak_over_the_wire_test PRIVATE
    gtest_main
    luminumbra_common)
add_test(NAME NetSoak COMMAND net_soak_over_the_wire_test)
set_tests_properties(NetSoak PROPERTIES
    LABELS "manual;net;soak"
    TIMEOUT 120
    RUN_SERIAL TRUE)

# register the spike benchmark under the "manual" label. It is
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

# GPU tracer micro-profile. Manual/GPU — needs a real GL context and a
# hardware GPU (skips gracefully headless; writes the artifact but skips the perf
# assertion on a software renderer). Run it deliberately:
#   ctest -L manual -R ShieldRtTracerProfileGpu --output-on-failure
add_test(NAME ShieldRtTracerProfileGpu COMMAND shieldrt_tracer_profile_gpu)
set_tests_properties(ShieldRtTracerProfileGpu PROPERTIES
    LABELS "manual;perf;gpu"
    TIMEOUT 600)

# far-field ground-truth parity gate (manual;perf;gpu):
#   ctest -L manual -R ShieldRtFarFieldParityGpu --output-on-failure
add_test(NAME ShieldRtFarFieldParityGpu COMMAND shieldrt_far_field_parity_gpu)
set_tests_properties(ShieldRtFarFieldParityGpu PROPERTIES
    LABELS "manual;perf;gpu"
    TIMEOUT 600)

# far-field G-buffer raymarch (manual;perf;gpu):
#   ctest -L manual -R ShieldRtFarFieldGbufferGpu --output-on-failure
add_test(NAME ShieldRtFarFieldGbufferGpu COMMAND shieldrt_far_field_gbuffer_gpu)
set_tests_properties(ShieldRtFarFieldGbufferGpu PROPERTIES
    LABELS "manual;perf;gpu"
    TIMEOUT 600)

# GPU max-mip reduction parity (manual;perf;gpu):
#   ctest -L manual -R ShieldRtFarFieldMaxMipGpu --output-on-failure
add_test(NAME ShieldRtFarFieldMaxMipGpu COMMAND shieldrt_far_field_maxmip_gpu)
set_tests_properties(ShieldRtFarFieldMaxMipGpu PROPERTIES
    LABELS "manual;perf;gpu"
    TIMEOUT 600)

#  per-flag fixtures for the WorldVisualSweep objective
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
    add_test(NAME RuntimeDataManifestDrift
        COMMAND "${Python3_EXECUTABLE}" ${CMAKE_SOURCE_DIR}/tools/check_runtime_data_manifest.py)
    set_tests_properties(RuntimeDataManifestDrift PROPERTIES LABELS "build;ops" TIMEOUT 30)
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
    message(FATAL_ERROR "No python with numpy found: VisualCritiqueFlags cannot be registered. Install numpy or configure with -DBUILD_TESTING=OFF.")
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
    message(FATAL_ERROR "No python with Pillow found: TimelapseSelftest cannot be registered. Install Pillow or configure with -DBUILD_TESTING=OFF.")
endif()

# SDF-contract doc-lint. Assert docs/shield/sdf-contract.md
# enumerates both producer tiers + the malformed-SDF regeneration rule, checked against
# the contract strings quoted in test/features/sdf-contract.feature. Stdlib-only checker,
# so any python3 from the shared candidate list works (probe a trivial import).
set(_sdf_python "")
foreach(_cand ${_vc_candidates})
    execute_process(COMMAND "${_cand}" -c "import re,sys,pathlib"
        RESULT_VARIABLE _sdf_rc OUTPUT_QUIET ERROR_QUIET)
    if(_sdf_rc EQUAL 0)
        set(_sdf_python "${_cand}")
        break()
    endif()
endforeach()
if(_sdf_python)
    add_test(NAME SdfContractDocLint
        COMMAND "${_sdf_python}" ${CMAKE_SOURCE_DIR}/tools/check_sdf_contract_doc.py)
    set_tests_properties(SdfContractDocLint PROPERTIES LABELS "docs;shield" TIMEOUT 60)
    message(STATUS "SdfContractDocLint registered using python: ${_sdf_python}")
else()
    message(WARNING "No python found: SdfContractDocLint (SHIELD SDF contract doc-lint) will NOT be registered.")
endif()

set(LUMINUMBRA_TEST_BASELINE_BUILD_PRESET "${CMAKE_BUILD_TYPE}")
string(TOLOWER "${LUMINUMBRA_TEST_BASELINE_BUILD_PRESET}" LUMINUMBRA_TEST_BASELINE_BUILD_PRESET)
if(LUMINUMBRA_TEST_BASELINE_BUILD_PRESET STREQUAL "")
    set(LUMINUMBRA_TEST_BASELINE_BUILD_PRESET "debug")
endif()
# The coverage preset reuses CMAKE_BUILD_TYPE=Debug but lives in build/coverage.
# Label its configure-time test manifest with the actual preset name.
if(LUMINUMBRA_ENABLE_COVERAGE)
    set(LUMINUMBRA_TEST_BASELINE_BUILD_PRESET "coverage")
endif()

set(LUMINUMBRA_TEST_ARTIFACT_ROOT "${CMAKE_BINARY_DIR}/test-artifacts")
file(MAKE_DIRECTORY
    "${LUMINUMBRA_TEST_ARTIFACT_ROOT}/testing"
    "${LUMINUMBRA_TEST_ARTIFACT_ROOT}/ui"
)

# the manifest roster + floors are DERIVED from
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
    "    \"UiSmokeTest.CapturesMaintainedMenuScreenshots\",\n"
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
    "    \"WorldgenPreviewTest.PerFrameRenderProducesComparableObservation\"\n"
    "  ],\n"
    "  \"excluded_patterns\": [\n"
    "    \"_NOT_BUILT$\"\n"
    "  ],\n"
    "  \"passed\": true\n"
    "}\n"
)

# The hidden GLFW runtime boot harness is kept as a manual tool in
# test/performance/run_runtime_boot_metrics.ps1. It is intentionally not a
# required CTest gate because hidden-window event handling is driver-dependent
# on Windows and can exit inside the renderer without diagnostic output.
