cmake_minimum_required(VERSION 3.10)

project(Luminumbra LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Add Dependencies ---

# Add GLFW from the external directory
add_subdirectory(external/glfw-3.4 EXCLUDE_FROM_ALL)

# Add GLAD source files
set(GLAD_SRC ${CMAKE_CURRENT_SOURCE_DIR}/external/glad/src/gl.c)
set(GLAD_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/glad/include)

# Find the OpenGL library itself
find_package(OpenGL REQUIRED)

# --- Our Executable ---

add_executable(Luminumbra
    src/luminumbra/main.cpp
    src/luminumbra/core/Engine.cpp
    src/luminumbra/core/ExternalImplementations.cpp
    src/luminumbra/rendering/Shader.cpp
    src/luminumbra/rendering/Camera.cpp
    src/luminumbra/world/Chunk.cpp
    src/luminumbra/world/MarchingCubes.cpp
    src/luminumbra/world/World.cpp
    ${GLAD_SRC}
)

# --- Link Dependencies ---

# Header-only libraries
set(GLM_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/glm-1.0.1)
set(FASTNOISE_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/FastNoiseLite)
target_compile_definitions(Luminumbra PRIVATE GLM_ENABLE_EXPERIMENTAL)

target_include_directories(Luminumbra PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include # <-- IMPORTANT: Make our new /include dir available
    ${GLAD_INCLUDE_DIR}
    ${GLM_INCLUDE_DIR}
    ${FASTNOISE_INCLUDE_DIR}
)

# Link our game against GLFW and OpenGL
target_link_libraries(Luminumbra PRIVATE
    glfw
    ${OPENGL_LIBRARIES}
)