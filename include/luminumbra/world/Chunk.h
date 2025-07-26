// include/luminumbra/world/Chunk.h
#pragma once

#include <vector>
#include <glm/glm.hpp>

struct fnl_state; 

namespace Luminumbra::World {

class Chunk {
public:
    Chunk(const glm::ivec3& position);
    ~Chunk();

    // No copying chunks
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    
    void render() const;

    const glm::mat4& getModelMatrix() const { return m_ModelMatrix; }

private:
    void generateNoiseData(fnl_state& noise);
    void generateMesh();

    glm::ivec3 m_Position;
    glm::mat4 m_ModelMatrix;

    // Rendering
    unsigned int m_VAO = 0, m_VBO = 0;
    int m_VertexCount = 0;

    // Voxel data
    std::vector<float> m_NoiseData;
    static constexpr int CHUNK_SIZE = 32;
};

} // namespace Luminumbra::World