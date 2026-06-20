# Include the list of source files for this library.
include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

# Create the static library with all its source files at once.
add_library(luminumbra_common STATIC ${COMMON_SOURCES})

# GameSession.cpp drives the whole fixed-tick system roster, instantiating ~16 entt component
# storages in one TU. Under GCC 15 at -O0 that tips the compiler into EXTERNALIZING the inline
# std helpers entt's paged storage needs (std::__advance / ranges::iter_move on the page-pointer
# vectors), producing undefined references at link time. Compiling just this file at -O1 emits
# the needed inline helpers out-of-line. Determinism is preserved: -ffp-contract=off stays on
# (no FMA) and GCC does NOT reorder IEEE float ops without -ffast-math, so results are bit-
# identical to -O0; verified by the NetworkStateHash run==replay gate.
#
# T-transcendental FR-003: the opt level of THIS one TU is asymmetric with the rest
# of the (-O0 debug) library, which is exactly how FMA/opt drift sneaks in. The
# Test-SimOptLevelParity gate builds a second tree with LUMINUMBRA_SIM_O0_PARITY=ON
# (overriding this file to -O2 -- a link-safe MORE-optimized level, since plain -O0
# reintroduces the vague-linkage link error above) and asserts the headless
# world_hash is identical, proving opt-level invariance.
option(LUMINUMBRA_SIM_O0_PARITY
       "Build GameSession.cpp at a sim-parity opt level for the O0/O1 world_hash invariance gate" OFF)
if(LUMINUMBRA_SIM_O0_PARITY)
    # -O2 (not -O0): -O0 reintroduces the entt vague-linkage undefined-reference link
    # error this override exists to dodge. -O2 is link-safe and a STRONGER opt level
    # than the normal -O1, so world_hash parity across -O1 vs -O2 is a stricter proof
    # of opt-level invariance than -O0 vs -O1 would have been.
    set_source_files_properties(world/GameSession.cpp PROPERTIES COMPILE_OPTIONS "-O2")
else()
    set_source_files_properties(world/GameSession.cpp PROPERTIES COMPILE_OPTIONS "-O1")
endif()

target_include_directories(luminumbra_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/vendor/spdlog/include"
    "${CMAKE_SOURCE_DIR}/vendor/soil2/src"
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

# T-I4-13: the TcpTransport (net/LockstepSession.cpp) uses winsock2 under _WIN32; link
# ws2_32 where (and only where) the platform needs it. The gates/tests drive
# LoopbackTransport (no sockets), so this link is for the real loopback/LAN session path.
if(WIN32)
    target_link_libraries(luminumbra_common PUBLIC ws2_32)
endif()

# T-I6: Steamworks SDK transport (SteamNetworkingTransport -> ISteamNetworkingSockets:
# real UDP + reliability + encryption + SDR). OFF by default; the SDK is owner-provided
# and NOT redistributable (vendor/steamworks/ is gitignored). When ON, link steam_api64
# + expose the headers + define LUMINUMBRA_ENABLE_STEAM so the transport body compiles.
option(LUMINUMBRA_ENABLE_STEAM "Build the Steamworks ISteamNetworkingSockets transport" OFF)
set(LUMINUMBRA_STEAM_SDK_DIR "${CMAKE_SOURCE_DIR}/vendor/steamworks/sdk" CACHE PATH "Steamworks SDK root")
if(LUMINUMBRA_ENABLE_STEAM)
    if(NOT EXISTS "${LUMINUMBRA_STEAM_SDK_DIR}/public/steam/steam_api.h")
        message(FATAL_ERROR "LUMINUMBRA_ENABLE_STEAM=ON but Steamworks SDK not found at ${LUMINUMBRA_STEAM_SDK_DIR} (see .forge/artifacts/engine-iteration-6/STEAMWORKS-SETUP.md)")
    endif()
    target_include_directories(luminumbra_common PUBLIC "${LUMINUMBRA_STEAM_SDK_DIR}/public")
    target_compile_definitions(luminumbra_common PUBLIC LUMINUMBRA_ENABLE_STEAM)
    if(WIN32)
        target_link_libraries(luminumbra_common PUBLIC
            "${LUMINUMBRA_STEAM_SDK_DIR}/redistributable_bin/win64/steam_api64.lib")
    endif()
    message(STATUS "Steamworks transport ENABLED (SDK: ${LUMINUMBRA_STEAM_SDK_DIR})")
endif()

# T-I6: standalone GameNetworkingSockets transport (GnsTransport). Valve's
# open-source UDP networking, Steam-INDEPENDENT -- so two processes can connect on
# ONE machine (the Steam SDK can't, one-instance-per-app), which is exactly what
# local dev/testing needs. OFF by default (FetchContent + a multi-minute GNS build);
# when ON, fetch GNS master (BCrypt crypto -> no OpenSSL/libsodium dep), link the
# static lib, and define LUMINUMBRA_ENABLE_GNS so the transport body compiles.
# Requires protobuf + abseil + protoc on PATH (ucrt64 packages).
option(LUMINUMBRA_ENABLE_GNS "Build the standalone GameNetworkingSockets UDP transport" OFF)
if(LUMINUMBRA_ENABLE_GNS)
    include(FetchContent)
    set(GAMENETWORKINGSOCKETS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GAMENETWORKINGSOCKETS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(USE_CRYPTO "BCrypt" CACHE STRING "" FORCE)
    FetchContent_Declare(
        GameNetworkingSockets
        GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(GameNetworkingSockets)
    target_link_libraries(luminumbra_common PUBLIC GameNetworkingSockets_s)
    target_compile_definitions(luminumbra_common PUBLIC LUMINUMBRA_ENABLE_GNS)
    message(STATUS "Standalone GameNetworkingSockets transport ENABLED (master, BCrypt)")
endif()
