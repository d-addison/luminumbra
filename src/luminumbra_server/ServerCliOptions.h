#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct ServerCliOptions {
    std::string root;
    std::string preset = "default";
    std::string seed = "424242";
    std::string world_id;
    std::uint64_t ticks = 90;
    int surface_radius = 4;
    int collision_radius = 2;
    std::uint64_t autosave_ticks = 0;
    //  spawn N deterministic player avatars (phyllotaxis ring
    // around spawn). 0 = none (byte-identical to the zero-avatar lane). With --smoke the
    // double-run already asserts the entities sub-hash matches, so --smoke --avatars N
    // validates avatar determinism through the existing gate path.
    int avatars = 0;
    // gate-populated-world-replay: --ecology-roster spawns the fixed deterministic
    // KINEMATIC creature roster (2 predators + 6 prey, gtest Populate fixture) into
    // the headless world so the hardened ecology stack runs LIVE. With --smoke the
    // double-run asserts run==replay across ALL sub-hashes incl. the new ecology
    // term (the PopulatedWorldReplay gate). DEFAULT off (empty roster -> neutral
    // ecology sub-hash -> additive `|ecology:` suffix only).
    bool ecology_roster = false;
    //  --planted-roster spawns a deterministic 6-plant roster so the smoke
    // exercises the plant sub-hash + growth + persistence end-to-end (default off ->
    // empty/neutral).
    bool planted_roster = false;
    // B' determinism harness: --smoke-moving drifts the streaming anchor deterministically each
    // tick so chunks stream IN/OUT during the run (the static smoke never does). It reproduces the
    // moving-case water determinism the boot warm-up (boot warm-up) does NOT cover. Implies
    // --smoke.
    bool moving = false;
    //  gate (runtime audit): --avail-trace captures the per-tick availability-set
    // digest in BOTH determinism runs and asserts they match per tick — the baseline a future
    // activation-queue must reproduce when it replaces the wait_for_streaming_jobs barrier.
    // Observability only (the digest mutates nothing); implies --smoke.
    bool availability_trace = false;
    bool water_hash_trace = false; //
    // water-smoke measurement workload: wet-anchor walk + water sub-phase timing
    // percentiles and sim-load counters in the smoke artifact (see
    // ServerWorldRunnerConfig::water_smoke). Observability only; implies --smoke.
    bool water_smoke = false;

    //  --replicate runs the authoritative server + an in-process loopback
    // ReplicationClient, broadcasts the avatar states each tick, and asserts the client
    // mirrors the server avatars (end-to-end live replication in the harness).
    bool replicate = false;
    //  spawn N server-side replicated NPC entities in --replicate (tagged
    // ReplicatedComponent, deterministic wander) to prove heterogeneous entities
    // (animals/NPCs) replicate alongside player avatars.
    int npcs = 0;
    //  fire one server-authoritative ballistic ARROW (type_id 2) in
    // --replicate -> replicates typed while in flight, reliable despawn on hit/expire.
    bool arrow = false;
    bool smoke = false;
    // REAL networked multiplayer over TCP sockets (two processes). --net-host
    // listens; --net-join connects. Same replication stack as --replicate, off-loopback.
    bool net_host = false;
    bool net_join = false;
    std::string host = "127.0.0.1";
    std::uint16_t port = 27015;
    // Multi-client accept: host accepts client ids 1..clients. Each joiner passes
    // --player-id K and connects to base_port + K - 1 for both TCP and GNS UDP.
    int clients = 1;
    std::uint32_t player_id = 1;
    // Dedicated server runtime mode: --net-host --server-mode starts ticking
    // immediately, accepts late TCP clients, and keeps running after clients leave.
    bool server_mode = false;
    //   (-C1): MULTIPROCESS soak harness over real TCP.
    //   --net-soak          authoritative server: accepts up to --clients TCP peers
    //                       (per-client port), ticks at 30 Hz for --ticks, RE-ARMS the
    //                       accept on a clean leave (disconnect/reconnect under load),
    //                       and ASSERTS the sustained tick-rate + per-connection
    //                       queue-depth/snapshot-age p95 + per-client bandwidth stay
    //                       within budget over the run (non-zero exit on a breach).
    //   --net-soak-client   a driving avatar client: connects to its --player-id port,
    //                       streams usercmds, then disconnects + RECONNECTS --soak-cycles
    //                       times to exercise the server's reconnect-under-load path.
    bool net_soak = false;
    bool net_soak_client = false;
    int soak_cycles = 2; // reconnect cycles the soak client performs
    // use the Steamworks ISteamNetworkingSockets transport (real UDP via the
    // Steam SDK) for --net-host/--net-join instead of raw TCP. Requires the build to
    // be configured with -DLUMINUMBRA_ENABLE_STEAM=ON and the Steam client running.
    bool steam = false;
    // use the standalone GameNetworkingSockets transport (real UDP, no Steam --
    // two processes can connect on ONE machine). Requires -DLUMINUMBRA_ENABLE_GNS=ON.
    bool udp = false;
    // WindFieldDeterminism gate. Boots a world, runs N ticks
    // twice, asserts the wind sub-hash is equal across runs + stable, and times
    // the per-tick wind update (budget <= 0.15 ms at the streamed extent).
    bool wind_bench = false;
    // WeatherVisual determinism. Boots the weather core (advected by
    // the wind field), runs N ticks twice, asserts the weather sub-hash is equal
    // across runs + stable + evolves + bounded storm cells, and times the per-tick
    // weather update (budget <= 0.20 ms at the streamed extent).
    bool weather_bench = false;
    // AetherFieldDeterminism. Boots the aether field (advected by the
    // wind field), runs N ticks twice, asserts the aether sub-hash is equal
    // across runs + stable + evolves, and times the per-tick update.
    bool aether_bench = false;
    //  heavy-mode oracle: tick N, save, load into a fresh session,
    // resimulate heavy_resim ticks on BOTH, compare full + sub hashes.
    bool heavy = false;
    std::uint64_t heavy_resim = 30;
    //  session replay (LREC1):
    //   --record <path>  record every tick (inputs + 30-tick hash checkpoints).
    //   --replay <path>  boot from the stream header, feed recorded inputs, and
    //                    verify live hashes against the recorded checkpoints.
    //   --mutate-replay-fixture <path>  read an LREC1 stream and rewrite it with
    //                    ONE checkpoint hash corrupted (gate fixture for the
    //                    ReplayDivergence oracle; least-hacky in-process mutation).
    std::string record_path;
    std::string replay_path;
    std::string mutate_replay_fixture;
    std::string artifact_path;
    //  lockstep transport. --lockstep-loopback drives BOTH peers in-process over
    // LoopbackTransport (the gate path: no sockets/ports), each peer backed by its own
    // ServerWorldRunner stepping the same world; the host is the sim authority and both
    // exchange hashes at the 30-tick cadence. Fault-injection knobs (for
    // LockstepFaultInjection) prove the horizon absorbs jitter and the oracle isn't vacuous:
    //   --lockstep-delay-input N  peer 1 withholds its inputs for the first N agreed ticks
    //                             (a DELAYED + DROPPED-then-released input within horizon
    //                             tolerance); the adaptive horizon must absorb it, no desync.
    //   --lockstep-corrupt-tick T peer 1's captured hashes corrupt from tick T (a deliberate
    //                             STATE divergence like the ReplayDivergence fixture); the
    //                             oracle must HALT and dump the LREC1 at exactly T.
    bool lockstep_loopback = false;
    std::uint64_t lockstep_delay_input = 0;
    std::uint64_t lockstep_corrupt_tick = 0;
    std::string lockstep_dump_path;
    bool parse_error = false;
};

bool HasWorldPresets(const std::filesystem::path& candidate);
std::filesystem::path ResolveServerRoot(const char* argv0);
std::string RootString(const std::filesystem::path& root);
ServerCliOptions ParseOptions(int argc, char* argv[]);
