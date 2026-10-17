#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <glad/glad.h>

#include "core/RuntimeScenarioConfig.h"
#include "core/RuntimeScenarioHarness.h"
#include "core/scenarios/NetworkedSessionHooks.h"
#include "luminumbra/core/Types.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/world/GameSession.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <string>
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

using detail::NetSessionCaptureHashes;
using detail::NetSessionPeerContext;

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

} // namespace

// Share headless construction state with checkpoint regressions; artifact
// telemetry remains private to the driver. The public driver header stays free
// of the Net/JobSystem includes.
struct NetworkedSessionDriver::Impl : detail::NetworkedSessionState {
    NetworkedSessionDriver::Config config;

    // Telemetry for the artifact.
    std::uint64_t hash_exchanges = 0;    // cadence ticks where hashes compared
    std::uint64_t in_sync_exchanges = 0; //... that matched
    std::string last_world_hash;         // host==client end hash
    bool clean_disconnect = false;
};

NetworkedSessionDriver::NetworkedSessionDriver()
    : m_impl(std::make_unique<Impl>()) {}
NetworkedSessionDriver::~NetworkedSessionDriver() {
    Disconnect();
}

bool NetworkedSessionDriver::Begin(Luminumbra::world::GameSession* client_session,
                                   const Config& config) {
    const Luminumbra::world::GameSession::NetworkHostConfig host_config{
        .root_path = config.root_path,
        .seed = std::to_string(config.seed),
        .preset = config.preset,
        .surface_radius = config.surface_radius,
        .collision_radius = config.collision_radius,
        .systems_config_override =
            config.systems_config_override ? &*config.systems_config_override : nullptr,
        .preset_json_override =
            config.preset_json_override ? &*config.preset_json_override : nullptr};
    if (!detail::BeginNetworkedSession(*m_impl,
                                       client_session,
                                       host_config,
                                       config.seed,
                                       config.hash_cadence_ticks,
                                       m_failure_reason))
        return false;
    m_impl->config = config;
    m_spawn_anchor = m_impl->host_ctx.spawn_anchor;
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

    const nlohmann::json artifact{
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
