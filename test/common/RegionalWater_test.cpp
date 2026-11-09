#include <gtest/gtest.h>

#include "luminumbra_common/systems/RegionalWater.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <latch>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::Systems;

void FillWater(Chunk& chunk, int resolution, std::int32_t depth = 1000) {
    chunk.has_water_sim.store(true);
    chunk.current_water_resolution.store(resolution);
    const auto cells = static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
    chunk.water_depth_mm.assign(cells, depth);
    chunk.water_bed_mm.assign(cells, 1000);
    chunk.water_edge_flux.assign(2 * cells, 0);
    chunk.water_level_data.assign(cells, 2.0f);
}

std::int64_t Mass(const Chunk& chunk) {
    return std::accumulate(
        chunk.water_depth_mm.begin(), chunk.water_depth_mm.end(), std::int64_t{0});
}

auto Snapshot(const Chunk& chunk) {
    return std::make_tuple(chunk.water_depth_mm,
                           chunk.water_edge_flux,
                           chunk.water_src_mm,
                           chunk.water_level_data,
                           chunk.is_water_sleeping.load(),
                           chunk.water_mesh_generated.load(),
                           chunk.water_mesh_dirty_ticks,
                           chunk.max_water_delta_last_tick,
                           chunk.is_water_state_dirty(),
                           chunk.has_water_authority(),
                           chunk.water_revision());
}

TEST(RegionalWater, InteriorAndSeamsConserveRandomizedSignedCoordinateGrids) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    std::mt19937 random(std::mt19937::result_type{7123});
    std::uniform_int_distribution<std::int32_t> depth(0, 100000), bed(-5000, 5000);
    for (const int resolution : {2, 4, 8, 16, 32}) {
        for (const bool is_x : {false, true}) {
            Chunk owner({-2, 0, -3}), neighbor(is_x ? IVec3(-1, 0, -3) : IVec3(-2, 0, -2));
            FillWater(owner, resolution);
            FillWater(neighbor, resolution);
            for (int step = 0; step < 12; ++step) {
                for (Chunk* chunk : {&owner, &neighbor}) {
                    for (std::size_t i = 0; i < chunk->water_depth_mm.size(); ++i) {
                        chunk->water_depth_mm[i] = depth(random);
                        chunk->water_bed_mm[i] = bed(random);
                    }
                }
                const auto before = Mass(owner) + Mass(neighbor);
                const auto a =
                    WaterKernels::StepInterior(owner, shield, {.rain_mm = 3, .evap_mm = 2});
                const auto b =
                    WaterKernels::StepInterior(neighbor, shield, {.rain_mm = 3, .evap_mm = 2});
                ASSERT_TRUE(a.applied && b.applied);
                EXPECT_EQ(a.cells_visited, owner.water_depth_mm.size());
                EXPECT_EQ(b.cells_visited, neighbor.water_depth_mm.size());
                EXPECT_EQ(a.depth_before + a.source_mm, a.depth_after + a.sink_mm);
                EXPECT_EQ(b.depth_before + b.source_mm, b.depth_after + b.sink_mm);
                const auto seam = WaterKernels::StepSharedFace(owner, neighbor, is_x);
                ASSERT_TRUE(seam.applied);
                EXPECT_EQ(seam.cells_visited, static_cast<std::size_t>(resolution));
                EXPECT_EQ(seam.depth_before, seam.depth_after);
                EXPECT_EQ(before + a.source_mm + b.source_mm,
                          Mass(owner) + Mass(neighbor) + a.sink_mm + b.sink_mm);
            }
        }
    }
}

TEST(RegionalWater, SourceAndDestinationOverflowRefuseBeforeMutationOrMatchLegacySaturation) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    for (const int resolution : {2, 8, 16}) {
        for (const bool source : {false, true}) {
            Chunk chunk({-1, 0, -1});
            FillWater(chunk, resolution, maximum - 1);
            chunk.water_bed_mm.assign(chunk.water_bed_mm.size(), maximum);
            if (!source) {
                chunk.water_bed_mm.back() = 1;
            }
            const auto before = Snapshot(chunk);
            const auto before_depth = chunk.water_depth_mm;
            const WaterKernels::InteriorInputs inputs{.rain_mm = source ? 2 : 0};
            const auto refused = WaterKernels::StepInterior(chunk, shield, inputs);
            EXPECT_FALSE(refused.applied);
            EXPECT_TRUE(refused.saturated);
            EXPECT_EQ(Snapshot(chunk), before);
            EXPECT_EQ(chunk.water_depth_mm, before_depth);
            const auto accepted = WaterKernels::StepInterior(
                chunk, shield, inputs, WaterKernels::OverflowPolicy::Saturate);
            ASSERT_TRUE(accepted.applied);
            EXPECT_TRUE(accepted.saturated);
            EXPECT_EQ(accepted.depth_before + accepted.source_mm,
                      accepted.depth_after + accepted.sink_mm);
            if (source) {
                EXPECT_EQ(chunk.water_depth_mm,
                          std::vector<std::int32_t>(before_depth.size(), maximum));
            } else {
                EXPECT_GT(accepted.sink_mm, 0);
            }
        }
    }
}

TEST(RegionalWater, CheckedCacheOverflowAndLegacyNormalizationReportSaturation) {
    TerrainGenParams params{};
    params.rivers_enabled = false;
    SHIELD_WorldSystem shield(nullptr, nullptr, params, 1337);
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    Chunk chunk({-1, 0, -1});
    FillWater(chunk, 8, maximum);
    chunk.water_bed_mm.assign(64, maximum);
    chunk.water_src_mm.assign(64, 24);
    const WaterKernels::InteriorInputs inputs{.finite_hydrology = false};
    const auto refused = WaterKernels::StepInterior(chunk, shield, inputs);
    EXPECT_FALSE(refused.applied);
    EXPECT_TRUE(refused.saturated);
    EXPECT_EQ(chunk.water_src_mm, std::vector<std::int32_t>(64, 24));
    EXPECT_EQ(chunk.water_depth_mm, std::vector<std::int32_t>(64, maximum));
    const auto accepted =
        WaterKernels::StepInterior(chunk, shield, inputs, WaterKernels::OverflowPolicy::Saturate);
    ASSERT_TRUE(accepted.applied);
    EXPECT_TRUE(accepted.saturated);
    EXPECT_EQ(accepted.source_mm, 0);
    Chunk cold({-1, 0, -1}), legacy_cold({-1, 0, -1});
    FillWater(cold, 8);
    FillWater(legacy_cold, 8);
    ASSERT_TRUE(WaterKernels::StepInterior(cold, shield, inputs).applied);
    ASSERT_TRUE(WaterKernels::StepInterior(
                    legacy_cold, shield, inputs, WaterKernels::OverflowPolicy::Saturate)
                    .applied);
    EXPECT_EQ(cold.water_src_mm, legacy_cold.water_src_mm);
    EXPECT_EQ(cold.water_src_mm, std::vector<std::int32_t>(64, 0));
    EXPECT_EQ(cold.water_depth_mm, legacy_cold.water_depth_mm);
    for (const auto bed : {std::numeric_limits<std::int32_t>::min(), -maximum}) {
        SCOPED_TRACE(bed);
        chunk.water_bed_mm.assign(64, bed);
        const auto before = Snapshot(chunk);
        const auto sea = WaterKernels::StepInterior(chunk, shield, {});
        EXPECT_EQ(sea.applied, bed == -maximum);
        EXPECT_EQ(sea.saturated, bed != -maximum);
        if (sea.applied) {
            // An applied step may refresh derived surface levels, but not depth.
            EXPECT_EQ(chunk.water_depth_mm, std::get<0>(before));
        } else {
            EXPECT_EQ(Snapshot(chunk), before);
        }
        const auto legacy =
            WaterKernels::StepInterior(chunk, shield, {}, WaterKernels::OverflowPolicy::Saturate);
        EXPECT_TRUE(legacy.applied);
        EXPECT_EQ(legacy.saturated, bed != -maximum);
        EXPECT_EQ(chunk.water_depth_mm, std::vector<std::int32_t>(64, maximum));
    }
    FillWater(chunk, 8, std::numeric_limits<std::int32_t>::min());
    const auto normalized =
        WaterKernels::StepInterior(chunk, shield, {}, WaterKernels::OverflowPolicy::Saturate);
    ASSERT_TRUE(normalized.applied);
    EXPECT_TRUE(normalized.saturated);
    EXPECT_EQ(normalized.depth_before + normalized.source_mm,
              normalized.depth_after + normalized.sink_mm);
    EXPECT_EQ(Mass(chunk), 0);
}

TEST(RegionalWater, ExtremeMomentumAndUncachedSourcesRefuseWithoutPublishing) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    Chunk chunk({-1, 0, -1});
    FillWater(chunk, 32, maximum);
    chunk.water_bed_mm.assign(1024, std::numeric_limits<std::int32_t>::min());
    chunk.water_bed_mm[0] = maximum;
    chunk.water_edge_flux.assign(2048, maximum);
    const auto before = Snapshot(chunk);
    const auto refused = WaterKernels::StepInterior(chunk, shield, {});
    EXPECT_FALSE(refused.applied);
    EXPECT_TRUE(refused.saturated);
    EXPECT_EQ(Snapshot(chunk), before);
    const auto accepted =
        WaterKernels::StepInterior(chunk, shield, {}, WaterKernels::OverflowPolicy::Saturate);
    ASSERT_TRUE(accepted.applied);
    EXPECT_TRUE(accepted.saturated);
    EXPECT_EQ(accepted.depth_before + accepted.source_mm, accepted.depth_after + accepted.sink_mm);
    for (const auto flux : chunk.water_edge_flux) {
        EXPECT_GE(flux, -2000000000);
        EXPECT_LE(flux, 2000000000);
    }
    FillWater(chunk, 8, maximum);
    const auto uncached = Snapshot(chunk);
    const auto source_refusal =
        WaterKernels::StepInterior(chunk, shield, {.finite_hydrology = false, .rain_mm = 1});
    EXPECT_FALSE(source_refusal.applied);
    EXPECT_TRUE(source_refusal.saturated);
    EXPECT_EQ(Snapshot(chunk), uncached);
}

TEST(RegionalWater, SeamReceiverCapacityRefusesOrSaturatesInBothDirectionsAndAxes) {
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    for (const int resolution : {2, 8, 16, 32}) {
        for (const bool is_x : {false, true}) {
            for (const bool reverse : {false, true}) {
                for (const int room : {0, 1}) {
                    Chunk owner({-1, 0, -1}), neighbor(is_x ? IVec3(0, 0, -1) : IVec3(-1, 0, 0));
                    FillWater(owner, resolution, reverse ? maximum - room : maximum);
                    FillWater(neighbor, resolution, reverse ? maximum : maximum - room);
                    owner.water_bed_mm.assign(owner.water_bed_mm.size(), reverse ? 1 : maximum);
                    neighbor.water_bed_mm.assign(neighbor.water_bed_mm.size(),
                                                 reverse ? maximum : 1);
                    const auto before_a = owner.water_depth_mm, before_b = neighbor.water_depth_mm;
                    const auto before = Mass(owner) + Mass(neighbor);
                    const auto state_a = Snapshot(owner), state_b = Snapshot(neighbor);
                    const auto refused = WaterKernels::StepSharedFace(owner, neighbor, is_x);
                    EXPECT_FALSE(refused.applied);
                    EXPECT_TRUE(refused.saturated);
                    EXPECT_EQ(Snapshot(owner), state_a);
                    EXPECT_EQ(Snapshot(neighbor), state_b);
                    EXPECT_EQ(owner.water_depth_mm, before_a);
                    EXPECT_EQ(neighbor.water_depth_mm, before_b);
                    EXPECT_EQ(owner.water_edge_flux,
                              std::vector<std::int32_t>(2 * before_a.size(), 0));
                    const auto accepted = WaterKernels::StepSharedFace(
                        owner, neighbor, is_x, WaterKernels::OverflowPolicy::Saturate);
                    ASSERT_TRUE(accepted.applied);
                    EXPECT_TRUE(accepted.saturated);
                    EXPECT_EQ(Mass(owner) + Mass(neighbor), before);
                    for (int t = 0; t < resolution; ++t) {
                        const auto ai =
                            static_cast<std::size_t>(is_x ? t * resolution + resolution - 1
                                                          : (resolution - 1) * resolution + t);
                        const auto bi = static_cast<std::size_t>(is_x ? t * resolution : t);
                        EXPECT_EQ(owner.water_depth_mm[ai], reverse ? maximum : maximum - room);
                        EXPECT_EQ(neighbor.water_depth_mm[bi], reverse ? maximum - room : maximum);
                        EXPECT_EQ(owner.water_edge_flux[2 * ai + (is_x ? 0u : 1u)],
                                  reverse ? -room : room);
                    }
                }
            }
        }
    }
}

TEST(RegionalWater, MalformedGridsAndFacesRefuseWithoutRepairOrPartialTransfer) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    for (int fault = 0; fault < 8; ++fault) {
        Chunk owner({-1, 0, -1}), neighbor({0, 0, -1});
        FillWater(owner, 8);
        FillWater(neighbor, 8);
        if (fault == 0) {
            owner.current_water_resolution.store(33);
        } else if (fault == 1) {
            owner.water_bed_mm.pop_back();
        } else if (fault == 2) {
            owner.water_depth_mm.pop_back();
        } else if (fault == 3) {
            owner.water_edge_flux.clear();
        } else if (fault == 4) {
            owner.water_depth_mm.back() = -1;
        } else if (fault == 5) {
            owner.has_water_sim.store(false);
        } else if (fault == 6) {
            neighbor.current_water_resolution.store(4);
        } else {
            neighbor.water_bed_mm.pop_back();
        }
        const auto depths = owner.water_depth_mm, flux = owner.water_edge_flux;
        if (fault < 6) {
            EXPECT_FALSE(WaterKernels::StepInterior(owner, shield, {}).applied);
        }
        EXPECT_FALSE(WaterKernels::StepSharedFace(owner, neighbor, true).applied);
        EXPECT_EQ(owner.water_depth_mm, depths);
        EXPECT_EQ(owner.water_edge_flux, flux);
        EXPECT_EQ(Mass(neighbor), 64000);
    }
    Chunk owner({0, 0, 0}), distant({2, 0, 0});
    FillWater(owner, 8);
    FillWater(distant, 8);
    EXPECT_FALSE(WaterKernels::StepSharedFace(owner, distant, true).applied);
    EXPECT_FALSE(WaterKernels::StepSharedFace(owner, owner, true).applied);
}
} // namespace

namespace {
namespace Planning = Luminumbra::Systems::RegionalWater;
using Luminumbra::world::ActiveRegionLedger;
using Luminumbra::world::RegionKey;
using Luminumbra::world::RegionSchedule;
using Luminumbra::world::RegionState;
using Luminumbra::world::WorldClock;

Planning::Resident ResidentAt(int x, int z = 0, int y = 0) {
    return {.id = Chunk::calculate_id({x, y, z})};
}

Planning::SourceInput SourceAt(std::uint64_t id, const Vec3& position, float rate = 1.0f) {
    return {.id = id, .position = position, .physical = {.flow_rate = rate}};
}

ActiveRegionLedger PlanningLedger(std::initializer_list<RegionKey> owners) {
    ActiveRegionLedger ledger;
    for (const auto owner : owners) {
        ledger.mark_edited(owner, WorldClock{});
    }
    return ledger;
}

constexpr std::uint64_t kPlanningCapacity = 2 * 64 + 4 * 8;

TEST(RegionalWaterPlanning, EveryResidentSourceAndDuePermutationProducesTheSameCanonicalPlan) {
    auto ledger = PlanningLedger({{0, 0}, {1, 0}});
    ledger.set_water_cursor({0, 0}, 1);
    std::vector<Planning::Resident> residents{
        ResidentAt(31), ResidentAt(32), ResidentAt(31, 1), ResidentAt(32, 1)};
    const std::vector<Planning::SourceInput> sources{
        SourceAt(1, {500, 0, 1}), SourceAt(2, {513, 0, 17}), SourceAt(3, {500, 0, 1}, 2.0f)};
    RegionSchedule schedule;
    schedule.due = {{1, 0}, {0, 0}};
    const Planning::Inputs inputs{.cell_budget = 10 * kPlanningCapacity};
    const auto expected = Planning::Build(residents, sources, ledger, schedule, inputs);
    ASSERT_EQ(expected.faces.size(), 4u);
    EXPECT_EQ(expected.border_windows, 4u);
    EXPECT_EQ(expected.participants.size(), 4u);
    EXPECT_EQ(expected.sources.size(), 3u);
    EXPECT_TRUE(std::is_sorted(expected.participants.begin(), expected.participants.end()));
    EXPECT_TRUE(std::is_sorted(expected.sources.begin(), expected.sources.end()));
    const auto encoded = ledger.encode();
    std::sort(residents.begin(), residents.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    do {
        auto permuted_sources = sources;
        do {
            EXPECT_EQ(Planning::Build(residents, permuted_sources, ledger, schedule, inputs),
                      expected);
            std::reverse(schedule.due.begin(), schedule.due.end());
            EXPECT_EQ(Planning::Build(residents, permuted_sources, ledger, schedule, inputs),
                      expected);
        } while (std::next_permutation(permuted_sources.begin(),
                                       permuted_sources.end(),
                                       [](const auto& a, const auto& b) { return a.id < b.id; }));
    } while (std::next_permutation(residents.begin(),
                                   residents.end(),
                                   [](const auto& a, const auto& b) { return a.id < b.id; }));
    EXPECT_EQ(ledger.encode(), encoded);
}

TEST(RegionalWaterPlanning, InteriorCadenceAndCommonSeamPhaseExcludeUnavailableOwners) {
    auto ledger = PlanningLedger({{0, 0}, {1, 0}, {2, 0}, {3, 0}});
    auto& records = const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records());
    records.at({0, 0}).state = RegionState::Reduced;
    records.at({0, 0}).cadence_shift = 2;
    records.at({0, 0}).cadence_phase = 3;
    records.at({1, 0}).state = RegionState::Reduced;
    records.at({1, 0}).cadence_shift = 1;
    records.at({1, 0}).cadence_phase = 1;
    records.at({2, 0}).state = RegionState::Frozen;
    const std::vector<Planning::Resident> residents{
        ResidentAt(31), ResidentAt(32), ResidentAt(64), ResidentAt(128)};
    RegionSchedule schedule;
    schedule.due = {{0, 0}, {1, 0}, {2, 0}, {4, 0}};
    Planning::Inputs inputs{.cell_budget = 4 * kPlanningCapacity};
    schedule.tick = 2;
    EXPECT_TRUE(Planning::Build(residents, {}, ledger, schedule, inputs).participants.empty());
    schedule.tick = 4;
    const auto common = Planning::Build(residents, {}, ledger, schedule, inputs);
    ASSERT_EQ(common.faces.size(), 1u);
    EXPECT_EQ(common.faces[0].common_shift, 2u);
    EXPECT_TRUE(common.interiors.empty());
    EXPECT_EQ(common.participants, (std::vector<ChunkID>{residents[0].id, residents[1].id}));
    schedule.tick = 3;
    const auto due = Planning::Build(residents, {}, ledger, schedule, inputs);
    EXPECT_EQ(due.interiors, (std::vector<ChunkID>{residents[0].id, residents[1].id}));
    EXPECT_TRUE(due.faces.empty());
}

TEST(RegionalWaterPlanning, PreparationAdmissionRefusesTheOwnerWithoutSpendingItsBudget) {
    auto ledger = PlanningLedger({{0, 0}, {1, 0}});
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, world::water_share::kQ32}}));
    std::vector<Planning::Resident> residents{ResidentAt(0), ResidentAt(1), ResidentAt(32)};
    residents[0].initialized = false;
    residents[0].preparation_ready = false;
    RegionSchedule schedule;
    schedule.due = {{0, 0}, {1, 0}};
    const Planning::Inputs inputs{.cell_budget = kPlanningCapacity};
    const auto refused = Planning::Build(residents, {}, ledger, schedule, inputs);
    EXPECT_EQ(refused.interiors, (std::vector<ChunkID>{residents[2].id}));
    EXPECT_EQ(refused.refused_preparation, (std::vector<RegionKey>{{0, 0}}));
    ASSERT_EQ(refused.regions.size(), 1u);
    EXPECT_EQ(refused.regions[0].owner, (RegionKey{1, 0}));
    residents[0].preparation_ready = true;
    const auto admitted = Planning::Build(residents, {}, ledger, schedule, inputs);
    EXPECT_TRUE(admitted.refused_preparation.empty());
    EXPECT_EQ(admitted.interiors, (std::vector<ChunkID>{residents[0].id}));
    EXPECT_EQ(admitted.preparation_cells, 64u);
    residents[0].initialized = true;
    residents[0].resolution = 4;
    residents[0].preparation_ready = false;
    EXPECT_EQ(Planning::Build(residents, {}, ledger, schedule, inputs), refused);
}

TEST(RegionalWaterPlanning, FixedBudgetAndSavedCursorsServeEveryOwnerAndHighestChunkId) {
    auto ledger = PlanningLedger({{0, 0}, {1, 0}, {2, 0}});
    const std::vector<Planning::Resident> residents{
        ResidentAt(0), ResidentAt(2), ResidentAt(4), ResidentAt(32), ResidentAt(64)};
    RegionSchedule schedule;
    schedule.due = {{2, 0}, {1, 0}, {0, 0}};
    std::map<ChunkID, unsigned int> visits;
    for (std::uint64_t tick = 0; tick < 60; ++tick) {
        schedule.tick = tick;
        const Planning::Inputs inputs{.cell_budget = kPlanningCapacity};
        const auto plan = Planning::Build(residents, {}, ledger, schedule, inputs);
        EXPECT_LE(plan.total_cells, inputs.cell_budget);
        EXPECT_LE(plan.allocated_windows + plan.border_windows, plan.global_windows);
        for (const auto id : plan.interiors) {
            ++visits[id];
        }
        auto debts = ledger.water_debts();
        for (const auto& region : plan.regions) {
            if (region.debt == 0) {
                debts.erase(region.owner);
            } else {
                debts.insert_or_assign(region.owner, region.debt);
            }
            ledger.set_water_cursor(region.owner, region.next_cursor);
        }
        ASSERT_TRUE(ledger.set_water_debts(debts));
    }
    for (const auto& resident : residents) {
        EXPECT_GT(visits[resident.id], 0u);
    }
    for (std::uint64_t budget = 0; budget < 8 * kPlanningCapacity; ++budget) {
        const auto plan = Planning::Build(residents, {}, ledger, schedule, {.cell_budget = budget});
        EXPECT_LE(plan.total_cells, budget);
        EXPECT_LE(plan.allocated_windows + plan.border_windows, plan.global_windows);
    }
}

TEST(RegionalWaterPlanning, SourceResolutionUsesResidentColumnsAndCanonicalQuantization) {
    auto ledger = PlanningLedger({{-1, -1}, {0, 0}});
    std::vector<Planning::Resident> residents{ResidentAt(-1, -1), ResidentAt(0, 0, 1)};
    residents[0].awake = residents[1].awake = false;
    RegionSchedule schedule;
    schedule.due = {{-1, -1}, {0, 0}};
    const std::vector<Planning::SourceInput> sources{
        SourceAt(1, {-0.5f, 32, -0.5f}), SourceAt(2, {1, 17, 1}), SourceAt(3, {1024, 0, 1})};
    const auto plan = Planning::Build(
        residents, sources, ledger, schedule, {.cell_budget = 2 * kPlanningCapacity});
    ASSERT_EQ(plan.sources.size(), 2u);
    EXPECT_EQ(plan.sources[0].target, residents[0].id);
    EXPECT_EQ(plan.sources[1].target, residents[1].id);
    EXPECT_EQ(plan.sources[0].cell, 63u);
    EXPECT_EQ(plan.sources[0].millimetres, 8);
    EXPECT_EQ(plan.interiors.size(), 2u);
    residents[1].initialized = false;
    const auto no_column = Planning::Build(
        residents, sources, ledger, schedule, {.cell_budget = 2 * kPlanningCapacity});
    EXPECT_EQ(no_column.sources.size(), 1u);
}

TEST(RegionalWaterPlanning, PriorAwakeFlagsWakeOnlyOneHopAndRainWakesSleepingResidents) {
    auto ledger = PlanningLedger({{0, 0}});
    std::vector<Planning::Resident> residents{ResidentAt(0), ResidentAt(1), ResidentAt(2)};
    residents[1].awake = residents[2].awake = false;
    RegionSchedule schedule;
    schedule.due = {{0, 0}};
    Planning::Inputs inputs{.cell_budget = 3 * kPlanningCapacity};
    const auto plan = Planning::Build(residents, {}, ledger, schedule, inputs);
    EXPECT_EQ(plan.interiors, (std::vector<ChunkID>{residents[0].id, residents[1].id}));
    inputs.raining = true;
    EXPECT_EQ(Planning::Build(residents, {}, ledger, schedule, inputs).interiors.size(), 3u);
    residents[0].available = false;
    inputs.raining = false;
    EXPECT_TRUE(Planning::Build(residents, {}, ledger, schedule, inputs).interiors.empty());
}

TEST(RegionalWaterPlanning, InvalidSourceRatesPositionsAndIdentitiesRefuseBeforeAllocation) {
    auto ledger = PlanningLedger({{0, 0}});
    std::vector<Planning::Resident> residents{ResidentAt(0)};
    RegionSchedule schedule;
    schedule.due = {{0, 0}};
    const Planning::Inputs inputs{.cell_budget = kPlanningCapacity};
    const auto before = ledger.encode();
    for (const float invalid : {-1.0f,
                                std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::max()}) {
        const std::vector sources{SourceAt(1, {10000, 0, 1}, invalid)};
        EXPECT_THROW(
            static_cast<void>(Planning::Build(residents, sources, ledger, schedule, inputs)),
            std::invalid_argument);
    }
    for (const Vec3 invalid : {Vec3(std::numeric_limits<float>::infinity()),
                               Vec3(0, std::numeric_limits<float>::quiet_NaN(), 0),
                               Vec3(16777216, 0, 0),
                               Vec3(0, 33554432, 0)}) {
        const std::vector sources{SourceAt(1, invalid)};
        EXPECT_THROW(
            static_cast<void>(Planning::Build(residents, sources, ledger, schedule, inputs)),
            std::invalid_argument);
    }
    const std::vector duplicate_sources{SourceAt(1, {0, 0, 0}), SourceAt(1, {1, 0, 1})};
    EXPECT_THROW(
        static_cast<void>(Planning::Build(residents, duplicate_sources, ledger, schedule, inputs)),
        std::invalid_argument);
    EXPECT_EQ(ledger.encode(), before);
}

TEST(RegionalWaterPlanning, InvalidCountsDuplicateResidentsAndConfigurationRefuseBeforeAllocation) {
    const auto ledger = PlanningLedger({{0, 0}});
    std::vector<Planning::Resident> residents{ResidentAt(0)};
    RegionSchedule schedule;
    schedule.due = {{0, 0}};
    const Planning::Inputs inputs{.cell_budget = kPlanningCapacity};
    const auto before = ledger.encode();
    residents.push_back(residents.front());
    EXPECT_THROW(static_cast<void>(Planning::Build(residents, {}, ledger, schedule, inputs)),
                 std::invalid_argument);
    residents.resize(1);
    schedule.due.push_back({0, 0});
    EXPECT_THROW(static_cast<void>(Planning::Build(residents, {}, ledger, schedule, inputs)),
                 std::invalid_argument);
    schedule.due.resize(1);
    EXPECT_THROW(
        static_cast<void>(Planning::Build(residents, {}, ledger, schedule, {.resolution = 3})),
        std::invalid_argument);
    residents[0].resolution = 3;
    EXPECT_THROW(static_cast<void>(Planning::Build(residents, {}, ledger, schedule, inputs)),
                 std::invalid_argument);
    residents[0].resolution = 8;
    EXPECT_THROW(
        static_cast<void>(Planning::Build(
            residents,
            {},
            ledger,
            schedule,
            {.cell_budget = (world::water_share::kMaxWindowBudget + 1) * kPlanningCapacity})),
        std::invalid_argument);
    std::vector<Planning::Resident> oversized(Planning::kMaxResidents + 1);
    EXPECT_THROW(static_cast<void>(Planning::Build(oversized, {}, ledger, schedule, inputs)),
                 std::invalid_argument);
    std::vector<Planning::SourceInput> oversized_sources(Planning::kMaxSources + 1);
    EXPECT_THROW(
        static_cast<void>(Planning::Build(residents, oversized_sources, ledger, schedule, inputs)),
        std::invalid_argument);
    schedule.due.resize(static_cast<std::size_t>(ActiveRegionLedger::kMaxRegions) + 1);
    EXPECT_THROW(static_cast<void>(Planning::Build(residents, {}, ledger, schedule, inputs)),
                 std::invalid_argument);
    EXPECT_EQ(ledger.encode(), before);
}

TEST(RegionalWaterPlanning, PlanningLeavesWaterFixturesAndAbsentOwnerRegistersUnchanged) {
    Chunk chunk({0, 0, 0});
    FillWater(chunk, 8);
    auto ledger = PlanningLedger({{0, 0}, {4, 0}});
    for (std::int32_t x = 5; x < 133; ++x) {
        ledger.mark_edited({x, 0}, WorldClock{});
    }
    ASSERT_TRUE(ledger.set_water_debts({{{4, 0}, 19}}));
    const auto encoded = ledger.encode();
    const auto water_before = Snapshot(chunk);
    const std::vector residents{ResidentAt(0)};
    RegionSchedule schedule;
    schedule.due = {{0, 0}};
    const auto plan =
        Planning::Build(residents, {}, ledger, schedule, {.cell_budget = kPlanningCapacity});
    ASSERT_EQ(plan.regions.size(), 1u);
    EXPECT_EQ(plan.regions[0].owner, (RegionKey{0, 0}));
    EXPECT_EQ(Snapshot(chunk), water_before);
    EXPECT_EQ(ledger.encode(), encoded);
    const auto empty = Planning::Build({}, {}, ledger, schedule, {});
    EXPECT_TRUE(empty.participants.empty());
    EXPECT_EQ(empty.residents_visited, 0u);
    EXPECT_EQ(plan.residents_visited, residents.size());
}

TEST(RegionalWaterPlanning, EveryResolutionSealsUnfundedOrResizingBordersAndPaysForPreparation) {
    const auto ledger = PlanningLedger({{0, 0}, {1, 0}});
    RegionSchedule schedule;
    schedule.due = {{0, 0}, {1, 0}};
    for (const int resolution : {4, 8, 16, 32}) {
        SCOPED_TRACE(resolution);
        std::vector<Planning::Resident> residents{ResidentAt(30), ResidentAt(31), ResidentAt(32)};
        for (auto& resident : residents) {
            resident.resolution = resolution;
        }
        const auto side = static_cast<std::uint64_t>(resolution);
        const auto capacity = 2 * side * side + 4 * side;
        for (std::uint64_t windows = 0; windows <= 6; ++windows) {
            const Planning::Inputs inputs{.resolution = resolution,
                                          .cell_budget = windows * capacity};
            const auto plan = Planning::Build(residents, {}, ledger, schedule, inputs);
            EXPECT_LE(plan.total_cells, inputs.cell_budget);
            EXPECT_LE(plan.border_windows + plan.allocated_windows, windows);
            EXPECT_EQ(plan.border_windows, windows >= 3 ? 2u : 0u);
        }
        residents[1].initialized = false;
        residents[1].awake = false;
        residents[0].initialized = false;
        residents[0].awake = false;
        const auto prepared =
            Planning::Build(residents,
                            {},
                            ledger,
                            schedule,
                            {.resolution = resolution, .cell_budget = 6 * capacity});
        EXPECT_EQ(prepared.border_windows, 0u);
        EXPECT_EQ(prepared.preparation_cells, 2 * side * side);
        ASSERT_EQ(prepared.faces.size(), 1u); // Selected preparations still share an interior face.
        EXPECT_EQ(prepared.faces[0].owner, prepared.faces[0].neighbor);
        EXPECT_LE(prepared.total_cells, 6 * capacity);
    }
}

TEST(RegionalWaterPlanning, EmptyClaimsAndZeroBudgetPreserveCursorsAndServiceOnlyDueDebt) {
    auto ledger = PlanningLedger({{0, 0}});
    ledger.set_water_cursor({0, 0}, std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, 2 * world::water_share::kQ32}}));
    RegionSchedule schedule;
    schedule.due = {{0, 0}};
    const std::vector residents{ResidentAt(0)};
    const auto zero = Planning::Build(residents, {}, ledger, schedule, {});
    ASSERT_EQ(zero.regions.size(), 1u);
    EXPECT_TRUE(zero.participants.empty());
    EXPECT_EQ(zero.regions[0].next_cursor, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(zero.regions[0].debt, 2 * world::water_share::kQ32);
    const auto empty =
        Planning::Build({}, {}, ledger, schedule, {.cell_budget = kPlanningCapacity});
    ASSERT_EQ(empty.regions.size(), 1u);
    EXPECT_EQ(empty.allocated_windows, 1u);
    EXPECT_TRUE(empty.participants.empty());
    EXPECT_EQ(empty.regions[0].next_cursor, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(empty.regions[0].debt, world::water_share::kQ32);
    EXPECT_TRUE(Planning::Build(residents, {}, ledger, {}, {.cell_budget = kPlanningCapacity})
                    .interiors.empty());
}

TEST(RegionalWaterPlanning, InvalidCadenceTickOwnersAndDebtOverflowRefuseWithoutMutation) {
    auto ledger = PlanningLedger({{0, 0}});
    RegionSchedule schedule;
    schedule.due = {{0, 0}};
    const std::vector residents{ResidentAt(0)};
    const Planning::Inputs inputs{.cell_budget = kPlanningCapacity};
    auto& record =
        const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records()).at({0, 0});
    record.cadence_shift = 17;
    EXPECT_THROW(static_cast<void>(Planning::Build(residents, {}, ledger, schedule, inputs)),
                 std::invalid_argument);
    record.cadence_shift = 0;
    record.cadence_phase = 1;
    EXPECT_THROW(static_cast<void>(Planning::Build(residents, {}, ledger, schedule, inputs)),
                 std::invalid_argument);
    record.cadence_phase = 0;
    record.state = static_cast<RegionState>(255);
    EXPECT_THROW(static_cast<void>(Planning::Build(residents, {}, ledger, schedule, inputs)),
                 std::invalid_argument);
    record.state = RegionState::Active;
    schedule.tick = WorldClock::kTickLimit;
    EXPECT_THROW(static_cast<void>(Planning::Build(residents, {}, ledger, schedule, inputs)),
                 std::invalid_argument);
    schedule.tick = 0;
    schedule.due = {{32768, 0}};
    EXPECT_THROW(static_cast<void>(Planning::Build({}, {}, ledger, schedule, inputs)),
                 std::invalid_argument);
    schedule.due = {{0, 0}};
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, world::water_share::kMaxDebt}}));
    const auto before = ledger.encode();
    EXPECT_THROW(static_cast<void>(Planning::Build(residents, {}, ledger, schedule, inputs)),
                 std::invalid_argument);
    EXPECT_EQ(ledger.encode(), before);
}

TEST(RegionalWaterPlanning, WorldBoundariesNeverWrapFacesAndZeroSourcesDoNotWakeResidents) {
    auto ledger = PlanningLedger({{-32768, 0}, {32767, 0}});
    const std::vector residents{ResidentAt(-1048576), ResidentAt(1048575)};
    RegionSchedule schedule;
    schedule.due = {{-32768, 0}, {32767, 0}};
    const auto plan =
        Planning::Build(residents, {}, ledger, schedule, {.cell_budget = 4 * kPlanningCapacity});
    EXPECT_TRUE(plan.faces.empty());
    EXPECT_EQ(plan.interiors.size(), 2u);
    auto sleeping = residents;
    for (auto& resident : sleeping) {
        resident.awake = false;
    }
    const std::vector sources{SourceAt(1, {-16777216, -33554432, 0}, 0.0f),
                              SourceAt(2, {16777215, 0, 0}, 0.00001f)};
    const auto zero = Planning::Build(
        sleeping, sources, ledger, schedule, {.cell_budget = 4 * kPlanningCapacity});
    EXPECT_TRUE(zero.sources.empty());
    EXPECT_TRUE(zero.participants.empty());
}

std::shared_ptr<Chunk> TransactionChunk(int x, std::int32_t depth = 1000, int resolution = 8) {
    auto chunk = std::make_shared<Chunk>(IVec3(x, 0, 0));
    FillWater(*chunk, resolution, depth);
    const auto cells = chunk->water_depth_mm.size();
    chunk->water_flow_data.assign(cells, Vec2(0.0f));
    chunk->water_sim_terrain_height.assign(cells, 1.0f);
    chunk->water_rest_level.assign(cells, 0.0f);
    chunk->water_src_mm.assign(cells, 0);
    return chunk;
}

// Preserve float bit patterns too, so a refused NaN input can be compared exactly.
std::string TransactionBytes(const Chunk& chunk, bool metadata = true) {
    std::string bytes;
    const auto append = [&bytes](const auto& values) {
        if (!values.empty()) {
            bytes.append(reinterpret_cast<const char*>(values.data()),
                         values.size() * sizeof(values.front()));
        }
    };
    append(chunk.water_depth_mm);
    append(chunk.water_bed_mm);
    append(chunk.water_edge_flux);
    append(chunk.water_level_data);
    append(chunk.water_flow_data);
    append(chunk.water_sim_terrain_height);
    append(chunk.water_rest_level);
    append(chunk.water_src_mm);
    const std::array scalars{
        static_cast<std::uint64_t>(chunk.has_water_sim.load()),
        static_cast<std::uint64_t>(chunk.current_water_resolution.load()),
        static_cast<std::uint64_t>(chunk.is_water_sleeping.load()),
        static_cast<std::uint64_t>(chunk.water_mesh_generated.load()),
        static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(chunk.max_water_delta_last_tick)),
        static_cast<std::uint64_t>(chunk.ticks_below_threshold),
        static_cast<std::uint64_t>(chunk.water_mesh_dirty_ticks)};
    append(scalars);
    if (!metadata) {
        return bytes;
    }
    const std::array revisions{static_cast<std::uint64_t>(chunk.water_revision()),
                               chunk.authority_hash_revision(),
                               static_cast<std::uint64_t>(chunk.voxel_revision()),
                               static_cast<std::uint64_t>(chunk.is_water_state_dirty()),
                               static_cast<std::uint64_t>(chunk.has_water_authority()),
                               static_cast<std::uint64_t>(chunk.is_voxel_data_dirty()),
                               static_cast<std::uint64_t>(chunk.sdf_provenance())};
    append(revisions);
    return bytes;
}

TEST(RegionalWaterTransaction, LateKernelFailureDiscardsAllPartialWorkAndNotifications) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    auto ledger = PlanningLedger({{0, 0}, {1, 0}});
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, 19}, {{1, 0}, 23}}));
    const std::vector chunks{TransactionChunk(0),
                             TransactionChunk(32, std::numeric_limits<std::int32_t>::max())};
    auto changes = std::make_shared<ChunkWaterChanges>();
    chunks[0]->bind_water_changes(changes);
    const auto before_a = TransactionBytes(*chunks[0]), before_b = TransactionBytes(*chunks[1]);
    const auto before_ledger = ledger.encode();
    const std::vector<Planning::RegionPlan> regions{
        {.owner = {0, 0}, .next_cursor = 7, .debt = 0},
        {.owner = {1, 0}, .next_cursor = 9, .debt = 29}};
    Planning::Transaction stage;
    stage.Begin(ledger, 0, chunks, regions);
    ASSERT_TRUE(stage.StepInterior(chunks[0]->get_id(), shield, {.rain_mm = 2}).applied);
    ASSERT_NE(stage.staged(chunks[0]->get_id())->water_depth_mm, chunks[0]->water_depth_mm);
    EXPECT_THROW(stage.StepInterior(chunks[1]->get_id(), shield, {.rain_mm = 1}),
                 std::invalid_argument);
    EXPECT_FALSE(stage.pending());
    EXPECT_THROW(stage.Publish(), std::invalid_argument);
    EXPECT_EQ(TransactionBytes(*chunks[0]), before_a);
    EXPECT_EQ(TransactionBytes(*chunks[1]), before_b);
    EXPECT_EQ(ledger.encode(), before_ledger);
    std::vector<ChunkID> notifications;
    changes->take(notifications);
    EXPECT_TRUE(notifications.empty());
}

TEST(RegionalWaterTransaction, PublishCommitsAllFieldsAndSparseRegistersWithConservedMass) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    auto ledger = PlanningLedger({{0, 0}, {1, 0}, {2, 0}});
    for (std::int32_t x = 3; x < 259; ++x) {
        ledger.mark_edited({x, 0}, WorldClock{});
    }
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, 19}, {{2, 0}, 31}}));
    const std::vector chunks{TransactionChunk(31, 1500), TransactionChunk(32, 500)};
    const std::vector<Planning::RegionPlan> regions{
        {.owner = {0, 0}, .next_cursor = 7}, {.owner = {1, 0}, .next_cursor = 9, .debt = 29}};
    Planning::Transaction stage;
    const auto before = Mass(*chunks[0]) + Mass(*chunks[1]);
    const auto ledger_before = ledger.encode();
    stage.Begin(ledger, 0, chunks, regions);
    EXPECT_EQ(stage.participants_visited(), chunks.size());
    ASSERT_TRUE(stage.StepInterior(chunks[0]->get_id(), shield, {}).applied);
    ASSERT_TRUE(stage.StepFace(chunks[0]->get_id(), chunks[1]->get_id(), true).applied);
    EXPECT_EQ(ledger.encode(), ledger_before);
    std::vector<std::string> expected;
    for (const auto& chunk : chunks) {
        const auto* staged = stage.staged(chunk->get_id());
        ASSERT_NE(staged, nullptr);
        expected.push_back(TransactionBytes(*staged, false));
    }
    EXPECT_EQ(Mass(*stage.staged(chunks[0]->get_id())) + Mass(*stage.staged(chunks[1]->get_id())),
              before);
    stage.Publish();
    EXPECT_FALSE(stage.pending());
    EXPECT_EQ(Mass(*chunks[0]) + Mass(*chunks[1]), before);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunk = chunks[i];
        EXPECT_EQ(chunk->water_revision(), 1u);
        EXPECT_EQ(chunk->authority_hash_revision(), 1u);
        EXPECT_TRUE(chunk->has_water_authority());
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        EXPECT_FALSE(chunk->is_water_state_dirty());
        EXPECT_EQ(TransactionBytes(*chunk, false), expected[i]);
    }
    EXPECT_EQ(ledger.records().at({0, 0}).water_cursor, 7u);
    EXPECT_EQ(ledger.records().at({1, 0}).water_cursor, 9u);
    EXPECT_FALSE(ledger.water_debts().contains({0, 0}));
    EXPECT_EQ(ledger.water_debts().at({1, 0}), 29u);
    EXPECT_EQ(ledger.water_debts().at({2, 0}), 31u);
}

TEST(RegionalWaterTransaction, RepeatedBackwardAndOverlappingStagesRefuseAndRollbackIsExplicit) {
    auto ledger = PlanningLedger({{0, 0}});
    const std::vector chunks{TransactionChunk(0)};
    Planning::Transaction stage;
    stage.Begin(ledger, 0, chunks, {});
    EXPECT_THROW(stage.Begin(ledger, 0, chunks, {}), std::invalid_argument);
    EXPECT_TRUE(stage.pending());
    stage.Publish();
    const auto before = TransactionBytes(*chunks[0]);
    EXPECT_THROW(stage.Publish(), std::invalid_argument);
    EXPECT_THROW(stage.Begin(ledger, 0, chunks, {}), std::invalid_argument);
    EXPECT_EQ(TransactionBytes(*chunks[0]), before);
    const WorldClock clock(1);
    static_cast<void>(ledger.schedule(clock));
    stage.Begin(ledger, clock.tick(), chunks, {});
    stage.Rollback();
    stage.Rollback();
    EXPECT_FALSE(stage.pending());
    EXPECT_THROW(stage.Begin(ledger, clock.tick(), chunks, {}), std::invalid_argument);
    EXPECT_THROW(stage.Begin(ledger, 0, chunks, {}), std::invalid_argument);
    EXPECT_EQ(TransactionBytes(*chunks[0]), before);
}

// Occupy the sole worker until the owner has staged and published water.
// Shared ownership and release during unwinding keep assertion exits safe.
class TransactionMeshGate {
public:
    TransactionMeshGate(JobSystem& jobs, SHIELD_WorldSystem& shield)
        : m_shield(shield) {
        const auto state = m_state;
        jobs.dispatch([state]() {
            state->entered.count_down();
            state->released.wait();
        });
        state->entered.wait();
    }
    TransactionMeshGate(const TransactionMeshGate&) = delete;
    TransactionMeshGate& operator=(const TransactionMeshGate&) = delete;
    ~TransactionMeshGate() {
        Release();
        m_shield.quiesce_streaming_jobs_for_save();
    }
    void Release() {
        if (!m_released) {
            m_released = true;
            m_state->released.count_down();
        }
    }

private:
    struct State {
        std::latch entered{1}, released{1};
    };
    SHIELD_WorldSystem& m_shield;
    std::shared_ptr<State> m_state = std::make_shared<State>();
    bool m_released = false;
};

TEST(RegionalWaterTransaction, DispatchedMeshingSnapshotSurvivesStagingAndPublish) {
    JobSystem jobs;
    jobs.startup(1);
    SHIELD_WorldSystem shield(&jobs, nullptr, TerrainGenParams{}, 1337);
    WaterSystem water(nullptr, &shield);
    shield.SetWaterSystem(&water);
    auto ledger = PlanningLedger({{0, 0}});
    const std::vector chunks{TransactionChunk(0, 999000)};
    chunks[0]->water_level_data.assign(64, 1000.0f);
    // Exact full terrain storage makes this an admitted mesh job, with a water
    // surface high enough that procedural shoreline clipping cannot affect it.
    chunks[0]->sdf_data.assign(static_cast<std::size_t>(CHUNK_SIZE_X + 1) *
                                   static_cast<std::size_t>(CHUNK_SIZE_Y + 1) *
                                   static_cast<std::size_t>(CHUNK_SIZE_Z + 1),
                               -1.0f);
    ASSERT_TRUE(shield.adopt_streamed_chunk(chunks[0]));
    entt::registry registry;
    TransactionMeshGate gate(jobs, shield);
    shield.begin_tick(0);
    shield.update(registry, Vec3(8.0f), nullptr, false);
    ASSERT_EQ(chunks[0]->pending_lod.load(), 0);
    ASSERT_FALSE(chunks[0]->pending_mesh_ready.load());
    const auto live_before = TransactionBytes(*chunks[0]);
    Planning::Transaction stage;
    stage.Begin(ledger, 0, chunks, {});
    ASSERT_TRUE(stage.StepInterior(chunks[0]->get_id(), shield, {.rain_mm = 3}).applied);
    EXPECT_EQ(TransactionBytes(*chunks[0]), live_before);
    stage.Publish();
    EXPECT_NE(TransactionBytes(*chunks[0]), live_before);
    gate.Release();
    shield.quiesce_streaming_jobs_for_save();
    EXPECT_TRUE(chunks[0]->pending_mesh_ready.load());
    EXPECT_FALSE(chunks[0]->pending_mesh_failed.load());
    ASSERT_EQ(chunks[0]->pending_water_mesh_vertices.size(), 64u * 4u);
    for (const auto& vertex : chunks[0]->pending_water_mesh_vertices) {
        EXPECT_EQ(vertex.position.y, 1000.0f);
    }
}

TEST(RegionalWaterTransaction, InvalidShapesAndNonfiniteInputsRefuseBeforeAnyWrite) {
    for (int fault = 0; fault < 16; ++fault) {
        SCOPED_TRACE(fault);
        auto ledger = PlanningLedger({{0, 0}});
        const std::vector chunks{TransactionChunk(0), TransactionChunk(1)};
        auto& chunk = *chunks[1];
        if (fault == 0) {
            chunk.water_bed_mm.pop_back();
        }
        if (fault == 1) {
            chunk.water_edge_flux.pop_back();
        }
        if (fault == 2) {
            chunk.water_depth_mm.front() = -1;
        }
        if (fault == 3) {
            chunk.water_flow_data.clear();
        }
        if (fault == 4) {
            chunk.current_water_resolution.store(3);
        }
        if (fault == 5) {
            chunk.has_water_sim.store(false);
        }
        if (fault == 6) {
            chunk.water_sim_terrain_height[0] = std::numeric_limits<float>::infinity();
        }
        if (fault == 7) {
            chunk.water_level_data[0] = std::numeric_limits<float>::quiet_NaN();
        }
        if (fault == 8) {
            chunk.water_rest_level[0] = std::numeric_limits<float>::infinity();
        }
        if (fault == 9) {
            chunk.water_flow_data[0].y = std::numeric_limits<float>::infinity();
        }
        if (fault == 10) {
            chunk.max_water_delta_last_tick = std::numeric_limits<float>::quiet_NaN();
        }
        if (fault == 11) {
            chunk.water_src_mm.pop_back();
        }
        if (fault == 12) {
            chunk.water_rest_level.pop_back();
        }
        if (fault == 13) {
            chunk.ticks_below_threshold = -1;
        }
        if (fault == 14) {
            chunk.water_mesh_dirty_ticks = 16;
        }
        if (fault == 15) {
            chunk.max_water_delta_last_tick = -1.0f;
        }
        const auto before_a = TransactionBytes(*chunks[0]), before_b = TransactionBytes(chunk);
        const auto before_ledger = ledger.encode();
        Planning::Transaction stage;
        EXPECT_THROW(stage.Begin(ledger, 0, chunks, {}), std::invalid_argument);
        EXPECT_FALSE(stage.pending());
        EXPECT_EQ(TransactionBytes(*chunks[0]), before_a);
        EXPECT_EQ(TransactionBytes(chunk), before_b);
        EXPECT_EQ(ledger.encode(), before_ledger);
    }
}

TEST(RegionalWaterTransaction, InvalidIdentitiesOwnersPatchesAndTicksRefuseUnchanged) {
    for (int fault = 0; fault < 10; ++fault) {
        SCOPED_TRACE(fault);
        auto ledger = PlanningLedger({{0, 0}});
        auto chunk = TransactionChunk(0);
        std::vector chunks{chunk};
        std::vector<Planning::RegionPlan> regions{{.owner = {0, 0}, .debt = 1}};
        std::uint64_t tick = 0;
        if (fault == 0) {
            chunks.push_back(chunk);
        }
        if (fault == 1) {
            chunks.push_back(nullptr);
        }
        if (fault == 2) {
            chunks.push_back(TransactionChunk(32));
        }
        if (fault == 3) {
            regions[0].owner = {1, 0};
        }
        if (fault == 4) {
            regions.push_back(regions.front());
        }
        if (fault == 5) {
            regions[0].debt = ActiveRegionLedger::kMaxWaterDebt + 1;
        }
        if (fault == 6) {
            regions[0].cadence_shift = 1;
        }
        if (fault == 7) {
            tick = 1;
        }
        if (fault == 8) {
            tick = WorldClock::kTickLimit;
        }
        if (fault == 9) {
            chunks.push_back(TransactionChunk(Chunk::kPackedMaxXz + 1));
        }
        const auto before = TransactionBytes(*chunk), before_ledger = ledger.encode();
        Planning::Transaction stage;
        EXPECT_THROW(stage.Begin(ledger, tick, chunks, regions), std::invalid_argument);
        EXPECT_FALSE(stage.pending());
        EXPECT_EQ(TransactionBytes(*chunk), before);
        EXPECT_EQ(ledger.encode(), before_ledger);
    }
}

TEST(RegionalWaterTransaction, EmptyStagesUnchangedPublicationAndExplicitRollbackAreSafe) {
    auto ledger = PlanningLedger({{0, 0}});
    Planning::Transaction empty;
    empty.Begin(ledger, 0, {}, {});
    EXPECT_EQ(empty.participants_visited(), 0u);
    EXPECT_EQ(empty.staged(0), nullptr);
    empty.Publish();
    const std::vector chunks{TransactionChunk(0)};
    const auto before = TransactionBytes(*chunks[0]);
    Planning::Transaction unchanged;
    unchanged.Begin(ledger, 0, chunks, {});
    unchanged.Publish();
    EXPECT_EQ(TransactionBytes(*chunks[0]), before);
    Planning::Transaction cancelled;
    cancelled.Begin(ledger, 0, chunks, {});
    cancelled.Rollback();
    EXPECT_EQ(TransactionBytes(*chunks[0]), before);
}

TEST(RegionalWaterTransaction, CountCapsRefuseBeforeVisitingOrCopyingParticipants) {
    auto ledger = PlanningLedger({{0, 0}});
    Planning::Transaction stage;
    const std::vector<std::shared_ptr<Chunk>> oversized(Planning::kMaxResidents + 1);
    EXPECT_THROW(stage.Begin(ledger, 0, oversized, {}), std::invalid_argument);
    EXPECT_EQ(stage.participants_visited(), 0u);
    const std::vector<Planning::RegionPlan> patches(
        static_cast<std::size_t>(ActiveRegionLedger::kMaxRegions) + 1);
    EXPECT_THROW(stage.Begin(ledger, 0, {}, patches), std::invalid_argument);
    EXPECT_EQ(stage.participants_visited(), 0u);
    EXPECT_FALSE(stage.pending());
}

TEST(RegionalWaterTransaction, SparseDebtPreparationRefusesTheFileCapBeforeAnyWrite) {
    ActiveRegionLedger ledger;
    for (std::uint32_t i = 0; i < ActiveRegionLedger::kMaxRegions; ++i) {
        ledger.mark_edited(
            {static_cast<std::int32_t>(i % 1024u), static_cast<std::int32_t>(i / 1024u)},
            WorldClock{});
    }
    const std::vector<Planning::RegionPlan> patches{{.owner = {0, 0}, .next_cursor = 7, .debt = 1}};
    Planning::Transaction stage;
    EXPECT_THROW(stage.Begin(ledger, 0, {}, patches), std::invalid_argument);
    EXPECT_FALSE(stage.pending());
    EXPECT_EQ(ledger.records().at({0, 0}).water_cursor, 0u);
    EXPECT_TRUE(ledger.water_debts().empty());
}

TEST(RegionalWaterTransaction, EmptyGridsCanStageButCannotExecuteAndFrozenOwnersRefuse) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    auto ledger = PlanningLedger({{0, 0}});
    const std::vector chunks{std::make_shared<Chunk>(IVec3(0))};
    const auto before = TransactionBytes(*chunks[0]);
    Planning::Transaction stage;
    stage.Begin(ledger, 0, chunks, {});
    EXPECT_THROW(stage.StepInterior(chunks[0]->get_id(), shield, {}), std::invalid_argument);
    EXPECT_EQ(TransactionBytes(*chunks[0]), before);
    auto& record =
        const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records()).at({0, 0});
    record.state = RegionState::Frozen;
    Planning::Transaction frozen;
    EXPECT_THROW(frozen.Begin(ledger, 0, chunks, {}), std::invalid_argument);
    const std::vector<Planning::RegionPlan> regions{{.owner = {0, 0}, .debt = 1}};
    EXPECT_THROW(frozen.Begin(ledger, 0, {}, regions), std::invalid_argument);
    EXPECT_EQ(TransactionBytes(*chunks[0]), before);
}

TEST(RegionalWaterTransaction, NegativeKernelInputsAbort) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    const std::array<WaterKernels::InteriorInputs, 3> inputs{
        WaterKernels::InteriorInputs{.rain_mm = -1},
        WaterKernels::InteriorInputs{.evap_mm = -1},
        WaterKernels::InteriorInputs{.weather_scale_mm = -1}};
    for (const auto& input : inputs) {
        auto ledger = PlanningLedger({{0, 0}});
        const std::vector chunks{TransactionChunk(0), TransactionChunk(1)};
        const auto before = TransactionBytes(*chunks[0]);
        Planning::Transaction stage;
        stage.Begin(ledger, 0, chunks, {});
        EXPECT_THROW(stage.StepInterior(chunks[0]->get_id(), shield, input), std::invalid_argument);
        EXPECT_FALSE(stage.pending());
        EXPECT_EQ(TransactionBytes(*chunks[0]), before);
    }
}

TEST(RegionalWaterTransaction, UnknownChunksAndReversedFacesAbort) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    for (const bool interior : {false, true}) {
        auto ledger = PlanningLedger({{0, 0}});
        const std::vector chunks{TransactionChunk(0), TransactionChunk(1)};
        const auto before = TransactionBytes(*chunks[0]);
        Planning::Transaction stage;
        stage.Begin(ledger, 0, chunks, {});
        if (interior) {
            EXPECT_THROW(stage.StepInterior(Chunk::calculate_id({2, 0, 0}), shield, {}),
                         std::invalid_argument);
        } else {
            EXPECT_THROW(stage.StepFace(chunks[1]->get_id(), chunks[0]->get_id(), true),
                         std::invalid_argument);
        }
        EXPECT_FALSE(stage.pending());
        EXPECT_EQ(TransactionBytes(*chunks[0]), before);
    }
}

TEST(RegionalWaterTransaction, ChangedLedgerTickRefusesPublicationAfterStaging) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    auto ledger = PlanningLedger({{0, 0}});
    const std::vector chunks{TransactionChunk(0)};
    const auto before = TransactionBytes(*chunks[0]);
    Planning::Transaction stage;
    stage.Begin(ledger, 0, chunks, {});
    ASSERT_TRUE(stage.StepInterior(chunks[0]->get_id(), shield, {.rain_mm = 1}).applied);
    static_cast<void>(ledger.schedule(WorldClock(1)));
    const auto scheduled = ledger.encode();
    EXPECT_THROW(stage.Publish(), std::invalid_argument);
    EXPECT_FALSE(stage.pending());
    EXPECT_EQ(ledger.encode(), scheduled);
    EXPECT_EQ(TransactionBytes(*chunks[0]), before);
}

TEST(RegionalWaterTransaction, EveryResolutionAccountsRainEvaporationAndConservesPublication) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    for (const int resolution : {4, 8, 16, 32}) {
        SCOPED_TRACE(resolution);
        auto ledger = PlanningLedger({{0, 0}});
        const std::vector chunks{TransactionChunk(0, 1000, resolution)};
        const auto before = Mass(*chunks[0]);
        Planning::Transaction stage;
        stage.Begin(ledger, 0, chunks, {});
        const auto result =
            stage.StepInterior(chunks[0]->get_id(), shield, {.rain_mm = 3, .evap_mm = 2});
        ASSERT_TRUE(result.applied);
        EXPECT_GT(result.source_mm, 0);
        EXPECT_GT(result.sink_mm, 0);
        const auto expected = before + result.source_mm - result.sink_mm;
        EXPECT_EQ(Mass(*stage.staged(chunks[0]->get_id())), expected);
        EXPECT_EQ(Mass(*chunks[0]), before);
        stage.Publish();
        EXPECT_EQ(Mass(*chunks[0]), expected);
    }
}

TEST(RegionalWaterTransaction, ExistingRevisionsAdvanceOnceAndStaleStagesRefuse) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    for (const bool stale : {false, true}) {
        auto ledger = PlanningLedger({{0, 0}});
        const std::vector chunks{TransactionChunk(0)};
        chunks[0]->mark_water_state_dirty();
        chunks[0]->mark_water_data_dirty();
        Planning::Transaction stage;
        stage.Begin(ledger, 0, chunks, {});
        ASSERT_TRUE(stage.StepInterior(chunks[0]->get_id(), shield, {.rain_mm = 2}).applied);
        EXPECT_EQ(chunks[0]->water_revision(), 1u);
        if (stale) {
            chunks[0]->mark_water_state_dirty();
            const auto before = TransactionBytes(*chunks[0]);
            EXPECT_THROW(stage.Publish(), std::invalid_argument);
            EXPECT_EQ(TransactionBytes(*chunks[0]), before);
        } else {
            stage.Publish();
            EXPECT_EQ(chunks[0]->water_revision(), 2u);
            EXPECT_EQ(chunks[0]->authority_hash_revision(), 2u);
        }
    }
}
} // namespace
