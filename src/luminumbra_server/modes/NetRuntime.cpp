#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// REAL networked multiplayer over actual TCP sockets (TcpTransport). Same
// authoritative-server replication stack as --replicate, but server and client
// run as SEPARATE PROCESSES over the wire instead of an in-process loopback.
// Proves the ILockstepTransport seam end-to-end off-loopback; optional GNS
// transport uses the same interface. Run: one process --net-host --port
// P, another --net-join --host H --port P.
// ---------------------------------------------------------------------------
void StartRuntimeTcpAccept(RuntimeTcpClientSlot& slot) {
    slot.transport = std::make_unique<Luminumbra::Net::TcpTransport>();
    Luminumbra::Net::TcpTransport* transport = slot.transport.get();
    const std::uint16_t port = slot.port;
    slot.accepting = true;
    slot.accept_result = std::async(std::launch::async, [transport, port]() {
        return transport->Listen(port, /*timeout_ms=*/250);
    });
}

bool PollRuntimeTcpAccept(RuntimeTcpClientSlot& slot) {
    if (slot.accepted) {
        return false;
    }
    if (!slot.accepting) {
        StartRuntimeTcpAccept(slot);
    }
    if (!slot.accept_result.valid() ||
        slot.accept_result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return false;
    }

    const bool accepted = slot.accept_result.get();
    slot.accepting = false;
    if (accepted) {
        slot.accepted = true;
        slot.connected = true;
        return true;
    }

    slot.transport.reset();
    return false;
}

void CloseRuntimeTcpSlot(RuntimeTcpClientSlot& slot) {
    if (slot.accepting && slot.accept_result.valid()) {
        slot.accept_result.wait();
    }
    if (slot.transport) {
        slot.transport->Close();
    }
}

int RunNetHostServerMode(const ServerCliOptions& options) {
    const std::uint32_t expected_clients = ExpectedNetworkClients(options);
    std::uint16_t last_accept_port = 0;
    if (!ResolveNetworkClientPort(options.port, expected_clients, last_accept_port)) {
        LUMINUMBRA_CORE_ERROR("net-host server-mode: cannot map {} client(s) from base port {}",
                              expected_clients,
                              options.port);
        return 2;
    }

    Luminumbra::Server::ServerWorldRunnerConfig config = RunnerConfigFrom(options);
    config.world_id.clear();
    config.world_name = "Net Host Server Mode";
    config.autosave_interval_ticks = 0;
    const int required_avatars = static_cast<int>(expected_clients) + 1;
    if (config.avatar_count < required_avatars) {
        config.avatar_count = required_avatars;
    }
    LUMINUMBRA_CORE_INFO("Net HOST server-mode: preset={} seed={} avatars={} ticks={} "
                         "client_slots={} -- polling TCP ports {}..{}",
                         options.preset,
                         options.seed,
                         config.avatar_count,
                         options.ticks,
                         expected_clients,
                         options.port,
                         last_accept_port);

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        LUMINUMBRA_CORE_ERROR("net-host server-mode: session failed to boot");
        return 1;
    }

    Luminumbra::Net::ReplicationServer server;
    std::vector<RuntimeTcpClientSlot> slots;
    slots.reserve(expected_clients);
    for (std::uint32_t client_id = 1; client_id <= expected_clients; ++client_id) {
        std::uint16_t client_port = 0;
        if (!ResolveNetworkClientPort(options.port, client_id, client_port)) {
            LUMINUMBRA_CORE_ERROR("net-host server-mode: cannot map client {} from base port {}",
                                  client_id,
                                  options.port);
            return 2;
        }
        RuntimeTcpClientSlot slot;
        slot.client_id = client_id;
        slot.port = client_port;
        if (client_id < runner.Avatars().size()) {
            slot.initial_x = runner.Avatars()[client_id].position.x;
        }
        slots.push_back(std::move(slot));
    }

    server.SetAoiChunkRadius(/*chunk_radius=*/3, /*chunk_size_mm=*/Luminumbra::CHUNK_SIZE_X * 1000);

    std::uint64_t executed = 0;
    std::uint32_t accepted_count = 0;
    std::uint32_t left_count = 0;
    auto next_tick_time = std::chrono::steady_clock::now();
    while (executed < options.ticks) {
        for (RuntimeTcpClientSlot& slot : slots) {
            if (PollRuntimeTcpAccept(slot)) {
                slot.joined_tick = executed;
                server.AddClient(slot.client_id, slot.transport.get());
                accepted_count += 1;
                LUMINUMBRA_CORE_INFO(
                    "net-host server-mode: client {} joined on port {} at host tick {}",
                    slot.client_id,
                    slot.port,
                    executed);
            }
        }

        server.PumpInbound();
        for (RuntimeTcpClientSlot& slot : slots) {
            if (!slot.accepted || !slot.transport) {
                continue;
            }
            const bool connected_now = slot.transport->IsPeerConnected();
            if (connected_now) {
                if (const Luminumbra::Net::UsercmdMsg* got = server.LatestUsercmd(slot.client_id)) {
                    runner.SetAvatarMove(got->player_id,
                                         static_cast<float>(got->move_x) / 32767.0f,
                                         static_cast<float>(got->move_z) / 32767.0f);
                }
            } else if (slot.connected) {
                slot.connected = false;
                slot.left_tick = executed;
                left_count += 1;
                runner.SetAvatarMove(slot.client_id, 0.0f, 0.0f);
                LUMINUMBRA_CORE_INFO(
                    "net-host server-mode: client {} left at host tick {}; server continues",
                    slot.client_id,
                    executed);
            }
        }

        const auto step = runner.RunFixedTicks(1);
        executed += step.ticks_executed;
        if (step.ticks_executed == 0) {
            LUMINUMBRA_CORE_ERROR("net-host server-mode: tick {} did not advance", executed + 1);
            for (RuntimeTcpClientSlot& slot : slots) {
                CloseRuntimeTcpSlot(slot);
            }
            return 1;
        }

        const auto states = Luminumbra::World::BuildAvatarReplStates(runner.Avatars());
        server.BroadcastSnapshot(executed, states);
        next_tick_time += std::chrono::milliseconds(33);
        std::this_thread::sleep_until(next_tick_time);
    }

    server.PumpInbound();
    std::uint32_t connected_at_shutdown = 0;
    nlohmann::json clients = nlohmann::json::array();
    for (RuntimeTcpClientSlot& slot : slots) {
        const bool connected_now =
            slot.accepted && slot.transport && slot.transport->IsPeerConnected();
        if (connected_now) {
            connected_at_shutdown += 1;
        }
        const float final_x = slot.client_id < runner.Avatars().size()
                                  ? runner.Avatars()[slot.client_id].position.x
                                  : 0.0f;
        LUMINUMBRA_CORE_INFO("net-host server-mode: client {} accepted={} connected={} "
                             "joined_tick={} left_tick={} acked_seq={} avatar_dx={:.2f} m",
                             slot.client_id,
                             slot.accepted,
                             connected_now,
                             slot.joined_tick,
                             slot.left_tick,
                             server.AckedSnapshotSeq(slot.client_id),
                             final_x - slot.initial_x);
        clients.push_back({
            {"client_id", slot.client_id},
            {"accept_port", slot.port},
            {"accepted", slot.accepted},
            {"connected_at_shutdown", connected_now},
            {"joined_tick", slot.joined_tick},
            {"left_tick", slot.left_tick},
            {"acked_snapshot_seq", server.AckedSnapshotSeq(slot.client_id)},
            {"avatar_dx_m", final_x - slot.initial_x},
        });
        CloseRuntimeTcpSlot(slot);
    }

    if (!options.artifact_path.empty()) {
        nlohmann::json artifact{
            {"schema", "luminumbra.net_host_server_mode.v1"},
            {"generated_by", "luminumbra_server_app --net-host --server-mode"},
            {"preset", options.preset},
            {"seed", options.seed},
            {"ticks_requested", options.ticks},
            {"ticks_executed", executed},
            {"expected_clients", expected_clients},
            {"accepted_clients", accepted_count},
            {"left_clients", left_count},
            {"connected_at_shutdown", connected_at_shutdown},
            {"base_port", options.port},
            {"last_accept_port", last_accept_port},
            {"clients", clients},
            {"passed", executed == options.ticks},
        };
        const fs::path artifact_path(options.artifact_path);
        std::error_code ec;
        if (artifact_path.has_parent_path()) {
            fs::create_directories(artifact_path.parent_path(), ec);
        }
        std::ofstream out(artifact_path);
        if (out.is_open()) {
            out << artifact.dump(2) << "\n";
            LUMINUMBRA_CORE_INFO("net-host server-mode artifact written: {}",
                                 options.artifact_path);
        }
    }

    LUMINUMBRA_CORE_INFO(
        "net-host server-mode: ran {} ticks with {} accepted, {} left, {} connected at shutdown.",
        executed,
        accepted_count,
        left_count,
        connected_at_shutdown);
    return executed == options.ticks ? 0 : 1;
}
