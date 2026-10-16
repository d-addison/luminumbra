#pragma once

#include "../fields/FieldPageState.h"
#include "../simulation/SimBudgetTelemetry.h"
#include "IrrigationSystem.h"
#include "SoilNutrientSystem.h"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

namespace luminumbra::foliage {
using ::Luminumbra::world::ActiveRegionLedger;
using ::Luminumbra::world::FieldPageState;
using ::Luminumbra::world::FoliageFieldPages;
using ::Luminumbra::world::RegionKey;
using ::Luminumbra::world::RegionState;
using ::Luminumbra::world::ScentCellKey;

inline ScentCellKey FoliageCell(const ::Luminumbra::Vec3& p) {
    if (!ActiveRegionLedger::valid_anchor(p))
        throw std::invalid_argument("Invalid foliage anchor");
    int x = 0, z = 0;
    WorldToCell(p.x, p.z, 0, 0, 1, x, z);
    return {x, z};
}
inline std::map<ScentCellKey, std::int32_t> FoliageCells(const FoliageFieldPages& pages) {
    std::map<ScentCellKey, std::int32_t> cells;
    for (const auto& [key, page] : pages.pages())
        for (unsigned i = 0; i < 256; ++i)
            if ((page.mask[i / 64] >> (i % 64)) & 1)
                cells.emplace(ScentCellKey{key.x * 16 + static_cast<int>(i % 16),
                                           key.z * 16 + static_cast<int>(i / 16)},
                              page.cells[i]);
    return cells;
}
// A sealed, key-ordered cell snapshot with contiguous storage. Evolution never
// inserts cells, so lower_bound replaces tree nodes without changing traversal.
class FlatFoliageCells {
public:
    using Cell = std::pair<ScentCellKey, std::int32_t>;
    static constexpr auto absent = std::numeric_limits<std::size_t>::max();
    FlatFoliageCells() = default;
    explicit FlatFoliageCells(const FoliageFieldPages& pages) {
        cells.reserve(pages.pages().size() * 256);
        // Pages are ordered by (x,z). Expand each page column's local X before
        // its ascending page/local Z to emit the exact cell-key order directly.
        // Masks may be sparse, and negative page coordinates need no special case.
        const auto& stored = pages.pages();
        for (auto column = stored.begin(); column != stored.end();) {
            auto next = column;
            while (next != stored.end() && next->first.x == column->first.x)
                ++next;
            for (int x = 0; x < 16; ++x)
                for (auto page = column; page != next; ++page)
                    for (int z = 0; z < 16; ++z) {
                        const auto i = static_cast<unsigned>(z * 16 + x);
                        if ((page->second.mask[i / 64] >> (i % 64)) & 1)
                            cells.emplace_back(
                                ScentCellKey{page->first.x * 16 + x, page->first.z * 16 + z},
                                page->second.cells[i]);
                    }
            column = next;
        }
    }
    auto begin() {
        return cells.begin();
    }
    auto end() {
        return cells.end();
    }
    auto begin() const {
        return cells.begin();
    }
    auto end() const {
        return cells.end();
    }
    auto find(ScentCellKey key) {
        const auto it = std::lower_bound(
            cells.begin(), cells.end(), key, [](const Cell& cell, ScentCellKey wanted) {
                return cell.first < wanted;
            });
        return it != cells.end() && it->first == key ? it : cells.end();
    }
    auto find(ScentCellKey key) const {
        const auto it = std::lower_bound(
            cells.begin(), cells.end(), key, [](const Cell& cell, ScentCellKey wanted) {
                return cell.first < wanted;
            });
        return it != cells.end() && it->first == key ? it : cells.end();
    }
    bool contains(ScentCellKey key) const {
        return find(key) != end();
    }
    std::int32_t at(ScentCellKey key) const {
        const auto it = find(key);
        if (it == end())
            throw std::out_of_range("Absent foliage cell");
        return it->second;
    }
    // Positive and negative neighbours retain the original -X,+X,-Z,+Z order.
    std::vector<std::array<std::size_t, 4>> interior_neighbors() const {
        std::vector<std::array<std::size_t, 4>> result(cells.size());
        // Translating sorted keys by a fixed offset leaves them sorted. Each
        // neighbour cursor therefore advances at most cells.size() times; this
        // replaces four binary searches per cell with four linear merge walks.
        std::array<std::size_t, 4> cursors{};
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const auto c = cells[i].first;
            const auto owner = FieldPageState::region_of_scent_cell(c);
            const ScentCellKey adjacent[] = {
                {c.x - 1, c.z}, {c.x + 1, c.z}, {c.x, c.z - 1}, {c.x, c.z + 1}};
            for (std::size_t n = 0; n < 4; ++n) {
                auto& cursor = cursors[n];
                while (cursor < cells.size() && cells[cursor].first < adjacent[n])
                    ++cursor;
                result[i][n] = cursor < cells.size() && cells[cursor].first == adjacent[n] &&
                                       FieldPageState::region_of_scent_cell(adjacent[n]) == owner
                                   ? cursor
                                   : absent;
            }
        }
        return result;
    }
    std::vector<Cell> cells;
};
// One page per source/feeder admission. Entire one-metre page is initialized,
// including baseline cells; activation never expands recursively from diffusion.
// Copy-and-commit at the caller makes two-channel/ledger admission atomic.
inline bool ActivateFoliagePage(FieldPageState& pages, ScentCellKey cell, bool soil) {
    const auto floor16 = [](int n) {
        return n >= 0 ? n / 16 : (n - 15) / 16;
    };
    const int x = floor16(cell.x) * 16, z = floor16(cell.z) * 16;
    const auto& initialized = soil ? pages.soil() : pages.irrigation();
    for (int iz = 0; iz < 16; ++iz)
        for (int ix = 0; ix < 16; ++ix) {
            const ScentCellKey c{x + ix, z + iz};
            if (initialized.initialized(c))
                continue;
            if (!(soil ? pages.set_soil(c, kSoilBaseline) : pages.set_irrigation(c, 0)))
                return false;
        }
    return true;
}

// One sealed snapshot/index preparation per interleaved catch-up. Each step
// touches only due owners; values are published before plant environment samples.
// Births may admit soil pages between steps: import just that page on first use.
// No replay step scans/rebuilds the dormant owners' cells or moisture indexes.
class FoliageInteriorReplay {
public:
    struct Work {
        std::uint64_t preparation_cells = 0;
        std::uint64_t neighbor_cells = 0;
        std::uint64_t evolved_cells = 0;
        std::uint64_t admitted_soil_cells = 0;
    };
    explicit FoliageInteriorReplay(const FieldPageState& pages, bool has_steps = true) {
        if (!has_steps)
            return; // An off-due host tick needs no interior preparation.
        const FlatFoliageCells soil(pages.soil());
        const FlatFoliageCells moisture(pages.irrigation());
        for (const auto& cell : soil)
            owners[FieldPageState::region_of_scent_cell(cell.first)].soil.cells.push_back(cell);
        for (const auto& cell : moisture)
            owners[FieldPageState::region_of_scent_cell(cell.first)].moisture.cells.push_back(cell);
        for (auto& [key, state] : owners) {
            state.neighbors = state.moisture.interior_neighbors();
            state.prior.resize(state.moisture.cells.size());
        }
        measured.preparation_cells =
            static_cast<std::uint64_t>(soil.cells.size() + moisture.cells.size());
        measured.neighbor_cells = static_cast<std::uint64_t>(moisture.cells.size());
    }
    const Work& work() const {
        return measured;
    }
    void AdmitSoil(const FieldPageState& pages, ScentCellKey cell) {
        auto& soil = owners[FieldPageState::region_of_scent_cell(cell)].soil;
        AdmitSoilPage(pages, cell, soil);
    }
    std::map<RegionKey, std::uint64_t>
    Step(entt::registry& reg,
         FieldPageState& pages,
         const std::map<RegionKey, std::uint32_t>& due,
         std::optional<std::uint64_t> current_tick = {},
         simulation::SimBudgetTelemetry::TickScope* budget = nullptr) {
        std::map<RegionKey, std::uint64_t> completed;
        if (budget)
            budget->Next(simulation::SimBudgetStage::Soil,
                         [this, &due] { return DueCells(due, true); });
        FeedSoil(reg, pages, due, current_tick, completed);
        EvolveSoil(pages, due, completed);
        if (budget)
            budget->Next(simulation::SimBudgetStage::Irrigation,
                         [this, &due] { return DueCells(due, false); });
        DepositMoisture(reg, due, current_tick, completed);
        EvolveMoisture(pages, due, completed);
        return completed;
    }

private:
    struct Owner {
        FlatFoliageCells soil;
        FlatFoliageCells moisture;
        std::vector<std::array<std::size_t, 4>> neighbors;
        std::vector<std::int32_t> prior;
    };
    std::map<RegionKey, Owner> owners;
    Work measured;

    static bool
    Existed(entt::registry& reg, entt::entity entity, std::optional<std::uint64_t> current_tick) {
        const auto* owner = reg.try_get<Comp::PlantRegionComponent>(entity);
        const auto* growth = reg.try_get<Comp::PlantGrowthComponent>(entity);
        return !current_tick || !owner || !growth || owner->region_stamp < *current_tick ||
               growth->planted_tick == *current_tick;
    }
    std::uint64_t DueCells(const std::map<RegionKey, std::uint32_t>& due, bool soil) const {
        std::uint64_t count = 0;
        for (const auto& [key, ticks] : due) {
            const auto found = owners.find(key);
            if (ticks != 0 && found != owners.end())
                count += static_cast<std::uint64_t>(soil ? found->second.soil.cells.size()
                                                         : found->second.moisture.cells.size());
        }
        return count;
    }
    // Existing cached cells retain their evolved values; only freshly initialized
    // cells are imported. A soil birth cannot change the moisture topology.
    void AdmitSoilPage(const FieldPageState& pages, ScentCellKey cell, FlatFoliageCells& soil) {
        const auto floor16 = [](int value) {
            return value >= 0 ? value / 16 : (value - 15) / 16;
        };
        const int px = floor16(cell.x), pz = floor16(cell.z);
        const auto page = pages.soil().pages().find({px, pz});
        if (page == pages.soil().pages().end())
            throw std::runtime_error("Unadmitted soil feeder");
        std::vector<FlatFoliageCells::Cell> added;
        for (unsigned i = 0; i < 256; ++i) {
            const ScentCellKey key{px * 16 + static_cast<int>(i % 16),
                                   pz * 16 + static_cast<int>(i / 16)};
            if (((page->second.mask[i / 64] >> (i % 64)) & 1) && !soil.contains(key))
                added.emplace_back(key, page->second.cells[i]);
        }
        if (added.empty())
            return;
        measured.admitted_soil_cells += static_cast<std::uint64_t>(added.size());
        soil.cells.insert(soil.cells.end(), added.begin(), added.end());
        std::sort(soil.cells.begin(), soil.cells.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
    }
    void FeedSoil(entt::registry& reg,
                  const FieldPageState& pages,
                  const std::map<RegionKey, std::uint32_t>& due,
                  std::optional<std::uint64_t> current_tick,
                  std::map<RegionKey, std::uint64_t>& completed) {
        std::vector<entt::entity> feeders;
        for (auto entity : reg.view<Comp::PlantTag,
                                    Comp::SoilFeederComponent,
                                    Comp::PlantGrowthComponent,
                                    Comp::TransformComponent>())
            feeders.push_back(entity);
        std::sort(feeders.begin(), feeders.end(), [&reg](auto a, auto b) {
            return reg.get<Comp::PlantRegionComponent>(a).persistent_id <
                   reg.get<Comp::PlantRegionComponent>(b).persistent_id;
        });
        for (auto entity : feeders) {
            const auto cell = FoliageCell(reg.get<Comp::TransformComponent>(entity).position);
            const auto key = FieldPageState::region_of_scent_cell(cell);
            const auto window = due.find(key);
            if (window == due.end() || window->second == 0 || !Existed(reg, entity, current_tick))
                continue;
            auto& soil = owners[key].soil;
            if (!soil.contains(cell))
                AdmitSoilPage(pages, cell, soil);
            auto found = soil.find(cell);
            if (found == soil.end())
                throw std::runtime_error("Unadmitted soil feeder");
            auto& feeder = reg.get<Comp::SoilFeederComponent>(entity);
            const auto draw = static_cast<std::int32_t>(
                static_cast<std::int64_t>(feeder.uptake) *
                SoilStageWeight(reg.get<Comp::PlantGrowthComponent>(entity).stage) / 16);
            const auto got = std::min(found->second, draw);
            found->second -= got;
            feeder.absorbed += static_cast<std::uint32_t>(got);
            ++completed[key];
        }
    }
    void EvolveSoil(FieldPageState& pages,
                    const std::map<RegionKey, std::uint32_t>& due,
                    std::map<RegionKey, std::uint64_t>& completed) {
        for (const auto& [key, ticks] : due) {
            auto found = owners.find(key);
            if (ticks == 0 || found == owners.end())
                continue;
            for (auto& [cell, value] : found->second.soil) {
                value = std::min(kSoilBaseline, value + kSoilRegenPerTick);
                if (!pages.set_soil(cell, value))
                    throw std::runtime_error("Soil commit refused");
                ++completed[key];
                ++measured.evolved_cells;
            }
        }
    }
    void DepositMoisture(entt::registry& reg,
                         const std::map<RegionKey, std::uint32_t>& due,
                         std::optional<std::uint64_t> current_tick,
                         std::map<RegionKey, std::uint64_t>& completed) {
        std::map<ScentCellKey, std::int64_t> deposits;
        for (auto entity : reg.view<Comp::WaterSourceComponent, Comp::TransformComponent>()) {
            const auto cell = FoliageCell(reg.get<Comp::TransformComponent>(entity).position);
            const auto key = FieldPageState::region_of_scent_cell(cell);
            const auto window = due.find(key);
            if (window != due.end() && window->second != 0 && Existed(reg, entity, current_tick)) {
                deposits[cell] += reg.get<Comp::WaterSourceComponent>(entity).output;
                ++completed[key];
            }
        }
        for (const auto& [cell, amount] : deposits) {
            auto& moisture = owners[FieldPageState::region_of_scent_cell(cell)].moisture;
            auto found = moisture.find(cell);
            if (found == moisture.end())
                throw std::runtime_error("Unadmitted irrigation source");
            found->second = static_cast<std::int32_t>(
                std::min<std::int64_t>(kMoistureMax, found->second + amount));
        }
    }
    void EvolveMoisture(FieldPageState& pages,
                        const std::map<RegionKey, std::uint32_t>& due,
                        std::map<RegionKey, std::uint64_t>& completed) {
        for (const auto& [key, ticks] : due) {
            auto found = owners.find(key);
            if (ticks == 0 || found == owners.end())
                continue;
            auto& state = found->second;
            for (std::size_t i = 0; i < state.prior.size(); ++i)
                state.prior[i] = state.moisture.cells[i].second;
            for (std::size_t i = 0; i < state.prior.size(); ++i) {
                const auto flux = state.prior[i] / kMoistureDiffuseShareDen;
                for (const auto neighbor : state.neighbors[i]) {
                    if (neighbor == FlatFoliageCells::absent)
                        continue;
                    state.moisture.cells[i].second -= flux;
                    state.moisture.cells[neighbor].second += flux;
                }
            }
            for (auto& [cell, value] : state.moisture) {
                value = std::max(0, std::min(kMoistureMax, value) - kMoistureDrainPerTick);
                if (!pages.set_irrigation(cell, value))
                    throw std::runtime_error("Irrigation commit refused");
                ++completed[key];
                ++measured.evolved_cells;
            }
        }
    }
};

// Standalone constant-input replay. The host also keeps one replay object across
// its interleaved growth/living steps, preserving per-tick clamping and samples.
inline std::map<RegionKey, std::uint64_t>
AdvanceFoliageInteriors(entt::registry& reg,
                        FieldPageState& pages,
                        const std::map<RegionKey, std::uint32_t>& windows,
                        std::optional<std::uint64_t> current_tick = {},
                        simulation::SimBudgetTelemetry::TickScope* budget = nullptr) {
    std::uint32_t max_window = 0;
    for (const auto& [key, ticks] : windows) {
        if (ticks > 65536)
            throw std::invalid_argument("Unbounded foliage window");
        max_window = std::max(max_window, ticks);
    }
    FoliageInteriorReplay replay(pages);
    std::map<RegionKey, std::uint64_t> work;
    for (std::uint32_t step = 0; step < max_window; ++step) {
        std::map<RegionKey, std::uint32_t> due;
        for (const auto& [key, ticks] : windows)
            if (step < ticks)
                due.emplace(key, 1);
        for (const auto& [key, updates] : replay.Step(reg, pages, due, current_tick, budget))
            work[key] += updates;
    }
    return work;
}

// Cross-owner exchange is a separate common-phase pass, independent of interior
// due phases. Both directions sample the same prior HOST tick, before deposits.
// Transfers use current balances in cell-key order, +X before +Z, so shared
// endpoints cannot spend drained moisture or overfill a recipient.
inline std::map<RegionKey, std::uint64_t>
ExchangeFoliageBorders(FieldPageState& pages,
                       const FieldPageState& previous,
                       const std::map<RegionKey, ::Luminumbra::world::ActiveRegionRecord>& records,
                       std::uint64_t tick) {
    const auto prior = FlatFoliageCells(previous.irrigation());
    std::map<ScentCellKey, std::int32_t> balances;
    std::map<RegionKey, std::uint64_t> work;
    for (const auto& [c, value] : prior) {
        const auto owner = FieldPageState::region_of_scent_cell(c);
        const auto a = records.find(owner);
        if (a == records.end() || a->second.state == RegionState::Frozen)
            continue;
        for (const auto n : {ScentCellKey{c.x + 1, c.z}, ScentCellKey{c.x, c.z + 1}}) {
            const auto other = FieldPageState::region_of_scent_cell(n);
            // Interior edges never participate in this pass. Avoid resolving a
            // second ledger record for every interior cell of every page.
            if (owner == other)
                continue;
            const auto b = records.find(other);
            if (!prior.contains(n) || b == records.end() || b->second.state == RegionState::Frozen)
                continue;
            const auto shift = std::max(a->second.cadence_shift, b->second.cadence_shift);
            if (tick % (std::uint64_t{1} << shift))
                continue;
            const auto flux = value / 8 - prior.at(n) / 8;
            auto& current = balances.try_emplace(c, pages.irrigation().sample(c, 0)).first->second;
            auto& neighbor = balances.try_emplace(n, pages.irrigation().sample(n, 0)).first->second;
            auto& donor = flux >= 0 ? current : neighbor;
            auto& recipient = flux >= 0 ? neighbor : current;
            const auto requested = flux >= 0 ? flux : -flux;
            const auto transfer = std::min({requested, donor, kMoistureMax - recipient});
            donor -= transfer;
            recipient += transfer;
            ++work[owner];
            ++work[other];
        }
    }
    for (const auto& [c, value] : balances) {
        if (!pages.set_irrigation(c, value))
            throw std::runtime_error("Irrigation border commit refused");
    }
    return work;
}
} // namespace luminumbra::foliage
