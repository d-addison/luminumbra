#include "../fixtures/TerrainFixtureParams.h"
#include "../support/DeterministicWorkBudget.h"

#include "luminumbra_client/rendering/UploadBudget.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/MarchingCubes.h"
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

Systems::TerrainGenParams FlatWorkBudgetTerrain() {
    auto params = Test::SyntheticTerrainParams();
    params.base_amplitude = 0;
    params.peaks_amplitude = 0;
    params.height_offset = 32.5f;
    params.continental_spline.clear();
    params.caves_enabled = false;
    return params;
}

std::uint64_t StreamingBacklog(const Systems::SHIELD_WorldSystem& world) {
    const auto runtime = world.get_runtime_chunk_stats();
    const auto& streaming = world.get_last_streaming_budget_stats();
    // Loading, Meshing and Ready remesh/promotion items all belong to the
    // publication queues. Idle chunks still need dispatch. Do not substitute
    // worker activity: completed jobs can still be waiting for their due tick.
    return world.pending_publication_chunks() + runtime.idle_chunks +
           streaming.deferred_generation +
           (streaming.meshing_candidates - streaming.submitted_meshing);
}

TEST(DeterministicWorkBudget, ReadyPendingLodKeepsBacklogUntilRemeshPublication) {
    for (const bool needs_promotion : {false, true}) {
        SCOPED_TRACE(needs_promotion);
        const auto params = FlatWorkBudgetTerrain();
        JobSystem jobs;
        jobs.startup(1);
        Systems::SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
        world.debug_set_streaming_radius_cap(1);
        entt::registry registry;
        const Vec3 anchor{8, 35, 8};
        auto chunk =
            std::make_shared<Chunk>(Systems::SHIELD_WorldSystem::world_to_chunk_coords(anchor));
        world.GenerateChunkData(*chunk, needs_promotion ? 2 : 1);
        World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 2);
        ASSERT_FALSE(chunk->mesh_vertices.empty());
        ASSERT_FALSE(chunk->mesh_indices.empty());
        chunk->current_lod = 1;
        chunk->set_state(ChunkState::Ready);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        const auto old_version = chunk->mesh_version.load();

        world.begin_tick(1);
        world.update(registry, anchor, nullptr);
        ASSERT_EQ(world.get_last_streaming_budget_stats().scheduled_meshing, 1u);
        ASSERT_EQ(chunk->get_state(), ChunkState::Ready);
        ASSERT_EQ(chunk->pending_lod.load(), 0);
        EXPECT_EQ(StreamingBacklog(world), 1u);

        // Worker completion cannot settle an unpublished remesh. The next
        // candidate scan sees no work because pending_lod already owns it.
        world.quiesce_streaming_jobs_for_save();
        world.begin_tick(2);
        world.update(registry, std::vector<Vec3>{anchor}, nullptr);
        const auto& streaming = world.get_last_streaming_budget_stats();
        EXPECT_EQ(streaming.generation_candidates, 0u);
        EXPECT_EQ(streaming.meshing_candidates, 0u);
        EXPECT_EQ(streaming.scheduled_meshing, 0u);
        EXPECT_EQ(StreamingBacklog(world), 1u);
        EXPECT_EQ(world.get_streaming_telemetry_stats().last_queue_depth, 1u);

        // Both promotion stages retain the original eight-tick due date.
        world.begin_tick(8);
        world.activate_due(8);
        EXPECT_EQ(StreamingBacklog(world), 1u);
        EXPECT_EQ(chunk->current_lod.load(), 1);
        EXPECT_EQ(chunk->mesh_version.load(), old_version);
        if (needs_promotion) {
            EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
            EXPECT_EQ(chunk->pending_lod.load(), 0);
            EXPECT_EQ(chunk->current_lod.load(), 1);
            EXPECT_EQ(StreamingBacklog(world), 1u);
        }
        world.begin_tick(9);
        world.activate_due(9);
        if (needs_promotion) {
            ASSERT_FALSE(chunk->sdf_data.empty());
            EXPECT_EQ(world.pending_publication_chunks(), 0u);
        }
        EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
        EXPECT_EQ(chunk->pending_lod.load(), -1);
        EXPECT_EQ(chunk->current_lod.load(), 0);
        EXPECT_EQ(chunk->mesh_version.load(), old_version + 1u);
        EXPECT_EQ(StreamingBacklog(world), 0u);
    }
}

TEST(DeterministicWorkBudget, DeferredGenerationKeepsTelemetryBackloggedBetweenActivationPasses) {
    for (const bool at_active_cap : {false, true}) {
        SCOPED_TRACE(at_active_cap);
        JobSystem jobs;
        jobs.startup(1);
        Systems::SHIELD_WorldSystem world(&jobs, nullptr, FlatWorkBudgetTerrain(), 1337);
        world.debug_set_streaming_radius_cap(8);
        entt::registry registry;
        const Vec3 anchor{8, 35, 8};
        // Activation runs every fourth update; both public update overloads
        // must report the same outstanding generation contract.
        for (std::int64_t tick = 1; tick <= 3; ++tick) {
            world.begin_tick(tick);
            world.update(registry, anchor, nullptr);
            EXPECT_EQ(world.get_streaming_telemetry_stats().last_queue_depth, 0u);
        }
        if (at_active_cap) {
            const auto cap = world.get_last_streaming_budget_stats().max_active_chunks_budget;
            ASSERT_GT(cap, 0u);
            // Empty resident chunks fill the cap without generating terrain.
            // They are outside the wanted set and evicted after budget selection.
            for (std::size_t index = 0; index < cap; ++index) {
                auto chunk = std::make_shared<Chunk>(IVec3(100 + static_cast<int>(index), 0, 0));
                chunk->set_state(ChunkState::Ready);
                ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
            }
        }
        world.begin_tick(4);
        world.update(registry, std::vector<Vec3>{anchor}, nullptr);
        const auto streaming = world.get_last_streaming_budget_stats();
        ASSERT_GT(streaming.deferred_generation, 0u);
        if (at_active_cap) {
            EXPECT_EQ(streaming.scheduled_generation, 0u);
            EXPECT_EQ(world.pending_publication_chunks(), 0u);
        } else {
            EXPECT_GT(streaming.scheduled_generation, 0u);
            EXPECT_EQ(streaming.scheduled_generation,
                      static_cast<std::size_t>(streaming.generation_budget));
        }
        for (std::int64_t tick = 4; tick <= 7; ++tick) {
            if (tick > 4) {
                world.begin_tick(tick);
                world.update(registry, anchor, nullptr);
                EXPECT_EQ(world.get_last_streaming_budget_stats().generation_candidates, 0u);
            }
            const auto& frame = world.get_last_streaming_budget_stats();
            const auto expected = world.pending_publication_chunks() +
                                  streaming.deferred_generation + frame.meshing_candidates -
                                  frame.submitted_meshing;
            EXPECT_EQ(world.get_streaming_telemetry_stats().last_queue_depth, expected);
            EXPECT_GT(world.get_streaming_telemetry_stats().last_queue_depth, 0u);
        }
        // Replace the eight-sample window's initial empty frames as well.
        for (std::int64_t tick = 8; tick <= 11; ++tick) {
            world.begin_tick(tick);
            world.update(registry, anchor, nullptr);
            EXPECT_GT(world.get_streaming_telemetry_stats().last_queue_depth, 0u);
        }
        EXPECT_GT(world.get_streaming_telemetry_stats().settled_queue_depth, 0u);
        world.clear_world(nullptr);
        world.begin_tick(12);
        world.update(registry, std::vector<Vec3>{}, nullptr);
        EXPECT_EQ(world.get_streaming_telemetry_stats().last_queue_depth, 0u);
    }
}

TEST(DeterministicWorkBudget, InitialGenerationAndMeshingStayInBacklogUntilPublication) {
    JobSystem jobs;
    jobs.startup(1);
    Systems::SHIELD_WorldSystem world(&jobs, nullptr, FlatWorkBudgetTerrain(), 1337);
    entt::registry registry;
    const Vec3 anchor{8, 35, 8};
    const auto coords = Systems::SHIELD_WorldSystem::world_to_chunk_coords(anchor);
    world.begin_tick(1);
    world.dispatch_generation_jobs(std::vector<IVec3>{coords});
    world.quiesce_streaming_jobs_for_save();
    const auto chunk = world.find_streamed_chunk(coords);
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->get_state(), ChunkState::Loading);
    EXPECT_EQ(world.pending_publication_chunks(), 1u);
    EXPECT_EQ(StreamingBacklog(world), 1u);
    world.begin_tick(9);
    world.activate_due(9);
    EXPECT_EQ(chunk->get_state(), ChunkState::Idle);
    EXPECT_EQ(world.pending_publication_chunks(), 0u);
    EXPECT_EQ(StreamingBacklog(world), 1u);

    world.update(registry, anchor, nullptr);
    world.quiesce_streaming_jobs_for_save();
    EXPECT_EQ(chunk->get_state(), ChunkState::Meshing);
    EXPECT_EQ(world.pending_publication_chunks(), 1u);
    EXPECT_EQ(StreamingBacklog(world), 1u);
    EXPECT_EQ(world.get_streaming_telemetry_stats().last_queue_depth, 1u);
    world.begin_tick(17);
    world.activate_due(17);
    EXPECT_EQ(chunk->get_state(), ChunkState::Ready);
    EXPECT_EQ(world.pending_publication_chunks(), 0u);
    EXPECT_EQ(StreamingBacklog(world), 0u);
}

TEST(DeterministicWorkBudget, OccupiedPromotionLaneKeepsRefusedCandidatesInBacklog) {
    for (const bool include_regular_remesh : {false, true}) {
        SCOPED_TRACE(include_regular_remesh);
        JobSystem jobs;
        jobs.startup(1);
        Systems::SHIELD_WorldSystem world(&jobs, nullptr, FlatWorkBudgetTerrain(), 1337);
        world.debug_set_streaming_radius_cap(1);
        entt::registry registry;
        const Vec3 anchor{8, 35, 8};
        const auto origin = Systems::SHIELD_WorldSystem::world_to_chunk_coords(anchor);
        const std::array<IVec3, 4> offsets{
            IVec3{0, 0, 0}, IVec3{1, 0, 0}, IVec3{0, 0, 1}, IVec3{1, 0, 1}};
        std::array<std::shared_ptr<Chunk>, 4> chunks{};
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            auto& chunk = chunks[i];
            chunk = std::make_shared<Chunk>(origin + offsets[i]);
            world.GenerateChunkData(*chunk, i == 3 ? 1 : 2);
            World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 2);
            ASSERT_FALSE(chunk->mesh_vertices.empty());
            ASSERT_FALSE(chunk->mesh_indices.empty());
            chunk->current_lod = 1;
            chunk->set_state(ChunkState::Ready);
        }
        ASSERT_TRUE(world.adopt_streamed_chunk(chunks[0]));
        world.begin_tick(1);
        world.update(registry, anchor, nullptr);
        world.quiesce_streaming_jobs_for_save();
        ASSERT_EQ(chunks[0]->pending_lod.load(), 0);
        ASSERT_EQ(world.pending_publication_chunks(), 1u);

        ASSERT_TRUE(world.adopt_streamed_chunk(chunks[1]));
        ASSERT_TRUE(world.adopt_streamed_chunk(chunks[2]));
        if (include_regular_remesh) {
            ASSERT_TRUE(world.adopt_streamed_chunk(chunks[3]));
        }
        world.begin_tick(2);
        world.update(registry, std::vector<Vec3>{anchor}, nullptr);
        const std::size_t regular_remeshes = include_regular_remesh ? 1u : 0u;
        const auto& streaming = world.get_last_streaming_budget_stats();
        EXPECT_EQ(streaming.meshing_candidates, 2u + regular_remeshes);
        EXPECT_EQ(streaming.scheduled_meshing, 0u);
        EXPECT_EQ(streaming.submitted_meshing, 0u);
        EXPECT_EQ(streaming.deferred_meshing, 2u + regular_remeshes);
        for (std::size_t i = 1; i <= 2; ++i) {
            EXPECT_EQ(chunks[i]->get_state(), ChunkState::Ready);
            EXPECT_EQ(chunks[i]->pending_lod.load(), -1);
        }
        // The occupied lane holds the sorted suffix, including an ordinary
        // remesh after the first refused promotion. No request is discarded.
        EXPECT_EQ(world.pending_publication_chunks(), 1u);
        EXPECT_EQ(world.get_streaming_telemetry_stats().last_queue_depth, 3u + regular_remeshes);
        EXPECT_EQ(StreamingBacklog(world), 3u + regular_remeshes);

        world.quiesce_streaming_jobs_for_save();
        world.begin_tick(9);
        world.activate_due(9);
        world.update(registry, anchor, nullptr);
        EXPECT_EQ(chunks[1]->pending_lod.load(), 0);
        EXPECT_EQ(chunks[2]->pending_lod.load(), 0);
        EXPECT_EQ(world.pending_publication_chunks(), 2u + regular_remeshes);
        EXPECT_EQ(world.get_streaming_telemetry_stats().last_queue_depth, 2u + regular_remeshes);
        EXPECT_EQ(StreamingBacklog(world), 2u + regular_remeshes);

        // The suffix selected at tick 2 keeps its original due tick 10, even
        // though the occupied promotion lane delayed admission until tick 9.
        world.activate_due(9);
        EXPECT_EQ(StreamingBacklog(world), 2u + regular_remeshes);
        world.begin_tick(10);
        world.activate_due(10);
        EXPECT_EQ(StreamingBacklog(world), 0u);
        world.begin_tick(17);
        world.activate_due(17);
        EXPECT_EQ(StreamingBacklog(world), 0u);
        world.begin_tick(25);
        world.activate_due(25);
        EXPECT_EQ(StreamingBacklog(world), 0u);
        const std::size_t adopted_chunks = 3u + regular_remeshes;
        for (std::size_t i = 0; i < adopted_chunks; ++i) {
            EXPECT_EQ(chunks[i]->get_state(), ChunkState::Ready);
            EXPECT_EQ(chunks[i]->pending_lod.load(), -1);
            EXPECT_EQ(chunks[i]->current_lod.load(), 0);
        }
    }
}

TEST(DeterministicWorkBudget, EveryPinnedLimitRejectsAnInjectedExcess) {
    const auto data = TestSupport::WorkBudgetData();
    ASSERT_EQ(data.size(), 5u);
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

TEST(DeterministicWorkBudget, TerrainAndWaterUploadWorkAndProgressStayPinned) {
    using namespace Rendering;
    struct Upload {
        std::uint64_t id = 0;
        unsigned revision = 0;
    };
    for (const bool water : {false, true}) {
        const std::string scenario = water ? "water_upload" : "terrain_upload";
        SCOPED_TRACE(scenario);
        const auto limits = water ? kWaterUploadLimits : kTerrainUploadLimits;
        const std::size_t payload_bytes = water ? 256 * 1024 : 512 * 1024;
        UploadQueue<Upload> queue;
        // Seventeen metadata-only payloads cross both shipping byte/count caps.
        // Five deterministic drains suffice for both lanes, followed by idle.
        std::vector<Upload> candidates;
        for (std::uint64_t id = 0; id < 17; ++id)
            candidates.push_back({id, 1});
        refresh_uploads(
            queue,
            candidates,
            [](const Upload& item) { return item.id; },
            [](const Upload& item) { return item.id; });
        WorkCounts peak;
        std::uint64_t adopted = 0;
        std::uint64_t next_id = 0;
        std::uint64_t uploaded_bytes = 0;
        std::uint64_t previous_backlog = 0;
        for (std::uint64_t step = 1; step <= 6; ++step) {
            const auto backlog = queue.size();
            const auto growth = backlog > previous_backlog ? backlog - previous_backlog : 0;
            const auto work = drain_uploads(
                queue,
                limits,
                [payload_bytes](const Upload&) { return payload_bytes; },
                [&](const Upload& item) {
                    EXPECT_EQ(item.id, next_id);
                    ++next_id;
                    EXPECT_EQ(item.revision, step == 1 ? 1u : 2u);
                    return true;
                },
                [] { return 0.0; }); // Fixed policy input, no CPU or GPU clock.
            adopted += work.adopted;
            uploaded_bytes += work.bytes;
            const WorkCounts observed{{"uploaded_bytes", work.bytes},
                                      {"uploads", work.attempts},
                                      {"backlog", backlog},
                                      {"backlog_growth", growth},
                                      {"drain_steps", work.attempts == 0 ? 0 : step}};
            EXPECT_TRUE(CheckWork(observed, scenario));
            TestSupport::PeakWork(peak, observed);
            previous_backlog = queue.size();
            // Refresh every retained payload to a new revision. A stale queued
            // descriptor must never replace the current one or restart its FIFO.
            candidates.clear();
            for (std::uint64_t id = adopted; id < 17; ++id)
                candidates.push_back({id, 2});
            refresh_uploads(
                queue,
                candidates,
                [](const Upload& item) { return item.id; },
                [](const Upload& item) { return item.id; });
        }
        EXPECT_EQ(adopted, 17u);
        EXPECT_EQ(uploaded_bytes, 17u * payload_bytes);
        EXPECT_EQ(queue.size(), 0u);
        // These fixtures attain every ceiling; none is an unmeasured allowance.
        EXPECT_EQ(peak, WorkLimits(scenario));
    }
}

TEST(DeterministicWorkBudget, LiveSubsystemMovementEditAndDrainStayBounded) {
    const auto limits = WorkLimits("simulation");
    const auto params = FlatWorkBudgetTerrain();
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
    std::uint64_t zero_backlog_step = 0;
    WorkCounts peak;
    for (std::uint64_t step = 1; step <= limits.at("drain_steps"); ++step) {
        SCOPED_TRACE(step);
        // Move one streaming column at step 8; edit loaded support at step 24.
        const Vec3 anchor{step < 8 ? 8.0f : 24.0f, 35, 8};
        if (step == 24) {
            ASSERT_GT(world.EditTerrainVoxel({24, 32.5f, 8}, 1.5f, false, nullptr), 0);
        }
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
        const auto backlog = StreamingBacklog(world);
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
            if (zero_backlog_step == 0)
                zero_backlog_step = step;
            // Drive a full activation interval after zero backlog so another
            // activation scan can reveal work hidden by the previous scan.
            // These confirmation updates use the same per-step work budgets.
            if (step - zero_backlog_step >=
                static_cast<std::uint64_t>(streaming.update_interval_frames)) {
                EXPECT_EQ(streaming.scheduled_generation, 0u);
                EXPECT_EQ(streaming.scheduled_meshing, 0u);
                EXPECT_EQ(world.get_streaming_telemetry_stats().last_queue_depth, 0u);
                drained = streaming.scheduled_generation == 0 && streaming.scheduled_meshing == 0;
                break;
            }
        } else {
            zero_backlog_step = 0;
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
    EXPECT_EQ(work.at("schema").get<std::string>(), "luminumbra.sim_work.v1");
    ASSERT_EQ(work.at("counters").size(), luminumbra::simulation::kSimWorkNames.size());
    for (std::size_t i = 0; i < luminumbra::simulation::kSimWorkNames.size(); ++i) {
        const auto& counter = work.at("counters").at(i);
        EXPECT_EQ(counter.at("name").get<std::string>(), luminumbra::simulation::kSimWorkNames[i]);
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
