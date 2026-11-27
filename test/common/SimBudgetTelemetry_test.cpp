#include "../fixtures/TerrainFixtureParams.h"
#include "../support/UniqueTempPath.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <iterator>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <set>
#include <string>
#include <unordered_map>

#include "../fixtures/smoke_artifact/FixedInputs.h"
#include "luminumbra_common/components/MortalComponents.h"
#include "luminumbra_common/core/WaterComponents.h"
#include "luminumbra_common/simulation/SimBudgetProfile.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include "luminumbra_common/systems/FarmingSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_server/ServerStreamingBudget.h"
#include "luminumbra_server/SimBudgetArtifact.h"
#include "luminumbra_server/modes/SmokeArtifact.h"

namespace {
using luminumbra::simulation::SimBudgetStage;
using luminumbra::simulation::SimBudgetTelemetry;

TEST(SimBudgetTelemetry, DisabledProductionArtifactMatchesDevelBytes) {
    namespace fs = std::filesystem;
    const Luminumbra::TestSupport::ScopedTempFile file("smoke_artifact_", ".json");
    const fs::path& output = file.Path();
    for (int scenario = 0; scenario != 3; ++scenario) {
        SCOPED_TRACE(scenario);
        ServerCliOptions options;
        Luminumbra::Server::SmokeRunResult first, replay;
        FixedSmokeInputs(scenario, options, first, replay);
        ASSERT_FALSE(options.sim_budget);
        ASSERT_FALSE(first.sim_budget.Enabled());
        ASSERT_FALSE(replay.sim_budget.Enabled());
        options.artifact_path = output.string();
        EXPECT_EQ(Luminumbra::Server::WriteSmokeArtifact(options, first, replay),
                  scenario == 2 ? 1 : 0);
        std::ifstream actual_file(output, std::ios::binary);
        ASSERT_TRUE(actual_file.is_open());
        const std::string actual((std::istreambuf_iterator<char>(actual_file)), {});
        std::ifstream baseline_file(fs::path(LUMINUMBRA_SOURCE_ROOT) /
                                        "test/fixtures/smoke_artifact" /
                                        ("devel-" + std::to_string(scenario) + ".json"),
                                    std::ios::binary);
        ASSERT_TRUE(baseline_file.is_open());
        std::string baseline((std::istreambuf_iterator<char>(baseline_file)), {});
#ifdef _WIN32
        // Devel's text-mode ofstream expands LF to CRLF on Windows as well.
        std::string native;
        for (const char c : baseline) {
            if (c == '\n')
                native += '\r';
            native += c;
        }
        baseline = std::move(native);
#endif
        EXPECT_EQ(actual, baseline);
    }
}

void PopulateRegionalWorkload(Luminumbra::Server::RegionalCadenceWorkload& workload,
                              entt::registry& registry) {
    namespace F = luminumbra::foliage;
    namespace C = Luminumbra::Components;
    F::SpeciesTemplate species;
    species.id = "regional_smoke_crop";
    species.perennial = true;
    species.lifespan_ticks = 900;
    species.gene_lo.fill(0.3f);
    species.gene_hi.fill(0.7f);
    auto rng = luminumbra::core::DeterministicRng::seeded(F::kPlantSeedOffset, 9001u, 7u);
    workload.Populate(registry, [&](entt::registry& output, const Luminumbra::Vec3& position) {
        const auto entity = F::MakePlantFromSpecies(output, position, species, rng, 0);
        output.remove<C::CropLifecycleComponent>(entity);
        output.emplace<C::MortalComponent>(entity);
        output.emplace<C::FlowingWaterSourceComponent>(entity);
        return entity;
    });
}

// Exercise the shipping profile through small in-memory sessions; no engine boot,
// disk world or streaming workers are needed for these bounded consumer checks.
TEST(SimBudgetTelemetry, RegionalWorkloadSmokeIsDeterministic) {
    using namespace Luminumbra;
    using namespace Luminumbra::Server;
    world::GameSession first, replay;
    RegionalCadenceWorkload workload(Vec3(0));
    RegionalCadenceWorkload repeated(Vec3(0));
    PopulateRegionalWorkload(workload, first.GetRegistry());
    PopulateRegionalWorkload(repeated, replay.GetRegistry());
    // Unrelated entities must not add diagnostic visits.
    for (int index = 0; index < 64; ++index) {
        static_cast<void>(first.GetRegistry().create());
        static_cast<void>(replay.GetRegistry().create());
    }
    ASSERT_TRUE(first.SetHostSimulationAnchors(workload.Anchors()));
    ASSERT_TRUE(replay.SetHostSimulationAnchors(repeated.Anchors()));
    std::set<world::RegionKey> owners;
    for (const auto& position : workload.positions)
        owners.insert(world::ActiveRegionLedger::region_at(position));
    EXPECT_EQ(owners.size(), RegionalCadenceWorkload::kParticipants);
    EXPECT_EQ(first.GetRegistry().view<Components::SoilFeederComponent>().size(), 9u);
    EXPECT_EQ(first.GetRegistry().view<Components::FlowingWaterSourceComponent>().size(), 9u);
    EXPECT_TRUE(first.GetRegistry().view<Components::CropLifecycleComponent>().empty());
    auto observed = RegionalCadenceDiagnostics::Start<Components::PlantGrowthComponent,
                                                      Components::MortalComponent>(
        workload, first.GetRegistry(), 0, nullptr);
    auto observed_replay = RegionalCadenceDiagnostics::Start<Components::PlantGrowthComponent,
                                                             Components::MortalComponent>(
        repeated, replay.GetRegistry(), 0, nullptr);
    for (int step = 0; step < 4; ++step) {
        ASSERT_EQ(first.TickSimulation(1.0 / 30.0), 1u);
        ASSERT_EQ(replay.TickSimulation(1.0 / 30.0), 1u);
        ASSERT_FALSE(first.GetRegionSchedulerFault());
        ASSERT_FALSE(replay.GetRegionSchedulerFault());
        observed.Observe<Components::PlantGrowthComponent, Components::MortalComponent>(
            first, first.GetRegistry(), workload, nullptr);
        observed_replay.Observe<Components::PlantGrowthComponent, Components::MortalComponent>(
            replay, replay.GetRegistry(), repeated, nullptr);
        EXPECT_EQ(observed, observed_replay);
        EXPECT_EQ(first.ComputePlantSubHash(), replay.ComputePlantSubHash());
        EXPECT_EQ(first.GetActiveRegionLedger().encode(), replay.GetActiveRegionLedger().encode());
        EXPECT_EQ(first.GetFieldPages().serialize(), replay.GetFieldPages().serialize());
    }
    for (std::size_t index = 0; index < RegionalCadenceWorkload::kAnchors; ++index) {
        EXPECT_EQ(observed.owners[index].services, 4u);
        EXPECT_EQ(observed.owners[index].elapsed_steps, 4u);
        EXPECT_EQ(observed.owners[index].max_service_gap, 1u);
    }
    EXPECT_EQ(observed.owners.back().services, 0u);
    EXPECT_EQ(observed.owners.back().elapsed_steps, 0u);
    EXPECT_EQ(observed.owners.back().unadmitted_samples, 4u);
    EXPECT_EQ(observed.owners.back().service_gap, 4u);
    EXPECT_EQ(observed.owner_visits, 4u * RegionalCadenceWorkload::kParticipants);
    EXPECT_EQ(observed.water_window_visits, 0u);
    const auto before = first.ComputePlantSubHash();
    EXPECT_THROW(PopulateRegionalWorkload(workload, first.GetRegistry()), std::invalid_argument);
    EXPECT_EQ(first.ComputePlantSubHash(), before);
    RegionalCadenceWorkload refused(Vec3(0));
    entt::registry empty_registry;
    EXPECT_THROW(
        refused.Populate(empty_registry, [](entt::registry&, const Vec3&) { return entt::null; }),
        std::invalid_argument);
    EXPECT_TRUE(empty_registry.view<Components::PlantTag>().empty());
    EXPECT_THROW(RegionalCadenceWorkload(Vec3(std::numeric_limits<float>::infinity())),
                 std::invalid_argument);
    EXPECT_THROW(RegionalCadenceWorkload(Vec3(16777000.0f, 0, 0)), std::invalid_argument);
}

TEST(SimBudgetTelemetry, RegionalDiagnosticsDoNotChangeAuthority) {
    using namespace Luminumbra;
    using namespace Luminumbra::Server;
    world::GameSession observed_session, baseline;
    RegionalCadenceWorkload workload(Vec3(0)), unobserved(Vec3(0));
    PopulateRegionalWorkload(workload, observed_session.GetRegistry());
    PopulateRegionalWorkload(unobserved, baseline.GetRegistry());
    ASSERT_TRUE(observed_session.SetHostSimulationAnchors(workload.Anchors()));
    ASSERT_TRUE(baseline.SetHostSimulationAnchors(unobserved.Anchors()));
    observed_session.SetSimBudgetTelemetryEnabled(true);
    baseline.SetSimBudgetTelemetryEnabled(true);
    auto diagnostic = RegionalCadenceDiagnostics::Start<Components::PlantGrowthComponent,
                                                        Components::MortalComponent>(
        workload, observed_session.GetRegistry(), 0, nullptr);
    for (int step = 0; step < 4; ++step) {
        ASSERT_EQ(observed_session.TickSimulation(1.0 / 30.0), 1u);
        ASSERT_EQ(baseline.TickSimulation(1.0 / 30.0), 1u);
        diagnostic.Observe<Components::PlantGrowthComponent, Components::MortalComponent>(
            observed_session, observed_session.GetRegistry(), workload, nullptr);
        const auto once = diagnostic;
        diagnostic.Observe<Components::PlantGrowthComponent, Components::MortalComponent>(
            observed_session, observed_session.GetRegistry(), workload, nullptr);
        EXPECT_EQ(diagnostic, once); // Repeated opportunities never double count.
        EXPECT_EQ(observed_session.ComputePlantSubHash(), baseline.ComputePlantSubHash());
        EXPECT_EQ(observed_session.GetActiveRegionLedger().encode(),
                  baseline.GetActiveRegionLedger().encode());
        EXPECT_EQ(observed_session.GetFieldPages().serialize(),
                  baseline.GetFieldPages().serialize());
        EXPECT_EQ(observed_session.SystemConfigIdentity(), baseline.SystemConfigIdentity());
        EXPECT_TRUE(
            observed_session.GetSimBudgetTelemetry().WorkMatches(baseline.GetSimBudgetTelemetry()));
    }
    EXPECT_EQ(diagnostic.observations, 4u);
    EXPECT_EQ(diagnostic.water_deferred_samples, 4u);
    observed_session.GetRegistry().destroy(workload.plants[0]);
    ASSERT_EQ(observed_session.TickSimulation(1.0 / 30.0), 1u);
    diagnostic.Observe<Components::PlantGrowthComponent, Components::MortalComponent>(
        observed_session, observed_session.GetRegistry(), workload, nullptr);
    EXPECT_EQ(diagnostic.owners[0].missing_samples, 1u);

    nlohmann::json artifact;
    AppendRegionalCadenceArtifact(artifact, diagnostic, std::nullopt);
    EXPECT_FALSE(artifact.at("regional_cadence").at("observations_replay_match").get<bool>());
    EXPECT_TRUE(artifact.at("regional_cadence").at("replay").is_null());
    AppendRegionalCadenceArtifact(artifact, diagnostic, diagnostic);
    EXPECT_TRUE(artifact.at("regional_cadence").at("observations_replay_match").get<bool>());
}

TEST(SimBudgetTelemetry, RegionalDiagnosticsObserveReducedFrozenAndWaterPublications) {
    using namespace Luminumbra;
    using namespace Luminumbra::Server;
    world::GameSession session;
    session.SetRegionSchedulerConfig({.work_limit = 0, .hold_ticks = 1, .reduced_shift = 1});
    RegionalCadenceWorkload workload(Vec3(0));
    PopulateRegionalWorkload(workload, session.GetRegistry());
    ASSERT_TRUE(session.SetHostSimulationAnchors(workload.Anchors()));
    const auto control = world::ActiveRegionLedger::region_at(workload.positions.back());
    session.PinActiveRegion(control, true);
    session.PinActiveRegion(control, false);
    Systems::SHIELD_WorldSystem shield(
        nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    Systems::WaterSystem water(nullptr, &shield);
    water.SetRegionalScheduling(true);
    water.SetSimResolution(8);
    water.SetHydrology(false, 0, 0);
    const auto position = workload.positions.front();
    auto chunk = std::make_shared<Chunk>(IVec3(static_cast<int>(position.x) / CHUNK_SIZE_X,
                                               0,
                                               static_cast<int>(position.z) / CHUNK_SIZE_Z));
    chunk->set_state(ChunkState::Idle);
    chunk->has_water_sim = true;
    chunk->current_water_resolution = 8;
    chunk->water_depth_mm.assign(64, 1000);
    chunk->water_bed_mm.assign(64, 1000);
    chunk->water_edge_flux.assign(128, 0);
    chunk->water_level_data.assign(64, 2.0f);
    const std::array residents{chunk};
    auto diagnostic = RegionalCadenceDiagnostics::Start<Components::PlantGrowthComponent,
                                                        Components::MortalComponent>(
        workload, session.GetRegistry(), 0, &water);
    bool saw_reduced = false, saw_frozen = false;
    for (int step = 0; step < 5; ++step) {
        // Deterministic pressure input exercises the existing scheduler policy.
        session.RecordActiveRegionWork({control, 1, 0, 0, 0});
        ASSERT_EQ(session.TickSimulation(1.0 / 30.0), 1u);
        ASSERT_FALSE(session.GetRegionSchedulerFault());
        const auto state = session.GetActiveRegionLedger().records().at(control).state;
        saw_reduced = saw_reduced || state == world::RegionState::Reduced;
        saw_frozen = saw_frozen || state == world::RegionState::Frozen;
        if (step == 0) {
            auto ledger = session.GetActiveRegionLedger();
            std::vector<world::RegionWork> completed;
            water.UpdateRegional(
                session.GetRegistry(), residents, ledger, session.GetRegionSchedule(), completed);
            ASSERT_GT(water.regional_work().allocated_windows, 0u);
            ASSERT_GT(water.regional_work().water_cells, 0u);
        }
        diagnostic.Observe<Components::PlantGrowthComponent, Components::MortalComponent>(
            session, session.GetRegistry(), workload, &water);
    }
    EXPECT_TRUE(saw_reduced);
    EXPECT_TRUE(saw_frozen);
    EXPECT_GT(diagnostic.owners.back().frozen_samples, 0u);
    EXPECT_EQ(diagnostic.owners.back().frozen_services, 0u);
    EXPECT_EQ(diagnostic.water_publications, 1u);
    EXPECT_EQ(diagnostic.water_deferred_samples, 4u);
    EXPECT_EQ(diagnostic.water_granted_windows, water.regional_work().allocated_windows);
    EXPECT_EQ(diagnostic.water_completed_cells, water.regional_work().water_cells);
    EXPECT_LE(diagnostic.water_total_cells_max, 4096u);
    EXPECT_EQ(diagnostic.owners.front().water_services, 1u);
    EXPECT_EQ(diagnostic.owners.front().water_service_gap, 4u);
    EXPECT_EQ(diagnostic.owners.front().water_interior_windows, diagnostic.water_granted_windows);
    EXPECT_EQ(diagnostic.owner_visits, 5u * RegionalCadenceWorkload::kParticipants);
    EXPECT_EQ(diagnostic.water_window_visits,
              RegionalCadenceWorkload::kParticipants * water.regional_work().visits.size());
}

TEST(SimBudgetTelemetry, RegionalWorkloadRefusesConflictingModesBeforeBoot) {
    ServerCliOptions options;
    EXPECT_TRUE(options.SupportsRegionalCadenceWorkload());
    const std::array incompatible = {&ServerCliOptions::lockstep_loopback,
                                     &ServerCliOptions::heavy,
                                     &ServerCliOptions::weather_bench,
                                     &ServerCliOptions::aether_bench,
                                     &ServerCliOptions::wind_bench,
                                     &ServerCliOptions::net_soak,
                                     &ServerCliOptions::net_soak_client,
                                     &ServerCliOptions::net_host,
                                     &ServerCliOptions::net_join,
                                     &ServerCliOptions::replicate,
                                     &ServerCliOptions::ecology_roster,
                                     &ServerCliOptions::planted_roster,
                                     &ServerCliOptions::moving,
                                     &ServerCliOptions::water_smoke};
    for (const auto flag : incompatible) {
        options.*flag = true;
        EXPECT_FALSE(options.SupportsRegionalCadenceWorkload());
        options.*flag = false;
    }
    const std::array paths = {&ServerCliOptions::mutate_replay_fixture,
                              &ServerCliOptions::replay_path,
                              &ServerCliOptions::record_path,
                              &ServerCliOptions::world_id};
    for (const auto path : paths) {
        options.*path = "input";
        EXPECT_FALSE(options.SupportsRegionalCadenceWorkload());
        (options.*path).clear();
    }
    for (const int avatars : {-1, 1, 64}) {
        options.avatars = avatars;
        EXPECT_FALSE(options.SupportsRegionalCadenceWorkload());
    }
    options.avatars = 0;
    options.smoke = options.sim_budget = options.availability_trace = options.water_hash_trace =
        true;
    EXPECT_TRUE(options.SupportsRegionalCadenceWorkload());
}

TEST(SimBudgetTelemetry, DisabledRegionalDiagnosticsPreserveArtifactBytes) {
    using namespace Luminumbra::Server;
    for (const bool budget_enabled : {false, true}) {
        SimBudgetTelemetry budget;
        budget.SetEnabled(budget_enabled);
        budget.Record(SimBudgetStage::Plants, 1, 9, 1.0);
        nlohmann::json artifact = {{"schema", "luminumbra.server_tick.v1"},
                                   {"world_hash", "fixed"}};
        AppendSimBudgetArtifact(artifact, budget, budget);
        const auto before = artifact.dump(2);
        AppendRegionalCadenceArtifact(artifact, std::nullopt, std::nullopt);
        EXPECT_EQ(artifact.dump(2), before);
        AppendRegionalCadenceArtifact(artifact, std::nullopt, RegionalCadenceDiagnostics{});
        EXPECT_EQ(artifact.dump(2), before);
        EXPECT_FALSE(artifact.contains("regional_cadence"));
        if (budget_enabled) {
            EXPECT_EQ(artifact.at("sim_budget").at("schema"), "luminumbra.sim_budget.v2");
        }
    }
}

TEST(SimBudgetTelemetry, WaterWorkIsZeroAfterEmptyAndPausedUpdates) {
    using namespace Luminumbra;
    Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    Systems::WaterSystem water(nullptr, &world);
    water.SetSimResolution(8);
    water.SetHydrology(false, 0, 0);
    entt::registry registry;
    auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
    chunk->has_water_sim = true;
    chunk->current_water_resolution = 8;
    chunk->water_depth_mm.assign(64, 1000);
    chunk->water_bed_mm.assign(64, 1000);
    chunk->water_src_mm.assign(64, 0);
    chunk->water_level_data.assign(64, 2.0f);
    chunk->water_flow_data.assign(64, Vec2(0.0f));
    chunk->water_sim_terrain_height.assign(64, 1.0f);
    chunk->water_rest_level.assign(64, 2.0f);
    const std::unordered_map<ChunkID, std::shared_ptr<Chunk>> chunks = {{chunk->get_id(), chunk}};
    const std::unordered_map<ChunkID, std::shared_ptr<Chunk>> empty;
    EXPECT_EQ(water.cells_stepped_last_update(), 0u);
    for (const bool paused : {false, true}) {
        SCOPED_TRACE(paused);
        water.SetBootPaused(false);
        chunk->is_water_sleeping = false;
        water.update(registry, chunks);
        ASSERT_EQ(water.cells_stepped_last_update(), 64u);
        ASSERT_EQ(water.dbg_cells_simmed(), 64u);
        water.SetBootPaused(paused);
        water.update(registry, paused ? chunks : empty);
        EXPECT_EQ(water.cells_stepped_last_update(), 0u);
        EXPECT_EQ(water.dbg_cells_simmed(), 64u); // Preserve the legacy flag-off output.
    }
}

TEST(SimBudgetTelemetry, StreamingIncludesNoWaterUpdateAndTailAfterLastInnerTimer) {
    struct Clock {
        using time_point = std::chrono::steady_clock::time_point;
        static time_point& Time() {
            static time_point time;
            return time;
        }
        static int& Reads() {
            static int reads = 0;
            return reads;
        }
        static time_point now() {
            ++Reads();
            return Time();
        }
        static void Work(int milliseconds) {
            Time() += std::chrono::milliseconds(milliseconds);
        }
    };
    SimBudgetTelemetry telemetry;
    telemetry.SetEnabled(true);
    Luminumbra::Server::ServerStreamingBudget<Clock> observer(telemetry);
    // Reuse the observer across wet -> dry -> wet updates. The zero-water case
    // never calls the water body. All intervals are exact integer milliseconds.
    std::uint64_t tick = 0;
    for (const bool run_water : {true, false, true}) {
        SCOPED_TRACE(run_water);
        ++tick;
        Luminumbra::Systems::SHIELD_WorldSystem::DbgStreamTimings timing;
        std::uint64_t water_work = 0;
        int water_calls = 0;
        int tail_calls = 0;
        const auto update_start = Clock::now();
        observer.MeasureUpdate([&] {
            auto previous = Clock::now();
            const auto split = [&](double& slot) {
                const auto end = Clock::now();
                slot = std::chrono::duration<double, std::milli>(end - previous).count();
                previous = end;
            };
            Clock::Work(2);
            split(timing.process_completed);
            Clock::Work(3);
            split(timing.telemetry);
            Clock::Work(5);
            split(timing.activation);
            if (run_water) {
                ++water_calls;
                water_work = 64;
                Clock::Work(100); // Larger than streaming: catches a double subtraction.
            }
            split(timing.water); // The bracket closes even when water does not run.
            Clock::Work(7);
            split(timing.meshing_pass);
            Clock::Work(11);
            split(timing.collision); // Last existing inner timer.
            ++tail_calls;
            Clock::Work(17); // Resident scan / queue bookkeeping before update returns.
        });
        const double whole_update =
            std::chrono::duration<double, std::milli>(Clock::now() - update_start).count();
        Clock::Work(1000); // Diagnostics between the update and activation are excluded.
        const auto wait_start = Clock::now();
        Clock::Work(13);
        const double wait_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - wait_start).count();
        observer.Record(tick, timing, 3, water_work, wait_ms);

        const auto& streaming =
            telemetry.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::ServerStreaming)];
        const auto& water =
            telemetry.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::ServerWater)];
        ASSERT_EQ(streaming.size(), tick);
        ASSERT_EQ(water.size(), tick);
        EXPECT_EQ(streaming.back().tick, tick);
        EXPECT_EQ(water.back().tick, tick);
        EXPECT_EQ(streaming.back().work, 3u);
        EXPECT_EQ(water_calls, run_water ? 1 : 0);
        EXPECT_EQ(water.back().work, run_water ? 64u : 0u);
        EXPECT_DOUBLE_EQ(water.back().duration_ms, run_water ? 100.0 : 0.0);
        EXPECT_DOUBLE_EQ(whole_update, run_water ? 145.0 : 45.0);
        ASSERT_EQ(tail_calls, 1);
        const double inner_streaming = timing.process_completed + timing.telemetry +
                                       timing.activation + timing.meshing_pass + timing.collision;
        EXPECT_DOUBLE_EQ(inner_streaming, 28.0);
        EXPECT_DOUBLE_EQ(streaming.back().duration_ms, 58.0);
        // The pre-fix inner-sum arithmetic gives 41 ms, losing all 17 ms of tail.
        EXPECT_DOUBLE_EQ(streaming.back().duration_ms - (inner_streaming + wait_ms), 17.0);
        EXPECT_GE(streaming.back().duration_ms, 0.0);
        EXPECT_GE(streaming.back().duration_ms, inner_streaming + 17.0 + wait_ms);
        EXPECT_DOUBLE_EQ(streaming.back().duration_ms + water.back().duration_ms,
                         whole_update + wait_ms);
        if (!run_water) {
            EXPECT_DOUBLE_EQ(timing.water, 0.0);
            EXPECT_DOUBLE_EQ(streaming.back().duration_ms, whole_update + wait_ms);
        }
    }
    telemetry.SetEnabled(false);
    const int reads_before = Clock::Reads();
    int updates = 0;
    observer.MeasureUpdate([&] {
        ++updates;
        Clock::Work(9);
    });
    observer.Record(++tick, {}, 0, 0, 0.0);
    EXPECT_EQ(updates, 1);
    EXPECT_EQ(Clock::Reads(), reads_before);
    for (const auto& samples : telemetry.SamplesByStage())
        EXPECT_TRUE(samples.empty());
}

TEST(SimBudgetTelemetry, DisabledSkipsCounterAndArtifactExtension) {
    SimBudgetTelemetry telemetry;
    EXPECT_FALSE(telemetry.Enabled());
    bool counter_called = false;
    {
        SimBudgetTelemetry::TickScope tick(telemetry, 1);
        tick.Next(SimBudgetStage::Plants, [&] {
            counter_called = true;
            return 6u;
        });
    }
    telemetry.Record(SimBudgetStage::Plants, 1, 6, 42.0);
    EXPECT_FALSE(counter_called);
    for (const auto& stage : telemetry.SamplesByStage())
        EXPECT_TRUE(stage.empty());

    // Unit coverage of the append helper only. The complete serializer has its
    // own fixed-input devel baseline test below.
    nlohmann::json artifact = {{"schema", "luminumbra.server_tick.v1"},
                               {"world_hash", "fixed hash"},
                               {"runs", {{{"wall_seconds", 1.25}, {"world_id", "fixed id"}}}},
                               {"future_key", "preserve\nthis"}};
    const std::string before = artifact.dump(2) + "\n";
    Luminumbra::Server::AppendSimBudgetArtifact(artifact, telemetry, telemetry);
    EXPECT_EQ(artifact.dump(2) + "\n", before);
    EXPECT_FALSE(artifact.contains("sim_budget"));
}

TEST(SimBudgetTelemetry, PercentilesUsePerfLinearInterpolation) {
    SimBudgetTelemetry telemetry;
    telemetry.SetEnabled(true);
    EXPECT_EQ(telemetry.Summarize(SimBudgetStage::Plants).samples, 0u);
    telemetry.Record(SimBudgetStage::Plants, 1, 6, 30.0);
    const auto single = telemetry.Summarize(SimBudgetStage::Plants);
    EXPECT_DOUBLE_EQ(single.p99_ms, 30.0);
    telemetry.Record(SimBudgetStage::Plants, 2, 5, 0.0);
    telemetry.Record(SimBudgetStage::Plants, 3, 4, 20.0);
    telemetry.Record(SimBudgetStage::Plants, 4, 3, 10.0);
    const auto summary = telemetry.Summarize(SimBudgetStage::Plants);
    EXPECT_EQ(summary.samples, 4u);
    EXPECT_EQ(summary.work_total, 18u);
    EXPECT_DOUBLE_EQ(summary.p50_ms, 15.0);
    EXPECT_DOUBLE_EQ(summary.p95_ms, 28.5);
    EXPECT_DOUBLE_EQ(summary.p99_ms, 29.7);
    EXPECT_DOUBLE_EQ(summary.max_ms, 30.0);
    EXPECT_DOUBLE_EQ(
        telemetry.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::Plants)][0].duration_ms,
        30.0);
}

TEST(SimBudgetTelemetry, ReplayComparesTicksAndWorkButNeverDurations) {
    SimBudgetTelemetry first;
    SimBudgetTelemetry replay;
    first.SetEnabled(true);
    replay.SetEnabled(true);
    first.Record(SimBudgetStage::Plants, 1, 6, 1.0);
    replay.Record(SimBudgetStage::Plants, 1, 6, 1000.0);
    EXPECT_TRUE(first.WorkMatches(replay));
    replay.Clear();
    replay.Record(SimBudgetStage::Plants, 2, 6, 1.0);
    EXPECT_FALSE(first.WorkMatches(replay));
    replay.Clear();
    replay.Record(SimBudgetStage::Plants, 1, 7, 1.0);
    EXPECT_FALSE(first.WorkMatches(replay));
    replay.Clear();
    replay.Record(SimBudgetStage::Creatures, 1, 6, 1.0);
    EXPECT_FALSE(first.WorkMatches(replay));
}

TEST(SimBudgetTelemetry, ArtifactSeparatesWorkTraceFromDurationDistribution) {
    SimBudgetTelemetry first;
    SimBudgetTelemetry replay;
    first.SetEnabled(true);
    replay.SetEnabled(true);
    first.Record(SimBudgetStage::Plants, 1, 6, 2.0);
    replay.Record(SimBudgetStage::Plants, 1, 6, 200.0);
    nlohmann::json artifact = {{"schema", "luminumbra.server_tick.v1"}};
    Luminumbra::Server::AppendSimBudgetArtifact(artifact, first, replay);
    const auto& block = artifact.at("sim_budget");
    EXPECT_EQ(block.at("schema"), "luminumbra.sim_budget.v2");
    EXPECT_TRUE(block.at("work_replay_match").get<bool>());
    const auto& stages = block.at("stages");
    const auto& plants = stages.at(static_cast<std::size_t>(SimBudgetStage::Plants));
    EXPECT_EQ(plants.at("work_total"), 6);
    EXPECT_EQ(plants.at("work_trace"), nlohmann::json::array({{{"tick", 1}, {"work", 6}}}));
    EXPECT_EQ(plants.at("duration_ms").at("p99"), 2.0);
    EXPECT_TRUE(
        stages.at(static_cast<std::size_t>(SimBudgetStage::Animation)).at("duration_ms").is_null());
}

TEST(SimBudgetTelemetry, BatchedTicksKeepAbsoluteIdsAndClearOnToggle) {
    Luminumbra::world::GameSession session;
    session.SetSimBudgetTelemetryEnabled(true);
    EXPECT_EQ(session.TickSimulation(0.0), 0u);
    EXPECT_EQ(session.TickSimulation(2.0 / 30.0), 2u);
    EXPECT_EQ(session.TickSimulation(1.0 / 30.0), 1u);
    const auto& telemetry = session.GetSimBudgetTelemetry();
    for (std::size_t stage = static_cast<std::size_t>(SimBudgetStage::Animation);
         stage <= static_cast<std::size_t>(SimBudgetStage::Events);
         ++stage) {
        const auto& samples = telemetry.SamplesByStage()[stage];
        if (stage == static_cast<std::size_t>(SimBudgetStage::WindWeather)) {
            EXPECT_TRUE(samples.empty());
            continue;
        }
        ASSERT_EQ(samples.size(), 3u);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            EXPECT_EQ(samples[i].tick, i + 1);
            EXPECT_EQ(samples[i].work, 0u);
            EXPECT_GE(samples[i].duration_ms, 0.0);
        }
    }
    session.SetSimBudgetTelemetryEnabled(false);
    EXPECT_EQ(session.TickSimulation(1.0 / 30.0), 1u);
    for (const auto& stage : telemetry.SamplesByStage())
        EXPECT_TRUE(stage.empty());
    session.SetSimBudgetTelemetryEnabled(true);
    EXPECT_EQ(session.TickSimulation(1.0 / 30.0), 1u);
    EXPECT_EQ(telemetry.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::Plants)][0].tick,
              5u);
}

// Instinct work is 4 at tick 1 and 9 at tick 2; no residency samples are ever recorded.
void RecordBudgetRun(SimBudgetTelemetry& telemetry) {
    telemetry.SetEnabled(true);
    telemetry.Record(SimBudgetStage::Instinct, 1, 4, 1.0);
    telemetry.Record(SimBudgetStage::Instinct, 2, 9, 1.0);
}

luminumbra::simulation::SimBudgetProfile InstinctCapProfile(std::uint64_t cap) {
    luminumbra::simulation::SimBudgetProfile profile;
    profile.stage_max_work["instinct"] = cap;
    return profile;
}

TEST(SimBudgetTelemetry, EnforcementReportsStageViolationWithCorrectFields) {
    SimBudgetTelemetry first;
    SimBudgetTelemetry replay;
    RecordBudgetRun(first);
    RecordBudgetRun(replay);
    nlohmann::json artifact = {{"schema", "luminumbra.server_tick.v1"}};
    Luminumbra::Server::AppendSimBudgetArtifact(artifact, first, replay);
    const auto verdict =
        Luminumbra::Server::AppendSimBudgetEnforcement(artifact, InstinctCapProfile(5), 77, first);
    EXPECT_FALSE(verdict.no_violation);
    const auto& block = artifact.at("sim_budget").at("enforcement");
    EXPECT_EQ(block.at("schema"), "luminumbra.sim_budget_enforcement.v1");
    EXPECT_EQ(block.at("profile_bytes"), 77);
    EXPECT_FALSE(block.at("no_violation").get<bool>());
    EXPECT_TRUE(block.at("coverage_complete").get<bool>());
    EXPECT_EQ(block.at("violations"),
              nlohmann::json::array({{{"kind", "stage"},
                                      {"name", "instinct"},
                                      {"tick", 2},
                                      {"observed", 9},
                                      {"limit", 5}}}));
    EXPECT_TRUE(block.at("unevaluated").empty());
}

TEST(SimBudgetTelemetry, EnforcementWithinCapReportsUnevaluatedResidency) {
    SimBudgetTelemetry first;
    SimBudgetTelemetry replay;
    RecordBudgetRun(first);
    RecordBudgetRun(replay);
    // The observed value equals the cap, which is not a violation.
    luminumbra::simulation::SimBudgetProfile profile = InstinctCapProfile(9);
    profile.residency_max_bytes["terrain_cache"] = 1024;
    nlohmann::json artifact = {{"schema", "luminumbra.server_tick.v1"}};
    Luminumbra::Server::AppendSimBudgetArtifact(artifact, first, replay);
    const auto verdict =
        Luminumbra::Server::AppendSimBudgetEnforcement(artifact, profile, 0, first);
    EXPECT_TRUE(verdict.no_violation);
    EXPECT_FALSE(verdict.coverage_complete);
    const auto& block = artifact.at("sim_budget").at("enforcement");
    EXPECT_TRUE(block.at("no_violation").get<bool>());
    EXPECT_FALSE(block.at("coverage_complete").get<bool>());
    EXPECT_TRUE(block.at("violations").empty());
    EXPECT_EQ(block.at("unevaluated"), nlohmann::json::array({"residency:terrain_cache"}));
}

TEST(SimBudgetTelemetry, EnforcementIsByteNeutralWithoutEnabledCollector) {
    SimBudgetTelemetry disabled;
    nlohmann::json artifact = {{"schema", "luminumbra.server_tick.v1"},
                               {"world_hash", "fixed hash"}};
    const std::string before = artifact.dump(2) + "\n";
    Luminumbra::Server::AppendSimBudgetArtifact(artifact, disabled, disabled);
    const auto verdict = Luminumbra::Server::AppendSimBudgetEnforcement(
        artifact, InstinctCapProfile(0), 9, disabled);
    EXPECT_TRUE(verdict.no_violation);
    EXPECT_EQ(artifact.dump(2) + "\n", before);
    EXPECT_FALSE(artifact.contains("sim_budget"));
}

TEST(SimBudgetTelemetry, SmokeFailsOnlyWhenBudgetProfileStageIsExceeded) {
    const Luminumbra::TestSupport::ScopedTempFile profile_file("smoke_budget_profile_", ".json");
    const auto& output = profile_file.Path();
    for (const std::uint64_t cap : {std::uint64_t{5}, std::uint64_t{9}}) {
        SCOPED_TRACE(cap);
        ServerCliOptions options;
        Luminumbra::Server::SmokeRunResult first, replay;
        FixedSmokeInputs(0, options, first, replay);
        options.sim_budget = true;
        options.sim_budget_profile = InstinctCapProfile(cap);
        options.sim_budget_profile_bytes = 123;
        RecordBudgetRun(first.sim_budget);
        RecordBudgetRun(replay.sim_budget);
        options.artifact_path = output.string();
        const bool violated = cap == 5;
        EXPECT_EQ(Luminumbra::Server::WriteSmokeArtifact(options, first, replay), violated ? 1 : 0);
        std::ifstream file(output);
        ASSERT_TRUE(file.is_open());
        const auto artifact = nlohmann::json::parse(file);
        EXPECT_EQ(artifact.at("passed").get<bool>(), !violated);
        const auto& block = artifact.at("sim_budget").at("enforcement");
        EXPECT_EQ(block.at("no_violation").get<bool>(), !violated);
        EXPECT_EQ(block.at("profile_bytes"), 123);
    }
}
} // namespace

namespace {
TEST(SimBudgetTelemetry, RegionalSubstepsAggregateWithinHostTickAndRemainSeparateAcrossTicks) {
    SimBudgetTelemetry telemetry;
    telemetry.SetEnabled(true);
    telemetry.Record(SimBudgetStage::Plants, 100, 2, 1.5);
    telemetry.Record(SimBudgetStage::Soil, 100, 256, 2.0);
    telemetry.Record(SimBudgetStage::Plants, 100, 3, 0.5);
    telemetry.Record(SimBudgetStage::Plants, 101, 4, 3.0);
    const auto& plants =
        telemetry.SamplesByStage()[static_cast<std::size_t>(SimBudgetStage::Plants)];
    ASSERT_EQ(plants.size(), 2u);
    EXPECT_EQ(plants[0].tick, 100u);
    EXPECT_EQ(plants[0].work, 5u);
    EXPECT_DOUBLE_EQ(plants[0].duration_ms, 2.0);
    EXPECT_EQ(plants[1].tick, 101u);
    EXPECT_EQ(plants[1].work, 4u);
    EXPECT_DOUBLE_EQ(plants[1].duration_ms, 3.0);
    EXPECT_EQ(telemetry.Summarize(SimBudgetStage::Soil).work_total, 256u);
}
} // namespace
