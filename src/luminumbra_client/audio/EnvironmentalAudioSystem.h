#pragma once
#include "MiniaudioManager.h"
#include "glm/glm.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace Luminumbra::Client {

//  (AU1): atmosphere-audio layer descriptors. The replicated weather/wind
// state drives two AMBIENCE layers (wind + rain) plus a weather-modulated reverb
// shift. This is render/client-only dressing (no world_hash, no visual-gate
// dependency): the NULL-audio path is unaffected because the model is a pure
// function the harness samples for telemetry and the live client applies through
// IAudioManager (suppressed centrally in null mode).
//
// PINNED model (the deterministic runtime contract ). Inputs are the WeatherSystem replicated
// sample fields, all in [0, 1] except wind which is a world-space vector:
//   wind_speed01 = clamp(|wind| / kAtmosphereWindRefSpeed, 0, 1)
//   precip01     = clamp(precip_intensity, 0, 1)
//   storm01      = clamp(storm_intensity, 0, 1)
// Layer volumes (each [0, 1]):
//   wind layer  = wind_speed01 (gusts louder in stronger wind; always present
//                 above a faint floor so a calm scene still has air movement)
//   rain layer  = max(precip01, storm01) (silent when clear, rises with rain and
//                 storm precipitation; storms add the heavier downpour)
// Weather reverb shift (added on top of the active biome reverb): wet/decay rise
// with precipitation (a wet, dense, overcast air carries early reflections longer
// and wetter), dry falls. The shift is bounded so it can never invert the mix.
struct AtmosphereAudioLayer {
    bool present = false;   // layer audible this frame (volume above the floor)
    float intensity = 0.0f; // [0, 1] driver (wind speed / precip) before volume
    float volume = 0.0f;    // [0, 1] applied ambience volume
};

struct AtmosphereAudioState {
    bool applied = false;
    AtmosphereAudioLayer wind; // wind ambience (scales with wind speed)
    AtmosphereAudioLayer rain; // rain ambience (scales with precip/storm)
    // Weather-modulated reverb the listener hears (base biome reverb + weather
    // shift). These are the values pushed to the audio backend's global reverb.
    float reverb_wet = 0.0f;
    float reverb_dry = 1.0f;
    float reverb_decay = 0.0f;
    // The precipitation-driven reverb shift applied on top of the biome base
    // (telemetry: proves the reverb param SHIFTS with weather).
    float reverb_weather_shift = 0.0f;
    std::uint64_t apply_count = 0; // distinct atmosphere transitions pushed
};

// Reference wind speed (m/s) at which the wind ambience layer saturates. Stronger
// gusts than this are clamped to full volume. PINNED so the telemetry sweep and
// the live client agree.
inline constexpr float kAtmosphereWindRefSpeed = 12.0f;
// Faint floor the wind ambience never drops below in the open air (so a calm
// outdoor scene still carries a barely-audible air bed). Below kAtmosphereLayerFloor
// the rain layer is treated as silent/absent.
inline constexpr float kAtmosphereWindFloor = 0.04f;
inline constexpr float kAtmosphereLayerFloor = 0.02f;
// Maximum weather reverb wet/decay boost added on top of the biome base at full
// precipitation. Bounded so the weather shift can never invert the dry/wet mix.
inline constexpr float kAtmosphereReverbWetBoost = 0.25f;
inline constexpr float kAtmosphereReverbDecayBoost = 0.6f;

class EnvironmentalAudioSystem {
public:
    EnvironmentalAudioSystem(MiniaudioManager* audioManager);
    ~EnvironmentalAudioSystem();

    void Update(const glm::vec3& listenerPosition, float deltaTime);

    // Environment detection and switching
    void SetEnvironment(AudioEnvironmentType type, const glm::vec3& position);
    void UpdateWeatherConditions(float windStrength,
                                 const glm::vec3& windDirection,
                                 bool isRaining = false);

    // Zone-based ambient audio
    void RegisterAmbientZone(const std::string& zoneId,
                             const glm::vec3& center,
                             float radius,
                             const AudioEventID& ambientSound);
    void UnregisterAmbientZone(const std::string& zoneId);

    // Material-based footstep system
    void PlayFootstepSound(const glm::vec3& position, const std::string& materialType);

    // Real-time environmental audio adjustments
    void SetTimeOfDay(float timeNormalized);     // 0.0 = midnight, 0.5 = noon
    void SetSeasonalEffects(float seasonFactor); // 0.0 = winter, 1.0 = summer

    //  ( rank ~93): day/night soundscape. The day bed (birdsong)
    // is GATED by sun elevation and crossfades against a night bed (crickets/
    // owls) over a few seconds (AudioModel::kDayNightCrossfadeTau). Once
    // configured, THIS system owns the two beds' lifecycle (start/stop/volume
    // via the IAudioManager ambient-loop surface) — the client must not also
    // PlayAmbientLoop them. Unconfigured, the system leaves ambient beds alone
    // (legacy behavior: whatever the client starts plays 24/7).
    void ConfigureDayNightBeds(const AudioEventID& dayBed, const AudioEventID& nightBed);

    // Sun-elevation INPUT seam: sinSunElevation = sin(sun elevation angle) in
    // [-1, 1] — the vertical component of the normalized TOWARD-sun direction.
    // Fed from OUTSIDE per frame (the client reads the render pipeline's
    // time-of-day state, e.g. -render_pipeline.sun_direction.y, and pushes it
    // here); the audio system deliberately never reaches into render globals.
    // The FIRST feed snaps the crossfade to the current day/night state (no
    // spurious dawn fade when loading a world at midnight); later feeds smooth.
    void SetSunElevation(float sinSunElevation);

    struct DayNightState {
        bool configured = false;
        bool has_sun_input = false;
        float sin_sun_elevation = 1.0f; // default full day == legacy 24/7 birdsong
        float target_night_factor = 0.0f;
        float night_factor = 0.0f; // smoothed [0,1]; 0 = day, 1 = night
        float day_weight = 1.0f;   // applied day-bed volume scale
        float night_weight = 0.0f; // applied night-bed volume scale
        bool day_bed_started = false;
        bool night_bed_started = false;
        std::uint64_t apply_count = 0; // bed start/stop transitions (telemetry)
    };
    const DayNightState& CurrentDayNight() const {
        return m_dayNight;
    }

    // apply the active biome's reverb profile. preset is the authored
    // label (opaque to the engine); wet/dry/decay drive the audio manager's
    // global reverb. Idempotent: re-applying the same profile is a no-op so the
    // per-tick Update path does not churn the backend. The last applied profile
    // is exposed for the audio telemetry artifact (reverb block).
    void ApplyBiomeReverb(const std::string& preset, float wet, float dry, float decay);

    // apply a reverb profile from an acoustic-space SIZE scalar
    // ([0,1], 0 ~ open field, 1 ~ canyon) via the canonical monotone curve
    // (AudioModel::BiomeReverbFromSize). Authored biomes should keep calling
    // ApplyBiomeReverb with their biomes.json values; this is the entry point
    // for procedural/unauthored spaces that only know a size.
    void ApplyBiomeReverbFromSize(const std::string& preset, float size01);

    struct BiomeReverbState {
        bool applied = false;
        std::string preset;
        float wet = 0.0f;
        float dry = 0.0f;
        float decay = 0.0f;
        std::uint64_t apply_count = 0; // distinct profile transitions
    };
    const BiomeReverbState& CurrentBiomeReverb() const {
        return m_biomeReverb;
    }

    //  (AU1): drive the atmosphere ambience + weather reverb shift from a
    // replicated weather sample. `wind` is the world-space local wind vector (the
    // WeatherSample.wind / WindFieldSystem sample, magnitude in m/s); precip and
    // storm are the [0, 1] WeatherSample fields. Pure data->audio: it computes the
    // ambience layer volumes (ComputeAtmosphere) and pushes them + the weather-
    // shifted global reverb through the audio manager. Idempotent at the backend:
    // re-applying an unchanged state is a no-op (does not churn per Update tick).
    // Render/client-only: writes nothing to the sim. Null-audio safe (the manager
    // suppresses the backend calls centrally).
    void UpdateAtmosphere(const glm::vec3& wind, float precipIntensity, float stormIntensity);

    // Pure function: the PINNED atmosphere model (the deterministic runtime contract ). Exposed
    // so the telemetry harness can sweep weather conditions without an audio
    // backend. `biomeWet/biomeDry/biomeDecay` are the active biome reverb base the
    // weather shift is layered on top of.
    static AtmosphereAudioState ComputeAtmosphere(const glm::vec3& wind,
                                                  float precipIntensity,
                                                  float stormIntensity,
                                                  float biomeWet,
                                                  float biomeDry,
                                                  float biomeDecay);

    const AtmosphereAudioState& CurrentAtmosphere() const {
        return m_atmosphere;
    }

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
        float timeOfDay = 0.5f;      // Noon by default
        float seasonalFactor = 0.5f; // Spring/Fall
    };

    void UpdateAmbientZones(const glm::vec3& listenerPosition);
    void UpdateWeatherAudio();
    // drive the sun-gated day/night ambient-bed crossfade (replaces
    // the old no-op UpdateTimeBasedEffects path). dt is the real elapsed
    // seconds since the last throttled tick, so the crossfade speed is
    // frame-rate independent.
    void UpdateDayNightBeds(const glm::vec3& listenerPosition, float dt);
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

    BiomeReverbState m_biomeReverb; // last applied per-biome reverb

    AtmosphereAudioState m_atmosphere; //  last applied atmosphere state

    // day/night bed crossfade state + the two configured bed events.
    DayNightState m_dayNight;
    AudioEventID m_dayBedEvent;
    AudioEventID m_nightBedEvent;
};

} // namespace Luminumbra::Client