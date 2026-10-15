//  deterministic network-condition simulator tests (latency / jitter / loss),
// the headless replacement for over-the-wire validation on a single PC.
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "net/NetworkSim.h"

namespace {

using Luminumbra::Net::FrameDelivery;
using Luminumbra::Net::kMaxFramePayloadBytes;
using Luminumbra::Net::NetworkConditions;
using Luminumbra::Net::NetworkSim;

std::vector<std::uint8_t> frame(std::uint8_t id) {
    return {id};
}

TEST(NetworkSim, AcceptsMaximumAndRejectsMaximumPlusOne) {
    NetworkSim sim(NetworkConditions{});
    for (const auto delivery : {FrameDelivery::Reliable, FrameDelivery::Unreliable}) {
        std::vector<std::uint8_t> payload(Luminumbra::Net::kMaxFramePayloadBytes + 1, 0xAB);
        EXPECT_FALSE(sim.A().SendFrame(payload, delivery));
        std::vector<std::uint8_t> out{0xA5};
        EXPECT_FALSE(sim.B().TryReceiveFrame(out));
        EXPECT_EQ(out, (std::vector<std::uint8_t>{0xA5}));
        EXPECT_TRUE(sim.A().IsPeerConnected());
        payload.resize(Luminumbra::Net::kMaxFramePayloadBytes);
        ASSERT_TRUE(sim.A().SendFrame(payload, delivery));
        ASSERT_TRUE(sim.B().TryReceiveFrame(payload));
        ASSERT_EQ(payload.size(), Luminumbra::Net::kMaxFramePayloadBytes);
        EXPECT_TRUE(std::all_of(
            payload.begin(), payload.end(), [](std::uint8_t byte) { return byte == 0xAB; }));
        EXPECT_FALSE(sim.B().TryReceiveFrame(payload));
    }
}

// A frame sent under N-tick latency is not delivered until N ticks pass.
TEST(NetworkSim, LatencyDelaysDelivery) {
    NetworkSim sim(NetworkConditions{/*latency*/ 3, 0, 0, 1});
    sim.A().SendFrame(frame(7));
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(sim.B().TryReceiveFrame(out)); // tick 0
    sim.Tick();
    EXPECT_FALSE(sim.B().TryReceiveFrame(out)); // tick 1
    sim.Tick();
    EXPECT_FALSE(sim.B().TryReceiveFrame(out)); // tick 2
    sim.Tick();                                 // tick 3
    ASSERT_TRUE(sim.B().TryReceiveFrame(out));
    EXPECT_EQ(out, frame(7));
}

// Reliable frames are never dropped, even under a loss regime.
TEST(NetworkSim, ReliableNeverDropped) {
    NetworkSim sim(NetworkConditions{0, 0, /*drop_one_in*/ 2, 1});
    for (int i = 0; i < 10; ++i)
        sim.A().SendFrame(frame(static_cast<std::uint8_t>(i)), FrameDelivery::Reliable);
    int got = 0;
    std::vector<std::uint8_t> out;
    while (sim.B().TryReceiveFrame(out))
        ++got;
    EXPECT_EQ(got, 10);
}

// Unreliable frames drop under loss — deterministically (same survivors every run).
TEST(NetworkSim, UnreliableDropsSomeDeterministically) {
    auto run = [] {
        NetworkSim sim(NetworkConditions{0, 0, /*drop_one_in*/ 3, 1});
        for (int i = 0; i < 30; ++i)
            sim.A().SendFrame(frame(static_cast<std::uint8_t>(i)), FrameDelivery::Unreliable);
        std::vector<std::uint8_t> survived, out;
        while (sim.B().TryReceiveFrame(out))
            survived.push_back(out[0]);
        return survived;
    };
    const std::vector<std::uint8_t> a = run();
    const std::vector<std::uint8_t> b = run();
    EXPECT_LT(a.size(), 30u); // some dropped
    EXPECT_GT(a.size(), 0u);
    EXPECT_EQ(a, b); // deterministic
}

// Jitter reorders frames but the whole session is still reproducible (run==replay).
TEST(NetworkSim, DeterministicUnderJitter) {
    auto run = [] {
        NetworkSim sim(NetworkConditions{/*latency*/ 2, /*jitter*/ 2, 0, 42});
        std::vector<std::uint8_t> order, out;
        for (int t = 0; t < 8; ++t) {
            sim.A().SendFrame(frame(static_cast<std::uint8_t>(t)));
            sim.Tick();
        }
        for (int t = 0; t < 8; ++t) {
            while (sim.B().TryReceiveFrame(out))
                order.push_back(out[0]);
            sim.Tick();
        }
        return order;
    };
    const std::vector<std::uint8_t> a = run();
    EXPECT_EQ(a.size(), 8u); // all reliable frames eventually delivered
    EXPECT_EQ(a, run());     // identical despite jitter reorder
}

TEST(NetworkSim, PeerCloseObserved) {
    NetworkSim sim(NetworkConditions{});
    EXPECT_TRUE(sim.A().IsPeerConnected());
    sim.B().Close();
    EXPECT_FALSE(sim.A().IsPeerConnected());
    EXPECT_FALSE(sim.A().SendFrame(frame(1)));
}

TEST(NetworkSim, RejectedOversizedSendAndClosePreserveQueuedFrames) {
    NetworkSim sim(NetworkConditions{});
    const std::vector<std::vector<std::uint8_t>> frames{frame(1), {}, frame(2)};
    for (const auto& payload : frames)
        ASSERT_TRUE(sim.A().SendFrame(payload));
    const std::vector<std::uint8_t> oversized(kMaxFramePayloadBytes + 1, 0xAB);
    EXPECT_FALSE(sim.A().SendFrame(oversized));
    sim.A().Close();
    EXPECT_TRUE(sim.B().IsPeerConnected());
    EXPECT_FALSE(sim.A().SendFrame(frame(3)));
    EXPECT_FALSE(sim.B().SendFrame(frame(3)));
    std::vector<std::uint8_t> received{0xA5};
    for (const auto& payload : frames) {
        EXPECT_TRUE(sim.B().IsPeerConnected());
        ASSERT_TRUE(sim.B().TryReceiveFrame(received));
        EXPECT_EQ(received, payload);
    }
    EXPECT_FALSE(sim.B().IsPeerConnected());
    const auto last = received;
    EXPECT_FALSE(sim.B().TryReceiveFrame(received));
    EXPECT_EQ(received, last);
    EXPECT_FALSE(sim.A().SendFrame(frame(3)));
    EXPECT_FALSE(sim.B().SendFrame(frame(3)));
}

TEST(NetworkSim, CloseWaitsForInboundDrainInBothDirections) {
    for (const bool send_from_a : {false, true}) {
        SCOPED_TRACE(send_from_a);
        NetworkSim sim(NetworkConditions{});
        auto& sender = send_from_a ? sim.A() : sim.B();
        auto& receiver = send_from_a ? sim.B() : sim.A();
        const std::vector<std::vector<std::uint8_t>> frames{frame(1), {}, frame(2)};
        for (const auto& payload : frames)
            ASSERT_TRUE(sender.SendFrame(payload));
        sender.Close();
        EXPECT_FALSE(sender.SendFrame(frame(3)));
        EXPECT_FALSE(receiver.SendFrame(frame(3)));
        std::vector<std::uint8_t> received;
        for (const auto& payload : frames) {
            EXPECT_TRUE(receiver.IsPeerConnected());
            ASSERT_TRUE(receiver.TryReceiveFrame(received));
            EXPECT_EQ(received, payload);
        }
        EXPECT_FALSE(receiver.IsPeerConnected());
        EXPECT_FALSE(receiver.TryReceiveFrame(received));
    }
}

TEST(NetworkSim, CloseWaitsForDelayedInboundDrainInBothDirections) {
    for (const bool send_from_a : {false, true}) {
        SCOPED_TRACE(send_from_a);
        NetworkSim sim(NetworkConditions{3, 0, 0, 1});
        auto& sender = send_from_a ? sim.A() : sim.B();
        auto& receiver = send_from_a ? sim.B() : sim.A();
        ASSERT_TRUE(sender.SendFrame(frame(1)));
        sim.Tick();
        ASSERT_TRUE(sender.SendFrame({}));
        ASSERT_TRUE(sender.SendFrame(frame(2)));
        sender.Close();
        EXPECT_FALSE(sender.SendFrame(frame(3)));
        EXPECT_FALSE(receiver.SendFrame(frame(3)));
        std::vector<std::uint8_t> received{0xA5};
        for (int tick = 1; tick < 3; ++tick) {
            EXPECT_TRUE(receiver.IsPeerConnected());
            EXPECT_FALSE(receiver.TryReceiveFrame(received));
            EXPECT_EQ(received, frame(0xA5));
            sim.Tick();
        }
        EXPECT_TRUE(receiver.IsPeerConnected());
        ASSERT_TRUE(receiver.TryReceiveFrame(received));
        EXPECT_EQ(received, frame(1));
        // No frame is ready, but the later frames must still precede closure.
        EXPECT_TRUE(receiver.IsPeerConnected());
        EXPECT_FALSE(receiver.TryReceiveFrame(received));
        EXPECT_EQ(received, frame(1));
        sim.Tick();
        EXPECT_TRUE(receiver.IsPeerConnected());
        ASSERT_TRUE(receiver.TryReceiveFrame(received));
        EXPECT_TRUE(received.empty());
        EXPECT_TRUE(receiver.IsPeerConnected());
        ASSERT_TRUE(receiver.TryReceiveFrame(received));
        EXPECT_EQ(received, frame(2));
        EXPECT_FALSE(receiver.IsPeerConnected());
        EXPECT_FALSE(receiver.TryReceiveFrame(received));
    }
}

} // namespace
