#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_client/app/RenderConfiguration.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/world/GameSession.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
        const auto expected = WorldSaveService::region_simulation_digest(
            snapshot.root() / "worlds/saves" / id, kDistantRegion, {});
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
} // namespace
