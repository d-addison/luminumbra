#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

namespace {
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

} // namespace

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
