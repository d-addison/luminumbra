// src/luminumbra/world/World.cpp
#include "luminumbra/world/World.h"
#include "luminumbra/world/Chunk.h"
#include "luminumbra/rendering/Shader.h"

namespace Luminumbra::World {

World::World() {
    // For now, let's create a static 3x1x3 grid of chunks
    const int renderDistance = 1; // 1 chunk in each direction from the center
    for (int x = -renderDistance; x <= renderDistance; ++x) {
        for (int z = -renderDistance; z <= renderDistance; ++z) {
            glm::ivec3 pos(x * Chunk::CHUNK_SIZE, 0, z * Chunk::CHUNK_SIZE);
            m_Chunks[pos] = std::make_unique<Chunk>(pos);
        }
    }
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