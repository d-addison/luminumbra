#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

#ifdef LUMINUMBRA_ENABLE_STEAM
// ---------------------------------------------------------------------------
// the SAME authoritative-server replication over the STEAMWORKS transport
// (ISteamNetworkingSockets -> real UDP + reliability + encryption). Identical
// loop to RunNetHost/RunNetJoin; only the transport type + the async connect
// (poll after RunCallbacks, vs TCP's blocking accept) differ. Needs the Steam
// client running + a dev app id (480). Built only with -DLUMINUMBRA_ENABLE_STEAM=ON.
// ---------------------------------------------------------------------------
int RunSteamHost(const ServerCliOptions& options) {
    if (!Luminumbra::Net::SteamLink::Init()) {
        LUMINUMBRA_CORE_ERROR(
            "steam-host: Steam not available -- start the Steam client and retry.");
        return 1;
    }
    Luminumbra::Server::ServerWorldRunnerConfig config = RunnerConfigFrom(options);
    config.world_id.clear();
    config.world_name = "Steam Host";
    config.autosave_interval_ticks = 0;
    if (config.avatar_count <= 1)
        config.avatar_count = 2;
    LUMINUMBRA_CORE_INFO(
        "Steam HOST: preset={} seed={} avatars={} ticks={} -- listening on UDP port {} (Steam)",
        options.preset,
        options.seed,
        config.avatar_count,
        options.ticks,
        options.port);

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        LUMINUMBRA_CORE_ERROR("steam-host: boot failed");
        return 1;
    }

    Luminumbra::Net::SteamNetworkingTransport transport;
    if (!transport.Listen(options.port)) {
        LUMINUMBRA_CORE_ERROR("steam-host: CreateListenSocketIP failed on port {}", options.port);
        return 1;
    }
    LUMINUMBRA_CORE_INFO("steam-host: listening over Steam UDP; waiting for a peer (30s)...");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!transport.IsPeerConnected() && std::chrono::steady_clock::now() < deadline) {
        Luminumbra::Net::SteamLink::RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!transport.IsPeerConnected()) {
        LUMINUMBRA_CORE_ERROR("steam-host: no peer connected within timeout");
        return 1;
    }
    LUMINUMBRA_CORE_INFO("steam-host: peer connected over Steam UDP.");

    Luminumbra::Net::ReplicationServer server;
    server.AddClient(1, &transport);
    server.SetAoiChunkRadius(3, Luminumbra::CHUNK_SIZE_X * 1000);
    const std::uint32_t controlled = runner.Avatars().size() > 1 ? 1u : 0u;
    const float initial_x =
        runner.Avatars().empty() ? 0.0f : runner.Avatars()[controlled].position.x;

    std::uint64_t executed = 0;
    while (executed < options.ticks) {
        Luminumbra::Net::SteamLink::RunCallbacks();
        server.PumpInbound();
        if (const Luminumbra::Net::UsercmdMsg* got = server.LatestUsercmd(1)) {
            runner.SetAvatarMove(got->player_id,
                                 static_cast<float>(got->move_x) / 32767.0f,
                                 static_cast<float>(got->move_z) / 32767.0f);
        }
        const auto step = runner.RunFixedTicks(1);
        executed += step.ticks_executed;
        if (step.ticks_executed == 0) {
            LUMINUMBRA_CORE_ERROR("steam-host: tick stalled");
            return 1;
        }
        const auto states = Luminumbra::World::BuildAvatarReplStates(runner.Avatars());
        server.BroadcastSnapshot(executed, states);
        if (!transport.IsPeerConnected()) {
            LUMINUMBRA_CORE_WARN("steam-host: peer left at tick {}", executed);
            break;
        }
    }
    const float final_x = runner.Avatars().empty() ? 0.0f : runner.Avatars()[controlled].position.x;
    LUMINUMBRA_CORE_INFO("steam-host: ran {} ticks over Steam UDP, {} avatars; controlled avatar "
                         "moved {:.2f} m in X.",
                         executed,
                         runner.Avatars().size(),
                         final_x - initial_x);
    // Flush + linger so the last reliable frames arrive before close.
    for (int i = 0; i < 50; ++i) {
        Luminumbra::Net::SteamLink::RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    transport.Close();
    Luminumbra::Net::SteamLink::Shutdown();
    return (executed == options.ticks) ? 0 : 1;
}

int RunSteamJoin(const ServerCliOptions& options) {
    if (!Luminumbra::Net::SteamLink::Init()) {
        LUMINUMBRA_CORE_ERROR(
            "steam-join: Steam not available -- start the Steam client and retry.");
        return 1;
    }
    LUMINUMBRA_CORE_INFO(
        "Steam JOIN: connecting to {}:{} over Steam UDP...", options.host, options.port);
    Luminumbra::Net::SteamNetworkingTransport transport;
    if (!transport.Connect(options.host, options.port)) {
        LUMINUMBRA_CORE_ERROR("steam-join: ConnectByIPAddress failed");
        return 1;
    }
    const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!transport.IsPeerConnected() && std::chrono::steady_clock::now() < connect_deadline) {
        Luminumbra::Net::SteamLink::RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!transport.IsPeerConnected()) {
        LUMINUMBRA_CORE_ERROR("steam-join: connect timed out");
        return 1;
    }
    LUMINUMBRA_CORE_INFO("steam-join: connected over Steam UDP.");

    Luminumbra::Net::ReplicationClient client(1, &transport);
    std::uint32_t last_seq = 0;
    std::size_t max_entities = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        Luminumbra::Net::SteamLink::RunCallbacks();
        Luminumbra::Net::UsercmdMsg cmd;
        cmd.tick = last_seq + 1;
        cmd.player_id = 1;
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
    const bool ok = client.has_snapshot() && max_entities >= 2 && last_seq > 0;
    if (!ok) {
        LUMINUMBRA_CORE_ERROR(
            "steam-join: did not mirror the host (has_snapshot={} max_entities={} last_seq={})",
            client.has_snapshot(),
            max_entities,
            last_seq);
        transport.Close();
        Luminumbra::Net::SteamLink::Shutdown();
        return 1;
    }
    float mirror_x = 0.0f;
    for (const auto& e : client.snapshot().entities) {
        if (e.entity_id == 1u)
            mirror_x = Luminumbra::Net::ReplDequantPos(e.px_mm);
    }
    LUMINUMBRA_CORE_INFO(
        "steam-join: mirrored host over STEAM UDP -- last seq {}, up to {} entities; controlled "
        "avatar (id 1) at x={:.2f} m. Real Steam-transport replication confirmed.",
        last_seq,
        max_entities,
        mirror_x);
    transport.Close();
    Luminumbra::Net::SteamLink::Shutdown();
    return 0;
}
#endif // LUMINUMBRA_ENABLE_STEAM
