# This file lists all source files for the 'luminumbra_client' target.

# List of source files that are part of the client library itself.
# These paths are relative to the 'src/luminumbra_client' directory.
# CRITICAL: main_client.cpp is NOT listed here, as it's the entry point 
# for the executable, not the library.
set(CLIENT_INTERNAL_SOURCES
    debug/WorldGenViewer.cpp

    # Core
    core/Log.cpp
    
    # Audio
    audio/AudioManagerFactory.cpp
    audio/MiniaudioManager.cpp

    # UI
    ui/Rml_UIManager.cpp
    ui/RmlUi_Backend.cpp
    ui/Rml_Interfaces.cpp

    # Rendering
    rendering/RenderSystem.cpp
    rendering/RenderPipeline.cpp
    rendering/Shader.cpp
)

# List of vendor source files that need to be compiled with the client.
# These paths are absolute from the project root to avoid prefixing issues.
set(CLIENT_VENDOR_SOURCES
    ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/backends/imgui_impl_opengl3.cpp
)