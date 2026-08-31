//   (track ): outbound backpressure unit tests for the lockstep/
// replication TCP send path. These lock the bounded-queue + high-water backpressure
// behaviour that REPLACED the WSAEWOULDBLOCK busy-spin in TcpTransport::SendFrame
// (src/luminumbra_common/net/LockstepSession.cpp). Like the rest of the net tests they
// are hermetic (no real sockets/ports/firewall): they drive OutboundByteQueue directly,
// exercising the exact drain contract the real socket send_some obeys -- a non-blocking
// sink returning bytes-accepted, 0 on would-block, -1 on fatal.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "luminumbra_common/net/LockstepSession.h"

namespace net = Luminumbra::Net;

namespace {
std::vector<std::uint8_t> MakeFrame(std::uint8_t tag, std::size_t n) {
    return std::vector<std::uint8_t>(n, tag);
}
} // namespace

// A would-block sink (kernel send buffer full) must NOT busy-spin: DrainOnce calls the
// sink EXACTLY ONCE, reports would-block (0), and loses NO bytes. The old code did
// `continue;` on WSAEWOULDBLOCK with zero progress -> an infinite spin; against this
// sink that old shape would never return and HANG this test.
TEST(OutboundBackpressure, WouldBlockSinkDoesNotSpinAndKeepsAllBytes) {
    net::OutboundByteQueue q;
    const auto a = MakeFrame(0xAA, 16);
    const auto b = MakeFrame(0xBB, 32);
    q.Append(a.data(), a.size());
    q.Append(b.data(), b.size());
    const std::size_t before = q.PendingBytes();
    ASSERT_EQ(before, a.size() + b.size());

    int calls = 0;
    const int r = q.DrainOnce([&](const std::uint8_t*, std::size_t) -> int {
        ++calls;
        return 0; // would-block
    });
    EXPECT_EQ(r, 0);                     // would-block reported (not fatal)
    EXPECT_EQ(calls, 1);                 // EXACTLY ONE call -> the busy-spin is gone
    EXPECT_EQ(q.PendingBytes(), before); // NO message loss
    EXPECT_FALSE(q.Empty());
}

// Across a full-then-drained sink, every byte of every frame is delivered in order:
// nothing is dropped while the sink is saturated.
TEST(OutboundBackpressure, NoLossAcrossFullThenDrainedSink) {
    net::OutboundByteQueue q;
    const std::vector<std::vector<std::uint8_t>> frames = {
        MakeFrame(0x01, 8),
        MakeFrame(0x02, 5),
        MakeFrame(0x03, 20),
        MakeFrame(0x04, 1),
        MakeFrame(0x05, 13),
    };
    std::vector<std::uint8_t> expected;
    for (const auto& f : frames) {
        q.Append(f.data(), f.size());
        expected.insert(expected.end(), f.begin(), f.end());
    }

    // Sink full -> nothing leaves, nothing lost.
    EXPECT_EQ(q.DrainOnce([](const std::uint8_t*, std::size_t) { return 0; }), 0);
    EXPECT_EQ(q.PendingBytes(), expected.size());

    // Sink accepts everything -> all bytes delivered in order, queue empties.
    std::vector<std::uint8_t> got;
    const int r = q.DrainOnce([&](const std::uint8_t* p, std::size_t n) -> int {
        got.insert(got.end(), p, p + n);
        return static_cast<int>(n);
    });
    EXPECT_EQ(r, 1); // fully drained
    EXPECT_TRUE(q.Empty());
    EXPECT_EQ(got, expected); // byte-exact, in order: zero loss
}

// A sink that accepts a partial amount and then would-blocks must make progress and then
// STOP (no spin), retaining exactly the unsent remainder.
TEST(OutboundBackpressure, PartialAcceptThenWouldBlockRetainsRemainder) {
    net::OutboundByteQueue q;
    const auto f = MakeFrame(0x7E, 100);
    q.Append(f.data(), f.size());

    int calls = 0;
    const int r = q.DrainOnce([&](const std::uint8_t*, std::size_t) -> int {
        ++calls;
        return (calls == 1) ? 30 : 0; // accept 30, then would-block
    });
    EXPECT_EQ(r, 0);                            // would-block
    EXPECT_EQ(calls, 2);                        // one progress call + one would-block call
    EXPECT_EQ(q.PendingBytes(), f.size() - 30); // remainder retained, none lost
}

// The high-water mark trips once the queue grows past it, but messages are STILL not
// dropped -- high-water only signals "apply backpressure"; every byte is retained.
TEST(OutboundBackpressure, HighWaterTripsWithoutDropping) {
    net::OutboundByteQueue q;
    q.high_water = 64;
    EXPECT_FALSE(q.OverHighWater());
    const auto f = MakeFrame(0x5A, 100);
    q.Append(f.data(), f.size());
    EXPECT_TRUE(q.OverHighWater());
    EXPECT_EQ(q.PendingBytes(), f.size()); // nothing dropped at/over high-water
}

// Append compaction: draining the consumed prefix then appending more must keep only the
// live (unsent) bytes and still deliver everything in order across drains -- no loss, no
// unbounded growth of already-sent bytes.
TEST(OutboundBackpressure, CompactsConsumedPrefixWithoutLoss) {
    net::OutboundByteQueue q;
    const auto a = MakeFrame(0x11, 10);
    q.Append(a.data(), a.size());

    // Drain only 4 bytes (partial), then would-block.
    int calls = 0;
    q.DrainOnce([&](const std::uint8_t*, std::size_t) -> int {
        ++calls;
        return (calls == 1) ? 4 : 0;
    });
    EXPECT_EQ(q.PendingBytes(), a.size() - 4);

    // Append a second frame; the consumed 4-byte prefix is compacted away, leaving the
    // 6 live bytes of `a` + all of `b`.
    const auto b = MakeFrame(0x22, 7);
    q.Append(b.data(), b.size());
    EXPECT_EQ(q.PendingBytes(), (a.size() - 4) + b.size());

    std::vector<std::uint8_t> got;
    const int r = q.DrainOnce([&](const std::uint8_t* p, std::size_t n) -> int {
        got.insert(got.end(), p, p + n);
        return static_cast<int>(n);
    });
    EXPECT_EQ(r, 1);
    EXPECT_TRUE(q.Empty());
    std::vector<std::uint8_t> expected(a.begin() + 4, a.end());
    expected.insert(expected.end(), b.begin(), b.end());
    EXPECT_EQ(got, expected);
}
