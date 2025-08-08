#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "../world/Chunk.h"
#include "../core/JobSystem.h"
#include <unordered_map>
#include <memory>
#include <vector>
#include "entt/entt.hpp"

namespace Luminumbra {
    class Chunk;
}

namespace Luminumbra::Systems {

struct TerrainGenParams {
    float base_frequency = 0.01f;
    float base_amplitude = 50.0f;
    int octaves = 4;
    float persistence = 0.5f;
    float lacunarity = 2.0f;
    float height_offset = 0.0f;
    bool caves_enabled = true;
    float cave_frequency = 0.02f;
};

class SHIELD_WorldSystem {
public:
    SHIELD_WorldSystem(JobSystem* job_system, const TerrainGenParams& params, int seed);

    void GenerateChunkData(Luminumbra::Chunk& chunk) const;

    // The main update function for the system, called once per simulation tick.
    // It takes the camera's position to determine the new set of active chunks.
    void update(entt::registry& registry, const Vec3& camera_position);

    // Provides the rendering system with a list of chunks ready to be drawn.
    std::vector<Chunk*> get_renderable_chunks();
    float get_density_at(const Vec3& world_pos) const;

private:
    // --- Chunk Management ---

    // The central storage for all loaded chunks in the world.
    // Using unique_ptr to avoid expensive copies and manage memory automatically.
    std::unordered_map<ChunkID, std::unique_ptr<Luminumbra::Chunk>> m_chunks;

    // --- Helper Functions ---

    // Identifies which chunks should be loaded or unloaded based on camera position.
    void update_chunk_activation(const Vec3& camera_position);

    // Dispatches jobs to generate the SDF data for newly loaded chunks.
    void dispatch_generation_jobs(const std::vector<IVec3>& chunks_to_generate);

    // Dispatches jobs to run Marching Cubes on chunks with loaded SDF data.
    void dispatch_meshing_jobs(const std::vector<Luminumbra::Chunk*>& chunks_to_mesh);

    // Converts a world-space position to chunk coordinates.
    static IVec3 world_to_chunk_coords(const Vec3& position);

    // --- Dependencies ---
    JobSystem* m_job_system; // A non-owning pointer to the engine's job system.
    TerrainGenParams m_params;
    int m_seed;
};

} // namespace Luminumbra::Systems
