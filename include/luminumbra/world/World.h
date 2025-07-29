#pragma once

#include <memory>
#include <map>
#include <glm/glm.hpp>
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <glad/gl.h>
#include <set>
#include "luminumbra/core/SaveData.h"
#include "luminumbra/audio/AudioManager.h"
#include "luminumbra/rendering/particles/Particle.h" 
#include "luminumbra/world/Chunk.h"
#include "luminumbra/world/WeatherManager.h"
#include "luminumbra/player/Player.h"
#include "luminumbra/rendering/Shader.h"

// Forward declarations
namespace Luminumbra::Rendering {
    class Shader;
    class Skybox;
    class CloudManager;
    class ParticleSystem;
    class WeatherManager;
}
namespace Luminumbra::World { class Chunk; }
namespace Luminumbra::Player { class Player; }

namespace Luminumbra::World {

struct IVec3Compare {
    bool operator()(const glm::ivec3& a, const glm::ivec3& b) const {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    }
};

struct ChunkLoadRequest {
    glm::ivec3 position;
    float priority;
    int lod;
    bool operator>(const ChunkLoadRequest& other) const {
        return priority > other.priority;
    }
};

struct LeafParticle {
    glm::vec3 position;
    glm::vec3 velocity;
    float life = 0.0f;
    float maxLife = 1.0f;
};

struct BurningTree {
    glm::ivec3 chunkCoord; // The coordinate of the chunk the tree is in
    size_t treeIndex;      // The index of the tree in the chunk's instance vector
    float timeBurning = 0.0f;
    uint32_t soundID = 0; // ID for the looping fire sound
    
    // For using in std::set/map
    bool operator==(const BurningTree& other) const {
        return chunkCoord == other.chunkCoord && treeIndex == other.treeIndex;
    }
};

class World {
public:
    World(const std::string& slotName, const std::string& seed, int screenWidth, int screenHeight);
    ~World();

    void update(float deltaTime);

    void renderTerrain(Rendering::Shader& shader, const glm::vec3& viewPos) const;
    void renderWater(Rendering::Shader& shader, const glm::vec3& viewPos) const;
    void renderSkyboxAndClouds(const glm::mat4& view, const glm::mat4& projection) const;
    void renderFoliage(Luminumbra::Rendering::Shader& foliageShader) const;
    bool isSolid(const glm::vec3& worldPosition) const;
    const char* getBiome(const glm::vec3& position) const;
    const std::string& getSlotName() const { return m_SlotName; }
    float getTimeOfDay() const { return m_TimeOfDay; }
    glm::vec3 getSunDirection() const;
    Luminumbra::Player::Player* getPlayer() const;
    glm::vec3 getSpawnPoint() const;
    float getSurfaceHeight(float x, float z) const;

    const std::map<glm::ivec3, std::unique_ptr<Chunk>, IVec3Compare>& getChunks() const { return m_Chunks; }
    // Luminumbra::Rendering::Skybox& getSkybox() const { return *m_Skybox; }
    Luminumbra::Rendering::CloudManager& getCloudManager() const { return *m_CloudManager; }
    Luminumbra::Rendering::ParticleSystem* getParticleSystem() const { return m_ParticleSystem.get(); }

    static constexpr int VIEW_DISTANCE = 8;
    static constexpr int LOD_DISTANCE = 5;
    static constexpr int UNLOAD_DISTANCE = 10;

    Core::WorldSaveData serialize() const;
    void applySaveData(const Core::WorldSaveData& data);
    void setTimeOfDay(float time);
    glm::vec3 getSkyColor() const;

    void renderCelestials(Luminumbra::Rendering::Shader& celestialShader, const glm::mat4& view, const glm::mat4& projection) const;
    void setWeather(WeatherType type);
    void startFireNearPlayer();
    void teleportToEffect(const std::string& effect);

    Audio::SoundEvent getFootstepSoundForPosition(const glm::vec3& position) const;

    WeatherManager* getWeatherManager() const { return m_WeatherManager.get(); }

private:
    void loadChunksAroundPosition(const glm::vec3& position);
    void unloadDistantChunks(const glm::vec3& position);
    void processChunkQueue();
    glm::ivec3 worldToChunkCoord(const glm::vec3& worldPos) const;
    float getChunkDistance(const glm::ivec3& chunkPos, const glm::vec3& viewPos) const;

    void initCelestials();
    void initFoliage();

     Audio::SoundEvent getAmbientSoundForBiome(const char* biomeName, bool isNight) const;

    GLuint m_SunVAO, m_SunVBO;
    GLuint m_MoonVAO, m_MoonVBO;
    GLuint m_StarVAO, m_StarVBO;
    size_t m_StarVertexCount;

    GLuint m_TreeTrunkVAO = 0, m_TreeTrunkVBO = 0, m_TreeTrunkEBO = 0;
    GLuint m_TreeLeavesVAO = 0, m_TreeLeavesVBO = 0, m_TreeLeavesEBO = 0;
    GLuint m_BushVAO = 0, m_BushVBO = 0, m_BushEBO = 0;
    GLuint m_FoliageInstanceVBO = 0;
    size_t m_TreeTrunkIndexCount = 0, m_TreeLeavesIndexCount = 0, m_BushIndexCount = 0;

    // World generation
    std::string m_Seed;
    std::string m_SlotName;
    
    // The world now owns its player
    std::unique_ptr<Luminumbra::Player::Player> m_Player;

    // Chunk storage & multithreading
    std::map<glm::ivec3, std::unique_ptr<Chunk>, IVec3Compare> m_Chunks;
    std::priority_queue<ChunkLoadRequest, std::vector<ChunkLoadRequest>, std::greater<ChunkLoadRequest>> m_ChunkLoadQueue;
    std::vector<std::thread> m_GenerationThreads;
    std::mutex m_ChunkMutex;
    std::mutex m_QueueMutex;
    std::atomic<bool> m_ShouldStop{false};
    
    // State
    glm::vec3 m_LastViewerPosition{0.0f};
    static constexpr float UPDATE_THRESHOLD = 16.0f;
    float m_TimeOfDay = 0.25f;
    static constexpr float DAY_DURATION = 100.0f;

    // Scenery
    // std::unique_ptr<Luminumbra::Rendering::Skybox> m_Skybox;
    std::unique_ptr<Luminumbra::Rendering::CloudManager> m_CloudManager;
    std::unique_ptr<Luminumbra::Rendering::ParticleSystem> m_ParticleSystem;

    // Particles
    void updateParticles(float deltaTime);
    float m_LeafEmitTimer = 0.0f;
    std::vector<BurningTree> m_BurningTrees;

    bool m_ShowClouds = true;
    bool m_ShowCampfire = true;
    bool m_ShowPortal = true;
    bool m_ShowAurora = false;

    // Test zone positions (so we can find them easily)
    const glm::vec3 CAMPFIRE_POS = glm::vec3(50.0f, 10.0f, 50.0f);
    const glm::vec3 PORTAL_POS = glm::vec3(0.0f, 20.0f, 0.0f);
    const glm::vec3 AURORA_HEIGHT = glm::vec3(0.0f, 200.0f, 0.0f);
    
    // Cloud spawn points
    const std::vector<glm::vec3> CLOUD_SPAWN_POINTS = {
        glm::vec3(100.0f, 100.0f, 100.0f),
        glm::vec3(-100.0f, 120.0f, -100.0f),
        glm::vec3(0.0f, 110.0f, 200.0f)
    };

    std::unique_ptr<WeatherManager> m_WeatherManager;
    float m_RainSplashTimer = 0.0f;
    uint32_t m_AmbientSoundID = 0;
    Audio::SoundEvent m_CurrentAmbientEvent = Audio::SoundEvent::UIClick;
};

} // namespace Luminumbra::World