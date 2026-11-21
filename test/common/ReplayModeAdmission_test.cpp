#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/core/LogDispatcherSink.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/persistence/ReplacementOutputFile.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/SavedInputFile.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_server/HeavyComparison.h"
#include "luminumbra_server/modes/ModeHelpers.h"
#include "luminumbra_server/modes/Modes.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {
struct ReplayCaptureFault {
    std::size_t calls = 0;
    std::size_t refuse_call = 0;
    std::filesystem::path save_dir;
    std::size_t owner_mutation_call = 0;
    bool unbound_avatar = false;
};
ReplayCaptureFault& CaptureFault() {
    static ReplayCaptureFault fault;
    return fault;
}
} // namespace

namespace Luminumbra::Server {
// Inject an incomplete owner through public world APIs immediately before a
// selected capture. The real runner still decides whether and how to refuse.
class RefusedCaptureRunner : public ServerWorldRunner {
public:
    using ServerWorldRunner::ServerWorldRunner;
    ~RefusedCaptureRunner() {
        if (m_incomplete) {
            m_incomplete->set_state(ChunkState::Idle);
        }
    }
    std::string ComputeWorldHash() {
        PrepareCapture();
        return ServerWorldRunner::ComputeWorldHash();
    }
    void ComputeWorldHashAndSubHashes(std::string& hash,
                                      Persistence::WorldStreamingStateSubHashes& sub) {
        PrepareCapture();
        ServerWorldRunner::ComputeWorldHashAndSubHashes(hash, sub);
    }

private:
    std::shared_ptr<Chunk> m_incomplete;
    void PrepareCapture() {
        auto& fault = CaptureFault();
        ++fault.calls;
        if (fault.calls == fault.owner_mutation_call) {
            auto* physics = Session()->GetPhysicsSystem();
            if (fault.unbound_avatar) {
                ASSERT_NE(physics->create_avatar_character(Session()->GetMetadata().spawnPoint),
                          Systems::PhysicsSystem::kInvalidAvatar);
            } else {
                ASSERT_TRUE(physics->create_player_controller(Session()->GetMetadata().spawnPoint));
            }
        }
        if (fault.calls != fault.refuse_call) {
            return;
        }
        fault.save_dir = Session()->GetWorldSaveDir();
        ASSERT_FALSE(fault.save_dir.empty());
        ASSERT_TRUE(std::filesystem::is_directory(fault.save_dir));
        m_incomplete = std::make_shared<Chunk>(IVec3(100000, 0, 100000));
        m_incomplete->mark_voxel_data_dirty();
        m_incomplete->set_state(ChunkState::Idle);
        ASSERT_TRUE(Session()->GetWorldSystem()->adopt_streamed_chunk(m_incomplete));
        m_incomplete->set_state(ChunkState::Loading);
    }
};
} // namespace Luminumbra::Server

// Keep production control flow and catches intact, with distinct fixture symbols
// because the shipping Replay.cpp is also linked into this test target.
#define ServerWorldRunner RefusedCaptureRunner
#define RunRecord RunRefusedCaptureRecord
#define RunReplay RunRefusedCaptureReplay
#define RunMutateReplayFixture RunRefusedCaptureMutateReplayFixture
#define RunHeavy RunRefusedCaptureHeavy
#define RunLockstepLoopback RunRecordedLockstepLoopback
#include "../../src/luminumbra_server/modes/Heavy.cpp"
#include "../../src/luminumbra_server/modes/Lockstep.cpp"
#include "../../src/luminumbra_server/modes/Replay.cpp"
#undef RunLockstepLoopback
#undef RunHeavy
#undef RunMutateReplayFixture
#undef RunReplay
#undef RunRecord
#undef ServerWorldRunner

namespace {
namespace fs = std::filesystem;
namespace replay = Luminumbra::Replay;

std::string ReadReplayModeFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct FinalizationOutputFault {
    fs::path replay_path;
    std::string step;
    bool refuse_payload = false;
    bool refuse_marker = true;
    std::size_t payload_hits = 0;
    std::size_t marker_hits = 0;
};
FinalizationOutputFault*& CurrentFinalizationOutputFault() {
    static FinalizationOutputFault* fault = nullptr;
    return fault;
}
void RefuseFinalizationOutput(const char* step, const fs::path& path) {
    auto* fault = CurrentFinalizationOutputFault();
    if (!fault || step != fault->step)
        return;
    if (fault->refuse_marker && path == fs::path(fault->replay_path.string() + ".incomplete")) {
        ++fault->marker_hits;
        throw std::runtime_error("marker publication refused");
    }
    if (fault->refuse_payload && path == fault->replay_path) {
        ++fault->payload_hits;
        throw std::runtime_error("replay publication refused");
    }
}
class ScopedFinalizationOutputFault {
public:
    explicit ScopedFinalizationOutputFault(FinalizationOutputFault& fault) {
        CurrentFinalizationOutputFault() = &fault;
        Luminumbra::Persistence::set_snapshot_step_hook_for_testing(RefuseFinalizationOutput);
    }
    ~ScopedFinalizationOutputFault() {
        Luminumbra::Persistence::set_snapshot_step_hook_for_testing(nullptr);
        CurrentFinalizationOutputFault() = nullptr;
    }
};

// Exercise the public shipping modes, with real checkpoint/configuration
// fields. Mutations change only the field named by each case, never a stub runner.
class ReplayModeAdmission : public testing::Test {
protected:
    fs::path root;
    ServerCliOptions options;
    std::ostringstream logs;
    std::shared_ptr<spdlog::sinks::ostream_sink_mt> log_sink;

    void SetUp() override {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("replay_mode_");
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
        fs::create_directories(root / "worlds/atlas/presets");
        std::ifstream input(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json");
        ASSERT_TRUE(input);
        auto preset = nlohmann::json::parse(input);
        preset["generation_params"]["terrain"]["hydro"]["enabled"] = false;
        {
            std::ofstream output(root / "worlds/atlas/presets/default.json");
            output << preset.dump();
            ASSERT_TRUE(output.good());
        }
        options.root = root.string();
        options.systems_config_override = luminumbra::core::SystemConfig{};
        options.test_streaming_radius_cap = 1;
        options.surface_radius = 0;
        options.collision_radius = 0;
        options.record_path = (root / "recording.lrec").string();
        options.replay_path = options.record_path;
        options.artifact_path = (root / "playback.json").string();
        log_sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(logs);
        log_sink->set_pattern("%v");
        Log::GetDispatcherSink()->Attach(log_sink);
    }

    void TearDown() override {
        CaptureFault() = {};
        Log::GetDispatcherSink()->Detach(log_sink);
        std::error_code error;
        fs::remove_all(root, error);
    }

    void ExpectRefusedSaveRemoved() const {
        ASSERT_FALSE(CaptureFault().save_dir.empty());
        EXPECT_FALSE(fs::exists(CaptureFault().save_dir));
        // Keep the catalog parent, but leave no abandoned world entry in it.
        EXPECT_TRUE(fs::is_empty(root / "worlds/saves"));
    }

    void Rewrite(const replay::ReplayContents& contents) {
        replay::ReplayWriter writer;
        ASSERT_TRUE(writer.Open(options.replay_path, contents.header));
        for (const auto& input : contents.inputs) {
            writer.RecordInput(input.tick, input.inputs);
        }
        for (const auto& checkpoint : contents.checkpoints) {
            writer.RecordCheckpoint(checkpoint);
        }
        ASSERT_TRUE(writer.Finalize(contents.tick_count));
        const auto reread = replay::ReadReplay(options.replay_path);
        ASSERT_TRUE(reread.has_value());
        if (!reread.has_value()) {
            return;
        }
        EXPECT_FALSE(reread->truncated);
        EXPECT_TRUE(reread->trailer_present);
        EXPECT_EQ(reread->tick_count, contents.tick_count);
        EXPECT_EQ(reread->inputs.size(), contents.inputs.size());
        EXPECT_EQ(reread->checkpoints.size(), contents.checkpoints.size());
    }

    nlohmann::json Artifact() const {
        std::ifstream input(options.artifact_path);
        return nlohmann::json::parse(input);
    }

    void ConfigureLegacyHeavyCheckpoints() {
        options.ticks = 0;
        options.heavy_resim = 0;
        // Keep the complete dry resident set on load. Regional loads omit regenerable
        // chunks and can regenerate them while boot-time water initialization is paused;
        // that independently fails the heavy settle contract before owner gating matters.
        options.systems_config_override = luminumbra::core::SystemConfig::FromJsonString(
            R"({"sim":{"active_regions":{"enabled":false}}})");
    }

    void ExpectHeavySettleContract(const nlohmann::json& artifact) const {
        EXPECT_EQ(artifact.at("settle_contract_ok"), true) << logs.str();
        for (const auto* checkpoint : {"boot_settle_original", "boot_settle_loaded"}) {
            SCOPED_TRACE(checkpoint);
            EXPECT_EQ(artifact.at(checkpoint).at("uninited"), 0);
            EXPECT_EQ(artifact.at(checkpoint).at("contract_ok"), true);
        }
        EXPECT_EQ(artifact.at("boot_settle_original").at("water_settle_skipped"), false);
        EXPECT_EQ(artifact.at("boot_settle_loaded").at("water_settle_skipped"), true);
    }

    void ExpectAdmissionRefused(const std::string& diagnostic) {
        // Refusal must precede config resolution: this runtime has no preset.
        // An existing artifact must also survive admission failures unchanged.
        const auto runtime_root = root / "unopened";
        options.root = runtime_root.string();
        {
            std::ofstream output(options.artifact_path);
            output << "retained";
            ASSERT_TRUE(output.good());
        }
        logs.str("");
        logs.clear();
        EXPECT_EQ(RunReplay(options), 1);
        EXPECT_NE(logs.str().find(diagnostic), std::string::npos) << logs.str();
        EXPECT_FALSE(fs::exists(runtime_root));
        EXPECT_EQ(ReadReplayModeFile(options.artifact_path), "retained");
        options.root = root.string();
    }
};

using CaptureRefusalCase = std::tuple<std::uint64_t, std::size_t, bool>;

// Model Shutdown() recreating saves, including two runners sharing Heavy's snapshot.
struct SavingRunner {
    const fs::path& save_dir;
    bool& saved;
    ~SavingRunner() {
        std::error_code error;
        fs::create_directories(save_dir, error);
        std::ofstream output(save_dir / "shutdown");
        output << "saved";
        output.close();
        saved = !error && output.good();
    }
};

class ThrowawaySaveCleanup : public ReplayModeAdmission,
                             public testing::WithParamInterface<std::tuple<int, bool>> {};

TEST_P(ThrowawaySaveCleanup, RemovesSavesAfterEveryRunnerOnReturnAndRefusal) {
    const int exit_kind = std::get<0>(GetParam());
    const bool shared = std::get<1>(GetParam());
    const auto first_save = root / "first";
    const auto second_save = shared ? first_save : root / "second";
    const auto retained = root / "persistent";
    fs::create_directories(retained);
    bool first_saved = false;
    bool second_saved = false;
    const auto run = [&]() -> int {
        Luminumbra::Server::ThrowawaySaveDirGuard first_cleanup;
        Luminumbra::Server::ThrowawaySaveDirGuard second_cleanup;
        const SavingRunner first{first_save, first_saved};
        fs::create_directories(first_save); // successful boot
        first_cleanup.Arm(first_save);
        if (exit_kind == 3) {
            return 1; // the second peer failed to boot; its guard stays unarmed
        }
        const SavingRunner second{second_save, second_saved};
        fs::create_directories(second_save);
        if (!shared) {
            second_cleanup.Arm(second_save);
        }
        // Force the runner destructors to recreate the directories, even if a
        // success path already removed them before returning.
        fs::remove_all(first_save);
        fs::remove_all(second_save);
        if (exit_kind == 1) {
            return 1;
        }
        if (exit_kind == 2) {
            throw Luminumbra::Persistence::AuthorityCaptureRefused(
                Luminumbra::Persistence::AuthorityCaptureStatus::IncompleteOwner);
        }
        return 0;
    };
    try {
        EXPECT_EQ(run(), exit_kind == 3 ? 1 : exit_kind);
    } catch (const Luminumbra::Persistence::AuthorityCaptureRefused& error) {
        EXPECT_EQ(exit_kind, 2);
        EXPECT_EQ(error.status(), Luminumbra::Persistence::AuthorityCaptureStatus::IncompleteOwner);
    }
    EXPECT_TRUE(first_saved);
    EXPECT_EQ(second_saved, exit_kind != 3);
    EXPECT_FALSE(fs::exists(first_save));
    EXPECT_FALSE(fs::exists(second_save));
    EXPECT_TRUE(fs::is_directory(retained));
}

INSTANTIATE_TEST_SUITE_P(ExitPaths,
                         ThrowawaySaveCleanup,
                         testing::Combine(testing::Values(0, 1, 2, 3), testing::Bool()));

TEST_F(ReplayModeAdmission, ThrowawayCleanupToleratesUnarmedEmptyMissingAndRemovalError) {
    const auto retained = root / "retained-file";
    {
        std::ofstream output(retained);
        output << "retained";
        ASSERT_TRUE(output.good());
    }
    {
        const Luminumbra::Server::ThrowawaySaveDirGuard unarmed;
        Luminumbra::Server::ThrowawaySaveDirGuard empty;
        empty.Arm({});
        Luminumbra::Server::ThrowawaySaveDirGuard missing;
        missing.Arm(root / "missing");
        Luminumbra::Server::ThrowawaySaveDirGuard failed;
        failed.Arm(retained / "not-a-directory");
    }
    EXPECT_EQ(ReadReplayModeFile(retained), "retained");
    EXPECT_FALSE(fs::exists(root / "missing"));
}

class ReplayModeCaptureRefusal : public ReplayModeAdmission,
                                 public testing::WithParamInterface<CaptureRefusalCase> {};

std::vector<CaptureRefusalCase> CaptureRefusalCases() {
    std::vector<CaptureRefusalCase> cases;
    // A case performs at most two boots and 31 ticks per boot. Separate native
    // test entries keep the refusal matrix below the per-test timeout.
    for (const std::uint64_t ticks : {0u, 1u, 30u, 31u}) {
        const std::size_t captures = ticks == 0 ? 2u : (ticks == 31 ? 4u : 3u);
        for (std::size_t call = 1; call <= captures; ++call) {
            for (const bool retained : {false, true}) {
                cases.emplace_back(ticks, call, retained);
            }
        }
    }
    return cases;
}

TEST_P(ReplayModeCaptureRefusal, RefusedVerifyCapturesNeverPublishDivergenceArtifacts) {
    const auto [ticks, call, retained] = GetParam();
    options.ticks = ticks;
    ASSERT_EQ(RunRecord(options), 0);
    if (retained) {
        std::ofstream output(options.artifact_path);
        output << "retained";
        ASSERT_TRUE(output.good());
    }
    CaptureFault() = {.calls = 0, .refuse_call = call, .save_dir = {}};
    logs.str("");
    logs.clear();
    EXPECT_EQ(RunRefusedCaptureReplay(options), 1);
    EXPECT_EQ(CaptureFault().calls, call);
    ExpectRefusedSaveRemoved();
    EXPECT_NE(logs.str().find("authority hash capture refused"), std::string::npos) << logs.str();
    EXPECT_EQ(logs.str().find("replay DIVERGED"), std::string::npos);
    EXPECT_EQ(fs::exists(options.artifact_path), retained);
    if (retained) {
        EXPECT_EQ(ReadReplayModeFile(options.artifact_path), "retained");
    }
}

TEST_P(ReplayModeCaptureRefusal, RefusedRecordCapturesNeverPublishReplayOrRegionTrace) {
    const auto [ticks, call, retained] = GetParam();
    options.ticks = ticks;
    const auto companion = options.record_path + ".regions.json";
    if (retained) {
        for (const auto& path : {options.record_path, companion}) {
            std::ofstream output(path);
            output << "retained";
            ASSERT_TRUE(output.good());
        }
    }
    CaptureFault() = {.calls = 0, .refuse_call = call, .save_dir = {}};
    EXPECT_EQ(RunRefusedCaptureRecord(options), 1);
    EXPECT_EQ(CaptureFault().calls, call);
    ExpectRefusedSaveRemoved();
    EXPECT_NE(logs.str().find("authority hash capture refused"), std::string::npos) << logs.str();
    for (const auto& path : {options.record_path, companion}) {
        EXPECT_EQ(fs::exists(path), retained);
        if (retained) {
            EXPECT_EQ(ReadReplayModeFile(path), "retained");
        }
    }
}

INSTANTIATE_TEST_SUITE_P(CaptureBoundaries,
                         ReplayModeCaptureRefusal,
                         testing::ValuesIn(CaptureRefusalCases()));

class HeavyModeCaptureRefusal : public ReplayModeAdmission,
                                public testing::WithParamInterface<std::tuple<std::size_t, bool>> {
};

TEST_P(HeavyModeCaptureRefusal, RefusedHeavyCapturesFailBeforeComparisonOrArtifact) {
    const auto [call, retained] = GetParam();
    options.ticks = 0;
    options.heavy_resim = 1;
    if (retained) {
        std::ofstream output(options.artifact_path);
        output << "retained";
        ASSERT_TRUE(output.good());
    }
    CaptureFault() = {.calls = 0, .refuse_call = call, .save_dir = {}};
    logs.str("");
    logs.clear();
    EXPECT_EQ(RunRefusedCaptureHeavy(options), 1);
    EXPECT_EQ(CaptureFault().calls, call);
    ExpectRefusedSaveRemoved();
    EXPECT_NE(logs.str().find("authority hash capture refused"), std::string::npos) << logs.str();
    EXPECT_EQ(logs.str().find("round-trip differs"), std::string::npos);
    EXPECT_EQ(logs.str().find("resim differs"), std::string::npos);
    EXPECT_EQ(fs::exists(options.artifact_path), retained);
    if (retained) {
        EXPECT_EQ(ReadReplayModeFile(options.artifact_path), "retained");
    }
}

INSTANTIATE_TEST_SUITE_P(CaptureBoundaries,
                         HeavyModeCaptureRefusal,
                         testing::Combine(testing::Values(std::size_t{1}, 2, 3, 4),
                                          testing::Bool()));

TEST_F(ReplayModeAdmission, HeavyCliComparesUnchangedOwnerCheckpoints) {
    ConfigureLegacyHeavyCheckpoints();
    ASSERT_EQ(RunHeavy(options), 0) << logs.str();
    const auto artifact = Artifact();
    ExpectHeavySettleContract(artifact);
    EXPECT_EQ(artifact.at("roundtrip_owner_difference"), "");
    EXPECT_EQ(artifact.at("resim_owner_difference"), "");
    EXPECT_EQ(artifact.at("passed"), true);
    EXPECT_EQ(logs.str().find("owner comparison skipped"), std::string::npos);
}

TEST_F(ReplayModeAdmission, HeavyCliOwnerMismatchFailsBothCheckpointPairs) {
    ConfigureLegacyHeavyCheckpoints();
    for (const std::size_t call : {std::size_t{2}, std::size_t{4}}) {
        SCOPED_TRACE(call);
        CaptureFault() = {.calls = 0,
                          .refuse_call = 0,
                          .save_dir = {},
                          .owner_mutation_call = call,
                          .unbound_avatar = false};
        // The included shipping implementation keeps the one-argument CLI dispatch.
        EXPECT_EQ(RunRefusedCaptureHeavy(options), 1) << logs.str();
        EXPECT_EQ(CaptureFault().calls, 4u);
        const auto artifact = Artifact();
        ExpectHeavySettleContract(artifact);
        EXPECT_EQ(artifact.at("roundtrip_owner_difference"),
                  call == 2 ? "owner.player.present" : "");
        EXPECT_EQ(artifact.at("resim_owner_difference"), "owner.player.present");
        EXPECT_EQ(artifact.at("roundtrip_match"), call == 4);
        EXPECT_EQ(artifact.at("resim_match"), false);
        EXPECT_EQ(artifact.at("passed"), false);
    }
}

TEST_F(ReplayModeAdmission, HeavyCliWeatherScheduleLogsSkippedOwnerComparison) {
    ConfigureLegacyHeavyCheckpoints();
    using Schedule = Luminumbra::Systems::WeatherSchedule;
    using Category = Luminumbra::Systems::WeatherCategory;
    options.weather_schedule = std::make_shared<const Schedule>(
        Schedule{.phases = {{0, Category::Clear, 0, 0}}, .strike_tick = std::nullopt});
    ASSERT_EQ(RunHeavy(options), 0) << logs.str();
    const auto artifact = Artifact();
    ExpectHeavySettleContract(artifact);
    EXPECT_FALSE(artifact.contains("roundtrip_owner_difference"));
    EXPECT_FALSE(artifact.contains("resim_owner_difference"));
    EXPECT_EQ(artifact.at("passed"), true);
    EXPECT_NE(logs.str().find("owner comparison skipped: weather schedule"), std::string::npos)
        << logs.str();
}

TEST_F(ReplayModeAdmission, HeavyCliInvalidWeatherScheduleStillRefusesBeforeSave) {
    ConfigureLegacyHeavyCheckpoints();
    options.weather_schedule = std::make_shared<const Luminumbra::Systems::WeatherSchedule>();
    {
        std::ofstream output(options.artifact_path);
        output << "retained";
        ASSERT_TRUE(output.good());
    }
    EXPECT_EQ(RunHeavy(options), 1);
    EXPECT_EQ(ReadReplayModeFile(options.artifact_path), "retained");
    EXPECT_FALSE(fs::exists(root / "worlds/saves"));
    EXPECT_NE(logs.str().find("owner comparison skipped: weather schedule"), std::string::npos)
        << logs.str();
    EXPECT_NE(logs.str().find("original session failed to boot"), std::string::npos) << logs.str();
}

TEST_F(ReplayModeAdmission, HeavyOwnerComparisonAcceptsUnchangedCheckpoints) {
    ConfigureLegacyHeavyCheckpoints();
    for (const bool enabled : {false, true}) {
        SCOPED_TRACE(enabled);
        CaptureFault() = {};
        EXPECT_EQ(RunRefusedCaptureHeavy(options, {.owner_continuation = enabled}), 0)
            << logs.str();
        EXPECT_EQ(CaptureFault().calls, 4u);
        const auto artifact = Artifact();
        ExpectHeavySettleContract(artifact);
        EXPECT_EQ(artifact.at("roundtrip_match"), true);
        EXPECT_EQ(artifact.at("resim_match"), true);
        EXPECT_EQ(artifact.at("passed"), true);
        if (enabled) {
            EXPECT_EQ(artifact.at("roundtrip_owner_difference"), "");
            EXPECT_EQ(artifact.at("resim_owner_difference"), "");
        } else {
            EXPECT_FALSE(artifact.contains("roundtrip_owner_difference"));
            EXPECT_FALSE(artifact.contains("resim_owner_difference"));
        }
    }
}

TEST_F(ReplayModeAdmission, HeavyOwnerComparisonGatesBothCheckpointPairs) {
    ConfigureLegacyHeavyCheckpoints();
    for (const bool enabled : {false, true}) {
        for (const std::size_t call : {std::size_t{2}, std::size_t{4}}) {
            SCOPED_TRACE(enabled);
            SCOPED_TRACE(call);
            CaptureFault() = {.calls = 0,
                              .refuse_call = 0,
                              .save_dir = {},
                              .owner_mutation_call = call,
                              .unbound_avatar = false};
            EXPECT_EQ(RunRefusedCaptureHeavy(options, {.owner_continuation = enabled}),
                      enabled ? 1 : 0)
                << logs.str();
            EXPECT_EQ(CaptureFault().calls, 4u);
            const auto artifact = Artifact();
            ExpectHeavySettleContract(artifact);
            EXPECT_EQ(artifact.at("roundtrip_match"), !enabled || call == 4);
            EXPECT_EQ(artifact.at("resim_match"), !enabled);
            EXPECT_EQ(artifact.at("passed"), !enabled);
            if (enabled) {
                EXPECT_EQ(artifact.at("roundtrip_owner_difference"),
                          call == 2 ? "owner.player.present" : "");
                EXPECT_EQ(artifact.at("resim_owner_difference"), "owner.player.present");
            } else {
                EXPECT_FALSE(artifact.contains("roundtrip_owner_difference"));
                EXPECT_FALSE(artifact.contains("resim_owner_difference"));
            }
        }
    }
}

TEST_F(ReplayModeAdmission, HeavyRefusedOwnerCaptureFailsBothCheckpointPairs) {
    ConfigureLegacyHeavyCheckpoints();
    CaptureFault() = {.calls = 0,
                      .refuse_call = 0,
                      .save_dir = {},
                      .owner_mutation_call = 2,
                      .unbound_avatar = true};
    EXPECT_EQ(RunRefusedCaptureHeavy(options), 1);
    EXPECT_EQ(CaptureFault().calls, 4u);
    const auto artifact = Artifact();
    ExpectHeavySettleContract(artifact);
    EXPECT_EQ(artifact.at("roundtrip_owner_difference"), "owner.capture_refused");
    EXPECT_EQ(artifact.at("resim_owner_difference"), "owner.capture_refused");
    EXPECT_EQ(artifact.at("passed"), false);
}

TEST_F(ReplayModeAdmission, SubHashCaptureSkipsRefusedRegionalPreparation) {
    namespace P = Luminumbra::Persistence;
    Luminumbra::JobSystem capture_jobs;
    capture_jobs.startup(1);
    options.systems_config_override = luminumbra::core::SystemConfig::FromJsonString(
        R"({"sim":{"active_regions":{"enabled":true}}})");
    Luminumbra::Server::ServerWorldRunner runner(RunnerConfigFrom(options));
    ASSERT_TRUE(runner.Boot());
    runner.Session()->SetJobSystem(&capture_jobs);
    const auto expected = runner.ComputeWorldSubHashes();
    const auto expected_hash = runner.ComputeWorldHash();
    capture_jobs.shutdown();
    const auto actual = runner.ComputeWorldSubHashes();
    EXPECT_EQ(actual.terrain, expected.terrain);
    EXPECT_EQ(actual.water, expected.water);
    EXPECT_EQ(actual.entities, expected.entities);
    std::string hash = "retained";
    P::WorldStreamingStateSubHashes sub;
    sub.water = "retained";
    for (const bool combined : {false, true}) {
        bool refused = false;
        try {
            if (combined) {
                runner.ComputeWorldHashAndSubHashes(hash, sub);
            } else {
                (void)runner.ComputeWorldHash();
            }
        } catch (const P::AuthorityCaptureRefused& error) {
            refused = true;
            EXPECT_EQ(error.status(), P::AuthorityCaptureStatus::PreparationRefused);
        }
        EXPECT_TRUE(refused);
        EXPECT_EQ(hash, "retained");
        EXPECT_EQ(sub.water, "retained");
    }
    capture_jobs.startup(1);
    // A latched scheduler fault alone does not make recovered authority unhashable.
    EXPECT_EQ(runner.ComputeWorldHash(), expected_hash);
    runner.Shutdown();
}

TEST_F(ReplayModeAdmission, RunnerCapturePreservesStatusAndCombinedOutputsOnRefusal) {
    namespace P = Luminumbra::Persistence;
    Luminumbra::Server::ServerWorldRunner runner(RunnerConfigFrom(options));
    const auto expect_refusal = [&](P::AuthorityCaptureStatus expected) {
        std::string hash = "retained";
        P::WorldStreamingStateSubHashes sub;
        sub.terrain = "retained";
        for (int slot = 0; slot != 3; ++slot) {
            bool refused = false;
            try {
                if (slot == 0) {
                    (void)runner.ComputeWorldHash();
                } else if (slot == 1) {
                    (void)runner.ComputeWorldSubHashes();
                } else {
                    runner.ComputeWorldHashAndSubHashes(hash, sub);
                }
            } catch (const P::AuthorityCaptureRefused& error) {
                refused = true;
                EXPECT_EQ(error.status(), expected);
            }
            EXPECT_TRUE(refused);
            EXPECT_EQ(hash, "retained");
            EXPECT_EQ(sub.terrain, "retained");
        }
    };
    expect_refusal(P::AuthorityCaptureStatus::MissingCursor);
    ASSERT_TRUE(runner.Boot());
    const auto hash = runner.ComputeWorldHash();
    const auto sub = runner.ComputeWorldSubHashes();
    std::string combined_hash;
    P::WorldStreamingStateSubHashes combined_sub;
    runner.ComputeWorldHashAndSubHashes(combined_hash, combined_sub);
    EXPECT_FALSE(hash.empty());
    EXPECT_EQ(combined_hash, hash);
    EXPECT_EQ(combined_sub.terrain, sub.terrain);
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(100000, 0, 100000));
    chunk->mark_voxel_data_dirty();
    chunk->set_state(Luminumbra::ChunkState::Idle);
    ASSERT_TRUE(runner.Session()->GetWorldSystem()->adopt_streamed_chunk(chunk));
    chunk->set_state(Luminumbra::ChunkState::Loading);
    expect_refusal(P::AuthorityCaptureStatus::IncompleteOwner);
    chunk->set_state(Luminumbra::ChunkState::Idle);
    runner.Shutdown();
    expect_refusal(P::AuthorityCaptureStatus::MissingCursor);
}

class ReplayModeRoundtrip : public ReplayModeAdmission,
                            public testing::WithParamInterface<std::uint64_t> {};

class ReplayModeWeatherRoundtrip : public ReplayModeAdmission,
                                   public testing::WithParamInterface<bool> {};

TEST_P(ReplayModeWeatherRoundtrip, ScheduledRecordPlaybackRestoresWeatherBeforeBoot) {
    using Schedule = Luminumbra::Systems::WeatherSchedule;
    using Category = Luminumbra::Systems::WeatherCategory;
    const Schedule schedule{.phases = {{0, Category::Clear, 0, 0},
                                       {2, Category::Rain, 65535, 65535},
                                       {5, Category::Clear, 0, 0}},
                            .strike_tick = 3};
    const bool regional = GetParam();
    SCOPED_TRACE(regional);
    options.systems_config_override = luminumbra::core::SystemConfig::FromJsonString(
        regional ? R"({"sim":{"active_regions":{"enabled":true}}})"
                 : R"({"sim":{"active_regions":{"enabled":false}}})");
    options.weather_schedule = std::make_shared<const Schedule>(schedule);
    options.ticks = 6;
    ASSERT_EQ(RunRecord(options), 0) << logs.str();
    const auto recorded = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(recorded.has_value());
    if (!recorded.has_value()) {
        return;
    }
    EXPECT_EQ(recorded->header.version, replay::kLrec1WeatherVersion);
    EXPECT_EQ(recorded->header.weather_schedule, schedule.canonical_bytes());
    EXPECT_EQ(RuntimeHelloFrom(options).system_config_identity,
              recorded->header.system_config_identity);
    // Playback must restore from the file, independently of recording options.
    options.weather_schedule.reset();
    const auto admitted = ReplayRunnerConfigFrom(options, recorded->header);
    ASSERT_TRUE(admitted.has_value());
    if (!admitted.has_value()) {
        return;
    }
    ASSERT_NE(admitted->weather_schedule, nullptr);
    EXPECT_EQ(*admitted->weather_schedule, schedule);
    ASSERT_EQ(RunReplay(options), 0) << logs.str();
    const auto artifact = Artifact();
    EXPECT_EQ(artifact.at("passed"), true);
    EXPECT_EQ(artifact.at("start_world_hash_match"), true);
    EXPECT_EQ(artifact.at("end_hash_match"), true);
    auto changed = recorded->header;
    auto altered = schedule;
    altered.phases[1].storm = 65534;
    changed.weather_schedule = altered.canonical_bytes();
    EXPECT_FALSE(ReplayRunnerConfigFrom(options, changed).has_value());
    for (const auto& invalid :
         {std::string{}, std::string{"WSC2"}, std::string(Schedule::kMaxEncodedBytes + 1, '0')}) {
        changed.weather_schedule = invalid;
        EXPECT_FALSE(ReplayRunnerConfigFrom(options, changed).has_value());
    }
    changed = recorded->header;
    changed.version = replay::kLrec1Version;
    EXPECT_FALSE(ReplayRunnerConfigFrom(options, changed).has_value());
    changed.version = 9;
    EXPECT_FALSE(ReplayRunnerConfigFrom(options, changed).has_value());
    const auto complete = ReadReplayModeFile(options.record_path);
    ASSERT_FALSE(complete.empty());
    const auto schedule_offset = complete.find(schedule.canonical_bytes());
    ASSERT_NE(schedule_offset, std::string::npos);
    {
        auto invalid = complete;
        invalid[schedule_offset + 3] = '2';
        std::ofstream output(options.record_path, std::ios::binary | std::ios::trunc);
        output << invalid;
        ASSERT_TRUE(output.good());
    }
    ExpectAdmissionRefused("invalid or has an unsupported LREC1 version");
    {
        std::ofstream output(options.record_path, std::ios::binary | std::ios::trunc);
        output << complete.substr(0, complete.size() - 1);
        ASSERT_TRUE(output.good());
    }
    ExpectAdmissionRefused("truncated (no valid trailer)");
}

INSTANTIATE_TEST_SUITE_P(WeatherBackends, ReplayModeWeatherRoundtrip, testing::Bool());

TEST_F(ReplayModeAdmission, InvalidRecordingSchedulePreservesOutputAndDoesNotCreateWorld) {
    {
        std::ofstream output(options.record_path);
        output << "retained";
        ASSERT_TRUE(output.good());
    }
    options.weather_schedule = std::make_shared<const Luminumbra::Systems::WeatherSchedule>();
    EXPECT_TRUE(RuntimeHelloFrom(options).system_config_identity.empty());
    EXPECT_EQ(RunRecord(options), 1);
    EXPECT_EQ(ReadReplayModeFile(options.record_path), "retained");
    EXPECT_FALSE(fs::exists(root / "worlds/saves"));
}

TEST_F(ReplayModeAdmission, CompleteBootDescriptorRefusesConflictsBeforeWorldOrOutput) {
    options.ticks = 0;
    ASSERT_EQ(RunRecord(options), 0);
    const auto recorded = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(recorded.has_value());
    if (!recorded) {
        return;
    }
    ASSERT_TRUE(recorded->header.authority_boot.has_value());
    if (!recorded->header.authority_boot) {
        return;
    }
    EXPECT_EQ(recorded->header.version, replay::kLrec1BootVersion);
    EXPECT_TRUE(ReplayRunnerConfigFrom(options, recorded->header).has_value());
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        SCOPED_TRACE(mutation);
        auto changed = *recorded;
        if (!changed.header.authority_boot) {
            FAIL() << "Missing captured descriptor";
            return;
        }
        auto& boot = *changed.header.authority_boot;
        switch (mutation) {
            case 0:
                ++boot.authority_rules_revision;
                break;
            case 1:
                boot.scalar_bits[0] ^= 1;
                break;
            case 2:
                boot.system_gates[0] ^= 1;
                break;
            case 3:
                ++boot.resolved_world_seed;
                break;
            case 4:
                ++boot.day_length_ticks;
                break;
            case 5:
                ++boot.seed_conversion_version;
                break;
            case 6:
                changed.header.version = replay::kLrec1Version;
                changed.header.authority_boot.reset();
                // A descriptor cannot travel under the legacy profile. Mutate the
                // version on disk below, after writing a valid complete stream.
                break;
            default:
                changed.header.version = replay::kLrec1BootElidedVersion;
                break;
        }
        if (mutation == 5) {
            EXPECT_FALSE(ReplayRunnerConfigFrom(options, changed.header).has_value());
            continue; // Invalid descriptor is refused by the writer as well.
        }
        if (mutation == 6) {
            changed = *recorded;
        }
        Rewrite(changed);
        ASSERT_FALSE(testing::Test::HasFatalFailure());
        if (mutation == 6) {
            auto bytes = ReadReplayModeFile(options.replay_path);
            ASSERT_GT(bytes.size(), 8u);
            bytes[6] = static_cast<char>(replay::kLrec1Version);
            std::ofstream output(options.replay_path, std::ios::binary | std::ios::trunc);
            output << bytes;
            ASSERT_TRUE(output.good());
        }
        logs.str("");
        logs.clear();
        std::ofstream(options.artifact_path) << "retained";
        EXPECT_EQ(RunReplay(options), 1);
        EXPECT_EQ(ReadReplayModeFile(options.artifact_path), "retained");
        EXPECT_TRUE(fs::is_empty(root / "worlds/saves"));
        EXPECT_EQ(logs.str().find("Water resolution wired:"), std::string::npos) << logs.str();
    }
}

TEST_F(ReplayModeAdmission, ExplicitWorkloadConflictsRefuseBeforeBootOrOutput) {
    options.ticks = 0;
    ASSERT_EQ(RunRecord(options), 0);
    const auto recorded = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(recorded.has_value());
    if (!recorded) {
        return;
    }
    const auto original = options;
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        SCOPED_TRACE(mutation);
        options = original;
        switch (mutation) {
            case 0:
                options.avatars = 1;
                break;
            case 1:
                options.ecology_roster = true;
                break;
            case 2:
                options.planted_roster = true;
                break;
            case 3:
                options.moving = true;
                break;
            case 4:
                options.water_smoke = true;
                break;
            case 5:
                options.parked_bytes_limit = 1;
                break;
            default:
                options.sim_budget_profile.emplace();
                options.sim_budget_profile->region_scheduler =
                    Luminumbra::world::RegionSchedulerConfig{
                        .work_limit = 0, .hold_ticks = 1, .reduced_shift = 2};
                break;
        }
        EXPECT_FALSE(ReplayRunnerConfigFrom(options, recorded->header).has_value());
        std::ofstream(options.artifact_path) << "retained";
        logs.str("");
        logs.clear();
        EXPECT_EQ(RunReplay(options), 1);
        EXPECT_EQ(ReadReplayModeFile(options.artifact_path), "retained");
        EXPECT_EQ(logs.str().find("Water resolution wired:"), std::string::npos);
        EXPECT_TRUE(fs::is_empty(root / "worlds/saves"));
    }
}

TEST_F(ReplayModeAdmission, BootDescriptionRefusesEmptySeedAndOutOfRangeRoster) {
    auto config = RunnerConfigFrom(options);
    config.seed.clear();
    EXPECT_THROW(
        static_cast<void>(Luminumbra::Server::ServerWorldRunner::DescribeReplayBoot(config)),
        std::invalid_argument);
    config.seed = "424242";
    for (const int avatars : {-1, 1025}) {
        config.avatar_count = avatars;
        EXPECT_THROW(
            static_cast<void>(Luminumbra::Server::ServerWorldRunner::DescribeReplayBoot(config)),
            std::invalid_argument);
    }
    EXPECT_FALSE(fs::exists(root / "worlds/saves"));
}

class ReplayProducerBoot : public ReplayModeAdmission, public testing::WithParamInterface<bool> {};

TEST_P(ReplayProducerBoot, CliAndAutomaticReconstructEquivalentAuthorityBoots) {
    namespace net = Luminumbra::Net;
    options.ticks = 1;
    options.seed = "replay-seed";
    options.avatars = 1;
    options.moving = true;
    options.parked_bytes_limit = 32u * 1024u * 1024u;
    options.systems_config_override = luminumbra::core::SystemConfig::FromJsonString(
        GetParam() ? R"({"sim":{"active_regions":{"enabled":true}}})"
                   : R"({"sim":{"active_regions":{"enabled":false}}})");
    ASSERT_EQ(RunRecord(options), 0) << logs.str();
    const auto cli = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(cli.has_value());
    if (!cli) {
        return;
    }
    ASSERT_TRUE(cli->header.authority_boot.has_value());
    Luminumbra::Server::ThrowawaySaveDirGuard cleanup;
    Luminumbra::Server::ServerWorldRunner runner(RunnerConfigFrom(options),
                                                 {.owner_continuation = true});
    EXPECT_THROW(static_cast<void>(runner.CaptureReplayHeader()), std::invalid_argument);
    ASSERT_TRUE(runner.Boot());
    cleanup.Arm(runner.Session()->GetWorldSaveDir());
    auto header = runner.CaptureReplayHeader();
    EXPECT_EQ(header.authority_boot, cli->header.authority_boot);
    EXPECT_EQ(runner.ComputeWorldHash(), cli->header.start_world_hash);
    net::LockstepConfig config;
    config.seed = runner.Session()->GetMetadata().resolvedSeed;
    config.preset = options.preset;
    config.system_config_identity = header.system_config_identity;
    config.preset_content_hash = header.preset_content_hash;
    config.hash_cadence_ticks = 1;
    config.replay_header = std::make_shared<const replay::ReplayHeader>(header);
    net::LockstepHooks hooks;
    hooks.user = &runner;
    hooks.apply_and_step = [](std::uint64_t, const std::vector<std::uint8_t>&, void* user) {
        return static_cast<Luminumbra::Server::ServerWorldRunner*>(user)
                   ->RunFixedTicks(1)
                   .ticks_executed == 1;
    };
    hooks.capture_hashes = [](std::uint64_t tick, net::HashMsg& out, void* user) {
        Luminumbra::Persistence::WorldStreamingStateSubHashes sub;
        static_cast<Luminumbra::Server::ServerWorldRunner*>(user)->ComputeWorldHashAndSubHashes(
            out.world_hash, sub);
        out.tick = tick;
        out.terrain = sub.terrain;
        out.water = sub.water;
        out.entities = sub.entities;
    };
    auto [local, peer] = net::MakeLoopbackPair();
    net::LockstepSession session(config, local.get(), hooks);
    const auto automatic_path = (root / "automatic.lrec").string();
    session.SetDumpPath(automatic_path);
    auto remote = config;
    remote.local_client_id = 1;
    remote.peer_client_id = 0;
    ASSERT_TRUE(peer->SendFrame(net::EncodeHello(net::MakeLockstepHello(remote))));
    ASSERT_TRUE(session.Handshake());
    ASSERT_TRUE(session.Status().replay_error.empty()) << session.Status().replay_error;
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
    ASSERT_EQ(session.PumpTick(1).outcome, net::TickOutcome::Finished);
    EXPECT_EQ(session.Status().agreed_tick, 1u);
    EXPECT_EQ(runner.TickCount(), 1u);
    net::HashMsg mismatch;
    hooks.capture_hashes(1, mismatch, hooks.user);
    mismatch.world_hash = "different";
    mismatch.terrain = "different";
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(mismatch)));
    ASSERT_EQ(session.PumpTick(1).outcome, net::TickOutcome::Desync);
    EXPECT_THROW(static_cast<void>(runner.CaptureReplayHeader()), std::invalid_argument);
    const auto automatic = replay::ReadReplay(automatic_path);
    ASSERT_TRUE(automatic.has_value());
    if (!automatic) {
        return;
    }
    EXPECT_EQ(automatic->header.version, replay::kLrec1BootVersion);
    EXPECT_EQ(automatic->header.authority_boot, cli->header.authority_boot);
    EXPECT_EQ(automatic->header.start_world_hash, cli->header.start_world_hash);
    ASSERT_EQ(automatic->checkpoints.size(), 1u);
    ASSERT_EQ(cli->checkpoints.size(), 1u);
    EXPECT_EQ(automatic->checkpoints[0].world_hash, cli->checkpoints[0].world_hash);
    // Reconstruction depends on the descriptor, not on remembered CLI workload flags.
    options.avatars = 0;
    options.moving = false;
    options.parked_bytes_limit = Luminumbra::Server::ServerWorldRunnerConfig{}.parked_bytes_limit;
    const auto restored_config = ReplayRunnerConfigFrom(options, automatic->header);
    ASSERT_TRUE(restored_config.has_value());
    if (!restored_config) {
        return;
    }
    Luminumbra::Server::ThrowawaySaveDirGuard restored_cleanup;
    Luminumbra::Server::ServerWorldRunner restored(*restored_config, {.owner_continuation = true});
    ASSERT_TRUE(restored.Boot());
    restored_cleanup.Arm(restored.Session()->GetWorldSaveDir());
    EXPECT_EQ(restored.CaptureReplayHeader().authority_boot, automatic->header.authority_boot);
    EXPECT_EQ(restored.ComputeWorldHash(), automatic->header.start_world_hash);
    ASSERT_EQ(restored.RunFixedTicks(1).ticks_executed, 1u);
    EXPECT_EQ(restored.ComputeWorldHash(), automatic->checkpoints[0].world_hash);
}

INSTANTIATE_TEST_SUITE_P(RegionBackends, ReplayProducerBoot, testing::Bool());

class ReplayShippingProducerBoot : public ReplayModeAdmission,
                                   public testing::WithParamInterface<bool> {};

TEST_P(ReplayShippingProducerBoot, ShippingLoopbackAutomaticallyCapturesTheCliBootProfile) {
    const bool scheduled = GetParam();
    if (scheduled) {
        using Schedule = Luminumbra::Systems::WeatherSchedule;
        using Category = Luminumbra::Systems::WeatherCategory;
        const Schedule schedule{.phases = {{0, Category::Clear, 0, 0}},
                                .strike_tick = std::nullopt};
        options.weather_schedule = std::make_shared<const Schedule>(schedule);
    }
    options.ticks = 30;
    options.seed = "replay-seed";
    options.avatars = 1;
    options.systems_config_override = luminumbra::core::SystemConfig::FromJsonString(
        R"({"sim":{"active_regions":{"enabled":false}}})");
    ASSERT_EQ(RunRecord(options), 0) << logs.str();
    const auto cli = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(cli.has_value());
    if (!cli) {
        return;
    }
    options.lockstep_corrupt_tick = 30;
    options.lockstep_dump_path = (root / "loopback.lrec").string();
    ASSERT_EQ(RunRecordedLockstepLoopback(options), 0) << logs.str();
    auto automatic = replay::ReadReplay(options.lockstep_dump_path);
    if (!automatic) {
        automatic = replay::ReadReplay(options.lockstep_dump_path + ".peer");
    }
    ASSERT_TRUE(automatic.has_value());
    if (!automatic) {
        return;
    }
    EXPECT_EQ(automatic->header.version,
              scheduled ? replay::kLrec1WeatherVersion : replay::kLrec1BootVersion);
    EXPECT_EQ(automatic->header.authority_boot, cli->header.authority_boot);
    EXPECT_EQ(automatic->header.weather_schedule, cli->header.weather_schedule);
    EXPECT_EQ(automatic->header.start_world_hash, cli->header.start_world_hash);
}

INSTANTIATE_TEST_SUITE_P(WeatherProfiles, ReplayShippingProducerBoot, testing::Bool());

TEST_F(ReplayModeAdmission, ScheduledLockstepRefusesMissingOrInvalidCapturedProfileBeforeOutput) {
    namespace net = Luminumbra::Net;
    using Schedule = Luminumbra::Systems::WeatherSchedule;
    using Category = Luminumbra::Systems::WeatherCategory;
    const Schedule schedule{.phases = {{0, Category::Clear, 0, 0}}, .strike_tick = std::nullopt};
    Luminumbra::world::GameSession identity;
    ASSERT_TRUE(identity.SetWeatherSchedule(schedule));
    net::LockstepConfig config;
    config.system_config_identity = identity.SystemConfigIdentity();
    config.preset_content_hash = "preset-content";
    replay::ReplayHeader header;
    header.version = replay::kLrec1WeatherVersion;
    header.seed = replay::kResolvedSeedTag | config.seed;
    header.preset = config.preset;
    header.system_config_identity = config.system_config_identity;
    header.preset_content_hash = config.preset_content_hash;
    header.weather_schedule = schedule.canonical_bytes();
    const std::string path = (root / "refused-automatic.lrec").string();
    {
        std::ofstream output(path);
        output << "retained";
        ASSERT_TRUE(output.good());
    }
    const std::array<std::uint16_t, 7> invalid_versions = {replay::kLrec1Version,
                                                           replay::kLrec1ElidedVersion,
                                                           replay::kLrec1BootVersion,
                                                           replay::kLrec1BootElidedVersion,
                                                           replay::kLrec1WeatherElidedVersion,
                                                           0,
                                                           9};
    // Seven invalid families, missing capture, and ten malformed/conflicting fields.
    for (unsigned mutation = 0; mutation < 18; ++mutation) {
        SCOPED_TRACE(mutation);
        auto invalid = header;
        if (mutation < invalid_versions.size()) {
            invalid.version = invalid_versions[mutation];
        } else {
            switch (mutation) {
                case 7: // No captured header.
                    break;
                case 8:
                    invalid.authority_boot.emplace();
                    break;
                case 9:
                    invalid.weather_schedule.clear();
                    break;
                case 10:
                    invalid.weather_schedule.pop_back();
                    break;
                case 11:
                    invalid.weather_schedule.assign(Schedule::kMaxEncodedBytes + 1, '0');
                    break;
                case 12:
                    invalid.seed ^= 1;
                    break;
                case 13:
                    invalid.preset = "other";
                    break;
                case 14:
                    invalid.tick_rate_hz = 0;
                    break;
                case 15:
                    invalid.system_config_identity.clear();
                    break;
                case 16:
                    invalid.preset_content_hash.clear();
                    break;
                default:
                    invalid.weather_schedule.replace(0, 4, "WSC2");
                    break;
            }
        }
        config.replay_header =
            mutation == 7 ? nullptr : std::make_shared<const replay::ReplayHeader>(invalid);
        auto remote = config;
        remote.local_client_id = config.peer_client_id;
        remote.peer_client_id = config.local_client_id;
        auto [local, peer] = net::MakeLoopbackPair();
        ASSERT_TRUE(peer->SendFrame(net::EncodeHello(net::MakeLockstepHello(remote))));
        std::size_t captures = 0;
        net::LockstepHooks hooks;
        hooks.user = &captures;
        hooks.capture_hashes = [](std::uint64_t, net::HashMsg&, void* user) {
            ++*static_cast<std::size_t*>(user);
        };
        net::LockstepSession session(config, local.get(), hooks);
        session.SetDumpPath(path);
        EXPECT_FALSE(session.Handshake());
        EXPECT_FALSE(session.Status().handshaken);
        EXPECT_EQ(session.Status().agreed_tick, 0u);
        EXPECT_EQ(session.Status().handshake_error, "scheduled weather is unsupported by lockstep");
        EXPECT_EQ(captures, 0u);
        EXPECT_EQ(ReadReplayModeFile(path), "retained");
        EXPECT_FALSE(fs::exists(path + ".incomplete"));
    }
    // A valid control makes this regression reject blanket weather refusals too.
    config.replay_header = std::make_shared<const replay::ReplayHeader>(header);
    auto remote = config;
    remote.local_client_id = config.peer_client_id;
    remote.peer_client_id = config.local_client_id;
    auto [local, peer] = net::MakeLoopbackPair();
    ASSERT_TRUE(peer->SendFrame(net::EncodeHello(net::MakeLockstepHello(remote))));
    net::LockstepSession admitted(config, local.get(), {});
    admitted.SetDumpPath((root / "admitted-automatic.lrec").string());
    EXPECT_TRUE(admitted.Handshake());
    EXPECT_TRUE(admitted.Status().handshaken);
    EXPECT_TRUE(admitted.Status().replay_error.empty()) << admitted.Status().replay_error;
    EXPECT_EQ(ReadReplayModeFile(path), "retained");
}

TEST_P(ReplayModeWeatherRoundtrip, ScheduledShippingLoopbackCapturesChangingWeatherBeforeTickOne) {
    using Schedule = Luminumbra::Systems::WeatherSchedule;
    using Category = Luminumbra::Systems::WeatherCategory;
    const Schedule schedule{.phases = {{0, Category::Clear, 0, 0},
                                       {2, Category::Rain, 65535, 65535},
                                       {5, Category::Clear, 0, 0}},
                            .strike_tick = 3};
    options.systems_config_override = luminumbra::core::SystemConfig::FromJsonString(
        GetParam() ? R"({"sim":{"active_regions":{"enabled":true}}})"
                   : R"({"sim":{"active_regions":{"enabled":false}}})");
    options.weather_schedule = std::make_shared<const Schedule>(schedule);
    options.ticks = 30;
    ASSERT_EQ(RunRecord(options), 0) << logs.str();
    const auto cli = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(cli.has_value());
    if (!cli.has_value()) {
        return;
    }
    options.lockstep_corrupt_tick = 30;
    options.lockstep_dump_path = (root / "scheduled-loopback.lrec").string();
    ASSERT_EQ(RunRecordedLockstepLoopback(options), 0) << logs.str();
    auto automatic = replay::ReadReplay(options.lockstep_dump_path);
    if (!automatic.has_value()) {
        automatic = replay::ReadReplay(options.lockstep_dump_path + ".peer");
    }
    ASSERT_TRUE(automatic.has_value());
    if (!automatic.has_value()) {
        return;
    }
    EXPECT_EQ(automatic->header.version, replay::kLrec1WeatherVersion);
    EXPECT_FALSE(automatic->header.authority_boot.has_value());
    EXPECT_EQ(automatic->header.weather_schedule, schedule.canonical_bytes());
    EXPECT_EQ(automatic->header.start_world_hash, cli->header.start_world_hash);
    ASSERT_EQ(automatic->checkpoints.size(), 1u);
    ASSERT_EQ(cli->checkpoints.size(), 1u);
    // The dump may contain the peer's deliberately corrupted checkpoint. Compare
    // the actual authorities, whose simulations are unchanged by fault injection.
    const auto artifact = Artifact();
    EXPECT_EQ(artifact.at("host").at("agreed_tick"), options.ticks);
    EXPECT_EQ(artifact.at("peer").at("agreed_tick"), options.ticks);
    EXPECT_EQ(artifact.at("host").at("world_hash"), cli->checkpoints[0].world_hash);
    EXPECT_EQ(artifact.at("peer").at("world_hash"), cli->checkpoints[0].world_hash);
    EXPECT_EQ(automatic->tick_count, options.ticks);
}

TEST_F(ReplayModeAdmission, FailedAutomaticFinalizationCannotAdmitAnOlderCompleteFile) {
    options.ticks = 0;
    ASSERT_EQ(RunRecord(options), 0);
    const auto recorded = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(recorded.has_value());
    if (!recorded) {
        return;
    }
    const auto original = ReadReplayModeFile(options.record_path);
    replay::QueuedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(17, 0));
    ASSERT_TRUE(recorder.Open(options.record_path, recorded->header));
    EXPECT_FALSE(recorder.RecordTick(
        1, std::make_shared<const std::vector<std::uint8_t>>(1, 0), nullptr, 0));
    EXPECT_FALSE(recorder.Finalize(options.record_path, 1, 1));
    EXPECT_EQ(ReadReplayModeFile(options.record_path), original);
    ASSERT_TRUE(fs::exists(options.record_path + ".incomplete"));
    ExpectAdmissionRefused("recording finalization is incomplete");
    // CLI cannot silently clear a different recorder's durable failure marker.
    const auto marker = ReadReplayModeFile(options.record_path + ".incomplete");
    EXPECT_EQ(RunRecord(options), 1);
    EXPECT_EQ(ReadReplayModeFile(options.record_path), original);
    EXPECT_EQ(ReadReplayModeFile(options.record_path + ".incomplete"), marker);
}

TEST_F(ReplayModeAdmission, MarkerPublicationFailureCannotAdmitRetainedAutomaticOrCliGeneration) {
    options.ticks = 0;
    for (const bool cli : {false, true}) {
        SCOPED_TRACE(cli);
        for (const char* step :
             {"output-open", "output-write", "output-flush", "output-close", "output-publish"}) {
            SCOPED_TRACE(step);
            ASSERT_EQ(RunRecord(options), 0) << logs.str();
            const auto recorded = replay::ReadReplay(options.record_path);
            ASSERT_TRUE(recorded.has_value());
            if (!recorded)
                return;
            FinalizationOutputFault fault{options.record_path, step, cli, true, 0, 0};
            if (cli) {
                const ScopedFinalizationOutputFault control(fault);
                EXPECT_EQ(RunRecord(options), 1);
                EXPECT_EQ(fault.payload_hits, 1u);
            } else {
                replay::QueuedReplayRecorder recorder;
                ASSERT_TRUE(recorder.Configure(17, 0));
                ASSERT_TRUE(recorder.Open(options.record_path, recorded->header));
                EXPECT_FALSE(recorder.RecordTick(
                    1, std::make_shared<const std::vector<std::uint8_t>>(1, 0), nullptr, 0));
                const ScopedFinalizationOutputFault control(fault);
                EXPECT_FALSE(recorder.Finalize(options.record_path, 1, 1));
            }
            EXPECT_EQ(fault.marker_hits, 1u);
            EXPECT_FALSE(fs::exists(options.record_path));
            EXPECT_FALSE(fs::exists(options.record_path + ".incomplete"));
            ExpectAdmissionRefused("invalid or has an unsupported LREC1 version");
            EXPECT_TRUE(fs::is_empty(root / "worlds/saves"));
        }
    }
}

TEST_F(ReplayModeAdmission, FailedCliFinalizationRetainsARefusedGenerationWhenMarkerPublishes) {
    options.ticks = 0;
    ASSERT_EQ(RunRecord(options), 0);
    const auto original = ReadReplayModeFile(options.record_path);
    FinalizationOutputFault fault{options.record_path, "output-publish", true, false, 0, 0};
    {
        const ScopedFinalizationOutputFault control(fault);
        EXPECT_EQ(RunRecord(options), 1);
    }
    EXPECT_EQ(fault.payload_hits, 1u);
    EXPECT_EQ(ReadReplayModeFile(options.record_path), original);
    ASSERT_TRUE(fs::exists(options.record_path + ".incomplete"));
    ExpectAdmissionRefused("recording finalization is incomplete");
    EXPECT_EQ(RunRecord(options), 1);
    EXPECT_EQ(ReadReplayModeFile(options.record_path), original);
}

TEST_F(ReplayModeAdmission, IncompleteMarkerRefusesEmptyPartialAndWrongTypeBeforeOutput) {
    options.ticks = 0;
    ASSERT_EQ(RunRecord(options), 0);
    const auto original = ReadReplayModeFile(options.record_path);
    const auto marker = fs::path(options.record_path + ".incomplete");
    for (unsigned kind = 0; kind < 3; ++kind) {
        SCOPED_TRACE(kind);
        if (kind == 2) {
            fs::create_directory(marker);
        } else {
            std::ofstream output(marker);
            output << (kind == 0 ? "" : "Incomplete replay");
            ASSERT_TRUE(output.good());
        }
        ExpectAdmissionRefused("recording finalization is incomplete");
        EXPECT_EQ(RunRecord(options), 1);
        EXPECT_EQ(ReadReplayModeFile(options.record_path), original);
        EXPECT_TRUE(fs::is_empty(root / "worlds/saves"));
        fs::remove(marker);
    }
}

TEST_F(ReplayModeAdmission, CompleteToSuffixPreservesCapturedBootProfile) {
    options.ticks = 0;
    ASSERT_EQ(RunRecord(options), 0);
    const auto recorded = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(recorded.has_value());
    if (!recorded) {
        return;
    }
    ASSERT_EQ(recorded->header.version, replay::kLrec1BootVersion);
    replay::BoundedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(17, 0));
    ASSERT_TRUE(recorder.Open(options.record_path, recorded->header));
    // Two fixed 1024-tick segments exercise actual complete-to-suffix conversion.
    for (std::uint64_t tick = 1; tick <= 2048; ++tick) {
        ASSERT_TRUE(recorder.RecordTick(tick, {}, nullptr, tick));
    }
    ASSERT_TRUE(recorder.Finalize(options.record_path, 2048, 2048));
    const auto suffix = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(suffix.has_value());
    if (!suffix) {
        return;
    }
    EXPECT_EQ(suffix->header.version, replay::kLrec1BootElidedVersion);
    EXPECT_EQ(suffix->header.authority_boot, recorded->header.authority_boot);
    EXPECT_EQ(suffix->elided_through_tick, 1024u);
    ExpectAdmissionRefused("diagnostic suffix");
}

TEST_F(ReplayModeAdmission, StrictCompleteTruncationNeverAdmitsOrReplacesOutput) {
    options.ticks = 1;
    ASSERT_EQ(RunRecord(options), 0);
    const auto complete = ReadReplayModeFile(options.record_path);
    ASSERT_GT(complete.size(), 32u);
    options.mutate_replay_fixture = options.record_path;
    for (const auto size : {complete.size() - 32, complete.size() - 1, complete.size() + 1}) {
        auto partial = complete;
        partial.resize(size, '\0');
        {
            std::ofstream output(options.record_path, std::ios::binary | std::ios::trunc);
            output << partial;
            ASSERT_TRUE(output.good());
        }
        EXPECT_FALSE(replay::ReadReplay(options.record_path).has_value());
        EXPECT_EQ(RunMutateReplayFixture(options), 1);
        EXPECT_EQ(ReadReplayModeFile(options.record_path), partial);
        ExpectAdmissionRefused("invalid or has an unsupported LREC1 version");
    }
}

TEST_F(ReplayModeAdmission, EmptyOriginalSeedReconstructsTheRecordedResolvedBoot) {
    options.seed.clear();
    options.ticks = 0;
    ASSERT_EQ(RunRecord(options), 0);
    const auto recorded = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(recorded.has_value());
    if (!recorded) {
        return;
    }
    const auto config = ReplayRunnerConfigFrom(options, recorded->header);
    ASSERT_TRUE(config.has_value());
    if (!config) {
        return;
    }
    EXPECT_FALSE(config->seed.empty());
    ASSERT_EQ(RunReplay(options), 0) << logs.str();
    EXPECT_EQ(Artifact().at("start_world_hash_match"), true);
}

TEST_F(ReplayModeAdmission, RecordingBudgetRefusesBeforeBootAndPreservesOutput) {
    std::ofstream(options.record_path) << "retained";
    // A regressed preflight must fail promptly instead of simulating a huge run.
    // The specific diagnostic below distinguishes budget admission from boot failure.
    options.root = (root / "unopened").string();
    // At 967741 ticks the inputs plus ceil(ticks / 30) checkpoints fit exactly.
    // The next tick exceeds the reader budget only when checkpoints are counted;
    // rounding down would incorrectly admit that off-cadence terminal checkpoint.
    for (const std::uint64_t ticks : {std::uint64_t{967742},
                                      std::uint64_t{replay::ReplayReadLimits{}.records},
                                      (std::numeric_limits<std::uint64_t>::max)()}) {
        SCOPED_TRACE(ticks);
        options.ticks = ticks;
        logs.str("");
        EXPECT_EQ(RunRecord(options), 1);
        EXPECT_NE(logs.str().find("requested workload exceeds replay admission budget"),
                  std::string::npos);
        EXPECT_FALSE(fs::exists(options.root));
        EXPECT_FALSE(fs::exists(root / "worlds/saves"));
        EXPECT_EQ(ReadReplayModeFile(options.record_path), "retained");
    }
}

TEST_F(ReplayModeAdmission, InvalidRecordingRosterRefusesBeforeBootOrOutput) {
    for (const int avatars : {-1, 1025}) {
        SCOPED_TRACE(avatars);
        options.avatars = avatars;
        options.ticks = 0;
        logs.str("");
        EXPECT_EQ(RunRecord(options), 1);
        EXPECT_EQ(RunRecordedLockstepLoopback(options), 1);
        EXPECT_EQ(logs.str().find("Water resolution wired:"), std::string::npos);
        EXPECT_FALSE(fs::exists(options.record_path));
        EXPECT_FALSE(fs::exists(root / "worlds/saves"));
    }
}

TEST_F(ReplayModeAdmission, AutomaticRecordingRefusesConflictingCapturedBootBeforeOutput) {
    namespace net = Luminumbra::Net;
    options.ticks = 0;
    ASSERT_EQ(RunRecord(options), 0);
    const auto recorded = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(recorded.has_value());
    if (!recorded) {
        return;
    }
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        SCOPED_TRACE(mutation);
        auto header = recorded->header;
        ASSERT_TRUE(header.authority_boot.has_value());
        if (!header.authority_boot) {
            return;
        }
        switch (mutation) {
            case 0:
                header.version = replay::kLrec1Version;
                break;
            case 1:
                header.authority_boot.reset();
                break;
            case 2:
                ++header.authority_boot->resolved_world_seed;
                break;
            case 3:
                header.preset = "other";
                break;
            case 4:
                ++header.tick_rate_hz;
                break;
            case 5:
                header.system_config_identity = "other";
                break;
            default:
                ++header.seed;
                break;
        }
        net::LockstepConfig config;
        config.seed = static_cast<std::uint32_t>(recorded->header.seed);
        config.preset = recorded->header.preset;
        config.system_config_identity = recorded->header.system_config_identity;
        config.preset_content_hash = recorded->header.preset_content_hash;
        config.replay_header = std::make_shared<const replay::ReplayHeader>(header);
        auto [local, peer] = net::MakeLoopbackPair();
        net::LockstepSession session(config, local.get(), {});
        const auto path = (root / "refused.lrec").string();
        session.SetDumpPath(path);
        auto remote = config;
        remote.local_client_id = 1;
        remote.peer_client_id = 0;
        ASSERT_TRUE(peer->SendFrame(net::EncodeHello(net::MakeLockstepHello(remote))));
        ASSERT_TRUE(session.Handshake());
        EXPECT_EQ(session.Status().replay_error,
                  "replay boot conflicts with session configuration");
        EXPECT_FALSE(fs::exists(path));
        EXPECT_EQ(session.AdmissionStatus().replay_segments, 0u);
    }
}

TEST_F(ReplayModeAdmission, FixtureMutationRecoversInterruptedPublicationBeforeReplacing) {
    using Identity = std::array<std::uint64_t, 3>;
    const auto identify = [](const fs::path& path) {
        const Luminumbra::Persistence::SavedInputFile file(path);
        Identity identity{};
        EXPECT_TRUE(file.identity(identity));
        return identity;
    };
    options.mutate_replay_fixture = options.record_path;
    const auto companion = options.record_path + ".regions.json";
    const auto journal = root / ".replay-pair.undo";
    for (const bool committed : {false, true}) {
        SCOPED_TRACE(committed);
        options.ticks = 1;
        ASSERT_EQ(RunRecord(options), 0);
        std::string error;
        replay::RegionScheduleTrace old_trace;
        ASSERT_TRUE(replay::ReadRegionScheduleTrace(options.record_path, 1, old_trace, error));
        const auto previous = identify(options.record_path);
        const auto previous_trace = identify(companion);
        const auto undo = ReadReplayModeFile(companion);
        // Both generations must carry the schedule playback actually derives.
        // Different tick counts make restoring the wrong companion observable.
        auto incoming_options = options;
        incoming_options.record_path = (root / "incoming").string();
        incoming_options.ticks = 2;
        ASSERT_EQ(RunRecord(incoming_options), 0);
        replay::RegionScheduleTrace new_trace;
        ASSERT_TRUE(
            replay::ReadRegionScheduleTrace(incoming_options.record_path, 2, new_trace, error));
        const auto incoming = identify(root / "incoming");
        fs::remove(companion);
        fs::rename(incoming_options.record_path + ".regions.json", companion);
        const auto incoming_trace = identify(companion);
        const nlohmann::json header{
            {"schema", "luminumbra.replay_pair.v1"},
            {"replay", "recording.lrec"},
            {"previous", previous},
            {"incoming", incoming},
            {"old_trace", previous_trace},
            {"new_trace", incoming_trace},
            {"undo_bytes", undo.size()},
            {"undo_checksum", Luminumbra::Persistence::fnv1a64_bytes(undo.data(), undo.size())}};
        {
            std::ofstream output(journal, std::ios::binary);
            output << header.dump() << '\n' << undo;
            ASSERT_TRUE(output.good());
        }
        if (committed) {
            fs::rename(options.record_path, root / "previous");
            fs::rename(root / "incoming", options.record_path);
        }
        ASSERT_EQ(RunMutateReplayFixture(options), 0);
        EXPECT_FALSE(fs::exists(journal));
        {
            const Luminumbra::Persistence::ReplayPairReadGuard playback(options.replay_path);
            ASSERT_TRUE(playback.is_open());
            replay::RegionScheduleTrace actual;
            ASSERT_TRUE(replay::ReadRegionScheduleTrace(
                options.replay_path, committed ? 2 : 1, actual, error));
            EXPECT_EQ(actual, committed ? new_trace : old_trace);
        }
        // Admission succeeds and reaches the deliberately corrupted checkpoint.
        ASSERT_EQ(RunReplay(options), 1);
        const auto artifact = Artifact();
        EXPECT_EQ(artifact.at("diverged"), true);
        EXPECT_EQ(artifact.at("divergence_tick"), committed ? 2 : 1);
        EXPECT_EQ(artifact.at("divergence_section"), "terrain");
    }
}

TEST_F(ReplayModeAdmission, FixtureMutationRefusalsPreserveReplayTraceAndJournal) {
    options.ticks = 1;
    options.mutate_replay_fixture = options.record_path;
    ASSERT_EQ(RunRecord(options), 0);
    const auto previous = ReadReplayModeFile(options.record_path);
    const auto companion = options.record_path + ".regions.json";
    const auto trace_bytes = ReadReplayModeFile(companion);
    const auto journal = root / ".replay-pair.undo";
    {
        const Luminumbra::Persistence::ReplayPairReadGuard publisher(options.record_path);
        ASSERT_TRUE(publisher.is_open());
        EXPECT_EQ(RunMutateReplayFixture(options), 1);
        EXPECT_EQ(ReadReplayModeFile(options.record_path), previous);
        EXPECT_EQ(ReadReplayModeFile(companion), trace_bytes);
    }
    for (const auto& record : {std::string{}, std::string{"{\"schema\":"}}) {
        {
            std::ofstream output(journal, std::ios::binary);
            output << record;
            ASSERT_TRUE(output.good());
        }
        EXPECT_EQ(RunMutateReplayFixture(options), 1);
        EXPECT_EQ(ReadReplayModeFile(options.record_path), previous);
        EXPECT_EQ(ReadReplayModeFile(companion), trace_bytes);
        EXPECT_TRUE(fs::exists(journal));
        EXPECT_EQ(ReadReplayModeFile(journal), record);
        fs::remove(journal);
    }
    // Failed source reads release ownership, including missing and partial files.
    for (const bool missing : {false, true}) {
        if (missing) {
            fs::remove(options.record_path);
        } else {
            std::ofstream output(options.record_path, std::ios::binary);
            output << "LREC1";
            ASSERT_TRUE(output.good());
        }
        EXPECT_EQ(RunMutateReplayFixture(options), 1);
        EXPECT_EQ(ReadReplayModeFile(companion), trace_bytes);
        const Luminumbra::Persistence::ReplayPairReadGuard released(options.record_path);
        EXPECT_TRUE(released.is_open());
        EXPECT_EQ(fs::exists(options.record_path), !missing);
        if (!missing) {
            EXPECT_EQ(ReadReplayModeFile(options.record_path), "LREC1");
        }
    }
    options.ticks = 0;
    ASSERT_EQ(RunRecord(options), 0);
    const auto empty = ReadReplayModeFile(options.record_path);
    EXPECT_EQ(RunMutateReplayFixture(options), 1);
    EXPECT_EQ(ReadReplayModeFile(options.record_path), empty);
    const Luminumbra::Persistence::ReplayPairReadGuard released(options.record_path);
    EXPECT_TRUE(released.is_open());
}

TEST_F(ReplayModeAdmission, FixtureMutationRefusesIncompleteStreamsWithoutReplacing) {
    options.ticks = 1;
    options.mutate_replay_fixture = options.record_path;
    ASSERT_EQ(RunRecord(options), 0);
    // Keep the legacy partial-result contract covered alongside strict C4 reads.
    const auto recorded = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(recorded.has_value());
    if (!recorded) {
        return;
    }
    auto legacy = *recorded;
    legacy.header.version = replay::kLrec1Version;
    legacy.header.authority_boot.reset();
    Rewrite(legacy);
    ASSERT_FALSE(testing::Test::HasFatalFailure());
    const auto companion = options.record_path + ".regions.json";
    const auto trace_bytes = ReadReplayModeFile(companion);
    auto bytes = ReadReplayModeFile(options.record_path);
    constexpr std::size_t trailer_bytes = sizeof(replay::kLrec1Trailer) + 3 * sizeof(std::uint64_t);
    ASSERT_GT(bytes.size(), trailer_bytes);
    const auto complete_size = bytes.size();
    bytes.push_back('\0');
    // Missing trailer, partial magic, partial counts, and junk after a valid
    // trailer must all refuse, even with a complete checkpoint to corrupt.
    for (const auto length : {complete_size - trailer_bytes,
                              complete_size - trailer_bytes + 1,
                              complete_size - 1,
                              complete_size + 1}) {
        SCOPED_TRACE(length);
        const auto partial = bytes.substr(0, length);
        {
            std::ofstream output(options.record_path, std::ios::binary | std::ios::trunc);
            output << partial;
            ASSERT_TRUE(output.good());
        }
        const auto contents = replay::ReadReplay(options.record_path);
        ASSERT_TRUE(contents.has_value());
        if (!contents.has_value()) {
            return;
        }
        ASSERT_TRUE(contents->truncated);
        ASSERT_EQ(contents->trailer_present, length > complete_size);
        ASSERT_FALSE(contents->checkpoints.empty());
        logs.str("");
        logs.clear();
        EXPECT_EQ(RunMutateReplayFixture(options), 1);
        EXPECT_NE(logs.str().find("truncated (no valid trailer)"), std::string::npos) << logs.str();
        EXPECT_EQ(ReadReplayModeFile(options.record_path), partial);
        EXPECT_EQ(ReadReplayModeFile(companion), trace_bytes);
        EXPECT_FALSE(fs::exists(root / ".replay-pair.undo"));
        const Luminumbra::Persistence::ReplayPairReadGuard released(options.record_path);
        EXPECT_TRUE(released.is_open());
    }
}

TEST_P(ReplayModeRoundtrip, AuthenticatesEveryExecutedTickAndPreservesCadenceBytes) {
    options.ticks = GetParam();
    ASSERT_EQ(RunRecord(options), 0);
    const auto contents = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    ASSERT_FALSE(contents->truncated);
    ASSERT_TRUE(contents->trailer_present);
    EXPECT_EQ(contents->tick_count, options.ticks);
    ASSERT_EQ(contents->inputs.size(), options.ticks);
    std::uint64_t tick = 0;
    for (const auto& input : contents->inputs) {
        EXPECT_EQ(input.tick, ++tick);
        EXPECT_TRUE(input.inputs.empty());
    }
    const auto checkpoint_count = options.ticks / 30 + (options.ticks % 30 != 0 ? 1u : 0u);
    ASSERT_EQ(contents->checkpoints.size(), checkpoint_count);
    std::uint64_t checkpoint_tick = 30;
    for (const auto& checkpoint : contents->checkpoints) {
        EXPECT_EQ(checkpoint.tick, (std::min)(checkpoint_tick, options.ticks));
        checkpoint_tick += 30;
    }
    if (options.ticks == 30 || options.ticks == 90) {
        // Reconstruct the pre-fix interleaving, including the original header
        // and trailer. No extra checkpoint or record reordering is permitted.
        const auto legacy_path = (root / "legacy.lrec").string();
        replay::ReplayWriter legacy;
        ASSERT_TRUE(legacy.Open(legacy_path, contents->header));
        auto checkpoint = contents->checkpoints.cbegin();
        for (const auto& input : contents->inputs) {
            legacy.RecordInput(input.tick, input.inputs);
            if (input.tick % 30 == 0) {
                ASSERT_NE(checkpoint, contents->checkpoints.cend());
                legacy.RecordCheckpoint(*checkpoint++);
            }
        }
        ASSERT_TRUE(legacy.Finalize(options.ticks));
        EXPECT_EQ(ReadReplayModeFile(options.record_path), ReadReplayModeFile(legacy_path));
    }
    ASSERT_EQ(RunReplay(options), 0);
    const auto artifact = Artifact();
    EXPECT_EQ(artifact.at("passed"), true);
    EXPECT_EQ(artifact.at("start_world_hash_match"), true);
    EXPECT_EQ(artifact.at("end_hash_match"), true);
    EXPECT_EQ(artifact.at("ticks_replayed"), options.ticks);
    EXPECT_EQ(artifact.at("checkpoints_verified"), checkpoint_count);
    EXPECT_EQ(artifact.at("input_records"), options.ticks);
    EXPECT_EQ(artifact.at("end_world_hash"),
              options.ticks == 0 ? contents->header.start_world_hash
                                 : contents->checkpoints.back().world_hash);
}

INSTANTIATE_TEST_SUITE_P(TickBoundaries,
                         ReplayModeRoundtrip,
                         testing::Values(std::uint64_t{0}, 1, 2, 29, 30, 31, 60, 90));

TEST_F(ReplayModeAdmission, ForeignEngineIsReadableButRefusedBeforeConfiguration) {
    options.ticks = 30;
    ASSERT_EQ(RunRecord(options), 0);
    auto contents = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    const auto foreign_engine = contents->header.engine_version + "-foreign";
    for (const auto& engine : {foreign_engine, std::string()}) {
        SCOPED_TRACE(engine);
        contents->header.engine_version = engine;
        Rewrite(*contents);
        ASSERT_FALSE(testing::Test::HasFatalFailure());
        const auto reread = replay::ReadReplay(options.replay_path);
        ASSERT_TRUE(reread.has_value());
        if (!reread.has_value()) {
            return;
        }
        EXPECT_EQ(reread->header.engine_version, engine);
        ExpectAdmissionRefused("engine mismatch");
        EXPECT_EQ(RunReplay(options), 1);
        EXPECT_EQ(ReadReplayModeFile(options.artifact_path), "retained");
    }
}

TEST_F(ReplayModeAdmission, IsolatedInputMutationsAreReadableButRefusedBeforeConfiguration) {
    options.ticks = 30; // Otherwise-valid cadence replay also passed on the base branch.
    ASSERT_EQ(RunRecord(options), 0);
    const auto valid = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(valid.has_value());
    if (!valid.has_value()) {
        return;
    }
    ASSERT_EQ(valid->inputs.size(), 30u);
    ASSERT_EQ(RunReplay(options), 0);
    // Each mutation retains the valid header, all hashes and the trailer tick
    // count. Rewrite updates record counts so parsing cannot mask admission.
    for (int mutation = 0; mutation < 12; ++mutation) {
        SCOPED_TRACE(mutation);
        auto changed = *valid;
        switch (mutation) {
            case 0:
                changed.inputs.clear();
                break;
            case 1:
                changed.inputs.erase(changed.inputs.begin());
                break;
            case 2:
                changed.inputs.pop_back();
                break;
            case 3:
                changed.inputs[14].tick = 16;
                break; // Interior gap/duplicate.
            case 4:
                changed.inputs[1].tick = 1;
                break;
            case 5:
                std::swap(changed.inputs[0], changed.inputs[1]);
                break;
            case 6:
                changed.inputs.push_back({31, {}});
                break;
            case 7:
                changed.inputs.front().tick = 0;
                break;
            case 8:
                changed.inputs.back().tick = (std::numeric_limits<std::uint64_t>::max)();
                break;
            case 9:
                changed.inputs.front().inputs = {0};
                break;
            case 10:
                changed.inputs[14].inputs = {1, 2, 3};
                break;
            case 11:
                changed.inputs.back().inputs = {0};
                break;
            default:
                FAIL() << "unhandled mutation";
        }
        Rewrite(changed);
        ASSERT_FALSE(testing::Test::HasFatalFailure());
        ExpectAdmissionRefused("input");
        // Use the valid runtime as well: on the base branch ignored mutations
        // would complete successfully with the unchanged tick-30 checkpoint.
        EXPECT_EQ(RunReplay(options), 1);
        EXPECT_EQ(ReadReplayModeFile(options.artifact_path), "retained");
    }
}

TEST_F(ReplayModeAdmission, ZeroTicksUseStartHashAndRefuseExtraInputs) {
    options.ticks = 0;
    ASSERT_EQ(RunRecord(options), 0);
    const auto valid = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(valid.has_value());
    if (!valid.has_value()) {
        return;
    }
    auto changed = *valid;
    changed.inputs.push_back({1, {}});
    Rewrite(changed);
    ExpectAdmissionRefused("input");
    changed = *valid;
    changed.header.start_world_hash = "tampered";
    Rewrite(changed);
    ASSERT_EQ(RunReplay(options), 1);
    const auto artifact = Artifact();
    EXPECT_EQ(artifact.at("diverged"), true);
    EXPECT_EQ(artifact.at("divergence_tick"), 0);
    EXPECT_EQ(artifact.at("expected_world_hash"), "tampered");
}

class ReplayModeTerminal : public ReplayModeAdmission,
                           public testing::WithParamInterface<std::uint64_t> {};

TEST_P(ReplayModeTerminal, MissingTerminalIsExplicitlyRefusedBeforeConfiguration) {
    options.ticks = GetParam();
    ASSERT_EQ(RunRecord(options), 0);
    auto contents = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    ASSERT_FALSE(contents->checkpoints.empty());
    contents->checkpoints.pop_back();
    Rewrite(*contents);
    ExpectAdmissionRefused("missing authenticated terminal checkpoint");
}

TEST_P(ReplayModeTerminal, EveryTerminalHashIsVerifiedAndLocalized) {
    options.ticks = GetParam();
    ASSERT_EQ(RunRecord(options), 0);
    const auto valid = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(valid.has_value());
    if (!valid.has_value()) {
        return;
    }
    ASSERT_FALSE(valid->checkpoints.empty());
    for (const auto* section : {"world_hash", "terrain", "water", "entities"}) {
        SCOPED_TRACE(section);
        auto changed = *valid;
        auto& terminal = changed.checkpoints.back();
        const std::string field(section);
        if (field == "world_hash") {
            terminal.world_hash = "tampered";
        } else if (field == "terrain") {
            terminal.terrain = "tampered";
        } else if (field == "water") {
            terminal.water = "tampered";
        } else {
            terminal.entities = "tampered";
        }
        Rewrite(changed);
        ASSERT_EQ(RunReplay(options), 1);
        const auto artifact = Artifact();
        EXPECT_EQ(artifact.at("diverged"), true);
        EXPECT_EQ(artifact.at("divergence_tick"), options.ticks);
        EXPECT_EQ(artifact.at("checkpoints_verified_before_divergence"),
                  options.ticks / 30 - (options.ticks % 30 == 0 ? 1u : 0u));
        if (field != "water") {
            EXPECT_EQ(artifact.at("divergence_section"), field);
        } else {
            EXPECT_EQ(artifact.at("divergence_section").get<std::string>().find("water"), 0u);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(PositiveTickBoundaries,
                         ReplayModeTerminal,
                         testing::Values(std::uint64_t{1}, 2, 29, 30, 31, 60, 90));

TEST_F(ReplayModeAdmission, MissingCadenceCheckpointStillFails) {
    options.ticks = 60;
    ASSERT_EQ(RunRecord(options), 0);
    auto contents = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    ASSERT_EQ(contents->checkpoints.size(), 2u);
    contents->checkpoints.erase(contents->checkpoints.begin());
    Rewrite(*contents);
    ASSERT_EQ(RunReplay(options), 1);
    EXPECT_NE(logs.str().find("missing recorded checkpoint at tick 30"), std::string::npos);
}

TEST_F(ReplayModeAdmission, CheckpointsInReverseRecordedOrderStillVerify) {
    options.ticks = 90;
    ASSERT_EQ(RunRecord(options), 0);
    auto contents = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    ASSERT_EQ(contents->checkpoints.size(), 3u);
    std::reverse(contents->checkpoints.begin(), contents->checkpoints.end());
    Rewrite(*contents);
    ASSERT_EQ(RunReplay(options), 0) << logs.str();
    const auto artifact = Artifact();
    EXPECT_EQ(artifact.at("passed"), true);
    EXPECT_EQ(artifact.at("checkpoints_verified"), 3u);
    EXPECT_EQ(artifact.at("end_hash_match"), true);
}

TEST_F(ReplayModeAdmission, UnreadableAndPartialStreamsRefuseBeforeConfiguration) {
    ExpectAdmissionRefused("invalid or has an unsupported LREC1 version");
    options.ticks = 30;
    ASSERT_EQ(RunRecord(options), 0);
    const auto bytes = ReadReplayModeFile(options.record_path);
    ASSERT_GT(bytes.size(), 32u);
    for (const auto length : {std::size_t{0}, std::size_t{3}, bytes.size() - 1}) {
        SCOPED_TRACE(length);
        {
            std::ofstream output(options.replay_path, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(length));
            ASSERT_TRUE(output.good());
        }
        ExpectAdmissionRefused("invalid or has an unsupported LREC1 version");
    }
}
} // namespace
