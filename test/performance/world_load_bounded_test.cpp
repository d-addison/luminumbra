//  ( step 3) proving gate: the interactive world load is
// BOUNDED. Twenty cold "CONSTRUCTING WORLD GEOMETRY"-style loads (clear_world
// then EnsureSurfaceReadyNear at interactive radii 12/4) each complete in
// under 60 seconds with the LUMINUMBRA_JOB_WATCHDOG reporter armed. The load
// path dispatches its surface builds in bounded 64-job sub-batches with
// named per-batch progress, so a wedge would surface as a named
// watchdog report + a blown per-load bound here — never a silent hang.
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
constexpr double kPerLoadBudgetMs = 60000.0;

TerrainGenParams LoadPresetParams(const fs::path& path) {
    const Luminumbra::world::TerrainPresetLoadResult result =
        Luminumbra::world::LoadTerrainPreset(path);
    EXPECT_TRUE(result.ok) << path.string();
    for (const std::string& error : result.errors) {
        ADD_FAILURE() << "preset parse error: " << error;
    }
    return result.params;
}

TEST(WorldLoadBounded, TwentyColdInteractiveLoadsUnderSixtySecondsEach) {
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

            EXPECT_LT(ms, kPerLoadBudgetMs)
                << "load " << load << " at (" << x << "," << z << ") took " << ms
                << " ms — the  bounded-load contract is broken (check the "
                   "named watchdog phase in the log for the wedged sub-batch)";
        }
        RecordProperty("worst_load_ms", static_cast<int>(worst_ms));
    }
    physics.shutdown();
    jobs.shutdown();
}

} // namespace
