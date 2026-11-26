#include <gtest/gtest.h>

#include "luminumbra_common/systems/RegionalWater.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace Luminumbra;
namespace R = Systems::RegionalWater;
namespace W = world::water_share;
using world::ActiveRegionLedger;
using world::RegionKey;
using world::RegionSchedule;
using world::RegionState;
using world::WorldClock;

R::Resident ResidentAt(int x, int resolution) {
    return {.id = Chunk::calculate_id({x, 0, 0}), .resolution = resolution};
}

ActiveRegionLedger Ledger(int owners) {
    ActiveRegionLedger ledger;
    for (int x = 0; x < owners; ++x) {
        ledger.mark_edited({x, 0}, WorldClock{});
        ledger.set_water_cursor({x, 0}, 17);
    }
    return ledger;
}

// Configure persisted cadence as neighbouring ledger/planner fixtures do. No
// production test seam is needed; these records belong to this mutable fixture.
void SetState(ActiveRegionLedger& ledger, RegionKey owner, RegionState state) {
    auto& records = const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records());
    auto& record = records.at(owner);
    record.state = state;
    record.cadence_shift = state == RegionState::Active ? 0 : 1;
    record.cadence_phase = ActiveRegionLedger::phase(owner, record.cadence_shift);
    if (state == RegionState::Frozen) {
        ASSERT_GT(ledger.tick(), 0u);
        record.frozen_at = ledger.tick();
        record.last_ticked = ledger.tick() - 1;
    }
}

struct MatrixCase {
    const char* name;
    bool preparation = false;
    bool resize = false;
    bool ready = true;
    bool sleeping = false;
    bool all_sleeping = false;
    RegionState boundary_state = RegionState::Active;
    bool boundary_due = true;
};

constexpr std::array kCases{
    MatrixCase{.name = "awake"},
    MatrixCase{.name = "initialize", .preparation = true},
    MatrixCase{.name = "resize", .preparation = true, .resize = true},
    MatrixCase{.name = "unready", .preparation = true, .ready = false},
    MatrixCase{.name = "sleeping endpoint", .sleeping = true},
    MatrixCase{.name = "all sleeping", .sleeping = true, .all_sleeping = true},
    MatrixCase{.name = "frozen endpoint", .boundary_state = RegionState::Frozen},
    MatrixCase{.name = "off phase", .boundary_state = RegionState::Reduced},
    MatrixCase{.name = "not due", .boundary_due = false},
};

void ExpectAccounting(const R::Plan& plan,
                      const std::vector<R::Resident>& residents,
                      int resolution,
                      std::uint64_t budget) {
    const auto width = static_cast<std::uint64_t>(resolution);
    const auto cells = width * width;
    const auto window = 2 * cells + 4 * width;
    // Count each actual operation from identities, independently of
    // the planner's aggregate counters and window cost formula.
    std::uint64_t prepared = 0, worked = 0, border_endpoints = 0;
    for (const auto id : plan.interiors) {
        const auto found = std::find_if(
            residents.begin(), residents.end(), [id](const auto& item) { return item.id == id; });
        ASSERT_NE(found, residents.end());
        if (!found->initialized || found->resolution != resolution) {
            prepared += cells;
        }
        worked += cells;
    }
    for (const auto& face : plan.faces) {
        for (const auto id : {face.positive, face.other}) {
            EXPECT_TRUE(std::binary_search(plan.participants.begin(), plan.participants.end(), id));
            worked += width;
            border_endpoints += face.owner != face.neighbor ? 1u : 0u;
        }
    }
    EXPECT_EQ(plan.global_windows, budget / window);
    EXPECT_EQ(plan.preparation_cells, prepared);
    EXPECT_EQ(plan.total_cells, prepared + worked);
    EXPECT_LE(prepared + worked, budget);
    EXPECT_EQ(plan.border_windows, border_endpoints);
    EXPECT_EQ(plan.allocated_windows, plan.interiors.size());
    EXPECT_LE(plan.border_windows + plan.allocated_windows, plan.global_windows);
    EXPECT_EQ(plan.residents_visited, residents.size());
}

TEST(RegionalWaterCadence, GlobalCapIncludesPreparationAndBothSeamEndpoints) {
    // Bounded matrix: 3 resolutions * 9 states * 8 budgets, with five residents.
    for (const int resolution : {8, 16, 32}) {
        const auto width = static_cast<std::uint64_t>(resolution);
        const auto cells = width * width;
        const auto window = 2 * cells + 4 * width;
        for (const auto& scenario : kCases) {
            SCOPED_TRACE(scenario.name);
            SCOPED_TRACE(resolution);
            auto ledger = Ledger(4);
            static_cast<void>(ledger.schedule(WorldClock(1)));
            SetState(ledger, {1, 0}, scenario.boundary_state);
            SetState(ledger, {3, 0}, RegionState::Frozen);
            ASSERT_TRUE(ledger.set_water_debts(
                {{{0, 0}, 8 * W::kQ32}, {{1, 0}, 8 * W::kQ32}, {{2, 0}, 19}, {{3, 0}, 23}}));
            RegionSchedule schedule;
            schedule.tick = ledger.tick();
            schedule.due = {{0, 0}, {2, 0}, {3, 0}};
            if (scenario.boundary_due) {
                schedule.due.push_back({1, 0});
            }
            if (scenario.boundary_state == RegionState::Reduced) {
                schedule.tick = ActiveRegionLedger::phase({1, 0}, 1) ^ 1u;
            }
            std::vector residents{ResidentAt(30, resolution),
                                  ResidentAt(31, resolution),
                                  ResidentAt(32, resolution),
                                  ResidentAt(64, resolution),
                                  ResidentAt(96, resolution)};
            residents[0].initialized = !scenario.preparation || scenario.resize;
            residents[0].resolution = scenario.resize ? (resolution == 8 ? 16 : 8) : resolution;
            residents[0].preparation_ready = scenario.ready;
            residents[2].awake = !scenario.sleeping;
            residents[3].awake = false; // Isolated sleeping owner retains its registers.
            if (scenario.all_sleeping) {
                residents[0].awake = false;
                residents[1].awake = false;
            }
            const auto before_records = ledger.records();
            const auto before_debts = ledger.water_debts();
            const std::array budgets{std::uint64_t{0},
                                     window - 1,
                                     window,
                                     2 * window - 1,
                                     2 * window,
                                     3 * window - 1,
                                     3 * window,
                                     8 * window + window - 1};
            for (const auto budget : budgets) {
                SCOPED_TRACE(budget);
                const auto plan =
                    R::Build(residents, {}, ledger, schedule, {resolution, budget, false});
                ExpectAccounting(plan, residents, resolution, budget);
                EXPECT_FALSE(std::binary_search(
                    plan.participants.begin(), plan.participants.end(), residents[3].id));
                EXPECT_FALSE(std::binary_search(
                    plan.participants.begin(), plan.participants.end(), residents[4].id));
                const bool sealed = scenario.boundary_state != RegionState::Active ||
                                    !scenario.boundary_due || !scenario.ready ||
                                    scenario.all_sleeping;
                if (sealed) {
                    EXPECT_EQ(plan.border_windows, 0u);
                }
                if (!scenario.ready) {
                    EXPECT_EQ(plan.refused_preparation, (std::vector<RegionKey>{{0, 0}}));
                }
                if (budget == budgets.back()) {
                    EXPECT_EQ(plan.border_windows, sealed ? 0u : 2u);
                    EXPECT_EQ(plan.preparation_cells,
                              scenario.preparation && scenario.ready ? cells : 0u);
                    const auto same_owner =
                        std::count_if(plan.faces.begin(), plan.faces.end(), [](const auto& face) {
                            return face.owner == face.neighbor;
                        });
                    EXPECT_EQ(same_owner, scenario.ready && !scenario.all_sleeping ? 1 : 0);
                }
                EXPECT_EQ(ledger.records(), before_records);
                EXPECT_EQ(ledger.water_debts(), before_debts);
            }
        }
    }
}

std::shared_ptr<Chunk> WaterChunk(int x, int resolution, std::int32_t depth) {
    auto chunk = std::make_shared<Chunk>(IVec3{x, 0, 0});
    chunk->has_water_sim.store(true);
    chunk->current_water_resolution.store(resolution);
    const auto cells = static_cast<std::size_t>(resolution * resolution);
    chunk->water_depth_mm.assign(cells, depth);
    chunk->water_bed_mm.assign(cells, 1000);
    chunk->water_edge_flux.assign(2 * cells, 0);
    chunk->water_level_data.assign(cells, 1.0f + static_cast<float>(depth) / 1000.0f);
    chunk->water_flow_data.assign(cells, Vec2(0.0f));
    chunk->water_sim_terrain_height.assign(cells, 1.0f);
    chunk->water_rest_level.assign(cells, 0.0f);
    chunk->water_src_mm.assign(cells, 0);
    return chunk;
}

std::int64_t Volume(const Chunk& chunk) {
    const auto depth =
        std::accumulate(chunk.water_depth_mm.begin(), chunk.water_depth_mm.end(), std::int64_t{0});
    const auto resolution = chunk.current_water_resolution.load();
    return depth * (4 * CHUNK_SIZE_X * CHUNK_SIZE_Z / (resolution * resolution));
}

auto Snapshot(const Chunk& chunk) {
    return std::make_tuple(chunk.water_depth_mm,
                           chunk.water_bed_mm,
                           chunk.water_edge_flux,
                           chunk.water_level_data,
                           chunk.water_flow_data,
                           chunk.water_sim_terrain_height,
                           chunk.water_rest_level,
                           chunk.water_src_mm,
                           chunk.is_water_sleeping.load(),
                           chunk.ticks_below_threshold,
                           chunk.water_mesh_generated.load(),
                           chunk.water_mesh_dirty_ticks,
                           chunk.max_water_delta_last_tick,
                           chunk.water_revision(),
                           chunk.authority_hash_revision(),
                           chunk.is_water_state_dirty(),
                           chunk.is_voxel_data_dirty(),
                           chunk.has_water_authority(),
                           chunk.has_water_sim.load(),
                           chunk.current_water_resolution.load());
}

TEST(RegionalWaterCadence, SealedOwnersConserveVolumeAndRegisters) {
    Systems::SHIELD_WorldSystem shield(nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
    // Three sealing reasons, three saved resolutions, four chunks per transaction.
    for (const int resolution : {8, 16, 32}) {
        for (const int reason : {0, 1, 2}) {
            SCOPED_TRACE(resolution);
            SCOPED_TRACE(reason);
            auto ledger = Ledger(3);
            static_cast<void>(ledger.schedule(WorldClock(1)));
            if (reason != 0) {
                SetState(ledger, {1, 0}, reason == 1 ? RegionState::Frozen : RegionState::Reduced);
            }
            ASSERT_TRUE(ledger.set_water_debts({{{1, 0}, 23}, {{2, 0}, 29}}));
            RegionSchedule schedule;
            schedule.tick = ledger.tick();
            schedule.due = {{0, 0}, {2, 0}};
            if (reason != 0) {
                schedule.due.push_back({1, 0});
            }
            Systems::WaterSystem water(nullptr, &shield);
            water.SetRegionalScheduling(true);
            // Sessions reject 32 on 16-metre chunks. Retain saved 32-cell grids
            // for due migration and sealing, but keep the sleeping control at
            // the session resolution so it needs no funded preparation.
            const int session_resolution = std::min(resolution, CHUNK_SIZE_X);
            water.SetSimResolution(session_resolution);
            ASSERT_EQ(water.sim_resolution(), session_resolution);
            std::vector chunks{WaterChunk(30, resolution, 1500),
                               WaterChunk(31, resolution, 500),
                               WaterChunk(32, resolution, 2000),
                               WaterChunk(64, session_resolution, 1000)};
            chunks[3]->is_water_sleeping.store(true);
            const auto sealed_before = Snapshot(*chunks[2]);
            const auto sleeping_before = Snapshot(*chunks[3]);
            const auto records_before = ledger.records();
            const auto volume_before = Volume(*chunks[0]) + Volume(*chunks[1]);
            entt::registry registry;
            std::vector<world::RegionWork> completed;
            const auto width = static_cast<std::uint64_t>(session_resolution);
            const auto cells = width * width;
            const auto budget = 2 * (2 * cells + 4 * width);
            water.UpdateRegional(registry, chunks, ledger, schedule, completed, budget);
            EXPECT_EQ(Volume(*chunks[0]) + Volume(*chunks[1]), volume_before);
            EXPECT_NE(chunks[0]->water_edge_flux,
                      std::vector<std::int32_t>(static_cast<std::size_t>(2 * cells), 0));
            EXPECT_EQ(chunks[0]->current_water_resolution.load(), session_resolution);
            EXPECT_EQ(chunks[1]->current_water_resolution.load(), session_resolution);
            EXPECT_EQ(Snapshot(*chunks[2]), sealed_before);
            EXPECT_EQ(Snapshot(*chunks[3]), sleeping_before);
            for (const RegionKey owner : {RegionKey{1, 0}, RegionKey{2, 0}}) {
                EXPECT_EQ(ledger.records().at(owner), records_before.at(owner));
            }
            EXPECT_EQ(ledger.water_debts().at({1, 0}), 23u);
            EXPECT_EQ(ledger.water_debts().at({2, 0}), 29u);
            ASSERT_EQ(completed.size(), 1u);
            EXPECT_EQ(completed.front().key, (RegionKey{0, 0}));
            EXPECT_EQ(completed.front().water_cells, 2 * cells + 2 * width);
            EXPECT_EQ(water.regional_work().preparation_cells,
                      resolution == session_resolution ? 0u : 2 * cells);
            EXPECT_EQ(water.regional_work().visits,
                      (std::vector<ChunkID>{chunks[0]->get_id(), chunks[1]->get_id()}));
            EXPECT_EQ(water.regional_work().border_windows, 0u);
            EXPECT_EQ(water.regional_work().source_mm, 0);
            EXPECT_EQ(water.regional_work().sink_mm, 0);
            EXPECT_EQ(water.regional_work().volume_before_qmm,
                      water.regional_work().volume_after_qmm);
        }
    }
}

// Small independent oracle: all numerators fit u64 (W<=9, demand<=3).
// Select each original post-credit rank once, with bulk service before the next
// owner. No production allocator or binary-limb arithmetic is reused.
std::array<std::uint64_t, 3> Award(std::uint64_t windows, std::array<std::uint64_t, 3>& debt) {
    constexpr std::array<std::uint64_t, 3> demand{3, 1, 2};
    windows = std::min(windows, std::uint64_t{6});
    std::array<std::uint64_t, 3> remainder{}, grants{};
    std::uint64_t credited = 0;
    for (std::size_t i = 0; i < demand.size(); ++i) {
        const auto numerator = windows * demand[i] * W::kQ32;
        debt[i] += numerator / 6;
        credited += numerator / 6;
        remainder[i] = numerator % 6;
    }
    for (; credited < windows * W::kQ32; ++credited) {
        const auto best = std::max_element(remainder.begin(), remainder.end());
        ++debt[static_cast<std::size_t>(best - remainder.begin())];
        *best = 0;
    }
    auto ranks = debt;
    for (std::size_t rank = 0; rank < demand.size(); ++rank) {
        const auto best = std::max_element(ranks.begin(), ranks.end());
        const auto index = static_cast<std::size_t>(best - ranks.begin());
        *best = 0;
        grants[index] = std::min(debt[index] / W::kQ32, windows);
        debt[index] -= grants[index] * W::kQ32;
        windows -= grants[index];
    }
    return grants;
}

TEST(RegionalWaterCadence, ApprovedRankingSurvivesPermutationAndReload) {
    {
        auto tied = Ledger(3);
        ASSERT_TRUE(tied.set_water_debts(
            {{{0, 0}, 3 * W::kQ32}, {{1, 0}, 3 * W::kQ32}, {{2, 0}, 3 * W::kQ32}}));
        const std::array equal_demand{ResidentAt(0, 8), ResidentAt(32, 8), ResidentAt(64, 8)};
        RegionSchedule schedule;
        schedule.due = {{2, 0}, {1, 0}, {0, 0}};
        ActiveRegionLedger restored;
        std::string error;
        ASSERT_TRUE(ActiveRegionLedger::decode(tied.encode(), restored, error)) << error;
        const auto plan = R::Build(equal_demand, {}, restored, schedule, {8, 3 * 160, false});
        // Equal post-credit debt uses key order even when scheduler order is
        // reversed. Bulk grant truncation retains the two unselected windows.
        EXPECT_EQ(plan.interiors, (std::vector<ChunkID>{equal_demand[0].id}));
        ASSERT_EQ(plan.regions.size(), 3u);
        EXPECT_EQ(plan.regions[0].debt, 3 * W::kQ32);
        EXPECT_EQ(plan.regions[1].debt, 4 * W::kQ32);
        EXPECT_EQ(plan.regions[2].debt, 4 * W::kQ32);
        EXPECT_EQ(R::Build(equal_demand, {}, tied, schedule, {8, 3 * 160, false}), plan);
    }
    auto ledger = Ledger(3);
    ledger.set_water_cursor({0, 0}, std::numeric_limits<std::uint64_t>::max());
    std::array<std::uint64_t, 3> debt{W::kQ32 + 7, 3 * W::kQ32 + 9, 3 * W::kQ32 + 9};
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, debt[0]}, {{1, 0}, debt[1]}, {{2, 0}, debt[2]}}));
    const std::array<std::vector<R::Resident>, 3> candidates{
        std::vector{ResidentAt(0, 8), ResidentAt(2, 8), ResidentAt(4, 8)},
        std::vector{ResidentAt(32, 8)},
        std::vector{ResidentAt(64, 8), ResidentAt(66, 8)}};
    std::vector<R::Resident> residents;
    for (const auto& group : candidates) {
        residents.insert(residents.end(), group.begin(), group.end());
    }
    std::array<std::uint64_t, 3> cursor{std::numeric_limits<std::uint64_t>::max(), 17, 17};
    constexpr std::uint64_t window = 2 * 64 + 4 * 8;
    // Eight publications, each compared with all six owner orders and both
    // resident/source orders. Saved fractional and excess debt carry forward.
    constexpr std::array<std::uint64_t, 8> budgets{1, 2, 0, 3, 5, 9, 1, 2};
    for (std::size_t tick = 0; tick < budgets.size(); ++tick) {
        SCOPED_TRACE(tick);
        if (tick != 0) {
            static_cast<void>(ledger.schedule(WorldClock(tick)));
        }
        ActiveRegionLedger restored;
        std::string error;
        ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), restored, error)) << error;
        EXPECT_EQ(restored.encode(), ledger.encode());
        RegionSchedule schedule;
        schedule.tick = tick;
        schedule.due = {{0, 0}, {1, 0}, {2, 0}};
        std::vector<R::SourceInput> sources{{.id = 7, .position = Vec3{1.0f, 0.0f, 1.0f}},
                                            {.id = 9, .position = Vec3{513.0f, 0.0f, 1.0f}}};
        const R::Inputs inputs{8, budgets[tick] * window + window - 1, false};
        const auto before = ledger.encode();
        const auto plan = R::Build(residents, sources, ledger, schedule, inputs);
        const auto grants = Award(budgets[tick], debt);
        ASSERT_EQ(plan.regions.size(), 3u);
        std::vector<ChunkID> expected;
        for (std::size_t i = 0; i < grants.size(); ++i) {
            const auto count = static_cast<std::uint64_t>(candidates[i].size());
            const auto used = std::min(grants[i], count);
            debt[i] += (grants[i] - used) * W::kQ32;
            const auto start = cursor[i] % count;
            for (std::uint64_t selected = 0; selected < used; ++selected) {
                expected.push_back(
                    candidates[i][static_cast<std::size_t>((start + selected) % count)].id);
            }
            if (used != 0) {
                cursor[i] = (start + used) % count;
            }
            EXPECT_EQ(plan.regions[i].windows, used);
            EXPECT_EQ(plan.regions[i].debt, debt[i]);
            EXPECT_EQ(plan.regions[i].next_cursor, cursor[i]);
        }
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(plan.interiors, expected);
        EXPECT_TRUE(plan.faces.empty());
        do {
            for (int order = 0; order < 2; ++order) {
                EXPECT_EQ(R::Build(residents, sources, restored, schedule, inputs), plan);
                std::reverse(residents.begin(), residents.end());
                std::reverse(sources.begin(), sources.end());
            }
        } while (std::next_permutation(schedule.due.begin(), schedule.due.end()));
        EXPECT_EQ(ledger.encode(), before);
        EXPECT_EQ(restored.encode(), before);
        std::vector<ActiveRegionLedger::WaterPatch> patches;
        for (const auto& region : plan.regions) {
            patches.push_back({region.owner, region.debt, region.next_cursor});
        }
        ActiveRegionLedger::WaterDebts scratch;
        ASSERT_TRUE(ledger.prepare_water_patch(patches, scratch));
        ledger.commit_water_patch(patches, scratch);
    }
}
} // namespace
