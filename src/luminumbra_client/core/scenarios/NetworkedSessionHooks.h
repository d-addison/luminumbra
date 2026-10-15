#pragma once

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace Luminumbra::Client::ScenarioHarness::detail {

// Shared by the driver and headless checkpoint regressions.
// One peer's world-step + hash context the LockstepHooks' void* user points at.
// Mirrors the headless server's LockstepPeerContext, but a peer here owns a live
// GameSession (host: a dedicated authority world; client: the caller's render
// session) plus the physics + spawn anchor it steps from.
struct NetSessionPeerContext {
    Luminumbra::world::GameSession* session = nullptr;
    Luminumbra::Vec3 spawn_anchor{0.0f};
    double fixed_dt = 1.0 / 30.0;
    const luminumbra::core::SystemConfig* configuration = nullptr;
    // The peer owns no
    // independent wind/weather/aether/scent/ecology field state — it renders the
    // server-authoritative world via `session`. Enabled hashes use its complete authority. Those
    // fields are SERVER-side (GameSession::Get*FieldSystem / ComputeScentSubHash /
    // ai::ComputeEcologySubHash, folded by the server's ComposeWorldHash). If a client-owned copy
    // of any of them is ever added to this context, the driver's size assert fails to compile —
    // forcing a revisit of the server-authoritative hash boundary before the quantities can
    // diverge.
};

// the client's WORLD-AFFECTING input set is EMPTY today (no gameplay
// inputs yet). The empty blob still travels the lockstep path -- it is collected
// here, sent through LockstepSession::*Input, merged, and handed to apply_and_step
// below -- so the round-trip is real and carries real inputs unchanged once
// gameplay inputs exist. Camera LOOK is deliberately NOT collected here: it is
// render state applied locally each frame, never round-tripped.
inline std::vector<std::uint8_t> NetSessionCollectInput(std::uint64_t /*tick*/, void* /*user*/) {
    return {};
}

// Applies the agreed merged input set (empty today) and advances the world by
// EXACTLY one fixed sim tick from the SPAWN ANCHOR. Identical step shape on both
// peers (same fixed_dt, same anchor, same quiesce) => byte-identical worlds =>
// matching hashes. The merged input would be decoded + applied here once gameplay
// inputs exist; the camera is intentionally absent from this hashed path.
inline bool NetSessionApplyStep(std::uint64_t /*tick*/,
                                const std::vector<std::uint8_t>& /*merged*/,
                                void* user) {
    auto* ctx = static_cast<NetSessionPeerContext*>(user);
    if (!ctx || !ctx->session || !ctx->configuration ||
        ctx->session->ActiveRegionsEnabled() !=
            ctx->configuration->enabled(luminumbra::core::SysKey::SimActiveRegions)) {
        return false;
    }
    auto* world_system = ctx->session->GetWorldSystem();
    auto* physics_system = ctx->session->GetPhysicsSystem();
    if (!world_system || !physics_system) {
        return false;
    }
    // Same per-tick shape as ServerWorldRunner::RunFixedTicks (one frame == one
    // fixed tick): physics, then the fixed sim tick, then spawn-anchor streaming,
    // then quiesce so the next scheduler decision observes the identical settled
    // state on both peers. The streaming anchor is the spawn point -- NOT the
    // client camera -- so render-side look can never alter the hashed world.
    if (ctx->configuration->enabled(luminumbra::core::SysKey::SimActiveRegions))
        ctx->session->SetReplicatedSimulationAnchors({}); // Same zero-avatar host inputs.
    physics_system->update(static_cast<float>(ctx->fixed_dt));
    const std::uint32_t ran = ctx->session->TickSimulation(ctx->fixed_dt);
    if (ctx->session->GetRegionSchedulerFault())
        return false;
    world_system->update(ctx->session->GetRegistry(), ctx->spawn_anchor, physics_system);
    world_system->wait_for_streaming_jobs();
    return ran == 1;
}

// Enabled peers capture the same admitted session authority as the server runner.
// The disabled scenario retains its historical bare streamed-chunk projection.
inline void NetSessionCaptureHashes(std::uint64_t tick, Luminumbra::Net::HashMsg& out, void* user) {
    auto* ctx = static_cast<NetSessionPeerContext*>(user);
    out = {};
    out.tick = tick;
    if (!ctx || !ctx->session || !ctx->configuration)
        throw std::runtime_error("Network authority capture requires a configured live session");
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub;
    if (!ctx->session->ComputeNetworkWorldHashes(*ctx->configuration, out.world_hash, sub))
        throw std::runtime_error("Network authority capture failed");
    out.terrain = sub.terrain;
    out.water = sub.water;
    out.entities = sub.entities;
}

inline Luminumbra::Net::LockstepHooks MakeNetworkedSessionHooks(NetSessionPeerContext& context) {
    Luminumbra::Net::LockstepHooks hooks;
    hooks.collect_local_input = &NetSessionCollectInput;
    hooks.apply_and_step = &NetSessionApplyStep;
    hooks.capture_hashes = &NetSessionCaptureHashes;
    hooks.user = &context;
    return hooks;
}

// Owns the headless host and both lockstep peers. The caller's world-ready client
// session must outlive this state. Contexts and configuration stay at stable
// addresses until the lockstep peers are destroyed.
struct NetworkedSessionState {
    Luminumbra::JobSystem host_job_system;
    std::unique_ptr<Luminumbra::world::GameSession> host_session;
    std::shared_ptr<const luminumbra::core::SystemConfig> simulation_configuration;
    NetSessionPeerContext host_ctx;
    NetSessionPeerContext client_ctx;
    std::shared_ptr<Luminumbra::Net::LoopbackTransport::Channel> host_to_client;
    std::shared_ptr<Luminumbra::Net::LoopbackTransport::Channel> client_to_host;
    std::unique_ptr<Luminumbra::Net::LoopbackTransport> host_transport;
    std::unique_ptr<Luminumbra::Net::LoopbackTransport> client_transport;
    std::unique_ptr<Luminumbra::Net::LockstepSession> host;
    std::unique_ptr<Luminumbra::Net::LockstepSession> client;
    bool host_booted = false;

    ~NetworkedSessionState() {
        if (host)
            host->Disconnect();
        if (client)
            client->Disconnect();
        host.reset();
        client.reset();
        if (host_session) {
            if (auto* world_system = host_session->GetWorldSystem())
                world_system->clear_world(host_session->GetPhysicsSystem());
            host_session.reset();
        }
        if (host_booted)
            host_job_system.shutdown();
    }
};

// Complete production construction path, independent of rendering. Begin and
// headless regressions both use this function; neither installs hooks separately.
// As with Begin, use a fresh state and a world-ready client session.
inline bool
BeginNetworkedSession(NetworkedSessionState& state,
                      Luminumbra::world::GameSession* client_session,
                      const Luminumbra::world::GameSession::NetworkHostConfig& host_config,
                      std::uint64_t seed,
                      std::uint64_t hash_cadence_ticks,
                      std::string& failure_reason) {
    if (!client_session) {
        failure_reason = "client_session_null";
        return false;
    }
    state.simulation_configuration = client_session->GetSimulationConfiguration();
    if (!state.simulation_configuration) {
        failure_reason = "simulation_configuration_missing";
        return false;
    }

    // --- Boot the HOST authority world (a headless GameSession in this process).
    // Same seed/preset as the client => the two worlds tick identically and their
    // hashes agree at every cadence. Mirrors ServerWorldRunner::Boot.
    state.host_job_system.startup(1); // single worker: streamed hash is worker-count invariant
    state.host_booted = true;
    state.host_session = Luminumbra::world::GameSession::CreateNetworkHost(
        *client_session, state.host_job_system, host_config, failure_reason);
    if (!state.host_session)
        return false;
    const auto spawn_anchor = state.host_session->GetMetadata().spawnPoint;

    const double fixed_dt = state.host_session->GetSimulationClock().fixed_dt();
    state.host_ctx = NetSessionPeerContext{
        state.host_session.get(), spawn_anchor, fixed_dt, state.simulation_configuration.get()};
    state.client_ctx = NetSessionPeerContext{
        client_session, spawn_anchor, fixed_dt, state.simulation_configuration.get()};

    // --- Build the loopback pair + both session ends (no sockets/ports). The
    // client is client_id 1 (one remote) and the host is client_id 0 (authority).
    state.host_to_client = std::make_shared<Luminumbra::Net::LoopbackTransport::Channel>();
    state.client_to_host = std::make_shared<Luminumbra::Net::LoopbackTransport::Channel>();
    state.host_transport = std::make_unique<Luminumbra::Net::LoopbackTransport>(
        state.host_to_client, state.client_to_host);
    state.client_transport = std::make_unique<Luminumbra::Net::LoopbackTransport>(
        state.client_to_host, state.host_to_client);

    Luminumbra::Net::LockstepConfig host_cfg;
    host_cfg.seed = seed;
    host_cfg.preset = host_config.preset;
    host_cfg.tick_rate_hz = 30;
    host_cfg.local_client_id = 0;
    host_cfg.peer_client_id = 1;
    host_cfg.hash_cadence_ticks = hash_cadence_ticks;
    Luminumbra::Net::LockstepConfig client_cfg = host_cfg;
    client_cfg.local_client_id = 1;
    client_cfg.peer_client_id = 0;

    const auto host_hooks = MakeNetworkedSessionHooks(state.host_ctx);
    const auto client_hooks = MakeNetworkedSessionHooks(state.client_ctx);

    state.host = std::make_unique<Luminumbra::Net::LockstepSession>(
        host_cfg, state.host_transport.get(), host_hooks);
    state.client = std::make_unique<Luminumbra::Net::LockstepSession>(
        client_cfg, state.client_transport.get(), client_hooks);

    // Handshake: queue both Hellos first, then complete both ends (single-process
    // driver order, exactly as RunLockstepLoopback does).
    {
        Luminumbra::Net::HelloMsg ch;
        ch.seed = seed;
        ch.preset = host_config.preset;
        ch.tick_rate_hz = 30;
        ch.client_id = 1;
        state.client_transport->SendFrame(Luminumbra::Net::EncodeHello(ch));
        Luminumbra::Net::HelloMsg hh;
        hh.seed = seed;
        hh.preset = host_config.preset;
        hh.tick_rate_hz = 30;
        hh.client_id = 0;
        state.host_transport->SendFrame(Luminumbra::Net::EncodeHello(hh));
    }
    if (!state.host->Handshake() || !state.client->Handshake()) {
        failure_reason = "handshake_failed";
        return false;
    }
    failure_reason.clear();
    return true;
}

} // namespace Luminumbra::Client::ScenarioHarness::detail
