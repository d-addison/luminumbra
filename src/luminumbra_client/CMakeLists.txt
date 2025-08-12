# Declare the executable target for the client application.
add_executable(luminumbra_client_app WIN32 "")

# Use a separate file to manage the list of source files for this target.
include(sources.cmake)
target_sources(luminumbra_client_app PRIVATE ${CLIENT_SOURCES})

#-----------------------------------------------------------------------------
# USAGE REQUIREMENTS & CONFIGURATION
#-----------------------------------------------------------------------------

# Add include directories required by this target.
target_include_directories(luminumbra_client_app PUBLIC
    # Allow including local headers relative to this directory (e.g., "audio/IAudioManager.h").
    ${CMAKE_CURRENT_SOURCE_DIR}
)

#-----------------------------------------------------------------------------
# LINKING
# Specify which libraries this executable depends on. Linking against these
# targets automatically handles their include paths and build order.
#-----------------------------------------------------------------------------

target_link_libraries(luminumbra_client_app PRIVATE
    # --- Our own libraries ---
    luminumbra_common

    # --- Vendor libraries ---
    # Graphics & Windowing
    glfw
    glad
    glm
    
    # UI & UI Dependencies
    rmlui
    soil2
    stb_image
    freetype

    # Audio
    # miniaudio
    nlohmann_json

    # System
    Threads::Threads

    opengl32
    gdi32
    winmm
    shell32
    ole32
    user32
    jolt
)

# --- Automatic DLL Copying ---
# This command runs after the client is built successfully.
# It copies the required DLLs to the same directory as the executable.

function(copy_dll_to_target target_name dependency_name)
    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${dependency_name}>
            $<TARGET_FILE_DIR:${target_name}>
        COMMENT "Copying ${dependency_name}.dll to output directory"
    )
endfunction()

copy_dll_to_target(luminumbra_client_app RmlUi::Core)
copy_dll_to_target(luminumbra_client_app RmlUi::Debugger)
copy_dll_to_target(luminumbra_client_app soil2)
