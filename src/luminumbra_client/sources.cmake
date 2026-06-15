# This file lists all source files for the 'luminumbra_client' targets.
# Paths are anchored to this file so the list is safe to include from any
# CMakeLists.txt in the tree.

set(CLIENT_INTERNAL_SOURCES
    # Core
    ${CMAKE_CURRENT_LIST_DIR}/core/RuntimeScenarioHarness.cpp

    # Audio
    ${CMAKE_CURRENT_LIST_DIR}/audio/AudioManagerFactory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/AudioPropagationSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/AudioSpatialCluster.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/EnvironmentalAudioSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/MiniaudioManager.cpp

    # Debug
    ${CMAKE_CURRENT_LIST_DIR}/debug/RuntimeOverlaySchema.cpp
    ${CMAKE_CURRENT_LIST_DIR}/debug/WorldGenViewer.cpp

    # Player
    ${CMAKE_CURRENT_LIST_DIR}/player/PlayerController.cpp

    # Rendering
    ${CMAKE_CURRENT_LIST_DIR}/rendering/CaptureHooks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/FarLodSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/LightningBolt.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/Mesh.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/RenderPipeline.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/RenderSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/Shader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/WorldLoadingVisualizer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/GBufferPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/LightingPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/ShadowPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/SkyboxPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/ParticlePass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/SsaoPass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/passes/WaterPass.cpp

    # UI
    ${CMAKE_CURRENT_LIST_DIR}/ui/Rml_Interfaces.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/Rml_UIManager.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/core/UIDataStream.cpp
)

# List of vendor source files that need to be compiled with the client.
set(CLIENT_VENDOR_SOURCES
    ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/backends/imgui_impl_opengl3.cpp
)

# Main client executable sources.
set(CLIENT_APP_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/main_client.cpp
)

# Complete executable source set for legacy consumers that include this file
# directly.
set(CLIENT_SOURCES
    ${CLIENT_INTERNAL_SOURCES}
    ${CLIENT_VENDOR_SOURCES}
    ${CLIENT_APP_SOURCES}
)

# T-I3-18: RuntimeScenarioHarness.cpp instantiates enough EnTT storage
# templates (debug, no inlining) to overflow the default COFF section limit
# on MinGW; -mbig-obj lifts it (same pattern googletest/nlohmann use).
if(MINGW)
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/core/RuntimeScenarioHarness.cpp
        PROPERTIES COMPILE_OPTIONS "-Wa,-mbig-obj")
endif()
