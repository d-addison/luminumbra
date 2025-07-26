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
    src/luminumbra/rendering/Shader.cpp # <-- ADD THIS LINE
    ${GLAD_SRC}
)

# --- Link Dependencies ---

target_include_directories(Luminumbra PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include # <-- IMPORTANT: Make our new /include dir available
    ${GLAD_INCLUDE_DIR}
)

# Link our game against GLFW and OpenGL
target_link_libraries(Luminumbra PRIVATE
    glfw
    ${OPENGL_LIBRARIES}
)