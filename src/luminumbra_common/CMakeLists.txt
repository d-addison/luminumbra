# Include the list of source files for this library.
include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

# Create the static library with all its source files at once.
add_library(luminumbra_common STATIC ${COMMON_SOURCES})

# config-schema drift gate (compile/CI). SystemConfig.h/.cpp GENERATE the
# SysKey/SysParam enums + kKeys/kParams registries by expanding the X-macro tables in
# core/SystemConfigRegistry.gen.h, whose single authored home is core/ConfigSchema.json. If the
# committed generated header is stale vs the schema (someone edited the schema but forgot to
# re-emit), the compiled registry would silently drift. `config_codegen.py --check` proves the
# header is byte-fresh; run it at configure time and HARD-FAIL on drift. ConfigSchema.json and the
# generated header are registered as configure dependencies so editing either re-triggers the
# check. Skipped (with a warning, never a hard fail) when no Python interpreter is available, so
# the engine still builds on a Python-less box; the engine-frontier gate also enforces --check.
set(_config_schema "${CMAKE_CURRENT_SOURCE_DIR}/core/ConfigSchema.json")
set(_config_gen_header "${CMAKE_CURRENT_SOURCE_DIR}/core/SystemConfigRegistry.gen.h")
set(_config_codegen "${CMAKE_SOURCE_DIR}/tools/config_codegen.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_config_schema}" "${_config_gen_header}" "${_config_codegen}")
find_package(Python3 COMPONENTS Interpreter QUIET)
set(_config_python "")
if(Python3_FOUND)
    set(_config_python "${Python3_EXECUTABLE}")
else()
    foreach(_pyname python python3 py)
        find_program(_config_py_${_pyname} ${_pyname})
        if(_config_py_${_pyname})
            set(_config_python "${_config_py_${_pyname}}")
            break()
        endif()
    endforeach()
endif()
if(_config_python)
    execute_process(
        COMMAND "${_config_python}" "${_config_codegen}" --check
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _config_check_rc
        OUTPUT_VARIABLE _config_check_out
        ERROR_VARIABLE _config_check_err)
    if(NOT _config_check_rc EQUAL 0)
        message(FATAL_ERROR
            "SystemConfig schema drift (config_codegen.py --check failed):\n"
            "${_config_check_out}${_config_check_err}\n"
            "Edit core/ConfigSchema.json then regenerate: "
            "python tools/config_codegen.py --emit src/luminumbra_common/core/SystemConfigRegistry.gen.h")
    endif()
    message(STATUS "SystemConfig schema drift check passed (config_codegen.py --check) via ${_config_python}")
else()
    message(WARNING "No Python interpreter found: SystemConfig schema drift check (config_codegen.py --check) NOT run at configure time; the engine-frontier gate still enforces it.")
endif()

# GameSession.cpp drives the whole fixed-tick system roster, instantiating ~16 entt component
# storages in one TU. Under GCC 15 at -O0 that tips the compiler into EXTERNALIZING the inline
# std helpers entt's paged storage needs (std::__advance / ranges::iter_move on the page-pointer
# vectors), producing undefined references at link time. Compiling just this file at -O1 emits
# the needed inline helpers out-of-line. Determinism is preserved: -ffp-contract=off stays on
# (no FMA) and GCC does NOT reorder IEEE float ops without -ffast-math, so results are bit-
# identical to -O0; verified by the NetworkStateHash run==replay gate.
#
# T-transcendental: the opt level of THIS one TU is asymmetric with the rest
# of the (-O0 debug) library, which is exactly how FMA/opt drift sneaks in. The
# Test-SimOptLevelParity gate builds a second tree with LUMINUMBRA_SIM_O0_PARITY=ON
# (overriding this file to - -- a link-safe MORE-optimized level, since plain -O0
# reintroduces the vague-linkage link error above) and asserts the headless
# world_hash is identical, proving opt-level invariance.
option(LUMINUMBRA_SIM_O0_PARITY
       "Build GameSession.cpp at a GCC sim-parity opt level for world-hash invariance" OFF)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(LUMINUMBRA_SIM_O0_PARITY)
        # - (not -O0): -O0 reintroduces the entt vague-linkage undefined-reference link
        # error this override exists to dodge. - is link-safe and a STRONGER opt level
        # than the normal -O1, so world_hash parity across -O1 vs - is a stricter proof
        # of opt-level invariance than -O0 vs -O1 would have been.
        set_source_files_properties(world/GameSession.cpp PROPERTIES COMPILE_OPTIONS "-")
    else()
        set_source_files_properties(world/GameSession.cpp PROPERTIES COMPILE_OPTIONS "-O1")
    endif()
endif()

# LuaState.cpp is the one translation unit that includes sol2. GCC 15 diagnoses
# an unused, ill-formed optional-reference member in sol2 3.3.0 while parsing
# the template body. Confine that compiler-specific suppression to this file;
# Clang treats the option itself as an error under -Werror.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
    set_source_files_properties(scripting/LuaState.cpp PROPERTIES
        COMPILE_OPTIONS "-Wno-template-body")
endif()

target_include_directories(luminumbra_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/vendor"
)

target_link_libraries(luminumbra_common PUBLIC
    entt
    Jolt
    nlohmann_json
    sol2
    lua
    spdlog
    lz4
    glm
    FastNoise
)

# the TcpTransport (net/LockstepSession.cpp) uses winsock2 under _WIN32; link
# ws2_32 where (and only where) the platform needs it. The gates/tests drive
# LoopbackTransport (no sockets), so this link is for the real loopback/LAN session path.
if(WIN32)
    target_link_libraries(luminumbra_common PUBLIC ws2_32)
endif()

# Steamworks SDK transport (SteamNetworkingTransport -> ISteamNetworkingSockets:
# real UDP + reliability + encryption + SDR). OFF by default; the SDK is owner-provided
# and NOT redistributable (vendor/steamworks/ is gitignored). When ON, link steam_api64
# + expose the headers + define LUMINUMBRA_ENABLE_STEAM so the transport body compiles.
option(LUMINUMBRA_ENABLE_STEAM "Build the Steamworks ISteamNetworkingSockets transport" OFF)
set(LUMINUMBRA_STEAM_SDK_DIR "${CMAKE_SOURCE_DIR}/vendor/steamworks/sdk" CACHE PATH "Steamworks SDK root")
if(LUMINUMBRA_ENABLE_STEAM)
    if(NOT EXISTS "${LUMINUMBRA_STEAM_SDK_DIR}/public/steam/steam_api.h")
        message(FATAL_ERROR "LUMINUMBRA_ENABLE_STEAM=ON but Steamworks SDK headers were not found at ${LUMINUMBRA_STEAM_SDK_DIR}")
    endif()
    target_include_directories(luminumbra_common PUBLIC "${LUMINUMBRA_STEAM_SDK_DIR}/public")
    target_compile_definitions(luminumbra_common PUBLIC LUMINUMBRA_ENABLE_STEAM)
    if(WIN32)
        target_link_libraries(luminumbra_common PUBLIC
            "${LUMINUMBRA_STEAM_SDK_DIR}/redistributable_bin/win64/steam_api64.lib")
    elseif(UNIX AND NOT APPLE)
        # the linux64 redistributable is a shared object; the server loads it from
        # its own directory at runtime (mirror of the win64 steam_api64.dll copy).
        target_link_libraries(luminumbra_common PUBLIC
            "${LUMINUMBRA_STEAM_SDK_DIR}/redistributable_bin/linux64/libsteam_api.so")
    else()
        message(FATAL_ERROR "LUMINUMBRA_ENABLE_STEAM=ON is wired for Windows and Linux only")
    endif()
    message(STATUS "Steamworks transport ENABLED (SDK: ${LUMINUMBRA_STEAM_SDK_DIR})")
endif()

# standalone GameNetworkingSockets transport (GnsTransport). Valve's
# open-source UDP networking, Steam-INDEPENDENT -- so two processes can connect on
# ONE machine (the Steam SDK can't, one-instance-per-app), which is exactly what
# local dev/testing needs. OFF by default (FetchContent + a multi-minute GNS build);
# when ON, fetch a PINNED GNS release, link the static lib, and define
# LUMINUMBRA_ENABLE_GNS so the transport body compiles. Crypto backend: BCrypt on
# Windows (no OpenSSL/libsodium dep); GNS's default (OpenSSL) elsewhere.
# Requires protobuf + abseil + protoc on PATH (ucrt64 packages on Windows;
# libprotobuf-dev + libssl-dev on Linux).
option(LUMINUMBRA_ENABLE_GNS "Build the standalone GameNetworkingSockets UDP transport" OFF)
if(LUMINUMBRA_ENABLE_GNS)
    include(FetchContent)
    set(GAMENETWORKINGSOCKETS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GAMENETWORKINGSOCKETS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    if(WIN32)
        # BCrypt is a Windows OS API; forcing it elsewhere breaks the build.
        set(USE_CRYPTO "BCrypt" CACHE STRING "" FORCE)
    endif()
    FetchContent_Declare(
        GameNetworkingSockets
        GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
        # v1.6.0, pinned to the immutable commit (same discipline as the
        # SHA-pinned GitHub Actions): a moving `master` cannot silently change
        # what an enabled build links.
        GIT_TAG 2cb93a06350bb065db53abdb0d87cf297e0bfd34
    )
    FetchContent_MakeAvailable(GameNetworkingSockets)
    target_link_libraries(luminumbra_common PUBLIC GameNetworkingSockets_s)
    target_compile_definitions(luminumbra_common PUBLIC LUMINUMBRA_ENABLE_GNS)
    message(STATUS "Standalone GameNetworkingSockets transport ENABLED (v1.6.0 pinned)")
endif()
