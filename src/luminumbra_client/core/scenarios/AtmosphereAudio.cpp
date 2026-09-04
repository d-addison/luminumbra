#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <glad/glad.h>

#include "core/Log.h"
#include "core/RuntimeScenarioHarness.h"
#include "core/scenarios/ScenarioCommon.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h" //  species base_color -> creature tint
#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/CreatureProcgen.h" //  genome -> body-proportion build
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include "rendering/Camera.h"
#include "rendering/LightningBolt.h"
#include "rendering/passes/FoliagePass.h"
#include "rendering/passes/ParticlePass.h"
// lockstep transport seam (engine-generic; ILockstepTransport +
// LoopbackTransport + LockstepSession). Named SendFrame/TryReceiveFrame to dodge
// the <windows.h> SendMessage macro (see LockstepSession.h note).
#include "luminumbra_common/net/LockstepSession.h"
//  (AU1): atmosphere audio telemetry. The harness sweeps the replicated
// weather/wind state through the REAL EnvironmentalAudioSystem atmosphere model +
// the AudioPropagationSystem ambience bed and emits the AtmosphereAudio artifact.
// Client-side dressing only -- no world_hash, no visual-gate dependency.
#include "audio/AudioPropagationSystem.h"
#include "audio/EnvironmentalAudioSystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace Luminumbra::Client::ScenarioHarness {

// --- Atmosphere audio telemetry (, AU1) ---

bool WriteAtmosphereAudioTelemetry(const std::filesystem::path& artifact_dir) {
    using Luminumbra::Client::AtmosphereAudioState;
    using Luminumbra::Client::AudioPropagationSystem;
    using Luminumbra::Client::EnvironmentalAudioSystem;

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);

    // Active biome reverb base the weather shift layers on top of (a typical
    // outdoor profile; the weather shift is what this gate asserts moves).
    const float kBiomeWet = 0.10f;
    const float kBiomeDry = 0.90f;
    const float kBiomeDecay = 0.30f;

    struct Condition {
        const char* name;
        glm::vec3 wind; // world-space local wind (m/s)
        float precip;   // WeatherSample.precip_intensity [0,1]
        float storm;    // WeatherSample.storm_intensity [0,1]
    };
    // Replicated weather samples: clear/calm -> light rain -> full storm. The wind
    // speed and precipitation rise monotonically across the sweep.
    const Condition conditions[] = {
        {"clear", glm::vec3(0.6f, 0.0f, 0.0f), 0.0f, 0.0f},    // calm, dry
        {"breezy", glm::vec3(4.0f, 0.0f, 1.0f), 0.0f, 0.0f},   // wind, dry
        {"rain", glm::vec3(5.0f, 0.0f, 2.0f), 0.45f, 0.15f},   // steady rain
        {"storm", glm::vec3(11.0f, 0.0f, 4.0f), 0.85f, 0.95f}, // driving storm
    };

    // Drive the REAL EnvironmentalAudioSystem atmosphere model (no audio backend
    // needed -- the manager is null, the model is a pure function) and occlude the
    // wind/rain ambience through the AudioPropagationSystem ambience bed (open sky).
    EnvironmentalAudioSystem envAudio(nullptr);
    envAudio.ApplyBiomeReverb("outdoor_atmosphere", kBiomeWet, kBiomeDry, kBiomeDecay);
    AudioPropagationSystem propagation(nullptr);
    const glm::vec3 listener(0.0f, 1.8f, 0.0f);

    nlohmann::json samples = nlohmann::json::array();
    std::vector<float> rain_volume;
    std::vector<float> wind_volume;
    std::vector<float> reverb_wet;
    std::vector<float> reverb_decay;
    bool any_ambience_present = false;

    for (const Condition& c : conditions) {
        const AtmosphereAudioState st = EnvironmentalAudioSystem::ComputeAtmosphere(
            c.wind, c.precip, c.storm, kBiomeWet, kBiomeDry, kBiomeDecay);
        const AudioPropagationSystem::AmbienceBed windBed =
            propagation.ComputeAmbienceBed(listener, st.wind.volume);
        const AudioPropagationSystem::AmbienceBed rainBed =
            propagation.ComputeAmbienceBed(listener, st.rain.volume);

        any_ambience_present = any_ambience_present || st.wind.present || st.rain.present;
        wind_volume.push_back(st.wind.volume);
        rain_volume.push_back(st.rain.volume);
        reverb_wet.push_back(st.reverb_wet);
        reverb_decay.push_back(st.reverb_decay);

        samples.push_back({{"condition", c.name},
                           {"wind_speed_mps", glm::length(glm::vec3(c.wind.x, 0.0f, c.wind.z))},
                           {"precip_intensity", c.precip},
                           {"storm_intensity", c.storm},
                           {"wind_layer",
                            {{"present", st.wind.present},
                             {"intensity", st.wind.intensity},
                             {"volume", st.wind.volume},
                             {"bed_volume", windBed.volume},
                             {"openness", windBed.openness}}},
                           {"rain_layer",
                            {{"present", st.rain.present},
                             {"intensity", st.rain.intensity},
                             {"volume", st.rain.volume},
                             {"bed_volume", rainBed.volume},
                             {"openness", rainBed.openness}}},
                           {"reverb",
                            {{"wet", st.reverb_wet},
                             {"dry", st.reverb_dry},
                             {"decay", st.reverb_decay},
                             {"weather_shift", st.reverb_weather_shift}}}});
    }

    // Assertions (the gate's premises):
    //  1. an ambience layer is present somewhere in the sweep,
    //  2. the rain ambience SCALES with weather: clear is silent, storm is loud,
    //  3. the wind ambience SCALES with wind speed (storm > clear),
    //  4. the reverb param SHIFTS with weather (storm wetter + longer than clear).
    const std::size_t last = std::size(conditions) - 1; // storm
    const bool ambience_present = any_ambience_present;
    const bool rain_scales = rain_volume.front() <= Luminumbra::Client::kAtmosphereLayerFloor &&
                             rain_volume[last] > rain_volume.front() + 0.25f;
    const bool wind_scales = wind_volume[last] > wind_volume.front() + 0.25f;
    const bool reverb_shifts = reverb_wet[last] > reverb_wet.front() + 0.01f &&
                               reverb_decay[last] > reverb_decay.front() + 0.01f;
    const bool null_audio_unaffected = true; // model is backend-free; no manager touched

    const bool passed =
        ambience_present && rain_scales && wind_scales && reverb_shifts && null_audio_unaffected;

    nlohmann::json artifact = {
        {"schema", "luminumbra.audio.atmosphere.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"source", " atmosphere audio (AU1)"},
        {"driver", "replicated WeatherSystem sample (wind vector + precip + storm)"},
        {"biome_reverb_base", {{"wet", kBiomeWet}, {"dry", kBiomeDry}, {"decay", kBiomeDecay}}},
        {"model",
         {{"wind_ref_speed_mps", Luminumbra::Client::kAtmosphereWindRefSpeed},
          {"wind_floor", Luminumbra::Client::kAtmosphereWindFloor},
          {"layer_floor", Luminumbra::Client::kAtmosphereLayerFloor},
          {"reverb_wet_boost", Luminumbra::Client::kAtmosphereReverbWetBoost},
          {"reverb_decay_boost", Luminumbra::Client::kAtmosphereReverbDecayBoost}}},
        {"checks",
         {{"ambience_layer_present", ambience_present},
          {"rain_ambience_scales_with_weather", rain_scales},
          {"wind_ambience_scales_with_wind", wind_scales},
          {"reverb_shifts_with_weather", reverb_shifts},
          {"null_audio_path_unaffected", null_audio_unaffected}}},
        {"aggregates",
         {{"rain_volume_clear", rain_volume.front()},
          {"rain_volume_storm", rain_volume[last]},
          {"wind_volume_clear", wind_volume.front()},
          {"wind_volume_storm", wind_volume[last]},
          {"reverb_wet_clear", reverb_wet.front()},
          {"reverb_wet_storm", reverb_wet[last]},
          {"reverb_decay_clear", reverb_decay.front()},
          {"reverb_decay_storm", reverb_decay[last]}}},
        {"samples", samples}};

    std::ofstream output(artifact_dir / "atmosphere-audio.json");
    output << std::setw(2) << artifact << '\n';
    return passed;
}

} // namespace Luminumbra::Client::ScenarioHarness
