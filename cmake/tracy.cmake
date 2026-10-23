# cmake/tracy.cmake
#
# Tracy Profiler provides a GPU-zone abstraction that
# spans OpenGL AND Vulkan/D3D12, so it survives the GL -> Diligent RHI migration that
# the bespoke GL-only GpuTimerPass cannot. Vendored via FetchContent (same rationale
# as cmake/diligent.cmake: sidesteps the vendor/ junction hazard on this box).
#
# DETERMINISM BY CONSTRUCTION: OFF by default. This file ALWAYS defines the INTERFACE
# target `luminumbra_profiler`; consumers link it unconditionally. When OFF it is an
# EMPTY interface (no define, no link, no fetch) -> the Profiler.h shim expands every
# macro to nothing -> the sim binaries and `world_hash` are byte-identical to a
# Tracy-free tree. Enable ONLY in a dedicated dev/profiling build
# (`-DLUMINUMBRA_ENABLE_TRACY=ON`), NEVER in a determinism-gate or release build.

option(LUMINUMBRA_ENABLE_TRACY
    "Vendor the Tracy Profiler and activate the Profiler.h zones (dev/profiling builds only -- OFF keeps gate/release byte-identical)" OFF)

# The seam every profiled target links. Exists in BOTH configurations so target
# CMakeLists never branch on the option.
add_library(luminumbra_profiler INTERFACE)

if(LUMINUMBRA_ENABLE_TRACY)
    include(FetchContent)

    # On-demand: the client buffers/streams only while a Tracy viewer is attached,
    # instead of from process start -- the right default for an occasional dev tool.
    set(TRACY_ENABLE     ON  CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND  ON  CACHE BOOL "" FORCE)

    # Tracy v0.13.1, pinned to its immutable upstream commit.
    set(LUMINUMBRA_TRACY_TAG "v0.13.1")
    set(LUMINUMBRA_TRACY_REVISION "05cceee0df3b8d7c6fa87e9638af311dbabc63cb")

    FetchContent_Declare(
        tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG        "${LUMINUMBRA_TRACY_REVISION}"
    )
    FetchContent_MakeAvailable(tracy)

    # TracyClient PUBLICly propagates TRACY_ENABLE + the tracy/ include dir; we add our
    # own gate define so the Profiler.h shim keys on the Luminumbra option name.
    target_link_libraries(luminumbra_profiler INTERFACE TracyClient)
    target_compile_definitions(luminumbra_profiler INTERFACE LUMINUMBRA_ENABLE_TRACY)

    message(STATUS "Tracy Profiler vendored via FetchContent (${LUMINUMBRA_TRACY_TAG}, on-demand) -- zones ACTIVE")
else()
    message(STATUS "Tracy Profiler disabled (LUMINUMBRA_ENABLE_TRACY=OFF) -- Profiler.h zones are no-ops")
endif()
