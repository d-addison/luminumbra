// OVER-THE-WIRE 32-client replication SOAK + the frozen net_soak.v1 baseline.
//
// Live over-the-wire validation before this landed was only N=4 (the multiprocess
// `luminumbra_server_app --net-soak` + N `--net-soak-client` processes); the 32-client
// budgets were hand-set constants. THIS gate closes that gap with a SELF-CONTAINED,
// single-executable soak that spins the whole 32-client session in-process over REAL TCP
// sockets on loopback -- no second process, no per-client port scheme, no orchestration.
//
// Shape (the real  dedicated-server accept shape):
//   * ONE authoritative ReplicationServer on the MAIN thread.
//   * ONE TcpListener bound to ONE OS-assigned ephemeral port (Listen(0)); it fans the
//     32 inbound connections out via AcceptOneBlocking -> TcpTransport::FromAcceptedSocket,
//     each registered with a distinct server-side client id (1..N) via AddClient.
//   * 32 driving-avatar CLIENTS, each on its OWN thread with its OWN TcpTransport +
//     ReplicationClient, connected to 127.0.0.1:<port>. Each thread streams usercmds and
//     mirrors+acks snapshots -- realistic upstream traffic that keeps acks/backpressure live.
//
// THREADING INVARIANT (why this is race-free without a single mutex): every socket object
// has exactly ONE owning thread. The server + all N accepted server-side transports are
// touched ONLY by the main thread (PumpInbound / BroadcastSnapshot); each client + its
// transport is touched ONLY by its own thread. The sole cross-thread objects are the TCP
// sockets themselves, which the kernel synchronizes. Cross-thread signalling is a handful
// of std::atomics + the std::thread::join happens-before edge (main reads each thread's
// computed result only AFTER joining it).
//
// The server ticks K times at 30 Hz (paced -- NOT flat-out; the between-tick window is what
// lets clients ack so snapshot-age stays ~1) with MARCHING avatars, MEASURING per real
// broadcast: total wire bytes, worst per-client bytes, queue-depth p95, snapshot-age p95,
// and per-tick processing time (work only, excluding the pace-sleep). Those MEASURED values
// are FROZEN into a luminumbra.net_soak.v1 JSON artifact under LUMINUMBRA_TEST_ARTIFACT_DIR
// -- that written artifact IS the frozen baseline the manual-tier NetSoak gate reads. The
// run then ASSERTS every measured value is within an explicit, bounded budget AND that all
// 32 clients converged to the identical authoritative state hash.
//
// This is a MANUAL-TIER soak: real sockets + 32 threads + 30 Hz wall-clock pacing make it a
// wall-clock/OS-bound run, NOT a deterministic ctest (the standing constraint: never gate
// the default lane on multiprocess/socket timing -- it flakes). It is meant to be invoked
// explicitly by `validate-engine-frontier.ps1 -Mode NetSoak`, which reads the artifact.
//
// Engine-generic + world_hash-neutral: this is transport-side glue; nothing here feeds the
// simulation determinism hash.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "luminumbra_common/net/LockstepSession.h"     // TcpListener / TcpTransport
#include "luminumbra_common/net/ReplicationEndpoint.h" // ReplicationServer / ReplicationClient
#include "luminumbra_common/net/ReplicationProtocol.h" // UsercmdMsg / ReplEntityState

namespace fs = std::filesystem;
using namespace Luminumbra::Net;
using clock_type = std::chrono::steady_clock;

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

namespace {

// --- Soak size + budgets (mirror the multiprocess RunNetSoak real-TCP ceilings so the two
// over-the-wire soak paths stay consistent). Over real TCP the kernel send buffer can hold
// frames transiently, so these are looser than the in-process LoopbackTransport ctest's. --
constexpr std::uint32_t kSoakClients = 32;
constexpr std::uint64_t kSoakTicks = 300;        // 10 s of sim at 30 Hz
constexpr int kTickPeriodMs = 33;                // ~30 Hz pacing
constexpr std::int64_t kChunkSizeMm = 32 * 1000; // 32 m streaming chunk (AOI)

constexpr std::uint32_t kQueueDepthP95Budget = 16;  // endpoint queue drains to ~0 over loopback
constexpr std::uint32_t kSnapshotAgeP95Budget = 30; // ~1 s of un-acked snapshots
constexpr std::size_t kMaxClientBytesBudget = 64u * 1024u;
constexpr std::size_t kPerTickBytesP95Budget = 256u * 1024u; // total wire bytes / tick (32 clients)
constexpr double kTickMsP95Budget = 33.0;                    // per-tick work under one 30 Hz frame
constexpr double kTickRateToleranceFactor = 1.40;            // allow 40% wall-clock overrun

// Deterministic authoritative avatar set for tick `t`: N avatars clustered inside ONE chunk
// (so the chunk-AOI gather includes the full set -- worst case for per-client bytes),
// MARCHING in +X by the tick number. This is a pure function of (n, t): every connected
// client that applies the tick-t snapshot mirrors the identical set, so convergence at the
// final tick is by construction. Usercmd content does NOT steer this set (the soak measures
// the broadcast itself, not sim-authored motion).
std::vector<ReplEntityState> AvatarStates(std::uint32_t n, std::uint64_t t) {
    std::vector<ReplEntityState> states;
    states.reserve(n);
    for (std::uint32_t i = 1; i <= n; ++i) {
        ReplEntityState e;
        e.entity_id = i;
        e.px_mm = static_cast<std::int32_t>((i * 250) + static_cast<std::int32_t>(t)); // marches +X
        e.py_mm = 1000;
        e.pz_mm = static_cast<std::int32_t>(i * 250);
        e.yaw_mrad = static_cast<std::int16_t>(i * 7);
        e.flags = 1; // grounded
        states.push_back(e);
    }
    return states;
}

// Order-independent FNV-1a hash of an entity SET (sorts a copy by entity_id). Two clients
// that reconstructed the same authoritative set hash equal regardless of internal ordering,
// so "converge to the same authoritative hash" is exactly set-equality. Test-only; this
// hashes transport-side replicated state, never a sim-determinism input.
std::uint64_t HashEntityStates(std::vector<ReplEntityState> es) {
    std::sort(es.begin(), es.end(), [](const ReplEntityState& a, const ReplEntityState& b) {
        return a.entity_id < b.entity_id;
    });
    std::uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
    auto mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    mix(es.size());
    for (const ReplEntityState& e : es) {
        mix(e.entity_id);
        mix(static_cast<std::uint32_t>(e.px_mm));
        mix(static_cast<std::uint32_t>(e.py_mm));
        mix(static_cast<std::uint32_t>(e.pz_mm));
        mix(static_cast<std::uint16_t>(e.yaw_mrad));
        mix(e.flags);
        mix(e.type_id);
        mix(e.anim_state);
        mix(e.anim_phase);
    }
    return h;
}

template<typename T>
T Percentile95(std::vector<T> v) {
    if (v.empty())
        return T{};
    std::sort(v.begin(), v.end());
    const std::size_t idx = (v.size() - 1) * 95 / 100; // nearest-rank
    return v[idx];
}

fs::path NetSoakArtifactDir() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "net";
}

// Per-client cross-thread result. atomics carry the live signal (applied seq + connected)
// the main thread polls DURING the run; the plain fields are written by the client thread
// just before it exits and read by main ONLY after join (the happens-before edge).
struct ClientResult {
    std::atomic<std::uint32_t> applied_seq{0};
    std::atomic<bool> connected{false};
    std::uint64_t final_hash = 0; // hash of the newest mirrored set at exit
    std::uint32_t last_seq = 0;   // newest snapshot_seq mirrored at exit
    bool has_snapshot = false;
};

// One driving-avatar client thread: connect (retry to a deadline), then stream usercmds +
// mirror/ack snapshots until `done`, then a bounded final drain to catch the last (seq K)
// snapshot before computing its convergence hash. Touches ONLY its own transport/client.
void RunClient(std::uint16_t port,
               std::uint32_t nominal_id,
               ClientResult* out,
               std::atomic<bool>* done) {
    TcpTransport transport;
    const auto connect_deadline = clock_type::now() + std::chrono::seconds(15);
    bool connected = false;
    while (!done->load(std::memory_order_relaxed) && clock_type::now() < connect_deadline) {
        if (transport.Connect("127.0.0.1", port, /*timeout_ms=*/2000)) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // brief backoff, retry
    }
    if (!connected)
        return; // out->connected stays false -> counted as a failed connection
    out->connected.store(true, std::memory_order_relaxed);

    ReplicationClient client(nominal_id, &transport);
    std::uint64_t cmd_tick = 0;
    while (!done->load(std::memory_order_relaxed)) {
        UsercmdMsg cmd;
        cmd.tick = ++cmd_tick;
        cmd.player_id = nominal_id;
        cmd.move_x = 32767; // walk +X (upstream traffic; does not author the authoritative set)
        client.SendUsercmd(cmd);
        client.PumpInbound(); // apply newest snapshot + ack it
        if (client.has_snapshot()) {
            out->applied_seq.store(client.snapshot().snapshot_seq, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8)); // responsive, low-CPU
    }
    // Final drain: `done` is only raised once every client already reported seq >= K, so the
    // seq-K snapshot is present; this just flushes any last frame before we snapshot the hash.
    for (int i = 0; i < 25; ++i) {
        client.PumpInbound();
        if (client.has_snapshot()) {
            out->applied_seq.store(client.snapshot().snapshot_seq, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (client.has_snapshot()) {
        out->has_snapshot = true;
        out->last_seq = client.snapshot().snapshot_seq;
        out->final_hash = HashEntityStates(client.snapshot().entities);
    }
    transport.Close();
}

} // namespace

// The one soak. Real TCP over loopback, 32 clients, 300 ticks at 30 Hz, frozen net_soak.v1.
TEST(NetSoak, OverTheWireThirtyTwoClientsWithinBudget) {
    fs::path art_dir = NetSoakArtifactDir();
    std::error_code ec;
    fs::create_directories(art_dir, ec);
    const fs::path artifact_path = art_dir / "net_soak.v1.json";

    // ONE listen socket on ONE OS-assigned ephemeral port (no fixed port -> no conflict/flake).
    TcpListener listener;
    ASSERT_TRUE(listener.Listen(/*port=*/0, /*backlog=*/static_cast<int>(kSoakClients) + 8))
        << "supported platforms must provide TCP loopback for the replication soak";
    const std::uint16_t port = listener.port();

    ReplicationServer server;
    server.SetAoiChunkRadius(/*chunk_radius=*/3, kChunkSizeMm); // the real dedicated-server scope

    // Launch the 32 driving-avatar client threads.
    std::atomic<bool> done{false};
    std::vector<std::unique_ptr<ClientResult>> results;
    results.reserve(kSoakClients);
    for (std::uint32_t i = 0; i < kSoakClients; ++i)
        results.push_back(std::make_unique<ClientResult>());
    std::vector<std::thread> threads;
    threads.reserve(kSoakClients);
    for (std::uint32_t i = 0; i < kSoakClients; ++i) {
        threads.emplace_back(RunClient, port, i + 1, results[i].get(), &done);
    }

    // Accept all 32 (bounded by a hard deadline; a soak that hangs is worse than one that
    // fails). Each fans out into its own transport, registered with a distinct client id.
    std::vector<std::unique_ptr<TcpTransport>> server_transports;
    server_transports.reserve(kSoakClients);
    std::uint32_t accepted = 0;
    const auto accept_deadline = clock_type::now() + std::chrono::seconds(20);
    while (accepted < kSoakClients && clock_type::now() < accept_deadline) {
        std::unique_ptr<TcpTransport> conn = listener.AcceptOneBlocking(/*timeout_ms=*/500);
        if (conn) {
            server.AddClient(accepted + 1, conn.get());
            server_transports.push_back(std::move(conn));
            ++accepted;
        }
    }

    // Measured over the whole run.
    std::vector<std::size_t> per_tick_total_bytes;
    std::vector<double> tick_work_ms;
    std::size_t max_client_bytes = 0;
    std::uint32_t worst_queue_p95 = 0;
    std::uint32_t worst_age_p95 = 0;
    std::uint64_t ticks_executed = 0;
    double elapsed_ms = 0.0;
    const double expected_ms = static_cast<double>(kSoakTicks) * static_cast<double>(kTickPeriodMs);
    bool all_settled = false;

    if (accepted == kSoakClients) {
        per_tick_total_bytes.reserve(kSoakTicks);
        tick_work_ms.reserve(kSoakTicks);

        const auto wall_start = clock_type::now();
        auto next_tick = wall_start;
        for (std::uint64_t t = 1; t <= kSoakTicks; ++t) {
            const auto work_start = clock_type::now();
            server.PumpInbound(); // drain usercmds + acks first (keeps snapshot-age bounded)
            server.BroadcastSnapshot(t, AvatarStates(kSoakClients, t));
            const auto work_end = clock_type::now();
            ++ticks_executed;

            per_tick_total_bytes.push_back(server.last_broadcast_total_bytes());
            max_client_bytes = std::max(max_client_bytes, server.last_broadcast_max_client_bytes());
            worst_queue_p95 = std::max(worst_queue_p95, server.QueueDepthP95());
            worst_age_p95 = std::max(worst_age_p95, server.SnapshotAgeP95());
            tick_work_ms.push_back(
                std::chrono::duration<double, std::milli>(work_end - work_start).count());

            next_tick += std::chrono::milliseconds(kTickPeriodMs);
            std::this_thread::sleep_until(next_tick); // 30 Hz pace
        }
        elapsed_ms =
            std::chrono::duration<double, std::milli>(clock_type::now() - wall_start).count();

        // Settle: keep draining acks while waiting (bounded) for every client to apply the
        // final seq-K snapshot the loop already sent. No re-broadcast (that would move seq/tick
        // past K and change the expected hash) -- just let the wire finish delivering seq K.
        const auto settle_deadline = clock_type::now() + std::chrono::seconds(8);
        while (clock_type::now() < settle_deadline) {
            server.PumpInbound();
            all_settled = true;
            for (const auto& r : results) {
                if (r->applied_seq.load(std::memory_order_relaxed) < kSoakTicks) {
                    all_settled = false;
                    break;
                }
            }
            if (all_settled)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Stop + join every client thread BEFORE reading their computed hashes (join is the
    // happens-before edge that makes the plain result fields safe to read here).
    done.store(true, std::memory_order_relaxed);
    for (std::thread& th : threads)
        th.join();

    // Convergence: every client's newest mirrored set must equal AvatarStates(32, K).
    const std::uint64_t expected_hash = HashEntityStates(AvatarStates(kSoakClients, kSoakTicks));
    std::uint32_t connected_clients = 0;
    std::uint32_t converged_clients = 0;
    for (const auto& r : results) {
        if (r->connected.load(std::memory_order_relaxed))
            ++connected_clients;
        if (r->has_snapshot && r->last_seq == static_cast<std::uint32_t>(kSoakTicks) &&
            r->final_hash == expected_hash) {
            ++converged_clients;
        }
    }

    const std::size_t per_tick_bytes_p95 = Percentile95(per_tick_total_bytes);
    const std::size_t per_tick_bytes_max =
        per_tick_total_bytes.empty()
            ? 0u
            : *std::max_element(per_tick_total_bytes.begin(), per_tick_total_bytes.end());
    const double tick_ms_p95 = Percentile95(tick_work_ms);
    const double tick_ms_max =
        tick_work_ms.empty() ? 0.0 : *std::max_element(tick_work_ms.begin(), tick_work_ms.end());

    // Per-budget verdicts (each surfaced individually so a breach is diagnosable from the
    // artifact).
    const bool clients_ok = accepted == kSoakClients;
    const bool ticks_ok = ticks_executed == kSoakTicks;
    const bool converged = converged_clients == kSoakClients;
    const bool per_tick_bytes_ok = per_tick_bytes_p95 <= kPerTickBytesP95Budget;
    const bool client_bytes_ok = max_client_bytes <= kMaxClientBytesBudget;
    const bool tick_ms_ok = tick_ms_p95 <= kTickMsP95Budget;
    const bool queue_ok = worst_queue_p95 <= kQueueDepthP95Budget;
    const bool age_ok = worst_age_p95 <= kSnapshotAgeP95Budget;
    const bool rate_ok = ticks_ok && elapsed_ms <= expected_ms * kTickRateToleranceFactor;
    const bool passed = clients_ok && ticks_ok && converged && per_tick_bytes_ok &&
                        client_bytes_ok && tick_ms_ok && queue_ok && age_ok && rate_ok;

    // FREEZE the measured baseline. Written UNCONDITIONALLY and BEFORE any assertion, so a
    // budget breach still leaves the artifact on disk with passed=false + the per-budget *_ok
    // flags -- exactly what the manual NetSoak gate reads. This file IS the frozen baseline.
    nlohmann::json artifact{
        {"schema", "luminumbra.net_soak.v1"},
        {"generated_by", "net_soak_over_the_wire_test"},
        {"transport", "tcp-loopback"},
        {"delta_compression", false},
        {"listen_port", port},
        // headcount (both the task's `clients` and the gate's `expected_clients`).
        {"clients", kSoakClients},
        {"expected_clients", kSoakClients},
        {"connected_clients", connected_clients},
        {"converged_clients", converged_clients},
        // run.
        {"ticks", ticks_executed},
        {"ticks_requested", kSoakTicks},
        {"elapsed_ms", elapsed_ms},
        {"expected_ms", expected_ms},
        // measured bandwidth.
        {"per_tick_bytes_p95", per_tick_bytes_p95},
        {"per_tick_bytes_max", per_tick_bytes_max},
        {"max_client_bytes", max_client_bytes},
        // measured tick-rate (processing time only -- excludes the 30 Hz pace-sleep).
        {"tick_ms_p95", tick_ms_p95},
        {"tick_ms_max", tick_ms_max},
        // measured backpressure / aging.
        {"queue_depth_p95", worst_queue_p95},
        {"snapshot_age_p95", worst_age_p95},
        {"converged_hash", expected_hash},
        {"settled_within_window",
         all_settled}, // diagnostic: did every client reach seq K before settle timed out
        // explicit, bounded budgets the run is asserted against.
        {"budget_per_tick_bytes_p95", kPerTickBytesP95Budget},
        {"budget_max_client_bytes", kMaxClientBytesBudget},
        {"budget_tick_ms_p95", kTickMsP95Budget},
        {"budget_queue_depth_p95", kQueueDepthP95Budget},
        {"budget_snapshot_age_p95", kSnapshotAgeP95Budget},
        {"budget_rate_tolerance", kTickRateToleranceFactor},
        // per-budget verdicts.
        {"clients_ok", clients_ok},
        {"ticks_ok", ticks_ok},
        {"converged", converged},
        {"per_tick_bytes_ok", per_tick_bytes_ok},
        {"client_bytes_ok", client_bytes_ok},
        {"tick_ms_ok", tick_ms_ok},
        {"queue_ok", queue_ok},
        {"age_ok", age_ok},
        {"rate_ok", rate_ok},
        {"passed", passed},
    };
    {
        std::ofstream out(artifact_path);
        ASSERT_TRUE(out.is_open()) << "could not write net_soak.v1 artifact to " << artifact_path;
        out << artifact.dump(2) << "\n";
    }

    // Assertions (EXPECT only -- the artifact is already durable above).
    EXPECT_EQ(accepted, kSoakClients) << "not all clients connected over the wire";
    EXPECT_EQ(ticks_executed, kSoakTicks);
    EXPECT_EQ(converged_clients, kSoakClients)
        << "only " << converged_clients << "/" << kSoakClients
        << " clients converged to the authoritative state hash";
    EXPECT_LE(per_tick_bytes_p95, kPerTickBytesP95Budget) << "per-tick wire bytes p95 over budget";
    EXPECT_LE(max_client_bytes, kMaxClientBytesBudget)
        << "worst per-client snapshot bytes over budget";
    EXPECT_LE(tick_ms_p95, kTickMsP95Budget) << "per-tick processing p95 over one 30 Hz frame";
    EXPECT_LE(worst_queue_p95, kQueueDepthP95Budget) << "outbound queue-depth p95 over budget";
    EXPECT_LE(worst_age_p95, kSnapshotAgeP95Budget) << "snapshot-age p95 over budget";
    EXPECT_TRUE(rate_ok) << "sustained tick-rate breached: ran " << elapsed_ms << " ms vs budget "
                         << expected_ms * kTickRateToleranceFactor << " ms";
    EXPECT_TRUE(passed) << "net_soak.v1 FAILED; see " << artifact_path;

    RecordProperty("clients", kSoakClients);
    RecordProperty("ticks", static_cast<int>(ticks_executed));
    RecordProperty("per_tick_bytes_p95", static_cast<int>(per_tick_bytes_p95));
    RecordProperty("max_client_bytes", static_cast<int>(max_client_bytes));
    RecordProperty("converged_clients", converged_clients);
}
