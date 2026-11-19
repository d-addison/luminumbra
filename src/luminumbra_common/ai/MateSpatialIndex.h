#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace luminumbra::ai {

// Tick-local acceleration only: indices retain the caller's participant order. Queries
// visit nine cells; distance filtering and index tie-breaking remain with the caller.
class MateSpatialIndex {
public:
    explicit MateSpatialIndex(float radius)
        : m_cell_width(2.0 * std::max(1.0, static_cast<double>(radius))) {}

    void Insert(std::size_t index, float x, float z) {
        // Non-finite positions cannot win the existing strict distance comparison.
        if (std::isfinite(x) && std::isfinite(z)) {
            m_cells[{Cell(x), Cell(z)}].push_back(index);
        }
    }

    template<typename Visitor>
    void Visit(float x, float z, const Visitor& visitor) const {
        if (!std::isfinite(x) || !std::isfinite(z)) {
            return;
        }
        const auto cx = Cell(x);
        const auto cz = Cell(z);
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dz = -1; dz <= 1; ++dz) {
                const auto found = m_cells.find({cx + dx, cz + dz});
                if (found != m_cells.end()) {
                    for (const auto index : found->second) {
                        visitor(index);
                    }
                }
            }
        }
    }

private:
    [[nodiscard]] std::int64_t Cell(float coordinate) const {
        // Clamp before converting; extreme finite coordinates coalesce conservatively.
        // Headroom makes all nine neighbour keys safe. Twice the radius also covers
        // rounded float distances at boundaries; the minimum width covers underflow.
        constexpr std::int64_t limit = std::int64_t{1} << 60;
        const double cell = std::floor(static_cast<double>(coordinate) / m_cell_width);
        return static_cast<std::int64_t>(
            std::clamp(cell, -static_cast<double>(limit), static_cast<double>(limit)));
    }

    struct CellKey {
        std::int64_t x, z;
        bool operator==(const CellKey&) const = default;
    };
    struct CellHash {
        [[nodiscard]] std::size_t operator()(const CellKey& key) const {
            // Explicit unsigned FNV-1a over the two integer coordinates. Only lookup
            // uses this hash; queries never iterate the unordered container.
            std::uint64_t hash = 14695981039346656037ull;
            for (const auto coordinate : {key.x, key.z}) {
                const auto bits = static_cast<std::uint64_t>(coordinate);
                for (unsigned shift = 0; shift < 64; shift += 8) {
                    hash ^= (bits >> shift) & 0xffu;
                    hash *= 1099511628211ull;
                }
            }
            return static_cast<std::size_t>(hash);
        }
    };

    double m_cell_width;
    // At most one entry per inserted participant; total stored indices <= participants.
    std::unordered_map<CellKey, std::vector<std::size_t>, CellHash> m_cells;
};

} // namespace luminumbra::ai
