#include "../fixtures/TerrainFixtureParams.h"
#include "../support/DeterministicWorkBudget.h"

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_server/ServerStreamingBudget.h"
#include "luminumbra_server/SimBudgetArtifact.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using namespace Luminumbra;
using luminumbra::simulation::SimBudgetTelemetry;
using luminumbra::simulation::SimWork;
using TestSupport::CheckWork;
using TestSupport::CompareWork;
using TestSupport::WorkCounts;
using TestSupport::WorkLimits;

TEST(DeterministicWorkBudget, EveryPinnedLimitRejectsAnInjectedExcess) {
    const auto data = TestSupport::WorkBudgetData();
    ASSERT_EQ(data.size(), 3u);
    for (const auto& [scenario, values] : data.items()) {
        (void)values;
        const auto limits = WorkLimits(scenario);
        for (const auto& [counter, limit] : limits) {
            SCOPED_TRACE(scenario + "/" + counter);
            auto observed = limits;
            std::ostringstream accepted_report;
            EXPECT_TRUE(CompareWork(observed, limits, accepted_report));
            // Every reviewed ceiling is small enough that +1 cannot overflow.
            ASSERT_LT(limit, std::numeric_limits<std::uint64_t>::max());
            observed.at(counter) = limit + 1;
            std::ostringstream rejected_report;
            EXPECT_FALSE(CompareWork(observed, limits, rejected_report));
            EXPECT_NE(rejected_report.str().find(counter +
                                                 ": observed=" + std::to_string(limit + 1) +
                                                 " limit=" + std::to_string(limit)),
                      std::string::npos);
        }
    }
}

TEST(DeterministicWorkBudget, IdleRejectsOneUploadByteAndOneHashByteIndependently) {
    const auto limits = WorkLimits("idle");
    const std::array<std::string, 2> byte_counters{"uploaded_bytes", "hashed_bytes"};
    for (const auto& counter : byte_counters) {
        SCOPED_TRACE(counter);
        ASSERT_EQ(limits.at(counter), 0u);
        auto observed = limits;
        observed.at(counter) = 1;
        std::ostringstream report;
        EXPECT_FALSE(CompareWork(observed, limits, report));
        EXPECT_NE(report.str().find(counter + ": observed=1 limit=0"), std::string::npos);
    }
}

TEST(DeterministicWorkBudget, MissingOrExtraMeasurementsRefuse) {
    const auto limits = WorkLimits("simulation");
    auto observed = limits;
    observed.erase(observed.begin());
    std::ostringstream missing;
    EXPECT_FALSE(CompareWork(observed, limits, missing));
    observed = limits;
    observed.emplace("unknown", 0);
    std::ostringstream extra;
    EXPECT_FALSE(CompareWork(observed, limits, extra));
}

TEST(DeterministicWorkBudget, LiveSubsystemMovementEditAndDrainStayBounded) {
    const auto limits = WorkLimits("simulation");
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0;
    params.peaks_amplitude = 0;
    params.height_offset = 32.5f;
    params.continental_spline.clear();
    params.caves_enabled = false;
    JobSystem jobs;
    jobs.startup(1);
    // Destruction order keeps the worker host alive until the world drains.
    Systems::SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
    world.debug_set_streaming_radius_cap(1);
    Systems::PhysicsSystem physics;
    physics.startup();
    const auto sphere = physics.create_dynamic_sphere({8, 40, 8}, {0, 0, 0}, 0.25f);
    physics.create_player_controller({8, 40, 8});
    physics.create_avatar_character({9, 40, 8});
    entt::registry registry;
    Systems::WaterSystem water(nullptr, &world);
    water.SetSimResolution(8);
    water.SetHydrology(false, 0, 0);
    auto wet = std::make_shared<Chunk>(IVec3(0, 0, 0));
    wet->has_water_sim = true;
    wet->current_water_resolution = 8;
    // Exactly one 8x8 fixed-point water tile, independent of streaming terrain.
    wet->water_depth_mm.assign(64, 1000);
    wet->water_bed_mm.assign(64, 1000);
    wet->water_src_mm.assign(64, 0);
    wet->water_level_data.assign(64, 2.0f);
    wet->water_flow_data.assign(64, Vec2(0.0f));
    wet->water_sim_terrain_height.assign(64, 1.0f);
    wet->water_rest_level.assign(64, 2.0f);
    const std::unordered_map<ChunkID, std::shared_ptr<Chunk>> water_chunks{{wet->get_id(), wet}};
    ::Luminumbra::world::ActiveRegionLedger ledger;
    ::Luminumbra::world::WorldClock clock;
    SimBudgetTelemetry telemetry;
    telemetry.SetEnabled(true);
    Server::ServerStreamingBudget<> observer(telemetry);
    std::uint64_t previous_backlog = 0;
    std::uint64_t generated = 0, meshed = 0, queries = 0, copies = 0;
    bool drained = false;
    WorkCounts peak;
    for (std::uint64_t step = 1; step <= limits.at("drain_steps"); ++step) {
        SCOPED_TRACE(step);
        // Move one streaming column at step 8; edit loaded support at step 24.
        const Vec3 anchor{step < 8 ? 8.0f : 24.0f, 35, 8};
        if (step == 24)
            ASSERT_GT(world.EditTerrainVoxel({24, 32.5f, 8}, 1.5f, false, nullptr), 0);
        physics.update(1.0f / 30.0f);
        physics.update_player({0, 0, 0}, false, 0, 1.0f / 30.0f);
        physics.update_avatars(1.0f / 30.0f);
        wet->is_water_sleeping = false;
        water.update(registry, water_chunks);
        clock.set_tick(step);
        ledger.set_local_anchor(step < 8 ? Vec3{8, 0, 8} : Vec3{520, 0, 8});
        const auto schedule = ledger.schedule(clock);
        const auto snapshot_before = world.snapshot_copies();
        world.begin_tick(static_cast<std::int64_t>(step));
        // Both public overloads share the same work contract.
        if (step % 2 == 0)
            world.update(registry, std::vector<Vec3>{anchor}, nullptr);
        else
            world.update(registry, anchor, nullptr);
        world.activate_due(static_cast<std::int64_t>(step));
        const auto snapshot = world.snapshot_streamed_chunks();
        const auto renderable = world.get_renderable_chunks();
        const auto snapshot_work = world.snapshot_copies() - snapshot_before;
        EXPECT_EQ(snapshot_work, snapshot.size() + renderable.size());
        const auto& streaming = world.get_last_streaming_budget_stats();
        const auto physics_work = physics.work_items();
        const auto runtime = world.get_runtime_chunk_stats();
        const auto backlog = runtime.loading_chunks + runtime.idle_chunks + runtime.meshing_chunks +
                             streaming.deferred_generation + streaming.deferred_meshing;
        const auto growth = backlog > previous_backlog ? backlog - previous_backlog : 0;
        previous_backlog = backlog;
        const WorkCounts observed{{"water_cells", water.cells_stepped_last_update()},
                                  {"streaming_generation", streaming.scheduled_generation},
                                  {"streaming_meshing", streaming.scheduled_meshing},
                                  {"physics_bodies", physics_work.bodies},
                                  {"physics_characters", physics_work.characters},
                                  {"active_region_records", schedule.ledger_records},
                                  {"active_region_due", schedule.due.size()},
                                  {"snapshot_copies", snapshot_work},
                                  {"activation_queries", streaming.target_surface_columns},
                                  {"backlog", backlog},
                                  {"backlog_growth", growth},
                                  {"drain_steps", step}};
        EXPECT_TRUE(CheckWork(observed, "simulation"));
        TestSupport::PeakWork(peak, observed);
        EXPECT_EQ(water.cells_stepped_last_update(), 64u);
        EXPECT_EQ(physics_work.bodies, 1u);
        EXPECT_EQ(physics_work.characters, 2u);
        EXPECT_FALSE(schedule.due.empty());
        generated += streaming.scheduled_generation;
        meshed += streaming.scheduled_meshing;
        queries += streaming.target_surface_columns;
        copies += snapshot_work;
        observer.RecordWorkItems(
            step, streaming, physics_work, water.cells_stepped_last_update(), snapshot_work);
        for (std::size_t i = 0; i < luminumbra::simulation::kSimWorkNames.size(); ++i) {
            if (i == static_cast<std::size_t>(SimWork::ActiveRegionRecords) ||
                i == static_cast<std::size_t>(SimWork::ActiveRegionDue))
                continue; // Session-owned counters are tested through TickSimulation below.
            const auto& samples = telemetry.WorkItems()[i];
            ASSERT_EQ(samples.size(), step);
            EXPECT_EQ(samples.back().tick, step);
            EXPECT_EQ(samples.back().work,
                      observed.at(std::string(luminumbra::simulation::kSimWorkNames[i])));
        }
        if (step >= 32 && backlog == 0) {
            drained = true;
            break;
        }
    }
    EXPECT_TRUE(drained) << "streaming backlog failed to reach zero within the pinned step bound";
    EXPECT_GT(generated, 0u);
    EXPECT_GT(meshed, 0u);
    EXPECT_GT(queries, 0u);
    EXPECT_GT(copies, 0u);
    EXPECT_TRUE(CheckWork(peak, "simulation"));
    physics.destroy_body(sphere);
}

TEST(DeterministicWorkBudget, SessionRegionCountersLeaveCanonicalIdentityUnchanged) {
    ::Luminumbra::world::GameSession unobserved;
    ::Luminumbra::world::GameSession observed;
    unobserved.SetActiveRegionsEnabled(true);
    observed.SetActiveRegionsEnabled(true);
    observed.SetSimBudgetTelemetryEnabled(true);
    for (std::uint64_t step = 1; step <= 2; ++step) {
        const Vec3 anchor{step == 1 ? 8.0f : 520.0f, 0, 8};
        for (auto* session : {&unobserved, &observed}) {
            session->SetLocalPlayerSimulationPosition(anchor, true);
            session->NotifyGroundObjectEdit(anchor);
            ASSERT_EQ(session->TickSimulation(1.0 / 30.0), 1u);
            ASSERT_FALSE(session->GetRegionSchedulerFault());
        }
        EXPECT_EQ(unobserved.GetWorldClock().canonical_bytes(),
                  observed.GetWorldClock().canonical_bytes());
        EXPECT_EQ(unobserved.GetActiveRegionLedger().canonical_bytes(),
                  observed.GetActiveRegionLedger().canonical_bytes());
        EXPECT_EQ(unobserved.GetRegionSchedule(), observed.GetRegionSchedule());
        const auto& schedule = observed.GetRegionSchedule();
        ASSERT_GT(schedule.ledger_records, 0u);
        ASSERT_FALSE(schedule.due.empty());
        const auto& trace = observed.GetSimBudgetTelemetry().WorkItems();
        const auto& records = trace[static_cast<std::size_t>(SimWork::ActiveRegionRecords)];
        const auto& due = trace[static_cast<std::size_t>(SimWork::ActiveRegionDue)];
        ASSERT_EQ(records.size(), step);
        ASSERT_EQ(due.size(), step);
        EXPECT_EQ(records.back().tick, step);
        EXPECT_EQ(records.back().work, schedule.ledger_records);
        EXPECT_EQ(due.back().tick, step);
        EXPECT_EQ(due.back().work, schedule.due.size());
        for (const auto& samples : unobserved.GetSimBudgetTelemetry().WorkItems())
            EXPECT_TRUE(samples.empty());
    }
}

TEST(DeterministicWorkBudget, WorkTraceIsDiagnosticAndRejectsCounterOrTickDrift) {
    SimBudgetTelemetry first;
    SimBudgetTelemetry replay;
    first.SetEnabled(true);
    replay.SetEnabled(true);
    for (std::size_t i = 0; i < luminumbra::simulation::kSimWorkNames.size(); ++i) {
        const auto counter = static_cast<SimWork>(i);
        first.RecordWork(counter, 1, i);
        replay.RecordWork(counter, 1, i);
        EXPECT_TRUE(first.WorkMatches(replay));
    }
    nlohmann::json artifact;
    Server::AppendSimBudgetArtifact(artifact, first, replay);
    const auto& work = artifact.at("sim_budget").at("work_items");
    EXPECT_EQ(work.at("schema"), "luminumbra.sim_work.v1");
    ASSERT_EQ(work.at("counters").size(), luminumbra::simulation::kSimWorkNames.size());
    for (std::size_t i = 0; i < luminumbra::simulation::kSimWorkNames.size(); ++i) {
        const auto& counter = work.at("counters").at(i);
        EXPECT_EQ(counter.at("name"), luminumbra::simulation::kSimWorkNames[i]);
        EXPECT_EQ(counter.at("work_trace"), nlohmann::json::array({{{"tick", 1}, {"work", i}}}));
    }
    // Every supplemental counter participates in deterministic replay refusal.
    for (std::size_t changed = 0; changed < luminumbra::simulation::kSimWorkNames.size();
         ++changed) {
        for (const bool change_tick : {false, true}) {
            replay.Clear();
            for (std::size_t i = 0; i < luminumbra::simulation::kSimWorkNames.size(); ++i)
                replay.RecordWork(static_cast<SimWork>(i),
                                  change_tick && i == changed ? 2 : 1,
                                  !change_tick && i == changed ? i + 1 : i);
            EXPECT_FALSE(first.WorkMatches(replay));
        }
    }
    replay = first;
    replay.RecordWork(SimWork::WaterCells, 2, 0);
    EXPECT_FALSE(first.WorkMatches(replay));
    first.RecordWork(SimWork::WaterCells, 2, 1);
    EXPECT_FALSE(first.WorkMatches(replay));
    replay.SetEnabled(false);
    replay.RecordWork(SimWork::WaterCells, 1, 1);
    for (const auto& samples : replay.WorkItems())
        EXPECT_TRUE(samples.empty());
    first.Clear();
    EXPECT_TRUE(first.WorkMatches(replay));
}
} // namespace
