# CMakeLists.txt (root)
cmake_minimum_required(VERSION 3.16)
project(Luminumbra VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DRMLUI_ARCH_64" CACHE STRING "Set C++ compiler flags" FORCE)

# --- Include our automated dependency manager ---
include(cmake/dependencies.cmake)

# --- Subdirectories for our code ---
add_subdirectory(src/luminumbra_common)
add_subdirectory(src/luminumbra_client)
add_subdirectory(src/luminumbra_server)

# --- Top-level tools ---
add_subdirectory(tools)