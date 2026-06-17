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

// T-I6 polish: CHUNK-INDEX AOI. With a 16 m chunk and radius 1 chunk, each client
// sees only entities in the 3x3 chunk neighbourhood of its own avatar's chunk.
TEST(ReplicationEndpoint, ChunkAoiScopesToNeighbourhood) {
    auto pair_a = MakeLoopbackPair();
    auto pair_b = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(1, pair_a.first.get());
    server.AddClient(2, pair_b.first.get());
    ReplicationClient client_a(1, pair_a.second.get());
    ReplicationClient client_b(2, pair_b.second.get());

    // 16 m chunk edge (16000 mm), radius 1 chunk -> +/-1 chunk in X/Z about centre.
    server.SetAoiChunkRadius(/*chunk_radius=*/1, /*chunk_size_mm=*/16000);
    std::vector<ReplEntityState> entities = {
        MakeEntity(1, 0, 0, 0),         // client 1 avatar -> chunk (0,0)
        MakeEntity(2, 320000, 0, 0),    // client 2 avatar -> chunk (20,0), 320 m away
        MakeEntity(10, 20000, 0, 8000), // chunk (1,0): adjacent to avatar 1 -> in
        MakeEntity(11, 40000, 0, 0),    // chunk (2,0): two chunks from avatar 1 -> out
        MakeEntity(20, 312000, 0, 0),   // chunk (19,0): adjacent to avatar 2 -> in
    };
    server.BroadcastSnapshot(10, entities);
    client_a.PumpInbound();
    client_b.PumpInbound();

    auto ids = [](const SnapshotMsg& s) {
        std::vector<std::uint32_t> v;
        for (const auto& e : s.entities) v.push_back(e.entity_id);
        return v; // chunk-AOI emits sorted by entity_id already
    };
    ASSERT_TRUE(client_a.has_snapshot());
    ASSERT_TRUE(client_b.has_snapshot());
    EXPECT_EQ(ids(client_a.snapshot()), (std::vector<std::uint32_t>{1, 10})); // not 11/2/20
    EXPECT_EQ(ids(client_b.snapshot()), (std::vector<std::uint32_t>{2, 20}));
}

// Chunk-AOI radius 0 -> only the avatar's own chunk (self + co-located entities).
TEST(ReplicationEndpoint, ChunkAoiRadiusZeroIsOwnChunkOnly) {
    auto pair = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(1, pair.first.get());
    ReplicationClient client(1, pair.second.get());
    server.SetAoiChunkRadius(0, 16000);
    std::vector<ReplEntityState> entities = {
        MakeEntity(1, 1000, 0, 1000),   // chunk (0,0)
        MakeEntity(10, 2000, 0, 2000),  // chunk (0,0): same chunk -> in
        MakeEntity(11, 20000, 0, 0),    // chunk (1,0): adjacent -> out at radius 0
    };
    server.BroadcastSnapshot(1, entities);
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    EXPECT_EQ(client.snapshot().entities.size(), 2u);
    EXPECT_EQ(client.snapshot().entities[0].entity_id, 1u);
    EXPECT_EQ(client.snapshot().entities[1].entity_id, 10u);
}

// T-I6 SCALE/STRESS: the 20+ player scalability contract. 64 players spread on an
// 8x8 grid 100 m apart; with a 16 m chunk + radius 1 (a 48 m neighbourhood) almost
// no two share a neighbourhood, so chunk-AOI must keep each client's snapshot ~1
// entity REGARDLESS of population, while a full-set broadcast grows linearly with
// N. Proves per-connection bandwidth (and aggregate server egress) is bounded by
// LOCAL density, not headcount -- the property that lets a single server hold the
// 20-32+ players the sizing target calls for.
TEST(ReplicationScale, ChunkAoiBoundsPerClientBandwidthAsPlayersScale) {
    constexpr int N = 64;
    constexpr std::int32_t kSpacingMm = 100000; // 100 m, >> 48 m neighbourhood
    std::vector<decltype(MakeLoopbackPair())> pairs;
    pairs.reserve(N);
    ReplicationServer server;
    std::vector<ReplEntityState> entities;
    entities.reserve(N);
    for (int i = 0; i < N; ++i) {
        pairs.push_back(MakeLoopbackPair());
        const std::uint32_t id = static_cast<std::uint32_t>(i + 1);
        server.AddClient(id, pairs.back().first.get());
        entities.push_back(MakeEntity(id, (i % 8) * kSpacingMm, 0, (i / 8) * kSpacingMm));
    }
    ASSERT_EQ(server.client_count(), static_cast<std::size_t>(N));

    server.SetAoiChunkRadius(/*chunk_radius=*/1, /*chunk_size_mm=*/16000);
    server.BroadcastSnapshot(1, entities);
    const std::size_t aoi_max = server.last_broadcast_max_client_bytes();
    const std::size_t aoi_total = server.last_broadcast_total_bytes();

    server.SetAoiChunkRadius(/*disable=*/-1, 0); // full set: each client gets all N
    server.BroadcastSnapshot(2, entities);
    const std::size_t full_max = server.last_broadcast_total_bytes() == 0 ? 0
                                  : server.last_broadcast_max_client_bytes();
    const std::size_t full_total = server.last_broadcast_total_bytes();

    // Per-connection AOI bytes are a small fraction of the full-set bytes, and the
    // aggregate egress collapses (full = N clients x N entities).
    EXPECT_LT(aoi_max * 8, full_max);
    EXPECT_LT(aoi_total * 8, full_total);

    // Determinism at scale: re-broadcasting identical state is byte-identical.
    server.SetAoiChunkRadius(1, 16000);
    server.BroadcastSnapshot(3, entities);
    EXPECT_EQ(server.last_broadcast_max_client_bytes(), aoi_max);
    EXPECT_EQ(server.last_broadcast_total_bytes(), aoi_total);
}

// T-I6 polish: PRUNE-INTO-TICK despawn. A disconnect is folded into the very next
// broadcast's removed_ids for surviving clients, and repeated for robustness.
TEST(ReplicationLifecycle, PruneFoldsDespawnIntoNextSnapshot) {
    auto pa = MakeLoopbackPair();
    auto pb = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(1, pa.first.get());
    server.AddClient(2, pb.first.get());
    ReplicationClient client_a(1, pa.second.get());
    std::vector<ReplEntityState> entities = {MakeEntity(1, 0, 0, 0), MakeEntity(2, 1000, 0, 0)};

    // Client 2 leaves; prune detects it and enqueues its despawn.
    pb.second->Close();
    server.PumpInbound();
    auto removed = server.PruneDisconnectedClients();
    ASSERT_EQ(removed.size(), 1u);

    // The NEXT broadcast tells the survivor to despawn entity 2 -- same tick.
    server.BroadcastSnapshot(20, entities);
    client_a.PumpInbound();
    ASSERT_TRUE(client_a.has_snapshot());
    const auto& rem = client_a.snapshot().removed_ids;
    EXPECT_NE(std::find(rem.begin(), rem.end(), 2u), rem.end());

    // Repeated across the next couple of unreliable snapshots (drop-robust), then stops.
    server.BroadcastSnapshot(21, entities);
    client_a.PumpInbound();
    const auto& rem2 = client_a.snapshot().removed_ids;
    EXPECT_NE(std::find(rem2.begin(), rem2.end(), 2u), rem2.end());

    server.BroadcastSnapshot(22, entities);
    client_a.PumpInbound();
    server.BroadcastSnapshot(23, entities); // 4th broadcast: repeat count (3) exhausted
    client_a.PumpInbound();
    const auto& rem4 = client_a.snapshot().removed_ids;
    EXPECT_EQ(std::find(rem4.begin(), rem4.end(), 2u), rem4.end());
}

// Caller-supplied removed_ids still flow (and merge with pending) deterministically.
TEST(ReplicationLifecycle, CallerRemovedIdsStillDelivered) {
    auto pair = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(1, pair.first.get());
    ReplicationClient client(1, pair.second.get());
    server.BroadcastSnapshot(1, {MakeEntity(1, 0, 0, 0)}, {2000u});
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    const auto& rem = client.snapshot().removed_ids;
    EXPECT_NE(std::find(rem.begin(), rem.end(), 2000u), rem.end());
}

// P3.3: client-side remote-entity interpolation (render-behind lerp).
TEST(SnapshotInterpolation, LerpsBetweenSnapshots) {
    SnapshotInterpolator interp;
    SnapshotMsg s0; s0.server_tick = 0;  s0.entities = {MakeEntity(1, 0, 1000, 0)};
    SnapshotMsg s1; s1.server_tick = 10; s1.entities = {MakeEntity(1, 1000, 1000, 2000)};
    interp.Push(s1); // out-of-order push tolerated
    interp.Push(s0);
    EXPECT_EQ(interp.buffered(), 2u);
    EXPECT_EQ(interp.newest_tick(), 10u);

    // Halfway (tick 5) -> position halfway.
    auto mid = interp.Sample(5.0);
    ASSERT_EQ(mid.size(), 1u);
    EXPECT_EQ(mid[0].entity_id, 1u);
    EXPECT_NEAR(mid[0].px_mm, 500, 1);
    EXPECT_NEAR(mid[0].pz_mm, 1000, 1);

    // Quarter (tick 2.5) -> 25%.
    EXPECT_NEAR(interp.Sample(2.5)[0].px_mm, 250, 1);
}

TEST(SnapshotInterpolation, ClampsOutsideRangeNoExtrapolation) {
    SnapshotInterpolator interp;
    SnapshotMsg s0; s0.server_tick = 10; s0.entities = {MakeEntity(1, 100, 0, 0)};
    SnapshotMsg s1; s1.server_tick = 20; s1.entities = {MakeEntity(1, 200, 0, 0)};
    interp.Push(s0);
    interp.Push(s1);
    // Before the buffer -> oldest; after -> newest (no extrapolation past 200).
    EXPECT_EQ(interp.Sample(5.0)[0].px_mm, 100);
    EXPECT_EQ(interp.Sample(99.0)[0].px_mm, 200);
}

TEST(SnapshotInterpolation, NewEntityPassesThroughUntilInBoth) {
    SnapshotInterpolator interp;
    SnapshotMsg s0; s0.server_tick = 0;  s0.entities = {MakeEntity(1, 0, 0, 0)};
    SnapshotMsg s1; s1.server_tick = 10; s1.entities = {MakeEntity(1, 1000, 0, 0), MakeEntity(2, 5000, 0, 0)};
    interp.Push(s0);
    interp.Push(s1);
    auto mid = interp.Sample(5.0);
    // Entity 1 (in both) is lerped; entity 2 (only in the newer) passes through.
    ASSERT_EQ(mid.size(), 2u);
    const ReplEntityState* e2 = nullptr;
    for (const auto& e : mid) if (e.entity_id == 2u) e2 = &e;
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(e2->px_mm, 5000);
}

TEST(SnapshotInterpolation, EmptyAndEviction) {
    SnapshotInterpolator interp(/*max_buffer=*/3);
    EXPECT_TRUE(interp.empty());
    for (std::uint32_t t = 0; t < 6; ++t) {
        SnapshotMsg s; s.server_tick = t; interp.Push(s);
    }
    EXPECT_EQ(interp.buffered(), 3u);   // capped
    EXPECT_EQ(interp.newest_tick(), 5u); // newest kept
}

// P3.3: local-player prediction + reconciliation.
TEST(LocalPlayerPrediction, PredictsImmediatelyAndReconciles) {
    LocalPlayerPredictor pred(/*speed*/4.0f, /*dt*/0.1f); // 0.4 m per full-axis tick
    pred.SetPosition(0.0f, 0.0f, 0.0f);
    pred.RecordInput(1, 1.0f, 0.0f);
    pred.RecordInput(2, 1.0f, 0.0f);
    pred.RecordInput(3, 1.0f, 0.0f);
    // Predicted immediately: 3 * 0.4 = 1.2 m in X.
    EXPECT_NEAR(pred.predicted().x, 1.2f, 1e-4f);
    EXPECT_EQ(pred.pending_inputs(), 3u);

    // Server acks tick 1 with the matching authoritative position (0.4). Reconcile
    // drops cmd1, snaps to 0.4, replays cmds 2+3 -> back to 1.2 (server agreed).
    pred.Reconcile(0.4f, 0.0f, 0.0f, /*acked*/1);
    EXPECT_NEAR(pred.predicted().x, 1.2f, 1e-4f);
    EXPECT_EQ(pred.pending_inputs(), 2u);
}

TEST(LocalPlayerPrediction, SnapsToAuthoritativeOnDivergence) {
    LocalPlayerPredictor pred(4.0f, 0.1f);
    pred.SetPosition(0.0f, 0.0f, 0.0f);
    pred.RecordInput(1, 1.0f, 0.0f); // predicts 0.4
    pred.RecordInput(2, 1.0f, 0.0f); // predicts 0.8
    // Server says after tick 1 the avatar was actually at x=0.2 (blocked/slope),
    // acks tick 1. Reconcile snaps to 0.2 + replays cmd2 (0.4) -> 0.6.
    pred.Reconcile(0.2f, 0.0f, 0.0f, 1);
    EXPECT_NEAR(pred.predicted().x, 0.6f, 1e-4f);
    EXPECT_EQ(pred.pending_inputs(), 1u);
}

TEST(LocalPlayerPrediction, FullAckClearsBufferAndMatchesAuthoritative) {
    LocalPlayerPredictor pred(4.0f, 0.1f);
    pred.RecordInput(1, 1.0f, 0.0f);
    pred.RecordInput(2, 0.0f, 1.0f);
    // Server acks through tick 2 -> all inputs folded in; predicted == authoritative.
    pred.Reconcile(0.4f, 0.0f, 0.4f, 2);
    EXPECT_EQ(pred.pending_inputs(), 0u);
    EXPECT_NEAR(pred.predicted().x, 0.4f, 1e-4f);
    EXPECT_NEAR(pred.predicted().z, 0.4f, 1e-4f);
}

// P4: persistent-server join/leave lifecycle.
TEST(ReplicationLifecycle, JoinLeaveDoesNotDisturbSurvivors) {
    auto pa = MakeLoopbackPair();
    auto pb = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(1, pa.first.get());
    server.AddClient(2, pb.first.get());
    ReplicationClient client_a(1, pa.second.get());
    std::vector<ReplEntityState> entities = {MakeEntity(1, 0, 0, 0)};

    server.BroadcastSnapshot(10, entities);
    client_a.PumpInbound();
    EXPECT_EQ(client_a.snapshot().snapshot_seq, 1u);
    EXPECT_EQ(server.client_count(), 2u);

    // Client 2 LEAVES (its transport end closes).
    pb.second->Close();
    server.PumpInbound();                       // drain anything pending
    auto removed = server.PruneDisconnectedClients();
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0], 2u);
    EXPECT_EQ(server.client_count(), 1u);
    EXPECT_FALSE(server.has_client(2));

    // Survivor (client 1) is unaffected -- keeps receiving, seq advances.
    server.BroadcastSnapshot(20, entities);
    client_a.PumpInbound();
    EXPECT_EQ(client_a.snapshot().snapshot_seq, 2u);

    // A NEW client JOINS mid-session and gets its own fresh seq (baseline).
    auto pc = MakeLoopbackPair();
    server.AddClient(3, pc.first.get());
    ReplicationClient client_c(3, pc.second.get());
    server.BroadcastSnapshot(30, entities);
    client_a.PumpInbound();
    client_c.PumpInbound();
    EXPECT_EQ(client_a.snapshot().snapshot_seq, 3u); // survivor continues
    ASSERT_TRUE(client_c.has_snapshot());
    EXPECT_EQ(client_c.snapshot().snapshot_seq, 1u); // joiner starts fresh
}

TEST(ReplicationLifecycle, ConnectedClientNotPruned) {
    auto pa = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(1, pa.first.get());
    ReplicationClient client_a(1, pa.second.get());
    server.BroadcastSnapshot(1, {MakeEntity(1, 0, 0, 0)});
    client_a.PumpInbound(); // acks back -> still connected
    server.PumpInbound();
    EXPECT_TRUE(server.PruneDisconnectedClients().empty());
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
