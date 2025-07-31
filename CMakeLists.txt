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

add_subdirectory(tools/ModelConverter)

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

# minaudio
add_library(minaudio INTERFACE)
target_include_directories(minaudio INTERFACE external/minaudio)

# ====================================================================
# AUTOMATIC MODEL CONVERSION
# ====================================================================

# 1. Define the specific source model and its desired output location.
set(SOURCE_TREE_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/res/models/tree/tree.obj")
set(CONVERTED_TREE_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/res/models/converted/tree/tree.lbm")

# 2. Create a list that will contain all our converted models.
set(CONVERTED_MODELS ${CONVERTED_TREE_MODEL})

get_filename_component(OUTPUT_DIR ${CONVERTED_TREE_MODEL} DIRECTORY)
file(MAKE_DIRECTORY ${OUTPUT_DIR})

# 3. Create a single, explicit custom command for the tree model.
add_custom_command(
    OUTPUT ${CONVERTED_TREE_MODEL}
    # Use $<TARGET_FILE:...> to get the full path to the converter executable.
    COMMAND $<TARGET_FILE:model_converter> ${SOURCE_TREE_MODEL} ${CONVERTED_TREE_MODEL}
    # This command depends on the source .obj file and the converter itself.
    DEPENDS ${SOURCE_TREE_MODEL} model_converter
    COMMENT "Converting tree model: ${SOURCE_TREE_MODEL}"
    VERBATIM
)

# --- Define Our Executable ---

add_executable(Luminumbra 
    ${LUMINUMBRA_SOURCES}
    ${CONVERTED_MODELS}
)

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
    minaudio

    # System libraries
    OpenGL::GL # Modern way to link OpenGL
    stdc++fs
)