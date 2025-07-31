#include "audio/MiniaudioManager.h"
#include <iostream>
#include <fstream>
#include <random>

namespace Luminumbra::Client {

MiniaudioManager::MiniaudioManager() : m_engine(nullptr), m_rng(std::random_device{}()) {}
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

    // Garbage collect any sounds that have finished playing
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
        // FIX: Explicitly uninitialize all active sounds before clearing the map.
        for (auto& [handle, sound_ptr] : m_activeSounds) {
            ma_sound_uninit(sound_ptr.get());
        }
        m_activeSounds.clear(); // Now this is safe.

        m_eventDefinitions.clear();
        ma_engine_uninit(m_engine.get());
        m_engine = nullptr;
        std::cout << "Miniaudio Manager Shutdown." << std::endl;
    }
}

bool MiniaudioManager::LoadBank(const std::string& bankPath) {
    std::ifstream f(bankPath);
    if (!f.is_open()) {
        std::cerr << "AUDIO ERROR: Failed to open sound bank: " << bankPath << std::endl;
        return false;
    }
    nlohmann::json bank_json;
    try {
        bank_json = nlohmann::json::parse(f);
    } catch (nlohmann::json::parse_error& e) {
        std::cerr << "AUDIO ERROR: Failed to parse sound bank JSON: " << bankPath << " - " << e.what() << std::endl;
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
        
        m_eventDefinitions[event_id] = def;
    }

    std::cout << "Loaded sound bank: " << bankPath << std::endl;
    return true;
}

void MiniaudioManager::UnloadBank(const std::string& bankPath) {
    // For simplicity, we're not unloading banks right now.
    // A real implementation would need to track which events belong to which bank.
    std::cout << "AUDIO WARNING: Unloading banks is not fully implemented." << std::endl;
}

void MiniaudioManager::SetListenerTransform(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
    if (!m_engine) return;
    ma_engine_listener_set_position(m_engine.get(), 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(m_engine.get(), 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_up(m_engine.get(), 0, up.x, up.y, up.z);
}

bool MiniaudioManager::PlayEvent(const AudioEventID& eventID, AudioEventHandle& outHandle) {
    if (!m_engine) return false;

    const AudioEventDefinition* def = GetEventDefinition(eventID);
    if (!def) return false;

    auto sound = std::make_unique<ma_sound>();
    uint32_t flags = MA_SOUND_FLAG_NO_PITCH;
    if (def->is_2d) {
        flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
    }

    std::uniform_int_distribution<> dist(0, def->files.size() - 1);
    const std::string& file_path = def->files[dist(m_rng)];

    if (LoadSoundResource(file_path, sound.get(), flags) != MA_SUCCESS) {
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
    if (!def) return false;
    
    std::uniform_int_distribution<> dist(0, def->files.size() - 1);
    const std::string& file_path = def->files[dist(m_rng)];
    
    ma_engine_play_sound(m_engine.get(), file_path.c_str(), NULL);
    return true;
}

bool MiniaudioManager::PlayOneShot(const AudioEventID& eventID, const glm::vec3& position) {
    // Not implemented yet, requires creating a temporary sound
    return false;
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
    // not implemented yet
    return false;
}

bool MiniaudioManager::SetEventParameter(AudioEventHandle handle, const AudioParamID& paramID, float value) {
    // not implemented yet, would map to miniaudio effects
    return false;
}

ma_result MiniaudioManager::LoadSoundResource(const std::string& path, ma_sound* sound, uint32_t flags) {
    return ma_sound_init_from_file(m_engine.get(), path.c_str(), flags, NULL, NULL, sound);
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