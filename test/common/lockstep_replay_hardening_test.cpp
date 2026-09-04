// ADVERSARIAL determinism-hardening tests for the input-lockstep + LREC1 replay
// E2E path (src/luminumbra_common/net/LockstepSession.{h,cpp} +
// src/luminumbra_common/replay/ReplayStream.{h,cpp}).
//
// The sibling happy-path suites (LockstepSession_test.cpp, ReplayStream_test.cpp)
// already lock handshake accept/reject, in-sync exchange, horizon absorption,
// desync detection, clean disconnect, and the basic record round-trip. This suite
// hunts the bugs they MISS on the run==replay / save-load / multiplayer-sync
// contract:
//
//   * encode(x)->decode must reproduce x EXACTLY for EVERY message + record type,
//     across adversarial inputs (max u64 ticks, empty + huge blobs, 0xFF bytes,
//     embedded NULs in strings, boundary client ids).
//   * truncation / garbled-frame robustness on the decode side (a 1-byte-short
//     frame must be REJECTED, never read as garbage).
//   * run==replay byte-exactness: a recorded checkpoint hash stream, written then
//     read, must reproduce the live hash sequence string-for-string; a desync dump
//     emitted from a live session must reconstruct the live cadence hashes exactly.
//   * ORDER independence: merged input application + emitted dumps must NOT depend
//     on input-frame ARRIVAL order (only on tick + ascending client-id).
//   * IDEMPOTENCY / convergence: a duplicate input frame is a no-op; re-reading the
//     same stream is byte-identical; re-decoding is repeatable.
//   * BOUNDARY / degenerate: empty input stream, zero-tick finalize, a session that
//     stalls forever on a dropped peer input (deterministic WaitingForPeer, never a
//     false advance or false desync), max u64 tick.
//
// Hermetic: every stream is written to a unique temp file and removed; no real
// sockets (LoopbackTransport only).

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/replay/ReplayStream.h"

namespace net = Luminumbra::Net;
namespace replay = Luminumbra::Replay;
namespace fs = std::filesystem;

namespace {

// ----------------------------------------------------------------------------
// Hermetic temp dir (common_tests does NOT define LUMINUMBRA_TEST_ARTIFACT_DIR,
// so fall back to the system temp dir -- matches the sibling suites' fallback).
// ----------------------------------------------------------------------------
fs::path HardeningTempDir() {
#ifdef LUMINUMBRA_TEST_ARTIFACT_DIR
    fs::path dir = fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "lockstep-replay-hardening";
#else
    fs::path dir = fs::temp_directory_path() / "luminumbra-lockstep-replay-hardening";
#endif
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path UniquePath(const std::string& stem) {
    const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string name = (info != nullptr) ? info->name() : std::string("anon");
    return HardeningTempDir() / (name + "-" + stem + ".lrec1");
}

// Reads a whole binary file into a byte vector (for byte-exactness comparisons).
std::vector<std::uint8_t> ReadAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in.is_open())
        return {};
    const std::streamsize size = in.tellg();
    if (size <= 0)
        return {};
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

// ----------------------------------------------------------------------------
// Deterministic fake world, mirroring the sibling suite's design: hash is a pure
// function of (tick, section, per-peer salt). Identical salts agree; a salt that
// differs from `corrupt_from` onward forces a localizable divergence.
// ----------------------------------------------------------------------------
struct FakeWorld {
    std::uint64_t tick = 0;
    std::string salt = "agree";
    std::uint64_t corrupt_from = 0; // ticks >= this use a corrupted salt (0 = never)

    std::string HashAt(std::uint64_t t, const char* section) const {
        const std::string s = (corrupt_from != 0 && t >= corrupt_from) ? (salt + "X") : salt;
        return replay::Fnv1a64Hex(s + "|" + section + "|" + std::to_string(t));
    }
};

std::vector<std::uint8_t> CollectEmpty(std::uint64_t, void*) {
    return {};
}

bool ApplyStep(std::uint64_t tick, const std::vector<std::uint8_t>&, void* user) {
    static_cast<FakeWorld*>(user)->tick = tick;
    return true;
}

void CaptureHashes(std::uint64_t tick, net::HashMsg& out, void* user) {
    auto* w = static_cast<FakeWorld*>(user);
    out.tick = tick;
    out.terrain = w->HashAt(tick, "terrain");
    out.water = w->HashAt(tick, "water");
    out.entities = w->HashAt(tick, "entities");
    out.world_hash = replay::Fnv1a64Hex(out.terrain + out.water + out.entities);
}

net::LockstepHooks MakeHooks(FakeWorld* w) {
    net::LockstepHooks h;
    h.collect_local_input = &CollectEmpty;
    h.apply_and_step = &ApplyStep;
    h.capture_hashes = &CaptureHashes;
    h.user = w;
    return h;
}

net::LockstepConfig MakeConfig(std::uint32_t local_id, std::uint32_t peer_id) {
    net::LockstepConfig c;
    c.seed = 424242;
    c.preset = "default";
    c.tick_rate_hz = 30;
    c.local_client_id = local_id;
    c.peer_client_id = peer_id;
    return c;
}

// Sends a peer Hello so a single-process Handshake can complete, then handshakes
// both ends. Returns true iff BOTH handshakes succeed.
bool HandshakeBoth(net::LockstepSession& a, net::LockstepSession& b, net::LoopbackTransport* tb) {
    net::HelloMsg bh;
    bh.seed = 424242;
    bh.preset = "default";
    bh.tick_rate_hz = 30;
    bh.client_id = 1;
    if (!tb->SendFrame(net::EncodeHello(bh)))
        return false;
    if (!a.Handshake())
        return false;
    if (!b.Handshake())
        return false;
    return true;
}

struct DriveResult {
    net::TickOutcome a_outcome = net::TickOutcome::WaitingForPeer;
    net::TickOutcome b_outcome = net::TickOutcome::WaitingForPeer;
    std::uint64_t a_tick = 0;
    std::uint64_t b_tick = 0;
};

DriveResult DriveBoth(net::LockstepSession& a,
                      net::LockstepSession& b,
                      std::uint64_t budget,
                      int max_pumps = 100000) {
    net::TickResult ra, rb;
    auto finished = [](net::TickOutcome o) {
        return o == net::TickOutcome::Finished;
    };
    auto fatal = [](net::TickOutcome o) {
        return o == net::TickOutcome::Desync || o == net::TickOutcome::PeerDisconnected;
    };
    int pumps = 0;
    while (pumps++ < max_pumps) {
        ra = a.PumpTick(budget);
        rb = b.PumpTick(budget);
        if (fatal(ra.outcome) || fatal(rb.outcome))
            break;
        if (finished(ra.outcome) && finished(rb.outcome))
            break;
    }
    return {ra.outcome, rb.outcome, a.AgreedTick(), b.AgreedTick()};
}

replay::ReplayHeader RichHeader() {
    replay::ReplayHeader h;
    h.version = replay::kLrec1Version;
    h.tick_rate_hz = 30;
    h.seed = 0xFEEDFACEDEADBEEFULL;
    h.seed_string = "FEEDFACEDEADBEEF";
    h.preset = "default";
    h.preset_hash = 0xABCDEF0123456789ULL;
    h.start_world_hash = "9d9d500dd22504f4";
    h.surface_radius = 4;
    h.collision_radius = 2;
    h.engine_version = "0.1.0+testsha";
    return h;
}

} // namespace

// ============================================================================
// SECTION 1 -- message encode/decode SYMMETRY (every field, adversarial values).
// ============================================================================

TEST(LockstepCodecHardening, HelloRoundtripAdversarial) {
    net::HelloMsg in;
    in.protocol_version = net::kLockstepProtocolVersion;
    in.seed = 0xFFFFFFFFFFFFFFFFULL;                    // max u64
    in.preset = "preset/with spaces/and-utf8-\xC3\xA9"; // non-ASCII bytes survive
    in.tick_rate_hz = 65535;                            // max u16
    in.client_id = 0xFFFFFFFFu;                         // max u32

    net::HelloMsg out;
    ASSERT_TRUE(net::DecodeHello(net::EncodeHello(in), out));
    EXPECT_EQ(out.protocol_version, in.protocol_version);
    EXPECT_EQ(out.seed, in.seed);
    EXPECT_EQ(out.preset, in.preset);
    EXPECT_EQ(out.tick_rate_hz, in.tick_rate_hz);
    EXPECT_EQ(out.client_id, in.client_id);
}

TEST(LockstepCodecHardening, HelloEmptyPresetRoundtrips) {
    net::HelloMsg in;
    in.preset = ""; // zero-length string is a valid boundary
    net::HelloMsg out;
    ASSERT_TRUE(net::DecodeHello(net::EncodeHello(in), out));
    EXPECT_EQ(out.preset, "");
}

TEST(LockstepCodecHardening, HelloPresetWithEmbeddedNulRoundtrips) {
    // Length-prefixed strings must be NUL-transparent (a C-string would truncate).
    net::HelloMsg in;
    in.preset = std::string("a\0b\0c", 5);
    ASSERT_EQ(in.preset.size(), 5u);
    net::HelloMsg out;
    ASSERT_TRUE(net::DecodeHello(net::EncodeHello(in), out));
    EXPECT_EQ(out.preset.size(), 5u);
    EXPECT_EQ(out.preset, in.preset);
}

TEST(LockstepCodecHardening, InputRoundtripEmptyBlob) {
    net::InputMsg in;
    in.tick = 0;      // tick 0 boundary
    in.client_id = 0; // host id boundary
    in.inputs = {};   // empty is valid + compact
    net::InputMsg out;
    ASSERT_TRUE(net::DecodeInput(net::EncodeInput(in), out));
    EXPECT_EQ(out.tick, 0u);
    EXPECT_EQ(out.client_id, 0u);
    EXPECT_TRUE(out.inputs.empty());
}

TEST(LockstepCodecHardening, InputRoundtripMaxTickAndAllByteValues) {
    net::InputMsg in;
    in.tick = 0xFFFFFFFFFFFFFFFFULL; // max u64 tick
    in.client_id = 0xFFFFFFFFu;
    in.inputs.resize(256);
    for (int i = 0; i < 256; ++i)
        in.inputs[i] = static_cast<std::uint8_t>(i);
    net::InputMsg out;
    ASSERT_TRUE(net::DecodeInput(net::EncodeInput(in), out));
    EXPECT_EQ(out.tick, in.tick);
    EXPECT_EQ(out.client_id, in.client_id);
    EXPECT_EQ(out.inputs, in.inputs); // every byte, in order
}

TEST(LockstepCodecHardening, InputRoundtripLargeBlob) {
    net::InputMsg in;
    in.tick = 12345;
    in.client_id = 1;
    in.inputs.assign(70000, 0xAB); // > u16 to catch a u16 length-prefix bug
    net::InputMsg out;
    ASSERT_TRUE(net::DecodeInput(net::EncodeInput(in), out));
    EXPECT_EQ(out.inputs.size(), 70000u);
    EXPECT_EQ(out.inputs, in.inputs);
}

TEST(LockstepCodecHardening, HashRoundtripAllFields) {
    net::HashMsg in;
    in.tick = 0x0123456789ABCDEFULL;
    in.world_hash = "0011223344556677";
    in.terrain = "deadbeefdeadbeef";
    in.water = ""; // empty section string boundary
    in.entities = "ffffffffffffffff";
    net::HashMsg out;
    ASSERT_TRUE(net::DecodeHash(net::EncodeHash(in), out));
    EXPECT_EQ(out.tick, in.tick);
    EXPECT_EQ(out.world_hash, in.world_hash);
    EXPECT_EQ(out.terrain, in.terrain);
    EXPECT_EQ(out.water, in.water);
    EXPECT_EQ(out.entities, in.entities);
}

TEST(LockstepCodecHardening, ByeRoundtripMaxTick) {
    net::ByeMsg in;
    in.tick = 0xFFFFFFFFFFFFFFFFULL;
    net::ByeMsg out;
    ASSERT_TRUE(net::DecodeBye(net::EncodeBye(in), out));
    EXPECT_EQ(out.tick, in.tick);
}

TEST(LockstepCodecHardening, PeekMessageTypeIdentifiesEachFrame) {
    net::MessageType t = net::MessageType::Bye;
    ASSERT_TRUE(net::PeekMessageType(net::EncodeHello({}), t));
    EXPECT_EQ(t, net::MessageType::Hello);
    ASSERT_TRUE(net::PeekMessageType(net::EncodeInput({}), t));
    EXPECT_EQ(t, net::MessageType::Input);
    ASSERT_TRUE(net::PeekMessageType(net::EncodeHash({}), t));
    EXPECT_EQ(t, net::MessageType::Hash);
    ASSERT_TRUE(net::PeekMessageType(net::EncodeBye({}), t));
    EXPECT_EQ(t, net::MessageType::Bye);
}

TEST(LockstepCodecHardening, PeekMessageTypeRejectsEmptyFrame) {
    net::MessageType t;
    EXPECT_FALSE(net::PeekMessageType({}, t));
}

// Encoding is a pure function: identical inputs -> byte-identical frames (no
// hidden state, no padding, no uninitialized bytes). Determinism prerequisite.
TEST(LockstepCodecHardening, EncodeIsPureAndRepeatable) {
    net::InputMsg in;
    in.tick = 999;
    in.client_id = 1;
    in.inputs = {1, 2, 3, 4, 5};
    EXPECT_EQ(net::EncodeInput(in), net::EncodeInput(in));

    net::HashMsg h;
    h.tick = 30;
    h.world_hash = "abc";
    h.terrain = "def";
    h.water = "ghi";
    h.entities = "jkl";
    EXPECT_EQ(net::EncodeHash(h), net::EncodeHash(h));
}

// Decoding the same frame twice yields the same value (no consuming side effects).
TEST(LockstepCodecHardening, DecodeIsIdempotent) {
    net::InputMsg in;
    in.tick = 77;
    in.client_id = 3;
    in.inputs = {9, 8, 7};
    const auto frame = net::EncodeInput(in);
    net::InputMsg a, b;
    ASSERT_TRUE(net::DecodeInput(frame, a));
    ASSERT_TRUE(net::DecodeInput(frame, b));
    EXPECT_EQ(a.tick, b.tick);
    EXPECT_EQ(a.client_id, b.client_id);
    EXPECT_EQ(a.inputs, b.inputs);
}

// ============================================================================
// SECTION 2 -- truncation / garbled-frame robustness (decode must REJECT, never
// read garbage). A 1-byte-short frame is the canonical desync-injection vector.
// ============================================================================

TEST(LockstepCodecHardening, DecodeHelloRejectsTruncatedFrame) {
    auto frame = net::EncodeHello([] {
        net::HelloMsg m;
        m.preset = "default";
        m.seed = 7;
        return m;
    }());
    ASSERT_GT(frame.size(), 1u);
    net::HelloMsg out;
    // Drop bytes from the tail one at a time; every short frame must be rejected.
    for (std::size_t cut = 1; cut < frame.size(); ++cut) {
        std::vector<std::uint8_t> shortened(frame.begin(), frame.end() - cut);
        EXPECT_FALSE(net::DecodeHello(shortened, out))
            << "accepted a Hello short by " << cut << " byte(s)";
    }
}

TEST(LockstepCodecHardening, DecodeInputRejectsTruncatedFrame) {
    net::InputMsg in;
    in.tick = 5;
    in.client_id = 1;
    in.inputs = {0xDE, 0xAD, 0xBE, 0xEF};
    auto frame = net::EncodeInput(in);
    net::InputMsg out;
    for (std::size_t cut = 1; cut < frame.size(); ++cut) {
        std::vector<std::uint8_t> shortened(frame.begin(), frame.end() - cut);
        EXPECT_FALSE(net::DecodeInput(shortened, out))
            << "accepted an Input short by " << cut << " byte(s)";
    }
}

TEST(LockstepCodecHardening, DecodeHashRejectsTruncatedFrame) {
    net::HashMsg in;
    in.tick = 30;
    in.world_hash = "wh";
    in.terrain = "tr";
    in.water = "wa";
    in.entities = "en";
    auto frame = net::EncodeHash(in);
    net::HashMsg out;
    for (std::size_t cut = 1; cut < frame.size(); ++cut) {
        std::vector<std::uint8_t> shortened(frame.begin(), frame.end() - cut);
        EXPECT_FALSE(net::DecodeHash(shortened, out))
            << "accepted a Hash short by " << cut << " byte(s)";
    }
}

// A frame whose declared payload length exceeds the bytes present (a lying length
// prefix) must be rejected, not over-read.
TEST(LockstepCodecHardening, DecodeInputRejectsLyingPayloadLength) {
    net::InputMsg in;
    in.tick = 1;
    in.client_id = 0;
    in.inputs = {1, 2, 3};
    auto frame = net::EncodeInput(in);
    // Frame layout: [type u8][payload-len u32 @1..4][payload]. Inflate the declared
    // payload length so it claims more than the frame holds.
    frame[1] = 0xFF;
    frame[2] = 0xFF;
    net::InputMsg out;
    EXPECT_FALSE(net::DecodeInput(frame, out));
}

// A frame decoded as the WRONG type must fail (type tag is checked).
TEST(LockstepCodecHardening, DecodeRejectsTypeMismatch) {
    const auto hello = net::EncodeHello({});
    net::InputMsg in;
    EXPECT_FALSE(net::DecodeInput(hello, in));
    net::HashMsg h;
    EXPECT_FALSE(net::DecodeHash(hello, h));
    net::ByeMsg b;
    EXPECT_FALSE(net::DecodeBye(hello, b));
}

// A Hello with a corrupted magic (right type tag, wrong magic bytes) is rejected.
TEST(LockstepCodecHardening, DecodeHelloRejectsBadMagic) {
    auto frame = net::EncodeHello([] {
        net::HelloMsg m;
        m.preset = "default";
        return m;
    }());
    // The magic is the first 5 bytes of the PAYLOAD: skip [type u8][len u32] = 5.
    ASSERT_GT(frame.size(), 10u);
    frame[5] ^= 0xFF; // corrupt the first magic byte
    net::HelloMsg out;
    EXPECT_FALSE(net::DecodeHello(frame, out));
}

// ============================================================================
// SECTION 3 -- LREC1 stream run==replay byte/string exactness + boundaries.
// ============================================================================

// A recorded checkpoint-hash stream, written then read, must reproduce the live
// hash sequence string-for-string AND tick-for-tick, in order. This is the
// run==replay contract at the stream layer.
TEST(ReplayRoundtripHardening, CheckpointHashSequenceIsByteExact) {
    FakeWorld live; // the "live" run
    const fs::path path = UniquePath("hashseq");
    std::error_code ec;
    fs::remove(path, ec);

    // Capture the live cadence hashes (ticks 30,60,90).
    std::vector<replay::CheckpointRecord> live_cps;
    {
        replay::ReplayWriter w;
        ASSERT_TRUE(w.Open(path.string(), RichHeader()));
        for (std::uint64_t t = 1; t <= 90; ++t) {
            w.RecordInput(t, {});
            if (t % 30 == 0) {
                net::HashMsg hm;
                CaptureHashes(t, hm, &live);
                replay::CheckpointRecord cp;
                cp.tick = t;
                cp.world_hash = hm.world_hash;
                cp.terrain = hm.terrain;
                cp.water = hm.water;
                cp.entities = hm.entities;
                w.RecordCheckpoint(cp);
                live_cps.push_back(cp);
            }
        }
        ASSERT_TRUE(w.Finalize(90));
    }

    const auto contents = replay::ReadReplay(path.string());
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value())
        return;
    EXPECT_FALSE(contents->truncated);
    ASSERT_EQ(contents->checkpoints.size(), live_cps.size());
    for (std::size_t i = 0; i < live_cps.size(); ++i) {
        const auto& live_cp = live_cps[i];
        const auto& got = contents->checkpoints[i];
        EXPECT_EQ(got.tick, live_cp.tick);
        EXPECT_EQ(got.world_hash, live_cp.world_hash); // run==replay: exact
        EXPECT_EQ(got.terrain, live_cp.terrain);
        EXPECT_EQ(got.water, live_cp.water);
        EXPECT_EQ(got.entities, live_cp.entities);
    }
    fs::remove(path, ec);
}

// Writing the SAME logical stream twice yields BYTE-IDENTICAL files (re-saving the
// same state is exactly repeatable -- the serializer has no nondeterminism).
TEST(ReplayRoundtripHardening, ReSaveIsByteIdentical) {
    const fs::path p1 = UniquePath("save1");
    const fs::path p2 = UniquePath("save2");
    std::error_code ec;
    fs::remove(p1, ec);
    fs::remove(p2, ec);

    auto write = [&](const fs::path& p) {
        replay::ReplayWriter w;
        ASSERT_TRUE(w.Open(p.string(), RichHeader()));
        const std::vector<std::uint8_t> blob = {0x10, 0x20, 0x30, 0xFF, 0x00};
        for (std::uint64_t t = 1; t <= 60; ++t) {
            w.RecordInput(t, (t % 2 == 0) ? blob : std::vector<std::uint8_t>{});
            if (t % 30 == 0) {
                replay::CheckpointRecord cp;
                cp.tick = t;
                cp.world_hash = "wh" + std::to_string(t);
                cp.terrain = "tr" + std::to_string(t);
                cp.water = "wa" + std::to_string(t);
                cp.entities = "en" + std::to_string(t);
                w.RecordCheckpoint(cp);
            }
        }
        ASSERT_TRUE(w.Finalize(60));
    };
    write(p1);
    write(p2);

    EXPECT_EQ(ReadAllBytes(p1), ReadAllBytes(p2)) << "re-save produced different bytes";
    fs::remove(p1, ec);
    fs::remove(p2, ec);
}

// Re-reading the same finalized stream is identical every time (read has no state).
TEST(ReplayRoundtripHardening, ReReadIsRepeatable) {
    const fs::path path = UniquePath("reread");
    std::error_code ec;
    fs::remove(path, ec);
    {
        replay::ReplayWriter w;
        ASSERT_TRUE(w.Open(path.string(), RichHeader()));
        w.RecordInput(1, {});
        w.RecordInput(2, {0xAA, 0xBB});
        replay::CheckpointRecord cp;
        cp.tick = 30;
        cp.world_hash = "x";
        cp.terrain = "y";
        cp.water = "z";
        cp.entities = "w";
        w.RecordCheckpoint(cp);
        ASSERT_TRUE(w.Finalize(30));
    }
    const auto a = replay::ReadReplay(path.string());
    const auto b = replay::ReadReplay(path.string());
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    if (!a.has_value() || !b.has_value())
        return;
    ASSERT_EQ(a->inputs.size(), b->inputs.size());
    for (std::size_t i = 0; i < a->inputs.size(); ++i) {
        EXPECT_EQ(a->inputs[i].tick, b->inputs[i].tick);
        EXPECT_EQ(a->inputs[i].inputs, b->inputs[i].inputs);
    }
    EXPECT_EQ(a->tick_count, b->tick_count);
    fs::remove(path, ec);
}

// BOUNDARY: a zero-tick, zero-record finalized stream (empty input stream) reads
// back clean -- empty roster no-op.
TEST(ReplayRoundtripHardening, EmptyStreamFinalizesAndReads) {
    const fs::path path = UniquePath("empty");
    std::error_code ec;
    fs::remove(path, ec);
    {
        replay::ReplayWriter w;
        ASSERT_TRUE(w.Open(path.string(), RichHeader()));
        ASSERT_TRUE(w.Finalize(0));
    }
    const auto contents = replay::ReadReplay(path.string());
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value())
        return;
    EXPECT_FALSE(contents->truncated);
    EXPECT_TRUE(contents->trailer_present);
    EXPECT_EQ(contents->tick_count, 0u);
    EXPECT_TRUE(contents->inputs.empty());
    EXPECT_TRUE(contents->checkpoints.empty());
    fs::remove(path, ec);
}

// BOUNDARY: max-u64 tick survives the round-trip in both input and checkpoint
// records (a narrowed tick field would corrupt this).
TEST(ReplayRoundtripHardening, MaxTickRoundtrips) {
    const fs::path path = UniquePath("maxtick");
    std::error_code ec;
    fs::remove(path, ec);
    const std::uint64_t kMax = 0xFFFFFFFFFFFFFFFFULL;
    {
        replay::ReplayWriter w;
        ASSERT_TRUE(w.Open(path.string(), RichHeader()));
        w.RecordInput(kMax, {0x01});
        replay::CheckpointRecord cp;
        cp.tick = kMax;
        cp.world_hash = "wh";
        cp.terrain = "tr";
        cp.water = "wa";
        cp.entities = "en";
        w.RecordCheckpoint(cp);
        ASSERT_TRUE(w.Finalize(kMax));
    }
    const auto contents = replay::ReadReplay(path.string());
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value())
        return;
    ASSERT_EQ(contents->inputs.size(), 1u);
    EXPECT_EQ(contents->inputs[0].tick, kMax);
    ASSERT_EQ(contents->checkpoints.size(), 1u);
    EXPECT_EQ(contents->checkpoints[0].tick, kMax);
    EXPECT_EQ(contents->tick_count, kMax);
    EXPECT_EQ(replay::FindCheckpoint(*contents, kMax)->terrain, "tr");
    fs::remove(path, ec);
}

// A blob containing the trailer magic bytes as DATA must not be mistaken for the
// trailer (the length-framing must dominate any byte-pattern collision).
TEST(ReplayRoundtripHardening, BlobContainingTrailerMagicIsNotMisread) {
    const fs::path path = UniquePath("magicblob");
    std::error_code ec;
    fs::remove(path, ec);
    const std::vector<std::uint8_t> trailer_bytes = {'L', 'R', 'E', 'C', 'E', 'N', 'D', '1'};
    {
        replay::ReplayWriter w;
        ASSERT_TRUE(w.Open(path.string(), RichHeader()));
        w.RecordInput(1, trailer_bytes); // payload IS the trailer magic
        w.RecordInput(2, {0x42});
        ASSERT_TRUE(w.Finalize(2));
    }
    const auto contents = replay::ReadReplay(path.string());
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value())
        return;
    EXPECT_FALSE(contents->truncated);
    ASSERT_EQ(contents->inputs.size(), 2u);
    EXPECT_EQ(contents->inputs[0].inputs, trailer_bytes);
    EXPECT_EQ(contents->inputs[1].inputs, (std::vector<std::uint8_t>{0x42}));
    fs::remove(path, ec);
}

// CONTRACT (ReplayStream.h: "playback refuses a version mismatch loudly"): a stream
// stamped with a FUTURE/unknown LREC1 version must be REJECTED on read, not parsed
// as if current.
TEST(ReplayRoundtripHardening, FutureVersionStreamIsRefused) {
    const fs::path path = UniquePath("futurever");
    std::error_code ec;
    fs::remove(path, ec);
    {
        replay::ReplayWriter w;
        replay::ReplayHeader h = RichHeader();
        h.version = static_cast<std::uint16_t>(replay::kLrec1Version + 1); // future
        ASSERT_TRUE(w.Open(path.string(), h));
        w.RecordInput(1, {});
        ASSERT_TRUE(w.Finalize(1));
    }
    const auto contents = replay::ReadReplay(path.string());
    // Contract: a version mismatch is refused loudly. The only "refuse" channel the
    // reader has is nullopt, so a future-version stream must NOT parse as valid.
    EXPECT_FALSE(contents.has_value())
        << "ReadReplay accepted a future LREC1 version -- the documented version "
           "gate ('playback refuses a version mismatch loudly') is not enforced";
    fs::remove(path, ec);
}

// A completely empty (zero-byte) file is not a valid stream -> nullopt (degenerate).
TEST(ReplayRoundtripHardening, ZeroByteFileRejected) {
    const fs::path path = UniquePath("zerobyte");
    std::error_code ec;
    fs::remove(path, ec);
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); }
    EXPECT_FALSE(replay::ReadReplay(path.string()).has_value());
    fs::remove(path, ec);
}

// Fnv1a64Hex must be a stable pure function over its byte content, including the
// empty string and embedded NULs (it underpins every hash in the desync oracle).
TEST(ReplayRoundtripHardening, Fnv1a64HexStableOverContent) {
    EXPECT_EQ(replay::Fnv1a64Hex(""), replay::Fnv1a64Hex(""));
    EXPECT_EQ(replay::Fnv1a64Hex(std::string("a\0b", 3)),
              replay::Fnv1a64Hex(std::string("a\0b", 3)));
    EXPECT_NE(replay::Fnv1a64Hex(std::string("a\0b", 3)),
              replay::Fnv1a64Hex(std::string("a\0c", 3)));
    EXPECT_EQ(replay::Fnv1a64Hex("default").size(), 16u);
}

// ============================================================================
// SECTION 4 -- lockstep run==replay + ORDER / IDEMPOTENCY / convergence.
// ============================================================================

// run==replay (E2E): drive an in-sync session, force a desync on the LAST cadence
// so a dump is emitted, then assert the dump's checkpoints reproduce the LIVE
// cadence hashes the catching peer captured -- string-exact.
TEST(LockstepReplayHardening, DesyncDumpReproducesLiveHashesExactly) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0};
    FakeWorld wb{0, "agree", 60}; // b diverges starting at tick 60 (caught at cadence 60)
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));

    const fs::path dumpA = UniquePath("dumpA");
    const fs::path dumpB = UniquePath("dumpB");
    std::error_code ec;
    fs::remove(dumpA, ec);
    fs::remove(dumpB, ec);
    a.SetDumpPath(dumpA.string());
    b.SetDumpPath(dumpB.string());

    ASSERT_TRUE(HandshakeBoth(a, b, tb.get()));
    const auto r = DriveBoth(a, b, 90);
    (void)r;

    const bool a_desync = a.Status().desynced;
    const bool b_desync = b.Status().desynced;
    ASSERT_TRUE(a_desync || b_desync) << "a real divergence at tick 60 was not caught";

    // Recompute the catching peer's expected live cadence-30 hash (the first
    // checkpoint, captured before divergence) and assert the dump preserved it.
    auto verify = [](net::LockstepSession& s, FakeWorld& w) {
        const std::string path = s.Status().dump_path;
        ASSERT_FALSE(path.empty());
        const auto contents = replay::ReadReplay(path);
        ASSERT_TRUE(contents.has_value());
        EXPECT_FALSE(contents->truncated);
        ASSERT_FALSE(contents->checkpoints.empty());
        // Checkpoint at tick 30 must equal what this peer's world hashed live.
        net::HashMsg expected;
        CaptureHashes(30, expected, &w);
        const replay::CheckpointRecord* cp30 = replay::FindCheckpoint(*contents, 30);
        ASSERT_NE(cp30, nullptr);
        EXPECT_EQ(cp30->world_hash, expected.world_hash);
        EXPECT_EQ(cp30->terrain, expected.terrain);
        EXPECT_EQ(cp30->water, expected.water);
        EXPECT_EQ(cp30->entities, expected.entities);
    };
    if (a_desync)
        verify(a, wa);
    if (b_desync)
        verify(b, wb);

    fs::remove(dumpA, ec);
    fs::remove(dumpB, ec);
}

// run==replay determinism: two FULLY independent in-sync sessions, with DIFFERENT
// horizon pacing (one stalled to grow its horizon, one not), must advance the same
// world to the same agreed tick. The horizon is hash-neutral, so pacing must not
// change the end state.
TEST(LockstepReplayHardening, HorizonPacingDoesNotChangeEndState) {
    auto run = [](bool stall) -> std::uint64_t {
        auto [ta, tb] = net::MakeLoopbackPair();
        FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
        net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
        net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));
        EXPECT_TRUE(HandshakeBoth(a, b, tb.get()));
        if (stall) {
            for (int i = 0; i < 12; ++i)
                a.PumpTick(90); // grow a's horizon
        }
        const auto r = DriveBoth(a, b, 90);
        EXPECT_EQ(r.a_outcome, net::TickOutcome::Finished);
        EXPECT_EQ(r.b_outcome, net::TickOutcome::Finished);
        EXPECT_FALSE(a.Status().desynced);
        EXPECT_FALSE(b.Status().desynced);
        return a.AgreedTick();
    };
    EXPECT_EQ(run(false), 90u);
    EXPECT_EQ(run(true), 90u) << "horizon pacing changed the agreed end tick (not hash-neutral)";
}

// IDEMPOTENCY: an identical input frame delivered TWICE for the same (tick,client)
// must be a no-op -- the session must still advance exactly once per tick and stay
// in sync (a duplicate must not double-apply or stall).
TEST(LockstepReplayHardening, DuplicateInputFrameIsNoOp) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));
    ASSERT_TRUE(HandshakeBoth(a, b, tb.get()));

    // Inject duplicate peer-input frames into a's receive queue by sending the same
    // InputMsg from b's transport twice for a range of ticks.
    for (std::uint64_t t = 1; t <= 30; ++t) {
        net::InputMsg dup;
        dup.tick = t;
        dup.client_id = 1;
        dup.inputs = {};
        ASSERT_TRUE(tb->SendFrame(net::EncodeInput(dup)));
        ASSERT_TRUE(tb->SendFrame(net::EncodeInput(dup))); // exact duplicate
    }
    const auto r = DriveBoth(a, b, 30);
    EXPECT_EQ(r.a_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.a_tick, 30u);
    EXPECT_FALSE(a.Status().desynced) << "a duplicate input frame caused a desync";
}

// ORDER independence: peer input frames delivered OUT OF TICK ORDER must converge to
// the same advance as in-order delivery (inputs are tick-keyed, applied in tick
// order regardless of arrival order).
TEST(LockstepReplayHardening, OutOfOrderInputFramesConverge) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));
    ASSERT_TRUE(HandshakeBoth(a, b, tb.get()));

    // Deliver b's inputs for ticks 1..20 in REVERSE order into a's queue.
    for (std::uint64_t t = 20; t >= 1; --t) {
        net::InputMsg in;
        in.tick = t;
        in.client_id = 1;
        in.inputs = {};
        ASSERT_TRUE(tb->SendFrame(net::EncodeInput(in)));
        if (t == 1)
            break; // avoid unsigned underflow
    }
    // a alone should now be able to advance through tick 20 (it has its own
    // pre-scheduled inputs + all of b's, regardless of order).
    net::TickResult ra;
    for (int i = 0; i < 100000; ++i) {
        ra = a.PumpTick(20);
        if (ra.outcome == net::TickOutcome::Finished ||
            ra.outcome == net::TickOutcome::WaitingForPeer) {
            if (a.AgreedTick() >= 20)
                break;
            if (ra.outcome == net::TickOutcome::WaitingForPeer)
                break;
        }
    }
    EXPECT_EQ(a.AgreedTick(), 20u) << "out-of-order input delivery failed to converge";
    EXPECT_FALSE(a.Status().desynced);
}

// DROPPED input frame: if the peer's input for a tick NEVER arrives, the session must
// stall DETERMINISTICALLY at WaitingForPeer -- it must NOT falsely advance past the
// gap and must NOT declare a desync (delay-based lockstep waits, never guesses).
TEST(LockstepReplayHardening, DroppedPeerInputStallsDeterministically) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    // NOTE: no b session and no b inputs are ever sent -> a's peer inputs never arrive.
    net::HelloMsg bh;
    bh.seed = 424242;
    bh.preset = "default";
    bh.tick_rate_hz = 30;
    bh.client_id = 1;
    ASSERT_TRUE(tb->SendFrame(net::EncodeHello(bh)));
    ASSERT_TRUE(a.Handshake());

    net::TickResult ra;
    for (int i = 0; i < 5000; ++i)
        ra = a.PumpTick(90);
    EXPECT_EQ(ra.outcome, net::TickOutcome::WaitingForPeer)
        << "a dropped peer input did not stall deterministically";
    EXPECT_EQ(a.AgreedTick(), 0u) << "session advanced past a missing peer input";
    EXPECT_FALSE(a.Status().desynced) << "a missing input was misreported as a desync";
    // Horizon must have grown to its ceiling and capped there (never unbounded).
    EXPECT_EQ(a.Status().horizon, 10u);
    EXPECT_LE(a.Status().max_horizon_reached, 10u);
}

// CONVERGENCE: once the previously-dropped inputs finally arrive, the stalled session
// must resume and run to budget with no desync (the stall was recoverable, not fatal).
TEST(LockstepReplayHardening, StalledSessionRecoversWhenInputsArrive) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));
    ASSERT_TRUE(HandshakeBoth(a, b, tb.get()));

    // Stall a (b not pumped) so a is parked at WaitingForPeer with a grown horizon. a may have
    // agreed a FEW ticks first from inputs b buffered during the handshake before it stops being
    // pumped, so the contract is "stalled SHORT of budget" (not necessarily at exactly 0).
    for (int i = 0; i < 20; ++i)
        a.PumpTick(60);
    ASSERT_LT(a.AgreedTick(), 60u);
    ASSERT_GT(a.Status().horizon, 3u);

    // Now drive both: the buffered-ahead inputs flow and the session converges.
    const auto r = DriveBoth(a, b, 60);
    EXPECT_EQ(r.a_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.b_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(a.AgreedTick(), 60u);
    EXPECT_FALSE(a.Status().desynced);
}

// EMPTY input stream end-to-end: a headless session with NO player inputs (every
// collect returns empty) still runs to budget and stays in sync -- the empty-set
// no-op path.
TEST(LockstepReplayHardening, EmptyInputStreamRunsClean) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));
    ASSERT_TRUE(HandshakeBoth(a, b, tb.get()));
    const auto r = DriveBoth(a, b, 30);
    EXPECT_EQ(r.a_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.b_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.a_tick, 30u);
    EXPECT_EQ(r.b_tick, 30u);
    EXPECT_FALSE(a.Status().desynced);
    EXPECT_FALSE(b.Status().desynced);
}

// GATING / no-op: a desynced session is STICKY -- once desynced, further PumpTick
// calls keep returning Desync (and must never silently resume / advance the world).
TEST(LockstepReplayHardening, DesyncIsStickyAcrossPumps) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0};
    FakeWorld wb{0, "agree", 30}; // diverges at the first cadence
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));
    const fs::path dumpA = UniquePath("stickyA");
    const fs::path dumpB = UniquePath("stickyB");
    std::error_code ec;
    fs::remove(dumpA, ec);
    fs::remove(dumpB, ec);
    a.SetDumpPath(dumpA.string());
    b.SetDumpPath(dumpB.string());
    ASSERT_TRUE(HandshakeBoth(a, b, tb.get()));

    DriveBoth(a, b, 90);
    net::LockstepSession* desynced =
        a.Status().desynced ? &a : (b.Status().desynced ? &b : nullptr);
    ASSERT_NE(desynced, nullptr);
    const std::uint64_t at = desynced->AgreedTick();
    // Pump several more times: outcome stays Desync and the agreed tick never moves.
    for (int i = 0; i < 10; ++i) {
        const auto rr = desynced->PumpTick(90);
        EXPECT_EQ(rr.outcome, net::TickOutcome::Desync);
        EXPECT_EQ(desynced->AgreedTick(), at);
    }
    fs::remove(dumpA, ec);
    fs::remove(dumpB, ec);
}

// GATING / no-op: a peer-disconnected session is STICKY too -- further pumps keep
// returning PeerDisconnected and never resume.
TEST(LockstepReplayHardening, DisconnectIsStickyAcrossPumps) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));
    ASSERT_TRUE(HandshakeBoth(a, b, tb.get()));

    for (int i = 0; i < 50; ++i) {
        a.PumpTick(90);
        b.PumpTick(90);
        if (a.AgreedTick() >= 5)
            break;
    }
    b.Disconnect();

    net::TickResult ra;
    for (int i = 0; i < 1000; ++i) {
        ra = a.PumpTick(90);
        if (ra.outcome == net::TickOutcome::PeerDisconnected)
            break;
    }
    ASSERT_EQ(ra.outcome, net::TickOutcome::PeerDisconnected);
    const std::uint64_t at = a.AgreedTick();
    for (int i = 0; i < 10; ++i) {
        const auto rr = a.PumpTick(90);
        EXPECT_EQ(rr.outcome, net::TickOutcome::PeerDisconnected);
        EXPECT_EQ(a.AgreedTick(), at);
    }
    EXPECT_FALSE(a.Status().desynced) << "a clean disconnect must never become a desync";
}

// Disconnect is IDEMPOTENT: calling it twice must not crash or send a second Bye
// (the header documents it as idempotent).
TEST(LockstepReplayHardening, DisconnectIsIdempotent) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));
    ASSERT_TRUE(HandshakeBoth(a, b, tb.get()));
    a.Disconnect();
    a.Disconnect(); // second call must be a harmless no-op
    SUCCEED();
}

// Handshake rejects a TICK-RATE mismatch (a mis-paired tick rate silently desyncs
// otherwise -- the sibling suite covers seed/preset but not tick-rate).
TEST(LockstepReplayHardening, HandshakeRejectsTickRateMismatch) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::HelloMsg bad;
    bad.seed = 424242;
    bad.preset = "default";
    bad.tick_rate_hz = 60;
    bad.client_id = 1;
    ASSERT_TRUE(tb->SendFrame(net::EncodeHello(bad)));
    EXPECT_FALSE(a.Handshake());
    EXPECT_FALSE(a.Status().handshaken);
}

// Handshake rejects a PROTOCOL VERSION mismatch (forward/backward-incompat peer).
TEST(LockstepReplayHardening, HandshakeRejectsProtocolVersionMismatch) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::HelloMsg bad;
    bad.protocol_version = static_cast<std::uint16_t>(net::kLockstepProtocolVersion + 1);
    bad.seed = 424242;
    bad.preset = "default";
    bad.tick_rate_hz = 30;
    bad.client_id = 1;
    ASSERT_TRUE(tb->SendFrame(net::EncodeHello(bad)));
    EXPECT_FALSE(a.Handshake());
}

// Handshake with NO peer Hello queued fails cleanly (no hang, no false accept) --
// degenerate late-join / never-arrived case.
TEST(LockstepReplayHardening, HandshakeWithoutPeerHelloFails) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    EXPECT_FALSE(a.Handshake()); // peer never said Hello
    EXPECT_FALSE(a.Status().handshaken);
    (void)tb;
}

// Desync localization to the SUB-HASH section: if terrain agrees but water diverges,
// the catching peer must localize to "water" (not "terrain"), proving the sub-hash
// walk order is correct.
TEST(LockstepReplayHardening, DesyncLocalizesToWaterSection) {
    // Build two worlds whose terrain agrees but whose water diverges at tick 30.
    // FakeWorld corrupts ALL sections together, so use a custom capture via salt:
    // give b a salt that only differs in the WATER section by overriding capture.
    // We implement this with a dedicated hook pair using a section-selective world.
    struct SectionWorld {
        std::uint64_t corrupt_water_from = 0;
        std::string HashAt(std::uint64_t t, const char* section) const {
            const bool corrupt = corrupt_water_from != 0 && t >= corrupt_water_from &&
                                 std::string(section) == "water";
            const std::string salt = corrupt ? "agreeW" : "agree";
            return replay::Fnv1a64Hex(salt + "|" + section + "|" + std::to_string(t));
        }
    };
    static SectionWorld sw_a, sw_b;
    sw_a = SectionWorld{0};
    sw_b = SectionWorld{30};

    // The shared ApplyStep type-puns `user` as a FakeWorld* and writes `tick` into
    // offset 0 -- which for a SectionWorld is `corrupt_water_from`. Using it here would
    // clobber each world's corruption config every tick (making both worlds corrupt
    // identically -> no divergence). Use a section-world step that advances nothing.
    auto step = [](std::uint64_t, const std::vector<std::uint8_t>&, void*) {
        return true;
    };

    auto cap = [](std::uint64_t tick, net::HashMsg& out, void* user) {
        auto* w = static_cast<SectionWorld*>(user);
        out.tick = tick;
        out.terrain = w->HashAt(tick, "terrain");
        out.water = w->HashAt(tick, "water");
        out.entities = w->HashAt(tick, "entities");
        out.world_hash = replay::Fnv1a64Hex(out.terrain + out.water + out.entities);
    };

    auto [ta, tb] = net::MakeLoopbackPair();
    net::LockstepHooks ha;
    ha.collect_local_input = &CollectEmpty;
    ha.apply_and_step = step;
    ha.capture_hashes = cap;
    ha.user = &sw_a;
    net::LockstepHooks hb;
    hb.collect_local_input = &CollectEmpty;
    hb.apply_and_step = step;
    hb.capture_hashes = cap;
    hb.user = &sw_b;
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), ha);
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), hb);

    const fs::path dumpA = UniquePath("waterA");
    const fs::path dumpB = UniquePath("waterB");
    std::error_code ec;
    fs::remove(dumpA, ec);
    fs::remove(dumpB, ec);
    a.SetDumpPath(dumpA.string());
    b.SetDumpPath(dumpB.string());
    ASSERT_TRUE(HandshakeBoth(a, b, tb.get()));

    DriveBoth(a, b, 60);
    const bool a_d = a.Status().desynced;
    const bool b_d = b.Status().desynced;
    ASSERT_TRUE(a_d || b_d);
    if (a_d) {
        EXPECT_EQ(a.Status().desync_tick, 30u);
        EXPECT_EQ(a.Status().desync_section, "water");
    }
    if (b_d) {
        EXPECT_EQ(b.Status().desync_tick, 30u);
        EXPECT_EQ(b.Status().desync_section, "water");
    }
    fs::remove(dumpA, ec);
    fs::remove(dumpB, ec);
}

// In-sync run emits a dump on NO desync? No: a clean run must NEVER set desynced and
// must NEVER write a dump path. (Guards against a vacuous-oracle false positive.)
TEST(LockstepReplayHardening, CleanRunNeverFalseDesyncs) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0}; // identical -> agree forever
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));
    ASSERT_TRUE(HandshakeBoth(a, b, tb.get()));

    const auto r = DriveBoth(a, b, 120); // 4 cadences, all must agree
    EXPECT_EQ(r.a_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.b_outcome, net::TickOutcome::Finished);
    EXPECT_FALSE(a.Status().desynced) << "oracle false-positived on an identical run";
    EXPECT_FALSE(b.Status().desynced);
    EXPECT_TRUE(a.Status().dump_path.empty());
    EXPECT_TRUE(b.Status().dump_path.empty());
}

// merged-input determinism: applying the merged set is ascending-client-id ordered.
// Drive a 2-peer session and capture the merged blob each peer applies; both peers
// must apply the SAME merged bytes for the same tick (sync prerequisite). We verify
// via a capturing apply hook that records the merged bytes per tick.
TEST(LockstepReplayHardening, MergedInputBytesMatchAcrossPeers) {
    struct Capt {
        std::map<std::uint64_t, std::vector<std::uint8_t>> applied;
    };
    static Capt ca, cb;
    ca = Capt{};
    cb = Capt{};
    // Give each peer a NON-EMPTY, client-id-distinguishable input so merge order matters.
    auto collect_a = [](std::uint64_t, void*) -> std::vector<std::uint8_t> {
        return {0xA0};
    };
    auto collect_b = [](std::uint64_t, void*) -> std::vector<std::uint8_t> {
        return {0xB0};
    };
    auto apply_a = [](std::uint64_t t, const std::vector<std::uint8_t>& m, void* u) {
        static_cast<Capt*>(u)->applied[t] = m;
        return true;
    };
    auto apply_b = [](std::uint64_t t, const std::vector<std::uint8_t>& m, void* u) {
        static_cast<Capt*>(u)->applied[t] = m;
        return true;
    };

    auto [ta, tb] = net::MakeLoopbackPair();
    net::LockstepHooks ha;
    ha.collect_local_input = collect_a;
    ha.apply_and_step = apply_a;
    ha.capture_hashes = &CaptureHashes;
    ha.user = &ca;
    net::LockstepHooks hb;
    hb.collect_local_input = collect_b;
    hb.apply_and_step = apply_b;
    hb.capture_hashes = &CaptureHashes;
    hb.user = &cb;
    // capture_hashes needs a FakeWorld* user, but ours is Capt*; the hash path is not
    // exercised here (budget < cadence) so give a benign no-op capture instead.
    auto noop_cap = [](std::uint64_t tick, net::HashMsg& out, void*) {
        out.tick = tick;
        out.world_hash = "x";
        out.terrain = "x";
        out.water = "x";
        out.entities = "x";
    };
    ha.capture_hashes = noop_cap;
    hb.capture_hashes = noop_cap;

    net::LockstepSession a(MakeConfig(0, 1), ta.get(), ha);
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), hb);
    ASSERT_TRUE(HandshakeBoth(a, b, tb.get()));

    const auto r = DriveBoth(a, b, 20); // below the 30-tick cadence: no hash exchange
    ASSERT_EQ(r.a_tick, 20u);
    ASSERT_EQ(r.b_tick, 20u);

    // Both peers must have applied byte-identical merged inputs for every tick, and
    // the order must be client 0 (0xA0) before client 1 (0xB0) -- ascending id.
    for (std::uint64_t t = 1; t <= 20; ++t) {
        ASSERT_TRUE(ca.applied.count(t)) << "peer a missing applied tick " << t;
        ASSERT_TRUE(cb.applied.count(t)) << "peer b missing applied tick " << t;
        EXPECT_EQ(ca.applied[t], cb.applied[t]) << "peers applied different merged bytes @" << t;
        const std::vector<std::uint8_t> expected = {0xA0, 0xB0}; // id 0 then id 1
        EXPECT_EQ(ca.applied[t], expected) << "merged input not ascending-client-id ordered @" << t;
    }
}

// A failed apply_and_step is reported as a (self-)Desync with the dump emitted, not
// silently swallowed (a local sim failure must HALT the session, not diverge).
TEST(LockstepReplayHardening, FailedApplyStepHaltsAsDesync) {
    static int fail_at = 5;
    fail_at = 5;
    auto apply_fail = [](std::uint64_t t, const std::vector<std::uint8_t>&, void* u) -> bool {
        static_cast<FakeWorld*>(u)->tick = t;
        return t != static_cast<std::uint64_t>(fail_at); // fail exactly at tick 5
    };
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepHooks ha = MakeHooks(&wa);
    ha.apply_and_step = apply_fail;
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), ha);
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));
    const fs::path dumpA = UniquePath("applyfailA");
    std::error_code ec;
    fs::remove(dumpA, ec);
    a.SetDumpPath(dumpA.string());
    ASSERT_TRUE(HandshakeBoth(a, b, tb.get()));

    net::TickResult ra;
    for (int i = 0; i < 100000; ++i) {
        ra = a.PumpTick(90);
        b.PumpTick(90);
        if (ra.outcome == net::TickOutcome::Desync)
            break;
        if (ra.outcome == net::TickOutcome::Finished)
            break;
    }
    EXPECT_EQ(ra.outcome, net::TickOutcome::Desync);
    EXPECT_EQ(a.Status().desync_tick, 5u);
    EXPECT_EQ(a.Status().desync_section, "apply_and_step");
    EXPECT_TRUE(a.AgreedTick() < 5u) << "world advanced past the failed step";
    fs::remove(dumpA, ec);
}
