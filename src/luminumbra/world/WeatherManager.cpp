#include "luminumbra/world/WeatherManager.h"
#include "luminumbra/world/World.h"
#include "luminumbra/rendering/particles/Particle.h"
#include "luminumbra/audio/AudioManager.h"
#include <random>
#include <glm/gtc/constants.hpp>

namespace Luminumbra::World {

// Helper to get random numbers
static std::mt19937 s_RandomEngine(std::random_device{}());
static std::uniform_real_distribution<float> s_UnitDistribution(0.0f, 1.0f);

// Pre-defined properties for each weather type
static const std::unordered_map<WeatherType, WeatherProperties> s_WeatherPresets = {
    { WeatherType::Clear,       { glm::vec3(0.5f, 0.8f, 1.0f), 1.0f, 0.0f, 0.1f, false } },
    { WeatherType::Rain,        { glm::vec3(0.4f, 0.45f, 0.5f), 1.5f, 500.0f, 0.5f, false } },
    { WeatherType::Thunderstorm,{ glm::vec3(0.2f, 0.25f, 0.3f), 2.0f, 1000.0f, 1.0f, true } },
    { WeatherType::Snow,        { glm::vec3(0.6f, 0.7f, 0.8f), 1.8f, 300.0f, 0.3f, false } }
};

WeatherManager::WeatherManager(World* world) : m_World(world) {
    m_CurrentProperties = s_WeatherPresets.at(WeatherType::Clear);
    m_TargetProperties = s_WeatherPresets.at(WeatherType::Clear);
    m_ThunderTimer = 5.0f + s_UnitDistribution(s_RandomEngine) * 10.0f; // Initial thunder delay
}

void WeatherManager::setWeather(WeatherType type, float transitionDuration) {
    if (type == m_TargetType) return;

    m_TargetType = type;
    m_State = WeatherState::Transitioning;
    m_TransitionTimer = 0.0f;
    m_TransitionDuration = transitionDuration;

    // Set the properties we will be transitioning FROM and TO
    m_CurrentProperties = s_WeatherPresets.at(m_CurrentType);
    m_TargetProperties = s_WeatherPresets.at(m_TargetType);
}

void WeatherManager::update(float deltaTime, const glm::vec3& playerPosition) {
    if (m_State == WeatherState::Transitioning) {
        handleTransitionState(deltaTime);
    } else {
        handleStableState(deltaTime, playerPosition);
    }
    updateEffects(playerPosition);
}

void WeatherManager::handleTransitionState(float deltaTime) {
    m_TransitionTimer += deltaTime;
    float transitionProgress = glm::clamp(m_TransitionTimer / m_TransitionDuration, 0.0f, 1.0f);

    // If the target is 'Clear', we are fading out, so intensity goes from 1 to 0.
    // Otherwise, we are fading in, so intensity goes from 0 to 1.
    if (m_TargetType == WeatherType::Clear) {
        m_Intensity = 1.0f - transitionProgress;
    } else {
        m_Intensity = transitionProgress;
    }

    if (transitionProgress >= 1.0f) {
        m_State = WeatherState::Stable;
        m_CurrentType = m_TargetType;
        m_CurrentProperties = m_TargetProperties;
        m_Intensity = (m_CurrentType == WeatherType::Clear) ? 0.0f : 1.0f;
    }
}

void WeatherManager::handleStableState(float deltaTime, const glm::vec3& playerPosition) {
    // In a stable state, we might randomly decide to change the weather
    // (This logic can be expanded for a more complex weather simulation)
    
    if (m_CurrentType == WeatherType::Thunderstorm) {
        updateThunder(deltaTime, playerPosition);
    }
}

void WeatherManager::updateEffects(const glm::vec3& playerPosition) {
    auto* particleSystem = m_World->getParticleSystem();
    if (!particleSystem) return;

    WeatherType activeWeather = (m_TargetType == WeatherType::Clear) ? m_CurrentType : m_TargetType;

    // Emit particles based on interpolated precipitation rate
    float rate = glm::mix(m_CurrentProperties.precipitationRate, m_TargetProperties.precipitationRate, m_Intensity);
    if (rate > 0) {
        int particlesToSpawn = static_cast<int>(rate * (1.0f / 60.0f)); // Assuming 60fps update
        for (int i = 0; i < particlesToSpawn; ++i) {
            Rendering::ParticleType particleType = (activeWeather == WeatherType::Snow) ? Rendering::ParticleType::Snow : Rendering::ParticleType::Rain;
            Rendering::ParticleProps props = particleSystem->getPresetProperties(particleType);
            
            float offsetX = (s_UnitDistribution(s_RandomEngine) - 0.5f) * 80.0f;
            float offsetZ = (s_UnitDistribution(s_RandomEngine) - 0.5f) * 80.0f;
            props.position = playerPosition + glm::vec3(offsetX, 50.0f, offsetZ);
            
            particleSystem->emit(props);
        }
    }

    // Handle audio transitions
    // (This is a simplified example; a real system might have more layers and crossfades)
    // TODO: Implement proper audio handling based on intensity
}

void WeatherManager::updateThunder(float deltaTime, const glm::vec3& playerPosition) {
    if (!s_WeatherPresets.at(m_CurrentType).hasLightning) return;

    m_ThunderTimer -= deltaTime;
    if (m_ThunderTimer <= 0.0f) {
        // Reset timer for the next thunder clap
        m_ThunderTimer = 5.0f + s_UnitDistribution(s_RandomEngine) * 15.0f;

        float distance = 100.0f + s_UnitDistribution(s_RandomEngine) * 500.0f;
        float angle = s_UnitDistribution(s_RandomEngine) * 2.0f * glm::pi<float>();
        glm::vec3 soundPos = playerPosition + glm::vec3(cos(angle) * distance, 0, sin(angle) * distance);

        if (s_UnitDistribution(s_RandomEngine) > 0.4f) {
            Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::EnvThunderDistant, soundPos);
        } else {
            Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::EnvThunderClose, soundPos);
        }
    }
}


float WeatherManager::getWetness() const {
    WeatherType activeType = (m_State == WeatherState::Stable) ? m_CurrentType : m_TargetType;
    if (activeType == WeatherType::Rain || activeType == WeatherType::Thunderstorm) {
        return m_Intensity;
    }
    // If we are transitioning FROM rain TO clear, we need to dry off
    if (m_CurrentType == WeatherType::Rain || m_CurrentType == WeatherType::Thunderstorm) {
         return m_Intensity;
    }
    return 0.0f;
}

glm::vec3 WeatherManager::getFogColor() const {
    return glm::mix(m_CurrentProperties.fogColor, m_TargetProperties.fogColor, m_Intensity);
}

} // namespace Luminumbra::World