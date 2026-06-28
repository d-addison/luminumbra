// spec-019 FR-C (Wave 019-C1): 32-CLIENT REPLICATION SOAK gate.
//
// The MULTIPROCESS soak harness (1 authoritative server + N client PROCESSES over
// real TCP) is the runnable validation path -- `luminumbra_server_app --net-soak`
// + N `--net-soak-client` (see main_server.cpp / docs). It boots the full world,
// binds sockets, and times a sustained run; that path is wall-clock + OS-port +
// process-orchestration bound, so it is NOT a deterministic ctest (the standing
// constraint: don't gate on multiprocess timing -- it flakes).
//
// THIS gate is the deterministic in-process equivalent: it drives the SAME
// ReplicationServer / ReplicationClient endpoint stack across N=32 clients over the
// in-process LoopbackTransport (no sockets, no ports, no wall-clock), runs a
// sustained tick budget, and ASSERTS -- not merely logs -- that the per-connection
// backpressure (QueueDepthP95), snapshot-aging (SnapshotAgeP95) and per-client
// bandwidth (last_broadcast_max_client_bytes) stay within an explicit budget over
// the whole run, plus clean disconnect/reconnect UNDER LOAD with the surviving
// clients unaffected. Engine-generic, world_hash-neutral (transport-side glue).

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "luminumbra_common/net/ReplicationEndpoint.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/net/ReplicationProtocol.h"

namespace {

using namespace Luminumbra::Net;

// --- Soak budgets (spec-019 FR-E-003). Explicit numbers the gate FAILS on. -----------
// 32 clients, each a player avatar entity; with the chunk-AOI neighbourhood every client
// sees the (clustered) full set, so the per-client snapshot is ~32 small fixed-point
// entities + header. 16 KiB is ~16x that headroom but catches a runaway (e.g. AOI that
// stops scoping). A healthy loopback peer accepts every frame immediately, so the queue
// drains fully each broadcast (depth 0) and the ack returns within one tick (age <= ~1).
constexpr std::uint32_t kSoakClients          = 32;
constexpr std::uint64_t kSoakTicks            = 300;   // 10 s of sim at 30 Hz
constexpr std::uint32_t kQueueDepthP95Budget  = 2;     // healthy peers drain immediately
constexpr std::uint32_t kSnapshotAgeP95Budget = 3;     // ack lag is ~1 tick
constexpr std::size_t   kMaxClientBytesBudget = 16u * 1024u;
constexpr std::int64_t  kChunkSizeMm          = 32 * 1000; // 32 m streaming chunk

// One simulated connection: a loopback pair (server end + client end) plus the client
// endpoint that mirrors snapshots and acks them.
struct SimClient {
    std::uint32_t id = 0;
    std::unique_ptr<LoopbackTransport> server_end;
    std::unique_ptr<LoopbackTransport> client_end;
    std::unique_ptr<ReplicationClient> client;
};

SimClient MakeSimClient(ReplicationServer& server, std::uint32_t id) {
    SimClient sc;
    sc.id = id;
    auto pair = MakeLoopbackPair();
    sc.server_end = std::move(pair.first);
    sc.client_end = std::move(pair.second);
    server.AddClient(id, sc.server_end.get());
    sc.client = std::make_unique<ReplicationClient>(id, sc.client_end.get());
    return sc;
}

// Deterministic authoritative avatar set for tick `t`: N avatars on a ring drifting in X.
std::vector<ReplEntityState> AvatarStates(std::uint32_t n, std::uint64_t t) {
    std::vector<ReplEntityState> states;
    states.reserve(n);
    for (std::uint32_t i = 1; i <= n; ++i) {
        ReplEntityState e;
        e.entity_id = i;
        // Clustered near origin (within one chunk) so the chunk-AOI gather includes the
        // full set -- the worst case for per-client snapshot bytes.
        e.px_mm = static_cast<std::int32_t>((i * 250) + static_cast<std::int32_t>(t));
        e.py_mm = 1000;
        e.pz_mm = static_cast<std::int32_t>(i * 250);
        e.yaw_mrad = static_cast<std::int16_t>(i * 7);
        e.flags = 1; // grounded
        states.push_back(e);
    }
    return states;
}

// Drives one tick of the full bidirectional loop for the supplied live clients:
//   clients send usercmds -> server drains -> server broadcasts -> clients apply+ack
//   -> server drains acks (so snapshot-age stays bounded).
void StepTick(ReplicationServer& server, std::vector<SimClient*>& live, std::uint64_t tick) {
    for (SimClient* sc : live) {
        UsercmdMsg cmd;
        cmd.tick = tick;
        cmd.player_id = sc->id;
        cmd.move_x = 32767; // walk +X
        sc->client->SendUsercmd(cmd);
    }
    server.PumpInbound();
    server.BroadcastSnapshot(tick, AvatarStates(kSoakClients, tick));
    for (SimClient* sc : live) {
        sc->client->PumpInbound(); // apply snapshot + send ack
    }
    server.PumpInbound(); // drain the acks so snapshot-age does not climb
}

// 32 clients, sustained run, every per-connection metric stays within budget the WHOLE
// run -- the core soak assertion.
TEST(ReplicationScale, SustainsThirtyTwoClientsWithinBudget) {
    ReplicationServer server;
    server.SetAoiChunkRadius(/*chunk_radius=*/3, kChunkSizeMm);

    std::vector<SimClient> clients;
    clients.reserve(kSoakClients);
    for (std::uint32_t id = 1; id <= kSoakClients; ++id) {
        clients.push_back(MakeSimClient(server, id));
    }
    ASSERT_EQ(server.client_count(), kSoakClients);

    std::vector<SimClient*> live;
    for (SimClient& c : clients) live.push_back(&c);

    std::uint32_t worst_queue_p95 = 0;
    std::uint32_t worst_age_p95 = 0;
    std::size_t worst_client_bytes = 0;

    for (std::uint64_t t = 1; t <= kSoakTicks; ++t) {
        StepTick(server, live, t);

        // Sample the across-connected-clients p95 + per-client bandwidth EVERY tick and
        // hold the high-water; the gate fails if any sample breaches budget.
        worst_queue_p95 = std::max(worst_queue_p95, server.QueueDepthP95());
        worst_age_p95 = std::max(worst_age_p95, server.SnapshotAgeP95());
        worst_client_bytes = std::max(worst_client_bytes, server.last_broadcast_max_client_bytes());

        EXPECT_LE(server.QueueDepthP95(), kQueueDepthP95Budget)
            << "queue-depth p95 over budget at tick " << t;
        EXPECT_LE(server.SnapshotAgeP95(), kSnapshotAgeP95Budget)
            << "snapshot-age p95 over budget at tick " << t;
        EXPECT_LE(server.last_broadcast_max_client_bytes(), kMaxClientBytesBudget)
            << "per-client snapshot bytes over budget at tick " << t;
    }

    // Every client mirrored the authoritative set and kept up.
    for (const SimClient& c : clients) {
        EXPECT_TRUE(c.client->has_snapshot()) << "client " << c.id << " never got a snapshot";
        EXPECT_EQ(c.client->snapshot().entities.size(), kSoakClients)
            << "client " << c.id << " did not mirror the full avatar set";
        EXPECT_EQ(server.OutboundQueueDepth(c.id), 0u) << "client " << c.id << " left backed up";
        EXPECT_EQ(server.DroppedFrames(c.id), 0u) << "client " << c.id << " dropped frames";
    }

    // Sustained-rate proxy: every client's last-acked snapshot seq reached the run length
    // (no connection silently fell behind over the soak).
    for (const SimClient& c : clients) {
        EXPECT_GE(server.AckedSnapshotSeq(c.id), kSoakTicks - 1)
            << "client " << c.id << " fell behind (acked seq "
            << server.AckedSnapshotSeq(c.id) << ")";
    }

    RecordProperty("clients", kSoakClients);
    RecordProperty("ticks", kSoakTicks);
    RecordProperty("worst_queue_depth_p95", worst_queue_p95);
    RecordProperty("worst_snapshot_age_p95", worst_age_p95);
    RecordProperty("worst_client_snapshot_bytes", static_cast<std::uint32_t>(worst_client_bytes));
}

// Clean disconnect + reconnect UNDER LOAD: a client leaves mid-soak, the server prunes it
// and KEEPS ticking, surviving clients are unaffected, then the client reconnects and
// resumes mirroring.
TEST(ReplicationScale, DisconnectReconnectUnderLoad) {
    ReplicationServer server;
    server.SetAoiChunkRadius(/*chunk_radius=*/3, kChunkSizeMm);

    std::vector<SimClient> clients;
    clients.reserve(kSoakClients);
    for (std::uint32_t id = 1; id <= kSoakClients; ++id) {
        clients.push_back(MakeSimClient(server, id));
    }

    auto live_set = [&](std::uint32_t drop_id) {
        std::vector<SimClient*> v;
        for (SimClient& c : clients) {
            if (c.id != drop_id) v.push_back(&c);
        }
        return v;
    };

    const std::uint32_t leaver = 7;

    // Warm up with everyone connected.
    {
        std::vector<SimClient*> all;
        for (SimClient& c : clients) all.push_back(&c);
        for (std::uint64_t t = 1; t <= 30; ++t) StepTick(server, all, t);
    }
    const std::uint32_t survivor_seq_before = server.AckedSnapshotSeq(1);

    // Client `leaver` cleanly disconnects (closes its end). The server observes the peer
    // gone after its queued frames drain, prunes it, and continues to serve the rest.
    clients[leaver - 1].client_end->Close();
    clients[leaver - 1].server_end->Close();

    std::vector<SimClient*> live = live_set(leaver);
    for (std::uint64_t t = 31; t <= 80; ++t) {
        StepTick(server, live, t);
        const auto removed = server.PruneDisconnectedClients();
        (void)removed;
    }

    EXPECT_FALSE(server.has_client(leaver)) << "leaver was not pruned";
    EXPECT_EQ(server.client_count(), kSoakClients - 1);
    // Survivors kept advancing (server did not stall on the leaver).
    EXPECT_GT(server.AckedSnapshotSeq(1), survivor_seq_before);

    // Reconnect under load: re-establish the connection (a fresh transport pair) and
    // re-add the client. The server keeps ticking; the rejoiner mirrors again.
    {
        auto pair = MakeLoopbackPair();
        clients[leaver - 1].server_end = std::move(pair.first);
        clients[leaver - 1].client_end = std::move(pair.second);
        server.AddClient(leaver, clients[leaver - 1].server_end.get());
        clients[leaver - 1].client =
            std::make_unique<ReplicationClient>(leaver, clients[leaver - 1].client_end.get());
    }
    EXPECT_TRUE(server.has_client(leaver));
    EXPECT_EQ(server.client_count(), kSoakClients);

    std::vector<SimClient*> all;
    for (SimClient& c : clients) all.push_back(&c);
    for (std::uint64_t t = 81; t <= 140; ++t) StepTick(server, all, t);

    EXPECT_TRUE(clients[leaver - 1].client->has_snapshot()) << "rejoiner never re-mirrored";
    EXPECT_EQ(clients[leaver - 1].client->snapshot().entities.size(), kSoakClients);
    // Whole-session p95 still within budget after the churn.
    EXPECT_LE(server.QueueDepthP95(), kQueueDepthP95Budget);
    EXPECT_LE(server.SnapshotAgeP95(), kSnapshotAgeP95Budget);
}

// A backed-up client (transport always would-blocks) must be FLAGGED by the metrics --
// the queue grows but stays bounded by the cap (no unbounded memory), frames drop, and
// the across-clients p95 reflects the degradation -- WITHOUT stalling the healthy ones.
struct BlockedSendTransport final : ILockstepTransport {
    bool SendFrame(const std::vector<std::uint8_t>&,
                   FrameDelivery = FrameDelivery::Reliable) override { return false; }
    bool TryReceiveFrame(std::vector<std::uint8_t>&) override { return false; }
    [[nodiscard]] bool IsPeerConnected() const override { return true; }
    void Close() override {}
};

TEST(ReplicationScale, BackpressureFlaggedAndBoundedUnderStall) {
    ReplicationServer server;

    // 8 healthy loopback clients + 1 stalled client.
    std::vector<SimClient> healthy;
    for (std::uint32_t id = 1; id <= 8; ++id) healthy.push_back(MakeSimClient(server, id));
    BlockedSendTransport blocked;
    const std::uint32_t stalled_id = 9;
    server.AddClient(stalled_id, &blocked);

    std::vector<SimClient*> live;
    for (SimClient& c : healthy) live.push_back(&c);

    // Drive enough ticks to overflow the stalled client's bounded queue (cap 256).
    for (std::uint64_t t = 1; t <= 400; ++t) {
        for (SimClient* sc : live) {
            UsercmdMsg cmd; cmd.tick = t; cmd.player_id = sc->id; sc->client->SendUsercmd(cmd);
        }
        server.PumpInbound();
        server.BroadcastSnapshot(t, AvatarStates(9, t));
        for (SimClient* sc : live) sc->client->PumpInbound();
        server.PumpInbound();
    }

    // The stalled client is flagged: queue saturated (bounded, never unbounded) + drops.
    EXPECT_GT(server.OutboundQueueDepth(stalled_id), 0u);
    EXPECT_LE(server.OutboundQueueDepth(stalled_id), 256u) << "queue grew past its cap";
    EXPECT_GT(server.DroppedFrames(stalled_id), 0u) << "overflow did not drop the oldest";

    // The healthy clients are untouched -- a slow peer never shared-fate-stalls the rest.
    for (const SimClient& c : healthy) {
        EXPECT_EQ(server.OutboundQueueDepth(c.id), 0u);
        EXPECT_EQ(server.DroppedFrames(c.id), 0u);
        EXPECT_TRUE(c.client->has_snapshot());
    }
}

} // namespace
