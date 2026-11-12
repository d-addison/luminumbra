#pragma once

#include "../fields/FieldPageState.h"
#include "IrrigationSystem.h"
#include "SoilNutrientSystem.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace luminumbra::foliage {
using ::Luminumbra::world::ActiveRegionLedger;
using ::Luminumbra::world::FieldPageKey;
using ::Luminumbra::world::FieldPageState;
using ::Luminumbra::world::FoliageFieldPages;
using ::Luminumbra::world::RegionKey;
using ::Luminumbra::world::ScentCellKey;
using FoliageCellMap = std::map<ScentCellKey, std::int32_t>;

inline ScentCellKey FoliageCell(const ::Luminumbra::Vec3& position) {
    if (!ActiveRegionLedger::valid_anchor(position))
        throw std::invalid_argument("Invalid foliage anchor");
    int x = 0, z = 0;
    WorldToCell(position.x, position.z, 0.0f, 0.0f, 1.0f, x, z);
    return {x, z};
}

// Full-store scalar snapshot for diagnostics and reference evolution. Tick callers
// use owner-selected preparation below rather than expanding dormant pages.
inline FoliageCellMap FoliageCells(const FoliageFieldPages& pages) {
    FoliageCellMap cells;
    for (const auto& [key, page] : pages.pages())
        for (unsigned index = 0; index < 256; ++index)
            if ((page.mask[index / 64] >> (index % 64)) & 1)
                cells.emplace(ScentCellKey{key.x * 16 + static_cast<int>(index % 16),
                                           key.z * 16 + static_cast<int>(index / 16)},
                              page.cells[index]);
    return cells;
}

// Sealed key-ordered topology: callers may edit values, but cannot insert, erase
// or reorder cells. Neighbor indices and scratch are reused across all steps.
class FlatFoliageCells {
public:
    using Cell = std::pair<ScentCellKey, std::int32_t>;
    static constexpr auto absent = std::numeric_limits<std::size_t>::max();
    FlatFoliageCells() = default;
    explicit FlatFoliageCells(const FoliageFieldPages& pages) {
        Build(pages, {});
    }
    FlatFoliageCells(const FoliageFieldPages& pages, RegionKey owner) {
        Build(pages, owner);
    }
    const std::vector<Cell>& cells() const {
        return m_cells;
    }
    auto begin() const {
        return m_cells.begin();
    }
    auto end() const {
        return m_cells.end();
    }
    auto find(ScentCellKey key) const {
        const auto found = LowerBound(key);
        return found != m_cells.end() && found->first == key ? found : m_cells.end();
    }
    bool contains(ScentCellKey key) const {
        return find(key) != end();
    }
    std::int32_t at(ScentCellKey key) const {
        const auto found = find(key);
        if (found == end())
            throw std::out_of_range("Absent foliage cell");
        return found->second;
    }
    std::int32_t& at(ScentCellKey key) {
        const auto found = LowerBound(key);
        if (found == m_cells.end() || found->first != key)
            throw std::out_of_range("Absent foliage cell");
        return found->second;
    }
    // Const diagnostics count actual page materialization and range probes,
    // including probes that find no pages. No dormant page masks are inspected.
    std::uint64_t prepared_pages() const {
        return m_preparedPages;
    }
    std::uint64_t page_probes() const {
        return m_pageProbes;
    }
    std::vector<std::array<std::size_t, 4>> interior_neighbors() const {
        // At most kMaxPages * 256 initialized cells, four indices per cell.
        std::vector<std::array<std::size_t, 4>> result(m_cells.size());
        std::array<std::size_t, 4> cursors{};
        for (std::size_t index = 0; index < m_cells.size(); ++index) {
            const auto cell = m_cells[index].first;
            const auto owner = FieldPageState::region_of_scent_cell(cell);
            const std::array<ScentCellKey, 4> adjacent{{{cell.x - 1, cell.z},
                                                        {cell.x + 1, cell.z},
                                                        {cell.x, cell.z - 1},
                                                        {cell.x, cell.z + 1}}};
            // Translation preserves key order: each cursor advances at most N
            // times in total, retaining the -X,+X,-Z,+Z transfer order.
            for (std::size_t direction = 0; direction < adjacent.size(); ++direction) {
                auto& cursor = cursors[direction];
                while (cursor < m_cells.size() && m_cells[cursor].first < adjacent[direction])
                    ++cursor;
                result[index][direction] =
                    cursor < m_cells.size() && m_cells[cursor].first == adjacent[direction] &&
                            FieldPageState::region_of_scent_cell(adjacent[direction]) == owner
                        ? cursor
                        : absent;
            }
        }
        return result;
    }
    void PrepareIrrigation() {
        if (m_prior.size() == m_cells.size())
            return;
        m_neighbors = interior_neighbors();
        m_prior.resize(m_cells.size());
    }
    void EvolveSoil() {
        for (auto& cell : m_cells)
            cell.second = std::min(kSoilBaseline, cell.second + kSoilRegenPerTick);
    }
    void EvolveIrrigation() {
        PrepareIrrigation();
        for (std::size_t index = 0; index < m_cells.size(); ++index)
            m_prior[index] = m_cells[index].second;
        for (std::size_t index = 0; index < m_cells.size(); ++index) {
            const auto flux = m_prior[index] / kMoistureDiffuseShareDen;
            for (const auto neighbor : m_neighbors[index]) {
                if (neighbor == absent)
                    continue;
                m_cells[index].second -= flux;
                m_cells[neighbor].second += flux;
            }
        }
        for (auto& cell : m_cells)
            cell.second = std::max(0, std::min(kMoistureMax, cell.second) - kMoistureDrainPerTick);
    }

private:
    using StoredPage = std::map<FieldPageKey, FoliageFieldPages::Page>::value_type;
    std::vector<Cell> m_cells;
    std::vector<std::array<std::size_t, 4>> m_neighbors;
    std::vector<std::int32_t> m_prior;
    std::uint64_t m_preparedPages = 0, m_pageProbes = 0;

    std::vector<Cell>::const_iterator LowerBound(ScentCellKey key) const {
        return std::lower_bound(
            m_cells.begin(), m_cells.end(), key, [](const Cell& cell, ScentCellKey wanted) {
                return cell.first < wanted;
            });
    }
    std::vector<Cell>::iterator LowerBound(ScentCellKey key) {
        return std::lower_bound(
            m_cells.begin(), m_cells.end(), key, [](const Cell& cell, ScentCellKey wanted) {
                return cell.first < wanted;
            });
    }
    void Build(const FoliageFieldPages& pages, std::optional<RegionKey> owner) {
        const auto& stored = pages.pages();
        std::vector<const StoredPage*> selected;
        // A 512 m owner contains at most 32 * 32 whole 16 m pages. Full
        // diagnostic snapshots are bounded by the storage's kMaxPages cap.
        selected.reserve(std::min(stored.size(), owner ? std::size_t{1024} : stored.size()));
        if (owner) {
            const std::int64_t first_x = static_cast<std::int64_t>(owner->x) * 32;
            const std::int64_t first_z = static_cast<std::int64_t>(owner->z) * 32;
            // Valid foliage pages lie in [-1048576, 1048575] on both axes.
            // Check before narrowing; arbitrary int32 owner keys are harmless.
            if (first_x < -1048576 || first_x > 1048544 || first_z < -1048576 || first_z > 1048544)
                return;
            for (std::int64_t x = first_x; x < first_x + 32; ++x) {
                ++m_pageProbes;
                auto page = stored.lower_bound(
                    {static_cast<std::int32_t>(x), static_cast<std::int32_t>(first_z)});
                for (; page != stored.end() && page->first.x == x && page->first.z < first_z + 32;
                     ++page)
                    selected.push_back(&*page);
            }
        } else {
            for (const auto& page : stored)
                selected.push_back(&page);
        }
        std::size_t count = 0;
        for (const auto* page : selected)
            for (const auto word : page->second.mask) {
                const auto added = static_cast<std::size_t>(std::popcount(word));
                if (added > m_cells.max_size() - count)
                    throw std::length_error("Foliage snapshot too large");
                count += added;
            }
        m_cells.reserve(count);
        // Emit local X before ascending page/local Z, matching map order even
        // across negative pages and sparse masks. No per-cell allocation.
        for (std::size_t column = 0; column < selected.size();) {
            auto next = column;
            while (next < selected.size() && selected[next]->first.x == selected[column]->first.x)
                ++next;
            for (int x = 0; x < 16; ++x)
                for (auto page = column; page < next; ++page)
                    for (int z = 0; z < 16; ++z) {
                        const auto index = static_cast<unsigned>(z * 16 + x);
                        const auto& [key, data] = *selected[page];
                        if ((data.mask[index / 64] >> (index % 64)) & 1)
                            m_cells.emplace_back(ScentCellKey{key.x * 16 + x, key.z * 16 + z},
                                                 data.cells[index]);
                    }
            column = next;
        }
        m_preparedPages = static_cast<std::uint64_t>(selected.size());
    }
};

// Inputs have valid foliage keys and already consumed/deposited channel values.
// Scalar helpers keep the sealed prior map as an independent diffusion oracle.
inline void EvolveFoliageSoil(FoliageCellMap& soil) {
    for (auto& [cell, value] : soil)
        value = std::min(kSoilBaseline, value + kSoilRegenPerTick);
}
inline void EvolveFoliageSoil(FlatFoliageCells& soil) {
    soil.EvolveSoil();
}
inline void EvolveFoliageIrrigation(FoliageCellMap& moisture) {
    const auto prior = moisture;
    for (const auto& [cell, value] : prior) {
        const auto owner = FieldPageState::region_of_scent_cell(cell);
        const ScentCellKey adjacent[] = {
            {cell.x - 1, cell.z}, {cell.x + 1, cell.z}, {cell.x, cell.z - 1}, {cell.x, cell.z + 1}};
        for (const auto neighbor : adjacent) {
            if (!prior.contains(neighbor) ||
                FieldPageState::region_of_scent_cell(neighbor) != owner)
                continue;
            const auto flux = value / kMoistureDiffuseShareDen;
            moisture.at(cell) -= flux;
            moisture.at(neighbor) += flux;
        }
    }
    for (auto& [cell, value] : moisture)
        value = std::max(0, std::min(kMoistureMax, value) - kMoistureDrainPerTick);
}
inline void EvolveFoliageIrrigation(FlatFoliageCells& moisture) {
    moisture.EvolveIrrigation();
}

// Admit exactly one page, preserving initialized values. A valid page either
// exists already or its first write checks capacity before any cells change.
inline bool ActivateFoliagePage(FieldPageState& pages, ScentCellKey cell, bool soil) {
    if (!FoliageFieldPages::valid_cell(cell))
        return false;
    const auto floor16 = [](int value) {
        return value >= 0 ? value / 16 : (value - 15) / 16;
    };
    const int x = floor16(cell.x) * 16, z = floor16(cell.z) * 16;
    const auto& initialized = soil ? pages.soil() : pages.irrigation();
    for (int iz = 0; iz < 16; ++iz)
        for (int ix = 0; ix < 16; ++ix) {
            const ScentCellKey key{x + ix, z + iz};
            if (initialized.initialized(key))
                continue;
            if (!(soil ? pages.set_soil(key, kSoilBaseline) : pages.set_irrigation(key, 0)))
                return false;
        }
    return true;
}

// Short-lived, sealed preparation across interleaved interior steps. Supply only
// touched/due owners; each is prepared on first use. Storage topology must remain
// unchanged for this object's lifetime. Recreate it after admissions or loading.
// Each Step evolves once for nonzero entries; scheduling/window expansion is the
// caller's responsibility. No registry, host clock or source enumeration is used.
class FoliageInteriorReplay {
public:
    struct Work {
        std::uint64_t preparation_pages = 0, preparation_cells = 0, page_probes = 0;
        std::uint64_t neighbor_cells = 0, evolved_cells = 0;
    };
    struct Owner {
        FlatFoliageCells soil, moisture;
    };
    const Work& work() const {
        return m_work;
    }
    Owner& Prepare(const FieldPageState& pages, RegionKey key) {
        const auto found = m_owners.find(key);
        if (found != m_owners.end())
            return found->second;
        Owner state{FlatFoliageCells(pages.soil(), key), FlatFoliageCells(pages.irrigation(), key)};
        state.moisture.PrepareIrrigation();
        const auto prepared_pages = state.soil.prepared_pages() + state.moisture.prepared_pages();
        const auto probes = state.soil.page_probes() + state.moisture.page_probes();
        const auto soil_cells = static_cast<std::uint64_t>(state.soil.cells().size());
        const auto moisture_cells = static_cast<std::uint64_t>(state.moisture.cells().size());
        auto inserted = m_owners.emplace(key, std::move(state));
        m_work.preparation_pages += prepared_pages;
        m_work.page_probes += probes;
        m_work.preparation_cells += soil_cells + moisture_cells;
        m_work.neighbor_cells += moisture_cells;
        return inserted.first->second;
    }
    std::map<RegionKey, std::uint64_t> Step(FieldPageState& pages,
                                            const std::map<RegionKey, std::uint32_t>& due) {
        std::map<RegionKey, std::uint64_t> completed;
        // At most 32 range probes per channel per newly participating owner,
        // then only its initialized cells. Prepared owners never rescan pages.
        for (const auto& [key, ticks] : due) {
            if (ticks == 0)
                continue;
            auto& state = Prepare(pages, key);
            const auto count = static_cast<std::uint64_t>(state.soil.cells().size()) +
                               static_cast<std::uint64_t>(state.moisture.cells().size());
            if (count != 0)
                completed.emplace(key, count);
        }
        // Finish all allocations before evolution or publication. Existing page
        // writes below cannot allocate while the sealed topology is retained.
        for (const auto& [key, count] : completed) {
            auto& state = m_owners.at(key);
            EvolveFoliageSoil(state.soil);
            EvolveFoliageIrrigation(state.moisture);
            for (const auto& [cell, value] : state.soil)
                if (!pages.set_soil(cell, value))
                    throw std::runtime_error("Soil commit refused");
            for (const auto& [cell, value] : state.moisture)
                if (!pages.set_irrigation(cell, value))
                    throw std::runtime_error("Irrigation commit refused");
            m_work.evolved_cells += count;
        }
        return completed;
    }

private:
    std::map<RegionKey, Owner> m_owners;
    Work m_work;
};
} // namespace luminumbra::foliage
