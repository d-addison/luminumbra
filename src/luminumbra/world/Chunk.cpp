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

    // PRE-CALCULATE BIOMES FOR THE CHUNK
    for (int z = 0; z <= CHUNK_SIZE; ++z) {
        for (int x = 0; x <= CHUNK_SIZE; ++x) {
            float worldX = (float)(m_Position.x + x);
            float worldZ = (float)(m_Position.z + z);
            m_BiomeData[x + z * (CHUNK_SIZE + 1)] = getBiomeAt(worldX, worldZ);
        }
    }

    // --- Terrain Noise ---
    noise.noise_type = FNL_NOISE_OPENSIMPLEX2;
    noise.frequency = 0.005f;
    noise.fractal_type = FNL_FRACTAL_FBM;
    noise.octaves = 4;
    noise.lacunarity = 2.0f;
    noise.gain = 0.5f;

    // --- 3D Cave Noise ---
    fnl_state cave_noise = fnlCreateState();
    cave_noise.seed = noise.seed + 1; // Use a different seed
    cave_noise.noise_type = FNL_NOISE_PERLIN;
    cave_noise.fractal_type = FNL_FRACTAL_RIDGED; // Ridged is great for tunnels
    cave_noise.frequency = 0.02f;
    cave_noise.octaves = 2;

    // --- Define terrain shape ---
    float baseGroundHeight = 10.0f;   // The average sea level for the terrain
    float terrainAmplitude = 30.0f;   // The max height of hills and depth of valleys

    // --- Loop through every point in the chunk's data grid ---
    for (int y = 0; y <= CHUNK_SIZE; ++y) {
        for (int z = 0; z <= CHUNK_SIZE; ++z) {
            for (int x = 0; x <= CHUNK_SIZE; ++x) {
                float worldX = (float)(m_Position.x + x);
                float worldY = (float)(m_Position.y + y);
                float worldZ = (float)(m_Position.z + z);

                // 1. Calculate base terrain height (like before)
                float groundHeightNoise = fnlGetNoise2D(&noise, worldX, worldZ);
                float groundHeight = baseGroundHeight + (groundHeightNoise * terrainAmplitude);

                // 2. Base terrain density is the distance from the ground surface
                float terrain_density = worldY - groundHeight;
                
                // ADDED LOGIC FOR CAVES AND FLOATING ISLANDS
                // Make the base terrain fade out at the bottom to create floating islands
                float island_fade_factor = 1.0f - glm::smoothstep(0.0f, 15.0f, worldY);
                terrain_density += island_fade_factor * 20.0f; // Push density towards air at the bottom

                // 3. Calculate 3D cave noise
                float cave_density = fnlGetNoise3D(&cave_noise, worldX, worldY, worldZ);

                // 4. Combine the densities
                // We will add the cave noise. Since ridged noise is mostly negative,
                // this will carve out areas (make them more "air-like").
                // We only apply cave carving below the surface.
                float final_density = terrain_density;
                if (terrain_density < 0.0f) { // If underground
                    // The closer to 0 cave_density is, the more "hollow" it is.
                    // We can add it to make the terrain less dense.
                    // A multiplier strengthens the effect.
                    final_density += (cave_density + 0.2f) * 2.0f;
                }

                int index = x + z * (CHUNK_SIZE + 1) + y * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1);
                m_NoiseData[index] = final_density;
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
        std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
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
    m_GpuStatus = GpuStatus::NeedsGpuUpload;
}

void Chunk::generateFoliage(fnl_state& noise, const MarchingCubes::IndexedMesh& terrainMesh) {
    noise.noise_type = FNL_NOISE_PERLIN;
    noise.frequency = 0.8f;
    // noise.frequency = 0.1f;
    // noise.cellular_return_type = FNL_CELLULAR_RETURN_TYPE_CELLVALUE;
    // noise.cellular_jitter_mod = 1.0f;

    std::mt19937 rng(static_cast<unsigned int>(std::hash<std::string>{}(std::to_string(m_Position.x) + "_" + std::to_string(m_Position.z))));
    std::uniform_real_distribution<float> scaleDist(0.8f, 1.5f);
    std::uniform_real_distribution<float> rotDist(0.0f, 2.0f * 3.14159f);

    std::map<std::pair<int, int>, float> highestY;
    std::map<std::pair<int, int>, glm::vec3> surfaceNormals;

    // First pass: find the highest point and average normal for each (x, z) grid cell
    for (size_t i = 0; i < terrainMesh.indices.size(); i += 3) {
        unsigned int i0 = terrainMesh.indices[i];
        unsigned int i1 = terrainMesh.indices[i + 1];
        unsigned int i2 = terrainMesh.indices[i + 2];

        glm::vec3 v0 = terrainMesh.vertices[i0];
        glm::vec3 v1 = terrainMesh.vertices[i1];
        glm::vec3 v2 = terrainMesh.vertices[i2];

        glm::vec3 faceNormal = glm::normalize(glm::cross(v1 - v0, v2 - v0));

        // Process all three vertices of the triangle
        for (const auto& v : { v0, v1, v2 }) {
            int ix = static_cast<int>(floor(v.x));
            int iz = static_cast<int>(floor(v.z));
            auto key = std::make_pair(ix, iz);

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
            glm::vec3 worldPos = glm::vec3(m_Position) + glm::vec3(x, y, z);

            if (worldPos.y > WATER_LEVEL + 1.0f) { // Don't spawn foliage underwater
                glm::vec3 normal = surfaceNormals[key];
                if (normal.y > 0.85f) { // Only on relatively flat ground
                    float foliageValue = fnlGetNoise2D(&noise, worldPos.x, worldPos.z);

                    if (foliageValue > 0.7f) { // Threshold for trees
                        m_TreeInstances.push_back({ glm::vec3(x, y, z), scaleDist(rng), rotDist(rng) });
                    } else if (foliageValue > 0.6f) { // Threshold for bushes
                        m_BushInstances.push_back({ glm::vec3(x, y - 4.0f, z), scaleDist(rng) * 0.5f, rotDist(rng) });
                    }
                }
            }
        }
    }
}


void Chunk::uploadToGpu() {
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

    // Clear the CPU-side data after uploading to free up RAM
    m_VertexData.clear();
    m_IndexData.clear();
    m_VertexData.shrink_to_fit();
    m_IndexData.shrink_to_fit();
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

BiomeType Chunk::getBiomeAt(float worldX, float worldZ) { // Remove height, as it's often a result of biome
    // --- Temperature Noise (Continental Scale) ---
    fnl_state temp_noise = fnlCreateState();
    temp_noise.seed = 1337;
    temp_noise.noise_type = FNL_NOISE_OPENSIMPLEX2;
    temp_noise.frequency = 0.0005f; // Very low frequency for large regions
    float temperature = fnlGetNoise2D(&temp_noise, worldX, worldZ); // Range [-1, 1]

    // --- Humidity Noise (Regional Scale) ---
    fnl_state humidity_noise = fnlCreateState();
    humidity_noise.seed = 42;
    humidity_noise.noise_type = FNL_NOISE_OPENSIMPLEX2;
    humidity_noise.frequency = 0.001f;
    float humidity = fnlGetNoise2D(&humidity_noise, worldX, worldZ); // Range [-1, 1]

    // --- Whittaker Diagram Logic ---
    if (temperature > 0.4f) { // Hot
        if (humidity > 0.3f) return BiomeType::CANOPY_BRIDGES; // Tropical/Jungle-like
        else return BiomeType::CRYSTAL_GROVES; // Hot & Dry -> Crystalline Desert
    } else if (temperature > -0.3f) { // Temperate
        if (humidity > 0.0f) return BiomeType::WHISPERING_GLADE; // Forest
        else return BiomeType::WHISPERING_GLADE; // Plains (could be a separate biome)
    } else { // Cold
        if (humidity > -0.2f) return BiomeType::SUNKEN_HOLLOWS; // Taiga/Swampy
        else return BiomeType::CRYSTAL_GROVES; // Tundra/Icy
    }
    
    return BiomeType::WHISPERING_GLADE; // Fallback
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
    // Only generate a water mesh if the chunk intersects the water level
    if (m_Position.y > WATER_LEVEL || m_Position.y + CHUNK_SIZE < WATER_LEVEL) {
        m_WaterVertexCount = 0;
        return;
    }

    // This struct should only contain what the shader needs.
    // The shader only needs position.
    struct WaterVertex {
        glm::vec3 position;
    };

    std::vector<WaterVertex> vertices;
    float waterY = WATER_LEVEL - m_Position.y;

    // Define the quad for the water surface
    vertices.push_back({glm::vec3(0, waterY, 0)});
    vertices.push_back({glm::vec3(0, waterY, CHUNK_SIZE)});
    vertices.push_back({glm::vec3(CHUNK_SIZE, waterY, 0)});

    vertices.push_back({glm::vec3(CHUNK_SIZE, waterY, 0)});
    vertices.push_back({glm::vec3(0, waterY, CHUNK_SIZE)});
    vertices.push_back({glm::vec3(CHUNK_SIZE, waterY, CHUNK_SIZE)});
    
    m_WaterVertexCount = vertices.size();
    if (m_WaterVertexCount == 0) {
        return;
    }

    // Store the data for later upload
    m_WaterVertexData.resize(vertices.size() * sizeof(WaterVertex) / sizeof(float));
    memcpy(m_WaterVertexData.data(), vertices.data(), vertices.size() * sizeof(WaterVertex));
}

} // namespace Luminumbra::World
