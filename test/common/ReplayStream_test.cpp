// LREC1 session replay stream unit tests
// (src/luminumbra_common/replay/ReplayStream.{h,cpp}).
//
// These tests lock the ENGINE-GENERIC stream behavior independent of the server:
// header roundtrip, record append/read identity (including the empty-input
// compact case), truncation detection, checkpoint encode/decode, and the
// versioning gate. They are hermetic -- every stream is written to a unique temp
// file under the test artifact dir and removed afterward, so the suite never
// touches the committed gate fixtures.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "luminumbra_common/replay/ReplayStream.h"

namespace replay = Luminumbra::Replay;
namespace fs = std::filesystem;

namespace {

// Unique temp path under the test artifact dir (hermetic; auto-removed by the
// fixture). LUMINUMBRA_TEST_ARTIFACT_DIR is provided by the build for some
// targets; fall back to the temp dir so the test is portable.
fs::path ReplayTempDir() {
#ifdef LUMINUMBRA_TEST_ARTIFACT_DIR
    fs::path dir = fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "replay-unit";
#else
    fs::path dir = fs::temp_directory_path() / "luminumbra-replay-unit";
#endif
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
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
        return h;
    }

    std::string m_path;
};

// Header fields survive a write/read round-trip exactly.
TEST_F(ReplayStreamTest, HeaderRoundtrip) {
    const replay::ReplayHeader original = MakeHeader();

    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(m_path, original));
    ASSERT_TRUE(writer.Finalize(0));

    const auto contents = replay::ReadReplay(m_path);
    ASSERT_TRUE(contents.has_value());
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
    EXPECT_TRUE(truncated->truncated);
    EXPECT_FALSE(truncated->trailer_present);
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
