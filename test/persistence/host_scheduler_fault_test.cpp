#include "../support/SimulationContentFixture.h"
#include <gtest/gtest.h>

#include "luminumbra_client/app/SessionExit.h"
#include "luminumbra_common/ai/CreaturePersistence.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/replay/RegionScheduleTrace.h"
#include "luminumbra_server/HeavyComparison.h"
#include "luminumbra_server/SimBudgetArtifact.h"
#include "luminumbra_server/modes/ModeHelpers.h"
#include "luminumbra_server/modes/Modes.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
    unsigned fault_kind = 0;
    bool inject_fault = true;
    bool fail_recovery_save = false;
    bool fail_shutdown_io = false;
    bool fault_before_shutdown = false;
    bool fault_after_shutdown = false;
    unsigned shutdown_preparation_calls = 0;
    unsigned shutdown_io_calls = 0;
    int capture_stage = 0;
    int heavy_fault = 0;
    unsigned original_calls = 0, loaded_calls = 0;
    unsigned birth_construction_calls = 0;
    std::uint64_t physics_steps = 0, streaming_frames = 0;
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

Luminumbra::Server::ServerWorldRunnerConfig
ConfigureHostFault(Luminumbra::Server::ServerWorldRunnerConfig config) {
    if (HostObservation()->fault_kind == 11 || HostObservation()->fault_kind == 12)
        WriteHostFile(
            fs::path(config.root_path) / "data/common/sim/distant_policy.json",
            R"({"schema":"luminumbra.distant_policy.v1","creature_species":{"fixture_creature":{"coarse_need_rate_ppm":0,"coarse_travel_m_per_cadence":0,"promote_distance_m":512,"lifespan_cadences":0,"reproduce_cadences":1}},"plant_species":{}})");
    if (HostObservation()->fault_kind == 1 || HostObservation()->fault_kind == 7 ||
        HostObservation()->fault_kind == 8) {
        Luminumbra::world::RegionPopulator populator;
        populator.contract = "fault-population:v1";
        populator.canonical_configuration = "founder-preparation";
        const bool invalid = HostObservation()->fault_kind == 1;
        populator.populate = [invalid](const Luminumbra::world::RegionPopulateRequest& request) {
            if (request.key != kFaultRegion)
                return;
            const Luminumbra::Vec3 position{static_cast<float>(request.key.x) * 512.0f + 128.0f,
                                            100.0f,
                                            static_cast<float>(request.key.z) * 512.0f + 128.0f};
            request.sink.spawn(Luminumbra::world::CreatureSpawnCommand{
                Luminumbra::Components::CreatureSpeciesId16("fixture_creature"), position, false});
            if (invalid)
                request.sink.spawn(
                    Luminumbra::world::CreatureSpawnCommand{std::uint16_t{0}, position, false});
        };
        config.region_populator = std::move(populator);
    }
    return config;
}
void ArmCreatureHostFault(GameSession& session, unsigned kind) {
    session.GetSimulationEventBus().subscribe([&session, kind](const auto& event) {
        if (event.topic == "host.prepare") {
            session.PinActiveRegion(kFaultRegion, true);
            if (kind == 2 || kind == 4 || kind == 6 || kind >= 10) {
                namespace C = Luminumbra::Components;
                auto& registry = session.GetRegistry();
                // Avatar anchors are restored by the runner before each tick. Keep coarse
                // parents in the pinned distant owner so those anchors cannot promote them.
                const auto birth_position =
                    kind == 11 || kind == 12
                        ? Luminumbra::Vec3{static_cast<float>(kFaultRegion.x) * 512.0f + 128.0f,
                                           100.0f,
                                           static_cast<float>(kFaultRegion.z) * 512.0f + 128.0f}
                        : session.GetMetadata().spawnPoint;
                const auto entity = registry.create();
                registry.emplace<C::TransformComponent>(entity).position = birth_position;
                registry.emplace<C::CreatureComponent>(entity).species_id =
                    C::CreatureSpeciesId16("fixture_creature");
                ASSERT_TRUE(session.AdmitCreature(entity, kind == 2));
                if (kind == 4 || kind == 13)
                    registry.get<C::TransformComponent>(entity).position = {
                        static_cast<float>(kFaultRegion.x) * 512.0f + 128.0f,
                        100.0f,
                        static_cast<float>(kFaultRegion.z) * 512.0f + 128.0f};
                if (kind == 4 || kind == 13) {
                    // Move the persistent owner together with the valid source projection.
                    auto& owner = registry.get<C::CreatureRegionComponent>(entity);
                    owner.owner_x = kFaultRegion.x;
                    owner.owner_z = kFaultRegion.z;
                }
                if (kind == 6 || (kind >= 10 && kind <= 12)) {
                    auto& female = registry.emplace<C::CreatureGenomeComponent>(entity);
                    female.female = true;
                    female.age_ticks = 120;
                    registry.get<C::CreatureComponent>(entity).hunger = .05f;
                    const auto male = registry.create();
                    registry.emplace<C::TransformComponent>(male).position = birth_position;
                    registry.emplace<C::CreatureComponent>(male).species_id =
                        C::CreatureSpeciesId16("fixture_creature");
                    ASSERT_TRUE(session.AdmitCreature(male));
                    auto& genome = registry.emplace<C::CreatureGenomeComponent>(male);
                    genome.female = false;
                    genome.age_ticks = 120;
                    registry.get<C::CreatureComponent>(male).hunger = .05f;
                    if (kind == 11 || kind == 12) {
                        for (const auto parent : {entity, male}) {
                            auto& coarse = registry.get<C::CreatureCoarseComponent>(parent);
                            coarse.tier = C::CreatureTier::Coarse;
                            coarse.age_cadences = 120;
                            coarse.reproduce_cadences = 120;
                            coarse.courting_cadences = 2;
                            coarse.hunger_ppm = 50000;
                        }
                    }
                }
            }
        }
        if (kind == 5 && event.topic == "host.admit") {
            namespace C = Luminumbra::Components;
            auto& registry = session.GetRegistry();
            const auto entity = registry.create();
            registry.emplace<C::TransformComponent>(entity).position =
                session.GetMetadata().spawnPoint;
            registry.emplace<C::CreatureComponent>(entity).species_id =
                C::CreatureSpeciesId16("fixture_creature");
            try {
                (void)session.AdmitCreature(entity, true);
            } catch (...) {
                registry.destroy(entity); // The event's source remains the caller's responsibility.
                throw;
            }
        }
        if (kind == 3 && event.activation && event.activation->x == kFaultRegion.x &&
            event.activation->z == kFaultRegion.z)
            throw std::runtime_error("Activation host delivery failure");
    });
    session.GetSimulationEventBus().publish(kFaultTick - 1, "host.prepare", "");
    if (kind == 5)
        session.GetSimulationEventBus().publish(kFaultTick, "host.admit", "");
    if (kind == 4 || kind == 5 || kind == 13)
        session.SetCreatureProjectionPreparationHook([&session, kind](entt::entity) {
            if (kind == 13 && session.GetWorldClock().tick() == kFaultTick)
                throw 7;
            if (session.GetWorldClock().tick() == kFaultTick)
                throw std::runtime_error("Creature host projection failure");
        });
    if (kind == 7 || kind == 8) {
        const auto fail = [&session] {
            if (session.GetWorldClock().tick() == kFaultTick)
                throw std::runtime_error("Population host preparation failure");
        };
        session.SetPopulationPreparationHooks(
            kind == 7 ? Luminumbra::world::PopulationPreparationHooks{fail, {}}
                      : Luminumbra::world::PopulationPreparationHooks{{}, fail});
    }
    if (kind == 9)
        session.SetActivationEnqueuePreparationHook([&session] {
            if (session.GetWorldClock().tick() == kFaultTick)
                throw std::runtime_error("Activation host enqueue failure");
        });
    if (kind == 6 || kind == 10) {
        luminumbra::ai::ReproductionTuning tuning;
        tuning.courtship_ticks = 1;
        session.SetReproductionTuning(tuning);
    }
    if (kind == 2)
        session.SetCreatureControllerPreparationHook(
            [] { throw std::runtime_error("Creature host controller failure"); });
}
} // namespace

namespace Luminumbra::Server {
// Compile the shipping mode bodies below against this test-only adapter. Only
// setup, removal of injected corruption and observations wrap the real runner;
// RunFixedTicks, fault reporting and Shutdown/save all execute production code.
class SchedulerFaultTestRunner : public ServerWorldRunner {
public:
    explicit SchedulerFaultTestRunner(ServerWorldRunnerConfig config)
        : ServerWorldRunner(ConfigureHostFault(config))
        , loaded_(config.world_name == "Heavy Loaded") {}

    bool Boot() {
        if (!ServerWorldRunner::Boot())
            return false;
        HostObservation()->save_dir = Session()->GetWorldSaveDir();
        Session()->SetSimBudgetTelemetryEnabled(true);
        if (HostObservation()->fault_kind == 6 || HostObservation()->fault_kind == 12)
            birth_connection_ = Session()
                                    ->GetRegistry()
                                    .on_construct<Components::PerceptionComponent>()
                                    .connect<&SchedulerFaultTestRunner::ConstructNewborn>(*this);
        if (HostObservation()->fault_kind == 10 || HostObservation()->fault_kind == 11)
            birth_connection_ = Session()
                                    ->GetRegistry()
                                    .on_construct<Components::CreatureGenomeComponent>()
                                    .connect<&SchedulerFaultTestRunner::ConstructNewborn>(*this);
        if (HostObservation()->inject_fault) {
            if (HostObservation()->fault_kind == 0)
                ArmHostFault(*Session());
            else
                ArmCreatureHostFault(*Session(), HostObservation()->fault_kind);
            if (testing::Test::HasFatalFailure())
                return false;
            DirtyHostChunk(*Session());
        }
        return !testing::Test::HasFatalFailure();
    }

    ServerTickReport RunFixedTicks(std::uint64_t count) {
        auto& observation = *HostObservation();
        auto& calls = loaded_ ? observation.loaded_calls : observation.original_calls;
        ++calls;
        if ((observation.heavy_fault == 1 && !loaded_ && calls == 2) ||
            (observation.heavy_fault == 2 && loaded_ && calls == 1)) {
            Session()->PinActiveRegion({30, 30}, true);
            Session()->SetActivationEnqueuePreparationHook(
                [] { throw std::runtime_error("Heavy continuation preparation failure"); });
        }
        const auto report = ServerWorldRunner::RunFixedTicks(count);
        HostObservation()->ticks += report.ticks_executed;
        HostObservation()->frames += report.frames_executed;
        HostObservation()->availability_ticks = AvailabilityTrace().size();
        using Stage = luminumbra::simulation::SimBudgetStage;
        observation.physics_steps =
            Session()->GetSimBudgetTelemetry().Summarize(Stage::ServerPhysics).work_total;
        observation.streaming_frames =
            Session()->GetSimBudgetTelemetry().Summarize(Stage::ServerStreaming).samples;
        if (report.region_fault && HostObservation()->fault_message.empty()) {
            HostObservation()->fault_message = report.region_fault_message;
            EXPECT_TRUE(Session()->GetRegionSchedulerFault().has_value());
            if (observation.heavy_fault == 0)
                EXPECT_EQ(Session()->GetSimulationTickCount(), kFaultTick);
            const auto physics =
                Session()->GetSimBudgetTelemetry().Summarize(Stage::ServerPhysics).samples;
            const auto streaming =
                Session()->GetSimBudgetTelemetry().Summarize(Stage::ServerStreaming).samples;
            const auto avatars = Avatars();
            const auto world_frames =
                Session()->GetWorldSystem()->get_streaming_telemetry_stats().frames_observed;
            const auto chunks = Session()->GetWorldSystem()->snapshot_streamed_chunks();
            std::vector<std::uint64_t> mesh_versions;
            for (const auto& chunk : chunks)
                mesh_versions.push_back(chunk->mesh_version);
            const auto refused = ServerWorldRunner::RunFixedTicks(3);
            EXPECT_EQ(refused.ticks_executed, 0u);
            EXPECT_EQ(refused.frames_executed, 0u);
            EXPECT_EQ(Session()->GetSimBudgetTelemetry().Summarize(Stage::ServerPhysics).samples,
                      physics);
            EXPECT_EQ(Session()->GetSimBudgetTelemetry().Summarize(Stage::ServerStreaming).samples,
                      streaming);
            EXPECT_EQ(Session()->GetWorldSystem()->get_streaming_telemetry_stats().frames_observed,
                      world_frames);
            for (std::size_t index = 0; index < chunks.size(); ++index)
                EXPECT_EQ(chunks[index]->mesh_version, mesh_versions[index]);
            EXPECT_EQ(Avatars().size(), avatars.size());
            for (std::size_t index = 0; index < avatars.size(); ++index) {
                EXPECT_EQ(Avatars()[index].position, avatars[index].position);
                EXPECT_EQ(Avatars()[index].velocity, avatars[index].velocity);
            }
            if (HostObservation()->inject_fault && HostObservation()->fault_kind == 0)
                RemoveHostFaultFile(*Session());
            HostObservation()->expected_contents = LiveAuthoritativeContents(*Session());
        }
        return report;
    }

    std::string ComputeWorldHash() {
        ++world_captures_;
        if ((HostObservation()->capture_stage == 1 && world_captures_ == 1) ||
            (HostObservation()->capture_stage == 3 && world_captures_ == 2))
            FailPendingCapture();
        return ServerWorldRunner::ComputeWorldHash();
    }
    void ComputeWorldHashAndSubHashes(std::string& hash,
                                      Persistence::WorldStreamingStateSubHashes& sub) {
        if (HostObservation()->capture_stage == 2)
            FailPendingCapture();
        ServerWorldRunner::ComputeWorldHashAndSubHashes(hash, sub);
    }
    void Shutdown() {
        // Capture before the destructor can save: removing RunServer's explicit
        // Shutdown must fail even though the real runner also saves on destruction.
        HostObservation()->explicit_shutdown = true;
        HostObservation()->fault_before_shutdown = Session()->GetRegionSchedulerFault().has_value();
        world::WorldStateSaveReport report;
        if (HostObservation()->fail_recovery_save)
            Session()->SetPendingStatePreparationHook([](const char*) {
                ++HostObservation()->shutdown_preparation_calls;
                throw std::runtime_error("Recovery save preparation failure");
            });
        if (HostObservation()->fail_shutdown_io)
            WorldSaveService::set_before_active_regions_replace_for_testing(+[] {
                ++HostObservation()->shutdown_io_calls;
                return false;
            });
        birth_connection_.release();
        ServerWorldRunner::Shutdown(&report);
        WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
        if (const auto& fault = GetRegionSchedulerFault()) {
            HostObservation()->fault_after_shutdown = true;
            const auto message = fault->message;
            const auto tick = fault->tick;
            ServerWorldRunner::Shutdown();
            const auto& preserved = GetRegionSchedulerFault();
            ASSERT_TRUE(preserved);
            EXPECT_EQ(preserved->message, message);
            EXPECT_EQ(preserved->tick, tick);
        }
        HostObservation()->recovery_saved = report.saved;
        HostObservation()->metadata_at_shutdown =
            ReadHostFile(HostObservation()->save_dir / "world_info.json");
    }

private:
    bool loaded_ = false;
    unsigned world_captures_ = 0;
    entt::scoped_connection birth_connection_;
    void ConstructNewborn(entt::registry& registry, entt::entity entity) {
        if (!registry.all_of<Components::CreatureRegionComponent>(entity)) {
            ++HostObservation()->birth_construction_calls;
            EXPECT_EQ(Session()->GetWorldClock().tick(), kFaultTick);
            if (HostObservation()->fault_kind == 11 || HostObservation()->fault_kind == 12) {
                unsigned parents = 0;
                for (const auto parent : registry.view<Components::CreatureRegionComponent,
                                                       Components::CreatureGenomeComponent>()) {
                    ++parents;
                    EXPECT_EQ(registry.get<Components::CreatureCoarseComponent>(parent).tier,
                              Components::CreatureTier::Coarse);
                    const auto& owner = registry.get<Components::CreatureRegionComponent>(parent);
                    EXPECT_EQ(owner.owner_x, kFaultRegion.x);
                    EXPECT_EQ(owner.owner_z, kFaultRegion.z);
                }
                EXPECT_EQ(parents, 2u);
            }
            throw std::runtime_error("Host newborn construction failure");
        }
    }
    void FailPendingCapture() {
        Session()->SetPendingStatePreparationHook([](const char*) {
            throw std::runtime_error("Host pending capture preparation failure");
        });
    }
};
} // namespace Luminumbra::Server

// Headers are included above before substitution. Keep the mode control flow
// verbatim so deleting its fault branch, Finalize guard or explicit shutdown
// changes the behavior under test. These sources are not separately linked.
// Isolate every mode definition, as in CreatureHostAcceptance_test.cpp.
namespace HostSchedulerFaultModes {
#define ServerWorldRunner SchedulerFaultTestRunner
#include "../../src/luminumbra_server/modes/Heavy.cpp"
#include "../../src/luminumbra_server/modes/Replay.cpp"
#include "../../src/luminumbra_server/modes/Server.cpp"
#undef ServerWorldRunner
} // namespace HostSchedulerFaultModes

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
    EXPECT_EQ(HostSchedulerFaultModes::RunServer(options), 3);
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
    EXPECT_EQ(HostSchedulerFaultModes::RunRecord(options), 1);
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
    EXPECT_EQ(HostSchedulerFaultModes::RunRecord(options), 1);
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
    ASSERT_EQ(HostSchedulerFaultModes::RunRecord(options), 0);
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
} // namespace

TEST_F(HostSchedulerFault, PopulationFaultSavesCoherentStateAndReturnsFaultExit) {
    observation.fault_kind = 1;
    const auto options = Options();
    EXPECT_EQ(HostSchedulerFaultModes::RunServer(options), 3);
    EXPECT_EQ(observation.ticks, kFaultTick);
    ASSERT_TRUE(observation.explicit_shutdown);
    ASSERT_TRUE(observation.recovery_saved);
    ASSERT_NO_FATAL_FAILURE(
        ExpectRecoveryWorld(observation.save_dir, observation.expected_contents));
    luminumbra::ai::CreatureSnapshot creatures;
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService::load_creature_entities(creatures, observation.save_dir, &errors));
    EXPECT_TRUE(creatures.entities.empty());
    Luminumbra::world::ActiveRegionLedger ledger;
    ASSERT_TRUE(WorldSaveService::load_active_regions(
        ledger, observation.save_dir, &errors, Luminumbra::world::WorldClock(kFaultTick)));
    EXPECT_FALSE(ledger.records().at(kFaultRegion).populated);
    EXPECT_EQ(ledger.next_persistent_id(), 1u);
    EXPECT_TRUE(ledger.completed_work().empty());
}

TEST_F(HostSchedulerFault, CreatureConsumerFaultStopsRemainingFrameTicks) {
    observation.fault_kind = 2;
    auto options = Options();
    options.avatars = 1;
    EXPECT_EQ(HostSchedulerFaultModes::RunServer(options), 3);
    EXPECT_EQ(observation.ticks, kFaultTick);
    EXPECT_EQ(observation.frames, kFaultTick);
    EXPECT_EQ(observation.availability_ticks, kFaultTick - 1);
    ASSERT_TRUE(observation.recovery_saved);
    ASSERT_NO_FATAL_FAILURE(
        ExpectRecoveryWorld(observation.save_dir, observation.expected_contents));
}

TEST_F(HostSchedulerFault, ActivationDeliveryFaultRetainsRecoverableOutbox) {
    observation.fault_kind = 3;
    const auto options = Options();
    EXPECT_EQ(HostSchedulerFaultModes::RunServer(options), 3);
    EXPECT_EQ(observation.ticks, kFaultTick);
    ASSERT_TRUE(observation.recovery_saved);
    ASSERT_NO_FATAL_FAILURE(
        ExpectRecoveryWorld(observation.save_dir, observation.expected_contents));
    Luminumbra::world::ActiveRegionLedger ledger;
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService::load_active_regions(
        ledger, observation.save_dir, &errors, Luminumbra::world::WorldClock(kFaultTick)));
    ASSERT_EQ(ledger.pending_activations().size(), 1u);
    const auto& pending = ledger.pending_activations().begin()->second;
    EXPECT_EQ(pending.key, kFaultRegion);
    EXPECT_EQ(pending.activation_tick, kFaultTick);
    EXPECT_EQ(pending.cause, Luminumbra::world::ActivationCause::Pin);
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    GameSession restored;
    restored.SetJobSystem(&jobs);
    restored.SetRootPath(root.generic_string() + "/");
    restored.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(restored.LoadWorld(observation.save_dir.filename().string()));
    EXPECT_EQ(restored.GetActiveRegionLedger().pending_activations(), ledger.pending_activations());
    EXPECT_EQ(restored.TickSimulation(restored.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_FALSE(restored.GetRegionSchedulerFault());
    EXPECT_TRUE(restored.GetActiveRegionLedger().pending_activations().empty());
}

namespace {
class LockstepReplayHardening : public HostSchedulerFault {
protected:
    ServerCliOptions PopulatedReplayOptions() {
        observation.inject_fault = false;
        for (const auto* name : {"grovestrider", "ridgeback_stalker"})
            WriteHostFile(root / "data/common/creatures/species" / (std::string(name) + ".json"),
                          "{\"id\":\"" + std::string(name) + "\"}");
        auto options = Options();
        options.ecology_roster = true;
        options.avatars = 1;
        options.ticks = 30;
        options.artifact_path = (root / "replay-result.json").string();
        return options;
    }
};
TEST_F(LockstepReplayHardening, EnabledFreshWorldCreatureReplayMatchesStartAndCheckpoints) {
    auto options = PopulatedReplayOptions();
    ASSERT_EQ(HostSchedulerFaultModes::RunRecord(options), 0);
    const auto recording = Luminumbra::Replay::ReadReplay(options.record_path);
    ASSERT_TRUE(recording);
    ASSERT_EQ(recording->checkpoints.size(), 1u);
    EXPECT_EQ(recording->checkpoints[0].tick, 30u);
    EXPECT_FALSE(recording->header.start_world_hash.empty());
    observation.ticks = 0;
    options.replay_path = options.record_path;
    ASSERT_EQ(HostSchedulerFaultModes::RunReplay(options), 0);
    EXPECT_EQ(observation.ticks, 30u);
    const auto artifact = nlohmann::json::parse(ReadHostFile(options.artifact_path));
    EXPECT_EQ(artifact.at("start_world_hash_match"), true);
    EXPECT_EQ(artifact.at("checkpoints_verified"), 1u);
}
TEST_F(LockstepReplayHardening, ChangedCreaturePolicyRefusesBeforeReplayTicks) {
    auto options = PopulatedReplayOptions();
    ASSERT_EQ(HostSchedulerFaultModes::RunRecord(options), 0);
    WriteHostFile(
        root / "data/common/sim/distant_policy.json",
        R"({"schema":"luminumbra.distant_policy.v1","creature_species":{"fixture_creature":{"coarse_need_rate_ppm":1,"coarse_travel_m_per_cadence":0,"promote_distance_m":512,"lifespan_cadences":0,"reproduce_cadences":0}},"plant_species":{}})");
    observation.ticks = 0;
    options.replay_path = options.record_path;
    EXPECT_EQ(HostSchedulerFaultModes::RunReplay(options), 1);
    EXPECT_EQ(observation.ticks, 0u);
    const auto artifact = nlohmann::json::parse(ReadHostFile(options.artifact_path));
    EXPECT_EQ(artifact.at("divergence_tick"), 0u);
    EXPECT_EQ(artifact.at("checkpoints_verified_before_divergence"), 0u);
}
TEST_F(LockstepReplayHardening, ChangedPopulationContractRefusesBeforeReplayTicks) {
    auto options = PopulatedReplayOptions();
    ASSERT_EQ(HostSchedulerFaultModes::RunRecord(options), 0);
    observation.ticks = 0;
    options.replay_path = options.record_path;
    options.ecology_roster = false;
    EXPECT_EQ(HostSchedulerFaultModes::RunReplay(options), 1);
    EXPECT_EQ(observation.ticks, 0u);
    const auto artifact = nlohmann::json::parse(ReadHostFile(options.artifact_path));
    EXPECT_EQ(artifact.at("divergence_tick"), 0u);
    EXPECT_EQ(artifact.at("checkpoints_verified_before_divergence"), 0u);
}
TEST_F(LockstepReplayHardening, EnabledCreatureFaultAbortsRecordingWithoutCompletedSidecar) {
    auto options = PopulatedReplayOptions();
    observation.inject_fault = true;
    observation.fault_kind = 2;
    EXPECT_EQ(HostSchedulerFaultModes::RunRecord(options), 1);
    EXPECT_EQ(observation.ticks, kFaultTick);
    EXPECT_FALSE(fs::exists(options.record_path));
    EXPECT_FALSE(fs::exists(options.record_path + ".regions.json"));
    EXPECT_FALSE(fs::exists(options.artifact_path));
    EXPECT_FALSE(observation.fault_message.empty());
}
} // namespace

TEST_F(HostSchedulerFault, ServerFaultMatrixStopsPhysicsAndStreamingPublication) {
    for (unsigned kind : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u}) {
        SCOPED_TRACE(kind);
        observation = {};
        observation.fault_kind = kind;
        auto options = Options();
        options.avatars = 1;
        EXPECT_EQ(HostSchedulerFaultModes::RunServer(options), 3);
        ASSERT_FALSE(observation.fault_message.empty());
        if (kind == 6 || (kind >= 10 && kind <= 12))
            EXPECT_EQ(observation.birth_construction_calls, 1u);
        EXPECT_EQ(observation.ticks, kFaultTick);
        EXPECT_EQ(observation.frames, kFaultTick);
        EXPECT_EQ(observation.physics_steps, kFaultTick);
        EXPECT_EQ(observation.streaming_frames, kFaultTick - 1);
        EXPECT_TRUE(observation.explicit_shutdown);
        EXPECT_TRUE(observation.recovery_saved);
        EXPECT_TRUE(WorldSaveService::validate_save(observation.save_dir));
    }
}
TEST_F(HostSchedulerFault, ServerStillExitsThreeWhenRecoveryPreparationFails) {
    observation.fault_kind = 2;
    observation.fail_recovery_save = true;
    auto options = Options();
    options.avatars = 1;
    EXPECT_EQ(HostSchedulerFaultModes::RunServer(options), 3);
    EXPECT_TRUE(observation.explicit_shutdown);
    EXPECT_FALSE(observation.recovery_saved);
    EXPECT_EQ(observation.physics_steps, kFaultTick);
    EXPECT_EQ(observation.streaming_frames, kFaultTick - 1);
}
TEST_F(HostSchedulerFault, ServerExitsThreeWhenFirstFaultOccursDuringShutdown) {
    observation.inject_fault = false;
    observation.fail_recovery_save = true;
    auto options = Options();
    options.avatars = 1;
    options.ticks = 2;
    EXPECT_EQ(HostSchedulerFaultModes::RunServer(options), 3);
    EXPECT_TRUE(observation.fault_message.empty());
    EXPECT_FALSE(observation.fault_before_shutdown);
    EXPECT_TRUE(observation.fault_after_shutdown);
    EXPECT_EQ(observation.shutdown_preparation_calls, 1u);
    EXPECT_TRUE(observation.explicit_shutdown);
    EXPECT_FALSE(observation.recovery_saved);
    EXPECT_EQ(observation.ticks, options.ticks);
    EXPECT_EQ(observation.frames, options.ticks);
    EXPECT_EQ(observation.physics_steps, options.ticks);
    EXPECT_EQ(observation.streaming_frames, options.ticks);
}
TEST_F(HostSchedulerFault, RetryableShutdownIoFailureDoesNotBecomeSimulationFault) {
    observation.inject_fault = false;
    observation.fail_shutdown_io = true;
    auto options = Options();
    options.ticks = 2;
    EXPECT_EQ(HostSchedulerFaultModes::RunServer(options), 0);
    EXPECT_TRUE(observation.fault_message.empty());
    EXPECT_FALSE(observation.fault_before_shutdown);
    EXPECT_FALSE(observation.fault_after_shutdown);
    EXPECT_EQ(observation.shutdown_preparation_calls, 0u);
    EXPECT_EQ(observation.shutdown_io_calls, 1u);
    EXPECT_TRUE(observation.explicit_shutdown);
    EXPECT_FALSE(observation.recovery_saved);
    EXPECT_EQ(observation.ticks, options.ticks);
}
TEST_F(HostSchedulerFault, RecordFaultMatrixPreservesAbsentAndExistingArtifacts) {
    for (unsigned kind : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u}) {
        for (const bool existing : {false, true}) {
            SCOPED_TRACE(std::to_string(kind) + (existing ? ":existing" : ":absent"));
            observation = {};
            observation.fault_kind = kind;
            auto options = Options();
            options.avatars = 1;
            fs::remove(options.record_path);
            fs::remove(options.record_path + ".regions.json");
            if (existing) {
                WriteHostFile(options.record_path, "previous complete recording");
                WriteHostFile(options.record_path + ".regions.json", "previous complete sidecar");
            }
            EXPECT_EQ(HostSchedulerFaultModes::RunRecord(options), 1);
            EXPECT_FALSE(observation.fault_message.empty());
            if (kind == 6 || (kind >= 10 && kind <= 12))
                EXPECT_EQ(observation.birth_construction_calls, 1u);
            EXPECT_EQ(observation.physics_steps, kFaultTick);
            EXPECT_EQ(observation.streaming_frames, kFaultTick - 1);
            if (existing) {
                EXPECT_EQ(ReadHostFile(options.record_path), "previous complete recording");
                EXPECT_EQ(ReadHostFile(options.record_path + ".regions.json"),
                          "previous complete sidecar");
            } else {
                EXPECT_FALSE(fs::exists(options.record_path));
                EXPECT_FALSE(fs::exists(options.record_path + ".regions.json"));
            }
        }
    }
}
TEST_F(HostSchedulerFault, BoundaryCaptureNeverFinalizesRecordingOrSidecar) {
    for (int stage : {1, 2, 3}) {
        for (const bool existing : {false, true}) {
            SCOPED_TRACE(std::to_string(stage) + (existing ? ":existing" : ":absent"));
            observation = {};
            observation.inject_fault = false;
            observation.capture_stage = stage;
            auto options = Options();
            options.ticks = stage == 2 ? 30 : 1;
            fs::remove(options.record_path);
            fs::remove(options.record_path + ".regions.json");
            if (existing) {
                WriteHostFile(options.record_path, "previous complete recording");
                WriteHostFile(options.record_path + ".regions.json", "previous complete sidecar");
            }
            EXPECT_EQ(HostSchedulerFaultModes::RunRecord(options), 1);
            if (existing) {
                EXPECT_EQ(ReadHostFile(options.record_path), "previous complete recording");
                EXPECT_EQ(ReadHostFile(options.record_path + ".regions.json"),
                          "previous complete sidecar");
            } else {
                EXPECT_FALSE(fs::exists(options.record_path));
                EXPECT_FALSE(fs::exists(options.record_path + ".regions.json"));
            }
        }
    }
}
TEST_F(HostSchedulerFault, HeavyStopsBeforeSiblingContinuationOrComparisonPublication) {
    for (int target : {1, 2}) {
        SCOPED_TRACE(target);
        observation = {};
        observation.inject_fault = false;
        observation.heavy_fault = target;
        auto options = Options();
        options.ticks = 2;
        options.heavy_resim = 3;
        options.artifact_path = Luminumbra::Filesystem::GenericPathString(root / "heavy.json");
        WriteHostFile(options.artifact_path, "previous complete comparison");
        EXPECT_EQ(HostSchedulerFaultModes::RunHeavy(options), 1);
        EXPECT_EQ(observation.original_calls, 2u);
        EXPECT_EQ(observation.loaded_calls, target == 1 ? 0u : 1u);
        EXPECT_EQ(ReadHostFile(options.artifact_path), "previous complete comparison");
    }
}
TEST_F(HostSchedulerFault, ClientConsumesTheProductionContinuationGate) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(Luminumbra::Filesystem::GenericPathString(root) + "/");
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Client continuation gate", "1337", "default"));
    session.PinActiveRegion({0, 0}, true);
    session.SetActivationEnqueuePreparationHook(
        [] { throw std::runtime_error("Client frame preparation failure"); });
    unsigned physics = 0, steps = 0, autosaves = 0, streaming = 0, dressing = 0;
    bool quit = false;
    const auto stop = [&] {
        return Luminumbra::Client::PollSessionFault(session, [&] { quit = true; });
    };
    // The same decision surrounds every production driver and the frame tail.
    const auto frame = [&] {
        if (stop())
            return;
        ++physics;
        for (unsigned substep = 0; substep < 4; ++substep) {
            ++steps;
            session.TickSimulation(session.GetSimulationClock().fixed_dt());
            if (stop())
                return;
        }
        ++autosaves;
        ++streaming;
        ++dressing;
    };
    frame();
    EXPECT_TRUE(quit);
    EXPECT_EQ(physics, 1u);
    EXPECT_EQ(steps, 1u);
    EXPECT_EQ(autosaves, 0u);
    EXPECT_EQ(streaming, 0u);
    EXPECT_EQ(dressing, 0u);
    frame(); // Prelatched frame exits before physics.
    EXPECT_EQ(physics, 1u);
    EXPECT_EQ(steps, 1u);
    bool spawn = false, saved = false;
    Luminumbra::Client::SaveSessionOnExit(&session, [&] { spawn = true; }, [&] { saved = true; });
    EXPECT_TRUE(spawn);
    EXPECT_TRUE(saved);
}
