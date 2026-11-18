#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/Log.h"
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
#include <sstream>
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
        if (++fault.calls != fault.refuse_call) {
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
#include "../../src/luminumbra_server/modes/Heavy.cpp"
#include "../../src/luminumbra_server/modes/Replay.cpp"
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

// Exercise the public shipping modes, with real checkpoint/configuration
// fields. Mutations change only the field named by each case, never a stub runner.
class ReplayModeAdmission : public testing::Test {
protected:
    fs::path root;
    ServerCliOptions options;
    std::ostringstream logs;
    std::shared_ptr<spdlog::logger> previous_logger;

    void SetUp() override {
        previous_logger = Log::GetCoreLogger();
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
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(logs);
        auto logger = std::make_shared<spdlog::logger>("replay-mode-test", sink);
        logger->set_pattern("%v");
        Log::GetCoreLogger() = std::move(logger);
    }

    void TearDown() override {
        CaptureFault() = {};
        Log::GetCoreLogger() = previous_logger;
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

TEST_F(ReplayModeAdmission, BootSchemaIsOptInAndRefusedUntilPlaybackWiring) {
    options.ticks = 0;
    ASSERT_EQ(RunRecord(options), 0);
    const auto recorded = replay::ReadReplay(options.record_path);
    ASSERT_TRUE(recorded.has_value());
    if (!recorded.has_value()) {
        return;
    }
    EXPECT_EQ(recorded->header.version, 3u);
    EXPECT_FALSE(recorded->header.authority_boot.has_value());
    for (const auto version : {replay::kLrec1BootVersion, replay::kLrec1BootElidedVersion}) {
        auto changed = *recorded;
        changed.header.version = version;
        Luminumbra::world::AuthorityBootDescriptor boot;
        boot.authority_rules_revision =
            luminumbra::core::SimulationIdentityValues::kAuthorityRulesRevision;
        boot.system_config_identity = changed.header.system_config_identity;
        changed.header.authority_boot = boot;
        Rewrite(changed);
        ASSERT_FALSE(testing::Test::HasFatalFailure());
        ExpectAdmissionRefused("unsupported LREC version");
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
        ExpectAdmissionRefused(length < 4 ? "invalid or has an unsupported LREC1 version"
                                          : "truncated");
    }
}
} // namespace
