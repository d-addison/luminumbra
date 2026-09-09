#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <unordered_map>

#include "../fixtures/smoke_artifact/FixedInputs.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_server/SimBudgetArtifact.h"
#include "luminumbra_server/modes/SmokeArtifact.h"

namespace {
using luminumbra::simulation::SimBudgetStage;
using luminumbra::simulation::SimBudgetTelemetry;

TEST(SimBudgetTelemetry, DisabledProductionArtifactMatchesDevelBytes) {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path output =
        fs::temp_directory_path() / ("smoke_artifact_" + std::to_string(stamp) + ".json");
    for (int scenario = 0; scenario != 3; ++scenario) {
        SCOPED_TRACE(scenario);
        ServerCliOptions options;
        Luminumbra::Server::SmokeRunResult first, replay;
        FixedSmokeInputs(scenario, options, first, replay);
        ASSERT_FALSE(options.sim_budget);
        ASSERT_FALSE(first.sim_budget.Enabled());
        ASSERT_FALSE(replay.sim_budget.Enabled());
        options.artifact_path = output.string();
        EXPECT_EQ(Luminumbra::Server::WriteSmokeArtifact(options, first, replay),
                  scenario == 2 ? 1 : 0);
        std::ifstream actual_file(output, std::ios::binary);
        ASSERT_TRUE(actual_file.is_open());
        const std::string actual((std::istreambuf_iterator<char>(actual_file)), {});
        std::ifstream baseline_file(fs::path(LUMINUMBRA_SOURCE_ROOT) /
                                        "test/fixtures/smoke_artifact" /
                                        ("devel-" + std::to_string(scenario) + ".json"),
                                    std::ios::binary);
        ASSERT_TRUE(baseline_file.is_open());
        std::string baseline((std::istreambuf_iterator<char>(baseline_file)), {});
#ifdef _WIN32
        // Devel's text-mode ofstream expands LF to CRLF on Windows as well.
        std::string native;
        for (const char c : baseline) {
            if (c == '\n')
                native += '\r';
            native += c;
        }
        baseline = std::move(native);
#endif
        EXPECT_EQ(actual, baseline);
    }
    fs::remove(output);
}

TEST(SimBudgetTelemetry, WaterWorkIsZeroAfterEmptyAndPausedUpdates) {
    using namespace Luminumbra;
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
    Systems::WaterSystem water(nullptr, &world);
    entt::registry registry;
    auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
    chunk->has_water_sim = true;
    chunk->current_water_resolution = 8;
    chunk->water_depth_mm.assign(64, 1000);
    chunk->water_bed_mm.assign(64, 1000);
    chunk->water_src_mm.assign(64, 0);
    chunk->water_level_data.assign(64, 2.0f);
    chunk->water_flow_data.assign(64, Vec2(0.0f));
    chunk->water_sim_terrain_height.assign(64, 1.0f);
    chunk->water_rest_level.assign(64, 2.0f);
    const std::unordered_map<ChunkID, std::shared_ptr<Chunk>> chunks = {{chunk->get_id(), chunk}};
    const std::unordered_map<ChunkID, std::shared_ptr<Chunk>> empty;
    EXPECT_EQ(water.cells_stepped_last_update(), 0u);
    for (const bool paused : {false, true}) {
        SCOPED_TRACE(paused);
        water.SetBootPaused(false);
        chunk->is_water_sleeping = false;
        water.update(registry, chunks);
        ASSERT_EQ(water.cells_stepped_last_update(), 64u);
        ASSERT_EQ(water.dbg_cells_simmed(), 64u);
        water.SetBootPaused(paused);
        water.update(registry, paused ? chunks : empty);
        EXPECT_EQ(water.cells_stepped_last_update(), 0u);
        EXPECT_EQ(water.dbg_cells_simmed(), 64u); // Preserve the legacy flag-off output.
    }
}

TEST(SimBudgetTelemetry, DisabledSkipsCounterAndArtifactExtension) {
    SimBudgetTelemetry telemetry;
    EXPECT_FALSE(telemetry.Enabled());
    bool counter_called = false;
    {
        SimBudgetTelemetry::TickScope tick(telemetry, 1);
        tick.Next(SimBudgetStage::Plants, [&] {
            counter_called = true;
            return 6u;
        });
    }
    telemetry.Record(SimBudgetStage::Plants, 1, 6, 42.0);
    EXPECT_FALSE(counter_called);
    for (const auto& stage : telemetry.SamplesByStage())
        EXPECT_TRUE(stage.empty());

    // Unit coverage of the append helper only. The complete serializer has its
    // own fixed-input devel baseline test below.
    nlohmann::json artifact = {{"schema", "luminumbra.server_tick.v1"},
                               {"world_hash", "fixed hash"},
                               {"runs", {{{"wall_seconds", 1.25}, {"world_id", "fixed id"}}}},
                               {"future_key", "preserve\nthis"}};
    const std::string before = artifact.dump(2) + "\n";
    Luminumbra::Server::AppendSimBudgetArtifact(artifact, telemetry, telemetry);
    EXPECT_EQ(artifact.dump(2) + "\n", before);
    EXPECT_FALSE(artifact.contains("sim_budget"));
}

TEST(SimBudgetTelemetry, PercentilesUsePerfLinearInterpolation) {
    SimBudgetTelemetry telemetry;
    telemetry.SetEnabled(true);
    EXPECT_EQ(telemetry.Summarize(SimBudgetStage::Plants).samples, 0u);
    telemetry.Record(SimBudgetStage::Plants, 1, 6, 30.0);
    const auto single = telemetry.Summarize(SimBudgetStage::Plants);
    EXPECT_DOUBLE_EQ(single.p99_ms, 30.0);
    telemetry.Record(SimBudgetStage::Plants, 2, 5, 0.0);
    telemetry.Record(SimBudgetStage::Plants, 3, 4, 20.0);
    telemetry.Record(SimBudgetStage::Plants, 4, 3, 10.0);
    const auto summary = telemetry.Summarize(SimBudgetStage::Plants);
    EXPECT_EQ(summary.samples, 4u);
    EXPECT_EQ(summary.work_total, 18u);
    EXPECT_DOUBLE_EQ(summary.p50_ms, 15.0);
    EXPECT_DOUBLE_EQ(summary.p95_ms, 28.5);
    EXPECT_DOUBLE_EQ(summary.p99_ms, 29.7);
    EXPECT_DOUBLE_EQ(summary.max_ms, 30.0);
    EXPECT_DOUBLE_EQ(
        telemetry.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::Plants)][0].duration_ms,
        30.0);
}

TEST(SimBudgetTelemetry, ReplayComparesTicksAndWorkButNeverDurations) {
    SimBudgetTelemetry first;
    SimBudgetTelemetry replay;
    first.SetEnabled(true);
    replay.SetEnabled(true);
    first.Record(SimBudgetStage::Plants, 1, 6, 1.0);
    replay.Record(SimBudgetStage::Plants, 1, 6, 1000.0);
    EXPECT_TRUE(first.WorkMatches(replay));
    replay.Clear();
    replay.Record(SimBudgetStage::Plants, 2, 6, 1.0);
    EXPECT_FALSE(first.WorkMatches(replay));
    replay.Clear();
    replay.Record(SimBudgetStage::Plants, 1, 7, 1.0);
    EXPECT_FALSE(first.WorkMatches(replay));
    replay.Clear();
    replay.Record(SimBudgetStage::Creatures, 1, 6, 1.0);
    EXPECT_FALSE(first.WorkMatches(replay));
}

TEST(SimBudgetTelemetry, ArtifactSeparatesWorkTraceFromDurationDistribution) {
    SimBudgetTelemetry first;
    SimBudgetTelemetry replay;
    first.SetEnabled(true);
    replay.SetEnabled(true);
    first.Record(SimBudgetStage::Plants, 1, 6, 2.0);
    replay.Record(SimBudgetStage::Plants, 1, 6, 200.0);
    nlohmann::json artifact = {{"schema", "luminumbra.server_tick.v1"}};
    Luminumbra::Server::AppendSimBudgetArtifact(artifact, first, replay);
    const auto& block = artifact.at("sim_budget");
    EXPECT_EQ(block.at("schema"), "luminumbra.sim_budget.v2");
    EXPECT_TRUE(block.at("work_replay_match").get<bool>());
    const auto& stages = block.at("stages");
    const auto& plants = stages.at(static_cast<std::size_t>(SimBudgetStage::Plants));
    EXPECT_EQ(plants.at("work_total"), 6);
    EXPECT_EQ(plants.at("work_trace"), nlohmann::json::array({{{"tick", 1}, {"work", 6}}}));
    EXPECT_EQ(plants.at("duration_ms").at("p99"), 2.0);
    EXPECT_TRUE(
        stages.at(static_cast<std::size_t>(SimBudgetStage::Animation)).at("duration_ms").is_null());
}

TEST(SimBudgetTelemetry, BatchedTicksKeepAbsoluteIdsAndClearOnToggle) {
    Luminumbra::world::GameSession session;
    session.SetSimBudgetTelemetryEnabled(true);
    EXPECT_EQ(session.TickSimulation(0.0), 0u);
    EXPECT_EQ(session.TickSimulation(2.0 / 30.0), 2u);
    EXPECT_EQ(session.TickSimulation(1.0 / 30.0), 1u);
    const auto& telemetry = session.GetSimBudgetTelemetry();
    for (std::size_t stage = static_cast<std::size_t>(SimBudgetStage::Animation);
         stage <= static_cast<std::size_t>(SimBudgetStage::Events);
         ++stage) {
        const auto& samples = telemetry.SamplesByStage()[stage];
        if (stage == static_cast<std::size_t>(SimBudgetStage::WindWeather)) {
            EXPECT_TRUE(samples.empty());
            continue;
        }
        ASSERT_EQ(samples.size(), 3u);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            EXPECT_EQ(samples[i].tick, i + 1);
            EXPECT_EQ(samples[i].work, 0u);
            EXPECT_GE(samples[i].duration_ms, 0.0);
        }
    }
    session.SetSimBudgetTelemetryEnabled(false);
    EXPECT_EQ(session.TickSimulation(1.0 / 30.0), 1u);
    for (const auto& stage : telemetry.SamplesByStage())
        EXPECT_TRUE(stage.empty());
    session.SetSimBudgetTelemetryEnabled(true);
    EXPECT_EQ(session.TickSimulation(1.0 / 30.0), 1u);
    EXPECT_EQ(telemetry.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::Plants)][0].tick,
              5u);
}
} // namespace
