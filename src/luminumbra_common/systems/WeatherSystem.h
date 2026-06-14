#pragma once

// T-I5a-3 (B1): deterministic weather core (sim-authoritative).
//
// MODEL (design-decisions.md §6 Weather core B1):
//   * A slow "weather pressure" base field from a `seed+12` low-frequency noise,
//     MODULATED by a deterministic temperature/humidity climate pair (the same
//     FBm-Simplex family the iteration-4 +8/+9 climate channels use) -> a region
//     weather CATEGORY {clear, overcast, rain, snow, fog}. The category is the
//     spatial weather classification at a world position.
//   * Discrete STORM CELLS spawned on a deterministic seeded schedule derived
//     from (seed+12, region, tick-epoch) -- NO wall-clock, NO std::random on the
//     tick path. Each storm cell is ADVECTED every tick by the A2 wind grid
//     (SampleWind), carrying an intensity that ramps in and decays out over a
//     fixed lifetime. Bounded: <= 16 active storm cells (critique F9).
//   * A precipitation-intensity field over the streamed extent (flat memory,
//     FieldGrid<float>): base precip from the category plus additive storm-cell
//     contribution. Bounded: a fixed-size grid (no growth).
//
// DETERMINISM. Every transcendental goes through core/DeterministicMath.h (NO
// libm sin/cos/exp on the tick path) so the field is bit-deterministic across
// runs and machines and passes SimDeterminismLint. The whole weather state is a
// pure function of (seed, tick, region anchor); a save/load/resim reaches the
// same state at the same tick because all three inputs reproduce. The base
// noise uses the FastNoise batch path (batch==sample parity, like wind/worldgen).
//
// DETERMINISM SURFACE. The weather state (category map + storm cells + precip
// field) feeds the `weather` world_hash sub-hash (design-decisions.md §2). The
// slot is laid out so a LATER lightning strike schedule (T-I5a-5) folds in
// without reordering the existing bytes (a reserved strike-schedule epoch is
// hashed as 0 in 5a; B3 replaces it).
//
// ONE-WAY (critique F2). This is a SIM system. The render overlay + wetness
// material response READ the replicated state via the public query API below and
// write NOTHING back into the sim or world_hash.

#include <cstdint>
#include <string>
#include <vector>

#include "FastNoise/FastNoise.h"

#include "../fields/FieldGrid.h"
#include "../../../include/luminumbra/core/Types.h"

namespace Luminumbra::Systems {

// Region weather category. The enum order is the canonical hash/serialization
// order; clear is the control-phase default (premise guard F4). Storms only
// raise precipitation; the category is the large-scale sky state.
enum class WeatherCategory : std::uint8_t {
    Clear = 0,
    Overcast = 1,
    Rain = 2,
    Snow = 3,
    Fog = 4,
};
inline constexpr int kWeatherCategoryCount = 5;
const char* WeatherCategoryName(WeatherCategory category) noexcept;

// PINNED geometry. The weather grid shares the wind grid's coarse region-
// following shape (24 m cells, 64-cell streamed extent) so the precip field and
// the category map align with the wind advection sampling. Vertical weather is
// ignored in 5a (a single 2D layer).
inline constexpr float kWeatherCellSizeM = 24.0f;
inline constexpr int kWeatherExtentCells = 64;

// PINNED bounded-state cap (critique F9 / design-decisions §7): at most this many
// storm cells are ever active. The container is fixed-capacity; a scheduled spawn
// is dropped when full (deterministically -- the schedule itself is bounded so
// this is belt-and-suspenders, asserted by Endurance300Storm).
inline constexpr int kMaxStormCells = 16;

// One active storm cell. Position/velocity are world-space (XZ); the cell is
// advected every tick by the wind grid. spawn_tick/lifetime_ticks bound the
// intensity envelope (deterministic ramp-in/decay). seed_salt makes each cell's
// later strike-schedule (T-I5a-5) an independent seeded stream.
struct StormCell {
    Vec2 center_world = Vec2(0.0f);
    Vec2 velocity = Vec2(0.0f);     // m/s-ish, set from wind at spawn + re-advected
    float intensity = 0.0f;         // current [0, 1] envelope value
    std::uint64_t spawn_tick = 0;
    std::uint32_t lifetime_ticks = 0;
    std::uint32_t seed_salt = 0;    // per-cell RNG salt (T-I5a-5 strike stream)
};

// Replicated weather snapshot at a world position -- what the render overlay +
// wetness response read (one-way). POD; carries no engine handles.
struct WeatherSample {
    WeatherCategory category = WeatherCategory::Clear;
    float precip_intensity = 0.0f;  // [0, 1] local precipitation
    float storm_intensity = 0.0f;   // [0, 1] nearest-storm contribution
    Vec2 wind = Vec2(0.0f);         // local wind vector (from the storm advection field)
};

class WindFieldSystem;

class WeatherSystem {
public:
    // seed is the WORLD seed; the weather noises use seed + 12 (seed-offset
    // registry, design-decisions.md §1 -- appended FIRST, append only, never
    // reusing +1..+11).
    explicit WeatherSystem(int world_seed);

    // Advance weather to `tick` anchored on the streamed region around
    // `region_anchor` (world-space; the spawn/stream anchor). Pure function of
    // (seed, tick, anchored origin) + the wind field for advection; no
    // wall-clock, no RNG. `wind` may be null (storm cells then drift on their
    // last velocity); production always supplies it.
    // Budget: <= 0.20 ms/tick at the streamed extent (WeatherVisual gate).
    void Update(std::uint64_t tick, const Vec3& region_anchor, const WindFieldSystem* wind);

    // --- Public weather-state query API (stable; consumed by T-I5a-5 lightning
    //     + T-I5a-4 precip + 5b ecology/audio). All are pure functions of the
    //     CURRENT state; none mutate. ---

    // Region weather category at a world position (the large-scale sky state).
    [[nodiscard]] WeatherCategory CategoryAt(const Vec3& world_pos) const;
    // Local precipitation intensity [0, 1] at a world position (category base +
    // storm contribution). In-region reads the precip field; out-of-region
    // returns the category base only.
    [[nodiscard]] float PrecipitationAt(const Vec3& world_pos) const;
    // Full replicated sample (category + precip + nearest-storm + advected wind).
    [[nodiscard]] WeatherSample SampleAt(const Vec3& world_pos) const;

    // The active storm cells (canonical order: spawn order, stable across the
    // tick). Consumed by T-I5a-5 (strike schedule) + T-I5a-4 (precip emitters).
    [[nodiscard]] const std::vector<StormCell>& StormCells() const noexcept { return m_storm_cells; }
    [[nodiscard]] int active_storm_count() const noexcept {
        return static_cast<int>(m_storm_cells.size());
    }

    // The dominant region category at the anchor (gate diagnostics / overlay).
    [[nodiscard]] WeatherCategory AnchorCategory() const noexcept { return m_anchor_category; }

    // Deterministic sub-hash over the weather state, in canonical order. Folded
    // into the world_hash `weather` slot (mega-bump #2).
    [[nodiscard]] std::string ComputeWeatherSubHash() const;

    // Geometry accessors (gate diagnostics / downstream readers).
    [[nodiscard]] int extent_cells() const noexcept { return m_grid_extent; }
    [[nodiscard]] float cell_size_m() const noexcept { return m_grid_cell_size; }
    [[nodiscard]] std::uint64_t last_tick() const noexcept { return m_last_tick; }

private:
    // Map a world position to a local cell (clamped result reported via in_region).
    void LocalCell(const Vec3& world_pos, int& out_lx, int& out_lz, bool& in_region) const;
    // Classify a (pressure, temperature, humidity) triple into a category.
    static WeatherCategory Classify(float pressure, float temperature, float humidity) noexcept;
    // Deterministic per-tick storm-cell spawn schedule + lifetime/advection.
    void StepStormCells(std::uint64_t tick, const Vec3& region_anchor, const WindFieldSystem* wind);
    // Rebuild the category map + precipitation field for the current tick.
    void RebuildFields(std::uint64_t tick, const Vec3& region_anchor);
    // Additive storm precipitation contribution at a world position [0, 1].
    [[nodiscard]] float StormPrecipAt(const Vec3& world_pos) const;
    // Nearest-storm intensity at a world position [0, 1] (for the render sample).
    [[nodiscard]] float StormIntensityAt(const Vec3& world_pos) const;

    int m_world_seed = 0;
    int m_weather_seed = 0; // m_world_seed + 12

    luminumbra::fields::FieldGrid<float> m_precip;          // precipitation intensity
    luminumbra::fields::FieldGrid<std::uint8_t> m_category; // per-cell WeatherCategory
    int m_grid_extent = 0;
    float m_grid_cell_size = 0.0f;
    Vec2 m_anchor_wind = Vec2(0.0f);
    WeatherCategory m_anchor_category = WeatherCategory::Clear;
    std::uint64_t m_last_tick = 0;

    // Bounded storm-cell set (<= kMaxStormCells). Ordered by spawn (canonical).
    // Each cell's seed_salt is derived deterministically from (seed, tick-epoch)
    // so it never depends on wall-clock/RNG; T-I5a-5 seeds its strike stream off it.
    std::vector<StormCell> m_storm_cells;

    // seed+12 weather noises (low-frequency FBm Simplex, batch-path parity).
    FastNoise::SmartNode<FastNoise::Generator> m_pressure_noise;    // base weather pressure
    FastNoise::SmartNode<FastNoise::Generator> m_temperature_noise; // climate modulation
    FastNoise::SmartNode<FastNoise::Generator> m_humidity_noise;    // climate modulation

    // Per-tick scratch (no tick-path heap allocation), sized to the grid.
    std::vector<float> m_scratch_px;
    std::vector<float> m_scratch_pz;
    std::vector<float> m_scratch_pressure;
    std::vector<float> m_scratch_temperature;
    std::vector<float> m_scratch_humidity;
};

} // namespace Luminumbra::Systems
