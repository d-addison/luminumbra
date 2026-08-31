// true single-port multi-connection accept (dedicated-server shape).
//
// Proves the real dedicated-server accept shape: ONE listen socket (TcpListener) binds a
// single port and accepts N simultaneous incoming client connections, fanning EACH into
// its own transport that ReplicationServer::AddClient registers as a DISTINCT client id.
// This replaces the old "one port per client, one connection per TcpTransport::Listen"
// scheme. Headless / loopback only (127.0.0.1); the listener uses an OS-assigned ephemeral
// port so the test never collides with a fixed port or another test run.
//
// proving_signal: opening ONE listen socket and accepting 3+ simultaneous client
// connections yields 3+ distinct AddClient registrations, each with its own client id,
// each backed by an INDEPENDENT connection stream (a distinct per-client hello arrives on
// each accepted transport -- so they are truly separate connections, not one re-counted).
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/net/ReplicationEndpoint.h"

namespace {

using namespace Luminumbra::Net;

// Non-blockingly poll one transport for a single framed message, up to a bounded budget
// (real loopback sockets deliver asynchronously). Returns true and fills `out` on success.
bool ReceiveFrameWithin(ILockstepTransport& t,
                        std::vector<std::uint8_t>& out,
                        int max_tries = 200) {
    for (int i = 0; i < max_tries; ++i) {
        if (t.TryReceiveFrame(out))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// A one-byte "hello" frame carrying the client's marker (distinct per client). Sent right
// after Connect so each accepted server-side transport can be shown to be an independent
// stream by reading back its client's marker.
std::vector<std::uint8_t> MarkerFrame(std::uint8_t marker) {
    return {marker};
}

TEST(SinglePortAccept, AcceptsThreeSimultaneousConnectionsAsDistinctClients) {
    constexpr int kNumClients = 3;

    // ONE listen socket on an ephemeral port.
    TcpListener listener;
    ASSERT_TRUE(listener.Listen(/*port=*/0, /*backlog=*/kNumClients + 4));
    ASSERT_TRUE(listener.IsListening());
    const std::uint16_t port = listener.port();
    ASSERT_NE(port, 0) << "ephemeral port should be resolved via getsockname";

    // Connect all N clients FIRST (all live at once -> simultaneous, sitting in the listen
    // backlog), each sending its distinct marker before any accept happens.
    std::vector<std::unique_ptr<TcpTransport>> clients;
    const std::uint8_t kMarkerBase = 101;
    for (int i = 0; i < kNumClients; ++i) {
        auto c = std::make_unique<TcpTransport>();
        ASSERT_TRUE(c->Connect("127.0.0.1", port, /*timeout_ms=*/3000))
            << "client " << i << " failed to connect on port " << port;
        ASSERT_TRUE(c->SendFrame(MarkerFrame(static_cast<std::uint8_t>(kMarkerBase + i))));
        clients.push_back(std::move(c));
    }

    // Accept all N from the ONE listen socket, fanning each into a DISTINCT AddClient id.
    ReplicationServer server;
    std::vector<std::unique_ptr<TcpTransport>> accepted; // caller owns the accepted transports
    std::uint32_t next_client_id = 1;
    for (int i = 0; i < kNumClients; ++i) {
        std::unique_ptr<TcpTransport> t = listener.AcceptOneBlocking(/*timeout_ms=*/3000);
        ASSERT_NE(t, nullptr) << "expected to accept connection " << i
                              << " from the single listen socket";
        EXPECT_TRUE(t->IsPeerConnected());
        server.AddClient(next_client_id++, t.get());
        accepted.push_back(std::move(t));
    }

    // Each accepted connection became its OWN client id: distinct 1,2,3 -> count 3.
    ASSERT_EQ(server.client_count(), static_cast<std::size_t>(kNumClients));
    EXPECT_TRUE(server.has_client(1));
    EXPECT_TRUE(server.has_client(2));
    EXPECT_TRUE(server.has_client(3));

    // Each accepted transport is an INDEPENDENT stream: read back exactly one marker per
    // accepted connection; the set of markers must be {101,102,103} (no dup, none missing),
    // proving 3 truly separate connections rather than one connection counted three times.
    std::set<std::uint8_t> markers_seen;
    for (auto& t : accepted) {
        std::vector<std::uint8_t> frame;
        ASSERT_TRUE(ReceiveFrameWithin(*t, frame))
            << "accepted connection delivered no marker frame";
        ASSERT_EQ(frame.size(), 1u);
        markers_seen.insert(frame[0]);
    }
    const std::set<std::uint8_t> expected = {101, 102, 103};
    EXPECT_EQ(markers_seen, expected);

    listener.Close();
    for (auto& c : clients)
        c->Close();
    for (auto& t : accepted)
        t->Close();
}

// The non-blocking tick-path API (IConnectionAcceptor::AcceptOne, used through the abstract
// seam a server would hold) also fans a single listen socket into distinct clients.
TEST(SinglePortAccept, NonBlockingAcceptOneFansIntoDistinctClients) {
    constexpr int kNumClients = 4;

    TcpListener listener;
    ASSERT_TRUE(listener.Listen(/*port=*/0, /*backlog=*/kNumClients + 4));
    const std::uint16_t port = listener.port();
    ASSERT_NE(port, 0);

    std::vector<std::unique_ptr<TcpTransport>> clients;
    for (int i = 0; i < kNumClients; ++i) {
        auto c = std::make_unique<TcpTransport>();
        ASSERT_TRUE(c->Connect("127.0.0.1", port, /*timeout_ms=*/3000));
        clients.push_back(std::move(c));
    }

    // Drive accept purely through the abstract IConnectionAcceptor (non-blocking AcceptOne),
    // polling until all N pending connections have been fanned out -- exactly the shape a
    // dedicated server's per-tick accept poll uses.
    IConnectionAcceptor& acceptor = listener;
    ReplicationServer server;
    std::vector<std::unique_ptr<ILockstepTransport>> accepted;
    std::uint32_t next_client_id = 10; // arbitrary starting id -> ids 10,11,12,13
    for (int tries = 0; tries < 400 && static_cast<int>(accepted.size()) < kNumClients; ++tries) {
        if (std::unique_ptr<ILockstepTransport> t = acceptor.AcceptOne()) {
            server.AddClient(next_client_id++, t.get());
            accepted.push_back(std::move(t));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ASSERT_EQ(accepted.size(), static_cast<std::size_t>(kNumClients));
    EXPECT_EQ(server.client_count(), static_cast<std::size_t>(kNumClients));
    for (std::uint32_t id = 10; id < 10 + static_cast<std::uint32_t>(kNumClients); ++id) {
        EXPECT_TRUE(server.has_client(id)) << "missing distinct client id " << id;
    }

    listener.Close();
    for (auto& c : clients)
        c->Close();
    for (auto& t : accepted)
        t->Close();
}

} // namespace
