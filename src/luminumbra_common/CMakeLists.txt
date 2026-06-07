# Include the list of source files for this library
include(sources.cmake)

# Create the static library with all its source files at once.
# The paths in COMMON_SOURCES are relative to this CMakeLists.txt file.
add_library(luminumbra_common STATIC ${COMMON_SOURCES})

# --- CORRECTED USAGE REQUIREMENTS ---
target_include_directories(luminumbra_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/vendor/spdlog/include"
    "${CMAKE_SOURCE_DIR}/vendor/soil2/src"
    "${CMAKE_SOURCE_DIR}/external/FastNoiseLite"
    "${CMAKE_SOURCE_DIR}/vendor"
)

target_link_libraries(luminumbra_common PUBLIC 
    entt
    Jolt
    nlohmann_json
    sol2
    lua
    spdlog
    lz4
    glm
    FastNoise
)