#include <gtest/gtest.h>

#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/net/NetSocketsTransport.h"
#include "steam/isteamnetworkingsockets.h"

// This isolated target needs only the public log accessor, without engine startup.
std::shared_ptr<spdlog::logger>& Log::GetCoreLogger() {
    static auto logger = spdlog::default_logger();
    return logger;
}

namespace {
using namespace Luminumbra::Net;

class NetSocketsTransportTest : public ::testing::Test {
protected:
    NetSocketsBackend backend{nullptr, nullptr, nullptr, true};
    NetSocketsTransport transport{backend, "socket-contract"};

    void SetUp() override {
        *SteamNetworkingSockets() = FakeNetworkingSockets{};
        ASSERT_TRUE(transport.Connect("127.0.0.1", 1));
        SteamNetConnectionStatusChangedCallback_t info{};
        info.m_hConn = 2;
        info.m_info.m_eState = k_ESteamNetworkingConnectionState_Connected;
        NetSocketsTransport::OnConnStatusChangedStatic(&info);
        ASSERT_TRUE(transport.IsPeerConnected());
    }
};

TEST_F(NetSocketsTransportTest, TemporaryResultsRetryIdenticalBytes) {
    auto& sockets = *SteamNetworkingSockets();
    const std::vector<std::uint8_t> frame{1, 0, 0xFF};
    for (const auto delivery : {FrameDelivery::Reliable, FrameDelivery::Unreliable}) {
        for (const auto result : {k_EResultLimitExceeded, k_EResultIgnored}) {
            sockets.result = result;
            EXPECT_FALSE(transport.SendFrame(frame, delivery));
            std::vector<std::uint8_t> received;
            EXPECT_FALSE(transport.TryReceiveFrame(received));
            EXPECT_TRUE(transport.IsPeerConnected());
            EXPECT_EQ(sockets.close_calls, 0u);
            sockets.result = k_EResultOK;
            EXPECT_TRUE(transport.SendFrame(frame, delivery));
        }
    }
    ASSERT_EQ(sockets.attempts.size(), 8u);
    for (const auto& attempted : sockets.attempts)
        EXPECT_EQ(attempted, frame);
}

TEST_F(NetSocketsTransportTest, PermanentResultsDrainBackendAndLocalQueueBeforeClosure) {
    auto& sockets = *SteamNetworkingSockets();
    for (const auto result :
         {k_EResultInvalidParam, k_EResultInvalidState, k_EResultNoConnection, k_EResultFail}) {
        SCOPED_TRACE(static_cast<int>(result));
        NetSocketsTransport connection(backend, "permanent-refusal");
        ASSERT_TRUE(connection.Connect("127.0.0.1", 1));
        SteamNetConnectionStatusChangedCallback_t info{};
        info.m_hConn = 2;
        info.m_info.m_eState = k_ESteamNetworkingConnectionState_Connected;
        NetSocketsTransport::OnConnStatusChangedStatic(&info);
        // Cross both the per-drain message bound and the local byte budget.
        for (std::uint8_t index = 0; index < 40; ++index)
            sockets.incoming.emplace_back(8192, index);
        std::vector<std::uint8_t> received;
        ASSERT_TRUE(connection.TryReceiveFrame(received));
        EXPECT_EQ(received.front(), 0u);
        const auto closes_before = sockets.close_calls;
        sockets.result = result;
        EXPECT_FALSE(connection.SendFrame({0xAB}));
        const auto sends_after_failure = sockets.send_calls;
        EXPECT_FALSE(connection.SendFrame({0xAB}));
        EXPECT_EQ(sockets.send_calls, sends_after_failure);
        for (std::uint8_t index = 1; index < 40; ++index) {
            EXPECT_TRUE(connection.IsPeerConnected());
            ASSERT_TRUE(connection.TryReceiveFrame(received));
            EXPECT_EQ(received, (std::vector<std::uint8_t>(8192, index)));
        }
        EXPECT_FALSE(connection.TryReceiveFrame(received));
        EXPECT_FALSE(connection.IsPeerConnected());
        EXPECT_EQ(sockets.close_calls, closes_before + 1);
        EXPECT_TRUE(sockets.incoming.empty());
    }
    EXPECT_EQ(sockets.released_messages, 160u);
}

TEST_F(NetSocketsTransportTest, BackendLimitIsInclusiveAndOversizeNeverReachesBackend) {
    auto& sockets = *SteamNetworkingSockets();
    EXPECT_EQ(transport.MaxFramePayloadBytes(), 512u * 1024u);
    for (const auto delivery : {FrameDelivery::Reliable, FrameDelivery::Unreliable}) {
        std::vector<std::uint8_t> frame(transport.MaxFramePayloadBytes() + 1, 0xAB);
        const auto sends_before = sockets.send_calls;
        EXPECT_FALSE(transport.SendFrame(frame, delivery));
        EXPECT_EQ(sockets.send_calls, sends_before);
        frame.pop_back();
        EXPECT_TRUE(transport.SendFrame(frame, delivery));
        EXPECT_EQ(sockets.send_calls, sends_before + 1);
    }
}
} // namespace
