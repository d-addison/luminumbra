#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
//   (-C1): MULTIPROCESS 32-client SOAK harness over real TCP.
//
// One authoritative server process + up to --clients driving avatar client
// processes (luminumbra_server_app --net-soak-client). The server accepts each
// peer (per-client port, the proven multi-connection-accept path), ticks the
// world at 30 Hz for --ticks, and over the WHOLE run ASSERTS:
//   * sustained tick-rate -- measured wall-clock vs the expected 33 ms * ticks
//     budget (a silent overrun under sleep_until would otherwise pass);
//   * per-connection backpressure  (QueueDepthP95  <= budget);
//   * per-connection snapshot-aging (SnapshotAgeP95 <= budget);
//   * per-client bandwidth          (max snapshot bytes <= budget).
// A peer that cleanly LEAVES is pruned and its accept slot RE-ARMS, so the same
// client can RECONNECT under load -- the server never shared-fate-stalls on it.
// A budget breach makes the run FAIL (non-zero exit) so this is a real gate, not
// a logger. The deterministic in-process equivalent is ReplicationScale (ctest);
// this path is the over-the-wire validation run at a modest N locally.
// ---------------------------------------------------------------------------
int RunNetSoak(const ServerCliOptions& options) {
    const std::uint32_t expected_clients = ExpectedNetworkClients(options);
    std::uint16_t last_accept_port = 0;
    if (!ResolveNetworkClientPort(options.port, expected_clients, last_accept_port)) {
        LUMINUMBRA_CORE_ERROR(
            "net-soak: cannot map {} client(s) from base port {}", expected_clients, options.port);
        return 2;
    }

    // Per-connection soak budgets. Over real TCP the kernel send
    // buffer can transiently hold frames, so these are looser than the loopback ctest's,
    // but a sustained breach still fails the run.
    constexpr std::uint32_t kQueueDepthP95Budget = 16;
    constexpr std::uint32_t kSnapshotAgeP95Budget = 30; // ~1 s of un-acked snapshots
    constexpr std::size_t kMaxClientBytesBudget = 64u * 1024u;
    constexpr double kTickRateToleranceFactor = 1.40; // allow 40% wall-clock overrun

    Luminumbra::Server::ServerWorldRunnerConfig config = RunnerConfigFrom(options);
    config.world_id.clear();
    config.world_name = "Net Soak";
    config.autosave_interval_ticks = 0;
    const int required_avatars = static_cast<int>(expected_clients) + 1;
    if (config.avatar_count < required_avatars) {
        config.avatar_count = required_avatars;
    }
    LUMINUMBRA_CORE_INFO("Net SOAK: preset={} seed={} avatars={} ticks={} client_slots={} -- "
                         "polling TCP ports {}..{}",
                         options.preset,
                         options.seed,
                         config.avatar_count,
                         options.ticks,
                         expected_clients,
                         options.port,
                         last_accept_port);

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        LUMINUMBRA_CORE_ERROR("net-soak: session failed to boot");
        return 1;
    }

    Luminumbra::Net::ReplicationServer server;
    server.SetAoiChunkRadius(/*chunk_radius=*/3, /*chunk_size_mm=*/Luminumbra::CHUNK_SIZE_X * 1000);

    std::vector<RuntimeTcpClientSlot> slots;
    slots.reserve(expected_clients);
    for (std::uint32_t client_id = 1; client_id <= expected_clients; ++client_id) {
        std::uint16_t client_port = 0;
        if (!ResolveNetworkClientPort(options.port, client_id, client_port)) {
            LUMINUMBRA_CORE_ERROR(
                "net-soak: cannot map client {} from base port {}", options.port, client_id);
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

    std::uint64_t executed = 0;
    std::uint32_t accept_events = 0; // total accepts (>= joins, counts reconnects)
    std::uint32_t leave_events = 0;  // total clean leaves observed
    std::uint32_t peak_connected = 0;
    std::uint32_t worst_queue_p95 = 0;
    std::uint32_t worst_age_p95 = 0;
    std::size_t worst_client_bytes = 0;

    const auto wall_start = std::chrono::steady_clock::now();
    auto next_tick_time = wall_start;
    while (executed < options.ticks) {
        // Accept / RE-ACCEPT (reconnect-under-load): a re-armed slot picks the peer up.
        for (RuntimeTcpClientSlot& slot : slots) {
            if (PollRuntimeTcpAccept(slot)) {
                slot.joined_tick = executed;
                server.AddClient(slot.client_id, slot.transport.get());
                accept_events += 1;
                LUMINUMBRA_CORE_INFO("net-soak: client {} (re)joined on port {} at host tick {}",
                                     slot.client_id,
                                     slot.port,
                                     executed);
            }
        }

        server.PumpInbound();

        std::uint32_t connected_now_count = 0;
        for (RuntimeTcpClientSlot& slot : slots) {
            if (!slot.accepted || !slot.transport)
                continue;
            if (slot.transport->IsPeerConnected()) {
                connected_now_count += 1;
                if (const Luminumbra::Net::UsercmdMsg* got = server.LatestUsercmd(slot.client_id)) {
                    runner.SetAvatarMove(got->player_id,
                                         static_cast<float>(got->move_x) / 32767.0f,
                                         static_cast<float>(got->move_z) / 32767.0f);
                }
            } else if (slot.connected) {
                // Clean leave: stop the avatar, prune the client, and RE-ARM the accept
                // slot so this peer can reconnect under load (the server keeps ticking).
                slot.connected = false;
                slot.left_tick = executed;
                leave_events += 1;
                runner.SetAvatarMove(slot.client_id, 0.0f, 0.0f);
                server.RemoveClient(slot.client_id);
                slot.transport.reset();
                slot.accepted = false; // PollRuntimeTcpAccept re-arms the listen next loop
                LUMINUMBRA_CORE_INFO("net-soak: client {} left at host tick {}; slot re-armed",
                                     slot.client_id,
                                     executed);
            }
        }
        peak_connected = std::max(peak_connected, connected_now_count);

        const auto step = runner.RunFixedTicks(1);
        executed += step.ticks_executed;
        if (step.ticks_executed == 0) {
            LUMINUMBRA_CORE_ERROR("net-soak: tick {} did not advance", executed + 1);
            for (RuntimeTcpClientSlot& slot : slots)
                CloseRuntimeTcpSlot(slot);
            return 1;
        }

        const auto states = Luminumbra::World::BuildAvatarReplStates(runner.Avatars());
        server.BroadcastSnapshot(executed, states);

        // Sample + hold the worst per-connection metric this tick.
        worst_queue_p95 = std::max(worst_queue_p95, server.QueueDepthP95());
        worst_age_p95 = std::max(worst_age_p95, server.SnapshotAgeP95());
        worst_client_bytes = std::max(worst_client_bytes, server.last_broadcast_max_client_bytes());

        next_tick_time += std::chrono::milliseconds(33);
        std::this_thread::sleep_until(next_tick_time);
    }
    const auto wall_end = std::chrono::steady_clock::now();
    server.PumpInbound();

    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(wall_end - wall_start).count();
    const double expected_ms = static_cast<double>(options.ticks) * 33.0;
    const bool ticks_ok = executed == options.ticks;
    const bool rate_ok = elapsed_ms <= expected_ms * kTickRateToleranceFactor;
    const bool queue_ok = worst_queue_p95 <= kQueueDepthP95Budget;
    const bool age_ok = worst_age_p95 <= kSnapshotAgeP95Budget;
    const bool bytes_ok = worst_client_bytes <= kMaxClientBytesBudget;
    const bool passed = ticks_ok && rate_ok && queue_ok && age_ok && bytes_ok;

    nlohmann::json clients = nlohmann::json::array();
    for (RuntimeTcpClientSlot& slot : slots) {
        const bool connected_now =
            slot.accepted && slot.transport && slot.transport->IsPeerConnected();
        const float final_x = slot.client_id < runner.Avatars().size()
                                  ? runner.Avatars()[slot.client_id].position.x
                                  : 0.0f;
        clients.push_back({
            {"client_id", slot.client_id},
            {"accept_port", slot.port},
            {"connected_at_shutdown", connected_now},
            {"queue_depth", server.OutboundQueueDepth(slot.client_id)},
            {"snapshot_age", server.SnapshotAge(slot.client_id)},
            {"peak_queue_depth", server.PeakOutboundQueueDepth(slot.client_id)},
            {"dropped_frames", server.DroppedFrames(slot.client_id)},
            {"acked_snapshot_seq", server.AckedSnapshotSeq(slot.client_id)},
            {"avatar_dx_m", final_x - slot.initial_x},
        });
        CloseRuntimeTcpSlot(slot);
    }

    if (!options.artifact_path.empty()) {
        nlohmann::json artifact{
            {"schema", "luminumbra.net_soak.v1"},
            {"generated_by", "luminumbra_server_app --net-soak"},
            {"preset", options.preset},
            {"seed", options.seed},
            {"ticks_requested", options.ticks},
            {"ticks_executed", executed},
            {"expected_clients", expected_clients},
            {"accept_events", accept_events},
            {"leave_events", leave_events},
            {"peak_connected", peak_connected},
            {"elapsed_ms", elapsed_ms},
            {"expected_ms", expected_ms},
            {"worst_queue_depth_p95", worst_queue_p95},
            {"worst_snapshot_age_p95", worst_age_p95},
            {"worst_client_snapshot_bytes", worst_client_bytes},
            {"budget_queue_depth_p95", kQueueDepthP95Budget},
            {"budget_snapshot_age_p95", kSnapshotAgeP95Budget},
            {"budget_client_bytes", kMaxClientBytesBudget},
            {"ticks_ok", ticks_ok},
            {"rate_ok", rate_ok},
            {"queue_ok", queue_ok},
            {"age_ok", age_ok},
            {"bytes_ok", bytes_ok},
            {"clients", clients},
            {"passed", passed},
        };
        const fs::path artifact_path(options.artifact_path);
        std::error_code ec;
        if (artifact_path.has_parent_path())
            fs::create_directories(artifact_path.parent_path(), ec);
        std::ofstream out(artifact_path);
        if (out.is_open()) {
            out << artifact.dump(2) << "\n";
            LUMINUMBRA_CORE_INFO("net-soak artifact written: {}", options.artifact_path);
        }
    }

    LUMINUMBRA_CORE_INFO(
        "net-soak: ran {}/{} ticks in {:.0f} ms (budget {:.0f} ms), peak_connected={}, "
        "accepts={}, leaves={}; worst q-p95={} (<= {}), age-p95={} (<= {}), client_bytes={} (<= "
        "{}) -> {}",
        executed,
        options.ticks,
        elapsed_ms,
        expected_ms * kTickRateToleranceFactor,
        peak_connected,
        accept_events,
        leave_events,
        worst_queue_p95,
        kQueueDepthP95Budget,
        worst_age_p95,
        kSnapshotAgeP95Budget,
        worst_client_bytes,
        kMaxClientBytesBudget,
        passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}

// the driving avatar client for --net-soak. Connects to its per-client
// port, streams +X usercmds while mirroring the host, then DISCONNECTS and RECONNECTS
// --soak-cycles times -- exercising the server's reconnect-under-load re-arm path over
// real TCP. Each cycle runs roughly ticks/cycles host ticks worth of input.
int RunNetSoakClient(const ServerCliOptions& options) {
    const std::uint32_t player_id = LocalNetworkPlayerId(options);
    std::uint16_t connect_port = 0;
    if (!ResolveNetworkClientPort(options.port, player_id, connect_port)) {
        LUMINUMBRA_CORE_ERROR(
            "net-soak-client: cannot map player id {} from base port {}", player_id, options.port);
        return 2;
    }

    const int cycles = std::max(1, options.soak_cycles);
    const std::uint64_t ticks_per_cycle =
        std::max<std::uint64_t>(1, options.ticks / static_cast<std::uint64_t>(cycles));
    std::uint32_t total_snapshots = 0;
    int connected_cycles = 0;

    for (int cycle = 0; cycle < cycles; ++cycle) {
        // The server's per-client accept re-arms its listen in short windows, so a single
        // connect can land in the gap between windows. RETRY until a deadline (the server
        // may also still be booting on the first cycle / re-arming between cycles) so the
        // soak is not flaky on accept timing.
        Luminumbra::Net::TcpTransport transport;
        bool connected = false;
        const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < connect_deadline) {
            if (transport.Connect(options.host, connect_port, /*timeout_ms=*/2000)) {
                connected = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // brief backoff, retry
        }
        if (!connected) {
            LUMINUMBRA_CORE_WARN(
                "net-soak-client: player {} cycle {} could not connect to {}:{} within deadline",
                player_id,
                cycle,
                options.host,
                connect_port);
            continue;
        }
        connected_cycles += 1;
        Luminumbra::Net::ReplicationClient client(player_id, &transport);
        LUMINUMBRA_CORE_INFO("net-soak-client: player {} connected (cycle {}/{}) on port {}",
                             player_id,
                             cycle + 1,
                             cycles,
                             connect_port);

        std::uint32_t last_seq = 0;
        for (std::uint64_t i = 0; i < ticks_per_cycle; ++i) {
            Luminumbra::Net::UsercmdMsg cmd;
            cmd.tick = last_seq + 1;
            cmd.player_id = player_id;
            cmd.move_x = 32767; // +1.0
            client.SendUsercmd(cmd);
            client.PumpInbound();
            if (client.has_snapshot())
                last_seq = client.snapshot().snapshot_seq;
            if (!transport.IsPeerConnected() && last_seq > 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 Hz input
        }
        total_snapshots = std::max(total_snapshots, last_seq);
        // Clean disconnect; the server prunes us and re-arms the slot for the next cycle.
        transport.Close();
        LUMINUMBRA_CORE_INFO(
            "net-soak-client: player {} cycle {} done (last seq {}); disconnecting",
            player_id,
            cycle + 1,
            last_seq);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(250)); // let the server prune + re-arm
    }

    const bool ok = connected_cycles > 0 && total_snapshots > 0;
    LUMINUMBRA_CORE_INFO(
        "net-soak-client: player {} finished {} connected cycle(s), {} snapshots mirrored -> {}",
        player_id,
        connected_cycles,
        total_snapshots,
        ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
