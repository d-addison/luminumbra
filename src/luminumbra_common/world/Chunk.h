#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include <atomic>
#include <mutex>
#include <vector>
#include "core/Log.h"

namespace Luminumbra {

enum class ChunkState : u8 {
    Unloaded, Loading, Idle, Meshing, Ready, Unloading
};

struct VoxelVertex {
    Vec3 position;
    Vec3 normal;
    u32 material_id;
};

class Chunk {
public:
    Chunk(const IVec3& coords);
    const IVec3& get_coords() const { return m_coords; }
    ChunkID get_id() const { return m_id; }

    // Thread-safe state management
    ChunkState get_state() const { 
        std::lock_guard<std::mutex> lock(m_state_mutex);
        return m_state; 
    }
    
    void set_state(ChunkState new_state) {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_state = new_state;
    }

    // --- Voxel edit tracking ---
    // True when sdf/heightmap voxel data was mutated AFTER generation and has
    // not been persisted yet. Generation, loading, and meshing leave the flag
    // clear; runtime voxel edits must call mark_voxel_data_dirty(), and a
    // successful save clears it again.
    void mark_voxel_data_dirty() { m_voxel_data_dirty.store(true, std::memory_order_release); }
    void clear_voxel_data_dirty() { m_voxel_data_dirty.store(false, std::memory_order_release); }
    bool is_voxel_data_dirty() const { return m_voxel_data_dirty.load(std::memory_order_acquire); }

    // --- Voxel & SDF Data ---
    std::vector<f32> sdf_data;
    std::vector<f32> heightmap_data;

    // --- Render Data ---
    std::vector<VoxelVertex> mesh_vertices;
    std::vector<u32> mesh_indices;
    std::vector<VoxelVertex> water_mesh_vertices;
    std::vector<u32> water_mesh_indices;

    std::vector<VoxelVertex> pending_mesh_vertices;
    std::vector<u32> pending_mesh_indices;
    std::vector<VoxelVertex> pending_water_mesh_vertices;
    std::vector<u32> pending_water_mesh_indices;

    std::atomic<bool> has_collision{false};
    std::atomic<int> current_lod{-1};
    std::atomic<int> pending_lod{-1};
    // Bitmask of MarchingCubes::TerrainTransitionFace skirts baked into the
    // current mesh_vertices. Lets the streaming update detect coarse chunks
    // whose finer neighbors arrived AFTER this chunk was meshed (persistent
    // LOD seam cracks) without scanning mesh vertices per frame.
    std::atomic<u8> applied_transition_faces{0};
    std::atomic<bool> pending_mesh_ready{false};
    std::atomic<bool> pending_mesh_failed{false};
    std::atomic<u32> mesh_version{0};
    std::atomic<u32> water_mesh_version{0};
    
    // --- Water Simulation Data ---
    std::vector<f32> water_level_data;
    std::vector<Vec2> water_flow_data;
    std::vector<f32> water_sim_terrain_height;
    std::atomic<bool> has_water_sim{false};
    std::atomic<bool> water_mesh_generated{false};
    std::atomic<int> current_water_resolution{8}; // Current water grid resolution (4, 8, 16, or 32)

    // <<< OPTIMIZATION: Activity Culling State >>>
    std::atomic<bool> is_water_sleeping{false};
    // Max change in water level from the last sim tick. Only written by the chunk's own sim job.
    float max_water_delta_last_tick{0.0f};
    // How many consecutive ticks the water has been calm. Only accessed by the main thread.
    int ticks_below_threshold{0};
    // Coalesces water render mesh invalidation so simulation ticks do not force a remesh every frame.
    int water_mesh_dirty_ticks{0};

    static ChunkID calculate_id(const IVec3& coords);
    static bool is_valid_state_transition(ChunkState from, ChunkState to);
    bool try_set_state(ChunkState expected_state, ChunkState new_state);

private:
    const IVec3 m_coords;
    const ChunkID m_id;
    ChunkState m_state;
    mutable std::mutex m_state_mutex;  // mutable for use in const getter
    std::atomic<bool> m_voxel_data_dirty{false};
};

} // namespace Luminumbra
