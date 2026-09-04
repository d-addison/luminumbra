#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

int RunNetHost(const ServerCliOptions& options) {
    if (options.server_mode) {
        return RunNetHostServerMode(options);
    }

    const std::uint32_t expected_clients = ExpectedNetworkClients(options);

    Luminumbra::Server::ServerWorldRunnerConfig config = RunnerConfigFrom(options);
    config.world_id.clear();
    config.world_name = "Net Host";
    config.autosave_interval_ticks = 0;
    const int required_avatars = static_cast<int>(expected_clients) + 1;
    if (config.avatar_count < required_avatars) {
        config.avatar_count = required_avatars; // avatar ids 1..N are controlled by remote clients
    }
    LUMINUMBRA_CORE_INFO("Net HOST: preset={} seed={} avatars={} ticks={} clients={} -- accepting "
                         "{} client(s) on ONE TCP port {}",
                         options.preset,
                         options.seed,
                         config.avatar_count,
                         options.ticks,
                         expected_clients,
                         expected_clients,
                         options.port);

    // ONE listen socket fans N client connections into distinct AddClient
    // slots. Bind+listen BEFORE runner.Boot (world-gen is ~15s) so a client that dials in
    // during boot lands in the listen backlog (its Hello buffered on the socket) and is accepted
    // once the world is ready -- the client's short connect-retry must not race the slow boot.
    Luminumbra::Net::TcpListener listener;
    if (!listener.Listen(options.port, /*backlog=*/static_cast<int>(expected_clients) + 1)) {
        LUMINUMBRA_CORE_ERROR("net-host: could not open the listen socket on port {}",
                              options.port);
        return 1;
    }

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        LUMINUMBRA_CORE_ERROR("net-host: session failed to boot");
        return 1;
    }

    Luminumbra::Net::ReplicationServer server;
    std::vector<std::unique_ptr<Luminumbra::Net::TcpTransport>> transports;
    std::vector<std::uint32_t> client_ids;
    transports.reserve(expected_clients);
    client_ids.reserve(expected_clients);

    // Each accepted connection's FIRST frame is its Hello, which carries the client id: it is
    // read PRE-AddClient (below) so the slot id comes from the HANDSHAKE, not the accept order.
    // The Hello's trailing usercmd frames stay buffered in the same transport for PumpInbound.
    const std::uint64_t seed_num = std::strtoull(options.seed.c_str(), nullptr, 10);
    for (std::uint32_t accepted = 0; accepted < expected_clients; ++accepted) {
        LUMINUMBRA_CORE_INFO("net-host: waiting for client {}/{} over TCP on port {}...",
                             accepted + 1,
                             expected_clients,
                             options.port);
        std::unique_ptr<Luminumbra::Net::TcpTransport> transport =
            listener.AcceptOneBlocking(/*timeout_ms=*/30000);
        if (!transport) {
            LUMINUMBRA_CORE_ERROR("net-host: accept failed on port {} after {} of {} client(s) "
                                  "(timed out waiting for a client?)",
                                  options.port,
                                  accepted,
                                  expected_clients);
            for (auto& t : transports)
                t->Close();
            listener.Close();
            return 1;
        }
        // Read the client's Hello (its first frame) to learn its declared id. Bounded poll --
        // a real loopback/LAN socket delivers asynchronously; a peer that never says Hello (or
        // drops) is a REJECT, never a hang.
        Luminumbra::Net::HelloMsg hello;
        bool got_hello = false;
        std::vector<std::uint8_t> frame;
        const auto hello_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < hello_deadline) {
            if (transport->TryReceiveFrame(frame)) {
                if (!Luminumbra::Net::DecodeHello(frame, hello)) {
                    LUMINUMBRA_CORE_ERROR(
                        "net-host: an accepted client's first frame was not a valid Hello");
                    for (auto& t : transports)
                        t->Close();
                    transport->Close();
                    listener.Close();
                    return 1;
                }
                got_hello = true;
                break;
            }
            if (!transport->IsPeerConnected())
                break; // peer left before saying Hello
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!got_hello) {
            LUMINUMBRA_CORE_ERROR(
                "net-host: never received a Hello from an accepted client on port {}",
                options.port);
            for (auto& t : transports)
                t->Close();
            transport->Close();
            listener.Close();
            return 1;
        }
        // Reject a mis-paired client LOUDLY (a mismatched world would silently desync), mirroring
        // the lockstep handshake's protocol/seed/preset discipline.
        if (hello.protocol_version != Luminumbra::Net::kLockstepProtocolVersion) {
            LUMINUMBRA_CORE_ERROR("net-host: client protocol mismatch (host {} != client {})",
                                  Luminumbra::Net::kLockstepProtocolVersion,
                                  hello.protocol_version);
            for (auto& t : transports)
                t->Close();
            transport->Close();
            listener.Close();
            return 1;
        }
        if (hello.seed != seed_num || hello.preset != options.preset) {
            LUMINUMBRA_CORE_ERROR("net-host: client world mismatch (seed host {} != client {}; "
                                  "preset host '{}' != client '{}')",
                                  seed_num,
                                  hello.seed,
                                  options.preset,
                                  hello.preset);
            for (auto& t : transports)
                t->Close();
            transport->Close();
            listener.Close();
            return 1;
        }
        // The slot id is now DECLARED by the client (was the loop index), so validate it lands in
        // the server's avatar-slot range [1, expected_clients] -- restoring the invariant the old
        // loop-index scheme guaranteed. This keeps every downstream index by client_id
        // (initial_x_by_client, avatar mapping) in bounds, and rejects a bogus slot claim (0 = the
        // host's own reserved id) LOUDLY rather than corrupting a neighbour's slot.
        if (hello.client_id == 0u || hello.client_id > expected_clients) {
            LUMINUMBRA_CORE_ERROR("net-host: client declared out-of-range id {} (expected 1..{})",
                                  hello.client_id,
                                  expected_clients);
            for (auto& t : transports)
                t->Close();
            transport->Close();
            listener.Close();
            return 1;
        }
        const std::uint32_t client_id = hello.client_id;
        server.AddClient(client_id, transport.get());
        client_ids.push_back(client_id);
        transports.push_back(std::move(transport));
        LUMINUMBRA_CORE_INFO("net-host: client id {} joined on port {} ({}/{}).",
                             client_id,
                             options.port,
                             accepted + 1,
                             expected_clients);
    }
    listener.Close(); // all expected clients accepted; the listen socket is no longer needed
    LUMINUMBRA_CORE_INFO("net-host: {} client(s) connected over TCP (single-port fan-out).",
                         transports.size());

    server.SetAoiChunkRadius(/*chunk_radius=*/3, /*chunk_size_mm=*/Luminumbra::CHUNK_SIZE_X * 1000);

    std::vector<float> initial_x_by_client(expected_clients + 1u, 0.0f);
    for (const std::uint32_t client_id : client_ids) {
        if (client_id < runner.Avatars().size()) {
            initial_x_by_client[client_id] = runner.Avatars()[client_id].position.x;
        }
    }

    std::uint64_t executed = 0;
    bool all_clients_connected = true;
    while (executed < options.ticks) {
        server.PumpInbound(); // receive each client's usercmd (newest-wins)
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
            LUMINUMBRA_CORE_ERROR("net-host: tick {} did not advance", executed + 1);
            for (auto& transport : transports) {
                transport->Close();
            }
            return 1;
        }
        const auto states = Luminumbra::World::BuildAvatarReplStates(runner.Avatars());
        server.BroadcastSnapshot(executed, states); // reliable over TCP
        std::size_t connected_count = 0;
        for (const auto& transport : transports) {
            if (transport->IsPeerConnected()) {
                ++connected_count;
            }
        }
        if (connected_count != transports.size()) {
            LUMINUMBRA_CORE_WARN("net-host: {}/{} client(s) still connected at tick {}",
                                 connected_count,
                                 transports.size(),
                                 executed);
            all_clients_connected = false;
            break;
        }
    }
    server.PumpInbound(); // drain final ack
    for (const std::uint32_t client_id : client_ids) {
        const float final_x =
            client_id < runner.Avatars().size() ? runner.Avatars()[client_id].position.x : 0.0f;
        LUMINUMBRA_CORE_INFO("net-host: client {} acked seq {}; avatar {} moved {:.2f} m in X.",
                             client_id,
                             server.AckedSnapshotSeq(client_id),
                             client_id,
                             final_x - initial_x_by_client[client_id]);
    }
    LUMINUMBRA_CORE_INFO("net-host: ran {} ticks, {} avatars, {} TCP client(s). "
                         "Holding briefly so the last frames flush, then closing.",
                         executed,
                         runner.Avatars().size(),
                         transports.size());
    // Give TCP a moment to flush the final snapshot(s) before the socket closes.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    for (auto& transport : transports) {
        transport->Close();
    }
    return (executed == options.ticks && all_clients_connected) ? 0 : 1;
}

int RunNetJoin(const ServerCliOptions& options) {
    const std::uint32_t player_id = LocalNetworkPlayerId(options);
    // connect to the host's SINGLE listen port (the dedicated-server shape),
    // NOT a per-client base_port+K-1 port. Our server-side slot id is DECLARED in the Hello below,
    // so the host fans this connection into slot `player_id` regardless of accept order.
    const std::uint16_t connect_port = options.port;

    LUMINUMBRA_CORE_INFO(
        "Net JOIN: player {} connecting to {}:{}...", player_id, options.host, connect_port);
    Luminumbra::Net::TcpTransport transport;
    if (!transport.Connect(options.host, connect_port, /*timeout_ms=*/30000)) {
        LUMINUMBRA_CORE_ERROR("net-join: could not connect to {}:{}", options.host, connect_port);
        return 1;
    }
    // Send our Hello FIRST (before any usercmd) so the host reads our declared id and validates
    // the shared world (protocol/seed/preset). Same wire form as the lockstep handshake Hello;
    // the host consumes exactly this one frame pre-AddClient, then the usercmd stream follows.
    {
        Luminumbra::Net::HelloMsg hello;
        hello.seed = std::strtoull(options.seed.c_str(), nullptr, 10);
        hello.preset = options.preset;
        hello.tick_rate_hz = 30;
        hello.client_id = player_id;
        if (!transport.SendFrame(Luminumbra::Net::EncodeHello(hello))) {
            LUMINUMBRA_CORE_ERROR(
                "net-join: failed to send Hello to {}:{}", options.host, connect_port);
            transport.Close();
            return 1;
        }
    }
    LUMINUMBRA_CORE_INFO("net-join: player {} connected over TCP.", player_id);

    Luminumbra::Net::ReplicationClient client(player_id, &transport);

    std::uint32_t last_seq = 0;
    std::size_t max_entities = 0;
    // Pump until we have mirrored the host's full run (seq >= ticks) or it leaves.
    // Send a +X usercmd each iteration so the selected host avatar walks under our input.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        Luminumbra::Net::UsercmdMsg cmd;
        cmd.tick = last_seq + 1;
        cmd.player_id = player_id;
        cmd.move_x = 32767; // +1.0
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
        LUMINUMBRA_CORE_ERROR("net-join: player {} did not mirror the host (has_snapshot={} "
                              "max_entities={} last_seq={})",
                              player_id,
                              client.has_snapshot(),
                              max_entities,
                              last_seq);
        transport.Close();
        return 1;
    }
    float mirror_x = 0.0f;
    for (const auto& e : client.snapshot().entities) {
        if (e.entity_id == player_id)
            mirror_x = Luminumbra::Net::ReplDequantPos(e.px_mm);
    }
    LUMINUMBRA_CORE_INFO("net-join: player {} mirrored host over TCP -- last seq {}, up to {} "
                         "entities; controlled avatar "
                         "mirrored at x={:.2f} m. Real over-the-wire replication confirmed.",
                         player_id,
                         last_seq,
                         max_entities,
                         mirror_x);
    transport.Close();
    return 0;
}
