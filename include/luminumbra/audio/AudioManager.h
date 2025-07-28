#pragma once

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <unordered_map>
#include <glm/glm.hpp>
#include <minaudio.h>

// Forward declaration for miniaudio types
struct ma_engine;
struct ma_sound;

namespace Luminumbra::Audio {

// An enum to represent all possible sound effects in the game.
// This decouples the game logic from specific audio file paths.
enum class SoundEvent {
    // Player Actions
    PlayerJump,         // Grunt/exertion for jumping
    PlayerLand,         // Landing sound, separate from footsteps
    PlayerHurt,         // Taking damage
    PlayerDeath,        // Player death sound
    PlayerSwim,         // Swimming strokes loop
    PlayerSplash,       // Entering/exiting water
    PlayerOutOfStamina, // Exhaustion sound

    // Player Movement (triggered by material type)
    FootstepDirt,
    FootstepStone,
    FootstepWood,
    FootstepGrass,
    FootstepSand,       // Example for a potential new terrain
    FootstepSnow,
    FootstepWater,      // Wading in shallow water
    SlideLoop,          // Sliding down a surface
    GlideLoop,          // Gliding/flying

    // World & Environment Loops (Base Layers)
    AmbienceForestDay,
    AmbienceForestNight,
    AmbienceCave,
    AmbienceOcean,
    AmbienceWindLight,
    AmbienceWindHeavy,

    // World & Environment One-Shots (for dynamism)
    EnvBirdChirp,       // Randomly play during the day
    EnvOwlHoot,         // Randomly play at night
    EnvWaterDrip,       // Common in caves
    EnvThunderClose,
    EnvThunderDistant,

    // Weather
    RainLoop,
    RainSplash,         // Particle-based splashes

    // Interactive Objects
    ObjectFireLoop,
    ObjectFireExtinguish,
    ObjectDoorOpen,
    ObjectDoorClose,
    ObjectChestOpen,
    ObjectChestClose,

    // UI
    UIClick,
    UIHover,
    MusicSplashScreen,
    MusicMainMenu,
    MusicGameplay,      // Main game music loop
    // ... other events
};

// Enum for volume control categories
enum class SoundGroup {
    Master,
    Music,
    Ambience,
    Player,
    SFX
};

// A struct to manage an active sound instance, whether it's a one-shot or a loop.
struct ActiveSound {
    std::unique_ptr<ma_sound> sound;
    uint32_t id = 0;
    bool isLooping = false;
};

// Custom deleter for the miniaudio engine
struct MAEngineDeleter {
    void operator()(ma_engine* pEngine) const;
};

class AudioManager {
public:
    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    ~AudioManager();

    static AudioManager& getInstance();

    void init();
    void update();

    // --- Sound Playback ---
    uint32_t playSound(SoundEvent event, const glm::vec3& position);
    uint32_t playSound(SoundEvent event);
    
    uint32_t playLoopingSound(SoundEvent event, const glm::vec3& position);
    uint32_t playLoopingSound(SoundEvent event);                           
    void stopSound(uint32_t id);
    void updateSoundPosition(uint32_t id, const glm::vec3& position);
    bool isSoundActive(uint32_t id) const;

    // --- Music ---
    void playMusic(const std::string& filePath);
    void stopMusic();

    // --- Volume Control ---
    void setGroupVolume(SoundGroup group, float volume);

    // --- Listener ---
    void setListenerPosition(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& up);

private:
    AudioManager();
    
    void initializeSoundMap();
    std::string getRandomSoundPath(SoundEvent event);

    std::unique_ptr<ma_engine, MAEngineDeleter> m_Engine;
    std::unique_ptr<ma_sound> m_Music;

    // Sound Groups for Volume Control
    std::unique_ptr<ma_sound_group> m_MasterGroup;
    std::unique_ptr<ma_sound_group> m_MusicGroup;
    std::unique_ptr<ma_sound_group> m_AmbienceGroup;
    std::unique_ptr<ma_sound_group> m_PlayerGroup;
    std::unique_ptr<ma_sound_group> m_SfxGroup;

    // Sound Management
    std::unordered_map<uint32_t, ActiveSound> m_ActiveSounds;
    std::map<SoundEvent, std::vector<std::string>> m_SoundMap;
    uint32_t m_NextSoundID = 1;
};

} // namespace Luminumbra::Audio