#pragma once

#include "ScentField.h"

#include <array>
#include <cstddef>
#include <utility>

namespace luminumbra::ai::detail {

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
    std::array<bool, 4> due{};
    std::array<std::array<bool, 4>, 4> open{};
    const std::array<std::pair<int, int>, 4> corners{
        std::pair{0, 0}, {cells - 1, 0}, {0, cells - 1}, {cells - 1, cells - 1}};
    bool all_due = true, all_open = true;
    for (const auto& [x, z] : corners) {
        due[tile(x, z)] = cell_due(x, z);
        all_due = all_due && due[tile(x, z)];
        for (const auto& [nx, nz] : corners) {
            open[tile(x, z)][tile(nx, nz)] = edge_open(x, z, nx, nz);
            all_open = all_open && open[tile(x, z)][tile(nx, nz)];
        }
    }
    ScentStepGates gates;
    if (!all_due)
        gates.participants = [tile, due](int x, int z) {
            return due[tile(x, z)];
        };
    // An open domain uses the original ungated arithmetic, with the same
    // bilinear interpolation and without per-sample sealed-path walks.
    if (!all_open)
        gates.exchange = [tile, open](int x, int z, int nx, int nz) {
            return open[tile(x, z)][tile(nx, nz)];
        };
    return gates;
}

} // namespace luminumbra::ai::detail
