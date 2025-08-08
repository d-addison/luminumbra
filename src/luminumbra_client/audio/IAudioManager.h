#pragma once
#include <string>
#include <cstdint>
#include "luminumbra_common/components/audio/AudioTypes.h"
#include "glm/glm.hpp"
#include <memory>

using Luminumbra::Common::AudioEventHandle;
using Luminumbra::Common::AudioEventID;
using Luminumbra::Common::AudioParamID;

namespace Luminumbra::Client {

class IAudioManager {
public:
    virtual ~IAudioManager() = default;

    virtual bool Init() = 0;
    virtual void Update() = 0;
    virtual void Shutdown() = 0;

    virtual bool LoadBank(const std::string& bankPath) = 0;
    virtual void UnloadBank(const std::string& bankPath) = 0;

    virtual void SetListenerTransform(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) = 0;

    virtual bool PlayEvent(const AudioEventID& eventID, AudioEventHandle& outHandle) = 0;
    virtual bool PlayOneShot(const AudioEventID& eventID, const glm::vec3& position) = 0;
    virtual bool PlayOneShot2D(const AudioEventID& eventID) = 0;

    // NEW
    virtual void PlayMusic(const AudioEventID& musicEventID) = 0;
    virtual void StopMusic() = 0;

    virtual bool StopEvent(AudioEventHandle handle, bool immediate = true) = 0;
    virtual bool SetEventPosition(AudioEventHandle handle, const glm::vec3& position) = 0;
    virtual bool SetEventVolume(AudioEventHandle handle, float volume) = 0;
    virtual bool SetEventParameter(AudioEventHandle handle, const AudioParamID& paramID, float value) = 0;
};

// Factory function to create the concrete instance based on config/build flags
std::unique_ptr<IAudioManager> CreateAudioManager();

} // namespace Luminumbra::Client
