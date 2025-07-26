// src/luminumbra/world/Chunk.cpp
#include "luminumbra/world/Chunk.h"
#include "luminumbra/world/MarchingCubes.h"
#include "FastNoiseLite.h"
#include "luminumbra/core/Debug.h"

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

#include <vector>

namespace Luminumbra::World {

Chunk::Chunk(const glm::ivec3& position) : m_Position(position) {
    LOG("Chunk::Constructor - Start");
    m_ModelMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(m_Position));

    // Create and configure noise state using the C API
    fnl_state noise = fnlCreateState();
    noise.noise_type = FNL_NOISE_OPENSIMPLEX2;
    noise.frequency = 0.02f;
    LOG("Chunk::Constructor - Noise state created.");

    generateNoiseData(noise); // Pass the struct by reference
    LOG("Chunk::Constructor - Noise data generated.");
    generateMesh();
    LOG("Chunk::Constructor - Mesh generated.");
    LOG("Chunk::Constructor - Finish");
}

Chunk::~Chunk() {
    glDeleteVertexArrays(1, &m_VAO);
    glDeleteBuffers(1, &m_VBO);
}

void Chunk::generateNoiseData(fnl_state& noise) { // Updated function signature
    LOG("Chunk::generateNoiseData - Start");
    m_NoiseData.resize((CHUNK_SIZE + 1) * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1));
    
    // Create multiple noise states for different features
    fnl_state terrainNoise = noise;
    terrainNoise.frequency = 0.01f;
    terrainNoise.octaves = 4;
    terrainNoise.lacunarity = 2.0f;
    terrainNoise.gain = 0.5f;
    
    fnl_state caveNoise = fnlCreateState();
    caveNoise.noise_type = FNL_NOISE_PERLIN;
    caveNoise.frequency = 0.03f;
    caveNoise.octaves = 2;
    
    fnl_state detailNoise = fnlCreateState();
    detailNoise.noise_type = FNL_NOISE_VALUE;
    detailNoise.frequency = 0.1f;
    
    for (int x = 0; x <= CHUNK_SIZE; ++x) {
        for (int y = 0; y <= CHUNK_SIZE; ++y) {
            for (int z = 0; z <= CHUNK_SIZE; ++z) {
                float worldX = (float)(m_Position.x + x);
                float worldY = (float)(m_Position.y + y);
                float worldZ = (float)(m_Position.z + z);

                // Base terrain height using 2D noise for more consistent ground
                float terrainHeight = fnlGetNoise2D(&terrainNoise, worldX * 0.5f, worldZ * 0.5f) * 20.0f + 30.0f;
                
                // Base density based on height
                float density = (terrainHeight - worldY) * 0.1f;
                
                // Add 3D noise for terrain variation
                density += fnlGetNoise3D(&terrainNoise, worldX, worldY * 0.5f, worldZ) * 5.0f;
                
                // Add caves (subtract density where cave noise is high)
                float caveValue = fnlGetNoise3D(&caveNoise, worldX, worldY, worldZ);
                if (caveValue > 0.4f && worldY < terrainHeight - 5.0f) {
                    density -= (caveValue - 0.4f) * 20.0f;
                }
                
                // Add surface detail
                if (worldY < terrainHeight + 10.0f && worldY > terrainHeight - 10.0f) {
                    density += fnlGetNoise3D(&detailNoise, worldX * 2.0f, worldY * 2.0f, worldZ * 2.0f) * 2.0f;
                }
                
                // Create overhangs and cliffs
                if (worldY > 20.0f && worldY < 40.0f) {
                    float overhangNoise = fnlGetNoise2D(&terrainNoise, worldX * 0.1f, worldZ * 0.1f);
                    if (overhangNoise > 0.3f) {
                        density += sin((worldY - 20.0f) * 0.3f) * overhangNoise * 5.0f;
                    }
                }
                
                int index = x + z * (CHUNK_SIZE + 1) + y * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1);
                m_NoiseData[index] = density;
            }
        }
    }
    LOG("Chunk::generateNoiseData - Finish");
}

// Helper array to get the 8 corner offsets of a cube.
const glm::ivec3 cornerOffsets[8] = {
    {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1},
    {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}
};

void Chunk::generateMesh() {
    LOG("Chunk::generateMesh - Start");
    std::vector<MarchingCubes::Triangle> triangles;
    float isolevel = 0.0f; // The "surface" level of our noise field

    for (int x = 0; x < CHUNK_SIZE; ++x) {
        for (int y = 0; y < CHUNK_SIZE; ++y) {
            for (int z = 0; z < CHUNK_SIZE; ++z) {
                MarchingCubes::GridCell cell;
                
                // Populate grid cell vertices and values
                for (int i = 0; i < 8; ++i) {
                    // Calculate the position of each corner in the chunk
                    glm::ivec3 cornerPos = glm::ivec3(x, y, z) + cornerOffsets[i];

                    int index = cornerPos.x + cornerPos.z * (CHUNK_SIZE + 1) + cornerPos.y * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1);
                    
                    cell.p[i] = glm::vec3(cornerPos);
                    cell.val[i] = m_NoiseData.at(index); // Using .at() can give better debug info than []
                }

                MarchingCubes::Polygonise(cell, isolevel, triangles);
            }
        }
    }

    LOG("Chunk::generateMesh - Polygonization loop finished.");
    LOG("Chunk::generateMesh - Generated " + std::to_string(triangles.size()) + " triangles for chunk.");
    if (triangles.empty()) {
        m_VertexCount = 0;
        LOG("Chunk::generateMesh - No triangles, exiting early.");
        return;
    }

    m_VertexCount = triangles.size() * 3;
    
    struct Vertex {
        glm::vec3 position;
        glm::vec3 normal;
    };
    std::vector<Vertex> vertices;
    vertices.reserve(m_VertexCount);

    for(const auto& tri : triangles) {
        // Calculate the normal of the triangle face
        glm::vec3 normal = glm::normalize(glm::cross(tri.p[1] - tri.p[0], tri.p[2] - tri.p[0]));
        
        vertices.push_back({tri.p[0], normal});
        vertices.push_back({tri.p[1], normal});
        vertices.push_back({tri.p[2], normal});
    }
    LOG("Chunk::generateMesh - Vertex vector created.");

    // Create VAO and VBO
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    LOG("Chunk::generateMesh - VAO/VBO generated.");

    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);
    LOG("Chunk::generateMesh - Buffer data sent to GPU.");

    // Position attribute
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));

    // Normal attribute
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));

    glBindVertexArray(0);
    LOG("Chunk::generateMesh - Finish");
}

void Chunk::render() const {
    if (m_VertexCount == 0) return;
    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, m_VertexCount);
}

bool Chunk::isSolid(const glm::vec3& worldPosition) const {
    // Convert world position to local chunk coordinates
    glm::vec3 localPos = worldPosition - glm::vec3(m_Position);

    // Check bounds
    if (localPos.x < 0 || localPos.x >= CHUNK_SIZE ||
        localPos.y < 0 || localPos.y >= CHUNK_SIZE ||
        localPos.z < 0 || localPos.z >= CHUNK_SIZE) {
        return false; // Not in this chunk
    }

    // Convert to integer coordinates for array access
    glm::ivec3 voxelPos = glm::floor(localPos);

    // Get the noise value at that position
    int index = voxelPos.x + voxelPos.z * (CHUNK_SIZE + 1) + voxelPos.y * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1);
    
    if (index < 0 || index >= m_NoiseData.size()) {
        return false; // Out of bounds of the noise data
    }

    // If the density is below the isolevel, it's considered "solid"
    return m_NoiseData[index] < 0.0f;
}

} // namespace Luminumbra::World
