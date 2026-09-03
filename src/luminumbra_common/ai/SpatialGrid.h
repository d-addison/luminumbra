#pragma once

// perf-lane-and-ecology-tick (BOIDS): a deterministic UNIFORM SPATIAL GRID for the
// creature herd-flocking neighbour query. Replaces the per-creature O(N) snapshot scan
// (CreatureBrainSystem) with an O(N+k) bucket-once / radius-query path: build the grid
// ONCE per tick from the pre-tick snapshot, then each creature queries only the 3x3 cell
// neighbourhood around itself instead of scanning every other creature.
//
// DETERMINISM CONTRACT (sim-safe): this is a PURE perf accelerator — it changes WHICH
// neighbours are visited and in WHAT order, never the steering MATH. The downstream
// accumulation (Flocking.h) is order-invariant (fixed-point integer sums), so the radius
// query may return indices in ANY order without affecting the result. Cell math is pure
// integer floor-division (handles negative world coords), no RNG, no libm, no float compare
// for bucketing. Cell size == the flocking neighbour radius, so the cohesion radius (<=
// neighbor_radius) is fully covered by the queried 3x3 cell span.

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace luminumbra::ai {

// A point inserted into the grid: an opaque caller index plus its XZ position. The caller
// (CreatureBrainSystem) inserts only SAME-ROLE creatures into a per-role grid, so a radius
// query returns role-matched neighbours directly.
struct GridPoint {
    std::uint32_t index = 0; // caller-side index (e.g. into the snapshot vector)
    float x = 0.0f;
    float z = 0.0f;
};

// Uniform grid over the XZ plane. Cells are `cell_size` on a side; a point at (x,z) lives in
// cell (floor(x/cell_size), floor(z/cell_size)). A radius query for a point returns every
// inserted index in the 3x3 block of cells centred on that point's cell — a superset of the
// true neighbours within `cell_size`, which the caller then distance-filters (exactly as the
// old full scan did, just over far fewer candidates).
class UniformSpatialGrid {
public:
    explicit UniformSpatialGrid(float cell_size)
        : m_cell_size(cell_size > 0.0f ? cell_size : 1.0f)
        , m_inv_cell_size(1.0f / (cell_size > 0.0f ? cell_size : 1.0f)) {}

    // Build the grid from a span of points in a single pass. Reserving up front keeps the
    // per-tick build allocation-light.
    void Build(const std::vector<GridPoint>& points) {
        m_buckets.clear();
        m_buckets.reserve(points.size());
        for (const GridPoint& p : points) {
            const std::int64_t key = CellKey(CellCoord(p.x), CellCoord(p.z));
            m_buckets[key].push_back(p.index);
        }
    }

    // Append every index in the 3x3 cell block centred on (qx,qz) to `out` (NOT cleared —
    // the caller owns the buffer so it can be reused across creatures without reallocating).
    // Order is unspecified by design: the downstream accumulation is order-invariant.
    void QueryRadius(float qx, float qz, std::vector<std::uint32_t>& out) const {
        const std::int32_t cx = CellCoord(qx);
        const std::int32_t cz = CellCoord(qz);
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const auto it = m_buckets.find(CellKey(cx + dx, cz + dz));
                if (it == m_buckets.end())
                    continue;
                out.insert(out.end(), it->second.begin(), it->second.end());
            }
        }
    }

    [[nodiscard]] float cell_size() const {
        return m_cell_size;
    }

private:
    // floor(coord / cell_size) as an int32, correct for negative coords (the C++ cast to int
    // truncates toward zero, so subtract 1 when the truncation rounded the wrong way).
    [[nodiscard]] std::int32_t CellCoord(float coord) const {
        const float scaled = coord * m_inv_cell_size;
        std::int32_t c = static_cast<std::int32_t>(scaled);
        if (static_cast<float>(c) > scaled)
            --c; // floor for negatives
        return c;
    }

    // Pack two int32 cell coords into one int64 key (deterministic, collision-free).
    [[nodiscard]] static std::int64_t CellKey(std::int32_t cx, std::int32_t cz) {
        return (static_cast<std::int64_t>(cx) << 32) ^
               (static_cast<std::int64_t>(static_cast<std::uint32_t>(cz)));
    }

    float m_cell_size;
    float m_inv_cell_size;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> m_buckets;
};

} // namespace luminumbra::ai
