# Include the list of source files for this library.
include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

# Create the static library with all its source files at once.
add_library(luminumbra_common STATIC ${COMMON_SOURCES})

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
