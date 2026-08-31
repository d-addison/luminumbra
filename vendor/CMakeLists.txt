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
set(LZ4_BUILD_CLI OFF)
set(LZ4_BUILD_LEGACY_LZ4C OFF)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)

# FetchContent dependencies use immutable revisions and populate build/_deps.
# FastNoise and the RenderDoc integration header remain committed under vendor/;
# all other dependencies are consumed directly from their fetched source trees.
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
# Match the runtime selected by first-party MSVC targets. Jolt defaults to the
# static runtime (/MT), while CMake targets default to the DLL runtime (/MD);
# mixing them produces LNK2038 failures in every executable that consumes the
# common engine library.
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)

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
FetchContent_Declare(freetype      GIT_REPOSITORY https://github.com/freetype/freetype.git  GIT_TAG 0a0221a1347e2f1e07c395263540026e9a0aa7c7)
FetchContent_Declare(RmlUi         GIT_REPOSITORY https://github.com/mikke89/RmlUi.git      GIT_TAG 6.1           GIT_SHALLOW TRUE)
FetchContent_Declare(soil2         GIT_REPOSITORY https://github.com/SpartanJ/SOIL2.git     GIT_TAG 1.3.0         GIT_SHALLOW TRUE)
FetchContent_Declare(miniaudio     GIT_REPOSITORY https://github.com/mackron/miniaudio.git  GIT_TAG 0.11.22       GIT_SHALLOW TRUE)
FetchContent_Declare(meshoptimizer GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git GIT_TAG v0.22         GIT_SHALLOW TRUE)
FetchContent_Declare(lua_upstream  GIT_REPOSITORY https://github.com/lua/lua.git            GIT_TAG v5.4.8        GIT_SHALLOW TRUE)
FetchContent_Declare(sol2          GIT_REPOSITORY https://github.com/ThePhD/sol2.git        GIT_TAG v3.3.0        GIT_SHALLOW TRUE)
FetchContent_Declare(spdlog        GIT_REPOSITORY https://github.com/gabime/spdlog.git      GIT_TAG v1.15.1       GIT_SHALLOW TRUE)
# Dear ImGui ships no CMake project, so its fetched source tree is built below.
FetchContent_Declare(imgui_src     GIT_REPOSITORY https://github.com/ocornut/imgui.git      GIT_TAG v1.92.1       GIT_SHALLOW TRUE)
# stb is a header-only collection without releases; pin an immutable commit.
FetchContent_Declare(stb           GIT_REPOSITORY https://github.com/nothings/stb.git       GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4)
FetchContent_Declare(googletest    GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG 373af2e3df71599b87a40ce0e37164523849166b)
FetchContent_Declare(lz4_upstream  GIT_REPOSITORY https://github.com/lz4/lz4.git GIT_TAG ebb370ca83af193212df4dcbadcc5d87bc0de2f0 SOURCE_SUBDIR build/cmake)

FetchContent_MakeAvailable(freetype)
if(TARGET freetype AND NOT TARGET Freetype::Freetype)
  add_library(Freetype::Freetype ALIAS freetype)
endif()

FetchContent_MakeAvailable(EnTT glm glfw glad nlohmann_json JoltPhysics RmlUi soil2 miniaudio meshoptimizer lua_upstream sol2 spdlog imgui_src stb googletest lz4_upstream)

# RmlUi 6.1's bundled robin_hood header uses SIZE_MAX and fixed-width integer
# types without including <cstdint>. Other standard-library implementations
# expose them transitively, but UCRT64 does not.
if(MINGW)
  foreach(_rmlui_target rmlui_core rmlui_debugger)
    if(TARGET ${_rmlui_target})
      target_compile_options(${_rmlui_target} PRIVATE -include cstdint)
    endif()
  endforeach()
endif()

if(TARGET lz4_static AND NOT TARGET lz4)
  add_library(lz4 ALIAS lz4_static)
endif()

# SOIL2 ships no CMake project, so build it directly from the fetched tree.
if (NOT TARGET soil2)
  file(GLOB SOIL2_SRC "${soil2_SOURCE_DIR}/src/SOIL2/*.c")
  add_library(soil2 STATIC ${SOIL2_SRC})
  target_include_directories(soil2 PUBLIC "${soil2_SOURCE_DIR}/src")
  if (WIN32)
    target_link_libraries(soil2 PUBLIC opengl32)
  endif()
endif()

# stb is header-only. Export its fetched include directory without copying
# generated dependency state back into the source checkout.
if(NOT TARGET stb_headers)
  add_library(stb_headers INTERFACE)
  target_include_directories(stb_headers SYSTEM INTERFACE "${stb_SOURCE_DIR}")
endif()

# Dear ImGui does not provide a CMake project. Build the pinned sources and GL
# backends in place so every consumer uses the same target and include paths.
if(NOT TARGET imgui)
  find_package(OpenGL REQUIRED)
  add_library(imgui STATIC
    "${imgui_src_SOURCE_DIR}/imgui.cpp"
    "${imgui_src_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_src_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_src_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_src_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_src_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_src_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
  target_include_directories(imgui SYSTEM PUBLIC
    "${imgui_src_SOURCE_DIR}"
    "${imgui_src_SOURCE_DIR}/backends")
  target_link_libraries(imgui PUBLIC glfw glad OpenGL::GL)
  target_compile_features(imgui PUBLIC cxx_std_20)
  add_library(imgui::imgui ALIAS imgui)
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
  if(MINGW)
    target_compile_options(rmlui INTERFACE -include cstdint)
  endif()
endif()

# Jolt: ensure <Jolt/...> resolves from the fetched source root.
if (TARGET Jolt)
  target_include_directories(Jolt PUBLIC $<BUILD_INTERFACE:${joltphysics_SOURCE_DIR}>)
  if(MSVC)
    # Visual Studio 18 emits C4865 from Windows SDK display enums included by
    # Jolt. Jolt enables /Wall /WX for its own target, so suppress this SDK-only
    # migration warning without changing first-party warning policy.
    target_compile_options(Jolt PRIVATE /wd4865)
  endif()
endif()

# lua: upstream ships no CMake — build a static `lua` target from its core .c
# (excluding the lua.c / luac.c standalone mains and the onelua.c amalgamation,
# which embeds the interpreter main unless built with a private make define).
file(GLOB LUA_CORE_SRC "${lua_upstream_SOURCE_DIR}/*.c")
list(FILTER LUA_CORE_SRC EXCLUDE REGEX "/(lua|luac|onelua)\\.c$")
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

# Terrain is persisted and network-authoritative, so the noise library must
# produce the same samples in debug and optimized builds. Keep its debug
# code-generation mode aligned with release while first-party code remains
# fully debuggable and sanitizer-instrumented.
if(TARGET FastNoise)
  target_compile_definitions(FastNoise PRIVATE $<$<CONFIG:Debug>:NDEBUG>)
  if(MSVC)
    target_compile_options(FastNoise PRIVATE $<$<CONFIG:Debug>:/O2>)
  else()
    target_compile_options(FastNoise PRIVATE $<$<CONFIG:Debug>:-O3>)
  endif()
endif()
