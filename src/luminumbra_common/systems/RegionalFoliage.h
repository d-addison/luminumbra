#pragma once

#include "../fields/FieldPageState.h"
#include "../simulation/SimBudgetTelemetry.h"
#include "../world/DistantPolicy.h"
#include "IrrigationSystem.h"
#include "RegionConsumerGate.h"
#include "SoilNutrientSystem.h"
#include <map>
#include <optional>
#include <stdexcept>

namespace luminumbra::foliage {
using ::Luminumbra::world::ActiveRegionLedger;
using ::Luminumbra::world::FieldPageState;
using ::Luminumbra::world::FoliageFieldPages;
using ::Luminumbra::world::RegionKey;
using ::Luminumbra::world::RegionState;
using ::Luminumbra::world::ScentCellKey;

// Species cadence never changes region-owned page or water scheduling.
inline bool PlantPolicyDue(const ::Luminumbra::world::ActiveRegionRecord& record,
                           RegionKey key,
                           const ::Luminumbra::world::PlantDistantPolicy& policy,
                           std::uint64_t tick) {
    if (record.state == RegionState::Frozen)
        return false;
    const auto shift = record.state == RegionState::Active
                           ? std::uint8_t{0}
                           : std::min(record.cadence_shift, policy.max_cadence_shift);
    return (tick & ((std::uint64_t{1} << shift) - 1)) == ActiveRegionLedger::phase(key, shift);
}
inline std::optional<std::uint32_t>
PlantPolicyWindow(const ::Luminumbra::world::ActiveRegionRecord& record,
                  RegionKey key,
                  const ::Luminumbra::world::PlantDistantPolicy& policy,
                  const ::Luminumbra::world::WorldClock& clock,
                  std::uint64_t stamp) {
    if (record.state == RegionState::Frozen || stamp > clock.tick())
        return {};
    if (record.frozen_at) {
        const auto resume = ActiveRegionLedger::resume_window(
            key, record.frozen_at, clock, std::min(policy.resume_cap_ticks, std::uint32_t{36000}));
        return static_cast<std::uint32_t>(std::min(clock.tick() - stamp, resume.elapsed_ticks));
    }
    if (!PlantPolicyDue(record, key, policy, clock.tick()))
        return {};
    return static_cast<std::uint32_t>(std::min(clock.tick() - stamp, std::uint64_t{65536}));
}

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

// Replay bounded single integer operations with current constant stage/uptake
// and source output. This deliberately preserves per-tick clamping and absorbed
// ordering, rather than multiplying a net rate at depleted/saturated cells.
// Resume uses <=36000 passes; the ledger permits ordinary cadence up to 65536. No new pages are
// created by evolution. Historical cells continue even with an empty roster.
inline std::map<RegionKey, std::uint64_t>
AdvanceFoliageInteriors(entt::registry& reg,
                        FieldPageState& pages,
                        const std::map<RegionKey, std::uint32_t>& windows,
                        std::optional<std::uint64_t> current_tick = {},
                        simulation::SimBudgetTelemetry::TickScope* budget = nullptr,
                        const ::Luminumbra::world::RegionEntityGate& participant_gate = {}) {
    auto soil = FoliageCells(pages.soil());
    auto moisture = FoliageCells(pages.irrigation());
    std::map<RegionKey, std::uint64_t> work;
    std::vector<entt::entity> feeders, sources;
    for (auto e : reg.view<Comp::PlantTag,
                           Comp::SoilFeederComponent,
                           Comp::PlantGrowthComponent,
                           Comp::TransformComponent>())
        if (::Luminumbra::world::AdmitRegionEntity(participant_gate, e))
            feeders.push_back(e);
    for (auto e : reg.view<Comp::WaterSourceComponent, Comp::TransformComponent>())
        if (::Luminumbra::world::AdmitRegionEntity(participant_gate, e))
            sources.push_back(e);
    std::sort(feeders.begin(), feeders.end(), [&reg](auto a, auto b) {
        return reg.get<Comp::PlantRegionComponent>(a).persistent_id <
               reg.get<Comp::PlantRegionComponent>(b).persistent_id;
    });
    std::sort(sources.begin(), sources.end());
    std::uint32_t max_window = 0;
    for (const auto& [owner, k] : windows) {
        if (k > 65536)
            throw std::invalid_argument("Unbounded foliage window");
        max_window = std::max(max_window, k);
    }
    const auto due = [&windows](ScentCellKey c, std::uint32_t step) {
        const auto it = windows.find(FieldPageState::region_of_scent_cell(c));
        return it != windows.end() && step < it->second;
    };
    const auto existed = [&reg, current_tick](entt::entity e) {
        const auto* owner = reg.try_get<Comp::PlantRegionComponent>(e);
        const auto* growth = reg.try_get<Comp::PlantGrowthComponent>(e);
        return !current_tick || !owner || !growth || owner->region_stamp < *current_tick ||
               growth->planted_tick == *current_tick;
    };
    for (std::uint32_t step = 0; step < max_window; ++step) {
        if (budget)
            budget->Next(simulation::SimBudgetStage::Soil, [&soil, &due, step] {
                return static_cast<std::uint64_t>(
                    std::count_if(soil.begin(), soil.end(), [&due, step](const auto& cell) {
                        return due(cell.first, step);
                    }));
            });
        for (auto e : feeders) {
            const auto c = FoliageCell(reg.get<Comp::TransformComponent>(e).position);
            if (!due(c, step) || !existed(e))
                continue;
            auto it = soil.find(c);
            if (it == soil.end())
                throw std::runtime_error("Unadmitted soil feeder");
            auto& f = reg.get<Comp::SoilFeederComponent>(e);
            const auto draw = static_cast<std::int32_t>(
                static_cast<std::int64_t>(f.uptake) *
                SoilStageWeight(reg.get<Comp::PlantGrowthComponent>(e).stage) / 16);
            const auto got = std::min(it->second, draw);
            it->second -= got;
            f.absorbed += static_cast<std::uint32_t>(got);
            ++work[FieldPageState::region_of_scent_cell(c)];
        }
        for (auto& [c, value] : soil)
            if (due(c, step)) {
                value = std::min(kSoilBaseline, value + kSoilRegenPerTick);
                ++work[FieldPageState::region_of_scent_cell(c)];
            }
        if (budget)
            budget->Next(simulation::SimBudgetStage::Irrigation, [&moisture, &due, step] {
                return static_cast<std::uint64_t>(
                    std::count_if(moisture.begin(), moisture.end(), [&due, step](const auto& cell) {
                        return due(cell.first, step);
                    }));
            });
        std::map<ScentCellKey, std::int64_t> deposits;
        for (auto e : sources) {
            const auto c = FoliageCell(reg.get<Comp::TransformComponent>(e).position);
            if (due(c, step) && existed(e)) {
                deposits[c] += reg.get<Comp::WaterSourceComponent>(e).output;
                ++work[FieldPageState::region_of_scent_cell(c)];
            }
        }
        for (const auto& [c, deposit] : deposits) {
            auto it = moisture.find(c);
            if (it == moisture.end())
                throw std::runtime_error("Unadmitted irrigation source");
            it->second = static_cast<std::int32_t>(
                std::min<std::int64_t>(kMoistureMax, it->second + deposit));
        }
        const auto prior = moisture;
        for (const auto& [c, value] : prior)
            if (due(c, step)) {
                const auto owner = FieldPageState::region_of_scent_cell(c);
                const ScentCellKey neighbors[] = {
                    {c.x - 1, c.z}, {c.x + 1, c.z}, {c.x, c.z - 1}, {c.x, c.z + 1}};
                for (const auto n : neighbors) {
                    if (!prior.contains(n) || FieldPageState::region_of_scent_cell(n) != owner)
                        continue;
                    const auto flux = value / kMoistureDiffuseShareDen;
                    moisture[c] -= flux;
                    moisture[n] += flux;
                }
            }
        for (auto& [c, value] : moisture)
            if (due(c, step)) {
                value = std::max(0, std::min(kMoistureMax, value) - kMoistureDrainPerTick);
                ++work[FieldPageState::region_of_scent_cell(c)];
            }
    }
    for (const auto& [c, value] : soil)
        if (!pages.set_soil(c, value))
            throw std::runtime_error("Soil commit refused");
    for (const auto& [c, value] : moisture)
        if (!pages.set_irrigation(c, value))
            throw std::runtime_error("Irrigation commit refused");
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
    const auto prior = FoliageCells(previous.irrigation());
    std::map<ScentCellKey, std::int32_t> balances;
    std::map<RegionKey, std::uint64_t> work;
    for (const auto& [c, value] : prior) {
        const auto owner = FieldPageState::region_of_scent_cell(c);
        const auto a = records.find(owner);
        if (a == records.end() || a->second.state == RegionState::Frozen)
            continue;
        for (const auto n : {ScentCellKey{c.x + 1, c.z}, ScentCellKey{c.x, c.z + 1}}) {
            const auto other = FieldPageState::region_of_scent_cell(n);
            const auto b = records.find(other);
            if (owner == other || !prior.contains(n) || b == records.end() ||
                b->second.state == RegionState::Frozen)
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
