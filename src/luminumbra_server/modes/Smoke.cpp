#include "ModeHelpers.h"
#include "Modes.h"
#include "SmokeArtifact.h"

namespace fs = std::filesystem;

namespace {
using Luminumbra::Server::SmokeRunResult;

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
    if (options.sim_budget)
        result.sim_budget = runner.Session()->GetSimBudgetTelemetry();
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

} // namespace

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

    return Luminumbra::Server::WriteSmokeArtifact(options, first, replay);
}
