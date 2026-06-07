#pragma once
#include "../core/JobSystem.h"
#include "entt/entt.hpp"
#include <unordered_map>
#include <memory>
#include <vector>
#include "../../../include/luminumbra/core/Types.h"

namespace Luminumbra { class Chunk; }
namespace Luminumbra::Systems { class SHIELD_WorldSystem; }
namespace Luminumbra::Components { struct TransformComponent; }

namespace Luminumbra::Systems {

// Adaptive water grid resolution levels
enum class WaterDetailLevel {
    Off = 0,      // No simulation
    Low = 4,      // 4x4 grid (16 cells)  
    Medium = 8,   // 8x8 grid (64 cells) - current default
    High = 16,    // 16x16 grid (256 cells)
    Ultra = 32    // 32x32 grid (1024 cells)
};

/**

 * @brief Manages the dynamic simulation of water flow and volume.

 * Operates on a 2.5D grid mapped across active chunks with adaptive resolution.

 */

class WaterSystem {

public:

    WaterSystem(JobSystem* job_system, SHIELD_WorldSystem* shield_system);


    /**

     * @brief The main simulation update, called once per game tick (30Hz).

     * @param registry The main ECS registry.

     * @param active_chunks A map of currently loaded chunks from the SHIELD system.

     */

    void update(entt::registry& registry, const std::unordered_map<ChunkID, std::shared_ptr<Chunk>>& active_chunks);


    // --- Public Queries ---


    /**

     * @brief Gets the interpolated water surface height at a specific world position.

     * @return The absolute world-space Y coordinate of the water surface.

     */

    f32 get_water_level_at(float world_x, float world_z) const;


    /**

     * @brief Gets the interpolated 2D flow velocity of water at a specific world position.

     * @return A Vec2 representing the horizontal flow vector.

     */

    Vec2 get_water_flow_at(float world_x, float world_z) const;


    /**

     * @brief Applies a displacement to the water simulation (e.g., from an explosion or object falling).

     * @param world_pos The center of the displacement.

     * @param volume The amount of water to displace (can be negative to create a hole).

     */

    void apply_displacement(const Vec3& world_pos, f32 volume);

    // --- Adaptive Water Grid System ---
    
    /**
     * @brief Calculate the required water detail level for a chunk
     * @param chunk The chunk to evaluate
     * @param camera_distance Distance from camera to chunk center
     * @param has_player_interaction Whether the chunk has recent player interaction
     * @return The required water detail level
     */
    WaterDetailLevel CalculateRequiredDetail(const Chunk& chunk, float camera_distance, bool has_player_interaction);
    
    /**
     * @brief Resize a chunk's water simulation grid
     * @param chunk The chunk to resize
     * @param new_level The new detail level
     */
    void ResizeSimulationGrid(Chunk& chunk, WaterDetailLevel new_level);


private:

    struct WaterChunkSnapshot {
        ChunkID id{};
        IVec3 coords{};
        int resolution = 0;
        std::vector<f32> water_levels;
        std::vector<Vec2> flow_data;
        std::vector<f32> terrain_height;
    };

    struct WaterChunkSimulationOutput {
        std::vector<f32> water_levels;
        std::vector<Vec2> flow_data;
        f32 max_delta = 0.0f;
    };

    struct WaterSimNeighbors {
        const WaterChunkSnapshot* north = nullptr;
        const WaterChunkSnapshot* south = nullptr;
        const WaterChunkSnapshot* east  = nullptr;
        const WaterChunkSnapshot* west  = nullptr;
    };

    void dispatch_simulation_jobs(const std::vector<Chunk*>& chunks_to_simulate);
    void simulate_chunk_water(const WaterChunkSnapshot& snapshot, const WaterSimNeighbors& neighbors, WaterChunkSimulationOutput& output);


    JobSystem* m_job_system;

    SHIELD_WorldSystem* m_shield_system;


    // Holds a read-only pointer to the main chunk map from SHIELD_WorldSystem.

    // This is updated each frame in the `update` call.

    const std::unordered_map<ChunkID, std::shared_ptr<Chunk>>* m_active_chunks = nullptr;


};


} // namespace Luminumbra::Systems
