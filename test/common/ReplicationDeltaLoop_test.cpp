//  the ack-driven DELTA replication LOOP, end-to-end over the deterministic
// NetworkSim harness (injected latency / jitter / loss). This is the slice the critique
// gated behind a network-sim harness (single-PC constraint): the server sends each client
// only what changed since the snapshot it last ACKed, and the client reconstructs the full
// authoritative set. The tests assert: (1) delta mode reconstructs EXACTLY the full set the
// server holds; (2) it stays correct under heavy packet loss (the ack-driven baseline makes
// a dropped delta self-healing); (3) idle frames cost far less than the first full snapshot
// (the bandwidth win); (4) the whole exchange is deterministic (run == replay).
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "net/NetworkSim.h"
#include "net/ReplicationEndpoint.h"
#include "net/ReplicationProtocol.h"

namespace {

using namespace Luminumbra::Net;

// A small authoritative world: kN entities, one of which marches along +x each tick so
// successive snapshots genuinely differ (a real delta to send), the rest hold still.
constexpr std::uint32_t kClientId = 1;
constexpr int kN = 6;

std::vector<ReplEntityState> AuthoritativeAt(int tick) {
    std::vector<ReplEntityState> set;
    set.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        ReplEntityState e;
        e.entity_id = static_cast<std::uint32_t>(i + 1); // ids 1..kN (1 == the client avatar)
        e.px_mm = (i == 2) ? tick * 50 : i * 1000;       // entity #3 moves; others static
        e.py_mm = 0;
        e.pz_mm = i * 250;
        e.yaw_mrad = static_cast<std::int16_t>(i * 10);
        set.push_back(e);
    }
    return set;
}

// Drive one full server->client->ack round, advancing the sim clock enough to deliver in
// both directions under the configured latency/jitter.
void PumpRound(NetworkSim& sim, ReplicationClient& client, ReplicationServer& server) {
    for (int k = 0; k < 6; ++k) {
        sim.Tick();
        client.PumpInbound(); // receive snapshot(s), reconstruct, send ack
        sim.Tick();
        server.PumpInbound(); // fold in acks
    }
}

// The reconstructed client set must equal the authoritative set the server broadcast. Both
// are emitted in canonical entity_id order, so a direct vector compare is exact.
void ExpectConverged(const ReplicationClient& client, int tick) {
    ASSERT_TRUE(client.has_snapshot());
    const std::vector<ReplEntityState>& got = client.snapshot().entities;
    const std::vector<ReplEntityState> want = AuthoritativeAt(tick);
    ASSERT_EQ(got.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i)
        EXPECT_TRUE(got[i] == want[i]) << "entity " << i;
}

// Clean link: delta mode reconstructs the exact authoritative set every tick.
TEST(ReplicationDeltaLoop, ReconstructsFullSetOnCleanLink) {
    NetworkConditions cond;
    cond.latency_ticks = 1;
    cond.seed = 11;
    NetworkSim sim(cond);

    ReplicationServer server;
    server.SetDeltaCompression(true);
    server.AddClient(kClientId, &sim.A());
    ReplicationClient client(kClientId, &sim.B());

    int last = 0;
    for (int t = 0; t < 30; ++t) {
        server.BroadcastSnapshot(static_cast<std::uint64_t>(t), AuthoritativeAt(t));
        PumpRound(sim, client, server);
        last = t;
    }
    ExpectConverged(client, last);
    // The server must have actually used the acked baseline (a true delta), not full sends.
    EXPECT_GT(server.AckedSnapshotSeq(kClientId), 0u);
}

// Heavy packet loss: a dropped delta is recovered by the next one because the server keeps
// deltaing against the last-ACKED baseline. The client still converges to the truth.
TEST(ReplicationDeltaLoop, ConvergesUnderHeavyLoss) {
    NetworkConditions cond;
    cond.latency_ticks = 1;
    cond.jitter_ticks = 1;
    cond.drop_one_in = 2; // ~50% of unreliable frames dropped
    cond.seed = 99;
    NetworkSim sim(cond);

    ReplicationServer server;
    server.SetDeltaCompression(true);
    server.AddClient(kClientId, &sim.A());
    ReplicationClient client(kClientId, &sim.B());

    int last = 0;
    for (int t = 0; t < 80; ++t) {
        server.BroadcastSnapshot(static_cast<std::uint64_t>(t), AuthoritativeAt(t));
        PumpRound(sim, client, server);
        last = t;
    }
    // Drain a few quiet rounds so the final state propagates through the lossy link.
    for (int q = 0; q < 12; ++q) {
        server.BroadcastSnapshot(static_cast<std::uint64_t>(last), AuthoritativeAt(last));
        PumpRound(sim, client, server);
    }
    ExpectConverged(client, last);
}

// The bandwidth win: once the client has acked a baseline, an UNCHANGED world costs only a
// tiny header-sized delta, far less than the first full snapshot of all kN entities.
TEST(ReplicationDeltaLoop, IdleDeltaIsFarSmallerThanFullSnapshot) {
    NetworkConditions cond;
    cond.latency_ticks = 1;
    cond.seed = 5;
    NetworkSim sim(cond);

    ReplicationServer server;
    server.SetDeltaCompression(true);
    server.AddClient(kClientId, &sim.A());
    ReplicationClient client(kClientId, &sim.B());

    // A STATIC world (entity #3 frozen too) so post-baseline deltas carry zero entities.
    const std::vector<ReplEntityState> world = AuthoritativeAt(7);

    server.BroadcastSnapshot(0, world); // first send: full (no baseline yet)
    const std::size_t full_bytes = server.last_broadcast_max_client_bytes();
    PumpRound(sim, client, server); // client acks the full baseline

    // Now the server has an acked baseline; an unchanged broadcast is a near-empty delta.
    server.BroadcastSnapshot(1, world);
    const std::size_t idle_bytes = server.last_broadcast_max_client_bytes();
    PumpRound(sim, client, server);

    EXPECT_GT(full_bytes, 0u);
    EXPECT_LT(idle_bytes, full_bytes / 2)
        << "idle delta " << idle_bytes << " not much smaller than full " << full_bytes;
    ExpectConverged(client, 7); // still correct despite sending almost nothing
}

// Determinism: identical conditions + sequence -> byte-identical reconstructed state.
TEST(ReplicationDeltaLoop, DeterministicRunEqualsReplay) {
    auto run = [] {
        NetworkConditions cond;
        cond.latency_ticks = 2;
        cond.jitter_ticks = 1;
        cond.drop_one_in = 3;
        cond.seed = 4242;
        NetworkSim sim(cond);
        ReplicationServer server;
        server.SetDeltaCompression(true);
        server.AddClient(kClientId, &sim.A());
        ReplicationClient client(kClientId, &sim.B());
        for (int t = 0; t < 40; ++t) {
            server.BroadcastSnapshot(static_cast<std::uint64_t>(t), AuthoritativeAt(t));
            PumpRound(sim, client, server);
        }
        return client.snapshot().entities;
    };
    const std::vector<ReplEntityState> a = run();
    const std::vector<ReplEntityState> b = run();
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        EXPECT_TRUE(a[i] == b[i]);
}

// Full-snapshot mode (delta OFF) is unchanged: the client still reconstructs the full set
// (delta_from_seq==0 path), proving the new client logic is backward-compatible.
TEST(ReplicationDeltaLoop, FullSnapshotModeStillWorks) {
    NetworkConditions cond;
    cond.latency_ticks = 1;
    cond.seed = 1;
    NetworkSim sim(cond);
    ReplicationServer server; // delta compression OFF (default)
    server.AddClient(kClientId, &sim.A());
    ReplicationClient client(kClientId, &sim.B());

    int last = 0;
    for (int t = 0; t < 10; ++t) {
        server.BroadcastSnapshot(static_cast<std::uint64_t>(t), AuthoritativeAt(t));
        PumpRound(sim, client, server);
        last = t;
    }
    EXPECT_FALSE(server.delta_compression());
    ExpectConverged(client, last);
}

} // namespace
