// perf-lane-and-ecology-tick ( / ): the EcologyTickPerf gate.
//
// This is the campaign's first PERF gate over the live ecology tick. Every other
// ecology test proves correctness/determinism; NONE measures per-tick cost. After
// the boids O(N^2) herd gather was replaced by a uniform spatial grid
// (ai/SpatialGrid.h, commit c48b466) we need a gate that actually EXERCISES the
// scaling path at N >> the 8-creature gtest roster, or the win is unverified.
//
// Design (plan ):
//   * Reuse the PopulatedWorldReplay KINEMATIC roster shape (the same component set
//     the gate-populated-world-replay roster spawns: predators w/ pack+mortal, prey
//     w/ genome+alarm+mortal+decay+migratory+territory) so the integration cost is
//     representative of the shipped ecology tick, NOT conflated with Jolt physics
//     (no CreaturePhysicsComponent -> the brain's direct X/Z integration runs).
//   * Parametrize the roster to N in {256, 1000, 4000} via a deterministic
//     phyllotaxis ("sunflower") spawn layout so creatures are spread across a disc
//     (not co-located), which is what makes the spatial-grid radius query do real
//     work (a degenerate stack would put every creature in one cell).
//   * Tick the GameSession slot-order ecology stack 300 times headless, time each
//     tick with a steady_clock, and report MEDIAN + p99 ms/tick per N.
//   * Emit ecology_tick_perf.json (luminumbra.ecology_tick_perf.v1) for the
//     -Mode EcologyTickPerf validator gate to read + assert median under budget.
//
// This file is a SELF-CONTAINED gtest exe (its own target): it links only
// luminumbra_common + gtest, runs no world/physics, and is libm-free on the spawn
// path (DeterministicMath) so it stays clean even though spawn positions here do
// not feed any hashed sim term.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <entt/entt.hpp>

#include "ai/CreatureBrainSystem.h"
#include "ai/CreatureReproductionSystem.h"
#include "ai/DecompositionSystem.h"
#include "ai/HerdAlarmSystem.h"
#include "ai/LifespanSystem.h"
#include "ai/MigrationSystem.h"
#include "ai/PredatorPackSystem.h"
#include "ai/SteeringConsumer.h"
#include "ai/TerritorySystem.h"
#include "components/AlarmComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/DecayComponents.h"
#include "components/MigratoryComponents.h"
#include "components/MortalComponents.h"
#include "components/PackHunterComponents.h"
#include "components/TerritoryComponents.h"
#include "core/DeterministicMath.h"

namespace fs = std::filesystem;

namespace {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

fs::path ArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "sim";
}

// One deterministic ecology tick in GameSession's slot order (mirrors
// ecology_pipeline_test::EcologyTick exactly so the gate times the SAME stack the
// world ticks).
void EcologyTick(entt::registry& r, std::uint64_t tick) {
    constexpr float dt = 1.0f / 30.0f;
    luminumbra::ai::RunCreatureBrainSystemOnTick(r, dt); // decide + move (bodyless)
    luminumbra::ai::RunMateSeekingOnTick(r);             // 2e-mate A
    luminumbra::ai::RunSteeringConsumerOnTick(r);        // 2e-steer
    luminumbra::ai::RunMatingResolveOnTick(r, tick);     // 2e-mate B
    luminumbra::ai::RunHerdAlarmOnTick(r, dt);
    luminumbra::ai::RunLifespanOnTick(r, tick);
    luminumbra::ai::RunDecompositionOnTick(r, tick);
    luminumbra::ai::RunPredatorPackOnTick(r, tick);
    luminumbra::ai::RunMigrationOnTick(r, 0.25f);
    luminumbra::ai::RunTerritoryOnTick(r, tick);
}

// Spawn N kinematic creatures laid out by phyllotaxis (sunflower) so they occupy a
// disc rather than a single grid cell. ~1 predator per 8 prey, matching the
// gate-populated-world-replay roster's pack/herd mix. The radius grows as sqrt(i)
// to keep a roughly uniform areal density (and so the spatial grid sees many
// occupied cells, which is the case the radius query must handle).
void SpawnRoster(entt::registry& r, int n) {
    constexpr float kGoldenAngle = 2.39996323f; // 137.5 deg, radians
    // Spacing chosen so neighbours sit ~within the flocking neighbour radius (12 m)
    // at the inner ring but the disc stays bounded at N=4000 (sqrt(4000)*k ~ a few
    // hundred metres), i.e. a realistic crowded-but-spread herd, not a point.
    constexpr float kSpacing = 5.0f;
    int prey_idx = 0;
    for (int i = 0; i < n; ++i) {
        const float angle = static_cast<float>(i) * kGoldenAngle;
        const float radius = kSpacing * dm::Sqrt(static_cast<float>(i));
        const float x = radius * dm::Cos(angle);
        const float z = radius * dm::Sin(angle);

        auto e = r.create();
        auto& tf = r.emplace<Comp::TransformComponent>(e);
        tf.position = Luminumbra::Vec3(x, 0.0f, z);
        auto& cr = r.emplace<Comp::CreatureComponent>(e);

        // Every 9th creature is a predator (pack hunter); the rest are prey,
        // carrying the full breeding/aging/territorial/migratory component set so
        // the WHOLE ecology stack runs (not just the brain).
        const bool predator = (i % 9 == 0);
        if (predator) {
            cr.is_predator = true;
            cr.hunger = 0.9f;
            cr.move_speed = 4.2f;
            r.emplace<Comp::PackHunterComponent>(e);
            // Long lifespan so predators are NOT culled mid-run (keeps N stable for
            // a clean ms/tick measurement; this is a PERF gate, not a population
            // gate -- determinism/population are covered by EcologyPipeline.*).
            r.emplace<Comp::MortalComponent>(e).lifespan_ticks = 1000000u;
        } else {
            cr.is_predator = false;
            cr.hunger = 0.05f;
            cr.stamina = 1.0f;
            cr.move_speed = 3.0f;
            auto& gn = r.emplace<Comp::CreatureGenomeComponent>(e);
            gn.female = (prey_idx++ % 2 == 0);
            gn.age_ticks = 100u;
            r.emplace<Comp::AlarmComponent>(e);
            // Long lifespan + far-future decay: keep the roster size flat across the
            // 300 ticks so the timing is over a stable N (not a shrinking herd).
            r.emplace<Comp::MortalComponent>(e).lifespan_ticks = 1000000u;
            r.emplace<Comp::DecayComponent>(e).decay_duration = 90u;
            r.emplace<Comp::MigratoryComponent>(e);
            r.emplace<Comp::TerritoryComponent>(e);
            r.emplace<Comp::TerritoryBiasComponent>(e);
        }
    }
}

double Percentile(std::vector<double> values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double index = (percentile / 100.0) * static_cast<double>(values.size() - 1u);
    const std::size_t lower = static_cast<std::size_t>(std::floor(index));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(index));
    if (lower == upper) {
        return values[lower];
    }
    const double fraction = index - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

double Median(std::vector<double> values) {
    return Percentile(std::move(values), 50.0);
}

struct PerfResult {
    int n = 0;
    int spawned = 0;
    double median_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
    double total_ms = 0.0;
};

PerfResult MeasureRoster(int n, int ticks) {
    entt::registry r;
    SpawnRoster(r, n);
    const int spawned = static_cast<int>(r.view<Comp::CreatureComponent>().size());

    std::vector<double> samples_ms;
    samples_ms.reserve(static_cast<std::size_t>(ticks));
    for (int t = 0; t < ticks; ++t) {
        const auto start = std::chrono::steady_clock::now();
        EcologyTick(r, static_cast<std::uint64_t>(t));
        const auto end = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - start).count();
        samples_ms.push_back(ms);
    }

    PerfResult result;
    result.n = n;
    result.spawned = spawned;
    result.median_ms = Median(samples_ms);
    result.p99_ms = Percentile(samples_ms, 99.0);
    result.max_ms =
        samples_ms.empty() ? 0.0 : *std::max_element(samples_ms.begin(), samples_ms.end());
    for (double s : samples_ms)
        result.total_ms += s;
    return result;
}

// N in {256, 1000, 4000}, 300 ticks, report median + p99 ms/tick.
TEST(EcologyTickPerf, MeasuresMedianAndP99AcrossRosterSizes) {
    constexpr int kTicks = 300;
    const std::vector<int> roster_sizes = {256, 1000, 4000};

    std::vector<PerfResult> results;
    results.reserve(roster_sizes.size());
    for (int n : roster_sizes) {
        results.push_back(MeasureRoster(n, kTicks));
    }

    nlohmann::json results_json = nlohmann::json::array();
    for (const PerfResult& r : results) {
        results_json.push_back({
            {"n", r.n},
            {"spawned", r.spawned},
            {"median_ms", r.median_ms},
            {"p99_ms", r.p99_ms},
            {"max_ms", r.max_ms},
            {"total_ms", r.total_ms},
        });
    }

#ifdef NDEBUG
    const char* build_mode = "release";
#else
    const char* build_mode = "debug";
#endif

    const nlohmann::json artifact = {
        {"schema", "luminumbra.ecology_tick_perf.v1"},
        {"ticks", kTicks},
        {"roster_kind", "kinematic"},
        {"build_mode", build_mode},
        {"results", results_json},
    };

    fs::create_directories(ArtifactRoot());
    const fs::path out = ArtifactRoot() / "ecology_tick_perf.json";
    std::ofstream output(out);
    ASSERT_TRUE(output) << out.string();
    output << std::setw(2) << artifact << "\n";

    // Non-vacuity: every roster must have spawned the requested N and produced a
    // positive, finite median (so the emitted numbers are real measurements, not a
    // zero-creature no-op the gate would read as "free").
    for (const PerfResult& r : results) {
        EXPECT_EQ(r.spawned, r.n) << "roster N=" << r.n << " spawned " << r.spawned;
        EXPECT_GT(r.median_ms, 0.0) << "roster N=" << r.n << " median ms/tick was non-positive";
        EXPECT_TRUE(std::isfinite(r.median_ms) && std::isfinite(r.p99_ms))
            << "roster N=" << r.n << " produced a non-finite timing";
        // The gtest itself is NOT the budget gate (budgets live in the
        // EcologyTickPerf validator against the reviewed release summary
        // ecology_tick block, blessed via capture-ecology-tick-budgets.ps1); it
        // only asserts the measurement is well-formed and emits the artifact.
        //
        // STANDING RULE: there is NO ecology budget CAP in
        // the sim today — the WHOLE creature roster ticks every tick, and these
        // budgets only measure/enforce that full-roster cost. If holding the
        // budget ever requires capping per-tick ecology work, the cap MUST be a
        // deterministic rotating id-sorted window (the WaterSystem
        // MAX_WATER_CELLS_PER_TICK pattern: sort by stable id, advance a persisted
        // cursor by the window size each tick), NEVER a time-based/adaptive
        // cutoff — wall-clock caps make the processed set machine-dependent and
        // break run==replay and host==peer.
    }
}

} // namespace
