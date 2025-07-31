#pragma once
#include "audio/IAudioManager.h"
#include "vendor/miniaudio/miniaudio.h"
#include "vendor/nlohmann/json.hpp"
#include <unordered_map>
#include <vector>
#include <random>

namespace Luminumbra::Client {

// --- Forward Declarations and Internal Structs ---
struct AudioEventDefinition {
    std::vector<std::string> files;
    float volume = 1.0f;
    float pitch_variation = 0.0f;
    bool is_2d = false;
    bool is_looping = false;
    std::string strategy = "random";
};

class MiniaudioManager final : public IAudioManager {
public:
    MiniaudioManager();
    ~MiniaudioManager();

    // --- IAudioManager Interface ---
    bool Init() override;
    void Update() override;
    void Shutdown() override;

    bool LoadBank(const std::string& bankPath) override;
    void UnloadBank(const std::string& bankPath) override;

    void SetListenerTransform(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) override;

    bool PlayEvent(const AudioEventID& eventID, AudioEventHandle& outHandle) override;
    bool PlayOneShot(const AudioEventID& eventID, const glm::vec3& position) override;
    bool PlayOneShot2D(const AudioEventID& eventID) override;

    bool StopEvent(AudioEventHandle handle, bool immediate = true) override;
    bool SetEventPosition(AudioEventHandle handle, const glm::vec3& position) override;
    bool SetEventVolume(AudioEventHandle handle, float volume) override;
    bool SetEventParameter(AudioEventHandle handle, const AudioParamID& paramID, float value) override;

private:
    ma_result LoadSoundResource(const std::string& path, ma_sound* sound, uint32_t flags);
    const AudioEventDefinition* GetEventDefinition(const AudioEventID& eventID);

    std::unique_ptr<ma_engine> m_engine;
    std::unordered_map<AudioEventID, AudioEventDefinition> m_eventDefinitions;
    
    // For controllable, active sounds
    std::unordered_map<AudioEventHandle, std::unique_ptr<ma_sound>> m_activeSounds;
    AudioEventHandle m_nextHandle = 1;

    // For random selection in banks
    std::mt19937 m_rng;
};

} // namespace Luminumbra::Client