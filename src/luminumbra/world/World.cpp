#include "luminumbra/world/World.h"
#include "luminumbra/world/Chunk.h"
#include "luminumbra/player/Player.h"
#include "luminumbra/rendering/Shader.h"
#include "luminumbra/rendering/Skybox.h"
#include "luminumbra/rendering/CloudManager.h"
#include "luminumbra/core/Debug.h"
#include "luminumbra/rendering/particles/Particle.h"
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
    m_ParticleSystem = std::make_unique<Luminumbra::Rendering::ParticleSystem>();
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

    float spawnY = 30.0f;
    int wait_attempts = 0;
    const int max_wait_attempts = 100; // Wait for max 5 seconds
    const float expected_min_spawn_y = 0.0f; // Don't accept ground below this!

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
    m_Player->reset(glm::vec3(0.0f, spawnY + 2.0f, 0.0f));
    initCelestials();
    initFoliage();
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
    // NEW: Clean up foliage resources
    glDeleteVertexArrays(1, &m_TreeTrunkVAO);
    glDeleteBuffers(1, &m_TreeTrunkVBO);
    glDeleteBuffers(1, &m_TreeTrunkEBO);
    glDeleteVertexArrays(1, &m_TreeLeavesVAO);
    glDeleteBuffers(1, &m_TreeLeavesVBO);
    glDeleteBuffers(1, &m_TreeLeavesEBO);
    glDeleteVertexArrays(1, &m_BushVAO);
    glDeleteBuffers(1, &m_BushVBO);
    glDeleteBuffers(1, &m_BushEBO);
    glDeleteBuffers(1, &m_FoliageInstanceVBO);
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

struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
};


void World::initFoliage() {
    // A local struct to hold interleaved vertex data for position and normal
    struct Vertex {
        glm::vec3 Position;
        glm::vec3 Normal;
    };

    // --- Tree Trunk (Cylinder) ---
    std::vector<Vertex> trunkVertices;
    std::vector<unsigned int> trunkIndices;
    const int segments = 8;
    const float radius = 0.5f;
    const float height = 8.0f;
    for (int i = 0; i < segments; ++i) {
        float angle = (float)i / segments * 2.0f * glm::pi<float>();
        float x = cos(angle) * radius;
        float z = sin(angle) * radius;
        // The normal for a cylinder vertex points horizontally out from the center
        glm::vec3 normal = glm::normalize(glm::vec3(x, 0.0f, z)); 
        trunkVertices.push_back({{x, -1*0.5, z}, normal});      // Bottom vertex
        trunkVertices.push_back({{x, height, z}, normal}); // Top vertex
    }
    for (unsigned int i = 0; i < segments; ++i) {
        unsigned int b_l = i * 2;       // bottom-left
        unsigned int t_l = b_l + 1;     // top-left
        unsigned int b_r = ((i + 1) % segments) * 2; // bottom-right
        unsigned int t_r = b_r + 1;     // top-right
        
        // Define triangles with Counter-Clockwise (CCW) winding order
        trunkIndices.insert(trunkIndices.end(), {b_l, t_l, b_r});
        trunkIndices.insert(trunkIndices.end(), {b_r, t_l, t_r});
    }
    m_TreeTrunkIndexCount = trunkIndices.size();

    // --- Tree Leaves & Bush (Icosphere) ---
    std::vector<Vertex> icoVertices;
    const float t = (1.0f + sqrt(5.0f)) / 2.0f;
    std::vector<glm::vec3> initialPositions = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
        {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}
    };
    for(auto& pos : initialPositions) {
        // For a sphere centered at the origin, the normal is just the normalized position vector
        glm::vec3 normal = glm::normalize(pos); 
        glm::vec3 scaledPos = normal * 4.0f; // Scale the icosphere model
        scaledPos.y += 7.0f;                 // Sit canopy on top of the trunk
        icoVertices.push_back({scaledPos, normal});
    }
    std::vector<unsigned int> icoIndices = {
        0, 11, 5,  0, 5, 1,  0, 1, 7,  0, 7, 10,  0, 10, 11,
        1, 5, 9,  5, 11, 4, 11, 10, 2, 10, 7, 6,  7, 1, 8,
        3, 9, 4,  3, 4, 2,  3, 2, 6,  3, 6, 8,  3, 8, 9,
        4, 9, 5,  2, 4, 11, 6, 2, 10, 8, 6, 7,  9, 8, 1
    };
    m_TreeLeavesIndexCount = m_BushIndexCount = icoIndices.size();

    // --- GPU Buffer and VAO Setup ---
    
    // Create VBOs and EBOs
    glGenBuffers(1, &m_TreeTrunkVBO);
    glGenBuffers(1, &m_TreeTrunkEBO);
    glGenBuffers(1, &m_TreeLeavesVBO);
    glGenBuffers(1, &m_TreeLeavesEBO);
    // Instance VBO is shared
    glGenBuffers(1, &m_FoliageInstanceVBO);
    
    // Setup Tree Trunk VAO
    glGenVertexArrays(1, &m_TreeTrunkVAO);
    glBindVertexArray(m_TreeTrunkVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_TreeTrunkVBO);
    glBufferData(GL_ARRAY_BUFFER, trunkVertices.size() * sizeof(Vertex), trunkVertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_TreeTrunkEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, trunkIndices.size() * sizeof(unsigned int), trunkIndices.data(), GL_STATIC_DRAW);
    // Location 0: Vertex Position
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Position));
    // Location 1: Vertex Normal
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));

    // Setup Tree Leaves VAO
    glGenVertexArrays(1, &m_TreeLeavesVAO);
    glBindVertexArray(m_TreeLeavesVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_TreeLeavesVBO);
    glBufferData(GL_ARRAY_BUFFER, icoVertices.size() * sizeof(Vertex), icoVertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_TreeLeavesEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, icoIndices.size() * sizeof(unsigned int), icoIndices.data(), GL_STATIC_DRAW);
    // Location 0: Vertex Position
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Position));
    // Location 1: Vertex Normal
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));

    // Setup Bush VAO (reuses leaves mesh data)
    glGenVertexArrays(1, &m_BushVAO);
    glBindVertexArray(m_BushVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_TreeLeavesVBO); // Use same VBO
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_TreeLeavesEBO); // Use same EBO
    // Location 0: Vertex Position
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Position));
    // Location 1: Vertex Normal
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));

    // --- Instance VBO Setup for all Foliage VAOs ---
    glBindBuffer(GL_ARRAY_BUFFER, m_FoliageInstanceVBO);
    // Allocate space for instance matrices (will be filled each frame)
    glBufferData(GL_ARRAY_BUFFER, 10000 * sizeof(glm::mat4), nullptr, GL_STREAM_DRAW);

    for (auto vao : {m_TreeTrunkVAO, m_TreeLeavesVAO, m_BushVAO}) {
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_FoliageInstanceVBO);
        // Set up the instanced model matrix, starting at attribute location 2
        for (int i = 0; i < 4; ++i) {
            // Locations 2, 3, 4, 5 for the four vec4s of the mat4
            glEnableVertexAttribArray(2 + i);
            glVertexAttribPointer(2 + i, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(sizeof(glm::vec4) * i));
            // Tell OpenGL this is an instanced vertex attribute.
            glVertexAttribDivisor(2 + i, 1);
        }
    }
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
    if (starBrightness > 0.0f) {
        celestialShader.setMat4("model", glm::mat4(1.0f));
        celestialShader.setVec3("objectColor", glm::vec3(1.0f, 1.0f, 0.95f));
        celestialShader.setFloat("brightness", starBrightness);
        glBindVertexArray(m_StarVAO);
        glDrawArrays(GL_POINTS, 0, m_StarVertexCount);
    }

    glm::mat4 billboardRotation = glm::mat4(glm::mat3(view));

    // --- Render Sun ---
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

    // Step 1: Blend between pure day color and sunset color.
    // This factor is 0.0 at the horizon (sunset) and 1.0 higher up (full day).
    float dayFactor = glm::smoothstep(0.0f, 0.2f, sunHeight);
    glm::vec3 daySunsetMix = glm::mix(sunsetColor, dayColor, dayFactor);

    // Step 2: Blend the result of Step 1 with the night color.
    // This factor is 0.0 at night and 1.0 during the day/sunset.
    float lightFactor = glm::smoothstep(-0.15f, 0.0f, sunHeight);
    glm::vec3 finalSky = glm::mix(nightColor, daySunsetMix, lightFactor);

    return finalSky;
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

#include <glm/gtc/constants.hpp> // For glm::two_pi()

glm::vec3 World::getSunDirection() const {
    // m_TimeOfDay is a value from 0.0 (midnight) to 1.0 (next midnight)
    float angle = m_TimeOfDay * glm::two_pi<float>();

    glm::vec3 direction;
    
    // Use -cos(angle) for height.
    // -cos(0) = -1.0 (midnight)
    // -cos(pi) = +1.0 (midday at time = 0.5)
    // -cos(2*pi) = -1.0 (next midnight at time = 1.0)
    direction.y = -glm::cos(angle);

    // Use sin(angle) for east-west movement along the X-axis.
    direction.x = glm::sin(angle);
    
    // Optional: Tilt the sun's path on the Z-axis so it's not directly overhead.
    direction.z = 0.3f;

    // Return a normalized vector, as is standard for directions.
    return glm::normalize(direction);
}

void World::update(float deltaTime) {
    // 1. GPU UPLOAD STAGE
    // Upload mesh data for newly generated chunks.
    {
        std::lock_guard<std::mutex> lock(m_ChunkMutex);
        for (auto const& [pos, chunk] : m_Chunks) {
            if (chunk && chunk->isReadyForGpu()) {
                chunk->uploadToGpu();
            }
        }
    }

    // 2. CORE STATE UPDATE
    // Update player position (for this frame's logic) and time of day.
    const glm::vec3 viewerPosition = m_Player->getPosition();
    m_TimeOfDay += deltaTime / DAY_DURATION;
    if (m_TimeOfDay > 1.0f) {
        m_TimeOfDay -= 1.0f;
    }

    // 3. GAMEPLAY & EFFECTS LOGIC
    // A) Emit new particles based on the current world state (rain, fire, etc.).
    updateParticles(deltaTime);

    // B) Update the particle system and process death events (e.g., rain splashes).
    auto deathEvents = m_ParticleSystem->update(deltaTime, *this);
    for (const auto& event : deathEvents) {
        if (event.type == Luminumbra::Rendering::ParticleType::Rain) {
            // Create 5 splash droplets on impact
            for (int i = 0; i < 5; ++i) {
                Luminumbra::Rendering::ParticleProps splashProps = m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::Splash);
                splashProps.position = event.position;
                m_ParticleSystem->emit(splashProps);
            }
        }
    }

    // C) Update other dynamic systems like clouds.
    m_CloudManager->update(deltaTime);


    // 4. CHUNK MANAGEMENT STAGE
    // Check if the player has moved far enough to load/unload chunks.
    if (glm::length(viewerPosition - m_LastViewerPosition) > UPDATE_THRESHOLD) {
        m_LastViewerPosition = viewerPosition;
        loadChunksAroundPosition(viewerPosition);
        unloadDistantChunks(viewerPosition);
    }
}

void World::setWeather(WeatherType type) {
    m_CurrentWeather = type;
}

void World::startFireNearPlayer() {
    glm::vec3 playerPos = m_Player->getPosition();
    float closestDist = std::numeric_limits<float>::max();
    BurningTree closestTree;
    bool foundTree = false;

    // Find the closest tree to the player (within 128 units)
    for (const auto& pair : m_Chunks) {
        if (getChunkDistance(pair.first, playerPos) > 128.0f) continue;
        
        const auto& treeInstances = pair.second->getTreeInstances();
        for (size_t i = 0; i < treeInstances.size(); ++i) {
            glm::vec3 treePos = glm::vec3(pair.second->getModelMatrix() * glm::vec4(treeInstances[i].position, 1.0));
            float dist = glm::distance(playerPos, treePos);
            if (dist < closestDist) {
                closestDist = dist;
                closestTree = {pair.first, i, 0.0f};
                foundTree = true;
            }
        }
    }

    if (foundTree) {
        // Check if the tree is already in the vector before adding it
        if (std::find(m_BurningTrees.begin(), m_BurningTrees.end(), closestTree) == m_BurningTrees.end()) {
            m_BurningTrees.push_back(closestTree);
        }
    }
}

void World::teleportToEffect(const std::string& effect) {
    if (!m_Player) return;
    
    glm::vec3 targetPos;
    if (effect == "campfire") {
        targetPos = CAMPFIRE_POS + glm::vec3(0.0f, 2.0f, 0.0f);
    }
    else if (effect == "portal") {
        targetPos = PORTAL_POS + glm::vec3(0.0f, 2.0f, 0.0f);
    }
    else if (effect == "aurora") {
        targetPos = AURORA_HEIGHT + glm::vec3(0.0f, -150.0f, 0.0f);
    }
    else if (effect == "clouds") {
        targetPos = CLOUD_SPAWN_POINTS[0] + glm::vec3(0.0f, -20.0f, 0.0f);
    }
    
    m_Player->setPosition(targetPos);
}

void World::updateParticles(float deltaTime) {
    static std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    static std::mt19937 gen(std::random_device{}());

    // 1. Weather System: Rain
    if (m_CurrentWeather == WeatherType::Rainy) {
        int rainDensity = 20;
        for (int i = 0; i < rainDensity; ++i) {
            // Get preset and customize position
            Luminumbra::Rendering::ParticleProps props = m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::Rain);
            float offsetX = (dist(gen) - 0.5f) * 80.0f;
            float offsetZ = (dist(gen) - 0.5f) * 80.0f;
            props.position = m_Player->getPosition() + glm::vec3(offsetX, 50.0f, offsetZ);
            m_ParticleSystem->emit(props);
        }

        if (!m_BurningTrees.empty()) {
            for (const auto& tree : m_BurningTrees) {
                 auto chunkIt = m_Chunks.find(tree.chunkCoord);
                 if (chunkIt == m_Chunks.end()) continue;
                 glm::vec3 treePos = glm::vec3(chunkIt->second->getModelMatrix() * glm::vec4(chunkIt->second->getTreeInstances()[tree.treeIndex].position, 1.0));
                
                 // Emit a final puff of smoke as the fire goes out
                 for (int i = 0; i < 20; ++i) {
                    Luminumbra::Rendering::ParticleProps smokeProps = m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::Smoke);
                    smokeProps.position = treePos + glm::vec3((dist(gen) - 0.5f) * 5.0f, 6.0f + dist(gen) * 5.0f, (dist(gen) - 0.5f) * 5.0f);
                    smokeProps.lifeTime = 1.5f; // Short-lived puff
                    m_ParticleSystem->emit(smokeProps);
                 }
            }
            m_BurningTrees.clear();
        }
    } else if (m_CurrentWeather == WeatherType::Clear) {
        m_LeafEmitTimer += deltaTime;
        if (m_LeafEmitTimer > 0.1f) {
            m_LeafEmitTimer = 0.0f;

            // Loop through chunks near the player
            for (const auto& pair : m_Chunks) {
                if (getChunkDistance(pair.first, m_Player->getPosition()) > 128.0f) continue;
                
                // Check trees within the chunk
                for (const auto& tree : pair.second->getTreeInstances()) {
                    if (dist(gen) < 0.05f) { // 5% chance per update cycle
                        // Get the preset for a leaf and set its position
                        Luminumbra::Rendering::ParticleProps props = m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::Leaf);
                        glm::vec3 treePos = glm::vec3(pair.second->getModelMatrix() * glm::vec4(tree.position, 1.0));
                        props.position = treePos + glm::vec3((dist(gen) - 0.5f) * 4.0f, 7.0f, (dist(gen) - 0.5f) * 4.0f);
                        m_ParticleSystem->emit(props);
                    }
                }
            }
        }
    }

    // Cloud System
    {
        static float cloudTimer = 0.0f;
        cloudTimer += deltaTime;

        // Cloud spawn points in a grid pattern
        const glm::vec3 CLOUD_BASE_POSITIONS[] = {
            glm::vec3(100.0f, 100.0f, 100.0f),
            glm::vec3(-100.0f, 120.0f, -100.0f),
            glm::vec3(0.0f, 110.0f, 200.0f),
            glm::vec3(200.0f, 115.0f, -150.0f),
            glm::vec3(-150.0f, 105.0f, 150.0f)
        };

        if (cloudTimer > 0.5f) {  // Spawn more frequently
            cloudTimer = 0.0f;
            
            for (const auto& basePos : CLOUD_BASE_POSITIONS) {
                // Create main cloud body with multiple layers
                for (int i = 0; i < 3; i++) { // Multiple particles per cloud
                    Luminumbra::Rendering::ParticleProps cloudProps = 
                        m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::Cloud);
                    
                    // Vary the position within the cloud volume
                    glm::vec3 offset = glm::vec3(
                        dist(gen) * 30.0f - 15.0f,  // Wider spread
                        dist(gen) * 15.0f - 7.5f,   // Variable height
                        dist(gen) * 30.0f - 15.0f   // Wider spread
                    );
                    
                    cloudProps.position = basePos + offset;
                    cloudProps.isVolumetric = true;
                    cloudProps.volumetricLayers = 16;  // More layers for better volume
                    cloudProps.volumeDepth = 12.0f;    // Deeper volume
                    
                    // Vary the colors slightly for more natural look
                    float whiteness = 0.95f + dist(gen) * 0.05f;
                    cloudProps.colorBegin = glm::vec4(whiteness, whiteness, whiteness, 0.3f);
                    cloudProps.colorEnd = glm::vec4(whiteness * 0.9f, whiteness * 0.9f, whiteness * 0.95f, 0.0f);
                    
                    // Larger size variation
                    cloudProps.sizeBegin = 35.0f + dist(gen) * 15.0f;
                    cloudProps.sizeEnd = cloudProps.sizeBegin * 1.2f;
                    
                    // Longer lifetime
                    cloudProps.lifeTime = 8.0f + dist(gen) * 4.0f;
                    
                    // Gentle random movement
                    cloudProps.velocity = glm::vec3(
                        dist(gen) * 2.0f - 1.0f,
                        dist(gen) * 0.5f - 0.25f,
                        dist(gen) * 2.0f - 1.0f
                    );
                    
                    m_ParticleSystem->emit(cloudProps);
                }
            }
        }
    }

    // Volumetric Fire Test Zone
    {
        static const glm::vec3 campfirePos(50.0f, 10.0f, 50.0f);
        
        // Main fire
        if (dist(gen) < 0.3f) {
            Luminumbra::Rendering::ParticleProps fireProps = 
                m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::VolumetricFire);
            fireProps.position = campfirePos + glm::vec3(
                dist(gen) * 2.0f - 1.0f,
                0.0f,
                dist(gen) * 2.0f - 1.0f
            );
            m_ParticleSystem->emit(fireProps);
            
            // Add volumetric smoke above the fire
            Luminumbra::Rendering::ParticleProps smokeProps = 
                m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::VolumetricSmoke);
            smokeProps.position = fireProps.position + glm::vec3(0.0f, 3.0f, 0.0f);
            m_ParticleSystem->emit(smokeProps);
        }
    }

    // Magic Portal Test Zone
    {
        static const glm::vec3 portalCenter(0.0f, 20.0f, 0.0f);
        static float portalAngle = 0.0f;
        portalAngle += deltaTime;

        // Create a swirling portal effect
        if (dist(gen) < 0.4f) {
            float radius = 3.0f;
            float angle = portalAngle + dist(gen) * glm::two_pi<float>();
            
            glm::vec3 offset(
                cos(angle) * radius,
                sin(angle * 2.0f) * 2.0f,  // Figure-8 pattern
                sin(angle) * radius
            );

            Luminumbra::Rendering::ParticleProps portalProps = 
                m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::Magic);
            portalProps.isVolumetric = true;
            portalProps.volumetricLayers = 6;
            portalProps.volumeDepth = 2.0f;
            portalProps.position = portalCenter + offset;
            portalProps.velocity = glm::vec3(0.0f);
            portalProps.colorBegin = glm::vec4(0.5f + 0.5f * sin(angle), 0.2f, 0.8f, 0.8f);
            portalProps.sizeBegin = 0.5f;
            portalProps.sizeEnd = 0.1f;
            portalProps.lifeTime = 1.0f;
            
            m_ParticleSystem->emit(portalProps);
        }
    }

    // Aurora Borealis Test Zone
    {
        static const float auroraY = 200.0f;
        static float auroraTime = 0.0f;
        auroraTime += deltaTime;

        if (dist(gen) < 0.2f) {
            float baseX = sin(auroraTime * 0.1f) * 100.0f;
            
            for (int i = 0; i < 3; i++) {
                Luminumbra::Rendering::ParticleProps auroraProps = 
                    m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::Magic);
                auroraProps.isVolumetric = true;
                auroraProps.volumetricLayers = 4;
                auroraProps.volumeDepth = 10.0f;
                
                float x = baseX + dist(gen) * 200.0f - 100.0f;
                float z = dist(gen) * 200.0f - 100.0f;
                float wave = sin(x * 0.02f + auroraTime) * 20.0f;
                
                auroraProps.position = glm::vec3(x, auroraY + wave, z);
                auroraProps.velocity = glm::vec3(0.0f, sin(auroraTime + x * 0.1f) * 2.0f, 0.0f);
                auroraProps.colorBegin = glm::vec4(0.1f, 0.8f, 0.3f, 0.3f);
                auroraProps.colorEnd = glm::vec4(0.2f, 0.5f, 0.8f, 0.0f);
                auroraProps.sizeBegin = 20.0f;
                auroraProps.sizeEnd = 25.0f;
                auroraProps.lifeTime = 4.0f;
                
                m_ParticleSystem->emit(auroraProps);
            }
        }
    }
    
    // Fire & Spark System for Trees
    for (auto& tree : m_BurningTrees) {
        tree.timeBurning += deltaTime;
        
        auto chunkIt = m_Chunks.find(tree.chunkCoord);
        if (chunkIt == m_Chunks.end() || tree.treeIndex >= chunkIt->second->getTreeInstances().size()) continue;
        glm::vec3 treePos = glm::vec3(chunkIt->second->getModelMatrix() * 
            glm::vec4(chunkIt->second->getTreeInstances()[tree.treeIndex].position, 1.0));

        // Emit volumetric smoke
        if (dist(gen) < 0.3f) {
            Luminumbra::Rendering::ParticleProps smokeProps = 
                m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::VolumetricSmoke);
            smokeProps.position = treePos + glm::vec3(
                (dist(gen) - 0.5f) * 3.0f,  // Spread around tree
                6.0f + dist(gen) * 4.0f,     // Height variation
                (dist(gen) - 0.5f) * 3.0f    // Spread around tree
            );
            smokeProps.volumetricLayers = 12;  // More layers for better volume
            smokeProps.volumeDepth = 4.0f;     // Wider smoke column
            smokeProps.sizeBegin = 3.0f + dist(gen);  // Larger initial size
            smokeProps.sizeEnd = 6.0f + dist(gen) * 2.0f;  // Even larger as it rises
            smokeProps.lifeTime = 4.0f + dist(gen) * 2.0f;  // Longer lifetime
            m_ParticleSystem->emit(smokeProps);
        }

        if (tree.timeBurning > 1.0f) {  // Start fire after initial smoke
            // Emit volumetric fire
            if (dist(gen) < 0.4f) {
                Luminumbra::Rendering::ParticleProps fireProps = 
                    m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::VolumetricFire);
                fireProps.position = treePos + glm::vec3(
                    (dist(gen) - 0.5f) * 4.0f,  // Spread around tree
                    dist(gen) * 8.0f,           // Variable height up the tree
                    (dist(gen) - 0.5f) * 4.0f   // Spread around tree
                );
                fireProps.volumetricLayers = 16;  // More layers for better volume
                fireProps.volumeDepth = 3.0f;     // Deeper fire effect
                fireProps.sizeBegin = 2.0f + dist(gen);  // Larger flames
                fireProps.sizeEnd = 4.0f + dist(gen) * 2.0f;
                fireProps.lifeTime = 2.0f + dist(gen);
                m_ParticleSystem->emit(fireProps);
            }

            // Add some sparks for extra effect
            if (dist(gen) < 0.1f) {
                Luminumbra::Rendering::ParticleProps sparkProps = 
                    m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::Spark);
                sparkProps.position = treePos + glm::vec3(
                    (dist(gen) - 0.5f) * 3.0f,
                    3.0f + dist(gen) * 6.0f,
                    (dist(gen) - 0.5f) * 3.0f
                );
                m_ParticleSystem->emit(sparkProps);
            }
        }

        // Handle rain extinguishing the fire
        if (m_CurrentWeather == WeatherType::Rainy) {
            // Final puff of volumetric smoke as fire goes out
            for (int i = 0; i < 15; ++i) {
                Luminumbra::Rendering::ParticleProps smokeProps = 
                    m_ParticleSystem->getPresetProperties(Luminumbra::Rendering::ParticleType::VolumetricSmoke);
                smokeProps.position = treePos + glm::vec3(
                    (dist(gen) - 0.5f) * 5.0f,
                    4.0f + dist(gen) * 6.0f,
                    (dist(gen) - 0.5f) * 5.0f
                );
                smokeProps.lifeTime = 2.0f;
                smokeProps.colorBegin.a = 0.4f;  // More opaque smoke for dramatic effect
                m_ParticleSystem->emit(smokeProps);
            }
        }
    }
}

Luminumbra::Player::Player* World::getPlayer() const {
    return m_Player.get();
}

float World::getSurfaceHeight(float x, float z) const {
    // Start from the top of the world and check downwards
    for (float y = 255.0f; y >= 0.0f; --y) {
        if (isSolid(glm::vec3(x, y, z))) {
            // This is the highest solid block. The surface is just above it.
            return y;
        }
    }
    // If no solid ground is found, return 0
    return 0.0f;
}

glm::vec3 World::getSpawnPoint() const {
    // Get surface height at origin and add a small buffer for the player
    float spawnY = getSurfaceHeight(0.0f, 0.0f);
    return glm::vec3(0.0f, spawnY + 2.0f, 0.0f);
}

void World::renderTerrain(Rendering::Shader& shader, const glm::vec3& viewPos) const {
    for (const auto& pair : m_Chunks) {
        const auto& chunk = pair.second;
        if (chunk) {
            float distance = getChunkDistance(pair.first, viewPos);
            if (distance <= VIEW_DISTANCE * Chunk::CHUNK_SIZE) {
                // FIX: Changed "model" to "u_model" to match the shader
                shader.setMat4("u_model", chunk->getModelMatrix()); 
                chunk->renderTerrain();
            }
        }
    }
}

void World::renderWater(Rendering::Shader& shader, const glm::vec3& viewPos) const {
    for (const auto& [position, chunk] : m_Chunks) {
        // Set the unique model matrix for this specific chunk
        shader.setMat4("u_Model", chunk->getModelMatrix());
        
        // Now tell the chunk to draw itself
        chunk->renderWater(); 
    }
}

void World::renderFoliage(Luminumbra::Rendering::Shader& foliageShader) const {
    foliageShader.use();
    foliageShader.setMat4("projection", m_Player->getCamera().getProjectionMatrix());
    foliageShader.setMat4("view", m_Player->getCamera().getViewMatrix());

    std::vector<glm::mat4> treeMatrices;
    std::vector<glm::mat4> bushMatrices;

    // Collect all instance data from visible chunks
    for (const auto& pair : m_Chunks) {
        const auto& chunk = pair.second;
        if (chunk) {
            float distance = getChunkDistance(pair.first, m_Player->getPosition());
            if (distance <= (VIEW_DISTANCE - 1) * Chunk::CHUNK_SIZE) {
                for (const auto& inst : chunk->getTreeInstances()) {
                    glm::mat4 model = glm::translate(chunk->getModelMatrix(), inst.position);
                    model = glm::rotate(model, inst.rotationY, glm::vec3(0, 1, 0));
                    model = glm::scale(model, glm::vec3(inst.scale));
                    treeMatrices.push_back(model);
                }
                for (const auto& inst : chunk->getBushInstances()) {
                    glm::mat4 model = glm::translate(chunk->getModelMatrix(), inst.position);
                    model = glm::rotate(model, inst.rotationY, glm::vec3(0, 1, 0));
                    model = glm::scale(model, glm::vec3(inst.scale));
                    bushMatrices.push_back(model);
                }
            }
        }
    }

    if (treeMatrices.empty() && bushMatrices.empty()) return;

    glBindBuffer(GL_ARRAY_BUFFER, m_FoliageInstanceVBO);
    glBufferData(GL_ARRAY_BUFFER, (treeMatrices.size() + bushMatrices.size()) * sizeof(glm::mat4), nullptr, GL_STREAM_DRAW);

    // Render Trees
    if (!treeMatrices.empty()) {
        glBufferSubData(GL_ARRAY_BUFFER, 0, treeMatrices.size() * sizeof(glm::mat4), treeMatrices.data());
        // Trunk
        foliageShader.setVec3("objectColor", glm::vec3(0.4f, 0.26f, 0.13f));
        glBindVertexArray(m_TreeTrunkVAO);
        glDrawElementsInstanced(GL_TRIANGLES, m_TreeTrunkIndexCount, GL_UNSIGNED_INT, 0, treeMatrices.size());
        // Leaves
        foliageShader.setVec3("objectColor", glm::vec3(0.13f, 0.54f, 0.13f));
        glBindVertexArray(m_TreeLeavesVAO);
        glDrawElementsInstanced(GL_TRIANGLES, m_TreeLeavesIndexCount, GL_UNSIGNED_INT, 0, treeMatrices.size());
    }

    // Render Bushes
    if (!bushMatrices.empty()) {
        glBufferSubData(GL_ARRAY_BUFFER, treeMatrices.size() * sizeof(glm::mat4), bushMatrices.size() * sizeof(glm::mat4), bushMatrices.data());
        foliageShader.setVec3("objectColor", glm::vec3(0.2f, 0.6f, 0.2f));
        glBindVertexArray(m_BushVAO);
        glDrawElementsInstanced(GL_TRIANGLES, m_BushIndexCount, GL_UNSIGNED_INT, 0, bushMatrices.size());
    }

    glBindVertexArray(0);
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
    const int LOD_DISTANCE = 8;  // Example LOD distance in chunks

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
