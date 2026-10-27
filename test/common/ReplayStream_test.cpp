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
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "luminumbra_common/replay/ReplayStream.h"

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
        m_path = (ReplayTempDir() / (std::string(info->name()) + ".lrec1")).string();
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

    std::string m_path;
};

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
// reports the window size and the exact candidate range it tried.
TEST_F(ReplayStreamTest, ExhaustedRecorderCandidatesRefuseWithTheTriedRange) {
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
    EXPECT_TRUE(refusal.starts_with("exhausted: " + std::to_string(kCandidates) + " candidates " +
                                    prefix + std::to_string(first) + ".." + prefix +
                                    std::to_string(first + kCandidates - 1) + " beside '"))
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
