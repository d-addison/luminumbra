//  deterministic network-condition simulator tests (latency / jitter / loss),
// the headless replacement for over-the-wire validation on a single PC.
#include <gtest/gtest.h>

#include <vector>

#include "net/NetworkSim.h"

namespace {

using Luminumbra::Net::FrameDelivery;
using Luminumbra::Net::NetworkConditions;
using Luminumbra::Net::NetworkSim;

std::vector<std::uint8_t> frame(std::uint8_t id) {
    return {id};
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

} // namespace
