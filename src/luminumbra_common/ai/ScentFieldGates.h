#pragma once

#include "ScentField.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace luminumbra::ai::detail {

// Membership for the near window's at most four world owners. Only Refresh reads
// the global schedule; every deposit, steering and foraging query stays bounded.
template<typename Owner>
class NearScentDueCache {
public:
    template<typename OwnerOf, typename IsDue>
    void Refresh(int cells, const OwnerOf& owner_of, const IsDue& is_due) {
        m_count = 0;
        for (const auto& [x, z] : std::array<std::pair<int, int>, 4>{
                 std::pair{0, 0}, {cells - 1, 0}, {0, cells - 1}, {cells - 1, cells - 1}}) {
            ++m_queryVisits;
            const auto owner = owner_of(x, z);
            bool present = false;
            for (std::size_t i = 0; i < m_count; ++i) {
                ++m_queryVisits;
                present = present || m_owners[i] == owner;
            }
            if (!present) {
                m_owners[m_count] = owner;
                ++m_queryVisits;
                m_due[m_count++] = is_due(owner);
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

// Cached gates own only the four tile decisions. The callback constructors retain
// the owning compatibility form for callers that supply arbitrary live gates.
class ScentParticipants {
public:
    ScentParticipants() = default;
    template<typename Gate>
        requires(!std::is_same_v<std::remove_cvref_t<Gate>, ScentParticipants> &&
                 std::is_constructible_v<ScentField::CellGate, Gate &&>)
    ScentParticipants(Gate&& gate)
        : m_callback(std::forward<Gate>(gate)) {}
    ScentParticipants(int split_x, int split_z, const std::array<bool, 4>& due)
        : m_splitX(split_x)
        , m_splitZ(split_z)
        , m_due(due)
        , m_cached(true) {}
    explicit operator bool() const {
        return m_cached || static_cast<bool>(m_callback);
    }
    bool operator()(int x, int z) const {
        if (m_cached) {
            return m_due[static_cast<std::size_t>((x >= m_splitX) + 2 * (z >= m_splitZ))];
        }
        return m_callback(x, z);
    }

private:
    // Copy the owning callback first so a failed copy leaves cached decisions unchanged.
    ScentField::CellGate m_callback;
    int m_splitX = 0, m_splitZ = 0;
    std::array<bool, 4> m_due{};
    bool m_cached = false;
};

class ScentExchange {
public:
    ScentExchange() = default;
    template<typename Gate>
        requires(!std::is_same_v<std::remove_cvref_t<Gate>, ScentExchange> &&
                 std::is_constructible_v<ScentField::EdgeGate, Gate &&>)
    ScentExchange(Gate&& gate)
        : m_callback(std::forward<Gate>(gate)) {}
    ScentExchange(int split_x, int split_z, std::uint32_t open)
        : m_splitX(split_x)
        , m_splitZ(split_z)
        , m_open(open)
        , m_cached(true) {}
    explicit operator bool() const {
        return m_cached || static_cast<bool>(m_callback);
    }
    bool operator()(int x, int z, int nx, int nz) const {
        if (m_cached) {
            const auto tile = [this](int cx, int cz) {
                return static_cast<unsigned>((cx >= m_splitX) + 2 * (cz >= m_splitZ));
            };
            return (m_open & (std::uint32_t{1} << (4 * tile(x, z) + tile(nx, nz)))) != 0;
        }
        return m_callback(x, z, nx, nz);
    }

private:
    // Copy the owning callback first so a failed copy leaves cached decisions unchanged.
    ScentField::EdgeGate m_callback;
    int m_splitX = 0, m_splitZ = 0;
    std::uint32_t m_open = 0;
    bool m_cached = false;
};

struct ScentStepGates {
    ScentParticipants participants;
    ScentExchange exchange;
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
        std::uint32_t open = 0;
        bool all_due = true, all_open = true;
    } decisions;
    const std::array<std::pair<int, int>, 4> corners{
        std::pair{0, 0}, {cells - 1, 0}, {0, cells - 1}, {cells - 1, cells - 1}};
    for (const auto& [x, z] : corners) {
        decisions.due[tile(x, z)] = cell_due(x, z);
        decisions.all_due = decisions.all_due && decisions.due[tile(x, z)];
        for (const auto& [nx, nz] : corners) {
            const bool open = edge_open(x, z, nx, nz);
            if (open) {
                decisions.open |= std::uint32_t{1} << (4 * tile(x, z) + tile(nx, nz));
            }
            decisions.all_open = decisions.all_open && open;
        }
    }
    ScentStepGates gates;
    if (!decisions.all_due) {
        gates.participants = ScentParticipants(split_x, split_z, decisions.due);
    }
    // An open domain uses the original ungated arithmetic, with the same
    // bilinear interpolation and without per-sample sealed-path walks.
    if (!decisions.all_open) {
        gates.exchange = ScentExchange(split_x, split_z, decisions.open);
    }
    return gates;
}

} // namespace luminumbra::ai::detail
