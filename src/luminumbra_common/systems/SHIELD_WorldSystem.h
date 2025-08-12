#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "../world/Chunk.h"
#include "../core/JobSystem.h"
#include <unordered_map>
#include <memory>
#include <vector>
#include "entt/entt.hpp"
#include "FastNoiseLite.h"

namespace Luminumbra { class Chunk; }

namespace Luminumbra::Systems {

class PhysicsSystem;

struct TerrainGenParams {
    float base_frequency = 0.01f;
    float base_amplitude = 50.0f;
    int octaves = 4;
    float persistence = 0.5f;
    float lacunarity = 2.0f;
    float height_offset = 0.0f;

    bool caves_enabled = true;
    float cave_frequency = 0.02f;

    bool island_mask_enabled = false;
    float island_mask_frequency = 0.004f;
};

struct ChunkLOD {
    int level;      // The LOD identifier (0 = highest detail)
    int step;       // The step size for Marching Cubes (1, 2, 4, etc.)
    float distance; // The maximum camera distance at which this LOD is used
};

class SHIELD_WorldSystem {
public:
    SHIELD_WorldSystem(JobSystem* job_system, const TerrainGenParams& params, int seed);

    void GenerateChunkData(Luminumbra::Chunk& chunk) const;
    void update(entt::registry& registry, const Vec3& camera_position, PhysicsSystem* physics_system);
    std::vector<Chunk*> get_renderable_chunks();
    float get_density_at(const Vec3& world_pos) const;
    float GetTerrainHeightAt(float world_x, float world_z) const;

    // --- API for WorldGenViewer ---
    const TerrainGenParams& get_params() const { return m_params; }
    void set_params(const TerrainGenParams& params);
    void set_seed(int seed);
    void regenerate_all_chunks(PhysicsSystem* physics_system);
    void clear_world(PhysicsSystem* physics_system);


private:
    // --- Chunk Management ---
    // The central storage for all loaded chunks in the world.
    // Using shared_ptr for thread safety: ensures chunks aren't deleted by the main
    // thread while a worker thread is still processing them (e.g., meshing).
    std::unordered_map<ChunkID, std::shared_ptr<Luminumbra::Chunk>> m_chunks;

    const std::vector<ChunkLOD> m_lod_levels = {
        {0, 1, 96.0f},   // LOD 0: Full detail up to 96 meters (~6 chunks)
        {1, 2, 192.0f},  // LOD 1: Half resolution up to 192 meters (~12 chunks)
        {2, 4, 512.0f}   // LOD 2: Quarter resolution up to 512 meters (~32 chunks)
    };
    int get_lod_level_for_distance(float dist) const;

    // --- Helper Functions ---
    void update_chunk_activation(const Vec3& player_pos, PhysicsSystem* physics_system);
    void dispatch_generation_jobs(const std::vector<IVec3>& chunks_to_generate);
    // Signature updated to use shared_ptr
    void dispatch_meshing_jobs(const std::vector<std::pair<std::shared_ptr<Chunk>, int>>& chunks_to_mesh);
    static IVec3 world_to_chunk_coords(const Vec3& position);
    void reinitialize_noise();

    // --- Dependencies ---
    JobSystem* m_job_system;
    TerrainGenParams m_params;
    int m_seed;

    // Noise states for procedural generation
    fnl_state m_terrainNoise;
    fnl_state m_caveNoise;
    fnl_state m_islandMaskNoise;
};

} // namespace Luminumbra::Systems