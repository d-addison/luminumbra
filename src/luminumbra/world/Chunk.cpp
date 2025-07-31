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

Chunk::Chunk(const glm::ivec3& position, const std::string& seed, int lod, std::shared_ptr<const GenerationProfile> profile) 
    : m_Position(position), m_LOD(lod) 
{
    m_ModelMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(m_Position));

    // CORRECTED ORDER AND CALLS
    // 1. Generate the 3D density data using the profile
    generateNoiseData(*profile);
    
    // 2. Determine foliage positions based on the density data (before meshing)
    fnl_state foliage_noise = fnlCreateState();
    foliage_noise.seed = m_Position.x * 1337 + m_Position.z * 7331;
    generateFoliage(foliage_noise, *profile);

    // 3. Generate the terrain mesh from the density data
    generateMesh(m_LOD);

    // 4. Generate the water mesh
    generateWaterMesh(*profile);
}

Chunk::~Chunk() {
    glDeleteVertexArrays(1, &m_VAO);
    glDeleteBuffers(1, &m_VBO);
    glDeleteBuffers(1, &m_EBO);
    glDeleteVertexArrays(1, &m_WaterVAO);
    glDeleteBuffers(1, &m_WaterVBO);
}

void Chunk::generateNoiseData(const GenerationProfile& profile) {
    m_NoiseData.resize((CHUNK_SIZE + 1) * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1));
    m_BiomeData.resize((CHUNK_SIZE + 1) * (CHUNK_SIZE + 1));

    // 1. Pre-calculate biomes for the chunk
    for (int z = 0; z <= CHUNK_SIZE; ++z) {
        for (int x = 0; x <= CHUNK_SIZE; ++x) {
            float worldX = (float)(m_Position.x + x);
            float worldZ = (float)(m_Position.z + z);
            m_BiomeData[x + z * (CHUNK_SIZE + 1)] = getBiomeAt(worldX, worldZ);
        }
    }

    // 2. Loop through every point and calculate density
    for (int y = 0; y <= CHUNK_SIZE; ++y) {
        for (int z = 0; z <= CHUNK_SIZE; ++z) {
            for (int x = 0; x <= CHUNK_SIZE; ++x) {
                float worldX = (float)(m_Position.x + x);
                float worldY = (float)(m_Position.y + y);
                float worldZ = (float)(m_Position.z + z);

                // This is a simplified blending example. A real implementation of
                // getBlendedBiomeAt would use Voronoi noise or multiple noise layers
                // to determine the two dominant biomes and a blend factor.
                BiomeType primary_type = getBiomeAt(worldX, worldZ); // Your existing function
                BiomeType secondary_type = getBiomeAt(worldX + 150.f, worldZ - 150.f); // Sample a nearby point for a secondary biome

                fnl_state blend_noise = fnlCreateState();
                blend_noise.frequency = 0.002f; // A different frequency for the blend map
                float blend_factor = (fnlGetNoise2D(&blend_noise, worldX, worldZ) + 1.0f) / 2.0f; // Noise in [0, 1] range

                const BiomeProfile& primary_biome = profile.biome_profiles.at(primary_type);
                const BiomeProfile& secondary_biome = profile.biome_profiles.at(secondary_type);

                // --- INTERPOLATE BIOME PARAMETERS ---
                float base_height      = glm::mix(primary_biome.base_height, secondary_biome.base_height, blend_factor);
                float terrain_variance = glm::mix(primary_biome.terrain_variance, secondary_biome.terrain_variance, blend_factor);
                float mountains_amp    = glm::mix(primary_biome.mountains.amplitude, secondary_biome.mountains.amplitude, blend_factor);
                float caves_amp        = glm::mix(primary_biome.caves.amplitude, secondary_biome.caves.amplitude, blend_factor);

                // For noise states, you can't easily mix them.
                // A common approach is to calculate the noise for both biomes and then mix the *results*.

                // --- BASE TERRAIN (blended) ---
                float primary_height_noise = fnlGetNoise2D(&primary_biome.base_terrain.noise, worldX, worldZ);
                float secondary_height_noise = fnlGetNoise2D(&secondary_biome.base_terrain.noise, worldX, worldZ);
                float heightmap_noise = glm::mix(primary_height_noise, secondary_height_noise, blend_factor);
                
                // --- MOUNTAINS (blended) ---
                float primary_mountain_noise = pow(glm::abs(fnlGetNoise2D(&primary_biome.mountains.noise, worldX, worldZ)), 2.0f);
                float secondary_mountain_noise = pow(glm::abs(fnlGetNoise2D(&secondary_biome.mountains.noise, worldX, worldZ)), 2.0f);
                float mountain_noise = glm::mix(primary_mountain_noise, secondary_mountain_noise, blend_factor);

                // Combine heightmap and mountains using interpolated parameters
                float groundHeight = base_height + (heightmap_noise * terrain_variance) + (mountain_noise * mountains_amp);
                float terrain_density = worldY - groundHeight;

                // --- CAVES (blended) ---
                float primary_cave_noise = fnlGetNoise3D(&primary_biome.caves.noise, worldX, worldY, worldZ);
                float secondary_cave_noise = fnlGetNoise3D(&secondary_biome.caves.noise, worldX, worldY, worldZ);
                float cave_noise_raw = glm::mix(primary_cave_noise, secondary_cave_noise, blend_factor);
                float cave_density = glm::smoothstep(0.5f, 0.6f, cave_noise_raw) * caves_amp;

                // --- FINAL DENSITY CALCULATION (Same as before) ---
                float final_density = terrain_density;
                if (terrain_density < 0.0f) {
                    final_density = glm::max(terrain_density, -cave_density);
                }
                
                // --- FLOATING ISLANDS (Same as before) ---
                float island_fade = 1.0f - glm::smoothstep(profile.island_fade_start, profile.island_fade_end, worldY);
                final_density += island_fade * 20.0f;

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
    MarchingCubes::IndexedMesh raw_mesh; // We'll generate a raw, hole-free mesh first.
    float isolevel = 0.0f;
    int step = 1 << lod;

    // --- 1. Generate Raw Mesh WITHOUT Vertex Sharing ---
    // By declaring the caches inside the loop, we prevent the buggy sharing
    // between cells, guaranteeing a topologically sound (hole-free) mesh.
    for (int y = 0; y < CHUNK_SIZE; y += step) {
        for (int z = 0; z < CHUNK_SIZE; z += step) {
            for (int x = 0; x < CHUNK_SIZE; x += step) {
                std::unordered_map<glm::ivec3, unsigned int> cacheX, cacheY, cacheZ; // Fresh cache every time
                MarchingCubes::GridCell cell;
                for (int i = 0; i < 8; ++i) {
                    glm::ivec3 cornerPos = glm::ivec3(x, y, z) + cornerOffsets[i] * step;
                    int index = cornerPos.x + cornerPos.z * (CHUNK_SIZE + 1) + cornerPos.y * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1);
                    cell.p[i] = glm::vec3(cornerPos);
                    cell.val[i] = m_NoiseData.at(index);
                }
                MarchingCubes::Polygonise(cell, isolevel, raw_mesh, cacheX, cacheY, cacheZ);
            }
        }
    }

    if (raw_mesh.indices.empty()) {
        m_IndexCount = 0;
        return;
    }

    // --- 2. Vertex Welding Post-Process ---
    // Now, we'll merge the duplicate vertices from the raw mesh to create a
    // clean, indexed mesh suitable for smooth shading and LOD skirts.
    MarchingCubes::IndexedMesh welded_mesh;
    std::map<glm::vec3, unsigned int> position_to_new_index;

    // Custom comparator for glm::vec3 to use it in a std::map
    struct vec3comp {
        bool operator()(const glm::vec3& a, const glm::vec3& b) const {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            return a.z < b.z;
        }
    };
    std::map<glm::vec3, unsigned int, vec3comp> unique_vertices;

    for (const auto& old_index : raw_mesh.indices) {
        glm::vec3 pos = raw_mesh.vertices[old_index];

        // If we haven't seen a vertex at this exact position before...
        if (unique_vertices.find(pos) == unique_vertices.end()) {
            // ...add it to our new vertex list and store its new index.
            unique_vertices[pos] = welded_mesh.vertices.size();
            welded_mesh.vertices.push_back(pos);
        }
        // Add the new, correct index to our final index list.
        welded_mesh.indices.push_back(unique_vertices[pos]);
    }
    
    // --- 3. Generate LOD Skirt on the Clean, Welded Mesh ---
    // (This logic is identical to before, but now operates on the 'welded_mesh')
    if (m_LOD > 0) {
        // ... (The entire skirt generation block from the previous answer goes here) ...
        // Note: Make sure it operates on 'welded_mesh', not 'raw_mesh'.
        const float skirt_depth = (float)CHUNK_SIZE;
        std::vector<unsigned int> skirt_indices;
        std::map<unsigned int, unsigned int> edge_vertex_to_skirt_vertex;
        for (size_t i = 0; i < welded_mesh.indices.size(); i += 3) {
            unsigned int indices[3] = { welded_mesh.indices[i], welded_mesh.indices[i+1], welded_mesh.indices[i+2] };
            glm::vec3 points[3] = { welded_mesh.vertices[indices[0]], welded_mesh.vertices[indices[1]], welded_mesh.vertices[indices[2]] };
            for (int j = 0; j < 3; ++j) {
                glm::vec3 p1 = points[j]; glm::vec3 p2 = points[(j + 1) % 3];
                bool is_on_boundary = (p1.x == p2.x && (p1.x == 0 || p1.x == CHUNK_SIZE)) || (p1.z == p2.z && (p1.z == 0 || p1.z == CHUNK_SIZE));
                if (is_on_boundary) {
                    unsigned int idx1 = indices[j], idx2 = indices[(j + 1) % 3], skirt_idx1, skirt_idx2;
                    if (edge_vertex_to_skirt_vertex.find(idx1) == edge_vertex_to_skirt_vertex.end()) {
                        welded_mesh.vertices.push_back(p1 - glm::vec3(0, skirt_depth, 0));
                        skirt_idx1 = welded_mesh.vertices.size() - 1; edge_vertex_to_skirt_vertex[idx1] = skirt_idx1;
                    } else { skirt_idx1 = edge_vertex_to_skirt_vertex[idx1]; }
                    if (edge_vertex_to_skirt_vertex.find(idx2) == edge_vertex_to_skirt_vertex.end()) {
                        welded_mesh.vertices.push_back(p2 - glm::vec3(0, skirt_depth, 0));
                        skirt_idx2 = welded_mesh.vertices.size() - 1; edge_vertex_to_skirt_vertex[idx2] = skirt_idx2;
                    } else { skirt_idx2 = edge_vertex_to_skirt_vertex[idx2]; }
                    skirt_indices.push_back(idx1); skirt_indices.push_back(idx2); skirt_indices.push_back(skirt_idx1);
                    skirt_indices.push_back(skirt_idx1); skirt_indices.push_back(idx2); skirt_indices.push_back(skirt_idx2);
                }
            }
        }
        welded_mesh.indices.insert(welded_mesh.indices.end(), skirt_indices.begin(), skirt_indices.end());
    }

    // --- 4. Calculate Normals and Prepare Final GPU Buffers ---
    struct Vertex { glm::vec3 p, n; };
    std::vector<Vertex> final_vertices(welded_mesh.vertices.size());
    std::vector<glm::vec3> final_normals(welded_mesh.vertices.size(), glm::vec3(0.0f));

    for (size_t i = 0; i < welded_mesh.indices.size(); i += 3) {
        unsigned int i0 = welded_mesh.indices[i], i1 = welded_mesh.indices[i+1], i2 = welded_mesh.indices[i+2];
        const glm::vec3& v0 = welded_mesh.vertices[i0];
        const glm::vec3& v1 = welded_mesh.vertices[i1];
        const glm::vec3& v2 = welded_mesh.vertices[i2];
        glm::vec3 faceNormal = glm::cross(v1 - v0, v2 - v0);
        final_normals[i0] += faceNormal;
        final_normals[i1] += faceNormal;
        final_normals[i2] += faceNormal;
    }

    for (size_t i = 0; i < welded_mesh.vertices.size(); ++i) {
        final_vertices[i].p = welded_mesh.vertices[i];
        if (glm::length(final_normals[i]) > 0.0f) {
            final_vertices[i].n = glm::normalize(final_normals[i]);
        } else {
            final_vertices[i].n = glm::vec3(0.0, 1.0, 0.0);
        }
    }

    m_VertexData.resize(final_vertices.size() * sizeof(Vertex) / sizeof(float));
    memcpy(m_VertexData.data(), final_vertices.data(), final_vertices.size() * sizeof(Vertex));

    m_IndexData = welded_mesh.indices;
    m_IndexCount = m_IndexData.size();
    m_GpuStatus = GpuStatus::NeedsGpuUpload;
}

void Chunk::generateFoliage(fnl_state& noise, const GenerationProfile& profile) {
    noise.noise_type = FNL_NOISE_PERLIN;
    noise.frequency = 0.8f;

    std::mt19937 rng(static_cast<unsigned int>(std::hash<std::string>{}(std::to_string(m_Position.x) + "_" + std::to_string(m_Position.z))));
    std::uniform_real_distribution<float> scaleDist(0.8f, 1.5f);
    std::uniform_real_distribution<float> rotDist(0.0f, 2.0f * 3.14159f);

    auto get_density = [&](int x, int y, int z) {
        if (x < 0 || x > CHUNK_SIZE || y < 0 || y > CHUNK_SIZE || z < 0 || z > CHUNK_SIZE) return 1.0f; // Treat out of bounds as air
        int index = x + z * (CHUNK_SIZE + 1) + y * (CHUNK_SIZE + 1) * (CHUNK_SIZE + 1);
        return m_NoiseData[index];
    };

    for (int x = 1; x < CHUNK_SIZE - 1; ++x) {
        for (int z = 1; z < CHUNK_SIZE - 1; ++z) {
            // Scan downwards from the top of the chunk to find the surface
            for (int y = CHUNK_SIZE - 1; y >= 1; --y) {
                float density_here = get_density(x, y, z);
                float density_below = get_density(x, y - 1, z);

                // Check for a transition from air (positive density) to solid (negative density)
                if (density_here >= 0.0f && density_below < 0.0f) {
                    float worldY = m_Position.y + y;
                    if (worldY < profile.water_level + 1.0f) break;

                    // Approximate the surface normal using the gradient of the density field
                    glm::vec3 normal = glm::normalize(glm::vec3(
                        get_density(x - 1, y, z) - get_density(x + 1, y, z),
                        get_density(x, y - 1, z) - get_density(x, y + 1, z),
                        get_density(x, y, z - 1) - get_density(x, y, z + 1)
                    ));

                    // Only place foliage on relatively flat ground (normal pointing up)
                    if (normal.y > 0.85f) {
                        float worldX_foliage = m_Position.x + x;
                        float worldZ_foliage = m_Position.z + z;
                        float foliage_value = fnlGetNoise2D(&noise, worldX_foliage, worldZ_foliage);
                        
                        // Use noise to decide whether to place a tree or bush
                        if (foliage_value > 0.7f) {
                            m_TreeInstances.push_back({ glm::vec3(x, y, z), scaleDist(rng), rotDist(rng) });
                        } else if (foliage_value > 0.6f) {
                            m_BushInstances.push_back({ glm::vec3(x, y - 0.5f, z), scaleDist(rng) * 0.5f, rotDist(rng) });
                        }
                    }
                    
                    // We found the surface for this (x,z) column, so we can stop scanning down
                    break; 
                }
            }
        }
    }
}


void Chunk::uploadToGpu() {
    if (m_IndexCount > 0) {
        struct Vertex { glm::vec3 p, n; };

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

void Chunk::generateWaterMesh(const GenerationProfile& profile) {
    float waterY = profile.water_level - m_Position.y;

    if (m_Position.y > profile.water_level || m_Position.y + CHUNK_SIZE < profile.water_level) {
        m_WaterVertexCount = 0;
        return;
    }

    // This struct should only contain what the shader needs.
    // The shader only needs position.
    struct WaterVertex {
        glm::vec3 position;
    };

    std::vector<WaterVertex> vertices;

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
