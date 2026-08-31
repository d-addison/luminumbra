// Manual latency diagnostic for twenty cold "CONSTRUCTING WORLD GEOMETRY"-style
// loads (clear_world then EnsureSurfaceReadyNear at interactive radii 12/4).
// The watchdog names slow sub-batches, while the test records observed latency
// without imposing a machine-specific absolute performance threshold. Release
// gating uses paired relative measurements from tools/perf instead.
#include "gtest/gtest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "core/JobSystem.h"
#include "systems/PhysicsSystem.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/TerrainPresetLoader.h"

namespace fs = std::filesystem;

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

constexpr int kSeed = 424242;
constexpr int kSurfaceRadius = 12; // the interactive client load radius
constexpr int kCollisionRadius = 4;
constexpr int kLoads = 20;

TerrainGenParams LoadPresetParams(const fs::path& path) {
    const Luminumbra::world::TerrainPresetLoadResult result =
        Luminumbra::world::LoadTerrainPreset(path);
    EXPECT_TRUE(result.ok) << path.string();
    for (const std::string& error : result.errors) {
        ADD_FAILURE() << "preset parse error: " << error;
    }
    return result.params;
}

TEST(WorldLoadBounded, TwentyColdInteractiveLoadsReportLatency) {
    // Arm the named-phase wedge reporter BEFORE the first wait runs (the
    // enabled check is cached on first use). Observability only.
#ifdef _WIN32
    _putenv_s("LUMINUMBRA_JOB_WATCHDOG", "1");
#else
    setenv("LUMINUMBRA_JOB_WATCHDOG", "1", 1);
#endif

    const fs::path preset_path =
        fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json";
    ASSERT_TRUE(fs::exists(preset_path)) << preset_path.string();
    const TerrainGenParams params = LoadPresetParams(preset_path);

    JobSystem jobs;
    jobs.startup();
    PhysicsSystem physics;
    physics.startup();
    {
        SHIELD_WorldSystem world(&jobs, nullptr, params, kSeed);

        double worst_ms = 0.0;
        for (int load = 0; load < kLoads; ++load) {
            // A true cold load: drop every chunk, then rebuild the
            // interactive surface horizon from scratch. Vary the position a
            // little so the loads are not byte-for-byte replays of one
            // another (the column-span cache legitimately persists, exactly
            // as it does across interactive teleports).
            world.clear_world(&physics);
            const float x = 8.0f + static_cast<float>(load) * 40.0f;
            const float z = 8.0f + static_cast<float>(load % 5) * 40.0f;
            const float height = world.GetTerrainHeightAt(x, z);
            ASSERT_TRUE(std::isfinite(height));
            const Vec3 pos(x, height + 1.95f, z);

            const auto t0 = std::chrono::steady_clock::now();
            ASSERT_TRUE(
                world.EnsureSurfaceReadyNear(pos, &physics, kSurfaceRadius, kCollisionRadius))
                << "load " << load << " failed outright";
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            worst_ms = std::max(worst_ms, ms);

            RecordProperty("load_" + std::to_string(load) + "_ms", static_cast<int>(ms));
        }
        RecordProperty("worst_load_ms", static_cast<int>(worst_ms));
    }
    physics.shutdown();
    jobs.shutdown();
}

} // namespace
