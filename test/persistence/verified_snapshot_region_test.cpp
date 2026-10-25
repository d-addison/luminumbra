#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_client/app/RenderConfiguration.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

namespace {
namespace fs = std::filesystem;
namespace Measurement = Luminumbra::Client::Measurement;
using Luminumbra::Persistence::WorldSaveService;
using Luminumbra::world::GameSession;
constexpr Luminumbra::world::RegionKey kDistantRegion{40, -40};

class VerifiedSnapshotRegion : public testing::Test {
protected:
    fs::path root;
    std::ostringstream logs;
    std::shared_ptr<spdlog::logger> previous_logger;

    void SetUp() override {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("verified-region-");
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                      root / "worlds/atlas/presets/default.json");
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
        previous_logger = Log::GetCoreLogger();
        Log::GetCoreLogger() = std::make_shared<spdlog::logger>(
            "verified-region", std::make_shared<spdlog::sinks::ostream_sink_mt>(logs));
    }

    void TearDown() override {
        Log::GetCoreLogger() = previous_logger;
        std::error_code error;
        fs::remove_all(root, error);
    }

    static void FreezeDistantRegion(GameSession& session) {
        session.PinActiveRegion(kDistantRegion, true);
        session.GetSimulationEventBus().subscribe([&session](const auto& event) {
            session.RecordActiveRegionWork({kDistantRegion, 5, 0, 0, 0});
            (void)event;
        });
        for (std::uint64_t tick = 1; tick <= 3; ++tick) {
            session.GetSimulationEventBus().publish(tick, "region-pressure", "");
            ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
            ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        }
        const auto& record = session.GetActiveRegionLedger().records().at(kDistantRegion);
        ASSERT_EQ(record.state, Luminumbra::world::RegionState::Frozen);
    }
};

TEST_F(VerifiedSnapshotRegion, LivePartialRegionCannotChangeFreezeAfterFailedSave) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    // Missing, empty and truncated live authority must all leave the verified digest unchanged.
    for (const char* replacement : std::array<const char*, 3>{nullptr, "", "LREG"}) {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.generic_string() + "/");
        session.SetActiveRegionsEnabled(true);
        session.SetRegionSchedulerConfig({1, 1, 1});
        ASSERT_TRUE(session.CreateWorld("Verified region", "1337", "default"));
        const auto directory = session.GetWorldSaveDir();
        const auto id = session.GetMetadata().worldId;
        const Measurement::VerifiedSaveSnapshot snapshot(
            directory, Measurement::saved_world_digest(directory), id);
        ASSERT_TRUE(session.LoadWorldFrom(id, snapshot.root()));
        GameSession control;
        control.SetJobSystem(&jobs);
        control.SetRootPath(root.generic_string() + "/");
        control.SetActiveRegionsEnabled(true);
        control.SetRegionSchedulerConfig({1, 1, 1});
        ASSERT_TRUE(control.LoadWorldFrom(id, snapshot.root()));
        ASSERT_NO_FATAL_FAILURE(FreezeDistantRegion(control));
        // A freeze includes the field evolution of its three ticks as well as chunks.
        const auto expected =
            control.GetActiveRegionLedger().records().at(kDistantRegion).frozen_digest;
        auto field_bytes = control.GetFieldPages().region_bytes(kDistantRegion);
        if (const auto* energy = control.GetEnergyFieldState())
            field_bytes += energy->RegionBytes(kDistantRegion);
        std::uint64_t energy_age = 0;
        if (control.GetActiveRegionLedger().energy_age(kDistantRegion, energy_age)) {
            field_bytes += "|energy_age:";
            for (int byte = 0; byte < 8; ++byte)
                field_bytes.push_back(static_cast<char>((energy_age >> (8 * byte)) & 255));
        }
        EXPECT_EQ(
            expected,
            WorldSaveService::region_simulation_digest(
                snapshot.root() / "worlds/saves" / id,
                kDistantRegion,
                Luminumbra::Persistence::RegionDigestInputs{.live = {},
                                                            .parked = {},
                                                            .durable = {},
                                                            .ground_object_bytes = {},
                                                            .field_page_bytes = field_bytes}));
        const auto live_region =
            WorldSaveService::region_file_path(directory, kDistantRegion.x, kDistantRegion.z);
        if (replacement) {
            fs::create_directories(live_region.parent_path());
            std::ofstream output(live_region, std::ios::binary);
            output << replacement;
            ASSERT_TRUE(output.good());
        }
        // A failed canonical save must not switch the durable-read directory either.
        const auto metadata = directory / "world_info.json";
        ASSERT_TRUE(fs::remove(metadata));
        ASSERT_TRUE(fs::create_directory(metadata));
        EXPECT_FALSE(session.SaveWorld());
        ASSERT_NO_FATAL_FAILURE(FreezeDistantRegion(session));
        EXPECT_EQ(session.GetActiveRegionLedger().records().at(kDistantRegion).frozen_digest,
                  expected);
    }
    jobs.shutdown();
}

TEST_F(VerifiedSnapshotRegion, SuccessfulCanonicalSaveReplacesSnapshotAuthority) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetJobSystem(&jobs);
    session.SetRootPath(root.generic_string() + "/");
    session.SetActiveRegionsEnabled(true);
    session.SetRegionSchedulerConfig({1, 1, 1});
    ASSERT_TRUE(session.CreateWorld("Canonical region", "1337", "default"));
    {
        const auto id = session.GetMetadata().worldId;
        const Measurement::VerifiedSaveSnapshot snapshot(
            session.GetWorldSaveDir(),
            Measurement::saved_world_digest(session.GetWorldSaveDir()),
            id);
        ASSERT_TRUE(session.LoadWorldFrom(id, snapshot.root()));
        ASSERT_TRUE(session.SaveWorld());
    }
    // The snapshot is gone: a freeze must now use the complete canonical save.
    ASSERT_NO_FATAL_FAILURE(FreezeDistantRegion(session));
    jobs.shutdown();
}

TEST_F(VerifiedSnapshotRegion, SnapshotRestoresRegionalQueriesAndDiskOnlyAuthority) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded, canonical;
    for (auto* session : {&owner, &loaded, &canonical}) {
        session->SetJobSystem(&jobs);
        session->SetRootPath(root.generic_string() + "/");
        session->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(owner.CreateWorld("Regional snapshot", "1337", "default"));
    for (int tick = 0; tick < 4; ++tick)
        ASSERT_EQ(owner.TickSimulation(owner.GetSimulationClock().fixed_dt()), 1u);
    const Luminumbra::IVec3 distant(1280, 0, -1280);
    auto chunk = std::make_shared<Luminumbra::Chunk>(distant);
    chunk->sdf_data.assign(17 * 17 * 17, -4.0f);
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    const auto initial =
        owner.GetWorldSystem()->GetInitialChunkLoadList(owner.GetMetadata().spawnPoint);
    ASSERT_FALSE(initial.empty());
    auto near = std::make_shared<Luminumbra::Chunk>(initial.front());
    near->sdf_data.assign(17 * 17 * 17, -3.0f);
    near->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(near));
    owner.GetWorldSystem()->SetWaterSimWindowCursor(19);
    ASSERT_TRUE(owner.SaveWorld());
    const auto id = owner.GetMetadata().worldId;
    const Measurement::VerifiedSaveSnapshot snapshot(
        owner.GetWorldSaveDir(), Measurement::saved_world_digest(owner.GetWorldSaveDir()), id);
    ASSERT_TRUE(loaded.LoadWorldFrom(id, snapshot.root())) << loaded.GetWorldOpenError();
    ASSERT_TRUE(canonical.LoadWorld(id)) << canonical.GetWorldOpenError();
    EXPECT_EQ(loaded.GetWorldClock().canonical_bytes(),
              canonical.GetWorldClock().canonical_bytes());
    EXPECT_EQ(loaded.GetWorldSystem()->GetWaterSimWindowCursor(), 19u);
    const auto restored_near = loaded.GetWorldSystem()->find_streamed_chunk(initial.front());
    ASSERT_NE(restored_near, nullptr);
    EXPECT_EQ(restored_near->sdf_data, near->sdf_data);
    EXPECT_FALSE(loaded.GetWorldSystem()->find_streamed_chunk(distant));
    EXPECT_EQ(loaded.GetWorldSystem()->durable_chunk_inventory(),
              canonical.GetWorldSystem()->durable_chunk_inventory());
    // Both query consumers and their hash projections must use the restored field pages.
    for (int tick = 0; tick < 2; ++tick) {
        EXPECT_EQ(loaded.GetFieldPages().canonical_bytes(),
                  canonical.GetFieldPages().canonical_bytes());
        EXPECT_EQ(loaded.GetWindFieldSystem()->ComputeWindSubHash(),
                  canonical.GetWindFieldSystem()->ComputeWindSubHash());
        EXPECT_EQ(loaded.GetWeatherSystem()->ComputeWeatherSubHash(),
                  canonical.GetWeatherSystem()->ComputeWeatherSubHash());
        EXPECT_EQ(loaded.GetAetherFieldSystem()->ComputeAetherSubHash(),
                  canonical.GetAetherFieldSystem()->ComputeAetherSubHash());
        for (const auto position : {owner.GetMetadata().spawnPoint, Luminumbra::Vec3{520, 5, -8}}) {
            EXPECT_EQ(loaded.GetWindFieldSystem()->SampleWind(position),
                      canonical.GetWindFieldSystem()->SampleWind(position));
            EXPECT_EQ(loaded.GetWeatherSystem()->CategoryAt(position),
                      canonical.GetWeatherSystem()->CategoryAt(position));
            EXPECT_EQ(loaded.GetAetherFieldSystem()->SampleAether(position),
                      canonical.GetAetherFieldSystem()->SampleAether(position));
        }
        ASSERT_EQ(loaded.TickSimulation(loaded.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_EQ(canonical.TickSimulation(canonical.GetSimulationClock().fixed_dt()), 1u);
    }
}

TEST_F(VerifiedSnapshotRegion, PendingSnapshotRefusesBeforeParsingAndPreservesLiveWorld) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetJobSystem(&jobs);
    session.SetRootPath(root.generic_string() + "/");
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Pending snapshot", "1337", "default"));
    const auto id = session.GetMetadata().worldId;
    const auto live_world = session.GetWorldSystem();
    const auto live_fields = session.GetFieldPages().canonical_bytes();
    const auto live_ledger = session.GetActiveRegionLedger().canonical_bytes();
    for (const char* metadata : std::array<const char*, 4>{nullptr, "", "{", "{}"}) {
        const Measurement::VerifiedSaveSnapshot snapshot(
            session.GetWorldSaveDir(),
            Measurement::saved_world_digest(session.GetWorldSaveDir()),
            id);
        const auto directory = snapshot.root() / "worlds/saves" / id;
        const auto path = directory / "world_info.json";
        if (metadata) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << metadata;
            ASSERT_TRUE(output.good());
        } else {
            ASSERT_TRUE(fs::remove(path));
        }
        fs::create_directories(Luminumbra::Persistence::journal_directory(directory));
        {
            std::ofstream output(Luminumbra::Persistence::journal_directory(directory) /
                                 "txn.json");
            output << "{";
            ASSERT_TRUE(output.good());
        }
        const auto contents = [&directory] {
            std::map<fs::path, std::string> files;
            for (const auto& entry : fs::recursive_directory_iterator(directory)) {
                if (entry.is_regular_file()) {
                    std::ifstream input(entry.path(), std::ios::binary);
                    files.emplace(entry.path().lexically_relative(directory),
                                  std::string(std::istreambuf_iterator<char>{input}, {}));
                }
            }
            return files;
        };
        const auto before = contents();
        bool loaded = true;
        EXPECT_NO_THROW(loaded = session.LoadWorldFrom(id, snapshot.root()));
        EXPECT_FALSE(loaded);
        EXPECT_NE(session.GetWorldOpenError().find("pending recovery"), std::string::npos);
        EXPECT_EQ(contents(), before);
        EXPECT_EQ(session.GetWorldSystem(), live_world);
        EXPECT_EQ(session.GetFieldPages().canonical_bytes(), live_fields);
        EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), live_ledger);
        EXPECT_TRUE(session.SaveWorld());
    }
}
} // namespace
