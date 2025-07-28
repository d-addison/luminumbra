// src/luminumbra/world/Chunk.cpp
#include "luminumbra/world/Chunk.h"
#include "luminumbra/world/MarchingCubes.h"
#include "FastNoiseLite.h"
#include "luminumbra/core/Debug.h"
#include "luminumbra/core/GLError.h"

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

#include <vector>
#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <map>

namespace Luminumbra::World {

Chunk::Chunk(const glm::ivec3& position, const std::string& seed, int lod) : m_Position(position), m_LOD(lod) {
    m_ModelMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(m_Position));

    fnl_state noise = fnlCreateState();
    noise.seed = static_cast<int>(std::hash<std::string>{}(seed));
    
    // This part is CPU-only and safe for worker threads.
    generateNoiseData(noise); 
    generateMesh(m_LOD);
    generateWaterMesh();

    m_GpuStatus = GpuStatus::ReadyForUpload;
}

Chunk::~Chunk() {
    glDeleteVertexArrays(1, &m_VAO);
    glDeleteBuffers(1, &m_VBO);
    glDeleteBuffers(1, &m_EBO);
    glDeleteVertexArrays(1, &m_WaterVAO);
    glDeleteBuffers(1, &m_WaterVBO);
}

void Chunk::generateNoiseData(fnl_state& noise) {
    m_NoiseData.resize((CHUNK_SIZE + 1) * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1));
    m_BiomeData.resize((CHUNK_SIZE + 1) * (CHUNK_SIZE + 1), BiomeType::WHISPERING_GLADE);

    noise.noise_type = FNL_NOISE_OPENSIMPLEX2;
    noise.frequency = 0.005f;
    noise.fractal_type = FNL_FRACTAL_FBM;       // Use Fractal Brownian Motion
    noise.octaves = 4;                          // Number of noise layers
    noise.lacunarity = 2.0f;                      // How quickly frequency increases for each octave
    noise.gain = 0.5f;                          // How much each octave contributes

    // --- NEW: Setup a second noise generator for caves ---
    fnl_state caveNoise = fnlCreateState();
    caveNoise.seed = noise.seed + 1; // Use a different seed
    caveNoise.noise_type = FNL_NOISE_PERLIN;
    caveNoise.frequency = 0.025f; // Caves should have a higher frequency
    caveNoise.fractal_type = FNL_FRACTAL_FBM;
    caveNoise.octaves = 2; // Fewer octaves for simpler cave shapes

    // --- Define terrain shape ---
    float baseGroundHeight = 8.0f;   // The average sea level for the terrain
    float terrainAmplitude = 22.0f;   // The max height of hills and depth of valleys

    // --- Loop through every point in the chunk's data grid ---
    for (int y = 0; y <= CHUNK_SIZE; ++y) {
        for (int z = 0; z <= CHUNK_SIZE; ++z) {
            for (int x = 0; x <= CHUNK_SIZE; ++x) {
                float worldX = (float)(m_Position.x + x);
                float worldZ = (float)(m_Position.z + z);
                float worldY = (float)(m_Position.y + y);

                // 1. Calculate the base terrain density as before
                float groundHeightNoise = fnlGetNoise2D(&noise, worldX, worldZ);
                float groundHeight = baseGroundHeight + (groundHeightNoise * terrainAmplitude);
                float groundDensity = worldY - groundHeight;

                // 2. NEW: Calculate a 3D "carver" value for caves
                // We get a value from [-1, 1]. Positive values will carve away terrain.
                float caveCarver = fnlGetNoise3D(&caveNoise, worldX, worldY, worldZ);

                // 3. Combine the densities
                // We subtract the carver value from the ground density.
                // This "hollows out" areas where caveCarver is high.
                float finalDensity = groundDensity - caveCarver;

                // Store the final density value
                int index = x + z * (CHUNK_SIZE + 1) + y * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1);
                m_NoiseData[index] = finalDensity;
            }
        }
    }
}

// Helper array to get the 8 corner offsets of a cube.
const glm::ivec3 cornerOffsets[8] = {
    {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1},
    {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}
};

void Chunk::generateMesh(int lod) {
    MarchingCubes::IndexedMesh mesh;
    float isolevel = 0.0f;
    int step = 1 << lod;

    for (int x = 0; x < CHUNK_SIZE; x += step) {
        for (int y = 0; y < CHUNK_SIZE; y += step) {
            for (int z = 0; z < CHUNK_SIZE; z += step) {
                MarchingCubes::GridCell cell;
                for (int i = 0; i < 8; ++i) {
                    glm::ivec3 cornerPos = glm::ivec3(x, y, z) + cornerOffsets[i] * step;
                    int index = cornerPos.x + cornerPos.z * (CHUNK_SIZE + 1) + cornerPos.y * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1);
                    cell.p[i] = glm::vec3(cornerPos);
                    cell.val[i] = m_NoiseData.at(index);
                }
                MarchingCubes::Polygonise(cell, isolevel, mesh);
            }
        }
    }

    if (mesh.indices.empty()) {
        m_IndexCount = 0;
        return;
    }

    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        std::swap(mesh.indices[i], mesh.indices[i + 1]);
    }

    fnl_state foliage_noise = fnlCreateState();
    foliage_noise.seed = m_Position.x * 1337 + m_Position.z * 7331; // Unique seed for foliage
    generateFoliage(foliage_noise, mesh);

    m_IndexCount = mesh.indices.size();
    
    struct Vertex { glm::vec3 p, n, c; };
    std::vector<Vertex> vertices(mesh.vertices.size());
    std::vector<glm::vec3> normals(mesh.vertices.size(), glm::vec3(0.0f));

    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        unsigned int i0 = mesh.indices[i], i1 = mesh.indices[i+1], i2 = mesh.indices[i+2];
        glm::vec3 v0 = mesh.vertices[i0], v1 = mesh.vertices[i1], v2 = mesh.vertices[i2];
        glm::vec3 faceNormal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
        normals[i0] += faceNormal;
        normals[i1] += faceNormal;
        normals[i2] += faceNormal;
    }

    // Populate the vertex buffer with all data
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        vertices[i].p = mesh.vertices[i];
        vertices[i].n = glm::normalize(normals[i]);
        
        glm::vec3 worldPos = glm::vec3(m_Position) + vertices[i].p;
        int biomeX = glm::clamp((int)vertices[i].p.x, 0, CHUNK_SIZE);
        int biomeZ = glm::clamp((int)vertices[i].p.z, 0, CHUNK_SIZE);
        int biomeIndex = biomeX + biomeZ * (CHUNK_SIZE + 1);
        BiomeType biome = m_BiomeData[biomeIndex];

        float density = 0.0f;
        int voxelX = (int)vertices[i].p.x, voxelY = (int)vertices[i].p.y, voxelZ = (int)vertices[i].p.z;
        int voxelIndex = voxelX + voxelZ * (CHUNK_SIZE + 1) + voxelY * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1);
        if (voxelIndex >= 0 && voxelIndex < m_NoiseData.size()) {
            density = m_NoiseData[voxelIndex];
        }
        vertices[i].c = getTerrainColor(worldPos.y, density, biome);
    }

    m_VertexData.resize(vertices.size() * sizeof(Vertex) / sizeof(float));
    memcpy(m_VertexData.data(), vertices.data(), vertices.size() * sizeof(Vertex));
    
    m_IndexData = mesh.indices;
    m_GpuStatus = GpuStatus::Generating;
}

void Chunk::generateFoliage(fnl_state& noise, const MarchingCubes::IndexedMesh& terrainMesh) {
    noise.noise_type = FNL_NOISE_PERLIN;
    noise.frequency = 0.8f;

    std::mt19937 rng(static_cast<unsigned int>(std::hash<std::string>{}(std::to_string(m_Position.x) + "_" + std::to_string(m_Position.z))));
    std::uniform_real_distribution<float> scaleDist(0.8f, 1.5f);
    std::uniform_real_distribution<float> rotDist(0.0f, 2.0f * glm::pi<float>());

    std::map<std::pair<int, int>, float> highestY;
    std::map<std::pair<int, int>, glm::vec3> surfaceNormals;

    for (size_t i = 0; i < terrainMesh.indices.size(); i += 3) {
        glm::vec3 v0 = terrainMesh.vertices[terrainMesh.indices[i]];
        glm::vec3 v1 = terrainMesh.vertices[terrainMesh.indices[i + 1]];
        glm::vec3 v2 = terrainMesh.vertices[terrainMesh.indices[i + 2]];
        glm::vec3 faceNormal = glm::normalize(glm::cross(v1 - v0, v2 - v0));

        for (const auto& v : { v0, v1, v2 }) {
            auto key = std::make_pair((int)floor(v.x), (int)floor(v.z));
            if (highestY.find(key) == highestY.end() || v.y > highestY[key]) {
                highestY[key] = v.y;
                surfaceNormals[key] = faceNormal;
            }
        }
    }

    for (int x = 1; x < CHUNK_SIZE - 1; ++x) {
        for (int z = 1; z < CHUNK_SIZE - 1; ++z) {
            auto key = std::make_pair(x, z);
            if (highestY.find(key) == highestY.end()) continue;

            float y = highestY[key];
            glm::vec3 localPos(x, y, z);
            glm::vec3 worldPos = glm::vec3(m_Position) + localPos;

            if (worldPos.y > WATER_LEVEL + 1.0f && surfaceNormals[key].y > 0.85f) {
                float foliageValue = fnlGetNoise2D(&noise, worldPos.x, worldPos.z);

                // This lambda creates the local transformation matrix for an instance
                auto createTransform = [&](const glm::vec3& pos, float scale, float rotation) {
                    glm::mat4 transform = glm::translate(glm::mat4(1.0f), pos);
                    transform = glm::rotate(transform, rotation, glm::vec3(0.0f, 1.0f, 0.0f));
                    transform = glm::scale(transform, glm::vec3(scale));
                    return transform;
                };

                if (foliageValue > 0.7f) { // Trees
                    float scale = scaleDist(rng);
                    float rotation = rotDist(rng);
                    // The transform is now correctly calculated based on the instance's position *within* the chunk
                    glm::mat4 transform = createTransform(localPos, scale, rotation);
                    m_TreeInstances.push_back({ localPos, scale, rotation, transform });
                } else if (foliageValue > 0.6f) { // Bushes
                    float scale = scaleDist(rng) * 0.5f;
                    float rotation = rotDist(rng);
                    glm::mat4 transform = createTransform(localPos, scale, rotation);
                    m_BushInstances.push_back({ localPos, scale, rotation, transform });
                }
            }
        }
    }
}



void Chunk::uploadToGpu() {
    if (m_GpuStatus != GpuStatus::ReadyForUpload) return;

    if (m_IndexCount > 0) {
        struct Vertex { glm::vec3 p, n, c; };

        GLCall(glGenVertexArrays(1, &m_VAO));
        GLCall(glGenBuffers(1, &m_VBO));
        GLCall(glGenBuffers(1, &m_EBO));

        GLCall(glBindVertexArray(m_VAO));
        GLCall(glBindBuffer(GL_ARRAY_BUFFER, m_VBO));
        GLCall(glBufferData(GL_ARRAY_BUFFER, m_VertexData.size() * sizeof(float), m_VertexData.data(), GL_STATIC_DRAW));

        GLCall(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO));
        GLCall(glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_IndexData.size() * sizeof(unsigned int), m_IndexData.data(), GL_STATIC_DRAW));

        GLCall(glEnableVertexAttribArray(0));
        GLCall(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, p)));
        GLCall(glEnableVertexAttribArray(1));
        GLCall(glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, n)));
        GLCall(glEnableVertexAttribArray(2));
        GLCall(glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, c)));

        GLCall(glBindVertexArray(0));
    }

    if (m_WaterVertexCount > 0) {
        struct WaterVertex { glm::vec3 position; };

        GLCall(glGenVertexArrays(1, &m_WaterVAO));
        GLCall(glGenBuffers(1, &m_WaterVBO));

        GLCall(glBindVertexArray(m_WaterVAO));
        GLCall(glBindBuffer(GL_ARRAY_BUFFER, m_WaterVBO));
        GLCall(glBufferData(GL_ARRAY_BUFFER, m_WaterVertexData.size() * sizeof(float), m_WaterVertexData.data(), GL_STATIC_DRAW));

        // Position attribute
        GLCall(glEnableVertexAttribArray(0));
        GLCall(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(WaterVertex), (void*)offsetof(WaterVertex, position)));

        GLCall(glBindVertexArray(0));

        // Clear water data after upload
        m_WaterVertexData.clear();
        m_WaterVertexData.shrink_to_fit();
    }

    m_VertexData.clear();
    m_VertexData.shrink_to_fit();
    m_IndexData.clear();
    m_IndexData.shrink_to_fit();
    m_WaterVertexData.clear();
    m_WaterVertexData.shrink_to_fit();

    m_GpuStatus = GpuStatus::Uploaded;
}

void Chunk::renderTerrain() const {
    // This function now ONLY renders the solid terrain mesh.
    if (m_GpuStatus == GpuStatus::Uploaded && m_IndexCount > 0) {
        GLCall(glBindVertexArray(m_VAO));
        GLCall(glDrawElements(GL_TRIANGLES, m_IndexCount, GL_UNSIGNED_INT, 0));
        GLCall(glBindVertexArray(0));
    }
}

Chunk::GpuStatus Chunk::getGpuStatus() const {
    return m_GpuStatus.load();
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

    // If the density is below the isolevel (0.0), it's considered "solid"
    // This matches the marching cubes logic
    return m_NoiseData[index] < 0.0f;
}

BiomeType Chunk::getBiomeAt(float worldX, float worldZ, float height) {
    // Use cellular noise for distinct biome regions
    fnl_state biomeNoise = fnlCreateState();
    biomeNoise.noise_type = FNL_NOISE_CELLULAR;
    biomeNoise.frequency = 0.003f;
    biomeNoise.cellular_distance_func = FNL_CELLULAR_DISTANCE_MANHATTAN;
    biomeNoise.cellular_return_type = FNL_CELLULAR_RETURN_TYPE_CELLVALUE;
    biomeNoise.seed = 42;
    
    float biomeValue = fnlGetNoise2D(&biomeNoise, worldX, worldZ);
    
    // Also factor in height for more realistic biome distribution
    float heightInfluence = height / 100.0f;
    
    // Map noise values to biomes
    if (biomeValue < -0.3f) {
        if (heightInfluence < 0.3f) {
            return BiomeType::SUNKEN_HOLLOWS;
        } else {
            return BiomeType::WHISPERING_GLADE;
        }
    } else if (biomeValue < 0.1f) {
        return BiomeType::WHISPERING_GLADE;
    } else if (biomeValue < 0.5f) {
        if (heightInfluence > 0.5f) {
            return BiomeType::CRYSTAL_GROVES;
        } else {
            return BiomeType::WHISPERING_GLADE;
        }
    } else {
        if (heightInfluence > 0.6f) {
            return BiomeType::CANOPY_BRIDGES;
        } else {
            return BiomeType::CRYSTAL_GROVES;
        }
    }
}

glm::vec3 Chunk::getTerrainColor(float worldY, float density, BiomeType biome) {
    // Base colors for different biomes based on the README's color palettes
    glm::vec3 baseColor;
    
    switch (biome) {
        case BiomeType::WHISPERING_GLADE:
            // Gentle greens and golds
            if (worldY < 20.0f) {
                baseColor = glm::vec3(0.42f, 0.56f, 0.14f); // Living green (#6B8E23)
            } else if (worldY < 40.0f) {
                baseColor = glm::vec3(0.52f, 0.60f, 0.25f); // Lighter green
            } else {
                baseColor = glm::vec3(0.70f, 0.65f, 0.40f); // Sandy/rocky
            }
            break;
            
        case BiomeType::CRYSTAL_GROVES:
            // Crystalline blues and purples
            if (density > 5.0f) {
                baseColor = glm::vec3(0.60f, 0.40f, 0.80f); // Amethyst (#9932CC)
            } else if (worldY < 30.0f) {
                baseColor = glm::vec3(0.28f, 0.24f, 0.55f); // Deep indigo (#483D8B)
            } else {
                baseColor = glm::vec3(0.45f, 0.35f, 0.65f); // Crystal blue-purple
            }
            break;
            
        case BiomeType::SUNKEN_HOLLOWS:
            // Dark, mysterious colors with bioluminescence hints
            if (worldY < 15.0f) {
                baseColor = glm::vec3(0.15f, 0.20f, 0.25f); // Very dark blue-grey
            } else if (worldY < 25.0f) {
                baseColor = glm::vec3(0.13f, 0.70f, 0.67f); // Bioluminescent teal (#20B2AA)
            } else {
                baseColor = glm::vec3(0.25f, 0.30f, 0.35f); // Dark stone
            }
            break;
            
        case BiomeType::CANOPY_BRIDGES:
            // High altitude, ethereal colors
            if (worldY > 60.0f) {
                baseColor = glm::vec3(1.00f, 0.84f, 0.00f); // Warm gold (#FFD700)
            } else if (worldY > 45.0f) {
                baseColor = glm::vec3(1.00f, 0.87f, 0.68f); // Dawn peach (#FFDAB9)
            } else {
                baseColor = glm::vec3(0.85f, 0.75f, 0.60f); // Light stone
            }
            break;
            
        default:
        case BiomeType::SKY_VOID:
            baseColor = glm::vec3(0.5f, 0.6f, 0.7f); // Sky blue-grey
            break;
    }
    
    // Add some variation based on noise density
    float variation = (density + 10.0f) / 20.0f;
    variation = glm::clamp(variation, 0.8f, 1.2f);
    
    return baseColor * variation;
}

void Chunk::renderWater() const {
    // This function should ONLY be responsible for the draw call.
    // Blending state should be managed in the main render loop.
    if (m_WaterVertexCount > 0) {
        glBindVertexArray(m_WaterVAO);
        glDrawArrays(GL_TRIANGLES, 0, m_WaterVertexCount);
        glBindVertexArray(0);
    }
}

void Chunk::generateWaterMesh() {
    if (m_Position.y > WATER_LEVEL || m_Position.y + CHUNK_SIZE < WATER_LEVEL) {
        m_WaterVertexCount = 0;
        return;
    }

    struct WaterVertex { glm::vec3 position; };
    std::vector<WaterVertex> vertices;
    float waterY = WATER_LEVEL - m_Position.y;

    // FIX #2: Define the quad with a Counter-Clockwise (CCW) winding order
    // so it is considered a front-face when viewed from above.
    
    // Triangle 1
    vertices.push_back({glm::vec3(0, waterY, 0)});
    vertices.push_back({glm::vec3(CHUNK_SIZE, waterY, 0)});
    vertices.push_back({glm::vec3(0, waterY, CHUNK_SIZE)});

    // Triangle 2
    vertices.push_back({glm::vec3(CHUNK_SIZE, waterY, 0)});
    vertices.push_back({glm::vec3(CHUNK_SIZE, waterY, CHUNK_SIZE)});
    vertices.push_back({glm::vec3(0, waterY, CHUNK_SIZE)});
    
    m_WaterVertexCount = vertices.size();
    if (m_WaterVertexCount == 0) return;

    m_WaterVertexData.resize(vertices.size() * sizeof(WaterVertex) / sizeof(float));
    memcpy(m_WaterVertexData.data(), vertices.data(), vertices.size() * sizeof(WaterVertex));
}

} // namespace Luminumbra::World
