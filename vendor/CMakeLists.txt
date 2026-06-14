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

# ---------------------------------------------------------------------------
# Third-party libraries restored via FetchContent (pinned tags), 2026-06-14.
# These were previously vendored on-disk under vendor/<lib>; that working-tree
# copy was lost (a worktree-cleanup deleted through junctions) and the sources
# were never tracked in git, so they are now declared as FetchContent
# dependencies — reproducible and impossible to lose again. Sources download to
# build/_deps at configure time. The BUILD_*/*_TESTS OFF flags above still
# apply. Pinned versions match the original vendoring vintage (imgui 1.92.1 /
# glfw 3.4 / lua 5.4.8 confirmed from surviving artifacts; glm 1.0.1 confirmed
# from the prior external/glm-1.0.1 reference). DETERMINISM ACCEPTANCE TEST: a
# clean rebuild must reproduce HeadlessServerTick world_hash 0eac465289e7c88b.
# Still vendored on-disk (unchanged): fastnoise, imgui, sol2, spdlog, googletest.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# CMake 4.0 removed compatibility with cmake_minimum_required(VERSION < 3.5).
# Several pinned third-party projects (e.g. glad 0.1.x) still declare an older
# minimum; this shim lets them configure under CMake 4.0 without editing their
# sources.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# glm: header-only interface target `glm`.
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL  OFF CACHE BOOL "" FORCE)

# Jolt (v5.x option names are TARGET_*; build only the library).
set(TARGET_UNIT_TESTS       OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD      OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES          OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER           OFF CACHE BOOL "" FORCE)

# glad (glad1 generator; emits <glad/glad.h>; GL 4.6 core; needs python).
set(GLAD_PROFILE    "core"   CACHE STRING "" FORCE)
set(GLAD_API        "gl=4.6" CACHE STRING "" FORCE)
set(GLAD_GENERATOR  "c"      CACHE STRING "" FORCE)
set(GLAD_EXTENSIONS ""       CACHE STRING "" FORCE)
set(GLAD_INSTALL    OFF      CACHE BOOL   "" FORCE)

FetchContent_Declare(EnTT          GIT_REPOSITORY https://github.com/skypjack/entt.git      GIT_TAG v3.15.0       GIT_SHALLOW TRUE)
FetchContent_Declare(glm           GIT_REPOSITORY https://github.com/g-truc/glm.git         GIT_TAG 1.0.1         GIT_SHALLOW TRUE)
FetchContent_Declare(glfw          GIT_REPOSITORY https://github.com/glfw/glfw.git          GIT_TAG 3.4           GIT_SHALLOW TRUE)
FetchContent_Declare(glad          GIT_REPOSITORY https://github.com/Dav1dde/glad.git       GIT_TAG v0.1.36       GIT_SHALLOW TRUE)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git      GIT_TAG v3.12.0       GIT_SHALLOW TRUE)
FetchContent_Declare(JoltPhysics   GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git GIT_TAG v5.3.0        GIT_SHALLOW TRUE SOURCE_SUBDIR Build)
FetchContent_Declare(RmlUi         GIT_REPOSITORY https://github.com/mikke89/RmlUi.git      GIT_TAG 6.1           GIT_SHALLOW TRUE)
FetchContent_Declare(soil2         GIT_REPOSITORY https://github.com/SpartanJ/SOIL2.git     GIT_TAG 1.3.0         GIT_SHALLOW TRUE)
FetchContent_Declare(miniaudio     GIT_REPOSITORY https://github.com/mackron/miniaudio.git  GIT_TAG 0.11.22       GIT_SHALLOW TRUE)
FetchContent_Declare(meshoptimizer GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git GIT_TAG v0.22         GIT_SHALLOW TRUE)
FetchContent_Declare(lua_upstream  GIT_REPOSITORY https://github.com/lua/lua.git            GIT_TAG v5.4.8        GIT_SHALLOW TRUE)
# sol2 + spdlog: the on-disk vendor copies were also partially clipped (sol2
# missing cmake/*.in; spdlog include/ hollow), so restore them via FetchContent.
FetchContent_Declare(sol2          GIT_REPOSITORY https://github.com/ThePhD/sol2.git        GIT_TAG v3.3.0        GIT_SHALLOW TRUE)
FetchContent_Declare(spdlog        GIT_REPOSITORY https://github.com/gabime/spdlog.git      GIT_TAG v1.15.1       GIT_SHALLOW TRUE)
# imgui: on-disk vendor/imgui core is intact (v1.92.1) but its backends/ was
# clipped. Fetch the matching tag (populate only; imgui ships no CMake) so we
# can restore the backend sources below.
FetchContent_Declare(imgui_src     GIT_REPOSITORY https://github.com/ocornut/imgui.git      GIT_TAG v1.92.1       GIT_SHALLOW TRUE)
# stb: header-only, single-header collection (no releases/tags upstream) — pin a
# commit. tools/asset_processor hardcodes vendor/stb; restored below.
FetchContent_Declare(stb           GIT_REPOSITORY https://github.com/nothings/stb.git       GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4)

FetchContent_MakeAvailable(EnTT glm glfw glad nlohmann_json JoltPhysics RmlUi soil2 miniaudio meshoptimizer lua_upstream sol2 spdlog imgui_src stb)

# Restore the clipped imgui backend sources from the pinned FetchContent copy
# into the on-disk vendor/imgui/backends that src/CMakeLists.txt references.
# Idempotent; re-copies if the working-tree copy is ever lost again.
file(COPY "${imgui_src_SOURCE_DIR}/backends/"
     DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends"
     FILES_MATCHING PATTERN "imgui_impl_glfw.*" PATTERN "imgui_impl_opengl3*")

# tools/asset_processor compiles vendor/meshoptimizer/src/*.cpp and includes
# vendor/stb directly; restore those on-disk paths from the pinned fetches.
file(COPY "${meshoptimizer_SOURCE_DIR}/src/"
     DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer/src")
file(COPY "${stb_SOURCE_DIR}/stb_image.h" "${stb_SOURCE_DIR}/stb_image_write.h"
          "${stb_SOURCE_DIR}/stb_image_resize2.h"
     DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/stb")
# SOIL2 ships no CMake (premake upstream), so build the `soil2` static target
# ourselves from its sources, and restore its headers into the on-disk
# vendor/soil2/src path that src/luminumbra_client hardcodes (-Ivendor/soil2/src,
# <SOIL2/SOIL2.h>).
file(COPY "${soil2_SOURCE_DIR}/src/" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/soil2/src")
if (NOT TARGET soil2)
  file(GLOB SOIL2_SRC "${soil2_SOURCE_DIR}/src/SOIL2/*.c")
  add_library(soil2 STATIC ${SOIL2_SRC})
  target_include_directories(soil2 PUBLIC "${soil2_SOURCE_DIR}/src")
  if (WIN32)
    target_link_libraries(soil2 PUBLIC opengl32)
  endif()
endif()

# Compatibility shims so the engine's existing link names resolve to the targets
# the upstream packages now export.
# EnTT exports `EnTT` / `EnTT::EnTT`; the engine links lowercase `entt`.
if (TARGET EnTT AND NOT TARGET entt)
  add_library(entt ALIAS EnTT)
endif()
# RmlUi 6.x exports `RmlUi::Core` / `RmlUi::Debugger`; the engine links `rmlui`.
if (NOT TARGET rmlui)
  add_library(rmlui INTERFACE)
  if (TARGET RmlUi::Core)
    target_link_libraries(rmlui INTERFACE RmlUi::Core)
  endif()
  if (TARGET RmlUi::Debugger)
    target_link_libraries(rmlui INTERFACE RmlUi::Debugger)
  endif()
endif()

# Jolt: ensure <Jolt/...> resolves from the fetched source root.
if (TARGET Jolt)
  target_include_directories(Jolt PUBLIC $<BUILD_INTERFACE:${joltphysics_SOURCE_DIR}>)
endif()

# lua: upstream ships no CMake — build a static `lua` target from its core .c
# (excluding the lua.c / luac.c standalone-interpreter mains).
file(GLOB LUA_CORE_SRC "${lua_upstream_SOURCE_DIR}/*.c")
list(FILTER LUA_CORE_SRC EXCLUDE REGEX "/(lua|luac)\\.c$")
add_library(lua STATIC ${LUA_CORE_SRC})
target_include_directories(lua PUBLIC "${lua_upstream_SOURCE_DIR}")

# FastNoise: still vendored on-disk (present) — unchanged.
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

# Still vendored on-disk (present) — unchanged.
add_subdirectory(imgui)  # Now ENABLED for Quantum UI!
add_subdirectory(googletest)
