#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

#ifdef LUMINUMBRA_ENABLE_GNS
// ---------------------------------------------------------------------------
// the SAME authoritative-server replication over the STANDALONE
// GameNetworkingSockets transport (real UDP, no Steam). Unlike the Steam path,
// two processes CAN connect on one machine -- so this is the locally-testable
// real-UDP loop. Built only with -DLUMINUMBRA_ENABLE_GNS=ON.
// ---------------------------------------------------------------------------
int RunGnsHost(const ServerCliOptions& options) {
    if (!Luminumbra::Net::GnsLink::Init()) {
        LUMINUMBRA_CORE_ERROR("gns-host: GameNetworkingSockets init failed");
        return 1;
    }
    const std::uint32_t expected_clients = ExpectedNetworkClients(options);
    std::uint16_t last_accept_port = 0;
    if (!ResolveNetworkClientPort(options.port, expected_clients, last_accept_port)) {
        LUMINUMBRA_CORE_ERROR(
            "gns-host: cannot map {} client(s) from base port {}", expected_clients, options.port);
        Luminumbra::Net::GnsLink::Shutdown();
        return 2;
    }
    Luminumbra::Server::ServerWorldRunnerConfig config = RunnerConfigFrom(options);
    config.world_id.clear();
    config.world_name = "GNS Host";
    config.autosave_interval_ticks = 0;
    const int required_avatars = static_cast<int>(expected_clients) + 1;
    if (config.avatar_count < required_avatars)
        config.avatar_count = required_avatars;
    LUMINUMBRA_CORE_INFO(
        "GNS HOST: preset={} seed={} avatars={} ticks={} clients={} -- accepting UDP ports {}..{}",
        options.preset,
        options.seed,
        config.avatar_count,
        options.ticks,
        expected_clients,
        options.port,
        last_accept_port);

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        LUMINUMBRA_CORE_ERROR("gns-host: boot failed");
        Luminumbra::Net::GnsLink::Shutdown();
        return 1;
    }

    Luminumbra::Net::ReplicationServer server;
    std::vector<std::unique_ptr<Luminumbra::Net::GnsTransport>> transports;
    std::vector<std::uint32_t> client_ids;
    transports.reserve(expected_clients);
    client_ids.reserve(expected_clients);
    for (std::uint32_t client_id = 1; client_id <= expected_clients; ++client_id) {
        std::uint16_t client_port = 0;
        if (!ResolveNetworkClientPort(options.port, client_id, client_port)) {
            LUMINUMBRA_CORE_ERROR(
                "gns-host: cannot map client {} from base port {}", client_id, options.port);
            for (auto& accepted : transports) {
                accepted->Close();
            }
            Luminumbra::Net::GnsLink::Shutdown();
            return 2;
        }
        auto transport = std::make_unique<Luminumbra::Net::GnsTransport>();
        if (!transport->Listen(client_port)) {
            LUMINUMBRA_CORE_ERROR("gns-host: CreateListenSocketIP failed for client {} on port {}",
                                  client_id,
                                  client_port);
            for (auto& accepted : transports) {
                accepted->Close();
            }
            Luminumbra::Net::GnsLink::Shutdown();
            return 1;
        }
        LUMINUMBRA_CORE_INFO("gns-host: listening for client {} over UDP on port {} (30s)...",
                             client_id,
                             client_port);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!transport->IsPeerConnected() && std::chrono::steady_clock::now() < deadline) {
            Luminumbra::Net::GnsLink::RunCallbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!transport->IsPeerConnected()) {
            LUMINUMBRA_CORE_ERROR("gns-host: client {} did not connect within timeout", client_id);
            transport->Close();
            for (auto& accepted : transports) {
                accepted->Close();
            }
            Luminumbra::Net::GnsLink::Shutdown();
            return 1;
        }
        server.AddClient(client_id, transport.get());
        client_ids.push_back(client_id);
        transports.push_back(std::move(transport));
    }
    LUMINUMBRA_CORE_INFO("gns-host: {} client(s) connected over UDP.", transports.size());

    server.SetAoiChunkRadius(3, Luminumbra::CHUNK_SIZE_X * 1000);
    std::vector<float> initial_x_by_client(expected_clients + 1u, 0.0f);
    for (const std::uint32_t client_id : client_ids) {
        if (client_id < runner.Avatars().size()) {
            initial_x_by_client[client_id] = runner.Avatars()[client_id].position.x;
        }
    }

    std::uint64_t executed = 0;
    bool all_clients_connected = true;
    while (executed < options.ticks) {
        Luminumbra::Net::GnsLink::RunCallbacks();
        server.PumpInbound();
        for (const std::uint32_t client_id : client_ids) {
            if (const Luminumbra::Net::UsercmdMsg* got = server.LatestUsercmd(client_id)) {
                runner.SetAvatarMove(got->player_id,
                                     static_cast<float>(got->move_x) / 32767.0f,
                                     static_cast<float>(got->move_z) / 32767.0f);
            }
        }
        const auto step = runner.RunFixedTicks(1);
        executed += step.ticks_executed;
        if (step.ticks_executed == 0) {
            LUMINUMBRA_CORE_ERROR("gns-host: tick stalled");
            for (auto& transport : transports) {
                transport->Close();
            }
            Luminumbra::Net::GnsLink::Shutdown();
            return 1;
        }
        const auto states = Luminumbra::World::BuildAvatarReplStates(runner.Avatars());
        server.BroadcastSnapshot(executed, states);
        std::size_t connected_count = 0;
        for (const auto& transport : transports) {
            if (transport->IsPeerConnected()) {
                ++connected_count;
            }
        }
        if (connected_count != transports.size()) {
            LUMINUMBRA_CORE_WARN("gns-host: {}/{} client(s) still connected at tick {}",
                                 connected_count,
                                 transports.size(),
                                 executed);
            all_clients_connected = false;
            break;
        }
    }
    for (const std::uint32_t client_id : client_ids) {
        const float final_x =
            client_id < runner.Avatars().size() ? runner.Avatars()[client_id].position.x : 0.0f;
        LUMINUMBRA_CORE_INFO("gns-host: client {} acked seq {}; avatar {} moved {:.2f} m in X.",
                             client_id,
                             server.AckedSnapshotSeq(client_id),
                             client_id,
                             final_x - initial_x_by_client[client_id]);
    }
    LUMINUMBRA_CORE_INFO("gns-host: ran {} ticks over UDP, {} avatars, {} client(s).",
                         executed,
                         runner.Avatars().size(),
                         transports.size());
    for (int i = 0; i < 50; ++i) {
        Luminumbra::Net::GnsLink::RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (auto& transport : transports) {
        transport->Close();
    }
    Luminumbra::Net::GnsLink::Shutdown();
    return (executed == options.ticks && all_clients_connected) ? 0 : 1;
}

int RunGnsJoin(const ServerCliOptions& options) {
    if (!Luminumbra::Net::GnsLink::Init()) {
        LUMINUMBRA_CORE_ERROR("gns-join: GameNetworkingSockets init failed");
        return 1;
    }
    const std::uint32_t player_id = LocalNetworkPlayerId(options);
    std::uint16_t connect_port = 0;
    if (!ResolveNetworkClientPort(options.port, player_id, connect_port)) {
        LUMINUMBRA_CORE_ERROR(
            "gns-join: cannot map player id {} from base port {}", player_id, options.port);
        Luminumbra::Net::GnsLink::Shutdown();
        return 2;
    }
    LUMINUMBRA_CORE_INFO("GNS JOIN: player {} connecting to {}:{} over UDP...",
                         player_id,
                         options.host,
                         connect_port);
    Luminumbra::Net::GnsTransport transport;
    if (!transport.Connect(options.host, connect_port)) {
        LUMINUMBRA_CORE_ERROR("gns-join: ConnectByIPAddress failed");
        Luminumbra::Net::GnsLink::Shutdown();
        return 1;
    }
    const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!transport.IsPeerConnected() && std::chrono::steady_clock::now() < connect_deadline) {
        Luminumbra::Net::GnsLink::RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!transport.IsPeerConnected()) {
        LUMINUMBRA_CORE_ERROR("gns-join: connect timed out");
        transport.Close();
        Luminumbra::Net::GnsLink::Shutdown();
        return 1;
    }
    LUMINUMBRA_CORE_INFO("gns-join: player {} connected over UDP.", player_id);

    Luminumbra::Net::ReplicationClient client(player_id, &transport);
    std::uint32_t last_seq = 0;
    std::size_t max_entities = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        Luminumbra::Net::GnsLink::RunCallbacks();
        Luminumbra::Net::UsercmdMsg cmd;
        cmd.tick = last_seq + 1;
        cmd.player_id = player_id;
        cmd.move_x = 32767;
        client.SendUsercmd(cmd);
        client.PumpInbound();
        if (client.has_snapshot()) {
            last_seq = client.snapshot().snapshot_seq;
            max_entities = std::max(max_entities, client.snapshot().entities.size());
        }
        if (last_seq >= options.ticks)
            break;
        if (!transport.IsPeerConnected() && last_seq > 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool ok =
        client.has_snapshot() && max_entities > static_cast<std::size_t>(player_id) && last_seq > 0;
    if (!ok) {
        LUMINUMBRA_CORE_ERROR("gns-join: player {} did not mirror the host (has_snapshot={} "
                              "max_entities={} last_seq={})",
                              player_id,
                              client.has_snapshot(),
                              max_entities,
                              last_seq);
        transport.Close();
        Luminumbra::Net::GnsLink::Shutdown();
        return 1;
    }
    float mirror_x = 0.0f;
    for (const auto& e : client.snapshot().entities) {
        if (e.entity_id == player_id)
            mirror_x = Luminumbra::Net::ReplDequantPos(e.px_mm);
    }
    LUMINUMBRA_CORE_INFO(
        "gns-join: player {} mirrored host over UDP -- last seq {}, up to {} entities; controlled "
        "avatar at x={:.2f} m. Real UDP replication confirmed.",
        player_id,
        last_seq,
        max_entities,
        mirror_x);
    transport.Close();
    Luminumbra::Net::GnsLink::Shutdown();
    return 0;
}
#endif // LUMINUMBRA_ENABLE_GNS
