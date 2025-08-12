# src/CMakeLists.txt

# --- Common Library (Core Engine Logic) ---
add_subdirectory(luminumbra_common)

# --- Client Library (Rendering, UI, Input) ---
# Include the sources list which now defines CLIENT_INTERNAL_SOURCES and CLIENT_VENDOR_SOURCES
include(luminumbra_client/sources.cmake)

# Prefix only the internal sources with the subdirectory path
set(CLIENT_INTERNAL_SOURCES_PREFIXED)
foreach(source ${CLIENT_INTERNAL_SOURCES})
    list(APPEND CLIENT_INTERNAL_SOURCES_PREFIXED "luminumbra_client/${source}")
endforeach()

# Create the library with BOTH the prefixed internal sources and the absolute-path vendor sources
add_library(luminumbra_client STATIC
    ${CLIENT_INTERNAL_SOURCES_PREFIXED}
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

# --- Client Executable ---
add_executable(luminumbra_client_app
    luminumbra_client/main_client.cpp
)

target_link_libraries(luminumbra_client_app PRIVATE
    luminumbra_client
)

# Link OpenGL
if(WIN32)
    target_link_libraries(luminumbra_client_app PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(luminumbra_client_app PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(luminumbra_client_app PRIVATE OpenGL::GL)
endif()