// LockstepSession unit tests (src/luminumbra_common/net/LockstepSession.{h,cpp}).
//
// These lock the ENGINE-GENERIC delay-based lockstep behavior over LoopbackTransport
// ONLY (no real sockets/ports -- hermetic, no firewall/CI flakiness): handshake
// accept/reject, in-sync tick exchange, adaptive-horizon absorption of a late input,
// desync detection + LREC1 dump emission, and clean disconnect. Both session ends run in
// this one process, driven turn-by-turn so the loopback queues carry their frames.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/replay/ReplayStream.h"

namespace net = Luminumbra::Net;
namespace replay = Luminumbra::Replay;
namespace fs = std::filesystem;

namespace {

fs::path LockstepTempDir() {
#ifdef LUMINUMBRA_TEST_ARTIFACT_DIR
    fs::path dir = fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "lockstep-unit";
#else
    fs::path dir = fs::temp_directory_path() / "luminumbra-lockstep-unit";
#endif
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// A deterministic per-peer fake "world": the hash is a function of (tick, a per-peer
// salt). With identical salts the two peers agree at every cadence (in sync); a differing
// salt at/after some tick forces a desync the oracle must catch.
struct FakeWorld {
    std::uint64_t tick = 0;
    std::string salt;               // identical across peers => agree
    std::uint64_t corrupt_from = 0; // ticks >= this use a corrupted salt (0 = never)

    std::string HashAt(std::uint64_t t, const char* section) const {
        const std::string s = (corrupt_from != 0 && t >= corrupt_from) ? (salt + "X") : salt;
        return replay::Fnv1a64Hex(s + "|" + section + "|" + std::to_string(t));
    }
};

std::vector<std::uint8_t> CollectEmpty(std::uint64_t, void*) {
    return {}; // headless: no player inputs today (empty input set must work)
}

bool ApplyStep(std::uint64_t tick, const std::vector<std::uint8_t>&, void* user) {
    auto* w = static_cast<FakeWorld*>(user);
    w->tick = tick;
    return true;
}

void CaptureHashes(std::uint64_t tick, net::HashMsg& out, void* user) {
    auto* w = static_cast<FakeWorld*>(user);
    out.tick = tick;
    out.terrain = w->HashAt(tick, "terrain");
    out.water = w->HashAt(tick, "water");
    out.entities = w->HashAt(tick, "entities");
    // The combined world_hash mixes the sub-hashes (the real server folds mesh in too;
    // for the fake world terrain/water/entities suffice to localize divergence).
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

} // namespace

// Both peers handshake successfully on matching params.
TEST(LockstepSessionTest, HandshakeAccepts) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));

    // Both must send Hello before either can complete (single-process driver order):
    // a's Handshake sends a's Hello but cannot yet see b's, so prime b's send first.
    // Drive: send each Hello, then complete each handshake.
    // Easiest: have each send its Hello by attempting Handshake; since the peer Hello is
    // not yet queued, we send b's Hello first via a manual encode, then handshake a, then b.
    // Simpler and faithful: call b.Handshake AFTER a has sent -- but Handshake is atomic.
    // Use the documented pattern: send both Hellos, then handshake. We achieve that by
    // calling each Handshake twice if needed; instead we send Hellos explicitly:
    // (Handshake sends its own Hello, so calling a then b leaves a's Hello waiting for b.)
    // a.Handshake will fail to find b's Hello (not sent yet) -> so we send b's Hello first.
    EXPECT_TRUE(tb->SendFrame(net::EncodeHello([&] {
        net::HelloMsg m;
        m.seed = 424242;
        m.preset = "default";
        m.tick_rate_hz = 30;
        m.client_id = 1;
        return m;
    }())));
    EXPECT_TRUE(a.Handshake());
    // Now a has sent its Hello (into a->b queue); b can handshake against it.
    EXPECT_TRUE(b.Handshake());
    EXPECT_TRUE(a.Status().handshaken);
    EXPECT_TRUE(b.Status().handshaken);
}

// A seed mismatch is rejected loudly (a mis-paired session must not silently desync).
TEST(LockstepSessionTest, HandshakeRejectsSeedMismatch) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));

    // Queue a peer Hello with a DIFFERENT seed.
    net::HelloMsg bad;
    bad.seed = 999;
    bad.preset = "default";
    bad.tick_rate_hz = 30;
    bad.client_id = 1;
    EXPECT_TRUE(tb->SendFrame(net::EncodeHello(bad)));
    EXPECT_FALSE(a.Handshake());
    EXPECT_FALSE(a.Status().handshaken);
}

// A preset mismatch is likewise rejected.
TEST(LockstepSessionTest, HandshakeRejectsPresetMismatch) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::HelloMsg bad;
    bad.seed = 424242;
    bad.preset = "mountains";
    bad.tick_rate_hz = 30;
    bad.client_id = 1;
    EXPECT_TRUE(tb->SendFrame(net::EncodeHello(bad)));
    EXPECT_FALSE(a.Handshake());
}

// Drives two in-sync sessions to N ticks over the loopback pair, alternating pumps so the
// queues carry frames. Returns the final outcomes.
namespace {
struct DriveResult {
    net::TickOutcome a_outcome;
    net::TickOutcome b_outcome;
    std::uint64_t a_tick;
    std::uint64_t b_tick;
};

DriveResult DriveBoth(net::LockstepSession& a,
                      net::LockstepSession& b,
                      std::uint64_t budget,
                      int max_pumps = 100000) {
    net::TickResult ra, rb;
    ra.outcome = net::TickOutcome::WaitingForPeer;
    rb.outcome = net::TickOutcome::WaitingForPeer;
    int pumps = 0;
    auto finished = [](net::TickOutcome o) {
        return o == net::TickOutcome::Finished;
    };
    auto fatal = [](net::TickOutcome o) {
        return o == net::TickOutcome::Desync || o == net::TickOutcome::PeerDisconnected;
    };
    while (pumps++ < max_pumps) {
        ra = a.PumpTick(budget);
        rb = b.PumpTick(budget);
        // A desync/disconnect on EITHER side is terminal for the whole session (the other
        // side will stall waiting for inputs that never come -- stop driving immediately).
        if (fatal(ra.outcome) || fatal(rb.outcome))
            break;
        if (finished(ra.outcome) && finished(rb.outcome))
            break;
    }
    return {ra.outcome, rb.outcome, a.AgreedTick(), b.AgreedTick()};
}
} // namespace

// Two in-sync peers run 90 ticks, exchange hashes every 30, and BOTH finish at the same
// agreed tick with no desync.
TEST(LockstepSessionTest, InSyncTickExchange) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));

    // Handshake (send b's Hello, a handshakes, then b handshakes against a's Hello).
    net::HelloMsg bh;
    bh.seed = 424242;
    bh.preset = "default";
    bh.tick_rate_hz = 30;
    bh.client_id = 1;
    ASSERT_TRUE(tb->SendFrame(net::EncodeHello(bh)));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());

    const auto r = DriveBoth(a, b, 90);
    EXPECT_EQ(r.a_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.b_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.a_tick, 90u);
    EXPECT_EQ(r.b_tick, 90u);
    EXPECT_FALSE(a.Status().desynced);
    EXPECT_FALSE(b.Status().desynced);
}

// The adaptive horizon ABSORBS a delayed peer input: peer A withholds its inputs for a
// burst of ticks, then releases them. The session must stay in sync (no desync), the
// horizon must have grown, and both must still finish at the same tick.
TEST(LockstepSessionTest, HorizonAbsorbsLateInput) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));

    net::HelloMsg bh;
    bh.seed = 424242;
    bh.preset = "default";
    bh.tick_rate_hz = 30;
    bh.client_id = 1;
    ASSERT_TRUE(tb->SendFrame(net::EncodeHello(bh)));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());

    // Stall b: pump ONLY a for several rounds. a will WaitForPeer (b's inputs absent) and
    // grow its horizon. Then resume both -- the buffered-ahead inputs absorb the jitter.
    for (int i = 0; i < 15; ++i) {
        a.PumpTick(90); // b's input for the wanted tick is missing -> a grows horizon
    }
    EXPECT_GT(a.Status().horizon, 3u) << "horizon did not grow on a late peer";
    EXPECT_GT(a.Status().late_input_events, 0u);

    const auto r = DriveBoth(a, b, 90);
    EXPECT_EQ(r.a_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.b_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.a_tick, 90u);
    EXPECT_EQ(r.b_tick, 90u);
    EXPECT_FALSE(a.Status().desynced) << "horizon failed to absorb the jitter (false desync)";
    EXPECT_FALSE(b.Status().desynced);
    EXPECT_GE(a.Status().max_horizon_reached, 4u);
}

// A real state divergence is CAUGHT by the oracle: peer B's world corrupts from tick 30,
// so at the first cadence (tick 30) the hashes mismatch. The session must HALT with a
// desync localized to tick 30 (terrain section) and emit an LREC1 dump that re-reads.
TEST(LockstepSessionTest, DesyncDetectedAndDumpEmitted) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0};
    FakeWorld wb{0, "agree", 30}; // b diverges from tick 30
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));

    const std::string dumpA = (LockstepTempDir() / "desync-a.lrec1").string();
    const std::string dumpB = (LockstepTempDir() / "desync-b.lrec1").string();
    std::error_code ec;
    fs::remove(dumpA, ec);
    fs::remove(dumpB, ec);
    a.SetDumpPath(dumpA);
    b.SetDumpPath(dumpB);

    net::HelloMsg bh;
    bh.seed = 424242;
    bh.preset = "default";
    bh.tick_rate_hz = 30;
    bh.client_id = 1;
    ASSERT_TRUE(tb->SendFrame(net::EncodeHello(bh)));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());

    const auto r = DriveBoth(a, b, 90);
    // At least one side must report the desync (the one that holds both hashes at tick 30).
    const bool a_desync = a.Status().desynced;
    const bool b_desync = b.Status().desynced;
    EXPECT_TRUE(a_desync || b_desync) << "oracle is vacuous: a real divergence was not caught";

    // The catching side localizes to tick 30, section terrain, and wrote a dump.
    if (a_desync) {
        EXPECT_EQ(a.Status().desync_tick, 30u);
        EXPECT_EQ(a.Status().desync_section, "terrain");
        ASSERT_FALSE(a.Status().dump_path.empty());
        const auto contents = replay::ReadReplay(a.Status().dump_path);
        ASSERT_TRUE(contents.has_value());
        if (!contents.has_value())
            return;
        EXPECT_FALSE(contents->truncated);
        EXPECT_FALSE(contents->checkpoints.empty());
    }
    if (b_desync) {
        EXPECT_EQ(b.Status().desync_tick, 30u);
        EXPECT_EQ(b.Status().desync_section, "terrain");
        ASSERT_FALSE(b.Status().dump_path.empty());
        const auto contents = replay::ReadReplay(b.Status().dump_path);
        ASSERT_TRUE(contents.has_value());
        if (!contents.has_value())
            return;
        EXPECT_FALSE(contents->truncated);
    }

    std::error_code rmec;
    fs::remove(dumpA, rmec);
    fs::remove(dumpB, rmec);
}

// A clean Bye from one peer ENDS the session cleanly on the other (regression review): the
// surviving side reports PeerDisconnected with the disconnect tick -- NOT a desync, NOT a
// hang.
TEST(LockstepSessionTest, CleanDisconnect) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));

    net::HelloMsg bh;
    bh.seed = 424242;
    bh.preset = "default";
    bh.tick_rate_hz = 30;
    bh.client_id = 1;
    ASSERT_TRUE(tb->SendFrame(net::EncodeHello(bh)));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());

    // Run a handful of ticks in sync, then b disconnects cleanly.
    for (int i = 0; i < 50; ++i) {
        a.PumpTick(90);
        b.PumpTick(90);
        if (a.AgreedTick() >= 10)
            break;
    }
    b.Disconnect();

    // a keeps pumping: it must observe the clean disconnect and stop (no hang).
    net::TickResult ra;
    for (int i = 0; i < 1000; ++i) {
        ra = a.PumpTick(90);
        if (ra.outcome == net::TickOutcome::PeerDisconnected)
            break;
    }
    EXPECT_EQ(ra.outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_TRUE(a.Status().peer_disconnected);
    EXPECT_FALSE(a.Status().desynced) << "a clean disconnect must NOT be reported as a desync";
}
