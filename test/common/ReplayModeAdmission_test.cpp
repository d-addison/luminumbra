#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_server/modes/ModeHelpers.h"
#include "luminumbra_server/modes/Modes.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
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
