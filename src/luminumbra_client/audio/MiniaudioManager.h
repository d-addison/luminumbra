#pragma once
#include "audio/IAudioManager.h"
#include <miniaudio.h>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>
#include <random>
#include <array>

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
    bool PlayOneShot(const AudioEventID& eventID, const glm::vec3& position) override;
    bool PlayOneShot2D(const AudioEventID& eventID) override;
    void PlayMusic(const AudioEventID& musicEventID) override;
    void StopMusic();

    void SetMasterVolume(float volume) override;

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

    std::string m_rootPath;
    std::unique_ptr<ma_engine> m_engine;
    std::unordered_map<AudioEventID, AudioEventDefinition> m_eventDefinitions;
    
    // For controllable, active sounds
    std::unordered_map<AudioEventHandle, std::unique_ptr<ma_sound>> m_activeSounds;
    AudioEventHandle m_nextHandle = 1;

    // Fire-and-forget 3D one-shots (PlayOneShot). These MUST outlive the call: miniaudio's
    // mixing thread reads the ma_sound until it finishes, so the node has to stay alive (and be
    // ma_sound_uninit'd, not just freed) — Update() reaps the ones that have stopped playing.
    std::vector<std::unique_ptr<ma_sound>> m_oneShotSounds;

    // For music
    std::unique_ptr<ma_sound> m_currentMusic;
    AudioEventID m_currentMusicID;

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
    
    // For random selection in banks
    std::mt19937 m_rng;
    
    // Spatial audio clustering system
    std::unique_ptr<AudioSpatialCluster> m_spatial_cluster;
    bool m_spatial_clustering_enabled = true;
};

} // namespace Luminumbra::Client
