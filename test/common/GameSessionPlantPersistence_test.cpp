#include "../support/GameSessionTestWorld.h"
#include "../support/PersistenceFileOps.h"
#include "../support/SystemConfigVariants.h"

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/WorldSaveService.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

using Luminumbra::JobSystem;
using Luminumbra::TestSupport::AdoptRegionGoldenChunk;
using Luminumbra::TestSupport::ClockTestPlant;
using Luminumbra::TestSupport::ExpectSameWorldHash;
using Luminumbra::TestSupport::HeadlessRoot;
using Luminumbra::TestSupport::ReadClockTestFile;
using Luminumbra::world::GameSession;
namespace P = Luminumbra::Persistence;

TEST(GameSessionHeadlessWorldTest, PlantDeletionFailureFailsLegacySaveWithoutDirtyChunks) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    {
        GameSession session;
        session.SetRootPath(root.root_string());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(false);
        session.SetAetherStateEnabled(false);
        ASSERT_TRUE(session.CreateWorld("Plant deletion failure", "1337", "default"));
        const auto save = session.GetWorldSaveDir();
        const auto path = Luminumbra::Persistence::WorldSaveService::plant_entities_path(save);
        Luminumbra::Ecs::EntityRegistrySnapshot plants;
        plants.entities.push_back(Luminumbra::Ecs::EntitySnapshotRecord{
            .entity_id = 1, .name = "stale plant", .components = {}});
        ASSERT_TRUE(Luminumbra::Persistence::WorldSaveService::save_plant_entities(plants, save));
        for (const auto& chunk : session.GetWorldSystem()->snapshot_streamed_chunks())
            ASSERT_FALSE(chunk->is_voxel_data_dirty());
        Luminumbra::Persistence::detail::set_file_publication_hook_for_testing(
            [](const fs::path& destination, bool removing) {
                return !(removing && destination.filename() == "plant-entities.json");
            });
        Luminumbra::world::WorldStateSaveReport report;
        EXPECT_FALSE(session.SaveWorldStateTo(save, &report));
        EXPECT_EQ(report.chunks_dirty, 0u);
        EXPECT_FALSE(report.saved);
        Luminumbra::Ecs::EntityRegistrySnapshot retained;
        EXPECT_TRUE(Luminumbra::Persistence::WorldSaveService::load_plant_entities(retained, save));
        EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(retained),
                  Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(plants));
        Luminumbra::Persistence::detail::set_file_publication_hook_for_testing(nullptr);
        EXPECT_TRUE(session.SaveWorldStateTo(save));
        EXPECT_FALSE(fs::exists(path));
    }
    jobs.shutdown();
}

TEST(GameSessionHeadlessWorldTest, DisabledRegionsMatchDevelWorldHashAndExactSaveBytes) {
    const HeadlessRoot root;
    {
        const auto path = root.path() / "worlds/atlas/presets/default.json";
        std::ifstream input(path);
        auto preset = nlohmann::json::parse(input);
        input.close();
        preset["generation_params"]["terrain"]["island_mask_enabled"] = false;
        std::ofstream output(path);
        output << preset.dump(2);
    }
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, expected;
    for (auto* s : {&session, &expected}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(false);
        s->SetAetherStateEnabled(false);
        s->SetWaterHighResEnabled(false);
        s->SetWeatherRainEnabled(false);
        s->SetWeatherEventsEnabled(false);
        s->SetFiniteHydrologyEnabled(false);

        ASSERT_TRUE(s->CreateWorld("Region golden", "1337", "default"));
        ASSERT_TRUE(s->LoadWorldStateFrom(root.path() / "unmarked-snapshot"));
        s->GetWorldSystem()->SetWaterHydrology(false, 0, 0);
        ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(*s, 0));
    }
    // Disabled entry points must be inert, including otherwise invalid inputs.
    session.SetLocalPlayerSimulationPosition(Luminumbra::Vec3(20000, 0, 20000), true);
    session.PinActiveRegion({123, 45}, true);
    session.NotifyGroundObjectEdit(Luminumbra::Vec3(5000, 0, 5000));
    session.RecordActiveRegionWork({{123, 45}, UINT64_MAX, 0, 0, 0});
    for (int i = 0; i < 7; ++i) {
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_EQ(expected.TickSimulation(expected.GetSimulationClock().fixed_dt()), 1u);
    }
    const auto save = root.path() / "golden-save";
    ASSERT_TRUE(session.SaveWorldStateTo(save));
    EXPECT_FALSE(fs::exists(save / "preset.json"));
    EXPECT_FALSE(fs::exists(save / "world_info.json"));
    ExpectSameWorldHash(session, expected);
    EXPECT_TRUE(session.GetActiveRegionLedger().records().empty());
    const auto fixture =
        fs::path(LUMINUMBRA_SOURCE_ROOT) / "test/fixtures/active-regions/devel-off";
    std::size_t files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(save)) {
        if (!entry.is_regular_file())
            continue;
        ++files;
        EXPECT_EQ(ReadClockTestFile(entry.path()),
                  ReadClockTestFile(fixture / entry.path().filename()));
    }
    EXPECT_EQ(files, 2u);
    EXPECT_FALSE(fs::exists(P::WorldSaveService::active_regions_path(save)));
}

void SavePlantRosterThenMultiRegionEdit(GameSession& session, const fs::path& save) {
    ClockTestPlant(session.GetRegistry(), Luminumbra::Vec3(8, 20, 8));
    ASSERT_TRUE(session.SaveWorldStateTo(save));
    ASSERT_TRUE(P::WorldSaveService::has_world_save(save));
    ASSERT_TRUE(fs::exists(P::WorldSaveService::plant_entities_path(save)));
    ASSERT_FALSE(fs::exists(P::WorldSaveService::region_file_path(save, 0, 0)));
    for (const int x : {0, 32}) {
        ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(session, x));
    }
    ASSERT_TRUE(session.SaveWorldStateTo(save));
}

TEST(GameSessionHeadlessWorldTest,
     DisabledPlantRosterThenMultiRegionEditMatchesDevelHashAndExactSaveBytes) {
    const HeadlessRoot root;
    {
        const auto path = root.path() / "worlds/atlas/presets/default.json";
        std::ifstream input(path);
        auto preset = nlohmann::json::parse(input);
        input.close();
        preset["generation_params"]["terrain"]["island_mask_enabled"] = false;
        std::ofstream output(path);
        output << preset.dump(2);
    }
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded, expected_live, expected_loaded;
    for (auto* s : {&session, &loaded, &expected_live, &expected_loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(false);
        s->SetAetherStateEnabled(false);
        s->SetWaterHighResEnabled(false);
        s->SetWeatherRainEnabled(false);
        s->SetWeatherEventsEnabled(false);
        s->SetFiniteHydrologyEnabled(false);

        ASSERT_TRUE(s->CreateTransientWorld("Plant region golden", "1337", "default"));
        ASSERT_TRUE(s->LoadWorldStateFrom(root.path() / "unmarked-snapshot"));
    }
    for (auto* s : {&expected_live, &expected_loaded}) {
        ClockTestPlant(s->GetRegistry(), Luminumbra::Vec3(8, 20, 8));
        ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(*s, 0));
    }
    // Only the live session retains the clean, unsaved second region.
    ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(expected_live, 32));
    const auto save = root.path() / "plant-golden-save";
    ASSERT_NO_FATAL_FAILURE(SavePlantRosterThenMultiRegionEdit(session, save));
    EXPECT_FALSE(fs::exists(save / "preset.json"));
    EXPECT_FALSE(fs::exists(save / "world_info.json"));
    EXPECT_FALSE(session.ActiveRegionsEnabled());
    EXPECT_FALSE(fs::exists(P::WorldSaveService::region_file_path(save, 1, 0)));
    const auto fixture =
        fs::path(LUMINUMBRA_SOURCE_ROOT) / "test/fixtures/active-regions/devel-off-plant-edit";
    ExpectSameWorldHash(session, expected_live);
    ASSERT_TRUE(loaded.LoadWorldStateFrom(save));
    EXPECT_EQ(loaded.GetWorldSystem()->snapshot_streamed_chunks().size(), 1u);
    ExpectSameWorldHash(loaded, expected_loaded);
    std::size_t files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(save)) {
        if (!entry.is_regular_file())
            continue;
        ++files;
        EXPECT_EQ(ReadClockTestFile(entry.path()),
                  ReadClockTestFile(fixture / entry.path().filename()));
    }
    EXPECT_EQ(files, 3u); // plant roster, dirty region and manifest; no clean region
}

TEST(GameSessionHeadlessWorldTest, EffectiveIdentityUsesDefaultsAndCurrentSetters) {
    using luminumbra::core::SystemConfig;
    GameSession direct, applied;
    applied.ApplySystemConfig(SystemConfig::Defaults());
    const auto baseline = direct.SystemConfigIdentity();
    EXPECT_FALSE(baseline.empty());
    EXPECT_EQ(baseline, applied.SystemConfigIdentity());
    EXPECT_EQ(baseline, "bf3da613768a50cb");
    EXPECT_EQ(direct.LegacyRevision3SystemConfigIdentity(), "60ab3c242d54ce89");
    for (const auto& [name, identity] : Luminumbra::TestSupport::EffectiveIdentityMismatches()) {
        SCOPED_TRACE(name);
        EXPECT_NE(identity, baseline);
    }
    // Turning off a tuning override with no nondefault tuning changes no effective rules.
    applied.ApplySystemConfig(SystemConfig::FromJsonString(
        R"({"sim":{"ecology":{"enabled":false,"params":{"herd_weight":0.9}}},
            "render":{"circadian":{"enabled":false,"params":{"amplitude":0.5}}}})"));
    EXPECT_EQ(applied.SystemConfigIdentity(), baseline);
    applied.ApplySystemConfig(SystemConfig::FromJsonString(
        R"({"sim":{"reproduction":{"params":{"maturity_ticks":90.75}}}})"));
    EXPECT_EQ(applied.SystemConfigIdentity(), baseline);
    const auto variants = Luminumbra::TestSupport::EffectiveIdentityMismatches();
    ASSERT_GE(variants.size(), 2u);
    EXPECT_NE(variants[variants.size() - 2].second, variants.back().second);
    applied.SetEcologyTuning(direct.GetEcologyTuning());
    direct.SetCircadianAmplitude(0.5f);
    applied.SetCircadianAmplitude(0.5f);
    EXPECT_EQ(direct.SystemConfigIdentity(), applied.SystemConfigIdentity());
}

} // namespace
