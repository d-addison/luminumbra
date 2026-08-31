// Headless server entry point. Simulation authority
// only: links luminumbra_common and nothing client-side (no OpenGL/GLFW/
// miniaudio/imgui/RmlUi). See the ServerHeadlessHygiene ctest for the
// include boundary.
//
// Modes:
//   default        boot a world (preset or existing save id), run --ticks
//                  fixed 30 Hz simulation ticks, save on shutdown.
//   --smoke        determinism double-run (HeadlessServerTick gate): boots a
//                  fresh world twice in this one process with the same
//                  seed/preset, runs N ticks each, and emits the
//                  luminumbra.server_tick.v1 artifact asserting
//                  world_hash == world_hash_replay.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

#include "ServerWorldRunner.h"
#include "core/EngineVersion.h"
#include "luminumbra_common/ai/InstinctLocomotionSystem.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/net/GnsTransport.h" // body #ifdef LUMINUMBRA_ENABLE_GNS
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/net/ReplicationEndpoint.h"
#include "luminumbra_common/net/ReplicationProtocol.h"
#include "luminumbra_common/net/SteamNetworkingTransport.h" // body #ifdef LUMINUMBRA_ENABLE_STEAM
#include "luminumbra_common/network/NetworkLoopbackAuthority.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/PlayerAvatar.h"

#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include <cmath>
#include <entt/entt.hpp>

namespace fs = std::filesystem;

namespace {

constexpr const char* kServerTickArtifactSchema = "luminumbra.server_tick.v1";

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

bool HasWorldPresets(const fs::path& candidate) {
    std::error_code ec;
    return fs::is_directory(candidate / "worlds" / "atlas" / "presets", ec);
}

// Mirrors the client's runtime-root discovery: walk the ancestors of the
// working directory and the executable directory until a directory carrying
// the world presets is found.
fs::path ResolveServerRoot(const char* argv0) {
    std::error_code ec;
    const fs::path starts[] = {
        fs::current_path(ec),
        (argv0 && argv0[0] != '\0') ? fs::absolute(fs::path(argv0).parent_path(), ec) : fs::path(),
    };
    for (const fs::path& start : starts) {
        fs::path probe = start;
        while (!probe.empty()) {
            if (HasWorldPresets(probe)) {
                const fs::path canonical = fs::weakly_canonical(probe, ec);
                return ec ? probe : canonical;
            }
            const fs::path parent = probe.parent_path();
            if (parent == probe) {
                break;
            }
            probe = parent;
        }
    }
    return fs::current_path(ec);
}

// GameSession concatenates root + relative paths, so the root string carries
// a trailing separator.
std::string RootString(const fs::path& root) {
    std::string value = root.generic_string();
    if (!value.empty() && value.back() != '/') {
        value.push_back('/');
    }
    return value;
}

ServerCliOptions ParseOptions(int argc, char* argv[]) {
    ServerCliOptions options;
    auto next_value = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            LUMINUMBRA_CORE_ERROR("Missing value for argument '{}'", argv[i]);
            options.parse_error = true;
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--wind-bench") == 0) {
            options.wind_bench = true;
        } else if (std::strcmp(arg, "--weather-bench") == 0) {
            options.weather_bench = true;
        } else if (std::strcmp(arg, "--aether-bench") == 0) {
            options.aether_bench = true;
        } else if (std::strcmp(arg, "--smoke") == 0) {
            options.smoke = true;
        } else if (std::strcmp(arg, "--heavy") == 0) {
            options.heavy = true;
        } else if (std::strcmp(arg, "--heavy-resim") == 0) {
            if (const char* v = next_value(i))
                options.heavy_resim = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(arg, "--record") == 0) {
            if (const char* v = next_value(i))
                options.record_path = v;
        } else if (std::strcmp(arg, "--replay") == 0) {
            if (const char* v = next_value(i))
                options.replay_path = v;
        } else if (std::strcmp(arg, "--mutate-replay-fixture") == 0) {
            if (const char* v = next_value(i))
                options.mutate_replay_fixture = v;
        } else if (std::strcmp(arg, "--lockstep-loopback") == 0) {
            options.lockstep_loopback = true;
        } else if (std::strcmp(arg, "--lockstep-delay-input") == 0) {
            if (const char* v = next_value(i))
                options.lockstep_delay_input = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(arg, "--lockstep-corrupt-tick") == 0) {
            if (const char* v = next_value(i))
                options.lockstep_corrupt_tick = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(arg, "--lockstep-dump") == 0) {
            if (const char* v = next_value(i))
                options.lockstep_dump_path = v;
        } else if (std::strcmp(arg, "--root") == 0) {
            if (const char* v = next_value(i))
                options.root = v;
        } else if (std::strcmp(arg, "--preset") == 0) {
            if (const char* v = next_value(i))
                options.preset = v;
        } else if (std::strcmp(arg, "--seed") == 0) {
            if (const char* v = next_value(i))
                options.seed = v;
        } else if (std::strcmp(arg, "--world-id") == 0) {
            if (const char* v = next_value(i))
                options.world_id = v;
        } else if (std::strcmp(arg, "--ticks") == 0) {
            if (const char* v = next_value(i))
                options.ticks = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(arg, "--radius") == 0) {
            if (const char* v = next_value(i))
                options.surface_radius = std::atoi(v);
        } else if (std::strcmp(arg, "--collision-radius") == 0) {
            if (const char* v = next_value(i))
                options.collision_radius = std::atoi(v);
        } else if (std::strcmp(arg, "--autosave-ticks") == 0) {
            if (const char* v = next_value(i))
                options.autosave_ticks = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(arg, "--avatars") == 0) {
            if (const char* v = next_value(i))
                options.avatars = std::atoi(v);
        } else if (std::strcmp(arg, "--ecology-roster") == 0) {
            options.ecology_roster = true;
        } else if (std::strcmp(arg, "--planted-roster") == 0) {
            options.planted_roster = true;
        } else if (std::strcmp(arg, "--smoke-moving") == 0) {
            options.smoke = true;
            options.moving = true;
        } else if (std::strcmp(arg, "--avail-trace") == 0) {
            options.smoke = true;
            options.availability_trace = true;
        } else if (std::strcmp(arg, "--water-hash-trace") == 0) {
            // per-tick water-state hash sequence into the smoke artifact
            // (the WaterCrossBuild gate compares debug vs release sequences).
            options.smoke = true;
            options.water_hash_trace = true;
        } else if (std::strcmp(arg, "--replicate") == 0) {
            options.replicate = true;
        } else if (std::strcmp(arg, "--npcs") == 0) {
            if (const char* v = next_value(i))
                options.npcs = std::atoi(v);
        } else if (std::strcmp(arg, "--arrow") == 0) {
            options.arrow = true;
        } else if (std::strcmp(arg, "--net-host") == 0) {
            options.net_host = true;
        } else if (std::strcmp(arg, "--net-join") == 0) {
            options.net_join = true;
        } else if (std::strcmp(arg, "--steam") == 0) {
            options.steam = true;
        } else if (std::strcmp(arg, "--udp") == 0) {
            options.udp = true;
        } else if (std::strcmp(arg, "--host") == 0) {
            if (const char* v = next_value(i))
                options.host = v;
        } else if (std::strcmp(arg, "--port") == 0) {
            if (const char* v = next_value(i))
                options.port = static_cast<std::uint16_t>(std::atoi(v));
        } else if (std::strcmp(arg, "--clients") == 0) {
            if (const char* v = next_value(i))
                options.clients = std::max(1, std::atoi(v));
        } else if (std::strcmp(arg, "--player-id") == 0) {
            if (const char* v = next_value(i)) {
                options.player_id = static_cast<std::uint32_t>(std::max(1, std::atoi(v)));
            }
        } else if (std::strcmp(arg, "--server-mode") == 0) {
            options.server_mode = true;
        } else if (std::strcmp(arg, "--net-soak") == 0) {
            options.net_soak = true;
        } else if (std::strcmp(arg, "--net-soak-client") == 0) {
            options.net_soak_client = true;
        } else if (std::strcmp(arg, "--soak-cycles") == 0) {
            if (const char* v = next_value(i))
                options.soak_cycles = std::max(1, std::atoi(v));
        } else if (std::strcmp(arg, "--artifact") == 0) {
            if (const char* v = next_value(i))
                options.artifact_path = v;
        } else {
            LUMINUMBRA_CORE_ERROR("Unknown argument '{}'", arg);
            options.parse_error = true;
        }
    }
    return options;
}

Luminumbra::Server::ServerWorldRunnerConfig RunnerConfigFrom(const ServerCliOptions& options) {
    Luminumbra::Server::ServerWorldRunnerConfig config;
    config.root_path = options.root;
    config.seed = options.seed;
    config.preset = options.preset;
    config.world_id = options.world_id;
    config.surface_radius = options.surface_radius;
    config.collision_radius = options.collision_radius;
    config.autosave_interval_ticks = options.autosave_ticks;
    config.avatar_count = options.avatars;
    config.ecology_roster = options.ecology_roster;
    config.planted_roster = options.planted_roster;
    config.moving_anchor = options.moving;
    config.availability_trace = options.availability_trace;
    config.water_hash_trace = options.water_hash_trace;
    return config;
}

std::uint32_t ExpectedNetworkClients(const ServerCliOptions& options) {
    return options.clients > 0 ? static_cast<std::uint32_t>(options.clients) : 1u;
}

std::uint32_t LocalNetworkPlayerId(const ServerCliOptions& options) {
    return options.player_id == 0u ? 1u : options.player_id;
}

bool ResolveNetworkClientPort(const std::uint16_t base_port,
                              const std::uint32_t client_id,
                              std::uint16_t& out_port) {
    return luminumbra::network::TryNetworkMultiClientAcceptPortForClient(
        base_port, client_id, out_port);
}

struct SmokeRunResult {
    bool ok = false;
    std::string world_hash;
    // per-system sub-hashes (additive; top-level world_hash unchanged).
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub_hashes;
    std::string scent_hash;
    // gate-populated-world-replay: id-ordered ecology sub-hash (empty when no
    // roster) + creature counts before/after the run (non-vacuity oracle).
    std::string ecology_hash;
    //  id-ordered plant sub-hash (empty when no PlantTag roster). Folded into the
    // composite world_hash (bump #7) and surfaced here so the gate verifies plant run==replay too.
    std::string plant_hash;
    std::size_t creature_count_start = 0;
    std::size_t creature_count_end = 0;
    //  gate: per-tick availability-set trace (empty unless --avail-trace).
    std::vector<std::pair<std::uint64_t, std::string>> avail_trace;
    // per-tick water-state hash trace (empty unless --water-hash-trace).
    std::vector<std::pair<std::uint64_t, std::uint64_t>> water_hash_trace;
    std::string world_id;
    Luminumbra::Server::ServerTickReport ticks;
    std::size_t chunks_streamed = 0;
    Luminumbra::world::WorldStateSaveReport shutdown_save;
};

// One boot + N-tick + hash + shutdown pass over a FRESH world. Smoke worlds
// are throwaway: the save directory is removed afterwards so repeated gate
// runs do not accumulate world_<timestamp> directories.
SmokeRunResult RunSmokeOnce(const ServerCliOptions& options, const char* run_label) {
    SmokeRunResult result;

    Luminumbra::Server::ServerWorldRunnerConfig config = RunnerConfigFrom(options);
    config.world_id.clear(); // determinism runs always boot fresh worlds
    config.world_name = std::string("Headless Smoke ") + run_label;
    if (config.autosave_interval_ticks == 0) {
        // Exercise the autosave path on the gate by default (incremental
        // contract: a never-edited world records passes, writes nothing).
        config.autosave_interval_ticks = 30;
    }

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        return result;
    }

    // gate-populated-world-replay: capture the creature count BEFORE the run so
    // the gate can assert non-vacuity (start != end => the ecology actually
    // birthed/culled creatures over the horizon, not a frozen roster).
    result.creature_count_start = runner.CreatureCount();

    result.ticks = runner.RunFixedTicks(options.ticks);
    //  avatar physics telemetry — confirm the server-authoritative avatar
    // characters SETTLED on the terrain (grounded; not fallen through the world).
    if (!runner.Avatars().empty()) {
        auto* phys = runner.Session() ? runner.Session()->GetPhysicsSystem() : nullptr;
        int grounded = 0;
        for (std::size_t i = 0; i < runner.Avatars().size(); ++i) {
            if (phys && phys->is_avatar_grounded(i))
                ++grounded;
        }
        const auto& a0 = runner.Avatars().front();
        LUMINUMBRA_CORE_INFO("Smoke {}: avatars={} grounded={} (avatar0 y={:.2f})",
                             run_label,
                             runner.Avatars().size(),
                             grounded,
                             a0.position.y);
    }
    result.world_hash = runner.ComputeWorldHash();
    result.sub_hashes = runner.ComputeWorldSubHashes();
    result.scent_hash = runner.Session() ? runner.Session()->ComputeScentSubHash() : std::string();
    result.ecology_hash = runner.ComputeEcologySubHash();
    result.plant_hash = runner.Session() ? runner.Session()->ComputePlantSubHash() : std::string();
    result.creature_count_end = runner.CreatureCount();
    result.avail_trace = runner.AvailabilityTrace();   // empty unless --avail-trace
    result.water_hash_trace = runner.WaterHashTrace(); // empty unless --water-hash-trace
    result.chunks_streamed = runner.StreamedChunkCount();
    result.world_id = runner.Session()->GetMetadata().worldId;
    const fs::path save_dir = runner.Session()->GetWorldSaveDir();
    runner.Shutdown(&result.shutdown_save);

    if (!save_dir.empty()) {
        std::error_code ec;
        fs::remove_all(save_dir, ec);
    }

    result.ok = !result.world_hash.empty() && result.ticks.ticks_executed == options.ticks;
    LUMINUMBRA_CORE_INFO("Smoke {}: world_hash={} ticks={} chunks={} wall={:.2f}s",
                         run_label,
                         result.world_hash,
                         result.ticks.ticks_executed,
                         result.chunks_streamed,
                         result.ticks.wall_seconds);
    return result;
}

nlohmann::json SmokeRunJson(const SmokeRunResult& run) {
    return nlohmann::json{
        {"ok", run.ok},
        {"world_hash", run.world_hash},
        {"sub_hashes",
         {
             {"terrain", run.sub_hashes.terrain},
             {"mesh", run.sub_hashes.mesh},
             {"water", run.sub_hashes.water},
             {"entities", run.sub_hashes.entities},
             {"wind", run.sub_hashes.wind},
             {"weather", run.sub_hashes.weather},
             {"aether", run.sub_hashes.aether},
             {"aether_state", run.sub_hashes.aether_state},
             {"scents", run.scent_hash},
             {"ecology", run.ecology_hash},
             {"plants", run.plant_hash},
         }},
        {"entity_count_start", run.creature_count_start},
        {"entity_count_end", run.creature_count_end},
        {"world_id", run.world_id},
        {"ticks_executed", run.ticks.ticks_executed},
        {"frames_executed", run.ticks.frames_executed},
        {"chunks_streamed", run.chunks_streamed},
        {"simulated_seconds", run.ticks.simulated_seconds},
        {"wall_seconds", run.ticks.wall_seconds},
        {"autosave_passes", run.ticks.autosave_passes},
        {"autosave_writes", run.ticks.autosave_writes},
        {"shutdown_save",
         {
             {"chunks_total", run.shutdown_save.chunks_total},
             {"chunks_dirty", run.shutdown_save.chunks_dirty},
             {"chunks_saved", run.shutdown_save.chunks_saved},
             {"saved", run.shutdown_save.saved},
         }},
    };
}

int RunSmoke(const ServerCliOptions& options) {
    LUMINUMBRA_CORE_INFO(
        "Headless server determinism smoke: preset={} seed={} ticks={} radius={}/{}",
        options.preset,
        options.seed,
        options.ticks,
        options.surface_radius,
        options.collision_radius);

    const SmokeRunResult first = RunSmokeOnce(options, "run-1");
    const SmokeRunResult replay = RunSmokeOnce(options, "run-2");

    // per-system sub-hashes must also match between run and replay; a
    // mismatch in any one localizes the divergence to that subsystem.
    // The MESH sub-hash is RENDER-only and intentionally NON-DETERMINISTIC: parallel chunk
    // meshing emits identical geometry in a worker-order-dependent vertex/index order. It is
    // NOT folded into world_hash (collision uses the heightmap, not this mesh — see
    // WorldPersistenceRoundtrip kRenderMeshHashExcludedFields), so it is reported for
    // localization but NOT required to match run==replay. Every SIM-truth sub-hash below
    // (terrain/water/entities/wind/weather/aether/scents/ecology/plants) must still match.
    const bool sub_hashes_match = first.sub_hashes.terrain == replay.sub_hashes.terrain &&
                                  first.sub_hashes.water == replay.sub_hashes.water &&
                                  first.sub_hashes.entities == replay.sub_hashes.entities &&
                                  first.sub_hashes.wind == replay.sub_hashes.wind &&
                                  first.sub_hashes.weather == replay.sub_hashes.weather &&
                                  first.sub_hashes.aether == replay.sub_hashes.aether &&
                                  first.sub_hashes.aether_state == replay.sub_hashes.aether_state &&
                                  first.scent_hash == replay.scent_hash &&
                                  first.ecology_hash == replay.ecology_hash &&
                                  first.plant_hash == replay.plant_hash;

    const bool deterministic =
        first.ok && replay.ok && first.world_hash == replay.world_hash && sub_hashes_match;

    // The per-tick availability-set trace must be run==replay. This proves the
    // activation queue preserves deterministic availability tick-for-tick, not
    // merely at the final world_hash. Only evaluated under --avail-trace.
    bool avail_trace_match = true;
    long long avail_first_divergent_tick = -1;
    if (options.availability_trace) {
        avail_trace_match = (first.avail_trace.size() == replay.avail_trace.size());
        const std::size_t n = std::min(first.avail_trace.size(), replay.avail_trace.size());
        for (std::size_t k = 0; k < n; ++k) {
            if (first.avail_trace[k] != replay.avail_trace[k]) {
                avail_trace_match = false;
                avail_first_divergent_tick = static_cast<long long>(first.avail_trace[k].first);
                break;
            }
        }
    }

    // NOTE: avail_trace_match is REPORT-ONLY, deliberately NOT folded into `passed`. The
    // per-tick availability set is run==replay deterministic for a STATIC anchor, but for a
    // MOVING anchor it only CONVERGES (the resident Ready-set differs per tick run-to-run while
    // the final world_hash matches — chunk stream-in/evict timing varies but settles). The
    // determinism gate is the final world_hash (run==replay in both modes). The trace's purpose
    // is the  before/after diff + surfacing the static-vs-moving residency property.
    const bool passed = deterministic && first.ticks.ticks_executed == options.ticks &&
                        replay.ticks.ticks_executed == options.ticks && first.chunks_streamed > 0;

    nlohmann::json artifact{
        {"schema", kServerTickArtifactSchema},
        {"generated_by", "luminumbra_server_app --smoke ()"},
        {"preset", options.preset},
        {"seed", options.seed},
        {"tick_rate_hz", 30.0},
        {"ticks_requested", options.ticks},
        {"surface_radius", options.surface_radius},
        {"collision_radius", options.collision_radius},
        {"runs", nlohmann::json::array({SmokeRunJson(first), SmokeRunJson(replay)})},
        {"world_hash", first.world_hash},
        {"world_hash_replay", replay.world_hash},
        {"sub_hashes",
         {
             {"terrain", first.sub_hashes.terrain},
             {"mesh", first.sub_hashes.mesh},
             {"water", first.sub_hashes.water},
             {"entities", first.sub_hashes.entities},
             {"wind", first.sub_hashes.wind},
             {"weather", first.sub_hashes.weather},
             {"aether", first.sub_hashes.aether},
             {"aether_state", first.sub_hashes.aether_state},
             {"scents", first.scent_hash},
             {"ecology", first.ecology_hash},
         }},
        {"sub_hashes_replay",
         {
             {"terrain", replay.sub_hashes.terrain},
             {"mesh", replay.sub_hashes.mesh},
             {"water", replay.sub_hashes.water},
             {"entities", replay.sub_hashes.entities},
             {"wind", replay.sub_hashes.wind},
             {"weather", replay.sub_hashes.weather},
             {"aether", replay.sub_hashes.aether},
             {"aether_state", replay.sub_hashes.aether_state},
             {"scents", replay.scent_hash},
             {"ecology", replay.ecology_hash},
         }},
        {"sub_hashes_match", sub_hashes_match},
        {"entity_count_start", first.creature_count_start},
        {"entity_count_end", first.creature_count_end},
        {"entity_count_start_replay", replay.creature_count_start},
        {"entity_count_end_replay", replay.creature_count_end},
        {"deterministic", deterministic},
        {"passed", passed},
    };

    // main-thread streaming-wait latency (the cost the activation queue activation queue
    // targets). Wall-clock observability — never feeds world_hash.
    artifact["main_wait_ms"] = {
        {"p50", first.ticks.main_wait_p50_ms},
        {"p95", first.ticks.main_wait_p95_ms},
        {"p99", first.ticks.main_wait_p99_ms},
        {"max", first.ticks.main_wait_max_ms},
        {"total", first.ticks.main_wait_total_ms},
    };
    LUMINUMBRA_CORE_INFO("Main-thread streaming-wait (activation-wait): p50={:.3f}ms p95={:.3f}ms "
                         "p99={:.3f}ms max={:.3f}ms "
                         "total={:.1f}ms over {} ticks",
                         first.ticks.main_wait_p50_ms,
                         first.ticks.main_wait_p95_ms,
                         first.ticks.main_wait_p99_ms,
                         first.ticks.main_wait_max_ms,
                         first.ticks.main_wait_total_ms,
                         first.ticks.ticks_executed);

    //  gate: emit the per-tick availability trace + run==replay verdict when on.
    if (options.availability_trace) {
        nlohmann::json trace = nlohmann::json::array();
        for (const auto& [tick, digest] : first.avail_trace) {
            trace.push_back({{"tick", tick}, {"avail", digest}});
        }
        artifact["availability_trace"] = std::move(trace);
        artifact["availability_trace_match"] = avail_trace_match;
        artifact["availability_trace_first_divergent_tick"] = avail_first_divergent_tick;
        if (avail_trace_match) {
            LUMINUMBRA_CORE_INFO(
                "Availability trace: {} ticks, run==replay MATCH (per-tick availability set is "
                "deterministic — the  activation-queue baseline)",
                first.avail_trace.size());
        } else if (options.moving) {
            // Expected for the moving anchor: per-tick residency converges (final world_hash
            // run==replay) but the intermediate Ready-set differs run-to-run as chunks
            // stream in / evict with timing variance. Informational, not a failure.
            LUMINUMBRA_CORE_INFO(
                "Availability trace: per-tick residency diverges at tick {} but CONVERGES "
                "(final world_hash run==replay) — expected for the moving anchor; gate stays the "
                "world_hash. sizes {}/{}",
                avail_first_divergent_tick,
                first.avail_trace.size(),
                replay.avail_trace.size());
        } else {
            // STATIC anchor: a per-tick mismatch is a real per-tick-residency regression.
            LUMINUMBRA_CORE_WARN(
                "Availability trace MISMATCH at tick {} for a STATIC anchor (per-tick residency "
                "should be deterministic — investigate) — sizes {}/{}",
                avail_first_divergent_tick,
                first.avail_trace.size(),
                replay.avail_trace.size());
        }
    }

    //  ( water cross-process): emit the per-tick water-state hash sequence + the
    // in-process run==replay verdict. The debug-vs-release comparison (the
    // host==peer cross-build gate) is validate-determinism-matrix.ps1
    // -Mode WaterCrossBuild, which diffs this array between the two builds'
    // artifacts. Hashes serialize as hex STRINGS (JSON numbers lose 64-bit
    // precision past 2^53).
    if (options.water_hash_trace) {
        bool water_trace_match = (first.water_hash_trace.size() == replay.water_hash_trace.size());
        long long water_first_divergent = -1;
        const std::size_t wn =
            std::min(first.water_hash_trace.size(), replay.water_hash_trace.size());
        for (std::size_t k = 0; k < wn; ++k) {
            if (first.water_hash_trace[k] != replay.water_hash_trace[k]) {
                water_trace_match = false;
                water_first_divergent = static_cast<long long>(first.water_hash_trace[k].first);
                break;
            }
        }
        nlohmann::json wtrace = nlohmann::json::array();
        for (const auto& [tick, hash] : first.water_hash_trace) {
            wtrace.push_back({{"tick", tick}, {"hash", fmt::format("{:016x}", hash)}});
        }
        artifact["water_hash_trace"] = std::move(wtrace);
        artifact["water_hash_trace_match"] = water_trace_match;
        artifact["water_hash_trace_first_divergent_tick"] = water_first_divergent;
        if (water_trace_match) {
            LUMINUMBRA_CORE_INFO("Water hash trace: {} ticks, run==replay MATCH (per-tick water "
                                 "state is deterministic)",
                                 first.water_hash_trace.size());
        } else {
            LUMINUMBRA_CORE_WARN(
                "Water hash trace MISMATCH at tick {} (per-tick water state diverged run-to-run "
                "IN-PROCESS — investigate before trusting any cross-build diff)",
                water_first_divergent);
        }
    }

    if (!options.artifact_path.empty()) {
        const fs::path artifact_path(options.artifact_path);
        std::error_code ec;
        if (artifact_path.has_parent_path()) {
            fs::create_directories(artifact_path.parent_path(), ec);
        }
        std::ofstream out(artifact_path);
        if (!out.is_open()) {
            LUMINUMBRA_CORE_ERROR("Failed to write smoke artifact: {}", options.artifact_path);
            return 1;
        }
        out << artifact.dump(2) << "\n";
        LUMINUMBRA_CORE_INFO("Smoke artifact written: {}", options.artifact_path);
    }

    if (!passed) {
        LUMINUMBRA_CORE_ERROR(
            "Headless server smoke FAILED: world_hash={} world_hash_replay={} ticks={}/{}",
            first.world_hash,
            replay.world_hash,
            first.ticks.ticks_executed,
            replay.ticks.ticks_executed);
        return 1;
    }

    LUMINUMBRA_CORE_INFO(
        "Headless server smoke passed: world_hash == world_hash_replay ({}), {} ticks per run",
        first.world_hash,
        options.ticks);
    return 0;
}

// ---------------------------------------------------------------------------
//   WindFieldDeterminism gate driver. Two independent runs of N
// WindFieldSystem updates with the same seed/anchor must reach the IDENTICAL
// wind sub-hash (the bit-determinism the world_hash `wind` slot depends on),
// the field must EVOLVE (sub-hash differs from the tick-0 field, so the gate is
// not vacuous), and the per-tick wind update cost is measured against the PINNED
// <= 0.15 ms budget at the streamed extent. The wind field is exercised in
// isolation (no chunk streaming) so the timing is the wind update ALONE.
// ---------------------------------------------------------------------------
std::string RunWindUpdatesAndHash(int seed, std::uint64_t ticks, const Luminumbra::Vec3& anchor) {
    Luminumbra::Systems::WindFieldSystem wind(seed);
    for (std::uint64_t t = 1; t <= ticks; ++t) {
        wind.Update(t, anchor);
    }
    return wind.ComputeWindSubHash();
}

int RunWindBench(const ServerCliOptions& options) {
    const int seed = static_cast<int>(std::strtoul(options.seed.c_str(), nullptr, 10));
    const std::uint64_t ticks = options.ticks;
    const Luminumbra::Vec3 anchor(8.0f, 100.0f, 8.0f);

    LUMINUMBRA_CORE_INFO(
        "Headless server WIND-BENCH: seed={} ticks={} (24 m cells x 3 layers x {} extent)",
        seed,
        ticks,
        Luminumbra::Systems::kWindExtentCells);

    // Determinism: two independent runs to the same tick must match.
    const std::string hash_run1 = RunWindUpdatesAndHash(seed, ticks, anchor);
    const std::string hash_run2 = RunWindUpdatesAndHash(seed, ticks, anchor);
    const bool deterministic = !hash_run1.empty() && hash_run1 == hash_run2;

    // Non-vacuity: the field at tick 0 differs from the field after N ticks.
    Luminumbra::Systems::WindFieldSystem wind_evolve(seed);
    const std::string hash_tick0 = wind_evolve.ComputeWindSubHash(); // constructed at tick 0
    for (std::uint64_t t = 1; t <= ticks; ++t) {
        wind_evolve.Update(t, anchor);
    }
    const std::string hash_evolved = wind_evolve.ComputeWindSubHash();
    const bool evolves = hash_tick0 != hash_evolved;

    // Budget: time the per-tick wind update in isolation. Warm up, then average a
    // large iteration count so the per-tick number is stable. This is TELEMETRY
    // (never hashed), the same justification as the runner's wall_seconds report.
    Luminumbra::Systems::WindFieldSystem wind_timed(seed);
    constexpr std::uint64_t kWarmup = 30;
    constexpr std::uint64_t kMeasured = 600;
    for (std::uint64_t t = 1; t <= kWarmup; ++t) {
        wind_timed.Update(t, anchor);
    }
    const auto t_start = std::chrono::steady_clock::now();
    for (std::uint64_t t = 1; t <= kMeasured; ++t) {
        wind_timed.Update(kWarmup + t, anchor);
    }
    const auto t_end = std::chrono::steady_clock::now();
    const double total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    const double per_tick_ms = total_ms / static_cast<double>(kMeasured);
    constexpr double kBudgetMs = 0.15;
    const bool within_budget = per_tick_ms <= kBudgetMs;

    // The bench's pass/fail is the BIT-DETERMINISM contract (deterministic +
    // evolves); the per-tick budget is REPORTED as data (within_budget /
    // per_tick_update_ms) for the gate to enforce against the appropriate
    // (release) preset -- an un-optimized debug build runs the same field ~10x
    // slower, so binding the budget into the bench's exit code would make the
    // debug-preset gate falsely fail a RELEASE-build budget (design ).
    const bool passed = deterministic && evolves;

    nlohmann::json artifact{
        {"schema", "luminumbra.wind_field_determinism.v1"},
        {"generated_by", "luminumbra_server_app --wind-bench ()"},
        {"seed", seed},
        {"ticks", ticks},
        {"cell_size_m", Luminumbra::Systems::kWindCellSizeM},
        {"extent_cells", Luminumbra::Systems::kWindExtentCells},
        {"layer_count", Luminumbra::Systems::kWindLayerCount},
        {"wind_sub_hash", hash_run1},
        {"wind_sub_hash_replay", hash_run2},
        {"deterministic", deterministic},
        {"wind_sub_hash_tick0", hash_tick0},
        {"wind_sub_hash_evolved", hash_evolved},
        {"evolves", evolves},
        {"per_tick_update_ms", per_tick_ms},
        {"budget_ms", kBudgetMs},
        {"within_budget", within_budget},
        {"measured_ticks", kMeasured},
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
            LUMINUMBRA_CORE_INFO("Wind-bench artifact written: {}", options.artifact_path);
        } else {
            LUMINUMBRA_CORE_ERROR("Failed to write wind-bench artifact: {}", options.artifact_path);
            return 1;
        }
    }

    if (!passed) {
        LUMINUMBRA_CORE_ERROR("Wind-bench FAILED (determinism): deterministic={} evolves={} "
                              "(wind_hash={} replay={})",
                              deterministic,
                              evolves,
                              hash_run1,
                              hash_run2);
        return 1;
    }

    LUMINUMBRA_CORE_INFO("Wind-bench passed: wind_sub_hash={} stable across runs, field evolves; "
                         "per_tick_update={:.4f} ms (budget {:.4f} ms, within_budget={}; budget "
                         "enforced by the gate on the release build)",
                         hash_run1,
                         per_tick_ms,
                         kBudgetMs,
                         within_budget);
    return 0;
}

// ---------------------------------------------------------------------------
//  AetherFieldDeterminism driver. Ticks a wind field + the Aether
// scalar field together (so the bench exercises the full advection+diffuse
// pipeline), twice, and asserts the aether sub-hash is bit-identical across
// runs and evolves over ticks. Same telemetry-only budget treatment as wind.
// ---------------------------------------------------------------------------
std::string RunAetherUpdatesAndHash(int seed, std::uint64_t ticks, const Luminumbra::Vec3& anchor) {
    Luminumbra::Systems::WindFieldSystem wind(seed);
    Luminumbra::Systems::AetherFieldSystem aether(seed);
    for (std::uint64_t t = 1; t <= ticks; ++t) {
        wind.Update(t, anchor);
        aether.Update(t, anchor, &wind);
    }
    return aether.ComputeAetherSubHash();
}

int RunAetherBench(const ServerCliOptions& options) {
    const int seed = static_cast<int>(std::strtoul(options.seed.c_str(), nullptr, 10));
    const std::uint64_t ticks = options.ticks;
    const Luminumbra::Vec3 anchor(8.0f, 100.0f, 8.0f);

    LUMINUMBRA_CORE_INFO(
        "Headless server: seed={} ticks={} (24 m cells x {} extent x {} diffuse sweeps)",
        seed,
        ticks,
        Luminumbra::Systems::kAetherExtentCells,
        Luminumbra::Systems::kAetherDiffuseIterations);

    const std::string hash_run1 = RunAetherUpdatesAndHash(seed, ticks, anchor);
    const std::string hash_run2 = RunAetherUpdatesAndHash(seed, ticks, anchor);
    const bool deterministic = !hash_run1.empty() && hash_run1 == hash_run2;

    // Non-vacuity: tick 0 differs from tick N.
    Luminumbra::Systems::AetherFieldSystem aether_evolve(seed);
    const std::string hash_tick0 = aether_evolve.ComputeAetherSubHash();
    Luminumbra::Systems::WindFieldSystem wind_evolve(seed);
    for (std::uint64_t t = 1; t <= ticks; ++t) {
        wind_evolve.Update(t, anchor);
        aether_evolve.Update(t, anchor, &wind_evolve);
    }
    const std::string hash_evolved = aether_evolve.ComputeAetherSubHash();
    const bool evolves = hash_tick0 != hash_evolved;

    // Telemetry-only per-tick budget (NOT hashed). Aether does an advection pass
    // + N Gauss-Seidel diffuse sweeps, so its budget is higher than wind's.
    Luminumbra::Systems::WindFieldSystem wind_timed(seed);
    Luminumbra::Systems::AetherFieldSystem aether_timed(seed);
    constexpr std::uint64_t kWarmup = 30;
    constexpr std::uint64_t kMeasured = 600;
    for (std::uint64_t t = 1; t <= kWarmup; ++t) {
        wind_timed.Update(t, anchor);
        aether_timed.Update(t, anchor, &wind_timed);
    }
    const auto t_start = std::chrono::steady_clock::now();
    for (std::uint64_t t = 1; t <= kMeasured; ++t) {
        wind_timed.Update(kWarmup + t, anchor);
        aether_timed.Update(kWarmup + t, anchor, &wind_timed);
    }
    const auto t_end = std::chrono::steady_clock::now();
    const double total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    const double per_tick_ms = total_ms / static_cast<double>(kMeasured);
    constexpr double kBudgetMs = 0.40; // wind + aether advect/diffuse, release target
    const bool within_budget = per_tick_ms <= kBudgetMs;

    const bool passed = deterministic && evolves;

    nlohmann::json artifact{
        {"schema", "luminumbra.aether_field_determinism.v1"},
        {"generated_by", "luminumbra_server_app --aether-bench ()"},
        {"seed", seed},
        {"ticks", ticks},
        {"cell_size_m", Luminumbra::Systems::kAetherCellSizeM},
        {"extent_cells", Luminumbra::Systems::kAetherExtentCells},
        {"diffuse_iterations", Luminumbra::Systems::kAetherDiffuseIterations},
        {"aether_sub_hash", hash_run1},
        {"aether_sub_hash_replay", hash_run2},
        {"deterministic", deterministic},
        {"aether_sub_hash_tick0", hash_tick0},
        {"aether_sub_hash_evolved", hash_evolved},
        {"evolves", evolves},
        {"per_tick_update_ms", per_tick_ms},
        {"budget_ms", kBudgetMs},
        {"within_budget", within_budget},
        {"measured_ticks", kMeasured},
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
            LUMINUMBRA_CORE_INFO("Aether-bench artifact written: {}", options.artifact_path);
        } else {
            LUMINUMBRA_CORE_ERROR("Failed to write aether-bench artifact: {}",
                                  options.artifact_path);
            return 1;
        }
    }

    if (!passed) {
        LUMINUMBRA_CORE_ERROR("Aether-bench FAILED (determinism): deterministic={} evolves={} "
                              "(aether_hash={} replay={})",
                              deterministic,
                              evolves,
                              hash_run1,
                              hash_run2);
        return 1;
    }

    LUMINUMBRA_CORE_INFO(
        "Aether-bench passed: aether_sub_hash={} stable across runs, field evolves; "
        "per_tick_update={:.4f} ms (budget {:.4f} ms, within_budget={}; budget "
        "enforced by the gate on the release build)",
        hash_run1,
        per_tick_ms,
        kBudgetMs,
        within_budget);
    return 0;
}

// ---------------------------------------------------------------------------
//   WeatherVisual determinism driver. Two independent runs of N
// WeatherSystem updates (advected by a parallel wind field) with the same
// seed/anchor must reach the IDENTICAL weather sub-hash (the bit-determinism the
// world_hash `weather` slot + the WeatherVisual state-hash assertion depend on),
// the state must EVOLVE (tick 0 != tick N -- gate is not vacuous), storm cells
// must stay BOUNDED (<= kMaxStormCells, ), and the per-tick weather update
// cost is measured against the PINNED <= 0.20 ms budget at the streamed extent.
// The weather core is exercised in isolation (no chunk streaming) so the timing
// is the weather update ALONE (plus the wind advection sample it requires).
// ---------------------------------------------------------------------------
struct WeatherBenchResult {
    std::string sub_hash;
    int max_storm_cells = 0;
    // lightning strike telemetry. total_strikes counts every strike
    // event scheduled over the run (the seed+13 schedule is non-vacuous when > 0);
    // max_live_strikes is the peak schedule-window size (bounded <= kMaxLiveStrikes).
    std::uint64_t total_strikes = 0;
    int max_live_strikes = 0;
};

WeatherBenchResult
RunWeatherUpdatesAndHash(int seed, std::uint64_t ticks, const Luminumbra::Vec3& anchor) {
    Luminumbra::Systems::WindFieldSystem wind(seed);
    Luminumbra::Systems::WeatherSystem weather(seed);
    WeatherBenchResult result;
    for (std::uint64_t t = 1; t <= ticks; ++t) {
        wind.Update(t, anchor);
        weather.Update(t, anchor, &wind);
        result.max_storm_cells = std::max(result.max_storm_cells, weather.active_storm_count());
        // Count strikes that LAND on this tick (each is a unique scheduled event).
        result.total_strikes += static_cast<std::uint64_t>(weather.StrikesThisTick().size());
        result.max_live_strikes = std::max(result.max_live_strikes, weather.live_strike_count());
    }
    result.sub_hash = weather.ComputeWeatherSubHash();
    return result;
}

int RunWeatherBench(const ServerCliOptions& options) {
    const int seed = static_cast<int>(std::strtoul(options.seed.c_str(), nullptr, 10));
    // A storm-bearing run: enough ticks for the seeded schedule to spawn + advect
    // several storm cells (the dedicated weather scenario, premise guard ). 300
    // ticks (10 s at 30 Hz) is the Endurance300Storm horizon.
    const std::uint64_t ticks = options.ticks > 0 ? options.ticks : 300;
    const Luminumbra::Vec3 anchor(8.0f, 100.0f, 8.0f);

    LUMINUMBRA_CORE_INFO("Headless server WEATHER-BENCH: seed={} ticks={} (24 m cells x {} extent, "
                         "storm-cell cap {})",
                         seed,
                         ticks,
                         Luminumbra::Systems::kWeatherExtentCells,
                         Luminumbra::Systems::kMaxStormCells);

    // Determinism: two independent runs to the same tick must match.
    const WeatherBenchResult run1 = RunWeatherUpdatesAndHash(seed, ticks, anchor);
    const WeatherBenchResult run2 = RunWeatherUpdatesAndHash(seed, ticks, anchor);
    const bool deterministic = !run1.sub_hash.empty() && run1.sub_hash == run2.sub_hash;

    // Non-vacuity: the state at tick 0 differs from the state after N ticks.
    Luminumbra::Systems::WindFieldSystem wind_evolve(seed);
    Luminumbra::Systems::WeatherSystem weather_evolve(seed);
    const std::string hash_tick0 = weather_evolve.ComputeWeatherSubHash();
    for (std::uint64_t t = 1; t <= ticks; ++t) {
        wind_evolve.Update(t, anchor);
        weather_evolve.Update(t, anchor, &wind_evolve);
    }
    const std::string hash_evolved = weather_evolve.ComputeWeatherSubHash();
    const bool evolves = hash_tick0 != hash_evolved;

    // Bounded state : the storm-cell count never exceeds the cap.
    const bool bounded = run1.max_storm_cells <= Luminumbra::Systems::kMaxStormCells &&
                         run2.max_storm_cells <= Luminumbra::Systems::kMaxStormCells;
    // Non-vacuity of the storm path: at least one storm cell spawned over the run
    // (so the gate actually exercised advection + the precip field).
    const bool storms_spawned = run1.max_storm_cells > 0;
    // non-vacuity of the LIGHTNING path -- at least one strike was
    // scheduled (proves the seed+13 schedule fired, exercising the strike sub-hash),
    // and the live strike window stayed BOUNDED (<= kMaxLiveStrikes, ). Strike
    // counts must MATCH across the two runs (the schedule is deterministic).
    const bool strikes_scheduled = run1.total_strikes > 0;
    const bool strikes_deterministic = run1.total_strikes == run2.total_strikes;
    const bool strikes_bounded = run1.max_live_strikes <= Luminumbra::Systems::kMaxLiveStrikes &&
                                 run2.max_live_strikes <= Luminumbra::Systems::kMaxLiveStrikes;

    // Budget: time the per-tick weather update (with wind advection) in isolation.
    // TELEMETRY (never hashed), same justification as the wind-bench timing.
    Luminumbra::Systems::WindFieldSystem wind_timed(seed);
    Luminumbra::Systems::WeatherSystem weather_timed(seed);
    constexpr std::uint64_t kWarmup = 30;
    constexpr std::uint64_t kMeasured = 600;
    for (std::uint64_t t = 1; t <= kWarmup; ++t) {
        wind_timed.Update(t, anchor);
        weather_timed.Update(t, anchor, &wind_timed);
    }
    const auto t_start = std::chrono::steady_clock::now();
    for (std::uint64_t t = 1; t <= kMeasured; ++t) {
        wind_timed.Update(kWarmup + t, anchor);
        weather_timed.Update(kWarmup + t, anchor, &wind_timed);
    }
    const auto t_end = std::chrono::steady_clock::now();
    const double total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    const double per_tick_ms = total_ms / static_cast<double>(kMeasured);
    constexpr double kBudgetMs = 0.20;
    const bool within_budget = per_tick_ms <= kBudgetMs;

    // Pass/fail is the BIT-DETERMINISM + bounded-state contract; the per-tick
    // budget is REPORTED for the gate to enforce on the release build.
    const bool passed = deterministic && evolves && bounded && storms_spawned &&
                        strikes_scheduled && strikes_deterministic && strikes_bounded;

    nlohmann::json artifact{
        {"schema", "luminumbra.weather_determinism.v1"},
        {"generated_by", "luminumbra_server_app --weather-bench ()"},
        {"seed", seed},
        {"ticks", ticks},
        {"cell_size_m", Luminumbra::Systems::kWeatherCellSizeM},
        {"extent_cells", Luminumbra::Systems::kWeatherExtentCells},
        {"max_storm_cell_cap", Luminumbra::Systems::kMaxStormCells},
        {"weather_sub_hash", run1.sub_hash},
        {"weather_sub_hash_replay", run2.sub_hash},
        {"deterministic", deterministic},
        {"weather_sub_hash_tick0", hash_tick0},
        {"weather_sub_hash_evolved", hash_evolved},
        {"evolves", evolves},
        {"max_storm_cells", run1.max_storm_cells},
        {"bounded_storm_cells", bounded},
        {"storms_spawned", storms_spawned},
        {"total_strikes", run1.total_strikes},
        {"total_strikes_replay", run2.total_strikes},
        {"max_live_strikes", run1.max_live_strikes},
        {"max_live_strike_cap", Luminumbra::Systems::kMaxLiveStrikes},
        {"strikes_scheduled", strikes_scheduled},
        {"strikes_deterministic", strikes_deterministic},
        {"strikes_bounded", strikes_bounded},
        {"per_tick_update_ms", per_tick_ms},
        {"budget_ms", kBudgetMs},
        {"within_budget", within_budget},
        {"measured_ticks", kMeasured},
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
            LUMINUMBRA_CORE_INFO("Weather-bench artifact written: {}", options.artifact_path);
        } else {
            LUMINUMBRA_CORE_ERROR("Failed to write weather-bench artifact: {}",
                                  options.artifact_path);
            return 1;
        }
    }

    if (!passed) {
        LUMINUMBRA_CORE_ERROR(
            "Weather-bench FAILED: deterministic={} evolves={} bounded={} storms_spawned={} "
            "(weather_hash={} replay={} max_storm_cells={})",
            deterministic,
            evolves,
            bounded,
            storms_spawned,
            run1.sub_hash,
            run2.sub_hash,
            run1.max_storm_cells);
        return 1;
    }

    LUMINUMBRA_CORE_INFO(
        "Weather-bench passed: weather_sub_hash={} stable across runs, state evolves; "
        "max_storm_cells={} (cap {}); per_tick_update={:.4f} ms (budget {:.4f} ms, "
        "within_budget={}; budget enforced by the gate on the release build)",
        run1.sub_hash,
        run1.max_storm_cells,
        Luminumbra::Systems::kMaxStormCells,
        per_tick_ms,
        kBudgetMs,
        within_budget);
    return 0;
}

// ---------------------------------------------------------------------------
//  heavy-mode oracle (Factorio "heavy mode", research Area 2 takeaway 5):
// tick N, SAVE, LOAD into a FRESH session, then resimulate M further ticks in
// BOTH the original and the loaded session and compare full + per-system
// hashes. Catches two bug classes the per-tick smoke cannot: (a) sim state not
// covered by the hash, and (b) save/load round-trip divergence. Reuses the
// existing WorldSaveService save/load machinery (GameSession::SaveWorldState +
// the runner's world_id boot path), so no new persistence format.
// ---------------------------------------------------------------------------

struct HeavyHashes {
    std::string world_hash;
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub;
    std::string scent_hash;
};

HeavyHashes CaptureHashes(Luminumbra::Server::ServerWorldRunner& runner) {
    HeavyHashes h;
    h.world_hash = runner.ComputeWorldHash();
    h.sub = runner.ComputeWorldSubHashes();
    h.scent_hash = runner.Session() ? runner.Session()->ComputeScentSubHash() : std::string();
    return h;
}

// the heavy oracle's equality is over AUTHORITATIVE SIMULATION STATE
// (terrain SDF + water sim + entities). Surface mesh geometry is EXCLUDED: it is
// a deterministically-regenerated DERIVED render artifact, and the async
// re-meshing of chunks adopted across a save/load boundary legitimately reaches
// the same geometry via a different in-memory pending-mesh/version snapshot than
// the originating session held. The sub-hashes localize this precisely (only
// `mesh` differs; terrain/water/entities are byte-identical), which is exactly
// the desync-localization the sub-hashes exist to provide. The top-level
// world_hash (which DOES include mesh) is reported but not asserted on across
// the round-trip for this reason; it is still asserted run==replay in --smoke.
bool AuthoritativeStateEqual(const HeavyHashes& a, const HeavyHashes& b) {
    // the heavy oracle compares two sessions at DIFFERENT tick
    // phases across the save/load boundary (original at tick N vs the freshly
    // loaded session at tick 0; later original at N+M vs loaded at M). Terrain/
    // water/entities are spatial state that is invariant once streaming settles,
    // so they compare exactly. The WIND field is TICK-DEPENDENT by design (it
    // evolves every tick), so it legitimately differs between two sessions at
    // different tick counts and is NOT compared here -- exactly like mesh is
    // excluded for a different reason. Wind's determinism is proven where the
    // comparison IS same-tick: the smoke (run==replay), WindFieldDeterminism,
    // and the replay roundtrip (same-tick checkpoint hashes, which include wind
    // via the composite world_hash).
    //
    // WEATHER is excluded for the IDENTICAL reason as wind. The
    // weather core (region category map + storm cells + precipitation field) is a
    // pure function of (seed+12, ABSOLUTE tick, anchor) -- it evolves every tick
    // and the storm-cell schedule keys on the absolute tick-epoch. Across the
    // save/load boundary the loaded session's tick counter resets to 0, so it has
    // no concept of the original's absolute tick; persisting the accumulated
    // weather state could NOT make a cross-phase compare match (original@N+M vs
    // loaded@M differ in absolute tick), so it is recompute-and-excluded here. Its
    // determinism is proven where the comparison IS same-tick: the smoke
    // (run==replay), the WeatherVisual state-hash (resim/replay at the same tick),
    // and the replay roundtrip / lockstep checkpoints (which include weather via
    // the composite world_hash).
    return a.sub.terrain == b.sub.terrain && a.sub.water == b.sub.water &&
           a.sub.entities == b.sub.entities;
}

bool MeshEqual(const HeavyHashes& a, const HeavyHashes& b) {
    return a.sub.mesh == b.sub.mesh;
}

nlohmann::json HeavyHashJson(const HeavyHashes& h) {
    return nlohmann::json{
        {"world_hash", h.world_hash},
        {"sub_hashes",
         {
             {"terrain", h.sub.terrain},
             {"mesh", h.sub.mesh},
             {"water", h.sub.water},
             {"entities", h.sub.entities},
             {"wind", h.sub.wind},
             {"weather", h.sub.weather},
             {"aether", h.sub.aether},
             {"aether_state", h.sub.aether_state},
             {"scents", h.scent_hash},
         }},
    };
}

int RunHeavy(const ServerCliOptions& options) {
    LUMINUMBRA_CORE_INFO(
        "Headless server HEAVY oracle: preset={} seed={} ticks={} resim={} radius={}/{}",
        options.preset,
        options.seed,
        options.ticks,
        options.heavy_resim,
        options.surface_radius,
        options.collision_radius);

    // --- : boot the ORIGINAL session, tick N, SAVE its state. ---
    Luminumbra::Server::ServerWorldRunnerConfig cfgOrig = RunnerConfigFrom(options);
    cfgOrig.world_id.clear();
    cfgOrig.world_name = "Heavy Original";
    cfgOrig.autosave_interval_ticks = 0; // explicit save below; no autosave noise

    Luminumbra::Server::ServerWorldRunner original(std::move(cfgOrig));
    if (!original.Boot()) {
        LUMINUMBRA_CORE_ERROR("heavy: original session failed to boot");
        return 1;
    }
    const auto pre_save_ticks = original.RunFixedTicks(options.ticks);

    // Persist the COMPLETE streamed-chunk set (a never-edited world has no dirty
    // chunks, so the dirty-gated GameSession::SaveWorldState would write
    // nothing; the heavy oracle needs the full set on disk to adopt on load).
    const std::size_t saved_chunks = original.SaveFullSnapshot();
    const std::string world_id = original.Session()->GetMetadata().worldId;
    const fs::path save_dir = original.Session()->GetWorldSaveDir();
    // the rotating sim-window cursor at save — the loaded session must
    // restore exactly this value or resim picks different windows and diverges.
    const std::size_t cursor_at_save =
        original.Session()->GetWorldSystem()->GetWaterSimWindowCursor();
    if (saved_chunks == 0 || world_id.empty()) {
        LUMINUMBRA_CORE_ERROR("heavy: SaveFullSnapshot wrote no chunks (world_id='{}')", world_id);
        return 1;
    }
    const HeavyHashes orig_at_save = CaptureHashes(original);

    // --- : LOAD a FRESH session from the saved world_id. ---
    Luminumbra::Server::ServerWorldRunnerConfig cfgLoad = RunnerConfigFrom(options);
    cfgLoad.world_id = world_id;
    cfgLoad.world_name = "Heavy Loaded";
    cfgLoad.autosave_interval_ticks = 0;

    Luminumbra::Server::ServerWorldRunner loaded(std::move(cfgLoad));
    if (!loaded.Boot()) {
        LUMINUMBRA_CORE_ERROR("heavy: loaded session failed to boot from world_id '{}'", world_id);
        return 1;
    }
    const HeavyHashes loaded_at_load = CaptureHashes(loaded);
    const std::size_t cursor_at_load =
        loaded.Session()->GetWorldSystem()->GetWaterSimWindowCursor();

    // The loaded session, immediately after load, must match the original at
    // save on AUTHORITATIVE sim state (terrain/water/entities). Mesh is tracked
    // informationally (see AuthoritativeStateEqual rationale).
    const bool roundtrip_ok = AuthoritativeStateEqual(orig_at_save, loaded_at_load);
    const bool roundtrip_mesh_match = MeshEqual(orig_at_save, loaded_at_load);

    // --- : resimulate M further ticks on BOTH sessions. ---
    const auto orig_resim = original.RunFixedTicks(options.heavy_resim);
    const auto loaded_resim = loaded.RunFixedTicks(options.heavy_resim);
    const HeavyHashes orig_final = CaptureHashes(original);
    const HeavyHashes loaded_final = CaptureHashes(loaded);

    const bool resim_ok = AuthoritativeStateEqual(orig_final, loaded_final);
    const bool resim_mesh_match = MeshEqual(orig_final, loaded_final);

    //  diagnostic: on a water resim mismatch, localize it — which chunks,
    // which fields. Field-by-field compare over both sessions' chunk snapshots.
    if (orig_final.sub.water != loaded_final.sub.water) {
        auto orig_chunks = original.Session()->GetWorldSystem()->snapshot_streamed_chunks();
        auto loaded_chunks = loaded.Session()->GetWorldSystem()->snapshot_streamed_chunks();
        std::unordered_map<Luminumbra::ChunkID, std::shared_ptr<Luminumbra::Chunk>> loaded_by_id;
        for (const auto& c : loaded_chunks)
            if (c)
                loaded_by_id[c->get_id()] = c;
        std::size_t diff_depth = 0, diff_bed = 0, diff_flux = 0, diff_sleep = 0, diff_ticks = 0,
                    diff_level = 0, diff_delta = 0, printed = 0;
        for (const auto& oc : orig_chunks) {
            if (!oc)
                continue;
            auto it = loaded_by_id.find(oc->get_id());
            if (it == loaded_by_id.end())
                continue;
            const auto& lc = it->second;
            const bool d_depth = oc->water_depth_mm != lc->water_depth_mm;
            const bool d_bed = oc->water_bed_mm != lc->water_bed_mm;
            const bool d_flux = oc->water_edge_flux != lc->water_edge_flux;
            const bool d_sleep = oc->is_water_sleeping.load() != lc->is_water_sleeping.load();
            const bool d_ticks = oc->ticks_below_threshold != lc->ticks_below_threshold;
            const bool d_level = oc->water_level_data != lc->water_level_data;
            const bool d_delta = oc->max_water_delta_last_tick != lc->max_water_delta_last_tick;
            diff_depth += d_depth;
            diff_bed += d_bed;
            diff_flux += d_flux;
            diff_sleep += d_sleep;
            diff_ticks += d_ticks;
            diff_level += d_level;
            diff_delta += d_delta;
            if ((d_depth || d_flux || d_sleep || d_ticks) && printed < 8) {
                ++printed;
                const auto cc = oc->get_coords();
                LUMINUMBRA_CORE_ERROR(
                    "water resim diff chunk ({},{},{}): depth={} bed={} flux={} sleep={} "
                    "(o={} l={}) tbt={} (o={} l={}) level={} delta={}",
                    cc.x,
                    cc.y,
                    cc.z,
                    d_depth,
                    d_bed,
                    d_flux,
                    d_sleep,
                    oc->is_water_sleeping.load(),
                    lc->is_water_sleeping.load(),
                    d_ticks,
                    oc->ticks_below_threshold,
                    lc->ticks_below_threshold,
                    d_level,
                    d_delta);
            }
        }
        LUMINUMBRA_CORE_ERROR(
            "water resim diff totals over {} chunks: depth={} bed={} flux={} sleep={} "
            "ticks_below={} level={} delta={}",
            orig_chunks.size(),
            diff_depth,
            diff_bed,
            diff_flux,
            diff_sleep,
            diff_ticks,
            diff_level,
            diff_delta);
    }
    // the settle CONTRACT that makes save/load water round-trip — the
    // FRESH (original) boot leaves zero uninitialized chunks; the LOADED boot skips
    // the water settle entirely (water paused through Boot, restored mid-flow state
    // authoritative, resuming from the persisted sim-window cursor). A violated
    // contract is exactly the water-roundtrip hazard this oracle measures. A global
    // all-asleep fixed point does not exist (boundary limit cycles + wake
    // propagation), so awake > 0 is expected and NOT asserted.
    const auto& settle_orig = original.GetBootSettleStats();
    const auto& settle_loaded = loaded.GetBootSettleStats();
    const bool settle_ok = settle_orig.contract_ok() && settle_loaded.contract_ok() &&
                           settle_loaded.water_settle_skipped;
    const bool passed = roundtrip_ok && resim_ok && settle_ok &&
                        pre_save_ticks.ticks_executed == options.ticks &&
                        orig_resim.ticks_executed == options.heavy_resim &&
                        loaded_resim.ticks_executed == options.heavy_resim;

    auto SettleJson = [](const Luminumbra::Server::ServerWorldRunner::BootSettleStats& s) {
        return nlohmann::json{
            {"water_chunks", s.water_chunks},
            {"awake", s.awake},
            {"uninited", s.uninited},
            {"iterations", s.iterations},
            {"water_settle_skipped", s.water_settle_skipped},
            {"contract_ok", s.contract_ok()},
        };
    };

    nlohmann::json artifact{
        {"schema", "luminumbra.server_tick_heavy.v1"},
        {"generated_by", "luminumbra_server_app --heavy ()"},
        {"preset", options.preset},
        {"seed", options.seed},
        {"tick_rate_hz", 30.0},
        {"ticks_before_save", options.ticks},
        {"resim_ticks", options.heavy_resim},
        {"world_id", world_id},
        {"original_at_save", HeavyHashJson(orig_at_save)},
        {"loaded_at_load", HeavyHashJson(loaded_at_load)},
        {"roundtrip_match", roundtrip_ok},
        {"roundtrip_mesh_match", roundtrip_mesh_match},
        {"original_final", HeavyHashJson(orig_final)},
        {"loaded_final", HeavyHashJson(loaded_final)},
        {"resim_match", resim_ok},
        {"resim_mesh_match", resim_mesh_match},
        {"boot_settle_original", SettleJson(settle_orig)},
        {"boot_settle_loaded", SettleJson(settle_loaded)},
        {"settle_contract_ok", settle_ok},
        {"water_sim_cursor_at_save", cursor_at_save},
        {"water_sim_cursor_at_load", cursor_at_load},
        {"authoritative_sections", nlohmann::json::array({"terrain", "water", "entities"})},
        {"mesh_excluded_reason",
         "surface mesh is a deterministically-regenerated derived render artifact; async "
         "re-meshing across a save/load boundary reaches identical geometry via a different "
         "in-memory pending-mesh snapshot. Authoritative sim state (terrain/water/entities) "
         "round-trips exactly."},
        {"passed", passed},
    };

    // Tear down both and clean up the throwaway save directory.
    original.Shutdown();
    loaded.Shutdown();
    if (!save_dir.empty()) {
        std::error_code ec;
        fs::remove_all(save_dir, ec);
    }

    if (!options.artifact_path.empty()) {
        const fs::path artifact_path(options.artifact_path);
        std::error_code ec;
        if (artifact_path.has_parent_path()) {
            fs::create_directories(artifact_path.parent_path(), ec);
        }
        std::ofstream out(artifact_path);
        if (out.is_open()) {
            out << artifact.dump(2) << "\n";
            LUMINUMBRA_CORE_INFO("Heavy artifact written: {}", options.artifact_path);
        } else {
            LUMINUMBRA_CORE_ERROR("Failed to write heavy artifact: {}", options.artifact_path);
            return 1;
        }
    }

    if (!passed) {
        LUMINUMBRA_CORE_ERROR(
            "Headless server HEAVY FAILED: roundtrip_match={} resim_match={} "
            "settle_contract_ok={} (orig uninited={} awake={}; loaded uninited={} "
            "awake={} skipped={}) orig_save={} loaded_load={} orig_final={} loaded_final={}",
            roundtrip_ok,
            resim_ok,
            settle_ok,
            settle_orig.uninited,
            settle_orig.awake,
            settle_loaded.uninited,
            settle_loaded.awake,
            settle_loaded.water_settle_skipped,
            orig_at_save.world_hash,
            loaded_at_load.world_hash,
            orig_final.world_hash,
            loaded_final.world_hash);
        return 1;
    }

    LUMINUMBRA_CORE_INFO("Headless server HEAVY passed: round-trip + {}-tick resim hashes equal "
                         "(world_hash={})",
                         options.heavy_resim,
                         orig_final.world_hash);
    return 0;
}

// ---------------------------------------------------------------------------
//  session replay (LREC1). Recording is the desync-repro tool: a stream
// is (boot parameters + per-tick inputs + 30-tick world_hash checkpoints).
// Replay reboots from the header, feeds the recorded inputs, and verifies the
// live hashes against the recorded checkpoints -- the first mismatch localizes a
// desync to a tick + a sub-hash section (the  localization). The headless
// smoke has NO player inputs today, so the per-tick input set is empty; the
// format carries it anyway for  (lockstep transport, which consumes this
// stream as its desync dump format).
//
// Determinism: recording must NOT perturb the simulation. The ReplayWriter
// buffers all records in memory and flushes to disk only at Finalize, so no
// IO sits on the tick path. Checkpoint hashing reuses ComputeWorldHash /
// ComputeWorldSubHashes (the same quiesce-then-snapshot the smoke does), which
// reads state without mutating it. Proof: the ReplayRoundtrip gate asserts the
// recorded run reaches the SAME 0eac465289e7c88b as the smoke (
// hash revision: was 2fa007951a21e140 before the `wind` sub-hash slot landed).
// ---------------------------------------------------------------------------

constexpr std::uint64_t kCheckpointIntervalTicks = 30; // one second at 30 Hz

// Captures a checkpoint record at the given tick from a booted runner. Uses the
// single-snapshot combined hash path (one settled-state read per checkpoint).
Luminumbra::Replay::CheckpointRecord
CaptureCheckpoint(std::uint64_t tick, Luminumbra::Server::ServerWorldRunner& runner) {
    Luminumbra::Replay::CheckpointRecord cp;
    cp.tick = tick;
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub;
    runner.ComputeWorldHashAndSubHashes(cp.world_hash, sub);
    cp.terrain = sub.terrain;
    cp.water = sub.water;
    cp.entities = sub.entities;
    return cp;
}

int RunRecord(const ServerCliOptions& options) {
    LUMINUMBRA_CORE_INFO("Headless server RECORD (LREC1): preset={} seed={} ticks={} -> {}",
                         options.preset,
                         options.seed,
                         options.ticks,
                         options.record_path);

    Luminumbra::Server::ServerWorldRunnerConfig config = RunnerConfigFrom(options);
    config.world_id.clear();
    config.world_name = "Replay Record";
    config.autosave_interval_ticks = 0; // no autosave noise during recording

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        LUMINUMBRA_CORE_ERROR("record: session failed to boot");
        return 1;
    }

    Luminumbra::Replay::ReplayHeader header;
    header.version = Luminumbra::Replay::kLrec1Version;
    header.tick_rate_hz = 30;
    header.seed_string = options.seed;
    header.seed = std::strtoull(options.seed.c_str(), nullptr, 10);
    header.preset = options.preset;
    header.preset_hash =
        std::strtoull(Luminumbra::Replay::Fnv1a64Hex(options.preset).c_str(), nullptr, 16);
    header.surface_radius = static_cast<std::uint32_t>(options.surface_radius);
    header.collision_radius = static_cast<std::uint32_t>(options.collision_radius);
    header.engine_version = std::string(luminumbra::core::GetEngineVersionString());
    header.start_world_hash = runner.ComputeWorldHash();

    Luminumbra::Replay::ReplayWriter writer;
    if (!writer.Open(options.record_path, header)) {
        LUMINUMBRA_CORE_ERROR("record: cannot open replay stream '{}'", options.record_path);
        return 1;
    }

    // Tick-by-tick: record the (empty today) per-tick input set, then capture a
    // checkpoint every kCheckpointIntervalTicks ticks. Stepping one tick at a
    // time keeps the recorder's hash-capture aligned to the same settled state
    // the smoke's after-the-fact ComputeWorldHash observes.
    const std::vector<std::uint8_t> empty_inputs; // no player inputs in headless
    std::uint64_t executed = 0;
    while (executed < options.ticks) {
        // The input set for the tick ABOUT to run.  will populate this.
        writer.RecordInput(executed + 1, empty_inputs);
        const auto step = runner.RunFixedTicks(1);
        executed += step.ticks_executed;
        if (step.ticks_executed == 0) {
            LUMINUMBRA_CORE_ERROR("record: tick {} did not advance", executed + 1);
            return 1;
        }
        if ((executed % kCheckpointIntervalTicks) == 0) {
            writer.RecordCheckpoint(CaptureCheckpoint(executed, runner));
        }
    }

    if (!writer.Finalize(executed)) {
        LUMINUMBRA_CORE_ERROR("record: failed to finalize replay stream '{}'", options.record_path);
        return 1;
    }

    const std::string final_hash = runner.ComputeWorldHash();
    const fs::path save_dir = runner.Session()->GetWorldSaveDir();
    runner.Shutdown();
    if (!save_dir.empty()) {
        std::error_code ec;
        fs::remove_all(save_dir, ec);
    }

    LUMINUMBRA_CORE_INFO(
        "record: wrote {} ({} ticks, {} input records, {} checkpoints, end_hash={})",
        options.record_path,
        executed,
        writer.InputRecordCount(),
        writer.CheckpointRecordCount(),
        final_hash);
    return 0;
}

int RunReplay(const ServerCliOptions& options) {
    LUMINUMBRA_CORE_INFO("Headless server REPLAY (LREC1): {}", options.replay_path);

    auto contents = Luminumbra::Replay::ReadReplay(options.replay_path);
    if (!contents.has_value()) {
        LUMINUMBRA_CORE_ERROR("replay: '{}' is not a valid LREC1 stream", options.replay_path);
        return 1;
    }
    if (contents->truncated || !contents->trailer_present) {
        LUMINUMBRA_CORE_ERROR("replay: stream '{}' is truncated (no valid trailer)",
                              options.replay_path);
        return 1;
    }

    const Luminumbra::Replay::ReplayHeader& header = contents->header;
    // Refuse a version / engine mismatch loudly (Factorio replays break silently
    // across versions; we refuse instead -- research Area 2 takeaway 8).
    const std::string engine_now(luminumbra::core::GetEngineVersionString());
    if (header.version != Luminumbra::Replay::kLrec1Version) {
        LUMINUMBRA_CORE_ERROR("replay: stream LREC version {} != engine {}",
                              header.version,
                              Luminumbra::Replay::kLrec1Version);
        return 1;
    }

    // Boot the session from the header parameters (mirror ServerWorldRunnerConfig).
    Luminumbra::Server::ServerWorldRunnerConfig config;
    config.root_path = options.root;
    config.seed = header.seed_string;
    config.preset = header.preset;
    config.world_id.clear();
    config.world_name = "Replay Playback";
    config.surface_radius = static_cast<int>(header.surface_radius);
    config.collision_radius = static_cast<int>(header.collision_radius);
    config.autosave_interval_ticks = 0;

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        LUMINUMBRA_CORE_ERROR("replay: session failed to boot from header");
        return 1;
    }

    // The post-boot hash must match the recorded start_world_hash, else the boot
    // parameters or worldgen drifted before tick 1 (a tick-0 divergence).
    const std::string live_start = runner.ComputeWorldHash();
    bool start_match = (live_start == header.start_world_hash);

    // Drive the recorded ticks. At each checkpoint, compare live vs recorded.
    bool diverged = false;
    std::uint64_t divergence_tick = 0;
    std::string divergence_section;
    std::string expected_hash;
    std::string actual_hash;
    std::uint64_t checkpoints_verified = 0;

    std::uint64_t executed = 0;
    if (!start_match) {
        diverged = true;
        divergence_tick = 0;
        divergence_section = "world_hash";
        expected_hash = header.start_world_hash;
        actual_hash = live_start;
    }

    while (!diverged && executed < contents->tick_count) {
        const std::uint64_t next_tick = executed + 1;
        // Feed the recorded input set for this tick (empty today; the replay
        // driver applies it once  carries real inputs).
        const Luminumbra::Replay::InputRecord* input =
            Luminumbra::Replay::FindInput(*contents, next_tick);
        (void)input; // applied by the transport in; no-op for empty sets
        const auto step = runner.RunFixedTicks(1);
        executed += step.ticks_executed;
        if (step.ticks_executed == 0) {
            LUMINUMBRA_CORE_ERROR("replay: tick {} did not advance", next_tick);
            runner.Shutdown();
            return 1;
        }

        if ((executed % kCheckpointIntervalTicks) == 0) {
            const Luminumbra::Replay::CheckpointRecord* recorded =
                Luminumbra::Replay::FindCheckpoint(*contents, executed);
            if (recorded == nullptr) {
                LUMINUMBRA_CORE_ERROR("replay: missing recorded checkpoint at tick {}", executed);
                runner.Shutdown();
                return 1;
            }
            const auto live = CaptureCheckpoint(executed, runner);
            // Compare top-level hash, then localize via authoritative sub-hashes.
            if (live.world_hash != recorded->world_hash) {
                diverged = true;
                divergence_tick = executed;
                expected_hash = recorded->world_hash;
                actual_hash = live.world_hash;
                if (live.terrain != recorded->terrain) {
                    divergence_section = "terrain";
                } else if (live.water != recorded->water) {
                    divergence_section = "water";
                } else if (live.entities != recorded->entities) {
                    divergence_section = "entities";
                } else {
                    // Authoritative sub-hashes all match but the top-level hash
                    // differs -> the divergence is in the mesh (derived render
                    // artifact) or another non-authoritative component.
                    divergence_section = "world_hash";
                }
                break;
            }
            ++checkpoints_verified;
        }
    }

    const std::string live_end = diverged ? actual_hash : runner.ComputeWorldHash();
    const bool end_hash_match = !diverged && (live_end == [&]() -> std::string {
        // The recorded end-hash is the last checkpoint's world_hash if the run
        // ends on a checkpoint boundary; otherwise re-derive from the trailer
        // tick_count's checkpoint. For the 90-tick gate, 90 is a checkpoint.
        const Luminumbra::Replay::CheckpointRecord* last =
            Luminumbra::Replay::FindCheckpoint(*contents, contents->tick_count);
        return last ? last->world_hash : std::string();
    }());

    const fs::path save_dir = runner.Session()->GetWorldSaveDir();
    runner.Shutdown();
    if (!save_dir.empty()) {
        std::error_code ec;
        fs::remove_all(save_dir, ec);
    }

    if (diverged) {
        // Write the divergence artifact (JSON: tick, expected/actual per section).
        const Luminumbra::Replay::CheckpointRecord* recorded =
            Luminumbra::Replay::FindCheckpoint(*contents, divergence_tick);
        nlohmann::json artifact{
            {"schema", "luminumbra.replay_divergence.v1"},
            {"generated_by", "luminumbra_server_app --replay ()"},
            {"replay_path", options.replay_path},
            {"diverged", true},
            {"divergence_tick", divergence_tick},
            {"divergence_section", divergence_section},
            {"expected_world_hash", expected_hash},
            {"actual_world_hash", actual_hash},
            {"checkpoints_verified_before_divergence", checkpoints_verified},
            {"tick_count", contents->tick_count},
        };
        if (recorded != nullptr) {
            artifact["sections"] = {
                {"world_hash", {{"expected", recorded->world_hash}, {"actual", actual_hash}}},
                {"terrain", {{"expected", recorded->terrain}}},
                {"water", {{"expected", recorded->water}}},
                {"entities", {{"expected", recorded->entities}}},
            };
        }
        if (!options.artifact_path.empty()) {
            const fs::path artifact_path(options.artifact_path);
            std::error_code ec;
            if (artifact_path.has_parent_path()) {
                fs::create_directories(artifact_path.parent_path(), ec);
            }
            std::ofstream out(artifact_path);
            if (out.is_open()) {
                out << artifact.dump(2) << "\n";
                LUMINUMBRA_CORE_INFO("replay divergence artifact written: {}",
                                     options.artifact_path);
            }
        }
        LUMINUMBRA_CORE_ERROR(
            "replay DIVERGED at tick {} (section={}): expected world_hash={} actual={} "
            "({} checkpoints verified before divergence)",
            divergence_tick,
            divergence_section,
            expected_hash,
            actual_hash,
            checkpoints_verified);
        return 1;
    }

    // Clean completion: write the replay (success) artifact.
    nlohmann::json artifact{
        {"schema", "luminumbra.replay_roundtrip.v1"},
        {"generated_by", "luminumbra_server_app --replay ()"},
        {"replay_path", options.replay_path},
        {"diverged", false},
        {"engine_version", header.engine_version},
        {"replayed_engine_version", engine_now},
        {"preset", header.preset},
        {"seed", header.seed_string},
        {"tick_rate_hz", header.tick_rate_hz},
        {"start_world_hash", header.start_world_hash},
        {"start_world_hash_match", start_match},
        {"ticks_replayed", executed},
        {"tick_count", contents->tick_count},
        {"checkpoints_verified", checkpoints_verified},
        {"input_records", contents->inputs.size()},
        {"end_world_hash", live_end},
        {"end_hash_match", end_hash_match},
        {"passed", start_match && end_hash_match && executed == contents->tick_count},
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
            LUMINUMBRA_CORE_INFO("replay roundtrip artifact written: {}", options.artifact_path);
        } else {
            LUMINUMBRA_CORE_ERROR("replay: failed to write artifact '{}'", options.artifact_path);
            return 1;
        }
    }

    const bool passed = start_match && end_hash_match && executed == contents->tick_count;
    if (!passed) {
        LUMINUMBRA_CORE_ERROR("replay FAILED: start_match={} end_match={} ticks={}/{}",
                              start_match,
                              end_hash_match,
                              executed,
                              contents->tick_count);
        return 1;
    }
    LUMINUMBRA_CORE_INFO(
        "replay passed: {} ticks, {} checkpoints verified, end_hash={} matches recording",
        executed,
        checkpoints_verified,
        live_end);
    return 0;
}

//  ReplayDivergence gate fixture: read an LREC1 stream and rewrite it
// with ONE checkpoint's world_hash + authoritative sub-hashes corrupted, so a
// replay of the mutated stream MUST diverge at exactly that checkpoint. This is
// the least-hacky mutation: it parses the real stream (no fragile byte-offset
// math) and re-emits it with one record altered, proving the oracle is not
// vacuous -- a tampered stream is caught at the FIRST checkpoint after the edit.
int RunMutateReplayFixture(const ServerCliOptions& options) {
    LUMINUMBRA_CORE_INFO("Replay fixture mutation: {} -> (corrupt one checkpoint)",
                         options.mutate_replay_fixture);

    auto contents = Luminumbra::Replay::ReadReplay(options.mutate_replay_fixture);
    if (!contents.has_value()) {
        LUMINUMBRA_CORE_ERROR("mutate: '{}' is not a valid LREC1 stream",
                              options.mutate_replay_fixture);
        return 1;
    }
    if (contents->checkpoints.empty()) {
        LUMINUMBRA_CORE_ERROR("mutate: stream has no checkpoints to corrupt");
        return 1;
    }

    // Re-emit the stream with the FIRST checkpoint's hashes flipped to a value
    // that cannot occur (prefix "dead"), guaranteeing a mismatch there.
    Luminumbra::Replay::ReplayWriter writer;
    if (!writer.Open(options.mutate_replay_fixture, contents->header)) {
        LUMINUMBRA_CORE_ERROR("mutate: cannot reopen '{}'", options.mutate_replay_fixture);
        return 1;
    }
    for (const auto& input : contents->inputs) {
        writer.RecordInput(input.tick, input.inputs);
    }
    const std::uint64_t target_tick = contents->checkpoints.front().tick;
    for (auto cp : contents->checkpoints) {
        if (cp.tick == target_tick) {
            cp.world_hash = "dead" + cp.world_hash.substr(4);
            cp.terrain = "dead" + cp.terrain.substr(4);
        }
        writer.RecordCheckpoint(cp);
    }
    if (!writer.Finalize(contents->tick_count)) {
        LUMINUMBRA_CORE_ERROR("mutate: failed to finalize mutated stream");
        return 1;
    }
    LUMINUMBRA_CORE_INFO("mutate: corrupted checkpoint at tick {} in {}",
                         target_tick,
                         options.mutate_replay_fixture);
    return 0;
}

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

int RunServer(const ServerCliOptions& options) {
    Luminumbra::Server::ServerWorldRunner runner(RunnerConfigFrom(options));
    if (!runner.Boot()) {
        return 1;
    }

    const Luminumbra::Server::ServerTickReport report = runner.RunFixedTicks(options.ticks);
    LUMINUMBRA_CORE_INFO("Headless server run complete: {} ticks ({:.2f}s simulated) in {:.2f}s "
                         "wall, {} autosave passes",
                         report.ticks_executed,
                         report.simulated_seconds,
                         report.wall_seconds,
                         report.autosave_passes);

    runner.Shutdown();
    return report.ticks_executed == options.ticks ? 0 : 1;
}

} // namespace

// ---------------------------------------------------------------------------
//  live replication smoke. Boots the authoritative server with N
// avatars, wires an in-process ReplicationServer + a loopback ReplicationClient,
// and each tick broadcasts the avatar states + pumps the client/acks. Asserts the
// client mirrors the server's avatars (within mm) and that an ack flowed back --
// the end-to-end server->client replication loop with the REAL runner (physics-
// settled avatar positions), in the gate harness. RENDER/transport-side only:
// world_hash is untouched (this reads the avatar list, never writes the sim).
// ---------------------------------------------------------------------------
int RunReplicate(const ServerCliOptions& options) {
    Luminumbra::Server::ServerWorldRunnerConfig config = RunnerConfigFrom(options);
    config.world_id.clear();
    config.world_name = "Replication Smoke";
    config.autosave_interval_ticks = 0;
    if (config.avatar_count <= 0) {
        config.avatar_count = 3; // replication needs avatars to replicate
    }

    LUMINUMBRA_CORE_INFO("Headless server REPLICATE smoke: preset={} seed={} ticks={} avatars={}",
                         options.preset,
                         options.seed,
                         options.ticks,
                         config.avatar_count);

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        LUMINUMBRA_CORE_ERROR("replicate: session failed to boot");
        return 1;
    }

    auto pair = Luminumbra::Net::MakeLoopbackPair();
    Luminumbra::Net::ReplicationServer server;
    server.AddClient(/*client_id=*/1, pair.first.get());
    Luminumbra::Net::ReplicationClient client(/*player_id=*/1, pair.second.get());
    //  polish: CHUNK-INDEX area of interest. Scope each client's snapshot to the
    // chunk neighbourhood around its own avatar (the grid the world streams on), the
    // scalable interest-management path for a 20+ player persistent server. Radius 3
    // chunks (48 m) comfortably covers the spawn-clustered avatars + NPCs here, so
    // the mirror stays complete while the bucketed-AOI path is exercised in the gate.
    server.SetAoiChunkRadius(/*chunk_radius=*/3, /*chunk_size_mm=*/Luminumbra::CHUNK_SIZE_X * 1000);

    // the loopback client CONTROLS one avatar -- it sends a constant +X
    // move usercmd each tick; the server applies it so that avatar walks. We then
    // assert the avatar actually moved (network input -> server movement loop).
    const std::uint32_t controlled = runner.Avatars().size() > 1 ? 1u : 0u;
    const float initial_x =
        runner.Avatars().empty() ? 0.0f : runner.Avatars()[controlled].position.x;

    // spawn server-side replicated NPC entities (type_id 1 = "animal") in the
    // GameSession registry, tagged ReplicatedComponent. They wander deterministically
    // each tick and replicate through BuildEntityReplStates alongside the avatars --
    // proving heterogeneous entities (NPCs/animals) flow through the same pipeline.
    auto& registry = runner.Session()->GetRegistry();
    auto* world_sys = runner.Session()->GetWorldSystem();
    auto* npc_physics = runner.Session()->GetPhysicsSystem();
    const Luminumbra::Vec3 npc_origin = runner.Session()->GetMetadata().spawnPoint;
    // each NPC is a real Jolt CharacterVirtual (the same physics body the
    // avatars use -- gravity + terrain collision).  polish: instead of a
    // meaningless circle-wander, each NPC is a GOAP agent that PLANS toward a water
    // opportunity and the InstinctLocomotionSystem steers it there (seek + arrival).
    //
    // The water "opportunity" is a positioned entity the planner ranks and the
    // locomotion executor steers toward. Placed a short walk from the NPC line so
    // the animals visibly converge on it. Engine-generic: the action string is
    // game data; the executor only follows the planned target's transform.
    entt::entity water_opportunity = entt::null;
    if (options.npcs > 0) {
        const float wx = npc_origin.x + 2.0f;
        const float wz = npc_origin.z + 14.0f; // ahead of the NPC line
        const float wy = world_sys ? world_sys->GetTerrainHeightAt(wx, wz) : npc_origin.y;
        water_opportunity = registry.create();
        auto& otf = registry.emplace<Luminumbra::Components::TransformComponent>(water_opportunity);
        otf.position = Luminumbra::Vec3(wx, wy, wz);
        auto& opp =
            registry.emplace<Luminumbra::Components::OpportunityComponent>(water_opportunity);
        opp.id = "water-hole";
        opp.action = "drink";
        opp.target = "water-hole";
        opp.need = "thirst";
        opp.satisfaction = 1.0f;
        opp.urgency = 1.0f;
        opp.radius = 0.0f; // unbounded: always a candidate
    }

    struct NpcRec {
        entt::entity e;
        std::size_t char_index;
        Luminumbra::Vec3 start;
    };
    std::vector<NpcRec> npc_recs;
    for (int i = 0; i < options.npcs; ++i) {
        const float bx = npc_origin.x + 6.0f + static_cast<float>(i) * 2.0f;
        const float bz = npc_origin.z - 4.0f;
        const float by = (world_sys ? world_sys->GetTerrainHeightAt(bx, bz) : npc_origin.y) + 1.5f;
        const std::size_t ci =
            npc_physics ? npc_physics->create_avatar_character(Luminumbra::Vec3(bx, by, bz)) : 0u;
        auto e = registry.create();
        auto& tf = registry.emplace<Luminumbra::Components::TransformComponent>(e);
        tf.position = Luminumbra::Vec3(bx, by, bz);
        auto& rep = registry.emplace<Luminumbra::Components::ReplicatedComponent>(e);
        rep.network_id = 1000u + static_cast<std::uint32_t>(i); // distinct from avatar ids
        rep.type_id = 1u; // "animal" archetype (client picks the mesh)
        // GOAP agent: needs drive planning; the planner (GameSession tick slot 2)
        // writes the winning action into ActionPlanComponent each replan.
        auto& agent = registry.emplace<Luminumbra::Components::InstinctAgentComponent>(e);
        agent.actor_id = "npc-" + std::to_string(i);
        agent.archetype = "animal";
        agent.replan_interval_ticks = 10;
        auto& needs = registry.emplace<Luminumbra::Components::NeedsComponent>(e);
        needs.needs = {Luminumbra::Components::Need{"thirst", 0.8f, 0.005f}};
        auto& loco = registry.emplace<Luminumbra::Components::LocomotionProfile>(e);
        loco.move_speed = 2.0f;
        loco.arrival_radius = 1.5f;
        loco.slow_radius = 4.0f;
        npc_recs.push_back({e, ci, Luminumbra::Vec3(bx, by, bz)});
    }

    //  one server-authoritative ballistic ARROW (type_id 2). Fired at tick 10,
    // integrated under gravity, despawned (reliable removed_id) on ground-hit/timeout.
    constexpr std::uint32_t kArrowNetId = 2000u;
    constexpr std::uint64_t kArrowFireTick = 10;
    auto* physics = runner.Session()->GetPhysicsSystem();
    entt::entity arrow_entity = entt::null;
    JPH::BodyID arrow_body; //  real Jolt dynamic body
    bool arrow_active = false;
    bool arrow_seen_by_client = false;
    bool arrow_despawn_signalled = false;

    std::uint64_t executed = 0;
    while (executed < options.ticks) {
        // Client -> server: full +X movement input for its avatar this tick.
        Luminumbra::Net::UsercmdMsg cmd;
        cmd.tick = executed + 1;
        cmd.player_id = controlled;
        cmd.move_x = 32767; // normalized +1.0
        cmd.move_z = 0;
        client.SendUsercmd(cmd);
        server.PumpInbound(); // receive the usercmd (newest-wins)
        if (const Luminumbra::Net::UsercmdMsg* got = server.LatestUsercmd(1)) {
            runner.SetAvatarMove(got->player_id,
                                 static_cast<float>(got->move_x) / 32767.0f,
                                 static_cast<float>(got->move_z) / 32767.0f);
        }
        //  polish: GOAP-driven NPC locomotion. The planner ran inside the
        // PREVIOUS RunFixedTicks (GameSession tick slot 2) and wrote each NPC's
        // ActionPlanComponent; the locomotion executor turns that plan into a wish
        // velocity toward the planned target (seek + arrival). One-tick coupling
        // (plan from  steers N) -- the same pattern the avatar usercmd uses --
        // and fully deterministic (pure function of registry state). Set the wish
        // BEFORE the step so update_avatars (in RunFixedTicks) walks the
        // CharacterVirtual with real gravity + terrain collision.
        if (npc_physics) {
            luminumbra::ai::RunInstinctLocomotionOnTick(registry);
            for (std::size_t i = 0; i < npc_recs.size(); ++i) {
                glm::vec2 wish(0.0f);
                if (const auto* intent =
                        registry.try_get<Luminumbra::Components::LocomotionIntentComponent>(
                            npc_recs[i].e)) {
                    wish = intent->wish_xz;
                }
                npc_physics->set_avatar_wish_velocity(npc_recs[i].char_index, wish);
            }
        }

        const auto step = runner.RunFixedTicks(1);
        executed += step.ticks_executed;
        if (step.ticks_executed == 0) {
            LUMINUMBRA_CORE_ERROR("replicate: tick {} did not advance", executed + 1);
            return 1;
        }
        // mirror each NPC's Jolt CharacterVirtual position (stepped above with
        // gravity + terrain collision) into its replicated transform.
        if (npc_physics) {
            for (const NpcRec& n : npc_recs) {
                if (!registry.valid(n.e))
                    continue;
                registry.get<Luminumbra::Components::TransformComponent>(n.e).position =
                    npc_physics->get_avatar_position(n.char_index);
            }
        }

        // arrow lifecycle with REAL JOLT PHYSICS. Fire once -> a dynamic
        // sphere body that arcs under gravity and COLLIDES with the terrain; its
        // body position drives the replicated transform; despawn (reliable removed_id)
        // when the body comes to rest (sleeps) or times out.
        std::vector<std::uint32_t> tick_removed;
        if (options.arrow && !arrow_active && arrow_entity == entt::null &&
            executed == kArrowFireTick && physics) {
            const Luminumbra::Vec3 from =
                runner.Avatars().empty() ? npc_origin : runner.Avatars()[controlled].position;
            const Luminumbra::Vec3 spawn_pos(from.x, from.y + 1.2f, from.z);
            arrow_body = physics->create_dynamic_sphere(
                spawn_pos, Luminumbra::Vec3(10.0f, 6.0f, 0.0f), 0.12f);
            arrow_entity = registry.create();
            auto& tf = registry.emplace<Luminumbra::Components::TransformComponent>(arrow_entity);
            tf.position = spawn_pos;
            auto& rep = registry.emplace<Luminumbra::Components::ReplicatedComponent>(arrow_entity);
            rep.network_id = kArrowNetId;
            rep.type_id = 2u; // "arrow"
            arrow_active = true;
        }
        if (arrow_active && physics && registry.valid(arrow_entity)) {
            // Jolt stepped the body in RunFixedTicks above; mirror its position.
            const Luminumbra::Vec3 apos = physics->get_body_position(arrow_body);
            registry.get<Luminumbra::Components::TransformComponent>(arrow_entity).position = apos;
            // Despawn when the body comes to REST on the terrain (Jolt slept it), or it
            // fell below the world (no collision under it), or it times out -- whichever
            // first. Reliable removed_id that snapshot either way.
            const bool rested =
                executed > kArrowFireTick + 5 && !physics->body_is_active(arrow_body);
            const bool fell_through = apos.y < npc_origin.y - 30.0f;
            const bool expired = executed > kArrowFireTick + 60; // 2 s @30 Hz
            if (rested || fell_through || expired) {
                physics->destroy_body(arrow_body);
                arrow_body = JPH::BodyID();
                registry.destroy(arrow_entity);
                arrow_entity = entt::null;
                arrow_active = false;
                tick_removed.push_back(kArrowNetId); // reliable despawn this snapshot
            }
        }

        // Snapshot = player avatars + registry-driven entities (NPCs + live arrow).
        std::vector<Luminumbra::Net::ReplEntityState> states =
            Luminumbra::World::BuildAvatarReplStates(runner.Avatars());
        const auto entity_states = Luminumbra::World::BuildEntityReplStates(registry);
        states.insert(states.end(), entity_states.begin(), entity_states.end());
        server.BroadcastSnapshot(executed, states, tick_removed);
        client.PumpInbound(); // apply snapshot (most-recent-wins) + ack
        server.PumpInbound(); // drain the ack

        //  observe: did the client see the typed arrow + its reliable despawn?
        if (client.has_snapshot()) {
            for (const auto& e : client.snapshot().entities) {
                if (e.type_id == 2u)
                    arrow_seen_by_client = true;
            }
            for (std::uint32_t rid : client.snapshot().removed_ids) {
                if (rid == kArrowNetId)
                    arrow_despawn_signalled = true;
            }
        }
    }

    // Verify the client mirrors the server's authoritative avatars.
    const auto& avatars = runner.Avatars();
    const float final_x = avatars.empty() ? 0.0f : avatars[controlled].position.x;
    // The controlled avatar walked +X under network input (>= ~0.5 m over the run).
    const bool moved = (final_x - initial_x) > 0.5f;
    // Client mirrors avatars + the N replicated NPCs.
    const std::size_t expected_entities = avatars.size() + static_cast<std::size_t>(options.npcs);
    bool size_ok = client.has_snapshot() && client.snapshot().entities.size() == expected_entities;
    // the NPCs replicated as typed entities (type_id 1).
    std::size_t npc_seen = 0;
    if (client.has_snapshot()) {
        for (const auto& e : client.snapshot().entities) {
            if (e.type_id == 1u)
                ++npc_seen;
        }
    }
    const bool npcs_ok = npc_seen == static_cast<std::size_t>(options.npcs);
    //  polish: verify the GOAP locomotion actually STEERED the NPCs -- every
    // NPC must have ended meaningfully CLOSER to the water hole it planned toward
    // (not just replicated). This is the behavioural assert for action->locomotion.
    bool npcs_approached_water = true;
    double min_npc_approach_m = 1e9;
    if (options.npcs > 0 && water_opportunity != entt::null && registry.valid(water_opportunity)) {
        const Luminumbra::Vec3 w =
            registry.get<Luminumbra::Components::TransformComponent>(water_opportunity).position;
        auto dist_xz = [&](const Luminumbra::Vec3& p) {
            const float dx = p.x - w.x, dz = p.z - w.z;
            return std::sqrt(dx * dx + dz * dz);
        };
        for (const NpcRec& n : npc_recs) {
            if (!registry.valid(n.e)) {
                npcs_approached_water = false;
                continue;
            }
            const Luminumbra::Vec3 end =
                registry.get<Luminumbra::Components::TransformComponent>(n.e).position;
            const double approached = static_cast<double>(dist_xz(n.start) - dist_xz(end));
            min_npc_approach_m = std::min(min_npc_approach_m, approached);
            if (approached < 2.0)
                npcs_approached_water = false; // >= 2 m closer
        }
    } else {
        min_npc_approach_m = 0.0;
    }
    //  if an arrow was fired, the client must have SEEN it (typed) in flight AND
    // received its reliable despawn.
    const bool arrow_ok = !options.arrow || (arrow_seen_by_client && arrow_despawn_signalled);
    double max_pos_err = 0.0;
    bool ids_ok = size_ok;
    if (size_ok) {
        for (std::size_t i = 0; i < avatars.size(); ++i) {
            const auto& e = client.snapshot().entities[i];
            if (e.entity_id != avatars[i].player_id)
                ids_ok = false;
            max_pos_err =
                std::max(max_pos_err,
                         static_cast<double>(std::abs(Luminumbra::Net::ReplDequantPos(e.px_mm) -
                                                      avatars[i].position.x)));
            max_pos_err =
                std::max(max_pos_err,
                         static_cast<double>(std::abs(Luminumbra::Net::ReplDequantPos(e.py_mm) -
                                                      avatars[i].position.y)));
            max_pos_err =
                std::max(max_pos_err,
                         static_cast<double>(std::abs(Luminumbra::Net::ReplDequantPos(e.pz_mm) -
                                                      avatars[i].position.z)));
        }
    }
    const bool acked = server.AckedSnapshotSeq(1) > 0;
    // bandwidth: MEASURED per-client snapshot bytes (vs the research estimate).
    // est kbps per client = bytes * a realistic 20 Hz snapshot rate * 8 / 1000.
    const std::size_t snapshot_bytes = server.last_broadcast_max_client_bytes();
    const double est_kbps_per_client = static_cast<double>(snapshot_bytes) * 20.0 * 8.0 / 1000.0;
    const bool passed = size_ok && ids_ok && npcs_ok && npcs_approached_water && arrow_ok &&
                        max_pos_err < 0.01 && acked && moved && executed == options.ticks;

    nlohmann::json artifact{
        {"schema", "luminumbra.replication_smoke.v1"},
        {"generated_by", "luminumbra_server_app --replicate ( )"},
        {"preset", options.preset},
        {"seed", options.seed},
        {"ticks", options.ticks},
        {"avatar_count", avatars.size()},
        {"client_has_snapshot", client.has_snapshot()},
        {"client_entity_count", client.has_snapshot() ? client.snapshot().entities.size() : 0u},
        {"final_snapshot_seq", client.has_snapshot() ? client.snapshot().snapshot_seq : 0u},
        {"acked_snapshot_seq", server.AckedSnapshotSeq(1)},
        {"max_position_error_m", max_pos_err},
        {"controlled_avatar", controlled},
        {"controlled_dx_m", final_x - initial_x},
        {"snapshot_bytes_per_client", snapshot_bytes},
        {"est_kbps_per_client_at_20hz", est_kbps_per_client},
        {"npc_count", options.npcs},
        {"npcs_replicated", npc_seen},
        {"npcs_ok", npcs_ok},
        {"npcs_approached_water", npcs_approached_water},
        {"min_npc_approach_m", min_npc_approach_m},
        {"arrow_fired", options.arrow},
        {"arrow_seen_by_client", arrow_seen_by_client},
        {"arrow_despawn_signalled", arrow_despawn_signalled},
        {"arrow_ok", arrow_ok},
        {"size_ok", size_ok},
        {"ids_ok", ids_ok},
        {"ack_flowed", acked},
        {"input_moved_avatar", moved},
        {"passed", passed},
    };

    const fs::path save_dir = runner.Session() ? runner.Session()->GetWorldSaveDir() : fs::path();
    runner.Shutdown();
    if (!save_dir.empty()) {
        std::error_code ec;
        fs::remove_all(save_dir, ec);
    }

    if (!options.artifact_path.empty()) {
        const fs::path artifact_path(options.artifact_path);
        std::error_code ec;
        if (artifact_path.has_parent_path())
            fs::create_directories(artifact_path.parent_path(), ec);
        std::ofstream out(artifact_path);
        if (out.is_open()) {
            out << artifact.dump(2) << "\n";
            LUMINUMBRA_CORE_INFO("Replicate artifact written: {}", options.artifact_path);
        }
    }

    if (!passed) {
        LUMINUMBRA_CORE_ERROR(
            "Replicate smoke FAILED: size_ok={} ids_ok={} npcs_ok={} npcs_approached_water={} (min "
            "{:.2f} m) "
            "arrow_ok={} max_pos_err={:.4f} acked={} moved={} (dx={:.2f}) ticks={}/{}",
            size_ok,
            ids_ok,
            npcs_ok,
            npcs_approached_water,
            min_npc_approach_m,
            arrow_ok,
            max_pos_err,
            acked,
            moved,
            final_x - initial_x,
            executed,
            options.ticks);
        return 1;
    }
    LUMINUMBRA_CORE_INFO("Replicate smoke passed: {} avatars mirrored to client (seq={}, "
                         "acked_seq={}, max_pos_err={:.4f} m); "
                         "network input walked avatar {} +{:.2f} m in X; {} GOAP NPCs approached "
                         "water (min {:.2f} m closer); "
                         "bandwidth {} B/snapshot/client (~{:.1f} kbps @20Hz)",
                         avatars.size(),
                         client.snapshot().snapshot_seq,
                         server.AckedSnapshotSeq(1),
                         max_pos_err,
                         controlled,
                         final_x - initial_x,
                         options.npcs,
                         min_npc_approach_m,
                         snapshot_bytes,
                         est_kbps_per_client);
    return 0;
}

// ---------------------------------------------------------------------------
// REAL networked multiplayer over actual TCP sockets (TcpTransport). Same
// authoritative-server replication stack as --replicate, but server and client
// run as SEPARATE PROCESSES over the wire instead of an in-process loopback.
// Proves the ILockstepTransport seam end-to-end off-loopback; optional GNS
// transport uses the same interface. Run: one process --net-host --port
// P, another --net-join --host H --port P.
// ---------------------------------------------------------------------------
struct RuntimeTcpClientSlot {
    std::uint32_t client_id = 0;
    std::uint16_t port = 0;
    std::unique_ptr<Luminumbra::Net::TcpTransport> transport;
    bool accepted = false;
    bool connected = false;
    std::uint64_t joined_tick = 0;
    std::uint64_t left_tick = 0;
    float initial_x = 0.0f;
    bool accepting = false;
    std::future<bool> accept_result;
};

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

#ifdef LUMINUMBRA_ENABLE_GNS
// ---------------------------------------------------------------------------
// the SAME authoritative-server replication over the STANDALONE
// GameNetworkingSockets transport (real UDP, no Steam). Unlike the Steam path,
// two processes CAN connect on one machine -- so this is the locally-testable
// real-UDP loop. Built only with -DLUMINUMBRA_ENABLE_GNS=ON.
// ---------------------------------------------------------------------------
int RunGnsHost(const ServerCliOptions& options) {
    if (!Luminumbra::Net::GnsLink::Init()) {
        LUMINUMBRA_CORE_ERROR("gns-host: GameNetworkingSockets init failed");
        return 1;
    }
    const std::uint32_t expected_clients = ExpectedNetworkClients(options);
    std::uint16_t last_accept_port = 0;
    if (!ResolveNetworkClientPort(options.port, expected_clients, last_accept_port)) {
        LUMINUMBRA_CORE_ERROR(
            "gns-host: cannot map {} client(s) from base port {}", expected_clients, options.port);
        Luminumbra::Net::GnsLink::Shutdown();
        return 2;
    }
    Luminumbra::Server::ServerWorldRunnerConfig config = RunnerConfigFrom(options);
    config.world_id.clear();
    config.world_name = "GNS Host";
    config.autosave_interval_ticks = 0;
    const int required_avatars = static_cast<int>(expected_clients) + 1;
    if (config.avatar_count < required_avatars)
        config.avatar_count = required_avatars;
    LUMINUMBRA_CORE_INFO(
        "GNS HOST: preset={} seed={} avatars={} ticks={} clients={} -- accepting UDP ports {}..{}",
        options.preset,
        options.seed,
        config.avatar_count,
        options.ticks,
        expected_clients,
        options.port,
        last_accept_port);

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        LUMINUMBRA_CORE_ERROR("gns-host: boot failed");
        Luminumbra::Net::GnsLink::Shutdown();
        return 1;
    }

    Luminumbra::Net::ReplicationServer server;
    std::vector<std::unique_ptr<Luminumbra::Net::GnsTransport>> transports;
    std::vector<std::uint32_t> client_ids;
    transports.reserve(expected_clients);
    client_ids.reserve(expected_clients);
    for (std::uint32_t client_id = 1; client_id <= expected_clients; ++client_id) {
        std::uint16_t client_port = 0;
        if (!ResolveNetworkClientPort(options.port, client_id, client_port)) {
            LUMINUMBRA_CORE_ERROR(
                "gns-host: cannot map client {} from base port {}", client_id, options.port);
            for (auto& accepted : transports) {
                accepted->Close();
            }
            Luminumbra::Net::GnsLink::Shutdown();
            return 2;
        }
        auto transport = std::make_unique<Luminumbra::Net::GnsTransport>();
        if (!transport->Listen(client_port)) {
            LUMINUMBRA_CORE_ERROR("gns-host: CreateListenSocketIP failed for client {} on port {}",
                                  client_id,
                                  client_port);
            for (auto& accepted : transports) {
                accepted->Close();
            }
            Luminumbra::Net::GnsLink::Shutdown();
            return 1;
        }
        LUMINUMBRA_CORE_INFO("gns-host: listening for client {} over UDP on port {} (30s)...",
                             client_id,
                             client_port);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!transport->IsPeerConnected() && std::chrono::steady_clock::now() < deadline) {
            Luminumbra::Net::GnsLink::RunCallbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!transport->IsPeerConnected()) {
            LUMINUMBRA_CORE_ERROR("gns-host: client {} did not connect within timeout", client_id);
            transport->Close();
            for (auto& accepted : transports) {
                accepted->Close();
            }
            Luminumbra::Net::GnsLink::Shutdown();
            return 1;
        }
        server.AddClient(client_id, transport.get());
        client_ids.push_back(client_id);
        transports.push_back(std::move(transport));
    }
    LUMINUMBRA_CORE_INFO("gns-host: {} client(s) connected over UDP.", transports.size());

    server.SetAoiChunkRadius(3, Luminumbra::CHUNK_SIZE_X * 1000);
    std::vector<float> initial_x_by_client(expected_clients + 1u, 0.0f);
    for (const std::uint32_t client_id : client_ids) {
        if (client_id < runner.Avatars().size()) {
            initial_x_by_client[client_id] = runner.Avatars()[client_id].position.x;
        }
    }

    std::uint64_t executed = 0;
    bool all_clients_connected = true;
    while (executed < options.ticks) {
        Luminumbra::Net::GnsLink::RunCallbacks();
        server.PumpInbound();
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
            LUMINUMBRA_CORE_ERROR("gns-host: tick stalled");
            for (auto& transport : transports) {
                transport->Close();
            }
            Luminumbra::Net::GnsLink::Shutdown();
            return 1;
        }
        const auto states = Luminumbra::World::BuildAvatarReplStates(runner.Avatars());
        server.BroadcastSnapshot(executed, states);
        std::size_t connected_count = 0;
        for (const auto& transport : transports) {
            if (transport->IsPeerConnected()) {
                ++connected_count;
            }
        }
        if (connected_count != transports.size()) {
            LUMINUMBRA_CORE_WARN("gns-host: {}/{} client(s) still connected at tick {}",
                                 connected_count,
                                 transports.size(),
                                 executed);
            all_clients_connected = false;
            break;
        }
    }
    for (const std::uint32_t client_id : client_ids) {
        const float final_x =
            client_id < runner.Avatars().size() ? runner.Avatars()[client_id].position.x : 0.0f;
        LUMINUMBRA_CORE_INFO("gns-host: client {} acked seq {}; avatar {} moved {:.2f} m in X.",
                             client_id,
                             server.AckedSnapshotSeq(client_id),
                             client_id,
                             final_x - initial_x_by_client[client_id]);
    }
    LUMINUMBRA_CORE_INFO("gns-host: ran {} ticks over UDP, {} avatars, {} client(s).",
                         executed,
                         runner.Avatars().size(),
                         transports.size());
    for (int i = 0; i < 50; ++i) {
        Luminumbra::Net::GnsLink::RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (auto& transport : transports) {
        transport->Close();
    }
    Luminumbra::Net::GnsLink::Shutdown();
    return (executed == options.ticks && all_clients_connected) ? 0 : 1;
}

int RunGnsJoin(const ServerCliOptions& options) {
    if (!Luminumbra::Net::GnsLink::Init()) {
        LUMINUMBRA_CORE_ERROR("gns-join: GameNetworkingSockets init failed");
        return 1;
    }
    const std::uint32_t player_id = LocalNetworkPlayerId(options);
    std::uint16_t connect_port = 0;
    if (!ResolveNetworkClientPort(options.port, player_id, connect_port)) {
        LUMINUMBRA_CORE_ERROR(
            "gns-join: cannot map player id {} from base port {}", player_id, options.port);
        Luminumbra::Net::GnsLink::Shutdown();
        return 2;
    }
    LUMINUMBRA_CORE_INFO("GNS JOIN: player {} connecting to {}:{} over UDP...",
                         player_id,
                         options.host,
                         connect_port);
    Luminumbra::Net::GnsTransport transport;
    if (!transport.Connect(options.host, connect_port)) {
        LUMINUMBRA_CORE_ERROR("gns-join: ConnectByIPAddress failed");
        Luminumbra::Net::GnsLink::Shutdown();
        return 1;
    }
    const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!transport.IsPeerConnected() && std::chrono::steady_clock::now() < connect_deadline) {
        Luminumbra::Net::GnsLink::RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!transport.IsPeerConnected()) {
        LUMINUMBRA_CORE_ERROR("gns-join: connect timed out");
        transport.Close();
        Luminumbra::Net::GnsLink::Shutdown();
        return 1;
    }
    LUMINUMBRA_CORE_INFO("gns-join: player {} connected over UDP.", player_id);

    Luminumbra::Net::ReplicationClient client(player_id, &transport);
    std::uint32_t last_seq = 0;
    std::size_t max_entities = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        Luminumbra::Net::GnsLink::RunCallbacks();
        Luminumbra::Net::UsercmdMsg cmd;
        cmd.tick = last_seq + 1;
        cmd.player_id = player_id;
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
    const bool ok =
        client.has_snapshot() && max_entities > static_cast<std::size_t>(player_id) && last_seq > 0;
    if (!ok) {
        LUMINUMBRA_CORE_ERROR("gns-join: player {} did not mirror the host (has_snapshot={} "
                              "max_entities={} last_seq={})",
                              player_id,
                              client.has_snapshot(),
                              max_entities,
                              last_seq);
        transport.Close();
        Luminumbra::Net::GnsLink::Shutdown();
        return 1;
    }
    float mirror_x = 0.0f;
    for (const auto& e : client.snapshot().entities) {
        if (e.entity_id == player_id)
            mirror_x = Luminumbra::Net::ReplDequantPos(e.px_mm);
    }
    LUMINUMBRA_CORE_INFO(
        "gns-join: player {} mirrored host over UDP -- last seq {}, up to {} entities; controlled "
        "avatar at x={:.2f} m. Real UDP replication confirmed.",
        player_id,
        last_seq,
        max_entities,
        mirror_x);
    transport.Close();
    Luminumbra::Net::GnsLink::Shutdown();
    return 0;
}
#endif // LUMINUMBRA_ENABLE_GNS

int main(int argc, char* argv[]) {
    Log::Init();
    LUMINUMBRA_CORE_INFO("Luminumbra headless server");

    ServerCliOptions options = ParseOptions(argc, argv);
    if (options.parse_error) {
        LUMINUMBRA_CORE_ERROR(
            "Usage: luminumbra_server_app [--smoke] [--heavy [--heavy-resim <n>]] "
            "[--record <path>] [--replay <path>] [--mutate-replay-fixture <path>] "
            "[--lockstep-loopback [--lockstep-delay-input <n>] [--lockstep-corrupt-tick <t>] "
            "[--lockstep-dump <path>]] "
            "[--root <path>] [--preset <name>] "
            "[--seed <seed>] [--world-id <id>] [--ticks <n>] [--radius <chunks>] "
            "[--collision-radius <chunks>] [--autosave-ticks <n>] [--clients <n>] "
            "[--player-id <id>] [--server-mode] [--artifact <path>]");
        return 2;
    }

    if (options.root.empty()) {
        options.root = RootString(ResolveServerRoot(argc > 0 ? argv[0] : nullptr));
    } else {
        options.root = RootString(fs::path(options.root));
    }
    LUMINUMBRA_CORE_INFO("Server runtime root: {}", options.root);

    if (!options.mutate_replay_fixture.empty()) {
        return RunMutateReplayFixture(options);
    }
    if (options.lockstep_loopback) {
        return RunLockstepLoopback(options);
    }
    if (!options.replay_path.empty()) {
        return RunReplay(options);
    }
    if (!options.record_path.empty()) {
        return RunRecord(options);
    }
    if (options.heavy) {
        return RunHeavy(options);
    }
    if (options.weather_bench) {
        return RunWeatherBench(options);
    }
    if (options.aether_bench) {
        return RunAetherBench(options);
    }
    if (options.wind_bench) {
        return RunWindBench(options);
    }
    if (options.net_soak) {
        return RunNetSoak(options);
    }
    if (options.net_soak_client) {
        return RunNetSoakClient(options);
    }
    if (options.net_host) {
        if (options.udp) {
#ifdef LUMINUMBRA_ENABLE_GNS
            return RunGnsHost(options);
#else
            LUMINUMBRA_CORE_ERROR(
                "--udp requires a build configured with -DLUMINUMBRA_ENABLE_GNS=ON");
            return 2;
#endif
        }
        if (options.steam) {
#ifdef LUMINUMBRA_ENABLE_STEAM
            return RunSteamHost(options);
#else
            LUMINUMBRA_CORE_ERROR(
                "--steam requires a build configured with -DLUMINUMBRA_ENABLE_STEAM=ON");
            return 2;
#endif
        }
        return RunNetHost(options);
    }
    if (options.net_join) {
        if (options.udp) {
#ifdef LUMINUMBRA_ENABLE_GNS
            return RunGnsJoin(options);
#else
            LUMINUMBRA_CORE_ERROR(
                "--udp requires a build configured with -DLUMINUMBRA_ENABLE_GNS=ON");
            return 2;
#endif
        }
        if (options.steam) {
#ifdef LUMINUMBRA_ENABLE_STEAM
            return RunSteamJoin(options);
#else
            LUMINUMBRA_CORE_ERROR(
                "--steam requires a build configured with -DLUMINUMBRA_ENABLE_STEAM=ON");
            return 2;
#endif
        }
        return RunNetJoin(options);
    }
    if (options.replicate) {
        return RunReplicate(options);
    }
    return options.smoke ? RunSmoke(options) : RunServer(options);
}
