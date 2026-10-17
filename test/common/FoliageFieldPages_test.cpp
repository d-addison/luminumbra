#include "luminumbra_common/fields/FieldPageState.h"
#include "luminumbra_common/fields/FoliageFieldPages.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include "luminumbra_common/systems/RegionalFoliage.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra::world;
TEST(FoliageFieldPages, ExactUnitsAndInitializedBaselineSurviveTypedSave) {
    FieldPageState state;
    const ScentCellKey negative{-1, -1}, positive{512, 0};
    ASSERT_TRUE(state.set_soil(negative, 999997));
    ASSERT_TRUE(state.set_soil(positive, FieldPageState::kSoilBaseline));
    ASSERT_TRUE(state.set_irrigation(negative, 3999999));
    const auto bytes = state.serialize();
    FieldPageState restored;
    std::string error;
    ASSERT_TRUE(FieldPageState::parse(bytes, restored, error)) << error;
    EXPECT_EQ(restored.serialize(), bytes);
    EXPECT_EQ(restored.soil().sample(negative, 1000000), 999997);
    EXPECT_EQ(restored.irrigation().sample(negative, 0), 3999999);
    EXPECT_EQ(restored.soil().sample({0, 0}, 1000000), 1000000);
    EXPECT_TRUE(restored.owns_region({-1, -1}));
    EXPECT_TRUE(restored.owns_region({1, 0}));
    EXPECT_FALSE(restored.owns_region({0, 0}));
    EXPECT_NE(restored.region_bytes({1, 0}), "");    // baseline still initialized/owned
    EXPECT_EQ(restored.ambient_bytes(31, true), ""); // ambient slots unchanged by foliage
    EXPECT_EQ(restored.region_bytes({-1, -1}), state.region_bytes({-1, -1}));
}
TEST(FoliageFieldPages, StrictRecordValidationLeavesDestinationUntouched) {
    FoliageFieldPages original;
    ASSERT_TRUE(original.set({0, 0}, 10, 1000000));
    const auto record = original.records();
    FoliageFieldPages restored;
    ASSERT_TRUE(FoliageFieldPages::parse_records(record, 1, 1000000, restored));
    const auto prior = restored.records();
    for (const auto offset : {0u, 8u, 40u, 44u}) {
        auto corrupt = record;
        if (offset == 0)
            corrupt.replace(0, 4, std::string(4, '\x7f')); // invalid page
        else if (offset == 8)
            corrupt.replace(8, 32, std::string(32, '\0')); // empty mask
        else
            corrupt[offset] = '\xff'; // initialized overflow or nonzero uninitialized
        EXPECT_FALSE(FoliageFieldPages::parse_records(corrupt, 1, 100, restored));
        EXPECT_EQ(restored.records(), prior);
    }
    EXPECT_FALSE(FoliageFieldPages::parse_records(record + record, 2, 1000000, restored));
    EXPECT_FALSE(FoliageFieldPages::parse_records(
        record, FoliageFieldPages::kMaxPages + 1, 1000000, restored));
    EXPECT_EQ(restored.records(), prior);
}
TEST(FoliageFieldPages, HalfOpenWorldBoundsAndRefusalAreAtomic) {
    FoliageFieldPages pages;
    ASSERT_TRUE(pages.set({-16777216, -16777216}, 1, 1000000));
    ASSERT_TRUE(pages.set({16777215, 16777215}, 0, 1000000));
    const auto before = pages.records();
    EXPECT_FALSE(pages.set({16777216, 0}, 1, 1000000));
    EXPECT_FALSE(pages.set({-16777217, 0}, 1, 1000000));
    EXPECT_FALSE(pages.set({0, 0}, -1, 1000000));
    EXPECT_FALSE(pages.set({0, 0}, 1000001, 1000000));
    EXPECT_EQ(pages.records(), before);
}
} // namespace

namespace {
using namespace luminumbra::foliage;
TEST(FoliageFieldPages, LinearSnapshotsAndNeighborsMatchSparseCellMap) {
    FoliageFieldPages pages;
    // Partial masks, multiple Z pages in each X column, owner boundaries and
    // both world limits exercise ordering independently of evolution values.
    for (const int x : {-16777216, -513, -512, -17, -16, -1, 0, 15, 16, 511, 512, 16777215})
        for (const int z : {-16777216, -513, -512, -17, -16, -1, 0, 15, 16, 511, 512, 16777215})
            if ((x % 3) != (z % 3))
                ASSERT_TRUE(pages.set({x, z}, 123, 1000000));
    const FlatFoliageCells flat(pages);
    const auto reference = FoliageCells(pages);
    const std::vector<FlatFoliageCells::Cell> expected(reference.begin(), reference.end());
    ASSERT_EQ(flat.cells, expected);
    const auto neighbors = flat.interior_neighbors();
    ASSERT_EQ(neighbors.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto c = expected[i].first;
        const ScentCellKey adjacent[] = {
            {c.x - 1, c.z}, {c.x + 1, c.z}, {c.x, c.z - 1}, {c.x, c.z + 1}};
        for (std::size_t n = 0; n < 4; ++n) {
            const auto found = reference.find(adjacent[n]);
            const auto index =
                found != reference.end() && FieldPageState::region_of_scent_cell(adjacent[n]) ==
                                                FieldPageState::region_of_scent_cell(c)
                    ? static_cast<std::size_t>(std::distance(reference.begin(), found))
                    : FlatFoliageCells::absent;
            EXPECT_EQ(neighbors[i][n], index);
        }
    }
    EXPECT_TRUE(FlatFoliageCells(FoliageFieldPages{}).interior_neighbors().empty());
}
// Node-based scalar oracle retains sealed map copies and per-neighbour lookups.
inline std::map<RegionKey, std::uint64_t>
ReferenceFoliageInteriors(entt::registry& reg,
                          FieldPageState& pages,
                          const std::map<RegionKey, std::uint32_t>& windows,
                          std::optional<std::uint64_t> current_tick = {},
                          luminumbra::simulation::SimBudgetTelemetry::TickScope* budget = nullptr) {
    auto soil = FoliageCells(pages.soil());
    auto moisture = FoliageCells(pages.irrigation());
    std::map<RegionKey, std::uint64_t> work;
    std::vector<entt::entity> feeders, sources;
    for (auto e : reg.view<Comp::PlantTag,
                           Comp::SoilFeederComponent,
                           Comp::PlantGrowthComponent,
                           Comp::TransformComponent>())
        feeders.push_back(e);
    for (auto e : reg.view<Comp::WaterSourceComponent, Comp::TransformComponent>())
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
            budget->Next(luminumbra::simulation::SimBudgetStage::Soil, [&soil, &due, step] {
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
            budget->Next(luminumbra::simulation::SimBudgetStage::Irrigation,
                         [&moisture, &due, step] {
                             return static_cast<std::uint64_t>(std::count_if(
                                 moisture.begin(), moisture.end(), [&due, step](const auto& cell) {
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
ReferenceFoliageBorders(FieldPageState& pages,
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

TEST(FoliageFieldPages, FlatSnapshotsMatchScalarEvolutionAfterEditsLoadAndCadenceChanges) {
    namespace C = Luminumbra::Components;
    const auto make_registry = [] {
        entt::registry reg;
        std::uint64_t id = 0;
        for (const auto position : {Luminumbra::Vec3(-1, 0, -1),
                                    Luminumbra::Vec3(511, 0, 0),
                                    Luminumbra::Vec3(512, 0, 0)}) {
            const auto e = reg.create();
            reg.emplace<C::TransformComponent>(e).position = position;
            reg.emplace<C::PlantTag>(e);
            reg.emplace<C::PlantGrowthComponent>(e).stage = 3;
            reg.emplace<C::SoilFeederComponent>(e).uptake = 137;
            const auto owner = ActiveRegionLedger::region_at(position);
            reg.emplace<C::PlantRegionComponent>(e, ++id, owner.x, owner.z, std::uint64_t{0});
            reg.emplace<C::WaterSourceComponent>(e).output = 1234;
        }
        return reg;
    };
    auto actual_reg = make_registry(), expected_reg = make_registry();
    FieldPageState actual;
    for (const auto cell : {ScentCellKey{-1, -1}, ScentCellKey{511, 0}, ScentCellKey{512, 0}}) {
        ASSERT_TRUE(ActivateFoliagePage(actual, cell, true));
        ASSERT_TRUE(ActivateFoliagePage(actual, cell, false));
        ASSERT_TRUE(actual.set_soil(cell, 17)); // Ordered uptake/clamping matters at depletion.
        ASSERT_TRUE(actual.set_irrigation(cell, 999999));
    }
    auto expected = actual;
    for (std::uint64_t tick = 1; tick <= 8; ++tick) {
        SCOPED_TRACE(tick);
        std::map<RegionKey, ActiveRegionRecord> records;
        records[{-1, -1}] = {};
        records[{0, 0}] = {};
        records[{1, 0}] = {};
        records[{1, 0}].state = tick < 3 ? RegionState::Frozen : RegionState::Reduced;
        records[{1, 0}].cadence_shift = 2;
        records[{1, 0}].cadence_phase = 1;
        std::map<RegionKey, std::uint32_t> windows;
        windows[{-1, -1}] = 1;
        windows[{0, 0}] = tick % 2;
        if (tick >= 3 && tick % 4 == 1)
            windows[{1, 0}] = 4;
        const auto old_actual = actual.foliage_snapshot(),
                   old_expected = expected.foliage_snapshot();
        EXPECT_EQ(AdvanceFoliageInteriors(actual_reg, actual, windows),
                  ReferenceFoliageInteriors(expected_reg, expected, windows));
        EXPECT_EQ(ExchangeFoliageBorders(actual, old_actual, records, tick),
                  ReferenceFoliageBorders(expected, old_expected, records, tick));
        EXPECT_EQ(actual.serialize(), expected.serialize());
        for (auto e : actual_reg.view<C::SoilFeederComponent>())
            EXPECT_EQ(actual_reg.get<C::SoilFeederComponent>(e).absorbed,
                      expected_reg.get<C::SoilFeederComponent>(e).absorbed);
        if (tick == 3) {
            // A sparse new cell changes both ordering and neighbour admission.
            ASSERT_TRUE(actual.set_irrigation({-17, -1}, 111));
            ASSERT_TRUE(expected.set_irrigation({-17, -1}, 111));
        }
        if (tick == 5) {
            FieldPageState loaded;
            std::string error;
            ASSERT_TRUE(FieldPageState::parse(actual.serialize(), loaded, error)) << error;
            actual = std::move(loaded);
        }
    }
}
entt::registry CatchupRegistry() {
    namespace C = Luminumbra::Components;
    entt::registry registry;
    std::uint64_t identity = 0;
    // Shared cells test persistent-ID uptake order and summed source clamping.
    for (const auto position : {Luminumbra::Vec3{-1, 0, -1},
                                Luminumbra::Vec3{511, 0, 0},
                                Luminumbra::Vec3{511, 0, 0},
                                Luminumbra::Vec3{512, 0, 0}}) {
        const auto entity = registry.create();
        registry.emplace<C::TransformComponent>(entity).position = position;
        registry.emplace<C::PlantTag>(entity);
        registry.emplace<C::PlantGrowthComponent>(entity).stage = 3;
        registry.emplace<C::SoilFeederComponent>(entity).uptake = 137;
        const auto owner = ActiveRegionLedger::region_at(position);
        registry.emplace<C::PlantRegionComponent>(
            entity, ++identity, owner.x, owner.z, std::uint64_t{0});
        registry.emplace<C::WaterSourceComponent>(entity).output = 65535;
    }
    return registry;
}

void SetCatchupStages(entt::registry& registry, std::uint32_t step) {
    namespace C = Luminumbra::Components;
    for (auto entity : registry.view<C::PlantGrowthComponent>())
        registry.get<C::PlantGrowthComponent>(entity).stage = static_cast<std::uint8_t>(step % 6);
}

void ExpectCatchupState(const FieldPageState& actual,
                        const FieldPageState& expected,
                        const entt::registry& actual_registry,
                        const entt::registry& expected_registry) {
    namespace C = Luminumbra::Components;
    EXPECT_EQ(actual.soil().records(), expected.soil().records());
    EXPECT_EQ(actual.irrigation().records(), expected.irrigation().records());
    EXPECT_EQ(actual.serialize(), expected.serialize());
    EXPECT_EQ(Luminumbra::Persistence::StableChecksum(actual.canonical_bytes()),
              Luminumbra::Persistence::StableChecksum(expected.canonical_bytes()));
    for (auto entity : actual_registry.view<const C::SoilFeederComponent>())
        EXPECT_EQ(actual_registry.get<const C::SoilFeederComponent>(entity).absorbed,
                  expected_registry.get<const C::SoilFeederComponent>(entity).absorbed);
}

TEST(FoliageFieldPages, PreparedCatchupMatchesPerStepRebuildWithStageChangesAndBirthAdmission) {
    auto cached_registry = CatchupRegistry();
    auto rebuilt_registry = CatchupRegistry();
    auto scalar_registry = CatchupRegistry();
    FieldPageState cached;
    for (const auto cell : {ScentCellKey{-1, -1}, ScentCellKey{511, 0}, ScentCellKey{512, 0}}) {
        ASSERT_TRUE(ActivateFoliagePage(cached, cell, true));
        ASSERT_TRUE(ActivateFoliagePage(cached, cell, false));
        ASSERT_TRUE(cached.set_soil(cell, 17));
        ASSERT_TRUE(cached.set_irrigation(cell, kMoistureMax - 1));
    }
    // A loaded partial page must gain its missing cells even when a later birth
    // lands on its already initialized cell.
    ASSERT_TRUE(cached.set_soil({16, 0}, 17));
    auto rebuilt = cached;
    auto scalar = cached;
    FoliageInteriorReplay replay(cached);
    const auto preparation = replay.work().preparation_cells;
    for (std::uint32_t step = 0; step < 24; ++step) {
        SCOPED_TRACE(step);
        SetCatchupStages(cached_registry, step);
        SetCatchupStages(rebuilt_registry, step);
        SetCatchupStages(scalar_registry, step);
        std::map<RegionKey, std::uint32_t> due{{{0, 0}, 1}};
        if (step >= 12)
            due[{-1, -1}] = 1;
        if (step >= 17)
            due[{1, 0}] = 1;
        if (step == 4) {
            // A birth can admit a page and die later in the SAME substep. Its
            // initialized page must still evolve even with no surviving feeder.
            for (auto* pages : {&cached, &rebuilt, &scalar})
                ASSERT_TRUE(ActivateFoliagePage(*pages, {16, 0}, true));
            replay.AdmitSoil(cached, {16, 0});
            // Also exercise admission of an entirely new owner during replay.
            for (auto* pages : {&cached, &rebuilt, &scalar})
                ASSERT_TRUE(ActivateFoliagePage(*pages, {1024, 0}, true));
            replay.AdmitSoil(cached, {1024, 0});
        }
        if (step >= 4)
            due[{2, 0}] = 1;
        const auto tick = std::uint64_t{100} + step;
        const auto work = replay.Step(cached_registry, cached, due, tick);
        EXPECT_EQ(work, AdvanceFoliageInteriors(rebuilt_registry, rebuilt, due, tick));
        EXPECT_EQ(work, ReferenceFoliageInteriors(scalar_registry, scalar, due, tick));
        ExpectCatchupState(cached, rebuilt, cached_registry, rebuilt_registry);
        ExpectCatchupState(cached, scalar, cached_registry, scalar_registry);
    }
    EXPECT_EQ(replay.work().preparation_cells, preparation);
    EXPECT_EQ(replay.work().admitted_soil_cells, 511u);
}

TEST(FoliageFieldPages, CatchupPreparationAndEvolutionWorkExcludeDormantOwnersAtPageAndStepCaps) {
    FieldPageState pages;
    for (std::size_t i = 0; i < FoliageFieldPages::kMaxPages; ++i) {
        const ScentCellKey cell{static_cast<int>(i) * 512, 0};
        ASSERT_TRUE(pages.set_soil(cell, 0));
        ASSERT_TRUE(pages.set_irrigation(cell, 1000));
    }
    const auto dormant = pages.region_bytes({1, 0});
    FoliageInteriorReplay replay(pages);
    entt::registry registry;
    const std::map<RegionKey, std::uint32_t> due{{{0, 0}, 1}};
    for (std::uint32_t step = 0; step < 65536; ++step)
        replay.Step(registry, pages, due);
    EXPECT_EQ(replay.work().preparation_cells, 2u * FoliageFieldPages::kMaxPages);
    EXPECT_EQ(replay.work().neighbor_cells, FoliageFieldPages::kMaxPages);
    EXPECT_EQ(replay.work().evolved_cells, 2u * 65536u);
    EXPECT_EQ(replay.work().admitted_soil_cells, 0u);
    EXPECT_EQ(pages.region_bytes({1, 0}), dormant);
    EXPECT_EQ(pages.soil().sample({512, 0}, -1), 0);
    EXPECT_EQ(pages.irrigation().sample({512, 0}, -1), 1000);
}
} // namespace
