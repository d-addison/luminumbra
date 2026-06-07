# This file lists all source files for the 'luminumbra_client' targets.
# Paths are anchored to this file so the list is safe to include from any
# CMakeLists.txt in the tree.

set(CLIENT_INTERNAL_SOURCES
    # Audio
    ${CMAKE_CURRENT_LIST_DIR}/audio/AudioManagerFactory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/AudioPropagationSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/AudioSpatialCluster.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/EnvironmentalAudioSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/MiniaudioManager.cpp

    # Debug
    ${CMAKE_CURRENT_LIST_DIR}/debug/WorldGenViewer.cpp

    # Player
    ${CMAKE_CURRENT_LIST_DIR}/player/PlayerController.cpp

    # Rendering
    ${CMAKE_CURRENT_LIST_DIR}/rendering/Mesh.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/RenderPipeline.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/RenderSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/Shader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rendering/WorldLoadingVisualizer.cpp

    # UI
    ${CMAKE_CURRENT_LIST_DIR}/ui/EnhancedUIManager.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/Rml_Interfaces.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/Rml_UIManager.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/UIIntegration.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/components/common/Button.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/components/common/Input.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/components/common/Panel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/components/game/WorldList.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/core/UIComponent.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/core/UIDataStream.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/core/UIHotReload.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/core/UIManager.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui/core/UIStateManager.cpp
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
