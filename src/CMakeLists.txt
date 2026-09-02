# src/CMakeLists.txt

# --- Common Library (Core Engine Logic) ---
add_subdirectory(luminumbra_common)

# --- Client Library (Rendering, UI, Input) ---
# The shared source manifest is the single source of truth for the client.
include("${CMAKE_CURRENT_LIST_DIR}/luminumbra_client/sources.cmake")

# Create the library from the first-party client source manifest.
add_library(luminumbra_client STATIC
    ${CLIENT_INTERNAL_SOURCES}
    ${CLIENT_VENDOR_SOURCES}
)

# Set up include directories for the client
target_include_directories(luminumbra_client PUBLIC
    # The client's own root directory
    ${CMAKE_CURRENT_SOURCE_DIR}/luminumbra_client

    # The 'src' directory, which is the root for includes like "luminumbra_common/..."
    ${CMAKE_CURRENT_SOURCE_DIR}

    # vendor root, for single-header vendor libs included as "<lib>/<header>.h"
    # (e.g. renderdoc/renderdoc_app.h -- the  in-app capture API).
    ${CMAKE_SOURCE_DIR}/vendor
)

target_link_libraries(luminumbra_client PUBLIC
    luminumbra_common
    rmlui
    glfw
    miniaudio
    soil2
    glad
    Jolt::Jolt
    spdlog
    glm
    nlohmann_json
    sol2
    lua
    imgui
    stb_headers
)

# --- Headless Server Executable ---
# Simulation authority only: links luminumbra_common and NOTHING client-side
# (no OpenGL, GLFW, miniaudio, imgui, RmlUi). The ServerHeadlessHygiene ctest
# and the HeadlessServerTick validator mode enforce the include boundary.
include("${CMAKE_CURRENT_LIST_DIR}/luminumbra_server/sources.cmake")

add_executable(luminumbra_server_app
    ${SERVER_APP_SOURCES}
)

target_include_directories(luminumbra_server_app PRIVATE
    # The 'src' directory, the root for includes like "luminumbra_common/...".
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(luminumbra_server_app PRIVATE
    luminumbra_common
    # Tracy seam: empty INTERFACE when LUMINUMBRA_ENABLE_TRACY=OFF (default) -> zero
    # effect, byte-identical --smoke. Carries the define + TracyClient when ON.
    luminumbra_profiler
)

# First-party hygiene (warnings-as-errors per the build preset).
luminumbra_configure_first_party_target(luminumbra_server_app)

# when the Steamworks transport is built in, the redistributable steam_api64.dll
# must sit next to the server exe at runtime. Copy it post-build.
if(LUMINUMBRA_ENABLE_STEAM AND WIN32)
    add_custom_command(TARGET luminumbra_server_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LUMINUMBRA_STEAM_SDK_DIR}/redistributable_bin/win64/steam_api64.dll"
            "$<TARGET_FILE_DIR:luminumbra_server_app>/steam_api64.dll"
        COMMENT "Copying steam_api64.dll next to luminumbra_server_app")
elseif(LUMINUMBRA_ENABLE_STEAM AND UNIX AND NOT APPLE)
    # mirror of the win64 copy: the linux64 redistributable is a shared object the
    # loader must find next to the exe (the link references the SDK path, the
    # runtime should not).
    add_custom_command(TARGET luminumbra_server_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LUMINUMBRA_STEAM_SDK_DIR}/redistributable_bin/linux64/libsteam_api.so"
            "$<TARGET_FILE_DIR:luminumbra_server_app>/libsteam_api.so"
        COMMENT "Copying libsteam_api.so next to luminumbra_server_app")
endif()

# --- Client Executable ---
add_executable(luminumbra_client_app
    ${CLIENT_APP_SOURCES}
)

target_link_libraries(luminumbra_client_app PRIVATE
    luminumbra_client
)

# Link OpenGL
if(WIN32)
    target_link_libraries(luminumbra_client_app PRIVATE opengl32 DbgHelp Psapi)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(luminumbra_client_app PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(luminumbra_client_app PRIVATE OpenGL::GL)
endif()
