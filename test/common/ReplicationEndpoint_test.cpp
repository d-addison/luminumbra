// T-I6 P3.1: server/client replication endpoints. Proves the full bidirectional
// loop over the in-process LoopbackTransport: client usercmd reaches the server
// (newest-wins), server snapshot reaches the client (most-recent-wins), and the
// client's ack is reflected back on the server. No sockets (the UDP transport is
// a drop-in for LoopbackTransport later).
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "luminumbra_common/net/ReplicationEndpoint.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/net/ReplicationProtocol.h"

namespace {

using namespace Luminumbra::Net;

ReplEntityState MakeEntity(std::uint32_t id, std::int32_t x, std::int32_t y, std::int32_t z) {
    ReplEntityState e;
    e.entity_id = id;
    e.px_mm = x; e.py_mm = y; e.pz_mm = z;
    return e;
}

TEST(ReplicationEndpoint, FullLoopOverLoopback) {
    auto pair = MakeLoopbackPair();
    ILockstepTransport* server_end = pair.first.get();
    ILockstepTransport* client_end = pair.second.get();

    ReplicationServer server;
    server.AddClient(/*client_id=*/1, server_end);
    ReplicationClient client(/*player_id=*/1, client_end);
    ASSERT_EQ(server.client_count(), 1u);

    // 1) Client sends a usercmd; server drains it (newest-wins).
    UsercmdMsg cmd;
    cmd.tick = 42;
    cmd.player_id = 1;
    cmd.move_x = 12345;
    cmd.action_bits = 0b10;
    client.SendUsercmd(cmd);
    server.PumpInbound();
    const UsercmdMsg* got = server.LatestUsercmd(1);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->tick, 42u);
    EXPECT_EQ(got->move_x, 12345);

    // 2) Server broadcasts a 3-entity snapshot; client applies it.
    std::vector<ReplEntityState> entities = {
        MakeEntity(1, 1000, 34000, -2000),
        MakeEntity(2, 3200, 34050, 1500),
        MakeEntity(3, -800, 33900, 4000),
    };
    server.BroadcastSnapshot(/*server_tick=*/100, entities);
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    EXPECT_EQ(client.snapshot().server_tick, 100u);
    EXPECT_EQ(client.snapshot().snapshot_seq, 1u);
    // The server folded the client's usercmd tick into acked_usercmd_tick.
    EXPECT_EQ(client.snapshot().acked_usercmd_tick, 42u);
    ASSERT_EQ(client.snapshot().entities.size(), 3u);
    EXPECT_EQ(client.snapshot().entities[1].entity_id, 2u);
    EXPECT_EQ(client.snapshot().entities[1].px_mm, 3200);

    // 3) The client's PumpInbound auto-acked; the server sees it after draining.
    server.PumpInbound();
    EXPECT_EQ(server.AckedSnapshotSeq(1), 1u);

    // 4) A second snapshot advances the seq.
    server.BroadcastSnapshot(/*server_tick=*/130, entities);
    client.PumpInbound();
    EXPECT_EQ(client.snapshot().snapshot_seq, 2u);
}

TEST(ReplicationEndpoint, MultiClientEachGetsOwnSeq) {
    auto pair_a = MakeLoopbackPair();
    auto pair_b = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(1, pair_a.first.get());
    server.AddClient(2, pair_b.first.get());
    ReplicationClient client_a(1, pair_a.second.get());
    ReplicationClient client_b(2, pair_b.second.get());

    std::vector<ReplEntityState> entities = {MakeEntity(1, 0, 0, 0)};
    server.BroadcastSnapshot(10, entities);
    client_a.PumpInbound();
    client_b.PumpInbound();
    EXPECT_TRUE(client_a.has_snapshot());
    EXPECT_TRUE(client_b.has_snapshot());
    EXPECT_EQ(client_a.snapshot().snapshot_seq, 1u);
    EXPECT_EQ(client_b.snapshot().snapshot_seq, 1u);

    // Only client A sends a usercmd -> only A's link reports it.
    UsercmdMsg cmd; cmd.tick = 7; cmd.player_id = 1;
    client_a.SendUsercmd(cmd);
    server.PumpInbound();
    EXPECT_NE(server.LatestUsercmd(1), nullptr);
    EXPECT_EQ(server.LatestUsercmd(2), nullptr);

    server.RemoveClient(2);
    EXPECT_EQ(server.client_count(), 1u);
}

TEST(ReplicationEndpoint, StaleSnapshotDoesNotRegress) {
    auto pair = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(1, pair.first.get());
    ReplicationClient client(1, pair.second.get());

    std::vector<ReplEntityState> e1 = {MakeEntity(1, 100, 0, 0)};
    std::vector<ReplEntityState> e2 = {MakeEntity(1, 200, 0, 0)};
    server.BroadcastSnapshot(10, e1); // seq 1
    server.BroadcastSnapshot(20, e2); // seq 2
    // Client drains both in one pump; most-recent-wins keeps seq 2.
    client.PumpInbound();
    EXPECT_EQ(client.snapshot().snapshot_seq, 2u);
    EXPECT_EQ(client.snapshot().entities[0].px_mm, 200);
}

} // namespace
