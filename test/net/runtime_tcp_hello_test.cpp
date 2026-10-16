#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "luminumbra_server/modes/ModeHelpers.h"

namespace {

using Luminumbra::Net::EncodeHello;
using Luminumbra::Net::HelloMsg;
using Luminumbra::Net::TcpListener;
using Luminumbra::Net::TcpTransport;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Only socket delivery is asynchronous. Handshake time stays fixed at the
// caller's injected value, so no test waits for the real Hello timeout.
template<typename Predicate>
bool WaitUntil(Predicate ready) {
    const auto deadline = Clock::now() + 5s;
    while (Clock::now() < deadline) {
        if (ready()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

class RuntimeTcpHelloTest : public ::testing::Test {
protected:
    void SetUp() override {
        TcpListener listener;
        ASSERT_TRUE(listener.Listen(/*port=*/0));
        slot.port = listener.port();
        ASSERT_NE(slot.port, 0);
        slot.client_id = 7;
        expected.client_id = slot.client_id;
        expected.system_config_identity = "runtime-test-config";
        expected.preset_content_hash = "runtime-test-preset";
        expected.simulation_content_identity = "runtime-test-content";
        ASSERT_TRUE(peer.Connect("127.0.0.1", slot.port, /*timeout_ms=*/1000));
        slot.transport = listener.AcceptOneBlocking(/*timeout_ms=*/1000);
        ASSERT_NE(slot.transport, nullptr);
        listener.Close();

        // Feed an already completed real loopback accept through the same
        // future transition as StartRuntimeTcpAccept, without a fixed port.
        std::promise<bool> accepted;
        slot.accept_result = accepted.get_future();
        slot.accepting = true;
        accepted.set_value(true);
        ASSERT_FALSE(PollRuntimeTcpAccept(slot, expected, accepted_at));
        ASSERT_TRUE(slot.awaiting_hello);
        ASSERT_FALSE(slot.accepting);
        ASSERT_EQ(slot.hello_deadline, accepted_at + RuntimeTcpClientSlot::kHelloTimeout);
    }

    void TearDown() override {
        CloseRuntimeTcpSlot(slot);
        peer.Close();
    }

    void ExpectReleased() {
        EXPECT_EQ(slot.transport, nullptr);
        EXPECT_FALSE(slot.awaiting_hello);
        EXPECT_FALSE(slot.accepting);
        EXPECT_FALSE(slot.accepted);
        EXPECT_FALSE(slot.connected);
    }

    RuntimeTcpClientSlot slot;
    TcpTransport peer;
    HelloMsg expected;
    const Clock::time_point accepted_at{100s};
};

TEST_F(RuntimeTcpHelloTest, SilentPeerExpiresAndFreesSlot) {
    EXPECT_FALSE(PollRuntimeTcpAccept(slot, expected, slot.hello_deadline - 1ms));
    ASSERT_NE(slot.transport, nullptr);
    EXPECT_TRUE(slot.transport->IsPeerConnected());
    EXPECT_TRUE(slot.awaiting_hello);

    EXPECT_FALSE(PollRuntimeTcpAccept(slot, expected, slot.hello_deadline));
    ExpectReleased();
    ASSERT_TRUE(WaitUntil([&]() {
        std::vector<std::uint8_t> frame;
        (void)peer.TryReceiveFrame(frame);
        return !peer.IsPeerConnected();
    })) << "expired peer must observe the transport closing";
}

TEST_F(RuntimeTcpHelloTest, HelloBeforeDeadlineIsAcceptedAndRetainsFollowingFrame) {
    ASSERT_TRUE(peer.SendFrame(EncodeHello(expected)));
    const std::vector<std::uint8_t> following_frame{1, 2, 3};
    ASSERT_TRUE(peer.SendFrame(following_frame));
    ASSERT_TRUE(WaitUntil(
        [&]() { return PollRuntimeTcpAccept(slot, expected, slot.hello_deadline - 1ms); }));
    EXPECT_TRUE(slot.accepted);
    EXPECT_TRUE(slot.connected);
    EXPECT_FALSE(slot.awaiting_hello);
    ASSERT_NE(slot.transport, nullptr);

    EXPECT_FALSE(PollRuntimeTcpAccept(slot, expected, slot.hello_deadline + 1s));
    EXPECT_TRUE(slot.transport->IsPeerConnected());
    std::vector<std::uint8_t> received;
    ASSERT_TRUE(WaitUntil([&]() { return slot.transport->TryReceiveFrame(received); }));
    EXPECT_EQ(received, following_frame);
}

TEST_F(RuntimeTcpHelloTest, SlotAcceptsNewConnectionAfterExpiryWithFreshDeadline) {
    const auto expired_at = slot.hello_deadline;
    ASSERT_FALSE(PollRuntimeTcpAccept(slot, expected, expired_at));
    ExpectReleased();
    peer.Close();

    const auto reaccepted_at = expired_at + 1s;
    ASSERT_FALSE(PollRuntimeTcpAccept(slot, expected, reaccepted_at));
    EXPECT_TRUE(slot.accepting);
    EXPECT_TRUE(slot.accept_result.valid());
    TcpTransport replacement;
    auto connected = std::async(std::launch::async, [&replacement, port = slot.port]() {
        return WaitUntil(
            [&]() { return replacement.Connect("127.0.0.1", port, /*timeout_ms=*/100); });
    });
    // Keep polling to re-arm any 250 ms listen attempt that times out while
    // the connector retries. Only the connector thread touches replacement.
    ASSERT_TRUE(WaitUntil([&]() {
        (void)PollRuntimeTcpAccept(slot, expected, reaccepted_at);
        return slot.awaiting_hello;
    }));
    ASSERT_TRUE(connected.get());
    EXPECT_EQ(slot.hello_deadline, reaccepted_at + RuntimeTcpClientSlot::kHelloTimeout);
    ASSERT_TRUE(replacement.SendFrame(EncodeHello(expected)));
    ASSERT_TRUE(WaitUntil(
        [&]() { return PollRuntimeTcpAccept(slot, expected, slot.hello_deadline - 1ms); }));
    EXPECT_TRUE(slot.accepted);
    EXPECT_TRUE(slot.connected);
    EXPECT_FALSE(slot.awaiting_hello);
}

TEST_F(RuntimeTcpHelloTest, DisconnectBeforeDeadlineReleasesSlot) {
    peer.Close();
    ASSERT_TRUE(WaitUntil([&]() {
        (void)PollRuntimeTcpAccept(slot, expected, accepted_at + 1s);
        return slot.transport == nullptr;
    }));
    ExpectReleased();
}

TEST_F(RuntimeTcpHelloTest, HelloAtDeadlineCannotReviveExpiredSlot) {
    ASSERT_TRUE(peer.SendFrame(EncodeHello(expected)));
    EXPECT_FALSE(PollRuntimeTcpAccept(slot, expected, slot.hello_deadline));
    ExpectReleased();
}

TEST_F(RuntimeTcpHelloTest, MismatchedIdentityBeforeDeadlineStillReleasesSlot) {
    auto hello = expected;
    hello.preset_content_hash = "incompatible";
    ASSERT_TRUE(peer.SendFrame(EncodeHello(hello)));
    ASSERT_TRUE(WaitUntil([&]() {
        (void)PollRuntimeTcpAccept(slot, expected, accepted_at + 1s);
        return slot.transport == nullptr;
    }));
    ExpectReleased();
}

TEST_F(RuntimeTcpHelloTest, WrongClientIdBeforeDeadlineStillReleasesSlot) {
    auto hello = expected;
    hello.client_id += 1;
    ASSERT_TRUE(peer.SendFrame(EncodeHello(hello)));
    ASSERT_TRUE(WaitUntil([&]() {
        (void)PollRuntimeTcpAccept(slot, expected, accepted_at + 1s);
        return slot.transport == nullptr;
    }));
    ExpectReleased();
}

TEST_F(RuntimeTcpHelloTest, MismatchedSimulationContentReleasesSlot) {
    auto hello = expected;
    hello.simulation_content_identity = "different-content";
    ASSERT_TRUE(peer.SendFrame(EncodeHello(hello)));
    ASSERT_TRUE(WaitUntil([&]() {
        (void)PollRuntimeTcpAccept(slot, expected, accepted_at + 1s);
        return slot.transport == nullptr;
    }));
    ExpectReleased();
}

// The same transport-independent admission is used by TCP, GNS and Steam hosts.
TEST(RuntimeHelloAdmissionTest, ContentBindingAndFollowingFramesAcrossTransports) {
    using namespace Luminumbra::Net;
    for (const auto* variant :
         {"matching", "different", "missing peer", "missing local", "old version"}) {
        SCOPED_TRACE(variant);
        const std::string name = variant;
        auto [host, peer] = MakeLoopbackPair();
        HelloMsg expected;
        expected.client_id = 7;
        expected.system_config_identity = "config";
        expected.preset_content_hash = "preset";
        expected.simulation_content_identity = std::string(40, '\0');
        auto hello = expected;
        if (name == "different")
            hello.simulation_content_identity.back() = '\1';
        if (name == "missing peer")
            hello.simulation_content_identity.clear();
        if (name == "missing local")
            expected.simulation_content_identity.clear();
        if (name == "old version")
            hello.protocol_version = 3;
        EXPECT_EQ(PollRuntimeHello(*host, expected, 7), RuntimeHelloAdmission::Pending);
        ASSERT_TRUE(peer->SendFrame(EncodeHello(hello)));
        const std::vector<std::uint8_t> following{9, 8, 7};
        ASSERT_TRUE(peer->SendFrame(following));
        const auto admission = PollRuntimeHello(*host, expected, 7);
        if (name == "matching") {
            EXPECT_EQ(admission, RuntimeHelloAdmission::Accepted);
            std::vector<std::uint8_t> received;
            ASSERT_TRUE(host->TryReceiveFrame(received));
            EXPECT_EQ(received, following);
        } else {
            EXPECT_EQ(admission, RuntimeHelloAdmission::Refused);
            EXPECT_FALSE(peer->IsPeerConnected());
        }
    }
}

} // namespace
