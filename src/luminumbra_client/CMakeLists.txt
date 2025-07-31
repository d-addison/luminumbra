# Declare the executable target for the client application.
# WIN32 creates an application without a console window on Windows.
add_executable(luminumbra_client WIN32 "")

# Use a separate file to manage the list of source files for this target.
# This keeps the main CMake file clean and focused on build logic.
include(sources.cmake)
target_sources(luminumbra_client PRIVATE ${CLIENT_SOURCES})

#-----------------------------------------------------------------------------
# USAGE REQUIREMENTS & CONFIGURATION
# Define the properties this target needs to compile correctly.
#-----------------------------------------------------------------------------

# Add include directories required by this target.
# These are PRIVATE because this is an executable and won't be linked against.
target_include_directories(luminumbra_client PRIVATE
    # 1. Allow including local headers relative to this directory (e.g., "audio/IAudioManager.h").
    ${CMAKE_CURRENT_SOURCE_DIR}

    # 2. Add include paths for dependencies fetched by FetchContent.
    #    This is necessary because their targets don't always export them correctly.
    ${CMAKE_BINARY_DIR}/_deps/glad-src/include     # For <glad/glad.h>
    ${CMAKE_BINARY_DIR}/_deps/rmlui-src/Include    # For <RmlUi/Core.h>
)

# Add preprocessor definitions required for compilation.
target_compile_definitions(luminumbra_client PRIVATE
    # Fixes a build issue with RmlUi's internal robin_hood.h header,
    # which can fail to auto-detect the 64-bit architecture in some environments.
    RMLUI_ARCH_64
)

#-----------------------------------------------------------------------------
# LINKING
# Specify which libraries this executable depends on.
#-----------------------------------------------------------------------------

target_link_libraries(luminumbra_client PRIVATE
    # --- Our own libraries ---
    # This automatically brings in its usage requirements (like include paths).
    luminumbra_common

    # --- Vendor libraries ---
    # Graphics & Windowing
    glfw
    glad
    glm::glm
    
    # UI & UI Dependencies
    RmlCore 
    SOIL2
    Freetype::Freetype

    # System
    Threads::Threads
)