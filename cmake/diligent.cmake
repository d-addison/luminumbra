# cmake/diligent.cmake
#
# Vendor Diligent Engine through FetchContent. This deliberately differs from
# the local vendor/ convention because FetchContent avoids the worktree and
# worktree/junction hazard a vendored submodule tree hits on this box. GL + Vulkan
# device backends only. Diligent is used for device bring-up validation, not by
# links into the RhiDeviceBringupGpu ctest ONLY -- never a shipping binary -- so
# building luminumbra_server_app never compiles Diligent and --smoke stays
# byte-identical by construction.
#
# PREREQUISITES (Windows / ucrt64), documented like the Vulkan-SDK note in the
# gpu-modernization plan:
#   * A Vulkan SDK install (VULKAN_SDK) -- provides the Vulkan headers + loader
#     that Diligent's GraphicsEngineVk compiles/links against.
#   * git long-path support for the fresh clone: SPIRV-Cross ships reference/ test
#     files whose paths exceed Windows MAX_PATH. Set once:
#         git config --system core.longpaths true
#     (or export GIT_CONFIG_COUNT/KEY_0/VALUE_0 = core.longpaths=true for a scoped
#     configure). Without it the FetchContent clone fails with "Filename too long".
#
# TOOLCHAIN NOTE (ucrt64 GCC): Diligent only auto-detects mingw for the
# "MinGW Makefiles" generator (its root CMakeLists); under the Ninja generator we
# must force MINGW_BUILD=TRUE so Diligent (a) disables the ATL-only Win32 DXC that
# mingw genuinely cannot build, and (b) activates its mingw engine code paths.
# Stale third-party glslang predates the GCC-13+ tightening that stopped pulling
# <cstdint> transitively, so we force-include it -- but ONLY into the Diligent
# sub-build (scoped save/restore of CMAKE_CXX_FLAGS), leaving luminumbra's own
# targets on pristine flags so nothing about the sim build (or its hash) changes.

option(LUMINUMBRA_ENABLE_DILIGENT
    "Build the opt-in Diligent RHI pilot and its GPU tests" OFF)

if(LUMINUMBRA_ENABLE_DILIGENT)
    include(FetchContent)

    # Trim to the two backends the RHI pilot needs. D3D11/D3D12 pull MSVC-flavored
    # headers/d3dcompiler that are the least-traveled path under mingw.
    set(DILIGENT_NO_DIRECT3D11 ON  CACHE BOOL "" FORCE)
    set(DILIGENT_NO_DIRECT3D12 ON  CACHE BOOL "" FORCE)
    set(DILIGENT_NO_METAL      ON  CACHE BOOL "" FORCE)
    set(DILIGENT_NO_WEBGPU     ON  CACHE BOOL "" FORCE)
    set(DILIGENT_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
    set(DILIGENT_INSTALL_CORE  OFF CACHE BOOL "" FORCE)
    set(DILIGENT_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)

    if(WIN32 AND (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
        set(MINGW_BUILD TRUE CACHE INTERNAL "Building Diligent with mingw under the Ninja generator")
    endif()

    FetchContent_Declare(
        DiligentCore
        GIT_REPOSITORY https://github.com/DiligentGraphics/DiligentCore.git
        GIT_TAG        v2.5.6
        GIT_SHALLOW    TRUE
    )

    # Scope the transitive-include workaround to Diligent's targets only.
    set(_lumin_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include cstdint")
    endif()
    FetchContent_MakeAvailable(DiligentCore)
    set(CMAKE_CXX_FLAGS "${_lumin_saved_cxx_flags}")

    # The RHI pilot links ONLY the -static engine libs (test/CMakeLists.txt). Diligent
    # also defines -shared DLL targets that a bare `cmake --build` (the `all` target)
    # tries to build; the GL/Vk shared DLLs fail to link under Ninja+mingw because their
    # `-Wl,--version-script=export.map` link option (GraphicsEngineVulkan/CMakeLists.txt:293,
    # GraphicsEngineOpenGL/CMakeLists.txt:250) resolves `export.map` relative to the target
    # dir, which does not exist from the build root where Ninja runs the link. We never
    # consume the shared DLLs, so drop them from `all` to keep the documented
    # `cmake --build --preset debug` gate green. The -static targets are unaffected -- they
    # build on demand as ctest dependencies. The two Diligent test-framework libs
    # (GPU/TestFramework) build even under DILIGENT_BUILD_TESTS=OFF and list the shared
    # DLLs as order-only prerequisites, so they drag the DLLs back into `all` even after
    # the DLLs are excluded -- exclude them too (our ctests use their own gtest_main,
    # never Diligent's frameworks).
    foreach(_lumin_diligent_excl
            Diligent-GraphicsEngineOpenGL-shared
            Diligent-GraphicsEngineVk-shared
            Diligent-Archiver-shared
            Diligent-GPUTestFramework
            Diligent-TestFramework)
        if(TARGET ${_lumin_diligent_excl})
            set_target_properties(${_lumin_diligent_excl} PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endforeach()

    message(STATUS "Diligent Engine vendored via FetchContent (GL + Vulkan, ctest-only linkage)")
endif()
