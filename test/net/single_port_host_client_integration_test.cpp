//  (, full form): single-port HOST<->CLIENT id fan-out over real TCP loopback.
//
// The single_port_accept_test proves ONE listen socket accepts N connections as distinct clients;
// this proves the END-TO-END dedicated-server rewire that RunNetHost/RunNetJoin now use: each
// client CONNECTS to the ONE listen port and DECLARES its own server-side id in its Hello (its
// first frame); the host AcceptOneBlocking-s each connection, reads that Hello, and fans it into
// the DECLARED slot via ReplicationServer::AddClient -- so the slot id comes from the HANDSHAKE,
// not the accept order. That is the difference between "one TcpTransport::Listen per client on
// base_port+K-1" (the retired scheme) and the real one-port-N-connections shape.
//
// proving_signal: N clients that declare NON-SEQUENTIAL ids ({40,10,30,20}) in a SHUFFLED launch
// order fan into a server client-id SET that EQUALS the declared set -- a set an accept-order
// counter (1,2,3,4 / next_id++) could never produce. Each accepted transport keeps a live peer,
// no id collides, and the post-Hello byte stream survives AddClient (mirroring the way the real
// usercmd stream is drained by PumpInbound after the host consumes the Hello pre-AddClient).
//
// Headless / loopback only (127.0.0.1) on an OS-assigned ephemeral port (Listen(0)); single-
// process, bounded polls, no wall-clock pacing -- deterministic + fast, like
// single_port_accept_test.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "luminumbra_common/net/LockstepSession.h"     // TcpListener / TcpTransport / Hello
#include "luminumbra_common/net/ReplicationEndpoint.h" // ReplicationServer::AddClient

namespace {

using namespace Luminumbra::Net;
using clock_type = std::chrono::steady_clock;

// Non-blockingly poll one transport for a single framed message within a bounded budget (real
// loopback sockets deliver asynchronously). Returns true and fills `out` on success.
bool ReceiveFrameWithin(ILockstepTransport& t,
                        std::vector<std::uint8_t>& out,
                        int max_tries = 400) {
    for (int i = 0; i < max_tries; ++i) {
        if (t.TryReceiveFrame(out))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// One client thread (mirrors the RunNetJoin rewire): connect to the single listen port, send a
// Hello DECLARING `declared_id`, then a trailing 1-byte marker (== declared_id's low byte) so the
// "Hello then usercmd stream" shape is exercised, and HOLD the connection open until `done` so the
// server-side peer stays connected for the asserts. Touches ONLY its own transport (one owning
// thread per socket -> race-free without a mutex, like net_soak_over_the_wire_test).
void RunDeclaringClient(std::uint16_t port,
                        std::uint32_t declared_id,
                        std::atomic<bool>* hello_sent,
                        std::atomic<bool>* done) {
    TcpTransport transport;
    const auto connect_deadline = clock_type::now() + std::chrono::seconds(10);
    bool connected = false;
    while (!done->load(std::memory_order_relaxed) && clock_type::now() < connect_deadline) {
        if (transport.Connect("127.0.0.1", port, /*timeout_ms=*/2000)) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // brief backoff, retry
    }
    if (!connected)
        return; // hello_sent stays false -> the test fails loudly (never hangs)

    HelloMsg hello;
    hello.seed = 424242; // shared default world; the id fan-out (not seed) is under test here
    hello.preset = "default";
    hello.tick_rate_hz = 30;
    hello.client_id = declared_id;
    const bool ok_hello = transport.SendFrame(EncodeHello(hello));
    const bool ok_marker =
        transport.SendFrame(std::vector<std::uint8_t>{static_cast<std::uint8_t>(declared_id)});
    hello_sent->store(ok_hello && ok_marker, std::memory_order_relaxed);

    while (!done->load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // keep the connection live
    }
    transport.Close();
}

} // namespace

TEST(SinglePortHostClientIntegration, ClientsFanIntoHelloDeclaredIdsOrderIndependent) {
    // Distinct, NON-sequential declared ids in a deliberately SHUFFLED launch order. If slot ids
    // came from ACCEPT ORDER (1,2,3,4 or a next_id++ counter) the server's id set could never be
    // {10,20,30,40}; set-equality proves each id is read from that client's Hello, not its rank.
    const std::vector<std::uint32_t> declared_order = {40, 10, 30, 20};
    const std::set<std::uint32_t> expected_ids(declared_order.begin(), declared_order.end());
    const int kNumClients = static_cast<int>(declared_order.size());

    // ONE listen socket on an OS-assigned ephemeral port (no fixed port -> no collision/flake).
    TcpListener listener;
    if (!listener.Listen(/*port=*/0, /*backlog=*/kNumClients + 4)) {
        GTEST_SKIP() << "TCP loopback listen unavailable on this platform";
    }
    ASSERT_TRUE(listener.IsListening());
    const std::uint16_t port = listener.port();
    ASSERT_NE(port, 0) << "ephemeral port should be resolved via getsockname";

    // Launch the client threads in the shuffled declared order, with a tiny stagger so accept
    // order tends to follow launch order -- further de-correlating declared id from accept rank.
    std::atomic<bool> done{false};
    std::vector<std::unique_ptr<std::atomic<bool>>> hello_sent;
    hello_sent.reserve(kNumClients);
    for (int i = 0; i < kNumClients; ++i)
        hello_sent.push_back(std::make_unique<std::atomic<bool>>(false));
    std::vector<std::thread> threads;
    threads.reserve(kNumClients);
    for (int i = 0; i < kNumClients; ++i) {
        threads.emplace_back(
            RunDeclaringClient, port, declared_order[i], hello_sent[i].get(), &done);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    // Accept all N from the ONE listen socket; read each Hello to learn the DECLARED id and fan it
    // into that slot via AddClient -- exactly the RunNetHost single-port rewire.
    ReplicationServer server;
    std::vector<std::unique_ptr<TcpTransport>> accepted;
    std::vector<std::uint32_t> accepted_ids;
    std::set<std::uint32_t> accepted_id_set;
    for (int i = 0; i < kNumClients; ++i) {
        std::unique_ptr<TcpTransport> t = listener.AcceptOneBlocking(/*timeout_ms=*/5000);
        ASSERT_NE(t, nullptr) << "expected to accept connection " << i
                              << " from the single listen socket";
        EXPECT_TRUE(t->IsPeerConnected())
            << "accepted transport " << i << " should have a live peer";

        std::vector<std::uint8_t> frame;
        ASSERT_TRUE(ReceiveFrameWithin(*t, frame))
            << "accepted connection " << i << " sent no Hello";
        HelloMsg hello;
        ASSERT_TRUE(DecodeHello(frame, hello))
            << "first frame on connection " << i << " was not a valid Hello";

        const std::uint32_t id = hello.client_id;
        EXPECT_EQ(accepted_id_set.count(id), 0u)
            << "declared id " << id << " collided -> a slot overwrite";
        server.AddClient(id, t.get()); // slot id from the HANDSHAKE, not the accept order
        accepted_ids.push_back(id);
        accepted_id_set.insert(id);
        accepted.push_back(std::move(t));
    }

    // (a) ORDER-INDEPENDENT id match: the accepted id SET equals the DECLARED set, regardless of
    //     the order the connections were accepted in (proves Hello-declared, not accept-order,
    //     ids).
    EXPECT_EQ(accepted_id_set, expected_ids);
    // (c) NO COLLISION: N distinct ids registered as N distinct slots.
    EXPECT_EQ(accepted_ids.size(), static_cast<std::size_t>(kNumClients));
    EXPECT_EQ(accepted_id_set.size(), static_cast<std::size_t>(kNumClients));
    ASSERT_EQ(server.client_count(), static_cast<std::size_t>(kNumClients));
    for (std::uint32_t id : expected_ids) {
        EXPECT_TRUE(server.has_client(id)) << "missing distinct client id " << id;
    }

    // (b) each accepted transport still has a LIVE PEER (the client threads hold their connections
    //     open until `done`).
    for (std::size_t i = 0; i < accepted.size(); ++i) {
        EXPECT_TRUE(accepted[i]->IsPeerConnected())
            << "slot " << accepted_ids[i] << " lost its peer";
    }

    // The post-Hello byte stream SURVIVES AddClient: the trailing marker (== the id's low byte) is
    // still buffered on the SAME transport, exactly as the real usercmd stream is drained by
    // PumpInbound after the host consumes the Hello pre-AddClient. This also pins each connection's
    // Hello + follow-on frame to the SAME stream (the marker matches that connection's declared
    // id).
    for (std::size_t i = 0; i < accepted.size(); ++i) {
        std::vector<std::uint8_t> marker;
        ASSERT_TRUE(ReceiveFrameWithin(*accepted[i], marker))
            << "slot " << accepted_ids[i] << " lost its post-Hello frame";
        ASSERT_EQ(marker.size(), 1u);
        EXPECT_EQ(marker[0], static_cast<std::uint8_t>(accepted_ids[i]))
            << "post-Hello marker did not match the Hello's declared id on the same connection";
    }

    // Every client actually connected and sent its Hello + marker.
    for (int i = 0; i < kNumClients; ++i) {
        EXPECT_TRUE(hello_sent[i]->load(std::memory_order_relaxed))
            << "client " << i << " never sent its Hello";
    }

    // Stop + join every client thread BEFORE tearing down the accepted transports (join is the
    // happens-before edge; sockets are the only cross-thread objects and the kernel synchronizes
    // them).
    done.store(true, std::memory_order_relaxed);
    for (std::thread& th : threads)
        th.join();
    listener.Close();
    for (auto& t : accepted)
        t->Close();
}
