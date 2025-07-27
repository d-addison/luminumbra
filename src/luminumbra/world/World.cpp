#include "luminumbra/world/World.h"
#include "luminumbra/world/Chunk.h"
#include "luminumbra/player/Player.h"
#include "luminumbra/rendering/Shader.h"
#include "luminumbra/rendering/Skybox.h"
#include "luminumbra/rendering/CloudManager.h"
#include "luminumbra/core/Debug.h"
#include <glm/gtc/color_space.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
#include <random>

namespace Luminumbra::World {

World::World(const std::string& slotName, const std::string& seed, int screenWidth, int screenHeight) 
    : m_SlotName(slotName), m_Seed(seed) 
{
    LOG("World::Constructor - Creating world '" + m_SlotName + "' with seed: " + m_Seed);
    
    m_Player = std::make_unique<Luminumbra::Player::Player>(screenWidth, screenHeight);
    // m_Skybox = std::make_unique<Luminumbra::Rendering::Skybox>();
    m_CloudManager = std::make_unique<Luminumbra::Rendering::CloudManager>();
    m_CloudManager->init();
    
    // Start worker threads for chunk generation
    unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);
    LOG("World::Constructor - Starting " + std::to_string(numThreads) + " worker threads");
    for (unsigned int i = 0; i < numThreads; ++i) {
        m_GenerationThreads.emplace_back(&World::processChunkQueue, this);
    }
    
    // Load initial chunks around origin before finding spawn point
    loadChunksAroundPosition(glm::vec3(0.0f));

    float spawnY = 0.0f;
    int wait_attempts = 0;
    const int max_wait_attempts = 100; // Wait for max 5 seconds
    const float expected_min_spawn_y = 60.0f; // Don't accept ground below this!

    LOG("Waiting for spawn chunk to generate...");
    while (wait_attempts < max_wait_attempts) {
        spawnY = getSurfaceHeight(0.0f, 0.0f);
        // Only break the loop if we find ground at a reasonable height
        if (spawnY >= expected_min_spawn_y) {
            break; 
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        wait_attempts++;
    }

    // This fallback logic remains the same
    if (spawnY < expected_min_spawn_y) {
        LOG("Warning: Failed to find valid spawn surface, spawning at default height.");
        spawnY = 70.0f; // Fallback spawn height
    } else {
        LOG("Spawn surface found at Y: " + std::to_string(spawnY));
    }

    // Now spawn the player at the correct height
    m_Player->reset(glm::vec3(0.0f, spawnY + 10.0f, 0.0f));
    initCelestials();
    LOG("World::Constructor - Finish");
}

World::~World() {
    LOG("World::Destructor - Start");
    m_ShouldStop = true;
    for (auto& thread : m_GenerationThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    LOG("World::Destructor - Finish");
}

void World::initCelestials() {
    // --- Stars ---
    std::vector<glm::vec3> starPositions;
    std::mt19937 rng(std::hash<std::string>{}(m_Seed));
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    m_StarVertexCount = 1000;
    for (size_t i = 0; i < m_StarVertexCount; ++i) {
        float theta = 2.0f * 3.14159f * dist(rng);
        float phi = acos(1.0f - 2.0f * dist(rng));
        starPositions.emplace_back(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
    }

    glGenVertexArrays(1, &m_StarVAO);
    glGenBuffers(1, &m_StarVBO);
    glBindVertexArray(m_StarVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_StarVBO);
    glBufferData(GL_ARRAY_BUFFER, starPositions.size() * sizeof(glm::vec3), starPositions.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(0);

    // --- Sun & Moon (a simple quad) ---
    float quadVertices[] = {
        // positions
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.5f,  0.5f, 0.0f,
         0.5f,  0.5f, 0.0f,
        -0.5f,  0.5f, 0.0f,
        -0.5f, -0.5f, 0.0f,
    };
    
    // Sun
    glGenVertexArrays(1, &m_SunVAO);
    glGenBuffers(1, &m_SunVBO);
    glBindVertexArray(m_SunVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_SunVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Moon (can use the same VBO data, just needs a separate VAO)
    glGenVertexArrays(1, &m_MoonVAO);
    glBindVertexArray(m_MoonVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_SunVBO); // Re-use the sun's VBO
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

// Add this new function to render the celestial objects each frame
void World::renderCelestials(Luminumbra::Rendering::Shader& celestialShader, const glm::mat4& view, const glm::mat4& projection) const {
    glDepthFunc(GL_LEQUAL);
    celestialShader.use();

    glm::mat4 skyView = glm::mat4(glm::mat3(view));
    celestialShader.setMat4("projection", projection);
    celestialShader.setMat4("view", skyView);

    glm::vec3 sunDir = getSunDirection();

    // --- Render Stars ---
    float starBrightness = glm::smoothstep(0.0f, -0.25f, -1*sunDir.y);
    LOG("World::renderCelestials - Star Brightness: " + std::to_string(starBrightness));
    if (starBrightness > 0.0f) {
        celestialShader.setMat4("model", glm::mat4(1.0f));
        celestialShader.setVec3("objectColor", glm::vec3(1.0f, 1.0f, 0.95f));
        celestialShader.setFloat("brightness", starBrightness);
        glBindVertexArray(m_StarVAO);
        glDrawArrays(GL_POINTS, 0, m_StarVertexCount);
    }

    glm::mat4 billboardRotation = glm::mat4(glm::mat3(view));

    // --- Render Sun ---
    LOG("World::renderCelestials - Sun Direction: " + std::to_string(sunDir.y));
    if (sunDir.y < 0.0f) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), sunDir * 100.0f); 
        model = model * billboardRotation; // Apply billboarding to face the camera
        model = glm::scale(model, glm::vec3(50.0f)); // Scale it up
        
        celestialShader.setMat4("model", model);
        celestialShader.setVec3("objectColor", glm::vec3(1.0f, 1.0f, 0.8f));
        celestialShader.setFloat("brightness", 5.0f);
        glBindVertexArray(m_SunVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    
    // --- Render Moon ---
    if (sunDir.y > 0.0f) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), -sunDir * 100.0f); 
        model = model * billboardRotation; // Apply billboarding to face the camera
        model = glm::scale(model, glm::vec3(40.0f)); // Scale it up

        celestialShader.setMat4("model", model);
        celestialShader.setVec3("objectColor", glm::vec3(0.8f, 0.8f, 0.9f));
        celestialShader.setFloat("brightness", 5.0f);
        glBindVertexArray(m_MoonVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindVertexArray(0);
    glDepthFunc(GL_LESS);
}

Core::WorldSaveData World::serialize() const {
    Core::WorldSaveData data;
    data.slotName = m_SlotName; // <-- ADD THIS
    data.seed = m_Seed;
    data.timeOfDay = m_TimeOfDay;
    return data;
}

glm::vec3 World::getSkyColor() const {
    // Define the key colors for the cycle
    glm::vec3 dayColor    = glm::vec3(0.5f, 0.8f, 1.0f);
    glm::vec3 sunsetColor = glm::vec3(1.0f, 0.6f, 0.4f);
    glm::vec3 nightColor  = glm::vec3(0.05f, 0.05f, 0.15f);

    float sunHeight = getSunDirection().y;

    // Blend between daytime and sunset colors
    // CORRECTED: Added glm:: prefix
    float dayFactor = glm::smoothstep(0.0f, 0.05f, sunHeight);
    glm::vec3 sky = glm::mix(sunsetColor, dayColor, dayFactor);

    // Blend between the current sky color and night color
    // CORRECTED: Added glm:: prefix
    float nightFactor = glm::smoothstep(-0.05f, 0.0f, -1 * sunHeight);
    sky = glm::mix(nightColor, sky, nightFactor);
    LOG("World::getSkyColor - Sun Height: " + std::to_string(sunHeight) + ", Sky Color: (" + std::to_string(sky.x) + ", " + std::to_string(sky.y) + ", " + std::to_string(sky.z) + ")");

    return sky;
}

void World::applySaveData(const Core::WorldSaveData& data) {
    // This function applies data to an ALREADY CREATED world during a load.
    // The Player's data is handled separately by the Engine.
    m_SlotName = data.slotName;
    m_Seed = data.seed;
    m_TimeOfDay = data.timeOfDay;
}

void World::setTimeOfDay(float time) {
    m_TimeOfDay = std::clamp(time, 0.0f, 1.0f);
}

glm::vec3 World::getSunDirection() const {
    float angle = m_TimeOfDay * 2.0f * 3.14159f;
    return glm::normalize(glm::vec3(sin(angle), cos(angle), 0.1f));
}

// in src/luminumbra/world/World.cpp

void World::update(float deltaTime) {
    // 1. UPLOAD STAGE: Check for chunks that worker threads have finished generating
    // and upload their mesh data to the GPU. This must be on the main thread.
    {
        std::lock_guard<std::mutex> lock(m_ChunkMutex);
        for (auto const& [pos, chunk] : m_Chunks) {
            if (chunk && chunk->isReadyForGpu()) {
                chunk->uploadToGpu();
            }
        }
    }

    // 2. STATE UPDATE STAGE: Update time, clouds, etc.
    const glm::vec3 viewerPosition = m_Player->getPosition();
    m_CloudManager->update(deltaTime);

    m_TimeOfDay += deltaTime / DAY_DURATION;
    if (m_TimeOfDay > 1.0f) {
        m_TimeOfDay -= 1.0f;
    }

    // 3. CHUNK MANAGEMENT STAGE: If the player has moved far enough,
    // queue new chunks to be generated and unload ones that are too far away.
    if (glm::length(viewerPosition - m_LastViewerPosition) > UPDATE_THRESHOLD) {
        m_LastViewerPosition = viewerPosition;
        loadChunksAroundPosition(viewerPosition);
        unloadDistantChunks(viewerPosition);
    }
}

Luminumbra::Player::Player* World::getPlayer() const {
    return m_Player.get();
}

float World::getSurfaceHeight(float x, float z) const {
    // Start checking from a high altitude and move down
    for (float y = 255.0f; y > 0.0f; --y) {
        if (isSolid(glm::vec3(x, y, z))) {
            return y;
        }
    }
    return 0.0f; // Return 0 if no ground is found (e.g., void)
}

glm::vec3 World::getSpawnPoint() const {
    // Get surface height at origin and add a small buffer for the player
    float spawnY = getSurfaceHeight(0.0f, 0.0f);
    return glm::vec3(0.0f, spawnY + 2.0f, 0.0f);
}

// (The rest of the functions from your original file remain largely unchanged)

// in src/luminumbra/world/World.cpp

void World::render(Luminumbra::Rendering::Shader& shader, const glm::mat4& view, const glm::mat4& projection, const glm::vec3& viewPos) const {
    // Set all the uniforms that are the same for every chunk
    shader.setVec3("viewPos", viewPos);
    shader.setMat4("view", view);             // <-- ADD THIS LINE
    shader.setMat4("projection", projection); // <-- ADD THIS LINE

    // Now loop through and render each chunk
    for (const auto& pair : m_Chunks) {
        const auto& chunk = pair.second;
        if (chunk) {
            float distance = getChunkDistance(pair.first, viewPos);
            if (distance <= VIEW_DISTANCE * Chunk::CHUNK_SIZE) {
                // Set the per-chunk uniform (the model matrix)
                shader.setMat4("model", chunk->getModelMatrix());
                chunk->render();
            }
        }
    }
}

void World::renderSkyboxAndClouds(const glm::mat4& view, const glm::mat4& projection) const {
    // m_Skybox->render(view, projection);
    m_CloudManager->render(view, projection);
}

bool World::isSolid(const glm::vec3& worldPosition) const {
    glm::ivec3 chunkPos = worldToChunkCoord(worldPosition) * Chunk::CHUNK_SIZE;
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_ChunkMutex));
    auto it = m_Chunks.find(chunkPos);
    if (it != m_Chunks.end() && it->second) {
        return it->second->isSolid(worldPosition);
    }
    return false; // Not solid if the chunk doesn't exist
}

const char* World::getBiome(const glm::vec3& position) const {
    // Simple biome generation based on altitude and position
    float altitude = position.y;
    float temperature = 25.0f - (altitude / 10.0f); // Decrease temp with height
    float moisture = 50.0f + sin(position.x / 100.0f) * 25.0f; // Vary moisture with x-coord

    if (altitude > 150.0f) return "Snowy Peaks";
    if (altitude > 100.0f) return "Rocky Mountains";
    if (temperature < 5.0f) return "Tundra";
    if (temperature < 15.0f && moisture > 60.0f) return "Taiga";
    if (temperature > 20.0f && moisture > 70.0f) return "Jungle";
    if (temperature > 25.0f && moisture < 30.0f) return "Desert";
    if (moisture < 40.0f) return "Plains";
    return "Forest";
}

// in src/luminumbra/world/World.cpp

void World::loadChunksAroundPosition(const glm::vec3& position) {
    glm::ivec3 centerChunk = worldToChunkCoord(position);
    
    std::lock_guard<std::mutex> queueLock(m_QueueMutex);

    // --- NEW: Load chunks in a simple square pattern ---
    // This is much more reliable than the complex spiral algorithm.
    const int VIEW_DISTANCE = 16; // Example view distance in chunks
    const int LOD_DISTANCE = 12;  // Example LOD distance

    for (int x = -VIEW_DISTANCE; x <= VIEW_DISTANCE; ++x) {
        for (int z = -VIEW_DISTANCE; z <= VIEW_DISTANCE; ++z) {
            for (int y = -3; y <= 3; ++y) { // Keep vertical range limited
                glm::ivec3 chunkCoord = centerChunk + glm::ivec3(x, y, z);
                glm::ivec3 chunkPos = chunkCoord * Chunk::CHUNK_SIZE;

                // Check if chunk already exists or is queued
                {
                    std::lock_guard<std::mutex> chunkLock(m_ChunkMutex);
                    if (m_Chunks.count(chunkPos)) {
                        continue;
                    }
                }

                // Add to load queue with priority
                float priority = getChunkDistance(chunkPos, position);
                int lod = 0; // Default LOD
                if (std::abs(x) > LOD_DISTANCE || std::abs(z) > LOD_DISTANCE) {
                    lod = 1;
                }
                
                m_ChunkLoadQueue.push({chunkPos, priority, lod});
            }
        }
    }
}

void World::unloadDistantChunks(const glm::vec3& position) {
    std::lock_guard<std::mutex> lock(m_ChunkMutex);
    
    auto it = m_Chunks.begin();
    while (it != m_Chunks.end()) {
        float distance = getChunkDistance(it->first, position);
        
        if (distance > UNLOAD_DISTANCE * Chunk::CHUNK_SIZE) {
            LOG("World::unloadDistantChunks - Unloading chunk at " + 
                std::to_string(it->first.x) + ", " + 
                std::to_string(it->first.y) + ", " + 
                std::to_string(it->first.z));
            it = m_Chunks.erase(it);
        } else {
            ++it;
        }
    }
}

void World::processChunkQueue() {
    while (!m_ShouldStop) {
        ChunkLoadRequest request;
        bool hasWork = false;
        
        // Get chunk from queue
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            if (!m_ChunkLoadQueue.empty()) {
                request = m_ChunkLoadQueue.top();
                m_ChunkLoadQueue.pop();
                hasWork = true;
            }
        }
        
        if (hasWork) {
            // Generate chunk (this is the expensive operation)
            auto chunk = std::make_unique<Chunk>(request.position, m_Seed, request.lod);
            
            // Add to world
            {
                std::lock_guard<std::mutex> lock(m_ChunkMutex);
                m_Chunks[request.position] = std::move(chunk);
            }
            
            LOG("World::processChunkQueue - Generated chunk at " + 
                std::to_string(request.position.x) + ", " + 
                std::to_string(request.position.y) + ", " + 
                std::to_string(request.position.z));
        } else {
            // No work, sleep briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

glm::ivec3 World::worldToChunkCoord(const glm::vec3& worldPos) const {
    return glm::ivec3(
        (int)std::floor(worldPos.x / (float)Chunk::CHUNK_SIZE),
        (int)std::floor(worldPos.y / (float)Chunk::CHUNK_SIZE),
        (int)std::floor(worldPos.z / (float)Chunk::CHUNK_SIZE)
    );
}

float World::getChunkDistance(const glm::ivec3& chunkPos, const glm::vec3& viewPos) const {
    glm::vec3 chunkCenter = glm::vec3(chunkPos) + glm::vec3(Chunk::CHUNK_SIZE * 0.5f);
    return glm::length(chunkCenter - viewPos);
}

} // namespace Luminumbra::World
