# Dear ImGui CMakeLists.txt for Luminumbra
cmake_minimum_required(VERSION 3.20)

# Find OpenGL
find_package(OpenGL REQUIRED)

# Define the imgui library
add_library(imgui STATIC
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    
    # Platform-specific backends
    backends/imgui_impl_glfw.cpp
    backends/imgui_impl_opengl3.cpp
)

# Public headers
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/backends
)

# Link against required dependencies
target_link_libraries(imgui PUBLIC
    glfw
    glad
    ${OPENGL_LIBRARIES}
)

# Compiler-specific settings
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(imgui PRIVATE -Wall -Wextra -Wno-unused-parameter)
elseif(MSVC)
    target_compile_options(imgui PRIVATE /W3)
endif()

# C++20 standard requirement
target_compile_features(imgui PUBLIC cxx_std_20)

# Create alias for consistency
add_library(imgui::imgui ALIAS imgui)