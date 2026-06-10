#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "../world/Chunk.h"
#include "../core/JobSystem.h"
#include <array>
#include <cstddef>
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

struct WorldGenLayerSample {
    Vec3 world_pos{0.0f};

    float base_noise = 0.0f;
    float base_height = 0.0f;

    float island_noise = 0.0f;
    float island_mask = 1.0f;
    float final_height = 0.0f;

    float terrain_density = 0.0f;
    float cave_noise = 0.0f;
    float cave_value = 0.0f;
    float cave_density = 0.0f;
    float final_density = 0.0f;

    bool island_applied = false;
    bool caves_applied = false;
    bool solid = false;
    MaterialType material = MaterialType::Air;
};

struct ChunkLOD {
    int level;      // The LOD identifier (0 = highest detail)
    int step;       // The step size for Marching Cubes (1, 2, 4, etc.)
    float distance; // The maximum camera distance at which this LOD is used
};

class WaterSystem;

class SHIELD_WorldSystem {
public:
    struct StreamingBudgetFrameStats {
        int update_interval_frames = 0;
        int requested_render_radius = 0;
        int target_render_radius = 0;
        int generation_budget = 0;
        int meshing_budget = 0;
        std::size_t max_active_chunks_budget = 0;
        std::size_t active_chunks_before = 0;
        std::size_t active_chunks_after = 0;
        std::size_t ready_chunks = 0;
        std::size_t renderable_chunks = 0;
        std::size_t idle_chunks = 0;
        std::size_t loading_chunks = 0;
        std::size_t meshing_chunks = 0;
        std::size_t target_surface_columns = 0;
        std::size_t generation_candidates = 0;
        std::size_t surface_generation_candidates = 0;
        std::size_t vertical_generation_candidates = 0;
        std::size_t scheduled_generation = 0;
        std::size_t surface_generation_scheduled = 0;
        std::size_t vertical_generation_scheduled = 0;
        std::size_t deferred_generation = 0;
        std::size_t meshing_candidates = 0;
        std::size_t scheduled_meshing = 0;
        std::size_t deferred_meshing = 0;
        std::size_t unloaded_chunks = 0;
        bool generation_job_active = false;
        bool meshing_job_active = false;
    };

    struct RuntimeChunkStats {
        std::size_t total_chunks = 0;
        std::size_t unloaded_chunks = 0;
        std::size_t loading_chunks = 0;
        std::size_t idle_chunks = 0;
        std::size_t meshing_chunks = 0;
        std::size_t ready_chunks = 0;
        std::size_t unloading_chunks = 0;
        std::size_t renderable_chunks = 0;
        std::size_t collision_chunks = 0;
        std::size_t terrain_vertex_count = 0;
        std::size_t terrain_index_count = 0;
        std::size_t water_vertex_count = 0;
        std::size_t water_index_count = 0;
        std::size_t terrain_payload_bytes = 0;
        bool generation_job_active = false;
        bool meshing_job_active = false;
    };

    struct StreamingTelemetryStats {
        std::size_t peak_queue_depth = 0;
        std::size_t peak_meshing_candidates = 0;
        std::size_t cumulative_scheduled_meshing = 0;
        std::size_t cumulative_deferred_meshing = 0;
        uint64_t max_deferred_age_frames = 0;
        std::size_t last_queue_depth = 0;
        uint64_t frames_observed = 0;
    };

    struct CameraLocalCoverageStats {
        Vec3 camera_position{0.0f};
        IVec3 camera_chunk{0};
        IVec3 surface_chunk_under_camera{0};
        int horizontal_radius = 0;
        float terrain_height_under_camera = 0.0f;
        float camera_height_above_terrain = 0.0f;
        std::size_t expected_surface_chunks = 0;
        std::size_t present_surface_chunks = 0;
        std::size_t missing_surface_chunks = 0;
        std::size_t unloaded_surface_chunks = 0;
        std::size_t loading_surface_chunks = 0;
        std::size_t idle_surface_chunks = 0;
        std::size_t meshing_surface_chunks = 0;
        std::size_t ready_surface_chunks = 0;
        std::size_t renderable_surface_chunks = 0;
        std::size_t collision_surface_chunks = 0;
        std::size_t pending_lod_chunks = 0;
        std::array<std::size_t, 3> lod_counts{0u, 0u, 0u};
        std::size_t lod_unknown_chunks = 0;
        bool center_chunk_present = false;
        bool center_chunk_renderable = false;
        bool near_field_renderable = false;
    };

    SHIELD_WorldSystem(JobSystem* job_system, WaterSystem* water_system, const TerrainGenParams& params, int seed);
    ~SHIELD_WorldSystem();

    void GenerateChunkData(::Luminumbra::Chunk& chunk) const;
    void update(entt::registry& registry, const Vec3& camera_position, PhysicsSystem* physics_system);
    std::vector<::Luminumbra::Chunk*> get_renderable_chunks();
    float get_density_at(const Vec3& world_pos) const;
    float GetTerrainHeightAt(float world_x, float world_z) const;
    WorldGenLayerSample SampleWorldGenLayers(const Vec3& world_pos) const;

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
    bool EnsureCollisionReadyNear(const Vec3& world_pos, PhysicsSystem* physics_system, int horizontal_radius = 1);
    bool EnsureSurfaceReadyNear(const Vec3& world_pos, PhysicsSystem* physics_system, int surface_radius, int collision_radius);
    const StreamingBudgetFrameStats& get_last_streaming_budget_stats() const { return m_last_streaming_budget_stats; }
    const StreamingTelemetryStats& get_streaming_telemetry_stats() const { return m_streaming_telemetry_stats; }
    const std::vector<ChunkLOD>& get_lod_levels() const { return m_lod_levels; }
    RuntimeChunkStats get_runtime_chunk_stats() const;
    CameraLocalCoverageStats get_camera_local_coverage_stats(const Vec3& camera_position, int horizontal_radius) const;
    float get_density_at_from_precalculated(const Vec3& world_pos, float terrain_height) const;
    
    // GPU SDF generation integration
    void SetGPUSDFCallback(std::function<bool(const IVec3&, const TerrainGenParams&, int, std::vector<float>&)> callback);

    // --- Persistence integration (runtime save/load, T-I2-12) ---
    // Blocks until in-flight generation and meshing jobs complete so chunk
    // voxel/mesh data is stable for hashing and serialization.
    void wait_for_streaming_jobs();
    // Shared-ownership snapshot of every streamed chunk (save path).
    std::vector<std::shared_ptr<::Luminumbra::Chunk>> snapshot_streamed_chunks() const;
    std::shared_ptr<::Luminumbra::Chunk> find_streamed_chunk(const IVec3& coords) const;
    // Adopts an externally loaded chunk when its slot is empty. Returns false
    // (without clobbering the streamed chunk) when a chunk with the same id
    // is already active.
    bool adopt_streamed_chunk(const std::shared_ptr<::Luminumbra::Chunk>& chunk);

private:
    std::function<bool(const IVec3&, const TerrainGenParams&, int, std::vector<float>&)> m_gpu_sdf_callback;

private:
    struct StreamingState {
        std::unordered_map<ChunkID, std::shared_ptr<::Luminumbra::Chunk>> chunks;
        JobHandle generation_job_handle;
        // Meshing work is split across two batches per dispatch: hole-fill
        // candidates (no active mesh yet) ride the High job lane so visible
        // gaps close ahead of bulk LOD/water remeshes on the Normal lane.
        JobHandle meshing_job_handle;
        JobHandle meshing_job_handle_high;
        struct MeshingJobChunk {
            std::shared_ptr<::Luminumbra::Chunk> chunk;
            bool terrain_mesh_required = true;
            u8 transition_faces = 0;
        };
        std::vector<MeshingJobChunk> meshing_job_chunks;
    };

    StreamingState m_streaming_state;
    StreamingBudgetFrameStats m_last_streaming_budget_stats;
    StreamingTelemetryStats m_streaming_telemetry_stats;
    uint64_t m_deferred_backlog_age_frames = 0;

    const std::vector<ChunkLOD> m_lod_levels = {
        {0, 1, 192.0f},  // LOD 0: Full detail up to 192 meters (~12 chunks)
        {1, 2, 384.0f},  // LOD 1: Half resolution up to 384 meters (~24 chunks)
        {2, 4, 640.0f}   // LOD 2: Quarter resolution beyond the near visual range
    };
    int get_lod_level_for_distance(float dist) const;
    int get_lod_step_for_level(int lod_level) const;

    // Required streaming LOD for a chunk: horizontal-only distance for
    // surface-band chunks (keeps the terrain surface on one LOD per column
    // so vertical LOD seams cannot open), 3D distance for deep/air chunks.
    int get_required_lod_for_chunk(
        const IVec3& coords,
        const Vec3& chunk_center,
        const Vec3& camera_position);

    // Chunk-y of the terrain surface for a horizontal column, cached for the
    // lifetime of the current seed/params (terrain height is deterministic).
    int column_surface_chunk_y(int chunk_x, int chunk_z);
    std::unordered_map<u64, int> m_column_surface_chunk_y_cache;

    // --- Helper Functions ---
    void update_chunk_activation(const Vec3& player_pos, PhysicsSystem* physics_system);
    // Signature updated to use shared_ptr
    struct MeshingWorkItem {
        std::shared_ptr<::Luminumbra::Chunk> chunk;
        int lod_level = 0;
        bool terrain_mesh_required = true;
        // Near-field hole-fill work rides the High job lane (see
        // MAX_HIGH_PRIORITY_MESHING_JOBS_PER_DISPATCH in the .cpp).
        bool high_priority = false;
    };
    void dispatch_meshing_jobs(const std::vector<MeshingWorkItem>& chunks_to_mesh);
    void process_completed_meshing_jobs();
    bool meshing_jobs_active() const;
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
