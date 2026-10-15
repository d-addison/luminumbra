#include "../support/UniqueTempPath.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_server/ServerWorldRunner.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
namespace fs = std::filesystem;
using luminumbra::simulation::SimBudgetStage;
using luminumbra::simulation::SimBudgetTelemetry;

// Test-only wall-clock and cache observations, excluded from simulation telemetry.
class PhaseDiagnostics {
public:
    using Clock = std::chrono::steady_clock;
    using Stats = Luminumbra::Persistence::ChunkValidationCache::Stats;
    PhaseDiagnostics(bool enabled, bool telemetry)
        : m_enabled(enabled)
        , m_telemetry(telemetry) {}
    void Report(std::string_view phase, const Stats& stats) {
        const auto end = Clock::now();
        Print(phase, std::chrono::duration<double>(end - m_start).count(), stats);
        m_start = Clock::now();
    }
    void Print(std::string_view phase, double seconds, const Stats& stats) {
        std::cout << "[sim-budget-phase] active_regions=" << m_enabled
                  << " telemetry=" << m_telemetry << " phase=" << phase << " seconds=" << seconds;
        for (std::size_t i = 0; i < stats.modes.size(); ++i) {
            const auto& now = stats.modes[i];
            const auto& old = m_previous.modes[i];
            std::cout << " mode=" << (i ? "strict" : "permissive")
                      << " hits=" << now.hits - old.hits << " misses=" << now.misses - old.misses
                      << " decodes=" << now.decodes - old.decodes
                      << " insertions=" << now.insertions - old.insertions
                      << " evictions=" << now.evictions - old.evictions
                      << " absent=" << now.absent - old.absent
                      << " byte_mismatch=" << now.byte_mismatch - old.byte_mismatch
                      << " oversize=" << now.oversize - old.oversize
                      << " evicted=" << now.evicted - old.evicted << " entries=" << now.entries
                      << " payload_bytes=" << now.payload_bytes;
        }
        std::cout << " first_strict_records=" << stats.first_strict_records
                  << " first_strict_bytes=" << stats.first_strict_bytes << '\n';
        m_previous = stats;
    }

private:
    bool m_enabled, m_telemetry;
    Clock::time_point m_start = Clock::now();
    Stats m_previous;
};

class SimBudgetWorldParity : public testing::TestWithParam<bool> {
private:
    fs::path root;

protected:
    using Fingerprint = std::pair<std::uint64_t, std::uint64_t>;
    using Fingerprints = std::map<std::string, Fingerprint>;
    static constexpr std::uint64_t kTicks = 3600;
    void RunPopulatedPass(bool telemetry,
                          std::string& hash,
                          Fingerprints& save_fingerprints,
                          SimBudgetTelemetry* captured_trace = nullptr);

    const fs::path& Root() const {
        return root;
    }

    void SetUp() override {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("sim_budget_parity_");
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
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
        systems["sim"]["active_regions"]["enabled"] = GetParam();
        std::ofstream(systems_path) << systems.dump(2) << '\n';
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    static Fingerprints SavedContentFingerprints(const fs::path& directory,
                                                 Fingerprints* replay = nullptr) {
        namespace P = Luminumbra::Persistence;
        Fingerprints result;
        Fingerprint replay_metadata;
        if (replay) {
            replay->clear();
            nlohmann::json metadata;
            std::ifstream input(directory / "world_info.json");
            EXPECT_TRUE(input.is_open());
            input >> metadata;
            // Independent fresh worlds have different creation times. Only the
            // cross-pass comparison normalizes this field and its index digest;
            // the within-pass save-toggle comparison retains the original bytes.
            metadata.at("creationTime") = 0;
            const auto canonical = metadata.dump(4) + '\n';
            replay_metadata = {canonical.size(),
                               P::fnv1a64_bytes(canonical.data(), canonical.size())};
        }
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (!entry.is_regular_file())
                continue;
            std::uint64_t bytes = 0, hash = 0;
            Fingerprint replay_fingerprint;
            const auto relative = entry.path().lexically_relative(directory).generic_string();
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
                if (replay) {
                    for (auto& member : index.members) {
                        if (member.path == "world_info.json" && member.present) {
                            member.bytes = replay_metadata.first;
                            member.fnv64 = replay_metadata.second;
                        }
                    }
                    const auto replay_index = P::encode_snapshot_index(index);
                    replay_fingerprint = {
                        replay_index.size(),
                        P::fnv1a64_bytes(replay_index.data(), replay_index.size())};
                }
            } else {
                bool exists = false;
                EXPECT_TRUE(P::hash_file(entry.path(), bytes, hash, exists, nullptr));
                EXPECT_TRUE(exists);
                replay_fingerprint =
                    relative == "world_info.json" ? replay_metadata : Fingerprint{bytes, hash};
            }
            // Stream payloads: copying two whole saves alongside the live water-authoritative
            // world can exhaust memory. The index uses this same size/FNV identity (design.md:55).
            result.emplace(relative, Fingerprint{bytes, hash});
            if (replay)
                replay->emplace(relative, replay_fingerprint);
        }
        return result;
    }
};

void SimBudgetWorldParity::RunPopulatedPass(bool telemetry,
                                            std::string& hash,
                                            Fingerprints& save_fingerprints,
                                            SimBudgetTelemetry* captured_trace) {
    SCOPED_TRACE(telemetry);
    PhaseDiagnostics phases(GetParam(), telemetry);
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
    config.sim_budget = telemetry;
    Luminumbra::Server::ServerWorldRunner runner(config);
    ASSERT_TRUE(runner.Boot());
    phases.Report("boot", runner.Session()->GetChunkValidationCacheStats());
    ASSERT_EQ(runner.Session()->ActiveRegionsEnabled(), GetParam());
    ASSERT_EQ(runner.Avatars().size(), 1u);
    ASSERT_EQ(runner.CreatureCount(), 8u);
    ASSERT_FALSE(runner.Session()->GetRegistry().view<Luminumbra::Components::PlantTag>().empty());
    const auto report = runner.RunFixedTicks(kTicks);
    phases.Report("ticks", runner.Session()->GetChunkValidationCacheStats());
    ASSERT_EQ(report.ticks_executed, kTicks);
    hash = runner.ComputeWorldHash();
    phases.Report("composite_hash", runner.Session()->GetChunkValidationCacheStats());
    ASSERT_FALSE(hash.empty());
    std::cout << "SimBudgetParity active_regions=" << GetParam()
              << " telemetry=" << config.sim_budget << " world_hash=" << hash << '\n';
    if (!telemetry) {
        for (const auto& samples : runner.Session()->GetSimBudgetTelemetry().SamplesByStage())
            EXPECT_TRUE(samples.empty());
    } else {
        const auto& trace = runner.Session()->GetSimBudgetTelemetry();
        // Preserve the complete trace before the save toggle clears session samples.
        if (captured_trace)
            *captured_trace = trace;
        for (std::size_t stage = 0; stage < trace.SamplesByStage().size(); ++stage) {
            const auto& samples = trace.SamplesByStage()[stage];
            const auto identity = static_cast<SimBudgetStage>(stage);
            const bool unexecuted =
                GetParam() ? identity == SimBudgetStage::Wind || identity == SimBudgetStage::Weather
                           : identity == SimBudgetStage::WindWeather;
            if (unexecuted) {
                EXPECT_TRUE(samples.empty());
                continue;
            }
            ASSERT_EQ(samples.size(), kTicks);
            for (std::size_t i = 0; i < samples.size(); ++i) {
                EXPECT_EQ(samples[i].tick, i + 1);
                // Exact per-tick identities are stronger than field totals alone:
                // a missing cell at one tick cannot cancel an extra cell at another.
                if (identity == SimBudgetStage::Wind || identity == SimBudgetStage::Weather)
                    EXPECT_EQ(samples[i].work, 64u * 64u);
                else if (identity == SimBudgetStage::WindWeather)
                    EXPECT_EQ(samples[i].work, 2u * 64u * 64u);
                else if (identity == SimBudgetStage::ServerPhysics)
                    EXPECT_EQ(samples[i].work, 1u);
            }
        }
        EXPECT_GT(trace.Summarize(SimBudgetStage::Creatures).work_total, 0u);
        EXPECT_GT(trace.Summarize(SimBudgetStage::Plants).work_total, 0u);
        EXPECT_GT(trace.Summarize(SimBudgetStage::Scent).work_total, 0u);
        EXPECT_EQ(trace.Summarize(SimBudgetStage::Wind).work_total,
                  GetParam() ? 0u : kTicks * 64u * 64u);
        EXPECT_EQ(trace.Summarize(SimBudgetStage::Weather).work_total,
                  GetParam() ? 0u : kTicks * 64u * 64u);
        EXPECT_EQ(trace.Summarize(SimBudgetStage::WindWeather).work_total,
                  GetParam() ? kTicks * 2u * 64u * 64u : 0u);
        EXPECT_EQ(trace.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::Plants)][0].work,
                  6u);
        EXPECT_EQ(
            trace.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::Creatures)][0].work,
            8u);
    }

    phases.Report("trace_assertions", runner.Session()->GetChunkValidationCacheStats());

    // Exercise the ordinary save path, including live plant persistence and
    // metadata, across the toggle at the same settled tick.
    ASSERT_TRUE(runner.Session()->SaveWorldState());
    phases.Report("save_state_first", runner.Session()->GetChunkValidationCacheStats());
    ASSERT_TRUE(runner.Session()->SaveWorld());
    phases.Report("save_metadata_first", runner.Session()->GetChunkValidationCacheStats());
    const auto before =
        SavedContentFingerprints(runner.Session()->GetWorldSaveDir(), &save_fingerprints);
    phases.Report("fingerprint_before", runner.Session()->GetChunkValidationCacheStats());
    runner.Session()->SetSimBudgetTelemetryEnabled(!config.sim_budget);
    ASSERT_TRUE(runner.Session()->SaveWorldState());
    phases.Report("save_state_second", runner.Session()->GetChunkValidationCacheStats());
    ASSERT_TRUE(runner.Session()->SaveWorld());
    phases.Report("save_metadata_second", runner.Session()->GetChunkValidationCacheStats());
    EXPECT_EQ(SavedContentFingerprints(runner.Session()->GetWorldSaveDir()), before);
    phases.Report("fingerprint_after", runner.Session()->GetChunkValidationCacheStats());
    Luminumbra::Server::ServerWorldRunner::ShutdownDiagnostics shutdown;
    runner.Shutdown(nullptr, &shutdown);
    phases.Print("save_shutdown", shutdown.save_seconds, shutdown.cache);
    phases.Print("teardown", shutdown.teardown_seconds, shutdown.cache);
}

// Three independent populated runs per active-regions setting: traced A and B
// compare every stage's tick/work identities, hashes and saves; untraced C checks
// world and save parity against A across the telemetry toggle.
TEST_P(SimBudgetWorldParity, Populated3600TicksPreserveWorldHashAndWorkCounts) {
    std::string first_hash, repeated_hash, untraced_hash;
    Fingerprints first_save, repeated_save, untraced_save;
    SimBudgetTelemetry first_trace, repeated_trace;
    ASSERT_NO_FATAL_FAILURE(RunPopulatedPass(true, first_hash, first_save, &first_trace));
    ASSERT_NO_FATAL_FAILURE(RunPopulatedPass(true, repeated_hash, repeated_save, &repeated_trace));
    EXPECT_TRUE(first_trace.WorkMatches(repeated_trace));
    EXPECT_EQ(first_hash, repeated_hash);
    EXPECT_EQ(first_save, repeated_save);
    ASSERT_NO_FATAL_FAILURE(RunPopulatedPass(false, untraced_hash, untraced_save));
    EXPECT_EQ(first_hash, untraced_hash);
    EXPECT_EQ(first_save, untraced_save);
}
INSTANTIATE_TEST_SUITE_P(ActiveRegions,
                         SimBudgetWorldParity,
                         testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "Enabled" : "Disabled";
                         });
} // namespace
