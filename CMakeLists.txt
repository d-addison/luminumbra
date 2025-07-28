cmake_minimum_required(VERSION 3.12)
project(Luminumbra LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# --- Automatically find all our source files ---
file(GLOB_RECURSE LUMINUMBRA_SOURCES "src/luminumbra/*.cpp")

message(STATUS "Found source files:")
foreach(SOURCE ${LUMINUMBRA_SOURCES})
    message(STATUS "  ${SOURCE}")
endforeach()

# --- Define Dependencies as Targets for Cleaner Linking ---

# GLM (already a modern target)
add_subdirectory(external/glm-1.0.1 EXCLUDE_FROM_ALL)

# GLFW (already a modern target)
add_subdirectory(external/glfw-3.4 EXCLUDE_FROM_ALL)

# OpenGL
find_package(OpenGL REQUIRED)

# GLAD
# We create a simple library for GLAD to group its source and include path.
add_library(glad external/glad/src/gl.c)
target_include_directories(glad PUBLIC external/glad/include)

# ImGui
# We compile ImGui into its own library instead of adding sources to our exe.
add_library(imgui
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_glfw.cpp
    external/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    external/imgui
    external/imgui/backends
)
# ImGui needs to know where GLFW and OpenGL are
target_link_libraries(imgui PRIVATE glfw ${OpenGL_LIBRARIES})


# Header-only dependencies are defined as INTERFACE libraries.
# They don't compile anything, but they hold properties like include paths.

# nlohmann-json
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE external/nlohmann)

# FastNoiseLite
add_library(fastnoise INTERFACE)
target_include_directories(fastnoise INTERFACE external/FastNoiseLite)

# STB
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE external/stb)

# --- Define Our Executable ---

add_executable(Luminumbra ${LUMINUMBRA_SOURCES})

# Add our project's own include directory
target_include_directories(Luminumbra PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Add any compile definitions specific to our executable
target_compile_definitions(Luminumbra PRIVATE GLM_ENABLE_EXPERIMENTAL)

# --- Link All Dependencies ---

target_link_libraries(Luminumbra PRIVATE
    # Our compiled dependency targets
    glad
    imgui
    glfw      # From add_subdirectory
    glm::glm  # From add_subdirectory

    # Interface (header-only) targets
    nlohmann_json
    fastnoise
    stb

    # System libraries
    OpenGL::GL # Modern way to link OpenGL
    stdc++fs
)