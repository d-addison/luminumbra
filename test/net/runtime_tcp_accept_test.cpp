#include <gtest/gtest.h>

#include "../support/SystemConfigVariants.h"
#include "luminumbra_server/modes/ModeHelpers.h"

namespace {

using namespace Luminumbra::Net;

// Loopback delivery and async accept completion may need polling, but the host
// tick supplied to admission stays fixed. No handshake time passes while waiting.
template<typename Predicate>
bool PollUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        if (predicate())
            return true;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

class RuntimeTcpAccept : public testing::Test {
protected:
    static constexpr std::uint64_t kAcceptedTick = 1000;
    RuntimeTcpClientSlot slot;
    TcpTransport peer;
    HelloMsg expected;

    void SetUp() override {
        // Use an OS-assigned port and real TCP streams. Supply a completed accept
        // future so admission starts at an exact simulation tick.
        TcpListener listener;
        ASSERT_TRUE(listener.Listen(0));
        slot.port = listener.port();
        slot.client_id = 1;
        expected.client_id = slot.client_id;
        expected.system_config_identity = Luminumbra::world::GameSession{}.SystemConfigIdentity();
        expected.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
        ASSERT_FALSE(expected.system_config_identity.empty());
        ASSERT_FALSE(expected.preset_content_hash.empty());
        ASSERT_TRUE(peer.Connect("127.0.0.1", slot.port, /*timeout_ms=*/3000));
        slot.transport = listener.AcceptOneBlocking(/*timeout_ms=*/3000);
        ASSERT_NE(slot.transport, nullptr);
        listener.Close();
        std::promise<bool> accepted;
        slot.accept_result = accepted.get_future();
        slot.accepting = true;
        accepted.set_value(true);
        ASSERT_FALSE(PollRuntimeTcpAccept(slot, expected, kAcceptedTick));
        ASSERT_TRUE(slot.awaiting_hello);
        ASSERT_EQ(slot.hello_started_tick, kAcceptedTick);
    }

    void TearDown() override {
        CloseRuntimeTcpSlot(slot);
    }
};

TEST_F(RuntimeTcpAccept, SilentPeerExpiresAndSlotAcceptsAnotherClient) {
    for (std::uint64_t elapsed = 0; elapsed < kRuntimeTcpHelloTimeoutTicks; ++elapsed) {
        ASSERT_FALSE(PollRuntimeTcpAccept(slot, expected, kAcceptedTick + elapsed));
        ASSERT_TRUE(slot.awaiting_hello);
        ASSERT_TRUE(slot.transport->IsPeerConnected());
    }

    const auto expired_tick = kAcceptedTick + kRuntimeTcpHelloTimeoutTicks;
    EXPECT_FALSE(PollRuntimeTcpAccept(slot, expected, expired_tick));
    EXPECT_FALSE(slot.awaiting_hello);
    EXPECT_FALSE(slot.accepting);
    EXPECT_FALSE(slot.accepted);
    EXPECT_FALSE(slot.connected);
    EXPECT_EQ(slot.hello_started_tick, 0u);
    EXPECT_EQ(slot.transport, nullptr);
    ASSERT_TRUE(PollUntil([&] {
        std::vector<std::uint8_t> frame;
        peer.TryReceiveFrame(frame);
        return !peer.IsPeerConnected();
    }));

    // Exercise the real listen re-arm on the same slot and port. Connect on the
    // accept worker after bind/listen, before the accept timeout starts: Windows
    // Connect is blocking and must never race a listener that has not bound yet.
    // The future publishes the connection result before the main thread uses it.
    const auto reconnect_tick = expired_tick + 1;
    auto replacement = std::make_shared<TcpTransport>();
    auto connected = std::make_shared<std::promise<bool>>();
    auto connection_result = connected->get_future();
    const auto on_listening = [replacement, connected, port = slot.port] {
        connected->set_value(replacement->Connect("127.0.0.1", port, /*timeout_ms=*/3000));
    };
    ASSERT_FALSE(PollRuntimeTcpAccept(slot, expected, reconnect_tick, on_listening));
    ASSERT_TRUE(PollUntil([&] {
        return connection_result.wait_for(std::chrono::milliseconds(0)) ==
               std::future_status::ready;
    }));
    ASSERT_TRUE(connection_result.get());
    ASSERT_TRUE(PollUntil([&] {
        PollRuntimeTcpAccept(slot, expected, reconnect_tick);
        return slot.awaiting_hello;
    }));
    EXPECT_EQ(slot.hello_started_tick, reconnect_tick);
    ASSERT_TRUE(replacement->SendFrame(EncodeHello(expected)));
    ASSERT_TRUE(PollUntil([&] {
        return PollRuntimeTcpAccept(
            slot, expected, reconnect_tick + kRuntimeTcpHelloTimeoutTicks - 1);
    }));
    EXPECT_TRUE(slot.accepted);
    EXPECT_TRUE(slot.connected);
    EXPECT_FALSE(slot.awaiting_hello);
}

TEST_F(RuntimeTcpAccept, HelloOneTickBeforeDeadlineIsAccepted) {
    ASSERT_TRUE(peer.SendFrame(EncodeHello(expected)));
    const auto last_tick = kAcceptedTick + kRuntimeTcpHelloTimeoutTicks - 1;
    ASSERT_TRUE(PollUntil([&] { return PollRuntimeTcpAccept(slot, expected, last_tick); }));
    EXPECT_TRUE(slot.accepted);
    EXPECT_TRUE(slot.connected);
    EXPECT_FALSE(slot.awaiting_hello);
    EXPECT_EQ(slot.hello_started_tick, 0u);
    EXPECT_FALSE(
        PollRuntimeTcpAccept(slot, expected, kAcceptedTick + kRuntimeTcpHelloTimeoutTicks));
    ASSERT_NE(slot.transport, nullptr);
    EXPECT_TRUE(slot.transport->IsPeerConnected());
}

TEST_F(RuntimeTcpAccept, QueuedHelloAtDeadlineIsRefused) {
    ASSERT_TRUE(peer.SendFrame(EncodeHello(expected)));
    EXPECT_FALSE(
        PollRuntimeTcpAccept(slot, expected, kAcceptedTick + kRuntimeTcpHelloTimeoutTicks));
    EXPECT_FALSE(slot.awaiting_hello);
    EXPECT_FALSE(slot.accepted);
    EXPECT_FALSE(slot.connected);
    EXPECT_EQ(slot.transport, nullptr);
}

TEST_F(RuntimeTcpAccept, RepeatedPollsAtSameTickDoNotConsumeHelloWindow) {
    for (std::uint64_t poll = 0; poll <= kRuntimeTcpHelloTimeoutTicks; ++poll) {
        ASSERT_FALSE(PollRuntimeTcpAccept(slot, expected, kAcceptedTick));
        ASSERT_TRUE(slot.awaiting_hello);
    }
    ASSERT_TRUE(peer.SendFrame(EncodeHello(expected)));
    EXPECT_TRUE(PollUntil([&] { return PollRuntimeTcpAccept(slot, expected, kAcceptedTick); }));
}

} // namespace
