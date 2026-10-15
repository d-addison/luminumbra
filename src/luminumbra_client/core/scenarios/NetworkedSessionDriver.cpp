#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <glad/glad.h>

#include "core/Log.h"
#include "core/RuntimeScenarioHarness.h"
#include "core/scenarios/ScenarioCommon.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h" //  species base_color -> creature tint
#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/CreatureProcgen.h" //  genome -> body-proportion build
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include "rendering/Camera.h"
#include "rendering/LightningBolt.h"
#include "rendering/passes/FoliagePass.h"
#include "rendering/passes/ParticlePass.h"
// lockstep transport seam (engine-generic; ILockstepTransport +
// LoopbackTransport + LockstepSession). Named SendFrame/TryReceiveFrame to dodge
// the <windows.h> SendMessage macro (see LockstepSession.h note).
#include "luminumbra_common/net/LockstepSession.h"
//  (AU1): atmosphere audio telemetry. The harness sweeps the replicated
// weather/wind state through the REAL EnvironmentalAudioSystem atmosphere model +
// the AudioPropagationSystem ambience bed and emits the AtmosphereAudio artifact.
// Client-side dressing only -- no world_hash, no visual-gate dependency.
#include "audio/AudioPropagationSystem.h"
#include "audio/EnvironmentalAudioSystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace Luminumbra::Client::ScenarioHarness {

// ===========================================================================
// Client renders a server-owned world over
// the lockstep transport. See the header for the full contract. This is engine
// wiring: the transport + session are engine-generic (Luminumbra::Net) and the
// hashed world step uses the spawn anchor, so no game concept leaks into the
// lockstep path and render-side camera look never perturbs the hash.
// ===========================================================================

namespace {

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
    // of any of them is ever added to this context, the size assert below fails to compile —
    // forcing a revisit of the server-authoritative hash boundary before the quantities can
    // diverge.
};
namespace net_capture_tripwire {
// Reference layout: session, hashed-step anchor, fixed dt and shared configuration.
// No wind/weather/aether/scent/ecology field state.
struct ExpectedPeerContextLayout {
    Luminumbra::world::GameSession* session = nullptr;
    Luminumbra::Vec3 spawn_anchor{0.0f};
    double fixed_dt = 1.0 / 30.0;
    const luminumbra::core::SystemConfig* configuration = nullptr;
};
// Captured message layout stays tick + world/terrain/water/entities in both modes.
struct ExpectedHashMsgLayout {
    std::uint64_t tick = 0;
    std::string world_hash, terrain, water, entities;
};
} // namespace net_capture_tripwire
static_assert(sizeof(NetSessionPeerContext) ==
                  sizeof(net_capture_tripwire::ExpectedPeerContextLayout),
              "NetSessionPeerContext gained a member: the client peer must NOT own "
              "independent wind/weather/aether/scent/ecology state. Revisit the "
              "server-authoritative hash boundary before adding sub-term state here.");
static_assert(sizeof(Luminumbra::Net::HashMsg) ==
                  sizeof(net_capture_tripwire::ExpectedHashMsgLayout),
              "Luminumbra::Net::HashMsg shape changed: named diagnostics must remain "
              "outside the checkpoint wire layout.");

// the client's WORLD-AFFECTING input set is EMPTY today (no gameplay
// inputs yet). The empty blob still travels the lockstep path -- it is collected
// here, sent through LockstepSession::*Input, merged, and handed to apply_and_step
// below -- so the round-trip is real and carries real inputs unchanged once
// gameplay inputs exist. Camera LOOK is deliberately NOT collected here: it is
// render state applied locally each frame, never round-tripped.
std::vector<std::uint8_t> NetSessionCollectInput(std::uint64_t /*tick*/, void* /*user*/) {
    return {};
}

// Applies the agreed merged input set (empty today) and advances the world by
// EXACTLY one fixed sim tick from the SPAWN ANCHOR. Identical step shape on both
// peers (same fixed_dt, same anchor, same quiesce) => byte-identical worlds =>
// matching hashes. The merged input would be decoded + applied here once gameplay
// inputs exist; the camera is intentionally absent from this hashed path.
bool NetSessionApplyStep(std::uint64_t /*tick*/,
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
void NetSessionCaptureHashes(std::uint64_t tick, Luminumbra::Net::HashMsg& out, void* user) {
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

} // namespace

// Hidden state: the host authority world + job system, both transports, both
// session ends, and the per-peer step contexts. Kept in the.cpp so the header
// stays free of the Net/JobSystem includes.
struct NetworkedSessionDriver::Impl {
    Luminumbra::JobSystem host_job_system;
    std::unique_ptr<Luminumbra::world::GameSession> host_session;

    std::unique_ptr<Luminumbra::Net::LoopbackTransport> host_transport;
    std::unique_ptr<Luminumbra::Net::LoopbackTransport> client_transport;
    std::unique_ptr<Luminumbra::Net::LockstepSession> host;
    std::unique_ptr<Luminumbra::Net::LockstepSession> client;

    std::shared_ptr<const luminumbra::core::SystemConfig> simulation_configuration;
    NetSessionPeerContext host_ctx;
    NetSessionPeerContext client_ctx;

    NetworkedSessionDriver::Config config;

    // Telemetry for the artifact.
    std::uint64_t hash_exchanges = 0;    // cadence ticks where hashes compared
    std::uint64_t in_sync_exchanges = 0; //... that matched
    std::string last_world_hash;         // host==client end hash
    bool clean_disconnect = false;
    bool host_booted = false;
};

NetworkedSessionDriver::NetworkedSessionDriver()
    : m_impl(std::make_unique<Impl>()) {}
NetworkedSessionDriver::~NetworkedSessionDriver() {
    Disconnect();
    if (m_impl && m_impl->host_session) {
        if (auto* ws = m_impl->host_session->GetWorldSystem()) {
            ws->clear_world(m_impl->host_session->GetPhysicsSystem());
        }
    }
    if (m_impl && m_impl->host_booted) {
        m_impl->host_session.reset();
        m_impl->host_job_system.shutdown();
        m_impl->host_booted = false;
    }
}

bool NetworkedSessionDriver::Begin(Luminumbra::world::GameSession* client_session,
                                   const Config& config) {
    if (!client_session) {
        m_failure_reason = "client_session_null";
        return false;
    }
    m_impl->simulation_configuration = client_session->GetSimulationConfiguration();
    if (!m_impl->simulation_configuration) {
        m_failure_reason = "simulation_configuration_missing";
        return false;
    }
    m_impl->config = config;

    // --- Boot the HOST authority world (a headless GameSession in this process).
    // Same seed/preset as the client => the two worlds tick identically and their
    // hashes agree at every cadence. Mirrors ServerWorldRunner::Boot.
    m_impl->host_job_system.startup(1); // single worker: streamed hash is worker-count invariant
    m_impl->host_booted = true;
    const Luminumbra::world::GameSession::NetworkHostConfig host_config{config.root_path,
                                                                        std::to_string(config.seed),
                                                                        config.preset,
                                                                        config.surface_radius,
                                                                        config.collision_radius};
    m_impl->host_session = Luminumbra::world::GameSession::CreateNetworkHost(
        *client_session, m_impl->host_job_system, host_config, m_failure_reason);
    if (!m_impl->host_session)
        return false;
    m_spawn_anchor = m_impl->host_session->GetMetadata().spawnPoint;

    const double fixed_dt = m_impl->host_session->GetSimulationClock().fixed_dt();
    m_impl->host_ctx = NetSessionPeerContext{m_impl->host_session.get(),
                                             m_spawn_anchor,
                                             fixed_dt,
                                             m_impl->simulation_configuration.get()};
    m_impl->client_ctx = NetSessionPeerContext{
        client_session, m_spawn_anchor, fixed_dt, m_impl->simulation_configuration.get()};

    // --- Build the loopback pair + both session ends (no sockets/ports). The
    // client is client_id 1 (one remote) and the host is client_id 0 (authority).
    auto [host_transport, client_transport] = Luminumbra::Net::MakeLoopbackPair();
    m_impl->host_transport = std::move(host_transport);
    m_impl->client_transport = std::move(client_transport);

    Luminumbra::Net::LockstepHooks hooks_template;
    hooks_template.collect_local_input = &NetSessionCollectInput;
    hooks_template.apply_and_step = &NetSessionApplyStep;
    hooks_template.capture_hashes = &NetSessionCaptureHashes;

    Luminumbra::Net::LockstepConfig host_cfg;
    host_cfg.seed = config.seed;
    host_cfg.preset = config.preset;
    host_cfg.tick_rate_hz = 30;
    host_cfg.local_client_id = 0;
    host_cfg.peer_client_id = 1;
    host_cfg.hash_cadence_ticks = config.hash_cadence_ticks;
    Luminumbra::Net::LockstepConfig client_cfg = host_cfg;
    client_cfg.local_client_id = 1;
    client_cfg.peer_client_id = 0;

    Luminumbra::Net::LockstepHooks host_hooks = hooks_template;
    host_hooks.user = &m_impl->host_ctx;
    Luminumbra::Net::LockstepHooks client_hooks = hooks_template;
    client_hooks.user = &m_impl->client_ctx;

    m_impl->host = std::make_unique<Luminumbra::Net::LockstepSession>(
        host_cfg, m_impl->host_transport.get(), host_hooks);
    m_impl->client = std::make_unique<Luminumbra::Net::LockstepSession>(
        client_cfg, m_impl->client_transport.get(), client_hooks);

    // Handshake: queue both Hellos first, then complete both ends (single-process
    // driver order, exactly as RunLockstepLoopback does).
    {
        Luminumbra::Net::HelloMsg ch;
        ch.seed = config.seed;
        ch.preset = config.preset;
        ch.tick_rate_hz = 30;
        ch.client_id = 1;
        m_impl->client_transport->SendFrame(Luminumbra::Net::EncodeHello(ch));
        Luminumbra::Net::HelloMsg hh;
        hh.seed = config.seed;
        hh.preset = config.preset;
        hh.tick_rate_hz = 30;
        hh.client_id = 0;
        m_impl->host_transport->SendFrame(Luminumbra::Net::EncodeHello(hh));
    }
    if (!m_impl->host->Handshake() || !m_impl->client->Handshake()) {
        m_failure_reason = "handshake_failed";
        return false;
    }
    return true;
}

bool NetworkedSessionDriver::StepAgreedTick() {
    if (m_finished || m_desynced || !m_impl->host || !m_impl->client) {
        return false;
    }
    const std::uint64_t budget = m_impl->config.budget_ticks;

    // Pump both peers until BOTH advance one more agreed tick (or terminate). The
    // loopback is in-process, so a bounded pump count is enough; a runaway is
    // treated as a stall failure rather than an infinite loop.
    const std::uint64_t target = m_agreed_ticks + 1;
    auto fatal = [](Luminumbra::Net::TickOutcome o) {
        return o == Luminumbra::Net::TickOutcome::Desync ||
               o == Luminumbra::Net::TickOutcome::PeerDisconnected;
    };
    const int max_pumps = 2000;
    for (int pumps = 0; pumps < max_pumps; ++pumps) {
        const Luminumbra::Net::TickResult hr = m_impl->host->PumpTick(budget);
        const Luminumbra::Net::TickResult cr = m_impl->client->PumpTick(budget);

        // A cadence hash exchange ran on the client side this pump -> compare.
        if (cr.ran_hash_exchange || hr.ran_hash_exchange) {
            m_impl->hash_exchanges += 1;
        }

        if (fatal(hr.outcome) || fatal(cr.outcome)) {
            const auto hs = m_impl->host->Status();
            const auto cs = m_impl->client->Status();
            if (hs.desynced || cs.desynced) {
                m_desynced = true;
                m_failure_reason =
                    "desync_tick_" + std::to_string(hs.desynced ? hs.desync_tick : cs.desync_tick);
            }
            m_finished = true;
            return false;
        }
        if (hr.outcome == Luminumbra::Net::TickOutcome::Finished &&
            cr.outcome == Luminumbra::Net::TickOutcome::Finished) {
            m_agreed_ticks = m_impl->host->Status().agreed_tick;
            m_finished = true;
            return false;
        }
        const std::uint64_t agreed =
            std::min(m_impl->host->Status().agreed_tick, m_impl->client->Status().agreed_tick);
        if (agreed >= target) {
            m_agreed_ticks = agreed;
            return true;
        }
    }
    m_failure_reason = "pump_stalled_at_tick_" + std::to_string(m_agreed_ticks);
    m_finished = true;
    return false;
}

void NetworkedSessionDriver::Disconnect() {
    if (m_disconnected || !m_impl) {
        return;
    }
    if (m_impl->host) {
        m_impl->host->Disconnect();
    }
    if (m_impl->client) {
        m_impl->client->Disconnect();
    }
    m_disconnected = true;
    // A clean Bye on both ends with no desync == clean shutdown.
    if (!m_desynced) {
        m_impl->clean_disconnect = true;
    }
}

bool NetworkedSessionDriver::WriteArtifact(const std::filesystem::path& artifact_dir,
                                           double duration_seconds) {
    // Settle both worlds and capture the final hashes for the equality assert.
    std::string host_hash;
    std::string client_hash;
    if (m_impl->host_session && m_impl->host_session->GetWorldSystem() &&
        m_impl->client_ctx.session && m_impl->client_ctx.session->GetWorldSystem()) {
        try {
            Luminumbra::Net::HashMsg hm;
            NetSessionCaptureHashes(m_agreed_ticks, hm, &m_impl->host_ctx);
            host_hash = hm.world_hash;
            Luminumbra::Net::HashMsg cm;
            NetSessionCaptureHashes(m_agreed_ticks, cm, &m_impl->client_ctx);
            client_hash = cm.world_hash;
        } catch (const std::exception&) {
            m_impl->last_world_hash.clear();
            m_failure_reason = "authority_capture_failed";
            return false;
        }
    }
    m_impl->last_world_hash = host_hash;

    const auto host_status =
        m_impl->host ? m_impl->host->Status() : Luminumbra::Net::LockstepStatus{};
    const auto client_status =
        m_impl->client ? m_impl->client->Status() : Luminumbra::Net::LockstepStatus{};

    const bool end_hashes_equal = !host_hash.empty() && host_hash == client_hash;
    const bool reached_budget = host_status.agreed_tick == m_impl->config.budget_ticks &&
                                client_status.agreed_tick == m_impl->config.budget_ticks;
    const bool passed = m_failure_reason.empty() && !m_desynced && reached_budget &&
                        end_hashes_equal && m_impl->clean_disconnect;

    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();

    nlohmann::json artifact{
        {"schema", "luminumbra.networked_session.v1"},
        {"generated_by", "luminumbra_client_qa_app --scenario networked_session_smoke ()"},
        {"preset", m_impl->config.preset},
        {"seed", std::to_string(m_impl->config.seed)},
        {"tick_rate_hz", 30.0},
        {"ticks_requested", m_impl->config.budget_ticks},
        {"hash_cadence_ticks", m_impl->config.hash_cadence_ticks},
        {"duration_seconds", duration_seconds},
        {"transport", "loopback"},
        {"agreed_ticks", m_agreed_ticks},
        {"hash_exchanges", m_impl->hash_exchanges},
        {"in_sync_every_cadence", !m_desynced && m_impl->hash_exchanges > 0},
        {"desynced", m_desynced},
        {"end_hashes_equal", end_hashes_equal},
        {"end_hash", host_hash},
        {"camera_look_render_side", true},
        {"input_round_tripped", true},
        {"clean_disconnect", m_impl->clean_disconnect},
        {"failure_reason", m_failure_reason},
        {"host",
         {
             {"agreed_tick", host_status.agreed_tick},
             {"max_horizon_reached", host_status.max_horizon_reached},
             {"late_input_events", host_status.late_input_events},
             {"world_hash", host_hash},
             {"peer_disconnected", host_status.peer_disconnected},
         }},
        {"client",
         {
             {"agreed_tick", client_status.agreed_tick},
             {"max_horizon_reached", client_status.max_horizon_reached},
             {"late_input_events", client_status.late_input_events},
             {"world_hash", client_hash},
             {"peer_disconnected", client_status.peer_disconnected},
         }},
        {"gl_debug",
         {
             {"errors", gl_debug.errors},
             {"warnings", gl_debug.warnings},
         }},
        {"passed", passed},
    };

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream output(artifact_dir / "networked-session-analysis.json");
    output << std::setw(2) << artifact << '\n';
    return passed;
}

} // namespace Luminumbra::Client::ScenarioHarness
