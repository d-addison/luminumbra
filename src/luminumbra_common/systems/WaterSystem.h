#pragma once
#include "../core/JobSystem.h"
#include "entt/entt.hpp"
#include <cstdint>
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

    /**
     * @brief Sets the ECS entity whose TransformComponent drives camera-relative water LOD.
     * @param camera_entity The active camera entity, or entt::null to clear it.
     */
    void set_camera_entity(EntityID camera_entity);


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

    // Spec 009 Phase 2 — TERRAFORM the water bed: adjust water_bed_mm by delta_mm (dig<0 / dam>0) for
    // cells within radius_m of world_pos + wake them; the fixed-point solver then drains/pools for free.
    // Deterministic (replicate as a command for host==peer). Returns cells edited.
    int EditTerrainBed(const Vec3& world_pos, std::int32_t delta_mm, float radius_m);

    // Spec 010 FINITE HYDROLOGY: configure the conserved-water cycle. finite=true removes the perpetual
    // river source (water becomes finite/drainable); rain_mm_per_tick adds uniform rainfall (the caller
    // scales it by weather precipitation); evap_mm_per_tick recedes above-sea standing water. All integer
    // -> deterministic. Defaults (false,0,0) == classic Spec 009 behaviour, so gates stay green.
    void SetHydrology(bool finite, std::int32_t rain_mm_per_tick, std::int32_t evap_mm_per_tick) {
        m_finite_hydrology = finite;
        m_rain_mm_per_tick = rain_mm_per_tick;
        m_evap_mm_per_tick = evap_mm_per_tick;
    }

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
    Vec3 get_camera_position(entt::registry& registry) const;


    JobSystem* m_job_system;

    SHIELD_WorldSystem* m_shield_system;


    // Holds a read-only pointer to the main chunk map from SHIELD_WorldSystem.

    // This is updated each frame in the `update` call.

    const std::unordered_map<ChunkID, std::shared_ptr<Chunk>>* m_active_chunks = nullptr;

    // Spec 010 finite-hydrology config (set via SetHydrology; integer -> deterministic).
    bool m_finite_hydrology = false;
    std::int32_t m_rain_mm_per_tick = 0;
    std::int32_t m_evap_mm_per_tick = 0;

    // spec 008 follow-up (streaming-burst amortization): rotating cursor for the per-tick water-sim
    // budget. When more chunks are active than MAX_WATER_SIMS_PER_TICK, we sim a DETERMINISTIC window
    // (sorted by chunk id) and rotate it each tick so every chunk sims over a few ticks instead of
    // all-at-once (which blocked the main thread ~450ms on m_job_system->wait when moving into water).
    // Deterministic (no job-timing dependence) — guarded by the WaterDeterminism live-water gate.
    // Spec 009 mass-conservation telemetry (AC-2): last tick's total source/sink (mm) and whether the
    // integer mass invariant held (Σdepth change == Σsource − Σsink). Render/debug only — not hashed.
public:
    [[nodiscard]] std::int64_t dbg_last_source_mm() const { return m_dbg_last_source_mm; }
    [[nodiscard]] std::int64_t dbg_last_sink_mm() const { return m_dbg_last_sink_mm; }
    [[nodiscard]] bool dbg_mass_ok() const { return m_dbg_mass_ok; }
    // Spec 009 Phase 3: cross-chunk CONTINUITY proof — last tick's count of chunk-seam cell-pairs with
    // water depth>0 on BOTH sides (a river/lake spanning a chunk border). >0 proves cross-chunk flux works.
    [[nodiscard]] int dbg_seam_wet_pairs() const { return m_dbg_seam_wet_pairs; }
private:
    std::int64_t m_dbg_last_source_mm = 0;
    std::int64_t m_dbg_last_sink_mm = 0;
    bool m_dbg_mass_ok = true;
    int m_dbg_seam_wet_pairs = 0;

    std::size_t m_water_sim_cursor = 0;
    // spec 008 follow-up: rotating cursor for the per-tick water-grid RESIZE budget (see
    // MAX_WATER_RESIZES_PER_TICK). Same deterministic-window amortization as m_water_sim_cursor.
    std::size_t m_water_resize_cursor = 0;
    EntityID m_camera_entity = entt::null;


};


} // namespace Luminumbra::Systems
