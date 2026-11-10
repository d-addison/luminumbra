#include <gtest/gtest.h>

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
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
