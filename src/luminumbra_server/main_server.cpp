// Headless server entry point (T-I3-12). Simulation authority only: links
// luminumbra_common and nothing client-side (no OpenGL/GLFW/miniaudio/imgui/
// RmlUi). See the ServerHeadlessHygiene ctest for the include boundary.
#include "luminumbra_common/core/Log.h"

int main(int /*argc*/, char** /*argv*/) {
    Log::Init();
    LUMINUMBRA_CORE_INFO("Luminumbra headless server");
    return 0;
}
