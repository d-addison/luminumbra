#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include <vector>
#include <atomic>

namespace Luminumbra {

// Represents the state of a chunk in its lifecycle.
// This is atomic to allow thread-safe state transitions between the main thread
// and the job system (e.g., during meshing).
enum class ChunkState : u8 {
    Unloaded,      // No data in memory.
    Loading,       // Data is being loaded from disk or generated.
    Idle,          // Data is loaded, but no mesh has been generated.
    Meshing,       // A job is actively generating the mesh for this chunk.
    Ready,         // Mesh is generated and ready for rendering.
    Unloading      // Marked for removal, data will be freed soon.
};

// A single vertex for a chunk's polygonal mesh.
// This layout is optimized for sending directly to the GPU.
struct VoxelVertex {
    Vec3 position;   // 12 bytes
    Vec3 normal;     // 12 bytes
    u32 material_id; // 4 bytes (e.g., grass, rock, dirt)
}; // Total size: 28 bytes

class Chunk {
public:
    // --- Constructor & Identifier ---
    Chunk(const IVec3& coords);
    const IVec3& get_coords() const { return m_coords; }
    ChunkID get_id() const { return m_id; }

    // --- State Management ---
    ChunkState get_state() const { return m_state.load(); }
    void set_state(ChunkState new_state) { m_state.store(new_state); }

    // --- Voxel Data ---
    // The core simulation data. A tightly packed array representing the
    // Signed Distance Field (SDF) value for each voxel in the chunk.
    // A positive value is "outside" a surface, negative is "inside".
    // This is the source of truth for both meshing and far-field ray tracing.
    // **Optimization:** This could be a custom bit-packed array later,
    // but a simple float array is sufficient for the initial implementation.
    std::vector<f32> sdf_data;

    // --- Render Data ---
    // These vectors are populated by the Marching Cubes algorithm.
    // They are empty until the chunk's state is 'Ready'.
    std::vector<VoxelVertex> mesh_vertices;
    std::vector<u32> mesh_indices;

    // Helper function to compute a unique 64-bit ID from 3D coordinates.
    static ChunkID calculate_id(const IVec3& coords);

private:
    const IVec3 m_coords; // Integer coordinates in the world grid.
    const ChunkID m_id;   // Pre-calculated unique ID.

    // Thread-safe state for interaction with the job system.
    std::atomic<ChunkState> m_state;
};

} // namespace Luminumbra
