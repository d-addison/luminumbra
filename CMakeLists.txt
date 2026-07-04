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
option(LUMINUMBRA_ENABLE_COVERAGE "Instrument first-party targets with gcov coverage counters (--coverage)" OFF)

# Coverage instrumentation (coverage-instrumentation slice). Applied ONLY when
# LUMINUMBRA_ENABLE_COVERAGE is set, which is owned exclusively by the dedicated
# `coverage` preset (binaryDir build/coverage). It appends GCC/Clang's
# --coverage (== -fprofile-arcs -ftest-coverage) to compile + link options of
# first-party targets so gcov emits .gcno/.gcda counters. It is a no-op on MSVC
# (no gcov) and never touches -ffp-contract=off: the determinism contract is
# about FP contraction / integer RNG / id-ordering, none of which --coverage
# relaxes, and the coverage tree is never hashed, so any residual codegen drift
# is irrelevant. Vendor targets are untouched (the helper only runs on
# first-party targets routed through luminumbra_configure_first_party_target).
function(luminumbra_enable_coverage TARGET_NAME)
  if(NOT LUMINUMBRA_ENABLE_COVERAGE)
    return()
  endif()
  if(NOT TARGET ${TARGET_NAME})
    return()
  endif()
  if(MSVC)
    return()
  endif()
  get_target_property(_lec_type ${TARGET_NAME} TYPE)
  if(_lec_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()
  # Instrument this target's own translation units (PRIVATE).
  target_compile_options(${TARGET_NAME} PRIVATE --coverage)
  if(_lec_type STREQUAL "STATIC_LIBRARY" OR _lec_type STREQUAL "OBJECT_LIBRARY"
     OR _lec_type STREQUAL "SHARED_LIBRARY")
    # An instrumented library's objects reference the gcov runtime
    # (__gcov_init / __gcov_merge_add ...). Propagate --coverage to LINK as
    # PUBLIC so every consumer (test executables, the apps) pulls in the gcov
    # runtime and links cleanly, even if the consumer itself is not routed
    # through luminumbra_configure_first_party_target.
    target_link_options(${TARGET_NAME} PUBLIC --coverage)
  else()
    target_link_options(${TARGET_NAME} PRIVATE --coverage)
  endif()
endfunction()

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

  luminumbra_enable_coverage(${TARGET_NAME})
endfunction()

# T-I4-11 determinism contract (FP flag pinning). The simulation-authority
# targets (luminumbra_common, luminumbra_server_app) must produce bit-identical
# floating point across optimization levels and machines so the lockstep
# world_hash oracle (T-I4-13) can rely on it. The hazard here is FMA
# CONTRACTION: Jolt exports -mavx2 -mfma -mfpmath=sse as PUBLIC compile options
# (vendor/Jolt/Jolt.cmake:677/681), so those flags flow transitively onto every
# sim translation unit; with -mfma available, GCC's default -ffp-contract=fast
# can fuse a*b+c into a single rounding (FMA) at higher optimization, diverging
# from the unfused debug result. Pinning -ffp-contract=off forbids that fusion
# everywhere. No -ffast-math reaches these targets (it is confined to vendor
# FastNoise); this function asserts that invariant rather than assuming it.
# Generalizes the per-file AnimationRuntime.cpp pin (T-I3-15) to whole targets.
# MSVC's analogue is /fp:precise (default) + /fp:contract- but the build is
# GCC/MinGW today; the MSVC arm is added defensively for a future client lane.
function(luminumbra_pin_fp_determinism TARGET_NAME)
  if(NOT TARGET ${TARGET_NAME})
    return()
  endif()
  get_target_property(_lpfd_type ${TARGET_NAME} TYPE)
  if(_lpfd_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()
  if(MSVC)
    target_compile_options(${TARGET_NAME} PRIVATE /fp:precise)
    # /fp:contract- requires MSVC >= 19.30 (VS2022 17.0); the cmake generator
    # expression keeps older toolchains compiling (they default to no
    # contraction anyway).
    target_compile_options(${TARGET_NAME} PRIVATE
      $<$<CXX_COMPILER_VERSION:GREATER_EQUAL:19.30>:/fp:contract->)
  else()
    target_compile_options(${TARGET_NAME} PRIVATE -ffp-contract=off)
  endif()
endfunction()

include(cmake/engine_version.cmake)

# Fetch all third-party dependencies from a central file
# This makes our project self-contained and portable.
include(cmake/dependencies.cmake)
add_subdirectory(vendor)

# spec 021 GPU-P02: Diligent Engine (RHI pilot backend) via FetchContent. Only
# registers the GL/Vk targets -- they are compiled solely by the RhiDeviceBringupGpu
# ctest, never by a shipping binary, so this does not touch the server/sim build.
include(cmake/diligent.cmake)

# spec 021 (2026-07 research P1): Tracy Profiler seam. Defines the luminumbra_profiler
# INTERFACE target (empty + no fetch when LUMINUMBRA_ENABLE_TRACY=OFF, the default),
# so gate/release builds are byte-identical. Must precede the target subdirectories.
include(cmake/tracy.cmake)

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

# T-I4-11: pin FP determinism on the simulation-authority targets (see the
# luminumbra_pin_fp_determinism rationale above). Proven hash-neutral in the
# landing commit (HeadlessServerTick world_hash unchanged) because debug -O0
# never contracted; this guards the release lane and cross-machine builds.
luminumbra_pin_fp_determinism(luminumbra_common)
luminumbra_pin_fp_determinism(luminumbra_server_app)

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
  worldgen_layer_snapshot_test
  render_smoke_test
  render_capture_test
)
  luminumbra_configure_first_party_target(${FIRST_PARTY_TARGET})
endforeach()

# coverage-instrumentation slice: the client library (rendering / ui / audio
# source) was historically NOT routed through
# luminumbra_configure_first_party_target, so it never carried the first-party
# -Werror/-Wall/-Wextra set (it has pre-existing -Wswitch/-Wsign-compare
# findings out of scope here). To measure rendering/ui/audio coverage WITHOUT
# retroactively turning those warnings into hard errors, instrument it for
# coverage ONLY (no warning-as-error flags). This is a no-op unless the
# `coverage` preset set LUMINUMBRA_ENABLE_COVERAGE.
luminumbra_enable_coverage(luminumbra_client)
