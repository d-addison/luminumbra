#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

namespace {
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

} // namespace

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
