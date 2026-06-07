# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(Luminumbra VERSION 0.1.0 LANGUAGES C CXX)

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

include(cmake/engine_version.cmake)

# Fetch all third-party dependencies from a central file
# This makes our project self-contained and portable.
include(cmake/dependencies.cmake)
add_subdirectory(vendor)

# 1. Add the tools subdirectory so the asset_processor target is created.
add_subdirectory(tools)

# 2. Copy authored runtime data into the build tree as part of the build.
set(DATA_SOURCE_DIR ${CMAKE_SOURCE_DIR}/data)
set(DATA_OUTPUT_DIR ${CMAKE_BINARY_DIR}/data)

file(GLOB_RECURSE DATA_SOURCE_FILES CONFIGURE_DEPENDS "${DATA_SOURCE_DIR}/*")
list(FILTER DATA_SOURCE_FILES EXCLUDE REGEX "\\.lmesh$")
set(COPY_DATA_STAMP "${CMAKE_BINARY_DIR}/copy_data.stamp")

add_custom_command(
    OUTPUT "${COPY_DATA_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DATA_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${DATA_SOURCE_DIR}" "${DATA_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E touch "${COPY_DATA_STAMP}"
    DEPENDS ${DATA_SOURCE_FILES}
    COMMENT "Copying runtime data assets"
    VERBATIM
)

add_custom_target(copy_data ALL DEPENDS "${COPY_DATA_STAMP}")

# 3. Find all source mesh assets in a way that reconfigures when assets change.
set(ASSET_SOURCE_DIR ${CMAKE_SOURCE_DIR}/assets)
set(ASSET_OUTPUT_DIR ${DATA_OUTPUT_DIR})

file(GLOB_RECURSE SOURCE_ASSETS CONFIGURE_DEPENDS "${ASSET_SOURCE_DIR}/*.glb")

# 4. Loop through each found asset and create a custom command to process it.
set(PROCESSED_ASSETS)
foreach(SOURCE_ASSET IN LISTS SOURCE_ASSETS)
    # Determine the output path by replacing the directory and extension.
    file(RELATIVE_PATH ASSET_RELATIVE_PATH "${ASSET_SOURCE_DIR}" "${SOURCE_ASSET}")
    set(DESTINATION_ASSET "${ASSET_OUTPUT_DIR}/${ASSET_RELATIVE_PATH}")
    string(REGEX REPLACE "\\.glb$" ".lmesh" DESTINATION_ASSET "${DESTINATION_ASSET}")
    get_filename_component(DESTINATION_DIR "${DESTINATION_ASSET}" DIRECTORY)

    # Get the relative path for prettier log messages during build.
    file(RELATIVE_PATH RELATIVE_SOURCE_PATH ${CMAKE_SOURCE_DIR} ${SOURCE_ASSET})

    # This is the core of the automation. It defines how to create the .lmesh from the .glb.
    add_custom_command(
        OUTPUT "${DESTINATION_ASSET}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DESTINATION_DIR}"
        # The command to run: path/to/asset_processor.exe <input> <output>
        COMMAND $<TARGET_FILE:asset_processor> "${SOURCE_ASSET}" "${DESTINATION_ASSET}"
        # This command depends on the source file and the processor itself.
        # It will re-run if the .glb changes, data is recopied, or asset_processor is recompiled.
        MAIN_DEPENDENCY "${SOURCE_ASSET}"
        DEPENDS asset_processor "${COPY_DATA_STAMP}"
        COMMENT "Processing asset: ${RELATIVE_SOURCE_PATH}"
        VERBATIM
    )

    list(APPEND PROCESSED_ASSETS "${DESTINATION_ASSET}")
endforeach()

# 5. Define a custom target to represent the asset processing step.
if(PROCESSED_ASSETS)
    add_custom_target(process_assets ALL DEPENDS ${PROCESSED_ASSETS})
else()
    add_custom_target(process_assets ALL)
endif()
add_dependencies(process_assets copy_data)

# Add the main game source code subdirectory
add_subdirectory(src)

target_compile_definitions(luminumbra_client_app PRIVATE
    LUMINUMBRA_BUILD_DATA_DIR="${DATA_OUTPUT_DIR}"
    LUMINUMBRA_SOURCE_DATA_DIR="${DATA_SOURCE_DIR}"
)

target_include_directories(luminumbra_common PUBLIC
    "${LUMINUMBRA_ENGINE_VERSION_INCLUDE_DIR}"
)

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
