#include "audio/IAudioManager.h"
#include "audio/MiniaudioManager.h" // Include concrete implementations
// #include "audio/FMODManager.h"

namespace Luminumbra::Client {

std::unique_ptr<IAudioManager> CreateAudioManager(const std::string& root_path) {
// In a real scenario, this would come from a config file or build definition
#if defined(USE_FMOD_AUDIO)
    // If you were using FMOD, you'd pass the path here too
    return std::make_unique<FMODManager>(root_path);
#else
    // Pass the received root_path to the manager's constructor
    return std::make_unique<MiniaudioManager>(root_path);
#endif
}

} // namespace Luminumbra::Client
