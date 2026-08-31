#pragma once

// sim.irrigation: DETERMINISTIC soil-MOISTURE / water-diffusion grid that
// feeds plant moisture. This is the WATER half of the foliage resource loop (the
// nutrient half is systems/SoilNutrientSystem.h): a dug channel / spring deposits
// moisture into its cell, the moisture SPREADS to neighbours (so a single channel
// waters a band of soil, not just one tile), and fallow soil slowly DRAINS toward
// a dry baseline (so the player must keep water flowing). PlantGrowthSystem reads
// MoistureAt(x,z) when evaluating suitability.
//
// FIELD MODEL (mirrors systems/SoilNutrientSystem.h SoilGrid shape + ai/ScentField.h
// diffusion style): a fixed-size 2D grid anchored at a world (origin_x, origin_z)
// with a uniform cell_size. World (x,z) -> cell via floor((x-origin)/cell). Each
// cell holds a moisture level in fixed-point milli-units. Per tick:
//   1) DEPOSIT  — each WaterSourceComponent adds its output to its cell (summed
//                 per cell BEFORE apply, so the result is plant/source id-order
//                 independent — two springs on one cell add the same regardless).
//   2) DIFFUSE  — one conservative integer 4-neighbour pass (like ScalarFieldDiffusion's
//                 explicit step, but kept in integers): each cell gives a fixed
//                 fraction of its excess-over-baseline to each in-bounds neighbour.
//                 Done out-of-place (read snapshot, write fresh) so it is
//                 order-independent, and integer-exact (no float accumulates).
//   3) DRAIN    — every cell relaxes toward the dry baseline by a fixed integer
//                 step (evaporation / percolation), clamped so it never crosses
//                 the baseline from either side.
//
// DETERMINISM (this feeds world_hash once wired): the moisture store is INTEGER
// fixed-point milli-units — NO float accumulates in sim truth. Sources are visited
// in ascending entity-id order, but their deposit is ACCUMULATED per cell first and
// applied as one integer add, so the outcome is independent of traversal order.
// Diffusion is an out-of-place integer stencil (snapshot in, fresh out). NO rng
// (offset +21 reserved but UNUSED — a diffusion grid has no random draw), NO
// wall-clock, NO libm transcendentals.
//
// GATING: only entities carrying WaterSourceComponent deposit. A registry with no
// such source => RunIrrigationOnTick performs ZERO deposit; diffusion of an
// at-baseline grid is a fixed point (every cell == baseline => no excess to share)
// and the drain leaves an at-baseline cell unchanged, so a grid that started at
// baseline STAYS at baseline (a true no-op) and the canonical NetworkStateHash
// baseline is unperturbed until water sources are spawned.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CoreComponents.h"        // TransformComponent
#include "../components/IrrigationComponents.h"  // WaterSourceComponent

namespace luminumbra::foliage {

// Components live in the capital-L namespace (engine convention).
namespace Comp = ::Luminumbra::Components;

// Reserved seed-stream offset for the irrigation subsystem (registry: wind+11,
// weather+12/13, aether+14, plant+15, creature-reproduction+16, fire+17, soil+18,
// pollination+19, disease+20). The irrigation flow is a PURE diffusion grid (no
// rng), so this is recorded for collision-avoidance but intentionally never used.
inline constexpr std::uint64_t kIrrigationSeedOffset = 21ull;

// Moisture is stored as fixed-point milli-units (1.0 moisture == 1000 units).
// Baseline = "dry, undisturbed" soil; everything is clamped to [baseline, max].
inline constexpr std::int32_t kMoistureUnitScale = 1000;                  // milli-units per 1.0
inline constexpr std::int32_t kMoistureBaseline  = 0;                     // dry baseline (0.0)
inline constexpr std::int32_t kMoistureMax       = 4000 * kMoistureUnitScale; // saturation cap (4.0)
inline constexpr std::int32_t kMoistureDrainPerTick = 6;                  // integer milli-units/tick toward baseline

// Diffusion spread fraction is expressed as a 1/N share of each cell's excess
// given to EACH in-bounds neighbour. A cell sheds up to (neighbours/N) of its
// excess-over-baseline per tick; with N=8 and 4 neighbours that is at most half,
// which keeps the explicit integer stencil stable (no oscillation / overshoot).
inline constexpr std::int32_t kMoistureDiffuseShareDen = 8;

// A simple fixed-size 2D grid of moisture levels (integer milli-units), anchored
// at a world origin with a uniform cell size. Mirrors the SoilGrid/ScentField
// shape: construct with dimensions, query/sample by world coords or cell index.
class IrrigationGrid {
public:
    IrrigationGrid(int width, int height, std::int32_t initial = kMoistureBaseline)
        : m_w(width > 0 ? width : 0),
          m_h(height > 0 ? height : 0),
          m_cells(static_cast<std::size_t>(m_w) * static_cast<std::size_t>(m_h),
                  clampLevel(initial)) {}

    [[nodiscard]] int width() const { return m_w; }
    [[nodiscard]] int height() const { return m_h; }

    [[nodiscard]] bool in_bounds(int cx, int cz) const {
        return cx >= 0 && cz >= 0 && cx < m_w && cz < m_h;
    }

    // Moisture (milli-units) at a CELL index (baseline outside the grid).
    [[nodiscard]] std::int32_t AtCell(int cx, int cz) const {
        if (!in_bounds(cx, cz)) return kMoistureBaseline;
        return m_cells[index(cx, cz)];
    }

    // Set a cell directly (clamped to [baseline, max]); used by tests / seeding.
    void SetCell(int cx, int cz, std::int32_t value) {
        if (!in_bounds(cx, cz)) return;
        m_cells[index(cx, cz)] = clampLevel(value);
    }

    // Reset every cell to a value (a freshly-dry field by default).
    void Reset(std::int32_t value = kMoistureBaseline) {
        const std::int32_t v = clampLevel(value);
        std::fill(m_cells.begin(), m_cells.end(), v);
    }

    [[nodiscard]] bool all_at(std::int32_t value) const {
        for (std::int32_t c : m_cells) if (c != value) return false;
        return true;
    }

private:
    friend void ApplyDepositDiffuseDrain(IrrigationGrid&, const std::vector<std::int32_t>&);
    friend std::int32_t SampleMoistureCell(const IrrigationGrid&, int, int);

    [[nodiscard]] std::size_t index(int cx, int cz) const {
        return static_cast<std::size_t>(cz) * static_cast<std::size_t>(m_w) +
               static_cast<std::size_t>(cx);
    }
    static std::int32_t clampLevel(std::int32_t v) {
        if (v < kMoistureBaseline) return kMoistureBaseline;
        if (v > kMoistureMax) return kMoistureMax;
        return v;
    }

    int m_w;
    int m_h;
    std::vector<std::int32_t> m_cells;
};

// World coords -> cell index using a floor division anchored at the origin. Pure
// integer/float-compare (no transcendentals); the cast-to-int truncates toward
// zero, corrected for negatives so cells tile uniformly across the origin.
// (Local copy so this header is self-contained — same math as SoilNutrientSystem.)
inline void IrrigationWorldToCell(float x, float z, float origin_x, float origin_z,
                                  float cell_size, int& cx, int& cz) {
    const float fx = (x - origin_x) / cell_size;
    const float fz = (z - origin_z) / cell_size;
    int ix = static_cast<int>(fx);
    int iz = static_cast<int>(fz);
    if (fx < 0.0f && static_cast<float>(ix) != fx) --ix; // floor for negatives
    if (fz < 0.0f && static_cast<float>(iz) != fz) --iz;
    cx = ix;
    cz = iz;
}

// Friend-accessor: moisture at a CELL (used by the free MoistureAt world query).
inline std::int32_t SampleMoistureCell(const IrrigationGrid& g, int cx, int cz) {
    return g.AtCell(cx, cz);
}

// Telemetry / sub-hash for the irrigation tick.
struct IrrigationStats {
    int sources = 0;              // WaterSourceComponent entities considered this tick
    std::int64_t deposited = 0;   // total moisture deposited this tick (milli-units)
    std::int64_t drained = 0;     // total moisture drained this tick (milli-units)
};

// Internal: apply the accumulated per-cell deposit, then ONE conservative integer
// diffusion pass (out-of-place), then drain every cell toward the dry baseline.
// Three integer passes over the grid — deterministic and order-independent (the
// deposit was summed before this point; diffusion reads a snapshot).
inline void ApplyDepositDiffuseDrain(IrrigationGrid& g, const std::vector<std::int32_t>& deposit) {
    const int W = g.m_w;
    const int H = g.m_h;
    const std::size_t N = g.m_cells.size();

    // --- 1) deposit (already summed across sources on each cell) ---
    for (std::size_t i = 0; i < N; ++i) {
        std::int32_t v = g.m_cells[i];
        const std::int32_t d = (i < deposit.size()) ? deposit[i] : 0;
        v += d;
        if (v > kMoistureMax) v = kMoistureMax;
        if (v < kMoistureBaseline) v = kMoistureBaseline;
        g.m_cells[i] = v;
    }

    // --- 2) diffuse: conservative integer 4-neighbour stencil, out-of-place ---
    // Each cell sheds floor(excess / den) to EACH in-bounds neighbour, where
    // excess = level - baseline (only moisture above the dry baseline migrates).
    // Outflow is removed from the source and added to the neighbour, so total
    // moisture-above-baseline is conserved exactly (integer flux is symmetric per
    // edge: the donor subtracts the same integer the receiver adds). Reading the
    // pre-diffusion snapshot makes the result independent of cell visit order.
    if (W > 0 && H > 0) {
        std::vector<std::int32_t> snapshot = g.m_cells;
        // Start each cell at its current level; apply only the deltas.
        for (int z = 0; z < H; ++z) {
            for (int x = 0; x < W; ++x) {
                const std::size_t ci = static_cast<std::size_t>(z) *
                                       static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
                const std::int32_t excess = snapshot[ci] - kMoistureBaseline;
                if (excess <= 0) continue;
                const std::int32_t perNeighbour = excess / kMoistureDiffuseShareDen;
                if (perNeighbour <= 0) continue;
                // For each in-bounds neighbour, move `perNeighbour` from ci -> nbr.
                const int nx[4] = {x - 1, x + 1, x, x};
                const int nz[4] = {z, z, z - 1, z + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || nx[k] >= W || nz[k] < 0 || nz[k] >= H) continue;
                    const std::size_t ni = static_cast<std::size_t>(nz[k]) *
                                           static_cast<std::size_t>(W) +
                                           static_cast<std::size_t>(nx[k]);
                    g.m_cells[ci] -= perNeighbour;
                    g.m_cells[ni] += perNeighbour;
                }
            }
        }
        // Clamp after the conservative exchange (cannot exceed max via inflow).
        for (std::size_t i = 0; i < N; ++i) {
            if (g.m_cells[i] > kMoistureMax) g.m_cells[i] = kMoistureMax;
            if (g.m_cells[i] < kMoistureBaseline) g.m_cells[i] = kMoistureBaseline;
        }
    }

    // --- 3) drain toward the dry baseline (evaporation / percolation) ---
    for (std::size_t i = 0; i < N; ++i) {
        std::int32_t v = g.m_cells[i];
        if (v > kMoistureBaseline) {
            v -= kMoistureDrainPerTick;
            if (v < kMoistureBaseline) v = kMoistureBaseline; // never undershoot baseline
        }
        g.m_cells[i] = v;
    }
}

// Advance the soil-moisture field by ONE fixed tick. Pure function of registry
// state + the grid. Sources (WaterSourceComponent + Transform) are visited in
// id order, but their deposit is ACCUMULATED per cell (order-independent) then
// applied with the diffuse + drain passes.
inline IrrigationStats RunIrrigationOnTick(entt::registry& reg, IrrigationGrid& grid,
                                           float origin_x, float origin_z, float cell_size) {
    IrrigationStats stats;
    if (cell_size <= 0.0f || grid.width() <= 0 || grid.height() <= 0) {
        return stats; // degenerate grid: nothing to do
    }

    const int W = grid.width();
    const int H = grid.height();

    // Per-cell deposit (milli-units), summed over all sources on a cell so the
    // outcome is independent of source id order. Sized to the grid.
    std::vector<std::int32_t> deposit(
        static_cast<std::size_t>(W) * static_cast<std::size_t>(H), 0);

    auto view = reg.view<Comp::WaterSourceComponent, const Comp::TransformComponent>();

    // id-ordered traversal (deterministic; the per-cell sum is order-independent,
    // but we sort to keep telemetry + any future per-source bookkeeping stable).
    std::vector<entt::entity> ents;
    for (auto e : view) ents.push_back(e);
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    for (auto e : ents) {
        ++stats.sources;
        const auto& src = view.get<Comp::WaterSourceComponent>(e);
        const auto& tf  = view.get<const Comp::TransformComponent>(e);

        int cx = 0, cz = 0;
        IrrigationWorldToCell(tf.position.x, tf.position.z, origin_x, origin_z, cell_size, cx, cz);
        if (cx < 0 || cz < 0 || cx >= W || cz >= H) continue; // outside the grid -> no deposit

        const std::int32_t add = static_cast<std::int32_t>(src.output);
        if (add <= 0) continue;
        const std::size_t ci = static_cast<std::size_t>(cz) * static_cast<std::size_t>(W) +
                               static_cast<std::size_t>(cx);
        deposit[ci] += add;
        stats.deposited += add;
    }

    // Snapshot total moisture-above-baseline before draining so we can report the
    // drained amount as the post-drain delta (cheap + exact).
    std::int64_t before = 0;
    for (int z = 0; z < H; ++z)
        for (int x = 0; x < W; ++x) before += grid.AtCell(x, z) - kMoistureBaseline;

    ApplyDepositDiffuseDrain(grid, deposit);

    std::int64_t after = 0;
    for (int z = 0; z < H; ++z)
        for (int x = 0; x < W; ++x) after += grid.AtCell(x, z) - kMoistureBaseline;

    // drained = deposited (added this tick) + before - after; non-negative by
    // construction (diffusion conserves; drain only removes).
    const std::int64_t removed = before + stats.deposited - after;
    stats.drained = removed > 0 ? removed : 0;
    return stats;
}

// READ query for growth/availability: moisture (milli-units) at a WORLD (x,z).
// Returns the dry baseline outside the grid. Stable — a pure function of grid
// state + coords.
inline std::int32_t MoistureAt(const IrrigationGrid& grid, float x, float z,
                               float origin_x, float origin_z, float cell_size) {
    if (cell_size <= 0.0f) return kMoistureBaseline;
    int cx = 0, cz = 0;
    IrrigationWorldToCell(x, z, origin_x, origin_z, cell_size, cx, cz);
    return SampleMoistureCell(grid, cx, cz);
}

} // namespace luminumbra::foliage
