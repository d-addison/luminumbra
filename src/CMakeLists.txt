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

# --- Client QA Library (RuntimeScenarioHarness) ---
# The QA scenario harness (the gate/scenario driver behind
# `luminumbra_client_qa_app --scenario <name>`) lives in its own static library
# so interactive-client code and QA-harness code stop sharing one target. It
# links luminumbra_client PUBLIC for the include dirs + engine/render
# dependencies. The dependency is strictly one-way: the harness drives
# RenderPipeline/GameSession from luminumbra_client, while the helpers the
# client once pulled back out of the harness (WritePixelBufferPpm,
# FoliageSurfaceQuery, the GL debug counters) now live in luminumbra_client
# itself (rendering/PixelIo, rendering/FoliageSurface,
# core/RuntimeScenarioConfig), so no client TU references a harness symbol.
add_library(luminumbra_client_qa STATIC
    ${CLIENT_QA_SOURCES}
)

target_link_libraries(luminumbra_client_qa PUBLIC
    luminumbra_client
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

# --- Client Executables ---
# Two binaries share the main_client.cpp entry TU:
#   luminumbra_client_app     - the shipping interactive client. Links ONLY
#                               luminumbra_client (no QA harness in the image);
#                               `--scenario` exits with the documented
#                               availability error (exit code 6).
#   luminumbra_client_qa_app  - the QA/gate binary. Compiles the same TU with
#                               LUMINUMBRA_QA_RUNNER=1 (enables the
#                               CreateScenarioRunner call) and links the
#                               harness. tools/gates scenario runs target this.
add_executable(luminumbra_client_app
    ${CLIENT_APP_SOURCES}
)

target_link_libraries(luminumbra_client_app PRIVATE
    luminumbra_client
)

add_executable(luminumbra_client_qa_app
    ${CLIENT_APP_SOURCES}
)

target_compile_definitions(luminumbra_client_qa_app PRIVATE
    LUMINUMBRA_QA_RUNNER=1
)

target_link_libraries(luminumbra_client_qa_app PRIVATE
    luminumbra_client_qa
    luminumbra_client
)

# Link OpenGL (both client executables)
foreach(CLIENT_EXE_TARGET IN ITEMS luminumbra_client_app luminumbra_client_qa_app)
    if(WIN32)
        target_link_libraries(${CLIENT_EXE_TARGET} PRIVATE opengl32 DbgHelp Psapi)
    elseif(APPLE)
        find_package(OpenGL REQUIRED)
        target_link_libraries(${CLIENT_EXE_TARGET} PRIVATE ${OPENGL_LIBRARIES})
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(${CLIENT_EXE_TARGET} PRIVATE OpenGL::GL)
    endif()
endforeach()
