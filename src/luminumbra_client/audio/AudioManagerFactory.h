#pragma once

#include "IAudioManager.h"
#include <memory>

namespace Luminumbra::Client {

// Factory function to create the default audio manager
std::unique_ptr<IAudioManager> CreateAudioManager(const std::string& root_path);

} // namespace Luminumbra::Client
