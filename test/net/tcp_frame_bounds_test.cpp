// Exercise TCP framing with raw wire bytes through the accepted-socket seam.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "luminumbra_common/net/LockstepSession.h"

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr SocketHandle kInvalidSocket = -1;
#endif

struct Socket {
    SocketHandle handle = kInvalidSocket;

    ~Socket() {
        Close();
    }
    void Close() {
        if (handle == kInvalidSocket)
            return;
#ifdef _WIN32
        ::closesocket(handle);
#else
        ::close(handle);
#endif
        handle = kInvalidSocket;
    }
};

class TcpFrameBoundsTest : public ::testing::Test {
protected:
    void SetUp() override {
#ifdef _WIN32
        WSADATA wsa{};
        ASSERT_EQ(::WSAStartup(MAKEWORD(2, 2), &wsa), 0);
        m_winsock_started = true;
#endif
        Socket listener;
        listener.handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_NE(listener.handle, kInvalidSocket);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ASSERT_EQ(::bind(listener.handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)),
                  0);
        ASSERT_EQ(::listen(listener.handle, 1), 0);
        SocketLength length = sizeof(address);
        ASSERT_EQ(::getsockname(listener.handle, reinterpret_cast<sockaddr*>(&address), &length),
                  0);
        m_sender.handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_NE(m_sender.handle, kInvalidSocket);
        ASSERT_EQ(
            ::connect(m_sender.handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        const SocketHandle accepted = ::accept(listener.handle, nullptr, nullptr);
        ASSERT_NE(accepted, kInvalidSocket);
        m_receiver =
            Luminumbra::Net::TcpTransport::FromAcceptedSocket(static_cast<std::intptr_t>(accepted));
        ASSERT_NE(m_receiver, nullptr);
        m_receive_socket = accepted; // Borrowed only to synchronize delivery before each pump.
    }

    void TearDown() override {
        m_receiver.reset();
        m_sender.Close();
#ifdef _WIN32
        if (m_winsock_started)
            ::WSACleanup();
#endif
    }

    bool SendRaw(const std::vector<std::uint8_t>& bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto sent = ::send(m_sender.handle,
                                     reinterpret_cast<const char*>(bytes.data() + offset),
                                     static_cast<int>(bytes.size() - offset),
                                     0);
            if (sent <= 0)
                return false;
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }

    // A negative receive assertion is meaningful only after the raw bytes have arrived.
    // FIONREAD observes kernel bytes without consuming them or adding a transport seam.
    bool WaitForBytes(std::size_t expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        do {
#ifdef _WIN32
            u_long available = 0;
            if (::ioctlsocket(m_receive_socket, FIONREAD, &available) != 0)
                return false;
#else
            int available = 0;
            if (::ioctl(m_receive_socket, FIONREAD, &available) != 0)
                return false;
#endif
            if (static_cast<std::size_t>(available) >= expected)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    Socket m_sender;
    SocketHandle m_receive_socket = kInvalidSocket;
    std::unique_ptr<Luminumbra::Net::TcpTransport> m_receiver;
#ifdef _WIN32
    bool m_winsock_started = false;
#endif
};

class TcpFrameLengthPrefixTest : public TcpFrameBoundsTest,
                                 public ::testing::WithParamInterface<std::uint32_t> {};

TEST_P(TcpFrameLengthPrefixTest, HugePrefixWithoutPayloadNeverProducesFrame) {
    const std::uint32_t length = GetParam();
    std::vector<std::uint8_t> prefix;
    for (int i = 0; i < 4; ++i)
        prefix.push_back(static_cast<std::uint8_t>(length >> (8 * i)));
    ASSERT_TRUE(SendRaw(prefix));
    ASSERT_TRUE(WaitForBytes(prefix.size()));

    const std::vector<std::uint8_t> sentinel{0xA5, 0x5A};
    std::vector<std::uint8_t> received = sentinel;
    for (int pump = 0; pump < 3; ++pump) {
        EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
        EXPECT_EQ(received, sentinel);
        EXPECT_TRUE(m_receiver->IsPeerConnected());
        EXPECT_TRUE(m_receiver->SendFrame({}));
    }
}

INSTANTIATE_TEST_SUITE_P(WrappingLengths,
                         TcpFrameLengthPrefixTest,
                         ::testing::Values(0xFFFFFFFCu, 0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu));

TEST_F(TcpFrameBoundsTest, SplitPrefixAndPayloadProduceExactlyOneCompleteFrame) {
    const auto frame = Luminumbra::Net::EncodeBye({42});
    std::vector<std::uint8_t> wire;
    for (int i = 0; i < 4; ++i)
        wire.push_back(static_cast<std::uint8_t>(frame.size() >> (8 * i)));
    wire.insert(wire.end(), frame.begin(), frame.end());

    const std::vector<std::uint8_t> sentinel{0xA5, 0x5A};
    std::vector<std::uint8_t> received = sentinel;
    std::size_t offset = 0;
    for (const std::size_t end : {std::size_t{2}, std::size_t{4}, std::size_t{7}, wire.size()}) {
        const std::vector<std::uint8_t> fragment(wire.begin() + offset, wire.begin() + end);
        ASSERT_TRUE(SendRaw(fragment));
        ASSERT_TRUE(WaitForBytes(fragment.size()));
        if (end < wire.size()) {
            EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
            EXPECT_EQ(received, sentinel);
        } else {
            ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
            EXPECT_EQ(received, frame);
        }
        offset = end;
    }
    EXPECT_FALSE(m_receiver->TryReceiveFrame(received));

    // The consumed frame must leave the next frames intact, including an empty one.
    const std::vector<std::uint8_t> following{0, 0, 0, 0, 1, 0, 0, 0, 0x7E};
    ASSERT_TRUE(SendRaw(following));
    ASSERT_TRUE(WaitForBytes(following.size()));
    ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
    EXPECT_TRUE(received.empty());
    ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
    EXPECT_EQ(received, (std::vector<std::uint8_t>{0x7E}));
    EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
}

} // namespace
