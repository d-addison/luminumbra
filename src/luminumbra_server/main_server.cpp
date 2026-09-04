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

#include "ServerCliOptions.h"
#include "modes/Modes.h"

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
        } else if (std::strcmp(arg, "--water-smoke") == 0) {
            // water-heavy measurement workload: wet-anchor walk + water sub-phase
            // timings/counters in the artifact (perf lane; observability only).
            options.smoke = true;
            options.water_smoke = true;
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

int main(int argc, char* argv[]) {
    Log::Init();
    LUMINUMBRA_CORE_INFO("Luminumbra headless server");

    ServerCliOptions options = ParseOptions(argc, argv);
    if (options.parse_error) {
        LUMINUMBRA_CORE_ERROR(
            "Usage: luminumbra_server_app [--smoke] [--water-smoke] "
            "[--heavy [--heavy-resim <n>]] "
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
