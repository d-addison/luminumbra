#include "../luminumbra_common/core/Log.h"

int main(int argc, char* argv[]) {
    Log::Init();
    LUMINUMBRA_CORE_INFO("Luminumbra Server");
    // In the future, this will start the host manager and listen for connections.
    return 0;
}
