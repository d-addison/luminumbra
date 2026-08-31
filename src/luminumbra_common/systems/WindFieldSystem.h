#pragma once

// Deterministic coarse 2.5D wind grid (simulation-authoritative).
//
// Shape: 24 m cells, three layers
// (ground 0-32 m / mid 32-128 m / high 128-512 m AGL), over the streamed extent,
// on the 30 Hz tick. Each cell holds a 2D HORIZONTAL wind vector per layer
// (vertical wind is not modeled). The grid follows the streamed region; samples
// outside the region clamp to the base large-scale direction.
//
// UPDATE: the base large-scale direction is a `seed+11` low-frequency noise
// sampled by tick-time via the FastNoise batch path (sample-path == batch-path
// parity, like the worldgen noises); a cheap per-cell spatial blend adds local
// variation. Weather storm cells add bounded, localized gust and swirl
// perturbations. All math goes through DeterministicMath (NO libm
// sin/cos/exp on the tick path) so the field is bit-deterministic across runs
// and machines and passes SimDeterminismLint.
//
// Determinism: the wind cell values feed the `wind` world_hash sub-hash. The
// field is a pure function of seed, tick, region origin, and the sorted storm
// perturbations; save/load/resimulation reaches the same field at the same tick
// when those inputs match.

#include <cstdint>
#include <string>
#include <vector>

#include "FastNoise/FastNoise.h"

#include "../../../include/luminumbra/core/Types.h"
#include "../fields/FieldGrid.h"

namespace Luminumbra::Systems {

// Wind layers (AGL bands). Vertical wind is not modeled; each layer carries a
// 2D horizontal vector. The enum order is the canonical per-cell hash order.
enum class WindLayer : int {
    Ground = 0, // 0-32 m AGL
    Mid = 1,    // 32-128 m AGL
    High = 2,   // 128-512 m AGL
};
inline constexpr int kWindLayerCount = 3;

// Grid geometry is part of the deterministic runtime contract.
inline constexpr float kWindCellSizeM = 24.0f;
// Streamed extent the grid covers, in cells. A 24 m cell * 64 = 1536 m square
// region centred on the spawn/stream anchor -- comfortably covers the radius-4
// streaming footprint and the production view-distance band sampled by weather
// advection, rain slant, and foliage displacement.
inline constexpr int kWindExtentCells = 64;

// Layer AGL bands (metres) -- the boundaries that map a sample height to a layer.
inline constexpr float kWindGroundTopM = 32.0f;
inline constexpr float kWindMidTopM = 128.0f;
inline constexpr float kWindHighTopM = 512.0f;

// One grid cell: a horizontal wind vector for each of the 3 layers.
struct WindCell {
    Vec2 layer[kWindLayerCount] = {Vec2(0.0f), Vec2(0.0f), Vec2(0.0f)};
};

class WindFieldSystem {
public:
    // seed is the world seed; wind base-direction noise uses the dedicated
    // append-only seed offset +11.
    explicit WindFieldSystem(int world_seed);

    // Advance the field to `tick` anchored on the streamed region around
    // `region_anchor` (world-space; usually the spawn/stream anchor). Pure
    // function of (seed, tick, anchored origin); no wall-clock, no RNG.
    // Budget: <= 0.15 ms/tick at the streamed extent (WindFieldDeterminism gate).
    void Update(std::uint64_t tick, const Vec3& region_anchor);

    // Public sampling API consumed by weather advection, rain slant, and foliage
    // displacement. Returns the horizontal wind vector at the world position
    // for the layer covering world_pos.y. AGL is approximated by the absolute
    // height band because the field does not depend on per-column ground height.
    [[nodiscard]] Vec2 SampleWind(const Vec3& world_pos) const;
    // Explicit-layer variant: ignores world_pos.y, samples the named layer.
    [[nodiscard]] Vec2 SampleWind(const Vec3& world_pos, WindLayer layer) const;

    // The current base large-scale direction (unit-ish vector) -- what
    // out-of-region samples clamp to. Exposed for cloud scrolling and gate
    // diagnostics.
    [[nodiscard]] Vec2 BaseDirection() const noexcept {
        return m_base_direction;
    }

    // Queue localized storm gusts for the next Update. The queue is sorted by
    // value so caller iteration order cannot change the resulting field.
    struct StormPerturbation {
        Vec2 center_world = Vec2(0.0f);
        float radius_m = 0.0f;
        Vec2 velocity = Vec2(0.0f);
        float intensity = 0.0f;
    };
    void InjectStormPerturbation(const StormPerturbation& perturbation);
    void ClearStormPerturbations();

    // Deterministic sub-hash over the wind cell values, in the FieldGrid
    // canonical order. Folded into the world_hash `wind` slot.
    [[nodiscard]] std::string ComputeWindSubHash() const;

    // Geometry accessors (gate diagnostics / downstream readers).
    [[nodiscard]] int extent_cells() const noexcept {
        return m_grid.extent_cells();
    }
    [[nodiscard]] float cell_size_m() const noexcept {
        return m_grid.cell_size_m();
    }
    [[nodiscard]] std::uint64_t last_tick() const noexcept {
        return m_last_tick;
    }

private:
    // Resolve world_pos.y to a layer band.
    [[nodiscard]] static WindLayer LayerForHeight(float world_y) noexcept;
    // World-pos -> local cell indices for the current origin (clamped result is
    // reported via in_region).
    void LocalCell(const Vec3& world_pos, int& out_lx, int& out_lz, bool& in_region) const;

    int m_wind_seed = 0; // world seed + 11
    luminumbra::fields::FieldGrid<WindCell> m_grid;
    Vec2 m_base_direction = Vec2(1.0f, 0.0f);
    std::uint64_t m_last_tick = 0;

    FastNoise::SmartNode<FastNoise::Generator> m_direction_noise; // seed+11, low freq

    // Per-tick scratch, hoisted so Update does no per-tick heap allocation
    // (the field is updated every 30 Hz tick; reusing these keeps the budget
    // tight). Sized to the grid cell count at construction.
    std::vector<float> m_scratch_px;
    std::vector<float> m_scratch_pz;
    std::vector<float> m_scratch_cell_noise;
    std::vector<StormPerturbation> m_storm_perturbations;
};

} // namespace Luminumbra::Systems
