# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(Luminumbra LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set build output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Fetch all third-party dependencies from a central file
# This makes our project self-contained and portable.
include(cmake/dependencies.cmake)

# Add local third-party dependencies
add_subdirectory(vendor)

# Add the source code subdirectory
add_subdirectory(src)

# Enable testing framework
enable_testing()
add_subdirectory(test)

# Copy runtime data assets to the build directory
file(COPY data DESTINATION ${CMAKE_BINARY_DIR})

# Set compiler flags for performance and debugging
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  message(STATUS "Build type: Release")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(STATUS "Build type: Debug")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -Wall")
  add_compile_definitions(LUMINUMBRA_DEBUG)
  # Enable address sanitizer for memory safety checks in debug builds
  # add_link_options(-fsanitize=address)
  # add_compile_options(-fsanitize=address)
endif()
