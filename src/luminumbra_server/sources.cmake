# Source manifest for the headless server executable.
# The server links luminumbra_common ONLY: no OpenGL, GLFW, miniaudio, imgui,
# or RmlUi anywhere under src/luminumbra_server/ (enforced by the
# ServerHeadlessHygiene ctest and the HeadlessServerTick validator mode).
set(SERVER_APP_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/main_server.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ServerWorldRunner.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/AetherBench.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/Gns.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/Heavy.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/Lockstep.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/ModeHelpers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/NetRuntime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/NetSoak.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/NetTcp.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/Replay.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/Replicate.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/Server.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/Smoke.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/Steam.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/WeatherBench.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/modes/WindBench.cpp"
)
