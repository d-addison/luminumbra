#include "EnvironmentalAudioSystem.h"
#include "EnvironmentalAudioModel.h"
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
        if (zone->isActive && m_audioManager) {
            m_audioManager->StopAmbientLoop(zone->soundEvent);
        }
    }
    // stop the day/night beds this system owns.
    if (m_audioManager) {
        if (m_dayNight.day_bed_started)
            m_audioManager->StopAmbientLoop(m_dayBedEvent);
        if (m_dayNight.night_bed_started)
            m_audioManager->StopAmbientLoop(m_nightBedEvent);
    }
}

void EnvironmentalAudioSystem::Update(const glm::vec3& listenerPosition, float deltaTime) {
    m_updateTimer += deltaTime;

    if (m_updateTimer >= UPDATE_INTERVAL) {
        UpdateAmbientZones(listenerPosition);
        UpdateWeatherAudio();
        UpdateDayNightBeds(listenerPosition, m_updateTimer); //
        m_updateTimer = 0.0f;
    }
}

void EnvironmentalAudioSystem::SetEnvironment(AudioEnvironmentType type,
                                              const glm::vec3& position) {
    if (type == m_currentEnvironmentType)
        return;

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

void EnvironmentalAudioSystem::UpdateWeatherConditions(float windStrength,
                                                       const glm::vec3& windDirection,
                                                       bool isRaining) {
    m_weatherState.windStrength = windStrength;
    m_weatherState.windDirection = windDirection;
    m_weatherState.isRaining = isRaining;

    // Update wind parameters in audio manager
    m_audioManager->SetWindParameters(windDirection, windStrength);

    LUMINUMBRA_CORE_INFO("Weather updated - Wind: {}, Rain: {}", windStrength, isRaining);
}

void EnvironmentalAudioSystem::RegisterAmbientZone(const std::string& zoneId,
                                                   const glm::vec3& center,
                                                   float radius,
                                                   const AudioEventID& ambientSound) {
    auto zone = std::make_unique<AmbientZone>();
    zone->id = zoneId;
    zone->center = center;
    zone->radius = radius;
    zone->soundEvent = ambientSound;
    zone->fadeDistance = radius * 0.2f; // 20% of radius for fade

    m_ambientZones[zoneId] = std::move(zone);

    LUMINUMBRA_CORE_INFO(
        "Registered ambient zone: {} at ({}, {}, {})", zoneId, center.x, center.y, center.z);
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

void EnvironmentalAudioSystem::PlayFootstepSound(const glm::vec3& position,
                                                 const std::string& materialType) {
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

void EnvironmentalAudioSystem::ApplyBiomeReverb(const std::string& preset,
                                                float wet,
                                                float dry,
                                                float decay) {
    // Idempotent: skip when the active profile already matches (avoids churning
    // the audio backend every Update tick while the listener stays in a biome).
    if (m_biomeReverb.applied && m_biomeReverb.preset == preset && m_biomeReverb.wet == wet &&
        m_biomeReverb.dry == dry && m_biomeReverb.decay == decay) {
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
    LUMINUMBRA_CORE_INFO(
        "Biome reverb applied: preset={} wet={} dry={} decay={}", preset, wet, dry, decay);
}

void EnvironmentalAudioSystem::ApplyBiomeReverbFromSize(const std::string& preset, float size01) {
    // canonical monotone size->reverb curve (see EnvironmentalAudioModel.h).
    const AudioModel::BiomeReverbParams p = AudioModel::BiomeReverbFromSize(size01);
    ApplyBiomeReverb(preset, p.wet, p.dry, p.decay);
}

AtmosphereAudioState EnvironmentalAudioSystem::ComputeAtmosphere(const glm::vec3& wind,
                                                                 float precipIntensity,
                                                                 float stormIntensity,
                                                                 float biomeWet,
                                                                 float biomeDry,
                                                                 float biomeDecay) {
    //  (AU1) PINNED model (the deterministic runtime contract ). Pure function of the
    // replicated weather sample -> two ambience layers + a weather reverb shift.
    AtmosphereAudioState state;
    state.applied = true;

    const float windSpeed = glm::length(glm::vec3(wind.x, 0.0f, wind.z));
    const float windSpeed01 = std::clamp(windSpeed / kAtmosphereWindRefSpeed, 0.0f, 1.0f);
    const float precip01 = std::clamp(precipIntensity, 0.0f, 1.0f);
    const float storm01 = std::clamp(stormIntensity, 0.0f, 1.0f);

    // Wind ambience: scales with wind speed, never fully silent in open air (a
    // faint air bed remains so a calm scene still moves).
    state.wind.intensity = windSpeed01;
    state.wind.volume = std::max(kAtmosphereWindFloor, windSpeed01);
    state.wind.present = state.wind.volume > kAtmosphereLayerFloor;

    // Rain ambience: silent when clear; rises with rain/storm precipitation. The
    // heavier of the category precip and the nearest-storm contribution drives it.
    const float rain01 = std::max(precip01, storm01);
    state.rain.intensity = rain01;
    state.rain.volume = rain01;
    state.rain.present = state.rain.volume > kAtmosphereLayerFloor;

    // Weather reverb shift: wet, overcast, precipitation-laden air carries early
    // reflections longer + wetter. The shift is precipitation-driven (so it tracks
    // weather, not just wind) and bounded so it cannot invert the dry/wet mix.
    const float shift = rain01;
    state.reverb_weather_shift = shift;
    state.reverb_wet = std::clamp(biomeWet + shift * kAtmosphereReverbWetBoost, 0.0f, 1.0f);
    state.reverb_dry = std::clamp(biomeDry - shift * kAtmosphereReverbWetBoost, 0.0f, 1.0f);
    state.reverb_decay = std::max(0.0f, biomeDecay + shift * kAtmosphereReverbDecayBoost);
    return state;
}

void EnvironmentalAudioSystem::UpdateAtmosphere(const glm::vec3& wind,
                                                float precipIntensity,
                                                float stormIntensity) {
    AtmosphereAudioState next = ComputeAtmosphere(wind,
                                                  precipIntensity,
                                                  stormIntensity,
                                                  m_biomeReverb.wet,
                                                  m_biomeReverb.dry,
                                                  m_biomeReverb.decay);
    next.apply_count = m_atmosphere.apply_count;

    // Idempotent at the backend: only push when the audible state changed (keeps
    // the per-tick Update path from churning the audio engine while weather holds).
    const bool changed = !m_atmosphere.applied || next.wind.volume != m_atmosphere.wind.volume ||
                         next.rain.volume != m_atmosphere.rain.volume ||
                         next.reverb_wet != m_atmosphere.reverb_wet ||
                         next.reverb_dry != m_atmosphere.reverb_dry ||
                         next.reverb_decay != m_atmosphere.reverb_decay;

    if (!changed) {
        return;
    }
    ++next.apply_count;
    m_atmosphere = next;

    // Keep the WeatherState mirror used by the legacy wind/rain ambience path in
    // sync (UpdateWeatherAudio reads it), then push the weather-shifted reverb +
    // wind parameters through the manager. All backend calls are suppressed in
    // null-audio mode (the manager is null/Null), so the null-audio gates are
    // unaffected -- this is optional dressing layered on the existing systems.
    m_weatherState.windStrength = m_atmosphere.wind.intensity;
    m_weatherState.windDirection =
        glm::length(wind) > 1e-4f ? glm::normalize(wind) : m_weatherState.windDirection;
    m_weatherState.isRaining = m_atmosphere.rain.present;
    m_weatherState.rainIntensity = m_atmosphere.rain.intensity;

    if (m_audioManager) {
        m_audioManager->SetWindParameters(m_weatherState.windDirection, m_atmosphere.wind.volume);
        m_audioManager->SetGlobalReverb(
            m_atmosphere.reverb_wet, m_atmosphere.reverb_dry, m_atmosphere.reverb_decay);
    }

    LUMINUMBRA_CORE_INFO("Atmosphere audio: wind={:.2f} rain={:.2f} reverb wet={:.2f} dry={:.2f} "
                         "decay={:.2f} (shift={:.2f})",
                         m_atmosphere.wind.volume,
                         m_atmosphere.rain.volume,
                         m_atmosphere.reverb_wet,
                         m_atmosphere.reverb_dry,
                         m_atmosphere.reverb_decay,
                         m_atmosphere.reverb_weather_shift);
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
                fadeFactor =
                    1.0f - ((distance - (zone->radius - zone->fadeDistance)) / zone->fadeDistance);
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

void EnvironmentalAudioSystem::ConfigureDayNightBeds(const AudioEventID& dayBed,
                                                     const AudioEventID& nightBed) {
    m_dayBedEvent = dayBed;
    m_nightBedEvent = nightBed;
    m_dayNight.configured = true;
    LUMINUMBRA_CORE_INFO("Day/night soundscape configured: day='{}' night='{}'", dayBed, nightBed);
}

void EnvironmentalAudioSystem::SetSunElevation(float sinSunElevation) {
    m_dayNight.sin_sun_elevation = std::clamp(sinSunElevation, -1.0f, 1.0f);
    if (!m_dayNight.has_sun_input) {
        // First feed SNAPS to the current day/night state so loading a world at
        // midnight starts on crickets, not a 2.5 s fade out of phantom birdsong.
        m_dayNight.has_sun_input = true;
        m_dayNight.night_factor = AudioModel::NightFactor(m_dayNight.sin_sun_elevation);
    }
}

void EnvironmentalAudioSystem::UpdateDayNightBeds(const glm::vec3& listenerPosition, float dt) {
    // sun-gated day/night ambient-bed crossfade (replaces the old
    // do-nothing time-of-day no-op: the birdsong bed used to play 24/7).
    if (!m_dayNight.configured)
        return;

    m_dayNight.target_night_factor = AudioModel::NightFactor(m_dayNight.sin_sun_elevation);
    m_dayNight.night_factor = AudioModel::SmoothTowards(m_dayNight.night_factor,
                                                        m_dayNight.target_night_factor,
                                                        dt,
                                                        AudioModel::kDayNightCrossfadeTau);
    const AudioModel::DayNightWeights weights =
        AudioModel::DayNightCrossfade(m_dayNight.night_factor);
    m_dayNight.day_weight = weights.day;
    m_dayNight.night_weight = weights.night;

    // Null-audio (telemetry/harness) keeps the model state observable without a
    // backend; only the bed start/stop/volume calls need a live manager.
    if (!m_audioManager)
        return;

    const auto driveBed = [&](const AudioEventID& bed, float weight, bool& started) {
        if (bed.empty())
            return;
        if (weight > AudioModel::kBedSilenceFloor) {
            if (!started) {
                // Huge radius = effectively constant world ambience (matches the
                // client's existing beds); the listener follows the player anyway.
                m_audioManager->PlayAmbientLoop(bed, listenerPosition, 1.0e6f);
                started = true;
                ++m_dayNight.apply_count;
                LUMINUMBRA_CORE_INFO("Day/night bed started: '{}'", bed);
            }
            m_audioManager->SetAmbientVolume(bed, weight);
        } else if (started) {
            m_audioManager->StopAmbientLoop(bed);
            started = false;
            ++m_dayNight.apply_count;
            LUMINUMBRA_CORE_INFO("Day/night bed stopped: '{}'", bed);
        }
    };
    driveBed(m_dayBedEvent, m_dayNight.day_weight, m_dayNight.day_bed_started);
    driveBed(m_nightBedEvent, m_dayNight.night_weight, m_dayNight.night_bed_started);
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