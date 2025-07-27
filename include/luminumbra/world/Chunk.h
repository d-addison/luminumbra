// include/luminumbra/world/Chunk.h
#pragma once

#include <vector>
#include <glm/glm.hpp>
#include <string>
#include <atomic>

struct fnl_state; 

namespace Luminumbra::World {

// Biome types based on the README
enum class BiomeType {
    WHISPERING_GLADE,    // Gentle rolling hills with grass
    CRYSTAL_GROVES,      // Jagged crystalline formations
    SUNKEN_HOLLOWS,      // Valley/depression areas
    CANOPY_BRIDGES,      // High altitude connecting structures
    SKY_VOID            // Empty sky areas between islands
};

class Chunk {
public:
    Chunk(const glm::ivec3& position, const std::string& seed, int lod);
    ~Chunk();

    enum class GpuStatus { NeedsGpuUpload, Uploaded };

    // No copying chunks
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    
    void render() const;
    bool isSolid(const glm::vec3& worldPosition) const;

    const glm::mat4& getModelMatrix() const { return m_ModelMatrix; }
    static constexpr int CHUNK_SIZE = 32;
    static constexpr float WATER_LEVEL = 60.0f;

    void uploadToGpu(); // The new method for GPU work
    bool isReadyForGpu() const { return m_GpuStatus == GpuStatus::NeedsGpuUpload; }

private:
    void generateNoiseData(fnl_state& noise);
    void generateMesh(int lod);
    void generateWaterMesh();
    BiomeType getBiomeAt(float worldX, float worldZ, float height);
    glm::vec3 getTerrainColor(float worldY, float density, BiomeType biome);

    glm::ivec3 m_Position;
    glm::mat4 m_ModelMatrix;
    int m_LOD;

    // Rendering
    unsigned int m_VAO = 0, m_VBO = 0, m_EBO = 0;
    int m_IndexCount = 0;
    unsigned int m_WaterVAO = 0, m_WaterVBO = 0;
    int m_WaterVertexCount = 0;

    // Voxel data
    std::vector<float> m_NoiseData;
    std::vector<BiomeType> m_BiomeData;

    std::vector<float> m_VertexData;
    std::vector<unsigned int> m_IndexData;
    std::atomic<GpuStatus> m_GpuStatus{GpuStatus::NeedsGpuUpload};
};

} // namespace Luminumbra::World
