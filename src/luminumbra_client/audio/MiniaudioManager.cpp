#include "audio/MiniaudioManager.h"
#include "AudioSpatialCluster.h"
#include "../../luminumbra_common/systems/PhysicsSystem.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <random>
#include "core/Log.h"

namespace Luminumbra::Client {

MiniaudioManager::MiniaudioManager(const std::string& root_path) : m_rootPath(root_path), m_engine(nullptr), m_rng(std::random_device{}()) {
    // Initialize spatial clustering system
    m_spatial_cluster = std::make_unique<AudioSpatialCluster>();
}
MiniaudioManager::~MiniaudioManager() {
    // Shutdown should be called explicitly
}

bool MiniaudioManager::Init() {
    m_engine = std::make_unique<ma_engine>();
    ma_result result = ma_engine_init(NULL, m_engine.get());
    if (result != MA_SUCCESS) {
        LUMINUMBRA_CORE_ERROR("Failed to initialize miniaudio engine.");
        m_engine = nullptr;
        return false;
    }
    LUMINUMBRA_CORE_INFO("Miniaudio Manager Initialized.");
    return true;
}

void MiniaudioManager::Update() {
    if (!m_engine) return;

    // Update spatial audio clustering system
    if (m_spatial_clustering_enabled && m_spatial_cluster) {
        // Get listener position from miniaudio engine
        ma_vec3f ma_listener_pos = ma_engine_listener_get_position(m_engine.get(), 0);
        glm::vec3 listener_pos(ma_listener_pos.x, ma_listener_pos.y, ma_listener_pos.z);
        
        // Update spatial clustering (this handles batched occlusion calculations)
        m_spatial_cluster->Update(listener_pos, 1.0f/60.0f); // Assume 60 FPS for delta time
    }

    for (auto it = m_activeSounds.begin(); it != m_activeSounds.end(); ) {
        if (!ma_sound_is_playing(it->second.get())) {
            // Remove from spatial clustering system
            if (m_spatial_clustering_enabled && m_spatial_cluster) {
                m_spatial_cluster->RemoveAudioSource(it->first);
            }
            
            ma_sound_uninit(it->second.get());
            it = m_activeSounds.erase(it);
        } else {
            ++it;
        }
    }

    // Reap fire-and-forget 3D one-shots (PlayOneShot) that have finished playing.
    for (auto it = m_oneShotSounds.begin(); it != m_oneShotSounds.end(); ) {
        if (!ma_sound_is_playing(it->get())) {
            ma_sound_uninit(it->get());
            it = m_oneShotSounds.erase(it);
        } else {
            ++it;
        }
    }
    
    // Update wind effects on active sounds
    UpdateWindEffect();
}

void MiniaudioManager::Shutdown() {
    if (m_engine) {
        if (m_currentMusic) {
            ma_sound_uninit(m_currentMusic.get());
            m_currentMusic.reset();
        }
        for (auto& [handle, sound_ptr] : m_activeSounds) {
            ma_sound_uninit(sound_ptr.get());
        }
        m_activeSounds.clear();
        for (auto& sound_ptr : m_oneShotSounds) {
            ma_sound_uninit(sound_ptr.get());
        }
        m_oneShotSounds.clear();
        for (auto& [id, sound_ptr] : m_ambientSounds) {
            ma_sound_uninit(sound_ptr.get());
        }
        m_ambientSounds.clear();
        ma_engine_uninit(m_engine.get());
        m_engine = nullptr;
        LUMINUMBRA_CORE_ERROR("Miniaudio Manager Shutdown.");
    }
}

bool MiniaudioManager::LoadBank(const std::string& bankPath) {
    const std::string full_path = m_rootPath + bankPath;
    std::ifstream f(full_path);
    if (!f.is_open()) {
        LUMINUMBRA_CORE_ERROR("Failed to open sound bank: " + full_path);
        return false;
    }
    
    nlohmann::json bank_json;
    try {
        bank_json = nlohmann::json::parse(f);
    } catch (nlohmann::json::parse_error& e) {
        LUMINUMBRA_CORE_ERROR("Failed to parse sound bank JSON: " + full_path + " - " + e.what());
        return false;
    }

    bool is_streaming = bank_json.value("streaming", false);

    for (auto& [event_id, event_def_json] : bank_json["events"].items()) {
        AudioEventDefinition def;
        def.files = event_def_json["files"].get<std::vector<std::string>>();
        def.volume = event_def_json.value("volume", 1.0f);
        def.pitch_variation = event_def_json.value("pitch_variation", 0.0f);
        def.is_2d = event_def_json.value("is_2d", false);
        def.is_looping = event_def_json.value("looping", false);
        def.is_streaming = is_streaming;
        
        // Enhanced 3D Audio Properties
        def.min_distance = event_def_json.value("min_distance", 1.0f);
        def.max_distance = event_def_json.value("max_distance", 100.0f);
        def.rolloff_factor = event_def_json.value("rolloff_factor", 1.0f);
        def.attenuation_model = event_def_json.value("attenuation_model", "inverse");
        def.doppler_factor = event_def_json.value("doppler_factor", 1.0f);
        
        // Environmental Properties
        def.use_reverb = event_def_json.value("use_reverb", false);
        def.reverb_level = event_def_json.value("reverb_level", 0.0f);
        def.reverb_type = event_def_json.value("reverb_type", "room");
        
        m_eventDefinitions[event_id] = def;
    }

    LUMINUMBRA_CORE_INFO("Loaded sound bank: " + std::to_string(m_eventDefinitions.size()) + " events from " + full_path);
    return true;
}

void MiniaudioManager::UnloadBank(const std::string& bankPath) {
    LUMINUMBRA_CORE_WARN("AUDIO WARNING: Unloading banks is not fully implemented.");
}

void MiniaudioManager::SetListenerTransform(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
    if (!m_engine) return;
    ma_engine_listener_set_position(m_engine.get(), 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(m_engine.get(), 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(m_engine.get(), 0, up.x, up.y, up.z);
}

bool MiniaudioManager::PlayEvent(const AudioEventID& eventID, AudioEventHandle& outHandle) {
    if (!m_engine) return false;
    const AudioEventDefinition* def = GetEventDefinition(eventID);
    if (!def || def->files.empty()) return false;

    auto sound = std::make_unique<ma_sound>();
    
    std::uniform_int_distribution<> dist(0, static_cast<int>(def->files.size()) - 1);
    const std::string& rel_path = def->files[dist(m_rng)];
    const std::string full_path = m_rootPath + rel_path;

    uint32_t flags = MA_SOUND_FLAG_DECODE;
    if (def->is_2d) flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

    if (ma_sound_init_from_file(m_engine.get(), full_path.c_str(), flags, NULL, NULL, sound.get()) != MA_SUCCESS) {
        return false;
    }

    // Apply enhanced audio properties
    ma_sound_set_volume(sound.get(), def->volume);
    ma_sound_set_looping(sound.get(), def->is_looping);
    
    // 3D Audio enhancements
    if (!def->is_2d) {
        ma_sound_set_min_distance(sound.get(), def->min_distance);
        ma_sound_set_max_distance(sound.get(), def->max_distance);
        ma_sound_set_rolloff(sound.get(), def->rolloff_factor);
        ma_sound_set_doppler_factor(sound.get(), def->doppler_factor);
    }
    
    // Apply pitch variation for realism
    if (def->pitch_variation > 0.0f) {
        std::uniform_real_distribution<float> pitch_dist(-def->pitch_variation, def->pitch_variation);
        float pitch = 1.0f + pitch_dist(m_rng);
        ma_sound_set_pitch(sound.get(), pitch);
    }
    
    // Apply environmental effects
    ApplyEnvironmentalEffects(sound.get(), def);
    
    ma_sound_start(sound.get());

    outHandle = m_nextHandle++;
    m_activeSounds[outHandle] = std::move(sound);
    
    // Add to spatial clustering system if it's a 3D sound
    if (m_spatial_clustering_enabled && m_spatial_cluster && !def->is_2d) {
        // Default position at origin since this PlayEvent doesn't take a position parameter
        // Position will be set later via SetEventPosition
        glm::vec3 default_position(0.0f);
        m_spatial_cluster->AddAudioSource(outHandle, default_position, def->volume, def->min_distance, def->max_distance);
    }
    
    return true;
}

bool MiniaudioManager::PlayOneShot2D(const AudioEventID& eventID) {
    if (!m_engine) return false;
    const AudioEventDefinition* def = GetEventDefinition(eventID);
    if (!def || def->files.empty()) return false;

    std::uniform_int_distribution<> dist(0, static_cast<int>(def->files.size()) - 1);
    const std::string& rel = def->files[dist(m_rng)];
    const std::string full_path = m_rootPath + rel;

    ma_engine_play_sound(m_engine.get(), full_path.c_str(), nullptr);
    return true;
}

bool MiniaudioManager::PlayOneShot(const AudioEventID& eventID, const glm::vec3& position) {
    if (!m_engine) return false;
    const AudioEventDefinition* def = GetEventDefinition(eventID);
    if (!def || def->files.empty()) return false;

    std::uniform_int_distribution<> dist(0, static_cast<int>(def->files.size()) - 1);
    const std::string& rel_path = def->files[dist(m_rng)];
    const std::string full_path = m_rootPath + rel_path;

    auto sound = std::make_unique<ma_sound>();
    uint32_t flags = MA_SOUND_FLAG_DECODE;

    if (ma_sound_init_from_file(m_engine.get(), full_path.c_str(), flags, NULL, NULL, sound.get()) != MA_SUCCESS) {
        return false;
    }

    // Set position and 3D properties
    ma_sound_set_position(sound.get(), position.x, position.y, position.z);
    ma_sound_set_volume(sound.get(), def->volume);
    ma_sound_set_min_distance(sound.get(), def->min_distance);
    ma_sound_set_max_distance(sound.get(), def->max_distance);
    ma_sound_set_rolloff(sound.get(), def->rolloff_factor);
    ma_sound_set_doppler_factor(sound.get(), def->doppler_factor);
    
    // Apply pitch variation
    if (def->pitch_variation > 0.0f) {
        std::uniform_real_distribution<float> pitch_dist(-def->pitch_variation, def->pitch_variation);
        float pitch = 1.0f + pitch_dist(m_rng);
        ma_sound_set_pitch(sound.get(), pitch);
    }
    
    ApplyEnvironmentalEffects(sound.get(), def);
    ma_sound_start(sound.get());

    // Keep the node alive until it finishes (the mixing thread is still reading it). Update()
    // reaps stopped one-shots. Parking it here instead of a local unique_ptr fixes a
    // use-after-free: returning would have freed the ma_sound mid-playback.
    m_oneShotSounds.push_back(std::move(sound));
    return true;
}

// --- MODIFIED FUNCTION ---
void MiniaudioManager::PlayMusic(const AudioEventID& musicEventID) {
    if (!m_engine || musicEventID == m_currentMusicID) return;

    StopMusic();

    const AudioEventDefinition* def = GetEventDefinition(musicEventID);
    if (!def || def->files.empty()) {
        LUMINUMBRA_CORE_ERROR("Music event not found or has no files: " + musicEventID);
        return;
    }

    const std::string full_path = m_rootPath + def->files[0];

    uint32_t flags = MA_SOUND_FLAG_STREAM;
    if (def->is_2d) {
        flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
    }

    m_currentMusic = std::make_unique<ma_sound>();
    ma_result result = ma_sound_init_from_file(m_engine.get(), full_path.c_str(), flags, NULL, NULL, m_currentMusic.get());
    
    if (result != MA_SUCCESS) {
        LUMINUMBRA_CORE_ERROR("Failed to init music with ma_sound_init_from_file for '" + full_path
                  + "'. Miniaudio result: " + ma_result_description(result)
                  + " (" + std::to_string(result) + ")");
        m_currentMusic.reset();
        return;
    }
    
    ma_sound_set_volume(m_currentMusic.get(), def->volume);
    ma_sound_set_looping(m_currentMusic.get(), def->is_looping);
    ma_sound_start(m_currentMusic.get());

    m_currentMusicID = musicEventID;
    LUMINUMBRA_CORE_INFO("Started music event '{}' from '{}'.", musicEventID, full_path);
}
// --- END MODIFICATION ---

void MiniaudioManager::StopMusic() {
    if (m_currentMusic) {
        ma_sound_stop(m_currentMusic.get());
        ma_sound_uninit(m_currentMusic.get());
        m_currentMusic.reset();
        m_currentMusicID.clear();
    }
}

bool MiniaudioManager::StopEvent(AudioEventHandle handle, bool immediate) {
    auto it = m_activeSounds.find(handle);
    if (it != m_activeSounds.end()) {
        ma_sound_stop(it->second.get());
        if (m_spatial_clustering_enabled && m_spatial_cluster) {
            m_spatial_cluster->RemoveAudioSource(handle);
        }
        if (immediate) {
            ma_sound_uninit(it->second.get());
            m_activeSounds.erase(it);
        }
        return true;
    }
    return false;
}

bool MiniaudioManager::SetEventPosition(AudioEventHandle handle, const glm::vec3& position) {
    auto it = m_activeSounds.find(handle);
    if (it != m_activeSounds.end()) {
        ma_sound_set_position(it->second.get(), position.x, position.y, position.z);
        
        // Update spatial clustering system
        if (m_spatial_clustering_enabled && m_spatial_cluster) {
            m_spatial_cluster->UpdateSourcePosition(handle, position);
        }
        
        return true;
    }
    return false;
}

void MiniaudioManager::SetMasterVolume(float volume) {
    if (m_engine) {
        ma_engine_set_volume(m_engine.get(), volume);
    }
}

bool MiniaudioManager::SetEventVolume(AudioEventHandle handle, float volume) {
    auto it = m_activeSounds.find(handle);
    if (it != m_activeSounds.end()) {
        ma_sound_set_volume(it->second.get(), volume);

        if (m_spatial_clustering_enabled && m_spatial_cluster) {
            m_spatial_cluster->UpdateSourceVolume(handle, volume);
        }

        return true;
    }
    return false;
}

bool MiniaudioManager::SetEventParameter(AudioEventHandle handle, const AudioParamID& paramID, float value) {
    auto it = m_activeSounds.find(handle);
    if (it == m_activeSounds.end()) {
        return false;
    }

    if (paramID == "volume") {
        ma_sound_set_volume(it->second.get(), value);
        if (m_spatial_clustering_enabled && m_spatial_cluster) {
            m_spatial_cluster->UpdateSourceVolume(handle, value);
        }
        return true;
    }

    if (paramID == "pitch") {
        ma_sound_set_pitch(it->second.get(), value);
        return true;
    }

    return false;
}

const AudioEventDefinition* MiniaudioManager::GetEventDefinition(const AudioEventID& eventID) {
    auto it = m_eventDefinitions.find(eventID);
    if (it == m_eventDefinitions.end()) {
        LUMINUMBRA_CORE_ERROR("Unknown event ID: " + eventID);
        return nullptr;
    }
    return &it->second;
}

// === ENHANCED AUDIO FEATURES ===

void MiniaudioManager::SetEnvironment(const AudioEnvironment& environment) {
    m_currentEnvironment = environment;
    
    // Initialize echo delay if not done
    if (!m_echoInitialized && m_engine) {
        ma_delay_config delayConfig = ma_delay_config_init(2, 48000, (ma_uint32)(environment.echo_delay * 48000), 0.3f);
        delayConfig.decay = environment.echo_decay;
        delayConfig.wet = 0.3f;
        delayConfig.dry = 0.7f;
        
        if (ma_delay_init(&delayConfig, nullptr, &m_echoDelay) == MA_SUCCESS) {
            m_echoInitialized = true;
            LUMINUMBRA_CORE_INFO("Audio environment set: Echo initialized");
        }
    }
    
    LUMINUMBRA_CORE_INFO("Audio environment changed to type: {}", static_cast<int>(environment.type));
}

void MiniaudioManager::SetWindParameters(const glm::vec3& direction, float strength) {
    m_windDirection = direction;
    m_windStrength = strength;
    
    // Create or update wind sound
    if (strength > 0.0f && !m_windSound && m_engine) {
        // You would need a wind sound file
        const std::string windPath = m_rootPath + "assets/audio/sfx/weather/wind_loop.ogg";
        m_windSound = std::make_unique<ma_sound>();
        
        if (ma_sound_init_from_file(m_engine.get(), windPath.c_str(), 
                                   MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION, 
                                   NULL, NULL, m_windSound.get()) == MA_SUCCESS) {
            ma_sound_set_looping(m_windSound.get(), true);
            ma_sound_set_volume(m_windSound.get(), strength * 0.6f);
            ma_sound_start(m_windSound.get());
        }
    } else if (m_windSound) {
        ma_sound_set_volume(m_windSound.get(), strength * 0.6f);
        if (strength <= 0.0f) {
            ma_sound_stop(m_windSound.get());
            ma_sound_uninit(m_windSound.get());
            m_windSound.reset();
        }
    }
}

void MiniaudioManager::UpdateAudioOcclusion(AudioEventHandle handle, float occlusion_factor) {
    auto it = m_activeSounds.find(handle);
    if (it != m_activeSounds.end()) {
        // Apply occlusion by reducing high frequencies and volume
        float occluded_volume = (1.0f - occlusion_factor * 0.7f);
        ma_sound_set_volume(it->second.get(), occluded_volume);
        
        // In a more advanced implementation, you would apply low-pass filtering
        // This is a simplified approach
    }
}

void MiniaudioManager::SetGlobalReverb(float wet, float dry, float decay) {
    // This would require custom reverb implementation
    // miniaudio doesn't have built-in reverb, but you could implement it
    LUMINUMBRA_CORE_INFO("Global reverb set - Wet: {}, Dry: {}, Decay: {}", wet, dry, decay);
}

void MiniaudioManager::PlayAmbientLoop(const AudioEventID& eventID, const glm::vec3& position, float radius) {
    if (!m_engine) return;
    
    // Stop existing ambient if playing
    StopAmbientLoop(eventID);
    
    const AudioEventDefinition* def = GetEventDefinition(eventID);
    if (!def || def->files.empty()) return;
    
    auto sound = std::make_unique<ma_sound>();
    const std::string full_path = m_rootPath + def->files[0];
    
    // Decode the (short) loop fully into memory instead of streaming: ogg streaming can fail
    // silently, and a pre-decoded buffer loops seamlessly. Log failure so a missing/!decodable
    // ambient file is diagnosable rather than silent.
    const ma_result amb_rc = ma_sound_init_from_file(m_engine.get(), full_path.c_str(),
                                                     MA_SOUND_FLAG_DECODE, NULL, NULL, sound.get());
    if (amb_rc != MA_SUCCESS) {
        LUMINUMBRA_CORE_WARN("PlayAmbientLoop: failed to load '{}' (ma_result {})", full_path, static_cast<int>(amb_rc));
    }
    if (amb_rc == MA_SUCCESS) {
        ma_sound_set_position(sound.get(), position.x, position.y, position.z);
        ma_sound_set_volume(sound.get(), def->volume * m_currentEnvironment.ambient_volume);
        ma_sound_set_looping(sound.get(), true);
        ma_sound_set_min_distance(sound.get(), radius * 0.3f);
        ma_sound_set_max_distance(sound.get(), radius);
        ma_sound_start(sound.get());
        
        m_ambientSounds[eventID] = std::move(sound);
        LUMINUMBRA_CORE_INFO("Started ambient loop: {}", eventID);
    }
}

void MiniaudioManager::StopAmbientLoop(const AudioEventID& eventID) {
    auto it = m_ambientSounds.find(eventID);
    if (it != m_ambientSounds.end()) {
        ma_sound_stop(it->second.get());
        ma_sound_uninit(it->second.get());
        m_ambientSounds.erase(it);
    }
}

void MiniaudioManager::SetAmbientVolume(const AudioEventID& eventID, float scale) {
    auto it = m_ambientSounds.find(eventID);
    if (it == m_ambientSounds.end()) return;  // that bed isn't playing -> nothing to scale
    auto dit = m_eventDefinitions.find(eventID);
    const float base = (dit != m_eventDefinitions.end()) ? dit->second.volume : 1.0f;
    if (scale < 0.0f) scale = 0.0f;
    ma_sound_set_volume(it->second.get(), base * scale * m_currentEnvironment.ambient_volume);
}

void MiniaudioManager::ApplyEnvironmentalEffects(ma_sound* sound, const AudioEventDefinition* def) {
    if (!sound || !def) return;
    
    // Apply environment-based volume adjustments
    float env_volume_multiplier = 1.0f;
    
    switch (m_currentEnvironment.type) {
        case AudioEnvironmentType::Cave:
            env_volume_multiplier = 1.2f; // Caves amplify sound
            break;
        case AudioEnvironmentType::Outdoor:
            env_volume_multiplier = 0.9f; // Outdoors sounds travel less
            break;
        case AudioEnvironmentType::Forest:
            env_volume_multiplier = 0.8f; // Trees absorb sound
            break;
        case AudioEnvironmentType::Water:
            env_volume_multiplier = 0.6f; // Water muffles sound
            break;
    }
    
    float current_volume = ma_sound_get_volume(sound);
    ma_sound_set_volume(sound, current_volume * env_volume_multiplier);
    
    // Apply reverb if enabled for the event
    if (def->use_reverb && def->reverb_level > 0.0f) {
        // Custom reverb would go here
        // For now, just log that reverb should be applied
        LUMINUMBRA_CORE_INFO("Reverb applied - Level: {}, Type: {}", def->reverb_level, def->reverb_type);
    }
}

void MiniaudioManager::UpdateWindEffect() {
    // Update wind-based environmental effects
    if (m_windStrength > 0.1f) {
        // Wind could affect all 3D sounds by adding subtle position jitter
        for (auto& [handle, sound] : m_activeSounds) {
            // Add wind-based sound variation
            float wind_variation = m_windStrength * 0.1f;
            std::uniform_real_distribution<float> wind_dist(-wind_variation, wind_variation);
            
            // Slightly randomize pitch to simulate wind effect
            float current_pitch = ma_sound_get_pitch(sound.get());
            float wind_pitch = current_pitch + wind_dist(m_rng) * 0.05f;
            ma_sound_set_pitch(sound.get(), std::clamp(wind_pitch, 0.8f, 1.2f));
        }
    }
}

float MiniaudioManager::CalculateOcclusion(const glm::vec3& source, const glm::vec3& listener) {
    // This would require integration with the physics/world system
    // to perform line-of-sight checks and calculate occlusion
    
    float distance = glm::distance(source, listener);
    
    // Simple distance-based occlusion approximation
    if (distance > 50.0f) {
        return std::min(0.8f, (distance - 50.0f) / 100.0f);
    }
    
    return 0.0f;
}

void MiniaudioManager::SetPhysicsSystem(::Luminumbra::Systems::PhysicsSystem* physics_system) {
    if (m_spatial_cluster) {
        m_spatial_cluster->SetPhysicsSystem(physics_system);
        LUMINUMBRA_CORE_INFO("Physics system integration established for audio spatial clustering");
    }
}

} // namespace Luminumbra::Client
