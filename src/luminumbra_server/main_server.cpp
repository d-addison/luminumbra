// Headless server entry point (T-I3-12 / T-I3-13). Simulation authority
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
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "nlohmann/json.hpp"

#include "ServerWorldRunner.h"
#include "luminumbra_common/core/Log.h"

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
    bool smoke = false;
    std::string artifact_path;
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
        if (std::strcmp(arg, "--smoke") == 0) {
            options.smoke = true;
        } else if (std::strcmp(arg, "--root") == 0) {
            if (const char* v = next_value(i)) options.root = v;
        } else if (std::strcmp(arg, "--preset") == 0) {
            if (const char* v = next_value(i)) options.preset = v;
        } else if (std::strcmp(arg, "--seed") == 0) {
            if (const char* v = next_value(i)) options.seed = v;
        } else if (std::strcmp(arg, "--world-id") == 0) {
            if (const char* v = next_value(i)) options.world_id = v;
        } else if (std::strcmp(arg, "--ticks") == 0) {
            if (const char* v = next_value(i)) options.ticks = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(arg, "--radius") == 0) {
            if (const char* v = next_value(i)) options.surface_radius = std::atoi(v);
        } else if (std::strcmp(arg, "--collision-radius") == 0) {
            if (const char* v = next_value(i)) options.collision_radius = std::atoi(v);
        } else if (std::strcmp(arg, "--autosave-ticks") == 0) {
            if (const char* v = next_value(i)) options.autosave_ticks = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(arg, "--artifact") == 0) {
            if (const char* v = next_value(i)) options.artifact_path = v;
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
    return config;
}

struct SmokeRunResult {
    bool ok = false;
    std::string world_hash;
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

    result.ticks = runner.RunFixedTicks(options.ticks);
    result.world_hash = runner.ComputeWorldHash();
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
        run_label, result.world_hash, result.ticks.ticks_executed,
        result.chunks_streamed, result.ticks.wall_seconds);
    return result;
}

nlohmann::json SmokeRunJson(const SmokeRunResult& run) {
    return nlohmann::json{
        {"ok", run.ok},
        {"world_hash", run.world_hash},
        {"world_id", run.world_id},
        {"ticks_executed", run.ticks.ticks_executed},
        {"frames_executed", run.ticks.frames_executed},
        {"chunks_streamed", run.chunks_streamed},
        {"simulated_seconds", run.ticks.simulated_seconds},
        {"wall_seconds", run.ticks.wall_seconds},
        {"autosave_passes", run.ticks.autosave_passes},
        {"autosave_writes", run.ticks.autosave_writes},
        {"shutdown_save", {
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
        options.preset, options.seed, options.ticks,
        options.surface_radius, options.collision_radius);

    const SmokeRunResult first = RunSmokeOnce(options, "run-1");
    const SmokeRunResult replay = RunSmokeOnce(options, "run-2");

    const bool deterministic = first.ok && replay.ok && first.world_hash == replay.world_hash;
    const bool passed = deterministic &&
        first.ticks.ticks_executed == options.ticks &&
        replay.ticks.ticks_executed == options.ticks &&
        first.chunks_streamed > 0;

    nlohmann::json artifact{
        {"schema", kServerTickArtifactSchema},
        {"generated_by", "luminumbra_server_app --smoke (T-I3-13)"},
        {"preset", options.preset},
        {"seed", options.seed},
        {"tick_rate_hz", 30.0},
        {"ticks_requested", options.ticks},
        {"surface_radius", options.surface_radius},
        {"collision_radius", options.collision_radius},
        {"runs", nlohmann::json::array({SmokeRunJson(first), SmokeRunJson(replay)})},
        {"world_hash", first.world_hash},
        {"world_hash_replay", replay.world_hash},
        {"deterministic", deterministic},
        {"passed", passed},
    };

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
            first.world_hash, replay.world_hash,
            first.ticks.ticks_executed, replay.ticks.ticks_executed);
        return 1;
    }

    LUMINUMBRA_CORE_INFO(
        "Headless server smoke passed: world_hash == world_hash_replay ({}), {} ticks per run",
        first.world_hash, options.ticks);
    return 0;
}

int RunServer(const ServerCliOptions& options) {
    Luminumbra::Server::ServerWorldRunner runner(RunnerConfigFrom(options));
    if (!runner.Boot()) {
        return 1;
    }

    const Luminumbra::Server::ServerTickReport report = runner.RunFixedTicks(options.ticks);
    LUMINUMBRA_CORE_INFO(
        "Headless server run complete: {} ticks ({:.2f}s simulated) in {:.2f}s wall, {} autosave passes",
        report.ticks_executed, report.simulated_seconds, report.wall_seconds, report.autosave_passes);

    runner.Shutdown();
    return report.ticks_executed == options.ticks ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    Log::Init();
    LUMINUMBRA_CORE_INFO("Luminumbra headless server");

    ServerCliOptions options = ParseOptions(argc, argv);
    if (options.parse_error) {
        LUMINUMBRA_CORE_ERROR(
            "Usage: luminumbra_server_app [--smoke] [--root <path>] [--preset <name>] "
            "[--seed <seed>] [--world-id <id>] [--ticks <n>] [--radius <chunks>] "
            "[--collision-radius <chunks>] [--autosave-ticks <n>] [--artifact <path>]");
        return 2;
    }

    if (options.root.empty()) {
        options.root = RootString(ResolveServerRoot(argc > 0 ? argv[0] : nullptr));
    } else {
        options.root = RootString(fs::path(options.root));
    }
    LUMINUMBRA_CORE_INFO("Server runtime root: {}", options.root);

    return options.smoke ? RunSmoke(options) : RunServer(options);
}
