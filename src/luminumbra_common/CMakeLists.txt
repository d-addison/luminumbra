# Include the list of source files for this library.
include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

# Create the static library with all its source files at once.
add_library(luminumbra_common STATIC ${COMMON_SOURCES})

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
