// LREC1 session replay stream unit tests
// (src/luminumbra_common/replay/ReplayStream.{h,cpp}).
//
// These tests lock the ENGINE-GENERIC stream behavior independent of the server:
// header roundtrip, record append/read identity (including the empty-input
// compact case), truncation detection, checkpoint encode/decode, and the
// versioning gate. They are hermetic -- every stream is written to a unique temp
// file in a reserved temporary directory and removed afterward, so the suite never
// touches the committed gate fixtures.

#include "../support/SystemConfigVariants.h"
#include "../support/UniqueTempPath.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "luminumbra_common/persistence/ReplacementOutputFile.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/SavedInputFile.h"
#include "luminumbra_common/replay/RegionScheduleTrace.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace replay = Luminumbra::Replay;
namespace fs = std::filesystem;

namespace {

fs::path ReplayTempDir() {
    // CTest runs cases in separate processes. Reserve this process's namespace
    // exclusively and retain it until all local sessions/recorders have closed.
    static const Luminumbra::TestSupport::ScopedTempFile workspace("replay_", ".unused");
    return workspace.Path().parent_path();
}

class ReplayStreamTest : public ::testing::Test {
protected:
    void SetUp() override {
        const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
        auto name = std::string(info->name());
        std::replace(name.begin(), name.end(), '/', '_');
        m_path = (ReplayTempDir() / (name + ".lrec1")).string();
        std::error_code ec;
        fs::remove(m_path, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove(m_path, ec);
    }

    replay::ReplayHeader MakeHeader() const {
        replay::ReplayHeader h;
        h.version = replay::kLrec1Version;
        h.tick_rate_hz = 30;
        h.seed = 424242;
        h.seed_string = "424242";
        h.preset = "default";
        h.preset_hash = 0xABCDEF0123456789ULL;
        h.start_world_hash = "9d9d500dd22504f4";
        h.surface_radius = 4;
        h.collision_radius = 2;
        h.engine_version = "0.1.0+testsha";
        h.system_config_identity = Luminumbra::world::GameSession{}.SystemConfigIdentity();
        h.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
        return h;
    }

    std::string ReadBytes() const {
        std::ifstream file(m_path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    std::string m_path;
};

// The existing publication hook executes on the finalizing thread. Each test
// arms it only after recording has drained, and restores it on every exit.
struct OutputFailure {
    std::string step;
    fs::path destination;
    std::size_t skip = 0;
    std::size_t hits = 0;
};
OutputFailure*& CurrentOutputFailure() {
    static OutputFailure* failure = nullptr;
    return failure;
}
void RefuseOutputStep(const char* step, const fs::path& path) {
    auto* failure = CurrentOutputFailure();
    if (failure && failure->step == step &&
        (failure->destination.empty() || failure->destination == path)) {
        ++failure->hits;
        if (failure->hits > failure->skip) {
            throw std::runtime_error("output refused");
        }
    }
}
class ScopedOutputFailure {
public:
    explicit ScopedOutputFailure(OutputFailure& failure) {
        CurrentOutputFailure() = &failure;
        Luminumbra::Persistence::set_snapshot_step_hook_for_testing(RefuseOutputStep);
    }
    ~ScopedOutputFailure() {
        Luminumbra::Persistence::set_snapshot_step_hook_for_testing(nullptr);
        CurrentOutputFailure() = nullptr;
    }
};

class ReplayOutputFailureTest : public ReplayStreamTest,
                                public testing::WithParamInterface<const char*> {};

TEST_P(ReplayOutputFailureTest, BufferedRefusalPreservesOldReplayAndCloses) {
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(1, {7});
    ASSERT_TRUE(writer.Finalize(1));
    const auto old = ReadBytes();
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(2, {8});
    OutputFailure failure{GetParam(), m_path, 0, 0};
    {
        const ScopedOutputFailure control(failure);
        EXPECT_FALSE(writer.Finalize(2));
    }
    EXPECT_EQ(failure.hits, 1u);
    EXPECT_EQ(ReadBytes(), old);
    writer.RecordInput(3, {9});
    EXPECT_EQ(writer.InputRecordCount(), 1u);
    EXPECT_FALSE(writer.Finalize(3));
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(4, {10});
    ASSERT_TRUE(writer.Finalize(4));
    EXPECT_NE(ReadBytes(), old);
    const auto contents = replay::ReadReplay(m_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    EXPECT_FALSE(contents->truncated);
    ASSERT_EQ(contents->inputs.size(), 1u);
    EXPECT_EQ(contents->inputs.front().tick, 4u);
}

TEST_P(ReplayOutputFailureTest, BoundedRefusalPreservesOldReplayAndCloses) {
    {
        std::ofstream old(m_path, std::ios::binary);
        old << "previous replay";
        ASSERT_TRUE(old.good());
    }
    const auto old = ReadBytes();
    replay::BoundedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(256, 4));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    ASSERT_TRUE(recorder.RecordTick(1, {7}, nullptr, 1));
    OutputFailure failure{GetParam(), m_path, 0, 0};
    {
        const ScopedOutputFailure control(failure);
        EXPECT_FALSE(recorder.Finalize(m_path, 1, 1));
    }
    EXPECT_EQ(failure.hits, 1u);
    EXPECT_EQ(ReadBytes(), old);
    EXPECT_EQ(recorder.SegmentCount(), 0u);
    EXPECT_EQ(recorder.RetainedBytes(), 0u);
    EXPECT_FALSE(recorder.RecordTick(2, {8}, nullptr, 2));
    EXPECT_FALSE(recorder.Finalize(m_path, 1, 1));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    ASSERT_TRUE(recorder.RecordTick(1, {9}, nullptr, 1));
    ASSERT_TRUE(recorder.Finalize(m_path, 1, 1));
    EXPECT_NE(ReadBytes(), old);
}

TEST_P(ReplayOutputFailureTest, QueuedRefusalDrainsClosesAndCanReopen) {
    {
        std::ofstream old(m_path, std::ios::binary);
        old << "previous replay";
        ASSERT_TRUE(old.good());
    }
    const auto old = ReadBytes();
    replay::QueuedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(256, 4));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    const auto inputs = std::make_shared<const std::vector<std::uint8_t>>(1, 7);
    ASSERT_TRUE(recorder.RecordTick(1, inputs, nullptr, 1));
    ASSERT_TRUE(recorder.Flush());
    OutputFailure failure{GetParam(), m_path, 0, 0};
    {
        const ScopedOutputFailure control(failure);
        EXPECT_FALSE(recorder.Finalize(m_path, 1, 1));
    }
    EXPECT_EQ(failure.hits, 1u);
    EXPECT_EQ(ReadBytes(), old);
    EXPECT_EQ(recorder.PendingTicks(), 0u);
    EXPECT_EQ(recorder.PendingBytes(), 0u);
    EXPECT_EQ(recorder.SegmentCount(), 0u);
    EXPECT_EQ(recorder.RetainedBytes(), 0u);
    EXPECT_FALSE(recorder.RecordTick(2, inputs, nullptr, 2));
    EXPECT_FALSE(recorder.Finalize(m_path, 1, 1));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    ASSERT_TRUE(recorder.RecordTick(1, inputs, nullptr, 1));
    ASSERT_TRUE(recorder.Finalize(m_path, 1, 1));
    EXPECT_FALSE(fs::exists(m_path + ".incomplete"));
}

TEST_P(ReplayOutputFailureTest, TraceRefusalPreservesOldCompanion) {
    std::string error;
    const replay::RegionScheduleTrace old{{1, "0123456789abcdef"}};
    const replay::RegionScheduleTrace next{{1, "fedcba9876543210"}};
    ASSERT_TRUE(replay::WriteRegionScheduleTrace(m_path, old, error));
    const auto companion = m_path + ".regions.json";
    const auto read_companion = [&companion] {
        std::ifstream input(companion, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto old_bytes = read_companion();
    OutputFailure failure{GetParam(), companion, 0, 0};
    {
        const ScopedOutputFailure control(failure);
        EXPECT_FALSE(replay::WriteRegionScheduleTrace(m_path, next, error));
    }
    EXPECT_EQ(failure.hits, 1u);
    EXPECT_EQ(read_companion(), old_bytes);
    replay::RegionScheduleTrace actual;
    ASSERT_TRUE(replay::ReadRegionScheduleTrace(m_path, 1, actual, error));
    EXPECT_EQ(actual, old);
    ASSERT_TRUE(replay::WriteRegionScheduleTrace(m_path, next, error));
    ASSERT_TRUE(replay::ReadRegionScheduleTrace(m_path, 1, actual, error));
    EXPECT_EQ(actual, next);
    fs::remove(m_path + ".regions.json");
}

// Each boundary runs after an existing pair has been installed. In particular,
// output-publish for the replay is reached after the new trace rename.
TEST_P(ReplayOutputFailureTest, PairRefusalPreservesReplayAndTraceByteForByte) {
    replay::ReplayWriter writer;
    const replay::RegionScheduleTrace old_trace{{1, "0123456789abcdef"}};
    const replay::RegionScheduleTrace new_trace{{1, "fedcba9876543210"}};
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(1, {7});
    ASSERT_TRUE(writer.Finalize(1, &old_trace));
    const auto old_replay = ReadBytes();
    const auto companion = m_path + ".regions.json";
    const auto read_trace = [&companion] {
        std::ifstream file(companion, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    const auto old_bytes = read_trace();
    for (const bool remove_trace : {false, true}) {
        ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
        writer.RecordInput(1, {8});
        OutputFailure failure{GetParam(), m_path, 0, 0};
        {
            const ScopedOutputFailure control(failure);
            EXPECT_FALSE(writer.Finalize(1, remove_trace ? nullptr : &new_trace));
        }
        EXPECT_EQ(failure.hits, 1u);
        EXPECT_EQ(ReadBytes(), old_replay);
        EXPECT_EQ(read_trace(), old_bytes);
        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
        EXPECT_TRUE(guard.is_open());
    }
    fs::remove(companion);
}

TEST_P(ReplayOutputFailureTest, InitialPairRefusalPreservesAbsenceAndOrphanTrace) {
    const replay::RegionScheduleTrace next{{1, "fedcba9876543210"}};
    const auto companion = m_path + ".regions.json";
    for (const bool orphan_trace : {false, true}) {
        if (orphan_trace) {
            std::ofstream file(companion, std::ios::binary);
            file << "orphan trace";
            ASSERT_TRUE(file.good());
        }
        replay::ReplayWriter writer;
        ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
        OutputFailure failure{GetParam(), m_path, 0, 0};
        {
            const ScopedOutputFailure control(failure);
            EXPECT_FALSE(writer.Finalize(1, &next));
        }
        EXPECT_EQ(failure.hits, 1u);
        EXPECT_FALSE(fs::exists(m_path));
        const Luminumbra::Persistence::SavedInputFile trace(companion);
        if (orphan_trace) {
            std::string bytes;
            ASSERT_TRUE(trace.read(bytes, 64));
            EXPECT_EQ(bytes, "orphan trace");
        } else {
            EXPECT_TRUE(trace.missing());
        }
    }
    fs::remove(companion);
}

TEST_P(ReplayOutputFailureTest, PairTraceRefusalPreservesBothDestinations) {
    replay::ReplayWriter writer;
    const replay::RegionScheduleTrace trace{{1, "0123456789abcdef"}};
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(1, &trace));
    const auto old = ReadBytes();
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(1, {9});
    OutputFailure failure{GetParam(), m_path + ".regions.json", 0, 0};
    {
        const ScopedOutputFailure control(failure);
        EXPECT_FALSE(writer.Finalize(1, &trace));
    }
    EXPECT_EQ(failure.hits, 1u);
    EXPECT_EQ(ReadBytes(), old);
    replay::RegionScheduleTrace actual;
    std::string error;
    ASSERT_TRUE(replay::ReadRegionScheduleTrace(m_path, 1, actual, error));
    EXPECT_EQ(actual, trace);
    fs::remove(m_path + ".regions.json");
}

TEST_F(ReplayStreamTest, PairSuccessReplacesBothAndCanRemoveStaleTrace) {
    replay::ReplayWriter writer;
    const replay::RegionScheduleTrace old_trace{{1, "0123456789abcdef"}};
    const replay::RegionScheduleTrace new_trace{{1, "fedcba9876543210"}};
    for (const auto* trace : {&old_trace, &new_trace}) {
        ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
        writer.RecordInput(1, {static_cast<std::uint8_t>(trace == &old_trace ? 7 : 8)});
        ASSERT_TRUE(writer.Finalize(1, trace));
        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
        ASSERT_TRUE(guard.is_open());
        const auto contents = replay::ReadReplay(m_path);
        ASSERT_TRUE(contents.has_value());
        if (!contents.has_value())
            return;
        ASSERT_EQ(contents->inputs.size(), 1u);
        ASSERT_EQ(contents->inputs.front().inputs.size(), 1u);
        EXPECT_EQ(contents->inputs.front().inputs.front(), trace == &old_trace ? 7 : 8);
        replay::RegionScheduleTrace actual;
        std::string error;
        ASSERT_TRUE(replay::ReadRegionScheduleTrace(m_path, 1, actual, error));
        EXPECT_EQ(actual, *trace);
    }
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(0, nullptr));
    EXPECT_FALSE(fs::exists(m_path + ".regions.json"));
    // An already absent trace is also a valid successful pair.
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(0, nullptr));
}

TEST_F(ReplayStreamTest, AllReplayWritersAcceptLongDestinationComponents) {
    m_path = (fs::path(m_path).parent_path() / std::string(220, 'r')).string();
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(0));
    replay::BoundedReplayRecorder bounded;
    ASSERT_TRUE(bounded.Configure(256, 4));
    ASSERT_TRUE(bounded.Open(m_path, MakeHeader())) << bounded.OpenRefusal();
    ASSERT_TRUE(bounded.RecordTick(1, {}, nullptr, 1));
    ASSERT_TRUE(bounded.Finalize(m_path, 1, 1));
    replay::QueuedReplayRecorder queued;
    ASSERT_TRUE(queued.Configure(256, 4));
    ASSERT_TRUE(queued.Open(m_path, MakeHeader())) << queued.OpenRefusal();
    ASSERT_TRUE(
        queued.RecordTick(1, std::make_shared<const std::vector<std::uint8_t>>(), nullptr, 1));
    ASSERT_TRUE(queued.Finalize(m_path, 1, 1));
    const replay::RegionScheduleTrace trace{{1, "0123456789abcdef"}};
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(1, &trace));
    replay::RegionScheduleTrace actual;
    std::string error;
    ASSERT_TRUE(replay::ReadRegionScheduleTrace(m_path, 1, actual, error));
    EXPECT_EQ(actual, trace);
    fs::remove(m_path + ".regions.json");
}

class ReplayPairRecoveryTest : public ReplayStreamTest {
protected:
    using Identity = std::array<std::uint64_t, 3>;
    void SetUp() override {
        ReplayStreamTest::SetUp();
        m_directory = fs::path(m_path + ".pair");
        ASSERT_TRUE(fs::create_directory(m_directory));
        m_path = (m_directory / "recording.lrec1").string();
    }
    void TearDown() override {
        fs::remove_all(m_directory);
    }
    static void Write(const fs::path& path, const std::string& bytes) {
        std::ofstream file(path, std::ios::binary);
        file << bytes;
        ASSERT_TRUE(file.good());
    }
    static Identity Identify(const fs::path& path) {
        const Luminumbra::Persistence::SavedInputFile file(path);
        Identity identity{};
        EXPECT_TRUE(file.identity(identity));
        return identity;
    }
    nlohmann::json
    PrepareInterruption(bool committed, bool old_trace, bool new_trace, bool publish_trace = true) {
        Write(m_path, "old replay");
        const auto previous = Identify(m_path);
        Identity previous_trace{}, incoming_trace{};
        if (old_trace) {
            Write(m_path + ".regions.json", "old trace");
            previous_trace = Identify(m_path + ".regions.json");
        }
        Write(m_directory / "incoming", "new replay");
        const auto incoming = Identify(m_directory / "incoming");
        if (new_trace) {
            Write(m_directory / "incoming_trace", "new trace");
            incoming_trace = Identify(m_directory / "incoming_trace");
        }
        const std::string undo = old_trace ? "old trace" : "";
        nlohmann::json header{
            {"schema", "luminumbra.replay_pair.v1"},
            {"replay", "recording.lrec1"},
            {"previous", previous},
            {"incoming", incoming},
            {"old_trace", previous_trace},
            {"new_trace", incoming_trace},
            {"undo_bytes", undo.size()},
            {"undo_checksum", Luminumbra::Persistence::fnv1a64_bytes(undo.data(), undo.size())}};
        Write(m_directory / ".replay-pair.undo", header.dump() + '\n' + undo);
        if (publish_trace) {
            fs::remove(m_path + ".regions.json");
            if (new_trace)
                fs::rename(m_directory / "incoming_trace", m_path + ".regions.json");
        }
        if (committed) {
            fs::remove(m_path);
            fs::rename(m_directory / "incoming", m_path);
        }
        return header;
    }
    fs::path m_directory;
};

struct PairJournalCapture {
    fs::path replay;
    std::string record;
    bool refuse = false;
};
PairJournalCapture*& CurrentPairJournalCapture() {
    static PairJournalCapture* capture = nullptr;
    return capture;
}
void CapturePairJournal(const char* step, const fs::path& path) {
    auto* capture = CurrentPairJournalCapture();
    if (!capture || std::string_view(step) != "output-publish" || path != capture->replay) {
        return;
    }
    const Luminumbra::Persistence::SavedInputFile journal(path.parent_path() / ".replay-pair.undo");
    ASSERT_TRUE(journal.read(capture->record, 4096));
    if (capture->refuse) {
        throw std::runtime_error("replay publication refused");
    }
}
class ScopedPairJournalCapture {
public:
    explicit ScopedPairJournalCapture(PairJournalCapture& capture) {
        CurrentPairJournalCapture() = &capture;
        Luminumbra::Persistence::set_snapshot_step_hook_for_testing(CapturePairJournal);
    }
    ~ScopedPairJournalCapture() {
        Luminumbra::Persistence::set_snapshot_step_hook_for_testing(nullptr);
        CurrentPairJournalCapture() = nullptr;
    }
};

TEST_F(ReplayPairRecoveryTest, RealWriterJournalRoundTripsAcrossCommitAndRollback) {
    // Sixteen bounded cases cover replay/trace presence on both sides of commit.
    for (const bool committed : {false, true}) {
        for (const bool previous : {false, true}) {
            for (const bool old_trace : {false, true}) {
                for (const bool new_trace : {false, true}) {
                    SCOPED_TRACE(testing::Message()
                                 << committed << previous << old_trace << new_trace);
                    fs::remove_all(m_directory);
                    fs::create_directory(m_directory);
                    if (previous) {
                        Write(m_path, "old replay");
                    }
                    const auto companion = m_path + ".regions.json";
                    if (old_trace) {
                        Write(companion, "old trace");
                    }
                    PairJournalCapture capture{m_path, {}, !committed};
                    {
                        const ScopedPairJournalCapture control(capture);
                        Luminumbra::Persistence::ReplacementOutputFile output(m_path);
                        ASSERT_TRUE(output.write("new replay"));
                        const std::string trace = "new trace";
                        EXPECT_EQ(output.commit_with_companion(new_trace ? &trace : nullptr),
                                  committed);
                    }
                    ASSERT_FALSE(capture.record.empty());
                    const auto header =
                        nlohmann::json::parse(capture.record.substr(0, capture.record.find('\n')));
                    EXPECT_EQ(header.size(), 8u);
                    EXPECT_FALSE(fs::exists(m_directory / ".replay-pair.undo"));
                    // Re-admit the exact bytes emitted by the real writer, including
                    // the already-restored rollback case and committed cleanup.
                    Write(m_directory / ".replay-pair.undo", capture.record);
                    for (int admission = 0; admission < 2; ++admission) {
                        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
                        ASSERT_TRUE(guard.is_open());
                        if (committed || previous) {
                            EXPECT_EQ(ReadBytes(), committed ? "new replay" : "old replay");
                        } else {
                            EXPECT_FALSE(fs::exists(m_path));
                        }
                        const Luminumbra::Persistence::SavedInputFile trace(companion);
                        if (committed ? new_trace : old_trace) {
                            std::string bytes;
                            ASSERT_TRUE(trace.read(bytes, 64));
                            EXPECT_EQ(bytes, committed ? "new trace" : "old trace");
                        } else {
                            EXPECT_TRUE(trace.missing());
                        }
                        EXPECT_FALSE(fs::exists(m_directory / ".replay-pair.undo"));
                    }
                }
            }
        }
    }
}

TEST_F(ReplayPairRecoveryTest, RollbackRestoresARemovedEmptyOldTrace) {
    const auto companion = m_path + ".regions.json";
    Write(m_path, "old replay");
    const auto previous = Identify(m_path);
    Write(companion, "");
    const auto old_trace = Identify(companion);
    Write(m_directory / "incoming", "new replay");
    const auto incoming = Identify(m_directory / "incoming");
    const std::string undo;
    const nlohmann::json header{
        {"schema", "luminumbra.replay_pair.v1"},
        {"replay", "recording.lrec1"},
        {"previous", previous},
        {"incoming", incoming},
        {"old_trace", old_trace},
        {"new_trace", Identity{}},
        {"undo_bytes", undo.size()},
        {"undo_checksum", Luminumbra::Persistence::fnv1a64_bytes(undo.data(), undo.size())}};
    Write(m_directory / ".replay-pair.undo", header.dump() + '\n' + undo);
    // Interrupted after the empty companion was removed, before the replay commit.
    fs::remove(companion);
    {
        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
        ASSERT_TRUE(guard.is_open());
    }
    EXPECT_EQ(ReadBytes(), "old replay");
    const Luminumbra::Persistence::SavedInputFile trace(companion);
    EXPECT_FALSE(trace.missing());
    std::string bytes = "unread";
    ASSERT_TRUE(trace.read(bytes, 64));
    EXPECT_TRUE(bytes.empty());
    EXPECT_FALSE(fs::exists(m_directory / ".replay-pair.undo"));
}

TEST_F(ReplayPairRecoveryTest, StandaloneTraceWriteWaitsForThePairGuard) {
    const auto companion = m_path + ".regions.json";
    const replay::RegionScheduleTrace old{{1, "0123456789abcdef"}};
    const replay::RegionScheduleTrace next{{1, "fedcba9876543210"}};
    std::string error;
    ASSERT_TRUE(replay::WriteRegionScheduleTrace(m_path, old, error)) << error;
    {
        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
        ASSERT_TRUE(guard.is_open());
        EXPECT_FALSE(replay::WriteRegionScheduleTrace(m_path, next, error));
        EXPECT_EQ(error, "Cannot write region schedule trace.");
    }
    replay::RegionScheduleTrace actual;
    ASSERT_TRUE(replay::ReadRegionScheduleTrace(m_path, 1, actual, error)) << error;
    EXPECT_EQ(actual, old);
    ASSERT_TRUE(replay::WriteRegionScheduleTrace(m_path, next, error)) << error;
    ASSERT_TRUE(replay::ReadRegionScheduleTrace(m_path, 1, actual, error)) << error;
    EXPECT_EQ(actual, next);
}

TEST_F(ReplayPairRecoveryTest, PublicationRefusesUnrecoverableFilenamesBeforeChangingThePair) {
    // Case variants alias the journal on case-insensitive volumes on every platform.
    std::vector<std::string> names{".replay-pair.undo",
                                   ".replay-pair.lock",
                                   ".replay-pair.undo.",
                                   ".replay-pair.undo ",
                                   ".REPLAY-PAIR.UNDO",
                                   ".Replay-Pair.Lock"};
#if !defined(_WIN32)
    // These are leaf filenames on POSIX, but separators/streams on Windows.
    names.emplace_back("recording:stream");
    names.emplace_back("recording\\trace");
#endif
    for (const auto& name : names) {
        SCOPED_TRACE(name);
        fs::remove_all(m_directory);
        fs::create_directory(m_directory);
        const auto destination = m_directory / name;
        Write(m_path, "old replay");
        Write(m_path + ".regions.json", "old trace");
        {
            Luminumbra::Persistence::ReplacementOutputFile output(destination);
            ASSERT_TRUE(output.write("new replay"));
            const std::string trace = "new trace";
            EXPECT_FALSE(output.commit_with_companion(&trace));
        }
        EXPECT_EQ(ReadBytes(), "old replay");
        const Luminumbra::Persistence::SavedInputFile trace(m_path + ".regions.json");
        std::string bytes;
        ASSERT_TRUE(trace.read(bytes, 64));
        EXPECT_EQ(bytes, "old trace");
        EXPECT_FALSE(fs::exists(destination));
        EXPECT_FALSE(fs::exists(destination.string() + ".regions.json"));
        EXPECT_FALSE(fs::exists(m_directory / ".replay-pair.undo"));
    }
#if !defined(_WIN32)
    // Existing POSIX pairs must also survive refusal, with or without a new trace.
    for (const char* name : {"recording:stream", "recording\\trace"}) {
        fs::remove_all(m_directory);
        fs::create_directory(m_directory);
        const auto destination = m_directory / name;
        const auto companion = destination.string() + ".regions.json";
        Write(destination, "old replay");
        Write(companion, "old trace");
        for (const bool remove_trace : {false, true}) {
            Luminumbra::Persistence::ReplacementOutputFile output(destination);
            ASSERT_TRUE(output.write("new replay"));
            const std::string replacement = "new trace";
            EXPECT_FALSE(output.commit_with_companion(remove_trace ? nullptr : &replacement));
            const Luminumbra::Persistence::SavedInputFile replay_file(destination);
            const Luminumbra::Persistence::SavedInputFile trace_file(companion);
            std::string bytes;
            ASSERT_TRUE(replay_file.read(bytes, 64));
            EXPECT_EQ(bytes, "old replay");
            ASSERT_TRUE(trace_file.read(bytes, 64));
            EXPECT_EQ(bytes, "old trace");
            EXPECT_FALSE(fs::exists(m_directory / ".replay-pair.undo"));
        }
    }
#endif
}

TEST_F(ReplayPairRecoveryTest, InterruptedReplacementRecoversOnlyMatchingPairs) {
    // All presence transitions on both sides of the replay commit point.
    for (const bool committed : {false, true}) {
        for (const bool old_trace : {false, true}) {
            for (const bool new_trace : {false, true}) {
                fs::remove_all(m_directory);
                fs::create_directory(m_directory);
                PrepareInterruption(committed, old_trace, new_trace);
                const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
                ASSERT_TRUE(guard.is_open());
                EXPECT_EQ(ReadBytes(), committed ? "new replay" : "old replay");
                const bool has_trace = committed ? new_trace : old_trace;
                const Luminumbra::Persistence::SavedInputFile trace(m_path + ".regions.json");
                if (has_trace) {
                    std::string bytes;
                    ASSERT_TRUE(trace.read(bytes, 64));
                    EXPECT_EQ(bytes, committed ? "new trace" : "old trace");
                } else {
                    EXPECT_TRUE(trace.missing());
                }
                EXPECT_FALSE(fs::exists(m_directory / ".replay-pair.undo"));
            }
        }
    }
}

TEST_F(ReplayPairRecoveryTest, JournalBeforeFirstRenameAndRepeatedRecoveryKeepOldPair) {
    PrepareInterruption(false, true, true, false);
    {
        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
        ASSERT_TRUE(guard.is_open());
        EXPECT_EQ(ReadBytes(), "old replay");
    }
    // Recreate a pending journal, then emulate a completed trace restoration
    // whose journal retirement was interrupted. Its identity differs from undo.
    fs::remove_all(m_directory);
    fs::create_directory(m_directory);
    PrepareInterruption(false, true, true);
    Write(m_directory / "restored", "old trace");
    fs::remove(m_path + ".regions.json");
    fs::rename(m_directory / "restored", m_path + ".regions.json");
    {
        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
        ASSERT_TRUE(guard.is_open());
        EXPECT_EQ(ReadBytes(), "old replay");
    }
    const Luminumbra::Persistence::ReplayPairReadGuard repeated(m_path);
    EXPECT_TRUE(repeated.is_open());
    EXPECT_FALSE(fs::exists(m_directory / ".replay-pair.undo"));
}

TEST_F(ReplayPairRecoveryTest, InterruptedRollbackRetirementRestoresMissingTrace) {
    PrepareInterruption(false, true, true);
    fs::remove(m_path + ".regions.json");
    const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
    ASSERT_TRUE(guard.is_open());
    EXPECT_EQ(ReadBytes(), "old replay");
    const Luminumbra::Persistence::SavedInputFile trace(m_path + ".regions.json");
    std::string bytes;
    ASSERT_TRUE(trace.read(bytes, 64));
    EXPECT_EQ(bytes, "old trace");
}

TEST_P(ReplayOutputFailureTest, PairJournalRefusalPreservesBothDestinations) {
    replay::ReplayWriter writer;
    const replay::RegionScheduleTrace original{{1, "0123456789abcdef"}};
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(1, &original));
    const auto old = ReadBytes();
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(1, {9});
    const auto journal = fs::path(m_path).parent_path() / ".replay-pair.undo";
    OutputFailure failure{GetParam(), journal, 0, 0};
    {
        const ScopedOutputFailure control(failure);
        EXPECT_FALSE(writer.Finalize(1, nullptr));
    }
    EXPECT_EQ(failure.hits, 1u);
    EXPECT_EQ(ReadBytes(), old);
    replay::RegionScheduleTrace actual;
    std::string error;
    ASSERT_TRUE(replay::ReadRegionScheduleTrace(m_path, 1, actual, error));
    EXPECT_EQ(actual, original);
    fs::remove(m_path + ".regions.json");
}

TEST_F(ReplayPairRecoveryTest, MalformedOrPartialUndoRefusesWithoutChangingEitherFile) {
    const auto valid = PrepareInterruption(false, true, true);
    const auto refuse = [this](const std::string& record) {
        Write(m_directory / ".replay-pair.undo", record);
        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
        EXPECT_FALSE(guard.is_open());
        EXPECT_EQ(ReadBytes(), "old replay");
        const Luminumbra::Persistence::SavedInputFile trace(m_path + ".regions.json");
        std::string bytes;
        ASSERT_TRUE(trace.read(bytes, 64));
        EXPECT_EQ(bytes, "new trace");
        EXPECT_TRUE(fs::exists(m_directory / ".replay-pair.undo"));
    };
    refuse("");
    refuse("{\"schema\":");
    refuse(valid.dump() + "\nold trac");
    refuse(valid.dump() + "\nold tracE");
    refuse(std::string(4096 - valid.dump().size(), ' ') + valid.dump() + "\nold trace");
    refuse(std::string(4097, ' ') + "\nold trace");
    for (const char* field : {"schema",
                              "replay",
                              "previous",
                              "incoming",
                              "old_trace",
                              "new_trace",
                              "undo_bytes",
                              "undo_checksum"}) {
        auto invalid = valid;
        invalid[field] = nullptr;
        refuse(invalid.dump() + "\nold trace");
        invalid = valid;
        invalid.erase(field);
        refuse(invalid.dump() + "\nold trace");
    }
    for (const char* name : {"../escape",
                             "nested/recording",
                             "C:recording",
                             "recording\\trace",
                             ".",
                             "..",
                             ".replay-pair.lock",
                             ".replay-pair.undo",
                             ".replay-pair.undo.",
                             ".replay-pair.undo ",
                             ""}) {
        auto invalid = valid;
        invalid["replay"] = name;
        refuse(invalid.dump() + "\nold trace");
    }
    auto invalid = valid;
    invalid["previous"] = {-1, 0, 0};
    refuse(invalid.dump() + "\nold trace");
    invalid = valid;
    invalid["new_trace"] = {0, 0};
    refuse(invalid.dump() + "\nold trace");
    invalid = valid;
    invalid["extra"] = 1;
    refuse(invalid.dump() + "\nold trace");
    Write(m_directory / ".replay-pair.undo", valid.dump() + "\nold trace");
    fs::resize_file(m_directory / ".replay-pair.undo", 96u * 1024u * 1024u + 4097u);
    {
        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
        EXPECT_FALSE(guard.is_open());
    }
    // A header at the writer's inclusive 4096-byte bound remains recoverable.
    Write(m_directory / ".replay-pair.undo",
          std::string(4095 - valid.dump().size(), ' ') + valid.dump() + "\nold trace");
    const Luminumbra::Persistence::ReplayPairReadGuard recovered(m_path);
    EXPECT_TRUE(recovered.is_open());
}

TEST_F(ReplayPairRecoveryTest, UnknownOccupantsAndNonregularJournalRefuseRecovery) {
    PrepareInterruption(false, true, true, false);
    const auto companion = m_path + ".regions.json";
    fs::rename(companion, m_directory / "old_trace_inode");
    fs::rename(m_directory / "incoming_trace", companion);
    fs::rename(m_path, m_directory / "retained_replay");
    Write(m_path, "foreign replay");
    {
        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
        EXPECT_FALSE(guard.is_open());
        EXPECT_TRUE(fs::exists(m_directory / ".replay-pair.undo"));
        EXPECT_EQ(ReadBytes(), "foreign replay");
    }
    fs::remove(m_path);
    fs::rename(m_directory / "retained_replay", m_path);
    fs::rename(m_path + ".regions.json", m_directory / "retained_trace");
    Write(m_path + ".regions.json", "foreign trace");
    {
        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
        EXPECT_FALSE(guard.is_open());
        const Luminumbra::Persistence::SavedInputFile trace(m_path + ".regions.json");
        std::string bytes;
        ASSERT_TRUE(trace.read(bytes, 64));
        EXPECT_EQ(bytes, "foreign trace");
        EXPECT_TRUE(fs::exists(m_directory / ".replay-pair.undo"));
    }
    // Also force reuse of the recorded old identity, independent of allocation.
    fs::remove(companion);
    fs::rename(m_directory / "old_trace_inode", companion);
    Write(companion, "foreign trace");
    {
        const Luminumbra::Persistence::ReplayPairReadGuard refused(m_path);
        EXPECT_FALSE(refused.is_open());
        const Luminumbra::Persistence::SavedInputFile trace(companion);
        std::string bytes;
        ASSERT_TRUE(trace.read(bytes, 64));
        EXPECT_EQ(bytes, "foreign trace");
        EXPECT_TRUE(fs::exists(m_directory / ".replay-pair.undo"));
    }
    fs::remove(m_path + ".regions.json");
    fs::rename(m_directory / "retained_trace", m_path + ".regions.json");
    fs::rename(m_directory / ".replay-pair.undo", m_directory / "retained_undo");
    fs::create_directory(m_directory / ".replay-pair.undo");
    const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
    EXPECT_FALSE(guard.is_open());
    EXPECT_EQ(ReadBytes(), "old replay");
}

TEST_F(ReplayPairRecoveryTest, OldTraceIdentityWithForeignBytesRefusesRecovery) {
    const auto header = PrepareInterruption(false, true, true, false);
    const auto companion = m_path + ".regions.json";
    // An in-place edit deterministically exercises the same identity collision
    // as reuse of an unlinked old trace's inode.
    Write(companion, "foreign trace");
    EXPECT_EQ(Identify(companion), header["old_trace"].get<Identity>());
    const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
    EXPECT_FALSE(guard.is_open());
    EXPECT_EQ(ReadBytes(), "old replay");
    const Luminumbra::Persistence::SavedInputFile trace(companion);
    std::string bytes;
    ASSERT_TRUE(trace.read(bytes, 64));
    EXPECT_EQ(bytes, "foreign trace");
    EXPECT_TRUE(fs::exists(m_directory / ".replay-pair.undo"));
}

TEST_F(ReplayPairRecoveryTest, DifferentDirectoriesAdmitReadersAndWritersDuringContention) {
    const auto other = m_directory / "other";
    ASSERT_TRUE(fs::create_directory(other));
    const auto other_replay = other / "recording.lrec1";
    bool reader_open = false;
    bool contended_open = true;
    bool writer_committed = false;
    {
        const Luminumbra::Persistence::ReplayPairReadGuard first(m_path);
        ASSERT_TRUE(first.is_open());
        const Luminumbra::Persistence::ReplayPairReadGuard alias(m_directory / "." /
                                                                 "recording.lrec1");
        EXPECT_FALSE(alias.is_open());
        {
            const Luminumbra::Persistence::ReplayPairReadGuard second(other_replay);
            EXPECT_TRUE(second.is_open());
        }
        std::thread worker([&] {
            {
                const Luminumbra::Persistence::ReplayPairReadGuard reader(other_replay);
                reader_open = reader.is_open();
            }
            {
                const Luminumbra::Persistence::ReplayPairReadGuard contended(m_path);
                contended_open = contended.is_open();
            }
            replay::ReplayWriter writer;
            const replay::RegionScheduleTrace trace{{1, "0123456789abcdef"}};
            writer_committed =
                writer.Open(other_replay.string(), MakeHeader()) && writer.Finalize(1, &trace) &&
                writer.Open(other_replay.string(), MakeHeader()) && writer.Finalize(1, nullptr);
        });
        worker.join();
        EXPECT_TRUE(reader_open);
        EXPECT_FALSE(contended_open);
        EXPECT_TRUE(writer_committed);
        EXPECT_FALSE(fs::exists(other_replay.string() + ".regions.json"));
    }
    const Luminumbra::Persistence::ReplayPairReadGuard released(m_path);
    EXPECT_TRUE(released.is_open());
    EXPECT_FALSE(fs::exists(m_directory / ".replay-pair.undo"));
}

#if !defined(_WIN32)
TEST_F(ReplayPairRecoveryTest, RemovingHardLinkedPreviousTracePreservesBackup) {
    replay::ReplayWriter writer;
    const replay::RegionScheduleTrace original{{1, "0123456789abcdef"}};
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(1, &original));
    const auto companion = m_path + ".regions.json";
    const auto backup = m_directory / "trace-backup";
    std::string previous;
    {
        const Luminumbra::Persistence::SavedInputFile trace(companion);
        ASSERT_TRUE(trace.read(previous, 4096));
    }
    std::error_code error;
    fs::create_hard_link(companion, backup, error);
    if (error) {
        GTEST_SKIP() << "Hard links unavailable: " << error.message();
    }
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(1, {9});
    ASSERT_TRUE(writer.Finalize(1, nullptr));
    EXPECT_FALSE(fs::exists(companion));
    EXPECT_EQ(fs::hard_link_count(backup), 1u);
    const Luminumbra::Persistence::SavedInputFile retained(backup);
    std::string bytes;
    ASSERT_TRUE(retained.read(bytes, 4096));
    EXPECT_EQ(bytes, previous);
    EXPECT_FALSE(fs::exists(m_directory / ".replay-pair.undo"));
    const auto contents = replay::ReadReplay(m_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    EXPECT_TRUE(contents->trailer_present);
    EXPECT_EQ(contents->inputs.size(), 1u);
}

TEST_F(ReplayPairRecoveryTest, RollbackUnlinksOnlyHardLinkedIncomingTrace) {
    for (const bool old_trace : {false, true}) {
        SCOPED_TRACE(old_trace);
        fs::remove_all(m_directory);
        fs::create_directory(m_directory);
        PrepareInterruption(false, old_trace, true);
        const auto companion = m_path + ".regions.json";
        const auto backup = m_directory / "trace-backup";
        std::error_code error;
        fs::create_hard_link(companion, backup, error);
        if (error) {
            GTEST_SKIP() << "Hard links unavailable: " << error.message();
        }
        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
        ASSERT_TRUE(guard.is_open());
        EXPECT_EQ(ReadBytes(), "old replay");
        const Luminumbra::Persistence::SavedInputFile trace(companion);
        std::string bytes;
        if (old_trace) {
            ASSERT_TRUE(trace.read(bytes, 64));
            EXPECT_EQ(bytes, "old trace");
        } else {
            EXPECT_TRUE(trace.missing());
        }
        EXPECT_EQ(fs::hard_link_count(backup), 1u);
        const Luminumbra::Persistence::SavedInputFile retained(backup);
        ASSERT_TRUE(retained.read(bytes, 64));
        EXPECT_EQ(bytes, "new trace");
        EXPECT_FALSE(fs::exists(m_directory / ".replay-pair.undo"));
    }
}
#endif

TEST_F(ReplayPairRecoveryTest, PairLockContentionAndInvalidDirectoryRefuseAdmission) {
    {
        const Luminumbra::Persistence::ReplayPairReadGuard first(m_path);
        ASSERT_TRUE(first.is_open());
        const Luminumbra::Persistence::ReplayPairReadGuard second(m_path);
        EXPECT_FALSE(second.is_open());
    }
    Write(m_directory / "blocked", "not a directory");
    const Luminumbra::Persistence::ReplayPairReadGuard guard(m_directory / "blocked/recording");
    EXPECT_FALSE(guard.is_open());
}

TEST_F(ReplayPairRecoveryTest, ReadAdmissionCreatesNoFiles) {
    ASSERT_TRUE(fs::is_empty(m_directory));
    {
        const Luminumbra::Persistence::ReplayPairReadGuard guard(m_path);
        EXPECT_TRUE(guard.is_open());
        EXPECT_TRUE(fs::is_empty(m_directory));
    }
    EXPECT_TRUE(fs::is_empty(m_directory));
}

TEST_F(ReplayStreamTest, InvalidPairTraceRefusesBeforeEitherDestinationChanges) {
    replay::ReplayWriter writer;
    const replay::RegionScheduleTrace original{{1, "0123456789abcdef"}};
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(1, &original));
    const auto old = ReadBytes();
    const std::array<replay::RegionScheduleTrace, 3> invalid{
        {{}, {{2, "0123456789abcdef"}}, {{1, "invalid"}}}};
    for (const auto& trace : invalid) {
        ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
        EXPECT_FALSE(writer.Finalize(1, &trace));
        EXPECT_FALSE(writer.Finalize(1, &original));
        EXPECT_EQ(ReadBytes(), old);
        replay::RegionScheduleTrace actual;
        std::string error;
        ASSERT_TRUE(replay::ReadRegionScheduleTrace(m_path, 1, actual, error));
        EXPECT_EQ(actual, original);
    }
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    const replay::RegionScheduleTrace empty;
    ASSERT_TRUE(writer.Finalize(0, &empty));
    fs::remove(m_path + ".regions.json");
}

INSTANTIATE_TEST_SUITE_P(OutputBoundaries,
                         ReplayOutputFailureTest,
                         testing::Values("output-open",
                                         "output-write",
                                         "output-flush",
                                         "output-close",
                                         "output-publish"));

class ReplaySegmentFailureTest : public ReplayStreamTest,
                                 public testing::WithParamInterface<const char*> {};
TEST_P(ReplaySegmentFailureTest, RefusalPreservesOldReplayAndReleasesSegments) {
    {
        std::ofstream old(m_path);
        old << "previous replay";
        ASSERT_TRUE(old.good());
    }
    const auto old = ReadBytes();
    replay::BoundedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(256, 4));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    ASSERT_TRUE(recorder.RecordTick(1, {1, 2, 3}, nullptr, 1));
    OutputFailure failure{GetParam(), {}, 0, 0};
    {
        const ScopedOutputFailure control(failure);
        EXPECT_FALSE(recorder.Finalize(m_path, 1, 1));
    }
    EXPECT_EQ(failure.hits, 1u);
    EXPECT_EQ(ReadBytes(), old);
    EXPECT_EQ(recorder.SegmentCount(), 0u);
    EXPECT_FALSE(recorder.RecordTick(2, {}, nullptr, 2));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    ASSERT_TRUE(recorder.RecordTick(1, {}, nullptr, 1));
    ASSERT_TRUE(recorder.Finalize(m_path, 1, 1));
}
INSTANTIATE_TEST_SUITE_P(InputBoundaries,
                         ReplaySegmentFailureTest,
                         testing::Values("replay-segment-close", "replay-segment", "replay-read"));

TEST_F(ReplayStreamTest, BufferedOpenFailureIsTerminalAndRepairRequiresNewRecording) {
    const auto parent = fs::path(m_path + ".parent");
    {
        std::ofstream blocker(parent);
        blocker << "blocker";
        ASSERT_TRUE(blocker.good());
    }
    const auto destination = (parent / "recording").string();
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(destination, MakeHeader()));
    writer.RecordInput(1, {7});
    EXPECT_FALSE(writer.Finalize(1));
    fs::remove(parent);
    fs::create_directory(parent);
    EXPECT_FALSE(writer.Finalize(1));
    EXPECT_FALSE(fs::exists(destination));
    ASSERT_TRUE(writer.Open(destination, MakeHeader()));
    writer.RecordInput(2, {8});
    ASSERT_TRUE(writer.Finalize(2));
    const auto contents = replay::ReadReplay(destination);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    EXPECT_FALSE(contents->truncated);
    ASSERT_EQ(contents->inputs.size(), 1u);
    EXPECT_EQ(contents->inputs.front().tick, 2u);
    fs::remove_all(parent);
}

TEST_F(ReplayStreamTest, BoundedOpenFailureIsTerminalAndRepairAllowsReopen) {
    replay::BoundedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(256, 4));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    ASSERT_TRUE(recorder.RecordTick(1, {7}, nullptr, 1));
    const auto parent = fs::path(m_path + ".parent");
    {
        std::ofstream blocker(parent);
        blocker << "blocker";
        ASSERT_TRUE(blocker.good());
    }
    const auto destination = (parent / "recording").string();
    EXPECT_FALSE(recorder.Finalize(destination, 1, 1));
    EXPECT_EQ(recorder.SegmentCount(), 0u);
    fs::remove(parent);
    fs::create_directory(parent);
    EXPECT_FALSE(recorder.Finalize(destination, 1, 1));
    ASSERT_TRUE(recorder.Open(destination, MakeHeader())) << recorder.OpenRefusal();
    ASSERT_TRUE(recorder.RecordTick(1, {8}, nullptr, 1));
    ASSERT_TRUE(recorder.Finalize(destination, 1, 1));
    fs::remove_all(parent);
}

TEST_F(ReplayStreamTest, QueuedMarkerCleanupRefusalPreservesOldReplay) {
    {
        std::ofstream old(m_path);
        old << "previous replay";
        ASSERT_TRUE(old.good());
    }
    const auto old = ReadBytes();
    const auto marker = fs::path(m_path + ".incomplete");
    fs::create_directories(marker);
    {
        std::ofstream occupied(marker / "retained");
        occupied << "marker";
    }
    replay::QueuedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(256, 4));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    const auto inputs = std::make_shared<const std::vector<std::uint8_t>>(1, 7);
    ASSERT_TRUE(recorder.RecordTick(1, inputs, nullptr, 1));
    EXPECT_FALSE(recorder.Finalize(m_path, 1, 1));
    EXPECT_EQ(ReadBytes(), old);
    EXPECT_FALSE(recorder.Finalize(m_path, 1, 1));
    EXPECT_EQ(recorder.SegmentCount(), 0u);
    EXPECT_TRUE(fs::exists(marker / "retained"));
    fs::remove_all(marker);
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    ASSERT_TRUE(recorder.RecordTick(1, inputs, nullptr, 1));
    ASSERT_TRUE(recorder.Finalize(m_path, 1, 1));
    const auto contents = replay::ReadReplay(m_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    EXPECT_FALSE(contents->truncated);
}

class ReplayMalformedSegmentTest : public ReplayStreamTest,
                                   public testing::WithParamInterface<int> {};
TEST_P(ReplayMalformedSegmentTest, RefusedSegmentNeverTruncatesExistingReplay) {
    {
        std::ofstream old(m_path);
        old << "previous replay";
        ASSERT_TRUE(old.good());
    }
    const auto old = ReadBytes();
    replay::BoundedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(256, 2048));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    // Rotation closes the first segment before mutation, including on Windows.
    for (std::uint64_t tick = 1; tick <= 1025; ++tick) {
        ASSERT_TRUE(recorder.RecordTick(tick, {7}, nullptr, 0));
    }
    fs::path segment;
    for (const auto& entry : fs::directory_iterator(fs::path(m_path).parent_path())) {
        if (entry.path().filename().string().starts_with("lockstep-recording-")) {
            segment = entry.path() / "1";
            break;
        }
    }
    ASSERT_FALSE(segment.empty());
    ASSERT_TRUE(fs::exists(segment));
    switch (GetParam()) {
        case 0:
            fs::remove(segment);
            break;
        case 1:
            fs::resize_file(segment, 12);
            break;
        case 2: {
            std::fstream data(segment, std::ios::binary | std::ios::in | std::ios::out);
            data.put(static_cast<char>(0xff)); // unknown record type
            ASSERT_TRUE(data.good());
            break;
        }
        case 3: {
            std::fstream data(segment, std::ios::binary | std::ios::in | std::ios::out);
            data.seekp(9);
            const std::array<char, 4> oversized{char(-1), char(-1), char(-1), char(-1)};
            data.write(oversized.data(), static_cast<std::streamsize>(oversized.size()));
            ASSERT_TRUE(data.good());
            break;
        }
        case 4:
            fs::resize_file(segment, fs::file_size(segment) - 1);
            break;
        default:
            FAIL() << "unknown segment mutation";
    }
    EXPECT_FALSE(recorder.Finalize(m_path, 1025, 1025));
    EXPECT_EQ(ReadBytes(), old);
    EXPECT_EQ(recorder.SegmentCount(), 0u);
    EXPECT_EQ(recorder.RetainedBytes(), 0u);
    EXPECT_FALSE(recorder.RecordTick(1026, {}, nullptr, 0));
    EXPECT_FALSE(recorder.Finalize(m_path, 1025, 1025));
}
INSTANTIATE_TEST_SUITE_P(MalformedSegments,
                         ReplayMalformedSegmentTest,
                         testing::Values(0, 1, 2, 3, 4));

class ReplayReplacementTest : public ReplayStreamTest, public testing::WithParamInterface<int> {};
TEST_P(ReplayReplacementTest, SuccessReplacesTheSamePathWithoutWritingThroughOldLinks) {
    {
        std::ofstream old(m_path);
        old << "previous artifact";
        ASSERT_TRUE(old.good());
    }
    const auto old = ReadBytes();
    const auto retained = m_path + ".retained";
    fs::create_hard_link(m_path, retained);
    const std::vector<std::uint8_t> inputs(std::size_t{128} * 1024 + 17, 7);
    switch (GetParam()) {
        case 0: {
            replay::ReplayWriter writer;
            ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
            writer.RecordInput(1, inputs);
            ASSERT_TRUE(writer.Finalize(1));
            EXPECT_FALSE(writer.Finalize(1));
            break;
        }
        case 1: {
            replay::BoundedReplayRecorder recorder;
            ASSERT_TRUE(recorder.Configure(inputs.size() + 17, 4));
            ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
            ASSERT_TRUE(recorder.RecordTick(1, inputs, nullptr, 1));
            ASSERT_TRUE(recorder.Finalize(m_path, 1, 1));
            EXPECT_FALSE(recorder.RecordTick(2, {}, nullptr, 2));
            break;
        }
        case 2: {
            replay::QueuedReplayRecorder recorder;
            ASSERT_TRUE(recorder.Configure(inputs.size() + 17, 4));
            ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
            ASSERT_TRUE(recorder.RecordTick(
                1, std::make_shared<const std::vector<std::uint8_t>>(inputs), nullptr, 1));
            ASSERT_TRUE(recorder.Finalize(m_path, 1, 1));
            EXPECT_FALSE(recorder.Finalize(m_path, 1, 1));
            break;
        }
        default:
            FAIL() << "unknown recorder";
    }
    EXPECT_NE(ReadBytes(), old);
    {
        std::ifstream input(retained, std::ios::binary);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), old);
    }
    EXPECT_FALSE(fs::equivalent(m_path, retained));
    const auto contents = replay::ReadReplay(m_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    EXPECT_FALSE(contents->truncated);
    EXPECT_EQ(contents->tick_count, 1u);
    ASSERT_EQ(contents->inputs.size(), 1u);
    EXPECT_EQ(contents->inputs.front().inputs, inputs);
    fs::remove(retained);
}
INSTANTIATE_TEST_SUITE_P(RecorderKinds, ReplayReplacementTest, testing::Values(0, 1, 2));

TEST_F(ReplayStreamTest, TraceSuccessReplacesSamePathWithoutWritingThroughOldLink) {
    std::string error;
    const replay::RegionScheduleTrace old{{1, "0123456789abcdef"}};
    const replay::RegionScheduleTrace next{{1, "fedcba9876543210"}};
    ASSERT_TRUE(replay::WriteRegionScheduleTrace(m_path, old, error));
    const auto companion = m_path + ".regions.json";
    const auto retained = m_path + ".retained.regions.json";
    fs::create_hard_link(companion, retained);
    ASSERT_TRUE(replay::WriteRegionScheduleTrace(m_path, next, error));
    replay::RegionScheduleTrace actual;
    ASSERT_TRUE(replay::ReadRegionScheduleTrace(m_path, 1, actual, error));
    EXPECT_EQ(actual, next);
    ASSERT_TRUE(replay::ReadRegionScheduleTrace(m_path + ".retained", 1, actual, error));
    EXPECT_EQ(actual, old);
    EXPECT_FALSE(fs::equivalent(companion, retained));
    fs::remove(retained);
    fs::remove(companion);
}

TEST_F(ReplayStreamTest, BufferedRenameFailureIsTerminalAndRepairAllowsReopen) {
    fs::create_directory(m_path);
    const auto occupant = fs::path(m_path) / "retained";
    {
        std::ofstream old(occupant);
        old << "previous artifact";
        ASSERT_TRUE(old.good());
    }
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(1, {7});
    EXPECT_FALSE(writer.Finalize(1));
    EXPECT_TRUE(fs::is_directory(m_path));
    EXPECT_TRUE(fs::exists(occupant));
    fs::remove_all(m_path);
    EXPECT_FALSE(writer.Finalize(1));
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(0));
}

class ReplayLateWriteFailureTest : public ReplayStreamTest,
                                   public testing::WithParamInterface<std::size_t> {};
TEST_P(ReplayLateWriteFailureTest, FramePayloadAndTrailerRefusalsPreserveOldReplay) {
    {
        std::ofstream old(m_path);
        old << "previous replay";
        ASSERT_TRUE(old.good());
    }
    const auto old = ReadBytes();
    const std::vector<std::uint8_t> inputs(std::size_t{128} * 1024 + 17, 7);
    replay::BoundedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(inputs.size() + 17, 4));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    ASSERT_TRUE(recorder.RecordTick(1, inputs, nullptr, 1));
    OutputFailure failure{"output-write", m_path, GetParam(), 0};
    const ScopedOutputFailure control(failure);
    EXPECT_FALSE(recorder.Finalize(m_path, 1, 1));
    EXPECT_EQ(failure.hits, GetParam() + 1);
    EXPECT_EQ(ReadBytes(), old);
    EXPECT_EQ(recorder.SegmentCount(), 0u);
}
INSTANTIATE_TEST_SUITE_P(LateWrites,
                         ReplayLateWriteFailureTest,
                         testing::Values(std::size_t{1}, std::size_t{2}, std::size_t{5}));

TEST_F(ReplayStreamTest, InvalidDivergenceRefusalClosesBoundedRecorder) {
    {
        std::ofstream old(m_path);
        old << "previous replay";
        ASSERT_TRUE(old.good());
    }
    const auto old = ReadBytes();
    replay::BoundedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(256, 4));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    ASSERT_TRUE(recorder.RecordTick(1, {7}, nullptr, 1));
    EXPECT_FALSE(recorder.Finalize(m_path, 1, 0));
    EXPECT_EQ(ReadBytes(), old);
    EXPECT_EQ(recorder.SegmentCount(), 0u);
    EXPECT_FALSE(recorder.RecordTick(2, {}, nullptr, 2));
    EXPECT_FALSE(recorder.Finalize(m_path, 1, 1));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    ASSERT_TRUE(recorder.RecordTick(1, {}, nullptr, 1));
    ASSERT_TRUE(recorder.Finalize(m_path, 1, 1));
}

TEST_F(ReplayStreamTest, QueuedOpenFailureClosesAndRepairAllowsReopen) {
    replay::QueuedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(256, 4));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    const auto inputs = std::make_shared<const std::vector<std::uint8_t>>(1, 7);
    ASSERT_TRUE(recorder.RecordTick(1, inputs, nullptr, 1));
    const auto parent = fs::path(m_path + ".parent");
    {
        std::ofstream blocker(parent);
        blocker << "blocker";
        ASSERT_TRUE(blocker.good());
    }
    const auto destination = (parent / "recording").string();
    EXPECT_FALSE(recorder.Finalize(destination, 1, 1));
    EXPECT_EQ(recorder.PendingTicks(), 0u);
    EXPECT_EQ(recorder.SegmentCount(), 0u);
    fs::remove(parent);
    fs::create_directory(parent);
    EXPECT_FALSE(recorder.Finalize(destination, 1, 1));
    ASSERT_TRUE(recorder.Open(destination, MakeHeader())) << recorder.OpenRefusal();
    ASSERT_TRUE(recorder.RecordTick(1, inputs, nullptr, 1));
    ASSERT_TRUE(recorder.Finalize(destination, 1, 1));
    fs::remove_all(parent);
}

struct SubstitutedOutput {
    bool occupy = false;
    fs::path destination;
    fs::path staging;
    fs::path retained;
};
SubstitutedOutput*& OutputSubstitution() {
    static SubstitutedOutput* substitution = nullptr;
    return substitution;
}
void SubstituteOutput(const char* step, const fs::path& destination) {
    auto* substitution = OutputSubstitution();
    if (!substitution)
        return;
    if (std::string_view(step) == "output-create") {
        substitution->staging = destination;
        if (substitution->occupy) {
            std::ofstream foreign(destination, std::ios::binary);
            foreign << "foreign occupant";
        }
        return;
    }
    if (substitution->occupy || std::string_view(step) != "output-close" ||
        destination != substitution->destination)
        return;
    substitution->retained = substitution->staging.string() + ".retained";
    fs::rename(substitution->staging, substitution->retained);
    std::ofstream foreign(substitution->staging, std::ios::binary);
    foreign << "foreign occupant";
}
class ScopedOutputSubstitution {
public:
    explicit ScopedOutputSubstitution(SubstitutedOutput& substitution) {
        OutputSubstitution() = &substitution;
        Luminumbra::Persistence::set_snapshot_step_hook_for_testing(SubstituteOutput);
    }
    ~ScopedOutputSubstitution() {
        Luminumbra::Persistence::set_snapshot_step_hook_for_testing(nullptr);
        OutputSubstitution() = nullptr;
    }
};

TEST_F(ReplayStreamTest, ChangedStagingOwnershipRefusesAndPreservesForeignOccupant) {
    {
        std::ofstream old(m_path);
        old << "previous replay";
        ASSERT_TRUE(old.good());
    }
    const auto old = ReadBytes();
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(1, {7});
    SubstitutedOutput substitution{false, m_path, {}, {}};
    {
        const ScopedOutputSubstitution control(substitution);
        EXPECT_FALSE(writer.Finalize(1));
    }
    ASSERT_FALSE(substitution.staging.empty());
    EXPECT_EQ(ReadBytes(), old);
    {
        std::ifstream foreign(substitution.staging, std::ios::binary);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(foreign), {}), "foreign occupant");
    }
    EXPECT_TRUE(fs::exists(substitution.retained));
    EXPECT_FALSE(writer.Finalize(1));
    fs::remove(substitution.staging);
    fs::remove(substitution.retained);
}

TEST_F(ReplayStreamTest, OccupiedOutputSiblingIsNeverOpenedOrRemoved) {
    {
        std::ofstream old(m_path);
        old << "previous replay";
        ASSERT_TRUE(old.good());
    }
    const auto old = ReadBytes();
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(1, {7});
    SubstitutedOutput substitution{true, m_path, {}, {}};
    {
        const ScopedOutputSubstitution control(substitution);
        EXPECT_FALSE(writer.Finalize(1));
    }
    ASSERT_FALSE(substitution.staging.empty());
    EXPECT_EQ(ReadBytes(), old);
    {
        std::ifstream foreign(substitution.staging, std::ios::binary);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(foreign), {}), "foreign occupant");
    }
    EXPECT_FALSE(writer.Finalize(1));
    fs::remove(substitution.staging);
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(0));
}

TEST_F(ReplayStreamTest, RelativeOutputRefusalCleansOnlyItsOwnedSibling) {
    struct RestoreDirectory {
        fs::path previous = fs::current_path();
        ~RestoreDirectory() {
            std::error_code error;
            fs::current_path(previous, error);
        }
    };
    const RestoreDirectory restore;
    // Keep the relative leaf on the temporary drive, including Windows runners.
    fs::current_path(fs::path(m_path).parent_path());
    {
        std::ofstream old(m_path);
        old << "previous replay";
        ASSERT_TRUE(old.good());
    }
    const auto old = ReadBytes();
    const auto relative = fs::path(m_path).filename().string();
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(relative, MakeHeader()));
    writer.RecordInput(1, {7});
    OutputFailure failure{"output-close", {}, 0, 0};
    {
        const ScopedOutputFailure control(failure);
        EXPECT_FALSE(writer.Finalize(1));
    }
    EXPECT_EQ(failure.hits, 1u);
    EXPECT_EQ(ReadBytes(), old);
    const std::string prefix = ".output.tmp.";
    for (const auto& entry : fs::directory_iterator(fs::path(m_path).parent_path())) {
        EXPECT_FALSE(entry.path().filename().string().starts_with(prefix));
    }
    ASSERT_TRUE(writer.Open(relative, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(0));
    EXPECT_NE(ReadBytes(), old);
}

// A seekable sparse provider: only the prefix is stored. The remainder reads as
// zeros. Faults occur on this same provider, without filesystem timing or races.
class ReplayProvider final : public std::streambuf {
public:
    explicit ReplayProvider(const std::string& prefix, std::uint64_t size)
        : prefix_(prefix)
        , size_(size) {}

    enum class SeekFailure {
        None,
        End,
        Tell,
        Rewind,
        Skip
    };
    SeekFailure seek_failure = SeekFailure::None;
    std::uint64_t read_limit = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t bytes_read = 0;
    std::uint64_t allocation_after = std::numeric_limits<std::uint64_t>::max();
    std::string suffix;

protected:
    pos_type seekoff(off_type offset, std::ios::seekdir direction, std::ios::openmode) override {
        if ((direction == std::ios::end && seek_failure == SeekFailure::End) ||
            (direction == std::ios::cur && seek_failure == SeekFailure::Tell) ||
            (direction == std::ios::beg && offset == 0 && seek_failure == SeekFailure::Rewind) ||
            (direction == std::ios::beg && offset != 0 && seek_failure == SeekFailure::Skip)) {
            return pos_type(off_type{-1});
        }
        if (direction == std::ios::end && offset == 0) {
            position_ = size_;
        } else if (direction == std::ios::beg && offset >= 0 &&
                   static_cast<std::uint64_t>(offset) <= size_) {
            position_ = static_cast<std::uint64_t>(offset);
        } else if (direction != std::ios::cur || offset != 0) {
            return pos_type(off_type{-1});
        }
        return pos_type(static_cast<off_type>(position_));
    }
    pos_type seekpos(pos_type position, std::ios::openmode mode) override {
        return seekoff(static_cast<off_type>(position), std::ios::beg, mode);
    }
    std::streamsize xsgetn(char* destination, std::streamsize requested) override {
        if (bytes_read >= allocation_after) {
            throw std::bad_alloc();
        }
        const auto count = std::min(
            {static_cast<std::uint64_t>(requested), size_ - position_, read_limit - bytes_read});
        for (std::uint64_t index = 0; index < count; ++index) {
            destination[index] =
                position_ < prefix_.size() ? prefix_[static_cast<std::size_t>(position_)] : '\0';
            if (suffix.size() <= size_ && position_ >= size_ - suffix.size()) {
                destination[index] =
                    suffix[static_cast<std::size_t>(position_ - (size_ - suffix.size()))];
            }
            ++position_;
        }
        bytes_read += count;
        return static_cast<std::streamsize>(count);
    }

private:
    const std::string& prefix_;
    std::uint64_t size_;
    std::uint64_t position_ = 0;
};

void PatchU32(std::string& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes.at(offset + index) = static_cast<char>((value >> (8 * index)) & 0xFF);
    }
}

struct RecorderGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    const std::thread::id simulation_thread = std::this_thread::get_id();
    std::atomic<std::size_t> simulation_sink_calls{0};
    std::atomic<std::size_t> written_ticks{0};
    std::atomic<std::size_t> digest_bytes{0};
    std::atomic<std::size_t> closes{0};
    std::uint64_t fail_tick = 0;      // configured before Open, never changed by the producer
    std::vector<std::uint64_t> ticks; // inspected only after Flush/Close joins
    const std::uint8_t* first_input = nullptr;
};

void WaitForRecorderWorker(const std::shared_ptr<RecorderGate>& gate) {
    std::unique_lock lock(gate->mutex);
    gate->condition.wait(lock, [&gate] { return gate->entered; });
}

void ReleaseRecorder(const std::shared_ptr<RecorderGate>& gate) {
    const std::lock_guard lock(gate->mutex);
    gate->released = true;
    gate->condition.notify_all();
}

// Declared after the recorder so any fatal assertion releases the worker before
// recorder destruction joins it. No sleeps or wall-clock deadlines are involved.
class ReleaseRecorderGate {
public:
    explicit ReleaseRecorderGate(std::shared_ptr<RecorderGate> gate)
        : m_gate(std::move(gate)) {}
    ~ReleaseRecorderGate() {
        ReleaseRecorder(m_gate);
    }

private:
    std::shared_ptr<RecorderGate> m_gate;
};

class GatedReplaySink final : public replay::ReplayTickSink {
public:
    explicit GatedReplaySink(std::shared_ptr<RecorderGate> gate)
        : m_gate(std::move(gate)) {}

    bool Configure(std::size_t bytes, std::uint64_t ticks) override {
        return m_disk.Configure(bytes, ticks);
    }
    bool Open(const std::string& destination, const replay::ReplayHeader& header) override {
        return m_disk.Open(destination, header);
    }
    bool RecordTick(std::uint64_t tick,
                    const std::vector<std::uint8_t>& inputs,
                    const std::array<std::string_view, 4>* hashes,
                    std::uint64_t confirmed_tick) override {
        if (std::this_thread::get_id() == m_gate->simulation_thread)
            ++m_gate->simulation_sink_calls;
        {
            std::unique_lock lock(m_gate->mutex);
            m_gate->entered = true;
            m_gate->condition.notify_all();
            m_gate->condition.wait(lock, [this] { return m_gate->released; });
        }
        if (m_gate->ticks.empty())
            m_gate->first_input = inputs.data();
        m_gate->ticks.push_back(tick);
        const bool ok =
            tick != m_gate->fail_tick && m_disk.RecordTick(tick, inputs, hashes, confirmed_tick);
        ++m_gate->written_ticks;
        m_gate->digest_bytes += inputs.size();
        return ok;
    }
    bool Finalize(const std::string& destination,
                  std::uint64_t ticks,
                  std::uint64_t divergence) override {
        return m_disk.Finalize(destination, ticks, divergence);
    }
    void Close() override {
        m_disk.Close();
        ++m_gate->closes;
    }
    std::size_t RetainedBytes() const override {
        return m_disk.RetainedBytes();
    }
    std::size_t ByteLimit() const override {
        return m_disk.ByteLimit();
    }
    std::size_t SegmentCount() const override {
        return m_disk.SegmentCount();
    }
    std::string_view OpenRefusal() const override {
        return m_disk.OpenRefusal();
    }

private:
    std::shared_ptr<RecorderGate> m_gate;
    replay::BoundedReplayRecorder m_disk;
};

TEST_F(ReplayStreamTest, QueuedTickOnlyRetainsImmutableDataWhileSinkIsBlocked) {
    auto gate = std::make_shared<RecorderGate>();
    replay::QueuedReplayRecorder recorder(std::make_unique<GatedReplaySink>(gate));
    const ReleaseRecorderGate release{gate};
    ASSERT_TRUE(recorder.Configure(1024, 0));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    const auto inputs = std::make_shared<const std::vector<std::uint8_t>>(512, 0xab);
    const std::weak_ptr<const std::vector<std::uint8_t>> lifetime = inputs;
    ASSERT_TRUE(recorder.RecordTick(1, inputs, nullptr, 1));
    WaitForRecorderWorker(gate);
    ASSERT_TRUE(recorder.RecordTick(2, inputs, nullptr, 2));
    EXPECT_EQ(recorder.PendingTicks(), 2u); // includes the active, blocked sink tick
    EXPECT_EQ(gate->written_ticks.load(), 0u);
    EXPECT_EQ(gate->digest_bytes.load(), 0u);
    EXPECT_EQ(gate->simulation_sink_calls.load(), 0u);
    EXPECT_EQ(inputs.use_count(), 3); // caller, active tick, queued tick: no blob copy
    EXPECT_FALSE(lifetime.expired());
    ReleaseRecorder(gate);
    ASSERT_TRUE(recorder.Flush());
    EXPECT_EQ(gate->first_input, inputs->data());
    EXPECT_EQ(gate->written_ticks.load(), 2u);
    EXPECT_EQ(gate->digest_bytes.load(), 1024u);
    EXPECT_EQ(gate->simulation_sink_calls.load(), 0u);
    EXPECT_EQ(inputs.use_count(), 1);
    EXPECT_EQ(recorder.PendingBytes(), 0u);
}

TEST_F(ReplayStreamTest, QueuedRecorderLatchesCountAndByteOverloadAndDrainsAcceptedTicks) {
    for (const bool byte_limit : {false, true}) {
        SCOPED_TRACE(byte_limit);
        auto gate = std::make_shared<RecorderGate>();
        replay::QueuedReplayRecorder recorder(std::make_unique<GatedReplaySink>(gate));
        const ReleaseRecorderGate release{gate};
        // Each input frame costs 18 bytes. Both limits include the active tick.
        ASSERT_TRUE(recorder.Configure(
            18, 0, {.bytes = byte_limit ? 36u : 1024u, .ticks = byte_limit ? 4u : 2u}));
        ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
        const auto inputs = std::make_shared<const std::vector<std::uint8_t>>(1, 0xab);
        ASSERT_TRUE(recorder.RecordTick(1, inputs, nullptr, 1));
        WaitForRecorderWorker(gate);
        ASSERT_TRUE(recorder.RecordTick(2, inputs, nullptr, 2));
        EXPECT_FALSE(recorder.RecordTick(3, inputs, nullptr, 3));
        EXPECT_EQ(recorder.PendingTicks(), 2u);
        EXPECT_EQ(recorder.PendingBytes(), 36u);
        EXPECT_STREQ(recorder.FailureReason(), "recorder queue capacity exceeded");
        EXPECT_EQ(gate->written_ticks.load(), 0u);
        ReleaseRecorder(gate);
        EXPECT_FALSE(recorder.Flush());
        EXPECT_EQ(gate->ticks, (std::vector<std::uint64_t>{1, 2}));
        EXPECT_EQ(recorder.PendingTicks(), 0u);
        EXPECT_FALSE(recorder.RecordTick(4, inputs, nullptr, 4)); // sticky failure
        EXPECT_FALSE(recorder.Finalize(m_path, 3, 3));
        EXPECT_FALSE(fs::exists(m_path));
        const auto reason_path = m_path + ".incomplete";
        std::ifstream reason(reason_path);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(reason), {}),
                  "Incomplete replay at tick 3: recorder queue capacity exceeded\n");
        reason.close(); // close before removal on Windows
        fs::remove(reason_path);
        recorder.Close();
        EXPECT_EQ(gate->closes.load(), 1u);
    }
}

TEST_F(ReplayStreamTest, QueuedRecorderChargesRetainedCapacityBeforeEnqueue) {
    auto gate = std::make_shared<RecorderGate>();
    replay::QueuedReplayRecorder recorder(std::make_unique<GatedReplaySink>(gate));
    const ReleaseRecorderGate release{gate};
    ASSERT_TRUE(recorder.Configure(18, 0, {.bytes = 1024, .ticks = 2}));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    std::vector<std::uint8_t> storage;
    storage.reserve(2048);
    storage.push_back(0xab);
    const auto inputs = std::make_shared<const std::vector<std::uint8_t>>(std::move(storage));
    ASSERT_GE(inputs->capacity(), 2048u);
    EXPECT_FALSE(recorder.RecordTick(1, inputs, nullptr, 1));
    EXPECT_STREQ(recorder.FailureReason(), "recorder queue capacity exceeded");
    EXPECT_EQ(recorder.PendingTicks(), 0u);
    EXPECT_EQ(recorder.PendingBytes(), 0u);
    EXPECT_EQ(gate->written_ticks.load(), 0u);
}

TEST_F(ReplayStreamTest, QueuedRecorderPreservesOrderingDigestAndDesyncArtifactBytes) {
    const auto expected_path = m_path + ".expected";
    replay::BoundedReplayRecorder expected;
    ASSERT_TRUE(expected.Configure(256, 64));
    ASSERT_TRUE(expected.Open(expected_path, MakeHeader())) << expected.OpenRefusal();
    auto gate = std::make_shared<RecorderGate>();
    replay::QueuedReplayRecorder recorder(std::make_unique<GatedReplaySink>(gate));
    const ReleaseRecorderGate release{gate};
    ASSERT_TRUE(recorder.Configure(256, 64));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    constexpr std::uint64_t ticks = 5000;
    constexpr std::uint64_t divergence = 4970;
    for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
        const auto inputs =
            std::make_shared<const std::vector<std::uint8_t>>(1, static_cast<std::uint8_t>(tick));
        std::shared_ptr<const replay::CheckpointRecord> checkpoint;
        std::array<std::string_view, 4> hashes{};
        if (tick % 30 == 0) {
            checkpoint = std::make_shared<const replay::CheckpointRecord>(
                replay::CheckpointRecord{.tick = tick,
                                         .world_hash = "world",
                                         .terrain = "terrain",
                                         .water = "water",
                                         .entities = "entities"});
            hashes = {checkpoint->world_hash,
                      checkpoint->terrain,
                      checkpoint->water,
                      checkpoint->entities};
        }
        ASSERT_TRUE(expected.RecordTick(tick, *inputs, checkpoint ? &hashes : nullptr, tick - 1));
        ASSERT_TRUE(recorder.RecordTick(tick, inputs, checkpoint, tick - 1));
        if (tick == 1)
            WaitForRecorderWorker(gate);
        if (tick == 256)
            ReleaseRecorder(gate);
        if (tick % 256 == 0) {
            ASSERT_TRUE(recorder.Flush());
        }
    }
    ASSERT_TRUE(expected.Finalize(expected_path, ticks, divergence));
    // Finalize itself flushes the last partial batch, with checkpoints cut off at
    // the desync tick just like the synchronous artifact used by both drivers.
    ASSERT_TRUE(recorder.Finalize(m_path, ticks, divergence));
    ASSERT_EQ(gate->ticks.size(), ticks);
    for (std::size_t index = 0; index < gate->ticks.size(); ++index)
        EXPECT_EQ(gate->ticks[index], index + 1);
    EXPECT_EQ(gate->simulation_sink_calls.load(), 0u);
    std::ifstream actual(m_path, std::ios::binary);
    std::ifstream original(expected_path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(actual), {}),
              std::string(std::istreambuf_iterator<char>(original), {}));
    actual.close();
    original.close();
    fs::remove(expected_path);
}

TEST_F(ReplayStreamTest, QueuedSinkFailureIsReportedAfterAcceptedWorkDrains) {
    auto gate = std::make_shared<RecorderGate>();
    gate->fail_tick = 2;
    replay::QueuedReplayRecorder recorder(std::make_unique<GatedReplaySink>(gate));
    const ReleaseRecorderGate release{gate};
    ASSERT_TRUE(recorder.Configure(18, 0));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    const auto inputs = std::make_shared<const std::vector<std::uint8_t>>(1, 0xab);
    ASSERT_TRUE(recorder.RecordTick(1, inputs, nullptr, 1));
    WaitForRecorderWorker(gate);
    ASSERT_TRUE(recorder.RecordTick(2, inputs, nullptr, 2));
    ASSERT_TRUE(recorder.RecordTick(3, inputs, nullptr, 3));
    ReleaseRecorder(gate);
    EXPECT_FALSE(recorder.Flush());
    EXPECT_EQ(gate->ticks, (std::vector<std::uint64_t>{1, 2, 3}));
    EXPECT_EQ(recorder.PendingTicks(), 0u);
    EXPECT_STREQ(recorder.FailureReason(), "recorder sink write failed");
    EXPECT_FALSE(recorder.RecordTick(4, inputs, nullptr, 4));
    EXPECT_FALSE(recorder.Finalize(m_path, 3, 3));
    const auto reason_path = m_path + ".incomplete";
    std::ifstream reason(reason_path);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(reason), {}),
              "Incomplete replay at tick 2: recorder sink write failed\n");
    reason.close();
    fs::remove(reason_path);
}

TEST_F(ReplayStreamTest, QueuedShutdownDrainsBeforeRemovingAllWorkingFiles) {
    const auto directory = fs::path(m_path + ".working");
    ASSERT_TRUE(fs::create_directory(directory));
    const auto destination = (directory / "shutdown.lrec1").string();
    auto gate = std::make_shared<RecorderGate>();
    {
        replay::QueuedReplayRecorder recorder(std::make_unique<GatedReplaySink>(gate));
        const ReleaseRecorderGate release{gate};
        ASSERT_TRUE(recorder.Configure(18, 0));
        ASSERT_TRUE(recorder.Open(destination, MakeHeader())) << recorder.OpenRefusal();
        for (std::uint64_t tick = 1; tick <= 128; ++tick) {
            ASSERT_TRUE(recorder.RecordTick(
                tick, std::make_shared<const std::vector<std::uint8_t>>(1, 0xab), nullptr, tick));
        }
        WaitForRecorderWorker(gate);
        EXPECT_EQ(gate->written_ticks.load(), 0u);
        // The gate guard releases first; recorder destruction must then drain all
        // 128 accepted ticks before closing the sink and deleting its directory.
    }
    EXPECT_EQ(gate->written_ticks.load(), 128u);
    EXPECT_EQ(gate->closes.load(), 1u);
    ASSERT_EQ(gate->ticks.size(), 128u);
    EXPECT_EQ(gate->ticks.back(), 128u);
    EXPECT_TRUE(fs::is_empty(directory));
    fs::remove(directory);
}

// A deterministic counterexample for the shared namespace: a file occupying the
// next directory candidate makes setup fail, even with a valid replay header.
// This does not identify which filesystem operation failed in a hosted run.
TEST_F(ReplayStreamTest, OccupiedRecorderCandidateRefusesWithoutDeletingTheOtherOwner) {
    const auto directory = fs::path(m_path + ".occupied");
    ASSERT_TRUE(fs::create_directory(directory));
    const auto destination = (directory / "dump.lrec1").string();
    replay::BoundedReplayRecorder probe;
    ASSERT_TRUE(probe.Configure(18, 0));
    ASSERT_TRUE(probe.Open(destination, MakeHeader())) << probe.OpenRefusal();
    std::string name;
    {
        const auto entry = fs::directory_iterator(directory);
        ASSERT_NE(entry, fs::directory_iterator{});
        name = entry->path().filename().string();
    }
    const std::string prefix = "lockstep-recording-";
    ASSERT_EQ(name.find(prefix), 0u);
    const auto next = std::stoull(name.substr(prefix.size())) + 1;
    probe.Close();
    ASSERT_TRUE(fs::is_empty(directory));
    const auto occupied = directory / (prefix + std::to_string(next));
    {
        std::ofstream blocker(occupied, std::ios::binary);
        ASSERT_TRUE(blocker.is_open());
        blocker << "other owner";
        ASSERT_TRUE(blocker.good());
    }
    replay::BoundedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(18, 0));
    EXPECT_TRUE(recorder.OpenRefusal().empty());
    EXPECT_FALSE(recorder.Open(destination, MakeHeader()));
    EXPECT_EQ(recorder.RetainedBytes(), 0u);
    EXPECT_EQ(recorder.SegmentCount(), 0u);
    // The refusal names the reservation step, the candidate and the OS error the
    // same operation reports here, so a hosted failure identifies itself.
    std::error_code expected_error;
    EXPECT_FALSE(fs::create_directory(occupied, expected_error));
    ASSERT_TRUE(expected_error);
    const std::string refusal(recorder.OpenRefusal());
    EXPECT_LT(refusal.size(), replay::BoundedReplayRecorder::kOpenRefusalBytes);
    EXPECT_TRUE(refusal.starts_with("reserve-directory: attempt 0 candidate '" + prefix +
                                    std::to_string(next) + "' beside '"))
        << refusal;
    EXPECT_NE(refusal.find("(" + std::string(expected_error.category().name()) + " " +
                           std::to_string(expected_error.value()) + ")"),
              std::string::npos)
        << refusal;
    recorder.Close();
    EXPECT_FALSE(fs::exists(destination));
    {
        std::ifstream blocker(occupied, std::ios::binary);
        ASSERT_TRUE(blocker.is_open());
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(blocker), {}), "other owner");
    }
    // A reserved sibling workspace has no dependence on the occupied candidate.
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    EXPECT_TRUE(recorder.OpenRefusal().empty());
    ASSERT_TRUE(recorder.RecordTick(1, {0xab}, nullptr, 1));
    ASSERT_TRUE(recorder.Finalize(m_path, 1, 1));
    const auto contents = replay::ReadReplay(m_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents)
        return;
    EXPECT_FALSE(contents->truncated);
    EXPECT_TRUE(contents->trailer_present);
    ASSERT_EQ(contents->inputs.size(), 1u);
    EXPECT_EQ(contents->inputs.front().inputs, (std::vector<std::uint8_t>{0xab}));
    recorder.Close();
    ASSERT_TRUE(fs::remove(occupied));
    ASSERT_TRUE(fs::remove(directory));
}

// Every candidate directory in one attempt window is already taken: the refusal
// reports the attempt count and the first and last candidate it tried.
TEST_F(ReplayStreamTest, ExhaustedRecorderCandidatesRefuseWithTheTriedEnds) {
    constexpr auto kCandidates = replay::BoundedReplayRecorder::kSegmentTicks;
    const auto directory = fs::path(m_path + ".exhausted");
    ASSERT_TRUE(fs::create_directory(directory));
    const auto destination = (directory / "dump.lrec1").string();
    const std::string prefix = "lockstep-recording-";
    replay::BoundedReplayRecorder probe;
    ASSERT_TRUE(probe.Configure(18, 0));
    ASSERT_TRUE(probe.Open(destination, MakeHeader())) << probe.OpenRefusal();
    std::string name;
    {
        const auto entry = fs::directory_iterator(directory);
        ASSERT_NE(entry, fs::directory_iterator{});
        name = entry->path().filename().string();
    }
    ASSERT_EQ(name.find(prefix), 0u);
    const auto first = std::stoull(name.substr(prefix.size())) + 1;
    probe.Close();
    ASSERT_TRUE(fs::is_empty(directory));
    for (std::uint64_t ordinal = first; ordinal < first + kCandidates; ++ordinal)
        ASSERT_TRUE(fs::create_directory(directory / (prefix + std::to_string(ordinal))));
    replay::BoundedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(18, 0));
    EXPECT_FALSE(recorder.Open(destination, MakeHeader()));
    EXPECT_EQ(recorder.RetainedBytes(), 0u);
    EXPECT_EQ(recorder.SegmentCount(), 0u);
    const std::string refusal(recorder.OpenRefusal());
    EXPECT_LT(refusal.size(), replay::BoundedReplayRecorder::kOpenRefusalBytes);
    EXPECT_TRUE(refusal.starts_with("exhausted: " + std::to_string(kCandidates) +
                                    " candidates tried (first " + prefix + std::to_string(first) +
                                    ", last " + prefix + std::to_string(first + kCandidates - 1) +
                                    ") beside '"))
        << refusal;
    EXPECT_TRUE(refusal.ends_with("' already exist")) << refusal;
    std::error_code cleanup;
    fs::remove_all(directory, cleanup);
    EXPECT_FALSE(cleanup);
}

// A queued recorder without a sink has no refusal text to forward and refuses
// without dereferencing it.
TEST_F(ReplayStreamTest, QueuedRecorderWithoutSinkRefusesWithoutDereferencing) {
    replay::QueuedReplayRecorder queued(nullptr);
    EXPECT_TRUE(queued.OpenRefusal().empty());
    EXPECT_FALSE(queued.Configure(18, 0));
    EXPECT_FALSE(queued.Open(m_path, MakeHeader()));
    EXPECT_EQ(queued.OpenRefusal(), "unconfigured: Configure must succeed before Open");
}

// Each refused Open names its failing step in fixed-size text, carries the OS
// error where one exists, keeps only a path tail, and clears after success.
TEST_F(ReplayStreamTest, OpenRefusalsNameTheFailingStepAndStayBounded) {
    constexpr auto kBound = replay::BoundedReplayRecorder::kOpenRefusalBytes;
    replay::BoundedReplayRecorder recorder;
    EXPECT_TRUE(recorder.OpenRefusal().empty());
    EXPECT_FALSE(recorder.Open(m_path, MakeHeader()));
    EXPECT_EQ(recorder.OpenRefusal(), "unconfigured: Configure must succeed before Open");
    ASSERT_TRUE(recorder.Configure(18, 0));
    // An oversized header part is refused before any directory I/O.
    auto header = MakeHeader();
    const auto oversized = replay::BoundedReplayRecorder::kSegmentBytes;
    header.preset.assign(oversized, 'p');
    EXPECT_FALSE(recorder.Open(m_path, header));
    EXPECT_TRUE(recorder.OpenRefusal().starts_with("header-budget: part 1 holds " +
                                                   std::to_string(oversized) + " bytes, "))
        << recorder.OpenRefusal();
    EXPECT_FALSE(fs::exists(m_path));
    // A regular file where a parent directory should be refuses parent creation
    // with the same error the filesystem reports here.
    const auto blocker = m_path + ".parent";
    {
        std::ofstream file(blocker, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << "not a directory";
        ASSERT_TRUE(file.good());
    }
    const auto destination = (fs::path(blocker) / "sub" / "dump.lrec1").string();
    EXPECT_FALSE(recorder.Open(destination, MakeHeader()));
    std::error_code expected_error;
    fs::create_directories(fs::path(destination).parent_path(), expected_error);
    ASSERT_TRUE(expected_error);
    std::string refusal(recorder.OpenRefusal());
    EXPECT_LT(refusal.size(), kBound);
    EXPECT_TRUE(refusal.starts_with("create-parent: '")) << refusal;
    EXPECT_NE(refusal.find("(" + std::string(expected_error.category().name()) + " " +
                           std::to_string(expected_error.value()) + ")"),
              std::string::npos)
        << refusal;
    // A long destination contributes only its tail; the text stays bounded.
    const auto long_destination =
        (fs::path(blocker) / std::string(600, 'd') / "dump.lrec1").string();
    EXPECT_FALSE(recorder.Open(long_destination, MakeHeader()));
    refusal = std::string(recorder.OpenRefusal());
    EXPECT_LT(refusal.size(), kBound);
    EXPECT_TRUE(refusal.starts_with("create-parent: '...")) << refusal;
    EXPECT_EQ(refusal.find(long_destination), std::string::npos) << refusal;
    // Success clears the text; a second Open on the open recorder is refused.
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    EXPECT_TRUE(recorder.OpenRefusal().empty());
    EXPECT_FALSE(recorder.Open(m_path, MakeHeader()));
    EXPECT_TRUE(recorder.OpenRefusal().starts_with("already-open: working directory '"))
        << recorder.OpenRefusal();
    EXPECT_EQ(recorder.SegmentCount(), 0u);
    recorder.Close();
    // The queued recorder reports its own preconditions and forwards the sink's.
    replay::QueuedReplayRecorder queued;
    EXPECT_TRUE(queued.OpenRefusal().empty());
    EXPECT_FALSE(queued.Open(m_path, MakeHeader()));
    EXPECT_EQ(queued.OpenRefusal(), "unconfigured: Configure must succeed before Open");
    ASSERT_TRUE(queued.Configure(18, 0));
    EXPECT_FALSE(queued.Open(destination, MakeHeader()));
    EXPECT_TRUE(queued.OpenRefusal().starts_with("create-parent: '")) << queued.OpenRefusal();
    ASSERT_TRUE(queued.Open(m_path, MakeHeader())) << queued.OpenRefusal();
    EXPECT_TRUE(queued.OpenRefusal().empty());
    EXPECT_FALSE(queued.Open(m_path, MakeHeader()));
    EXPECT_EQ(queued.OpenRefusal(), "already-open: recorder is open");
    queued.Close();
    ASSERT_TRUE(fs::remove(blocker));
}

TEST_F(ReplayStreamTest, ClosingOneRecorderPreservesTheOtherRecordersWorkingFiles) {
    replay::BoundedReplayRecorder first;
    replay::BoundedReplayRecorder second;
    ASSERT_TRUE(first.Configure(18, 0));
    ASSERT_TRUE(second.Configure(18, 0));
    const auto other_path = m_path + ".peer";
    ASSERT_TRUE(first.Open(m_path, MakeHeader())) << first.OpenRefusal();
    ASSERT_TRUE(second.Open(other_path, MakeHeader())) << second.OpenRefusal();
    ASSERT_TRUE(first.RecordTick(1, {0xab}, nullptr, 1));
    ASSERT_TRUE(second.RecordTick(1, {0xcd}, nullptr, 1));
    ASSERT_TRUE(first.Finalize(m_path, 1, 1));
    first.Close();
    ASSERT_TRUE(second.RecordTick(2, {0xef}, nullptr, 2));
    ASSERT_TRUE(second.Finalize(other_path, 2, 2));
    second.Close();
    const auto contents = replay::ReadReplay(other_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents)
        return;
    EXPECT_FALSE(contents->truncated);
    EXPECT_TRUE(contents->trailer_present);
    ASSERT_EQ(contents->inputs.size(), 2u);
    EXPECT_EQ(contents->inputs.front().inputs, (std::vector<std::uint8_t>{0xcd}));
    EXPECT_EQ(contents->inputs.back().inputs, (std::vector<std::uint8_t>{0xef}));
    ASSERT_TRUE(fs::remove(other_path));
}

TEST_F(ReplayStreamTest, BoundedRecorderRejectsOverflowAndOversizedTicksBeforeWriting) {
    replay::BoundedReplayRecorder recorder;
    EXPECT_FALSE(recorder.Configure((std::numeric_limits<std::size_t>::max)(), 1));
    EXPECT_FALSE(recorder.Configure(17, (std::numeric_limits<std::uint64_t>::max)()));
    ASSERT_TRUE(recorder.Configure(17, 0));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    EXPECT_FALSE(recorder.RecordTick(1, {1}, nullptr, 1));
    EXPECT_EQ(recorder.RetainedBytes(), 0u);
    EXPECT_EQ(recorder.SegmentCount(), 0u);
    ASSERT_TRUE(recorder.RecordTick(1, {}, nullptr, 1));
    EXPECT_FALSE(recorder.RecordTick(1, {}, nullptr, 1));
    EXPECT_FALSE(recorder.Configure(17, 0));
}

TEST_F(ReplayStreamTest, BoundedRecorderKeepsCompleteReplayEncodingBeforeRotation) {
    const auto expected_path = m_path + ".expected";
    auto header = MakeHeader();
    ASSERT_EQ(header.version, 3u);
    header.start_world_hash = "boot";
    replay::ReplayWriter expected;
    ASSERT_TRUE(expected.Open(expected_path, header));
    replay::BoundedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(128, 3));
    ASSERT_TRUE(recorder.Open(m_path, header)) << recorder.OpenRefusal();
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        const std::vector<std::uint8_t> inputs{static_cast<std::uint8_t>(tick)};
        const replay::CheckpointRecord checkpoint{.tick = tick,
                                                  .world_hash = "world",
                                                  .terrain = "terrain",
                                                  .water = "water",
                                                  .entities = "entities"};
        const std::array<std::string_view, 4> hashes{
            checkpoint.world_hash, checkpoint.terrain, checkpoint.water, checkpoint.entities};
        ASSERT_TRUE(recorder.RecordTick(tick, inputs, &hashes, tick - 1));
        expected.RecordInput(tick, inputs);
        expected.RecordCheckpoint(checkpoint);
    }
    ASSERT_TRUE(expected.Finalize(3));
    ASSERT_TRUE(recorder.Finalize(m_path, 3, 3));
    const auto parsed = replay::ReadReplay(m_path);
    if (!parsed.has_value()) {
        FAIL() << "complete version-3 recording could not be read";
    }
    const auto& contents = *parsed;
    EXPECT_EQ(contents.header.version, 3u);
    EXPECT_EQ(contents.header.start_world_hash, "boot");
    EXPECT_EQ(contents.elided_through_tick, 0u);
    {
        std::ifstream actual(m_path, std::ios::binary);
        std::ifstream original(expected_path, std::ios::binary);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(actual), {}),
                  std::string(std::istreambuf_iterator<char>(original), {}));
    }
    fs::remove(expected_path);
}

TEST_F(ReplayStreamTest, BoundedRecorderPrefixDigestMatchesElidedWireBytes) {
    replay::BoundedReplayRecorder recorder;
    ASSERT_TRUE(recorder.Configure(18, 0));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    std::string prefix;
    for (std::uint64_t tick = 1; tick <= 2048; ++tick) {
        const auto byte = static_cast<std::uint8_t>(tick);
        ASSERT_TRUE(recorder.RecordTick(tick, {byte}, nullptr, tick));
        if (tick <= 1024) {
            prefix.push_back(static_cast<char>(replay::RecordType::Input));
            for (int shift = 0; shift < 64; shift += 8)
                prefix.push_back(static_cast<char>((tick >> shift) & 0xff));
            // Input frame payload size = u32 blob length + one byte.
            prefix.append({5, 0, 0, 0, 1, 0, 0, 0, static_cast<char>(byte)});
        }
    }
    ASSERT_TRUE(recorder.Finalize(m_path, 2048, 2048));
    const auto parsed = replay::ReadReplay(m_path);
    if (!parsed.has_value()) {
        FAIL() << "bounded replay header did not round-trip";
    }
    const auto& contents = *parsed;
    EXPECT_FALSE(contents.truncated);
    EXPECT_TRUE(contents.trailer_present);
    EXPECT_EQ(contents.header.version, 4u);
    EXPECT_TRUE(replay::ConfigurationMatches(contents.header,
                                             contents.header.system_config_identity,
                                             contents.header.preset_content_hash));
    EXPECT_EQ(contents.elided_through_tick, 1024u);
    EXPECT_EQ(contents.elided_prefix_digest, std::stoull(replay::Fnv1a64Hex(prefix), nullptr, 16));
    ASSERT_EQ(contents.inputs.size(), 1024u);
    EXPECT_EQ(contents.inputs.front().tick, 1025u);
    EXPECT_EQ(contents.inputs.back().tick, 2048u);
    EXPECT_EQ(contents.tick_count, 2048u);
    // Earlier version-3 readers reject this tag. It must not be accepted under
    // that header even by a reader that also supports diagnostic version 4.
    {
        std::fstream stream(m_path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(stream.is_open());
        stream.seekp(6); // magic + reserved byte, then little-endian u16 version
        stream.put(3);
        stream.put(0);
        ASSERT_TRUE(stream.good());
    }
    const auto downgraded = replay::ReadReplay(m_path);
    if (!downgraded.has_value()) {
        FAIL() << "version-3 header should be accepted before record validation";
    }
    const auto& invalid_contents = *downgraded;
    EXPECT_TRUE(invalid_contents.truncated);
    EXPECT_EQ(invalid_contents.elided_through_tick, 0u);
}

TEST_F(ReplayStreamTest, VersionFourHeaderPreservesIdentityAndCompleteRecordLayout) {
    auto header = MakeHeader();
    header.version = 4;
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, header));
    writer.RecordInput(1, {0xab});
    ASSERT_TRUE(writer.Finalize(1));
    const auto parsed = replay::ReadReplay(m_path);
    if (!parsed.has_value()) {
        FAIL() << "version-4 recording could not be read";
    }
    const auto& contents = *parsed;
    EXPECT_EQ(contents.header.version, 4u);
    EXPECT_TRUE(replay::ConfigurationMatches(
        contents.header, header.system_config_identity, header.preset_content_hash));
    EXPECT_FALSE(contents.truncated);
    EXPECT_TRUE(contents.trailer_present);
    EXPECT_EQ(contents.elided_through_tick, 0u);
    ASSERT_EQ(contents.inputs.size(), 1u);
    EXPECT_EQ(contents.inputs.front().tick, 1u);
    EXPECT_EQ(contents.inputs.front().inputs, (std::vector<std::uint8_t>{0xab}));
}

TEST_F(ReplayStreamTest, BoundedRecorderRotatesOnByteLimitAndRetainsMaximumTick) {
    replay::BoundedReplayRecorder recorder;
    constexpr std::size_t payload_bytes = replay::BoundedReplayRecorder::kSegmentBytes;
    ASSERT_TRUE(recorder.Configure(payload_bytes + 17, 0));
    ASSERT_TRUE(recorder.Open(m_path, MakeHeader())) << recorder.OpenRefusal();
    const std::vector<std::uint8_t> inputs(payload_bytes, 0xab);
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        ASSERT_TRUE(recorder.RecordTick(tick, inputs, nullptr, tick));
        EXPECT_EQ(recorder.SegmentCount(), 1u);
        EXPECT_EQ(recorder.RetainedBytes(), payload_bytes + 17);
        EXPECT_LE(recorder.RetainedBytes(), recorder.ByteLimit());
    }
    ASSERT_TRUE(recorder.Finalize(m_path, 3, 3));
    const auto parsed = replay::ReadReplay(m_path);
    if (!parsed.has_value()) {
        FAIL() << "large bounded replay could not be read";
    }
    const auto& contents = *parsed;
    EXPECT_FALSE(contents.truncated);
    EXPECT_EQ(contents.elided_through_tick, 2u);
    ASSERT_EQ(contents.inputs.size(), 1u);
    EXPECT_EQ(contents.inputs.front().tick, 3u);
    EXPECT_EQ(contents.inputs.front().inputs, inputs);
}

// Header fields survive a write/read round-trip exactly.
TEST_F(ReplayStreamTest, HeaderRoundtrip) {
    const replay::ReplayHeader original = MakeHeader();

    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, original));
    ASSERT_TRUE(writer.Finalize(0));

    const auto contents = replay::ReadReplay(m_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value())
        return;
    EXPECT_FALSE(contents->truncated);
    EXPECT_TRUE(contents->trailer_present);

    const replay::ReplayHeader& h = contents->header;
    EXPECT_EQ(h.version, original.version);
    EXPECT_EQ(h.tick_rate_hz, original.tick_rate_hz);
    EXPECT_EQ(h.seed, original.seed);
    EXPECT_EQ(h.seed_string, original.seed_string);
    EXPECT_EQ(h.preset, original.preset);
    EXPECT_EQ(h.preset_hash, original.preset_hash);
    EXPECT_EQ(h.start_world_hash, original.start_world_hash);
    EXPECT_EQ(h.surface_radius, original.surface_radius);
    EXPECT_EQ(h.collision_radius, original.collision_radius);
    EXPECT_EQ(h.engine_version, original.engine_version);
    EXPECT_EQ(h.system_config_identity, original.system_config_identity);
    EXPECT_EQ(h.preset_content_hash, original.preset_content_hash);
}

TEST_F(ReplayStreamTest, SamePresetNameWithDifferentResolvedContentRefusesVerification) {
    replay::ReplayWriter writer;
    const auto header = MakeHeader();
    ASSERT_TRUE(writer.Open(m_path, header));
    ASSERT_TRUE(writer.Finalize(0));
    const auto contents = replay::ReadReplay(m_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value())
        return;
    const auto& recorded_header = contents->header;
    auto different = Luminumbra::TestSupport::PresetIdentityFixture();
    different["generation_params"]["terrain"]["height_offset"] = 90.0;
    EXPECT_TRUE(replay::ConfigurationMatches(
        recorded_header, header.system_config_identity, header.preset_content_hash));
    EXPECT_FALSE(
        replay::ConfigurationMatches(recorded_header,
                                     header.system_config_identity,
                                     Luminumbra::world::TerrainPresetContentHash(different)));
    EXPECT_FALSE(replay::ConfigurationMatches(recorded_header, header.system_config_identity, ""));
    auto unbound = recorded_header;
    unbound.preset_content_hash.clear();
    EXPECT_FALSE(replay::ConfigurationMatches(
        unbound, header.system_config_identity, header.preset_content_hash));
}

TEST_F(ReplayStreamTest, VersionTwoWithoutPresetIdentityIsRejected) {
    auto header = MakeHeader();
    header.version = 2;
    header.preset_content_hash.clear();
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, header));
    ASSERT_TRUE(writer.Finalize(0));
    EXPECT_FALSE(replay::ReadReplay(m_path).has_value());
}

TEST_F(ReplayStreamTest, EffectiveConfigIdentityRefusesDifferentRulesBeforePlayback) {
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(0));
    const auto contents = replay::ReadReplay(m_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value())
        return;
    EXPECT_TRUE(
        replay::ConfigurationMatches(contents->header,
                                     Luminumbra::world::GameSession{}.SystemConfigIdentity(),
                                     MakeHeader().preset_content_hash));
    for (const auto& [name, identity] : Luminumbra::TestSupport::EffectiveIdentityMismatches()) {
        SCOPED_TRACE(name);
        EXPECT_FALSE(replay::ConfigurationMatches(
            contents->header, identity, MakeHeader().preset_content_hash));
    }
    EXPECT_FALSE(
        replay::ConfigurationMatches(contents->header, "", MakeHeader().preset_content_hash));
    auto unbound = contents->header;
    unbound.system_config_identity.clear();
    EXPECT_FALSE(replay::ConfigurationMatches(unbound, "", MakeHeader().preset_content_hash));
    EXPECT_FALSE(replay::ConfigurationMatches(
        unbound, MakeHeader().system_config_identity, MakeHeader().preset_content_hash));
}

TEST_F(ReplayStreamTest, LegacyHeaderWithoutConfigIdentityIsRejected) {
    auto header = MakeHeader();
    header.version = 1;
    header.system_config_identity.clear();
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, header));
    ASSERT_TRUE(writer.Finalize(0));
    EXPECT_FALSE(replay::ReadReplay(m_path).has_value());
}

// Input records (including an EMPTY input set) and checkpoints round-trip with
// identical content and order; the trailer tick count is preserved.
TEST_F(ReplayStreamTest, RecordAppendReadIdentity) {
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));

    // Tick 1: empty input (the headless-today case -- must be representable).
    writer.RecordInput(1, {});
    // Tick 2: a non-empty opaque input blob.
    const std::vector<std::uint8_t> blob = {0x10, 0x20, 0x30, 0x40, 0xFF};
    writer.RecordInput(2, blob);

    replay::CheckpointRecord cp;
    cp.tick = 30;
    cp.world_hash = "d9622e2c4cd6b833";
    cp.terrain = "dead4e08b210018a";
    cp.water = "0154414e2f81e203";
    cp.entities = "5735a5094c1e92a8";
    writer.RecordCheckpoint(cp);

    ASSERT_TRUE(writer.Finalize(30));
    EXPECT_EQ(writer.InputRecordCount(), 2u);
    EXPECT_EQ(writer.CheckpointRecordCount(), 1u);

    const auto contents = replay::ReadReplay(m_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value())
        return;
    EXPECT_FALSE(contents->truncated);
    EXPECT_TRUE(contents->trailer_present);
    EXPECT_EQ(contents->tick_count, 30u);

    ASSERT_EQ(contents->inputs.size(), 2u);
    EXPECT_EQ(contents->inputs[0].tick, 1u);
    EXPECT_TRUE(contents->inputs[0].inputs.empty()); // empty input set preserved
    EXPECT_EQ(contents->inputs[1].tick, 2u);
    EXPECT_EQ(contents->inputs[1].inputs, blob);

    ASSERT_EQ(contents->checkpoints.size(), 1u);
    const replay::CheckpointRecord& got = contents->checkpoints[0];
    EXPECT_EQ(got.tick, cp.tick);
    EXPECT_EQ(got.world_hash, cp.world_hash);
    EXPECT_EQ(got.terrain, cp.terrain);
    EXPECT_EQ(got.water, cp.water);
    EXPECT_EQ(got.entities, cp.entities);

    // Lookups by tick.
    const replay::InputRecord* in2 = replay::FindInput(*contents, 2);
    ASSERT_NE(in2, nullptr);
    EXPECT_EQ(in2->inputs, blob);
    EXPECT_EQ(replay::FindInput(*contents, 99), nullptr);

    const replay::CheckpointRecord* cp30 = replay::FindCheckpoint(*contents, 30);
    ASSERT_NE(cp30, nullptr);
    EXPECT_EQ(cp30->world_hash, cp.world_hash);
    EXPECT_EQ(replay::FindCheckpoint(*contents, 60), nullptr);
}

// A finalized stream truncated at its tail must be DETECTED (truncated=true /
// trailer_present=false), never silently accepted as complete.
TEST_F(ReplayStreamTest, TruncationDetected) {
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    for (std::uint64_t t = 1; t <= 5; ++t) {
        writer.RecordInput(t, {static_cast<std::uint8_t>(t)});
    }
    replay::CheckpointRecord cp;
    cp.tick = 30;
    cp.world_hash = "aaaaaaaaaaaaaaaa";
    cp.terrain = "bbbbbbbbbbbbbbbb";
    cp.water = "cccccccccccccccc";
    cp.entities = "dddddddddddddddd";
    writer.RecordCheckpoint(cp);
    ASSERT_TRUE(writer.Finalize(30));

    // A complete read first (sanity).
    {
        const auto whole = replay::ReadReplay(m_path);
        ASSERT_TRUE(whole.has_value());
        if (!whole.has_value())
            return;
        EXPECT_FALSE(whole->truncated);
        EXPECT_TRUE(whole->trailer_present);
    }

    // Lop off the trailing third of the file (drops the trailer and part of a
    // record), then re-read: truncation must be flagged.
    const auto full_size = static_cast<std::uintmax_t>(fs::file_size(m_path));
    ASSERT_GT(full_size, 32u);
    std::vector<char> bytes(full_size);
    {
        std::ifstream in(m_path, std::ios::binary);
        in.read(bytes.data(), static_cast<std::streamsize>(full_size));
    }
    const auto keep = static_cast<std::size_t>(full_size * 2 / 3);
    {
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(keep));
    }

    const auto truncated = replay::ReadReplay(m_path);
    // The header still parses, so we get contents -- but it must be flagged.
    ASSERT_TRUE(truncated.has_value());
    if (!truncated.has_value())
        return;
    EXPECT_TRUE(truncated->truncated);
    EXPECT_FALSE(truncated->trailer_present);
}

TEST_F(ReplayStreamTest, HugeHeaderStringLengthsAreRejected) {
    for (const std::uint32_t length : {0xFFFFFFFCu, 0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu}) {
        SCOPED_TRACE(length);
        replay::ReplayWriter writer;
        ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
        ASSERT_TRUE(writer.Finalize(0));
        {
            std::fstream file(m_path, std::ios::binary | std::ios::in | std::ios::out);
            ASSERT_TRUE(file.is_open());
            // The fixed header is 42 bytes, followed by the seed-string length.
            file.seekp(42);
            for (int i = 0; i < 4; ++i)
                file.put(static_cast<char>((length >> (8 * i)) & 0xFF));
            file.close();
            ASSERT_TRUE(file.good());
        }
        EXPECT_FALSE(replay::ReadReplay(m_path).has_value());
    }
}

TEST_F(ReplayStreamTest, HeaderAdmissionNeverReadsTheSparseRecordRegion) {
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    ASSERT_TRUE(writer.Finalize(0));
    const auto complete = ReadBytes();
    const auto header_size = complete.size() - 32;
    // Exercise every short-header boundary, including each string length/data.
    for (std::size_t length = 0; length < header_size; ++length) {
        std::istringstream input(complete.substr(0, length));
        EXPECT_FALSE(replay::ReadReplay(input).has_value()) << length;
    }
    auto invalid = complete;
    invalid[0] = 'X';
    ReplayProvider provider(invalid, std::uint64_t{1} << 40);
    std::istream input(&provider);
    EXPECT_FALSE(replay::ReadReplay(input).has_value());
    EXPECT_EQ(provider.bytes_read, sizeof(replay::kLrec1Magic));

    // The declared field fits the sparse file but exceeds the owned-byte budget.
    PatchU32(invalid, 42, 0xFFFFFFFFu);
    invalid[0] = 'L';
    ReplayProvider oversized(invalid, std::uint64_t{1} << 40);
    std::istream oversized_input(&oversized);
    EXPECT_FALSE(replay::ReadReplay(oversized_input).has_value());
    EXPECT_EQ(oversized.bytes_read, 46u);
}

TEST_F(ReplayStreamTest, CheckedProviderFailuresNeverReturnPartialSuccessOrThrow) {
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(1, {0xA5});
    ASSERT_TRUE(writer.Finalize(1));
    const auto bytes = ReadBytes();
    for (const auto failure : {ReplayProvider::SeekFailure::End,
                               ReplayProvider::SeekFailure::Tell,
                               ReplayProvider::SeekFailure::Rewind}) {
        for (const bool exceptions : {false, true}) {
            ReplayProvider provider(bytes, bytes.size());
            provider.seek_failure = failure;
            std::istream input(&provider);
            if (exceptions) {
                input.exceptions(std::ios::failbit | std::ios::badbit);
            }
            EXPECT_FALSE(replay::ReadReplay(input).has_value());
        }
    }
    // Includes failures inside the header, frame, field, and final trailer byte.
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        ReplayProvider provider(bytes, bytes.size());
        provider.read_limit = length;
        std::istream input(&provider);
        EXPECT_FALSE(replay::ReadReplay(input).has_value()) << length;
    }
    for (const std::size_t allocation_after : {std::size_t{0}, bytes.size() - 33}) {
        for (const bool exceptions : {false, true}) {
            ReplayProvider provider(bytes, bytes.size());
            provider.allocation_after = allocation_after;
            std::istream input(&provider);
            if (exceptions) {
                input.exceptions(std::ios::badbit);
            }
            EXPECT_FALSE(replay::ReadReplay(input).has_value());
        }
    }
    EXPECT_FALSE(replay::ReadReplay(m_path + ".missing").has_value());
    EXPECT_FALSE(replay::ReadReplay(ReplayTempDir().string()).has_value());
}

TEST_F(ReplayStreamTest, FieldAndRecordBudgetsRefuseBeforePayloadReads) {
    replay::ReplayWriter writer;
    const auto header = MakeHeader();
    ASSERT_TRUE(writer.Open(m_path, header));
    writer.RecordInput(1, {0xA5});
    writer.RecordInput(9, {});
    replay::CheckpointRecord checkpoint;
    checkpoint.tick = 9;
    checkpoint.world_hash = "world";
    checkpoint.terrain = "terrain";
    checkpoint.water = "water";
    checkpoint.entities = "entities";
    writer.RecordCheckpoint(checkpoint);
    ASSERT_TRUE(writer.Finalize(9));
    const auto bytes = ReadBytes();
    const auto header_fields = header.seed_string.size() + header.preset.size() +
                               header.start_world_hash.size() + header.engine_version.size() +
                               header.system_config_identity.size() +
                               header.preset_content_hash.size();
    const auto header_size = 42 + 6 * 4 + header_fields;
    ReplayProvider provider(bytes, bytes.size());
    std::istream input(&provider);
    EXPECT_FALSE(
        replay::ReadReplay(input, {.field_bytes = header_fields, .records = 3}).has_value());
    EXPECT_EQ(provider.bytes_read, header_size + 13 + 4);
    for (const std::size_t count : {0u, 1u, 2u}) {
        std::istringstream limited(bytes);
        EXPECT_FALSE(replay::ReadReplay(limited, {.records = count}).has_value());
    }
    const auto fields = header_fields + 1 + checkpoint.world_hash.size() +
                        checkpoint.terrain.size() + checkpoint.water.size() +
                        checkpoint.entities.size();
    std::istringstream exact(bytes);
    const auto admitted = replay::ReadReplay(exact, {.field_bytes = fields, .records = 3});
    ASSERT_TRUE(admitted.has_value());
    if (!admitted.has_value()) {
        return;
    }
    EXPECT_FALSE(admitted->truncated);
    std::istringstream short_budget(bytes);
    EXPECT_FALSE(
        replay::ReadReplay(short_budget, {.field_bytes = fields - 1, .records = 3}).has_value());
    EXPECT_FALSE(replay::ReadReplay(m_path, {.records = 0}).has_value());
}

TEST_F(ReplayStreamTest, SparseProviderPreservesOpaqueBlobsLargerThanRecorderSegments) {
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(std::numeric_limits<std::uint64_t>::max(), {});
    ASSERT_TRUE(writer.Finalize(std::numeric_limits<std::uint64_t>::max()));
    auto bytes = ReadBytes();
    const auto header_size = bytes.size() - 32 - 17;
    constexpr std::uint32_t kBlobBytes = 64 * 1024 * 1024 + 1;
    PatchU32(bytes, header_size + 9, kBlobBytes + 4);
    PatchU32(bytes, header_size + 13, kBlobBytes);
    const auto trailer = bytes.substr(bytes.size() - 32);
    bytes.resize(header_size + 17);
    ReplayProvider provider(bytes, bytes.size() + kBlobBytes + trailer.size());
    provider.suffix = trailer;
    std::istream input(&provider);
    const auto contents = replay::ReadReplay(input);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    EXPECT_FALSE(contents->truncated);
    EXPECT_TRUE(contents->trailer_present);
    ASSERT_EQ(contents->inputs.size(), 1u);
    EXPECT_EQ(contents->inputs[0].tick, std::numeric_limits<std::uint64_t>::max());
    ASSERT_EQ(contents->inputs[0].inputs.size(), kBlobBytes);
    EXPECT_TRUE(std::all_of(contents->inputs[0].inputs.begin(),
                            contents->inputs[0].inputs.end(),
                            [](std::uint8_t byte) { return byte == 0; }));
    EXPECT_EQ(provider.bytes_read, bytes.size() + kBlobBytes + trailer.size());
}

TEST_F(ReplayStreamTest, SparseOpaqueFrameExtensionsUseCheckedSeeks) {
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(5, {});
    ASSERT_TRUE(writer.Finalize(5));
    auto bytes = ReadBytes();
    const auto header_size = bytes.size() - 32 - 17;
    PatchU32(bytes, header_size + 9, 0xFFFFFFFFu);
    bytes.resize(header_size + 17);
    const auto virtual_size = header_size + 13 + std::uint64_t{0xFFFFFFFFu};
    ReplayProvider provider(bytes, virtual_size);
    std::istream input(&provider);
    const auto contents = replay::ReadReplay(input);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    EXPECT_TRUE(contents->truncated);
    ASSERT_EQ(contents->inputs.size(), 1u);
    EXPECT_TRUE(contents->inputs[0].inputs.empty());
    EXPECT_EQ(provider.bytes_read, bytes.size());
    ReplayProvider failed(bytes, virtual_size);
    failed.seek_failure = ReplayProvider::SeekFailure::Skip;
    std::istream failed_input(&failed);
    EXPECT_FALSE(replay::ReadReplay(failed_input).has_value());
}

TEST_F(ReplayStreamTest, TrailingBytesAreFlaggedAndValidReencodingIsByteIdentical) {
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(1, {});
    writer.RecordInput(200, {0, 0xFF, 'L'});
    replay::CheckpointRecord checkpoint;
    checkpoint.tick = 200;
    checkpoint.world_hash = "world";
    checkpoint.terrain = "terrain";
    checkpoint.water = "water";
    checkpoint.entities = "entities";
    writer.RecordCheckpoint(checkpoint);
    ASSERT_TRUE(writer.Finalize(200));
    const auto original = ReadBytes();
    const auto contents = replay::ReadReplay(m_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value()) {
        return;
    }
    ASSERT_FALSE(contents->truncated);
    ASSERT_TRUE(writer.Open(m_path, contents->header));
    for (const auto& record : contents->inputs) {
        writer.RecordInput(record.tick, record.inputs);
    }
    for (const auto& record : contents->checkpoints) {
        writer.RecordCheckpoint(record);
    }
    ASSERT_TRUE(writer.Finalize(contents->tick_count));
    EXPECT_EQ(ReadBytes(), original);
    for (const auto& tail : {std::string(1, '\0'), std::string("garbage")}) {
        {
            std::ofstream output(m_path, std::ios::binary | std::ios::trunc);
            output << original << tail;
            output.close();
            ASSERT_TRUE(output.good());
        }
        const auto invalid = replay::ReadReplay(m_path);
        ASSERT_TRUE(invalid.has_value());
        if (!invalid.has_value()) {
            return;
        }
        EXPECT_TRUE(invalid->truncated);
        EXPECT_TRUE(invalid->trailer_present);
        EXPECT_EQ(invalid->inputs.size(), 2u);
    }
}

TEST_F(ReplayStreamTest, TruncatedFramesAndNestedFieldsPreserveOnlyCompleteRecords) {
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    writer.RecordInput(1, {1, 2, 3});
    replay::CheckpointRecord checkpoint;
    checkpoint.tick = 7;
    checkpoint.world_hash = "w";
    checkpoint.terrain = "t";
    checkpoint.water = "a";
    checkpoint.entities = "e";
    writer.RecordCheckpoint(checkpoint);
    ASSERT_TRUE(writer.Finalize(7));
    const auto bytes = ReadBytes();
    const auto header_size = bytes.size() - 32 - (13 + 4 + 3) - (13 + 4 * 5);
    for (std::size_t size = header_size; size < bytes.size(); ++size) {
        std::istringstream input(bytes.substr(0, size));
        const auto contents = replay::ReadReplay(input);
        ASSERT_TRUE(contents.has_value()) << size;
        if (!contents.has_value()) {
            return;
        }
        EXPECT_TRUE(contents->truncated) << size;
        EXPECT_FALSE(contents->trailer_present) << size;
    }
    // The claimed field fits the file but crosses its own payload boundary.
    for (const std::size_t offset : {header_size + 13,
                                     header_size + 20 + 13,
                                     header_size + 20 + 18,
                                     header_size + 20 + 23,
                                     header_size + 20 + 28}) {
        auto invalid = bytes;
        PatchU32(invalid, offset, 32);
        std::istringstream input(invalid);
        const auto contents = replay::ReadReplay(input);
        ASSERT_TRUE(contents.has_value());
        if (!contents.has_value()) {
            return;
        }
        EXPECT_TRUE(contents->truncated);
        EXPECT_FALSE(contents->trailer_present);
        EXPECT_TRUE(contents->checkpoints.empty());
        EXPECT_EQ(contents->inputs.size(), offset == header_size + 13 ? 0u : 1u);
    }
    for (const std::size_t offset : {header_size, bytes.size() - 31, bytes.size() - 16}) {
        auto invalid = bytes;
        invalid.at(offset) = '\x7F'; // unknown tag, bad trailer magic, wrong record count
        std::istringstream input(invalid);
        const auto contents = replay::ReadReplay(input);
        ASSERT_TRUE(contents.has_value());
        if (!contents.has_value()) {
            return;
        }
        EXPECT_TRUE(contents->truncated);
        EXPECT_EQ(contents->trailer_present, offset == bytes.size() - 16);
        EXPECT_EQ(contents->inputs.size(), offset == header_size ? 0u : 1u);
    }
}

TEST_F(ReplayStreamTest, HugeRecordAndBlobLengthsAreTruncated) {
    for (const std::uint32_t length : {0xFFFFFFFCu, 0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu}) {
        SCOPED_TRACE(length);
        // Record layout: [u8 type][u64 tick][u32 payload len][u32 empty blob len].
        for (const std::streamoff length_offset : {9, 13}) {
            SCOPED_TRACE(length_offset);
            replay::ReplayWriter writer;
            ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
            writer.RecordInput(1, {});
            ASSERT_TRUE(writer.Finalize(1));
            const auto header_size = fs::file_size(m_path) - sizeof(replay::kLrec1Trailer) -
                                     3 * sizeof(std::uint64_t) - 17;
            {
                std::fstream file(m_path, std::ios::binary | std::ios::in | std::ios::out);
                ASSERT_TRUE(file.is_open());
                file.seekp(static_cast<std::streamoff>(header_size) + length_offset);
                for (int i = 0; i < 4; ++i)
                    file.put(static_cast<char>((length >> (8 * i)) & 0xFF));
                file.close();
                ASSERT_TRUE(file.good());
            }
            const auto contents = replay::ReadReplay(m_path);
            ASSERT_TRUE(contents.has_value());
            if (!contents.has_value())
                return;
            EXPECT_TRUE(contents->truncated);
            EXPECT_FALSE(contents->trailer_present);
            EXPECT_TRUE(contents->inputs.empty());
        }
    }
}

// A file too short to even hold the header (or with a bad magic) is rejected
// outright (nullopt) -- it is not a valid LREC1 stream.
TEST_F(ReplayStreamTest, BadMagicRejected) {
    {
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        const char junk[] = "not-an-lrec-stream-at-all";
        out.write(junk, sizeof(junk));
    }
    const auto contents = replay::ReadReplay(m_path);
    EXPECT_FALSE(contents.has_value());
}

// The trailer record counts are cross-checked: a body that loses a record before
// the trailer is flagged truncated even though a trailer is present.
TEST_F(ReplayStreamTest, CheckpointEncodeDecodeMultiple) {
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, MakeHeader()));
    for (std::uint64_t t = 1; t <= 90; ++t) {
        writer.RecordInput(t, {});
        if (t % 30 == 0) {
            replay::CheckpointRecord cp;
            cp.tick = t;
            cp.world_hash = "wh" + std::to_string(t);
            cp.terrain = "tr" + std::to_string(t);
            cp.water = "wa" + std::to_string(t);
            cp.entities = "en" + std::to_string(t);
            writer.RecordCheckpoint(cp);
        }
    }
    ASSERT_TRUE(writer.Finalize(90));

    const auto contents = replay::ReadReplay(m_path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value())
        return;
    EXPECT_FALSE(contents->truncated);
    EXPECT_EQ(contents->tick_count, 90u);
    EXPECT_EQ(contents->inputs.size(), 90u);
    ASSERT_EQ(contents->checkpoints.size(), 3u);
    EXPECT_EQ(contents->checkpoints[0].tick, 30u);
    EXPECT_EQ(contents->checkpoints[1].tick, 60u);
    EXPECT_EQ(contents->checkpoints[2].tick, 90u);
    EXPECT_EQ(contents->checkpoints[2].world_hash, "wh90");
}

// Fnv1a64Hex is stable and 16 lowercase hex chars (the shared hash algorithm).
TEST_F(ReplayStreamTest, Fnv1a64HexStable) {
    const std::string a = replay::Fnv1a64Hex("default");
    const std::string b = replay::Fnv1a64Hex("default");
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.size(), 16u);
    EXPECT_NE(a, replay::Fnv1a64Hex("other"));
    for (char c : a) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << "non-hex char: " << c;
    }
}

} // namespace
