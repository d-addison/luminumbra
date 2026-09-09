#include <gtest/gtest.h>

#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_server/ServerWorldRunner.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {
namespace fs = std::filesystem;
using luminumbra::simulation::SimBudgetStage;
using luminumbra::simulation::SimBudgetTelemetry;

class SimBudgetWorldParity : public testing::Test {
protected:
    fs::path root;
    void SetUp() override {
        root = fs::temp_directory_path() /
               ("sim_budget_parity_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                      root / "worlds/atlas/presets/default.json");
        fs::create_directories(root / "data");
        fs::copy(fs::path(LUMINUMBRA_SOURCE_ROOT) / "data/common",
                 root / "data/common",
                 fs::copy_options::recursive);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    static std::map<std::string, std::string> SavedBytes(const fs::path& directory) {
        std::map<std::string, std::string> result;
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (!entry.is_regular_file())
                continue;
            std::ifstream input(entry.path(), std::ios::binary);
            std::ostringstream bytes;
            bytes << input.rdbuf();
            result.emplace(entry.path().lexically_relative(directory).generic_string(),
                           bytes.str());
        }
        return result;
    }
};

TEST_F(SimBudgetWorldParity, Populated3600TicksPreserveWorldHashAndReplayWork) {
    constexpr std::uint64_t ticks = 3600;
    std::string untraced_hash;
    SimBudgetTelemetry first_trace;
    for (int pass = 0; pass < 3; ++pass) {
        SCOPED_TRACE(pass);
        Luminumbra::Server::ServerWorldRunnerConfig config;
        config.root_path = root.generic_string() + "/";
        config.seed = "1337";
        config.surface_radius = 1;
        config.collision_radius = 1;
        config.ecology_roster = true;
        config.planted_roster = true;
        config.sim_budget = pass != 0;
        Luminumbra::Server::ServerWorldRunner runner(config);
        ASSERT_TRUE(runner.Boot());
        ASSERT_EQ(runner.CreatureCount(), 8u);
        ASSERT_FALSE(
            runner.Session()->GetRegistry().view<Luminumbra::Components::PlantTag>().empty());
        const auto report = runner.RunFixedTicks(ticks);
        ASSERT_EQ(report.ticks_executed, ticks);
        const std::string hash = runner.ComputeWorldHash();
        ASSERT_FALSE(hash.empty());
        if (pass == 0) {
            untraced_hash = hash;
            for (const auto& samples : runner.Session()->GetSimBudgetTelemetry().SamplesByStage())
                EXPECT_TRUE(samples.empty());
        } else {
            EXPECT_EQ(hash, untraced_hash);
            const auto& trace = runner.Session()->GetSimBudgetTelemetry();
            for (const auto& samples : trace.SamplesByStage()) {
                ASSERT_EQ(samples.size(), ticks);
                for (std::size_t i = 0; i < samples.size(); ++i)
                    EXPECT_EQ(samples[i].tick, i + 1);
            }
            EXPECT_GT(trace.Summarize(SimBudgetStage::Creatures).work_total, 0u);
            EXPECT_GT(trace.Summarize(SimBudgetStage::Plants).work_total, 0u);
            EXPECT_GT(trace.Summarize(SimBudgetStage::Scent).work_total, 0u);
            EXPECT_EQ(trace.Summarize(SimBudgetStage::Wind).work_total, ticks * 64u * 64u);
            EXPECT_EQ(
                trace.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::Plants)][0].work,
                6u);
            EXPECT_EQ(
                trace.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::Creatures)][0].work,
                8u);
            if (pass == 1)
                first_trace = trace;
            else
                EXPECT_TRUE(first_trace.WorkMatches(trace));
        }

        // Re-saving the SAME settled world after toggling observation must not
        // insert telemetry into any supported save bytes (metadata included).
        ASSERT_GT(runner.SaveFullSnapshot(), 0u);
        const auto before = SavedBytes(runner.Session()->GetWorldSaveDir());
        runner.Session()->SetSimBudgetTelemetryEnabled(!config.sim_budget);
        ASSERT_GT(runner.SaveFullSnapshot(), 0u);
        EXPECT_EQ(SavedBytes(runner.Session()->GetWorldSaveDir()), before);
        runner.Shutdown();
    }
}
} // namespace
