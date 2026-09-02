#include "audio/MiniaudioManager.h"
#include "../../luminumbra_common/systems/PhysicsSystem.h"
#include "AudioSpatialCluster.h"
#include "audio/EnvironmentalAudioModel.h" // pure reverb-proxy param mapping
#include "core/Log.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>

namespace Luminumbra::Client {

MiniaudioManager::MiniaudioManager(const std::string& root_path)
    : m_rootPath(root_path)
    , m_engine(nullptr)
    , m_rng(std::random_device{}()) {
    // Initialize spatial clustering system
    m_spatial_cluster = std::make_unique<AudioSpatialCluster>();
}
MiniaudioManager::~MiniaudioManager() {
    // Shutdown should be called explicitly
}

bool MiniaudioManager::Init() {
    m_engine = std::make_unique<ma_engine>();
    ma_result result = ma_engine_init(nullptr, m_engine.get());
    if (result != MA_SUCCESS) {
        LUMINUMBRA_CORE_ERROR("Failed to initialize miniaudio engine.");
        m_engine = nullptr;
        return false;
    }

    // build the mix-bus group tree. sfx attaches to the engine
    // endpoint; ambient/events/ui attach UNDER sfx so user.audio_sfx scales every
    // non-music sound. All groups boot at volume 1.0 => the mix is byte-identical
    // to the pre-bus graph until a bus volume moves. A group-init failure is
    // non-fatal: GroupFor then returns nullptr and playback attaches straight
    // to the endpoint exactly as before.
    auto init_group =
        [this](std::unique_ptr<ma_sound_group>& group, ma_sound_group* parent, const char* name) {
            group = std::make_unique<ma_sound_group>();
            if (ma_sound_group_init(m_engine.get(), 0, parent, group.get()) != MA_SUCCESS) {
                LUMINUMBRA_CORE_WARN("Failed to init '{}' sound group; routing to endpoint.", name);
                group.reset();
            }
        };
    init_group(m_sfxGroup, nullptr, "sfx");
    init_group(m_ambientGroup, m_sfxGroup.get(), "ambient");
    init_group(m_eventsGroup, m_sfxGroup.get(), "events");
    init_group(m_uiGroup, m_sfxGroup.get(), "ui");

    m_ducker.Reset();
    m_hasLastUpdateTime = false;
    m_lastAppliedAmbientGain = -1.0f;
    m_lastAppliedMusicDuckGain = -1.0f;

    LUMINUMBRA_CORE_INFO("Miniaudio Manager Initialized.");
    return true;
}

ma_sound_group* MiniaudioManager::GroupFor(BusId bus) const {
    switch (bus) {
        case BusId::Sfx:
            return m_sfxGroup.get();
        case BusId::Ambient:
            return m_ambientGroup.get();
        case BusId::Events:
            return m_eventsGroup.get();
        case BusId::Ui:
            return m_uiGroup.get();
        // Master/Music playback never attaches to an sfx group; endpoint routing.
        case BusId::Master:
        case BusId::Music:
        default:
            return nullptr;
    }
}

void MiniaudioManager::ApplyAmbientBusGain() {
    if (!m_ambientGroup)
        return;
    const float gain = m_ambientVolume * m_ducker.AmbientGain();
    if (gain != m_lastAppliedAmbientGain) {
        ma_sound_group_set_volume(m_ambientGroup.get(), gain);
        m_lastAppliedAmbientGain = gain;
    }
}

void MiniaudioManager::Update() {
    if (!m_engine)
        return;

    // Update spatial audio clustering system
    if (m_spatial_clustering_enabled && m_spatial_cluster) {
        // Get listener position from miniaudio engine
        ma_vec3f ma_listener_pos = ma_engine_listener_get_position(m_engine.get(), 0);
        glm::vec3 listener_pos(ma_listener_pos.x, ma_listener_pos.y, ma_listener_pos.z);

        // Update spatial clustering (this handles batched occlusion calculations)
        m_spatial_cluster->Update(listener_pos, 1.0f / 60.0f); // Assume 60 FPS for delta time
    }

    for (auto it = m_activeSounds.begin(); it != m_activeSounds.end();) {
        if (!ma_sound_is_playing(it->second.get())) {
            // Remove from spatial clustering system
            if (m_spatial_clustering_enabled && m_spatial_cluster) {
                m_spatial_cluster->RemoveAudioSource(it->first);
            }

            m_soundBaseVolume.erase(it->first); // drop the occlusion base
            ma_sound_uninit(it->second.get());
            it = m_activeSounds.erase(it);
        } else {
            ++it;
        }
    }

    //  read-back: make the occlusion the spatial cluster computed AUDIBLE.
    // For every active 3D voice we captured a base volume for, ask the cluster how
    // blocked its source->listener path is (physics raycast through world geometry;
    // 0 when there is no physics system or the line of sight is clear) and fold
    // that into the voice's volume via the pure ComputeFinalVolume helper. base is
    // the authored, pre-occlusion gain, so re-deriving from it every frame is
    // idempotent (never compounds). busGain/spatialAttenuation = 1: the bus group
    // node already carries the user SFX gain and miniaudio's spatializer already
    // applies the distance falloff, so passing 1 here avoids double-applying them.
    // occlusion 0 => volume == base => byte-identical to the pre- mix.
    if (m_spatial_clustering_enabled && m_spatial_cluster && !m_soundBaseVolume.empty()) {
        const ma_vec3f ma_listener = ma_engine_listener_get_position(m_engine.get(), 0);
        const glm::vec3 listener_pos(ma_listener.x, ma_listener.y, ma_listener.z);
        for (const auto& [handle, base] : m_soundBaseVolume) {
            auto sit = m_activeSounds.find(handle);
            if (sit == m_activeSounds.end())
                continue; // reaped/stopped this frame
            const ma_vec3f ma_src = ma_sound_get_position(sit->second.get());
            const glm::vec3 source_pos(ma_src.x, ma_src.y, ma_src.z);
            const float occlusion = m_spatial_cluster->QueryOcclusion(source_pos, listener_pos);
            const float volume = ComputeFinalVolume(base, 1.0f, 1.0f, occlusion);
            // Skip redundant identical writes (the common occlusion==0 steady state).
            if (ma_sound_get_volume(sit->second.get()) != volume) {
                ma_sound_set_volume(sit->second.get(), volume);
            }
        }
    }

    // Reap fire-and-forget one-shots (PlayOneShot / PlayOneShot2D) that have finished
    // playing. An Events-bus voice ending releases the sidechain duck (ref-counted).
    for (auto it = m_oneShotSounds.begin(); it != m_oneShotSounds.end();) {
        if (!ma_sound_is_playing(it->sound.get())) {
            if (it->bus == BusId::Events) {
                m_ducker.OnEventEnd();
            }
            ma_sound_uninit(it->sound.get());
            it = m_oneShotSounds.erase(it);
        } else {
            ++it;
        }
    }

    //  sidechain ducking: advance the envelope with WALL-CLOCK dt (client
    // audio presentation only — never sim time) and fold the duck gain into the
    // ambient bus (and the music bed, only when a music floor < 1 is configured).
    {
        const auto now = std::chrono::steady_clock::now();
        float dt = 0.0f;
        if (m_hasLastUpdateTime) {
            dt = std::chrono::duration<float>(now - m_lastUpdateTime).count();
            // Clamp a hitch/debugger pause so the envelope just saturates sanely.
            if (dt > 0.25f)
                dt = 0.25f;
            if (dt < 0.0f)
                dt = 0.0f;
        }
        m_lastUpdateTime = now;
        m_hasLastUpdateTime = true;

        m_ducker.Advance(dt);
        ApplyAmbientBusGain();

        // Optional music duck (DuckParams::music_floor_gain < 1). Off by default:
        // MusicGain is exactly 1.0 then, and the first pass through here caches
        // it, so the music path is never re-scaled unless ducking music is enabled.
        const float music_duck = m_ducker.MusicGain();
        if (music_duck != m_lastAppliedMusicDuckGain) {
            m_lastAppliedMusicDuckGain = music_duck;
            if (m_currentMusic && m_ducker.params().music_floor_gain < 1.0f) {
                const AudioEventDefinition* def = nullptr;
                auto dit = m_eventDefinitions.find(m_currentMusicID);
                if (dit != m_eventDefinitions.end())
                    def = &dit->second;
                ma_sound_set_volume(m_currentMusic.get(),
                                    (def ? def->volume : 1.0f) * m_musicVolume * music_duck);
            }
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
        for (auto& voice : m_oneShotSounds) {
            ma_sound_uninit(voice.sound.get());
        }
        m_oneShotSounds.clear();
        for (auto& [id, sound_ptr] : m_ambientSounds) {
            ma_sound_uninit(sound_ptr.get());
        }
        m_ambientSounds.clear();
        if (m_windSound) {
            ma_sound_uninit(m_windSound.get());
            m_windSound.reset();
        }
        // Bus groups: uninit AFTER every attached sound, BEFORE the engine.
        auto uninit_group = [](std::unique_ptr<ma_sound_group>& group) {
            if (group) {
                ma_sound_group_uninit(group.get());
                group.reset();
            }
        };
        uninit_group(m_ambientGroup);
        uninit_group(m_eventsGroup);
        uninit_group(m_uiGroup);
        uninit_group(m_sfxGroup);
        // the reverb-proxy delay node dies AFTER the groups (its only
        // input was the ambient group, already detached above), BEFORE the engine.
        if (m_reverbNodeInitialized) {
            ma_delay_node_uninit(&m_reverbNode, nullptr);
            m_reverbNodeInitialized = false;
        }
        m_ducker.Reset();
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

        m_eventDefinitions[event_id] = def;
    }

    LUMINUMBRA_CORE_INFO("Loaded sound bank: " + std::to_string(m_eventDefinitions.size()) +
                         " events from " + full_path);
    return true;
}

void MiniaudioManager::UnloadBank(const std::string& bankPath) {
    LUMINUMBRA_CORE_WARN("AUDIO WARNING: Unloading banks is not fully implemented.");
}

void MiniaudioManager::SetListenerTransform(const glm::vec3& position,
                                            const glm::vec3& forward,
                                            const glm::vec3& up) {
    if (!m_engine)
        return;
    ma_engine_listener_set_position(m_engine.get(), 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(m_engine.get(), 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(m_engine.get(), 0, up.x, up.y, up.z);
}

bool MiniaudioManager::PlayEvent(const AudioEventID& eventID, AudioEventHandle& outHandle) {
    if (!m_engine)
        return false;
    const AudioEventDefinition* def = GetEventDefinition(eventID);
    if (!def || def->files.empty())
        return false;

    auto sound = std::make_unique<ma_sound>();

    std::uniform_int_distribution<> dist(0, static_cast<int>(def->files.size()) - 1);
    const std::string& rel_path = def->files[dist(m_rng)];
    const std::string full_path = m_rootPath + rel_path;

    uint32_t flags = MA_SOUND_FLAG_DECODE;
    if (def->is_2d)
        flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

    // Handle-based events are gameplay SFX: attach to the sfx bus.
    if (ma_sound_init_from_file(
            m_engine.get(), full_path.c_str(), flags, GroupFor(BusId::Sfx), nullptr, sound.get()) !=
        MA_SUCCESS) {
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
        std::uniform_real_distribution<float> pitch_dist(-def->pitch_variation,
                                                         def->pitch_variation);
        float pitch = 1.0f + pitch_dist(m_rng);
        ma_sound_set_pitch(sound.get(), pitch);
    }

    // Apply environmental effects
    ApplyEnvironmentalEffects(sound.get());

    ma_sound_start(sound.get());

    outHandle = m_nextHandle++;
    m_activeSounds[outHandle] = std::move(sound);

    // Add to spatial clustering system if it's a 3D sound
    if (m_spatial_clustering_enabled && m_spatial_cluster && !def->is_2d) {
        // Default position at origin since this PlayEvent doesn't take a position parameter
        // Position will be set later via SetEventPosition
        glm::vec3 default_position(0.0f);
        m_spatial_cluster->AddAudioSource(
            outHandle, default_position, def->volume, def->min_distance, def->max_distance);
    }

    // remember the authored (post env-mult) volume for 3D voices so
    // Update's occlusion read-back can re-derive their live volume without
    // compounding. Tracked independently of m_spatial_clustering_enabled so the
    // base survives a later EnableSpatialClustering(true). 2D voices are never
    // occluded and are intentionally not tracked.
    if (!def->is_2d) {
        m_soundBaseVolume[outHandle] = ma_sound_get_volume(m_activeSounds[outHandle].get());
    }

    return true;
}

bool MiniaudioManager::PlayOneShot2D(const AudioEventID& eventID, BusId bus) {
    if (!m_engine)
        return false;
    const AudioEventDefinition* def = GetEventDefinition(eventID);
    if (!def || def->files.empty())
        return false;

    std::uniform_int_distribution<> dist(0, static_cast<int>(def->files.size()) - 1);
    const std::string& rel = def->files[dist(m_rng)];
    const std::string full_path = m_rootPath + rel;

    // routed as a MANAGED one-shot instead of the old
    // ma_engine_play_sound fire-and-forget so (a) the voice attaches to its bus
    // group and (b) an Events-bus voice's end can release the sidechain duck.
    // NO_PITCH + NO_SPATIALIZATION + volume left at the default 1.0 match the
    // inline-sound semantics ma_engine_play_sound used (bank volume was never
    // applied on this path), so the audible result is unchanged.
    auto sound = std::make_unique<ma_sound>();
    const uint32_t flags =
        MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_file(
            m_engine.get(), full_path.c_str(), flags, GroupFor(bus), nullptr, sound.get()) !=
        MA_SUCCESS) {
        return false;
    }
    ma_sound_start(sound.get());

    if (bus == BusId::Events) {
        m_ducker.OnEventStart(); // released when Update() reaps the finished voice
    }
    m_oneShotSounds.push_back(OneShotVoice{std::move(sound), bus});
    return true;
}

bool MiniaudioManager::PlayOneShot(const AudioEventID& eventID,
                                   const glm::vec3& position,
                                   BusId bus) {
    if (!m_engine)
        return false;
    const AudioEventDefinition* def = GetEventDefinition(eventID);
    if (!def || def->files.empty())
        return false;

    std::uniform_int_distribution<> dist(0, static_cast<int>(def->files.size()) - 1);
    const std::string& rel_path = def->files[dist(m_rng)];
    const std::string full_path = m_rootPath + rel_path;

    auto sound = std::make_unique<ma_sound>();
    uint32_t flags = MA_SOUND_FLAG_DECODE;

    // spatial one-shots attach to their routed bus group
    // (default sfx; Events voices sidechain-duck the ambient bus).
    if (ma_sound_init_from_file(
            m_engine.get(), full_path.c_str(), flags, GroupFor(bus), nullptr, sound.get()) !=
        MA_SUCCESS) {
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
        std::uniform_real_distribution<float> pitch_dist(-def->pitch_variation,
                                                         def->pitch_variation);
        float pitch = 1.0f + pitch_dist(m_rng);
        ma_sound_set_pitch(sound.get(), pitch);
    }

    ApplyEnvironmentalEffects(sound.get());
    ma_sound_start(sound.get());

    if (bus == BusId::Events) {
        m_ducker.OnEventStart(); // released when Update() reaps the finished voice
    }

    // Keep the node alive until it finishes (the mixing thread is still reading it). Update
    // reaps stopped one-shots. Parking it here instead of a local unique_ptr fixes a
    // use-after-free: returning would have freed the ma_sound mid-playback.
    m_oneShotSounds.push_back(OneShotVoice{std::move(sound), bus});
    return true;
}

// --- MODIFIED FUNCTION ---
void MiniaudioManager::PlayMusic(const AudioEventID& musicEventID) {
    if (!m_engine || musicEventID == m_currentMusicID)
        return;

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
    ma_result result = ma_sound_init_from_file(
        m_engine.get(), full_path.c_str(), flags, nullptr, nullptr, m_currentMusic.get());

    if (result != MA_SUCCESS) {
        LUMINUMBRA_CORE_ERROR("Failed to init music with ma_sound_init_from_file for '" +
                              full_path + "'. Miniaudio result: " + ma_result_description(result) +
                              " (" + std::to_string(result) + ")");
        m_currentMusic.reset();
        return;
    }

    ma_sound_set_volume(m_currentMusic.get(), def->volume * m_musicVolume);
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
            m_soundBaseVolume.erase(handle); // drop the occlusion base
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

void MiniaudioManager::SetMusicVolume(float volume) {
    m_musicVolume = volume < 0.0f ? 0.0f : volume;
    // Re-scale the track that's already playing so the slider is felt immediately.
    if (m_currentMusic) {
        const AudioEventDefinition* def = nullptr;
        auto it = m_eventDefinitions.find(m_currentMusicID);
        if (it != m_eventDefinitions.end())
            def = &it->second;
        ma_sound_set_volume(m_currentMusic.get(), (def ? def->volume : 1.0f) * m_musicVolume);
    }
}

void MiniaudioManager::SetSfxVolume(float volume) {
    if (volume < 0.0f)
        volume = 0.0f;
    else if (volume > 1.0f)
        volume = 1.0f;
    m_sfxVolume = volume;
    // Group volume applies live to every playing + future non-music sound
    // (ambient/events/ui are children of this group)..
    if (m_sfxGroup) {
        ma_sound_group_set_volume(m_sfxGroup.get(), m_sfxVolume);
    }
}

void MiniaudioManager::SetBusVolume(BusId bus, float volume) {
    if (volume < 0.0f)
        volume = 0.0f;
    else if (volume > 1.0f)
        volume = 1.0f;
    switch (bus) {
        case BusId::Master:
            SetMasterVolume(volume);
            break;
        case BusId::Music:
            SetMusicVolume(volume);
            break;
        case BusId::Sfx:
            SetSfxVolume(volume);
            break;
        case BusId::Ambient:
            m_ambientVolume = volume;
            // Composed with the duck gain (never write the raw volume here or a
            // mid-duck slider move would pop the bed back to full volume).
            ApplyAmbientBusGain();
            break;
        case BusId::Events:
            m_eventsVolume = volume;
            if (m_eventsGroup)
                ma_sound_group_set_volume(m_eventsGroup.get(), volume);
            break;
        case BusId::Ui:
            m_uiVolume = volume;
            if (m_uiGroup)
                ma_sound_group_set_volume(m_uiGroup.get(), volume);
            break;
    }
}

bool MiniaudioManager::SetEventVolume(AudioEventHandle handle, float volume) {
    auto it = m_activeSounds.find(handle);
    if (it != m_activeSounds.end()) {
        ma_sound_set_volume(it->second.get(), volume);

        if (m_spatial_clustering_enabled && m_spatial_cluster) {
            m_spatial_cluster->UpdateSourceVolume(handle, volume);
        }

        // rebase the occlusion read-back on the caller's new volume (only
        // for tracked 3D voices) so Update re-occludes THIS value next frame
        // instead of clobbering it back to the old base.
        auto bit = m_soundBaseVolume.find(handle);
        if (bit != m_soundBaseVolume.end())
            bit->second = volume;

        return true;
    }
    return false;
}

bool MiniaudioManager::SetEventParameter(AudioEventHandle handle,
                                         const AudioParamID& paramID,
                                         float value) {
    auto it = m_activeSounds.find(handle);
    if (it == m_activeSounds.end()) {
        return false;
    }

    if (paramID == "volume") {
        ma_sound_set_volume(it->second.get(), value);
        if (m_spatial_clustering_enabled && m_spatial_cluster) {
            m_spatial_cluster->UpdateSourceVolume(handle, value);
        }
        // rebase the occlusion read-back on the caller's new volume so
        // Update re-occludes it rather than clobbering it (tracked 3D voices only).
        auto bit = m_soundBaseVolume.find(handle);
        if (bit != m_soundBaseVolume.end())
            bit->second = value;
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
        ma_delay_config delayConfig =
            ma_delay_config_init(2, 48000, (ma_uint32)(environment.echo_delay * 48000), 0.3f);
        delayConfig.decay = environment.echo_decay;
        delayConfig.wet = 0.3f;
        delayConfig.dry = 0.7f;

        if (ma_delay_init(&delayConfig, nullptr, &m_echoDelay) == MA_SUCCESS) {
            m_echoInitialized = true;
            LUMINUMBRA_CORE_INFO("Audio environment set: Echo initialized");
        }
    }

    LUMINUMBRA_CORE_INFO("Audio environment changed to type: {}",
                         static_cast<int>(environment.type));
}

void MiniaudioManager::SetWindParameters(const glm::vec3& direction, float strength) {
    m_windDirection = direction;
    m_windStrength = strength;

    // Create or update wind sound
    if (strength > 0.0f && !m_windSound && m_engine) {
        // You would need a wind sound file
        const std::string windPath = m_rootPath + "assets/audio/sfx/weather/wind_loop.ogg";
        m_windSound = std::make_unique<ma_sound>();

        // The wind bed is a looping ambience: route through the ambient bus.
        if (ma_sound_init_from_file(m_engine.get(),
                                    windPath.c_str(),
                                    MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                    GroupFor(BusId::Ambient),
                                    nullptr,
                                    m_windSound.get()) == MA_SUCCESS) {
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
    // the biome/weather reverb is REAL now. miniaudio
    // 0.11.22 (vendor FetchContent pin) has no built-in reverb DSP, so this is
    // an HONEST PROXY: one feedback delay line (ma_delay_node) spliced between
    // the ambient bus group and its parent. A short slap-back with feedback
    // reads as early reflections + tail — convincing for canyon/cave/wet-air
    // ambience, but NOT a true diffuse reverb (no allpass diffusion, and the
    // delay TIME is fixed at init because ma_delay allocates its buffer then).
    // wet/dry drive the node's mix; decay (pseudo-RT60 seconds from biomes.json
    // / the weather shift) maps to a stability-capped feedback gain via the
    // pure model (AudioModel::ReverbProxyFromParams — unit-tested CPU-only).
    if (m_engine && !m_reverbNodeInitialized && m_ambientGroup) {
        const ma_uint32 channels = ma_engine_get_channels(m_engine.get());
        const ma_uint32 sampleRate = ma_engine_get_sample_rate(m_engine.get());
        const ma_uint32 delayFrames = static_cast<ma_uint32>(
            (AudioModel::kReverbProxyDelayMs / 1000.0f) * static_cast<float>(sampleRate));
        ma_delay_node_config cfg = ma_delay_node_config_init(
            channels, sampleRate, delayFrames > 0 ? delayFrames : 1, 0.0f);
        if (ma_delay_node_init(
                ma_engine_get_node_graph(m_engine.get()), &cfg, nullptr, &m_reverbNode) ==
            MA_SUCCESS) {
            // Park the mix at the incoming params BEFORE the node goes live in
            // the graph (no one-buffer blip of the config defaults).
            const AudioModel::ReverbProxyParams first =
                AudioModel::ReverbProxyFromParams(wet, dry, decay);
            ma_delay_node_set_wet(&m_reverbNode, first.wet);
            ma_delay_node_set_dry(&m_reverbNode, first.dry);
            ma_delay_node_set_decay(&m_reverbNode, first.feedback);
            // Splice: ambient group -> reverb node -> the group's former parent
            // (sfx group, or the endpoint if sfx failed to init). ONE attach, so
            // the /10 ambient gain + sidechain duck upstream of the
            // splice keep working unchanged.
            ma_node* downstream = m_sfxGroup ? reinterpret_cast<ma_node*>(m_sfxGroup.get())
                                             : ma_engine_get_endpoint(m_engine.get());
            ma_node_attach_output_bus(&m_reverbNode, 0, downstream, 0);
            ma_node_attach_output_bus(m_ambientGroup.get(), 0, &m_reverbNode, 0);
            m_reverbNodeInitialized = true;
            LUMINUMBRA_CORE_INFO(
                "Global reverb proxy online: delay node {} ms spliced onto the ambient bus",
                AudioModel::kReverbProxyDelayMs);
        } else {
            LUMINUMBRA_CORE_WARN(
                "Global reverb proxy: ma_delay_node_init failed; reverb params log-only.");
        }
    }
    if (m_reverbNodeInitialized) {
        const AudioModel::ReverbProxyParams p = AudioModel::ReverbProxyFromParams(wet, dry, decay);
        ma_delay_node_set_wet(&m_reverbNode, p.wet);
        ma_delay_node_set_dry(&m_reverbNode, p.dry);
        ma_delay_node_set_decay(&m_reverbNode, p.feedback);
    }
    LUMINUMBRA_CORE_INFO("Global reverb set - Wet: {}, Dry: {}, Decay: {}{}",
                         wet,
                         dry,
                         decay,
                         m_reverbNodeInitialized ? "" : " (proxy offline: log-only)");
}

void MiniaudioManager::PlayAmbientLoop(const AudioEventID& eventID,
                                       const glm::vec3& position,
                                       float radius) {
    if (!m_engine)
        return;

    // Stop existing ambient if playing
    StopAmbientLoop(eventID);

    const AudioEventDefinition* def = GetEventDefinition(eventID);
    if (!def || def->files.empty())
        return;

    auto sound = std::make_unique<ma_sound>();
    const std::string full_path = m_rootPath + def->files[0];

    // Decode the (short) loop fully into memory instead of streaming: ogg streaming can fail
    // silently, and a pre-decoded buffer loops seamlessly. Log failure so a missing/!decodable
    // ambient file is diagnosable rather than silent. Ambient beds attach to the
    // ambient bus (child of sfx): user.audio_sfx scales them, events sidechain-duck them.
    const ma_result amb_rc = ma_sound_init_from_file(m_engine.get(),
                                                     full_path.c_str(),
                                                     MA_SOUND_FLAG_DECODE,
                                                     GroupFor(BusId::Ambient),
                                                     nullptr,
                                                     sound.get());
    if (amb_rc != MA_SUCCESS) {
        LUMINUMBRA_CORE_WARN("PlayAmbientLoop: failed to load '{}' (ma_result {})",
                             full_path,
                             static_cast<int>(amb_rc));
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
    if (it == m_ambientSounds.end())
        return; // that bed isn't playing -> nothing to scale
    auto dit = m_eventDefinitions.find(eventID);
    const float base = (dit != m_eventDefinitions.end()) ? dit->second.volume : 1.0f;
    if (scale < 0.0f)
        scale = 0.0f;
    ma_sound_set_volume(it->second.get(), base * scale * m_currentEnvironment.ambient_volume);
}

void MiniaudioManager::ApplyEnvironmentalEffects(ma_sound* sound) {
    if (!sound)
        return;

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
