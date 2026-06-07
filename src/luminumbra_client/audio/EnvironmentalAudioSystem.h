#pragma once
#include "MiniaudioManager.h"
#include "glm/glm.hpp"
#include <unordered_map>
#include <memory>

namespace Luminumbra::Client {

class EnvironmentalAudioSystem {
public:
    EnvironmentalAudioSystem(MiniaudioManager* audioManager);
    ~EnvironmentalAudioSystem();

    void Update(const glm::vec3& listenerPosition, float deltaTime);
    
    // Environment detection and switching
    void SetEnvironment(AudioEnvironmentType type, const glm::vec3& position);
    void UpdateWeatherConditions(float windStrength, const glm::vec3& windDirection, bool isRaining = false);
    
    // Zone-based ambient audio
    void RegisterAmbientZone(const std::string& zoneId, const glm::vec3& center, float radius, const AudioEventID& ambientSound);
    void UnregisterAmbientZone(const std::string& zoneId);
    
    // Material-based footstep system
    void PlayFootstepSound(const glm::vec3& position, const std::string& materialType);
    
    // Real-time environmental audio adjustments
    void SetTimeOfDay(float timeNormalized); // 0.0 = midnight, 0.5 = noon
    void SetSeasonalEffects(float seasonFactor); // 0.0 = winter, 1.0 = summer
    
private:
    struct AmbientZone {
        std::string id;
        glm::vec3 center;
        float radius;
        AudioEventID soundEvent;
        bool isActive = false;
        float fadeDistance = 10.0f;
    };
    
    struct WeatherState {
        float windStrength = 0.0f;
        glm::vec3 windDirection = {1.0f, 0.0f, 0.0f};
        bool isRaining = false;
        float rainIntensity = 0.0f;
        float timeOfDay = 0.5f; // Noon by default
        float seasonalFactor = 0.5f; // Spring/Fall
    };
    
    void UpdateAmbientZones(const glm::vec3& listenerPosition);
    void UpdateWeatherAudio();
    void UpdateTimeBasedEffects();
    AudioEnvironment CreateEnvironmentProfile(AudioEnvironmentType type);
    
    MiniaudioManager* m_audioManager;
    WeatherState m_weatherState;
    AudioEnvironmentType m_currentEnvironmentType = AudioEnvironmentType::Outdoor;
    
    std::unordered_map<std::string, std::unique_ptr<AmbientZone>> m_ambientZones;
    std::unordered_map<std::string, AudioEventID> m_materialFootstepMap;
    
    // Cached sound events for quick access
    AudioEventID m_rainSoundEvent = "rain_ambient";
    AudioEventID m_thunderSoundEvent = "thunder_distant";
    
    float m_updateTimer = 0.0f;
    const float UPDATE_INTERVAL = 0.1f; // Update every 100ms
};

} // namespace Luminumbra::Client