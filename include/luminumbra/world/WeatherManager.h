#pragma once

#include <glm/glm.hpp>
#include <string>

// Forward declarations
namespace Luminumbra::World { class World; }

namespace Luminumbra::World {

// Defines the types of weather available in the game.
enum class WeatherType {
    Clear,
    Rain,
    Thunderstorm,
    Snow
};

// Represents the properties of a specific weather type.
struct WeatherProperties {
    glm::vec3 fogColor;
    float fogDensity;
    float precipitationRate; // Particles per second
    float windStrength;
    bool hasLightning;
};

class WeatherManager {
public:
    WeatherManager(World* world);

    void update(float deltaTime, const glm::vec3& playerPosition);
    void setWeather(WeatherType type, float transitionDuration = 15.0f);

    float getIntensity() const { return m_Intensity; }
    float getWetness() const; // 0.0 for not raining, 1.0 for full rain
    glm::vec3 getFogColor() const;
    WeatherType getCurrentWeatherType() const { return m_CurrentType; }

private:
    // Represents the current state of the weather system.
    enum class WeatherState {
        Stable,
        Transitioning
    };

    void handleStableState(float deltaTime, const glm::vec3& playerPosition);
    void handleTransitionState(float deltaTime);
    void updateEffects(const glm::vec3& playerPosition);
    void updateThunder(float deltaTime, const glm::vec3& playerPosition);

    World* m_World; // Non-owning pointer to the world
    WeatherState m_State = WeatherState::Stable;

    WeatherType m_CurrentType = WeatherType::Clear;
    WeatherType m_TargetType = WeatherType::Clear;

    float m_TransitionTimer = 0.0f;
    float m_TransitionDuration = 0.0f;
    float m_Intensity = 0.0f; // 0.0 = clear, 1.0 = full weather effect

    WeatherProperties m_CurrentProperties;
    WeatherProperties m_TargetProperties;
    
    // Timers for intermittent effects
    float m_PrecipitationTimer = 0.0f;
    float m_ThunderTimer = 0.0f;
};

} // namespace Luminumbra::World