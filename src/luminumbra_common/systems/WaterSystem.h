#pragma once
#include "../../../include/luminumbra/core/Types.h"
#include "../core/JobSystem.h"
#include "entt/entt.hpp"
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Luminumbra {
class Chunk;
}
namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
}
namespace Luminumbra::Systems {
class WeatherSystem;
} // namespace Luminumbra::Systems
namespace Luminumbra::Components {
struct TransformComponent;
}

namespace Luminumbra::Systems {

// Adaptive water grid resolution levels
enum class WaterDetailLevel {
    Off = 0,    // No simulation
    Low = 4,    // 4x4 grid (16 cells)
    Medium = 8, // 8x8 grid (64 cells) - current default
    High = 16,  // 16x16 grid (256 cells)
    Ultra = 32  // 32x32 grid (1024 cells)
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

    void update(entt::registry& registry,
                const std::unordered_map<ChunkID, std::shared_ptr<Chunk>>& active_chunks);

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

     * @brief Applies a displacement to the water simulation (e.g., from an explosion or object
     falling).

     * @param world_pos The center of the displacement.

     * @param volume The amount of water to displace (can be negative to create a hole).

     */

    void apply_displacement(const Vec3& world_pos, f32 volume);

    // TERRAFORM the water bed: adjust water_bed_mm by delta_mm (dig<0 / dam>0) for
    // cells within radius_m of world_pos + wake them; the fixed-point solver then drains/pools for
    // free. Deterministic (replicate as a command for host==peer). Returns cells edited.
    int EditTerrainBed(const Vec3& world_pos, std::int32_t delta_mm, float radius_m);

    //  FINITE HYDROLOGY: configure the conserved-water cycle. finite=true removes the perpetual
    // river source (water becomes finite/drainable); rain_mm_per_tick adds uniform rainfall (the
    // caller scales it by weather precipitation); evap_mm_per_tick recedes above-sea standing
    // water. All integer
    // -> deterministic. Defaults (false,0,0) == classic  behaviour, so gates stay green.
    void SetHydrology(bool finite, std::int32_t rain_mm_per_tick, std::int32_t evap_mm_per_tick) {
        m_finite_hydrology = finite;
        m_rain_mm_per_tick = rain_mm_per_tick;
        m_evap_mm_per_tick = evap_mm_per_tick;
    }

    //  ==: WEATHER-DRIVEN rain. When a WeatherSystem
    // is wired (the session owner gates this on sim.hydrology_weather; null = OFF,
    // byte-identical), each cell's rain becomes int(PrecipitationAt(cell)*scale+0.5)
    // INTEGER-QUANTIZED AT THE BOUNDARY (the only float->int crossing), then the
    // existing mm solver. The weather state read is the one updated earlier THIS
    // tick (the weather core runs before water in TickSimulation): a fixed 0-tick
    // phase, deterministic — the same documented convention scent uses for wind.
    void SetWeatherRain(const WeatherSystem* weather, std::int32_t scale_mm) {
        m_weather_rain = weather;
        m_weather_rain_scale_mm = scale_mm;
    }

    // water-source diagnostics (see the members): sources seen / mm injected this session.
    [[nodiscard]] std::int64_t debug_sources_seen() const {
        return m_debug_sources_seen;
    }
    [[nodiscard]] std::int64_t debug_source_injected_mm() const {
        return m_debug_source_injected_mm;
    }

    //  ( T.1): the WATER EPOCH — incremented by every terraform bed
    // edit that touched cells. Render-side consumers (the waterfall site cache)
    // fold it into their keys so a dammed river triggers ONE bounded re-survey
    // instead of per-frame re-detection. Deterministic (driven by sim edits).
    [[nodiscard]] std::uint64_t water_epoch() const {
        return m_water_epoch;
    }

    //  boot-settle mode. The per-tick init/sim caps exist to bound LIVE-play frame
    // cost; during the server BOOT water settle they make the fixed point unreachable:
    // init drains at MAX_WATER_INITS_PER_TICK=6 while the calm check exits early
    // (fresh-seeded chunks read asleep), and the cell-budget rotating window (64 chunks at Medium)
    // advances each awake chunk's ticks_below_threshold only once per rotation — with ~2900
    // awake chunks that is one sleep-counter step per ~45 ticks, x120 needed, >> any sane
    // settle cap (measured: the loaded heavy-oracle session exits its 400-cap with ALL 5433
    // water chunks still awake). Boot mode lifts BOTH caps (init ALL pending in parallel,
    // sim ALL awake chunks) so settle converges; loading-phase wall time is the only cost.
    // Set ONLY around the Boot settle loop — live-play behaviour is byte-identical.
    void SetBootSettleMode(bool on) {
        m_boot_settle_mode = on;
    }

    //  loaded-boot water pause: a session booted FROM A SAVE must not advance
    // water during Boot at all — the restored state (depths, sleep flags, counters) IS
    // the authoritative mid-flow state, and the water network flows perpetually (wet/dry
    // boundary cells limit-cycle and wake propagation re-wakes their neighbours —
    // measured: awake GROWS past 2500 of 5433 even after 3000 full-set settle
    // iterations), so any boot-side stepping advances the loaded session past the
    // original's saved state and the water sub-hash can never round-trip. Paused,
    // update is a no-op; live ticks resume from the exact loaded state.
    void SetBootPaused(bool on) {
        m_boot_paused = on;
    }

    // (water-kernel perf) Per-sub-phase wall timing (ms) from the latest update() —
    // RUNTIME TELEMETRY ONLY (never hashed, never persisted), the DbgStreamTimings
    // pattern from SHIELD_WorldSystem. Localizes where the per-tick water budget goes.
    struct DbgWaterTimings {
        double init = 0.0;        // adaptive-resize scan/apply + first-time grid seeding
        double sim = 0.0;         // per-chunk fixed-point kernel (StepChunkWaterFixed)
        double seam = 0.0;        // cross-chunk owner-edge shared-flux pass
        double bookkeeping = 0.0; // sources, wake/sleep scans, window selection, mass telemetry
    };
    [[nodiscard]] const DbgWaterTimings& dbg_water_timings() const {
        return m_dbg_water;
    }

    // the rotating sim-window cursor is EVOLUTION-RELEVANT sim state whenever
    // more chunks are awake than the derived sim window (which window sims
    // first changes subsequent depths). It stays in CHUNK-INDEX space — the per-tick window
    // length is derived from the MAX_WATER_CELLS_PER_TICK budget, a constant 64 chunks at the
    // uniform Medium resolution. It is persisted with the world (world_info.json
    // waterSimCursor) and restored on load so a loaded session resimulates the exact
    // same windows the original would from the same state. Not itself hashed.
    [[nodiscard]] std::size_t GetSimWindowCursor() const {
        return m_water_sim_cursor;
    }
    void SetSimWindowCursor(std::size_t cursor) {
        m_water_sim_cursor = cursor;
    }

    // --- Adaptive Water Grid System ---

    /**
     * @brief Calculate the required water detail level for a chunk
     * @param chunk The chunk to evaluate
     * @param camera_distance Distance from camera to chunk center
     * @param has_player_interaction Whether the chunk has recent player interaction
     * @return The required water detail level
     */
    WaterDetailLevel
    CalculateRequiredDetail(const Chunk& chunk, float camera_distance, bool has_player_interaction);

    /**
     * @brief Resize a chunk's water simulation grid
     * @param chunk The chunk to resize
     * @param new_level The new detail level
     */
    void ResizeSimulationGrid(Chunk& chunk, WaterDetailLevel new_level);

private:
    // (water performance contract) The float render-mirror sim (dispatch_simulation_jobs /
    // simulate_chunk_water + its WaterChunkSnapshot/Output/Neighbors structs) was DEAD CODE — zero
    // callers; the live path is the integer  StepChunkWaterFixed. Removed.
    Vec3 get_camera_position(entt::registry& registry) const;

    JobSystem* m_job_system;

    SHIELD_WorldSystem* m_shield_system;

    // Holds a read-only pointer to the main chunk map from SHIELD_WorldSystem.

    // This is updated each frame in the `update` call.

    const std::unordered_map<ChunkID, std::shared_ptr<Chunk>>* m_active_chunks = nullptr;

    //  finite-hydrology config (set via SetHydrology; integer -> deterministic).
    bool m_finite_hydrology = false;
    std::int32_t m_rain_mm_per_tick = 0;
    std::int32_t m_evap_mm_per_tick = 0;
    // weather-driven rain (null = OFF; see SetWeatherRain).
    const WeatherSystem* m_weather_rain = nullptr;
    std::int32_t m_weather_rain_scale_mm = 0;
    // water-source diagnostics (never hashed): source entities seen by the injection loop
    // + total mm actually injected. Localizes "the spring did nothing" between
    // the entity/view/chunk-lookup half and the mm-write half.
    std::int64_t m_debug_sources_seen = 0;
    std::int64_t m_debug_source_injected_mm = 0;
    std::uint64_t m_water_epoch = 0; //  (see water_epoch())

    // boot-settle mode (see SetBootSettleMode). Lifts the init/sim caps during Boot.
    bool m_boot_settle_mode = false;
    // loaded-boot water pause (see SetBootPaused). update is a no-op while set.
    bool m_boot_paused = false;

    //  implementation note (streaming-burst amortization): rotating cursor for the per-tick
    //  water-sim
    // budget. When more cells are active than MAX_WATER_CELLS_PER_TICK, we sim a DETERMINISTIC
    // window (sorted by chunk id) and rotate it each tick so every chunk sims over a few ticks
    // instead of all-at-once (which blocked the main thread ~450ms on m_job_system->wait when
    // moving into water). Deterministic (no job-timing dependence) — guarded by the
    // WaterDeterminism live-water gate.
    //  mass-conservation telemetry: last tick's total source/sink (mm) and whether the
    // integer mass invariant held (Σdepth change == Σsource − Σsink). Render/debug only — not
    // hashed.
public:
    [[nodiscard]] std::int64_t dbg_last_source_mm() const {
        return m_dbg_last_source_mm;
    }
    [[nodiscard]] std::int64_t dbg_last_sink_mm() const {
        return m_dbg_last_sink_mm;
    }
    [[nodiscard]] bool dbg_mass_ok() const {
        return m_dbg_mass_ok;
    }
    //  cross-chunk CONTINUITY proof — last tick's count of chunk-seam cell-pairs with
    // water depth>0 on BOTH sides (a river/lake spanning a chunk border). >0 proves cross-chunk
    // flux works.
    [[nodiscard]] int dbg_seam_wet_pairs() const {
        return m_dbg_seam_wet_pairs;
    }
    //  per-tick sim-load telemetry (never hashed, never persisted): chunks that were
    // awake/eligible before the rotating window was applied, and cells actually simulated
    // by the window this tick (chunks-in-window x cells-per-chunk at the uniform resolution).
    [[nodiscard]] std::size_t dbg_awake_water_chunks() const {
        return m_dbg_awake_water_chunks;
    }
    [[nodiscard]] std::size_t dbg_cells_simmed() const {
        return m_dbg_cells_simmed;
    }

private:
    std::int64_t m_dbg_last_source_mm = 0;
    std::int64_t m_dbg_last_sink_mm = 0;
    bool m_dbg_mass_ok = true;
    int m_dbg_seam_wet_pairs = 0;
    std::size_t m_dbg_awake_water_chunks = 0; // see dbg_awake_water_chunks()
    std::size_t m_dbg_cells_simmed = 0;       // see dbg_cells_simmed()
    DbgWaterTimings m_dbg_water;              // sub-phase telemetry (see dbg_water_timings())

    std::size_t m_water_sim_cursor = 0;
    //  implementation note: rotating cursor for the per-tick water-grid RESIZE budget (see
    // MAX_WATER_RESIZES_PER_TICK). Same deterministic-window amortization as m_water_sim_cursor.
    std::size_t m_water_resize_cursor = 0;
    EntityID m_camera_entity = entt::null;
};

} // namespace Luminumbra::Systems
