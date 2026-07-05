#pragma once
#include "audio/IAudioManager.h"
#include "audio/MixerModel.h"
#include <miniaudio.h>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>
#include <random>
#include <array>
#include <chrono>

// Forward declaration for spatial clustering
namespace Luminumbra::Client { class AudioSpatialCluster; }
namespace Luminumbra::Systems { class PhysicsSystem; }

namespace Luminumbra::Client {

// --- Forward Declarations and Internal Structs ---
struct AudioLayer {
    std::vector<std::string> files;
    float volume = 1.0f;
    float pitch = 1.0f;
    float pitch_variation = 0.0f;
    float delay = 0.0f;
    float fade_in = 0.0f;
    float fade_out = 0.0f;
    bool sync_to_master = false;
    std::string trigger_condition = "always"; // always, random, parameter, distance
    float trigger_value = 0.0f;
    std::string filter_type = "none"; // none, lowpass, highpass, bandpass
    float filter_frequency = 1000.0f;
    float filter_resonance = 1.0f;
};

struct AudioEventDefinition {
    // Basic properties
    std::vector<std::string> files;
    std::vector<AudioLayer> layers; // For complex layered sounds
    float volume = 1.0f;
    float pitch_variation = 0.0f;
    bool is_2d = false;
    bool is_looping = false;
    bool is_streaming = false;
    std::string strategy = "random"; // random, sequential, simultaneous, layered, procedural
    
    // 3D Audio Properties
    float min_distance = 1.0f;
    float max_distance = 100.0f;
    float rolloff_factor = 1.0f;
    std::string attenuation_model = "inverse";
    float doppler_factor = 1.0f;
    
    // Environmental Properties
    bool use_reverb = false;
    float reverb_level = 0.0f;
    std::string reverb_type = "room";
    
    // Complex Sound Features
    bool enable_procedural = false;
    std::string procedural_type = "none"; // wind, fire, water, creature_vocal, machinery
    std::unordered_map<std::string, float> procedural_params;
    
    // Adaptive Properties
    bool adaptive_volume = false;
    bool adaptive_pitch = false;
    bool adaptive_filter = false;
    float adaptation_speed = 1.0f;
    
    // Interaction with other sounds
    std::vector<std::string> interrupt_events; // Events that stop this sound
    std::vector<std::string> triggered_events; // Events triggered when this plays
    float crossfade_time = 0.0f;
    
    // Variation system
    int max_simultaneous_instances = 1;
    float instance_spacing_ms = 100.0f;
    bool enable_chorus_effect = false;
    float chorus_voices = 2.0f;
    float chorus_spread = 0.1f;
};

// Environmental Audio Zones
enum class AudioEnvironmentType {
    Outdoor,
    Cave,
    Forest,
    Underground,
    Water,
    Canyon,
    Building
};

struct AudioEnvironment {
    AudioEnvironmentType type;
    float reverb_decay = 1.0f;
    float reverb_wet = 0.3f;
    float reverb_dry = 0.7f;
    float echo_delay = 0.15f;
    float echo_decay = 0.6f;
    float ambient_volume = 1.0f;
    glm::vec3 wind_direction = {1.0f, 0.0f, 0.0f};
    float wind_strength = 0.0f;
};

class MiniaudioManager final : public IAudioManager {
public:
    MiniaudioManager(const std::string& root_path);
    ~MiniaudioManager();

    // --- IAudioManager Interface ---
    bool Init() override;
    void Update() override;
    void Shutdown() override;

    bool LoadBank(const std::string& bankPath) override;
    void UnloadBank(const std::string& bankPath) override;

    void SetListenerTransform(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) override;

    bool PlayEvent(const AudioEventID& eventID, AudioEventHandle& outHandle) override;
    // NOTE: the re-stated defaults MUST match IAudioManager's (callers holding a
    // concrete MiniaudioManager* — e.g. EnvironmentalAudioSystem — bind these).
    bool PlayOneShot(const AudioEventID& eventID, const glm::vec3& position,
                     BusId bus = BusId::Sfx) override;
    bool PlayOneShot2D(const AudioEventID& eventID, BusId bus = BusId::Sfx) override;
    void PlayMusic(const AudioEventID& musicEventID) override;
    void StopMusic();

    void SetMasterVolume(float volume) override;
    void SetMusicVolume(float volume) override;
    void SetSfxVolume(float volume) override;
    void SetBusVolume(BusId bus, float volume) override;

    // AUDIO-10 sidechain-duck tuning (ambient floor / attack / release; music
    // floor 1.0 = music duck disabled). Safe to call any time; render-only.
    void SetDuckParams(const Audio::DuckParams& params) { m_ducker.SetParams(params); }

    bool StopEvent(AudioEventHandle handle, bool immediate = true) override;
    bool SetEventPosition(AudioEventHandle handle, const glm::vec3& position) override;
    bool SetEventVolume(AudioEventHandle handle, float volume) override;
    bool SetEventParameter(AudioEventHandle handle, const AudioParamID& paramID, float value) override;

    // Enhanced Audio Features
    void SetEnvironment(const AudioEnvironment& environment);
    void SetWindParameters(const glm::vec3& direction, float strength);
    void UpdateAudioOcclusion(AudioEventHandle handle, float occlusion_factor);
    void SetGlobalReverb(float wet, float dry, float decay);
    
    // Environmental Audio
    void PlayAmbientLoop(const AudioEventID& eventID, const glm::vec3& position, float radius) override;
    void StopAmbientLoop(const AudioEventID& eventID) override;
    void SetAmbientVolume(const AudioEventID& eventID, float scale) override;

    // Spatial Audio Clustering Integration
    void SetPhysicsSystem(::Luminumbra::Systems::PhysicsSystem* physics_system);
    void EnableSpatialClustering(bool enabled) { m_spatial_clustering_enabled = enabled; }
    bool IsSpatialClusteringEnabled() const { return m_spatial_clustering_enabled; }
    
private:
    ma_result LoadSoundResource(const std::string& path, ma_sound* sound, uint32_t flags);
    const AudioEventDefinition* GetEventDefinition(const AudioEventID& eventID);
    void ApplyEnvironmentalEffects(ma_sound* sound, const AudioEventDefinition* def);
    void UpdateWindEffect();
    float CalculateOcclusion(const glm::vec3& source, const glm::vec3& listener);

    // AUDIO-05/AUDIO-10 bus plumbing. GroupFor maps a BusId to the ma_sound_group
    // playback attaches to (nullptr => engine endpoint, the pre-bus behaviour —
    // also the graceful fallback if group init ever failed). ApplyAmbientBusGain
    // composes the user ambient volume with the ducker's sidechain gain.
    ma_sound_group* GroupFor(BusId bus) const;
    void ApplyAmbientBusGain();

    std::string m_rootPath;
    std::unique_ptr<ma_engine> m_engine;
    std::unordered_map<AudioEventID, AudioEventDefinition> m_eventDefinitions;

    // For controllable, active sounds
    std::unordered_map<AudioEventHandle, std::unique_ptr<ma_sound>> m_activeSounds;
    AudioEventHandle m_nextHandle = 1;

    // Fire-and-forget one-shots (PlayOneShot / PlayOneShot2D). These MUST outlive the
    // call: miniaudio's mixing thread reads the ma_sound until it finishes, so the node
    // has to stay alive (and be ma_sound_uninit'd, not just freed) — Update() reaps the
    // ones that have stopped playing. The bus is remembered so reaping an Events-bus
    // voice releases the sidechain duck.
    struct OneShotVoice {
        std::unique_ptr<ma_sound> sound;
        BusId bus = BusId::Sfx;
    };
    std::vector<OneShotVoice> m_oneShotSounds;

    // AUDIO-05/AUDIO-10: the mix-bus groups. sfx hangs off the engine endpoint;
    // ambient/events/ui are CHILDREN of sfx (so user.audio_sfx scales them all).
    // Music keeps its existing per-sound volume path (untouched). ma_sound_group
    // nodes live in the mixing graph: heap-owned so their addresses are stable,
    // uninited in Shutdown AFTER all attached sounds, BEFORE the engine.
    std::unique_ptr<ma_sound_group> m_sfxGroup;
    std::unique_ptr<ma_sound_group> m_ambientGroup;
    std::unique_ptr<ma_sound_group> m_eventsGroup;
    std::unique_ptr<ma_sound_group> m_uiGroup;
    float m_sfxVolume = 1.0f;      // user.audio_sfx
    float m_ambientVolume = 1.0f;  // authored ambient-bus gain (pre-duck)
    float m_eventsVolume = 1.0f;
    float m_uiVolume = 1.0f;

    // AUDIO-10 sidechain ducker (pure math, audio/MixerModel.h) — advanced with
    // WALL-CLOCK dt in Update(); client-side presentation only, never sim.
    Audio::MixerDucker m_ducker;
    std::chrono::steady_clock::time_point m_lastUpdateTime{};
    bool m_hasLastUpdateTime = false;
    float m_lastAppliedAmbientGain = -1.0f;  // dedupe ma_sound_group_set_volume calls
    float m_lastAppliedMusicDuckGain = -1.0f;

    // For music
    std::unique_ptr<ma_sound> m_currentMusic;
    AudioEventID m_currentMusicID;
    float m_musicVolume = 1.0f;  // music-bus gain (user.audio.music); multiplies the bank volume

    // Environmental Audio
    AudioEnvironment m_currentEnvironment;
    std::unordered_map<AudioEventID, std::unique_ptr<ma_sound>> m_ambientSounds;
    
    // Wind system
    std::unique_ptr<ma_sound> m_windSound;
    glm::vec3 m_windDirection = {1.0f, 0.0f, 0.0f};
    float m_windStrength = 0.0f;
    
    // Reverb/Echo effects (would need custom implementation)
    ma_delay m_echoDelay;
    bool m_echoInitialized = false;

    // AUDIO-09 (spec 021 rank ~100): global reverb PROXY. miniaudio 0.11.22 has
    // NO built-in reverb node, so SetGlobalReverb drives a single feedback
    // delay line (ma_delay_node, core miniaudio >= 0.11) spliced between the
    // AMBIENT bus group and its parent (ambient -> delay -> sfx -> endpoint):
    // one attach, so the AUDIO-05/10 ambient gain + sidechain duck stay
    // upstream and keep working unchanged. wet/dry map straight onto the
    // node's mix; biome decay maps to a stability-capped feedback gain; the
    // delay TIME is fixed at node init (see AudioModel::ReverbProxyFromParams
    // for the honest limitations of the proxy). Lazily created on the first
    // SetGlobalReverb call; requires the ambient group (no group => the proxy
    // stays off and SetGlobalReverb just logs, the old stub behaviour).
    ma_delay_node m_reverbNode;
    bool m_reverbNodeInitialized = false;
    
    // For random selection in banks
    std::mt19937 m_rng;
    
    // Spatial audio clustering system
    std::unique_ptr<AudioSpatialCluster> m_spatial_cluster;
    bool m_spatial_clustering_enabled = true;
};

} // namespace Luminumbra::Client
