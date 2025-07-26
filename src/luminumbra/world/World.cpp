// src/luminumbra/world/World.cpp
#include "luminumbra/world/World.h"
#include "luminumbra/world/Chunk.h"
#include "luminumbra/rendering/Shader.h"

namespace Luminumbra::World {

World::World() {
    // Create a larger initial world (5x3x5 chunks)
    for (int x = -2; x <= 2; ++x) {
        for (int y = -1; y <= 1; ++y) {
            for (int z = -2; z <= 2; ++z) {
                glm::ivec3 pos(x * Chunk::CHUNK_SIZE, y * Chunk::CHUNK_SIZE, z * Chunk::CHUNK_SIZE);
                m_Chunks[pos] = std::make_unique<Chunk>(pos);
            }
        }
    }
}

bool World::isSolid(const glm::vec3& worldPosition) const {
    // Calculate which chunk grid position contains this world position
    glm::ivec3 chunkCoord = glm::floor(worldPosition / (float)Chunk::CHUNK_SIZE);
    // Convert to actual chunk position (chunks are stored at positions like 0, 32, 64, etc.)
    glm::ivec3 chunkPos = chunkCoord * Chunk::CHUNK_SIZE;
    
    auto it = m_Chunks.find(chunkPos);
    if (it != m_Chunks.end()) {
        return it->second->isSolid(worldPosition);
    }
    return false; // Or true, depending on desired behavior for outside-of-world
}

World::~World() = default; // unique_ptr handles cleanup

void World::render(Luminumbra::Rendering::Shader& shader) const {
    for (const auto& pair : m_Chunks) {
        const auto& chunk = pair.second;
        shader.setMat4("model", chunk->getModelMatrix());
        chunk->render();
    }
}

} // namespace Luminumbra::World
