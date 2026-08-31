#pragma once

// deterministic coarse 2.5D Aether scalar field (sim-authoritative).
//
// The engine knows only an "emissive scalar field"; game content (the attuned
// crystals and flora) is what assigns meaning to it. The field is a single non-negative
// scalar per cell (energy / glow intensity) that game emitters drive and that
// the renderer taps into the materials-LUT emissive path.
//
// SHAPE (PINNED): 24 m cells, 64 x 64 = 1536 m square region on the 30 Hz tick,
// reusing the wind-field geometry so render + sim sample the same grid identity
// (FieldGrid<float>). The grid follows the streamed region; samples outside it
// read 0 (no aether beyond the streamed footprint).
//
// MODEL (determinism-critical): the field is a PURE FUNCTION of
// (seed, tick, region origin) -- exactly like the wind field -- so a
// save/load/resim reproduces it at the same tick WITHOUT persisting the grid
// (the heavy world_hash oracle handles wind/weather/aether identically:
// recompute-and-exclude from the cross-phase compare). Each Update:
//   1. derive a deterministic EMISSION source from a seed+14 low-frequency noise
//      (sampled over the cell world coords, scrolled slowly by tick-time),
//   2. ADVECT that source by the wind field via a deterministic semi-Lagrangian
//      backtrace + bilinear sample (no-op when no wind field is supplied),
//   3. DIFFUSE with a PINNED number of Gauss-Seidel iterations (spatial smooth).
// All math is +,-,*,/ and DeterministicMath:: trig only -- NO libm
// transcendentals, NO Exp (none exists in DeterministicMath), NO RNG, NO
// wall-clock -- so the field is bit-deterministic across runs/machines and
// passes SimDeterminismLint. The PINNED iteration count must not change without
// a deliberate world_hash bump.
//
// DETERMINISM SURFACE: the cell values feed the `aether` world_hash sub-hash
// (a deliberate bump, its own commit). Seed offset +14 (append-only registry:
// +11 wind, +12 weather, +13 lightning, +14 aether).

#include <cstdint>
#include <string>
#include <vector>

#include "FastNoise/FastNoise.h"

#include "../../../include/luminumbra/core/Types.h"
#include "../fields/FieldGrid.h"

namespace Luminumbra::Systems {

class WindFieldSystem; // advection source (optional, forward-declared)

// PINNED geometry (mirrors the wind field so the grids share identity).
inline constexpr float kAetherCellSizeM = 24.0f;
inline constexpr int kAetherExtentCells = 64;

// PINNED solver constants. Changing ANY of these changes the field bits and is
// therefore a deliberate world_hash bump; these values are frozen.
inline constexpr int kAetherDiffuseIterations = 8;           // Gauss-Seidel sweeps/tick
inline constexpr float kAetherDiffuseRate = 0.25f;           // neighbour coupling k
inline constexpr float kAetherEmissionFrequency = 0.0045f;   // low-freq source noise
inline constexpr double kAetherEmissionScrollPerTick = 0.02; // world scroll/tick
inline constexpr float kAetherAdvectionMetersPerTick = 0.6f; // semi-Lagrangian step

class AetherFieldSystem {
public:
    // seed is the WORLD seed; the emission noise uses seed + 14 (append-only
    // seed-offset registry: +11 wind, +12 weather, +13 lightning, +14 aether).
    explicit AetherFieldSystem(int world_seed);

    // Advance the field to `tick` anchored on the streamed region around
    // `region_anchor`. Pure function of (seed, tick, anchored origin[, wind]);
    // no wall-clock, no RNG. `wind` (optional) supplies the advection velocity;
    // when null the advection step is skipped (still deterministic + evolving).
    void
    Update(std::uint64_t tick, const Vec3& region_anchor, const WindFieldSystem* wind = nullptr);

    // Public sampling API: the (non-negative) aether scalar at the world
    // position, or 0 outside the streamed region. Consumed by the render
    // emissive tap and (later) planner stimuli.
    [[nodiscard]] float SampleAether(const Vec3& world_pos) const;

    // Deterministic sub-hash over the cell values in the FieldGrid canonical
    // order. Folded into the world_hash `aether` slot (deliberate bump).
    [[nodiscard]] std::string ComputeAetherSubHash() const;

    // Geometry / diagnostics accessors.
    [[nodiscard]] int extent_cells() const noexcept {
        return m_grid.extent_cells();
    }
    [[nodiscard]] float cell_size_m() const noexcept {
        return m_grid.cell_size_m();
    }
    [[nodiscard]] std::uint64_t last_tick() const noexcept {
        return m_last_tick;
    }
    [[nodiscard]] int diffuse_iterations() const noexcept {
        return kAetherDiffuseIterations;
    }

    // read-only access to the grid for the render emissive tap (the
    // client uploads cells as a texture, mapped by origin + cell size). One-way
    // bridge: render is a pure CONSUMER, never writes back.
    [[nodiscard]] const luminumbra::fields::FieldGrid<float>& grid() const noexcept {
        return m_grid;
    }

private:
    // World-pos -> local cell indices for the current origin (clamped result
    // reported via in_region). Mirrors WindFieldSystem::LocalCell.
    void LocalCell(const Vec3& world_pos, int& out_lx, int& out_lz, bool& in_region) const;

    int m_aether_seed = 0; // world seed + 14
    luminumbra::fields::FieldGrid<float> m_grid;
    std::uint64_t m_last_tick = 0;

    FastNoise::SmartNode<FastNoise::Generator> m_emission_noise; // seed+14, low freq

    // Per-tick scratch, hoisted so Update does no per-tick heap allocation.
    std::vector<float> m_scratch_px;       // emission-noise sample X coords
    std::vector<float> m_scratch_pz;       // emission-noise sample Z coords
    std::vector<float> m_scratch_emission; // raw emission noise -> source
    std::vector<float> m_scratch_advected; // post-advection source
};

} // namespace Luminumbra::Systems
