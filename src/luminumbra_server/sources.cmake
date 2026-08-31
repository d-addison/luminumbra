# Source manifest for the headless server executable.
# The server links luminumbra_common ONLY: no OpenGL, GLFW, miniaudio, imgui,
# or RmlUi anywhere under src/luminumbra_server/ (enforced by the
# ServerHeadlessHygiene ctest and the HeadlessServerTick validator mode).
set(SERVER_APP_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/main_server.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ServerWorldRunner.cpp"
)
