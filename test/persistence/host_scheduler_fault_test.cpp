#include "../support/SimulationContentFixture.h"
#include <gtest/gtest.h>

#include "../support/WorldgenTestData.h"

#include "luminumbra_client/app/SessionExit.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/replay/RegionScheduleTrace.h"
#include "luminumbra_server/modes/ModeHelpers.h"
#include "luminumbra_server/modes/Modes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/sinks/ostream_sink.h>

namespace {
namespace fs = std::filesystem;
using Luminumbra::Persistence::WorldSaveService;
using Luminumbra::world::GameSession;
constexpr Luminumbra::world::RegionKey kFaultRegion{40, -40};
constexpr std::uint64_t kFaultTick = 3;

std::string ReadHostFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void WriteHostFile(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    ASSERT_TRUE(output.good());
}

// Use the existing GameSession seam: pressure makes a pinned distant region
// freeze, whose built-in digest provider throws on a corrupt durable region.
// Reloading a valid ledger supplies the small test policy to an already booted
// runner; no private members or synthetic fault reports are used.
void ArmHostFault(GameSession& session) {
    ASSERT_TRUE(session.ActiveRegionsEnabled());
    ASSERT_EQ(session.GetSimulationTickCount(), 0u);
    ASSERT_TRUE(session.SaveWorldState());
    Luminumbra::world::ActiveRegionLedger ledger({1, 1, 1});
    ASSERT_TRUE(ledger.adopt_content_identity(session.GetSimulationContent()->identity));
    ledger.set_local_anchor(session.GetMetadata().spawnPoint);
    ASSERT_TRUE(WorldSaveService::save_active_regions(
        ledger, session.GetWorldClock(), session.GetWorldSaveDir()));
    ASSERT_TRUE(session.LoadWorldState());
    session.PinActiveRegion(kFaultRegion, true);
    ASSERT_TRUE(session.GetActiveRegionLedger().records().contains(kFaultRegion));
    session.GetSimulationEventBus().subscribe([&session](const auto& event) {
        if (event.tick == kFaultTick - 1 || event.tick == kFaultTick)
            session.RecordActiveRegionWork({kFaultRegion, 5, 0, 0, 0});
    });
    for (std::uint64_t tick = 1; tick <= kFaultTick + 1; ++tick)
        session.GetSimulationEventBus().publish(tick, "fault-test-work", "");
    WriteHostFile(WorldSaveService::region_file_path(
                      session.GetWorldSaveDir(), kFaultRegion.x, kFaultRegion.z),
                  "not a region file");
}

void RemoveHostFaultFile(const GameSession& session) {
    // As in the GameSession regression, remove only the injected corrupt file
    // after the provider has thrown. Recovery saves validate durable files.
    ASSERT_TRUE(fs::remove(WorldSaveService::region_file_path(
        session.GetWorldSaveDir(), kFaultRegion.x, kFaultRegion.z)));
}

std::string LiveAuthoritativeContents(GameSession& session) {
    session.GetWorldSystem()->quiesce_streaming_jobs_for_save();
    Luminumbra::WorldStreamingState state;
    for (const auto& chunk : session.GetWorldSystem()->snapshot_streamed_chunks())
        state.insert_chunk(chunk);
    return Luminumbra::Persistence::ComputeRegionContentHash(state);
}

void DirtyHostChunk(GameSession& session) {
    for (const auto& chunk : session.GetWorldSystem()->snapshot_streamed_chunks()) {
        if (chunk && !chunk->sdf_data.empty()) {
            chunk->sdf_data.front() = 1234.5f;
            chunk->mark_sdf_loaded_or_edited();
            chunk->mark_voxel_data_dirty();
            return;
        }
    }
    FAIL() << "Boot must supply a chunk to make recovery persistence non-vacuous";
}

struct HostFaultObservation {
    bool inject_fault = true;
    bool explicit_shutdown = false;
    bool recovery_saved = false;
    std::uint64_t ticks = 0;
    std::uint64_t frames = 0;
    std::size_t availability_ticks = 0;
    fs::path save_dir;
    std::string expected_contents;
    std::string fault_message;
    std::string metadata_at_shutdown;
};

// GTest runs these cases sequentially within this executable. Each fixture
// owns its observation and isolated runtime root; no shared machine files change.
HostFaultObservation*& HostObservation() {
    static HostFaultObservation* observation = nullptr;
    return observation;
}
} // namespace

namespace Luminumbra::Server {
// Compile the shipping mode bodies below against this test-only adapter. Only
// setup, removal of injected corruption and observations wrap the real runner;
// RunFixedTicks, fault reporting and Shutdown/save all execute production code.
class SchedulerFaultTestRunner : public ServerWorldRunner {
public:
    using ServerWorldRunner::ServerWorldRunner;

    bool Boot() {
        if (!ServerWorldRunner::Boot())
            return false;
        HostObservation()->save_dir = Session()->GetWorldSaveDir();
        if (HostObservation()->inject_fault) {
            ArmHostFault(*Session());
            if (testing::Test::HasFatalFailure())
                return false;
            DirtyHostChunk(*Session());
        }
        return !testing::Test::HasFatalFailure();
    }

    ServerTickReport RunFixedTicks(std::uint64_t count) {
        const auto report = ServerWorldRunner::RunFixedTicks(count);
        HostObservation()->ticks += report.ticks_executed;
        HostObservation()->frames += report.frames_executed;
        HostObservation()->availability_ticks = AvailabilityTrace().size();
        if (report.region_fault && HostObservation()->fault_message.empty()) {
            HostObservation()->fault_message = report.region_fault_message;
            EXPECT_TRUE(Session()->GetRegionSchedulerFault().has_value());
            EXPECT_EQ(Session()->GetSimulationTickCount(), kFaultTick);
            RemoveHostFaultFile(*Session());
            HostObservation()->expected_contents = LiveAuthoritativeContents(*Session());
        }
        return report;
    }

    void Shutdown() {
        // Capture before the destructor can save: removing RunServer's explicit
        // Shutdown must fail even though the real runner also saves on destruction.
        HostObservation()->explicit_shutdown = true;
        world::WorldStateSaveReport report;
        ServerWorldRunner::Shutdown(&report);
        HostObservation()->recovery_saved = report.saved;
        HostObservation()->metadata_at_shutdown =
            ReadHostFile(HostObservation()->save_dir / "world_info.json");
    }
};
} // namespace Luminumbra::Server

// Headers are included above before substitution. Keep the mode control flow
// verbatim so deleting its fault branch, Finalize guard or explicit shutdown
// changes the behavior under test. These sources are not separately linked.
#define ServerWorldRunner SchedulerFaultTestRunner
#include "../../src/luminumbra_server/modes/Replay.cpp"
#include "../../src/luminumbra_server/modes/Server.cpp"
#undef ServerWorldRunner

namespace {
class HostSchedulerFault : public testing::Test {
protected:
    fs::path root;
    HostFaultObservation observation;
    std::ostringstream log_output;
    std::shared_ptr<spdlog::logger> previous_logger;

    void SetUp() override {
        root = fs::temp_directory_path() /
               ("luminumbra_host_fault_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Luminumbra::TestSupport::WriteSimulationContentFixture(root);
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                      root / "worlds/atlas/presets/default.json");
        ASSERT_TRUE(fs::remove(root / "data/common/biomes.json"));
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
        WriteHostFile(root / "data/common/systems.json",
                      R"({"sim":{"active_regions":{"enabled":true}}})");
        previous_logger = Log::GetCoreLogger();
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(log_output);
        auto logger = std::make_shared<spdlog::logger>("host-fault-test", sink);
        logger->set_pattern("%v");
        Log::GetCoreLogger() = std::move(logger);
        HostObservation() = &observation;
    }

    void TearDown() override {
        WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
        HostObservation() = nullptr;
        Log::GetCoreLogger() = previous_logger;
        std::error_code error;
        fs::remove_all(root, error);
    }

    ServerCliOptions Options() const {
        ServerCliOptions options;
        options.root = root.generic_string() + "/";
        options.seed = "1337";
        options.ticks = kFaultTick + 5;
        options.surface_radius = 1;
        options.collision_radius = 1;
        options.test_streaming_radius_cap = 1;
        options.availability_trace = true;
        options.record_path = (root / "recording.lrec").string();
        return options;
    }

    void ExpectRecoveryWorld(const fs::path& directory, const std::string& expected_contents) {
        ASSERT_TRUE(WorldSaveService::validate_save(directory));
        Luminumbra::WorldStreamingState saved;
        std::vector<std::string> errors;
        ASSERT_TRUE(WorldSaveService{}.load_world(saved, directory, errors));
        EXPECT_FALSE(saved.empty());
        EXPECT_EQ(Luminumbra::Persistence::ComputeRegionContentHash(saved), expected_contents);
        Luminumbra::world::WorldClock saved_clock;
        bool requires_regions = false;
        ASSERT_TRUE(WorldSaveService::read_clock_metadata(
            directory, saved_clock, requires_regions, &errors));
        EXPECT_TRUE(requires_regions);
        EXPECT_EQ(saved_clock.tick(), kFaultTick);
        Luminumbra::world::ActiveRegionLedger saved_ledger;
        ASSERT_TRUE(
            WorldSaveService::load_active_regions(saved_ledger, directory, &errors, saved_clock));
        EXPECT_EQ(saved_ledger.header_tick(), kFaultTick);
        EXPECT_EQ(saved_ledger.records().at(kFaultRegion).last_save, kFaultTick);
    }
};

TEST_F(HostSchedulerFault, ServerSavesTheFaultedWorldBeforeReturningExitThree) {
    const auto options = Options();
    EXPECT_EQ(RunServer(options), 3);
    EXPECT_EQ(observation.ticks, kFaultTick);
    EXPECT_EQ(observation.frames, kFaultTick);
    // No streaming/trace publication after the faulting tick, including when a
    // later loop iteration would catch the already latched fault.
    EXPECT_EQ(observation.availability_ticks, kFaultTick - 1);
    ASSERT_FALSE(observation.fault_message.empty());
    ASSERT_TRUE(observation.explicit_shutdown);
    ASSERT_TRUE(observation.recovery_saved);
    const auto metadata = nlohmann::json::parse(observation.metadata_at_shutdown);
    EXPECT_EQ(metadata.at("simulationTick"), kFaultTick);
    EXPECT_EQ(metadata.at("seed"), "1337");
    EXPECT_EQ(metadata.at("name"), "Headless Server World");
    ASSERT_NO_FATAL_FAILURE(
        ExpectRecoveryWorld(observation.save_dir, observation.expected_contents));
}

TEST_F(HostSchedulerFault, RecordFaultAbortsWithoutPublishingReplayOrRegionTrace) {
    const auto options = Options();
    EXPECT_EQ(RunRecord(options), 1);
    ASSERT_FALSE(observation.fault_message.empty());
    EXPECT_EQ(observation.ticks, kFaultTick);
    EXPECT_EQ(observation.frames, kFaultTick);
    EXPECT_EQ(observation.availability_ticks, kFaultTick - 1);
    EXPECT_FALSE(fs::exists(options.record_path));
    EXPECT_FALSE(fs::exists(options.record_path + ".regions.json"));
    const auto diagnostic =
        "record: region scheduler fault at tick 3: " + observation.fault_message +
        "; recording aborted";
    EXPECT_NE(log_output.str().find(diagnostic), std::string::npos);
}

TEST_F(HostSchedulerFault, RecordFaultPreservesAnExistingReplayAndTraceByteForByte) {
    const auto options = Options();
    Luminumbra::Replay::ReplayHeader header;
    header.seed_string = "1337";
    header.seed = 1337;
    header.preset = "default";
    Luminumbra::Replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(options.record_path, header));
    writer.RecordInput(1, {});
    ASSERT_TRUE(writer.Finalize(1));
    ASSERT_TRUE(Luminumbra::Replay::ReadReplay(options.record_path).has_value());
    std::string error;
    ASSERT_TRUE(Luminumbra::Replay::WriteRegionScheduleTrace(
        options.record_path, {{1, "0123456789abcdef"}}, error))
        << error;
    const auto previous_replay = ReadHostFile(options.record_path);
    const auto previous_trace = ReadHostFile(options.record_path + ".regions.json");
    EXPECT_EQ(RunRecord(options), 1);
    ASSERT_FALSE(observation.fault_message.empty());
    EXPECT_EQ(ReadHostFile(options.record_path), previous_replay);
    EXPECT_EQ(ReadHostFile(options.record_path + ".regions.json"), previous_trace);
    const auto diagnostic =
        "record: region scheduler fault at tick 3: " + observation.fault_message +
        "; recording aborted";
    EXPECT_NE(log_output.str().find(diagnostic), std::string::npos);
}

TEST_F(HostSchedulerFault, HealthyRecordPublishesACompleteReplayAndRegionTrace) {
    observation.inject_fault = false;
    auto options = Options();
    options.ticks = 2;
    ASSERT_EQ(RunRecord(options), 0);
    const auto recording = Luminumbra::Replay::ReadReplay(options.record_path);
    ASSERT_TRUE(recording.has_value());
    if (recording) {
        EXPECT_EQ(recording.value().tick_count, options.ticks);
    }
    Luminumbra::Replay::RegionScheduleTrace trace;
    std::string error;
    ASSERT_TRUE(Luminumbra::Replay::ReadRegionScheduleTrace(
        options.record_path, options.ticks, trace, error))
        << error;
    ASSERT_EQ(trace.size(), options.ticks);
    EXPECT_EQ(trace.front().first, 1u);
    EXPECT_EQ(trace.back().first, options.ticks);
}

TEST_F(HostSchedulerFault, ClientFaultRequestsQuitAndSavesStateBeforeSpawnMetadata) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetJobSystem(&jobs);
    session.SetRootPath(root.generic_string() + "/");
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Client fault", "1337", "default"));
    ASSERT_NO_FATAL_FAILURE(ArmHostFault(session));
    // Headless client needs no streamed terrain: a dirty CPU chunk supplies the
    // recovery-save obligation without constructing a renderer or window.
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Meshing);
    chunk->heightmap_data = {731.0f};
    chunk->mark_sdf_loaded_or_edited();
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    bool quit_requested = false;
    Luminumbra::Client::PollSessionFault(session, [&] { quit_requested = true; });
    EXPECT_FALSE(quit_requested);
    for (std::uint64_t tick = 1; tick <= kFaultTick; ++tick)
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(session.GetRegionSchedulerFault().has_value());
    Luminumbra::Client::PollSessionFault(session, [&] { quit_requested = true; });
    ASSERT_TRUE(quit_requested);
    EXPECT_NE(log_output.str().find("; saving and quitting"), std::string::npos);
    // The injected corrupt file also refuses saving. Keep the client's existing
    // failure policy: neither update spawn metadata nor report a successful save.
    Luminumbra::Client::SaveSessionOnExit(
        &session,
        [] { FAIL() << "A failed state save must not reach the spawn update"; },
        [] { FAIL() << "A failed state save must not be reported as successful"; });
    EXPECT_TRUE(quit_requested);
    ASSERT_NO_FATAL_FAILURE(RemoveHostFaultFile(session));
    const auto expected_contents = LiveAuthoritativeContents(session);
    const auto directory = session.GetWorldSaveDir();
    bool spawn_updated = false;
    bool saved_reported = false;
    const Luminumbra::Vec3 exit_spawn(19.0f, 23.0f, -17.0f);
    Luminumbra::Client::SaveSessionOnExit(
        &session,
        [&] {
            // This callback runs only AFTER the recovery state is durable.
            ASSERT_NO_FATAL_FAILURE(ExpectRecoveryWorld(directory, expected_contents));
            session.SetSpawnPoint(exit_spawn);
            spawn_updated = true;
        },
        [&] {
            EXPECT_TRUE(spawn_updated);
            const auto metadata =
                nlohmann::json::parse(ReadHostFile(directory / "world_info.json"));
            EXPECT_EQ(metadata.at("spawnPoint").at("x"), exit_spawn.x);
            EXPECT_EQ(metadata.at("spawnPoint").at("y"), exit_spawn.y);
            EXPECT_EQ(metadata.at("spawnPoint").at("z"), exit_spawn.z);
            saved_reported = true;
        });
    EXPECT_TRUE(spawn_updated);
    EXPECT_TRUE(saved_reported);
    // GLFW loop exit and renderer reader draining remain NEEDS-NATIVE. This
    // test covers the production quit/save decisions without simulating a window.
    ASSERT_NO_FATAL_FAILURE(ExpectRecoveryWorld(directory, expected_contents));
}

TEST_F(HostSchedulerFault, ClientMetadataSaveFailureSuppressesSuccessAndAllowsRetry) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetJobSystem(&jobs);
    session.SetRootPath(root.generic_string() + "/");
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Client metadata failure", "1337", "default"));
    ASSERT_NO_FATAL_FAILURE(ArmHostFault(session));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Meshing);
    chunk->heightmap_data = {731.0f};
    chunk->mark_sdf_loaded_or_edited();
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    for (std::uint64_t tick = 1; tick <= kFaultTick; ++tick)
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    const auto fault_optional = session.GetRegionSchedulerFault();
    ASSERT_TRUE(fault_optional.has_value());
    if (!fault_optional.has_value())
        return;
    const auto& fault = fault_optional.value();
    const auto expect_consistent_session = [&] {
        const auto& current_fault = session.GetRegionSchedulerFault();
        ASSERT_TRUE(current_fault.has_value());
        if (!current_fault.has_value())
            return;
        EXPECT_EQ(current_fault->tick, fault.tick);
        EXPECT_EQ(current_fault->message, fault.message);
        EXPECT_EQ(session.GetSimulationTickCount(), kFaultTick);
        EXPECT_TRUE(session.IsSimulationTickBoundary());
        EXPECT_TRUE(session.GetWorldOpenError().empty());
    };

    // Establish the state-save failure baseline: edits and save stamps remain pending.
    const auto unsaved_ledger = session.GetActiveRegionLedger().encode();
    Luminumbra::Client::SaveSessionOnExit(
        &session,
        [] { FAIL() << "A failed state save must not reach the spawn update"; },
        [] { FAIL() << "A failed state save must not be reported as successful"; });
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_EQ(session.GetActiveRegionLedger().encode(), unsaved_ledger);
    ASSERT_NO_FATAL_FAILURE(expect_consistent_session());
    ASSERT_NO_FATAL_FAILURE(RemoveHostFaultFile(session));
    const auto expected_contents = LiveAuthoritativeContents(session);
    const auto directory = session.GetWorldSaveDir();
    const auto previous_spawn = session.GetMetadata().spawnPoint;
    const Luminumbra::Vec3 exit_spawn(19.0f, 23.0f, -17.0f);
    bool spawn_updated = false;
    bool saved_reported = false;
    std::string durable_metadata;
    std::string durable_ledger;
    std::string saved_ledger;
    Luminumbra::Client::SaveSessionOnExit(
        &session,
        [&] {
            // State is durable before arming the existing one-shot seam, so only
            // the subsequent SaveWorld metadata replacement can fail.
            ASSERT_NO_FATAL_FAILURE(ExpectRecoveryWorld(directory, expected_contents));
            EXPECT_FALSE(chunk->is_voxel_data_dirty());
            durable_metadata = ReadHostFile(directory / "world_info.json");
            durable_ledger = ReadHostFile(WorldSaveService::active_regions_path(directory));
            saved_ledger = session.GetActiveRegionLedger().encode();
            session.SetSpawnPoint(exit_spawn);
            spawn_updated = true;
            WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
        },
        [&] { saved_reported = true; });
    ASSERT_TRUE(spawn_updated);
    EXPECT_FALSE(saved_reported);
    // The failed metadata write preserves the successful state save's clean
    // chunks and save stamps, and retains the same fault as the failed state save.
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    EXPECT_EQ(session.GetActiveRegionLedger().encode(), saved_ledger);
    EXPECT_EQ(ReadHostFile(directory / "world_info.json"), durable_metadata);
    EXPECT_EQ(ReadHostFile(WorldSaveService::active_regions_path(directory)), durable_ledger);
    EXPECT_EQ(session.GetMetadata().spawnPoint, exit_spawn);
    const auto metadata = nlohmann::json::parse(durable_metadata);
    EXPECT_EQ(metadata.at("spawnPoint").at("x"), previous_spawn.x);
    EXPECT_EQ(metadata.at("spawnPoint").at("y"), previous_spawn.y);
    EXPECT_EQ(metadata.at("spawnPoint").at("z"), previous_spawn.z);
    ASSERT_NO_FATAL_FAILURE(expect_consistent_session());
    ASSERT_NO_FATAL_FAILURE(ExpectRecoveryWorld(directory, expected_contents));

    // The one-shot seam is consumed; retry has no pending chunk writes.
    Luminumbra::world::WorldStateSaveReport retry;
    ASSERT_TRUE(session.SaveWorldState(&retry));
    EXPECT_EQ(retry.chunks_dirty, 0u);
    EXPECT_EQ(retry.chunks_saved, 0u);
    EXPECT_FALSE(retry.saved);
    Luminumbra::Client::SaveSessionOnExit(
        &session, [&] { session.SetSpawnPoint(exit_spawn); }, [&] { saved_reported = true; });
    EXPECT_TRUE(saved_reported);
    const auto retried_metadata =
        nlohmann::json::parse(ReadHostFile(directory / "world_info.json"));
    EXPECT_EQ(retried_metadata.at("spawnPoint").at("x"), exit_spawn.x);
    EXPECT_EQ(retried_metadata.at("spawnPoint").at("y"), exit_spawn.y);
    EXPECT_EQ(retried_metadata.at("spawnPoint").at("z"), exit_spawn.z);
    ASSERT_NO_FATAL_FAILURE(expect_consistent_session());
    ASSERT_NO_FATAL_FAILURE(ExpectRecoveryWorld(directory, expected_contents));
}
} // namespace
