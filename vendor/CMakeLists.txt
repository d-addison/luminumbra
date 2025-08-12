# vendor/CMakeLists.txt
#
# Add local, third-party libraries that are included directly in the repository.

add_subdirectory(EnTT)
add_subdirectory(fastnoise)
add_subdirectory(glad)
add_subdirectory(glfw)
add_subdirectory(glm)
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