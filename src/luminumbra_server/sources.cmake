# Source manifest for the headless server executable (T-I3-12).
# The server links luminumbra_common ONLY: no OpenGL, GLFW, miniaudio, imgui,
# or RmlUi anywhere under src/luminumbra_server/ (enforced by the
# ServerHeadlessHygiene ctest and the HeadlessServerTick validator mode).
set(SERVER_APP_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/main_server.cpp"
)
