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

option(LUMINUMBRA_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" OFF)
option(LUMINUMBRA_ENABLE_ASAN "Enable AddressSanitizer for first-party targets in Debug builds" OFF)

function(luminumbra_configure_first_party_target TARGET_NAME)
  if(NOT TARGET ${TARGET_NAME})
    return()
  endif()

  target_compile_definitions(${TARGET_NAME} PRIVATE
    $<$<CONFIG:Debug>:LUMINUMBRA_DEBUG>
  )

  if(MSVC)
    target_compile_options(${TARGET_NAME} PRIVATE /W4 /permissive- /Zc:__cplusplus)
    if(LUMINUMBRA_WARNINGS_AS_ERRORS)
      target_compile_options(${TARGET_NAME} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${TARGET_NAME} PRIVATE -Wall -Wextra -Wpedantic)
    if(LUMINUMBRA_WARNINGS_AS_ERRORS)
      target_compile_options(${TARGET_NAME} PRIVATE -Werror -Wno-error=unused-parameter)
    endif()
  endif()

  if(LUMINUMBRA_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${TARGET_NAME} PRIVATE $<$<CONFIG:Debug>:/fsanitize=address>)
      target_link_options(${TARGET_NAME} PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL:NO>)
    else()
      target_compile_options(${TARGET_NAME} PRIVATE $<$<CONFIG:Debug>:-fsanitize=address>)
      target_link_options(${TARGET_NAME} PRIVATE $<$<CONFIG:Debug>:-fsanitize=address>)
    endif()
  endif()
endfunction()

# Fetch all third-party dependencies from a central file
# This makes our project self-contained and portable.
include(cmake/dependencies.cmake)
add_subdirectory(vendor)

# 1. Add the tools subdirectory so the asset_processor target is created.
add_subdirectory(tools)

# 2. Find all source assets (.glb files) in the 'assets' directory.
set(ASSET_SOURCE_DIR ${CMAKE_SOURCE_DIR}/assets)
set(ASSET_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/data) # We process into the source 'data' dir

file(GLOB_RECURSE SOURCE_ASSETS CONFIGURE_DEPENDS "${ASSET_SOURCE_DIR}/*.glb")
set(PROCESSED_ASSETS)

# 3. Loop through each found asset and create a custom command to process it.
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
    
    list(APPEND PROCESSED_ASSETS ${DESTINATION_ASSET})

endforeach()

# 4. Define a custom target to represent the asset processing step.
add_custom_target(process_assets ALL DEPENDS ${PROCESSED_ASSETS})

# Add the main game source code subdirectory
add_subdirectory(src)

# --- FINAL DEPENDENCY LINK ---
# Make the main game executable depend on the asset processing target.
# This ensures all assets are processed *before* the game is linked.
add_dependencies(luminumbra_client_app process_assets)

# Enable testing framework
enable_testing()
add_subdirectory(test)

foreach(FIRST_PARTY_TARGET IN ITEMS
  asset_processor
  luminumbra_common
  luminumbra_client_app
  world_generation_test
)
  luminumbra_configure_first_party_target(${FIRST_PARTY_TARGET})
endforeach()

# Copy runtime data assets to the build directory as part of every build.
add_custom_target(copy_data ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/data"
          "${CMAKE_BINARY_DIR}/data"
  COMMENT "Copying runtime data assets"
  VERBATIM
)
add_dependencies(copy_data process_assets)
add_dependencies(luminumbra_client_app copy_data)
