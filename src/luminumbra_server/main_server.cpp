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
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "ServerWorldRunner.h"
#include "core/EngineVersion.h"
#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/world/GameSession.h"

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
    // T-I6 P1 (multiplayer): spawn N deterministic player avatars (phyllotaxis ring
    // around spawn). 0 = none (byte-identical to the pre-P1 lane). With --smoke the
    // double-run already asserts the entities sub-hash matches, so --smoke --avatars N
    // validates avatar determinism through the existing gate path.
    int avatars = 0;
    bool smoke = false;
    // T-I5a-2 (A2): WindFieldDeterminism gate. Boots a world, runs N ticks
    // twice, asserts the wind sub-hash is equal across runs + stable, and times
    // the per-tick wind update (budget <= 0.15 ms at the streamed extent).
    bool wind_bench = false;
    // T-I5a-3 (B1): WeatherVisual determinism. Boots the weather core (advected by
    // the wind field), runs N ticks twice, asserts the weather sub-hash is equal
    // across runs + stable + evolves + bounded storm cells, and times the per-tick
    // weather update (budget <= 0.20 ms at the streamed extent).
    bool weather_bench = false;
    // T-I6-A1: AetherFieldDeterminism. Boots the aether field (advected by the
    // wind field), runs N ticks twice, asserts the aether sub-hash is equal
    // across runs + stable + evolves, and times the per-tick update.
    bool aether_bench = false;
    // T-I4-11 heavy-mode oracle: tick N, save, load into a fresh session,
    // resimulate heavy_resim ticks on BOTH, compare full + sub hashes.
    bool heavy = false;
    std::uint64_t heavy_resim = 30;
    // T-I4-12 session replay (LREC1):
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
    // T-I4-13 lockstep transport. --lockstep-loopback drives BOTH peers in-process over
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
            if (const char* v = next_value(i)) options.heavy_resim = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(arg, "--record") == 0) {
            if (const char* v = next_value(i)) options.record_path = v;
        } else if (std::strcmp(arg, "--replay") == 0) {
            if (const char* v = next_value(i)) options.replay_path = v;
        } else if (std::strcmp(arg, "--mutate-replay-fixture") == 0) {
            if (const char* v = next_value(i)) options.mutate_replay_fixture = v;
        } else if (std::strcmp(arg, "--lockstep-loopback") == 0) {
            options.lockstep_loopback = true;
        } else if (std::strcmp(arg, "--lockstep-delay-input") == 0) {
            if (const char* v = next_value(i)) options.lockstep_delay_input = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(arg, "--lockstep-corrupt-tick") == 0) {
            if (const char* v = next_value(i)) options.lockstep_corrupt_tick = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(arg, "--lockstep-dump") == 0) {
            if (const char* v = next_value(i)) options.lockstep_dump_path = v;
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
        } else if (std::strcmp(arg, "--avatars") == 0) {
            if (const char* v = next_value(i)) options.avatars = std::atoi(v);
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
    config.avatar_count = options.avatars;
    return config;
}

struct SmokeRunResult {
    bool ok = false;
    std::string world_hash;
    // T-I4-11: per-system sub-hashes (additive; top-level world_hash unchanged).
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub_hashes;
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
    // T-I6 P2: avatar physics telemetry — confirm the server-authoritative avatar
    // characters SETTLED on the terrain (grounded; not fallen through the world).
    if (!runner.Avatars().empty()) {
        auto* phys = runner.Session() ? runner.Session()->GetPhysicsSystem() : nullptr;
        int grounded = 0;
        for (std::size_t i = 0; i < runner.Avatars().size(); ++i) {
            if (phys && phys->is_avatar_grounded(i)) ++grounded;
        }
        const auto& a0 = runner.Avatars().front();
        LUMINUMBRA_CORE_INFO("Smoke {}: avatars={} grounded={} (avatar0 y={:.2f})",
            run_label, runner.Avatars().size(), grounded, a0.position.y);
    }
    result.world_hash = runner.ComputeWorldHash();
    result.sub_hashes = runner.ComputeWorldSubHashes();
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
        {"sub_hashes", {
            {"terrain", run.sub_hashes.terrain},
            {"mesh", run.sub_hashes.mesh},
            {"water", run.sub_hashes.water},
            {"entities", run.sub_hashes.entities},
            {"wind", run.sub_hashes.wind},
            {"aether", run.sub_hashes.aether},
        }},
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

    // T-I4-11: per-system sub-hashes must also match between run and replay; a
    // mismatch in any one localizes the divergence to that subsystem.
    const bool sub_hashes_match =
        first.sub_hashes.terrain == replay.sub_hashes.terrain &&
        first.sub_hashes.mesh == replay.sub_hashes.mesh &&
        first.sub_hashes.water == replay.sub_hashes.water &&
        first.sub_hashes.entities == replay.sub_hashes.entities &&
        first.sub_hashes.wind == replay.sub_hashes.wind &&
        first.sub_hashes.aether == replay.sub_hashes.aether;

    const bool deterministic = first.ok && replay.ok &&
        first.world_hash == replay.world_hash && sub_hashes_match;
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
        {"sub_hashes", {
            {"terrain", first.sub_hashes.terrain},
            {"mesh", first.sub_hashes.mesh},
            {"water", first.sub_hashes.water},
            {"entities", first.sub_hashes.entities},
            {"wind", first.sub_hashes.wind},
            {"aether", first.sub_hashes.aether},
        }},
        {"sub_hashes_replay", {
            {"terrain", replay.sub_hashes.terrain},
            {"mesh", replay.sub_hashes.mesh},
            {"water", replay.sub_hashes.water},
            {"entities", replay.sub_hashes.entities},
            {"wind", replay.sub_hashes.wind},
            {"aether", replay.sub_hashes.aether},
        }},
        {"sub_hashes_match", sub_hashes_match},
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

// ---------------------------------------------------------------------------
// T-I5a-2 (A2) WindFieldDeterminism gate driver. Two independent runs of N
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
        seed, ticks, Luminumbra::Systems::kWindExtentCells);

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
    // debug-preset gate falsely fail a RELEASE-build budget (design S7).
    const bool passed = deterministic && evolves;

    nlohmann::json artifact{
        {"schema", "luminumbra.wind_field_determinism.v1"},
        {"generated_by", "luminumbra_server_app --wind-bench (T-I5a-2)"},
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
        LUMINUMBRA_CORE_ERROR(
            "Wind-bench FAILED (determinism): deterministic={} evolves={} "
            "(wind_hash={} replay={})",
            deterministic, evolves, hash_run1, hash_run2);
        return 1;
    }

    LUMINUMBRA_CORE_INFO(
        "Wind-bench passed: wind_sub_hash={} stable across runs, field evolves; "
        "per_tick_update={:.4f} ms (budget {:.4f} ms, within_budget={}; budget "
        "enforced by the gate on the release build)",
        hash_run1, per_tick_ms, kBudgetMs, within_budget);
    return 0;
}

// ---------------------------------------------------------------------------
// T-I6-A1 AetherFieldDeterminism driver. Ticks a wind field + the Aetheric
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
        "Headless server AETHER-BENCH: seed={} ticks={} (24 m cells x {} extent x {} diffuse sweeps)",
        seed, ticks, Luminumbra::Systems::kAetherExtentCells,
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
        {"generated_by", "luminumbra_server_app --aether-bench (T-I6-A1)"},
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
            LUMINUMBRA_CORE_ERROR("Failed to write aether-bench artifact: {}", options.artifact_path);
            return 1;
        }
    }

    if (!passed) {
        LUMINUMBRA_CORE_ERROR(
            "Aether-bench FAILED (determinism): deterministic={} evolves={} "
            "(aether_hash={} replay={})",
            deterministic, evolves, hash_run1, hash_run2);
        return 1;
    }

    LUMINUMBRA_CORE_INFO(
        "Aether-bench passed: aether_sub_hash={} stable across runs, field evolves; "
        "per_tick_update={:.4f} ms (budget {:.4f} ms, within_budget={}; budget "
        "enforced by the gate on the release build)",
        hash_run1, per_tick_ms, kBudgetMs, within_budget);
    return 0;
}

// ---------------------------------------------------------------------------
// T-I5a-3 (B1) WeatherVisual determinism driver. Two independent runs of N
// WeatherSystem updates (advected by a parallel wind field) with the same
// seed/anchor must reach the IDENTICAL weather sub-hash (the bit-determinism the
// world_hash `weather` slot + the WeatherVisual state-hash assertion depend on),
// the state must EVOLVE (tick 0 != tick N -- gate is not vacuous), storm cells
// must stay BOUNDED (<= kMaxStormCells, F9), and the per-tick weather update
// cost is measured against the PINNED <= 0.20 ms budget at the streamed extent.
// The weather core is exercised in isolation (no chunk streaming) so the timing
// is the weather update ALONE (plus the wind advection sample it requires).
// ---------------------------------------------------------------------------
struct WeatherBenchResult {
    std::string sub_hash;
    int max_storm_cells = 0;
    // T-I5a-5 (B3): lightning strike telemetry. total_strikes counts every strike
    // event scheduled over the run (the seed+13 schedule is non-vacuous when > 0);
    // max_live_strikes is the peak schedule-window size (bounded <= kMaxLiveStrikes).
    std::uint64_t total_strikes = 0;
    int max_live_strikes = 0;
};

WeatherBenchResult RunWeatherUpdatesAndHash(int seed, std::uint64_t ticks, const Luminumbra::Vec3& anchor) {
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
    // several storm cells (the dedicated weather scenario, premise guard F4). 300
    // ticks (10 s at 30 Hz) is the Endurance300Storm horizon.
    const std::uint64_t ticks = options.ticks > 0 ? options.ticks : 300;
    const Luminumbra::Vec3 anchor(8.0f, 100.0f, 8.0f);

    LUMINUMBRA_CORE_INFO(
        "Headless server WEATHER-BENCH: seed={} ticks={} (24 m cells x {} extent, "
        "storm-cell cap {})",
        seed, ticks, Luminumbra::Systems::kWeatherExtentCells,
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

    // Bounded state (F9): the storm-cell count never exceeds the cap.
    const bool bounded = run1.max_storm_cells <= Luminumbra::Systems::kMaxStormCells &&
                         run2.max_storm_cells <= Luminumbra::Systems::kMaxStormCells;
    // Non-vacuity of the storm path: at least one storm cell spawned over the run
    // (so the gate actually exercised advection + the precip field).
    const bool storms_spawned = run1.max_storm_cells > 0;
    // T-I5a-5 (B3): non-vacuity of the LIGHTNING path -- at least one strike was
    // scheduled (proves the seed+13 schedule fired, exercising the strike sub-hash),
    // and the live strike window stayed BOUNDED (<= kMaxLiveStrikes, F9). Strike
    // counts must MATCH across the two runs (the schedule is deterministic).
    const bool strikes_scheduled = run1.total_strikes > 0;
    const bool strikes_deterministic = run1.total_strikes == run2.total_strikes;
    const bool strikes_bounded =
        run1.max_live_strikes <= Luminumbra::Systems::kMaxLiveStrikes &&
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
        {"generated_by", "luminumbra_server_app --weather-bench (T-I5a-3)"},
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
            LUMINUMBRA_CORE_ERROR("Failed to write weather-bench artifact: {}", options.artifact_path);
            return 1;
        }
    }

    if (!passed) {
        LUMINUMBRA_CORE_ERROR(
            "Weather-bench FAILED: deterministic={} evolves={} bounded={} storms_spawned={} "
            "(weather_hash={} replay={} max_storm_cells={})",
            deterministic, evolves, bounded, storms_spawned,
            run1.sub_hash, run2.sub_hash, run1.max_storm_cells);
        return 1;
    }

    LUMINUMBRA_CORE_INFO(
        "Weather-bench passed: weather_sub_hash={} stable across runs, state evolves; "
        "max_storm_cells={} (cap {}); per_tick_update={:.4f} ms (budget {:.4f} ms, "
        "within_budget={}; budget enforced by the gate on the release build)",
        run1.sub_hash, run1.max_storm_cells, Luminumbra::Systems::kMaxStormCells,
        per_tick_ms, kBudgetMs, within_budget);
    return 0;
}

// ---------------------------------------------------------------------------
// T-I4-11 heavy-mode oracle (Factorio "heavy mode", research Area 2 takeaway 5):
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
};

HeavyHashes CaptureHashes(Luminumbra::Server::ServerWorldRunner& runner) {
    HeavyHashes h;
    h.world_hash = runner.ComputeWorldHash();
    h.sub = runner.ComputeWorldSubHashes();
    return h;
}

// T-I4-11: the heavy oracle's equality is over AUTHORITATIVE SIMULATION STATE
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
    // T-I5a-2 (A2): the heavy oracle compares two sessions at DIFFERENT tick
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
    // T-I5a-3 (B1): WEATHER is excluded for the IDENTICAL reason as wind. The
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
    return a.sub.terrain == b.sub.terrain &&
        a.sub.water == b.sub.water &&
        a.sub.entities == b.sub.entities;
}

bool MeshEqual(const HeavyHashes& a, const HeavyHashes& b) {
    return a.sub.mesh == b.sub.mesh;
}

nlohmann::json HeavyHashJson(const HeavyHashes& h) {
    return nlohmann::json{
        {"world_hash", h.world_hash},
        {"sub_hashes", {
            {"terrain", h.sub.terrain},
            {"mesh", h.sub.mesh},
            {"water", h.sub.water},
            {"entities", h.sub.entities},
            {"wind", h.sub.wind},
            {"weather", h.sub.weather},
            {"aether", h.sub.aether},
        }},
    };
}

int RunHeavy(const ServerCliOptions& options) {
    LUMINUMBRA_CORE_INFO(
        "Headless server HEAVY oracle: preset={} seed={} ticks={} resim={} radius={}/{}",
        options.preset, options.seed, options.ticks, options.heavy_resim,
        options.surface_radius, options.collision_radius);

    // --- Phase 1: boot the ORIGINAL session, tick N, SAVE its state. ---
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
    if (saved_chunks == 0 || world_id.empty()) {
        LUMINUMBRA_CORE_ERROR("heavy: SaveFullSnapshot wrote no chunks (world_id='{}')", world_id);
        return 1;
    }
    const HeavyHashes orig_at_save = CaptureHashes(original);

    // --- Phase 2: LOAD a FRESH session from the saved world_id. ---
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

    // The loaded session, immediately after load, must match the original at
    // save on AUTHORITATIVE sim state (terrain/water/entities). Mesh is tracked
    // informationally (see AuthoritativeStateEqual rationale).
    const bool roundtrip_ok = AuthoritativeStateEqual(orig_at_save, loaded_at_load);
    const bool roundtrip_mesh_match = MeshEqual(orig_at_save, loaded_at_load);

    // --- Phase 3: resimulate M further ticks on BOTH sessions. ---
    const auto orig_resim = original.RunFixedTicks(options.heavy_resim);
    const auto loaded_resim = loaded.RunFixedTicks(options.heavy_resim);
    const HeavyHashes orig_final = CaptureHashes(original);
    const HeavyHashes loaded_final = CaptureHashes(loaded);

    const bool resim_ok = AuthoritativeStateEqual(orig_final, loaded_final);
    const bool resim_mesh_match = MeshEqual(orig_final, loaded_final);
    const bool passed = roundtrip_ok && resim_ok &&
        pre_save_ticks.ticks_executed == options.ticks &&
        orig_resim.ticks_executed == options.heavy_resim &&
        loaded_resim.ticks_executed == options.heavy_resim;

    nlohmann::json artifact{
        {"schema", "luminumbra.server_tick_heavy.v1"},
        {"generated_by", "luminumbra_server_app --heavy (T-I4-11)"},
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
        {"authoritative_sections", nlohmann::json::array({"terrain", "water", "entities"})},
        {"mesh_excluded_reason", "surface mesh is a deterministically-regenerated derived render artifact; async re-meshing across a save/load boundary reaches identical geometry via a different in-memory pending-mesh snapshot. Authoritative sim state (terrain/water/entities) round-trips exactly."},
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
            "orig_save={} loaded_load={} orig_final={} loaded_final={}",
            roundtrip_ok, resim_ok, orig_at_save.world_hash, loaded_at_load.world_hash,
            orig_final.world_hash, loaded_final.world_hash);
        return 1;
    }

    LUMINUMBRA_CORE_INFO(
        "Headless server HEAVY passed: round-trip + {}-tick resim hashes equal "
        "(world_hash={})",
        options.heavy_resim, orig_final.world_hash);
    return 0;
}

// ---------------------------------------------------------------------------
// T-I4-12 session replay (LREC1). Recording is the desync-repro tool: a stream
// is (boot parameters + per-tick inputs + 30-tick world_hash checkpoints).
// Replay reboots from the header, feeds the recorded inputs, and verifies the
// live hashes against the recorded checkpoints -- the first mismatch localizes a
// desync to a tick + a sub-hash section (the T-I4-11 localization). The headless
// smoke has NO player inputs today, so the per-tick input set is empty; the
// format carries it anyway for T-I4-13 (lockstep transport, which consumes this
// stream as its desync dump format).
//
// Determinism: recording must NOT perturb the simulation. The ReplayWriter
// buffers all records in memory and flushes to disk only at Finalize(), so no
// IO sits on the tick path. Checkpoint hashing reuses ComputeWorldHash /
// ComputeWorldSubHashes (the same quiesce-then-snapshot the smoke does), which
// reads state without mutating it. Proof: the ReplayRoundtrip gate asserts the
// recorded run reaches the SAME 0eac465289e7c88b as the smoke (T-I5a-2
// mega-bump: was 2fa007951a21e140 before the `wind` sub-hash slot landed).
// ---------------------------------------------------------------------------

constexpr std::uint64_t kCheckpointIntervalTicks = 30; // one second at 30 Hz

// Captures a checkpoint record at the given tick from a booted runner. Uses the
// single-snapshot combined hash path (one settled-state read per checkpoint).
Luminumbra::Replay::CheckpointRecord CaptureCheckpoint(
    std::uint64_t tick, Luminumbra::Server::ServerWorldRunner& runner) {
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
    LUMINUMBRA_CORE_INFO(
        "Headless server RECORD (LREC1): preset={} seed={} ticks={} -> {}",
        options.preset, options.seed, options.ticks, options.record_path);

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
    header.preset_hash = std::strtoull(
        Luminumbra::Replay::Fnv1a64Hex(options.preset).c_str(), nullptr, 16);
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
        // The input set for the tick ABOUT to run. T-I4-13 will populate this.
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
        options.record_path, executed, writer.InputRecordCount(),
        writer.CheckpointRecordCount(), final_hash);
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
        LUMINUMBRA_CORE_ERROR("replay: stream '{}' is truncated (no valid trailer)", options.replay_path);
        return 1;
    }

    const Luminumbra::Replay::ReplayHeader& header = contents->header;
    // Refuse a version / engine mismatch loudly (Factorio replays break silently
    // across versions; we refuse instead -- research Area 2 takeaway 8).
    const std::string engine_now(luminumbra::core::GetEngineVersionString());
    if (header.version != Luminumbra::Replay::kLrec1Version) {
        LUMINUMBRA_CORE_ERROR("replay: stream LREC version {} != engine {}",
            header.version, Luminumbra::Replay::kLrec1Version);
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
        // driver applies it once T-I4-13 carries real inputs).
        const Luminumbra::Replay::InputRecord* input =
            Luminumbra::Replay::FindInput(*contents, next_tick);
        (void)input; // applied by the transport in T-I4-13; no-op for empty sets
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
            {"generated_by", "luminumbra_server_app --replay (T-I4-12)"},
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
                LUMINUMBRA_CORE_INFO("replay divergence artifact written: {}", options.artifact_path);
            }
        }
        LUMINUMBRA_CORE_ERROR(
            "replay DIVERGED at tick {} (section={}): expected world_hash={} actual={} "
            "({} checkpoints verified before divergence)",
            divergence_tick, divergence_section, expected_hash, actual_hash, checkpoints_verified);
        return 1;
    }

    // Clean completion: write the replay (success) artifact.
    nlohmann::json artifact{
        {"schema", "luminumbra.replay_roundtrip.v1"},
        {"generated_by", "luminumbra_server_app --replay (T-I4-12)"},
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
        LUMINUMBRA_CORE_ERROR(
            "replay FAILED: start_match={} end_match={} ticks={}/{}",
            start_match, end_hash_match, executed, contents->tick_count);
        return 1;
    }
    LUMINUMBRA_CORE_INFO(
        "replay passed: {} ticks, {} checkpoints verified, end_hash={} matches recording",
        executed, checkpoints_verified, live_end);
    return 0;
}

// T-I4-12 ReplayDivergence gate fixture: read an LREC1 stream and rewrite it
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
        LUMINUMBRA_CORE_ERROR("mutate: '{}' is not a valid LREC1 stream", options.mutate_replay_fixture);
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
        target_tick, options.mutate_replay_fixture);
    return 0;
}

// ---------------------------------------------------------------------------
// T-I4-13 lockstep transport: in-process loopback drive of BOTH peers (the gate
// path -- no sockets/ports). Each peer owns a ServerWorldRunner stepping the same
// world (same seed/preset => identical hashes); the host is the sim authority and
// both exchange world_hash + sub-hashes at the 30-tick cadence (the LREC1 checkpoint
// cadence). The adaptive horizon is HASH-NEUTRAL: it only decides WHEN a tick runs,
// never WHAT it computes, so the canonical 90-tick hash 0eac465289e7c88b is unchanged
// by lockstep (T-I5a-2 mega-bump: was 2fa007951a21e140 pre-wind-slot).
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

bool LockstepApplyStep(std::uint64_t /*tick*/, const std::vector<std::uint8_t>& /*merged*/,
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
    LUMINUMBRA_CORE_INFO(
        "Headless server LOCKSTEP loopback: preset={} seed={} ticks={} delay_input={} corrupt_tick={}",
        options.preset, options.seed, options.ticks, options.lockstep_delay_input,
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

    const std::string dump_path = options.lockstep_dump_path.empty()
        ? (fs::temp_directory_path() / "lockstep-desync.lrec1").string()
        : options.lockstep_dump_path;
    host.SetDumpPath(dump_path);
    peer.SetDumpPath(dump_path + ".peer");

    // Handshake: send each Hello, then complete both (single-process driver order).
    // peer's Hello must be queued before host.Handshake() looks for it -- peer.Handshake()
    // sends peer's Hello into peer->host queue, then host.Handshake() consumes it; host's
    // Hello (sent by host.Handshake) is then consumed by a second peer drain inside its
    // own PumpTick drain. To keep it simple+robust we send both Hellos first.
    {
        Luminumbra::Net::HelloMsg ph;
        ph.seed = seed_num; ph.preset = options.preset; ph.tick_rate_hz = 30; ph.client_id = 1;
        peer_transport->SendFrame(Luminumbra::Net::EncodeHello(ph));
        Luminumbra::Net::HelloMsg hh;
        hh.seed = seed_num; hh.preset = options.preset; hh.tick_rate_hz = 30; hh.client_id = 0;
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
        if (fatal(hr.outcome) || fatal(pr.outcome)) { diverged = true; break; }
        if (hr.outcome == Luminumbra::Net::TickOutcome::Finished &&
            pr.outcome == Luminumbra::Net::TickOutcome::Finished) {
            break;
        }
    }

    const auto host_status = host.Status();
    const auto peer_status = peer.Status();
    const bool desynced = host_status.desynced || peer_status.desynced;
    const std::uint64_t desync_tick = host_status.desynced ? host_status.desync_tick : peer_status.desync_tick;
    const std::string desync_section = host_status.desynced ? host_status.desync_section : peer_status.desync_section;
    const std::string emitted_dump = host_status.desynced ? host_status.dump_path : peer_status.dump_path;

    // Final hashes from BOTH worlds (settled), for the gate's identical-end-hash assert.
    const std::string host_hash = host_runner->ComputeWorldHash();
    const std::string peer_hash = peer_runner->ComputeWorldHash();

    const fs::path host_save = host_runner->Session()->GetWorldSaveDir();
    const fs::path peer_save = peer_runner->Session()->GetWorldSaveDir();
    host_runner->Shutdown();
    peer_runner->Shutdown();
    for (const fs::path& d : {host_save, peer_save}) {
        if (!d.empty()) { std::error_code ec; fs::remove_all(d, ec); }
    }

    // Scenario classification: corrupt => expect a halt+dump; otherwise expect in-sync.
    const bool expect_desync = options.lockstep_corrupt_tick != 0;
    bool passed = false;
    if (expect_desync) {
        passed = desynced &&
            desync_tick == options.lockstep_corrupt_tick &&
            !emitted_dump.empty() && fs::exists(emitted_dump);
    } else {
        passed = !desynced &&
            host_status.agreed_tick == budget && peer_status.agreed_tick == budget &&
            host_hash == peer_hash && !host_hash.empty();
    }

    nlohmann::json artifact{
        {"schema", "luminumbra.lockstep_loopback.v1"},
        {"generated_by", "luminumbra_server_app --lockstep-loopback (T-I4-13)"},
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
        {"host", {
            {"agreed_tick", host_status.agreed_tick},
            {"final_horizon", host_status.horizon},
            {"max_horizon_reached", host_status.max_horizon_reached},
            {"late_input_events", host_status.late_input_events},
            {"world_hash", host_hash},
        }},
        {"peer", {
            {"agreed_tick", peer_status.agreed_tick},
            {"final_horizon", peer_status.horizon},
            {"max_horizon_reached", peer_status.max_horizon_reached},
            {"late_input_events", peer_status.late_input_events},
            {"world_hash", peer_hash},
        }},
        {"end_hashes_equal", host_hash == peer_hash},
        {"horizon_absorbed_jitter", options.lockstep_delay_input > 0 &&
                                    !desynced && host_status.max_horizon_reached > 3},
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
            expect_desync, desynced, desync_tick,
            host_status.agreed_tick, peer_status.agreed_tick, host_hash, peer_hash);
        return 1;
    }

    if (expect_desync) {
        LUMINUMBRA_CORE_INFO(
            "lockstep loopback passed: oracle HALTED at tick {} (section={}), LREC1 dump -> {}",
            desync_tick, desync_section, emitted_dump);
    } else {
        LUMINUMBRA_CORE_INFO(
            "lockstep loopback passed: {} ticks in sync, end_hash={} (host==peer), "
            "max_horizon={} late_inputs={}",
            budget, host_hash, host_status.max_horizon_reached, host_status.late_input_events);
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
            "Usage: luminumbra_server_app [--smoke] [--heavy [--heavy-resim <n>]] "
            "[--record <path>] [--replay <path>] [--mutate-replay-fixture <path>] "
            "[--lockstep-loopback [--lockstep-delay-input <n>] [--lockstep-corrupt-tick <t>] "
            "[--lockstep-dump <path>]] "
            "[--root <path>] [--preset <name>] "
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
    return options.smoke ? RunSmoke(options) : RunServer(options);
}
