# vendor/CMakeLists.txt
#
# Add local, third-party libraries that are included directly in the repository.

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
