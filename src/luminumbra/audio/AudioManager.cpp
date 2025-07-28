#include "luminumbra/audio/AudioManager.h"
#include "luminumbra/core/ResourceManager.h"
#include "luminumbra/core/Debug.h"
#include <stdexcept>
#include <random>

namespace Luminumbra::Audio {

// Helper to get a random number
static std::mt19937 s_RandomEngine(std::random_device{}());
static int getRandomInt(int min, int max) {
    std::uniform_int_distribution<std::mt19937::result_type> dist(min, max);
    return dist(s_RandomEngine);
}

// --- Deleter ---
void MAEngineDeleter::operator()(ma_engine* pEngine) const {
    if (pEngine) {
        ma_engine_uninit(pEngine);
        delete pEngine;
    }
}

// --- Singleton ---
AudioManager& AudioManager::getInstance() {
    static AudioManager instance;
    return instance;
}

// --- Constructor & Destructor ---
AudioManager::AudioManager() {
    m_Engine.reset(new ma_engine());
    if (ma_engine_init(NULL, m_Engine.get()) != MA_SUCCESS) {
        m_Engine.reset();
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
    m_ActiveSounds.clear(); // This will uninit sounds via ActiveSound destructor
    
    // Groups must be uninitialized before the engine
    ma_sound_group_uninit(m_SfxGroup.get());
    ma_sound_group_uninit(m_PlayerGroup.get());
    ma_sound_group_uninit(m_AmbienceGroup.get());
    ma_sound_group_uninit(m_MusicGroup.get());
    ma_sound_group_uninit(m_MasterGroup.get());

    // m_Engine is cleaned up automatically by its custom deleter.
    LOG("Audio Manager Shutdown.");
}

void AudioManager::init() {
    initializeSoundMap();
}

// --- Main Update ---
void AudioManager::update() {
    std::vector<uint32_t> finishedSounds;
    for (auto const& [id, activeSound] : m_ActiveSounds) {
        if (!activeSound.isLooping && ma_sound_at_end(activeSound.sound.get())) {
            finishedSounds.push_back(id);
        }
    }

    for (uint32_t id : finishedSounds) {
        stopSound(id);
    }
}

// --- Sound Playback ---
uint32_t AudioManager::playSound(SoundEvent event) {
    std::string fullPath = Core::ResourceManager::getInstance().getResourcePath(getRandomSoundPath(event));
    ma_engine_play_sound(m_Engine.get(), fullPath.c_str(), m_SfxGroup.get());
    return 0; // 2D sounds don't need an ID for now
}

uint32_t AudioManager::playSound(SoundEvent event, const glm::vec3& position) {
    uint32_t id = m_NextSoundID++;
    auto& activeSound = m_ActiveSounds[id];
    
    activeSound.sound = std::make_unique<ma_sound>();
    activeSound.id = id;
    activeSound.isLooping = false;

    std::string fullPath = Core::ResourceManager::getInstance().getResourcePath(getRandomSoundPath(event));
    
    // For 3D sounds, we need to disable the default spatializer and use our own coordinates.
    ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
    ma_result result = ma_sound_init_from_file(m_Engine.get(), fullPath.c_str(), flags, m_PlayerGroup.get(), NULL, activeSound.sound.get());

    if (result != MA_SUCCESS) {
        m_ActiveSounds.erase(id);
        LOG("Failed to load 3D sound: " + fullPath);
        return 0;
    }

    ma_sound_set_positioning(activeSound.sound.get(), ma_positioning_relative);
    ma_sound_set_position(activeSound.sound.get(), position.x, position.y, position.z);
    ma_sound_start(activeSound.sound.get());

    return id;
}

uint32_t AudioManager::playLoopingSound(SoundEvent event) {
    uint32_t id = m_NextSoundID++;
    auto& activeSound = m_ActiveSounds[id];
    
    activeSound.sound = std::make_unique<ma_sound>();
    activeSound.id = id;
    activeSound.isLooping = true;

    std::string fullPath = Core::ResourceManager::getInstance().getResourcePath(getRandomSoundPath(event));
    
    // Initialize the sound without positional flags, attached to the Music group.
    // The '0' for flags means we use default behavior for a non-positional sound.
    ma_result result = ma_sound_init_from_file(m_Engine.get(), fullPath.c_str(), 0, m_MusicGroup.get(), NULL, activeSound.sound.get());

    if (result != MA_SUCCESS) {
        m_ActiveSounds.erase(id);
        LOG("Failed to load looping 2D sound: " + fullPath);
        return 0;
    }
    
    ma_sound_set_looping(activeSound.sound.get(), true);
    ma_sound_start(activeSound.sound.get());

    return id;
}

uint32_t AudioManager::playLoopingSound(SoundEvent event, const glm::vec3& position) {
    uint32_t id = m_NextSoundID++;
    auto& activeSound = m_ActiveSounds[id];
    
    activeSound.sound = std::make_unique<ma_sound>();
    activeSound.id = id;
    activeSound.isLooping = true;

    std::string fullPath = Core::ResourceManager::getInstance().getResourcePath(getRandomSoundPath(event));
    ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
    ma_result result = ma_sound_init_from_file(m_Engine.get(), fullPath.c_str(), flags, m_PlayerGroup.get(), NULL, activeSound.sound.get());

    if (result != MA_SUCCESS) {
        m_ActiveSounds.erase(id);
        LOG("Failed to load looping 3D sound: " + fullPath);
        return 0;
    }
    
    ma_sound_set_looping(activeSound.sound.get(), true);
    ma_sound_set_positioning(activeSound.sound.get(), ma_positioning_relative);
    ma_sound_set_position(activeSound.sound.get(), position.x, position.y, position.z);
    ma_sound_start(activeSound.sound.get());

    return id;
}

void AudioManager::stopSound(uint32_t id) {
    auto it = m_ActiveSounds.find(id);
    if (it != m_ActiveSounds.end()) {
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
void AudioManager::playMusic(const std::string& relativePath) {
    stopMusic();
    m_Music = std::make_unique<ma_sound>();
    std::string fullPath = Core::ResourceManager::getInstance().getResourcePath(relativePath);

    ma_result result = ma_sound_init_from_file(m_Engine.get(), fullPath.c_str(), MA_SOUND_FLAG_STREAM, m_MusicGroup.get(), NULL, m_Music.get());
    if (result != MA_SUCCESS) {
        m_Music.reset();
        LOG("Failed to load music: " + fullPath);
        return;
    }
    ma_sound_set_looping(m_Music.get(), true);
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
    int randIndex = getRandomInt(0, it->second.size() - 1);
    return it->second[randIndex];
}

void AudioManager::initializeSoundMap() {
    LOG("Initializing Sound Map...");

    // == Player Actions ==
    m_SoundMap[SoundEvent::PlayerJump] = {
        "res/audio/Human Elements/Human Male Grunt 01.wav",
        "res/audio/Human Elements/Human Male Karate Yell Hey-Yah 01.wav"
    };
    m_SoundMap[SoundEvent::PlayerHurt] = {
        "res/audio/Human Elements/Human Male Yell Grunt In Pain 01.wav",
        "res/audio/Human Elements/Human Male Yell Grunt In Pain 02.wav"
    };
    m_SoundMap[SoundEvent::PlayerLand] = {
        "res/audio/Foley Footsteps/Foley Footstep Work Boots Dirt Debris Jump 01.wav",
        "res/audio/Foley Footsteps/Foley Footstep Boot Thump On Leaves 03.wav",
        "res/audio/Foley Footsteps/Foley Footstep Work Boots Concrete Jump 01.wav"
    };

    // == Footsteps ==
    m_SoundMap[SoundEvent::FootstepDirt] = {
        "res/audio/Foley Footsteps/Foley Footstep Work Boots Dirt Debris Walking 01.wav",
        "res/audio/Foley Footsteps/Foley Footstep Work Boots Dirt Debris Walking 02.wav",
        "res/audio/Foley Footsteps/Foley Footstep Cowboy Boots Dirt Debris Waking 02.wav"
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
        "res/audio/Player/Player Out Of Stamina 01.wav",
        "res/audio/Player/Player Out Of Stamina 02.wav"
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
    m_SoundMap[SoundEvent::AmbienceOcean] = { "res/audio/Ambience_2/Ambience Ocean Shore 01.wav" };

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

    LOG("Sound Map Initialized with " + std::to_string(m_SoundMap.size()) + " event types.");
}

} // namespace Luminumbra::Audio