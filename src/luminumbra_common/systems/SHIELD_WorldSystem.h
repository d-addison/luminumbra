#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "../world/Chunk.h"
#include "../core/JobSystem.h"
#include <unordered_map>
#include <memory>
#include <vector>
#include <functional>
#include "entt/entt.hpp"
#include "FastNoise/FastNoise.h"

namespace Luminumbra::Systems {

class PhysicsSystem;
class WaterSystem;
struct TerrainGenParams {
    float base_frequency = 0.01f;
    float base_amplitude = 50.0f;
    int octaves = 4;
    float persistence = 0.5f;
    float lacunarity = 2.0f;
    float height_offset = 0.0f;

    bool caves_enabled = true;
    float cave_frequency = 0.02f;
    float cave_threshold = 0.7f;
    float cave_carve_value = 2.0f;

    bool island_mask_enabled = false;
    float island_mask_frequency = 0.004f;
};

struct ChunkLOD {
    int level;      // The LOD identifier (0 = highest detail)
    int step;       // The step size for Marching Cubes (1, 2, 4, etc.)
    float distance; // The maximum camera distance at which this LOD is used
};

class WaterSystem;

class SHIELD_WorldSystem {
public:
    SHIELD_WorldSystem(JobSystem* job_system, WaterSystem* water_system, const TerrainGenParams& params, int seed);
    ~SHIELD_WorldSystem();

    void GenerateChunkData(::Luminumbra::Chunk& chunk) const;
    void update(entt::registry& registry, const Vec3& camera_position, PhysicsSystem* physics_system);
    std::vector<::Luminumbra::Chunk*> get_renderable_chunks();
    float get_density_at(const Vec3& world_pos) const;
    float GetTerrainHeightAt(float world_x, float world_z) const;

    static IVec3 world_to_chunk_coords(const Vec3& position);

    // --- API for WorldGenViewer ---
    const TerrainGenParams& get_params() const { return m_params; }
    void set_params(const TerrainGenParams& params);
    void set_seed(int seed);
    void regenerate_all_chunks(PhysicsSystem* physics_system);
    void clear_world(PhysicsSystem* physics_system);
    void SetWaterSystem(WaterSystem* water_system);
    std::vector<IVec3> GetInitialChunkLoadList(const Vec3& center_pos) const; // <<< NEW
    JobHandle dispatch_generation_jobs(const std::vector<IVec3>& chunks_to_generate);
    float get_density_at_from_precalculated(const Vec3& world_pos, float terrain_height) const;
    
    // GPU SDF generation integration
    void SetGPUSDFCallback(std::function<bool(const IVec3&, const TerrainGenParams&, int, std::vector<float>&)> callback);
    
private:
    std::function<bool(const IVec3&, const TerrainGenParams&, int, std::vector<float>&)> m_gpu_sdf_callback;

private:
    struct StreamingState {
        std::unordered_map<ChunkID, std::shared_ptr<::Luminumbra::Chunk>> chunks;
        JobHandle generation_job_handle;
        JobHandle meshing_job_handle;
    };

    StreamingState m_streaming_state;

    const std::vector<ChunkLOD> m_lod_levels = {
        {0, 1, 96.0f},   // LOD 0: Full detail up to 96 meters (~6 chunks)
        {1, 2, 192.0f},  // LOD 1: Half resolution up to 192 meters (~12 chunks)
        {2, 4, 512.0f}   // LOD 2: Quarter resolution up to 512 meters (~32 chunks)
    };
    int get_lod_level_for_distance(float dist) const;
    int get_lod_step_for_level(int lod_level) const;

    // --- Helper Functions ---
    void update_chunk_activation(const Vec3& player_pos, PhysicsSystem* physics_system);
    // Signature updated to use shared_ptr
    void dispatch_meshing_jobs(const std::vector<std::pair<std::shared_ptr<::Luminumbra::Chunk>, int>>& chunks_to_mesh);
    void wait_for_generation_jobs();
    void wait_for_meshing_jobs();
    void reinitialize_noise();

    int m_update_tick_counter = 0;

    // --- Dependencies ---
    JobSystem* m_job_system;
    TerrainGenParams m_params;
    int m_seed;

    // Noise states for procedural generation
    FastNoise::SmartNode<FastNoise::Generator> m_terrain_generator;
    FastNoise::SmartNode<FastNoise::Generator> m_cave_generator;
    FastNoise::SmartNode<FastNoise::Generator> m_island_mask_generator;

    WaterSystem* m_water_system;
};

} // namespace Luminumbra::Systems
