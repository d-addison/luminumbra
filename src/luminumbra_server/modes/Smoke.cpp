#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

namespace {
constexpr const char* kServerTickArtifactSchema = "luminumbra.server_tick.v1";

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
    // water-smoke non-vacuity: the workload must have exercised REAL water — the integer
    // mass invariant held every tick in both runs, cross-chunk seam flux was observed
    // (wet cell-pairs on both sides of a chunk border), and cells were actually simulated.
    // A dry spawn region or a broken wet-anchor scan fails the artifact loudly instead of
    // emitting vacuous timings. Always true outside the --water-smoke lane.
    const bool water_nonvacuous =
        !options.water_smoke ||
        (first.ticks.water.mass_ok && replay.ticks.water.mass_ok &&
         first.ticks.water.seam_wet_pairs_max > 0 && first.ticks.water.cells_simmed_total > 0);

    const bool passed = deterministic && first.ticks.ticks_executed == options.ticks &&
                        replay.ticks.ticks_executed == options.ticks && first.chunks_streamed > 0 &&
                        water_nonvacuous;

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
    // water-smoke: water sub-phase timing percentiles + sim-load counters and the
    // non-vacuity fields. Wall-clock/debug observability — never feeds world_hash.
    if (options.water_smoke) {
        const auto phase_json = [](const Luminumbra::Server::ServerTickReport::WaterPhaseStats& p) {
            return nlohmann::json{{"p50", p.p50_ms}, {"p95", p.p95_ms}, {"max", p.max_ms}};
        };
        const auto& w = first.ticks.water;
        artifact["water_phase_ms"] = {
            {"init", phase_json(w.init)},
            {"sim", phase_json(w.sim)},
            {"seam", phase_json(w.seam)},
            {"bookkeeping", phase_json(w.bookkeeping)},
            {"total", phase_json(w.total)},
        };
        artifact["water_cells_simmed_per_tick"] = w.cells_simmed_per_tick;
        artifact["water_cells_simmed_total"] = w.cells_simmed_total;
        artifact["water_awake_chunks_max"] = w.awake_chunks_max;
        artifact["water_mass_ok"] = w.mass_ok && replay.ticks.water.mass_ok;
        artifact["water_seam_wet_pairs_max"] = w.seam_wet_pairs_max;
        LUMINUMBRA_CORE_INFO(
            "Water phases: total p50={:.3f}ms p95={:.3f}ms max={:.3f}ms | sim p95={:.3f}ms "
            "init p95={:.3f}ms seam p95={:.3f}ms book p95={:.3f}ms | cells/tick={:.1f} "
            "awake_max={} seam_wet_max={} mass_ok={}",
            w.total.p50_ms,
            w.total.p95_ms,
            w.total.max_ms,
            w.sim.p95_ms,
            w.init.p95_ms,
            w.seam.p95_ms,
            w.bookkeeping.p95_ms,
            w.cells_simmed_per_tick,
            w.awake_chunks_max,
            w.seam_wet_pairs_max,
            w.mass_ok);
    }

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
