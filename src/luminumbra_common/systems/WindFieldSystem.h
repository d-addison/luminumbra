#pragma once

// T-I5a-2 (A2): deterministic coarse 2.5D wind grid (sim-authoritative).
//
// SHAPE (PINNED, design-decisions.md S4): 24 m cells, 3 layers
// (ground 0-32 m / mid 32-128 m / high 128-512 m AGL), over the streamed extent,
// on the 30 Hz tick. Each cell holds a 2D HORIZONTAL wind vector per layer
// (vertical wind ignored in 5a). The grid follows the streamed region; samples
// outside the region clamp to the base large-scale direction.
//
// UPDATE: the base large-scale direction is a `seed+11` low-frequency noise
// sampled by tick-time via the FastNoise batch path (sample-path == batch-path
// parity, like the worldgen noises); a cheap per-cell spatial blend adds local
// variation. A storm-perturbation injection hook is left as a no-op (consumed by
// the B1 weather task). All math goes through DeterministicMath (NO libm
// sin/cos/exp on the tick path) so the field is bit-deterministic across runs
// and machines and passes SimDeterminismLint.
//
// DETERMINISM SURFACE: the wind cell values feed the `wind` world_hash sub-hash
// (design-decisions.md S2). The field is a pure function of (seed, tick, region
// origin); a save/load/resim reaches the same field at the same tick because all
// three inputs reproduce.

#include <cstdint>
#include <string>
#include <vector>

#include "FastNoise/FastNoise.h"

#include "../fields/FieldGrid.h"
#include "../../../include/luminumbra/core/Types.h"

namespace Luminumbra::Systems {

// Wind layers (AGL bands). Vertical wind is ignored in 5a; each layer carries a
// 2D horizontal vector. The enum order is the canonical per-cell hash order.
enum class WindLayer : int {
    Ground = 0, // 0-32 m AGL
    Mid    = 1, // 32-128 m AGL
    High   = 2, // 128-512 m AGL
};
inline constexpr int kWindLayerCount = 3;

// PINNED geometry (design-decisions.md S4).
inline constexpr float kWindCellSizeM = 24.0f;
// Streamed extent the grid covers, in cells. A 24 m cell * 64 = 1536 m square
// region centred on the spawn/stream anchor -- comfortably covers the radius-4
// gate streaming footprint and the production view distance band the downstream
// readers (B1 advection, B2 rain slant, 5b foliage) sample.
inline constexpr int kWindExtentCells = 64;

// Layer AGL bands (metres) -- the boundaries that map a sample height to a layer.
inline constexpr float kWindGroundTopM = 32.0f;
inline constexpr float kWindMidTopM    = 128.0f;
inline constexpr float kWindHighTopM   = 512.0f;

// One grid cell: a horizontal wind vector for each of the 3 layers.
struct WindCell {
    Vec2 layer[kWindLayerCount] = {Vec2(0.0f), Vec2(0.0f), Vec2(0.0f)};
};

class WindFieldSystem {
public:
    // seed is the WORLD seed; the wind base-direction noise uses seed + 11
    // (seed-offset registry, design-decisions.md S1 -- appended FIRST, append
    // only, never reusing +1..+10).
    explicit WindFieldSystem(int world_seed);

    // Advance the field to `tick` anchored on the streamed region around
    // `region_anchor` (world-space; usually the spawn/stream anchor). Pure
    // function of (seed, tick, anchored origin); no wall-clock, no RNG.
    // Budget: <= 0.15 ms/tick at the streamed extent (WindFieldDeterminism gate).
    void Update(std::uint64_t tick, const Vec3& region_anchor);

    // Public sampling API (stable; consumed by B1 advection, B2 rain slant, and
    // 5b foliage displacement). Returns the horizontal wind vector at the world
    // position for the layer covering world_pos.y (AGL approximated as the
    // absolute height band -- 5a has no per-column ground height dependence).
    [[nodiscard]] Vec2 SampleWind(const Vec3& world_pos) const;
    // Explicit-layer variant: ignores world_pos.y, samples the named layer.
    [[nodiscard]] Vec2 SampleWind(const Vec3& world_pos, WindLayer layer) const;

    // The current base large-scale direction (unit-ish vector) -- what
    // out-of-region samples clamp to. Exposed for the clouds (C3) scroll vector
    // and for gate diagnostics.
    [[nodiscard]] Vec2 BaseDirection() const noexcept { return m_base_direction; }

    // Storm-perturbation injection hook (B1 weather). 5a leaves this a NO-OP
    // injection point: B1 will call this per storm cell before/within Update to
    // add a localized swirl/gust. Kept on the public surface so the B1 prompt
    // wires to a stable signature without touching this header's update math.
    struct StormPerturbation {
        Vec2 center_world = Vec2(0.0f);
        float radius_m = 0.0f;
        Vec2 velocity = Vec2(0.0f);
        float intensity = 0.0f;
    };
    void InjectStormPerturbation(const StormPerturbation& perturbation);
    void ClearStormPerturbations();

    // Deterministic sub-hash over the wind cell values, in the FieldGrid
    // canonical order. Folded into the world_hash `wind` slot (mega-bump).
    [[nodiscard]] std::string ComputeWindSubHash() const;

    // Geometry accessors (gate diagnostics / downstream readers).
    [[nodiscard]] int extent_cells() const noexcept { return m_grid.extent_cells(); }
    [[nodiscard]] float cell_size_m() const noexcept { return m_grid.cell_size_m(); }
    [[nodiscard]] std::uint64_t last_tick() const noexcept { return m_last_tick; }

private:
    // Resolve world_pos.y to a layer band.
    [[nodiscard]] static WindLayer LayerForHeight(float world_y) noexcept;
    // World-pos -> local cell indices for the current origin (clamped result is
    // reported via in_region).
    void LocalCell(const Vec3& world_pos, int& out_lx, int& out_lz, bool& in_region) const;

    int m_world_seed = 0;
    int m_wind_seed = 0; // m_world_seed + 11
    luminumbra::fields::FieldGrid<WindCell> m_grid;
    Vec2 m_base_direction = Vec2(1.0f, 0.0f);
    std::uint64_t m_last_tick = 0;

    FastNoise::SmartNode<FastNoise::Generator> m_direction_noise; // seed+11, low freq

    // Per-tick scratch, hoisted so Update() does no per-tick heap allocation
    // (the field is updated every 30 Hz tick; reusing these keeps the budget
    // tight). Sized to the grid cell count at construction.
    std::vector<float> m_scratch_px;
    std::vector<float> m_scratch_pz;
    std::vector<float> m_scratch_cell_noise;
};

} // namespace Luminumbra::Systems
