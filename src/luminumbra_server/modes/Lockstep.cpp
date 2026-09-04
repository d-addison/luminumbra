#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

namespace {
// ---------------------------------------------------------------------------
//  lockstep transport: in-process loopback drive of BOTH peers (the gate
// path -- no sockets/ports). Each peer owns a ServerWorldRunner stepping the same
// world (same seed/preset => identical hashes); the host is the sim authority and
// both exchange world_hash + sub-hashes at the 30-tick cadence (the LREC1 checkpoint
// cadence). The adaptive horizon is HASH-NEUTRAL: it only decides WHEN a tick runs,
// never WHAT it computes, so the canonical 90-tick hash 0eac465289e7c88b is unchanged
// by lockstep ( hash revision: was 2fa007951a21e140 pre-wind-slot).
//
// Fault injection (LockstepFaultInjection gate):
//  - delay_input N: peer 1 withholds its (empty) input for the first N agreed ticks,
//    a DELAYED+DROPPED-then-released input; the horizon must ABSORB it (no desync).
//  - corrupt_tick T: peer 1's captured hashes corrupt from tick T (a real STATE
//    divergence); the oracle must HALT + dump the LREC1 at exactly T.
// ---------------------------------------------------------------------------

// Per-peer context the LockstepHooks' void* user points at. Owns the runner and the
// fault-injection state for THIS peer.
struct LockstepPeerContext {
    Luminumbra::Server::ServerWorldRunner* runner = nullptr;
    std::uint32_t client_id = 0;
    // Fault injection (peer 1 only): withhold inputs for the first `delay_input_ticks`
    // ticks, and corrupt captured hashes from `corrupt_from_tick` (0 = never).
    std::uint64_t corrupt_from_tick = 0;
};

std::vector<std::uint8_t> LockstepCollectInput(std::uint64_t /*tick*/, void* /*user*/) {
    // Headless: no player inputs today. The (empty) set still travels the lockstep path
    // so it carries real inputs unchanged once gameplay inputs exist.
    return {};
}

bool LockstepApplyStep(std::uint64_t /*tick*/,
                       const std::vector<std::uint8_t>& /*merged*/,
                       void* user) {
    auto* ctx = static_cast<LockstepPeerContext*>(user);
    const auto step = ctx->runner->RunFixedTicks(1);
    return step.ticks_executed == 1;
}

void LockstepCaptureHashes(std::uint64_t tick, Luminumbra::Net::HashMsg& out, void* user) {
    auto* ctx = static_cast<LockstepPeerContext*>(user);
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub;
    ctx->runner->ComputeWorldHashAndSubHashes(out.world_hash, sub);
    out.terrain = sub.terrain;
    out.water = sub.water;
    out.entities = sub.entities;
    // Fault injection: deliberately corrupt this peer's authoritative hashes from
    // corrupt_from_tick so the oracle MUST detect a divergence (prove it is not vacuous).
    if (ctx->corrupt_from_tick != 0 && tick >= ctx->corrupt_from_tick) {
        out.terrain = "dead" + out.terrain.substr(4);
        out.world_hash = "dead" + out.world_hash.substr(4);
    }
    out.tick = tick;
}

} // namespace

int RunLockstepLoopback(const ServerCliOptions& options) {
    LUMINUMBRA_CORE_INFO("Headless server LOCKSTEP loopback: preset={} seed={} ticks={} "
                         "delay_input={} corrupt_tick={}",
                         options.preset,
                         options.seed,
                         options.ticks,
                         options.lockstep_delay_input,
                         options.lockstep_corrupt_tick);

    // Two runners: host (client 0) + the one remote (client 1). Same seed/preset => the
    // two worlds tick identically and their hashes agree at every cadence.
    auto make_runner = [&](const char* name) {
        Luminumbra::Server::ServerWorldRunnerConfig cfg = RunnerConfigFrom(options);
        cfg.world_id.clear();
        cfg.world_name = name;
        cfg.autosave_interval_ticks = 0;
        return std::make_unique<Luminumbra::Server::ServerWorldRunner>(std::move(cfg));
    };
    auto host_runner = make_runner("Lockstep Host");
    auto peer_runner = make_runner("Lockstep Peer");
    if (!host_runner->Boot() || !peer_runner->Boot()) {
        LUMINUMBRA_CORE_ERROR("lockstep: a peer session failed to boot");
        return 1;
    }

    const std::uint64_t seed_num = std::strtoull(options.seed.c_str(), nullptr, 10);

    LockstepPeerContext host_ctx{host_runner.get(), 0, 0};
    LockstepPeerContext peer_ctx{peer_runner.get(), 1, options.lockstep_corrupt_tick};

    Luminumbra::Net::LockstepHooks hooks_template;
    hooks_template.collect_local_input = &LockstepCollectInput;
    hooks_template.apply_and_step = &LockstepApplyStep;
    hooks_template.capture_hashes = &LockstepCaptureHashes;

    auto [host_transport, peer_transport] = Luminumbra::Net::MakeLoopbackPair();

    auto host_cfg = [&] {
        Luminumbra::Net::LockstepConfig c;
        c.seed = seed_num;
        c.preset = options.preset;
        c.tick_rate_hz = 30;
        c.local_client_id = 0;
        c.peer_client_id = 1;
        return c;
    }();
    auto peer_cfg = host_cfg;
    peer_cfg.local_client_id = 1;
    peer_cfg.peer_client_id = 0;

    Luminumbra::Net::LockstepHooks host_hooks = hooks_template;
    host_hooks.user = &host_ctx;
    Luminumbra::Net::LockstepHooks peer_hooks = hooks_template;
    peer_hooks.user = &peer_ctx;

    Luminumbra::Net::LockstepSession host(host_cfg, host_transport.get(), host_hooks);
    Luminumbra::Net::LockstepSession peer(peer_cfg, peer_transport.get(), peer_hooks);

    const std::string dump_path =
        options.lockstep_dump_path.empty()
            ? (fs::temp_directory_path() / "lockstep-desync.lrec1").string()
            : options.lockstep_dump_path;
    host.SetDumpPath(dump_path);
    peer.SetDumpPath(dump_path + ".peer");

    // Handshake: send each Hello, then complete both (single-process driver order).
    // peer's Hello must be queued before host.Handshake looks for it -- peer.Handshake
    // sends peer's Hello into peer->host queue, then host.Handshake consumes it; host's
    // Hello (sent by host.Handshake) is then consumed by a second peer drain inside its
    // own PumpTick drain. To keep it simple+robust we send both Hellos first.
    {
        Luminumbra::Net::HelloMsg ph;
        ph.seed = seed_num;
        ph.preset = options.preset;
        ph.tick_rate_hz = 30;
        ph.client_id = 1;
        peer_transport->SendFrame(Luminumbra::Net::EncodeHello(ph));
        Luminumbra::Net::HelloMsg hh;
        hh.seed = seed_num;
        hh.preset = options.preset;
        hh.tick_rate_hz = 30;
        hh.client_id = 0;
        host_transport->SendFrame(Luminumbra::Net::EncodeHello(hh));
    }
    if (!host.Handshake() || !peer.Handshake()) {
        LUMINUMBRA_CORE_ERROR("lockstep: handshake failed");
        return 1;
    }

    // Drive both peers to the tick budget. delay_input is modeled by NOT pumping peer 1
    // for the first `delay_input` rounds (its inputs lag, forcing the host's horizon to
    // grow and absorb the jitter), then resuming both.
    const std::uint64_t budget = options.ticks;
    std::uint64_t delay_remaining = options.lockstep_delay_input;

    auto fatal = [](Luminumbra::Net::TickOutcome o) {
        return o == Luminumbra::Net::TickOutcome::Desync ||
               o == Luminumbra::Net::TickOutcome::PeerDisconnected;
    };
    Luminumbra::Net::TickResult hr, pr;
    hr.outcome = Luminumbra::Net::TickOutcome::WaitingForPeer;
    pr.outcome = Luminumbra::Net::TickOutcome::WaitingForPeer;
    const int max_pumps = static_cast<int>(budget) * 50 + 100000;
    int pumps = 0;
    bool diverged = false;
    for (; pumps < max_pumps; ++pumps) {
        hr = host.PumpTick(budget);
        if (delay_remaining > 0) {
            // Peer is stalled this round (delayed input). Decrement once the host has
            // actually waited (so the host horizon grows), else just hold the peer.
            --delay_remaining;
        } else {
            pr = peer.PumpTick(budget);
        }
        if (fatal(hr.outcome) || fatal(pr.outcome)) {
            diverged = true;
            break;
        }
        if (hr.outcome == Luminumbra::Net::TickOutcome::Finished &&
            pr.outcome == Luminumbra::Net::TickOutcome::Finished) {
            break;
        }
    }

    const auto host_status = host.Status();
    const auto peer_status = peer.Status();
    const bool desynced = host_status.desynced || peer_status.desynced;
    const std::uint64_t desync_tick =
        host_status.desynced ? host_status.desync_tick : peer_status.desync_tick;
    const std::string desync_section =
        host_status.desynced ? host_status.desync_section : peer_status.desync_section;
    const std::string emitted_dump =
        host_status.desynced ? host_status.dump_path : peer_status.dump_path;

    // Final hashes from BOTH worlds (settled), for the gate's identical-end-hash assert.
    const std::string host_hash = host_runner->ComputeWorldHash();
    const std::string peer_hash = peer_runner->ComputeWorldHash();

    const fs::path host_save = host_runner->Session()->GetWorldSaveDir();
    const fs::path peer_save = peer_runner->Session()->GetWorldSaveDir();
    host_runner->Shutdown();
    peer_runner->Shutdown();
    for (const fs::path& d : {host_save, peer_save}) {
        if (!d.empty()) {
            std::error_code ec;
            fs::remove_all(d, ec);
        }
    }

    // Scenario classification: corrupt => expect a halt+dump; otherwise expect in-sync.
    const bool expect_desync = options.lockstep_corrupt_tick != 0;
    bool passed = false;
    if (expect_desync) {
        passed = desynced && desync_tick == options.lockstep_corrupt_tick &&
                 !emitted_dump.empty() && fs::exists(emitted_dump);
    } else {
        passed = !desynced && host_status.agreed_tick == budget &&
                 peer_status.agreed_tick == budget && host_hash == peer_hash && !host_hash.empty();
    }

    nlohmann::json artifact{
        {"schema", "luminumbra.lockstep_loopback.v1"},
        {"generated_by", "luminumbra_server_app --lockstep-loopback ()"},
        {"preset", options.preset},
        {"seed", options.seed},
        {"tick_rate_hz", 30.0},
        {"ticks_requested", budget},
        {"hash_cadence_ticks", 30},
        {"delay_input_ticks", options.lockstep_delay_input},
        {"corrupt_tick", options.lockstep_corrupt_tick},
        {"expect_desync", expect_desync},
        {"desynced", desynced},
        {"desync_tick", desync_tick},
        {"desync_section", desync_section},
        {"dump_path", emitted_dump},
        {"dump_present", !emitted_dump.empty() && fs::exists(emitted_dump)},
        {"host",
         {
             {"agreed_tick", host_status.agreed_tick},
             {"final_horizon", host_status.horizon},
             {"max_horizon_reached", host_status.max_horizon_reached},
             {"late_input_events", host_status.late_input_events},
             {"world_hash", host_hash},
         }},
        {"peer",
         {
             {"agreed_tick", peer_status.agreed_tick},
             {"final_horizon", peer_status.horizon},
             {"max_horizon_reached", peer_status.max_horizon_reached},
             {"late_input_events", peer_status.late_input_events},
             {"world_hash", peer_hash},
         }},
        {"end_hashes_equal", host_hash == peer_hash},
        {"horizon_absorbed_jitter",
         options.lockstep_delay_input > 0 && !desynced && host_status.max_horizon_reached > 3},
        {"passed", passed},
    };

    if (!options.artifact_path.empty()) {
        const fs::path artifact_path(options.artifact_path);
        std::error_code ec;
        if (artifact_path.has_parent_path()) {
            fs::create_directories(artifact_path.parent_path(), ec);
        }
        std::ofstream out(artifact_path);
        if (out.is_open()) {
            out << artifact.dump(2) << "\n";
            LUMINUMBRA_CORE_INFO("lockstep artifact written: {}", options.artifact_path);
        } else {
            LUMINUMBRA_CORE_ERROR("lockstep: failed to write artifact '{}'", options.artifact_path);
            return 1;
        }
    }

    if (!passed) {
        LUMINUMBRA_CORE_ERROR(
            "lockstep loopback FAILED: expect_desync={} desynced={} desync_tick={} "
            "host_tick={} peer_tick={} host_hash={} peer_hash={}",
            expect_desync,
            desynced,
            desync_tick,
            host_status.agreed_tick,
            peer_status.agreed_tick,
            host_hash,
            peer_hash);
        return 1;
    }

    if (expect_desync) {
        LUMINUMBRA_CORE_INFO(
            "lockstep loopback passed: oracle HALTED at tick {} (section={}), LREC1 dump -> {}",
            desync_tick,
            desync_section,
            emitted_dump);
    } else {
        LUMINUMBRA_CORE_INFO(
            "lockstep loopback passed: {} ticks in sync, end_hash={} (host==peer), "
            "max_horizon={} late_inputs={}",
            budget,
            host_hash,
            host_status.max_horizon_reached,
            host_status.late_input_events);
    }
    (void)diverged;
    return 0;
}
