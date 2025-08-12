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
add_subdirectory(vendor)

# 1. Add the tools subdirectory so the asset_processor target is created.
add_subdirectory(tools)

# 2. Define a custom target to represent the asset processing step.
#    This target doesn't build anything itself, but other targets can depend on it.
add_custom_target(process_assets ALL)

# 3. Find all source assets (.glb files) in the 'assets' directory.
set(ASSET_SOURCE_DIR ${CMAKE_SOURCE_DIR}/assets)
set(ASSET_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/data) # We process into the source 'data' dir

file(GLOB_RECURSE SOURCE_ASSETS "${ASSET_SOURCE_DIR}/*.glb")

# 4. Loop through each found asset and create a custom command to process it.
foreach(SOURCE_ASSET ${SOURCE_ASSETS})
    # Determine the output path by replacing the directory and extension.
    string(REPLACE "${ASSET_SOURCE_DIR}" "${ASSET_OUTPUT_DIR}" DESTINATION_ASSET ${SOURCE_ASSET})
    string(REPLACE ".glb" ".lmesh" DESTINATION_ASSET ${DESTINATION_ASSET})

    # Get the relative path for prettier log messages during build.
    file(RELATIVE_PATH RELATIVE_SOURCE_PATH ${CMAKE_SOURCE_DIR} ${SOURCE_ASSET})

    # This is the core of the automation. It defines how to create the .lmesh from the .glb.
    add_custom_command(
        OUTPUT ${DESTINATION_ASSET}
        # The command to run: path/to/asset_processor.exe <input> <output>
        COMMAND $<TARGET_FILE:asset_processor> ${SOURCE_ASSET} ${DESTINATION_ASSET}
        # This command depends on the source file and the processor itself.
        # It will only re-run if the .glb changes or if asset_processor is recompiled.
        MAIN_DEPENDENCY ${SOURCE_ASSET}
        DEPENDS asset_processor
        COMMENT "Processing asset: ${RELATIVE_SOURCE_PATH}"
        VERBATIM
    )
    
    # Add the generated file as a dependency to our custom target.
    add_dependencies(process_assets ${DESTINATION_ASSET})

endforeach()

# Add the main game source code subdirectory
add_subdirectory(src)

# --- FINAL DEPENDENCY LINK ---
# Make the main game executable depend on the asset processing target.
# This ensures all assets are processed *before* the game is linked.
add_dependencies(luminumbra_client_app process_assets)

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
