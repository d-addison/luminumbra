#pragma once

#include "ScentField.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace luminumbra::ai::detail {

// Membership for the near window's at most four world owners. Only Refresh reads
// the global schedule; every deposit, steering and foraging query stays bounded.
template<typename Owner>
class NearScentDueCache {
public:
    template<typename OwnerOf>
    void Refresh(int cells, const OwnerOf& owner_of, std::span<const Owner> due) {
        m_count = 0;
        for (const auto& [x, z] : std::array<std::pair<int, int>, 4>{
                 std::pair{0, 0}, {cells - 1, 0}, {0, cells - 1}, {cells - 1, cells - 1}}) {
            const auto owner = owner_of(x, z);
            bool present = false;
            for (std::size_t i = 0; i < m_count; ++i) {
                present = present || m_owners[i] == owner;
            }
            if (!present) {
                m_owners[m_count] = owner;
                m_due[m_count++] = std::find(due.begin(), due.end(), owner) != due.end();
            }
        }
    }
    [[nodiscard]] bool Allows(const Owner& owner) const {
        for (std::size_t i = 0; i < m_count; ++i) {
            ++m_queryVisits;
            if (m_owners[i] == owner) {
                return m_due[i];
            }
        }
        return false;
    }
    // Observational only; never persisted or hashed.
    [[nodiscard]] std::size_t query_visits() const {
        return m_queryVisits;
    }

private:
    std::array<Owner, 4> m_owners{};
    std::array<bool, 4> m_due{};
    std::size_t m_count = 0;
    mutable std::size_t m_queryVisits = 0;
};

struct ScentStepGates {
    ScentField::CellGate participants;
    ScentField::EdgeGate exchange;
};

// The nonempty square is smaller than an owner on both axes, so it intersects at
// most four tiles. Ownership and schedule must stay sealed through Step/Clamp.
// Kept separate so tests exercise the same cache and ungated selection as GameSession.
template<typename OwnerOf, typename CellDue, typename EdgeOpen>
[[nodiscard]] ScentStepGates CacheNearScentGates(int cells,
                                                 const OwnerOf& owner_of,
                                                 const CellDue& cell_due,
                                                 const EdgeOpen& edge_open) {
    const auto first = owner_of(0, 0);
    int split_x = cells, split_z = cells;
    for (int cell = 1; cell < cells; ++cell) {
        if (split_x == cells && owner_of(cell, 0).x != first.x)
            split_x = cell;
        if (split_z == cells && owner_of(0, cell).z != first.z)
            split_z = cell;
    }
    const auto tile = [split_x, split_z](int x, int z) {
        return static_cast<std::size_t>((x >= split_x) + 2 * (z >= split_z));
    };
    struct TileDecisions {
        std::array<bool, 4> due{};
        std::array<std::array<bool, 4>, 4> open{};
        bool all_due = true, all_open = true;
    } decisions;
    const std::array<std::pair<int, int>, 4> corners{
        std::pair{0, 0}, {cells - 1, 0}, {0, cells - 1}, {cells - 1, cells - 1}};
    for (const auto& [x, z] : corners) {
        decisions.due[tile(x, z)] = cell_due(x, z);
        decisions.all_due = decisions.all_due && decisions.due[tile(x, z)];
        for (const auto& [nx, nz] : corners) {
            decisions.open[tile(x, z)][tile(nx, nz)] = edge_open(x, z, nx, nz);
            decisions.all_open = decisions.all_open && decisions.open[tile(x, z)][tile(nx, nz)];
        }
    }
    ScentStepGates gates;
    if (!decisions.all_due)
        gates.participants = [tile, cached_due = decisions.due](int x, int z) {
            return cached_due[tile(x, z)];
        };
    // An open domain uses the original ungated arithmetic, with the same
    // bilinear interpolation and without per-sample sealed-path walks.
    if (!decisions.all_open)
        gates.exchange = [tile, cached_open = decisions.open](int x, int z, int nx, int nz) {
            return cached_open[tile(x, z)][tile(nx, nz)];
        };
    return gates;
}

} // namespace luminumbra::ai::detail
