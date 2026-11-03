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
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "luminumbra_common/net/LockstepSession.h"

namespace {

using Luminumbra::Net::kMaxFramePayloadBytes;

void AppendWireFrame(std::vector<std::uint8_t>& wire, const std::vector<std::uint8_t>& frame) {
    for (int i = 0; i < 4; ++i)
        wire.push_back(static_cast<std::uint8_t>(frame.size() >> (8 * i)));
    wire.insert(wire.end(), frame.begin(), frame.end());
}

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
        m_receive_socket = accepted; // Borrowed for delivery synchronization and send shutdown.
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

    void ExpectBufferedFramesBeforeClosure(bool reset) {
        const std::vector<std::vector<std::uint8_t>> frames{
            Luminumbra::Net::EncodeBye({42}), {}, {0x7E}, {0xAB, 0xCD}};
        // Consume a leading frame through the public API to read ahead the entire
        // batch before the peer closes. The remaining frames must survive FIN/RST.
        const std::vector<std::uint8_t> leading{0x12};
        std::vector<std::uint8_t> wire;
        AppendWireFrame(wire, leading);
        for (const auto& frame : frames)
            AppendWireFrame(wire, frame);
        ASSERT_TRUE(SendRaw(wire));
        ASSERT_TRUE(WaitForBytes(wire.size()));
        std::vector<std::uint8_t> received{0xA5};
        ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
        ASSERT_EQ(received, leading);
        ASSERT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, wire.size());
        ASSERT_EQ(m_receiver->GetBufferDiagnostics().consumed_receive_bytes, 4u + leading.size());

        if (reset) {
            // Abort only after all sent bytes are in the transport's buffer, so an
            // abortive close cannot discard any of the frames under test in the kernel.
            const linger abortive{1, 0};
            ASSERT_EQ(::setsockopt(m_sender.handle,
                                   SOL_SOCKET,
                                   SO_LINGER,
                                   reinterpret_cast<const char*>(&abortive),
                                   sizeof(abortive)),
                      0);
        }
        m_sender.Close();
        // With no wire bytes left in the kernel, readability signals FIN or RST.
        // Wait for it without consuming the EOF or the pending receive error.
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(m_receive_socket, &readable);
        timeval timeout{1, 0};
#ifdef _WIN32
        ASSERT_EQ(::select(0, &readable, nullptr, nullptr, &timeout), 1);
#else
        ASSERT_EQ(::select(m_receive_socket + 1, &readable, nullptr, nullptr, &timeout), 1);
#endif

        for (const auto& frame : frames) {
            ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
            EXPECT_EQ(received, frame);
            EXPECT_TRUE(m_receiver->IsPeerConnected());
        }
        const auto last_frame = received;
        for (int pump = 0; pump < 3; ++pump) {
            EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
            EXPECT_EQ(received, last_frame);
            EXPECT_FALSE(m_receiver->IsPeerConnected());
            EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, 0u);
            EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_capacity, 0u);
            EXPECT_EQ(m_receiver->GetBufferDiagnostics().consumed_receive_bytes, 0u);
        }
    }

    void ExpectBufferedFramesAfterSendFailure(const std::vector<std::uint8_t>& tail) {
        const std::vector<std::uint8_t> leading{0x12};
        const std::vector<std::vector<std::uint8_t>> frames{
            Luminumbra::Net::EncodeBye({42}), {}, {0xAB, 0xCD}};
        std::vector<std::uint8_t> wire;
        AppendWireFrame(wire, leading);
        for (const auto& frame : frames)
            AppendWireFrame(wire, frame);
        wire.insert(wire.end(), tail.begin(), tail.end());
        ASSERT_TRUE(SendRaw(wire));
        ASSERT_TRUE(WaitForBytes(wire.size()));
        std::vector<std::uint8_t> received;
        ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
        ASSERT_EQ(received, leading);
        ASSERT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, wire.size());
        ASSERT_EQ(m_receiver->GetBufferDiagnostics().consumed_receive_bytes, 4u + leading.size());

        // Force a send error without consuming any buffered inbound frames.
#ifdef _WIN32
        ASSERT_EQ(::shutdown(m_receive_socket, SD_SEND), 0);
#else
#ifdef SO_NOSIGPIPE
        const int no_sigpipe = 1;
        ASSERT_EQ(::setsockopt(
                      m_receive_socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)),
                  0);
#endif
        ASSERT_EQ(::shutdown(m_receive_socket, SHUT_WR), 0);
#endif
        ASSERT_FALSE(m_receiver->SendFrame({}));
        m_sender.Close();
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(m_receive_socket, &readable);
        timeval timeout{1, 0};
#ifdef _WIN32
        ASSERT_EQ(::select(0, &readable, nullptr, nullptr, &timeout), 1);
#else
        ASSERT_EQ(::select(m_receive_socket + 1, &readable, nullptr, nullptr, &timeout), 1);
#endif

        for (const auto& frame : frames) {
            ASSERT_TRUE(m_receiver->IsPeerConnected());
            ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
            EXPECT_EQ(received, frame);
        }
        // Neither a consumed prefix nor an incomplete tail is a deliverable frame.
        EXPECT_FALSE(m_receiver->IsPeerConnected());
        const auto last_frame = received;
        for (int pump = 0; pump < 3; ++pump) {
            EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
            EXPECT_EQ(received, last_frame);
            EXPECT_FALSE(m_receiver->IsPeerConnected());
            EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, 0u);
            EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_capacity, 0u);
            EXPECT_EQ(m_receiver->GetBufferDiagnostics().consumed_receive_bytes, 0u);
        }
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
        EXPECT_FALSE(m_receiver->IsPeerConnected());
        EXPECT_FALSE(m_receiver->SendFrame({}));
        EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, 0u);
        EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_capacity, 0u);
    }
}

INSTANTIATE_TEST_SUITE_P(WrappingLengths,
                         TcpFrameLengthPrefixTest,
                         ::testing::Values(static_cast<std::uint32_t>(kMaxFramePayloadBytes + 1),
                                           0xFFFFFFFCu,
                                           0xFFFFFFFDu,
                                           0xFFFFFFFEu,
                                           0xFFFFFFFFu));

TEST_F(TcpFrameBoundsTest, MaximumPrefixWaitsForPayloadAndTruncatedCloseDisconnects) {
    std::vector<std::uint8_t> prefix;
    for (int i = 0; i < 4; ++i)
        prefix.push_back(static_cast<std::uint8_t>(kMaxFramePayloadBytes >> (8 * i)));
    ASSERT_TRUE(SendRaw(prefix));
    ASSERT_TRUE(WaitForBytes(prefix.size()));
    const std::vector<std::uint8_t> sentinel{0xA5, 0x5A};
    std::vector<std::uint8_t> received = sentinel;
    EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
    EXPECT_EQ(received, sentinel);
    EXPECT_TRUE(m_receiver->IsPeerConnected());
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, 4u);

    const std::vector<std::uint8_t> fragment(4096, 0xAB);
    ASSERT_TRUE(SendRaw(fragment));
    ASSERT_TRUE(WaitForBytes(fragment.size()));
    EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
    EXPECT_EQ(received, sentinel);
    EXPECT_TRUE(m_receiver->IsPeerConnected());
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, 4u + fragment.size());
    EXPECT_LE(m_receiver->GetBufferDiagnostics().receive_capacity, 4u + kMaxFramePayloadBytes);

    // A truncated frame followed by EOF must not keep the transport connected.
    m_sender.Close();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
        if (!m_receiver->IsPeerConnected())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    EXPECT_FALSE(m_receiver->IsPeerConnected());
    EXPECT_EQ(received, sentinel);
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_capacity, 0u);
}

TEST_F(TcpFrameBoundsTest, MaximumFrameIsDeliveredCompletely) {
    std::vector<std::uint8_t> prefix;
    for (int i = 0; i < 4; ++i)
        prefix.push_back(static_cast<std::uint8_t>(kMaxFramePayloadBytes >> (8 * i)));
    ASSERT_TRUE(SendRaw(prefix));
    // Pump between small writes so no socket buffer must hold the whole frame,
    // and only the transport and delivered result allocate full-size buffers.
    const std::vector<std::uint8_t> chunk(4096, 0xAB);
    std::vector<std::uint8_t> received{0xA5};
    bool complete = false;
    for (std::size_t sent = 0; sent < kMaxFramePayloadBytes; sent += chunk.size()) {
        ASSERT_TRUE(SendRaw(chunk));
        complete = m_receiver->TryReceiveFrame(received);
        if (sent + chunk.size() < kMaxFramePayloadBytes) {
            ASSERT_FALSE(complete);
            ASSERT_EQ(received, (std::vector<std::uint8_t>{0xA5}));
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!complete && std::chrono::steady_clock::now() < deadline) {
        complete = m_receiver->TryReceiveFrame(received);
        std::this_thread::yield();
    }
    ASSERT_TRUE(complete);
    ASSERT_EQ(received.size(), kMaxFramePayloadBytes);
    EXPECT_TRUE(std::all_of(
        received.begin(), received.end(), [](std::uint8_t byte) { return byte == 0xAB; }));
    EXPECT_TRUE(m_receiver->IsPeerConnected());
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, 0u);
    EXPECT_LE(m_receiver->GetBufferDiagnostics().receive_capacity, 4u + kMaxFramePayloadBytes);
    EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
}

TEST_F(TcpFrameBoundsTest, OversizedPrefixDiscardsFollowingBytes) {
    std::vector<std::uint8_t> wire;
    for (int i = 0; i < 4; ++i)
        wire.push_back(static_cast<std::uint8_t>((kMaxFramePayloadBytes + 1) >> (8 * i)));
    wire.resize(4096, 0xAB);
    ASSERT_TRUE(SendRaw(wire));
    ASSERT_TRUE(WaitForBytes(wire.size()));
    std::vector<std::uint8_t> received{0xA5};
    EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
    EXPECT_EQ(received, (std::vector<std::uint8_t>{0xA5}));
    EXPECT_FALSE(m_receiver->IsPeerConnected());
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_capacity, 0u);
}

TEST_F(TcpFrameBoundsTest, CoalescedFramesAreReadAheadAndDeliveredInOrder) {
    const std::vector<std::vector<std::uint8_t>> frames{
        Luminumbra::Net::EncodeBye({42}), {}, {0x7E, 0xAB, 0xCD}};
    std::vector<std::uint8_t> wire;
    for (const auto& frame : frames)
        AppendWireFrame(wire, frame);
    ASSERT_TRUE(SendRaw(wire));
    ASSERT_TRUE(WaitForBytes(wire.size()));

    std::vector<std::uint8_t> received{0xA5};
    std::size_t remaining = wire.size();
    for (const auto& frame : frames) {
        ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
        EXPECT_EQ(received, frame);
        remaining -= 4 + frame.size();
        EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes,
                  remaining == 0 ? 0u : wire.size());
        EXPECT_EQ(m_receiver->GetBufferDiagnostics().consumed_receive_bytes,
                  remaining == 0 ? 0u : wire.size() - remaining);
        EXPECT_LE(m_receiver->GetBufferDiagnostics().receive_capacity, 4u + kMaxFramePayloadBytes);
        EXPECT_TRUE(m_receiver->IsPeerConnected());
    }
    EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
}

TEST_F(TcpFrameBoundsTest, BufferedFramesAreDeliveredBeforeEof) {
    ExpectBufferedFramesBeforeClosure(false);
}

TEST_F(TcpFrameBoundsTest, BufferedFramesAreDeliveredBeforeReceiveReset) {
    ExpectBufferedFramesBeforeClosure(true);
}

TEST_F(TcpFrameBoundsTest, BufferedFramesRemainConnectedAfterSendFailure) {
    ExpectBufferedFramesAfterSendFailure({});
}

TEST_F(TcpFrameBoundsTest, PartialPrefixAfterSendFailureDoesNotKeepPeerConnected) {
    ExpectBufferedFramesAfterSendFailure({0x03, 0x00});
}

TEST_F(TcpFrameBoundsTest, PartialPayloadAfterSendFailureDoesNotKeepPeerConnected) {
    ExpectBufferedFramesAfterSendFailure({0x03, 0x00, 0x00, 0x00, 0xAB});
}

TEST_F(TcpFrameBoundsTest, OversizedPrefixAfterSendFailureDoesNotKeepPeerConnected) {
    std::vector<std::uint8_t> tail;
    for (int i = 0; i < 4; ++i)
        tail.push_back(static_cast<std::uint8_t>((kMaxFramePayloadBytes + 1) >> (8 * i)));
    ExpectBufferedFramesAfterSendFailure(tail);
}

TEST_F(TcpFrameBoundsTest, EmptyFrameBatchAdvancesCursorWithoutMovingBufferedBytes) {
    const std::vector<std::uint8_t> wire(64u * 1024u, 0);
    ASSERT_TRUE(SendRaw(wire));
    ASSERT_TRUE(WaitForBytes(wire.size()));
    std::vector<std::uint8_t> received{0xA5};
    for (std::size_t consumed = 4; consumed <= wire.size(); consumed += 4) {
        ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
        ASSERT_TRUE(received.empty());
        ASSERT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes,
                  consumed == wire.size() ? 0u : wire.size());
        ASSERT_EQ(m_receiver->GetBufferDiagnostics().consumed_receive_bytes,
                  consumed == wire.size() ? 0u : consumed);
    }
    EXPECT_TRUE(m_receiver->IsPeerConnected());
}

TEST_F(TcpFrameBoundsTest, ConsumedPrefixCompactsOnlyWhenIncompleteFrameNeedsMoreBytes) {
    const std::vector<std::uint8_t> first{0x12};
    const std::vector<std::uint8_t> second{0x34};
    const std::vector<std::uint8_t> third{0x56, 0x78, 0x9A, 0xBC};
    std::vector<std::uint8_t> wire;
    AppendWireFrame(wire, first);
    AppendWireFrame(wire, second);
    AppendWireFrame(wire, third);
    const std::vector<std::uint8_t> tail(wire.end() - 2, wire.end());
    wire.resize(wire.size() - tail.size());
    ASSERT_TRUE(SendRaw(wire));
    ASSERT_TRUE(WaitForBytes(wire.size()));

    std::vector<std::uint8_t> received{0xA5};
    ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
    EXPECT_EQ(received, first);
    ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
    EXPECT_EQ(received, second);
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, wire.size());
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().consumed_receive_bytes, 10u);

    EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
    EXPECT_EQ(received, second);
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, 6u);
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().consumed_receive_bytes, 0u);
    EXPECT_TRUE(m_receiver->IsPeerConnected());
    ASSERT_TRUE(SendRaw(tail));
    ASSERT_TRUE(WaitForBytes(tail.size()));
    ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
    EXPECT_EQ(received, third);
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, 0u);
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().consumed_receive_bytes, 0u);
    EXPECT_LE(m_receiver->GetBufferDiagnostics().receive_capacity, 4u + kMaxFramePayloadBytes);
}

TEST_F(TcpFrameBoundsTest, ValidFramesPrecedeCoalescedOversizedPrefixDisconnect) {
    const auto first = Luminumbra::Net::EncodeBye({42});
    const std::vector<std::uint8_t> second{0x7E};
    std::vector<std::uint8_t> wire;
    AppendWireFrame(wire, first);
    AppendWireFrame(wire, second);
    for (int i = 0; i < 4; ++i)
        wire.push_back(static_cast<std::uint8_t>((kMaxFramePayloadBytes + 1) >> (8 * i)));
    wire.insert(wire.end(), 4096, 0xAB);
    ASSERT_TRUE(SendRaw(wire));
    ASSERT_TRUE(WaitForBytes(wire.size()));

    std::vector<std::uint8_t> received{0xA5};
    ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
    EXPECT_EQ(received, first);
    EXPECT_TRUE(m_receiver->IsPeerConnected());
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, wire.size());
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().consumed_receive_bytes, 4u + first.size());
    ASSERT_TRUE(m_receiver->TryReceiveFrame(received));
    EXPECT_EQ(received, second);
    EXPECT_TRUE(m_receiver->IsPeerConnected());
    EXPECT_FALSE(m_receiver->TryReceiveFrame(received));
    EXPECT_EQ(received, second);
    EXPECT_FALSE(m_receiver->IsPeerConnected());
    EXPECT_FALSE(m_receiver->SendFrame({}));
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_bytes, 0u);
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_capacity, 0u);
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().consumed_receive_bytes, 0u);
}

TEST_F(TcpFrameBoundsTest, OversizedPrefixEndsLockstepSessionPermanently) {
    using namespace Luminumbra::Net;
    LockstepConfig config;
    config.local_client_id = 0;
    config.peer_client_id = 1;
    config.system_config_identity = "config";
    config.preset_content_hash = "preset";
    HelloMsg hello;
    hello.seed = config.seed;
    hello.preset = config.preset;
    hello.tick_rate_hz = config.tick_rate_hz;
    hello.client_id = config.peer_client_id;
    hello.system_config_identity = config.system_config_identity;
    hello.preset_content_hash = config.preset_content_hash;
    const auto send_frame = [&](const std::vector<std::uint8_t>& frame) {
        std::vector<std::uint8_t> wire;
        for (int i = 0; i < 4; ++i)
            wire.push_back(static_cast<std::uint8_t>(frame.size() >> (8 * i)));
        wire.insert(wire.end(), frame.begin(), frame.end());
        return SendRaw(wire) && WaitForBytes(wire.size());
    };
    ASSERT_TRUE(send_frame(EncodeHello(hello)));
    LockstepSession session(config, m_receiver.get(), {});
    ASSERT_TRUE(session.Handshake());
    ASSERT_TRUE(send_frame(EncodeInput({1, config.peer_client_id, {}})));
    ASSERT_EQ(session.PumpTick(10).outcome, TickOutcome::Advanced);
    std::vector<std::uint8_t> prefix;
    for (int i = 0; i < 4; ++i)
        prefix.push_back(static_cast<std::uint8_t>((kMaxFramePayloadBytes + 1) >> (8 * i)));
    ASSERT_TRUE(SendRaw(prefix));
    ASSERT_TRUE(WaitForBytes(prefix.size()));
    for (int pump = 0; pump < 3; ++pump) {
        const auto result = session.PumpTick(10);
        EXPECT_EQ(result.outcome, TickOutcome::PeerDisconnected);
        EXPECT_EQ(result.tick, 1u);
    }
    EXPECT_TRUE(session.Status().peer_disconnected);
    EXPECT_FALSE(session.Status().desynced);
    EXPECT_EQ(session.Status().late_input_events, 0u);
    EXPECT_FALSE(m_receiver->IsPeerConnected());
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().receive_capacity, 0u);
}

TEST_F(TcpFrameBoundsTest, SendRejectsMaximumPlusOneWithoutWritingAndAcceptsMaximum) {
    // Reuse one allocation for both boundaries; the raw peer drains into a small buffer.
    std::vector<std::uint8_t> frame(kMaxFramePayloadBytes + 1, 0xAB);
    EXPECT_FALSE(m_receiver->SendFrame(frame));
    EXPECT_TRUE(m_receiver->IsPeerConnected());
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().pending_send_bytes, 0u);
#ifdef _WIN32
    u_long available = 0;
    ASSERT_EQ(::ioctlsocket(m_sender.handle, FIONREAD, &available), 0);
#else
    int available = 0;
    ASSERT_EQ(::ioctl(m_sender.handle, FIONREAD, &available), 0);
#endif
    EXPECT_EQ(available, 0u);

    frame.resize(kMaxFramePayloadBytes);
    bool received_all = false;
    std::thread reader([&] {
        std::size_t received = 0;
        const std::size_t wire_size = 4 + kMaxFramePayloadBytes;
        while (received < wire_size) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(m_sender.handle, &readable);
            timeval timeout{1, 0};
#ifdef _WIN32
            const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
            const int ready = ::select(m_sender.handle + 1, &readable, nullptr, nullptr, &timeout);
#endif
            if (ready <= 0)
                return;
            char bytes[4096];
            const int count = static_cast<int>(::recv(m_sender.handle, bytes, sizeof(bytes), 0));
            if (count <= 0)
                return;
            for (int i = 0; i < count; ++i) {
                const std::size_t offset = received + static_cast<std::size_t>(i);
                const auto expected =
                    offset < 4 ? static_cast<std::uint8_t>(kMaxFramePayloadBytes >> (8 * offset))
                               : std::uint8_t{0xAB};
                if (static_cast<std::uint8_t>(bytes[i]) != expected)
                    return;
            }
            received += static_cast<std::size_t>(count);
        }
        received_all = received == wire_size;
    });
    EXPECT_TRUE(m_receiver->SendFrame(frame));
    // SendFrame may leave a tail below the high-water mark; polling flushes that tail.
    std::vector<std::uint8_t> unused;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (m_receiver->GetBufferDiagnostics().pending_send_bytes > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        EXPECT_FALSE(m_receiver->TryReceiveFrame(unused));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    reader.join();
    EXPECT_EQ(m_receiver->GetBufferDiagnostics().pending_send_bytes, 0u);
    EXPECT_TRUE(received_all);
}

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
