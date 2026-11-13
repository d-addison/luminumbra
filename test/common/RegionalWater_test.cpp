#include <gtest/gtest.h>

#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/RegionalWater.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "nlohmann/json.hpp"
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
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

TEST(RegionalWater, MissingZeroFluxInitializationDoesNotDirtyButPartialFluxReplacementDoes) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    for (const bool partial : {false, true}) {
        SCOPED_TRACE(partial);
        Chunk chunk({0, 0, 0});
        FillWater(chunk, 8, 0);
        chunk.water_edge_flux.assign(partial ? 127u : 0u, 0);
        const auto before = Snapshot(chunk);
        EXPECT_FALSE(WaterKernels::StepInterior(chunk, shield, {}).applied);
        EXPECT_EQ(Snapshot(chunk), before);
        ASSERT_TRUE(
            WaterKernels::StepInterior(
                chunk, shield, {.finite_hydrology = true}, WaterKernels::OverflowPolicy::Saturate)
                .applied);
        EXPECT_EQ(chunk.water_depth_mm, std::vector<std::int32_t>(64, 0));
        EXPECT_EQ(chunk.water_edge_flux, std::vector<std::int32_t>(128, 0));
        EXPECT_EQ(chunk.is_water_state_dirty(), partial);
        EXPECT_EQ(chunk.is_voxel_data_dirty(), partial);
        EXPECT_EQ(chunk.water_revision(), partial ? 1u : 0u);
    }
}

TEST(RegionalWater, MaximalDirtyTickCounterInvalidatesWithoutOverflow) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    for (const auto policy :
         {WaterKernels::OverflowPolicy::Refuse, WaterKernels::OverflowPolicy::Saturate}) {
        for (const int initial_ticks :
             {std::numeric_limits<int>::max() - 1, std::numeric_limits<int>::max()}) {
            for (const std::int32_t rain_mm : {0, 1, 2}) {
                SCOPED_TRACE(::testing::Message() << initial_ticks << " rain=" << rain_mm
                                                  << " policy=" << static_cast<int>(policy));
                Chunk chunk({0, 0, 0});
                FillWater(chunk, 8);
                chunk.water_mesh_generated.store(true);
                chunk.water_mesh_dirty_ticks = initial_ticks;

                const auto result =
                    WaterKernels::StepInterior(chunk, shield, {.rain_mm = rain_mm}, policy);

                ASSERT_TRUE(result.applied);
                ASSERT_EQ(chunk.water_depth_mm.front(), 1000 + rain_mm);
                const bool invalidated = rain_mm > 1;
                EXPECT_EQ(result.water_mesh_invalidated, invalidated);
                EXPECT_EQ(chunk.water_mesh_generated.load(), !invalidated);
                EXPECT_EQ(chunk.water_mesh_dirty_ticks, invalidated ? 0 : initial_ticks);
            }
        }
    }
}

TEST(RegionalWater, DirtyTickCounterPreservesRemeshCadence) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    for (const auto policy :
         {WaterKernels::OverflowPolicy::Refuse, WaterKernels::OverflowPolicy::Saturate}) {
        for (int initial_ticks = 0; initial_ticks <= 16; ++initial_ticks) {
            for (const std::int32_t rain_mm : {0, 1, 2}) {
                SCOPED_TRACE(::testing::Message() << initial_ticks << " rain=" << rain_mm
                                                  << " policy=" << static_cast<int>(policy));
                Chunk chunk({0, 0, 0});
                FillWater(chunk, 8);
                chunk.water_mesh_generated.store(true);
                chunk.water_mesh_dirty_ticks = initial_ticks;

                const auto result =
                    WaterKernels::StepInterior(chunk, shield, {.rain_mm = rain_mm}, policy);

                ASSERT_TRUE(result.applied);
                ASSERT_EQ(chunk.water_depth_mm.front(), 1000 + rain_mm);
                const bool dirty = rain_mm > 1;
                const bool invalidated = dirty && initial_ticks >= 14;
                const int expected_ticks =
                    invalidated ? 0 : initial_ticks + static_cast<int>(dirty);
                EXPECT_EQ(result.water_mesh_invalidated, invalidated);
                EXPECT_EQ(chunk.water_mesh_generated.load(), !invalidated);
                EXPECT_EQ(chunk.water_mesh_dirty_ticks, expected_ticks);
            }
        }
    }
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
TEST(RegionalWaterPersistence, StrictWaterRefusesMalformedPresentArraysBeforeInsertion) {
    namespace P = Luminumbra::Persistence;
    WorldStreamingState original;
    auto chunk = original.get_or_create_chunk({-1, 0, 2});
    FillWater(*chunk, 4);
    chunk->water_flow_data.assign(16, Vec2(0.1f, -0.2f));
    chunk->water_sim_terrain_height.assign(16, -0.1f);
    const auto snapshot =
        nlohmann::json::parse(P::SerializeWorldStreamingStateSnapshotJson(original));
    const std::vector<std::pair<std::string, nlohmann::json>> changes{
        {"/water_depth_mm/0", -1},
        {"/water_depth_mm/0", 2147483648ULL},
        {"/water_bed_mm/0", -2147483649LL},
        {"/water_edge_flux/0", 1.5},
        {"/water_depth_mm", std::vector<int>(15, 0)},
        {"/water_bed_mm", std::vector<int>(17, 0)},
        {"/water_edge_flux", std::vector<int>(31, 0)},
        {"/water_level_data", std::vector<float>(1025, 0.0f)},
        {"/water_flow_data", nlohmann::json::array({{{"x", 0}, {"y", "invalid"}}})},
        {"/water_sim_terrain_height", std::vector<float>(1025, 0.0f)},
        {"/water_level_data/0", 1e100},
        {"/water_flow_data/0/x", 1e100},
        {"/water_flow_data/0/y", -1e100},
        {"/water_sim_terrain_height/0", 1e100},
        {"/water_level_data/0", nullptr},
        {"/water_depth_mm", std::vector<int>(1025, 0)},
        {"/water_edge_flux", std::vector<int>(2049, 0)}};
    for (const auto& [pointer, replacement] : changes) {
        SCOPED_TRACE(pointer);
        auto malformed = snapshot;
        // A valid first record must also be cleared when a later record fails.
        malformed["chunks"].push_back(malformed["chunks"][0]);
        malformed["chunks"][0]["coords"] = {{"x", 0}, {"y", 0}, {"z", 0}};
        malformed["chunks"][0]["chunk_id"] = Chunk::calculate_id({0, 0, 0});
        malformed["chunk_count"] = 2;
        malformed["chunks"][1][nlohmann::json::json_pointer(pointer)] = replacement;
        WorldStreamingState restored;
        std::vector<std::string> errors;
        EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(
            malformed.dump(), restored, errors, {.strict_water = true}));
        EXPECT_EQ(restored.size(), 0u);
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.front().find("Saved water"), std::string::npos);
    }
    for (const char* field : {"water_depth_mm", "water_bed_mm"}) {
        auto missing = snapshot;
        missing["chunks"][0].erase(field);
        WorldStreamingState restored;
        std::vector<std::string> errors;
        EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(
            missing.dump(), restored, errors, {.strict_water = true}));
        EXPECT_EQ(restored.size(), 0u);
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.front().find("Saved water"), std::string::npos);
    }
}

TEST(RegionalWaterPersistence, StrictWaterAcceptsDormantActiveSleepingOffAndPromotedWriters) {
    namespace P = Luminumbra::Persistence;
    for (const int resolution : {0, 2, 4, 8, 16, 32}) {
        for (int shape = 0; shape < 8; ++shape) {
            SCOPED_TRACE(resolution);
            SCOPED_TRACE(shape);
            WorldStreamingState original;
            const auto chunk = original.get_or_create_chunk({0, 0, 0});
            chunk->current_water_resolution.store(resolution);
            const auto cells =
                static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
            if (shape == 1) {
                // Dormant authority can precede bed, flux, and mirror initialization.
                chunk->water_depth_mm = {123};
                chunk->mark_water_data_dirty();
            } else if (shape == 2) {
                chunk->water_depth_mm.assign(cells, 123);
                chunk->water_bed_mm.assign(cells, -100);
                chunk->water_level_data = {0.023f};
                chunk->water_flow_data.assign(2, Vec2(0.1f, -0.2f));
                chunk->water_sim_terrain_height.assign(3, -0.1f);
            } else if (shape >= 3) {
                FillWater(*chunk, resolution);
                chunk->water_flow_data.assign(cells, Vec2(0.1f, -0.2f));
                chunk->water_sim_terrain_height.assign(cells, -0.1f);
                if (shape == 3) {
                    chunk->water_depth_mm.clear();
                    chunk->water_bed_mm.clear();
                    chunk->water_edge_flux.clear();
                } else if (shape == 5) {
                    chunk->is_water_sleeping.store(true);
                } else if (shape == 6) {
                    chunk->mark_water_data_dirty();
                    chunk->water_level_data.clear();
                    chunk->water_flow_data.clear();
                    chunk->water_sim_terrain_height.clear();
                } else if (shape == 7) {
                    WaterSystem water(nullptr, nullptr);
                    ASSERT_TRUE(water.ResizeSimulationGrid(*chunk, WaterDetailLevel::Off));
                }
            }
            const auto encoded = P::SerializeChunkRecordPayloadJson(*chunk);
            for (const auto& bytes : {P::SerializeWorldStreamingStateSnapshotJson(original),
                                      P::SerializeChunkAuthoritySnapshotJson(*chunk),
                                      encoded}) {
                WorldStreamingState restored;
                std::vector<std::string> errors;
                ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(
                    bytes, restored, errors, {.strict_water = true}))
                    << (errors.empty() ? "" : errors.front());
                const auto actual = restored.find_chunk({0, 0, 0});
                ASSERT_NE(actual, nullptr);
                EXPECT_TRUE(actual->same_water_state(*chunk));
            }
        }
    }
}

TEST(RegionalWaterPersistence, WaterObservationKeepsLegacyLoadsPermissiveAndStrictLoadsRefusing) {
    namespace P = Luminumbra::Persistence;
    for (const bool active : {false, true}) {
        for (int fault = 0; fault < 7; ++fault) {
            SCOPED_TRACE(active);
            SCOPED_TRACE(fault);
            WorldStreamingState original;
            const auto chunk = original.get_or_create_chunk({0, 0, 0});
            FillWater(*chunk, 4);
            chunk->has_water_sim.store(active);
            auto json =
                nlohmann::json::parse(P::SerializeWorldStreamingStateSnapshotJson(original));
            auto& water = json["chunks"][0];
            if (fault == 1) {
                water["water_depth_mm"][0] = -1;
            } else if (fault == 2) {
                water["water_edge_flux"].erase(water["water_edge_flux"].begin());
            } else if (fault == 3) {
                water.erase("water_depth_mm");
                water.erase("water_bed_mm");
                water.erase("water_edge_flux");
            } else if (fault == 4) {
                water.erase("water_edge_flux");
            } else if (fault == 5) {
                water["water_depth_mm"].push_back(0);
                water["chunk_id"] = Chunk::calculate_id({1, 0, 0});
            }
            const auto bytes = fault == 6 ? std::string("{") : json.dump();
            const bool loadable = fault < 5;
            const bool valid = loadable && fault != 1 && fault != 2;
            bool observed = true;
            WorldStreamingState restored;
            std::vector<std::string> errors;
            EXPECT_EQ(P::LoadWorldStreamingStateSnapshotJson(
                          bytes, restored, errors, {.strict_water_valid = &observed}),
                      loadable);
            EXPECT_EQ(observed, valid);
            errors.clear();
            EXPECT_EQ(P::LoadWorldStreamingStateSnapshotJson(
                          bytes, restored, errors, {.strict_water = true}),
                      valid);
            if (!valid) {
                EXPECT_TRUE(restored.empty());
                EXPECT_FALSE(errors.empty());
            }
        }
    }
}

TEST(RegionalWaterPersistence, StrictWaterPreservesMissingJsonIntegerDefaults) {
    namespace P = Luminumbra::Persistence;
    for (const bool missing_pair : {false, true}) {
        SCOPED_TRACE(missing_pair);
        WorldStreamingState original;
        const auto chunk = original.get_or_create_chunk({0, 0, 0});
        FillWater(*chunk, 4);
        auto json = nlohmann::json::parse(P::SerializeWorldStreamingStateSnapshotJson(original));
        auto& water = json["chunks"][0];
        water.erase("water_edge_flux");
        if (missing_pair) {
            water.erase("water_depth_mm");
            water.erase("water_bed_mm");
        }
        WorldStreamingState restored;
        std::vector<std::string> errors;
        ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(
            json.dump(), restored, errors, {.strict_water = true}))
            << (errors.empty() ? "" : errors.front());
        const auto actual = restored.find_chunk({0, 0, 0});
        ASSERT_NE(actual, nullptr);
        EXPECT_EQ(actual->water_depth_mm,
                  missing_pair ? std::vector<std::int32_t>{} : chunk->water_depth_mm);
        EXPECT_EQ(actual->water_bed_mm,
                  missing_pair ? std::vector<std::int32_t>{} : chunk->water_bed_mm);
        EXPECT_EQ(actual->water_edge_flux, std::vector<std::int32_t>(missing_pair ? 0u : 32u, 0));
    }
}

TEST(RegionalWaterPersistence, StrictWaterAcceptsHistoricalEmptyFluxForBothArrayEncodings) {
    namespace P = Luminumbra::Persistence;
    WorldStreamingState original;
    const auto chunk = original.get_or_create_chunk({0, 0, 0});
    FillWater(*chunk, 4);
    chunk->water_edge_flux.clear();
    for (const auto& bytes : {P::SerializeWorldStreamingStateSnapshotJson(original),
                              P::SerializeChunkRecordPayloadJson(*chunk)}) {
        WorldStreamingState restored;
        std::vector<std::string> errors;
        ASSERT_TRUE(
            P::LoadWorldStreamingStateSnapshotJson(bytes, restored, errors, {.strict_water = true}))
            << (errors.empty() ? "" : errors.front());
        const auto actual = restored.find_chunk({0, 0, 0});
        ASSERT_NE(actual, nullptr);
        EXPECT_TRUE(actual->same_water_state(*chunk));
        EXPECT_TRUE(actual->water_edge_flux.empty());
    }
}

TEST(RegionalWaterPersistence, StrictWaterRequiresCompletePopulatedActiveGrid) {
    namespace P = Luminumbra::Persistence;
    for (const int resolution : {2, 4, 8, 16, 32}) {
        const auto cells =
            static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
        for (const auto count : {cells - 1u, cells, cells + 1u}) {
            for (const bool active : {false, true}) {
                for (const bool empty_flux : {false, true}) {
                    SCOPED_TRACE(resolution);
                    SCOPED_TRACE(count);
                    SCOPED_TRACE(active);
                    SCOPED_TRACE(empty_flux);
                    WorldStreamingState original;
                    const auto chunk = original.get_or_create_chunk({0, 0, 0});
                    FillWater(*chunk, resolution);
                    chunk->has_water_sim.store(active);
                    chunk->water_depth_mm.assign(count, 123);
                    chunk->water_bed_mm.assign(count, -100);
                    chunk->water_edge_flux.assign(empty_flux ? 0u : 2u * count, -10);
                    chunk->mark_water_data_dirty();
                    const bool expected = count <= 1024u && (!active || count == cells);
                    for (const auto& bytes : {P::SerializeWorldStreamingStateSnapshotJson(original),
                                              P::SerializeChunkAuthoritySnapshotJson(*chunk),
                                              P::SerializeChunkRecordPayloadJson(*chunk)}) {
                        bool observed = !expected;
                        WorldStreamingState restored;
                        std::vector<std::string> errors;
                        ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(
                            bytes, restored, errors, {.strict_water_valid = &observed}));
                        EXPECT_EQ(observed, expected);
                        EXPECT_EQ(P::LoadWorldStreamingStateSnapshotJson(
                                      bytes,
                                      restored,
                                      errors,
                                      {.strict_water = true, .strict_water_valid = &observed}),
                                  expected);
                        EXPECT_EQ(observed, expected);
                        if (expected) {
                            ASSERT_NE(restored.find_chunk({0, 0, 0}), nullptr);
                            EXPECT_TRUE(restored.find_chunk({0, 0, 0})->same_water_state(*chunk));
                            EXPECT_TRUE(errors.empty());
                        } else {
                            EXPECT_TRUE(restored.empty());
                            ASSERT_FALSE(errors.empty());
                            EXPECT_NE(errors.front().find("Saved water"), std::string::npos);
                        }
                    }
                }
            }
        }
    }
}

TEST(RegionalWaterPersistence, StrictWaterRequiresExactSizesForEachPopulatedActiveArray) {
    namespace P = Luminumbra::Persistence;
    for (const int resolution : {2, 4, 8, 16, 32}) {
        WorldStreamingState original;
        const auto chunk = original.get_or_create_chunk({0, 0, 0});
        const auto cells =
            static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
        for (const char* field : {"water_depth_mm", "water_bed_mm", "water_edge_flux"}) {
            const std::string_view name(field);
            const auto required = name == "water_edge_flux" ? 2u * cells : cells;
            for (const auto count : {required - 1u, required, required + 1u}) {
                SCOPED_TRACE(resolution);
                SCOPED_TRACE(field);
                SCOPED_TRACE(count);
                FillWater(*chunk, resolution);
                auto& grid = name == "water_depth_mm" ? chunk->water_depth_mm
                             : name == "water_bed_mm" ? chunk->water_bed_mm
                                                      : chunk->water_edge_flux;
                grid.assign(count, 0);
                for (const auto& bytes : {P::SerializeWorldStreamingStateSnapshotJson(original),
                                          P::SerializeChunkRecordPayloadJson(*chunk)}) {
                    WorldStreamingState restored;
                    std::vector<std::string> errors;
                    EXPECT_EQ(P::LoadWorldStreamingStateSnapshotJson(
                                  bytes, restored, errors, {.strict_water = true}),
                              count == required);
                    if (count == required) {
                        EXPECT_EQ(restored.size(), 1u);
                        EXPECT_TRUE(errors.empty());
                    } else {
                        EXPECT_TRUE(restored.empty());
                        ASSERT_FALSE(errors.empty());
                        EXPECT_NE(errors.front().find("Saved water"), std::string::npos);
                    }
                }
            }
        }
    }
}

TEST(RegionalWaterPersistence, StrictEncodedWaterRefusesMalformedIntegersAndEnvelopes) {
    namespace P = Luminumbra::Persistence;
    Chunk chunk({0, 0, 0});
    FillWater(chunk, 4);
    auto snapshot = nlohmann::json::parse(P::SerializeChunkRecordPayloadJson(chunk));
    snapshot["chunks"].push_back(snapshot["chunks"][0]);
    snapshot["chunks"][1]["coords"] = {{"x", 1}, {"y", 0}, {"z", 0}};
    snapshot["chunks"][1]["chunk_id"] = Chunk::calculate_id({1, 0, 0});
    snapshot["chunk_count"] = 2;
    for (const char* field : {"water_depth_mm", "water_bed_mm", "water_edge_flux"}) {
        for (int fault = 0; fault < 17; ++fault) {
            SCOPED_TRACE(field);
            SCOPED_TRACE(fault);
            auto malformed = snapshot;
            auto& water = malformed["chunks"][1];
            auto& array = water[field];
            if (fault == 0) {
                water.erase(field);
            } else if (fault == 1) {
                array = nullptr;
            } else if (fault == 2) {
                array = nlohmann::json::array();
            } else if (fault == 3) {
                array.erase("count");
            } else if (fault == 4) {
                array["count"] = -1;
            } else if (fault == 5) {
                array["count"] = 1.5;
            } else if (fault == 6) {
                array["count"] = std::numeric_limits<std::uint64_t>::max();
            } else if (fault == 7) {
                array["count"] = 1025u;
            } else if (fault == 8) {
                array["type"] = "u32";
            } else if (fault == 9) {
                array["type"] = "i64";
            } else if (fault == 10) {
                array["enc"] = "unknown";
            } else if (fault == 11) {
                array["data"] = false;
            } else if (fault == 12) {
                array["data"] = "!" + array["data"].get<std::string>().substr(1);
            } else if (fault == 13) {
                array["data"] = array["data"].get<std::string>().substr(1);
            } else if (fault == 14) {
                array.erase("data");
            } else if (fault == 15) {
                array["extra"] = 0;
            } else {
                array["count"] = 0u;
            }
            WorldStreamingState restored;
            restored.get_or_create_chunk({2, 0, 0});
            std::vector<std::string> errors;
            bool valid = true;
            EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(
                malformed.dump(),
                restored,
                errors,
                {.strict_water = true, .strict_water_valid = &valid}));
            EXPECT_FALSE(valid);
            EXPECT_TRUE(restored.empty());
            ASSERT_FALSE(errors.empty());
            EXPECT_NE(errors.front().find("Saved water"), std::string::npos);
        }
    }
    for (const auto depth : {-1, std::numeric_limits<std::int32_t>::min()}) {
        FillWater(chunk, 4, depth);
        auto malformed = snapshot;
        malformed["chunks"][1]["water_depth_mm"] = nlohmann::json::parse(
            P::SerializeChunkRecordPayloadJson(chunk))["chunks"][0]["water_depth_mm"];
        WorldStreamingState restored;
        std::vector<std::string> errors;
        bool valid = true;
        ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(
            malformed.dump(), restored, errors, {.strict_water_valid = &valid}));
        EXPECT_FALSE(valid);
        EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(
            malformed.dump(), restored, errors, {.strict_water = true}));
        EXPECT_TRUE(restored.empty());
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.front().find("Saved water"), std::string::npos);
    }
}

TEST(RegionalWaterPersistence, StrictEncodedWaterChecksMirrorCountsBeforeDecodingArrays) {
    namespace P = Luminumbra::Persistence;
    Chunk chunk({0, 0, 0});
    FillWater(chunk, 4);
    const auto snapshot = nlohmann::json::parse(P::SerializeChunkRecordPayloadJson(chunk));
    for (const char* field : {"water_level_data", "water_flow_data", "water_sim_terrain_height"}) {
        for (const std::size_t count : {0u, 1u, 1024u}) {
            SCOPED_TRACE(field);
            SCOPED_TRACE(count);
            chunk.water_level_data.assign(count, 0.1f);
            chunk.water_flow_data.assign(count, Vec2(0.1f));
            chunk.water_sim_terrain_height.assign(count, -0.1f);
            bool valid = false;
            const auto bytes = P::SerializeChunkRecordPayloadJson(chunk, &valid);
            EXPECT_TRUE(valid);
            WorldStreamingState restored;
            std::vector<std::string> errors;
            ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(
                bytes, restored, errors, {.strict_water = true, .strict_water_valid = &valid}))
                << (errors.empty() ? "" : errors.front());
            EXPECT_TRUE(valid);
            ASSERT_NE(restored.find_chunk({0, 0, 0}), nullptr);
            EXPECT_TRUE(restored.find_chunk({0, 0, 0})->same_water_state(chunk));
        }
        for (int fault = 0; fault < 9; ++fault) {
            SCOPED_TRACE(field);
            SCOPED_TRACE(fault);
            auto malformed = snapshot;
            auto& array = malformed["chunks"][0][field];
            if (fault == 0) {
                array["count"] = 1025u;
            } else if (fault == 1) {
                array["count"] = std::numeric_limits<std::uint64_t>::max();
            } else if (fault == 2) {
                array["count"] = -1;
            } else if (fault == 3) {
                array["count"] = 1.5;
            } else if (fault == 4) {
                array["count"] = "1025";
            } else if (fault == 5) {
                array["count"] = nullptr;
            } else if (fault == 6) {
                array.erase("count");
            } else if (fault == 7) {
                array = nullptr;
            } else {
                malformed["chunks"][0].erase(field);
            }
            // The earlier SDF array must never be decoded before water counts are checked.
            malformed["chunks"][0]["sdf_data"] = nullptr;
            WorldStreamingState restored;
            restored.get_or_create_chunk({1, 0, 0});
            std::vector<std::string> errors;
            bool valid = true;
            EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(
                malformed.dump(),
                restored,
                errors,
                {.strict_water = true, .strict_water_valid = &valid}));
            EXPECT_FALSE(valid);
            EXPECT_TRUE(restored.empty());
            ASSERT_FALSE(errors.empty());
            EXPECT_NE(errors.front().find(fault < 6 ? "Saved water grid shape refused"
                                                    : "Saved water structure refused"),
                      std::string::npos);
        }
    }
}

TEST(RegionalWaterPersistence, StrictEncodedWaterRefusesInvalidResolutionAndState) {
    namespace P = Luminumbra::Persistence;
    Chunk chunk({0, 0, 0});
    FillWater(chunk, 4);
    const auto snapshot = nlohmann::json::parse(P::SerializeChunkRecordPayloadJson(chunk));
    const std::vector<std::pair<std::string, nlohmann::json>> changes{
        {"/water_state", nullptr},
        {"/water_state/has_water_sim", "true"},
        {"/water_state/current_water_resolution", -1},
        {"/water_state/current_water_resolution", 1},
        {"/water_state/current_water_resolution", 3},
        {"/water_state/current_water_resolution", 33},
        {"/water_state/current_water_resolution", std::numeric_limits<std::uint64_t>::max()},
        {"/water_state/current_water_resolution", 4.5},
        {"/water_state/current_water_resolution", "4"}};
    for (const auto& [pointer, replacement] : changes) {
        SCOPED_TRACE(pointer);
        SCOPED_TRACE(replacement.dump());
        auto malformed = snapshot;
        malformed["chunks"][0][nlohmann::json::json_pointer(pointer)] = replacement;
        WorldStreamingState restored;
        restored.get_or_create_chunk({1, 0, 0});
        std::vector<std::string> errors;
        bool valid = true;
        EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(
            malformed.dump(),
            restored,
            errors,
            {.strict_water = true, .strict_water_valid = &valid}));
        EXPECT_FALSE(valid);
        EXPECT_TRUE(restored.empty());
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.front().find("Saved water"), std::string::npos);
    }
}

TEST(RegionalWaterPersistence, StrictEncodedWaterPreservesIntegerRangeEndpoints) {
    namespace P = Luminumbra::Persistence;
    Chunk chunk({0, 0, 0});
    FillWater(chunk, 2);
    constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    chunk.water_depth_mm = {0, maximum, 0, maximum};
    chunk.water_bed_mm = {minimum, maximum, minimum, maximum};
    chunk.water_edge_flux = {
        minimum, maximum, minimum, maximum, minimum, maximum, minimum, maximum};
    WorldStreamingState restored;
    std::vector<std::string> errors;
    bool valid = false;
    ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(
        P::SerializeChunkRecordPayloadJson(chunk),
        restored,
        errors,
        {.strict_water = true, .strict_water_valid = &valid}));
    EXPECT_TRUE(valid);
    ASSERT_NE(restored.find_chunk({0, 0, 0}), nullptr);
    EXPECT_TRUE(restored.find_chunk({0, 0, 0})->same_water_state(chunk));
}

TEST(RegionalWaterPersistence, StrictWaterReportsMalformedStructuresBeforeInsertion) {
    namespace P = Luminumbra::Persistence;
    WorldStreamingState original;
    const auto chunk = original.get_or_create_chunk({0, 0, 0});
    FillWater(*chunk, 4);
    for (const auto& bytes : {P::SerializeWorldStreamingStateSnapshotJson(original),
                              P::SerializeChunkAuthoritySnapshotJson(*chunk)}) {
        const auto snapshot = nlohmann::json::parse(bytes);
        for (const char* field : {"water_state",
                                  "water_depth_mm",
                                  "water_bed_mm",
                                  "water_edge_flux",
                                  "water_level_data",
                                  "water_flow_data",
                                  "water_sim_terrain_height"}) {
            for (const auto& invalid : {nlohmann::json(nullptr),
                                        nlohmann::json(false),
                                        nlohmann::json(1),
                                        nlohmann::json(1.5),
                                        nlohmann::json("invalid"),
                                        nlohmann::json::array(),
                                        nlohmann::json::object()}) {
                // Empty flux and render mirrors are legal, independent writer shapes.
                const std::string_view name(field);
                if (invalid.is_array() && invalid.empty() &&
                    (name == "water_edge_flux" || name == "water_level_data" ||
                     name == "water_flow_data" || name == "water_sim_terrain_height"))
                    continue;
                SCOPED_TRACE(field);
                SCOPED_TRACE(invalid.dump());
                auto malformed = snapshot;
                malformed["chunks"][0][field] = invalid;
                WorldStreamingState restored;
                restored.get_or_create_chunk({1, 0, 0});
                std::vector<std::string> errors;
                bool valid = true;
                EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(
                    malformed.dump(),
                    restored,
                    errors,
                    {.strict_water = true, .strict_water_valid = &valid}));
                EXPECT_FALSE(valid);
                EXPECT_TRUE(restored.empty());
                ASSERT_FALSE(errors.empty());
                EXPECT_NE(errors.front().find("Saved water"), std::string::npos);
            }
        }
    }
}

TEST(RegionalWaterPersistence, StrictWaterRefusesPartialRetainedPairsRegardlessOfActivity) {
    namespace P = Luminumbra::Persistence;
    for (const bool active : {false, true}) {
        for (const int resolution : {0, 4, 8}) {
            for (const bool partial_flux : {false, true}) {
                SCOPED_TRACE(active);
                SCOPED_TRACE(resolution);
                SCOPED_TRACE(partial_flux);
                WorldStreamingState original;
                const auto chunk = original.get_or_create_chunk({0, 0, 0});
                FillWater(*chunk, 4);
                chunk->has_water_sim.store(active);
                chunk->current_water_resolution.store(resolution);
                chunk->water_depth_mm.pop_back();
                if (partial_flux) {
                    chunk->water_bed_mm.clear();
                    chunk->water_edge_flux = {2, 3, 4};
                }
                for (const auto& bytes : {P::SerializeWorldStreamingStateSnapshotJson(original),
                                          P::SerializeChunkRecordPayloadJson(*chunk)}) {
                    WorldStreamingState restored;
                    std::vector<std::string> errors;
                    EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(
                        bytes, restored, errors, {.strict_water = true}));
                    EXPECT_TRUE(restored.empty());
                    EXPECT_FALSE(errors.empty());
                }
            }
        }
    }
}

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

ActiveRegionLedger PlanningLedger(std::initializer_list<RegionKey> owners,
                                  world::RegionSchedulerConfig config = {}) {
    ActiveRegionLedger ledger(config);
    for (const auto owner : owners) {
        ledger.mark_edited(owner, WorldClock{});
    }
    return ledger;
}

constexpr std::uint64_t kPlanningCapacity = 2 * 64 + 4 * 8;

void ExpectInteriorCreditConserved(const ActiveRegionLedger& ledger, const Planning::Plan& plan) {
    const auto old_debt =
        std::accumulate(ledger.water_debts().begin(),
                        ledger.water_debts().end(),
                        std::uint64_t{0},
                        [](std::uint64_t sum, const auto& debt) { return sum + debt.second; });
    const auto new_debt =
        std::accumulate(plan.regions.begin(),
                        plan.regions.end(),
                        std::uint64_t{0},
                        [](std::uint64_t sum, const auto& region) { return sum + region.debt; });
    const auto demand =
        std::accumulate(plan.regions.begin(),
                        plan.regions.end(),
                        std::uint64_t{0},
                        [](auto sum, const auto& region) { return sum + region.candidates; });
    EXPECT_EQ(new_debt + plan.allocated_windows * world::water_share::kQ32,
              old_debt + std::min(plan.global_windows - plan.border_windows, demand) *
                             world::water_share::kQ32);
    EXPECT_EQ(plan.allocated_windows, plan.interiors.size());
}

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

TEST(RegionalWaterPlanning, InteriorAndSeamCadenceExcludeUnavailableOwners) {
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
    const auto off_phase = Planning::Build(residents, {}, ledger, schedule, inputs);
    EXPECT_TRUE(off_phase.faces.empty());
    EXPECT_TRUE(off_phase.interiors.empty());
    EXPECT_TRUE(off_phase.participants.empty());
    schedule.tick = 3;
    const auto due = Planning::Build(residents, {}, ledger, schedule, inputs);
    EXPECT_EQ(due.interiors, (std::vector<ChunkID>{residents[0].id, residents[1].id}));
    ASSERT_EQ(due.faces.size(), 1u);
    EXPECT_EQ(due.faces[0].common_shift, 2u);
    EXPECT_EQ(due.participants, due.interiors);
}

TEST(RegionalWaterPlanning, EveryFaceRequiresBothPersistedPhasesAndScheduleMembership) {
    struct Cadences {
        std::uint8_t left_shift, right_shift;
        std::uint32_t left_phase, right_phase;
    };
    for (const auto cadence : {Cadences{0, 0, 0, 0},
                               Cadences{1, 1, 1, 1},
                               Cadences{1, 1, 0, 1},
                               Cadences{2, 1, 3, 1},
                               Cadences{1, 2, 1, 2},
                               Cadences{16, 16, 65535, 65535}}) {
        auto ledger = PlanningLedger({{0, 0}, {1, 0}});
        auto& records =
            const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records());
        records.at({0, 0}).cadence_shift = cadence.left_shift;
        records.at({0, 0}).cadence_phase = cadence.left_phase;
        records.at({1, 0}).cadence_shift = cadence.right_shift;
        records.at({1, 0}).cadence_phase = cadence.right_phase;
        const auto before = ledger.encode();
        const std::vector residents{ResidentAt(30), ResidentAt(31), ResidentAt(32), ResidentAt(33)};
        for (const std::uint64_t tick : {std::uint64_t{0},
                                         std::uint64_t{1},
                                         std::uint64_t{2},
                                         std::uint64_t{3},
                                         std::uint64_t{4},
                                         std::uint64_t{5},
                                         std::uint64_t{6},
                                         std::uint64_t{7},
                                         std::uint64_t{65535},
                                         WorldClock::kTickLimit - 1}) {
            for (unsigned int membership = 0; membership < 4; ++membership) {
                SCOPED_TRACE(::testing::Message()
                             << static_cast<unsigned int>(cadence.left_shift) << ','
                             << static_cast<unsigned int>(cadence.right_shift) << ','
                             << cadence.left_phase << ',' << cadence.right_phase << ',' << tick
                             << ',' << membership);
                RegionSchedule schedule;
                schedule.tick = tick;
                if ((membership & 1u) != 0) {
                    schedule.due.push_back({0, 0});
                }
                if ((membership & 2u) != 0) {
                    schedule.due.push_back({1, 0});
                }
                const bool left_due =
                    (membership & 1u) != 0 &&
                    tick % (std::uint64_t{1} << cadence.left_shift) == cadence.left_phase;
                const bool right_due =
                    (membership & 2u) != 0 &&
                    tick % (std::uint64_t{1} << cadence.right_shift) == cadence.right_phase;
                const auto plan = Planning::Build(
                    residents, {}, ledger, schedule, {.cell_budget = 10 * kPlanningCapacity});
                const auto owner_count =
                    static_cast<std::size_t>(left_due) + static_cast<std::size_t>(right_due);
                EXPECT_EQ(plan.interiors.size(), 2 * owner_count);
                EXPECT_EQ(plan.regions.size(), owner_count);
                EXPECT_EQ(plan.faces.size(), owner_count + (left_due && right_due ? 1u : 0u));
                EXPECT_EQ(plan.border_windows, left_due && right_due ? 2u : 0u);
                EXPECT_EQ(plan.allocated_windows, plan.interiors.size());
                EXPECT_EQ(plan.participants, plan.interiors);
                for (const auto& face : plan.faces) {
                    EXPECT_TRUE(face.owner.x == 0 ? left_due : right_due);
                    EXPECT_TRUE(face.neighbor.x == 0 ? left_due : right_due);
                }
                EXPECT_LE(plan.total_cells, 10 * kPlanningCapacity);
                EXPECT_EQ(ledger.encode(), before);
            }
        }
    }
}

TEST(RegionalWaterPlanning, ThreeWindowsServeEveryEligibleSeamWithinItsFaceCountTicks) {
    for (const int seam_count : {1, 2, 3, 5}) {
        for (const bool is_x : {false, true}) {
            auto ledger = PlanningLedger({});
            RegionSchedule schedule;
            std::vector<Planning::Resident> residents;
            for (int seam = 0; seam < seam_count; ++seam) {
                for (const int coordinate : {64 * seam + 31, 64 * seam + 32}) {
                    const RegionKey owner =
                        is_x ? RegionKey{coordinate / 32, 0} : RegionKey{0, coordinate / 32};
                    ledger.mark_edited(owner, WorldClock{});
                    schedule.due.push_back(owner);
                    residents.push_back(is_x ? ResidentAt(coordinate) : ResidentAt(0, coordinate));
                }
            }
            const auto count = static_cast<std::uint64_t>(seam_count);
            for (const std::uint64_t windows : {0u, 1u, 2u, 3u, 4u, 5u, 12u}) {
                std::vector<Planning::Face> served;
                std::uint64_t selected_interiors = 0;
                const auto funded = std::min(count, windows == 0 ? 0 : (windows - 1) / 2);
                for (std::uint64_t step = 0; step < 3 * count; ++step) {
                    // Start near the tick limit to cover a large non-zero rotation offset.
                    schedule.tick = WorldClock::kTickLimit - 3 * count + step;
                    SCOPED_TRACE(::testing::Message() << seam_count << ',' << is_x << ',' << windows
                                                      << ',' << schedule.tick);
                    const Planning::Inputs inputs{.cell_budget = windows * kPlanningCapacity,
                                                  .raining = true};
                    const auto before = ledger.encode();
                    const auto plan = Planning::Build(residents, {}, ledger, schedule, inputs);
                    EXPECT_EQ(plan.faces.size(), funded);
                    EXPECT_EQ(plan.border_windows, 2 * funded);
                    // The reserved interior budget credits owners fractionally;
                    // selections consume whole Q32 units only after they accrue.
                    ExpectInteriorCreditConserved(ledger, plan);
                    selected_interiors += plan.allocated_windows;
                    EXPECT_LE(plan.border_windows + plan.allocated_windows, windows);
                    EXPECT_LE(plan.total_cells, inputs.cell_budget);
                    EXPECT_EQ(plan.residents_visited, residents.size());
                    EXPECT_TRUE(std::is_sorted(plan.faces.begin(), plan.faces.end()));
                    for (const auto& face : plan.faces) {
                        EXPECT_NE(face.owner, face.neighbor);
                        EXPECT_EQ(face.is_x, is_x);
                        EXPECT_TRUE(std::binary_search(
                            plan.participants.begin(), plan.participants.end(), face.positive));
                        EXPECT_TRUE(std::binary_search(
                            plan.participants.begin(), plan.participants.end(), face.other));
                        served.push_back(face);
                    }
                    std::reverse(residents.begin(), residents.end());
                    std::reverse(schedule.due.begin(), schedule.due.end());
                    EXPECT_EQ(Planning::Build(residents, {}, ledger, schedule, inputs), plan);
                    EXPECT_EQ(ledger.encode(), before);
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
                    if ((step + 1) % count == 0) {
                        std::sort(served.begin(), served.end());
                        served.erase(std::unique(served.begin(), served.end()), served.end());
                        EXPECT_EQ(served.size(), funded == 0 ? 0 : count);
                        served.clear();
                    }
                }
                EXPECT_EQ(selected_interiors == 0, windows == 0);
            }
        }
    }
}

TEST(RegionalWaterPlanning, ReducedCadenceRotatesEverySeamAcrossServiceOpportunities) {
    for (const int seam_count : {2, 3, 5}) {
        for (const bool is_x : {false, true}) {
            for (const std::uint8_t shift : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{16}}) {
                const auto period = std::uint64_t{1} << shift;
                for (const auto phase : {std::uint64_t{0}, period - 1}) {
                    auto ledger = PlanningLedger({});
                    RegionSchedule schedule;
                    std::vector<Planning::Resident> residents;
                    for (int seam = 0; seam < seam_count; ++seam) {
                        for (const int coordinate : {64 * seam + 31, 64 * seam + 32}) {
                            const RegionKey owner = is_x ? RegionKey{coordinate / 32, 0}
                                                         : RegionKey{0, coordinate / 32};
                            ledger.mark_edited(owner, WorldClock{});
                            schedule.due.push_back(owner);
                            residents.push_back(is_x ? ResidentAt(coordinate)
                                                     : ResidentAt(0, coordinate));
                        }
                    }
                    auto& records = const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(
                        ledger.records());
                    for (int seam = 0; seam < seam_count; ++seam) {
                        auto& record = records.at(schedule.due[static_cast<std::size_t>(2 * seam)]);
                        record.state = RegionState::Reduced;
                        record.cadence_shift = shift;
                        record.cadence_phase = static_cast<std::uint32_t>(phase);
                    }
                    const auto before = ledger.encode();
                    const auto count = static_cast<std::uint64_t>(seam_count);
                    for (const auto first :
                         {std::uint64_t{0}, (WorldClock::kTickLimit >> shift) - count}) {
                        std::vector<Planning::Face> served;
                        for (std::uint64_t opportunity = first; opportunity < first + count;
                             ++opportunity) {
                            schedule.tick = (opportunity << shift) + phase;
                            SCOPED_TRACE(::testing::Message()
                                         << seam_count << ',' << is_x << ','
                                         << static_cast<unsigned int>(shift) << ',' << phase << ','
                                         << schedule.tick);
                            const Planning::Inputs inputs{.cell_budget = 3 * kPlanningCapacity};
                            const auto plan =
                                Planning::Build(residents, {}, ledger, schedule, inputs);
                            ASSERT_EQ(plan.faces.size(), 1u);
                            EXPECT_EQ(plan.border_windows, 2u);
                            EXPECT_EQ(plan.faces[0].common_shift, shift);
                            EXPECT_LE(plan.total_cells, inputs.cell_budget);
                            served.push_back(plan.faces[0]);
                            std::reverse(residents.begin(), residents.end());
                            std::reverse(schedule.due.begin(), schedule.due.end());
                            EXPECT_EQ(Planning::Build(residents, {}, ledger, schedule, inputs),
                                      plan);
                            schedule.tick ^= 1u;
                            EXPECT_TRUE(Planning::Build(residents, {}, ledger, schedule, inputs)
                                            .faces.empty());
                        }
                        std::sort(served.begin(), served.end());
                        served.erase(std::unique(served.begin(), served.end()), served.end());
                        EXPECT_EQ(served.size(), count);
                    }
                    EXPECT_EQ(ledger.encode(), before);
                }
            }
        }
    }
}

TEST(RegionalWaterPlanning, ReloadPreservesOddCadenceSeamRotation) {
    auto ledger = PlanningLedger({{0, 0}, {1, 0}, {2, 0}, {3, 0}});
    auto& records = const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records());
    for (const RegionKey owner : {RegionKey{0, 0}, RegionKey{2, 0}}) {
        auto& record = records.at(owner);
        record.state = RegionState::Reduced;
        record.cadence_shift = 1;
        record.cadence_phase = ActiveRegionLedger::phase(owner, 1);
        ASSERT_EQ(record.cadence_phase, 1u);
    }
    ActiveRegionLedger reloaded;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), reloaded, error)) << error;
    const std::vector residents{ResidentAt(31), ResidentAt(32), ResidentAt(95), ResidentAt(96)};
    RegionSchedule schedule;
    schedule.due = {{0, 0}, {1, 0}, {2, 0}, {3, 0}};
    for (const auto first : {std::uint64_t{1}, WorldClock::kTickLimit - 3}) {
        std::vector<Planning::Face> served;
        for (const auto tick : {first, first + 2}) {
            schedule.tick = tick;
            const Planning::Inputs inputs{.cell_budget = 3 * kPlanningCapacity};
            const auto plan = Planning::Build(residents, {}, ledger, schedule, inputs);
            EXPECT_EQ(Planning::Build(residents, {}, reloaded, schedule, inputs), plan);
            ASSERT_EQ(plan.faces.size(), 1u);
            served.push_back(plan.faces[0]);
        }
        EXPECT_NE(served[0], served[1]);
    }
}

TEST(RegionalWaterPlanning, IneligibleAndSameOwnerFacesDoNotConsumeBorderRotationSlots) {
    const auto baseline_ledger = PlanningLedger({{0, 0}, {1, 0}, {2, 0}, {3, 0}});
    const std::vector baseline{ResidentAt(31), ResidentAt(32), ResidentAt(95), ResidentAt(96)};
    for (int fault = 0; fault < 8; ++fault) {
        SCOPED_TRACE(fault);
        auto ledger = PlanningLedger({{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}});
        auto& records =
            const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records());
        auto residents = baseline;
        residents.push_back(ResidentAt(fault == 0 ? 160 : 159));
        residents.push_back(ResidentAt(fault == 0 ? 161 : 160));
        RegionSchedule schedule;
        schedule.due = {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}};
        switch (fault) {
            case 0: // One reserved window cannot select both same-owner interiors.
                break;
            case 1:
                residents[4].initialized = false;
                break;
            case 2:
                residents[4].awake = residents[5].awake = false;
                break;
            case 3:
                schedule.due.pop_back();
                break;
            case 4:
                records.at({5, 0}).state = RegionState::Frozen;
                break;
            case 5:
                residents[4].initialized = false;
                residents[4].preparation_ready = false;
                break;
            case 6:
                residents[4].available = false;
                break;
            case 7:
                records.erase({5, 0});
                break;
        }
        for (std::uint64_t tick = 0; tick < 4; ++tick) {
            schedule.tick = tick;
            RegionSchedule baseline_schedule;
            baseline_schedule.tick = tick;
            baseline_schedule.due = {{0, 0}, {1, 0}, {2, 0}, {3, 0}};
            const Planning::Inputs inputs{.cell_budget = 3 * kPlanningCapacity};
            const auto expected =
                Planning::Build(baseline, {}, baseline_ledger, baseline_schedule, inputs);
            const auto plan = Planning::Build(residents, {}, ledger, schedule, inputs);
            EXPECT_EQ(plan.faces, expected.faces);
            EXPECT_EQ(plan.border_windows, 2u);
            EXPECT_EQ(plan.allocated_windows, 0u);
            const auto credited = std::accumulate(
                plan.regions.begin(),
                plan.regions.end(),
                std::uint64_t{0},
                [](std::uint64_t sum, const auto& region) { return sum + region.debt; });
            EXPECT_EQ(credited, world::water_share::kQ32);
            EXPECT_LE(plan.total_cells, inputs.cell_budget);
        }
    }
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

TEST(RegionalWaterPlanning, SameOwnerGridServesEveryFaceWithinTwiceItsFaceCountThroughReload) {
    constexpr std::uint64_t face_count = 10, plan_bound = 2 * face_count;
    for (const std::uint64_t windows : {2u, 4u}) {
        for (const std::uint8_t shift : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{16}}) {
            for (const auto first :
                 {std::uint64_t{0}, (WorldClock::kTickLimit >> shift) - plan_bound}) {
                auto ledger =
                    PlanningLedger({{0, 0}}, {.reduced_shift = std::max(std::uint8_t{1}, shift)});
                auto& record =
                    const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records())
                        .at({0, 0});
                record.state = shift == 0 ? RegionState::Active : RegionState::Reduced;
                record.cadence_shift = shift;
                record.cadence_phase = ActiveRegionLedger::phase({0, 0}, shift);
                const auto phase = record.cadence_phase;
                ledger.set_water_cursor({0, 0}, std::numeric_limits<std::uint64_t>::max());
                ActiveRegionLedger initial_reload;
                std::string initial_error;
                ASSERT_TRUE(
                    ActiveRegionLedger::decode(ledger.encode(), initial_reload, initial_error))
                    << initial_error;
                std::vector<Planning::Resident> residents;
                for (int z = 0; z < 2; ++z) {
                    for (int x = 0; x < 4; ++x) {
                        residents.push_back(ResidentAt(x, z));
                    }
                }
                std::vector<Planning::Face> served;
                RegionSchedule schedule;
                schedule.due = {{0, 0}};
                const Planning::Inputs inputs{.cell_budget = windows * kPlanningCapacity};
                for (std::uint64_t step = 0; step < plan_bound; ++step) {
                    schedule.tick = ((first + step) << shift) + phase;
                    SCOPED_TRACE(::testing::Message()
                                 << windows << ',' << static_cast<unsigned int>(shift) << ','
                                 << schedule.tick);
                    const auto before = ledger.encode();
                    const auto plan = Planning::Build(residents, {}, ledger, schedule, inputs);
                    EXPECT_EQ(plan.border_windows, 0u);
                    EXPECT_EQ(plan.interiors.size(), windows);
                    EXPECT_LE(plan.total_cells, inputs.cell_budget);
                    ExpectInteriorCreditConserved(ledger, plan);
                    for (const auto& face : plan.faces) {
                        EXPECT_EQ(face.owner, face.neighbor);
                        EXPECT_TRUE(std::binary_search(
                            plan.interiors.begin(), plan.interiors.end(), face.positive));
                        EXPECT_TRUE(std::binary_search(
                            plan.interiors.begin(), plan.interiors.end(), face.other));
                        served.push_back(face);
                    }
                    std::reverse(residents.begin(), residents.end());
                    EXPECT_EQ(Planning::Build(residents, {}, ledger, schedule, inputs), plan);
                    EXPECT_EQ(ledger.encode(), before);
                    ASSERT_EQ(plan.regions.size(), 1u);
                    ledger.set_water_cursor({0, 0}, plan.regions[0].next_cursor);
                    ActiveRegionLedger::WaterDebts debts;
                    if (plan.regions[0].debt != 0) {
                        debts.emplace(RegionKey{0, 0}, plan.regions[0].debt);
                    }
                    ASSERT_TRUE(ledger.set_water_debts(debts));
                    ActiveRegionLedger reloaded;
                    std::string error;
                    ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), reloaded, error))
                        << error;
                    EXPECT_EQ(Planning::Build(residents, {}, reloaded, schedule, inputs),
                              Planning::Build(residents, {}, ledger, schedule, inputs));
                    ledger = std::move(reloaded);
                }
                std::sort(served.begin(), served.end());
                served.erase(std::unique(served.begin(), served.end()), served.end());
                EXPECT_EQ(served.size(), face_count);
                EXPECT_EQ(std::count_if(served.begin(),
                                        served.end(),
                                        [](const auto& face) { return !face.is_x; }),
                          4);
            }
        }
    }
}

TEST(RegionalWaterPlanning, ActivePairCannotStarveOddPhaseReducedOwnerThroughReload) {
    constexpr auto unit = world::water_share::kQ32;
    constexpr unsigned int due_bound = 2;
    for (const auto saved :
         {std::uint64_t{0}, unit / 2, 4 * unit, world::water_share::kMaxDebt - unit}) {
        for (const auto first : {std::uint64_t{1}, WorldClock::kTickLimit - 5}) {
            auto ledger = PlanningLedger({{0, 0}, {1, 0}});
            auto& record =
                const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records())
                    .at({0, 0});
            record.state = RegionState::Reduced;
            record.cadence_shift = 1;
            record.cadence_phase = ActiveRegionLedger::phase({0, 0}, 1);
            ASSERT_EQ(record.cadence_phase, 1u);
            if (saved != 0) {
                ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, saved}}));
            }
            std::vector residents{ResidentAt(0), ResidentAt(34), ResidentAt(35)};
            const auto reduced_chunk = residents[0].id;
            RegionSchedule schedule;
            schedule.due = {{0, 0}, {1, 0}};
            unsigned int due_without_service = 0;
            for (std::uint64_t step = 0; step < 5; ++step) {
                schedule.tick = first + step;
                SCOPED_TRACE(::testing::Message() << saved << ',' << schedule.tick);
                const auto before = ledger.encode();
                const Planning::Inputs inputs{.cell_budget = 2 * kPlanningCapacity};
                const auto plan = Planning::Build(residents, {}, ledger, schedule, inputs);
                EXPECT_EQ(plan.border_windows, 0u);
                EXPECT_LE(plan.allocated_windows, 2u);
                EXPECT_LE(plan.total_cells, inputs.cell_budget);
                const bool served =
                    std::binary_search(plan.interiors.begin(), plan.interiors.end(), reduced_chunk);
                if (step % 2 == 0) {
                    ASSERT_EQ(plan.regions.size(), 2u);
                    ExpectInteriorCreditConserved(ledger, plan);
                    due_without_service = served ? 0 : due_without_service + 1;
                    EXPECT_LT(due_without_service, due_bound);
                } else {
                    EXPECT_FALSE(served);
                    ASSERT_EQ(plan.regions.size(), 1u);
                    EXPECT_EQ(plan.regions[0].owner, (RegionKey{1, 0}));
                }
                std::reverse(residents.begin(), residents.end());
                std::reverse(schedule.due.begin(), schedule.due.end());
                EXPECT_EQ(Planning::Build(residents, {}, ledger, schedule, inputs), plan);
                EXPECT_EQ(ledger.encode(), before);
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
                ActiveRegionLedger reloaded;
                std::string error;
                ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), reloaded, error)) << error;
                EXPECT_EQ(Planning::Build(residents, {}, reloaded, schedule, inputs),
                          Planning::Build(residents, {}, ledger, schedule, inputs));
                ledger = std::move(reloaded);
            }
        }
    }
}

TEST(RegionalWaterPlanning, EveryGrantedOwnerRotatesPairsOnItsOwnCadence) {
    constexpr std::uint64_t face_count = 3, plan_bound = 2 * face_count;
    for (const auto first : {std::uint64_t{1}, WorldClock::kTickLimit - 11}) {
        auto ledger = PlanningLedger({{0, 0}, {1, 0}});
        auto& record = const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records())
                           .at({0, 0});
        record.state = RegionState::Reduced;
        record.cadence_shift = 1;
        record.cadence_phase = ActiveRegionLedger::phase({0, 0}, 1);
        ASSERT_EQ(record.cadence_phase, 1u);
        std::vector residents{ResidentAt(0),
                              ResidentAt(1),
                              ResidentAt(2),
                              ResidentAt(3),
                              ResidentAt(34),
                              ResidentAt(35),
                              ResidentAt(36),
                              ResidentAt(37)};
        RegionSchedule schedule;
        schedule.due = {{0, 0}, {1, 0}};
        std::vector<Planning::Face> served;
        for (std::uint64_t step = 0; step < plan_bound; ++step) {
            schedule.tick = first + 2 * step;
            SCOPED_TRACE(schedule.tick);
            const Planning::Inputs inputs{.cell_budget = 4 * kPlanningCapacity};
            const auto plan = Planning::Build(residents, {}, ledger, schedule, inputs);
            ASSERT_EQ(plan.regions.size(), 2u);
            EXPECT_EQ(plan.border_windows, 0u);
            EXPECT_EQ(plan.allocated_windows, 4u);
            EXPECT_LE(plan.total_cells, inputs.cell_budget);
            ExpectInteriorCreditConserved(ledger, plan);
            for (const auto& region : plan.regions) {
                EXPECT_EQ(region.windows, 2u);
                EXPECT_EQ(region.debt, 0u);
                ledger.set_water_cursor(region.owner, region.next_cursor);
            }
            for (const auto& face : plan.faces) {
                EXPECT_EQ(face.owner, face.neighbor);
                served.push_back(face);
            }
            std::reverse(residents.begin(), residents.end());
            ActiveRegionLedger reloaded;
            std::string error;
            ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), reloaded, error)) << error;
            ledger = std::move(reloaded);
        }
        std::sort(served.begin(), served.end());
        served.erase(std::unique(served.begin(), served.end()), served.end());
        EXPECT_EQ(served.size(), 2 * face_count);
    }
}

TEST(RegionalWaterPlanning, PairReservationsKeepIsolatedCandidatesAndFilterIneligibleEndpoints) {
    auto ledger = PlanningLedger({{0, 0}});
    std::vector residents{
        ResidentAt(0), ResidentAt(1), ResidentAt(4), ResidentAt(5), ResidentAt(8)};
    residents[2].awake = residents[3].awake = false;
    RegionSchedule schedule;
    schedule.due = {{0, 0}};
    std::map<ChunkID, unsigned int> visits;
    for (std::uint64_t tick = 0; tick < 12; ++tick) {
        schedule.tick = tick;
        const auto plan = Planning::Build(
            residents, {}, ledger, schedule, {.cell_budget = 2 * kPlanningCapacity});
        ExpectInteriorCreditConserved(ledger, plan);
        EXPECT_EQ(plan.interiors.size(), 2u);
        if (tick % 2 != 0) {
            ASSERT_EQ(plan.faces.size(), 1u);
            EXPECT_EQ(plan.faces[0].positive, residents[0].id);
            EXPECT_EQ(plan.faces[0].other, residents[1].id);
        }
        for (const auto id : plan.interiors) {
            ++visits[id];
        }
        ASSERT_EQ(plan.regions.size(), 1u);
        EXPECT_EQ(plan.regions[0].debt, 0u);
        ledger.set_water_cursor({0, 0}, plan.regions[0].next_cursor);
    }
    EXPECT_GT(visits[residents[0].id], 0u);
    EXPECT_GT(visits[residents[1].id], 0u);
    EXPECT_GT(visits[residents[4].id], 0u);
    EXPECT_EQ(visits[residents[2].id], 0u);
    EXPECT_EQ(visits[residents[3].id], 0u);

    schedule.tick = 1;
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, world::water_share::kMaxDebt}}));
    const auto before = ledger.encode();
    EXPECT_THROW(static_cast<void>(Planning::Build(
                     residents, {}, ledger, schedule, {.cell_budget = 2 * kPlanningCapacity})),
                 std::invalid_argument);
    EXPECT_EQ(ledger.encode(), before);
}

TEST(RegionalWaterPlanning, ZeroCandidateOwnerCannotSpendSavedDebtAheadOfEligibleOwner) {
    constexpr auto saved = world::water_share::kMaxDebt;
    for (const bool sleeping : {false, true}) {
        auto ledger = PlanningLedger({{0, 0}, {1, 0}});
        ledger.set_water_cursor({0, 0}, std::numeric_limits<std::uint64_t>::max());
        ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, saved}}));
        RegionSchedule schedule;
        schedule.due = {{0, 0}, {1, 0}};
        std::vector residents{ResidentAt(34)};
        if (sleeping) {
            residents.push_back(ResidentAt(0));
            residents.back().awake = false;
        }
        const auto before = ledger.encode();
        const Planning::Inputs inputs{.cell_budget = kPlanningCapacity};
        const auto plan = Planning::Build(residents, {}, ledger, schedule, inputs);
        EXPECT_EQ(plan.interiors, (std::vector<ChunkID>{residents[0].id}));
        EXPECT_EQ(plan.allocated_windows, 1u);
        ExpectInteriorCreditConserved(ledger, plan);
        ASSERT_EQ(plan.regions.size(), 2u);
        EXPECT_EQ(plan.regions[0].candidates, 0u);
        EXPECT_EQ(plan.regions[0].windows, 0u);
        EXPECT_EQ(plan.regions[0].debt, saved);
        EXPECT_EQ(plan.regions[0].next_cursor, std::numeric_limits<std::uint64_t>::max());
        EXPECT_EQ(plan.regions[1].debt, 0u);
        std::reverse(residents.begin(), residents.end());
        std::reverse(schedule.due.begin(), schedule.due.end());
        EXPECT_EQ(Planning::Build(residents, {}, ledger, schedule, inputs), plan);
        EXPECT_EQ(ledger.encode(), before);
    }
}

TEST(RegionalWaterPlanning, FundedPairSpendsOwnerGrantAndRetainsUnselectedCredit) {
    auto ledger = PlanningLedger({{0, 0}, {1, 0}});
    constexpr auto saved = world::water_share::kMaxDebt - 2 * world::water_share::kQ32;
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, saved}}));
    const std::vector residents{ResidentAt(0), ResidentAt(1), ResidentAt(34), ResidentAt(36)};
    RegionSchedule schedule;
    schedule.tick = 1;
    schedule.due = {{0, 0}, {1, 0}};
    const auto before = ledger.encode();
    const auto plan =
        Planning::Build(residents, {}, ledger, schedule, {.cell_budget = 3 * kPlanningCapacity});
    EXPECT_EQ(plan.border_windows, 0u);
    EXPECT_EQ(plan.allocated_windows, 2u);
    ASSERT_EQ(plan.faces.size(), 1u);
    ASSERT_EQ(plan.regions.size(), 2u);
    EXPECT_EQ(plan.regions[0].windows, 2u);
    EXPECT_EQ(plan.regions[0].debt, saved - world::water_share::kQ32 / 2);
    EXPECT_EQ(plan.regions[1].windows, 0u);
    EXPECT_EQ(plan.regions[1].debt, 3 * world::water_share::kQ32 / 2);
    ExpectInteriorCreditConserved(ledger, plan);
    EXPECT_EQ(ledger.encode(), before);
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

TEST(RegionalWaterPlanning, SourceCellsFloorWorldCoordinatesBeforeSubtractingChunkOrigins) {
    struct AxisCase {
        float position;
        int chunk, local_metres; // -1 denotes the cell immediately below zero.
    };
    constexpr AxisCase cases[] = {{-1e-16f, -1, -1},
                                  {-0.0f, 0, 0},
                                  {0.0f, 0, 0},
                                  {1e-16f, 0, 0},
                                  {-16.0f, -1, 0},
                                  {16.0f, 1, 0},
                                  {-16777216.0f, -1048576, 0},
                                  {16777215.0f, 1048575, 15}};
    for (const int resolution : {4, 8, 16, 32}) {
        for (const auto& x : cases) {
            for (const auto& z : cases) {
                SCOPED_TRACE(::testing::Message()
                             << resolution << ',' << x.position << ',' << z.position);
                const auto owner =
                    ActiveRegionLedger::region_at({static_cast<float>(x.chunk * CHUNK_SIZE_X),
                                                   0.0f,
                                                   static_cast<float>(z.chunk * CHUNK_SIZE_Z)});
                const auto ledger = PlanningLedger({owner});
                auto resident = ResidentAt(x.chunk, z.chunk);
                resident.resolution = resolution;
                resident.awake = false;
                const std::vector residents{resident};
                const std::vector sources{SourceAt(1, {x.position, 0.0f, z.position})};
                RegionSchedule schedule;
                schedule.due = {owner};
                const auto side = static_cast<std::uint64_t>(resolution);
                const auto plan = Planning::Build(
                    residents,
                    sources,
                    ledger,
                    schedule,
                    {.resolution = resolution, .cell_budget = 2 * side * side + 4 * side});
                ASSERT_EQ(plan.sources.size(), 1u);
                EXPECT_EQ(plan.sources[0].target, resident.id);
                const int cell_x = x.local_metres < 0 ? resolution - 1
                                                      : x.local_metres * resolution / CHUNK_SIZE_X;
                const int cell_z = z.local_metres < 0 ? resolution - 1
                                                      : z.local_metres * resolution / CHUNK_SIZE_Z;
                EXPECT_EQ(plan.sources[0].cell,
                          static_cast<std::size_t>(cell_z * resolution + cell_x));
                EXPECT_EQ(plan.interiors, (std::vector<ChunkID>{resident.id}));
            }
        }
    }
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
                               Vec3(0, 33554432, 0),
                               Vec3(0, std::numeric_limits<float>::max(), 0),
                               Vec3(0, -std::numeric_limits<float>::max(), 0)}) {
        const std::vector sources{SourceAt(1, invalid)};
        EXPECT_THROW(
            static_cast<void>(Planning::Build(residents, sources, ledger, schedule, inputs)),
            std::invalid_argument);
    }
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity()}) {
        for (const Vec3 position :
             {Vec3(invalid, 0, 0), Vec3(0, invalid, 0), Vec3(0, 0, invalid)}) {
            const std::vector sources{SourceAt(1, position)};
            EXPECT_THROW(
                static_cast<void>(Planning::Build(residents, sources, ledger, schedule, inputs)),
                std::invalid_argument);
            EXPECT_THROW(static_cast<void>(Planning::Build({}, sources, ledger, schedule, inputs)),
                         std::invalid_argument);
        }
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
    EXPECT_EQ(empty.allocated_windows, 0u);
    EXPECT_EQ(empty.regions[0].windows, 0u);
    EXPECT_TRUE(empty.participants.empty());
    EXPECT_EQ(empty.regions[0].next_cursor, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(empty.regions[0].debt, 2 * world::water_share::kQ32);
    EXPECT_TRUE(Planning::Build(residents, {}, ledger, {}, {.cell_budget = kPlanningCapacity})
                    .interiors.empty());
}

TEST(RegionalWaterPlanning, DebtDrivenExcessGrantsRemainUnallocatedAndCarryUnservedDebt) {
    auto ledger = PlanningLedger({{0, 0}, {1, 0}});
    constexpr auto unit = world::water_share::kQ32;
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, 4 * unit}}));
    const auto before = ledger.encode();
    std::vector residents{ResidentAt(0), ResidentAt(32), ResidentAt(34), ResidentAt(36)};
    RegionSchedule schedule;
    schedule.due = {{0, 0}, {1, 0}};
    const Planning::Inputs inputs{.cell_budget = 4 * kPlanningCapacity};
    const auto plan = Planning::Build(residents, {}, ledger, schedule, inputs);
    ASSERT_EQ(plan.regions.size(), 2u);
    EXPECT_EQ(plan.regions[0].candidates, 1u);
    EXPECT_EQ(plan.regions[0].windows, 1u);
    EXPECT_EQ(plan.regions[0].debt, 4 * unit);
    EXPECT_EQ(plan.regions[1].windows, 0u);
    EXPECT_EQ(plan.regions[1].debt, 3 * unit);
    for (const auto& region : plan.regions) {
        const auto selected =
            std::count_if(plan.interiors.begin(), plan.interiors.end(), [&region](ChunkID id) {
                return Chunk::decode_id(id).x / 32 == region.owner.x;
            });
        EXPECT_EQ(region.windows, static_cast<std::uint64_t>(selected));
    }
    EXPECT_EQ(plan.allocated_windows, plan.interiors.size());
    EXPECT_EQ(plan.global_windows - plan.border_windows - plan.allocated_windows, 3u);
    EXPECT_EQ(plan.regions[0].debt + plan.regions[1].debt + plan.allocated_windows * unit,
              4 * unit + plan.global_windows * unit);
    std::reverse(residents.begin(), residents.end());
    std::reverse(schedule.due.begin(), schedule.due.end());
    EXPECT_EQ(Planning::Build(residents, {}, ledger, schedule, inputs), plan);
    EXPECT_EQ(ledger.encode(), before);
}

TEST(RegionalWaterPlanning, MaximumUnusedCapacityNeverAccumulatesDebtAcrossPublishedPlans) {
    auto ledger = PlanningLedger({{0, 0}});
    const std::vector residents{ResidentAt(0)};
    RegionSchedule schedule;
    schedule.due = {{0, 0}};
    const Planning::Inputs inputs{.cell_budget =
                                      world::water_share::kMaxWindowBudget * kPlanningCapacity};
    for (unsigned int call = 0; call < 4096; ++call) {
        SCOPED_TRACE(call);
        const auto before = ledger.encode();
        Planning::Plan plan;
        ASSERT_NO_THROW(plan = Planning::Build(residents, {}, ledger, schedule, inputs));
        EXPECT_EQ(ledger.encode(), before);
        ASSERT_EQ(plan.regions.size(), 1u);
        EXPECT_EQ(plan.interiors, (std::vector<ChunkID>{residents[0].id}));
        EXPECT_EQ(plan.global_windows, world::water_share::kMaxWindowBudget);
        EXPECT_EQ(plan.border_windows, 0u);
        EXPECT_EQ(plan.regions[0].debt, 0u);
        ExpectInteriorCreditConserved(ledger, plan);
        ledger.set_water_cursor(plan.regions[0].owner, plan.regions[0].next_cursor);
        ActiveRegionLedger::WaterDebts debts;
        if (plan.regions[0].debt != 0) {
            debts.emplace(plan.regions[0].owner, plan.regions[0].debt);
        }
        ASSERT_TRUE(ledger.set_water_debts(debts));
    }
}

TEST(RegionalWaterPlanning, StarvedCandidateCarriesSavedDebtUntilItsWindowIsFunded) {
    auto ledger = PlanningLedger({{0, 0}, {1, 0}});
    const std::vector residents{ResidentAt(0), ResidentAt(34)};
    RegionSchedule schedule;
    schedule.due = {{0, 0}, {1, 0}};
    constexpr auto unit = world::water_share::kQ32;
    const Planning::Inputs inputs{.cell_budget = kPlanningCapacity};
    for (unsigned int call = 0; call < 3; ++call) {
        SCOPED_TRACE(call);
        const auto before = ledger.encode();
        const auto plan = Planning::Build(residents, {}, ledger, schedule, inputs);
        ASSERT_EQ(plan.regions.size(), 2u);
        ExpectInteriorCreditConserved(ledger, plan);
        EXPECT_EQ(plan.regions[0].debt, call == 1 ? 0 : unit / 2);
        EXPECT_EQ(plan.regions[1].debt, call == 1 ? unit : unit / 2);
        if (call == 0) {
            EXPECT_TRUE(plan.interiors.empty());
        } else {
            EXPECT_EQ(plan.interiors, (std::vector<ChunkID>{residents[call - 1].id}));
        }
        EXPECT_EQ(ledger.encode(), before);
        ActiveRegionLedger::WaterDebts debts;
        for (const auto& region : plan.regions) {
            if (region.debt != 0) {
                debts.emplace(region.owner, region.debt);
            }
            ledger.set_water_cursor(region.owner, region.next_cursor);
        }
        ASSERT_TRUE(ledger.set_water_debts(debts));
        const auto encoded = ledger.encode();
        ActiveRegionLedger reloaded;
        std::string error;
        ASSERT_TRUE(ActiveRegionLedger::decode(encoded, reloaded, error)) << error;
        ledger = std::move(reloaded);
    }
}

TEST(RegionalWaterPlanning, CandidateCapsPreserveFractionalAndMaximumDebtAcrossBudgets) {
    constexpr auto unit = world::water_share::kQ32;
    for (const std::uint64_t count : {0u, 1u, 3u}) {
        std::vector<Planning::Resident> residents;
        for (std::uint64_t index = 0; index < count; ++index) {
            residents.push_back(ResidentAt(static_cast<int>(2 * index)));
        }
        for (const std::uint64_t windows : {std::uint64_t{0},
                                            std::uint64_t{1},
                                            std::uint64_t{2},
                                            std::uint64_t{4},
                                            world::water_share::kMaxWindowBudget}) {
            const auto credit = std::min(count, windows) * unit;
            for (const auto saved :
                 {std::uint64_t{0}, unit / 2, 4 * unit, world::water_share::kMaxDebt - credit}) {
                SCOPED_TRACE(::testing::Message() << count << ',' << windows << ',' << saved);
                auto ledger = PlanningLedger({{0, 0}});
                if (saved != 0) {
                    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, saved}}));
                }
                ledger.set_water_cursor({0, 0}, std::numeric_limits<std::uint64_t>::max());
                const auto before = ledger.encode();
                RegionSchedule schedule;
                schedule.due = {{0, 0}};
                const auto plan = Planning::Build(
                    residents, {}, ledger, schedule, {.cell_budget = windows * kPlanningCapacity});
                ASSERT_EQ(plan.regions.size(), 1u);
                const auto selected = std::min(count, windows);
                EXPECT_EQ(plan.interiors.size(), selected);
                EXPECT_EQ(plan.regions[0].windows, selected);
                EXPECT_EQ(plan.allocated_windows, plan.interiors.size());
                EXPECT_EQ(plan.regions[0].debt, saved + credit - selected * unit);
                EXPECT_EQ(plan.global_windows - plan.border_windows - plan.allocated_windows,
                          windows - selected);
                if (selected == 0) {
                    EXPECT_EQ(plan.regions[0].next_cursor,
                              std::numeric_limits<std::uint64_t>::max());
                } else {
                    EXPECT_EQ(plan.regions[0].next_cursor,
                              (std::numeric_limits<std::uint64_t>::max() % count + selected) %
                                  count);
                }
                EXPECT_EQ(ledger.encode(), before);
            }
        }
    }
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

std::shared_ptr<Chunk>
TransactionChunkAt(const IVec3& coords, std::int32_t depth = 1000, int resolution = 8) {
    auto chunk = std::make_shared<Chunk>(coords);
    FillWater(*chunk, resolution, depth);
    const auto cells = chunk->water_depth_mm.size();
    chunk->water_flow_data.assign(cells, Vec2(0.0f));
    chunk->water_sim_terrain_height.assign(cells, 1.0f);
    chunk->water_rest_level.assign(cells, 0.0f);
    chunk->water_src_mm.assign(cells, 0);
    return chunk;
}

std::shared_ptr<Chunk> TransactionChunk(int x, std::int32_t depth = 1000, int resolution = 8) {
    return TransactionChunkAt({x, 0, 0}, depth, resolution);
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

TEST(RegionalWaterTransaction, InitializationDoesNotPublishDurableWater) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    auto ledger = PlanningLedger({{0, 0}});
    const auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
    const std::vector chunks{chunk};
    Planning::Transaction stage;
    stage.Begin(ledger, 0, chunks, {});
    stage.PrepareGrid(chunk->get_id(), shield, 8);
    stage.Inject({.target = chunk->get_id(), .cell = 0, .millimetres = 0});
    stage.Publish(&shield);
    EXPECT_TRUE(chunk->has_complete_water_state());
    EXPECT_FALSE(chunk->has_water_authority());
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    EXPECT_EQ(chunk->water_revision(), 0u);
}

TEST(RegionalWaterTransaction, MirrorRefreshAndUnownedCalmTimeDoNotPublishDurableWater) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    for (const bool owned : {false, true}) {
        SCOPED_TRACE(owned);
        auto ledger = PlanningLedger({{0, 0}});
        const auto chunk = TransactionChunk(0, 0);
        if (owned) {
            chunk->set_water_authority();
        }
        chunk->clear_voxel_data_dirty();
        const std::vector chunks{chunk};
        Planning::Transaction stage;
        stage.Begin(ledger, 0, chunks, {});
        ASSERT_TRUE(
            stage.StepInterior(chunk->get_id(), shield, {.finite_hydrology = true}).applied);
        if (!owned) {
            stage.FinishInterior(chunk->get_id());
        }
        stage.Publish(&shield);
        EXPECT_EQ(chunk->water_level_data, std::vector<float>(64, 1.0f));
        EXPECT_EQ(chunk->ticks_below_threshold, owned ? 0 : 1);
        EXPECT_EQ(chunk->has_water_authority(), owned);
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        EXPECT_EQ(chunk->water_revision(), 0u);
    }
}

TEST(RegionalWaterTransaction, GenuineMutationsPublishAndRoundTripAfterCleanPreparation) {
    namespace P = Luminumbra::Persistence;
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    // Depth, flux-only decay, sleep, wake, and resolution each independently claim ownership.
    for (int mutation = 0; mutation < 5; ++mutation) {
        SCOPED_TRACE(mutation);
        auto ledger = PlanningLedger({{0, 0}});
        const auto chunk = TransactionChunk(0, 0);
        const std::vector chunks{chunk};
        {
            Planning::Transaction clean;
            clean.Begin(ledger, 0, chunks, {});
            ASSERT_TRUE(
                clean.StepInterior(chunk->get_id(), shield, {.finite_hydrology = true}).applied);
            clean.Publish(&shield);
        }
        ASSERT_FALSE(chunk->has_water_authority());
        ASSERT_EQ(chunk->water_revision(), 0u);
        if (mutation == 1) {
            chunk->water_edge_flux[0] = 1;
        }
        if (mutation == 2) {
            chunk->ticks_below_threshold = 119;
        }
        if (mutation == 3) {
            chunk->is_water_sleeping.store(true);
            chunk->max_water_delta_last_tick = 1.0f;
        }
        Planning::Transaction stage;
        stage.Begin(ledger, 0, chunks, {});
        if (mutation == 0) {
            stage.Inject({.target = chunk->get_id(), .cell = 0, .millimetres = 7});
        } else if (mutation == 1) {
            ASSERT_TRUE(
                stage.StepInterior(chunk->get_id(), shield, {.finite_hydrology = true}).applied);
            EXPECT_EQ(stage.staged(chunk->get_id())->water_depth_mm, chunk->water_depth_mm);
        } else if (mutation == 4) {
            stage.PrepareGrid(chunk->get_id(), shield, 16);
        } else {
            stage.FinishInterior(chunk->get_id());
        }
        stage.Publish(&shield);
        EXPECT_TRUE(chunk->has_water_authority());
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        EXPECT_EQ(chunk->water_revision(), 1u);
        WorldStreamingState restored;
        std::vector<std::string> errors;
        ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(
            P::SerializeChunkRecordPayloadJson(*chunk), restored, errors, {.strict_water = true}));
        const auto actual = restored.find_chunk(chunk->get_coords());
        ASSERT_NE(actual, nullptr);
        EXPECT_TRUE(actual->same_water_state(*chunk));
    }
}

TEST(RegionalWaterTransaction, GeneratedPlansPreserveCappedCreditAndZeroCandidateDebt) {
    constexpr auto unit = world::water_share::kQ32;
    for (const auto windows : {std::uint64_t{0},
                               std::uint64_t{1},
                               std::uint64_t{2},
                               world::water_share::kMaxWindowBudget}) {
        SCOPED_TRACE(windows);
        auto ledger = PlanningLedger({{0, 0}, {1, 0}, {2, 0}});
        ASSERT_TRUE(ledger.set_water_debts(
            {{{0, 0}, unit / 2}, {{1, 0}, world::water_share::kMaxDebt}, {{2, 0}, 31}}));
        ledger.set_water_cursor({1, 0}, std::numeric_limits<std::uint64_t>::max());
        auto sleeping = ResidentAt(32);
        sleeping.awake = false;
        const std::vector residents{ResidentAt(0), ResidentAt(1), sleeping};
        RegionSchedule schedule;
        schedule.due = {{0, 0}, {1, 0}};
        const auto plan = Planning::Build(
            residents, {}, ledger, schedule, {.cell_budget = windows * kPlanningCapacity});
        ASSERT_EQ(plan.regions.size(), 2u);
        EXPECT_EQ(plan.regions[0].windows, std::min(windows, std::uint64_t{2}));
        EXPECT_EQ(plan.regions[0].debt, unit / 2);
        EXPECT_EQ(plan.regions[1].candidates, 0u);
        EXPECT_EQ(plan.regions[1].debt, world::water_share::kMaxDebt);
        const auto before = ledger.encode();
        std::vector<std::shared_ptr<Chunk>> chunks;
        for (const auto id : plan.participants) {
            chunks.push_back(TransactionChunk(Chunk::decode_id(id).x));
        }
        Planning::Transaction stage;
        stage.Begin(ledger, schedule.tick, chunks, plan.regions);
        EXPECT_EQ(stage.participants_visited(), plan.participants.size());
        EXPECT_EQ(ledger.encode(), before);
        stage.Publish();
        for (const auto& region : plan.regions) {
            EXPECT_EQ(ledger.records().at(region.owner).water_cursor, region.next_cursor);
            EXPECT_EQ(ledger.water_debts().at(region.owner), region.debt);
        }
        EXPECT_EQ(ledger.water_debts().at({2, 0}), 31u);
        EXPECT_EQ(ledger.records().at({1, 0}).water_cursor,
                  std::numeric_limits<std::uint64_t>::max());
    }
}

TEST(RegionalWaterTransaction, GeneratedCadencePlanPublishesFundedPairsAndBorders) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    auto ledger = PlanningLedger({{0, 0}, {1, 0}});
    auto& records = const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records());
    for (auto& [owner, record] : records) {
        static_cast<void>(owner);
        record.cadence_shift = 1;
        record.cadence_phase = 1;
    }
    static_cast<void>(ledger.schedule(WorldClock(3)));
    RegionSchedule schedule;
    schedule.tick = 3;
    schedule.due = {{0, 0}, {1, 0}};
    const std::vector residents{ResidentAt(0), ResidentAt(1), ResidentAt(31), ResidentAt(32)};
    const auto plan =
        Planning::Build(residents, {}, ledger, schedule, {.cell_budget = 6 * kPlanningCapacity});
    EXPECT_EQ(plan.border_windows, 2u);
    EXPECT_EQ(plan.allocated_windows, 4u);
    ASSERT_EQ(plan.interiors.size(), 4u);
    ASSERT_EQ(plan.faces.size(), 2u);
    std::vector<std::shared_ptr<Chunk>> chunks;
    for (const auto id : plan.participants) {
        chunks.push_back(TransactionChunk(Chunk::decode_id(id).x));
    }
    Planning::Transaction stage;
    stage.Begin(ledger, schedule.tick, chunks, plan.regions);
    for (const auto id : plan.interiors) {
        ASSERT_TRUE(stage.StepInterior(id, shield, {}).applied);
    }
    for (const auto& face : plan.faces) {
        ASSERT_TRUE(stage.StepFace(face.positive, face.other, face.is_x).applied);
    }
    stage.Publish();
    for (const auto& region : plan.regions) {
        EXPECT_EQ(ledger.records().at(region.owner).water_cursor, region.next_cursor);
        EXPECT_EQ(region.debt, 0u);
        EXPECT_FALSE(ledger.water_debts().contains(region.owner));
    }
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
    // Adoption preserves Unloaded; explicitly admit this generated terrain for meshing.
    chunks[0]->set_state(ChunkState::Idle);
    ASSERT_TRUE(shield.adopt_streamed_chunk(chunks[0]));
    entt::registry registry;
    TransactionMeshGate gate(jobs, shield);
    shield.begin_tick(0);
    shield.update(registry, Vec3(8.0f), nullptr, false);
    ASSERT_EQ(chunks[0]->get_state(), ChunkState::Meshing);
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

TEST(RegionalWaterTransaction, MaximumLegalDepthsConserveAcrossResolutionsAndAccountingSigns) {
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    constexpr auto footprint = std::int64_t{4} * CHUNK_SIZE_X * CHUNK_SIZE_Z;
    constexpr auto max_chunk_volume = maximum * footprint;
    // Even the participant cap at maximum depths stays representable. Check via
    // division so this assertion cannot itself overflow if the caps change.
    EXPECT_LE(
        Planning::kMaxResidents,
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / max_chunk_volume));
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    for (const int resolution : {4, 8, 16, 32}) {
        SCOPED_TRACE(resolution);
        auto ledger = PlanningLedger({{0, 0}});
        const std::vector chunks{TransactionChunk(0, maximum, resolution),
                                 TransactionChunk(1, maximum, resolution)};
        const auto before = TransactionBytes(*chunks[0]);
        const auto ledger_before = ledger.encode();
        Planning::Transaction stage;
        stage.Begin(ledger, 0, chunks, {});
        const auto evaporated = stage.StepInterior(chunks[0]->get_id(), shield, {.evap_mm = 1});
        ASSERT_TRUE(evaporated.applied);
        const auto cells = static_cast<std::int64_t>(resolution) * resolution;
        EXPECT_EQ(evaporated.depth_before, maximum * cells);
        EXPECT_EQ(evaporated.depth_after, (maximum - 1) * cells);
        EXPECT_EQ(evaporated.sink_mm, cells);
        const auto rained = stage.StepInterior(chunks[0]->get_id(), shield, {.rain_mm = 1});
        ASSERT_TRUE(rained.applied);
        EXPECT_EQ(rained.source_mm, cells);
        EXPECT_EQ(rained.depth_after, maximum * cells);
        EXPECT_EQ(TransactionBytes(*chunks[0]), before);
        EXPECT_EQ(ledger.encode(), ledger_before);
        ASSERT_NO_THROW(stage.Publish());
        EXPECT_FALSE(stage.pending());
        EXPECT_EQ(Mass(*chunks[0]) + Mass(*chunks[1]), std::int64_t{2} * maximum * cells);
        EXPECT_EQ(ledger.encode(), ledger_before);
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

namespace {
RegionSchedule ExecutionSchedule(std::uint64_t tick, std::initializer_list<RegionKey> due) {
    RegionSchedule schedule;
    schedule.tick = tick;
    schedule.due = due;
    return schedule;
}

auto CompletionBytes(const std::vector<Luminumbra::world::RegionWork>& work) {
    std::vector<std::tuple<RegionKey, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
        result;
    for (const auto& item : work) {
        result.emplace_back(
            item.key, item.region_ticks, item.entity_updates, item.water_cells, item.page_updates);
    }
    return result;
}

TEST(RegionalWaterExecution, PublishesPlannedMassRegistersRevisionsAndOnlyParticipatingWork) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    WaterSystem water(nullptr, &shield);
    water.SetSimResolution(8);
    water.SetRegionalScheduling(true);
    auto ledger = PlanningLedger({{0, 0}, {1, 0}});
    for (std::int32_t x = 2; x < 258; ++x) {
        ledger.mark_edited({x, 0}, WorldClock{});
    }
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, 19}, {{1, 0}, 23}}));
    const std::vector chunks{TransactionChunk(0), TransactionChunk(1), TransactionChunk(32)};
    const RegionSchedule schedule = ExecutionSchedule(0, {{0, 0}, {1, 0}});
    const std::vector<Planning::Resident> residents{ResidentAt(0), ResidentAt(1), ResidentAt(32)};
    const auto plan =
        Planning::Build(residents, {}, ledger, schedule, {8, 2 * kPlanningCapacity, false});
    const auto before = Mass(*chunks[0]) + Mass(*chunks[1]) + Mass(*chunks[2]);
    std::vector<world::RegionWork> completed{{.key = {9, 0}, .entity_updates = 7}};
    entt::registry registry;
    water.UpdateRegional(registry, chunks, ledger, schedule, completed, 2 * kPlanningCapacity);
    EXPECT_EQ(Mass(*chunks[0]) + Mass(*chunks[1]) + Mass(*chunks[2]), before);
    EXPECT_EQ(water.regional_work().visits, plan.interiors);
    EXPECT_EQ(water.regional_work().residents_visited, chunks.size());
    EXPECT_EQ(water.regional_work().participants_visited, plan.participants.size());
    EXPECT_EQ(water.regional_work().total_cells, plan.total_cells);
    EXPECT_TRUE(water.dbg_mass_ok());
    EXPECT_EQ(water.cells_stepped_last_update(), plan.total_cells - plan.preparation_cells);
    for (const auto& patch : plan.regions) {
        EXPECT_EQ(ledger.records().at(patch.owner).water_cursor, patch.next_cursor);
        const auto debt = ledger.water_debts().find(patch.owner);
        EXPECT_EQ(debt == ledger.water_debts().end() ? 0 : debt->second, patch.debt);
    }
    for (const auto& chunk : chunks) {
        const bool selected =
            std::binary_search(plan.interiors.begin(), plan.interiors.end(), chunk->get_id());
        EXPECT_EQ(chunk->water_revision(), selected ? 1u : 0u);
        EXPECT_EQ(chunk->authority_hash_revision(), selected ? 1u : 0u);
        EXPECT_EQ(chunk->ticks_below_threshold, selected ? 1 : 0);
    }
    ASSERT_GE(completed.size(), 2u);
    EXPECT_EQ(completed.front().entity_updates, 7u);
    std::uint64_t cells = 0;
    for (std::size_t i = 1; i < completed.size(); ++i) {
        cells += completed[i].water_cells;
        EXPECT_EQ(completed[i].region_ticks, 1u);
    }
    EXPECT_EQ(cells, water.regional_work().water_cells);
    const auto bytes = ledger.encode();
    const auto published_work = water.regional_work();
    EXPECT_THROW(water.UpdateRegional(registry, chunks, ledger, schedule, completed),
                 std::invalid_argument);
    EXPECT_EQ(ledger.encode(), bytes);
    EXPECT_EQ(water.regional_work(), published_work);
}

TEST(RegionalWaterExecution, LateOverflowPreservesPublishedSleepCompletionAndTelemetry) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    WaterSystem water(nullptr, &shield);
    water.SetSimResolution(8);
    water.SetRegionalScheduling(true);
    auto ledger = PlanningLedger({{0, 0}, {1, 0}});
    const std::vector chunks{TransactionChunk(0), TransactionChunk(32)};
    chunks[0]->ticks_below_threshold = 118;
    entt::registry registry;
    std::vector<world::RegionWork> completed;
    water.UpdateRegional(
        registry, chunks, ledger, ExecutionSchedule(0, {{0, 0}, {1, 0}}), completed);
    static_cast<void>(ledger.schedule(WorldClock(1)));
    chunks[1]->water_depth_mm.assign(64, std::numeric_limits<std::int32_t>::max());
    water.SetHydrology(true, 1, 0);
    const auto a = TransactionBytes(*chunks[0]), b = TransactionBytes(*chunks[1]);
    const auto registers = ledger.encode();
    const auto work = water.regional_work();
    const auto completion = CompletionBytes(completed);
    const auto authority = shield.far_lod_authority_revision();
    const auto cells = water.cells_stepped_last_update();
    EXPECT_THROW(water.UpdateRegional(
                     registry, chunks, ledger, ExecutionSchedule(1, {{0, 0}, {1, 0}}), completed),
                 std::invalid_argument);
    EXPECT_EQ(TransactionBytes(*chunks[0]), a);
    EXPECT_EQ(TransactionBytes(*chunks[1]), b);
    EXPECT_EQ(ledger.encode(), registers);
    EXPECT_EQ(water.regional_work(), work);
    EXPECT_EQ(CompletionBytes(completed), completion);
    EXPECT_EQ(water.cells_stepped_last_update(), cells);
    EXPECT_EQ(water.dbg_last_source_mm(), work.source_mm);
    EXPECT_EQ(water.debug_source_injected_mm(), 0);
    EXPECT_EQ(shield.far_lod_authority_revision(), authority);
}

TEST(RegionalWaterExecution, PhysicalSourcesPublishOnceAndOverflowDiscardsInjection) {
    for (const bool overflow : {false, true}) {
        SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
        WaterSystem water(nullptr, &shield);
        water.SetSimResolution(8);
        water.SetRegionalScheduling(true);
        auto ledger = PlanningLedger({{0, 0}});
        const std::vector chunks{
            TransactionChunk(0, overflow ? std::numeric_limits<std::int32_t>::max() : 1000)};
        chunks[0]->is_water_sleeping.store(true);
        entt::registry registry;
        const auto entity = registry.create();
        registry.emplace<Components::TransformComponent>(
            entity, Components::TransformComponent{.position = Vec3(1.0f, 100.0f, 1.0f)});
        registry.emplace<Components::FlowingWaterSourceComponent>(
            entity, Components::FlowingWaterSourceComponent{.flow_rate = 1.0f});
        std::vector<world::RegionWork> completed;
        const auto before = TransactionBytes(*chunks[0]);
        const auto mass = Mass(*chunks[0]);
        const RegionSchedule schedule = ExecutionSchedule(0, {{0, 0}});
        if (overflow) {
            EXPECT_THROW(water.UpdateRegional(registry, chunks, ledger, schedule, completed),
                         std::invalid_argument);
            EXPECT_EQ(TransactionBytes(*chunks[0]), before);
            EXPECT_TRUE(completed.empty());
            EXPECT_EQ(water.debug_sources_seen(), 0);
            EXPECT_EQ(water.debug_source_injected_mm(), 0);
        } else {
            water.UpdateRegional(registry, chunks, ledger, schedule, completed);
            EXPECT_EQ(Mass(*chunks[0]) - mass, 8);
            EXPECT_EQ(water.debug_sources_seen(), 1);
            EXPECT_EQ(water.debug_source_injected_mm(), 8);
            EXPECT_EQ(water.dbg_last_source_mm(), 8);
            EXPECT_THROW(water.UpdateRegional(registry, chunks, ledger, schedule, completed),
                         std::invalid_argument);
            EXPECT_EQ(Mass(*chunks[0]) - mass, 8);
            static_cast<void>(ledger.schedule(WorldClock(1)));
            water.UpdateRegional(
                registry, chunks, ledger, ExecutionSchedule(1, {{0, 0}}), completed);
            EXPECT_EQ(Mass(*chunks[0]) - mass, 16);
            EXPECT_EQ(water.debug_sources_seen(), 2);
            EXPECT_EQ(water.debug_source_injected_mm(), 16);
        }
    }
}

TEST(RegionalWaterExecution, GridPreparationPreservesIntegerCornersAndAccountsChangedVolume) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    for (const int resolution : {4, 8, 16, 32}) {
        auto ledger = PlanningLedger({{0, 0}});
        const std::vector chunks{TransactionChunk(0, 1000, 8)};
        chunks[0]->water_depth_mm.front() = 2000;
        chunks[0]->water_bed_mm.back() = -7000;
        const auto before = TransactionBytes(*chunks[0]);
        Planning::Transaction stage;
        stage.Begin(ledger, 0, chunks, {});
        stage.PrepareGrid(chunks[0]->get_id(), shield, resolution);
        const auto* grid = stage.staged(chunks[0]->get_id());
        ASSERT_NE(grid, nullptr);
        EXPECT_EQ(grid->water_depth_mm.front(), 2000);
        EXPECT_EQ(grid->water_bed_mm.back(), -7000);
        EXPECT_EQ(grid->water_depth_mm.size(), static_cast<std::size_t>(resolution * resolution));
        EXPECT_EQ(TransactionBytes(*chunks[0]), before);
        stage.Publish(&shield);
        EXPECT_EQ(chunks[0]->current_water_resolution.load(), resolution);
    }
    WaterSystem water(nullptr, &shield);
    water.SetSimResolution(8);
    water.SetRegionalScheduling(true);
    auto ledger = PlanningLedger({{0, 0}});
    const std::vector chunks{std::make_shared<Chunk>(IVec3(0, 0, 0)), TransactionChunk(2, 1000, 4)};
    entt::registry registry;
    std::vector<world::RegionWork> completed;
    water.UpdateRegional(registry, chunks, ledger, ExecutionSchedule(0, {{0, 0}}), completed);
    EXPECT_EQ(water.regional_work().preparation_cells, 128u);
    EXPECT_EQ(water.regional_work().volume_before_qmm +
                  water.regional_work().preparation_volume_qmm +
                  water.regional_work().source_volume_qmm,
              water.regional_work().volume_after_qmm + water.regional_work().sink_volume_qmm);
    for (const auto& chunk : chunks) {
        EXPECT_TRUE(chunk->has_complete_water_state());
    }
}

TEST(RegionalWaterExecution, MeshInvalidationsPublishWithWaterAndRefusalRetainsExistingWork) {
    // Calm, below the interval, at the interval, resized, and newly initialized grids.
    for (int mode = 0; mode < 5; ++mode) {
        for (const bool refuse : {false, true}) {
            SCOPED_TRACE(mode);
            SCOPED_TRACE(refuse);
            SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
            WaterSystem water(nullptr, &shield);
            water.SetSimResolution(8);
            water.SetRegionalScheduling(true);
            water.SetHydrology(true, mode == 0 ? 0 : 3, 0);
            auto ledger = PlanningLedger({{0, 0}});
            auto first = mode == 4 ? std::make_shared<Chunk>(IVec3(0, 0, 0))
                                   : TransactionChunk(0, 1000, mode == 3 ? 4 : 8);
            first->water_mesh_generated.store(true);
            first->water_mesh_dirty_ticks = mode == 2 ? 14 : 0;
            const std::vector chunks{
                first,
                TransactionChunk(2, refuse ? std::numeric_limits<std::int32_t>::max() : 1000)};
            chunks[1]->water_mesh_dirty_ticks = 14;
            if (refuse && mode == 0) {
                chunks[1]->water_bed_mm.assign(64, std::numeric_limits<std::int32_t>::min());
            }
            // Keep unrelated work and an already queued participant through both outcomes.
            const auto unrelated = Chunk::calculate_id({100, 0, 0});
            water.retain_mesh_invalidation(unrelated);
            water.retain_mesh_invalidation(chunks[1]->get_id());
            const auto invalidations = water.mesh_invalidations();
            const auto before_first = TransactionBytes(*first);
            const auto before_second = TransactionBytes(*chunks[1]);
            const auto registers = ledger.encode();
            entt::registry registry;
            std::vector<world::RegionWork> completed;
            if (refuse) {
                EXPECT_THROW(
                    water.UpdateRegional(
                        registry, chunks, ledger, ExecutionSchedule(0, {{0, 0}}), completed),
                    std::invalid_argument);
                EXPECT_EQ(TransactionBytes(*first), before_first);
                EXPECT_EQ(TransactionBytes(*chunks[1]), before_second);
                EXPECT_EQ(ledger.encode(), registers);
                EXPECT_TRUE(completed.empty());
                EXPECT_EQ(water.mesh_invalidations(), invalidations);
            } else {
                water.UpdateRegional(
                    registry, chunks, ledger, ExecutionSchedule(0, {{0, 0}}), completed);
                EXPECT_EQ(water.regional_work().participants_visited, chunks.size());
                EXPECT_EQ(water.mesh_invalidations().contains(first->get_id()), mode >= 2);
                EXPECT_TRUE(water.mesh_invalidations().contains(unrelated));
                EXPECT_TRUE(water.mesh_invalidations().contains(chunks[1]->get_id()));
                EXPECT_EQ(water.mesh_invalidations().size(), mode >= 2 ? 3u : 2u);
                EXPECT_EQ(first->water_mesh_generated.load(), mode < 2);
            }
        }
    }
}

TEST(RegionalWaterTransaction, MeshInvalidationsSurviveFurtherStepsButNotRefusalOrRollback) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    for (int outcome = 0; outcome < 4; ++outcome) {
        SCOPED_TRACE(outcome);
        auto ledger = PlanningLedger({{0, 0}});
        const std::vector chunks{TransactionChunk(0)};
        chunks[0]->water_mesh_generated.store(true);
        chunks[0]->water_mesh_dirty_ticks = 14;
        std::unordered_set<ChunkID> invalidations{Chunk::calculate_id({100, 0, 0})};
        const auto before_invalidations = invalidations;
        Planning::Transaction stage;
        stage.Begin(ledger, 0, chunks, {});
        ASSERT_TRUE(
            stage.StepInterior(chunks[0]->get_id(), shield, {.rain_mm = 3}).water_mesh_invalidated);
        ASSERT_FALSE(stage.StepInterior(chunks[0]->get_id(), shield, {}).water_mesh_invalidated);
        stage.PrepareGrid(chunks[0]->get_id(), shield, 8);
        if (outcome == 0) {
            stage.Rollback();
        } else if (outcome == 1) {
            chunks[0]->mark_water_state_dirty();
        } else if (outcome == 2) {
            static_cast<void>(ledger.schedule(WorldClock(1)));
        } else {
            stage.Publish(nullptr, &invalidations);
            EXPECT_TRUE(invalidations.contains(chunks[0]->get_id()));
            EXPECT_TRUE(invalidations.contains(Chunk::calculate_id({100, 0, 0})));
            EXPECT_EQ(invalidations.size(), 2u);
            EXPECT_FALSE(chunks[0]->water_mesh_generated.load());
            EXPECT_FALSE(stage.pending());
            continue;
        }
        const auto before = TransactionBytes(*chunks[0]);
        EXPECT_THROW(stage.Publish(&shield, &invalidations), std::invalid_argument);
        EXPECT_EQ(invalidations, before_invalidations);
        EXPECT_EQ(TransactionBytes(*chunks[0]), before);
        EXPECT_FALSE(stage.pending());
    }
}

TEST(RegionalWaterExecution, SleepAndAuthorityIndexAdvanceOnlyOnPublishedInteriors) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    WaterSystem water(nullptr, &shield);
    water.SetSimResolution(8);
    water.SetRegionalScheduling(true);
    auto ledger = PlanningLedger({{0, 0}});
    const std::vector chunks{TransactionChunk(0)};
    chunks[0]->ticks_below_threshold = 119;
    entt::registry registry;
    std::vector<world::RegionWork> completed;
    water.UpdateRegional(registry, chunks, ledger, ExecutionSchedule(0, {{0, 0}}), completed);
    EXPECT_TRUE(chunks[0]->is_water_sleeping.load());
    EXPECT_EQ(chunks[0]->ticks_below_threshold, 120);
    EXPECT_EQ(shield.far_lod_authority_revision(), 1u);
    EXPECT_EQ(shield.far_lod_region_authority_revision(0, 0), 1u);
    EXPECT_EQ(shield.far_lod_region_authority_revision(-1, -1), 1u);
    EXPECT_EQ(shield.far_lod_region_authority_revision(9, 9), 0u);
    static_cast<void>(ledger.schedule(WorldClock(1)));
    water.UpdateRegional(registry, chunks, ledger, ExecutionSchedule(1, {{0, 0}}), completed);
    EXPECT_TRUE(water.regional_work().visits.empty());
    EXPECT_EQ(chunks[0]->water_revision(), 1u);
    EXPECT_EQ(shield.far_lod_authority_revision(), 1u);
}

TEST(RegionalWaterExecution, DisabledEntryPointLeavesLegacyFixturesByteIdentical) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    WaterSystem implicit(nullptr, &shield), explicit_off(nullptr, &shield);
    explicit_off.SetRegionalScheduling(false);
    implicit.SetSimResolution(8);
    explicit_off.SetSimResolution(8);
    const auto a = TransactionChunk(0), b = TransactionChunk(0);
    std::unordered_map<ChunkID, std::shared_ptr<Chunk>> left{{a->get_id(), a}},
        right{{b->get_id(), b}};
    entt::registry registry;
    auto ledger = PlanningLedger({{0, 0}});
    std::vector<world::RegionWork> completed;
    const std::vector resident{a};
    const auto before = TransactionBytes(*a);
    const auto registers = ledger.encode();
    implicit.UpdateRegional(registry, resident, ledger, ExecutionSchedule(0, {{0, 0}}), completed);
    EXPECT_EQ(TransactionBytes(*a), before);
    EXPECT_EQ(ledger.encode(), registers);
    EXPECT_TRUE(completed.empty());
    for (int tick = 0; tick < 3; ++tick) {
        implicit.update(registry, left);
        explicit_off.update(registry, right);
        EXPECT_EQ(TransactionBytes(*a), TransactionBytes(*b));
    }
}
} // namespace

namespace {
void CheckBorderOnlyExecution(bool is_x, bool overflow) {
    SCOPED_TRACE(is_x);
    SCOPED_TRACE(overflow);
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    WaterSystem water(nullptr, &shield);
    water.SetRegionalScheduling(true);
    water.SetSimResolution(8);
    const RegionKey other_owner = is_x ? RegionKey{1, 0} : RegionKey{0, 1};
    auto ledger = PlanningLedger({{0, 0}, other_owner});
    // Both owners must be due for the seam. Three windows fund the border
    // and one interior; saved credit makes that interior belong to {0,0}.
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, world::water_share::kQ32}}));
    const auto schedule = ExecutionSchedule(0, {{0, 0}, other_owner});
    constexpr auto budget = 3 * kPlanningCapacity;
    auto a = TransactionChunk(31, overflow ? std::numeric_limits<std::int32_t>::max() : 2000);
    auto b = TransactionChunk(32, overflow ? std::numeric_limits<std::int32_t>::max() : 1000);
    if (!is_x) {
        a = TransactionChunkAt({0, 0, 31},
                               overflow ? std::numeric_limits<std::int32_t>::max() : 2000);
        b = TransactionChunkAt({0, 0, 32},
                               overflow ? std::numeric_limits<std::int32_t>::max() : 1000);
    }
    if (overflow) {
        a->water_bed_mm.assign(64, std::numeric_limits<std::int32_t>::max());
        b->water_bed_mm.assign(64, 1);
    }
    const std::vector chunks{b, a};
    const std::vector<Planning::Resident> residents{{.id = b->get_id()}, {.id = a->get_id()}};
    const auto plan = Planning::Build(residents, {}, ledger, schedule, {8, budget, false});
    ASSERT_EQ(plan.border_windows, 2u);
    ASSERT_EQ(plan.interiors, std::vector<ChunkID>{a->get_id()});
    ASSERT_EQ(plan.faces.size(), 1u);
    entt::registry registry;
    std::vector<world::RegionWork> completed;
    const auto before_a = TransactionBytes(*a), before_b = TransactionBytes(*b);
    const auto registers = ledger.encode();
    const auto mass = Mass(*a) + Mass(*b);
    if (overflow) {
        // Prove this input reaches a late seam refusal after a valid interior.
        Planning::Transaction probe;
        probe.Begin(ledger, 0, chunks, plan.regions);
        ASSERT_TRUE(probe.StepInterior(a->get_id(), shield, {}).applied);
        EXPECT_THROW(probe.StepFace(a->get_id(), b->get_id(), is_x), std::invalid_argument);
        EXPECT_THROW(water.UpdateRegional(registry, chunks, ledger, schedule, completed, budget),
                     std::invalid_argument);
        EXPECT_EQ(TransactionBytes(*a), before_a);
        EXPECT_EQ(TransactionBytes(*b), before_b);
        EXPECT_EQ(ledger.encode(), registers);
        EXPECT_TRUE(completed.empty());
        EXPECT_EQ(shield.far_lod_authority_revision(), 0u);
    } else {
        // The border-only endpoint is one seam change short of its remesh interval.
        b->water_mesh_generated.store(true);
        b->water_mesh_dirty_ticks = 14;
        water.UpdateRegional(registry, chunks, ledger, schedule, completed, budget);
        EXPECT_EQ(Mass(*a) + Mass(*b), mass);
        EXPECT_FALSE(b->water_mesh_generated.load());
        EXPECT_EQ(b->water_mesh_dirty_ticks, 0);
        EXPECT_TRUE(water.mesh_invalidations().contains(b->get_id()));
        EXPECT_EQ(water.regional_work().border_windows, 2u);
        EXPECT_EQ(water.regional_work().water_cells, 80u);
        EXPECT_EQ(a->ticks_below_threshold, 1);
        EXPECT_EQ(b->ticks_below_threshold, 0);
        EXPECT_EQ(a->water_revision(), 1u);
        EXPECT_EQ(b->water_revision(), 1u);
        ASSERT_EQ(completed.size(), 2u);
        EXPECT_EQ(completed[0].water_cells, 72u);
        EXPECT_EQ(completed[1].water_cells, 8u);
    }
}

TEST(RegionalWaterExecution, BorderOnlyWorkConservesMassAndLateSeamRefusalRollsBackBothAxes) {
    for (const bool is_x : {false, true}) {
        for (const bool overflow : {false, true}) {
            CheckBorderOnlyExecution(is_x, overflow);
        }
    }
}

TEST(RegionalWaterExecution, InvalidExecutionInputsPreserveAllPublishedOutputs) {
    for (int fault = 0; fault < 8; ++fault) {
        SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
        WaterSystem water(nullptr, fault == 5 ? nullptr : &shield);
        water.SetRegionalScheduling(true);
        water.SetSimResolution(8);
        auto ledger = PlanningLedger({{0, 0}});
        std::vector chunks{TransactionChunk(0)};
        auto schedule = ExecutionSchedule(0, {{0, 0}});
        if (fault == 0) {
            chunks.push_back(nullptr);
        } else if (fault == 1) {
            chunks.push_back(chunks.front());
        } else if (fault == 2) {
            water.SetHydrology(true, -1, 0);
        } else if (fault == 3) {
            schedule.tick = 1;
        } else if (fault == 4) {
            chunks[0]->water_flow_data.clear();
        } else if (fault == 7) {
            water.SetSimResolution(2);
        }
        const auto before = TransactionBytes(*chunks[0]);
        const auto registers = ledger.encode();
        entt::registry registry;
        if (fault == 6) {
            const auto entity = registry.create();
            registry.emplace<Components::TransformComponent>(entity);
            registry.emplace<Components::FlowingWaterSourceComponent>(
                entity, Components::FlowingWaterSourceComponent{.flow_rate = -1.0f});
        }
        std::vector<world::RegionWork> completed{{.key = {0, 0}, .page_updates = 2}};
        const auto completion = CompletionBytes(completed);
        const auto work = water.regional_work();
        EXPECT_THROW(water.UpdateRegional(registry, chunks, ledger, schedule, completed),
                     std::invalid_argument);
        EXPECT_EQ(TransactionBytes(*chunks[0]), before);
        EXPECT_EQ(ledger.encode(), registers);
        EXPECT_EQ(CompletionBytes(completed), completion);
        EXPECT_EQ(water.regional_work(), work);
    }
}

TEST(RegionalWaterExecution, EmptyZeroBudgetAndPausedCallsDoNotPublishWork) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    WaterSystem water(nullptr, &shield);
    water.SetRegionalScheduling(true);
    water.SetSimResolution(8);
    auto ledger = PlanningLedger({{0, 0}});
    const std::vector chunks{TransactionChunk(0)};
    const auto before = TransactionBytes(*chunks[0]);
    entt::registry registry;
    std::vector<world::RegionWork> completed;
    water.SetBootPaused(true);
    water.UpdateRegional(registry, chunks, ledger, ExecutionSchedule(0, {{0, 0}}), completed);
    EXPECT_EQ(TransactionBytes(*chunks[0]), before);
    water.SetBootPaused(false);
    water.UpdateRegional(registry, chunks, ledger, ExecutionSchedule(0, {{0, 0}}), completed, 0);
    EXPECT_EQ(TransactionBytes(*chunks[0]), before);
    EXPECT_TRUE(completed.empty());
    EXPECT_EQ(water.cells_stepped_last_update(), 0u);
    static_cast<void>(ledger.schedule(WorldClock(1)));
    water.UpdateRegional(registry, {}, ledger, ExecutionSchedule(1, {}), completed);
    EXPECT_TRUE(water.regional_work().visits.empty());
    EXPECT_EQ(water.regional_work().participants_visited, 0u);
}

TEST(RegionalWaterExecution, InvalidPreparationAndInjectionAbortPrivateStorage) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    for (int fault = 0; fault < 5; ++fault) {
        auto ledger = PlanningLedger({{0, 0}});
        const std::vector chunks{TransactionChunk(0)};
        const auto before = TransactionBytes(*chunks[0]);
        Planning::Transaction stage;
        stage.Begin(ledger, 0, chunks, {});
        if (fault == 0) {
            EXPECT_THROW(stage.PrepareGrid(chunks[0]->get_id(), shield, 3), std::invalid_argument);
        } else if (fault == 1) {
            EXPECT_THROW(stage.PrepareGrid(Chunk::calculate_id({1, 0, 0}), shield, 8),
                         std::invalid_argument);
        } else if (fault == 2) {
            EXPECT_THROW(stage.Inject({chunks[0]->get_id(), 64, 1}), std::invalid_argument);
        } else if (fault == 3) {
            EXPECT_THROW(stage.Inject({chunks[0]->get_id(), 0, -1}), std::invalid_argument);
        } else {
            EXPECT_THROW(stage.FinishInterior(Chunk::calculate_id({1, 0, 0})),
                         std::invalid_argument);
        }
        EXPECT_FALSE(stage.pending());
        EXPECT_EQ(TransactionBytes(*chunks[0]), before);
    }
}
} // namespace

namespace {
TEST(RegionalWaterExecution, AuthorityPreparationCanBeDiscardedAndRetainsUnrelatedIndexNodes) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    const Persistence::WorldSaveService::SaveOwnership ownership;
    shield.notify_far_lod_authority_changed({100, 0, 100});
    const auto revision = shield.far_lod_authority_revision();
    const auto unrelated = shield.far_lod_region_authority_revision(3, 3);
    ASSERT_EQ(unrelated, revision);
    const auto chunk = TransactionChunk(0);
    const std::array<Chunk*, 2> invalid{chunk.get(), nullptr};
    EXPECT_THROW(shield.prepare_water_authority_publication(invalid), std::invalid_argument);
    EXPECT_EQ(shield.far_lod_authority_revision(), revision);
    EXPECT_EQ(shield.far_lod_region_authority_revision(0, 0), 0u);
    {
        const std::array valid{chunk.get()};
        auto prepared = shield.prepare_water_authority_publication(valid);
        EXPECT_EQ(shield.far_lod_authority_revision(), revision);
        EXPECT_FALSE(prepared.revisions.empty());
    }
    EXPECT_EQ(shield.far_lod_region_authority_revision(0, 0), 0u);
    auto ledger = PlanningLedger({{0, 0}});
    Planning::Transaction stage;
    const std::array chunks{chunk};
    stage.Begin(ledger, 0, chunks, {});
    ASSERT_TRUE(stage.StepInterior(chunk->get_id(), shield, {.rain_mm = 1}).applied);
    stage.Publish(&shield);
    EXPECT_EQ(shield.far_lod_region_authority_revision(0, 0), revision + 1);
    EXPECT_EQ(shield.far_lod_region_authority_revision(3, 3), unrelated);
}
} // namespace

namespace {
TEST(RegionalWaterExecution, LatchedDurableReadFailureRefusesWithoutPublishing) {
    SHIELD_WorldSystem shield(nullptr, nullptr, TerrainGenParams{}, 1337);
    shield.set_durable_residency(true);
    shield.set_durable_chunk_loader([](const IVec3&) -> std::shared_ptr<Chunk> { return {}; }, {});
    shield.remember_durable_chunks({{1, 0, 0}});
    WaterSystem water(nullptr, &shield);
    water.SetRegionalScheduling(true);
    water.SetSimResolution(8);
    auto ledger = PlanningLedger({{0, 0}});
    const std::vector chunks{TransactionChunk(0)};
    const auto before = TransactionBytes(*chunks[0]);
    const auto registers = ledger.encode();
    Planning::Transaction stage;
    stage.Begin(ledger, 0, chunks, {});
    ASSERT_TRUE(stage.StepInterior(chunks[0]->get_id(), shield, {.rain_mm = 1}).applied);
    ASSERT_TRUE(shield.restore_authoritative_chunk({1, 0, 0}));
    ASSERT_TRUE(shield.durable_read_failed());
    EXPECT_THROW(stage.Publish(&shield), std::invalid_argument);
    EXPECT_FALSE(stage.pending());
    entt::registry registry;
    std::vector<world::RegionWork> completed;
    EXPECT_THROW(
        water.UpdateRegional(registry, chunks, ledger, ExecutionSchedule(0, {{0, 0}}), completed),
        std::invalid_argument);
    EXPECT_EQ(TransactionBytes(*chunks[0]), before);
    EXPECT_EQ(ledger.encode(), registers);
    EXPECT_TRUE(completed.empty());
    EXPECT_EQ(shield.far_lod_authority_revision(), 0u);
}
} // namespace

namespace {
TEST(RegionalWaterExecution, NonfiniteAndOutOfRangeSeedHeightsDiscardPreparedArrays) {
    for (const float height : {1.0e10f, std::numeric_limits<float>::infinity()}) {
        TerrainGenParams params;
        params.height_offset = height;
        params.island_mask_enabled = false;
        params.cliffs_enabled = false;
        params.rivers_enabled = false;
        params.lakes_enabled = false;
        SHIELD_WorldSystem shield(nullptr, nullptr, params, 1337);
        WaterSystem water(nullptr, &shield);
        water.SetRegionalScheduling(true);
        water.SetSimResolution(8);
        auto ledger = PlanningLedger({{0, 0}});
        const std::vector chunks{std::make_shared<Chunk>(IVec3(0, 0, 0))};
        const auto before = TransactionBytes(*chunks[0]);
        const auto registers = ledger.encode();
        entt::registry registry;
        std::vector<world::RegionWork> completed;
        EXPECT_THROW(water.UpdateRegional(
                         registry, chunks, ledger, ExecutionSchedule(0, {{0, 0}}), completed),
                     std::invalid_argument);
        EXPECT_EQ(TransactionBytes(*chunks[0]), before);
        EXPECT_EQ(ledger.encode(), registers);
        EXPECT_TRUE(completed.empty());
        EXPECT_EQ(shield.far_lod_authority_revision(), 0u);
    }
}
} // namespace
