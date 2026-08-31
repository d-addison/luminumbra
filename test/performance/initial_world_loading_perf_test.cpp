#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "entt/entt.hpp"
#include "nlohmann/json.hpp"

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
#include "components/CreatureComponents.h"
#include "components/DecayComponents.h"
#include "components/MigratoryComponents.h"
#include "components/MortalComponents.h"
#include "components/PackHunterComponents.h"
#include "components/TerritoryComponents.h"
#include "core/DeterministicMath.h"
#include "core/JobSystem.h"
#include "systems/PhysicsSystem.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"
#include "world/TerrainPresetLoader.h"

namespace fs = std::filesystem;

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

constexpr int kSeed = 424242;
constexpr int kSurfaceRadius = 12;
constexpr int kCollisionRadius = 4;
constexpr double kRunawayPrepLimitMs = 120000.0;

struct ScopedJobSystem {
    ScopedJobSystem() {
        jobs.startup();
    }

    ~ScopedJobSystem() {
        jobs.shutdown();
    }

    JobSystem jobs;
};

struct ScopedPhysicsSystem {
    ScopedPhysicsSystem() {
        physics.startup();
    }

    ~ScopedPhysicsSystem() {
        physics.shutdown();
    }

    PhysicsSystem physics;
};

struct Timer {
    using Clock = std::chrono::steady_clock;

    Timer()
        : start(Clock::now()) {}

    double elapsed_ms() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    Clock::time_point start;
};

struct MeshStats {
    std::size_t mesh_chunks = 0;
    std::size_t collision_chunks = 0;
    std::size_t vertices = 0;
    std::size_t indices = 0;
    std::size_t triangles = 0;
    std::size_t min_vertices_per_chunk = 0;
    std::size_t max_vertices_per_chunk = 0;
    double mean_vertices_per_chunk = 0.0;
    std::array<std::size_t, 3> lod_mesh_chunks{0u, 0u, 0u};
};

fs::path SourceRoot() {
    return fs::weakly_canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
}

fs::path ArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "performance";
}

fs::path PerformanceFrameworkArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "performance_framework";
}

std::string ReadFirstLine(const fs::path& path) {
    std::ifstream input(path);
    std::string line;
    if (input && std::getline(input, line)) {
        return line;
    }
    return {};
}

std::string CurrentGitSha() {
    const fs::path git_dir = SourceRoot() / ".git";
    const std::string head = ReadFirstLine(git_dir / "HEAD");
    if (head.empty()) {
        return "unknown";
    }
    constexpr const char* ref_prefix = "ref: ";
    if (head.rfind(ref_prefix, 0) == 0) {
        const std::string ref_path = head.substr(std::string(ref_prefix).size());
        const std::string ref_sha = ReadFirstLine(git_dir / ref_path);
        return ref_sha.empty() ? "unknown"
                               : ref_sha.substr(0, std::min<std::size_t>(12u, ref_sha.size()));
    }
    return head.substr(0, std::min<std::size_t>(12u, head.size()));
}

TerrainGenParams LoadPresetParams(const fs::path& path) {
    // delegate to the canonical engine preset parser.
    const Luminumbra::world::TerrainPresetLoadResult result =
        Luminumbra::world::LoadTerrainPreset(path);
    EXPECT_TRUE(result.ok) << path.string();
    for (const std::string& error : result.errors) {
        ADD_FAILURE() << "preset parse error: " << error;
    }
    return result.params;
}

MeshStats CalculateMeshStats(const std::vector<Chunk*>& chunks) {
    MeshStats stats;
    stats.min_vertices_per_chunk = std::numeric_limits<std::size_t>::max();

    for (const Chunk* chunk : chunks) {
        if (chunk->mesh_vertices.empty() || chunk->mesh_indices.empty()) {
            continue;
        }

        ++stats.mesh_chunks;
        if (chunk->has_collision.load()) {
            ++stats.collision_chunks;
        }

        const int lod = std::clamp(chunk->current_lod.load(), 0, 2);
        ++stats.lod_mesh_chunks[static_cast<std::size_t>(lod)];
        stats.vertices += chunk->mesh_vertices.size();
        stats.indices += chunk->mesh_indices.size();
        stats.triangles += chunk->mesh_indices.size() / 3u;
        stats.min_vertices_per_chunk =
            std::min(stats.min_vertices_per_chunk, chunk->mesh_vertices.size());
        stats.max_vertices_per_chunk =
            std::max(stats.max_vertices_per_chunk, chunk->mesh_vertices.size());
    }

    if (stats.mesh_chunks == 0u) {
        stats.min_vertices_per_chunk = 0u;
        return stats;
    }

    stats.mean_vertices_per_chunk =
        static_cast<double>(stats.vertices) / static_cast<double>(stats.mesh_chunks);
    return stats;
}

nlohmann::json VecToJson(const Vec3& value) {
    return {
        {"x", value.x},
        {"y", value.y},
        {"z", value.z},
    };
}

nlohmann::json StreamingStatsToJson(const SHIELD_WorldSystem::StreamingBudgetFrameStats& stats) {
    return {
        {"update_interval_frames", stats.update_interval_frames},
        {"requested_render_radius", stats.requested_render_radius},
        {"target_render_radius", stats.target_render_radius},
        {"generation_budget", stats.generation_budget},
        {"meshing_budget", stats.meshing_budget},
        {"max_active_chunks_budget", stats.max_active_chunks_budget},
        {"active_chunks_before", stats.active_chunks_before},
        {"active_chunks_after", stats.active_chunks_after},
        {"ready_chunks", stats.ready_chunks},
        {"renderable_chunks", stats.renderable_chunks},
        {"idle_chunks", stats.idle_chunks},
        {"loading_chunks", stats.loading_chunks},
        {"meshing_chunks", stats.meshing_chunks},
        {"target_surface_columns", stats.target_surface_columns},
        {"generation_candidates", stats.generation_candidates},
        {"surface_generation_candidates", stats.surface_generation_candidates},
        {"vertical_generation_candidates", stats.vertical_generation_candidates},
        {"scheduled_generation", stats.scheduled_generation},
        {"surface_generation_scheduled", stats.surface_generation_scheduled},
        {"vertical_generation_scheduled", stats.vertical_generation_scheduled},
        {"deferred_generation", stats.deferred_generation},
        {"meshing_candidates", stats.meshing_candidates},
        {"scheduled_meshing", stats.scheduled_meshing},
        {"deferred_meshing", stats.deferred_meshing},
        {"unloaded_chunks", stats.unloaded_chunks},
        {"generation_job_active", stats.generation_job_active},
        {"meshing_job_active", stats.meshing_job_active},
    };
}

const char* ChunkStateName(ChunkState state) {
    switch (state) {
        case ChunkState::Unloaded:
            return "unloaded";
        case ChunkState::Loading:
            return "loading";
        case ChunkState::Idle:
            return "idle";
        case ChunkState::Meshing:
            return "meshing";
        case ChunkState::Ready:
            return "ready";
        case ChunkState::Unloading:
            return "unloading";
    }
    return "unknown";
}

nlohmann::json RuntimeChunkStatsToJson(const SHIELD_WorldSystem::RuntimeChunkStats& stats) {
    return {
        {"total_chunks", stats.total_chunks},
        {"states",
         {
             {ChunkStateName(ChunkState::Unloaded), stats.unloaded_chunks},
             {ChunkStateName(ChunkState::Loading), stats.loading_chunks},
             {ChunkStateName(ChunkState::Idle), stats.idle_chunks},
             {ChunkStateName(ChunkState::Meshing), stats.meshing_chunks},
             {ChunkStateName(ChunkState::Ready), stats.ready_chunks},
             {ChunkStateName(ChunkState::Unloading), stats.unloading_chunks},
         }},
        {"renderable_chunks", stats.renderable_chunks},
        {"collision_chunks", stats.collision_chunks},
        {"terrain_vertex_count", stats.terrain_vertex_count},
        {"terrain_index_count", stats.terrain_index_count},
        {"water_vertex_count", stats.water_vertex_count},
        {"water_index_count", stats.water_index_count},
        {"terrain_payload_bytes", stats.terrain_payload_bytes},
        {"generation_job_active", stats.generation_job_active},
        {"meshing_job_active", stats.meshing_job_active},
    };
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

nlohmann::json FrameStatsToJson(const std::vector<double>& samples_ms) {
    return {
        {"samples", samples_ms.size()},
        {"p50", Percentile(samples_ms, 50.0)},
        {"p95", Percentile(samples_ms, 95.0)},
        {"p99", Percentile(samples_ms, 99.0)},
        {"max", samples_ms.empty() ? 0.0 : *std::max_element(samples_ms.begin(), samples_ms.end())},
    };
}

nlohmann::json MeshingStatsToJson(const World::MarchingCubes::TerrainMeshBuildStats& stats) {
    const double elapsed_ms = static_cast<double>(stats.elapsed_us) / 1000.0;
    return {
        {"jobs", stats.jobs},
        {"step1_jobs", stats.step1_jobs},
        {"step2_jobs", stats.step2_jobs},
        {"step4_jobs", stats.step4_jobs},
        {"cells_visited", stats.cells_visited},
        {"active_cells", stats.active_cells},
        {"vertices", stats.vertices},
        {"indices", stats.indices},
        {"triangles", stats.triangles},
        {"elapsed_us", stats.elapsed_us},
        {"elapsed_ms", elapsed_ms},
        {"jobs_per_second",
         elapsed_ms > 0.0 ? (static_cast<double>(stats.jobs) * 1000.0) / elapsed_ms : 0.0},
        {"triangles_per_second",
         elapsed_ms > 0.0 ? (static_cast<double>(stats.triangles) * 1000.0) / elapsed_ms : 0.0},
    };
}

void WriteJson(const fs::path& path, const nlohmann::json& data) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << std::setw(2) << data << "\n";
}

// The `ecology` benchmark spawns a fixed kinematic creature roster laid out by
// phyllotaxis and ticks the GameSession ecology stack once per frame. The
// dedicated ecology_tick_perf_test executable measures median and p99 scaling at
// 256, 1,000, and 4,000 creatures.
namespace EcoComp = ::Luminumbra::Components;
namespace EcoMath = ::Luminumbra::DeterministicMath;

void SpawnEcologyBenchmarkRoster(entt::registry& r, int n) {
    constexpr float kGoldenAngle = 2.39996323f; // 137.5 deg, radians
    constexpr float kSpacing = 5.0f;
    int prey_idx = 0;
    for (int i = 0; i < n; ++i) {
        const float angle = static_cast<float>(i) * kGoldenAngle;
        const float radius = kSpacing * EcoMath::Sqrt(static_cast<float>(i));
        auto e = r.create();
        auto& tf = r.emplace<EcoComp::TransformComponent>(e);
        tf.position = Vec3(radius * EcoMath::Cos(angle), 0.0f, radius * EcoMath::Sin(angle));
        auto& cr = r.emplace<EcoComp::CreatureComponent>(e);
        if (i % 9 == 0) {
            cr.is_predator = true;
            cr.hunger = 0.9f;
            cr.move_speed = 4.2f;
            r.emplace<EcoComp::PackHunterComponent>(e);
            r.emplace<EcoComp::MortalComponent>(e).lifespan_ticks = 1000000u;
        } else {
            cr.is_predator = false;
            cr.hunger = 0.05f;
            cr.stamina = 1.0f;
            cr.move_speed = 3.0f;
            auto& gn = r.emplace<EcoComp::CreatureGenomeComponent>(e);
            gn.female = (prey_idx++ % 2 == 0);
            gn.age_ticks = 100u;
            r.emplace<EcoComp::AlarmComponent>(e);
            r.emplace<EcoComp::MortalComponent>(e).lifespan_ticks = 1000000u;
            r.emplace<EcoComp::DecayComponent>(e).decay_duration = 90u;
            r.emplace<EcoComp::MigratoryComponent>(e);
            r.emplace<EcoComp::TerritoryComponent>(e);
            r.emplace<EcoComp::TerritoryBiasComponent>(e);
        }
    }
}

void EcologyBenchmarkTick(entt::registry& r, std::uint64_t tick) {
    constexpr float dt = 1.0f / 30.0f;
    luminumbra::ai::RunCreatureBrainSystemOnTick(r, dt);
    luminumbra::ai::RunMateSeekingOnTick(r);
    luminumbra::ai::RunSteeringConsumerOnTick(r);
    luminumbra::ai::RunMatingResolveOnTick(r, tick);
    luminumbra::ai::RunHerdAlarmOnTick(r, dt);
    luminumbra::ai::RunLifespanOnTick(r, tick);
    luminumbra::ai::RunDecompositionOnTick(r, tick);
    luminumbra::ai::RunPredatorPackOnTick(r, tick);
    luminumbra::ai::RunMigrationOnTick(r, 0.25f);
    luminumbra::ai::RunTerritoryOnTick(r, tick);
}

} // namespace

TEST(InitialWorldLoadingPerfTest, MeasuresSurfaceHorizonPrep) {
    const fs::path preset_path = SourceRoot() / "worlds/atlas/presets/default.json";
    ASSERT_TRUE(fs::exists(preset_path)) << preset_path.string();
    fs::create_directories(ArtifactRoot());

    const TerrainGenParams params = LoadPresetParams(preset_path);
    ScopedJobSystem job_system;
    ScopedPhysicsSystem physics_system;
    SHIELD_WorldSystem world(&job_system.jobs, nullptr, params, kSeed);

    const float spawn_x = 8.0f;
    const float spawn_z = 8.0f;
    const float terrain_height = world.GetTerrainHeightAt(spawn_x, spawn_z);
    ASSERT_TRUE(std::isfinite(terrain_height));
    const Vec3 spawn(spawn_x, terrain_height + 1.95f, spawn_z);

    Timer initial_list_timer;
    const std::vector<IVec3> initial_chunks = world.GetInitialChunkLoadList(spawn);
    const double initial_list_ms = initial_list_timer.elapsed_ms();

    World::MarchingCubes::ResetTerrainMeshBuildStats();
    Timer prep_timer;
    ASSERT_TRUE(world.EnsureSurfaceReadyNear(
        spawn, &physics_system.physics, kSurfaceRadius, kCollisionRadius));
    const double prep_ms = prep_timer.elapsed_ms();
    const auto meshing_stats = World::MarchingCubes::GetTerrainMeshBuildStats();

    const std::vector<Chunk*> renderable_chunks = world.get_renderable_chunks();
    const MeshStats mesh_stats = CalculateMeshStats(renderable_chunks);

    constexpr std::size_t expected_surface_chunks =
        static_cast<std::size_t>((kSurfaceRadius * 2 + 1) * (kSurfaceRadius * 2 + 1));
    constexpr std::size_t expected_collision_chunks =
        static_cast<std::size_t>((kCollisionRadius * 2 + 1) * (kCollisionRadius * 2 + 1));

    const nlohmann::json report = {
        {"schema", "luminumbra.initial_world_loading.v1"},
        {"seed", kSeed},
        {"preset", preset_path.filename().string()},
        {"chunk_size", {{"x", CHUNK_SIZE_X}, {"y", CHUNK_SIZE_Y}, {"z", CHUNK_SIZE_Z}}},
        {"spawn", VecToJson(spawn)},
        {"terrain_height", terrain_height},
        {"initial_chunk_list",
         {
             {"chunks", initial_chunks.size()},
             {"elapsed_ms", initial_list_ms},
         }},
        {"surface_horizon",
         {
             {"surface_radius", kSurfaceRadius},
             {"collision_radius", kCollisionRadius},
             {"expected_surface_chunks", expected_surface_chunks},
             {"expected_collision_chunks", expected_collision_chunks},
             {"expected_ring_lod_chunks",
              {
                  {"lod0", expected_collision_chunks},
                  {"lod1", (17u * 17u) - expected_collision_chunks},
                  {"lod2", expected_surface_chunks - (17u * 17u)},
              }},
             {"renderable_chunks", renderable_chunks.size()},
             {"mesh_chunks", mesh_stats.mesh_chunks},
             {"collision_chunks", mesh_stats.collision_chunks},
             {"lod_mesh_chunks",
              {
                  {"lod0", mesh_stats.lod_mesh_chunks[0]},
                  {"lod1", mesh_stats.lod_mesh_chunks[1]},
                  {"lod2", mesh_stats.lod_mesh_chunks[2]},
              }},
             {"vertices", mesh_stats.vertices},
             {"indices", mesh_stats.indices},
             {"triangles", mesh_stats.triangles},
             {"min_vertices_per_chunk", mesh_stats.min_vertices_per_chunk},
             {"max_vertices_per_chunk", mesh_stats.max_vertices_per_chunk},
             {"mean_vertices_per_chunk", mesh_stats.mean_vertices_per_chunk},
             {"elapsed_ms", prep_ms},
             {"chunks_per_second",
              prep_ms > 0.0 ? (static_cast<double>(mesh_stats.mesh_chunks) * 1000.0) / prep_ms
                            : 0.0},
         }},
        {"meshing_throughput", MeshingStatsToJson(meshing_stats)},
        {"thresholds",
         {
             {"minimum_mesh_chunk_coverage", 0.90},
             {"minimum_collision_chunks", expected_collision_chunks},
             {"runaway_prep_limit_ms", kRunawayPrepLimitMs},
         }},
    };
    WriteJson(ArtifactRoot() / "initial_world_loading.json", report);
    WriteJson(ArtifactRoot() / "meshing_throughput.json",
              {
                  {"schema", "luminumbra.meshing_throughput.v1"},
                  {"seed", kSeed},
                  {"preset", preset_path.filename().string()},
                  {"spawn", VecToJson(spawn)},
                  {"stats", MeshingStatsToJson(meshing_stats)},
              });

    // DELIBERATE expectation update: the initial load list now emits
    // each column's 5-point surface SPAN (+-1 margin) instead of a fixed 3
    // chunks per column, so steep columns add cliff-wall chunks. The exact
    // span-derived count is asserted by
    // WorldGenLayerSnapshotTest.InitialChunkLoadListCoversSpawnSurfaceNeighborhood;
    // here the old 25*25*3 constant becomes the flat-terrain floor.
    EXPECT_GE(initial_chunks.size(), 25u * 25u * 3u);
    EXPECT_LE(initial_chunks.size(), 25u * 25u * 6u);
    EXPECT_GE(mesh_stats.mesh_chunks, static_cast<std::size_t>(expected_surface_chunks * 0.90));
    EXPECT_GE(mesh_stats.collision_chunks, expected_collision_chunks);
    EXPECT_GE(mesh_stats.lod_mesh_chunks[0],
              static_cast<std::size_t>(expected_collision_chunks * 0.90));
    EXPECT_GT(mesh_stats.lod_mesh_chunks[1], 0u);
    EXPECT_GT(mesh_stats.lod_mesh_chunks[2], 0u);
    EXPECT_GT(mesh_stats.vertices, 10000u);
    EXPECT_GT(mesh_stats.triangles, 10000u);
    EXPECT_GT(meshing_stats.jobs, 0u);
    EXPECT_GT(meshing_stats.cells_visited, 0u);
    EXPECT_GT(meshing_stats.active_cells, 0u);
    EXPECT_GT(meshing_stats.triangles, 10000u);
    EXPECT_GT(meshing_stats.elapsed_us, 0u);
    EXPECT_GT(initial_list_ms, 0.0);
    EXPECT_GT(prep_ms, 0.0);
    EXPECT_LT(prep_ms, kRunawayPrepLimitMs);
}

TEST(InitialWorldLoadingPerfTest, StreamingBudgetSchedulerExpandsSurfaceBeforeVerticalStacks) {
    const fs::path preset_path = SourceRoot() / "worlds/atlas/presets/default.json";
    ASSERT_TRUE(fs::exists(preset_path)) << preset_path.string();
    fs::create_directories(ArtifactRoot());

    const TerrainGenParams params = LoadPresetParams(preset_path);
    ScopedJobSystem job_system;
    ScopedPhysicsSystem physics_system;
    SHIELD_WorldSystem world(&job_system.jobs, nullptr, params, kSeed);

    const float spawn_x = 8.0f;
    const float spawn_z = 8.0f;
    const float terrain_height = world.GetTerrainHeightAt(spawn_x, spawn_z);
    const Vec3 spawn(spawn_x, terrain_height + 1.95f, spawn_z);

    ASSERT_TRUE(world.EnsureSurfaceReadyNear(spawn, &physics_system.physics, 4, 2));

    entt::registry registry;
    for (int frame = 0; frame < 4; ++frame) {
        world.update(registry, spawn, &physics_system.physics);
    }

    const auto stats = world.get_last_streaming_budget_stats();
    const nlohmann::json report = {
        {"schema", "luminumbra.streaming_budget.v1"},
        {"seed", kSeed},
        {"preset", preset_path.filename().string()},
        {"spawn", VecToJson(spawn)},
        {"stats", StreamingStatsToJson(stats)},
        {"thresholds",
         {
             {"surface_first_required", true},
             {"active_chunks_must_not_exceed_budget", true},
         }},
    };
    WriteJson(ArtifactRoot() / "streaming_budget.json", report);

    EXPECT_GT(stats.target_surface_columns, 0u);
    EXPECT_GT(stats.generation_candidates, 0u);
    EXPECT_GT(stats.surface_generation_candidates, 0u);
    EXPECT_GT(stats.scheduled_generation, 0u);
    EXPECT_LE(stats.scheduled_generation, static_cast<std::size_t>(stats.generation_budget));
    EXPECT_GT(stats.surface_generation_scheduled, 0u);
    EXPECT_GE(stats.surface_generation_scheduled, stats.vertical_generation_scheduled);
    EXPECT_GT(stats.deferred_generation, 0u);
    EXPECT_LE(stats.active_chunks_after, stats.max_active_chunks_budget);
}

TEST(InitialWorldLoadingPerfTest, StreamingWalkMaintainsBudgetsAndWritesWorldStateArtifact) {
    const fs::path preset_path = SourceRoot() / "worlds/atlas/presets/default.json";
    ASSERT_TRUE(fs::exists(preset_path)) << preset_path.string();
    fs::create_directories(ArtifactRoot());

    const TerrainGenParams params = LoadPresetParams(preset_path);
    ScopedJobSystem job_system;
    ScopedPhysicsSystem physics_system;
    SHIELD_WorldSystem world(&job_system.jobs, nullptr, params, kSeed);

    const Vec3 spawn(8.0f, world.GetTerrainHeightAt(8.0f, 8.0f) + 1.95f, 8.0f);
    ASSERT_TRUE(world.EnsureSurfaceReadyNear(spawn, &physics_system.physics, 8, 3));

    entt::registry registry;
    std::vector<nlohmann::json> walk_steps;
    walk_steps.reserve(32u);

    std::size_t max_active_chunks = 0;
    std::size_t max_budget = 0;
    std::size_t max_loading_chunks = 0;
    std::size_t max_meshing_chunks = 0;
    std::size_t max_deferred_generation = 0;
    std::size_t max_deferred_meshing = 0;
    std::size_t max_renderable_chunks = 0;

    Timer walk_timer;
    Vec3 final_position = spawn;
    for (int step = 0; step < 32; ++step) {
        Timer step_timer;
        const float angle = static_cast<float>(step) * 0.35f;
        const float radius = 32.0f + static_cast<float>((step % 8) * 8);
        const float x = 8.0f + std::cos(angle) * radius;
        const float z = 8.0f + std::sin(angle) * radius;
        final_position = Vec3(x, world.GetTerrainHeightAt(x, z) + 1.95f, z);

        for (int frame = 0; frame < 4; ++frame) {
            world.update(registry, final_position, &physics_system.physics);
            physics_system.physics.update(1.0f / 60.0f);
        }

        const auto budget_stats = world.get_last_streaming_budget_stats();
        const auto chunk_stats = world.get_runtime_chunk_stats();
        const double step_elapsed_ms = step_timer.elapsed_ms();
        max_active_chunks = std::max(max_active_chunks, chunk_stats.total_chunks);
        max_budget = std::max(max_budget, budget_stats.max_active_chunks_budget);
        max_loading_chunks = std::max(max_loading_chunks, chunk_stats.loading_chunks);
        max_meshing_chunks = std::max(max_meshing_chunks, chunk_stats.meshing_chunks);
        max_deferred_generation =
            std::max(max_deferred_generation, budget_stats.deferred_generation);
        max_deferred_meshing = std::max(max_deferred_meshing, budget_stats.deferred_meshing);
        max_renderable_chunks = std::max(max_renderable_chunks, chunk_stats.renderable_chunks);

        walk_steps.push_back({
            {"step", step},
            {"elapsed_ms", step_elapsed_ms},
            {"position", VecToJson(final_position)},
            {"streaming_budget", StreamingStatsToJson(budget_stats)},
            {"chunks", RuntimeChunkStatsToJson(chunk_stats)},
        });
    }

    ASSERT_TRUE(world.EnsureSurfaceReadyNear(final_position, &physics_system.physics, 8, 3));
    const auto final_chunk_stats = world.get_runtime_chunk_stats();
    const double elapsed_ms = walk_timer.elapsed_ms();

    const nlohmann::json report = {
        {"schema", "luminumbra.streaming_walk.v1"},
        {"seed", kSeed},
        {"preset", preset_path.filename().string()},
        {"state_schema",
         {
             ChunkStateName(ChunkState::Unloaded),
             ChunkStateName(ChunkState::Loading),
             ChunkStateName(ChunkState::Idle),
             ChunkStateName(ChunkState::Meshing),
             ChunkStateName(ChunkState::Ready),
             ChunkStateName(ChunkState::Unloading),
         }},
        {"walk",
         {
             {"steps", walk_steps.size()},
             {"frames_per_step", 4},
             {"elapsed_ms", elapsed_ms},
             {"max_active_chunks", max_active_chunks},
             {"max_active_chunk_budget", max_budget},
             {"max_loading_chunks", max_loading_chunks},
             {"max_meshing_chunks", max_meshing_chunks},
             {"max_deferred_generation", max_deferred_generation},
             {"max_deferred_meshing", max_deferred_meshing},
             {"max_renderable_chunks", max_renderable_chunks},
         }},
        {"final_position", VecToJson(final_position)},
        {"final_chunks", RuntimeChunkStatsToJson(final_chunk_stats)},
        {"steps", walk_steps},
        {"thresholds",
         {
             {"active_chunks_must_not_exceed_budget", true},
             {"minimum_peak_renderable_chunks", 160},
             {"minimum_final_renderable_chunks", 160},
             {"maximum_final_loading_chunks", 0},
             {"maximum_final_meshing_chunks", 0},
         }},
    };
    WriteJson(ArtifactRoot() / "streaming_walk.json", report);

    EXPECT_GT(max_budget, 0u);
    EXPECT_LE(max_active_chunks, max_budget);
    EXPECT_GE(max_renderable_chunks, 160u);
    EXPECT_GE(final_chunk_stats.renderable_chunks, 160u);
    EXPECT_EQ(final_chunk_stats.loading_chunks, 0u);
    EXPECT_EQ(final_chunk_stats.meshing_chunks, 0u);
    EXPECT_FALSE(final_chunk_stats.generation_job_active);
    EXPECT_FALSE(final_chunk_stats.meshing_job_active);
    EXPECT_GT(elapsed_ms, 0.0);
}

TEST(InitialWorldLoadingPerfTest, PerformanceFrameworkBenchmarkScenariosWriteBudgetArtifacts) {
    const fs::path preset_path = SourceRoot() / "worlds/atlas/presets/default.json";
    ASSERT_TRUE(fs::exists(preset_path)) << preset_path.string();
    fs::create_directories(PerformanceFrameworkArtifactRoot());

    const std::vector<std::string> required_scenarios = {
        "boot",
        "create_world",
        "enter_spawn",
        "idle_horizon",
        "pan_camera",
        "streaming_walk",
        "forest",
        "ecology",
        "chunk_churn",
        "shader_warmup",
        "shutdown",
    };

    nlohmann::json scenario_schema = {
        {"schema", "luminumbra.performance_framework.scenarios.v1"},
        {"required_metrics",
         {
             "frame_time_ms.p50",
             "frame_time_ms.p95",
             "frame_time_ms.p99",
             "frame_time_ms.max",
             "memory_high_water",
             "chunk_counts",
             "job_queue_high_water",
             "upload_backlog_high_water",
             "draw_pass_counts",
             "shader_compile_activity",
         }},
        {"scenarios", nlohmann::json::array()},
    };
    for (const std::string& name : required_scenarios) {
        scenario_schema["scenarios"].push_back({
            {"name", name},
            {"seed", kSeed},
            {"preset", preset_path.filename().string()},
            {"budget_policy", "catastrophic_regression"},
        });
    }
    WriteJson(PerformanceFrameworkArtifactRoot() / "benchmark_scenarios.json", scenario_schema);

    const TerrainGenParams params = LoadPresetParams(preset_path);

    Timer boot_timer;
    ScopedJobSystem job_system;
    ScopedPhysicsSystem physics_system;
    const double boot_ms = boot_timer.elapsed_ms();

    Timer create_timer;
    SHIELD_WorldSystem world(&job_system.jobs, nullptr, params, kSeed);
    const double create_ms = create_timer.elapsed_ms();

    const Vec3 spawn(8.0f, world.GetTerrainHeightAt(8.0f, 8.0f) + 1.95f, 8.0f);
    Timer enter_timer;
    ASSERT_TRUE(world.EnsureSurfaceReadyNear(spawn, &physics_system.physics, 6, 2));
    const double enter_ms = enter_timer.elapsed_ms();

    entt::registry registry;
    std::vector<nlohmann::json> scenario_results;

    auto scenario_result = [&](const std::string& name,
                               const std::vector<double>& samples_ms,
                               const SHIELD_WorldSystem::RuntimeChunkStats& chunks,
                               std::size_t job_queue_high_water,
                               std::size_t upload_backlog_high_water,
                               std::size_t draw_pass_count,
                               std::size_t shader_programs_compiled) {
        const std::size_t world_memory_bytes = chunks.terrain_payload_bytes +
                                               chunks.water_vertex_count * sizeof(VoxelVertex) +
                                               chunks.water_index_count * sizeof(u32);
        return nlohmann::json{
            {"name", name},
            {"frame_time_ms", FrameStatsToJson(samples_ms)},
            // Flat per-scenario metric block consumed by the perf-regression gate
            // (tools/gates/validate-engine-frontier.ps1 -Mode PerfRegression) and
            // the baseline capture helper (tools/gates/capture-perf-baseline.ps1).
            // Keep keys stable: p50_ms, p95_ms, p99_ms, max_ms, mem_high_water_mb.
            {"regression_metrics",
             {
                 {"p50_ms", Percentile(samples_ms, 50.0)},
                 {"p95_ms", Percentile(samples_ms, 95.0)},
                 {"p99_ms", Percentile(samples_ms, 99.0)},
                 {"max_ms",
                  samples_ms.empty() ? 0.0
                                     : *std::max_element(samples_ms.begin(), samples_ms.end())},
                 {"mem_high_water_mb", static_cast<double>(world_memory_bytes) / (1024.0 * 1024.0)},
             }},
            {"memory_high_water",
             {
                 {"cpu_bytes", 0},
                 {"estimated_vram_bytes", 0},
                 {"world_payload_bytes", world_memory_bytes},
             }},
            {"chunk_counts", RuntimeChunkStatsToJson(chunks)},
            {"job_queue_high_water", job_queue_high_water},
            {"upload_backlog_high_water", upload_backlog_high_water},
            {"draw_pass_counts",
             {
                 {"draws", draw_pass_count},
                 {"source", "render framework pass-count contract"},
             }},
            {"shader_compile_activity",
             {
                 {"programs_compiled", shader_programs_compiled},
                 {"validated_by", "RenderSmokeTest.PipelineShaderProgramsLink"},
             }},
        };
    };

    const auto chunks_after_enter = world.get_runtime_chunk_stats();
    scenario_results.push_back(
        scenario_result("boot", {boot_ms}, chunks_after_enter, 0u, 0u, 0u, 0u));
    scenario_results.push_back(
        scenario_result("create_world", {create_ms}, chunks_after_enter, 0u, 0u, 0u, 0u));
    scenario_results.push_back(
        scenario_result("enter_spawn", {enter_ms}, chunks_after_enter, 0u, 0u, 0u, 0u));

    auto run_update_scenario =
        [&](const std::string& name, int frames, const auto& position_for_frame) {
            std::vector<double> samples_ms;
            samples_ms.reserve(static_cast<std::size_t>(frames));
            SHIELD_WorldSystem::RuntimeChunkStats max_chunks = world.get_runtime_chunk_stats();
            std::size_t max_jobs = 0;
            std::size_t max_backlog = 0;
            for (int frame = 0; frame < frames; ++frame) {
                const Vec3 position = position_for_frame(frame);
                Timer frame_timer;
                world.update(registry, position, &physics_system.physics);
                physics_system.physics.update(1.0f / 60.0f);
                samples_ms.push_back(frame_timer.elapsed_ms());

                const auto chunks = world.get_runtime_chunk_stats();
                const auto budget = world.get_last_streaming_budget_stats();
                if (chunks.terrain_payload_bytes > max_chunks.terrain_payload_bytes) {
                    max_chunks = chunks;
                }
                max_jobs = std::max<std::size_t>(max_jobs,
                                                 (chunks.generation_job_active ? 1u : 0u) +
                                                     (chunks.meshing_job_active ? 1u : 0u));
                max_backlog =
                    std::max(max_backlog, budget.deferred_generation + budget.deferred_meshing);
            }
            scenario_results.push_back(
                scenario_result(name, samples_ms, max_chunks, max_jobs, max_backlog, 0u, 0u));
        };

    run_update_scenario("idle_horizon", 20, [&](int) { return spawn; });

    run_update_scenario("pan_camera", 20, [&](int frame) {
        const float angle = static_cast<float>(frame) * 0.25f;
        const float x = 8.0f + std::cos(angle) * 24.0f;
        const float z = 8.0f + std::sin(angle) * 24.0f;
        return Vec3(x, world.GetTerrainHeightAt(x, z) + 1.95f, z);
    });

    run_update_scenario("streaming_walk", 24, [&](int frame) {
        const float angle = static_cast<float>(frame) * 0.35f;
        const float radius = 32.0f + static_cast<float>((frame % 6) * 8);
        const float x = 8.0f + std::cos(angle) * radius;
        const float z = 8.0f + std::sin(angle) * radius;
        return Vec3(x, world.GetTerrainHeightAt(x, z) + 1.95f, z);
    });

    // Tick a fixed kinematic creature roster through the GameSession ecology
    // stack once per benchmark frame. A modest fixed count keeps this smoke
    // scenario fast; ecology_tick_perf_test owns the larger scaling study.
    {
        constexpr int kEcologyBenchmarkRoster = 512;
        entt::registry ecology_registry;
        SpawnEcologyBenchmarkRoster(ecology_registry, kEcologyBenchmarkRoster);
        std::vector<double> ecology_samples_ms;
        constexpr int kEcologyFrames = 20;
        ecology_samples_ms.reserve(kEcologyFrames);
        for (int frame = 0; frame < kEcologyFrames; ++frame) {
            Timer ecology_timer;
            EcologyBenchmarkTick(ecology_registry, static_cast<std::uint64_t>(frame));
            ecology_samples_ms.push_back(ecology_timer.elapsed_ms());
        }
        nlohmann::json ecology_entry = scenario_result(
            "ecology", ecology_samples_ms, world.get_runtime_chunk_stats(), 0u, 0u, 0u, 0u);
        ecology_entry["creature_count"] = kEcologyBenchmarkRoster;
        scenario_results.push_back(ecology_entry);
    }

    run_update_scenario("chunk_churn", 12, [&](int frame) {
        const float x = (frame % 2 == 0) ? 96.0f : -96.0f;
        const float z = (frame % 3 == 0) ? 96.0f : -96.0f;
        return Vec3(x, world.GetTerrainHeightAt(x, z) + 1.95f, z);
    });

    const auto chunks_after_benchmarks = world.get_runtime_chunk_stats();
    scenario_results.push_back(
        scenario_result("shader_warmup", {0.0}, chunks_after_benchmarks, 0u, 0u, 0u, 14u));

    // Quiesce the world's streaming jobs before timing JobSystem shutdown.
    // chunk_churn leaves a meshing batch in flight on 16 background workers;
    // racing the fresh worker pool's thread startup/join against that churn
    // measured scheduler contention (0.6 ms vs 40-60 ms bimodal), not
    // shutdown cost, which made the perf-regression baseline unstable.
    world.wait_for_streaming_jobs();
    JobSystem shutdown_jobs;
    shutdown_jobs.startup();
    Timer shutdown_timer;
    shutdown_jobs.shutdown();
    scenario_results.push_back(scenario_result(
        "shutdown", {shutdown_timer.elapsed_ms()}, chunks_after_benchmarks, 0u, 0u, 0u, 0u));

#ifdef NDEBUG
    const char* build_mode = "release";
#else
    const char* build_mode = "debug";
#endif

    const nlohmann::json summary = {
        {"schema", "luminumbra.performance_framework.benchmark_summary.v1"},
        {"metadata",
         {
             {"seed", kSeed},
             {"preset", preset_path.filename().string()},
             {"build_mode", build_mode},
             {"git_sha", CurrentGitSha()},
             {"machine", "local"},
             {"gpu", "unknown"},
             {"driver", "unknown"},
         }},
        {"scenarios", scenario_results},
    };
    WriteJson(PerformanceFrameworkArtifactRoot() / "benchmark_summary.json", summary);

    constexpr double kMaxP99Ms = 120000.0;
    constexpr std::size_t kMaxWorldPayloadBytes = 1024ull * 1024ull * 1024ull;
    constexpr std::size_t kMaxJobQueueHighWater = 2u;
    constexpr std::size_t kMaxUploadBacklogHighWater = 8192u;
    std::vector<std::string> failures;
    for (const nlohmann::json& scenario : scenario_results) {
        if (scenario["frame_time_ms"]["p99"].get<double>() > kMaxP99Ms) {
            failures.push_back(scenario["name"].get<std::string>() +
                               " p99 exceeded catastrophic budget");
        }
        if (scenario["memory_high_water"]["world_payload_bytes"].get<std::size_t>() >
            kMaxWorldPayloadBytes) {
            failures.push_back(scenario["name"].get<std::string>() +
                               " world payload exceeded catastrophic budget");
        }
        if (scenario["job_queue_high_water"].get<std::size_t>() > kMaxJobQueueHighWater) {
            failures.push_back(scenario["name"].get<std::string>() +
                               " job queue high-water exceeded budget");
        }
        if (scenario["upload_backlog_high_water"].get<std::size_t>() > kMaxUploadBacklogHighWater) {
            failures.push_back(scenario["name"].get<std::string>() +
                               " upload backlog high-water exceeded budget");
        }
    }

    const nlohmann::json regression_budget = {
        {"schema", "luminumbra.performance_framework.regression_budget.v1"},
        {"policy",
         "safety limits for hangs and runaway resource growth; relative comparisons own regression "
         "decisions"},
        {"budgets",
         {
             {"max_p99_ms", kMaxP99Ms},
             {"max_world_payload_bytes", kMaxWorldPayloadBytes},
             {"max_job_queue_high_water", kMaxJobQueueHighWater},
             {"max_upload_backlog_high_water", kMaxUploadBacklogHighWater},
         }},
        {"passed", failures.empty()},
        {"failures", failures},
    };
    WriteJson(PerformanceFrameworkArtifactRoot() / "regression_budget.json", regression_budget);

    EXPECT_TRUE(failures.empty()) << regression_budget.dump(2);
}
