// T-I6 P3.1: server/client replication endpoints. Proves the full bidirectional
// loop over the in-process LoopbackTransport: client usercmd reaches the server
// (newest-wins), server snapshot reaches the client (most-recent-wins), and the
// client's ack is reflected back on the server. No sockets (the UDP transport is
// a drop-in for LoopbackTransport later).
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "luminumbra_common/net/ReplicationEndpoint.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/net/ReplicationProtocol.h"
#include "luminumbra_common/world/PlayerAvatar.h"

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

// P3.1b: the avatar -> replication-state bridge, end-to-end over the transport.
// Server projects its authoritative PlayerAvatars to ReplEntityState, broadcasts;
// the client receives + the dequantized positions match the server avatars.
TEST(ReplicationEndpoint, ServerAvatarsReplicateToClient) {
    using Luminumbra::World::PlayerAvatar;
    using Luminumbra::World::BuildAvatarReplStates;

    std::vector<PlayerAvatar> avatars(3);
    avatars[0].player_id = 0; avatars[0].position = Luminumbra::Vec3(8.0f, 35.4f, 8.0f);  avatars[0].facing = 0.0f;
    avatars[1].player_id = 1; avatars[1].position = Luminumbra::Vec3(11.1f, 35.6f, 7.2f); avatars[1].facing = 1.57f;
    avatars[2].player_id = 2; avatars[2].position = Luminumbra::Vec3(6.4f, 35.2f, 10.8f); avatars[2].facing = -2.3f;

    const std::vector<ReplEntityState> states = BuildAvatarReplStates(avatars);
    ASSERT_EQ(states.size(), 3u);

    auto pair = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(1, pair.first.get());
    ReplicationClient client(1, pair.second.get());

    server.BroadcastSnapshot(/*server_tick=*/50, states);
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    const SnapshotMsg& snap = client.snapshot();
    ASSERT_EQ(snap.entities.size(), 3u);
    for (std::size_t i = 0; i < avatars.size(); ++i) {
        EXPECT_EQ(snap.entities[i].entity_id, avatars[i].player_id);
        // Dequantized client position matches the server avatar within mm tolerance.
        EXPECT_NEAR(ReplDequantPos(snap.entities[i].px_mm), avatars[i].position.x, 0.001f) << "avatar " << i;
        EXPECT_NEAR(ReplDequantPos(snap.entities[i].py_mm), avatars[i].position.y, 0.001f) << "avatar " << i;
        EXPECT_NEAR(ReplDequantPos(snap.entities[i].pz_mm), avatars[i].position.z, 0.001f) << "avatar " << i;
        EXPECT_NEAR(ReplDequantAngle(snap.entities[i].yaw_mrad), avatars[i].facing, 0.001f) << "avatar " << i;
    }
}

// P3.2: area-of-interest scoping. With a radius set, each client receives only
// entities near its OWN avatar (+ always its own), so a crowded world does not
// broadcast everyone to everyone.
TEST(ReplicationEndpoint, AoiScopesSnapshotPerClient) {
    auto pair_a = MakeLoopbackPair();
    auto pair_b = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(1, pair_a.first.get());
    server.AddClient(2, pair_b.first.get());
    ReplicationClient client_a(1, pair_a.second.get());
    ReplicationClient client_b(2, pair_b.second.get());

    // Radius 5 m. Client 1's avatar (id 1) at the origin; client 2's avatar (id 2)
    // 100 m away. A "near-1" entity (id 10) sits 2 m from avatar 1; a "near-2"
    // entity (id 20) sits 2 m from avatar 2.
    server.SetAoiRadiusMm(5000);
    std::vector<ReplEntityState> entities = {
        MakeEntity(1, 0, 0, 0),            // client 1's avatar
        MakeEntity(2, 100000, 0, 0),       // client 2's avatar (100 m away)
        MakeEntity(10, 2000, 0, 0),        // near avatar 1
        MakeEntity(20, 102000, 0, 0),      // near avatar 2
    };
    server.BroadcastSnapshot(10, entities);
    client_a.PumpInbound();
    client_b.PumpInbound();

    auto ids = [](const SnapshotMsg& s) {
        std::vector<std::uint32_t> v;
        for (const auto& e : s.entities) v.push_back(e.entity_id);
        std::sort(v.begin(), v.end());
        return v;
    };
    ASSERT_TRUE(client_a.has_snapshot());
    ASSERT_TRUE(client_b.has_snapshot());
    // Client 1 sees its own avatar (1) + the near entity (10); NOT the far ones.
    EXPECT_EQ(ids(client_a.snapshot()), (std::vector<std::uint32_t>{1, 10}));
    // Client 2 sees its own avatar (2) + the near entity (20).
    EXPECT_EQ(ids(client_b.snapshot()), (std::vector<std::uint32_t>{2, 20}));
}

TEST(ReplicationEndpoint, AoiDisabledByDefaultSendsAll) {
    auto pair = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(1, pair.first.get());
    ReplicationClient client(1, pair.second.get());
    std::vector<ReplEntityState> entities = {
        MakeEntity(1, 0, 0, 0), MakeEntity(2, 999000, 0, 0), MakeEntity(3, -999000, 0, 0),
    };
    server.BroadcastSnapshot(1, entities); // radius 0 -> disabled
    client.PumpInbound();
    EXPECT_EQ(client.snapshot().entities.size(), 3u);
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
