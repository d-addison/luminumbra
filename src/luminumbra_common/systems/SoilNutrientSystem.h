#pragma once

// sim.soil: DETERMINISTIC soil NUTRIENT field, consumed by plants and
// slowly REGENERATING toward a baseline each tick. This is the substrate that
// makes foliage a true resource loop: a dense stand of mature plants
// draws its cell down (so unmanaged monoculture starves itself), and fallow soil
// recovers over time (so crop rotation / spacing pays off). Growth can later
// READ NutrientAt(x,z) to fold availability into PlantGrowthSystem's suitability.
//
// FIELD MODEL (mirrors ai/ScentField.h / fields ScalarFieldDiffusion style): a
// fixed-size 2D grid anchored at a world (origin_x, origin_z) with a uniform
// cell_size. World (x,z) -> cell via floor((x-origin)/cell). Each cell holds a
// nutrient level. Unlike scent we do NOT diffuse (soil nutrient is local, and
// diffusion would couple cells and complicate the order-independence proof); the
// dynamics are pure per-cell: CONSUME (plants) then REGENERATE (toward baseline).
//
// DETERMINISM (this feeds world_hash once wired): the nutrient store is INTEGER
// fixed-point milli-units — no float accumulates in sim truth. Plants are visited
// in ascending entity-id order, but consumption is ACCUMULATED per cell first and
// applied as one integer subtraction, so the result is independent of traversal
// order (two plants on one cell deplete it the same regardless of id order).
// Regeneration is an integer step toward a baseline. NO rng (offset +18 reserved
// but unused — the flow is pure), NO wall-clock, NO libm transcendentals.
//
// GATING: only plants carrying BOTH the foliage PlantTag AND the new
// SoilFeederComponent (components/SoilComponents.h) consume. A registry with no
// such plant => RunSoilNutrientOnTick performs ZERO consumption; and if the soil
// grid started at baseline it STAYS at baseline (a true no-op), so the canonical
// NetworkStateHash baseline is unperturbed until soil-aware plants are spawned.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CoreComponents.h"   // TransformComponent
#include "../components/PlantComponents.h"  // PlantTag, PlantGrowthComponent, PlantStage
#include "../components/SoilComponents.h"   // SoilFeederComponent

namespace luminumbra::foliage {

// Components live in the capital-L namespace (engine convention).
namespace Comp = ::Luminumbra::Components;

// Reserved seed-stream offset for the soil subsystem (registry: wind+11, weather+12/13,
// aether+14, plant+15, creature-reproduction+16). The soil flow is PURE (no rng),
// so this is recorded for collision-avoidance but intentionally never consumed.
inline constexpr std::uint64_t kSoilSeedOffset = 18ull;

// Nutrient is stored as fixed-point milli-units (1.0 nutrient == 1000 units).
// Baseline = "rich, undisturbed" soil; everything is clamped to [0, baseline].
inline constexpr std::int32_t kSoilUnitScale   = 1000;            // milli-units per 1.0
inline constexpr std::int32_t kSoilBaseline    = 1000 * kSoilUnitScale; // 1.0 nutrient
inline constexpr std::int32_t kSoilRegenPerTick = 4;              // integer milli-units/tick

// Per-stage consumption WEIGHT (out of 16): a seed barely feeds, a fruiting plant
// feeds hard. Indexed by PlantStage (Seed..Fruiting). Integer so the draw stays
// fixed-point. The actual draw also scales by the feeder's `uptake` strength.
inline std::int32_t SoilStageWeight(std::uint8_t stage) {
    static const std::int32_t kW[Comp::kPlantStageCount] = {
        1,  // Seed
        2,  // Sprout
        4,  // Juvenile
        8,  // Mature
        10, // Flowering
        12  // Fruiting
    };
    return stage < Comp::kPlantStageCount ? kW[stage] : 12;
}

// A simple fixed-size 2D grid of nutrient levels (integer milli-units), anchored
// at a world origin with a uniform cell size. Mirrors the ScentField/ScalarField
// shape: construct with dimensions, query/sample by world coords or cell index.
class SoilGrid {
public:
    SoilGrid(int width, int height, std::int32_t initial = kSoilBaseline)
        : m_w(width > 0 ? width : 0),
          m_h(height > 0 ? height : 0),
          m_cells(static_cast<std::size_t>(m_w) * static_cast<std::size_t>(m_h),
                  initial < 0 ? 0 : initial) {}

    [[nodiscard]] int width() const { return m_w; }
    [[nodiscard]] int height() const { return m_h; }

    [[nodiscard]] bool in_bounds(int cx, int cz) const {
        return cx >= 0 && cz >= 0 && cx < m_w && cz < m_h;
    }

    // Nutrient (milli-units) at a CELL index (0 outside the grid).
    [[nodiscard]] std::int32_t AtCell(int cx, int cz) const {
        if (!in_bounds(cx, cz)) return 0;
        return m_cells[index(cx, cz)];
    }

    // Set a cell directly (clamped to [0, baseline]); used by tests / seeding.
    void SetCell(int cx, int cz, std::int32_t value) {
        if (!in_bounds(cx, cz)) return;
        m_cells[index(cx, cz)] = clampLevel(value);
    }

    // Reset every cell to the baseline (a freshly-tilled field).
    void Reset(std::int32_t value = kSoilBaseline) {
        const std::int32_t v = clampLevel(value);
        std::fill(m_cells.begin(), m_cells.end(), v);
    }

    [[nodiscard]] bool all_at(std::int32_t value) const {
        for (std::int32_t c : m_cells) if (c != value) return false;
        return true;
    }

private:
    friend void ApplyConsumptionAndRegen(SoilGrid&, const std::vector<std::int32_t>&);
    friend std::int32_t SampleSoilCell(const SoilGrid&, int, int);

    [[nodiscard]] std::size_t index(int cx, int cz) const {
        return static_cast<std::size_t>(cz) * static_cast<std::size_t>(m_w) +
               static_cast<std::size_t>(cx);
    }
    static std::int32_t clampLevel(std::int32_t v) {
        if (v < 0) return 0;
        if (v > kSoilBaseline) return kSoilBaseline;
        return v;
    }

    int m_w;
    int m_h;
    std::vector<std::int32_t> m_cells;
};

// World coords -> cell index using a floor division anchored at the origin. Pure
// integer/float-compare (no transcendentals); the cast-to-int floors toward zero,
// corrected for negatives so cells tile uniformly across the origin.
inline void WorldToCell(float x, float z, float origin_x, float origin_z, float cell_size,
                        int& cx, int& cz) {
    const float fx = (x - origin_x) / cell_size;
    const float fz = (z - origin_z) / cell_size;
    int ix = static_cast<int>(fx);
    int iz = static_cast<int>(fz);
    if (fx < 0.0f && static_cast<float>(ix) != fx) --ix; // floor for negatives
    if (fz < 0.0f && static_cast<float>(iz) != fz) --iz;
    cx = ix;
    cz = iz;
}

// Friend-accessor: nutrient at a CELL (used by the free NutrientAt world query).
inline std::int32_t SampleSoilCell(const SoilGrid& g, int cx, int cz) {
    return g.AtCell(cx, cz);
}

// Telemetry / sub-hash for the soil tick.
struct SoilNutrientStats {
    int participants = 0;       // PlantTag + SoilFeeder plants considered this tick
    std::int64_t consumed = 0;  // total nutrient consumed this tick (milli-units)
    std::int64_t regenerated = 0; // total nutrient regenerated this tick (milli-units)
};

// Internal: apply the accumulated per-cell consumption, then regenerate every cell
// toward the baseline. Two integer passes over the grid — deterministic and
// order-independent (consumption was summed before this point).
inline void ApplyConsumptionAndRegen(SoilGrid& g, const std::vector<std::int32_t>& demand) {
    for (std::size_t i = 0; i < g.m_cells.size(); ++i) {
        std::int32_t v = g.m_cells[i];
        const std::int32_t d = (i < demand.size()) ? demand[i] : 0;
        v -= d;                       // consume (already summed across plants on the cell)
        if (v < 0) v = 0;
        v += kSoilRegenPerTick;       // regenerate toward baseline
        if (v > kSoilBaseline) v = kSoilBaseline;
        g.m_cells[i] = v;
    }
}

// Advance the soil nutrient field by ONE fixed tick. Pure function of registry
// state + the grid. id-ordered traversal; consumption is ACCUMULATED per cell
// (order-independent) then applied with the regen step. Plants that opt in
// (PlantTag + SoilFeederComponent) draw from their cell proportional to growth
// stage * feeder uptake; their `absorbed` integer accrues what they actually got.
inline SoilNutrientStats RunSoilNutrientOnTick(entt::registry& reg, SoilGrid& soil,
                                              float origin_x, float origin_z,
                                              float cell_size) {
    SoilNutrientStats stats;
    if (cell_size <= 0.0f || soil.width() <= 0 || soil.height() <= 0) {
        return stats; // degenerate grid: nothing to do
    }

    // Per-cell consumption demand (milli-units), summed over all plants on a cell so
    // the outcome is independent of plant id order. Sized to the grid.
    std::vector<std::int32_t> demand(
        static_cast<std::size_t>(soil.width()) * static_cast<std::size_t>(soil.height()), 0);

    auto view = reg.view<Comp::PlantTag, Comp::SoilFeederComponent,
                         const Comp::TransformComponent, Comp::PlantGrowthComponent>();

    // id-ordered so `absorbed` accrual is deterministic per plant.
    std::vector<entt::entity> ents;
    for (auto e : view) ents.push_back(e);
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    const int W = soil.width();
    const int H = soil.height();

    for (auto e : ents) {
        ++stats.participants;
        const auto& tf     = view.get<const Comp::TransformComponent>(e);
        auto& feeder       = view.get<Comp::SoilFeederComponent>(e);
        const auto& growth = view.get<Comp::PlantGrowthComponent>(e);

        int cx = 0, cz = 0;
        WorldToCell(tf.position.x, tf.position.z, origin_x, origin_z, cell_size, cx, cz);
        if (cx < 0 || cz < 0 || cx >= W || cz >= H) continue; // outside the grid -> no draw

        // Per-plant draw (milli-units): stage weight (0..16) * uptake (milli) / scale.
        // Integer throughout (uptake milli * weight / 16 keeps the weight a 1/16 factor).
        const std::int32_t weight = SoilStageWeight(growth.stage);
        const std::int64_t draw64 =
            (static_cast<std::int64_t>(feeder.uptake) * static_cast<std::int64_t>(weight)) / 16;
        std::int32_t draw = static_cast<std::int32_t>(draw64);
        if (draw < 0) draw = 0;

        // Track the plant's intent; the cell may not have this much (clamped on apply),
        // but `absorbed` records demand-met after the cell is known. To keep absorbed
        // exact + order-independent we credit min(draw, available-before-regen) using
        // the cell's CURRENT level minus what's already demanded this tick.
        const std::size_t ci = static_cast<std::size_t>(cz) * static_cast<std::size_t>(W) +
                               static_cast<std::size_t>(cx);
        const std::int32_t avail = soil.AtCell(cx, cz) - demand[ci];
        const std::int32_t got = draw < avail ? draw : (avail > 0 ? avail : 0);
        demand[ci] += got;
        feeder.absorbed += static_cast<std::uint32_t>(got);
        stats.consumed += got;
    }

    // Regen total = baseline-bounded +regen per cell (report the nominal add).
    ApplyConsumptionAndRegen(soil, demand);
    stats.regenerated = static_cast<std::int64_t>(kSoilRegenPerTick) *
                        static_cast<std::int64_t>(W) * static_cast<std::int64_t>(H);
    return stats;
}

// READ query for growth/availability: nutrient (milli-units) at a WORLD (x,z).
// Returns 0 outside the grid. Stable — a pure function of grid state + coords.
inline std::int32_t NutrientAt(const SoilGrid& soil, float x, float z,
                              float origin_x, float origin_z, float cell_size) {
    if (cell_size <= 0.0f) return 0;
    int cx = 0, cz = 0;
    WorldToCell(x, z, origin_x, origin_z, cell_size, cx, cz);
    return SampleSoilCell(soil, cx, cz);
}

} // namespace luminumbra::foliage
