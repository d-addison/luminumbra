# This file lists all source files for the 'luminumbra_client' targets.
# Paths are anchored to this file so the list is safe to include from any
# CMakeLists.txt in the tree.

set(CLIENT_INTERNAL_SOURCES
    # Core
    ${CMAKE_CURRENT_LIST_DIR}/core/RuntimeScenarioConfig.cpp

    # Audio
    ${CMAKE_CURRENT_LIST_DIR}/audio/AudioManagerFactory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/AudioPropagationSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/AudioSpatialCluster.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/EnvironmentalAudioSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/MiniaudioManager.cpp

    # Debug
    ${CMAKE_CURRENT_LIST_DIR}/debug/RuntimeOverlaySchema.cpp
    ${CMAKE_CURRENT_LIST_DIR}/debug/WorldGenViewer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/debug/DebugCamera.cpp

    # Player
    ${CMAKE_CURRENT_LIST_DIR}/player/PlayerController.cpp

    # Rendering
    ${CMAKE_CURRENT_LIST_DIR}/rendering/AsyncReadbackRing.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/CaptureHooks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/FarLodSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/FrameScan.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/FrameHealth.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/GlDebugOutput.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/ImpostorBake.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/SceneSurvey.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/LightningBolt.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/Mesh.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/RenderPipeline.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/RenderResourceRegistry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/WaterfallDetect.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/RenderSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/Shader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/ShaderReflection.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/PassShaderLayouts.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/WorldLoadingVisualizer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/GBufferPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/LightingPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/ShadowPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/SkyboxPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/ParticlePass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/FoliagePass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/PlantProcgenPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/GroundDecalPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/DebugViewPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/SsaoPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/WaterPass.cpp

    # World (client-side)
    ${CMAKE_CURRENT_LIST_DIR}/world/WorldgenOverride.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/WorldgenPreview.cpp
    # world-dressing placement computation (extracted from main_client's
    # first-IN_GAME-frame scatter/wildlife bring-up; runs on a background job).
    ${CMAKE_CURRENT_LIST_DIR}/WorldDressing.cpp

    # UI
    ${CMAKE_CURRENT_LIST_DIR}/ui/Rml_Interfaces.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/Rml_UIManager.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/components/common/Button.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/components/common/Input.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/components/common/Panel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/components/game/WorldList.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/core/UIComponent.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/core/UIDataStream.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/core/UIHotReload.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/core/UIStateManager.cpp
    # RmlUi reference GL3 backend (vendored copy of the 6.1 renderer). It implements the
    # layered/filter/clip-mask render API absent from the old renderer, so
    # backdrop-filter / filter / box-shadow actually render. Compiled against the engine's
    # own glad loader via RMLUI_GL3_CUSTOM_LOADER (set below) instead of its bundled glad.
    ${CMAKE_CURRENT_LIST_DIR}/ui/gl3/RmlUi_Renderer_GL3.cpp
)

# Route the vendored GL3 backend at the engine's glad (gl 4.6 core) instead of the glad 2.x
# loader it bundles — two GL loaders in one link would collide. The custom-loader hook makes
# RmlGL3::Initialize/Shutdown no-ops (the engine already owns the GL context + loader).
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/ui/gl3/RmlUi_Renderer_GL3.cpp
    PROPERTIES COMPILE_DEFINITIONS "RMLUI_GL3_CUSTOM_LOADER=<glad/glad.h>")

# QA scenario-harness sources (the luminumbra_client_qa static library).
set(CLIENT_QA_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/core/RuntimeScenarioHarness.cpp
)

# Third-party implementation sources are owned by their dependency targets.
set(CLIENT_VENDOR_SOURCES)

# Main client executable sources.
set(CLIENT_APP_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/main_client.cpp
)

# Complete executable source set for legacy consumers that include this file
# directly.
set(CLIENT_SOURCES
    ${CLIENT_INTERNAL_SOURCES}
    ${CLIENT_QA_SOURCES}
    ${CLIENT_VENDOR_SOURCES}
    ${CLIENT_APP_SOURCES}
)

# RuntimeScenarioHarness.cpp instantiates enough EnTT storage
# templates (debug, no inlining) to overflow the default COFF section limit
# on MinGW; -mbig-obj lifts it (same pattern googletest/nlohmann use).
# main_client.cpp is a large monolith that likewise overflows once it gains more
# EnTT emplace/view instantiations (relocation truncated to fit IMAGE_REL_AMD64_ADDR32NB);
# give it the same treatment so the app TU keeps room to grow.
if(MINGW)
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/core/RuntimeScenarioHarness.cpp
        PROPERTIES COMPILE_OPTIONS "-Wa,-mbig-obj")
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/main_client.cpp
        PROPERTIES COMPILE_OPTIONS "-Wa,-mbig-obj")
endif()
