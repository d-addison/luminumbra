#include "audio/IAudioManager.h"
#include "audio/MiniaudioManager.h" // Include concrete implementations
// #include "audio/FMODManager.h"

namespace Luminumbra::Client {

std::unique_ptr<IAudioManager> CreateAudioManager() {
    // In a real scenario, this would come from a config file or build definition
    #if defined(USE_FMOD_AUDIO)
        return std::make_unique<FMODManager>();
    #else
        return std::make_unique<MiniaudioManager>();
    #endif
}

} // namespace Luminumbra::Client