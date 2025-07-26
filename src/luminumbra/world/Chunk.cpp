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
    
    for (int x = 0; x <= CHUNK_SIZE; ++x) {
        for (int y = 0; y <= CHUNK_SIZE; ++y) {
            for (int z = 0; z <= CHUNK_SIZE; ++z) {
                float worldX = (float)(m_Position.x + x);
                float worldY = (float)(m_Position.y + y);
                float worldZ = (float)(m_Position.z + z);

                // Simple 3D noise + a gradient to make it look like terrain
                float density = -worldY; 
                // Use the C-style function call, passing a pointer to the state
                density += fnlGetNoise3D(&noise, worldX, worldY, worldZ) * 10.0f;
                
                int index = x + y * (CHUNK_SIZE + 1) + z * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1);
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
                    
                    int index = cornerPos.x + cornerPos.y * (CHUNK_SIZE + 1) + cornerPos.z * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1);
                    
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
    
    // Using a more efficient way to copy data
    std::vector<glm::vec3> vertices;
    vertices.reserve(m_VertexCount);
    for(const auto& tri : triangles) {
        vertices.push_back(tri.p[0]);
        vertices.push_back(tri.p[1]);
        vertices.push_back(tri.p[2]);
    }

    LOG("Chunk::generateMesh - Vertex vector created.");

    // Create VAO and VBO
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    LOG("Chunk::generateMesh - VAO/VBO generated.");

    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(glm::vec3), vertices.data(), GL_STATIC_DRAW);
    LOG("Chunk::generateMesh - Buffer data sent to GPU.");

    // Position attribute
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(0);
    LOG("Chunk::generateMesh - Vertex attributes configured.");

    glBindVertexArray(0);
    LOG("Chunk::generateMesh - Finish");
}

void Chunk::render() const {
    if (m_VertexCount == 0) return;
    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, m_VertexCount);
}

} // namespace Luminumbra::World