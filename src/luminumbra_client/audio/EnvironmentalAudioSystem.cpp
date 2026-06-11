#include "EnvironmentalAudioSystem.h"
#include "core/Log.h"
#include <algorithm>

namespace Luminumbra::Client {

EnvironmentalAudioSystem::EnvironmentalAudioSystem(MiniaudioManager* audioManager)
    : m_audioManager(audioManager) {
    
    // Initialize material-to-footstep sound mapping
    m_materialFootstepMap["grass"] = "footstep_grass";
    m_materialFootstepMap["stone"] = "footstep_stone";
    m_materialFootstepMap["dirt"] = "footstep_grass"; // Use grass sounds for dirt
    m_materialFootstepMap["sand"] = "footstep_sand";
    m_materialFootstepMap["metal"] = "footstep_metal";
    m_materialFootstepMap["wood"] = "footstep_wood";
    m_materialFootstepMap["water"] = "footstep_water_splash";
    
    LUMINUMBRA_CORE_INFO("Environmental Audio System Initialized");
}

EnvironmentalAudioSystem::~EnvironmentalAudioSystem() {
    // Clean up ambient zones
    for (auto& [id, zone] : m_ambientZones) {
        if (zone->isActive) {
            m_audioManager->StopAmbientLoop(zone->soundEvent);
        }
    }
}

void EnvironmentalAudioSystem::Update(const glm::vec3& listenerPosition, float deltaTime) {
    m_updateTimer += deltaTime;
    
    if (m_updateTimer >= UPDATE_INTERVAL) {
        UpdateAmbientZones(listenerPosition);
        UpdateWeatherAudio();
        UpdateTimeBasedEffects();
        m_updateTimer = 0.0f;
    }
}

void EnvironmentalAudioSystem::SetEnvironment(AudioEnvironmentType type, const glm::vec3& position) {
    if (type == m_currentEnvironmentType) return;
    
    m_currentEnvironmentType = type;
    AudioEnvironment environment = CreateEnvironmentProfile(type);
    
    m_audioManager->SetEnvironment(environment);
    
    // Start appropriate ambient sounds based on environment
    switch (type) {
        case AudioEnvironmentType::Cave:
            RegisterAmbientZone("cave_main", position, 50.0f, "cave_ambience");
            break;
        case AudioEnvironmentType::Forest:
            RegisterAmbientZone("forest_main", position, 80.0f, "forest_ambience");
            break;
        case AudioEnvironmentType::Water:
            RegisterAmbientZone("water_main", position, 30.0f, "water_flow");
            break;
        default:
            break;
    }
    
    LUMINUMBRA_CORE_INFO("Environment changed to: {}", static_cast<int>(type));
}

void EnvironmentalAudioSystem::UpdateWeatherConditions(float windStrength, const glm::vec3& windDirection, bool isRaining) {
    m_weatherState.windStrength = windStrength;
    m_weatherState.windDirection = windDirection;
    m_weatherState.isRaining = isRaining;
    
    // Update wind parameters in audio manager
    m_audioManager->SetWindParameters(windDirection, windStrength);
    
    LUMINUMBRA_CORE_INFO("Weather updated - Wind: {}, Rain: {}", windStrength, isRaining);
}

void EnvironmentalAudioSystem::RegisterAmbientZone(const std::string& zoneId, const glm::vec3& center, float radius, const AudioEventID& ambientSound) {
    auto zone = std::make_unique<AmbientZone>();
    zone->id = zoneId;
    zone->center = center;
    zone->radius = radius;
    zone->soundEvent = ambientSound;
    zone->fadeDistance = radius * 0.2f; // 20% of radius for fade
    
    m_ambientZones[zoneId] = std::move(zone);
    
    LUMINUMBRA_CORE_INFO("Registered ambient zone: {} at ({}, {}, {})", zoneId, center.x, center.y, center.z);
}

void EnvironmentalAudioSystem::UnregisterAmbientZone(const std::string& zoneId) {
    auto it = m_ambientZones.find(zoneId);
    if (it != m_ambientZones.end()) {
        if (it->second->isActive) {
            m_audioManager->StopAmbientLoop(it->second->soundEvent);
        }
        m_ambientZones.erase(it);
        LUMINUMBRA_CORE_INFO("Unregistered ambient zone: {}", zoneId);
    }
}

void EnvironmentalAudioSystem::PlayFootstepSound(const glm::vec3& position, const std::string& materialType) {
    auto it = m_materialFootstepMap.find(materialType);
    if (it != m_materialFootstepMap.end()) {
        m_audioManager->PlayOneShot(it->second, position);
    } else {
        // Default to grass footsteps
        m_audioManager->PlayOneShot("footstep_grass", position);
    }
}

void EnvironmentalAudioSystem::SetTimeOfDay(float timeNormalized) {
    m_weatherState.timeOfDay = std::clamp(timeNormalized, 0.0f, 1.0f);
}

void EnvironmentalAudioSystem::SetSeasonalEffects(float seasonFactor) {
    m_weatherState.seasonalFactor = std::clamp(seasonFactor, 0.0f, 1.0f);
}

void EnvironmentalAudioSystem::ApplyBiomeReverb(const std::string& preset, float wet, float dry, float decay) {
    // Idempotent: skip when the active profile already matches (avoids churning
    // the audio backend every Update tick while the listener stays in a biome).
    if (m_biomeReverb.applied &&
        m_biomeReverb.preset == preset &&
        m_biomeReverb.wet == wet &&
        m_biomeReverb.dry == dry &&
        m_biomeReverb.decay == decay) {
        return;
    }
    m_biomeReverb.applied = true;
    m_biomeReverb.preset = preset;
    m_biomeReverb.wet = wet;
    m_biomeReverb.dry = dry;
    m_biomeReverb.decay = decay;
    ++m_biomeReverb.apply_count;
    if (m_audioManager) {
        m_audioManager->SetGlobalReverb(wet, dry, decay);
    }
    LUMINUMBRA_CORE_INFO("Biome reverb applied: preset={} wet={} dry={} decay={}",
                         preset, wet, dry, decay);
}

void EnvironmentalAudioSystem::UpdateAmbientZones(const glm::vec3& listenerPosition) {
    for (auto& [id, zone] : m_ambientZones) {
        float distance = glm::distance(listenerPosition, zone->center);
        bool shouldBeActive = distance <= zone->radius;
        
        if (shouldBeActive && !zone->isActive) {
            // Start ambient sound
            m_audioManager->PlayAmbientLoop(zone->soundEvent, zone->center, zone->radius);
            zone->isActive = true;
        } else if (!shouldBeActive && zone->isActive) {
            // Stop ambient sound
            m_audioManager->StopAmbientLoop(zone->soundEvent);
            zone->isActive = false;
        }
        
        // Adjust volume based on distance for smooth fading
        if (zone->isActive) {
            float fadeFactor = 1.0f;
            if (distance > zone->radius - zone->fadeDistance) {
                fadeFactor = 1.0f - ((distance - (zone->radius - zone->fadeDistance)) / zone->fadeDistance);
                fadeFactor = std::clamp(fadeFactor, 0.0f, 1.0f);
            }
            // Volume adjustment would require extending the audio manager interface
        }
    }
}

void EnvironmentalAudioSystem::UpdateWeatherAudio() {
    // Handle wind intensity changes
    if (m_weatherState.windStrength > 0.1f) {
        AudioEventID windEvent;
        if (m_weatherState.windStrength < 0.3f) {
            windEvent = "wind_light";
        } else if (m_weatherState.windStrength < 0.7f) {
            windEvent = "wind_medium";
        } else {
            windEvent = "wind_strong";
        }
        
        // This would need a way to transition between wind sounds
        // For now, just ensure wind is playing
    }
    
    // Handle rain
    if (m_weatherState.isRaining) {
        // Start rain ambient if not already playing
        // m_audioManager->PlayAmbientLoop(m_rainSoundEvent, listener_pos, 100.0f);
    }
}

void EnvironmentalAudioSystem::UpdateTimeBasedEffects() {
    // Adjust ambient volumes based on time of day
    float nightFactor = 1.0f;
    
    // Dawn/dusk periods (0.2-0.3 and 0.7-0.8)
    if (m_weatherState.timeOfDay >= 0.2f && m_weatherState.timeOfDay <= 0.3f) {
        // Dawn - gradually increase day sounds
        nightFactor = 1.0f - (m_weatherState.timeOfDay - 0.2f) / 0.1f;
    } else if (m_weatherState.timeOfDay >= 0.7f && m_weatherState.timeOfDay <= 0.8f) {
        // Dusk - gradually increase night sounds
        nightFactor = (m_weatherState.timeOfDay - 0.7f) / 0.1f;
    } else if (m_weatherState.timeOfDay > 0.3f && m_weatherState.timeOfDay < 0.7f) {
        // Day time
        nightFactor = 0.0f;
    } else {
        // Night time
        nightFactor = 1.0f;
    }
    
    // Apply time-based audio filtering (would need audio manager extensions)
    // This could involve:
    // - Reducing high frequencies at night
    // - Adding subtle reverb changes
    // - Switching between day/night ambient loops
}

AudioEnvironment EnvironmentalAudioSystem::CreateEnvironmentProfile(AudioEnvironmentType type) {
    AudioEnvironment env;
    env.type = type;
    
    switch (type) {
        case AudioEnvironmentType::Cave:
            env.reverb_decay = 2.5f;
            env.reverb_wet = 0.6f;
            env.reverb_dry = 0.4f;
            env.echo_delay = 0.3f;
            env.echo_decay = 0.8f;
            env.ambient_volume = 1.2f;
            env.wind_strength = 0.0f;
            break;
            
        case AudioEnvironmentType::Forest:
            env.reverb_decay = 0.8f;
            env.reverb_wet = 0.2f;
            env.reverb_dry = 0.8f;
            env.echo_delay = 0.1f;
            env.echo_decay = 0.3f;
            env.ambient_volume = 1.0f;
            env.wind_strength = m_weatherState.windStrength * 0.8f;
            break;
            
        case AudioEnvironmentType::Water:
            env.reverb_decay = 1.0f;
            env.reverb_wet = 0.4f;
            env.reverb_dry = 0.6f;
            env.echo_delay = 0.15f;
            env.echo_decay = 0.5f;
            env.ambient_volume = 0.8f;
            env.wind_strength = m_weatherState.windStrength * 0.6f;
            break;
            
        case AudioEnvironmentType::Canyon:
            env.reverb_decay = 3.0f;
            env.reverb_wet = 0.7f;
            env.reverb_dry = 0.3f;
            env.echo_delay = 0.5f;
            env.echo_decay = 0.9f;
            env.ambient_volume = 1.1f;
            env.wind_strength = m_weatherState.windStrength * 1.2f;
            break;
            
        case AudioEnvironmentType::Underground:
            env.reverb_decay = 2.0f;
            env.reverb_wet = 0.5f;
            env.reverb_dry = 0.5f;
            env.echo_delay = 0.25f;
            env.echo_decay = 0.7f;
            env.ambient_volume = 0.9f;
            env.wind_strength = 0.0f;
            break;
            
        default: // Outdoor
            env.reverb_decay = 0.3f;
            env.reverb_wet = 0.1f;
            env.reverb_dry = 0.9f;
            env.echo_delay = 0.05f;
            env.echo_decay = 0.2f;
            env.ambient_volume = 1.0f;
            env.wind_strength = m_weatherState.windStrength;
            break;
    }
    
    env.wind_direction = m_weatherState.windDirection;
    
    return env;
}

} // namespace Luminumbra::Client