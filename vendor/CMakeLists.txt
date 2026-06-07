# vendor/CMakeLists.txt
#
# Add local, third-party libraries that are included directly in the repository.

# Keep vendored projects from registering their standalone test/example suites.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_TESTING OFF)
set(BUILD_TESTS OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_SAMPLES OFF)
set(BUILD_BENCHMARKS OFF)
set(BUILD_BENCH OFF)
set(BUILD_DOCS OFF)

set(ENTT_BUILD_TESTING OFF)
set(FASTNOISE_BUILD_TESTS OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_BUILD_DOCS OFF)
set(GLM_BUILD_TESTS OFF)
set(GLM_TEST_ENABLE OFF)
set(JPH_BUILD_TESTS OFF)
set(JPH_BUILD_UNIT_TESTS OFF)
set(JPH_BUILD_SAMPLES OFF)
set(MINIAUDIO_BUILD_TESTS OFF)
set(MINIAUDIO_BUILD_EXAMPLES OFF)
set(JSON_BuildTests OFF)
set(RMLUI_BUILD_TESTS OFF)
set(RMLUI_BUILD_SAMPLES OFF)
set(SOL2_BUILD_TESTS OFF)
set(SOL2_TESTS OFF)
set(SPDLOG_BUILD_TESTS OFF)
set(SPDLOG_BUILD_TESTS_HO OFF)
set(SPDLOG_BUILD_BENCH OFF)
set(gtest_build_tests OFF)
set(gtest_build_samples OFF)
set(gmock_build_tests OFF)
set(MESHOPT_BUILD_TESTS OFF)
set(MESHOPT_BUILD_DEMO OFF)
set(MESHOPT_BUILD_BENCHMARKS OFF)

add_subdirectory(EnTT)
if (NOT TARGET FastNoise)
  if (EXISTS "${CMAKE_SOURCE_DIR}/external/FastNoiseLite/FastNoiseLite.h")
    add_library(FastNoise INTERFACE)
    if (NOT TARGET FastNoiseLite)
      add_library(FastNoiseLite ALIAS FastNoise)
    endif()
    target_include_directories(FastNoise INTERFACE
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/FastNoiseLite>
    )
  else()
    add_subdirectory(fastnoise)
  endif()
endif()
add_subdirectory(glad)
add_subdirectory(glfw)
if (NOT TARGET glm)
  if (EXISTS "${CMAKE_SOURCE_DIR}/external/glm-1.0.1/CMakeLists.txt")
    set(GLM_BUILD_LIBRARY OFF CACHE BOOL "Build GLM as a compiled library" FORCE)
    set(GLM_BUILD_TESTS OFF CACHE BOOL "Build GLM tests" FORCE)
    set(GLM_BUILD_INSTALL OFF CACHE BOOL "Generate GLM install rules" FORCE)
    add_subdirectory(
      "${CMAKE_SOURCE_DIR}/external/glm-1.0.1"
      "${CMAKE_BINARY_DIR}/external/glm-1.0.1"
    )
  else()
    add_subdirectory(glm)
  endif()
endif()
add_subdirectory(imgui)  # Now ENABLED for Quantum UI!
add_subdirectory(Jolt)

# Ensure headers like <Jolt/...> resolve correctly when Jolt lives under vendor/Jolt.
# Use BUILD_INTERFACE so the target remains relocatable for install/export.
if (TARGET Jolt)
  target_include_directories(Jolt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  )
endif()

add_subdirectory(lua)
add_subdirectory(miniaudio)
add_subdirectory(nlohmann)
add_subdirectory(rmlui)
add_subdirectory(sol2)
add_subdirectory(spdlog)
add_subdirectory(soil2)
add_subdirectory(googletest)
add_subdirectory(meshoptimizer)
