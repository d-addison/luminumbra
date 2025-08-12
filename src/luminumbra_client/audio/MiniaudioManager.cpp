#include "audio/MiniaudioManager.h"
#include <iostream>
#include <fstream>
#include <random>

namespace Luminumbra::Client {

MiniaudioManager::MiniaudioManager(const std::string& root_path) : m_rootPath(root_path), m_engine(nullptr), m_rng(std::random_device{}()) {}
MiniaudioManager::~MiniaudioManager() {
    // Shutdown should be called explicitly
}

bool MiniaudioManager::Init() {
    m_engine = std::make_unique<ma_engine>();
    ma_result result = ma_engine_init(NULL, m_engine.get());
    if (result != MA_SUCCESS) {
        std::cerr << "AUDIO ERROR: Failed to initialize miniaudio engine." << std::endl;
        m_engine = nullptr;
        return false;
    }
    std::cout << "Miniaudio Manager Initialized." << std::endl;
    return true;
}

void MiniaudioManager::Update() {
    if (!m_engine) return;

    for (auto it = m_activeSounds.begin(); it != m_activeSounds.end(); ) {
        if (!ma_sound_is_playing(it->second.get())) {
            ma_sound_uninit(it->second.get());
            it = m_activeSounds.erase(it);
        } else {
            ++it;
        }
    }
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
        ma_engine_uninit(m_engine.get());
        m_engine = nullptr;
        std::cout << "Miniaudio Manager Shutdown." << std::endl;
    }
}

bool MiniaudioManager::LoadBank(const std::string& bankPath) {
    const std::string full_path = m_rootPath + bankPath;
    std::ifstream f(full_path);
    if (!f.is_open()) {
        std::cerr << "AUDIO ERROR: Failed to open sound bank: " << full_path << std::endl;
        return false;
    }
    
    nlohmann::json bank_json;
    try {
        bank_json = nlohmann::json::parse(f);
    } catch (nlohmann::json::parse_error& e) {
        std::cerr << "AUDIO ERROR: Failed to parse sound bank JSON: " << full_path << " - " << e.what() << std::endl;
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
        
        m_eventDefinitions[event_id] = def;
    }

    std::cout << "Loaded sound bank: " << bankPath << std::endl;
    return true;
}

void MiniaudioManager::UnloadBank(const std::string& bankPath) {
    std::cout << "AUDIO WARNING: Unloading banks is not fully implemented." << std::endl;
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

    uint32_t flags = MA_SOUND_FLAG_DECODE; // Let's decode one-shots for performance
    if (def->is_2d) flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

    if (ma_sound_init_from_file(m_engine.get(), full_path.c_str(), flags, NULL, NULL, sound.get()) != MA_SUCCESS) {
        return false;
    }

    ma_sound_set_volume(sound.get(), def->volume);
    ma_sound_set_looping(sound.get(), def->is_looping);
    ma_sound_start(sound.get());

    outHandle = m_nextHandle++;
    m_activeSounds[outHandle] = std::move(sound);
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
    return false;
}

// --- MODIFIED FUNCTION ---
void MiniaudioManager::PlayMusic(const AudioEventID& musicEventID) {
    if (!m_engine || musicEventID == m_currentMusicID) return;

    StopMusic();

    const AudioEventDefinition* def = GetEventDefinition(musicEventID);
    if (!def || def->files.empty()) {
        std::cerr << "AUDIO ERROR: Music event not found or has no files: " << musicEventID << std::endl;
        return;
    }

    const std::string full_path = m_rootPath + def->files[0];

    ma_sound_config soundConfig = ma_sound_config_init();
    soundConfig.pFilePath = full_path.c_str();
    // Reverted back to STREAM as this is correct for music.
    soundConfig.flags = MA_SOUND_FLAG_STREAM; 
    if (def->is_2d) {
        soundConfig.flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
    }
    soundConfig.isLooping = def->is_looping;

    m_currentMusic = std::make_unique<ma_sound>();
    ma_result result = ma_sound_init_ex(m_engine.get(), &soundConfig, m_currentMusic.get());
    
    if (result != MA_SUCCESS) {
        std::cerr << "AUDIO ERROR: Failed to init music with ma_sound_init_ex for '" << full_path
                  << "'. Miniaudio result: " << ma_result_description(result)
                  << " (" << result << ")" << std::endl;
        m_currentMusic.reset();
        return;
    }
    
    ma_sound_set_volume(m_currentMusic.get(), def->volume);
    ma_sound_start(m_currentMusic.get());

    m_currentMusicID = musicEventID;
    std::cout << "Music started: " << musicEventID << std::endl;
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
        return true;
    }
    return false;
}

bool MiniaudioManager::SetEventPosition(AudioEventHandle handle, const glm::vec3& position) {
    auto it = m_activeSounds.find(handle);
    if (it != m_activeSounds.end()) {
        ma_sound_set_position(it->second.get(), position.x, position.y, position.z);
        return true;
    }
    return false;
}

bool MiniaudioManager::SetEventVolume(AudioEventHandle handle, float volume) {
    return false;
}

bool MiniaudioManager::SetEventParameter(AudioEventHandle handle, const AudioParamID& paramID, float value) {
    return false;
}

const AudioEventDefinition* MiniaudioManager::GetEventDefinition(const AudioEventID& eventID) {
    auto it = m_eventDefinitions.find(eventID);
    if (it == m_eventDefinitions.end()) {
        std::cerr << "AUDIO ERROR: Unknown event ID: " << eventID << std::endl;
        return nullptr;
    }
    return &it->second;
}

} // namespace Luminumbra::Client