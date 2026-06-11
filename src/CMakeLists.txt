# src/CMakeLists.txt

# --- Common Library (Core Engine Logic) ---
add_subdirectory(luminumbra_common)

# --- Client Library (Rendering, UI, Input) ---
# src/luminumbra_client/CMakeLists.txt is not the active entry point; keep the
# shared source manifest below as the single source of truth for the client.
include("${CMAKE_CURRENT_LIST_DIR}/luminumbra_client/sources.cmake")

# Create the library with both first-party client sources and vendored ImGui
# backend sources.
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
    
    # Vendor includes specific to the client
    ${CMAKE_SOURCE_DIR}/vendor/imgui
    ${CMAKE_SOURCE_DIR}/vendor/stb
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
)

# --- Headless Server Executable (T-I3-12) ---
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
)

# First-party hygiene (warnings-as-errors per the build preset).
luminumbra_configure_first_party_target(luminumbra_server_app)

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
