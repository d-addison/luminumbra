#include "luminumbra/audio/AudioManager.h"
#include "luminumbra/core/ResourceManager.h"
#include "luminumbra/core/Debug.h" // Assuming LOG() is defined here
#include <stdexcept>
#include <random>
#include <fstream>
#include <algorithm> // For std::erase_if

namespace Luminumbra::Audio {

// Helper to get a random number
static std::mt19937 s_RandomEngine(std::random_device{}());
static int getRandomInt(int min, int max) {
    if (min > max) {
        return min;
    }
    std::uniform_int_distribution<int> dist(min, max);
    return dist(s_RandomEngine);
}

// --- Singleton ---
AudioManager& AudioManager::getInstance() {
    static AudioManager instance;
    return instance;
}

// --- Constructor & Destructor ---
AudioManager::AudioManager() {
    m_Engine.reset(new ma_engine());
    ma_engine_config engineConfig = ma_engine_config_init();
    
    if (ma_engine_init(&engineConfig, m_Engine.get()) != MA_SUCCESS) {
        m_Engine.reset(); // Deleter will not be called if reset, so no double-uninit.
        throw std::runtime_error("Failed to initialize audio engine.");
    }

    // Initialize sound groups
    m_MasterGroup = std::make_unique<ma_sound_group>();
    m_MusicGroup = std::make_unique<ma_sound_group>();
    m_AmbienceGroup = std::make_unique<ma_sound_group>();
    m_PlayerGroup = std::make_unique<ma_sound_group>();
    m_SfxGroup = std::make_unique<ma_sound_group>();

    ma_sound_group_init(m_Engine.get(), 0, NULL, m_MasterGroup.get());
    ma_sound_group_init(m_Engine.get(), 0, m_MasterGroup.get(), m_MusicGroup.get());
    ma_sound_group_init(m_Engine.get(), 0, m_MasterGroup.get(), m_AmbienceGroup.get());
    ma_sound_group_init(m_Engine.get(), 0, m_MasterGroup.get(), m_PlayerGroup.get());
    ma_sound_group_init(m_Engine.get(), 0, m_MasterGroup.get(), m_SfxGroup.get());

    LOG("Audio Manager Initialized.");
}

AudioManager::~AudioManager() {
    stopMusic();

    for (auto const& [id, activeSound] : m_ActiveSounds) {
        ma_sound_uninit(activeSound.sound.get());
    }
    m_ActiveSounds.clear();
    
    // Groups must be uninitialized before the engine
    ma_sound_group_uninit(m_SfxGroup.get());
    ma_sound_group_uninit(m_PlayerGroup.get());
    ma_sound_group_uninit(m_AmbienceGroup.get());
    ma_sound_group_uninit(m_MusicGroup.get());
    ma_sound_group_uninit(m_MasterGroup.get());

    // m_Engine is cleaned up automatically by its custom MAEngineDeleter.
    LOG("Audio Manager Shutdown.");
}

void AudioManager::init() {
    initializeSoundMap();
    preloadSounds();
}

void AudioManager::preloadSounds() {
    LOG("Preloading sound effects into memory...");
    for (const auto& [event, paths] : m_SoundMap) {
        // Music is streamed, not preloaded.
        if (event == SoundEvent::MusicSplashScreen || event == SoundEvent::MusicMainMenu || event == SoundEvent::MusicGameplay) {
            continue;
        }

        for (const auto& relativePath : paths) {
            if (m_SoundDataCache.count(relativePath)) continue;

            std::string fullPath = Core::ResourceManager::getInstance().getResourcePath(relativePath);
            std::ifstream file(fullPath, std::ios::binary | std::ios::ate);

            if (!file.is_open()) {
                LOG("ERROR: Failed to open audio file for preloading: " + fullPath);
                continue;
            }

            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);

            std::vector<char> buffer(static_cast<size_t>(size));
            if (file.read(buffer.data(), size)) {
                m_SoundDataCache[relativePath] = std::move(buffer);

                // Now, register the encoded data with the engine's resource manager
                ma_resource_manager* pResourceManager = ma_engine_get_resource_manager(m_Engine.get());
                const auto& cachedData = m_SoundDataCache.at(relativePath);
                ma_result result = ma_resource_manager_register_encoded_data(pResourceManager, relativePath.c_str(), cachedData.data(), cachedData.size());
                if (result != MA_SUCCESS) {
                    LOG("ERROR: Failed to register sound data: " + relativePath);
                }
            }
        }
    }
    LOG("Finished preloading " + std::to_string(m_SoundDataCache.size()) + " sound effects.");
}

// --- Main Update ---
void AudioManager::update() {
    for (auto it = m_ActiveSounds.begin(); it != m_ActiveSounds.end(); /* no increment */) {
        auto& activeSound = it->second;
        if (!activeSound.isLooping && ma_sound_at_end(activeSound.sound.get())) {
            ma_sound_uninit(activeSound.sound.get());
            it = m_ActiveSounds.erase(it); // Erase and advance iterator
        } else {
            ++it; // Advance iterator
        }
    }
}


// --- Sound Playback ---
uint32_t AudioManager::playSound(SoundEvent event) {
    uint32_t id = m_NextSoundID++;
    auto& activeSound = m_ActiveSounds.try_emplace(id).first->second;
    
    activeSound.sound = std::make_unique<ma_sound>();
    activeSound.id = id;
    activeSound.isLooping = false;

    std::string path = getRandomSoundPath(event);
    if (path.empty() || m_SoundDataCache.find(path) == m_SoundDataCache.end()) {
        m_ActiveSounds.erase(id);
        LOG("Failed to find preloaded 2D sound for event.");
        return 0;
    }
    
    ma_result result = ma_sound_init_from_file(m_Engine.get(), path.c_str(), MA_SOUND_FLAG_DECODE, m_SfxGroup.get(), NULL, activeSound.sound.get());

    if (result != MA_SUCCESS) {
        m_ActiveSounds.erase(id);
        LOG("Failed to init 2D sound from memory.");
        return 0;
    }
    
    ma_sound_start(activeSound.sound.get());
    return id;
}

uint32_t AudioManager::playSound(SoundEvent event, const glm::vec3& position) {
    uint32_t id = m_NextSoundID++;
    auto& activeSound = m_ActiveSounds.try_emplace(id).first->second;
    
    activeSound.sound = std::make_unique<ma_sound>();
    activeSound.id = id;
    activeSound.isLooping = false;

    std::string path = getRandomSoundPath(event);
    if (path.empty() || m_SoundDataCache.find(path) == m_SoundDataCache.end()) {
        m_ActiveSounds.erase(id);
        LOG("Failed to find preloaded 3D sound for event.");
        return 0;
    }
    
    ma_uint32 flags = MA_SOUND_FLAG_DECODE; // Use DECODE, not 0. No spatialization flags needed by default.
    ma_result result = ma_sound_init_from_file(m_Engine.get(), path.c_str(), flags, m_PlayerGroup.get(), NULL, activeSound.sound.get());

    if (result != MA_SUCCESS) {
        m_ActiveSounds.erase(id);
        LOG("Failed to init 3D sound from memory.");
        return 0;
    }

    // FIX: Changed positioning to absolute, assuming 'position' is in world coordinates.
    ma_sound_set_positioning(activeSound.sound.get(), ma_positioning_absolute);
    ma_sound_set_position(activeSound.sound.get(), position.x, position.y, position.z);
    ma_sound_start(activeSound.sound.get());

    return id;
}

uint32_t AudioManager::playLoopingSound(SoundEvent event) {
    uint32_t id = m_NextSoundID++;
    auto& activeSound = m_ActiveSounds.try_emplace(id).first->second;
    
    activeSound.sound = std::make_unique<ma_sound>();
    activeSound.id = id;
    activeSound.isLooping = true;

    std::string path = getRandomSoundPath(event);
    if (path.empty() || m_SoundDataCache.find(path) == m_SoundDataCache.end()) {
        m_ActiveSounds.erase(id);
        LOG("Failed to find preloaded looping 2D sound for event.");
        return 0;
    }

    ma_result result = ma_sound_init_from_file(m_Engine.get(), path.c_str(), MA_SOUND_FLAG_DECODE, m_AmbienceGroup.get(), NULL, activeSound.sound.get());

    if (result != MA_SUCCESS) {
        m_ActiveSounds.erase(id);
        LOG("Failed to init looping 2D sound from memory.");
        return 0;
    }
    
    ma_sound_set_looping(activeSound.sound.get(), MA_TRUE);
    ma_sound_start(activeSound.sound.get());

    return id;
}

uint32_t AudioManager::playLoopingSound(SoundEvent event, const glm::vec3& position) {
    uint32_t id = m_NextSoundID++;
    auto& activeSound = m_ActiveSounds.try_emplace(id).first->second;
    
    activeSound.sound = std::make_unique<ma_sound>();
    activeSound.id = id;
    activeSound.isLooping = true;

    std::string path = getRandomSoundPath(event);
    if (path.empty() || m_SoundDataCache.find(path) == m_SoundDataCache.end()) {
        m_ActiveSounds.erase(id);
        LOG("Failed to find preloaded looping 3D sound for event.");
        return 0;
    }

    ma_uint32 flags = MA_SOUND_FLAG_DECODE; // Use DECODE, not 0.
    ma_result result = ma_sound_init_from_file(m_Engine.get(), path.c_str(), flags, m_PlayerGroup.get(), NULL, activeSound.sound.get());

    if (result != MA_SUCCESS) {
        m_ActiveSounds.erase(id);
        LOG("Failed to init looping 3D sound from memory.");
        return 0;
    }
    
    ma_sound_set_looping(activeSound.sound.get(), MA_TRUE);
    ma_sound_set_positioning(activeSound.sound.get(), ma_positioning_absolute);
    ma_sound_set_position(activeSound.sound.get(), position.x, position.y, position.z);
    ma_sound_start(activeSound.sound.get());

    return id;
}

void AudioManager::stopSound(uint32_t id) {
    auto it = m_ActiveSounds.find(id);
    if (it != m_ActiveSounds.end()) {
        // Uninit the sound resource, then erase it from the map.
        ma_sound_uninit(it->second.sound.get());
        m_ActiveSounds.erase(it);
    }
}

void AudioManager::updateSoundPosition(uint32_t id, const glm::vec3& position) {
    auto it = m_ActiveSounds.find(id);
    if (it != m_ActiveSounds.end()) {
        ma_sound_set_position(it->second.sound.get(), position.x, position.y, position.z);
    }
}

// --- Music ---
void AudioManager::playMusic(SoundEvent event) {
    stopMusic();
    
    std::string relativePath = getRandomSoundPath(event);
    if (relativePath.empty()) {
        LOG("Failed to find music for event.");
        return;
    }

    m_Music = std::make_unique<ma_sound>();
    std::string fullPath = Core::ResourceManager::getInstance().getResourcePath(relativePath);

    ma_result result = ma_sound_init_from_file(m_Engine.get(), fullPath.c_str(), MA_SOUND_FLAG_STREAM, m_MusicGroup.get(), NULL, m_Music.get());
    if (result != MA_SUCCESS) {
        m_Music.reset();
        LOG("Failed to load music: " + fullPath);
        return;
    }
    ma_sound_set_looping(m_Music.get(), MA_TRUE);
    ma_sound_start(m_Music.get());
}

void AudioManager::stopMusic() {
    if (m_Music) {
        ma_sound_uninit(m_Music.get());
        m_Music.reset();
    }
}

// --- Volume Control ---
void AudioManager::setGroupVolume(SoundGroup group, float volume) {
    ma_sound_group* targetGroup = nullptr;
    switch (group) {
        case SoundGroup::Master:   targetGroup = m_MasterGroup.get();   break;
        case SoundGroup::Music:    targetGroup = m_MusicGroup.get();    break;
        case SoundGroup::Ambience: targetGroup = m_AmbienceGroup.get(); break;
        case SoundGroup::Player:   targetGroup = m_PlayerGroup.get();   break;
        case SoundGroup::SFX:      targetGroup = m_SfxGroup.get();      break;
    }
    if (targetGroup) {
        ma_sound_group_set_volume(targetGroup, volume);
    }
}

bool AudioManager::isSoundActive(uint32_t id) const {
    return m_ActiveSounds.count(id) > 0;
}

// --- Listener ---
void AudioManager::setListenerPosition(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& up) {
    ma_engine_listener_set_position(m_Engine.get(), 0, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(m_Engine.get(), 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(m_Engine.get(), 0, up.x, up.y, up.z);
}

// --- Sound Map ---
std::string AudioManager::getRandomSoundPath(SoundEvent event) {
    auto it = m_SoundMap.find(event);
    if (it == m_SoundMap.end() || it->second.empty()) {
        LOG("Sound event has no audio files: " + std::to_string(static_cast<int>(event)));
        return "";
    }
    int randIndex = getRandomInt(0, static_cast<int>(it->second.size() - 1));
    return it->second[randIndex];
}

void AudioManager::initializeSoundMap() {
    LOG("Initializing Sound Map...");

    // == Player Actions ==
    m_SoundMap[SoundEvent::PlayerJump] = {
        "res/audio/Foley Footsteps/Foley Footstep Slide Boot Single 01.wav",
    };
    m_SoundMap[SoundEvent::PlayerSprint] = {
        "res/audio/Human Elements/Human Male Heavy Breathing 01.wav",
        "res/audio/Human Elements/Human Male Heavy Breathing 02.wav"
    };
    m_SoundMap[SoundEvent::PlayerHurt] = {
        "res/audio/Human Elements/Human Fingers Cracking 01.wav",
        "res/audio/Human Elements/Human Male Troll Scream 01.wav"
    };
    m_SoundMap[SoundEvent::PlayerLand] = {
        "res/audio/Foley Footsteps/Foley Footstep Work Boots Dirt Debris Jump 01.wav",
        "res/audio/Foley Footsteps/Foley Footstep Boot Thump On Leaves 03.wav",
        "res/audio/Foley Footsteps/Foley Footstep Work Boots Concrete Jump 01.wav"
    };

    // == Footsteps ==
    m_SoundMap[SoundEvent::FootstepDirt] = {
        "res/audio/Foley Footsteps/Foley Footstep Boot Single Step Left On Leaves 01.wav",
        "res/audio/Foley Footsteps/Foley Footstep Boot Single Step Right On Leaves 01.wav",
    };
    m_SoundMap[SoundEvent::FootstepStone] = {
        "res/audio/Foley Footsteps/Foley Footstep Work Boots Concrete Walking 01.wav",
        "res/audio/Foley Footsteps/Foley Footstep Golf Cleats Walking On Cement 02.wav"
    };
    m_SoundMap[SoundEvent::FootstepWood] = {
        "res/audio/Foley Footsteps/Foley Footstep Cowboy Boots Walking On Wood Board Walk 01.wav",
        "res/audio/Foley Footsteps/Foley Footstep Cowboy Boots Walking On Solid Wood Platform 01.wav",
        "res/audio/Foley Footsteps/Foley Footstep Hard Sole Dress Shoe Walking On Hollow Wood Surface 01.wav"
    };
    m_SoundMap[SoundEvent::FootstepGrass] = {
        "res/audio/Foley Footsteps/Foley Footstep Human Walking Through Tall Grass 01.wav",
        "res/audio/Foley Footsteps/Foley Footstep Human Walking Through Tall Grass 02.wav",
        "res/audio/Foley Footsteps/Foley Footstep Work Boots Leaves And Twigs Debris Walking 01.wav"
    };
    m_SoundMap[SoundEvent::FootstepSnow] = {
        "res/audio/Weather/Weather Snow Footstep Single 04.wav",
        "res/audio/Weather/Weather Snow Footstep Single 06.wav",
        "res/audio/Weather/Weather Snow Shoe Stepping On Snow 05.wav"
    };
    m_SoundMap[SoundEvent::FootstepSand] = {
        "res/audio/Foley Footsteps/Foley Footstep Work Boots Sand Walking 01.wav",
        "res/audio/Foley Footsteps/Foley Footstep Work Boots Sand Walking 02.wav"
    };
    m_SoundMap[SoundEvent::FootstepMetal] = {
        "res/audio/Foley Footsteps/Foley Footstep Work Boots Metal Single Step 01.wav",
        "res/audio/Foley Footsteps/Foley Footstep Work Boots Metal Single Step 02.wav"
    };
    m_SoundMap[SoundEvent::FootstepWater] = {
        "res/audio/Ambience_2/Ambience Water Hands Splashing Slow 01.wav" // Simulates wading
    };

    // == Movement Loops ==
    m_SoundMap[SoundEvent::SlideLoop] = { "res/audio/Foley Footsteps/Foley Footstep Work Boots Dirt Debris Slide 01.wav" };
    m_SoundMap[SoundEvent::GlideLoop] = { 
        "res/audio/Weather/Weather Ambience Wind Blizzard Snow Whistle Gust 01.wav",
        "res/audio/Ambience_2/Ambience Wind Blowing Through Trees 01.wav"
    };

    // == Player Actions ==
    m_SoundMap[SoundEvent::PlayerOutOfStamina] = {
        "res/audio/Human Elements/Human Male Cough Out Of Breath Cough 01.wav",
        "res/audio/Human Elements/Human Male Swallowing 01.wav"
    };

    // == Ambience & Environment ==
    m_SoundMap[SoundEvent::AmbienceForestDay] = { "res/audio/Ambience_2/Ambience Nature 180 01.wav" };
    m_SoundMap[SoundEvent::AmbienceForestNight] = { "res/audio/Ambience_2/Ambience Night Crickets And A Bullfrog 01.wav" };
    m_SoundMap[SoundEvent::AmbienceCave] = { 
        "res/audio/Ambience_2/Ambience Sewer Drain 01.wav", // Good for a wet, echoey feel
        "res/audio/Ambience_1/Ambience Creek 01.wav" // Can be used for underground streams
    }; 
    m_SoundMap[SoundEvent::EnvWaterDrip] = { // One-shot sounds for caves
        "res/audio/Weather/Weather Ambience Rain Drips Water 01.wav",
        "res/audio/Weather/Weather Ambience Rain Drips Water 02.wav"
    };
    m_SoundMap[SoundEvent::AmbienceOcean] = { "res/audio/Ambience_1/Ambience Ducks Water And Bugs Near City 01.wav" };

    // == Weather ==
    m_SoundMap[SoundEvent::RainLoop] = { "res/audio/Weather/Weather Rain 01.wav" };
    m_SoundMap[SoundEvent::RainSplash] = { "res/audio/Weather/Weather Ambience Rain Drips Water Splatty 01.wav" };
    m_SoundMap[SoundEvent::EnvThunderClose] = {
        "res/audio/Weather/Weather Storm Lightning Bolt Crash Crack 01.wav",
        "res/audio/Weather/Weather Storm Lightning Bolt Crash Crack 02.wav",
        "res/audio/Weather/Weather Storm Lightning Bolt Crash Crack 03.wav"
    };
    m_SoundMap[SoundEvent::EnvThunderDistant] = {
        "res/audio/Fire and Explosions/Explosion Distant 01.wav", // These can simulate distant thunder rumble
        "res/audio/Fire and Explosions/Explosion Distant 02.wav"
    };


    // == Interactive Objects ==
    m_SoundMap[SoundEvent::ObjectFireLoop] = { 
        "res/audio/Fire and Explosions/Fire Roar Blaze Bonfire 01.wav",
        "res/audio/Fire and Explosions/Fire Fireplace Pops Crackle 01.wav"
    };
    m_SoundMap[SoundEvent::ObjectFireExtinguish] = {
        "res/audio/Fire and Explosions/Fire Torch Sizzle Water Extinguish 01.wav"
    };

    // == Music ==
    m_SoundMap[SoundEvent::MusicSplashScreen] = { "res/audio/Music/splash.wav" };
    m_SoundMap[SoundEvent::MusicMainMenu] = { "res/audio/Music/menu.wav" };
    m_SoundMap[SoundEvent::MusicGameplay] = { "res/audio/Music/soundtrack.wav" };

    // == UI Sounds ==
    m_SoundMap[SoundEvent::UIClick] = { "res/audio/UI/click.wav" };

    LOG("Sound Map Initialized with " + std::to_string(m_SoundMap.size()) + " event types.");
}

} // namespace Luminumbra::Audio