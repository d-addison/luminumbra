#pragma once

#include "../ai/CreatureTickAdmission.h"
#include "../fields/FieldPageState.h"
#include "FoliageRegistryStage.h"
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
#include <set>
#include <span>
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
// caller's responsibility.
class FoliageInteriorReplay {
public:
    struct Work {
        std::uint64_t preparation_pages = 0, preparation_cells = 0, page_probes = 0;
        std::uint64_t neighbor_cells = 0, evolved_cells = 0;
        std::uint64_t source_preparations = 0, source_visits = 0;
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
    std::map<RegionKey, std::uint64_t> Step(entt::registry& registry,
                                            FieldPageState& pages,
                                            const std::map<RegionKey, std::uint32_t>& due,
                                            std::optional<std::uint64_t> subtick = {}) {
        namespace C = ::Luminumbra::Components;
        for (const auto& [key, ticks] : due) {
            if (ticks != 0) {
                Prepare(pages, key);
            }
        }
        PrepareSources(registry);
        for (const auto& [key, ticks] : due) {
            if (ticks == 0) {
                continue;
            }
            auto found = m_sources.find(key);
            if (found == m_sources.end()) {
                continue;
            }
            auto& sources = found->second;
            const auto through = subtick.value_or(std::numeric_limits<std::uint64_t>::max());
            while (!sources.pending.empty() && sources.pending.begin()->first <= through) {
                for (const auto index : sources.pending.begin()->second) {
                    sources.active.insert(index);
                }
                sources.pending.erase(sources.pending.begin());
            }
            for (const auto index : sources.active) {
                const auto& source = sources.ordered[index];
                ++m_work.source_visits;
                if (source.soil) {
                    auto& soil = m_owners.at(key).soil;
                    if (!soil.contains(source.cell)) {
                        continue;
                    }
                    auto& feeder = registry.get<C::SoilFeederComponent>(source.entity);
                    const auto draw =
                        static_cast<std::int32_t>(feeder.uptake) *
                        SoilStageWeight(
                            registry.get<C::PlantGrowthComponent>(source.entity).stage) /
                        16;
                    const auto got = std::min(soil.at(source.cell), draw);
                    soil.at(source.cell) -= got;
                    feeder.absorbed += static_cast<std::uint32_t>(got);
                } else {
                    auto& moisture = m_owners.at(key).moisture;
                    if (moisture.contains(source.cell)) {
                        moisture.at(source.cell) = std::min(
                            kMoistureMax,
                            moisture.at(source.cell) +
                                static_cast<std::int32_t>(
                                    registry.get<C::WaterSourceComponent>(source.entity).output));
                    }
                }
            }
        }
        return Step(pages, due);
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
    struct Source {
        entt::entity entity;
        ScentCellKey cell;
        bool soil;
        std::uint64_t first_tick;
    };
    struct Sources {
        std::vector<Source> ordered;
        std::map<std::uint64_t, std::vector<std::size_t>> pending;
        std::set<std::size_t> active;
    };
    std::map<RegionKey, Sources> m_sources;
    bool m_sourcesPrepared = false;
    // A replay seals the selected registry. Each component pool has EnTT's
    // entity-mask bound; sources are collected and ordered once, then only
    // activated sources of due owners are visited during replay.
    void PrepareSources(const entt::registry& registry) {
        if (m_sourcesPrepared) {
            return;
        }
        namespace C = ::Luminumbra::Components;
        const auto collect = [&](entt::entity entity, bool soil) {
            ++m_work.source_preparations;
            const auto cell = FoliageCell(registry.get<C::TransformComponent>(entity).position);
            const auto key = FieldPageState::region_of_scent_cell(cell);
            const auto* owner = registry.try_get<const C::PlantRegionComponent>(entity);
            if (owner && RegionKey{owner->owner_x, owner->owner_z} != key) {
                return;
            }
            const auto* growth = registry.try_get<const C::PlantGrowthComponent>(entity);
            const auto first =
                owner && growth
                    ? (growth->planted_tick == owner->region_stamp
                           ? owner->region_stamp
                           : owner->region_stamp +
                                 (owner->region_stamp != std::numeric_limits<std::uint64_t>::max()))
                    : (growth ? growth->planted_tick : 0);
            m_sources[key].ordered.push_back({entity, cell, soil, first});
        };
        for (const auto entity : registry.view<const C::PlantTag,
                                               const C::SoilFeederComponent,
                                               const C::PlantGrowthComponent,
                                               const C::TransformComponent>()) {
            collect(entity, true);
        }
        for (const auto entity :
             registry.view<const C::WaterSourceComponent, const C::TransformComponent>()) {
            collect(entity, false);
        }
        for (auto& [key, sources] : m_sources) {
            std::sort(sources.ordered.begin(),
                      sources.ordered.end(),
                      [&registry](const Source& left, const Source& right) {
                          if (left.soil != right.soil) {
                              return left.soil;
                          }
                          const auto* a =
                              registry.try_get<const C::PlantRegionComponent>(left.entity);
                          const auto* b =
                              registry.try_get<const C::PlantRegionComponent>(right.entity);
                          const auto id_a = a ? a->persistent_id : 0;
                          const auto id_b = b ? b->persistent_id : 0;
                          if ((id_a != 0) != (id_b != 0)) {
                              return id_a != 0;
                          }
                          return id_a != id_b ? id_a < id_b : left.entity < right.entity;
                      });
            for (std::size_t index = 0; index < sources.ordered.size(); ++index) {
                sources.pending[sources.ordered[index].first_tick].push_back(index);
            }
        }
        m_sourcesPrepared = true;
    }
    std::map<RegionKey, Owner> m_owners;
    Work m_work;
};
// Materialized edits publish matching stamps. Ordinary edits only wake the
// owner and must retain its entire historical field window across reopening.
inline std::uint64_t FoliagePriorTick(const ::Luminumbra::world::ActiveRegionRecord& record) {
    if (record.frozen_at != 0 &&
        !(record.last_edit != 0 && record.last_ticked == record.last_edit)) {
        return record.frozen_at;
    }
    return record.last_ticked;
}
struct RegionalFoliageWindows {
    std::map<RegionKey, std::uint32_t> elapsed;
    std::map<RegionKey, std::uint64_t> resumes;
};
inline RegionalFoliageWindows
BuildRegionalFoliageWindows(const ActiveRegionLedger& ledger,
                            std::span<const RegionKey> due,
                            const std::map<RegionKey, std::uint64_t>& previous,
                            const std::set<RegionKey>& pending,
                            const ::Luminumbra::world::WorldClock& clock) {
    RegionalFoliageWindows result;
    const auto add = [&](RegionKey key) {
        const auto found = ledger.records().find(key);
        if (found == ledger.records().end() ||
            found->second.state == ::Luminumbra::world::RegionState::Frozen) {
            return;
        }
        const auto& record = found->second;
        const auto old = previous.find(key);
        auto prior = old == previous.end() ? FoliagePriorTick(record) : old->second;
        if (record.frozen_at == 0) {
            prior = std::max(prior, record.first_active);
        }
        if (prior > clock.tick()) {
            throw std::invalid_argument("Foliage window precedes owner stamp");
        }
        const auto elapsed = record.frozen_at != 0
                                 ? ActiveRegionLedger::resume_window(
                                       key, std::max(prior, record.frozen_at), clock, 36000)
                                       .elapsed_ticks
                                 : std::min<std::uint64_t>(clock.tick() - prior, 65536);
        // Scheduling stamps last_ticked before foliage sees a new owner.
        // Its first due tick still runs once; known edit ticks remain consumed.
        result.elapsed[key] = static_cast<std::uint32_t>(
            elapsed == 0 && old == previous.end() && record.frozen_at == 0 ? 1 : elapsed);
        if (record.frozen_at != 0) {
            result.resumes[key] = record.frozen_at;
        }
    };
    for (const auto key : due) {
        add(key);
    }
    for (const auto key : pending) {
        const auto found = ledger.records().find(key);
        if (found != ledger.records().end() && found->second.frozen_at != 0) {
            add(key);
        }
    }
    return result;
}
inline std::map<RegionKey, std::uint64_t>
AdvanceFoliageInteriors(entt::registry& registry,
                        FieldPageState& pages,
                        const std::map<RegionKey, std::uint32_t>& windows,
                        std::optional<std::uint64_t> tick = {},
                        FoliageInteriorReplay::Work* diagnostic = nullptr) {
    std::uint32_t maximum = 0;
    for (const auto& [key, ticks] : windows) {
        if (ticks > 65536 || (tick && ticks > *tick)) {
            throw std::invalid_argument("Unbounded foliage window");
        }
        if (pages.soil().owns_region(key) || pages.irrigation().owns_region(key)) {
            maximum = std::max(maximum, ticks);
        }
    }
    FoliageInteriorReplay replay;
    std::map<RegionKey, std::uint64_t> work;
    // At most one start entry per participating owner; the replay cap is
    // 65,536 steps. Owners without pages never enter the per-step traversal.
    std::map<std::uint32_t, std::vector<RegionKey>> starts;
    for (const auto& [key, ticks] : windows) {
        if (ticks != 0 && (pages.soil().owns_region(key) || pages.irrigation().owns_region(key))) {
            starts[maximum - ticks].push_back(key);
        }
    }
    std::map<RegionKey, std::uint32_t> due;
    for (std::uint32_t step = 0; step < maximum; ++step) {
        const auto added = starts.find(step);
        if (added != starts.end()) {
            for (const auto key : added->second) {
                due.emplace(key, 1);
            }
        }
        for (const auto& [key, count] : replay.Step(
                 registry,
                 pages,
                 due,
                 tick ? std::optional<std::uint64_t>(*tick - maximum + step + 1) : std::nullopt)) {
            work[key] += count;
        }
    }
    if (diagnostic) {
        *diagnostic = replay.work();
    }
    return work;
}
using FoliageEdges = std::vector<std::pair<ScentCellKey, ScentCellKey>>;
inline FoliageEdges
FoliageBorderEdges(const FieldPageState& pages,
                   const std::map<RegionKey, ::Luminumbra::world::ActiveRegionRecord>& records,
                   std::span<const RegionKey> owners,
                   std::uint64_t tick) {
    FoliageEdges edges;
    // At most 1024 positive boundary cells per selected 512 m owner; storage
    // admission limits initialized endpoints to kMaxPages * 256 cells.
    for (const auto owner : owners) {
        const auto a = records.find(owner);
        if (a == records.end() || a->second.state == ::Luminumbra::world::RegionState::Frozen) {
            continue;
        }
        const auto x = static_cast<std::int64_t>(owner.x) * 512;
        const auto z = static_cast<std::int64_t>(owner.z) * 512;
        if (x < -16777216 || x > 16776704 || z < -16777216 || z > 16776704) {
            continue;
        }
        for (int axis = 0; axis < 2; ++axis) {
            const RegionKey other{owner.x + (axis == 0 ? 1 : 0), owner.z + (axis == 1 ? 1 : 0)};
            const auto b = records.find(other);
            if (b == records.end() || b->second.state == ::Luminumbra::world::RegionState::Frozen) {
                continue;
            }
            const auto shift = std::max(a->second.cadence_shift, b->second.cadence_shift);
            if (shift > 16 || (tick & ((std::uint64_t{1} << shift) - 1)) != 0) {
                continue;
            }
            for (int offset = 0; offset < 512; ++offset) {
                const ScentCellKey cell{static_cast<std::int32_t>(x) + (axis == 0 ? 511 : offset),
                                        static_cast<std::int32_t>(z) + (axis == 1 ? 511 : offset)};
                const ScentCellKey next{cell.x + (axis == 0 ? 1 : 0), cell.z + (axis == 1 ? 1 : 0)};
                if (pages.irrigation().initialized(cell) && pages.irrigation().initialized(next)) {
                    edges.emplace_back(cell, next);
                }
            }
        }
    }
    std::sort(edges.begin(), edges.end(), [](const auto& left, const auto& right) {
        return left.first == right.first ? left.second.x > right.second.x
                                         : left.first < right.first;
    });
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    return edges;
}
inline std::vector<std::int32_t> FoliageBorderFluxes(const FoliageFieldPages& irrigation,
                                                     const FoliageEdges& edges) {
    std::vector<std::int32_t> fluxes;
    // One prior-value difference per admitted edge, bounded by two positive
    // edges per initialized irrigation cell (kMaxPages * 256 cells).
    fluxes.reserve(edges.size());
    for (const auto& [cell, next] : edges) {
        fluxes.push_back(irrigation.sample(cell, 0) / kMoistureDiffuseShareDen -
                         irrigation.sample(next, 0) / kMoistureDiffuseShareDen);
    }
    return fluxes;
}
inline std::map<RegionKey, std::uint64_t> ExchangeFoliageBorders(
    FieldPageState& pages, std::span<const std::int32_t> fluxes, const FoliageEdges& edges) {
    if (fluxes.size() != edges.size()) {
        throw std::invalid_argument("Incomplete irrigation border snapshot");
    }
    std::map<RegionKey, std::uint64_t> work;
    std::size_t index = 0;
    for (const auto& [cell, next] : edges) {
        const auto flux = fluxes[index++];
        const auto donor = flux >= 0 ? cell : next, recipient = flux >= 0 ? next : cell;
        const auto available = pages.irrigation().sample(donor, 0);
        const auto balance = pages.irrigation().sample(recipient, 0);
        const auto amount = std::min({flux >= 0 ? flux : -flux, available, kMoistureMax - balance});
        if (!pages.set_irrigation(donor, available - amount) ||
            !pages.set_irrigation(recipient, balance + amount)) {
            throw std::runtime_error("Irrigation border commit refused");
        }
        ++work[FieldPageState::region_of_scent_cell(cell)];
        ++work[FieldPageState::region_of_scent_cell(next)];
    }
    return work;
}
inline entt::registry
SelectFoliageRegistry(const entt::registry& original,
                      const RegionalFoliageWindows& windows,
                      const std::set<FieldPageKey>& borders,
                      const luminumbra::ai::CreatureTickAdmission* admission = nullptr) {
    entt::registry selected;
    const auto collect = [&]<typename T>() {
        for (auto entity : original.view<const T>()) {
            if (selected.valid(entity)) {
                continue;
            }
            const auto* owner = original.try_get<const Comp::PlantRegionComponent>(entity);
            const auto* transform = original.try_get<const Comp::TransformComponent>(entity);
            bool include = owner && windows.elapsed.contains({owner->owner_x, owner->owner_z});
            if (!owner && !include && transform &&
                ActiveRegionLedger::valid_anchor(transform->position)) {
                const auto cell = FoliageCell(transform->position);
                const auto found = windows.elapsed.find(FieldPageState::region_of_scent_cell(cell));
                const auto page = [](int value) {
                    return value >= 0 ? value / 16 : (value - 15) / 16;
                };
                include = (found != windows.elapsed.end() && found->second != 0) ||
                          borders.contains({page(cell.x), page(cell.z)});
            }
            // Creature ownership was captured before movement. Keep admitted
            // creatures even when their current position crossed a region boundary.
            if (admission && original.all_of<Comp::CreatureComponent>(entity)) {
                include = admission->Allows(entity);
            }
            if (include) {
                if (selected.create(entity) != entity) {
                    throw std::runtime_error("Foliage handle refused");
                }
                stage_detail::CopyFoliage(original, entity, selected, entity);
            }
        }
    };
    stage_detail::Participants(collect);
    return selected;
}
} // namespace luminumbra::foliage
