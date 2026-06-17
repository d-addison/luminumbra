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
