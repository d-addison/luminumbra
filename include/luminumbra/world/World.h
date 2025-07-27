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
#include "luminumbra/core/SaveData.h"

// Forward declarations
namespace Luminumbra::Rendering {
    class Shader;
    class Skybox;
    class CloudManager;
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

class World {
public:
    // Constructor now takes screen dimensions to create the Player's camera
    World(const std::string& slotName, const std::string& seed, int screenWidth, int screenHeight);
    ~World();

    // Update no longer needs viewerPosition; it gets it from its own Player
    void update(float deltaTime);

    void render(Luminumbra::Rendering::Shader& shader, const glm::mat4& view, const glm::mat4& projection, const glm::vec3& viewPos) const;
    void renderSkyboxAndClouds(const glm::mat4& view, const glm::mat4& projection) const;
    bool isSolid(const glm::vec3& worldPosition) const;
    const char* getBiome(const glm::vec3& position) const;
    const std::string& getSlotName() const { return m_SlotName; }
    float getTimeOfDay() const { return m_TimeOfDay; }
    glm::vec3 getSunDirection() const;

    // --- NEW: Methods for Player interaction and spawning ---
    Luminumbra::Player::Player* getPlayer() const;
    glm::vec3 getSpawnPoint() const;
    float getSurfaceHeight(float x, float z) const;

    // Getters
    const std::map<glm::ivec3, std::unique_ptr<Chunk>, IVec3Compare>& getChunks() const { return m_Chunks; }
    // Luminumbra::Rendering::Skybox& getSkybox() const { return *m_Skybox; }
    Luminumbra::Rendering::CloudManager& getCloudManager() const { return *m_CloudManager; }

    // Constants
    static constexpr int VIEW_DISTANCE = 8;
    static constexpr int LOD_DISTANCE = 5;
    static constexpr int UNLOAD_DISTANCE = 10;

    Core::WorldSaveData serialize() const;
    void applySaveData(const Core::WorldSaveData& data);
    void setTimeOfDay(float time);
    glm::vec3 getSkyColor() const;

    void renderCelestials(Luminumbra::Rendering::Shader& celestialShader, const glm::mat4& view, const glm::mat4& projection) const;

private:
    void loadChunksAroundPosition(const glm::vec3& position);
    void unloadDistantChunks(const glm::vec3& position);
    void processChunkQueue();
    glm::ivec3 worldToChunkCoord(const glm::vec3& worldPos) const;
    float getChunkDistance(const glm::ivec3& chunkPos, const glm::vec3& viewPos) const;

    void initCelestials();
    GLuint m_SunVAO, m_SunVBO;
    GLuint m_MoonVAO, m_MoonVBO;
    GLuint m_StarVAO, m_StarVBO;
    size_t m_StarVertexCount;

    // World generation
    std::string m_Seed;
    std::string m_SlotName; // The name of the save slot for this world
    
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
};

} // namespace Luminumbra::World