# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(Luminumbra VERSION 0.3.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependency revisions are immutable in the FetchContent declarations. Once a
# source has been populated, configuring must be reproducible and work offline
# instead of contacting every upstream repository for an update probe.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Do not update already-populated FetchContent dependencies" FORCE)

# Set build output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(LUMINUMBRA_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" OFF)
option(LUMINUMBRA_ENABLE_ASAN "Enable AddressSanitizer for first-party targets in Debug builds" OFF)
option(LUMINUMBRA_ENABLE_COVERAGE "Instrument first-party targets with gcov coverage counters (--coverage)" OFF)
option(LUMINUMBRA_ENABLE_FRAME_POINTERS "Preserve frame pointers in first-party targets" OFF)
option(ENABLE_BINARY_RELEASES "Enable Windows/Linux runtime package generation" OFF)

include(cmake/performance_manifest.cmake)

function(luminumbra_enable_frame_pointers TARGET_NAME)
  if(NOT LUMINUMBRA_ENABLE_FRAME_POINTERS OR NOT TARGET ${TARGET_NAME})
    return()
  endif()
  get_target_property(_lefp_type ${TARGET_NAME} TYPE)
  if(_lefp_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()
  if(MSVC)
    target_compile_options(${TARGET_NAME} PRIVATE /Oy-)
  else()
    target_compile_options(${TARGET_NAME} PRIVATE -fno-omit-frame-pointer)
  endif()
endfunction()

# Coverage instrumentation (coverage-instrumentation slice). Applied ONLY when
# LUMINUMBRA_ENABLE_COVERAGE is set, which is owned exclusively by the dedicated
# `coverage` preset (binaryDir build/coverage). It appends GCC/Clang's
# --coverage (== -fprofile-arcs -ftest-coverage) to compile + link options of
# first-party targets so gcov emits.gcno/.gcda counters. It is a no-op on MSVC
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
    # (__gcov_init / __gcov_merge_add...). Propagate --coverage to LINK as
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

  luminumbra_enable_frame_pointers(${TARGET_NAME})

  if(LUMINUMBRA_ENABLE_ASAN)
    get_target_property(_lea_type ${TARGET_NAME} TYPE)
    if(_lea_type STREQUAL "STATIC_LIBRARY" OR
       _lea_type STREQUAL "SHARED_LIBRARY")
      set(_lea_link_scope PUBLIC)
    else()
      set(_lea_link_scope PRIVATE)
    endif()
    if(MSVC)
      target_compile_options(${TARGET_NAME} PRIVATE $<$<CONFIG:Debug>:/fsanitize=address>)
      target_link_options(${TARGET_NAME} ${_lea_link_scope} $<$<CONFIG:Debug>:/INCREMENTAL:NO>)
    else()
      target_compile_options(${TARGET_NAME} PRIVATE $<$<CONFIG:Debug>:-fsanitize=address>)
      target_link_options(${TARGET_NAME} ${_lea_link_scope} $<$<CONFIG:Debug>:-fsanitize=address>)
    endif()
  endif()

  luminumbra_enable_coverage(${TARGET_NAME})
endfunction()

#  determinism contract (FP flag pinning). The simulation-authority
# targets (luminumbra_common, luminumbra_server_app) must produce bit-identical
# floating point across optimization levels and machines so the lockstep
# world_hash oracle can rely on it. The hazard here is FMA
# CONTRACTION: Jolt exports -mavx2 -mfma -mfpmath=sse as PUBLIC compile options
# (vendor/Jolt/Jolt.cmake:677/681), so those flags flow transitively onto every
# sim translation unit; with -mfma available, GCC's default -ffp-contract=fast
# can fuse a*b+c into a single rounding (FMA) at higher optimization, diverging
# from the unfused debug result. Pinning -ffp-contract=off forbids that fusion
# everywhere. No -ffast-math reaches these targets (it is confined to vendor
# FastNoise); this function asserts that invariant rather than assuming it.
# Apply the same floating-point contract to every engine target. MSVC's
# /fp:precise contract keeps expression evaluation reproducible without using
# non-portable clang-cl switches.
function(luminumbra_pin_fp_determinism TARGET_NAME)
  if(NOT TARGET ${TARGET_NAME})
    return()
  endif()
  get_target_property(_lpfd_type ${TARGET_NAME} TYPE)
  if(_lpfd_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()
  # Static/shared libraries expose simulation headers with inline arithmetic.
  # Consumers must compile those helpers under the same floating-point contract;
  # a PRIVATE flag would leave tests and applications free to fuse operations.
  if(_lpfd_type STREQUAL "STATIC_LIBRARY" OR
     _lpfd_type STREQUAL "SHARED_LIBRARY")
    set(_lpfd_scope PUBLIC)
  else()
    set(_lpfd_scope PRIVATE)
  endif()
  if(MSVC)
    target_compile_options(${TARGET_NAME} ${_lpfd_scope} /fp:precise)
  else()
    target_compile_options(${TARGET_NAME} ${_lpfd_scope} -ffp-contract=off)
  endif()
endfunction()

include(cmake/engine_version.cmake)

# Fetch all third-party dependencies from a central file
# This makes our project self-contained and portable.
include(cmake/dependencies.cmake)
add_subdirectory(vendor)

# The experimental Diligent backend is opt-in and excluded from normal builds.
include(cmake/diligent.cmake)

# Tracy Profiler seam. Defines the luminumbra_profiler
# INTERFACE target (empty + no fetch when LUMINUMBRA_ENABLE_TRACY=OFF, the default),
# so gate/release builds are byte-identical. Must precede the target subdirectories.
include(cmake/tracy.cmake)

# 1. Add the tools subdirectory so the asset_processor target is created.
add_subdirectory(tools)

# 2. Copy authored runtime data into the build tree as part of the build.
set(DATA_SOURCE_DIR ${CMAKE_SOURCE_DIR}/data)
set(DATA_OUTPUT_DIR ${CMAKE_BINARY_DIR}/data)

# The broad CONFIGURE_DEPENDS glob used to traverse generated captures and processed
# model outputs on every Ninja invocation. Keep an explicit authored-input manifest
# instead; copy_directory still mirrors the complete runtime tree when an authored
# input changes, while untracked/generated trees no longer tax every no-op build.
include(${CMAKE_SOURCE_DIR}/cmake/runtime_data_manifest.cmake)
set(DATA_SOURCE_FILES)
foreach(DATA_RELATIVE_PATH IN LISTS LUMINUMBRA_RUNTIME_DATA_MANIFEST)
    set(DATA_ABSOLUTE_PATH "${DATA_SOURCE_DIR}/${DATA_RELATIVE_PATH}")
    if(NOT EXISTS "${DATA_ABSOLUTE_PATH}")
        message(FATAL_ERROR
            "Runtime-data manifest entry is missing: data/${DATA_RELATIVE_PATH}. "
            "Run tools/check_runtime_data_manifest.py after data-file changes.")
    endif()
    list(APPEND DATA_SOURCE_FILES "${DATA_ABSOLUTE_PATH}")
endforeach()
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

# 3. Find source mesh assets at configure time. New source assets require an explicit
# CMake reconfigure; avoiding CONFIGURE_DEPENDS prevents a recursive assets/ scan on
# every no-op build while still allowing untracked work-in-progress.glb inputs.
set(ASSET_SOURCE_DIR ${CMAKE_SOURCE_DIR}/assets)
set(ASSET_OUTPUT_DIR ${DATA_OUTPUT_DIR})

file(GLOB_RECURSE SOURCE_ASSETS "${ASSET_SOURCE_DIR}/*.glb")

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

    # This is the core of the automation. It defines how to create the.lmesh from the.glb.
    add_custom_command(
        OUTPUT "${DESTINATION_ASSET}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DESTINATION_DIR}"
        # The command to run: path/to/asset_processor.exe <input> <output>
        COMMAND $<TARGET_FILE:asset_processor> "${SOURCE_ASSET}" "${DESTINATION_ASSET}"
        # This command depends on the source file and the processor itself.
        # It will re-run if the.glb changes, data is recopied, or asset_processor is recompiled.
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

foreach(CLIENT_EXE_TARGET IN ITEMS luminumbra_client_app luminumbra_client_qa_app)
  target_compile_definitions(${CLIENT_EXE_TARGET} PRIVATE
      LUMINUMBRA_BUILD_DATA_DIR="${DATA_OUTPUT_DIR}"
      LUMINUMBRA_SOURCE_DATA_DIR="${DATA_SOURCE_DIR}"
  )
endforeach()

target_include_directories(luminumbra_common PUBLIC
    "${LUMINUMBRA_ENGINE_VERSION_INCLUDE_DIR}"
)

# pin FP determinism on the simulation-authority targets (see the
# luminumbra_pin_fp_determinism rationale above). Proven hash-neutral in the
# landing commit (HeadlessServerTick world_hash unchanged) because debug -O0
# never contracted; this guards the release lane and cross-machine builds.
luminumbra_pin_fp_determinism(luminumbra_common)
luminumbra_pin_fp_determinism(luminumbra_server_app)

# --- FINAL DEPENDENCY LINK ---
# Make the main game executable depend on the asset processing target.
# This ensures all assets are processed *before* the game is linked.
add_dependencies(luminumbra_client_app process_assets)
add_dependencies(luminumbra_client_qa_app process_assets)

# Tests are enabled by default for development presets and can be omitted from
# packaging-only builds with -DBUILD_TESTING=OFF.
if(BUILD_TESTING)
  add_subdirectory(test)
endif()

foreach(FIRST_PARTY_TARGET IN ITEMS
  asset_processor
  luminumbra_common
  luminumbra_client_app
  luminumbra_client_qa_app
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
luminumbra_enable_frame_pointers(luminumbra_client)
# The QA harness library carries the RuntimeScenarioHarness TU that used to
# compile inside luminumbra_client, so it keeps the same coverage-only
# treatment (same pre-existing-warnings rationale as above).
luminumbra_enable_coverage(luminumbra_client_qa)
luminumbra_enable_frame_pointers(luminumbra_client_qa)
luminumbra_enable_frame_pointers(worldgen_preview_test)
foreach(PROFILING_TARGET IN ITEMS
  forest_perf_budget_test
  initial_world_loading_perf_test
  world_load_bounded_test
  ecology_tick_perf_test
  shieldrt_spike_bench
)
  luminumbra_enable_frame_pointers(${PROFILING_TARGET})
endforeach()

# Binary packaging is implemented for release dry runs but is dormant by
# default. Source releases are produced directly from Git and never traverse
# this install graph.
if(ENABLE_BINARY_RELEASES)
  install(TARGETS luminumbra_client_app luminumbra_server_app
          RUNTIME DESTINATION bin)
  install(DIRECTORY data res worlds
          DESTINATION share/luminumbra
          PATTERN "audio" EXCLUDE
          PATTERN "captures" EXCLUDE)
  install(FILES README.md LICENSE THIRD_PARTY_LICENSES.md SECURITY.md
          DESTINATION share/doc/luminumbra)
  set(CPACK_PACKAGE_NAME "luminumbra")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_FILE_NAME "luminumbra-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
  if(WIN32)
    set(CPACK_GENERATOR "ZIP")
  else()
    set(CPACK_GENERATOR "TGZ")
  endif()
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
  include(CPack)
else()
  # Some fetched dependencies include CPack in the parent configure scope.
  # Their generated package files must not make a source-only Luminumbra build
  # appear to have binary release packaging enabled.
  file(REMOVE
       "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
       "${CMAKE_BINARY_DIR}/CPackSourceConfig.cmake"
       "${CMAKE_BINARY_DIR}/CPackProperties.cmake")
endif()
