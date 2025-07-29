#pragma once

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <unordered_map>
#include <glm/glm.hpp>
#include <minaudio.h>

// Forward declaration for minaudio types
struct ma_engine;
struct ma_sound;
// struct ma_sound_group;

namespace Luminumbra::Audio {

// An enum to represent all possible sound effects in the game.
// This decouples the game logic from specific audio file paths.
enum class SoundEvent {
    // Player Actions
    PlayerJump,
    PlayerLand,
    PlayerHurt,
    PlayerSprint,
    PlayerDeath,
    PlayerSwim,
    PlayerSplash,
    PlayerOutOfStamina,

    // Player Movement (triggered by material type)
    FootstepDirt,
    FootstepStone,
    FootstepWood,
    FootstepGrass,
    FootstepSand,
    FootstepMetal,
    FootstepSnow,
    FootstepWater,
    SlideLoop,
    GlideLoop,

    // World & Environment Loops (Base Layers)
    AmbienceForestDay,
    AmbienceForestNight,
    AmbienceCave,
    AmbienceOcean,
    AmbienceWindLight,
    AmbienceWindHeavy,

    // World & Environment One-Shots (for dynamism)
    EnvBirdChirp,
    EnvOwlHoot,
    EnvWaterDrip,
    EnvThunderClose,
    EnvThunderDistant,

    // Weather
    RainLoop,
    SnowLoop,
    RainSplash,

    // Interactive Objects
    ObjectFireLoop,
    ObjectFireExtinguish,
    ObjectDoorOpen,
    ObjectDoorClose,
    ObjectChestOpen,
    ObjectChestClose,

    // UI & Music
    UIClick,
    UIHover,
    MusicSplashScreen,
    MusicMainMenu,
    MusicGameplay,
};

enum class SoundGroup {
    Master, Music, Ambience, Player, SFX
};

struct ActiveSound {
    std::unique_ptr<ma_sound> sound;
    uint32_t id = 0;
    bool isLooping = false;
};

// FIX: The custom deleter must be fully defined before being used as a
// template argument for std::unique_ptr in the AudioManager class declaration.
struct MAEngineDeleter {
    void operator()(ma_engine* pEngine) const {
        if (pEngine) {
            ma_engine_uninit(pEngine);
            delete pEngine;
        }
    }
};

class AudioManager {
public:
    static AudioManager& getInstance();

    AudioManager(const AudioManager&) = delete;
    void operator=(const AudioManager&) = delete;

    void init();
    void update();

    // Sound Playback
    uint32_t playSound(SoundEvent event);
    uint32_t playSound(SoundEvent event, const glm::vec3& position);
    uint32_t playLoopingSound(SoundEvent event);
    uint32_t playLoopingSound(SoundEvent event, const glm::vec3& position);
    void stopSound(uint32_t id);
    void updateSoundPosition(uint32_t id, const glm::vec3& position);
    bool isSoundActive(uint32_t id) const;

    // Music
    void playMusic(SoundEvent event);
    void stopMusic();

    // Volume Control
    void setGroupVolume(SoundGroup group, float volume);

    // Listener
    void setListenerPosition(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& up);

private:
    AudioManager();
    ~AudioManager();

    void initializeSoundMap();
    std::string getRandomSoundPath(SoundEvent event);
    void preloadSounds();

    std::unique_ptr<ma_engine, MAEngineDeleter> m_Engine;
    std::unique_ptr<ma_sound> m_Music;
    
    // Sound Groups
    std::unique_ptr<ma_sound_group> m_MasterGroup;
    std::unique_ptr<ma_sound_group> m_MusicGroup;
    std::unique_ptr<ma_sound_group> m_AmbienceGroup;
    std::unique_ptr<ma_sound_group> m_PlayerGroup;
    std::unique_ptr<ma_sound_group> m_SfxGroup;

    // Sound Management
    std::unordered_map<uint32_t, ActiveSound> m_ActiveSounds;
    uint32_t m_NextSoundID = 1;

    // Data
    std::unordered_map<std::string, std::vector<char>> m_SoundDataCache;
    std::unordered_map<SoundEvent, std::vector<std::string>> m_SoundMap;
};

} // namespace Luminumbra::Audio