#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

namespace {
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

} // namespace

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
