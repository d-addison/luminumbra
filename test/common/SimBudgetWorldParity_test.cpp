#include "../support/UniqueTempPath.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_server/ServerWorldRunner.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <utility>

namespace {
namespace fs = std::filesystem;
using luminumbra::simulation::SimBudgetStage;
using luminumbra::simulation::SimBudgetTelemetry;

// Keep the replay oracle explicit: a new stage must be reviewed before it is covered.
constexpr std::array kWorkComparedStages = {
    SimBudgetStage::ServerPhysics,
    SimBudgetStage::Animation,
    SimBudgetStage::Instinct,
    SimBudgetStage::Perception,
    SimBudgetStage::Scent,
    SimBudgetStage::Locomotion,
    SimBudgetStage::Creatures,
    SimBudgetStage::Wind,
    SimBudgetStage::Weather,
    SimBudgetStage::WindWeather,
    SimBudgetStage::Aether,
    SimBudgetStage::Energy,
    SimBudgetStage::Irrigation,
    SimBudgetStage::Soil,
    SimBudgetStage::Plants,
    SimBudgetStage::Pollination,
    SimBudgetStage::Disease,
    SimBudgetStage::Crops,
    SimBudgetStage::Fire,
    SimBudgetStage::Grazing,
    SimBudgetStage::Lifespan,
    SimBudgetStage::Alarm,
    SimBudgetStage::Decay,
    SimBudgetStage::Circadian,
    SimBudgetStage::Territory,
    SimBudgetStage::Packs,
    SimBudgetStage::Migration,
    SimBudgetStage::Events,
    SimBudgetStage::ServerStreaming,
    SimBudgetStage::ServerWater,
};
static_assert(
    [] {
        if (kWorkComparedStages.size() != static_cast<std::size_t>(SimBudgetStage::Count))
            return false;
        for (std::size_t stage = 0; stage < kWorkComparedStages.size(); ++stage)
            if (static_cast<std::size_t>(kWorkComparedStages[stage]) != stage)
                return false;
        return true;
    }(),
    "Compare every simulation budget stage exactly once in enum order");

void ExpectStageSamples(const SimBudgetTelemetry& trace, std::uint64_t ticks, bool active_regions) {
    for (std::size_t stage = 0; stage < trace.SamplesByStage().size(); ++stage) {
        SCOPED_TRACE(std::string(luminumbra::simulation::kSimBudgetStageNames[stage]));
        const auto& samples = trace.SamplesByStage()[stage];
        const auto identity = static_cast<SimBudgetStage>(stage);
        const bool unexecuted =
            active_regions ? identity == SimBudgetStage::Wind || identity == SimBudgetStage::Weather
                           : identity == SimBudgetStage::WindWeather;
        if (unexecuted) {
            EXPECT_TRUE(samples.empty());
            continue;
        }
        ASSERT_EQ(samples.size(), ticks);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            EXPECT_EQ(samples[i].tick, i + 1);
            if (identity == SimBudgetStage::Wind || identity == SimBudgetStage::Weather) {
                EXPECT_EQ(samples[i].work, 64u * 64u);
            } else if (identity == SimBudgetStage::WindWeather) {
                EXPECT_EQ(samples[i].work, 2u * 64u * 64u);
            } else if (identity == SimBudgetStage::ServerPhysics) {
                EXPECT_EQ(samples[i].work, 1u);
            }
        }
    }
}

// Call after ExpectStageSamples succeeds so the first plant and creature samples exist.
void ExpectWorkSummary(const SimBudgetTelemetry& trace, std::uint64_t ticks, bool active_regions) {
    EXPECT_GT(trace.Summarize(SimBudgetStage::Creatures).work_total, 0u);
    EXPECT_GT(trace.Summarize(SimBudgetStage::Plants).work_total, 0u);
    EXPECT_GT(trace.Summarize(SimBudgetStage::Scent).work_total, 0u);
    EXPECT_EQ(trace.Summarize(SimBudgetStage::Wind).work_total,
              active_regions ? 0u : ticks * 64u * 64u);
    EXPECT_EQ(trace.Summarize(SimBudgetStage::Weather).work_total,
              active_regions ? 0u : ticks * 64u * 64u);
    EXPECT_EQ(trace.Summarize(SimBudgetStage::WindWeather).work_total,
              active_regions ? ticks * 2u * 64u * 64u : 0u);
    EXPECT_EQ(trace.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::Plants)][0].work, 6u);
    EXPECT_EQ(trace.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::Creatures)][0].work,
              8u);
}

void ExpectMatchingWork(const SimBudgetTelemetry& first, const SimBudgetTelemetry& repeated) {
    for (const auto identity : kWorkComparedStages) {
        const auto stage = static_cast<std::size_t>(identity);
        SCOPED_TRACE(std::string(luminumbra::simulation::kSimBudgetStageNames[stage]));
        const auto& expected = first.SamplesByStage()[stage];
        const auto& actual = repeated.SamplesByStage()[stage];
        ASSERT_EQ(actual.size(), expected.size());
        // Compare individual ticks and work, including dynamic work invisible to hashes.
        // Duration is observational wall-clock data and must not match across runs.
        for (std::size_t sample = 0; sample < expected.size(); ++sample) {
            EXPECT_EQ(actual[sample].tick, expected[sample].tick) << "sample=" << sample;
            EXPECT_EQ(actual[sample].work, expected[sample].work) << "sample=" << sample;
        }
    }
}

class SimBudgetWorldParity : public testing::TestWithParam<bool> {
private:
    fs::path root;

protected:
    const fs::path& Root() const {
        return root;
    }

    void SetUp() override {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("sim_budget_parity_");
        fs::create_directories(root / "worlds/atlas/presets");
        // Telemetry neutrality uses a fixed historical terrain workload.
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets-rev6/default.json",
                      root / "worlds/atlas/presets/default.json");
        fs::create_directories(root / "data");
        fs::copy(fs::path(LUMINUMBRA_SOURCE_ROOT) / "data/common",
                 root / "data/common",
                 fs::copy_options::recursive);
        const fs::path systems_path = root / "data/common/systems.json";
        nlohmann::json systems;
        {
            std::ifstream input(systems_path);
            input >> systems;
        }
        // Pin legacy simulation gates; the active-region axis remains exercised.
        for (auto& entry : systems["sim"].items()) {
            if (entry.value().is_object())
                entry.value()["enabled"] = false;
        }
        systems["sim"]["active_regions"]["enabled"] = GetParam();
        std::ofstream(systems_path) << systems.dump(2) << '\n';
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    static std::map<std::string, std::pair<std::uint64_t, std::uint64_t>>
    SavedContentFingerprints(const fs::path& directory) {
        namespace P = Luminumbra::Persistence;
        std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> result;
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (!entry.is_regular_file())
                continue;
            std::uint64_t bytes = 0, hash = 0;
            if (entry.path() == P::snapshot_index_path(directory)) {
                P::SnapshotIndex index;
                EXPECT_TRUE(P::read_snapshot_index(directory, index, nullptr));
                // design.md:55-60 permits a new transaction header for an identical save;
                // unchanged member identities AND generations must remain identical (:57).
                index.generation = 0;
                index.txn_id = 0;
                const auto canonical = P::encode_snapshot_index(index);
                bytes = canonical.size();
                hash = P::fnv1a64_bytes(canonical.data(), canonical.size());
            } else {
                bool exists = false;
                EXPECT_TRUE(P::hash_file(entry.path(), bytes, hash, exists, nullptr));
                EXPECT_TRUE(exists);
            }
            // Stream payloads: copying two whole saves alongside the live water-authoritative
            // world can exhaust memory. The index uses this same size/FNV identity (design.md:55).
            result.emplace(entry.path().lexically_relative(directory).generic_string(),
                           std::make_pair(bytes, hash));
        }
        return result;
    }
    static void ExpectWorkItemTicks(const SimBudgetTelemetry& trace, std::uint64_t ticks) {
        for (std::size_t counter = 0; counter < trace.WorkItems().size(); ++counter) {
            SCOPED_TRACE(counter);
            const auto& samples = trace.WorkItems()[counter];
            ASSERT_EQ(samples.size(), ticks);
            for (std::size_t i = 0; i < samples.size(); ++i)
                EXPECT_EQ(samples[i].tick, i + 1);
        }
    }
};

TEST_P(SimBudgetWorldParity, Populated3600TicksPreserveWorldHashAndReplayWork) {
    constexpr std::uint64_t ticks = 3600;
    std::string untraced_hash;
    SimBudgetTelemetry first_trace;
    // One untraced world and two independent traced worlds per parameter.
    for (int pass = 0; pass < 3; ++pass) {
        SCOPED_TRACE(pass);
        Luminumbra::Server::ServerWorldRunnerConfig config;
        config.root_path = Root().generic_string() + "/";
        config.seed = "1337";
        config.surface_radius = 1;
        config.collision_radius = 1;
        // The server supplies an explicit replicated anchor list. An avatar keeps
        // the roster's scent owners due; an empty list intentionally visits nothing.
        config.avatar_count = 1;
        config.ecology_roster = true;
        config.planted_roster = true;
        config.sim_budget = pass != 0;
        config.autosave_interval_ticks = 0; // Saves are tested explicitly below.
        config.finite_hydrology = false;
        Luminumbra::Server::ServerWorldRunner runner(config);
        ASSERT_TRUE(runner.Boot());
        EXPECT_FALSE(runner.Session()->FiniteHydrologyEnabled());
        EXPECT_EQ(runner.Session()->GetWorldSystem()->debug_water_sim_resolution(), 8);
        EXPECT_FALSE(runner.Session()->WeatherRainEnabled());
        EXPECT_FALSE(runner.Session()->WeatherEventsEnabled());
        EXPECT_FALSE(runner.Session()->AetherStateEnabled());
        ASSERT_EQ(runner.Session()->ActiveRegionsEnabled(), GetParam());
        ASSERT_EQ(runner.Avatars().size(), 1u);
        ASSERT_EQ(runner.CreatureCount(), 8u);
        ASSERT_FALSE(
            runner.Session()->GetRegistry().view<Luminumbra::Components::PlantTag>().empty());
        const auto report = runner.RunFixedTicks(ticks);
        ASSERT_EQ(report.ticks_executed, ticks);
        const std::string hash = runner.ComputeWorldHash();
        ASSERT_FALSE(hash.empty());
        std::cout << "SimBudgetParity active_regions=" << GetParam()
                  << " telemetry=" << config.sim_budget << " world_hash=" << hash << '\n';
        if (pass == 0) {
            untraced_hash = hash;
            for (const auto& samples : runner.Session()->GetSimBudgetTelemetry().SamplesByStage()) {
                EXPECT_TRUE(samples.empty());
            }
            for (const auto& samples : runner.Session()->GetSimBudgetTelemetry().WorkItems()) {
                EXPECT_TRUE(samples.empty());
            }
        } else {
            EXPECT_EQ(hash, untraced_hash);
            const auto& trace = runner.Session()->GetSimBudgetTelemetry();
            ASSERT_NO_FATAL_FAILURE(ExpectWorkItemTicks(trace, ticks));
            ASSERT_NO_FATAL_FAILURE(ExpectStageSamples(trace, ticks, GetParam()));
            ExpectWorkSummary(trace, ticks, GetParam());
            // Capture before the save toggle clears session telemetry.
            if (pass == 1)
                first_trace = trace;
            else {
                EXPECT_TRUE(first_trace.WorkMatches(trace));
                ASSERT_NO_FATAL_FAILURE(ExpectMatchingWork(first_trace, trace));
            }
        }

        // Exercise the ordinary save path, including live plant persistence and
        // metadata, across the toggle at the same settled tick.
        ASSERT_TRUE(runner.Session()->SaveWorldState());
        ASSERT_TRUE(runner.Session()->SaveWorld());
        const auto before = SavedContentFingerprints(runner.Session()->GetWorldSaveDir());
        runner.Session()->SetSimBudgetTelemetryEnabled(!config.sim_budget);
        ASSERT_TRUE(runner.Session()->SaveWorldState());
        ASSERT_TRUE(runner.Session()->SaveWorld());
        EXPECT_EQ(SavedContentFingerprints(runner.Session()->GetWorldSaveDir()), before);
        runner.Shutdown();
    }
}
INSTANTIATE_TEST_SUITE_P(ActiveRegions,
                         SimBudgetWorldParity,
                         testing::Bool(),
                         [](const testing::TestParamInfo<bool>& parameter) {
                             return parameter.param ? "Enabled" : "Disabled";
                         });
} // namespace
