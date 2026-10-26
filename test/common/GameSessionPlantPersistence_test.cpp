// asset-manifest split. The engine-side GameSession validates
// SIMULATION requirements only (preset and worldgen dependencies); renderer/UI
// asset requirements are caller-supplied by the client. Worldgen dependencies
// are simulation assets and must be available even for a headless host.
#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"

#include "../support/SystemConfigVariants.h"
#include "entt/entity/fwd.hpp"
#include "luminumbra/core/Types.h"
#include "luminumbra_common/components/CircadianComponents.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/MigratoryComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/core/DeterministicMath.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/WorldClock.h"
#include "luminumbra_common/world/WorldgenDependencySnapshot.h"
#include "luminumbra_common/world/WorldgenFileReader.h"
#include "nlohmann/json_fwd.hpp"
#include <gtest/gtest.h>
#include <ios>
#include <memory>
#include <optional>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "luminumbra_client/app/FarmSpeciesCache.h"
#include "luminumbra_client/app/SessionExit.h"
#include "luminumbra_client/rendering/TimeOfDayModel.h"
#include "luminumbra_common/ai/CircadianSystem.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h"
#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/ai/MigrationSystem.h"
#include "luminumbra_common/ai/StimulusChannels.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/foliage/SpeciesRegistry.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/replay/RegionScheduleTrace.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/PlantGrowthSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"

#include <nlohmann/json.hpp>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/scripting/LuaState.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarAuthorityOverlay.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "luminumbra_common/world/FarWantedSet.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include "luminumbra_common/world/WorldStreamingState.h"

namespace fs = std::filesystem;
namespace {
using Luminumbra::JobSystem;
using Luminumbra::world::GameSession;
namespace P = Luminumbra::Persistence;
namespace C = Luminumbra::Components;
namespace F = luminumbra::foliage;
#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

fs::path SourcePresetPath() {
    return fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds" / "atlas" / "presets" / "default.json";
}

// Temp root containing the preset and worldgen data, with no client assets.
// The root is unique per fixture instance so
// concurrent common_tests processes never share (or clobber) a directory; each
// TEST builds ONE instance and reuses it, so within-run path stability still
// holds. The destructor removes the tree.
class HeadlessRoot {
public:
    HeadlessRoot() {
        root_ =
            Luminumbra::TestSupport::CreateUniqueTempDirectory("luminumbra_headless_world_test_");
        fs::create_directories(root_ / "worlds" / "atlas" / "presets");
        fs::copy_file(SourcePresetPath(), root_ / "worlds" / "atlas" / "presets" / "default.json");
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root_);
    }
    ~HeadlessRoot() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    [[nodiscard]] const fs::path& path() const {
        return root_;
    }
    // GameSession::SetRootPath expects a trailing separator (paths are
    // concatenated, not joined).
    [[nodiscard]] std::string root_string() const {
        return root_.string() + static_cast<char>(fs::path::preferred_separator);
    }

private:
    fs::path root_;
};

std::string ReadClockTestFile(const fs::path& path) {
    // Consuming rdbuf() mutates the buffer; keep its owning stream mutable.
    std::ifstream input;
    input.open(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

void WriteSavedPresetTestJson(const fs::path& path, const nlohmann::json& json) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << json.dump(2) << '\n';
    ASSERT_TRUE(output.good());
}

std::map<std::string, std::string> SavedPresetTestDiskBytes(const fs::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        result.emplace(entry.path().lexically_relative(root).generic_string(),
                       entry.is_directory() ? "<directory>" : ReadClockTestFile(entry.path()));
    }
    return result;
}

class PendingSnapshotState;

class SavedWorldPresetTest : public testing::TestWithParam<bool> {
    HeadlessRoot root;
    JobSystem jobs;

protected:
    const HeadlessRoot& Root() const {
        return root;
    }

    void SetUp() override {
        jobs.startup(1);
    }
    void TearDown() override {
        jobs.shutdown();
    }
    void Configure(GameSession& session) {
        session.SetRootPath(root.root_string());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(GetParam());
    }
    void CreateSnapshotSession(GameSession& session) {
        SCOPED_TRACE("create snapshot session");
        Configure(session);
        session.SetAetherStateEnabled(true);
        ASSERT_TRUE(session.CreateWorld("Snapshot preset identity", "1337", "default"));
    }
    void ExpectSnapshotReloaded(const PendingSnapshotState& pending,
                                const Luminumbra::Chunk& chunk,
                                const fs::path& save,
                                const std::string& world_id);
    void ExpectRefusedWithoutWrites(const std::string& id, const std::string& reason) {
        const auto before = SavedPresetTestDiskBytes(root.path());
        const auto inspected = P::InspectSavedWorld(root.path(), id);
        EXPECT_NE(inspected.error.find(reason), std::string::npos) << inspected.error;
        GameSession loaded;
        Configure(loaded);
        EXPECT_FALSE(loaded.LoadWorld(id));
        EXPECT_NE(loaded.GetWorldOpenError().find(reason), std::string::npos);
        EXPECT_EQ(loaded.GetWorldSystem(), nullptr);
        EXPECT_FALSE(loaded.SaveWorld());
        EXPECT_FALSE(loaded.SaveWorldState());
        EXPECT_EQ(SavedPresetTestDiskBytes(root.path()), before);
    }
};

TEST_P(SavedWorldPresetTest, NewSaveRoundTripsWithMarkerAndCanonicalIdentity) {
    GameSession created;
    Configure(created);
    ASSERT_TRUE(created.CreateWorld("Preset identity", "1337", "default"));
    const auto save = created.GetWorldSaveDir();
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    const auto preset = nlohmann::json::parse(ReadClockTestFile(save / "preset.json"));
    EXPECT_EQ(metadata.at("presetFormat"), 1);
    EXPECT_EQ(metadata.at("presetIdentity"), P::StableChecksum(preset.dump()));
    EXPECT_TRUE(preset.at("generation_params").at("terrain").at("island_mask_enabled").get<bool>());
    // Formatting does not change the canonical identity, and no packaged copy is needed.
    ASSERT_TRUE(P::WorldSaveService::write_file_durably(save / "preset.json", preset.dump(4)));
    ASSERT_TRUE(fs::remove(Root().path() / "worlds/atlas/presets/default.json"));
    const auto before = SavedPresetTestDiskBytes(Root().path());
    const auto inspected = P::InspectSavedWorld(Root().path(), created.GetMetadata().worldId);
    ASSERT_TRUE(inspected.error.empty()) << inspected.error;
    EXPECT_EQ(inspected.preset_path, save / "preset.json");
    GameSession loaded;
    Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(created.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.PresetContentHash(), created.PresetContentHash());
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), before);
    ASSERT_TRUE(loaded.SaveWorld());
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(save / "world_info.json")), metadata);
}

TEST_P(SavedWorldPresetTest, CustomAdditionalNamedPresetCarriesItsExactIdentity) {
    const auto named = Root().path() / "worlds/atlas/presets/other.json";
    fs::copy_file(SourcePresetPath(), named);
    auto custom = nlohmann::json::parse(ReadClockTestFile(named));
    custom["generation_params"]["terrain"]["height_offset"] = 123.0;
    const auto text = custom.dump();
    GameSession created;
    Configure(created);
    ASSERT_TRUE(created.CreateWorld("Custom identity", "1337", "other", &text));
    const auto save = created.GetWorldSaveDir();
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    const auto embedded = nlohmann::json::parse(ReadClockTestFile(save / "preset.json"));
    EXPECT_EQ(metadata.at("presetFormat"), 1);
    EXPECT_EQ(metadata.at("presetIdentity"), P::StableChecksum(embedded.dump()));
    GameSession loaded;
    Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(created.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_FLOAT_EQ(loaded.GetWorldSystem()->get_params().height_offset, 123.0f);

    auto tampered = embedded;
    tampered["generation_params"]["terrain"]["height_offset"] = 124.0;
    WriteSavedPresetTestJson(save / "preset.json", tampered);
    ExpectRefusedWithoutWrites(created.GetMetadata().worldId, "identity");
}

class SnapshotPresetFiles {
public:
    explicit SnapshotPresetFiles(const fs::path& destination)
        : directory(destination)
        , preset_bytes(ReadClockTestFile(directory / "preset.json"))
        , metadata_bytes(ReadClockTestFile(directory / "world_info.json"))
        , other_preset(nlohmann::json::parse(preset_bytes))
        , other_metadata(nlohmann::json::parse(metadata_bytes)) {
        other_preset["generation_params"]["terrain"]["height_offset"] = 123.0;
        other_metadata["presetIdentity"] = P::StableChecksum(other_preset.dump());
    }

    void Rewrite(const char* changed) const {
        SCOPED_TRACE(directory.generic_string());
        if (std::string(changed) != "identity") {
            ASSERT_NO_FATAL_FAILURE(
                WriteSavedPresetTestJson(directory / "preset.json", other_preset));
        }
        if (std::string(changed) != "preset") {
            ASSERT_NO_FATAL_FAILURE(
                WriteSavedPresetTestJson(directory / "world_info.json", other_metadata));
        }
    }

    void Restore() const {
        SCOPED_TRACE(directory.generic_string());
        ASSERT_TRUE(
            P::WorldSaveService::write_file_durably(directory / "preset.json", preset_bytes));
        ASSERT_TRUE(
            P::WorldSaveService::write_file_durably(directory / "world_info.json", metadata_bytes));
    }

private:
    fs::path directory;
    std::string preset_bytes;
    std::string metadata_bytes;
    nlohmann::json other_preset;
    nlohmann::json other_metadata;
};

class SnapshotDestinationBytes {
public:
    explicit SnapshotDestinationBytes(const fs::path& destination)
        : directory(destination)
        , bytes(SavedPresetTestDiskBytes(directory)) {}

    void ExpectUnchanged() const {
        SCOPED_TRACE(directory.generic_string());
        EXPECT_EQ(SavedPresetTestDiskBytes(directory), bytes);
    }

private:
    fs::path directory;
    std::map<std::string, std::string> bytes;
};

void PreparePendingSnapshot(GameSession& session,
                            std::shared_ptr<Luminumbra::Chunk>& chunk,
                            bool existing_snapshot) {
    SCOPED_TRACE("prepare pending snapshot");
    chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->heightmap_data = {8.0f, 9.0f, 10.0f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    auto& registry = session.GetRegistry();
    const auto plant = registry.create();
    registry.emplace<C::PlantTag>(plant);
    registry.emplace<C::PlantGenomeComponent>(plant).genes.fill(0.5f);
    registry.emplace<C::PlantGrowthComponent>(plant);
    registry.emplace<C::TransformComponent>(plant).position = session.GetMetadata().spawnPoint;
    auto* energy = session.GetEnergyFieldState();
    ASSERT_NE(energy, nullptr);
    energy->SetAnchorCell(0, 0);
    energy->QueueDeposit(1, 0, 0, 0, 60000);
    energy->Tick(0);
    if (existing_snapshot) {
        ASSERT_TRUE(session.SaveWorldState());
        ASSERT_FALSE(chunk->is_voxel_data_dirty());
    }

    chunk->heightmap_data.front() = 42.0f;
    chunk->mark_voxel_data_dirty();
    registry.get<C::PlantGrowthComponent>(plant).growth_points = 123;
    energy->QueueDeposit(2, 0, 0, 0, 1000);
    energy->Tick(0);
}

class PendingSnapshotState {
public:
    explicit PendingSnapshotState(const GameSession& session)
        : plant_hash(session.ComputePlantSubHash())
        , energy_bytes(session.GetEnergyFieldState()->CanonicalBytes()) {}

    void ExpectUnchanged(const GameSession& session, const Luminumbra::Chunk& chunk) const {
        SCOPED_TRACE("pending snapshot state");
        EXPECT_TRUE(chunk.is_voxel_data_dirty());
        EXPECT_FLOAT_EQ(chunk.heightmap_data.front(), 42.0f);
        EXPECT_EQ(session.ComputePlantSubHash(), plant_hash);
        EXPECT_EQ(session.GetEnergyFieldState()->CanonicalBytes(), energy_bytes);
    }

    const std::string& PlantHash() const {
        return plant_hash;
    }
    const std::string& EnergyBytes() const {
        return energy_bytes;
    }

private:
    std::string plant_hash;
    std::string energy_bytes;
};

void SavedWorldPresetTest::ExpectSnapshotReloaded(const PendingSnapshotState& pending,
                                                  const Luminumbra::Chunk& chunk,
                                                  const fs::path& save,
                                                  const std::string& world_id) {
    SCOPED_TRACE(world_id);
    EXPECT_NE(ReadClockTestFile(P::WorldSaveService::plant_entities_path(save)), "");
    EXPECT_NE(ReadClockTestFile(save / "aether_state.efs"), "");
    GameSession loaded;
    Configure(loaded);
    loaded.SetAetherStateEnabled(true);
    ASSERT_TRUE(loaded.LoadWorld(world_id)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.ComputePlantSubHash(), pending.PlantHash());
    ASSERT_NE(loaded.GetEnergyFieldState(), nullptr);
    EXPECT_EQ(loaded.GetEnergyFieldState()->CanonicalBytes(), pending.EnergyBytes());
    Luminumbra::WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(P::WorldSaveService{}.load_world(restored, save, errors));
    const auto restored_chunk = restored.find_chunk(chunk.get_id());
    ASSERT_NE(restored_chunk, nullptr);
    EXPECT_EQ(restored_chunk->heightmap_data, chunk.heightmap_data);
}

TEST_P(SavedWorldPresetTest, OpenSessionPresetChangeRefusesBeforeSnapshotWritesAndAcknowledgement) {
    for (const bool existing_snapshot : {false, true}) {
        SCOPED_TRACE(existing_snapshot);
        GameSession session;
        CreateSnapshotSession(session);
        if (HasFatalFailure())
            return;
        const auto save = session.GetWorldSaveDir();
        const SnapshotPresetFiles preset_files(save);

        std::shared_ptr<Luminumbra::Chunk> chunk;
        PreparePendingSnapshot(session, chunk, existing_snapshot);
        if (HasFatalFailure())
            return;
        const PendingSnapshotState pending(session);

        for (const auto* changed : {"preset", "identity", "both"}) {
            SCOPED_TRACE(changed);
            preset_files.Rewrite(changed);
            if (HasFatalFailure())
                return;
            const SnapshotDestinationBytes before(save);
            EXPECT_FALSE(session.SaveWorldStateTo(save));
            before.ExpectUnchanged();
            pending.ExpectUnchanged(session, *chunk);
            EXPECT_FALSE(session.SaveWorldState());
            before.ExpectUnchanged();
            pending.ExpectUnchanged(session, *chunk);
            EXPECT_FALSE(session.SaveWorld());
            before.ExpectUnchanged();
            pending.ExpectUnchanged(session, *chunk);
            preset_files.Restore();
            if (HasFatalFailure())
                return;
        }

        ASSERT_TRUE(session.SaveWorldState());
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        ExpectSnapshotReloaded(pending, *chunk, save, session.GetMetadata().worldId);
        if (HasFatalFailure())
            return;
    }
}

TEST_P(SavedWorldPresetTest, MissingEmbeddedPresetRefusesWithoutWrites) {
    GameSession created;
    Configure(created);
    ASSERT_TRUE(created.CreateWorld("Missing preset", "1337", "default"));
    fs::create_directories(Root().path() / "worlds/atlas/presets-rev6");
    fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets-rev6/default.json",
                  Root().path() / "worlds/atlas/presets-rev6/default.json");
    ASSERT_TRUE(fs::remove(created.GetWorldSaveDir() / "preset.json"));
    ExpectRefusedWithoutWrites(created.GetMetadata().worldId, "required embedded preset.json");
}

TEST_P(SavedWorldPresetTest, MissingOrInvalidIdentityRefusesWithoutWrites) {
    GameSession created;
    Configure(created);
    ASSERT_TRUE(created.CreateWorld("Missing identity", "1337", "default"));
    const auto path = created.GetWorldSaveDir() / "world_info.json";
    auto metadata = nlohmann::json::parse(ReadClockTestFile(path));
    metadata.erase("presetIdentity");
    WriteSavedPresetTestJson(path, metadata);
    ExpectRefusedWithoutWrites(created.GetMetadata().worldId, "identity");
    for (const auto& identity : {nlohmann::json(nullptr),
                                 nlohmann::json(123),
                                 nlohmann::json(""),
                                 nlohmann::json("wrong")}) {
        SCOPED_TRACE(identity.dump());
        metadata["presetIdentity"] = identity;
        WriteSavedPresetTestJson(path, metadata);
        ExpectRefusedWithoutWrites(created.GetMetadata().worldId, "identity");
    }
}

TEST_P(SavedWorldPresetTest, TamperedEmbeddedPresetRefusesOnIdentityWithoutWrites) {
    GameSession created;
    Configure(created);
    ASSERT_TRUE(created.CreateWorld("Tampered preset", "1337", "default"));
    const auto path = created.GetWorldSaveDir() / "preset.json";
    auto preset = nlohmann::json::parse(ReadClockTestFile(path));
    preset["generation_params"]["terrain"]["island_mask_enabled"] = false;
    WriteSavedPresetTestJson(path, preset);
    ExpectRefusedWithoutWrites(created.GetMetadata().worldId, "identity");
}

TEST_P(SavedWorldPresetTest, InvalidFormatNeverFallsBack) {
    GameSession created;
    Configure(created);
    ASSERT_TRUE(created.CreateWorld("Invalid format", "1337", "default"));
    const auto save = created.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    ASSERT_TRUE(fs::remove(save / "preset.json"));
    for (const auto& format : {nlohmann::json(nullptr),
                               nlohmann::json(false),
                               nlohmann::json(0),
                               nlohmann::json(2),
                               nlohmann::json("1")}) {
        SCOPED_TRACE(format.dump());
        metadata["presetFormat"] = format;
        WriteSavedPresetTestJson(save / "world_info.json", metadata);
        ExpectRefusedWithoutWrites(created.GetMetadata().worldId, "preset format");
    }
}

TEST_P(SavedWorldPresetTest, UnmarkedLegacySaveUsesFrozenPresetAndEmbedsItOnResave) {
    const auto historical = Root().path() / "worlds/atlas/presets-rev6/default.json";
    fs::create_directories(historical.parent_path());
    fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets-rev6/default.json",
                  historical);
    const auto save = Root().path() / "worlds/saves/legacy";
    fs::create_directories(save);
    WriteSavedPresetTestJson(save / "world_info.json",
                             {{"container_version", 2}, {"worldType", "default"}});
    const auto before = SavedPresetTestDiskBytes(Root().path());
    const auto inspected = P::InspectSavedWorld(Root().path(), "legacy");
    ASSERT_TRUE(inspected.error.empty()) << inspected.error;
    EXPECT_EQ(inspected.preset_path, historical);
    GameSession loaded;
    Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld("legacy")) << loaded.GetWorldOpenError();
    EXPECT_FALSE(loaded.GetWorldSystem()->get_params().island_mask_enabled);
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), before);
    ASSERT_TRUE(loaded.SaveWorld());
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    EXPECT_EQ(metadata.at("presetFormat"), 1);
    EXPECT_EQ(metadata.at("presetIdentity"),
              P::StableChecksum(nlohmann::json::parse(ReadClockTestFile(historical)).dump()));
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(save / "preset.json")),
              nlohmann::json::parse(ReadClockTestFile(historical)));
    ASSERT_TRUE(fs::remove(historical));
    EXPECT_TRUE(P::InspectSavedWorld(Root().path(), "legacy").error.empty());
}

nlohmann::json SessionWorldSubHashes(GameSession& session) {
    Luminumbra::WorldStreamingState state;
    for (const auto& chunk : session.GetWorldSystem()->snapshot_streamed_chunks())
        state.insert_chunk(chunk);
    std::string aether = session.GetAetherFieldSystem()->ComputeAetherSubHash();
    const auto energy = session.ComputeAetherStateSubHash();
    if (!energy.empty())
        aether = P::StableChecksum(aether + "|state:" + energy);
    return {{"chunk",
             P::ComputeWorldStreamingStateHash(
                 state, session.GetWorldSystem()->GetWaterSimWindowCursor())},
            {"wind", session.GetWindFieldSystem()->ComputeWindSubHash()},
            {"weather", session.GetWeatherSystem()->ComputeWeatherSubHash()},
            {"aether", aether},
            {"scents", session.ComputeScentSubHash()},
            {"ecology",
             session.FoldClockIntoEcologyHash(
                 luminumbra::ai::ComputeEcologySubHash(session.GetRegistry()))},
            {"plants", session.ComputePlantSubHash()}};
}

// The base consumes no structure pool. Only the custom candidate opts in, so
// named-preset admission cannot mask the dependency refusal under test.
nlohmann::json PrepareCreationRefusalBase(const fs::path& root) {
    auto base =
        nlohmann::json::parse(ReadClockTestFile(root / "worlds/atlas/presets/default.json"));
    base["generation_params"]["features"]["structures_enabled"] = false;
    base["generation_params"]["biomes"]["table"] = "";
    WriteSavedPresetTestJson(root / "worlds/atlas/presets/default.json", base);
    fs::remove_all(root / "data/common/structures");
    return base;
}

std::array<std::string, 3> CreationRefusalInputs(const nlohmann::json& base) {
    auto semantic = base;
    semantic["generation_params"]["terrain"]["height_offset"] = "invalid height";
    auto dependency = base;
    dependency["generation_params"]["features"]["structures_enabled"] = true;
    return {"{", semantic.dump(), dependency.dump()};
}

class RetainedCreationState {
public:
    explicit RetainedCreationState(GameSession& session)
        : world(session.GetWorldSystem())
        , water(session.GetWaterSystem())
        , physics(session.GetPhysicsSystem())
        , species(session.GetSpeciesTable())
        , dependencies(world->get_params().worldgen_dependencies)
        , metadata(session.GetMetadata())
        , identity(session.PresetContentHash())
        , generated_identity(
              Luminumbra::World::ComputeTerrainParamsHash(world->get_params(), world->get_seed()))
        , hashes(SessionWorldSubHashes(session))
        , tick(session.GetSimulationTickCount())
        , ledger(session.GetActiveRegionLedger().encode()) {}

    void ExpectUnchanged(GameSession& session) const {
        EXPECT_EQ(session.GetWorldSystem(), world);
        EXPECT_EQ(session.GetWaterSystem(), water);
        EXPECT_EQ(session.GetPhysicsSystem(), physics);
        EXPECT_EQ(session.GetSpeciesTable(), species);
        EXPECT_EQ(session.GetWorldSystem()->get_params().worldgen_dependencies, dependencies);
        EXPECT_EQ(session.GetMetadata().name, metadata.name);
        EXPECT_EQ(session.GetMetadata().seed, metadata.seed);
        EXPECT_EQ(session.GetMetadata().worldType, metadata.worldType);
        EXPECT_EQ(session.GetMetadata().worldId, metadata.worldId);
        EXPECT_EQ(session.GetMetadata().creationTime, metadata.creationTime);
        EXPECT_EQ(session.GetMetadata().spawnPoint, metadata.spawnPoint);
        EXPECT_EQ(session.PresetContentHash(), identity);
        EXPECT_EQ(Luminumbra::World::ComputeTerrainParamsHash(
                      session.GetWorldSystem()->get_params(), session.GetWorldSystem()->get_seed()),
                  generated_identity);
        EXPECT_EQ(SessionWorldSubHashes(session), hashes);
        EXPECT_EQ(session.GetSimulationTickCount(), tick);
        EXPECT_EQ(session.GetActiveRegionLedger().encode(), ledger);
    }

private:
    const Luminumbra::Systems::SHIELD_WorldSystem* world;
    const Luminumbra::Systems::WaterSystem* water;
    const Luminumbra::Systems::PhysicsSystem* physics;
    const luminumbra::ai::CreatureSpeciesRegistry* species;
    std::shared_ptr<const Luminumbra::World::WorldgenDependencySnapshot> dependencies;
    Luminumbra::world::WorldMetadata metadata;
    std::string identity;
    std::uint64_t generated_identity;
    nlohmann::json hashes;
    std::uint64_t tick;
    std::string ledger;
};

TEST_P(SavedWorldPresetTest, NormalCreationRefusalsRetainDirtySessionAndExitSave) {
    const auto base = PrepareCreationRefusalBase(Root().path());
    ASSERT_TRUE(GameSession::ValidateWorldConfig(Root().root_string(), "default").ok);
    const auto resolved = Luminumbra::world::ResolveEffectivePreset(
        base, Luminumbra::world::TerrainPresetUse::NewWorld);
    const auto loaded_base =
        Luminumbra::world::LoadTerrainPresetFromJson(resolved, Root().path() / "data");
    ASSERT_TRUE(loaded_base.ok);
    ASSERT_FALSE(loaded_base.params.structures_enabled);
    const auto invalid = CreationRefusalInputs(base);
    const auto dependency = Luminumbra::world::LoadTerrainPresetFromJson(
        nlohmann::json::parse(invalid.back()), Root().path() / "data");
    ASSERT_FALSE(dependency.ok);
    ASSERT_FALSE(dependency.errors.empty());
    ASSERT_NE(dependency.errors.front().find("structure pools"), std::string::npos);

    // Both the first snapshot and the incremental snapshot must remain saveable.
    for (const bool existing_snapshot : {false, true}) {
        for (const auto& custom : invalid) {
            SCOPED_TRACE(existing_snapshot);
            SCOPED_TRACE(custom);
            GameSession session;
            CreateSnapshotSession(session);
            ASSERT_FALSE(HasFatalFailure());
            for (int tick = 0; tick < 3; ++tick)
                ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
            ASSERT_GT(session.GetSimulationTickCount(), 0u);
            if (GetParam()) {
                ASSERT_GT(session.GetActiveRegionLedger().tick(), 0u);
            }
            std::shared_ptr<Luminumbra::Chunk> chunk;
            PreparePendingSnapshot(session, chunk, existing_snapshot);
            ASSERT_FALSE(HasFatalFailure());
            const PendingSnapshotState pending(session);
            const RetainedCreationState retained(session);
            const auto disk = SavedPresetTestDiskBytes(Root().path());
            const auto save = session.GetWorldSaveDir();
            const auto id = session.GetMetadata().worldId;
            EXPECT_FALSE(session.CreateWorld("Refused replacement", "999", "default", &custom));
            EXPECT_FALSE(session.GetWorldOpenError().empty());
            retained.ExpectUnchanged(session);
            pending.ExpectUnchanged(session, *chunk);
            // Includes every directory, even an orphan with no catalog metadata.
            EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);

            bool spawn_updated = false;
            bool saved_reported = false;
            Luminumbra::Client::SaveSessionOnExit(
                &session,
                true,
                [&] {
                    session.SetSpawnPoint(Luminumbra::Vec3(19, 23, -17));
                    spawn_updated = true;
                },
                [&] {
                    EXPECT_TRUE(spawn_updated);
                    ExpectSnapshotReloaded(pending, *chunk, save, id);
                    EXPECT_FALSE(chunk->is_voxel_data_dirty());
                    const auto info =
                        nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
                    EXPECT_EQ(info.at("spawnPoint").at("x"), 19);
                    saved_reported = true;
                });
            ASSERT_TRUE(spawn_updated);
            ASSERT_TRUE(saved_reported);
            EXPECT_FALSE(session.GetWorldOpenError().empty());
            // Explicit manual/autosave entry points also work without clearing diagnostics.
            ASSERT_TRUE(session.SaveWorldState());
            ASSERT_TRUE(session.SaveWorld());
            ExpectSnapshotReloaded(pending, *chunk, save, id);
            GameSession reopened;
            Configure(reopened);
            ASSERT_TRUE(reopened.LoadWorld(id)) << reopened.GetWorldOpenError();
            EXPECT_EQ(reopened.GetSimulationTickCount(), GetParam() ? 3u : 0u);
            if (GetParam()) {
                EXPECT_EQ(reopened.GetActiveRegionLedger().encode(),
                          session.GetActiveRegionLedger().encode());
            }
        }
    }
}

TEST_P(SavedWorldPresetTest, RefusedOpenAndMissingJobsPreserveSaveEligibility) {
    GameSession session;
    CreateSnapshotSession(session);
    ASSERT_FALSE(HasFatalFailure());
    std::shared_ptr<Luminumbra::Chunk> chunk;
    PreparePendingSnapshot(session, chunk, false);
    ASSERT_FALSE(HasFatalFailure());
    const PendingSnapshotState pending(session);
    const RetainedCreationState retained(session);
    const auto disk = SavedPresetTestDiskBytes(Root().path());
    const auto id = session.GetMetadata().worldId;
    session.SetJobSystem(nullptr);
    EXPECT_FALSE(session.CreateWorld("No jobs", "999", "default"));
    EXPECT_NE(session.GetWorldOpenError().find("JobSystem"), std::string::npos);
    EXPECT_FALSE(session.LoadWorld(id));
    EXPECT_NE(session.GetWorldOpenError().find("JobSystem"), std::string::npos);
    Configure(session);
    EXPECT_FALSE(session.CreateWorld("Missing named preset", "999", "missing-preset"));
    EXPECT_FALSE(session.CreateWorld("Recording mismatch",
                                     "999",
                                     "default",
                                     nullptr,
                                     Luminumbra::world::TerrainPresetUse::Historical,
                                     "different-content"));
    session.SetRequiredClientAssets({"missing-required-asset"});
    EXPECT_FALSE(session.CreateWorld("Missing asset", "999", "default"));
    EXPECT_FALSE(session.LoadWorld(id));
    session.SetRequiredClientAssets({});
    EXPECT_FALSE(session.LoadWorld("missing-world"));
    EXPECT_FALSE(session.GetWorldOpenError().empty());
    retained.ExpectUnchanged(session);
    pending.ExpectUnchanged(session, *chunk);
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.SaveWorld());
    ExpectSnapshotReloaded(pending, *chunk, session.GetWorldSaveDir(), id);
}

TEST_P(SavedWorldPresetTest, InvalidLivePlantRefusesBeforeAnySnapshotPayloadWrite) {
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(session));
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, true));
    auto plants = session.GetRegistry().view<C::PlantTag, C::PlantGenomeComponent>();
    ASSERT_NE(plants.begin(), plants.end());
    plants.get<C::PlantGenomeComponent>(*plants.begin()).genes[0] =
        std::numeric_limits<float>::infinity();
    const PendingSnapshotState pending(session);
    const RetainedCreationState retained(session);
    const auto disk = SavedPresetTestDiskBytes(Root().path());
    const auto destination = Root().path() / "invalid-live-plant";
    EXPECT_FALSE(session.SaveWorldStateTo(destination));
    EXPECT_FALSE(fs::exists(destination));
    EXPECT_FALSE(session.SaveWorldState());
    retained.ExpectUnchanged(session);
    pending.ExpectUnchanged(session, *chunk);
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
}

TEST_P(SavedWorldPresetTest, SnapshotReloadReplacesPlantsClockAndChunksTransactionally) {
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(session));
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, true));
    const auto save = session.GetWorldSaveDir();
    Luminumbra::Ecs::EntityRegistrySnapshot saved;
    ASSERT_TRUE(P::WorldSaveService::load_plant_entities(saved, save));
    ASSERT_EQ(saved.entities.size(), 1u);
    const auto saved_id = static_cast<entt::entity>(saved.entities.front().entity_id);
    const auto disk = SavedPresetTestDiskBytes(Root().path());
    auto& registry = session.GetRegistry();
    const auto extra = registry.create();
    registry.emplace<C::PlantTag>(extra);
    registry.emplace<C::PlantGrowthComponent>(extra);
    registry.emplace<C::PlantGenomeComponent>(extra);
    registry.emplace<C::TransformComponent>(extra);
    session.GetWorldSystem()->SetWaterSimWindowCursor(23);
    for (int tick = 0; tick < 3; ++tick)
        session.TickSimulation(session.GetSimulationClock().fixed_dt());
    for (int reload = 0; reload < 2; ++reload) {
        ASSERT_TRUE(session.LoadWorldStateFrom(save)) << session.GetWorldOpenError();
        EXPECT_EQ(registry.view<C::PlantTag>().size(), 1u);
        ASSERT_TRUE(registry.valid(saved_id));
        EXPECT_EQ(registry.get<C::PlantGrowthComponent>(saved_id).growth_points, 0u);
        EXPECT_FALSE(registry.valid(extra));
        EXPECT_EQ(session.GetSimulationTickCount(), 0u);
        EXPECT_EQ(session.GetWorldSystem()->GetWaterSimWindowCursor(), 0u);
        const auto chunks = session.GetWorldSystem()->snapshot_streamed_chunks();
        const auto restored =
            std::find_if(chunks.begin(), chunks.end(), [&](const auto& candidate) {
                return candidate && candidate->get_id() == chunk->get_id();
            });
        ASSERT_NE(restored, chunks.end());
        EXPECT_EQ((*restored)->heightmap_data, (std::vector<float>{8.0f, 9.0f, 10.0f}));
        EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
    }
    const auto residents = session.GetWorldSystem()->snapshot_streamed_chunks();
    ASSERT_FALSE(residents.empty());
    ASSERT_TRUE(session.LoadWorldStateFrom(Root().path() / "missing-chunk-input"));
    EXPECT_EQ(session.GetWorldSystem()->snapshot_streamed_chunks(), residents);
    // An actual empty snapshot replaces chunks; a missing snapshot is a clean miss.
    const auto empty_save = Root().path() / "empty-chunk-input";
    const Luminumbra::WorldStreamingState empty_chunks;
    ASSERT_TRUE(P::WorldSaveService{}.save_world(empty_chunks, empty_save));
    ASSERT_TRUE(session.LoadWorldStateFrom(empty_save));
    EXPECT_TRUE(session.GetWorldSystem()->snapshot_streamed_chunks().empty());
}

TEST_P(SavedWorldPresetTest, PlantIdentityClashRefusesSnapshotReloadWithoutAdoptionOrWrites) {
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(session));
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, true));
    // The session's plants are replaceable. A non-plant holding the saved slot
    // is a foreign identity clash and must preserve the entire live session.
    const auto plants = session.GetRegistry().view<C::PlantTag>();
    ASSERT_NE(plants.begin(), plants.end());
    session.GetRegistry().remove<C::PlantTag>(*plants.begin());
    const PendingSnapshotState pending(session);
    const RetainedCreationState retained(session);
    const auto save = session.GetWorldSaveDir();
    const auto disk = SavedPresetTestDiskBytes(Root().path());
    EXPECT_FALSE(session.LoadWorldStateFrom(save));
    EXPECT_NE(session.GetWorldOpenError().find("plant entity identity clash"), std::string::npos);
    retained.ExpectUnchanged(session);
    pending.ExpectUnchanged(session, *chunk);
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
    // Full world loads replace the registry, so their old roster is not a clash.
    const auto world_id = session.GetMetadata().worldId;
    ASSERT_TRUE(session.LoadWorldFrom(world_id, Root().path()));
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
}

TEST_P(SavedWorldPresetTest, NonPlantAllocatorOwnersArePreservedOrRefusedBeforeAdoption) {
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(session));
    auto& registry = session.GetRegistry();
    const auto foreign = registry.create();
    registry.emplace<C::TransformComponent>(foreign).position = Luminumbra::Vec3(7.0f);
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, true));
    const PendingSnapshotState pending(session);
    const RetainedCreationState retained(session);
    const auto disk = SavedPresetTestDiskBytes(Root().path());
    const auto world_id = session.GetMetadata().worldId;
    // A full load has no non-plant owners to restore. It must refuse before reset.
    EXPECT_FALSE(session.LoadWorld(world_id));
    EXPECT_FALSE(session.LoadWorldFrom(world_id, Root().path()));
    retained.ExpectUnchanged(session);
    pending.ExpectUnchanged(session, *chunk);
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
    // In-place reload has the exact foreign owner and preserves its components.
    ASSERT_TRUE(session.LoadWorldStateFrom(session.GetWorldSaveDir()));
    ASSERT_TRUE(registry.valid(foreign));
    EXPECT_EQ(registry.get<C::TransformComponent>(foreign).position, Luminumbra::Vec3(7.0f));
    EXPECT_EQ(registry.view<C::PlantTag>().size(), 1u);
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
}

TEST_P(SavedWorldPresetTest, InvalidPlantProjectionRefusesAllLoadPathsWithoutAdoptionOrWrites) {
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(session));
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, true));
    const PendingSnapshotState pending(session);
    const RetainedCreationState retained(session);
    const auto save = session.GetWorldSaveDir();
    const auto path = P::WorldSaveService::plant_entities_path(save);
    auto snapshot = luminumbra::foliage::BuildPlantEntitySnapshot(session.GetRegistry());
    ASSERT_FALSE(snapshot.entities.empty());
    for (auto& component : snapshot.entities.front().components) {
        if (component.type == "PlantGrowth")
            component.data["stage"] = 1e38;
    }
    ASSERT_TRUE(P::WorldSaveService::write_file_durably(
        path, Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(snapshot)));
    const auto disk = SavedPresetTestDiskBytes(Root().path());
    const auto world_id = session.GetMetadata().worldId;
    for (int entry = 0; entry < 3; ++entry) {
        SCOPED_TRACE(entry);
        const bool loaded = entry == 0   ? session.LoadWorld(world_id)
                            : entry == 1 ? session.LoadWorldFrom(world_id, Root().path())
                                         : session.LoadWorldStateFrom(save);
        EXPECT_FALSE(loaded);
        EXPECT_NE(session.GetWorldOpenError().find("invalid plant component"), std::string::npos);
        retained.ExpectUnchanged(session);
        pending.ExpectUnchanged(session, *chunk);
        EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
    }
    // A refused load did not disable saving a coherent live session to another destination.
    EXPECT_TRUE(session.SaveWorldStateTo(Root().path() / "after-refused-plant"));
}

#if !defined(_WIN32)
TEST_P(SavedWorldPresetTest, NonRegularPlantFilesRefuseAllLoadPathsBeforeAdoption) {
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(session));
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, true));
    const PendingSnapshotState pending(session);
    const RetainedCreationState retained(session);
    const auto save = session.GetWorldSaveDir();
    const auto path = P::WorldSaveService::plant_entities_path(save);
    const auto snapshot = luminumbra::foliage::BuildPlantEntitySnapshot(session.GetRegistry());
    ASSERT_FALSE(snapshot.entities.empty());
    const auto bytes = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(snapshot);
    const auto target = save / "valid-plant-roster.json";
    ASSERT_TRUE(P::WorldSaveService::write_file_durably(target, bytes));
    const auto world_id = session.GetMetadata().worldId;
    for (const int kind : {0, 1, 2, 3}) {
        SCOPED_TRACE(kind);
        ASSERT_TRUE(fs::remove(path));
        if (kind == 0)
            ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);
        else
            fs::create_symlink(kind == 1   ? target
                               : kind == 2 ? fs::path("/dev/zero")
                                           : save / "absent-roster",
                               path);
        const int keeper = kind == 0 ? ::open(path.c_str(), O_RDONLY | O_NONBLOCK) : -1;
        struct CloseReader {
            int descriptor;
            ~CloseReader() {
                if (descriptor >= 0)
                    ::close(descriptor);
            }
        } close_reader{keeper};
        if (kind == 0)
            ASSERT_GE(keeper, 0);
        for (int entry = 0; entry < 3; ++entry) {
            SCOPED_TRACE(entry);
            if (kind == 0) {
                const int writer = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
                ASSERT_GE(writer, 0);
                const auto written = ::write(writer, bytes.data(), bytes.size());
                const int closed = ::close(writer);
                ASSERT_EQ(written, static_cast<ssize_t>(bytes.size()));
                ASSERT_EQ(closed, 0);
            }
            const bool loaded = entry == 0   ? session.LoadWorld(world_id)
                                : entry == 1 ? session.LoadWorldFrom(world_id, Root().path())
                                             : session.LoadWorldStateFrom(save);
            if (kind == 0) {
                // Each entry point sees one valid JSON payload, even when the
                // preceding refusal left all FIFO bytes unread.
                std::string unread(bytes.size(), '\0');
                EXPECT_GE(::read(keeper, unread.data(), unread.size()), 0);
            }
            ASSERT_FALSE(loaded);
            EXPECT_FALSE(session.GetWorldOpenError().empty());
            retained.ExpectUnchanged(session);
            pending.ExpectUnchanged(session, *chunk);
        }
    }
}
#endif

TEST_P(SavedWorldPresetTest, FatalSnapshotAndFinalMetadataRefusalsCannotBeClearedByCreation) {
    const auto invalid = std::string("{");
    for (const bool metadata_failure : {false, true}) {
        SCOPED_TRACE(metadata_failure);
        GameSession session;
        Configure(session);
        if (metadata_failure) {
            // The generic replacement seam also intercepts preset publication.
            // The existing ledger hook reaches the final metadata save after adoption.
            session.SetActiveRegionsEnabled(true);
            P::WorldSaveService::set_before_active_regions_replace_for_testing(
                [] { return false; });
            const bool created = session.CreateWorld("Incomplete metadata", "1337", "default");
            P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
            ASSERT_FALSE(created);
            ASSERT_NE(session.GetWorldOpenError().find("metadata"), std::string::npos);
        } else {
            ASSERT_TRUE(session.CreateWorld("Corrupt snapshot", "1337", "default"));
            const auto corrupt = Root().path() / "corrupt-state";
            ASSERT_TRUE(P::WorldSaveService::write_file_durably(corrupt / "world_info.json", "{"));
            ASSERT_FALSE(session.LoadWorldStateFrom(corrupt));
            ASSERT_FALSE(session.GetWorldOpenError().empty());
        }
        ASSERT_NE(session.GetWorldSystem(), nullptr);
        const auto disk = SavedPresetTestDiskBytes(Root().path());
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorld());
        EXPECT_FALSE(session.CreateWorld("Refused", "999", "default", &invalid));
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorld());
        session.SetJobSystem(nullptr);
        EXPECT_FALSE(session.CreateWorld("Missing jobs", "999", "default"));
        EXPECT_NE(session.GetWorldOpenError().find("JobSystem"), std::string::npos);
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorld());
        EXPECT_FALSE(session.LoadWorldState());
        unsigned spawn_updates = 0;
        Luminumbra::Client::SaveSessionOnExit(
            &session,
            true,
            [&] { ++spawn_updates; },
            [] { FAIL() << "Fatal session reported save success"; });
        EXPECT_EQ(spawn_updates, 1u);
        EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
        Configure(session);
        EXPECT_FALSE(session.LoadWorld("missing-world"));
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorld());
        ASSERT_TRUE(session.CreateWorld("Coherent replacement", "1337", "default"));
        EXPECT_TRUE(session.GetWorldOpenError().empty());
        EXPECT_TRUE(session.SaveWorldState());
        EXPECT_TRUE(session.SaveWorld());
    }
}

TEST_P(SavedWorldPresetTest, FreshAndTransientRefusalsAndExactCustomReplacement) {
    const auto base = PrepareCreationRefusalBase(Root().path());
    const auto invalid = CreationRefusalInputs(base);
    GameSession session;
    Configure(session);
    for (const auto& custom : invalid) {
        const auto disk = SavedPresetTestDiskBytes(Root().path());
        EXPECT_FALSE(session.CreateWorld("Fresh refusal", "999", "default", &custom));
        EXPECT_FALSE(session.GetWorldOpenError().empty());
        EXPECT_EQ(session.GetWorldSystem(), nullptr);
        EXPECT_EQ(session.GetSpeciesTable(), nullptr);
        EXPECT_TRUE(session.PresetContentHash().empty());
        EXPECT_TRUE(session.GetMetadata().worldId.empty());
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorld());
        EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
    }
    auto semantic = nlohmann::json::parse(invalid[1]);
    WriteSavedPresetTestJson(Root().path() / "worlds/atlas/presets/default.json", semantic);
    const auto fresh_disk = SavedPresetTestDiskBytes(Root().path());
    EXPECT_FALSE(session.CreateTransientWorld("Fresh transient refusal", "1337", "default"));
    EXPECT_EQ(session.GetWorldSystem(), nullptr);
    EXPECT_FALSE(session.LoadWorldState());
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), fresh_disk);
    WriteSavedPresetTestJson(Root().path() / "worlds/atlas/presets/default.json", base);
    ASSERT_TRUE(session.CreateTransientWorld("Menu", "1337", "default"));
    const RetainedCreationState retained(session);
    WriteSavedPresetTestJson(Root().path() / "worlds/atlas/presets/default.json", semantic);
    const auto disk = SavedPresetTestDiskBytes(Root().path());
    EXPECT_FALSE(session.CreateTransientWorld("Refused menu", "999", "default"));
    retained.ExpectUnchanged(session);
    EXPECT_TRUE(session.LoadWorldState());
    EXPECT_FALSE(session.SaveWorld());
    EXPECT_FALSE(session.SaveWorldState());
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
    WriteSavedPresetTestJson(Root().path() / "worlds/atlas/presets/default.json", base);
    auto custom = base;
    custom["generation_params"]["terrain"]["height_offset"] = 123.0;
    custom["generation_params"]["terrain"].erase("surface_sampling_revision");
    const auto text = custom.dump();
    const auto resolved = Luminumbra::world::ResolveEffectivePreset(
        custom, Luminumbra::world::TerrainPresetUse::NewWorld);
    const auto expected =
        Luminumbra::world::LoadTerrainPresetFromJson(resolved, Root().path() / "data");
    ASSERT_TRUE(expected.ok);
    ASSERT_TRUE(session.CreateWorld("Valid replacement", "777", "default", &text));
    EXPECT_TRUE(session.GetWorldOpenError().empty());
    const auto save = session.GetWorldSaveDir();
    EXPECT_EQ(ReadClockTestFile(save / "preset.json"), resolved.dump() + "\n");
    EXPECT_EQ(session.PresetContentHash(), expected.preset_content_hash);
    EXPECT_EQ(session.GetWorldSystem()->get_params().surface_sampling_revision, 2);
    EXPECT_FLOAT_EQ(session.GetWorldSystem()->get_params().height_offset, 123.0f);
    const auto info = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    EXPECT_EQ(info.at("presetIdentity"), P::StableChecksum(resolved.dump()));
    EXPECT_EQ(info.at("worldType"), "default");
    GameSession reopened;
    Configure(reopened);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    EXPECT_EQ(reopened.PresetContentHash(), expected.preset_content_hash);
    EXPECT_EQ(reopened.GetWorldSystem()->get_params().surface_sampling_revision, 2);
    EXPECT_FLOAT_EQ(reopened.GetWorldSystem()->get_params().height_offset, 123.0f);
}

TEST_P(SavedWorldPresetTest, SnapshotLoadRestoresWaterCursorAndDefaultsMissingMetadataToZero) {
    GameSession source, reader;
    Configure(source);
    Configure(reader);
    ASSERT_TRUE(source.CreateWorld("Water cursor source", "1337", "default"));
    ASSERT_TRUE(reader.CreateTransientWorld("Water cursor reader", "1337", "default"));
    // A value beyond the 64-chunk update cap must survive exactly; clamping it
    // would choose a different next water window when more chunks are awake.
    constexpr std::size_t saved_cursor = 65;
    source.GetWorldSystem()->SetWaterSimWindowCursor(saved_cursor);
    ASSERT_TRUE(source.SaveWorld());
    const auto source_save = source.GetWorldSaveDir();
    ASSERT_TRUE(source.LoadWorld(source.GetMetadata().worldId)) << source.GetWorldOpenError();
    EXPECT_EQ(source.GetWorldSystem()->GetWaterSimWindowCursor(), saved_cursor);

    const auto snapshot = Root().path() / "water-cursor-snapshot";
    for (const bool with_chunks : {false, true}) {
        SCOPED_TRACE(with_chunks);
        fs::remove_all(snapshot);
        ASSERT_TRUE(fs::create_directories(snapshot));
        fs::copy_file(source_save / "world_info.json", snapshot / "world_info.json");
        fs::copy_file(source_save / "preset.json", snapshot / "preset.json");
        if (with_chunks) {
            Luminumbra::WorldStreamingState state;
            auto chunk = state.get_or_create_chunk(Luminumbra::IVec3(0, 0, 0));
            chunk->set_state(Luminumbra::ChunkState::Idle);
            ASSERT_TRUE(P::WorldSaveService{}.save_world(state, snapshot));
        }
        reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
        auto before = SavedPresetTestDiskBytes(snapshot);
        ASSERT_TRUE(reader.LoadWorldStateFrom(snapshot)) << reader.GetWorldOpenError();
        EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), saved_cursor);
        EXPECT_EQ(SavedPresetTestDiskBytes(snapshot), before);

        auto metadata = nlohmann::json::parse(ReadClockTestFile(snapshot / "world_info.json"));
        ASSERT_EQ(metadata.erase("waterSimCursor"), 1u);
        ASSERT_NO_FATAL_FAILURE(WriteSavedPresetTestJson(snapshot / "world_info.json", metadata));
        reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
        before = SavedPresetTestDiskBytes(snapshot);
        ASSERT_TRUE(reader.LoadWorldStateFrom(snapshot)) << reader.GetWorldOpenError();
        EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 0u);
        EXPECT_EQ(SavedPresetTestDiskBytes(snapshot), before);

        ASSERT_TRUE(fs::remove(snapshot / "world_info.json"));
        reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
        ASSERT_TRUE(reader.LoadWorldStateFrom(snapshot)) << reader.GetWorldOpenError();
        EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 0u);
    }
    reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
    ASSERT_TRUE(reader.LoadWorldStateFrom(Root().path() / "missing-water-snapshot"));
    EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 0u);
}

TEST_P(SavedWorldPresetTest, InvalidWaterCursorRefusesCatalogAndSnapshotWithoutChangingCursor) {
    GameSession source;
    Configure(source);
    ASSERT_TRUE(source.CreateWorld("Invalid water cursor", "1337", "default"));
    const auto save = source.GetWorldSaveDir();
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    const std::array<nlohmann::json, 8> invalid{
        nullptr, -1, 1.5, 1e38, true, "65", nlohmann::json::array(), nlohmann::json::object()};
    for (const auto& value : invalid) {
        SCOPED_TRACE(value.dump());
        auto malformed = metadata;
        malformed["waterSimCursor"] = value;
        ASSERT_NO_FATAL_FAILURE(WriteSavedPresetTestJson(save / "world_info.json", malformed));
        const auto before = SavedPresetTestDiskBytes(save);
        GameSession reader;
        Configure(reader);
        ASSERT_TRUE(reader.CreateTransientWorld("Cursor refusal", "1337", "default"));
        reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
        EXPECT_FALSE(reader.LoadWorldStateFrom(save));
        EXPECT_NE(reader.GetWorldOpenError().find("water simulation cursor"), std::string::npos);
        EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 19u);
        EXPECT_FALSE(
            P::InspectSavedWorld(Root().path(), source.GetMetadata().worldId).error.empty());
        EXPECT_EQ(SavedPresetTestDiskBytes(save), before);
    }
}

TEST_P(SavedWorldPresetTest, UnmarkedInvalidDestinationRefusesBeforePresetOrPayloadWrites) {
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(session));
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, false));
    const PendingSnapshotState pending(session);
    const auto save = session.GetWorldSaveDir();
    const std::array<std::pair<fs::path, std::string>, 5> artifacts{{
        {P::WorldSaveService::world_state_path(save), "obsolete snapshot"},
        {P::WorldSaveService::region_file_path(save, 0, 0), std::string("LMR1\xff\xff\0\0", 8)},
        {P::WorldSaveService::region_file_path(save, 0, 0), "LMR1"},
        {P::WorldSaveService::world_manifest_path(save), "{"},
        {P::WorldSaveService::region_directory(save) / "unknown.bin", "unknown artifact"},
    }};
    for (const auto& [path, bytes] : artifacts) {
        SCOPED_TRACE(path.generic_string());
        fs::remove_all(save);
        fs::create_directories(path.parent_path());
        {
            std::ofstream output(path, std::ios::binary);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.close();
            ASSERT_FALSE(output.fail());
        }
        ASSERT_FALSE(fs::exists(save / "preset.json"));
        ASSERT_FALSE(fs::exists(save / "world_info.json"));
        ASSERT_FALSE(P::WorldSaveService::validate_save(save));
        const SnapshotDestinationBytes before(save);
        EXPECT_FALSE(session.SaveWorld());
        before.ExpectUnchanged();
        pending.ExpectUnchanged(session, *chunk);
        EXPECT_FALSE(session.SaveWorldState());
        before.ExpectUnchanged();
        pending.ExpectUnchanged(session, *chunk);
        EXPECT_FALSE(session.SaveWorldStateTo(save));
        before.ExpectUnchanged();
        pending.ExpectUnchanged(session, *chunk);
        EXPECT_FALSE(fs::exists(save / "preset.json"));
        EXPECT_FALSE(fs::exists(save / "world_info.json"));
    }
}

INSTANTIATE_TEST_SUITE_P(ActiveRegionsEnabledAndDisabled, SavedWorldPresetTest, testing::Bool());

TEST(GameSessionHeadlessWorldTest,
     TransientCalendarSnapshotsPreservePresetIdentityAndRefuseChanges) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original;
    original.SetRootPath(root.root_string());
    original.SetJobSystem(&jobs);
    original.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(original.CreateWorld("Snapshot identity", "1337", "default"));
    const auto original_metadata =
        nlohmann::json::parse(ReadClockTestFile(original.GetWorldSaveDir() / "world_info.json"));

    for (const bool dirty : {false, true}) {
        SCOPED_TRACE(dirty);
        GameSession preview;
        preview.SetRootPath(root.root_string());
        preview.SetJobSystem(&jobs);
        preview.SetActiveRegionsEnabled(false);
        const SnapshotDestinationBytes before_preview(root.path());
        ASSERT_TRUE(preview.CreateTransientWorld("Calendar preview", "1337", "default"));
        before_preview.ExpectUnchanged();
        EXPECT_EQ(preview.PresetContentHash(), original.PresetContentHash());
        const auto snapshot = root.path() / (dirty ? "dirty-preview" : "empty-preview");
        ASSERT_TRUE(preview.SaveWorldStateTo(snapshot));
        const auto metadata =
            nlohmann::json::parse(ReadClockTestFile(snapshot / "world_info.json"));
        EXPECT_EQ(metadata.at("renderCalendar"), original_metadata.at("renderCalendar"));
        EXPECT_EQ(metadata.at("presetFormat"), original_metadata.at("presetFormat"));
        EXPECT_EQ(metadata.at("presetIdentity"), original_metadata.at("presetIdentity"));
        EXPECT_EQ(metadata.at("presetIdentity"),
                  P::StableChecksum(
                      nlohmann::json::parse(ReadClockTestFile(snapshot / "preset.json")).dump()));
        const SnapshotPresetFiles preset_files(snapshot);
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
        chunk->set_state(Luminumbra::ChunkState::Idle);
        if (dirty) {
            chunk->mark_voxel_data_dirty();
            ASSERT_TRUE(preview.GetWorldSystem()->adopt_streamed_chunk(chunk));
        }
        for (const auto* changed : {"preset", "identity", "both"}) {
            SCOPED_TRACE(changed);
            ASSERT_NO_FATAL_FAILURE(preset_files.Rewrite(changed));
            const SnapshotDestinationBytes before(root.path());
            EXPECT_FALSE(preview.SaveWorldStateTo(snapshot));
            before.ExpectUnchanged();
            EXPECT_EQ(chunk->is_voxel_data_dirty(), dirty);
            ASSERT_NO_FATAL_FAILURE(preset_files.Restore());
        }
        ASSERT_TRUE(preview.SaveWorldStateTo(snapshot));
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
    }
}

} // namespace
