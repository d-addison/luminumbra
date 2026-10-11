cmake_minimum_required(VERSION 3.24)
project(LuminumbraInstalledConsumer LANGUAGES CXX)

# This directory can be copied out of the repository and built on its own.
set(CMAKE_FIND_USE_PACKAGE_REGISTRY FALSE)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY FALSE)
find_package(LuminumbraStaticPreview CONFIG REQUIRED)
get_target_property(preview_imported Luminumbra::StaticPreview IMPORTED)
if(NOT preview_imported)
    message(FATAL_ERROR "The consumer requires the installed imported SDK target")
endif()

add_executable(installed_consumer main.cpp)
target_link_libraries(installed_consumer PRIVATE Luminumbra::StaticPreview)
