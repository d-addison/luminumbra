#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/persistence/ReplacementOutputFile.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/SavedInputFile.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/WeatherSystem.h"
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
#include <utility>
#include <vector>

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
        Log::GetCoreLogger() = previous_logger;
        std::error_code error;
        fs::remove_all(root, error);
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
    changed.version = 7;
    EXPECT_FALSE(ReplayRunnerConfigFrom(options, changed).has_value());
    const auto complete = ReadReplayModeFile(options.record_path);
    ASSERT_FALSE(complete.empty());
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
