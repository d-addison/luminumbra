#include <gtest/gtest.h>

#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <tuple>
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
        EXPECT_FALSE(errors.empty());
    }
    for (const char* field : {"water_depth_mm", "water_bed_mm"}) {
        auto missing = snapshot;
        missing["chunks"][0].erase(field);
        WorldStreamingState restored;
        std::vector<std::string> errors;
        EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(
            missing.dump(), restored, errors, {.strict_water = true}));
        EXPECT_EQ(restored.size(), 0u);
        EXPECT_FALSE(errors.empty());
    }
    const auto encoded = nlohmann::json::parse(P::SerializeChunkRecordPayloadJson(*chunk));
    for (const char* field : {"water_level_data", "water_flow_data", "water_sim_terrain_height"}) {
        for (const bool nonfinite : {false, true}) {
            auto malformed = encoded;
            if (nonfinite) {
                auto& data = malformed["chunks"][0][field]["data"].get_ref<std::string&>();
                // Little-endian positive infinity in the first float, with unchanged length.
                data.replace(0, 8, "AACAfwAA");
            } else {
                malformed["chunks"][0][field]["count"] = 15u;
            }
            WorldStreamingState restored;
            std::vector<std::string> errors;
            EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(
                malformed.dump(), restored, errors, {.strict_water = true}));
            EXPECT_EQ(restored.size(), 0u);
            EXPECT_FALSE(errors.empty());
        }
    }
}

TEST(RegionalWaterPersistence, StrictWaterPreservesWriterShapesAndContinuationValues) {
    namespace P = Luminumbra::Persistence;
    for (const int resolution : {0, 2, 4, 8, 16, 32}) {
        for (const bool encoded : {false, true}) {
            WorldStreamingState original;
            auto chunk = original.get_or_create_chunk({0, 0, 0});
            if (resolution != 0) {
                FillWater(*chunk, resolution);
            } else {
                // A dormant writer has no active grid shape to impose on retained integers.
                chunk->water_depth_mm = {123};
            }
            chunk->water_mesh_dirty_ticks = 17;
            chunk->ticks_below_threshold = -1;
            const auto bytes = encoded ? P::SerializeChunkRecordPayloadJson(*chunk)
                                       : P::SerializeWorldStreamingStateSnapshotJson(original);
            WorldStreamingState restored;
            std::vector<std::string> errors;
            ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(
                bytes, restored, errors, {.strict_water = true}))
                << (errors.empty() ? "" : errors.front());
            const auto actual = restored.find_chunk({0, 0, 0});
            ASSERT_NE(actual, nullptr);
            EXPECT_TRUE(actual->same_water_state(*chunk));
            EXPECT_EQ(actual->water_mesh_dirty_ticks, 17);
            EXPECT_EQ(actual->ticks_below_threshold, -1);
        }
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
            bool valid = false;
            const auto encoded = P::SerializeChunkRecordPayloadJson(*chunk, &valid);
            ASSERT_TRUE(valid);
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

TEST(RegionalWaterPersistence, StrictWaterPreservesMissingIntegerDefaultsForBothArrayEncodings) {
    namespace P = Luminumbra::Persistence;
    for (const bool encoded : {false, true}) {
        for (const bool missing_pair : {false, true}) {
            SCOPED_TRACE(encoded);
            SCOPED_TRACE(missing_pair);
            WorldStreamingState original;
            const auto chunk = original.get_or_create_chunk({0, 0, 0});
            FillWater(*chunk, 4);
            auto json = nlohmann::json::parse(
                encoded ? P::SerializeChunkRecordPayloadJson(*chunk)
                        : P::SerializeWorldStreamingStateSnapshotJson(original));
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
            EXPECT_EQ(actual->water_edge_flux,
                      std::vector<std::int32_t>(missing_pair ? 0u : 32u, 0));
        }
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

TEST(RegionalWaterPersistence, StrictWaterRefusesPartialRetainedPairsRegardlessOfActivity) {
    namespace P = Luminumbra::Persistence;
    for (const bool active : {false, true}) {
        for (const int resolution : {0, 4, 8}) {
            for (const bool partial_flux : {false, true}) {
                for (const bool encoded : {false, true}) {
                    SCOPED_TRACE(active);
                    SCOPED_TRACE(resolution);
                    SCOPED_TRACE(partial_flux);
                    SCOPED_TRACE(encoded);
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
                    const auto bytes = encoded
                                           ? P::SerializeChunkRecordPayloadJson(*chunk)
                                           : P::SerializeWorldStreamingStateSnapshotJson(original);
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

} // namespace
