#include <gtest/gtest.h>

#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/IrrigationComponents.h"
#include "luminumbra_common/core/WaterComponents.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {
using namespace Luminumbra;
using world::ActiveRegionLedger;
using world::RegionKey;
using world::RegionSchedule;
using world::RegionState;
using world::WorldClock;
constexpr std::uint64_t kCapacity = 2 * 64 + 4 * 8;

std::shared_ptr<Chunk> WaterChunk(const IVec3& coords, std::int32_t depth = 1000, int res = 8) {
    auto chunk = std::make_shared<Chunk>(coords);
    chunk->set_state(ChunkState::Idle);
    chunk->has_water_sim.store(true);
    chunk->current_water_resolution.store(res);
    const auto cells = static_cast<std::size_t>(res * res);
    chunk->water_depth_mm.assign(cells, depth);
    chunk->water_bed_mm.assign(cells, 1000);
    chunk->water_edge_flux.assign(2 * cells, 0);
    chunk->water_level_data.assign(cells, static_cast<float>(depth) / 1000.0f + 1.0f);
    chunk->water_flow_data.assign(cells, Vec2(0));
    chunk->water_sim_terrain_height.assign(cells, 1.0f);
    return chunk;
}

struct WaterState {
    std::vector<std::int32_t> depth, bed, flux, source;
    std::vector<float> level, terrain, rest;
    std::vector<Vec2> flow;
    bool initialized, sleeping, mesh, dirty, authority;
    int resolution, calm, mesh_ticks;
    float delta;
    std::uint32_t revision, voxel_revision;
    ChunkSdfProvenance provenance;
    bool operator==(const WaterState&) const = default;
};
WaterState CaptureWater(const Chunk& c) {
    return {c.water_depth_mm,
            c.water_bed_mm,
            c.water_edge_flux,
            c.water_src_mm,
            c.water_level_data,
            c.water_sim_terrain_height,
            c.water_rest_level,
            c.water_flow_data,
            c.has_water_sim.load(),
            c.is_water_sleeping.load(),
            c.water_mesh_generated.load(),
            c.is_water_state_dirty(),
            c.has_water_authority(),
            c.current_water_resolution.load(),
            c.ticks_below_threshold,
            c.water_mesh_dirty_ticks,
            c.max_water_delta_last_tick,
            c.water_revision(),
            c.voxel_revision(),
            c.sdf_provenance()};
}

class RegionalWaterFixture : public testing::Test {
protected:
    RegionalWaterFixture()
        : shield(nullptr, nullptr, Systems::TerrainGenParams{}, 1337)
        , water(nullptr, &shield) {
        water.SetHydrology(true, 0, 0);
    }
    void Owner(RegionKey key) {
        ledger.mark_edited(key, WorldClock{});
    }
    void Step(std::uint64_t windows, const std::vector<RegionKey>& due) {
        RegionSchedule schedule;
        schedule.tick = ledger.tick();
        schedule.due = due;
        water.UpdateRegional(registry, chunks, ledger, schedule, work, windows * kCapacity);
    }
    void Source(const Vec3& position, float rate = 1.0f) {
        const auto entity = registry.create();
        registry.emplace<Components::TransformComponent>(entity).position = position;
        registry.emplace<Components::WaterFlowSourceComponent>(entity).flow_rate = rate;
    }
    Systems::SHIELD_WorldSystem shield;
    Systems::WaterSystem water;
    ActiveRegionLedger ledger;
    entt::registry registry;
    std::vector<std::shared_ptr<Chunk>> chunks;
    std::vector<world::RegionWork> work;
};

TEST_F(RegionalWaterFixture, UninitializedAndOldResolutionClaimsEarnPreparationAndOneKernel) {
    Owner({0, 0});
    auto fresh = std::make_shared<Chunk>(IVec3(0, 0, 0));
    fresh->set_state(ChunkState::Idle);
    chunks = {fresh, WaterChunk({1, 0, 0}, 17, 4)};
    water.SetHydrology(true, 1, 0);
    Step(2, {{0, 0}});
    const auto& result = water.regional_work();
    EXPECT_EQ(result.used_windows, 2u);
    EXPECT_EQ(result.visits.size(), 2u);
    EXPECT_EQ(result.preparation_cells, 128u);
    EXPECT_EQ(result.water_cells, 144u); // two kernels plus the same-owner face
    EXPECT_EQ(result.total_cells, 272u);
    EXPECT_LE(result.total_cells, 2 * kCapacity);
    EXPECT_TRUE(result.mass_ok);
    EXPECT_EQ(result.volume_after_qmm - result.volume_before_qmm,
              result.source_volume_qmm - result.sink_volume_qmm + result.preparation_volume_qmm);
    EXPECT_EQ(result.source_volume_qmm, result.source_mm * 16);
    for (const auto& chunk : chunks) {
        EXPECT_TRUE(chunk->has_water_sim.load());
        EXPECT_EQ(chunk->water_depth_mm.size(), 64u);
        EXPECT_TRUE(chunk->has_water_authority());
        EXPECT_EQ(chunk->water_revision(), 1u);
    }
    ASSERT_EQ(work.size(), 1u);
    EXPECT_EQ(work.front().region_ticks, 1u);
    EXPECT_EQ(work.front().water_cells, 144u);
    EXPECT_EQ(shield.far_lod_authority_revision(), 2u);
    EXPECT_EQ(shield.far_lod_region_authority_revision(0, 0), 2u);
    EXPECT_EQ(shield.far_lod_region_authority_revision(-1, -1), 1u);
}

TEST(RegionalWater,
     EntirelyUninitializedOwnerEarnsOnePreparationAndKernelAtLegalSessionResolutions) {
    for (const int resolution : {4, 8, 16}) {
        SCOPED_TRACE(resolution);
        Systems::SHIELD_WorldSystem shield(nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
        Systems::WaterSystem water(nullptr, &shield);
        water.SetHydrology(true, 0, 0);
        water.SetSimResolution(resolution);
        ActiveRegionLedger ledger;
        ledger.mark_edited({0, 0}, WorldClock{});
        auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
        chunk->set_state(ChunkState::Idle);
        std::vector<std::shared_ptr<Chunk>> resident{chunk};
        entt::registry registry;
        RegionSchedule schedule;
        schedule.due = {{0, 0}};
        std::vector<world::RegionWork> work;
        const auto cells = static_cast<std::uint64_t>(resolution * resolution);
        water.UpdateRegional(registry,
                             resident,
                             ledger,
                             schedule,
                             work,
                             2 * cells + 4 * static_cast<std::uint64_t>(resolution));
        EXPECT_EQ(water.regional_work().used_windows, 1u);
        EXPECT_EQ(water.regional_work().preparation_cells, cells);
        EXPECT_EQ(water.regional_work().water_cells, cells);
        EXPECT_TRUE(water.regional_work().mass_ok);
        EXPECT_TRUE(chunk->has_water_sim.load());
        EXPECT_EQ(chunk->current_water_resolution.load(), resolution);
        EXPECT_EQ(chunk->water_depth_mm.size(), cells);
        ASSERT_EQ(work.size(), 1u);
        EXPECT_EQ(work.front().water_cells, cells);
    }
}

TEST_F(RegionalWaterFixture, SavedUltraGridResizesWithExactPhysicalVolumeAccounting) {
    Owner({0, 0});
    chunks = {WaterChunk({0, 0, 0}, 1000, 32)};
    Step(1, {{0, 0}});
    EXPECT_EQ(chunks.front()->current_water_resolution.load(), 8);
    EXPECT_EQ(water.regional_work().preparation_cells, 64u);
    EXPECT_EQ(water.regional_work().volume_before_qmm, 1024 * 1000);
    EXPECT_EQ(water.regional_work().preparation_volume_qmm, 0);
    EXPECT_TRUE(water.regional_work().mass_ok);
}

TEST_F(RegionalWaterFixture, MalformedBedsFluxDepthMirrorsAndDuplicateIdentitiesRefuseAtomically) {
    Owner({0, 0});
    for (int fault = 0; fault < 8; ++fault) {
        SCOPED_TRACE(fault);
        auto a = WaterChunk({0, 0, 0});
        auto b = WaterChunk({1, 0, 0});
        if (fault == 0)
            b->water_bed_mm.pop_back();
        if (fault == 1)
            b->water_edge_flux.pop_back();
        if (fault == 2)
            b->water_depth_mm.front() = -1;
        if (fault == 3)
            b->water_flow_data.clear();
        if (fault == 4)
            b->current_water_resolution.store(3);
        if (fault == 5)
            b->has_water_sim.store(false);
        if (fault == 7)
            b->water_sim_terrain_height[0] = std::numeric_limits<float>::infinity();
        chunks = {a, b};
        if (fault == 6)
            chunks.push_back(b);
        const auto prior_a = CaptureWater(*a), prior_b = CaptureWater(*b);
        const auto prior_ledger = ledger.encode();
        const auto telemetry = water.regional_work();
        EXPECT_THROW(Step(4, {{0, 0}}), std::runtime_error);
        EXPECT_EQ(CaptureWater(*a), prior_a);
        EXPECT_EQ(CaptureWater(*b), prior_b);
        EXPECT_EQ(ledger.encode(), prior_ledger);
        EXPECT_EQ(water.regional_work(), telemetry);
        EXPECT_TRUE(work.empty());
    }
}

TEST(RegionalWater, ZeroThroughThreeWindowsPreserveInteriorAndSealUnfundedPairs) {
    for (const std::uint64_t windows : {0u, 1u, 2u, 3u}) {
        Systems::SHIELD_WorldSystem shield(nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
        Systems::WaterSystem water(nullptr, &shield);
        water.SetHydrology(true, 1, 0);
        ActiveRegionLedger ledger;
        ledger.mark_edited({0, 0}, WorldClock{});
        ledger.mark_edited({1, 0}, WorldClock{});
        ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, world::water_share::kQ32}}));
        std::vector<std::shared_ptr<Chunk>> chunks{WaterChunk({31, 0, 0}),
                                                   WaterChunk({32, 0, 0}, 0),
                                                   WaterChunk({31, 0, 1}),
                                                   WaterChunk({32, 0, 1}, 0)};
        const auto old = CaptureWater(*chunks[0]);
        RegionSchedule schedule;
        schedule.due = {{0, 0}, {1, 0}};
        std::vector<world::RegionWork> work;
        entt::registry registry;
        water.UpdateRegional(registry, chunks, ledger, schedule, work, windows * kCapacity);
        const auto& result = water.regional_work();
        EXPECT_EQ(result.global_windows, windows);
        EXPECT_EQ(result.border_windows, windows == 3 ? 2u : 0u);
        EXPECT_LE(result.border_windows + result.allocated_windows, windows);
        EXPECT_LE(result.border_windows + result.used_windows, windows);
        EXPECT_EQ(result.allocated_windows, result.used_windows + result.unused_windows);
        EXPECT_EQ(windows,
                  result.border_windows + result.allocated_windows + result.unallocated_windows);
        EXPECT_LE(result.total_cells, windows * kCapacity);
        std::uint64_t total_debt = 0;
        for (const auto& [key, debt] : ledger.water_debts()) {
            total_debt += debt;
            (void)key;
        }
        EXPECT_EQ(total_debt,
                  world::water_share::kQ32 +
                      (windows - result.border_windows) * world::water_share::kQ32 -
                      result.allocated_windows * world::water_share::kQ32);
        if (windows > 0)
            EXPECT_GE(result.used_windows, 1u);
        else {
            EXPECT_EQ(CaptureWater(*chunks[0]), old);
            EXPECT_TRUE(work.empty());
        }
        EXPECT_TRUE(result.mass_ok);
        EXPECT_FALSE(result.fixed_cohort_bound_known);
    }
}

TEST(RegionalWater, HugeGrantDiscardsCapacityWithoutRepeatingRainDampingSourceOrSleep) {
    const auto run = [](std::uint64_t windows) {
        Systems::SHIELD_WorldSystem shield(nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
        Systems::WaterSystem water(nullptr, &shield);
        water.SetHydrology(true, 3, 2);
        ActiveRegionLedger ledger;
        ledger.mark_edited({0, 0}, WorldClock{});
        auto chunk = WaterChunk({0, 0, 0});
        chunk->water_edge_flux[0] = 100;
        chunk->ticks_below_threshold = 119;
        std::vector<std::shared_ptr<Chunk>> chunks{chunk};
        entt::registry registry;
        const auto e = registry.create();
        registry.emplace<Components::TransformComponent>(e).position = Vec3(1, 0, 1);
        registry.emplace<Components::WaterFlowSourceComponent>(e).flow_rate = 1.0f;
        RegionSchedule schedule;
        schedule.due = {{0, 0}};
        std::vector<world::RegionWork> work;
        water.UpdateRegional(registry, chunks, ledger, schedule, work, windows * kCapacity);
        const auto result = water.regional_work();
        EXPECT_EQ(result.allocated_windows, windows);
        EXPECT_EQ(result.used_windows, 1u);
        EXPECT_EQ(result.unused_windows, windows - 1);
        EXPECT_EQ(result.water_cells, 64u);
        EXPECT_TRUE(ledger.water_debts().empty());
        EXPECT_EQ(chunk->water_edge_flux[0], 78);
        EXPECT_EQ(result.source_mm, 64 * 3 + 8);
        EXPECT_EQ(result.sink_mm, 64 * 2);
        EXPECT_EQ(work.size(), 1u);
        return CaptureWater(*chunk);
    };
    EXPECT_EQ(run(1), run(100));
}

TEST_F(RegionalWaterFixture,
       ZeroAwakeOldDebtGrantsHaveNoVisitsOrCursorAdvanceAndAbsentDebtSurvives) {
    Owner({0, 0});
    Owner({4, 0});
    ledger.set_water_cursor({0, 0}, 999);
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, 3 * world::water_share::kQ32}, {{4, 0}, 17}}));
    auto chunk = WaterChunk({0, 0, 0});
    chunk->is_water_sleeping.store(true);
    chunks = {chunk};
    const auto prior = CaptureWater(*chunk);
    Step(10, {{0, 0}});
    const auto& result = water.regional_work();
    EXPECT_EQ(result.allocated_windows, 3u);
    EXPECT_EQ(result.used_windows, 0u);
    EXPECT_EQ(result.unused_windows, 3u);
    EXPECT_EQ(result.unallocated_windows, 7u);
    EXPECT_EQ(ledger.water_debts(), (ActiveRegionLedger::WaterDebts{{{4, 0}, 17}}));
    EXPECT_EQ(ledger.records().at({0, 0}).water_cursor, 999u);
    EXPECT_EQ(CaptureWater(*chunk), prior);
    EXPECT_TRUE(work.empty());
}

TEST_F(RegionalWaterFixture,
       BorderOnlyPositiveFacesSharedCornerPreserveOrientationWakeAndUnionMass) {
    Owner({-1, 0});
    Owner({0, 0});
    auto positive = WaterChunk({-1, 0, 31}, 1000);
    auto east = WaterChunk({0, 0, 31}, 0);
    auto north = WaterChunk({-1, 0, 32}, 0);
    Owner({-1, 1});
    east->is_water_sleeping.store(true);
    north->is_water_sleeping.store(true);
    east->ticks_below_threshold = 119;
    north->ticks_below_threshold = 119;
    chunks = {north, east, positive};
    water.SetHydrology(true, 5, 1);
    Source({-.25f, 0, 511.75f}); // border-only sources/rain never execute a kernel
    Source({.25f, 0, 511.75f});
    const auto total = [](const auto& values) {
        std::int64_t sum = 0;
        for (const auto& chunk : values)
            sum += std::accumulate(
                chunk->water_depth_mm.begin(), chunk->water_depth_mm.end(), std::int64_t{0});
        return sum;
    };
    const auto before = total(chunks);
    Step(4, {});
    EXPECT_TRUE(water.regional_work().visits.empty());
    EXPECT_EQ(water.regional_work().border_windows, 4u);
    EXPECT_EQ(water.regional_work().water_cells, 32u);
    EXPECT_EQ(water.regional_work().source_mm, 0);
    EXPECT_EQ(positive->water_depth_mm[63], 906); // +Z 48 then +X 46 in sorted region-key order
    EXPECT_EQ(east->water_depth_mm[56], 46);
    EXPECT_EQ(north->water_depth_mm[7], 48);
    EXPECT_EQ(east->ticks_below_threshold, 0);
    EXPECT_EQ(north->ticks_below_threshold, 0);
    EXPECT_FALSE(east->is_water_sleeping.load());
    EXPECT_EQ(total(chunks), before);
    EXPECT_TRUE(water.regional_work().mass_ok);
    EXPECT_TRUE(ledger.water_debts().empty());
    ASSERT_EQ(work.size(), 3u);
    EXPECT_EQ(work[0].water_cells, 16u); // (-1,0) has two separate face endpoints
    EXPECT_EQ(work[1].water_cells, 8u);
    EXPECT_EQ(work[2].water_cells, 8u);
    for (const auto& contribution : work)
        EXPECT_EQ(contribution.region_ticks, 1u);
}

TEST_F(RegionalWaterFixture, CommonBorderRunsOffInteriorPhaseAndFrozenOrResizingNeighborIsSealed) {
    Owner({0, 0});
    Owner({1, 0});
    Owner({0, 1});
    auto& records = const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records());
    records.at({0, 0}).state = RegionState::Reduced;
    records.at({0, 0}).cadence_shift = 2;
    records.at({0, 0}).cadence_phase = 3;
    records.at({1, 0}).state = RegionState::Reduced;
    records.at({1, 0}).cadence_shift = 1;
    records.at({1, 0}).cadence_phase = 1;
    auto a = WaterChunk({31, 0, 31}), b = WaterChunk({32, 0, 31}, 0);
    auto frozen = WaterChunk({31, 0, 32}, 0);
    records.at({0, 1}).state = RegionState::Frozen;
    const auto frozen_before = CaptureWater(*frozen);
    chunks = {a, b, frozen};
    Step(4, {}); // common phase at t=0; neither interior is due
    EXPECT_EQ(water.regional_work().border_windows, 2u);
    EXPECT_EQ(water.regional_work().used_windows, 0u);
    EXPECT_EQ(CaptureWater(*frozen), frozen_before);
    EXPECT_EQ(b->water_depth_mm[0], 48);
}

TEST_F(RegionalWaterFixture,
       FloorMappedResidentColumnSourceAndOneHopWakeExcludeUnselectedAndLoading) {
    Owner({-1, -1});
    auto target = WaterChunk({-1, 0, -1}, 0);
    target->is_water_sleeping.store(true);
    auto loading = WaterChunk({-2, 0, -1});
    loading->set_state(ChunkState::Loading);
    chunks = {target, loading};
    const auto untouched = CaptureWater(*loading);
    Source({-.25f, -.25f, -.25f}); // direct y=-1 missing; resident y=0, floor cell (7,7)
    Step(1, {{-1, -1}});
    EXPECT_EQ(target->water_depth_mm[63], 8);
    EXPECT_EQ(water.regional_work().source_mm, 8);
    EXPECT_EQ(CaptureWater(*loading), untouched);
    EXPECT_EQ(water.regional_work().visits, (std::vector<ChunkID>{target->get_id()}));
}

TEST_F(RegionalWaterFixture,
       SnapshotWakeCannotCascadeAndSourceCannotVisitUnknownFrozenOrUnselected) {
    Owner({0, 0});
    Owner({1, 0});
    auto a = WaterChunk({0, 0, 0}), b = WaterChunk({1, 0, 0}), c = WaterChunk({2, 0, 0});
    b->is_water_sleeping.store(true);
    c->is_water_sleeping.store(true);
    auto unknown = WaterChunk({64, 0, 0});
    auto frozen = WaterChunk({32, 0, 0});
    auto& records = const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records());
    records.at({1, 0}).state = RegionState::Frozen;
    const auto old_c = CaptureWater(*c), old_unknown = CaptureWater(*unknown),
               old_frozen = CaptureWater(*frozen);
    chunks = {a, b, c, unknown, frozen, nullptr};
    Source({1025, 0, 1});
    Source({513, 0, 1});
    Step(2, {{0, 0}});
    EXPECT_EQ(water.regional_work().used_windows, 2u);
    EXPECT_EQ(CaptureWater(*c), old_c);
    EXPECT_EQ(CaptureWater(*unknown), old_unknown);
    EXPECT_EQ(CaptureWater(*frozen), old_frozen);
    EXPECT_FALSE(b->is_water_sleeping.load());
}

TEST(RegionalWater, PermutedResidentsAndSourcesRetainCursorDebtPhysicalOrderAndExactWork) {
    const auto run = [](bool reversed) {
        Systems::SHIELD_WorldSystem shield(nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
        Systems::WaterSystem water(nullptr, &shield);
        water.SetHydrology(true, 1, 0);
        ActiveRegionLedger ledger;
        ledger.mark_edited({0, 0}, WorldClock{});
        ledger.mark_edited({1, 0}, WorldClock{});
        ledger.set_water_cursor({0, 0}, 1);
        std::vector<std::shared_ptr<Chunk>> chunks{WaterChunk({31, 0, 0}),
                                                   WaterChunk({32, 0, 0}, 0),
                                                   WaterChunk({31, 0, 1}),
                                                   WaterChunk({32, 0, 1}, 0)};
        entt::registry registry;
        std::vector<Vec3> positions{Vec3(500, 0, 1), Vec3(513, 0, 17)};
        if (reversed) {
            std::reverse(chunks.begin(), chunks.end());
            std::reverse(positions.begin(), positions.end());
        }
        for (const auto& position : positions) {
            const auto e = registry.create();
            registry.emplace<Components::TransformComponent>(e).position = position;
            registry.emplace<Components::WaterFlowSourceComponent>(e).flow_rate = 1;
        }
        RegionSchedule schedule;
        schedule.due = {{1, 0}, {0, 0}};
        std::vector<world::RegionWork> work;
        water.UpdateRegional(registry, chunks, ledger, schedule, work, 10 * kCapacity);
        const auto result = water.regional_work();
        EXPECT_EQ(result.border_windows, 4u);
        EXPECT_EQ(result.allocated_windows, 6u);
        EXPECT_EQ(result.used_windows, 4u);
        EXPECT_EQ(result.unused_windows, 2u);
        EXPECT_EQ(result.water_cells, 320u); // all four kernels and all four physical faces
        EXPECT_EQ(result.source_mm, 4 * 64 + 16);
        EXPECT_TRUE(result.mass_ok);
        EXPECT_EQ(std::set<ChunkID>(result.visits.begin(), result.visits.end()).size(), 4u);
        std::sort(chunks.begin(), chunks.end(), [](const auto& a, const auto& b) {
            return a->get_id() < b->get_id();
        });
        std::vector<WaterState> states;
        for (const auto& c : chunks)
            states.push_back(CaptureWater(*c));
        return std::make_tuple(result, ledger.encode(), states);
    };
    EXPECT_EQ(run(false), run(true));
}

TEST_F(RegionalWaterFixture, CheckedSourceAllocatorAndLateSeamOverflowRefuseWholePlan) {
    Owner({0, 0});
    Owner({1, 0});
    for (int fault = 0; fault < 4; ++fault) {
        SCOPED_TRACE(fault);
        registry.clear();
        auto a = WaterChunk({31, 0, 0}), b = WaterChunk({32, 0, 0});
        chunks = {a, b};
        if (fault == 0) {
            a->water_depth_mm.assign(64, std::numeric_limits<std::int32_t>::max());
            water.SetHydrology(true, 1, 0);
        } else if (fault == 1) {
            water.SetHydrology(true, 0, 0);
            a->water_depth_mm[0] = std::numeric_limits<std::int32_t>::max();
            Source({497, 0, 1});
        } else if (fault == 2) {
            ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, world::water_share::kMaxDebt}}));
        } else {
            ASSERT_TRUE(ledger.set_water_debts({}));
            a->water_bed_mm.assign(64, std::numeric_limits<std::int32_t>::max());
            b->water_bed_mm.assign(64, 0);
            b->water_depth_mm.assign(64, std::numeric_limits<std::int32_t>::max());
            Source({497, 0, 1}); // staged source and kernel succeed before the face refuses
        }
        const auto old_a = CaptureWater(*a), old_b = CaptureWater(*b);
        const auto old_ledger = ledger.encode();
        const auto telemetry = water.regional_work();
        const auto revision = shield.far_lod_region_authority_revision(0, 0);
        EXPECT_THROW(Step(4, {{0, 0}}), std::runtime_error);
        EXPECT_EQ(CaptureWater(*a), old_a);
        EXPECT_EQ(CaptureWater(*b), old_b);
        EXPECT_EQ(ledger.encode(), old_ledger);
        EXPECT_EQ(water.regional_work(), telemetry);
        EXPECT_EQ(shield.far_lod_region_authority_revision(0, 0), revision);
        EXPECT_TRUE(work.empty());
    }
}

TEST_F(RegionalWaterFixture, RepeatedTickRefusesAndAllSameOwnerFacesFitCellBudget) {
    Owner({0, 0});
    chunks = {
        WaterChunk({0, 0, 0}), WaterChunk({1, 0, 0}), WaterChunk({0, 0, 1}), WaterChunk({1, 0, 1})};
    Step(4, {{0, 0}});
    EXPECT_EQ(water.regional_work().water_cells, 320u);
    EXPECT_LE(water.regional_work().total_cells, 4 * kCapacity);
    const auto old = CaptureWater(*chunks[0]);
    const auto ledger_before = ledger.encode();
    const auto output = water.regional_work();
    EXPECT_THROW(Step(4, {{0, 0}}), std::runtime_error);
    EXPECT_EQ(CaptureWater(*chunks[0]), old);
    EXPECT_EQ(ledger.encode(), ledger_before);
    EXPECT_EQ(water.regional_work(), output);
}

TEST_F(RegionalWaterFixture, ResizeIsExcludedFromCrossBorderReservationUntilItsBudgetedVisit) {
    Owner({0, 0});
    Owner({1, 0});
    auto a = WaterChunk({31, 0, 0}, 1000, 4), b = WaterChunk({32, 0, 0}, 1000, 4);
    const auto old_b = CaptureWater(*b);
    chunks = {a, b};
    Step(3, {{0, 0}});
    EXPECT_EQ(water.regional_work().border_windows, 0u);
    EXPECT_EQ(water.regional_work().used_windows, 1u);
    EXPECT_EQ(water.regional_work().preparation_cells, 64u);
    EXPECT_EQ(a->current_water_resolution.load(), 8);
    EXPECT_EQ(CaptureWater(*b), old_b);
}

TEST_F(RegionalWaterFixture,
       WeatherRainConservativelyAdmitsSleepingReadyGridsButDoesNotInjectIrrigation) {
    Owner({0, 0});
    auto a = WaterChunk({0, 0, 0}, 0);
    a->is_water_sleeping.store(true);
    chunks = {a};
    const auto e = registry.create();
    registry.emplace<Components::TransformComponent>(e).position = Vec3(1, 0, 1);
    registry.emplace<Components::WaterSourceComponent>(e).output = 65535;
    Systems::WeatherSystem weather(1337);
    water.SetWeatherRain(&weather, 1);
    Step(1, {{0, 0}});
    EXPECT_EQ(water.regional_work().used_windows, 1u);
    EXPECT_EQ(water.debug_source_injected_mm(), 0);
    EXPECT_EQ(water.debug_sources_seen(), 0);
    EXPECT_LE(water.regional_work().source_mm, 64); // weather only, never reinterpret u16 output
}

TEST_F(RegionalWaterFixture, WorldLimitFacesDoNotWrapPackedIdentityAndBadCoordinateRefuses) {
    Owner({32767, 0});
    Owner({-32768, 0});
    auto high = WaterChunk({1048575, 0, 0});
    auto low = WaterChunk({-1048576, 0, 0}, 0);
    const auto old_high = CaptureWater(*high), old_low = CaptureWater(*low);
    chunks = {high, low};
    Step(4, {});
    EXPECT_EQ(water.regional_work().border_windows, 0u);
    EXPECT_EQ(CaptureWater(*high), old_high);
    EXPECT_EQ(CaptureWater(*low), old_low);
    chunks.push_back(WaterChunk({1048576, 0, 0}));
    const auto encoded = ledger.encode();
    Systems::WaterSystem fresh(nullptr, &shield);
    RegionSchedule invalid;
    invalid.tick = ledger.tick();
    EXPECT_THROW(fresh.UpdateRegional(registry, chunks, ledger, invalid, work, 4 * kCapacity),
                 std::runtime_error);
    EXPECT_EQ(ledger.encode(), encoded);
}

TEST_F(RegionalWaterFixture,
       SourceConversionAndInternalDestinationOverflowRefuseBeforePublication) {
    Owner({0, 0});
    auto a = WaterChunk({0, 0, 0});
    chunks = {a};
    Source({1, 0, 1}, std::numeric_limits<float>::infinity());
    auto old = CaptureWater(*a);
    EXPECT_THROW(Step(1, {{0, 0}}), std::runtime_error);
    EXPECT_EQ(CaptureWater(*a), old);
    registry.clear();
    a->water_depth_mm.assign(64, std::numeric_limits<std::int32_t>::max());
    a->water_bed_mm.assign(64, std::numeric_limits<std::int32_t>::max());
    a->water_bed_mm[0] = 0; // neighbors transfer toward an already full destination
    old = CaptureWater(*a);
    const auto encoded = ledger.encode();
    EXPECT_THROW(Step(1, {{0, 0}}), std::runtime_error);
    EXPECT_EQ(CaptureWater(*a), old);
    EXPECT_EQ(ledger.encode(), encoded);
    EXPECT_TRUE(work.empty());
}

TEST_F(RegionalWaterFixture,
       CursorDebtAndCandidateMembershipResumeFromCanonicalLedgerWithoutCatchUp) {
    Owner({0, 0});
    Owner({4, 0});
    ledger.pin({0, 0}, true, WorldClock{});
    ledger.set_water_cursor({0, 0}, 5);
    ASSERT_TRUE(ledger.set_water_debts({{{4, 0}, 19}}));
    chunks = {WaterChunk({0, 0, 0}), WaterChunk({1, 0, 0}), WaterChunk({2, 0, 0})};
    Step(1, {{0, 0}});
    EXPECT_EQ(water.regional_work().visits, (std::vector<ChunkID>{chunks[2]->get_id()}));
    EXPECT_EQ(ledger.records().at({0, 0}).water_cursor, 0u);
    EXPECT_EQ(ledger.water_debts().at({4, 0}), 19u); // absent due claim untouched
    const auto before = ledger.encode();
    ActiveRegionLedger restored;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(before, restored, error)) << error;
    EXPECT_EQ(restored.encode(), before);
    // No ticking on restore; next due tick uses the same cursor/candidate membership.
    const std::span<const Vec3> no_anchors;
    auto schedule = ledger.schedule(WorldClock(1), no_anchors);
    auto restored_schedule = restored.schedule(WorldClock(1), no_anchors);
    schedule.due = {{0, 0}};
    restored_schedule.due = {{0, 0}};
    Systems::WaterSystem resumed(nullptr, &shield);
    resumed.SetHydrology(true, 0, 0);
    std::vector<std::shared_ptr<Chunk>> copies;
    for (const auto& c : chunks) {
        auto copy = WaterChunk(c->get_coords());
        copy->water_depth_mm = c->water_depth_mm;
        copy->water_bed_mm = c->water_bed_mm;
        copy->water_edge_flux = c->water_edge_flux;
        copy->ticks_below_threshold = c->ticks_below_threshold;
        copies.push_back(copy);
    }
    std::vector<world::RegionWork> resumed_work;
    water.UpdateRegional(registry, chunks, ledger, schedule, work, kCapacity);
    resumed.UpdateRegional(registry, copies, restored, restored_schedule, resumed_work, kCapacity);
    EXPECT_EQ(water.regional_work(), resumed.regional_work());
    EXPECT_EQ(ledger.encode(), restored.encode());
    EXPECT_EQ(chunks[0]->water_depth_mm, copies[0]->water_depth_mm);
    EXPECT_FALSE(water.regional_work().fixed_cohort_bound_known);
}

TEST_F(RegionalWaterFixture, WindowCapAndDuplicateOrUnknownDueOwnersRefuseAtomicPlan) {
    Owner({0, 0});
    chunks = {WaterChunk({0, 0, 0})};
    const auto before = CaptureWater(*chunks.front());
    const auto encoded = ledger.encode();
    const auto telemetry = water.regional_work();
    EXPECT_THROW(Step(world::water_share::kMaxWindowBudget + 1, {{0, 0}}), std::runtime_error);
    EXPECT_THROW(Step(1, {{0, 0}, {0, 0}}), std::runtime_error);
    EXPECT_THROW(Step(1, {{9, 9}}), std::runtime_error);
    EXPECT_EQ(CaptureWater(*chunks.front()), before);
    EXPECT_EQ(ledger.encode(), encoded);
    EXPECT_EQ(water.regional_work(), telemetry);
    EXPECT_TRUE(work.empty());
}

TEST(RegionalWaterPersistence, StrictDecodeRejectsMalformedAuthorityAndKeepsLegacyFluxRepair) {
    WorldStreamingState state;
    ASSERT_TRUE(state.insert_chunk(WaterChunk({0, 0, 0})));
    ASSERT_TRUE(state.insert_chunk(WaterChunk({1, 0, 0}, 500, 4)));
    const auto original =
        nlohmann::json::parse(Persistence::SerializeWorldStreamingStateSnapshotJson(state));
    for (int fault = 0; fault < 8; ++fault) {
        SCOPED_TRACE(fault);
        auto snapshot = original;
        auto& malformed = snapshot.at("chunks").at(1); // fail after a valid record
        switch (fault) {
            case 0:
                malformed.erase("water_edge_flux");
                break;
            case 1:
                malformed.at("water_bed_mm").erase(0);
                break;
            case 2:
                malformed.at("water_depth_mm").at(0) = -1;
                break;
            case 3:
                malformed.at("water_edge_flux").at(0) = std::uint64_t{1} << 32;
                break;
            case 4:
                malformed.at("water_bed_mm").at(0) = -2147483649ll;
                break;
            case 5:
                malformed.at("water_state").at("current_water_resolution") = 3;
                break;
            case 6:
                malformed.at("water_flow_data").clear();
                break;
            case 7:
                malformed.at("coords").at("x") = (std::uint64_t{1} << 32) + 1;
                break; // would narrow to the original x=1 and preserve its packed ID
        }
        WorldStreamingState restored;
        std::vector<std::string> errors;
        EXPECT_FALSE(Persistence::LoadWorldStreamingStateSnapshotJson(
            snapshot.dump(), restored, errors, true));
        EXPECT_EQ(restored.size(), 0u); // no partial authority adoption
        EXPECT_FALSE(errors.empty());
        if (fault == 0) {
            errors.clear();
            ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(
                snapshot.dump(), restored, errors));
            EXPECT_EQ(restored.find_chunk(Chunk::calculate_id({1, 0, 0}))->water_edge_flux,
                      std::vector<std::int32_t>(32, 0));
        }
    }
    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(
        Persistence::LoadWorldStreamingStateSnapshotJson(original.dump(), restored, errors, true));
    const auto old_resolution = restored.find_chunk(Chunk::calculate_id({1, 0, 0}));
    ASSERT_NE(old_resolution, nullptr);
    EXPECT_EQ(old_resolution->current_water_resolution.load(), 4);
    EXPECT_EQ(CaptureWater(*old_resolution),
              CaptureWater(*state.find_chunk(old_resolution->get_id())));
}

} // namespace
