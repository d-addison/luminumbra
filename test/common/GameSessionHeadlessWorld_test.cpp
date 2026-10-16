// asset-manifest split. The engine-side GameSession validates
// SIMULATION requirements only (world preset readable/parseable); renderer/UI
// asset requirements are caller-supplied by the client. Enabled headless creation
// also validates simulation content inventories and the optional policy parent.
#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"

#include "../support/SystemConfigVariants.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "luminumbra_client/app/RenderMeasurement.h"
#include "luminumbra_client/app/SessionExit.h"
#include "luminumbra_client/rendering/TimeOfDayModel.h"
#include "luminumbra_common/ai/CircadianSystem.h"
#include "luminumbra_common/ai/CreaturePersistence.h"
#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/ai/LifespanSystem.h"
#include "luminumbra_common/ai/MigrationSystem.h"
#include "luminumbra_common/ai/ScentField.h"
#include "luminumbra_common/ai/StimulusChannels.h"
#include "luminumbra_common/components/FieldEmitterComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/replay/RegionScheduleTrace.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/CropLifecycleSystem.h"
#include "luminumbra_common/systems/FireSpreadSystem.h"
#include "luminumbra_common/systems/FoliageRegistryStage.h"
#include "luminumbra_common/systems/PlantDiseaseSystem.h"
#include "luminumbra_common/systems/PlantGrowthSystem.h"
#include "luminumbra_common/systems/RegionalFoliage.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"

#include <nlohmann/json.hpp>

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/fields/FieldPageState.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/scripting/LuaState.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarAuthorityOverlay.h"
#include "luminumbra_common/world/FarWantedSet.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include "luminumbra_server/SimBudgetArtifact.h"

namespace fs = std::filesystem;

namespace {

using Luminumbra::JobSystem;
using Luminumbra::world::GameSession;
using Luminumbra::world::WorldConfigValidationResult;

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

fs::path SourcePresetPath() {
    return fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds" / "atlas" / "presets" / "default.json";
}

// Temp root with a world preset and optional minimal simulation species; no client assets. The root
// is unique per fixture instance so concurrent common_tests processes never share (or clobber) a
// directory; each TEST builds ONE instance and reuses it, so within-run path stability still holds.
// The destructor removes the tree.
class HeadlessRoot {
public:
    explicit HeadlessRoot(bool simulation_content = true) {
        root_ =
            Luminumbra::TestSupport::CreateUniqueTempDirectory("luminumbra_headless_world_test_");
        fs::create_directories(root_ / "worlds" / "atlas" / "presets");
        fs::copy_file(SourcePresetPath(), root_ / "worlds" / "atlas" / "presets" / "default.json");
        if (simulation_content)
            Luminumbra::TestSupport::WriteSimulationContentFixture(root_);
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

TEST(GameSessionHeadlessWorldTest, ValidateWorldConfigIsSimulationOnlyByDefault) {
    const HeadlessRoot root(false);
    ASSERT_FALSE(fs::exists(root.path() / "res"));
    ASSERT_FALSE(fs::exists(root.path() / "data"));

    const WorldConfigValidationResult result =
        GameSession::ValidateWorldConfig(root.root_string(), "default");
    EXPECT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_TRUE(result.errors.empty());

    const auto preset_only = Luminumbra::world::LoadTerrainPreset(result.preset_path);
    ASSERT_TRUE(preset_only.ok);
    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root.path());
    const auto full_data = Luminumbra::world::LoadTerrainPreset(result.preset_path);
    ASSERT_TRUE(full_data.ok);
    EXPECT_NE(preset_only.preset_content_hash, full_data.preset_content_hash);
}

TEST(GameSessionHeadlessWorldTest, CallerSuppliedRequiredAssetsAreEnforced) {
    const HeadlessRoot root;

    const std::vector<fs::path> client_assets = {
        fs::path("res") / "shaders" / "basic.vert",
        fs::path("data") / "ui" / "main_menu.rml",
    };
    const WorldConfigValidationResult result =
        GameSession::ValidateWorldConfig(root.root_string(), "default", client_assets);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.errors.size(), 2u);
    EXPECT_NE(result.errors.front().find("missing required runtime asset"), std::string::npos);
}

TEST(GameSessionHeadlessWorldTest, SessionRequiredAssetListFailsCreateWorldWhenMissing) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        session.SetRequiredClientAssets({fs::path("res") / "shaders" / "basic.vert"});
        EXPECT_FALSE(session.CreateWorld("ClientAssetMissing", "12345", "default"));
    }
    jobs.shutdown();
}

TEST(GameSessionHeadlessWorldTest, HeadlessCreateWorldSucceedsWithPresetOnly) {
    const HeadlessRoot root(false);
    ASSERT_FALSE(fs::exists(root.path() / "data"));
    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        // A headless host registers no client assets, but enabled simulation still needs content.
        EXPECT_FALSE(session.CreateWorld("HeadlessWorld", "12345", "default"));
        EXPECT_FALSE(session.GetWorldOpenError().empty());
        EXPECT_EQ(session.GetWorldSystem(), nullptr);
        EXPECT_FALSE(fs::exists(root.path() / "worlds/saves"));
        Luminumbra::TestSupport::WriteSimulationContentFixture(root.path());
        ASSERT_TRUE(session.CreateWorld("HeadlessWorld", "12345", "default"));
        EXPECT_NE(session.GetWorldSystem(), nullptr);
        EXPECT_NE(session.GetPhysicsSystem(), nullptr);
        EXPECT_FALSE(session.GetMetadata().worldId.empty());
        EXPECT_TRUE(session.ActiveRegionsEnabled());
        EXPECT_TRUE(session.AetherStateEnabled());
        EXPECT_TRUE(session.WeatherRainEnabled());
        EXPECT_TRUE(session.WeatherEventsEnabled());
        EXPECT_TRUE(session.FiniteHydrologyEnabled());
        EXPECT_EQ(session.GetWorldSystem()->debug_water_sim_resolution(), 16);
        EXPECT_NE(session.GetEnergyFieldState(), nullptr);
        const auto saved_preset =
            root.path() / "worlds/saves" / session.GetMetadata().worldId / "preset.json";
        std::ifstream input(saved_preset);
        ASSERT_TRUE(input);
        const auto resolved = nlohmann::json::parse(input);
        EXPECT_EQ(resolved.at("schema_rev"), 7);
        EXPECT_TRUE(
            resolved.at("generation_params").at("terrain").at("island_mask_enabled").get<bool>());

        // World metadata landed inside the headless root.
        EXPECT_TRUE(fs::exists(root.path() / "worlds" / "saves" / session.GetMetadata().worldId /
                               "world_info.json"));
    }
    jobs.shutdown();
}

TEST(GameSessionHeadlessWorldTest, ScriptHostTracksTheSessionEnergyField) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        session.SetAetherStateEnabled(true);
        ASSERT_TRUE(session.CreateWorld("ScriptWorld", "12345", "default"));
        ASSERT_NE(session.GetScriptState(), nullptr);
        ASSERT_NE(session.GetEnergyFieldState(), nullptr);

        session.GetEnergyFieldState()->SetAnchorCell(0, 0);
        session.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 512);
        session.GetEnergyFieldState()->Tick(1);
        double value = 0.0;
        ASSERT_TRUE(session.GetScriptState()->EvalNumber(
            "return world.sample_energy_field(1, 0, 1)", value));
        EXPECT_DOUBLE_EQ(value, 2.0);
    }
    jobs.shutdown();
}

// A customized world embeds its resolved preset in its OWN save dir (no global custom files) and
// actually generates different terrain. Proves the create-world custom-params path below the UI
// callback boundary, which the UI e2e cannot reach.
TEST(GameSessionHeadlessWorldTest, CustomPresetEmbedsInSaveAndChangesTerrain) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup();
    {
        // Base world from the named preset.
        GameSession base;
        base.SetJobSystem(&jobs);
        base.SetRootPath(root.root_string());
        ASSERT_TRUE(base.CreateWorld("Base", "777", "default"));
        ASSERT_NE(base.GetWorldSystem(), nullptr);

        // Customized world: same seed, much larger amplitude (a resolved preset built off default).
        std::ifstream pf(root.path() / "worlds" / "atlas" / "presets" / "default.json");
        nlohmann::json j;
        pf >> j;
        const double orig = j["generation_params"]["terrain"]["base_amplitude"].get<double>();
        j["generation_params"]["terrain"]["base_amplitude"] = orig + 120.0;
        const std::string custom = j.dump();

        GameSession custom_world;
        custom_world.SetJobSystem(&jobs);
        custom_world.SetRootPath(root.root_string());
        ASSERT_TRUE(custom_world.CreateWorld("Custom", "777", "default", &custom));
        ASSERT_NE(custom_world.GetWorldSystem(), nullptr);
        EXPECT_FALSE(base.PresetContentHash().empty());
        EXPECT_NE(base.PresetContentHash(), custom_world.PresetContentHash());
        const auto identity = Luminumbra::world::LoadTerrainPresetFromJson(j, root.path() / "data");
        ASSERT_TRUE(identity.ok);
        EXPECT_EQ(custom_world.PresetContentHash(), identity.preset_content_hash);

        // The resolved preset is embedded in THIS world's save dir; worldType keeps the base name.
        const fs::path embedded =
            root.path() / "worlds" / "saves" / custom_world.GetMetadata().worldId / "preset.json";
        EXPECT_TRUE(fs::exists(embedded))
            << "custom preset must be embedded in the world's own save";
        EXPECT_EQ(custom_world.GetMetadata().worldType, "default")
            << "worldType records the base name";

        // The cranked amplitude must change generated terrain at some sampled point.
        const std::vector<std::pair<float, float>> pts = {{8.f, 8.f}, {41.f, 17.f}, {-33.f, 52.f}};
        bool differs = false;
        for (const auto& [x, z] : pts) {
            if (base.GetWorldSystem()->GetTerrainHeightAt(x, z) !=
                custom_world.GetWorldSystem()->GetTerrainHeightAt(x, z)) {
                differs = true;
                break;
            }
        }
        EXPECT_TRUE(differs) << "a larger amplitude override must produce different terrain";
    }
    jobs.shutdown();
}

// a save carrying a wrong-sized (non-empty, != (CHUNK+1)^3)
// SDF lattice is QUARANTINED at adoption — the chunk loads with its sdf_data
// cleared (marked for deterministic regeneration) and is never fed to the
// unit-step polygonise. A valid full lattice in the same save survives verbatim.
TEST(GameSessionHeadlessWorldTest, WrongSizedSdfLatticeIsRefusedOnLoad) {
    namespace P = Luminumbra::Persistence;
    using Luminumbra::Chunk;
    using Luminumbra::ChunkState;
    using Luminumbra::IVec3;
    using Luminumbra::WorldStreamingState;
    constexpr std::size_t kFullLattice = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                         (Luminumbra::CHUNK_SIZE_Y + 1) *
                                         (Luminumbra::CHUNK_SIZE_Z + 1);

    const HeadlessRoot root;
    // Author a save whose region holds one CORRUPT chunk (truncated lattice)
    // and one VALID full-lattice control, both Ready with a renderable mesh.
    const fs::path save_dir = root.path() / "worlds" / "saves" / "corrupt_sdf_world";
    const IVec3 corrupt_coords(40, 0, 40);
    const IVec3 control_coords(41, 0, 40);
    {
        WorldStreamingState state;
        const auto make = [&state](const IVec3& coords) {
            auto chunk = state.get_or_create_chunk(coords);
            chunk->set_state(ChunkState::Ready);
            chunk->heightmap_data = {8.0f, 8.5f, 9.0f};
            chunk->mesh_vertices = {
                {Luminumbra::Vec3(0.0f, 1.0f, 0.0f), Luminumbra::Vec3(0.0f, 1.0f, 0.0f), 1u},
                {Luminumbra::Vec3(1.0f, 1.0f, 0.0f), Luminumbra::Vec3(0.0f, 1.0f, 0.0f), 2u},
                {Luminumbra::Vec3(0.0f, 1.0f, 1.0f), Luminumbra::Vec3(0.0f, 1.0f, 0.0f), 3u}};
            chunk->mesh_indices = {0u, 1u, 2u};
            return chunk;
        };
        make(corrupt_coords)->sdf_data = {-2.0f, -0.5f, 0.25f, 1.0f}; // 4 != full lattice
        make(control_coords)->sdf_data.assign(kFullLattice, 1.0f);    // valid (all air)
        P::WorldSaveService service;
        std::vector<std::string> errors;
        ASSERT_TRUE(service.save_world(state, save_dir, &errors));
        ASSERT_TRUE(errors.empty());
    }

    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        ASSERT_TRUE(session.CreateWorld("QuarantineWorld", "12345", "default"));
        EXPECT_FALSE(session.LoadWorldStateFrom(save_dir));
        EXPECT_EQ(session.GetLastLoadedChunkCount(), 0u);
        EXPECT_FALSE(session.GetWorldOpenError().empty());
        EXPECT_TRUE(session.GetWorldSystem()->snapshot_streamed_chunks().empty());
        EXPECT_FALSE(session.SaveWorldStateTo(save_dir));
        EXPECT_FALSE(session.SaveWorld());
    }
    jobs.shutdown();
}

} // namespace

namespace {
namespace P = Luminumbra::Persistence;
namespace C = Luminumbra::Components;
namespace F = luminumbra::foliage;

std::string ReadClockTestFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
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

struct PendingSnapshotState;

class SavedWorldPresetTest : public testing::TestWithParam<bool> {
protected:
    HeadlessRoot root;
    JobSystem jobs;

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
    ASSERT_TRUE(fs::remove(root.path() / "worlds/atlas/presets/default.json"));
    const auto before = SavedPresetTestDiskBytes(root.path());
    const auto inspected = P::InspectSavedWorld(root.path(), created.GetMetadata().worldId);
    ASSERT_TRUE(inspected.error.empty()) << inspected.error;
    EXPECT_EQ(inspected.preset_path, save / "preset.json");
    GameSession loaded;
    Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(created.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.PresetContentHash(), created.PresetContentHash());
    EXPECT_EQ(SavedPresetTestDiskBytes(root.path()), before);
    ASSERT_TRUE(loaded.SaveWorld());
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(save / "world_info.json")), metadata);
}

TEST_P(SavedWorldPresetTest, CustomAdditionalNamedPresetCarriesItsExactIdentity) {
    const auto named = root.path() / "worlds/atlas/presets/other.json";
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
        if (std::string(changed) != "identity")
            ASSERT_NO_FATAL_FAILURE(
                WriteSavedPresetTestJson(directory / "preset.json", other_preset));
        if (std::string(changed) != "preset")
            ASSERT_NO_FATAL_FAILURE(
                WriteSavedPresetTestJson(directory / "world_info.json", other_metadata));
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

struct PendingSnapshotState {
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
    EXPECT_EQ(loaded.ComputePlantSubHash(), pending.plant_hash);
    ASSERT_NE(loaded.GetEnergyFieldState(), nullptr);
    EXPECT_EQ(loaded.GetEnergyFieldState()->CanonicalBytes(), pending.energy_bytes);
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
    fs::create_directories(root.path() / "worlds/atlas/presets-rev6");
    fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets-rev6/default.json",
                  root.path() / "worlds/atlas/presets-rev6/default.json");
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
    const auto historical = root.path() / "worlds/atlas/presets-rev6/default.json";
    fs::create_directories(historical.parent_path());
    fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets-rev6/default.json",
                  historical);
    const auto save = root.path() / "worlds/saves/legacy";
    fs::create_directories(save);
    WriteSavedPresetTestJson(save / "world_info.json",
                             {{"container_version", 2}, {"worldType", "default"}});
    const auto before = SavedPresetTestDiskBytes(root.path());
    const auto inspected = P::InspectSavedWorld(root.path(), "legacy");
    ASSERT_TRUE(inspected.error.empty()) << inspected.error;
    EXPECT_EQ(inspected.preset_path, historical);
    GameSession loaded;
    Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld("legacy")) << loaded.GetWorldOpenError();
    EXPECT_FALSE(loaded.GetWorldSystem()->get_params().island_mask_enabled);
    EXPECT_EQ(SavedPresetTestDiskBytes(root.path()), before);
    ASSERT_TRUE(loaded.SaveWorld());
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    EXPECT_EQ(metadata.at("presetFormat"), 1);
    EXPECT_EQ(metadata.at("presetIdentity"),
              P::StableChecksum(nlohmann::json::parse(ReadClockTestFile(historical)).dump()));
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(save / "preset.json")),
              nlohmann::json::parse(ReadClockTestFile(historical)));
    ASSERT_TRUE(fs::remove(historical));
    EXPECT_TRUE(P::InspectSavedWorld(root.path(), "legacy").error.empty());
}

INSTANTIATE_TEST_SUITE_P(ActiveRegionsEnabledAndDisabled, SavedWorldPresetTest, testing::Bool());

// Reads a record that production writes through a text-mode stream, so its bytes
// carry the host's line endings: the energy record (GameSession writes
// aether_state.efs with a default-mode ofstream) is LF on Linux and CRLF on
// Windows. That platform difference predates this change; normalising here keeps
// the assertions about record CONTENT independent of it. Tests that assert exact
// bytes written by the test itself use binary streams instead.
std::string ReadClockTestTextRecord(const fs::path& path) {
    std::string text = ReadClockTestFile(path);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
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
             session.ActiveRegionsEnabled() ? session.ComputeChunkStateHash()
                                            : P::ComputeWorldStreamingStateHash(state)},
            {"wind", session.GetWindFieldSystem()->ComputeWindSubHash()},
            {"weather", session.GetWeatherSystem()->ComputeWeatherSubHash()},
            {"aether", aether},
            {"scents", session.ComputeScentSubHash()},
            {"ecology",
             session.FoldClockIntoEcologyHash(
                 luminumbra::ai::ComputeEcologySubHash(session.GetRegistry()))},
            {"plants", session.ComputePlantSubHash()}};
}

std::string SessionWorldHash(GameSession& session) {
    const auto hashes = SessionWorldSubHashes(session);
    return P::ComposeWorldHash(hashes.at("chunk"),
                               hashes.at("wind"),
                               hashes.at("weather"),
                               hashes.at("aether"),
                               hashes.at("scents"),
                               hashes.at("ecology"),
                               hashes.at("plants"));
}

entt::entity ClockTestPlant(entt::registry& registry, const Luminumbra::Vec3& position) {
    const auto plant = registry.create();
    registry.emplace<C::PlantTag>(plant);
    registry.emplace<C::PlantGenomeComponent>(plant).genes.fill(0.5f);
    registry.emplace<C::PlantGrowthComponent>(plant);
    registry.emplace<C::TransformComponent>(plant).position = position;
    return plant;
}

entt::entity ClockTestCreature(entt::registry& registry) {
    const auto creature = registry.create();
    registry.emplace<C::CreatureComponent>(creature);
    registry.emplace<C::CircadianComponent>(creature);
    registry.emplace<C::MigratoryComponent>(creature);
    registry.emplace<C::TransformComponent>(creature).position = Luminumbra::Vec3(0.0f);
    registry.emplace<C::InstinctAgentComponent>(creature).actor_id = "clock-observer";
    registry.emplace<C::NeedsComponent>(creature).needs = {{"day", 0.0f, 0.0f},
                                                           {"year", 0.0f, 0.0f}};
    registry.emplace<C::StimulusSubscriptionComponent>(creature).subscriptions = {
        {luminumbra::ai::StimulusChannel::TimeOfDay, "day", 0.001f},
        {luminumbra::ai::StimulusChannel::Season, "year", 0.001f}};
    return creature;
}

TEST(GameSessionHeadlessWorldTest, ActiveClockSaveLoadContinuationMatchesUninterruptedWorldHash) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, control, loaded, absent_energy, wrong_clock;
    for (auto* session : {&original, &control, &loaded, &absent_energy, &wrong_clock}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetAetherStateEnabled(true);
    }
    ASSERT_TRUE(original.CreateWorld("Clock saved", "1337", "default"));
    ASSERT_TRUE(control.CreateTransientWorld("Clock control", "1337", "default"));
    const auto save = original.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    const Luminumbra::world::WorldCalendar calendar{120, 4};
    Luminumbra::world::WorldClock(0, calendar).write_metadata(metadata);
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), save));
    ASSERT_TRUE(original.LoadWorldStateFrom(save));
    ASSERT_TRUE(control.LoadWorldStateFrom(save));
    for (auto* session : {&original, &control}) {
        ClockTestPlant(session->GetRegistry(), session->GetMetadata().spawnPoint);
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
        chunk->set_state(Luminumbra::ChunkState::Idle);
        chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(session->GetWorldSystem()->adopt_streamed_chunk(chunk));
    }
    constexpr int n = 317, k = 73;
    const double dt = original.GetSimulationClock().fixed_dt();
    for (int i = 0; i < n; ++i) {
        if (i == n - 1)
            for (auto* session : {&original, &control})
                ASSERT_TRUE(
                    session->GetEnergyFieldState()->QueueRegionalDeposit(1, 0, 0, 0, 60000));
        ASSERT_EQ(original.TickSimulation(dt), 1u);
        ASSERT_EQ(control.TickSimulation(dt), 1u);
    }
    ASSERT_FALSE(original.GetWeatherSystem()->StormCells().empty());
    ASSERT_FALSE(original.ComputeAetherStateSubHash().empty());
    const auto before_save = SessionWorldHash(original);
    ASSERT_TRUE(original.SaveWorldState());
    EXPECT_EQ(SessionWorldHash(original), before_save);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetSimulationTickCount(), n);
    EXPECT_EQ(SessionWorldHash(loaded), before_save);
    EXPECT_EQ(loaded.GetWeatherSystem()->ComputeWeatherSubHash(),
              original.GetWeatherSystem()->ComputeWeatherSubHash());
    const auto& saved_storms = original.GetWeatherSystem()->StormCells();
    const auto& loaded_storms = loaded.GetWeatherSystem()->StormCells();
    ASSERT_EQ(saved_storms.size(), loaded_storms.size());
    for (std::size_t i = 0; i < saved_storms.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(saved_storms[i].spawn_tick, loaded_storms[i].spawn_tick);
        EXPECT_EQ(saved_storms[i].center_world, loaded_storms[i].center_world);
        EXPECT_EQ(saved_storms[i].velocity, loaded_storms[i].velocity);
        EXPECT_EQ(saved_storms[i].intensity, loaded_storms[i].intensity);
    }
    const auto missing_save = root.path() / "missing-energy";
    const auto wrong_save = root.path() / "wrong-clock";
    fs::copy(save, missing_save, fs::copy_options::recursive);
    fs::copy(save, wrong_save, fs::copy_options::recursive);
    ASSERT_TRUE(P::WorldSaveService::remove_member(missing_save, "aether_state.efs"));
    metadata = nlohmann::json::parse(ReadClockTestFile(wrong_save / "world_info.json"));
    Luminumbra::world::WorldClock(n + 1, calendar).write_metadata(metadata);
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), wrong_save));
    ASSERT_TRUE(absent_energy.CreateTransientWorld("Missing energy", "1337", "default"));
    ASSERT_TRUE(wrong_clock.CreateTransientWorld("Wrong clock", "1337", "default"));
    ASSERT_TRUE(absent_energy.LoadWorldStateFrom(missing_save));
    ASSERT_TRUE(wrong_clock.LoadWorldStateFrom(wrong_save));
    EXPECT_NE(SessionWorldHash(absent_energy), before_save);
    EXPECT_NE(SessionWorldHash(wrong_clock), before_save);

    // This slice persists plants and energy, not creature records. Attach the same
    // observing creature to each independently advanced world at the checkpoint.
    std::vector<entt::entity> creatures;
    const std::vector<GameSession*> sessions{
        &original, &control, &loaded, &absent_energy, &wrong_clock};
    for (auto* session : sessions)
        creatures.push_back(ClockTestCreature(session->GetRegistry()));
    for (int i = 0; i < k; ++i) {
        for (auto* session : sessions)
            ASSERT_EQ(session->TickSimulation(dt), 1u);
        const auto& reference = control.GetRegistry();
        for (std::size_t j = 0; j < 3; ++j) {
            const auto& registry = sessions[j]->GetRegistry();
            EXPECT_FLOAT_EQ(registry.get<C::CircadianComponent>(creatures[j]).activity,
                            reference.get<C::CircadianComponent>(creatures[1]).activity);
            const auto& actual = registry.get<C::MigratoryComponent>(creatures[j]);
            const auto& expected = reference.get<C::MigratoryComponent>(creatures[1]);
            EXPECT_FLOAT_EQ(actual.drive, expected.drive);
            EXPECT_FLOAT_EQ(actual.wish_x, expected.wish_x);
            EXPECT_FLOAT_EQ(actual.wish_z, expected.wish_z);
            const auto& needs = registry.get<C::NeedsComponent>(creatures[j]).needs;
            const auto& expected_needs = reference.get<C::NeedsComponent>(creatures[1]).needs;
            for (std::size_t need = 0; need < needs.size(); ++need)
                EXPECT_FLOAT_EQ(needs[need].pressure, expected_needs[need].pressure);
        }
    }
    EXPECT_EQ(loaded.GetSimulationTickCount(), n + k);
    EXPECT_EQ(loaded.GetWorldClock().calendar(), calendar);
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(control));
    EXPECT_EQ(SessionWorldHash(original), SessionWorldHash(control));
    EXPECT_NE(SessionWorldHash(absent_energy), SessionWorldHash(control));
    EXPECT_NE(SessionWorldHash(wrong_clock), SessionWorldHash(control));
    EXPECT_NE(wrong_clock.GetRegistry().get<C::CircadianComponent>(creatures[4]).activity,
              control.GetRegistry().get<C::CircadianComponent>(creatures[1]).activity);
    EXPECT_NE(wrong_clock.GetRegistry().get<C::NeedsComponent>(creatures[4]).needs[0].pressure,
              control.GetRegistry().get<C::NeedsComponent>(creatures[1]).needs[0].pressure);
}

void CheckCommittedSnapshotReloadAndSave(const HeadlessRoot& root,
                                         JobSystem& jobs,
                                         const std::string& world_id,
                                         const fs::path& save,
                                         std::uint64_t expected_tick) {
    // answers.md:31 and design.md:60-61 require recovery at world open, before reading members.
    if (P::journal_pending(save)) {
        GameSession recovery;
        recovery.SetRootPath(root.root_string());
        recovery.SetJobSystem(&jobs);
        recovery.SetActiveRegionsEnabled(true);
        ASSERT_TRUE(recovery.LoadWorld(world_id)) << recovery.GetWorldOpenError();
        EXPECT_EQ(recovery.GetSimulationTickCount(), expected_tick);
        EXPECT_FALSE(P::journal_pending(save));
    }
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    const auto metadata_bytes = ReadClockTestFile(save / "world_info.json");
    const auto ledger_path = P::WorldSaveService::active_regions_path(save);
    const auto ledger_bytes = ReadClockTestFile(ledger_path);
    Luminumbra::world::ActiveRegionLedger durable;
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(durable, save));
    const auto tick =
        nlohmann::json::parse(metadata_bytes).at("simulationTick").get<std::uint64_t>();
    ASSERT_EQ(tick, expected_tick);
    ASSERT_EQ(durable.tick(), tick);
    for (const bool edit : {false, true}) {
        SCOPED_TRACE(edit);
        // Each fresh session must load the previous whole commit, including the edit case.
        {
            P::SaveTransaction staged(save, tick, {}, &P::WorldSaveService::member_digest);
            ASSERT_TRUE(staged.begin(nullptr));
            ASSERT_TRUE(
                P::WorldSaveService::write_member_bytes(save, "world_info.json", metadata_bytes));
            ASSERT_TRUE(P::WorldSaveService::write_member_bytes(
                save, "chunks/region/active-regions.arl", ledger_bytes));
            ASSERT_TRUE(staged.commit(nullptr));
        }
        GameSession loaded, reloaded;
        for (auto* session : {&loaded, &reloaded}) {
            session->SetRootPath(root.root_string());
            session->SetJobSystem(&jobs);
            session->SetActiveRegionsEnabled(true);
        }
        ASSERT_TRUE(loaded.LoadWorld(world_id)) << loaded.GetWorldOpenError();
        EXPECT_EQ(loaded.GetSimulationTickCount(), tick);
        // Loading the recovered commit preserves its causal clock and canonical ledger.
        EXPECT_EQ(loaded.GetActiveRegionLedger().tick(), durable.tick());
        EXPECT_EQ(loaded.GetActiveRegionLedger().header_tick(), tick);
        EXPECT_EQ(loaded.GetActiveRegionLedger().canonical_bytes(), durable.canonical_bytes());
        EXPECT_EQ(loaded.GetActiveRegionLedger().records(), durable.records());
        EXPECT_EQ(loaded.GetActiveRegionLedger().config(), durable.config());
        EXPECT_EQ(loaded.GetActiveRegionLedger().local_anchor(), durable.local_anchor());
        EXPECT_EQ(loaded.GetRegionSchedule().tick, 0u);
        if (edit) {
            loaded.NotifyGroundObjectEdit(Luminumbra::Vec3(5200, 0, -1500));
            EXPECT_EQ(loaded.GetActiveRegionLedger().records().at({10, -3}).last_edit, tick);
            EXPECT_TRUE(loaded.GetActiveRegionLedger().records().at({10, -3}).wake_pending);
        }
        ASSERT_TRUE(loaded.SaveWorld());
        ASSERT_TRUE(loaded.SaveWorldState());
        EXPECT_EQ(loaded.GetSimulationTickCount(), tick);
        EXPECT_EQ(loaded.GetRegionSchedule().tick, 0u);
        // Only observational save stamps (and the explicit edit) may change.
        auto saved_records = loaded.GetActiveRegionLedger().records();
        if (edit)
            saved_records.erase({10, -3});
        auto expected_records = durable.records();
        for (auto& [key, record] : expected_records) {
            (void)key;
            record.last_save = tick;
        }
        EXPECT_EQ(saved_records, expected_records);
        ASSERT_TRUE(P::WorldSaveService::validate_save(save));
        ASSERT_TRUE(reloaded.LoadWorld(world_id)) << reloaded.GetWorldOpenError();
        EXPECT_EQ(reloaded.GetSimulationTickCount(), tick);
        EXPECT_EQ(reloaded.GetActiveRegionLedger().encode(),
                  loaded.GetActiveRegionLedger().encode());
        EXPECT_EQ(SessionWorldHash(reloaded), SessionWorldHash(loaded));
    }
}

TEST(GameSessionHeadlessWorldTest,
     ActiveClockWritesMetadataWithoutDirtyChunksAndExplicitLoadsRestoreIt) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Empty clock", "1337", "default"));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    const auto alternate = root.path() / "explicit-snapshot";
    ASSERT_TRUE(session.SaveWorldStateTo(alternate));
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(alternate / "world_info.json"));
    EXPECT_EQ(metadata.at("presetFormat"), 1);
    EXPECT_EQ(metadata.at("presetIdentity"),
              P::StableChecksum(
                  nlohmann::json::parse(ReadClockTestFile(alternate / "preset.json")).dump()));
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(alternate / "preset.json")),
              nlohmann::json::parse(ReadClockTestFile(session.GetWorldSaveDir() / "preset.json")));
    EXPECT_EQ(metadata.at("simulationTick"), 1u);
    EXPECT_TRUE(fs::exists(P::WorldSaveService::active_regions_path(alternate)));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    Luminumbra::world::ActiveRegionLedger saved_ledger;
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(saved_ledger, save));
    ASSERT_EQ(saved_ledger.tick(), 2u);
    ASSERT_EQ(P::InspectSavedWorld(root.path(), session.GetMetadata().worldId).clock.tick(), 2u);
    ASSERT_TRUE(session.LoadWorldStateFrom(alternate));
    EXPECT_EQ(session.GetSimulationTickCount(), 1u);
    EXPECT_EQ(session.GetWorldClock().calendar(), Luminumbra::world::WorldCalendar{});
    const auto restored_ledger = session.GetActiveRegionLedger().canonical_bytes();
    const auto newer_metadata = ReadClockTestFile(save / "world_info.json");
    const auto newer_ledger = ReadClockTestFile(P::WorldSaveService::active_regions_path(save));
    auto invalid_metadata = metadata;
    invalid_metadata["simulationTick"] = 3u;
    auto incoming_ledger = session.GetActiveRegionLedger();
    EXPECT_FALSE(P::WorldSaveService::save_metadata_and_active_regions(
        invalid_metadata.dump(), incoming_ledger, save));
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), newer_metadata);
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::active_regions_path(save)), newer_ledger);
    ASSERT_TRUE(session.SaveWorld());
    const auto inspected = P::InspectSavedWorld(root.path(), session.GetMetadata().worldId);
    EXPECT_TRUE(inspected.error.empty());
    EXPECT_EQ(inspected.clock.tick(), 1u);
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(saved_ledger, save));
    EXPECT_EQ(saved_ledger.tick(), 1u);
    EXPECT_EQ(saved_ledger.canonical_bytes(), restored_ledger);
    const auto restored_hash = SessionWorldHash(session);
    ASSERT_TRUE(session.LoadWorld(session.GetMetadata().worldId)) << session.GetWorldOpenError();
    EXPECT_EQ(session.GetSimulationTickCount(), 1u);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), restored_ledger);
    EXPECT_EQ(SessionWorldHash(session), restored_hash);
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_TRUE(P::WorldSaveService::validate_save(save));

    // design.md:60-61: a failure between rewind replacements restores the whole previous commit.
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(session.SaveWorld());
    const auto before_rewind_metadata = ReadClockTestFile(save / "world_info.json");
    const auto before_rewind_ledger =
        ReadClockTestFile(P::WorldSaveService::active_regions_path(save));
    const auto before_rewind_index = ReadClockTestFile(P::snapshot_index_path(save));
    ASSERT_TRUE(session.LoadWorldStateFrom(alternate));
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool interrupted_save = session.SaveWorld();
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(interrupted_save);
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    EXPECT_EQ(P::InspectSavedWorld(root.path(), session.GetMetadata().worldId).clock.tick(), 2u);
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(saved_ledger, save));
    EXPECT_EQ(saved_ledger.tick(), 2u);
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), before_rewind_metadata);
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::active_regions_path(save)),
              before_rewind_ledger);
    EXPECT_EQ(ReadClockTestFile(P::snapshot_index_path(save)), before_rewind_index);
    ASSERT_NO_FATAL_FAILURE(
        CheckCommittedSnapshotReloadAndSave(root, jobs, session.GetMetadata().worldId, save, 2));
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(session.LoadWorld(session.GetMetadata().worldId)) << session.GetWorldOpenError();
    EXPECT_EQ(session.GetSimulationTickCount(), 1u);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), restored_ledger);
    EXPECT_EQ(SessionWorldHash(session), restored_hash);
}

TEST(GameSessionHeadlessWorldTest, FailedForwardLedgerWriteReloadsAndSavesImmediately) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Forward failure", "1337", "default"));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    const auto before_metadata = ReadClockTestFile(save / "world_info.json");
    const auto before_ledger = ReadClockTestFile(P::WorldSaveService::active_regions_path(save));
    const auto before_index = ReadClockTestFile(P::snapshot_index_path(save));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    P::WorldSaveService::set_before_active_regions_replace_for_testing([] { return false; });
    const bool saved = session.SaveWorld();
    P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    ASSERT_FALSE(saved);
    // design.md:60-61: failed forward writes also restore the whole previous commit.
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), before_metadata);
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::active_regions_path(save)), before_ledger);
    EXPECT_EQ(ReadClockTestFile(P::snapshot_index_path(save)), before_index);
    ASSERT_NO_FATAL_FAILURE(
        CheckCommittedSnapshotReloadAndSave(root, jobs, session.GetMetadata().worldId, save, 1));
}

#if GTEST_HAS_DEATH_TEST && !defined(_WIN32)
TEST(GameSessionHeadlessWorldDeathTest, TerminatedLedgerStagingLeavesLoadableAndSavableWorld) {
    // Fork before starting any workers, so the child owns every job it must drain.
    // Fast death-test mode also preserves this unique directory in the child.
    GTEST_FLAG_SET(death_test_style, "fast");
    const HeadlessRoot root;
    ASSERT_EXIT(
        {
            JobSystem jobs;
            jobs.startup(1);
            GameSession session;
            session.SetRootPath(root.root_string());
            session.SetJobSystem(&jobs);
            session.SetActiveRegionsEnabled(true);
            if (!session.CreateWorld("Ledger crash", "1337", "default"))
                std::_Exit(1);
            session.TickSimulation(session.GetSimulationClock().fixed_dt());
            if (!session.SaveWorld())
                std::_Exit(2);
            session.TickSimulation(session.GetSimulationClock().fixed_dt());
            P::WorldSaveService::set_before_active_regions_replace_for_testing([]() -> bool {
                std::_Exit(73); // no destructors, unwinding or temporary-file cleanup
            });
            session.SaveWorld();
            std::_Exit(3);
        },
        testing::ExitedWithCode(73),
        "");
    const auto save = fs::directory_iterator(root.path() / "worlds/saves")->path();
    std::vector<fs::path> debris;
    for (const auto& entry : fs::recursive_directory_iterator(save)) {
        if (entry.path().filename().string().find("active-regions.arl.tmp.") == 0)
            debris.push_back(entry.path());
    }
    ASSERT_EQ(debris.size(), 1u);
    EXPECT_EQ(debris.front().parent_path(), save);
    Luminumbra::world::ActiveRegionLedger staged;
    std::string error;
    ASSERT_TRUE(Luminumbra::world::ActiveRegionLedger::decode(
        ReadClockTestFile(debris.front()), staged, error))
        << error;
    EXPECT_EQ(staged.tick(), 2u);
    // design.md:60-61: installed members may be incomplete until world-open recovery runs.
    EXPECT_TRUE(P::journal_pending(save));
    EXPECT_FALSE(P::WorldSaveService::validate_save(save));
    JobSystem jobs;
    jobs.startup(1);
    ASSERT_NO_FATAL_FAILURE(
        CheckCommittedSnapshotReloadAndSave(root, jobs, save.filename().string(), save, 1));
    // Recovery never depends on cleaning the staging file up, but the next
    // successful save must reclaim it. Otherwise every interruption leaks a full
    // ledger copy and repeated crashes exhaust the volume.
    std::size_t remaining = 0;
    for (const auto& entry : fs::recursive_directory_iterator(save))
        if (entry.path().filename().string().find("active-regions.arl.tmp.") == 0)
            ++remaining;
    EXPECT_EQ(remaining, 0u);
    EXPECT_FALSE(fs::exists(debris.front()));
}
#endif

void CheckLegacyWeatherAnchorReconstruction(GameSession& correct_anchor,
                                            GameSession& wrong_anchor,
                                            const Luminumbra::Vec3& anchor,
                                            const Luminumbra::Vec3& spawn) {
    auto& correct_weather = *correct_anchor.GetWeatherSystem();
    auto& wrong_weather = *wrong_anchor.GetWeatherSystem();
    // C4 hashes persisted pages. This separate C1 control uses legacy grids for both sides.
    correct_weather.BindRegionalBackend({}, {});
    wrong_weather.BindRegionalBackend({}, {});
    correct_weather.RestoreAtTick(
        correct_anchor.GetWorldClock().tick(), anchor, *correct_anchor.GetWindFieldSystem());
    wrong_weather.RestoreAtTick(
        wrong_anchor.GetWorldClock().tick(), anchor, *wrong_anchor.GetWindFieldSystem());
    EXPECT_EQ(wrong_weather.ComputeWeatherSubHash(), correct_weather.ComputeWeatherSubHash());
    // Change only the reconstruction anchor; seed, tick, backend and hash format stay equal.
    wrong_weather.RestoreAtTick(
        wrong_anchor.GetWorldClock().tick(), spawn, *wrong_anchor.GetWindFieldSystem());
    EXPECT_NE(wrong_weather.ComputeWeatherSubHash(), correct_weather.ComputeWeatherSubHash());
}

TEST(GameSessionHeadlessWorldTest, ActiveClockKeepsAmbientAnchorWhenClientSavesMovedSpawn) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, control, loaded, explicit_load, correct_anchor, wrong_anchor,
        wrong_regional_anchor;
    for (auto* session : {&original,
                          &control,
                          &loaded,
                          &explicit_load,
                          &correct_anchor,
                          &wrong_anchor,
                          &wrong_regional_anchor}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(original.CreateWorld("Moving spawn", "1337", "default"));
    ASSERT_TRUE(control.CreateTransientWorld("Control", "1337", "default"));
    const auto anchor = original.GetMetadata().spawnPoint;
    for (auto* session : {&original, &control})
        ClockTestPlant(session->GetRegistry(), anchor);
    const double dt = original.GetSimulationClock().fixed_dt();
    constexpr int n = 317, k = 73;
    for (int i = 0; i < n; ++i) {
        ASSERT_EQ(original.TickSimulation(dt), 1u);
        ASSERT_EQ(control.TickSimulation(dt), 1u);
    }
    ASSERT_FALSE(original.GetWeatherSystem()->StormCells().empty());
    const auto before = SessionWorldHash(original);
    const auto spawn = anchor + Luminumbra::Vec3(2400.0f, 10.0f, -1200.0f);
    // The production quit/shutdown path changes the respawn position before saving.
    original.SetSpawnPoint(spawn);
    control.SetSpawnPoint(spawn);
    ASSERT_TRUE(original.SaveWorldState());
    ASSERT_TRUE(original.SaveWorld());
    EXPECT_EQ(SessionWorldHash(original), before);
    const auto save = original.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    EXPECT_EQ(metadata.at("spawnPoint").at("x"), spawn.x);
    EXPECT_EQ(metadata.at("ambientFieldAnchor").at("x"), anchor.x);
    EXPECT_EQ(metadata.at("ambientFieldAnchor").at("y"), anchor.y);
    EXPECT_EQ(metadata.at("ambientFieldAnchor").at("z"), anchor.z);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetMetadata().spawnPoint, spawn);
    const auto explicit_save = root.path() / "moved-spawn-snapshot";
    ASSERT_TRUE(original.SaveWorldStateTo(explicit_save));
    ASSERT_TRUE(explicit_load.CreateTransientWorld("Explicit", "1337", "default"));
    ASSERT_TRUE(explicit_load.LoadWorldStateFrom(explicit_save));
    ASSERT_TRUE(correct_anchor.CreateTransientWorld("Correct anchor", "1337", "default"));
    ASSERT_TRUE(correct_anchor.LoadWorldStateFrom(explicit_save));
    // Model the bug: using the new respawn position for historical reconstruction.
    metadata["ambientFieldAnchor"] = metadata.at("spawnPoint");
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), explicit_save));
    ASSERT_TRUE(wrong_anchor.CreateTransientWorld("Wrong anchor", "1337", "default"));
    ASSERT_TRUE(wrong_anchor.LoadWorldStateFrom(explicit_save));
    ASSERT_TRUE(
        wrong_regional_anchor.CreateTransientWorld("Wrong regional anchor", "1337", "default"));
    ASSERT_TRUE(wrong_regional_anchor.LoadWorldStateFrom(explicit_save));
    // Regional weather restores the same world-space authority at either anchor.
    // The negative control must still detect the different ambient projection domain.
    ASSERT_EQ(wrong_anchor.GetFieldPages().canonical_bytes(),
              control.GetFieldPages().canonical_bytes());
    ASSERT_EQ(wrong_regional_anchor.GetFieldPages().canonical_bytes(),
              control.GetFieldPages().canonical_bytes());
    for (int i = 0; i <= k; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(SessionWorldHash(original), SessionWorldHash(control));
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(control));
        for (auto* session : {&loaded, &explicit_load}) {
            EXPECT_EQ(session->GetWindFieldSystem()->ComputeWindSubHash(),
                      control.GetWindFieldSystem()->ComputeWindSubHash());
            EXPECT_EQ(session->GetWeatherSystem()->ComputeWeatherSubHash(),
                      control.GetWeatherSystem()->ComputeWeatherSubHash());
            EXPECT_EQ(session->GetAetherFieldSystem()->ComputeAetherSubHash(),
                      control.GetAetherFieldSystem()->ComputeAetherSubHash());
            const auto& storms = session->GetWeatherSystem()->StormCells();
            const auto& expected = control.GetWeatherSystem()->StormCells();
            ASSERT_EQ(storms.size(), expected.size());
            for (std::size_t j = 0; j < storms.size(); ++j) {
                EXPECT_EQ(storms[j].spawn_tick, expected[j].spawn_tick);
                EXPECT_EQ(storms[j].center_world, expected[j].center_world);
                EXPECT_EQ(storms[j].velocity, expected[j].velocity);
                EXPECT_EQ(storms[j].intensity, expected[j].intensity);
            }
        }
        // Keep the regional negative control bound while the separate legacy control
        // disables its backend. Identical pages must still hash differently by anchor.
        EXPECT_NE(wrong_regional_anchor.GetWeatherSystem()->ComputeWeatherSubHash(),
                  control.GetWeatherSystem()->ComputeWeatherSubHash());
        ASSERT_NO_FATAL_FAILURE(
            CheckLegacyWeatherAnchorReconstruction(correct_anchor, wrong_anchor, anchor, spawn));
        if (i < k) {
            for (auto* session : {&original,
                                  &control,
                                  &loaded,
                                  &explicit_load,
                                  &correct_anchor,
                                  &wrong_anchor,
                                  &wrong_regional_anchor})
                ASSERT_EQ(session->TickSimulation(dt), 1u);
        }
    }
}

TEST(GameSessionHeadlessWorldTest, ClockAnchorMetadataUsesLegacySpawnAndRejectsInvalidAnchors) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Anchor metadata", "1337", "default"));
    const auto save = session.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    metadata.erase("ambientFieldAnchor");
    metadata["spawnPoint"] = {{"x", 2400.0f}, {"y", 100.0f}, {"z", -1200.0f}};
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), save));
    ASSERT_TRUE(session.LoadWorldStateFrom(save));
    ASSERT_TRUE(session.SaveWorld());
    metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    EXPECT_EQ(metadata.at("ambientFieldAnchor"),
              (nlohmann::json{{"x", 2400.0f}, {"y", 100.0f}, {"z", -1200.0f}}));
    const auto valid_bytes = ReadClockTestFile(save / "world_info.json");
    const std::vector<nlohmann::json> invalid{nullptr,
                                              true,
                                              "anchor",
                                              {{"x", 0.0f}},
                                              {{"x", 1e100}, {"y", 0.0f}, {"z", 0.0f}},
                                              {{"x", false}, {"y", 0.0f}, {"z", 0.0f}}};
    for (const auto& value : invalid) {
        SCOPED_TRACE(value.dump());
        metadata["ambientFieldAnchor"] = value;
        EXPECT_FALSE(P::WorldSaveService::save_metadata(metadata.dump(), save));
        EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), valid_bytes);
        // Binary mode: the reader compares exact bytes, and Windows text mode would
        // translate newlines and make the comparison fail for the wrong reason.
        std::ofstream(save / "world_info.json", std::ios::binary) << metadata.dump();
        EXPECT_FALSE(P::WorldSaveService::validate_save(save));
        EXPECT_FALSE(
            P::InspectSavedWorld(root.path(), session.GetMetadata().worldId).error.empty());
        Luminumbra::world::WorldClock clock;
        bool required = false;
        EXPECT_FALSE(P::WorldSaveService::read_clock_metadata(save, clock, required));
        std::ofstream(save / "world_info.json", std::ios::binary) << valid_bytes;
    }
}

TEST(GameSessionHeadlessWorldTest,
     RegionalEmitterGatherRefusalDoesNotLatchFaultOrPoisonLaterTicks) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetAetherStateEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Bounded energy emitters", "1337", "default"));
    auto& registry = session.GetRegistry();
    const auto add_emitter =
        [&registry](Luminumbra::Vec3 position, int radius, std::uint32_t rate) {
            const auto entity = registry.create();
            registry.emplace<C::TransformComponent>(entity).position = position;
            auto& emitter = registry.emplace<C::FieldEmitterComponent>(entity);
            emitter.radius_cells = radius;
            emitter.rate_raw_per_tick = rate;
            return entity;
        };
    add_emitter({std::numeric_limits<float>::quiet_NaN(), 0, 0}, 0, 123);
    add_emitter({0, 0, 0}, std::numeric_limits<int>::max(), 123);
    add_emitter({0, 0, 0}, 0, 123);
    using Energy = luminumbra::fields::EnergyFieldState;
    std::vector<entt::entity> oversized;
    for (std::size_t i = 0; i <= Energy::kMaxRegionalDeposits / (63u * 63u); ++i)
        oversized.push_back(add_emitter({0, 0, 0}, 31, std::numeric_limits<std::uint32_t>::max()));
    auto* energy = session.GetEnergyFieldState();
    ASSERT_NE(energy, nullptr);
    ASSERT_TRUE(energy->QueueRegionalDeposit(0, 0, 0, 0, 17));
    const auto dt = session.GetSimulationClock().fixed_dt();
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    EXPECT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(energy->total_raw(), 17u); // only previously admitted input survives
    for (auto entity : oversized)
        registry.destroy(entity);
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    EXPECT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(energy->total_raw(), 140u); // invalid emitters skipped; valid gather recovers
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    EXPECT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(energy->total_raw(), 263u);
}

TEST(GameSessionHeadlessWorldTest, EmptyEnergyResumeAlignsFirstDepositWithoutReplayingHistory) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession control, loaded, explicit_load;
    for (auto* session : {&control, &loaded, &explicit_load}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetAetherStateEnabled(true);
    }
    ASSERT_TRUE(control.CreateWorld("Empty energy", "1337", "default"));
    const double dt = control.GetSimulationClock().fixed_dt();
    for (int i = 0; i < 317; ++i)
        ASSERT_EQ(control.TickSimulation(dt), 1u);
    ASSERT_TRUE(control.SaveWorldState());
    const auto save = control.GetWorldSaveDir();
    ASSERT_FALSE(fs::exists(save / "aether_state.efs"));
    ASSERT_TRUE(loaded.LoadWorld(control.GetMetadata().worldId));
    ASSERT_TRUE(explicit_load.CreateTransientWorld("Explicit", "1337", "default"));
    // An absent record must also clear a reused loader's pages and pending input.
    auto* previous = explicit_load.GetEnergyFieldState();
    ASSERT_TRUE(previous->QueueRegionalDeposit(1, 0, 0, 0, 40000));
    ASSERT_EQ(explicit_load.TickSimulation(dt), 1u);
    ASSERT_TRUE(previous->QueueRegionalDeposit(2, 0, 0, 0, 1000));
    ASSERT_TRUE(explicit_load.LoadWorldStateFrom(save));
    const auto saved_ages = control.GetActiveRegionLedger().energy_ages();
    ASSERT_EQ(saved_ages.at({0, 0}), 317u);
    for (auto* session : {&control, &loaded, &explicit_load}) {
        auto* energy = session->GetEnergyFieldState();
        ASSERT_EQ(energy->page_count(), 0u);
        EXPECT_EQ(session->GetActiveRegionLedger().energy_ages(), saved_ages);
        ASSERT_TRUE(energy->QueueRegionalDeposit(1, 0, 0, 0, 60000));
        ASSERT_EQ(session->TickSimulation(dt), 1u);
        EXPECT_EQ(energy->at_cell(0, 0), 60000u);
        EXPECT_EQ(session->GetActiveRegionLedger().energy_ages().at({0, 0}), 318u);
    }
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(control));
        EXPECT_EQ(SessionWorldHash(explicit_load), SessionWorldHash(control));
        for (auto* session : {&control, &loaded, &explicit_load})
            ASSERT_EQ(session->TickSimulation(dt), 1u);
    }
}

TEST(GameSessionHeadlessWorldTest, EmptyLegacyEnergySnapshotPreservesNonAlignedCadence) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession legacy, loaded, explicit_load;
    for (auto* session : {&legacy, &loaded, &explicit_load}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetAetherStateEnabled(true);
        session->SetActiveRegionsEnabled(false);
    }
    ASSERT_TRUE(legacy.CreateWorld("Legacy energy", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(legacy.GetWorldSystem()->adopt_streamed_chunk(chunk));
    const double dt = legacy.GetSimulationClock().fixed_dt();
    for (int i = 0; i < 5; ++i)
        ASSERT_EQ(legacy.TickSimulation(dt), 1u);
    legacy.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 1);
    legacy.GetEnergyFieldState()->Tick(5);
    ASSERT_TRUE(legacy.SaveWorldState());
    const auto save = legacy.GetWorldSaveDir();
    ASSERT_FALSE(nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"))
                     .contains("simulationTick"));
    const auto bytes = ReadClockTestTextRecord(save / "aether_state.efs");
    ASSERT_EQ(bytes.substr(0, 9), "EFS1 2 3\n");
    ASSERT_TRUE(loaded.LoadWorld(legacy.GetMetadata().worldId));
    ASSERT_TRUE(explicit_load.CreateTransientWorld("Explicit legacy", "1337", "default"));
    ASSERT_TRUE(explicit_load.LoadWorldStateFrom(save));
    for (auto* session : {&loaded, &explicit_load}) {
        EXPECT_EQ(session->GetSimulationTickCount(), 0u);
        EXPECT_EQ(session->GetEnergyFieldState()->next_fire_tick(), 3u);
        EXPECT_EQ(session->GetEnergyFieldState()->total_raw(), 1u);
        session->GetEnergyFieldState()->QueueDeposit(2, 0, 0, 0, 60000);
    }
    for (int tick = 1; tick <= 8; ++tick) {
        for (auto* session : {&loaded, &explicit_load}) {
            ASSERT_EQ(session->TickSimulation(dt), 1u);
            if (tick < 3)
                EXPECT_EQ(session->GetEnergyFieldState()->at_cell(0, 0), 60001u);
            else
                EXPECT_LT(session->GetEnergyFieldState()->at_cell(0, 0), 60001u);
        }
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(explicit_load));
    }
    // The legacy writer leaves an empty layer's existing phase-bearing file byte-identical.
    ASSERT_EQ(loaded.GetEnergyFieldState()->DeserializeRecord("EFS1 2 4\n",
                                                              loaded.GetSimulationTickCount()),
              true);
    ASSERT_TRUE(loaded.SaveWorldState());
    EXPECT_EQ(ReadClockTestTextRecord(save / "aether_state.efs"), bytes);
}

TEST(GameSessionHeadlessWorldTest, EnabledLoadingRefusesPhaseBearingLegacyEnergyWithoutMigration) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession legacy, enabled;
    for (auto* session : {&legacy, &enabled}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetAetherStateEnabled(true);
    }
    enabled.SetActiveRegionsEnabled(true);
    legacy.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(legacy.CreateWorld("Legacy refusal", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(legacy.GetWorldSystem()->adopt_streamed_chunk(chunk));
    legacy.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 1);
    ASSERT_EQ(legacy.TickSimulation(legacy.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(legacy.SaveWorldState());
    const auto path = legacy.GetWorldSaveDir() / "aether_state.efs";
    const auto before = ReadClockTestFile(path);
    EXPECT_FALSE(enabled.LoadWorld(legacy.GetMetadata().worldId));
    EXPECT_FALSE(enabled.GetWorldOpenError().empty());
    EXPECT_EQ(ReadClockTestFile(path), before);
}

TEST(GameSessionHeadlessWorldTest, EmptyAlignedEnergySaveRemovesStaleRecord) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, loaded;
    for (auto* session : {&original, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetAetherStateEnabled(true);
    }
    ASSERT_TRUE(original.CreateWorld("Aligned energy", "1337", "default"));
    const double dt = original.GetSimulationClock().fixed_dt();
    ASSERT_TRUE(original.GetEnergyFieldState()->QueueRegionalDeposit(1, 0, 0, 0, 1));
    ASSERT_EQ(original.TickSimulation(dt), 1u);
    ASSERT_TRUE(original.SaveWorldState());
    ASSERT_TRUE(fs::exists(original.GetWorldSaveDir() / "aether_state.efs"));
    for (int i = 1; i < 8; ++i)
        ASSERT_EQ(original.TickSimulation(dt), 1u);
    ASSERT_EQ(original.GetEnergyFieldState()->page_count(), 0u);
    ASSERT_TRUE(original.SaveWorldState());
    EXPECT_FALSE(fs::exists(original.GetWorldSaveDir() / "aether_state.efs"));
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetEnergyFieldState()->total_raw(), 0u);
    EXPECT_EQ(loaded.GetActiveRegionLedger().energy_ages(),
              original.GetActiveRegionLedger().energy_ages());
    ASSERT_EQ(loaded.GetActiveRegionLedger().energy_ages().at({0, 0}), 8u);
    for (auto* session : {&original, &loaded})
        ASSERT_TRUE(session->GetEnergyFieldState()->QueueRegionalDeposit(2, 0, 0, 0, 60000));
    for (int tick = 9; tick <= 16; ++tick) {
        for (auto* session : {&original, &loaded})
            ASSERT_EQ(session->TickSimulation(dt), 1u);
        EXPECT_EQ(loaded.GetActiveRegionLedger().energy_ages(),
                  original.GetActiveRegionLedger().energy_ages());
        EXPECT_EQ(loaded.GetEnergyFieldState()->CanonicalRegionalBytes(),
                  original.GetEnergyFieldState()->CanonicalRegionalBytes());
        EXPECT_EQ(loaded.GetEnergyFieldState()->at_cell(0, 0) == 60000u, tick < 16);
    }
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
}

TEST(GameSessionHeadlessWorldTest, TransientClockSnapshotRefusesBeforeWritingAnyMember) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, transient;
    for (auto* session : {&original, &transient}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetAetherStateEnabled(true);
    }
    ASSERT_TRUE(original.CreateWorld("Snapshot owner", "1337", "default"));
    ClockTestPlant(original.GetRegistry(), original.GetMetadata().spawnPoint);
    ASSERT_TRUE(original.GetEnergyFieldState()->QueueRegionalDeposit(1, 0, 0, 0, 60000));
    ASSERT_EQ(original.TickSimulation(original.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(original.SaveWorldState());
    const auto save = original.GetWorldSaveDir();
    ASSERT_TRUE(transient.CreateTransientWorld("Snapshot reader", "1337", "default"));
    ASSERT_TRUE(transient.LoadWorldStateFrom(save));
    // Make every member writer observable if it runs before the refusal.
    ClockTestPlant(transient.GetRegistry(), transient.GetMetadata().spawnPoint);
    ASSERT_TRUE(transient.GetEnergyFieldState()->QueueRegionalDeposit(2, 0, 0, 0, 1000));
    ASSERT_EQ(transient.TickSimulation(transient.GetSimulationClock().fixed_dt()), 1u);
    const auto snapshot_bytes = [](const fs::path& path) {
        std::map<fs::path, std::string> bytes;
        for (const auto& entry : fs::recursive_directory_iterator(path))
            bytes.emplace(entry.path().lexically_relative(path),
                          entry.is_regular_file() ? ReadClockTestFile(entry.path()) : "");
        return bytes;
    };
    const auto before = snapshot_bytes(save);
    const auto missing = root.path() / "refused-transient-snapshot";
    for (const bool dirty : {false, true}) {
        SCOPED_TRACE(dirty);
        if (dirty) {
            auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
            chunk->set_state(Luminumbra::ChunkState::Idle);
            chunk->mark_voxel_data_dirty();
            ASSERT_TRUE(transient.GetWorldSystem()->adopt_streamed_chunk(chunk));
        }
        EXPECT_FALSE(transient.SaveWorldStateTo(save));
        EXPECT_EQ(snapshot_bytes(save), before);
        EXPECT_FALSE(transient.SaveWorldStateTo(missing));
        EXPECT_FALSE(fs::exists(missing));
        EXPECT_FALSE(transient.SaveWorld());
    }
}

TEST(GameSessionHeadlessWorldTest, EnergyOnlyExplicitSnapshotCreatesDestinationAndRoundTrips) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, loaded;
    for (auto* session : {&original, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetAetherStateEnabled(true);
    }
    ASSERT_TRUE(original.CreateWorld("Energy only", "1337", "default"));
    ASSERT_TRUE(original.GetEnergyFieldState()->QueueRegionalDeposit(1, 0, 0, 0, 60000));
    const double dt = original.GetSimulationClock().fixed_dt();
    ASSERT_EQ(original.TickSimulation(dt), 1u);
    const auto save = root.path() / "energy-only";
    ASSERT_FALSE(fs::exists(save));
    Luminumbra::world::WorldStateSaveReport report;
    ASSERT_TRUE(original.SaveWorldStateTo(save, &report));
    EXPECT_EQ(report.chunks_dirty, 0u);
    EXPECT_TRUE(fs::exists(P::WorldSaveService::active_regions_path(save)));
    EXPECT_TRUE(fs::exists(save / "aether_state.efs"));
    ASSERT_TRUE(loaded.CreateTransientWorld("Energy reader", "1337", "default"));
    ASSERT_TRUE(loaded.LoadWorldStateFrom(save));
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(original.TickSimulation(dt), 1u);
        ASSERT_EQ(loaded.TickSimulation(dt), 1u);
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
    }
}

TEST(GameSessionHeadlessWorldTest, ExplicitlyDisabledCallbacksKeepLegacySavesLoadsAndNestedTicks) {
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
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    session.SetAetherStateEnabled(false);
    session.SetWaterHighResEnabled(false);
    session.SetWeatherRainEnabled(false);
    session.SetWeatherEventsEnabled(false);
    session.SetFiniteHydrologyEnabled(false);

    ASSERT_TRUE(session.CreateWorld("Legacy callbacks", "1337", "default"));
    const auto save = session.GetWorldSaveDir();
    const auto before = ReadClockTestFile(save / "world_info.json");
    const double dt = session.GetSimulationClock().fixed_dt();
    bool called = false;
    session.GetSimulationEventBus().subscribe([&](const auto&) {
        called = true;
        EXPECT_TRUE(session.IsSimulationTickBoundary());
        EXPECT_TRUE(session.SaveWorld());
        EXPECT_TRUE(session.SaveWorldState());
        EXPECT_TRUE(session.SaveWorldStateTo(root.path() / "legacy-explicit"));
        EXPECT_FALSE(fs::exists(root.path() / "legacy-explicit"));
        EXPECT_TRUE(session.LoadWorldState());
        EXPECT_TRUE(session.LoadWorldStateFrom(save));
        EXPECT_EQ(session.TickSimulation(dt), 1u);
        EXPECT_EQ(session.GetSimulationTickCount(), 2u);
        EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), before);
        // Failed world opens historically reached catalog validation even in a
        // callback. A successful open clears the bus, so do not mutate its handlers.
        EXPECT_FALSE(session.LoadWorld("missing-world"));
        EXPECT_FALSE(session.GetWorldOpenError().empty());
    });
    session.GetSimulationEventBus().publish(1, "legacy-callback", "");
    EXPECT_EQ(session.TickSimulation(dt), 1u);
    EXPECT_TRUE(called);
}

TEST(GameSessionHeadlessWorldTest, ClockSnapshotsRefuseInsideCatchUpBatch) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Boundary", "1337", "default"));
    const auto metadata = session.GetWorldSaveDir() / "world_info.json";
    const auto before = ReadClockTestFile(metadata);
    int callbacks = 0;
    session.GetSimulationEventBus().subscribe([&](const auto& event) {
        ++callbacks;
        EXPECT_EQ(session.GetSimulationTickCount(), event.tick);
        EXPECT_FALSE(session.IsSimulationTickBoundary());
        EXPECT_FALSE(session.SaveWorld());
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorldStateTo(root.path() / "reentrant-save"));
        EXPECT_FALSE(session.LoadWorldStateFrom(session.GetWorldSaveDir()));
        EXPECT_FALSE(session.LoadWorld(session.GetMetadata().worldId));
        EXPECT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 0u);
        EXPECT_EQ(ReadClockTestFile(metadata), before);
    });
    session.GetSimulationEventBus().publish(1, "save", "");
    session.GetSimulationEventBus().publish(2, "save", "");
    EXPECT_EQ(session.TickSimulation(2 * session.GetSimulationClock().fixed_dt()), 2u);
    EXPECT_EQ(callbacks, 2);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(metadata)).at("simulationTick"), 2u);
}

TEST(GameSessionHeadlessWorldTest, ExplicitlyDisabledKeepsLegacyMetadataBytesAndTickZeroLoad) {
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
    GameSession session, loaded;
    for (auto* s : {&session, &loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(false);
        s->SetAetherStateEnabled(false);
        s->SetWaterHighResEnabled(false);
        s->SetWeatherRainEnabled(false);
        s->SetWeatherEventsEnabled(false);
        s->SetFiniteHydrologyEnabled(false);
    }
    ASSERT_TRUE(session.CreateWorld("Legacy clock", "1337", "default"));
    const auto path = session.GetWorldSaveDir() / "world_info.json";
    const auto before = ReadClockTestFile(path);
    const auto& m = session.GetMetadata();
    // Clock keys remain absent; the preset marker and identity apply to every new world.
    const nlohmann::json legacy = {
        {"container_version", 2},
        {"presetFormat", 1},
        {"presetIdentity",
         P::StableChecksum(
             nlohmann::json::parse(ReadClockTestFile(session.GetWorldSaveDir() / "preset.json"))
                 .dump())},
        {"name", m.name},
        {"seed", m.seed},
        {"worldType", m.worldType},
        {"creationTime", m.creationTime},
        {"spawnPoint", {{"x", m.spawnPoint.x}, {"y", m.spawnPoint.y}, {"z", m.spawnPoint.z}}},
        {"waterSimCursor", std::size_t{0}}};
    EXPECT_EQ(before, legacy.dump(4) + "\n");
    for (int i = 0; i < 10; ++i)
        session.TickSimulation(session.GetSimulationClock().fixed_dt());
    EXPECT_EQ(session.FoldClockIntoEcologyHash("legacy-ecology"), "legacy-ecology");
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.SaveWorld());
    EXPECT_EQ(ReadClockTestFile(path), before);
    ASSERT_TRUE(loaded.LoadWorld(m.worldId));
    EXPECT_EQ(loaded.GetSimulationTickCount(), 0u);
    EXPECT_EQ(ReadClockTestFile(path), before);
}

TEST(GameSessionHeadlessWorldTest, EnabledClockOverflowRefusesWithoutAdvancingEitherClock) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Clock limit", "1337", "default"));
    const auto save = session.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    const auto last = Luminumbra::world::WorldClock::kTickLimit - 1;
    Luminumbra::world::WorldClock(last).write_metadata(metadata);
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), save));
    ASSERT_TRUE(session.LoadWorldStateFrom(save));
    const auto before = SessionWorldHash(session);
    EXPECT_THROW(session.TickSimulation(session.GetSimulationClock().fixed_dt()),
                 std::overflow_error);
    EXPECT_EQ(session.GetSimulationTickCount(), last);
    EXPECT_EQ(session.GetSimulationClock().tick_count(), last);
    EXPECT_EQ(session.GetSimulationClock().accumulator(), 0.0);
    EXPECT_EQ(SessionWorldHash(session), before);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
}

TEST(GameSessionHeadlessWorldTest, CalendarConsumersUsePinnedPhasesOnTheRealTickPath) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Phases", "1337", "default"));
    const auto id = session.GetMetadata().worldId;
    const auto save = session.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    struct Pin {
        std::uint64_t tick;
        float day, year, daylight, warmth;
    };
    for (const auto pin : {Pin{30, .25f, .0625f, .575f, .125f},
                           Pin{60, .5f, .125f, 1.0f, .25f},
                           Pin{120, 0.0f, .25f, .15f, .5f},
                           Pin{240, 0.0f, .5f, .15f, 1.0f},
                           Pin{360, 0.0f, .75f, .15f, .5f},
                           Pin{480, 0.0f, 0.0f, .15f, 0.0f}}) {
        SCOPED_TRACE(pin.tick);
        Luminumbra::world::WorldClock(pin.tick - 1, {120, 4}).write_metadata(metadata);
        ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), save));
        ASSERT_TRUE(session.LoadWorld(id)) << session.GetWorldOpenError();
        const auto position = session.GetMetadata().spawnPoint;
        const auto plant = ClockTestPlant(session.GetRegistry(), position);
        const auto creature = ClockTestCreature(session.GetRegistry());
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        const auto& clock = session.GetWorldClock();
        EXPECT_FLOAT_EQ(static_cast<float>(clock.day_phase(pin.tick)), pin.day);
        EXPECT_FLOAT_EQ(static_cast<float>(clock.year_phase(pin.tick)), pin.year);
        EXPECT_FLOAT_EQ(session.GetRegistry().get<C::CircadianComponent>(creature).activity,
                        luminumbra::ai::CircadianActivity(pin.day, false));
        const float spring = pin.year >= .25f ? pin.year - .25f : pin.year + .75f;
        const auto target = luminumbra::ai::MigrationTargetAt(spring);
        const auto& migration = session.GetRegistry().get<C::MigratoryComponent>(creature);
        EXPECT_FLOAT_EQ(migration.drive, luminumbra::ai::MigrationDriveAt(spring));
        const auto& creature_position =
            session.GetRegistry().get<C::TransformComponent>(creature).position;
        const float dx = target.x - creature_position.x;
        const float dz = target.z - creature_position.z;
        const float length = Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);
        EXPECT_NEAR(migration.wish_x, dx / length * migration.drive, 1e-6f);
        EXPECT_NEAR(migration.wish_z, dz / length * migration.drive, 1e-6f);
        const auto& needs = session.GetRegistry().get<C::NeedsComponent>(creature).needs;
        EXPECT_FLOAT_EQ(needs[0].pressure,
                        0.001f * luminumbra::ai::CircadianActivity(pin.day, false));
        const float season_stimulus =
            0.5f * (1.0f + Luminumbra::DeterministicMath::Sin(
                               spring * Luminumbra::DeterministicMath::kTwoPi));
        EXPECT_FLOAT_EQ(needs[1].pressure, 0.001f * season_stimulus);
        const auto sky = Luminumbra::Rendering::ComputeSeason(clock);
        EXPECT_FLOAT_EQ(sky.phase, spring);
        EXPECT_FLOAT_EQ(Luminumbra::Rendering::TimeOfDayFromWorldClock(clock),
                        pin.day >= .5f ? pin.day - .5f : pin.day + .5f);

        // Independent one-tick plant reference: pinned light/season values,
        // with the same terrain and rain samples but no session calendar math.
        F::PlantEnvSample env;
        auto* world = session.GetWorldSystem();
        const float height = world->GetTerrainHeightAt(position.x, position.z);
        switch (world->SurfaceVertexMaterial(position.x, position.z, height)) {
            case Luminumbra::MaterialType::Grass:
                env.soil_quality = .95f;
                break;
            case Luminumbra::MaterialType::Soil:
                env.soil_quality = .85f;
                break;
            case Luminumbra::MaterialType::Sand:
                env.soil_quality = .45f;
                break;
            case Luminumbra::MaterialType::Stone:
                env.soil_quality = .30f;
                break;
            default:
                env.soil_quality = .25f;
                break;
        }
        env.temperature = F::clamp01(
            F::clamp01(.60f - std::max(0.0f, height - static_cast<float>(Luminumbra::SEA_LEVEL)) *
                                  .00045f) +
            (pin.warmth - .5f) * .30f);
        env.light = pin.daylight;
        env.moisture =
            F::clamp01(.30f + session.GetWeatherSystem()->PrecipitationAt(position) * .70f);
        entt::registry expected;
        const auto reference = ClockTestPlant(expected, position);
        F::RunPlantGrowthSystemOnTick(expected, pin.tick, [&](const auto&) { return env; });
        const auto& actual = session.GetRegistry().get<C::PlantGrowthComponent>(plant);
        const auto& wanted = expected.get<C::PlantGrowthComponent>(reference);
        EXPECT_EQ(actual.growth_points, wanted.growth_points);
        EXPECT_EQ(actual.stress_points, wanted.stress_points);
        EXPECT_EQ(actual.last_tick, pin.tick);
    }
}
} // namespace

// Expected sessions contain the devel fixture's logical state without executing
// the persistence sequence under test. Ambient fields are computed in this build:
// their legacy FastNoise auto-dispatch differs between AVX2 and AVX-512, unlike
// terrain's pinned AVX2 path. Exact serialized fixture bytes remain the disk oracle.
void AdoptRegionGoldenChunk(GameSession& session, int x) {
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(x, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->heightmap_data = {8.0f + x, 8.5f + x, 9.0f + x};
    if (x == 0)
        chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
}

void ExpectSameWorldHash(GameSession& actual, GameSession& expected) {
    EXPECT_EQ(SessionWorldHash(actual), SessionWorldHash(expected))
        << "actual: " << SessionWorldSubHashes(actual).dump()
        << "\nexpected: " << SessionWorldSubHashes(expected).dump();
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
    for (const int x : {0, 32})
        ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(session, x));
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
    }
    for (auto* s : {&expected_live, &expected_loaded}) {
        ClockTestPlant(s->GetRegistry(), Luminumbra::Vec3(8, 20, 8));
        ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(*s, 0));
    }
    // Only the live session retains the clean, unsaved second region.
    ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(expected_live, 32));
    const auto save = root.path() / "plant-golden-save";
    ASSERT_NO_FATAL_FAILURE(SavePlantRosterThenMultiRegionEdit(session, save));
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

TEST(GameSessionHeadlessWorldTest, AbsentLedgerAtRestoredTickAllowsEditAndPinBeforeSave) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original;
    original.SetRootPath(root.root_string());
    original.SetJobSystem(&jobs);
    original.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(original.CreateWorld("Absent ledger", "1337", "default"));
    for (int i = 0; i < 7; ++i)
        ASSERT_EQ(original.TickSimulation(original.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(original.SaveWorldState());
    for (const bool pin : {false, true}) {
        ASSERT_TRUE(P::WorldSaveService::remove_member(original.GetWorldSaveDir(),
                                                       "chunks/region/field-pages.fpg"));
        ASSERT_TRUE(P::WorldSaveService::remove_member(original.GetWorldSaveDir(),
                                                       "chunks/region/active-regions.arl"));
        GameSession loaded, restored;
        for (auto* s : {&loaded, &restored}) {
            s->SetRootPath(root.root_string());
            s->SetJobSystem(&jobs);
            s->SetActiveRegionsEnabled(true);
        }
        ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId));
        ASSERT_EQ(loaded.GetWorldClock().tick(), 7u);
        ASSERT_EQ(loaded.GetActiveRegionLedger().tick(), 7u);
        ASSERT_TRUE(loaded.GetActiveRegionLedger().records().empty());
        EXPECT_EQ(loaded.GetRegionSchedule().tick, 0u); // no synthetic simulation tick
        const Luminumbra::world::RegionKey key{10, -3};
        if (pin)
            loaded.PinActiveRegion(key, true);
        else
            loaded.NotifyGroundObjectEdit(Luminumbra::Vec3(5200, 0, -1500));
        ASSERT_TRUE(loaded.IsSimulationTickBoundary());
        ASSERT_TRUE(loaded.SaveWorldState());
        ASSERT_TRUE(restored.LoadWorld(original.GetMetadata().worldId));
        EXPECT_EQ(restored.GetWorldClock().tick(), 7u);
        EXPECT_EQ(restored.GetActiveRegionLedger().canonical_bytes(),
                  loaded.GetActiveRegionLedger().canonical_bytes());
        EXPECT_EQ(restored.GetActiveRegionLedger().records().at(key).first_active, 7u);
        EXPECT_TRUE(restored.GetActiveRegionLedger().records().at(key).wake_pending);
        ASSERT_EQ(restored.TickSimulation(restored.GetSimulationClock().fixed_dt()), 1u);
        EXPECT_EQ(restored.GetActiveRegionLedger().records().at(key).last_ticked, 8u);
    }
}

void ExpectLatestRegionTelemetry(const GameSession& original,
                                 const GameSession& loaded,
                                 std::uint64_t tick) {
    const auto& original_samples = original.GetSimBudgetTelemetry().SamplesByStage();
    const auto& loaded_samples = loaded.GetSimBudgetTelemetry().SamplesByStage();
    for (std::size_t stage = 0; stage < original_samples.size(); ++stage) {
        if (original_samples[stage].empty()) {
            EXPECT_TRUE(loaded_samples[stage].empty());
            continue;
        }
        ASSERT_FALSE(loaded_samples[stage].empty());
        EXPECT_EQ(loaded_samples[stage].back().tick, tick);
        EXPECT_EQ(loaded_samples[stage].back().work, original_samples[stage].back().work);
    }
}

void SaveAndReopenRegionalSchedule(GameSession& original,
                                   GameSession& render_only,
                                   GameSession& loaded) {
    const auto before = SessionWorldHash(original);
    ASSERT_TRUE(original.SaveWorldState());
    EXPECT_EQ(SessionWorldHash(original), before);
    ASSERT_TRUE(render_only.SaveWorldState());
    EXPECT_EQ(
        ReadClockTestFile(P::WorldSaveService::active_regions_path(original.GetWorldSaveDir())),
        ReadClockTestFile(P::WorldSaveService::active_regions_path(render_only.GetWorldSaveDir())));
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetActiveRegionLedger().encode(), original.GetActiveRegionLedger().encode());
    EXPECT_EQ(SessionWorldHash(loaded), before);
}

TEST(GameSessionHeadlessWorldTest, RegionScheduleAndWorldHashResumeAcrossBothHoldCounters) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, uninterrupted, render_only, loaded;
    for (auto* session : {&original, &uninterrupted, &render_only, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetAetherStateEnabled(false);
        session->SetWaterHighResEnabled(false);
        session->SetWeatherRainEnabled(false);
        session->SetWeatherEventsEnabled(false);
        session->SetFiniteHydrologyEnabled(false);
        session->SetRegionSchedulerConfig({10, 3, 2});
    }
    // Exercise the merged telemetry path through reduction, freeze, recovery,
    // and reload. Its observations must not become scheduler pressure or state.
    original.SetSimBudgetTelemetryEnabled(true);
    loaded.SetSimBudgetTelemetryEnabled(true);
    ASSERT_TRUE(original.CreateWorld("Region resume", "1337", "default"));
    ASSERT_TRUE(uninterrupted.CreateTransientWorld("Uninterrupted", "1337", "default"));
    ASSERT_TRUE(render_only.CreateWorld("Render variant", "1337", "default"));
    const Luminumbra::world::RegionKey far{40, -40};
    const Luminumbra::IVec3 coords(1280, 0, -1280);
    for (auto* session : {&original, &uninterrupted, &render_only}) {
        auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
        chunk->set_state(Luminumbra::ChunkState::Idle);
        chunk->heightmap_data = {8.0f, 8.5f, 9.0f};
        chunk->mark_voxel_data_dirty();
        if (session == &render_only) {
            chunk->mesh_vertices.resize(3);
            chunk->mesh_indices = {0, 1, 2};
            chunk->water_mesh_vertices.resize(3);
            chunk->water_mesh_indices = {2, 1, 0};
            chunk->water_mesh_generated.store(true);
            chunk->water_mesh_dirty_ticks = 17;
        }
        ASSERT_TRUE(session->GetWorldSystem()->adopt_streamed_chunk(chunk));
    }
    for (auto* session : {&original, &uninterrupted, &render_only})
        session->PinActiveRegion(far, true);
    const auto dt = original.GetSimulationClock().fixed_dt();
    for (std::uint64_t tick = 1; tick <= 20; ++tick) {
        SCOPED_TRACE(tick);
        if (tick == 5) {
            // Change live simulation after the save at tick 3. At freeze, the
            // durable copy is stale and must lose to the dirty resident chunk.
            for (auto* session : {&original, &uninterrupted, &render_only, &loaded}) {
                // Bounded reopening leaves this far authority on disk until requested.
                // Adopt its exact durable bytes before editing the live authoritative object.
                const auto chunk = session->GetWorldSystem()->find_authoritative_chunk(coords);
                ASSERT_NE(chunk, nullptr);
                if (!session->GetWorldSystem()->find_streamed_chunk(coords))
                    ASSERT_TRUE(session->GetWorldSystem()->adopt_streamed_chunk(chunk));
                chunk->heightmap_data[0] = 12.0f;
                chunk->mark_voxel_data_dirty();
            }
        }
        for (auto* session : {&original, &uninterrupted, &render_only}) {
            session->RecordActiveRegionWork({far, tick <= 9 ? 11u : 10u, 0, 0, 0});
            ASSERT_EQ(session->TickSimulation(dt), 1u);
        }
        EXPECT_EQ(original.GetRegionSchedule(), uninterrupted.GetRegionSchedule());
        EXPECT_EQ(SessionWorldHash(original), SessionWorldHash(uninterrupted));
        EXPECT_EQ(original.GetRegionSchedule(), render_only.GetRegionSchedule());
        EXPECT_EQ(original.GetActiveRegionLedger().canonical_bytes(),
                  render_only.GetActiveRegionLedger().canonical_bytes());
        EXPECT_EQ(SessionWorldHash(original), SessionWorldHash(render_only));
        if (tick == 7) {
            ASSERT_EQ(original.GetActiveRegionLedger().records().at(far).state,
                      Luminumbra::world::RegionState::Frozen);
            EXPECT_EQ(original.GetActiveRegionLedger().records().at(far).frozen_digest,
                      uninterrupted.GetActiveRegionLedger().records().at(far).frozen_digest);
        }
        if (tick >= 4) {
            loaded.RecordActiveRegionWork({far, tick <= 9 ? 11u : 10u, 0, 0, 0});
            ASSERT_EQ(loaded.TickSimulation(dt), 1u);
            EXPECT_EQ(loaded.GetRegionSchedule(), original.GetRegionSchedule());
            EXPECT_EQ(loaded.GetActiveRegionLedger().canonical_bytes(),
                      original.GetActiveRegionLedger().canonical_bytes());
            EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
            ASSERT_NO_FATAL_FAILURE(ExpectLatestRegionTelemetry(original, loaded, tick));
        }
        if (tick == 3 || tick == 11) {
            ASSERT_NO_FATAL_FAILURE(SaveAndReopenRegionalSchedule(original, render_only, loaded));
        }
    }
    EXPECT_EQ(loaded.GetSimulationTickCount(), 20u);
    const auto& combined =
        original.GetSimBudgetTelemetry().SamplesByStage()[static_cast<std::size_t>(
            luminumbra::simulation::SimBudgetStage::WindWeather)];
    ASSERT_EQ(combined.size(), 20u);
    for (const auto& sample : combined)
        EXPECT_EQ(sample.work, 2u * 64u * 64u);
    for (const auto& stage : uninterrupted.GetSimBudgetTelemetry().SamplesByStage())
        EXPECT_TRUE(stage.empty());
}

static std::uint64_t Digest(const std::filesystem::path& save,
                            Luminumbra::world::RegionKey key,
                            std::vector<std::shared_ptr<Luminumbra::Chunk>> live,
                            std::vector<std::shared_ptr<Luminumbra::Chunk>> parked = {}) {
    return P::WorldSaveService::region_simulation_digest(
        save,
        key,
        P::RegionDigestInputs{.live = std::move(live),
                              .parked = std::move(parked),
                              .durable = {},
                              .ground_object_bytes = {},
                              .field_page_bytes = {}});
}

TEST(GameSessionHeadlessWorldTest, RegionDigestMergesLiveAndDurableSimulationOnly) {
    const HeadlessRoot root;
    // The content-v2 digest includes only authoritative edits. Keep the combined
    // live/durable regression by marking every simulated durable fixture as edited.
    Luminumbra::WorldStreamingState durable;
    const auto old = durable.get_or_create_chunk({0, 0, 0});
    old->heightmap_data = {1.0f};
    old->mark_voxel_data_dirty();
    const auto unloaded = durable.get_or_create_chunk({1, 0, 0});
    unloaded->heightmap_data = {2.0f};
    unloaded->mark_voxel_data_dirty();
    const auto other_region = durable.get_or_create_chunk({32, 0, 0});
    other_region->heightmap_data = {3.0f};
    other_region->mark_voxel_data_dirty();
    const auto save = root.path() / "digest";
    ASSERT_TRUE(P::WorldSaveService{}.save_world(durable, save));
    const auto live = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    live->heightmap_data = {4.0f};
    live->mark_voxel_data_dirty();
    const auto digest = P::WorldSaveService::region_simulation_digest(save, {0, 0}, {live});
    EXPECT_EQ(digest, P::WorldSaveService::region_simulation_digest({}, {0, 0}, {unloaded, live}));
    EXPECT_NE(digest, P::WorldSaveService::region_simulation_digest(save, {0, 0}, {}));
    // Both durable render data and live render data are outside the projection.
    unloaded->mesh_vertices.resize(3);
    unloaded->mesh_indices = {0, 1, 2};
    unloaded->water_mesh_generated.store(true);
    unloaded->water_mesh_dirty_ticks = 11;
    ASSERT_TRUE(P::WorldSaveService{}.save_world(durable, save));
    live->mesh_vertices.resize(3);
    live->mesh_indices = {2, 1, 0};
    live->water_mesh_dirty_ticks = 29;
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(save, {0, 0}, {live}), digest);
    for (const auto state : {Luminumbra::ChunkState::Meshing, Luminumbra::ChunkState::Ready}) {
        live->set_state(state);
        for (const bool collision : {true, false}) {
            live->has_collision.store(collision);
            EXPECT_EQ(P::WorldSaveService::region_simulation_digest(save, {0, 0}, {live}), digest);
        }
    }
    live->heightmap_data[0] = 5.0f;
    EXPECT_NE(P::WorldSaveService::region_simulation_digest(save, {0, 0}, {live}), digest);
    live->heightmap_data[0] = 4.0f;
    live->material_data = {1};
    EXPECT_NE(P::WorldSaveService::region_simulation_digest(save, {0, 0}, {live}), digest);
    live->material_data.clear();
    live->water_depth_mm = {1};
    EXPECT_NE(P::WorldSaveService::region_simulation_digest(save, {0, 0}, {live}), digest);
}

TEST(GameSessionHeadlessWorldTest, RegionDigestExcludesPristineAndLoadingChunks) {
    const auto empty = Digest({}, {0, 0}, {});
    const auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->heightmap_data = {1.0f};
    EXPECT_EQ(Digest({}, {0, 0}, {chunk}), empty);
    chunk->heightmap_data = {2.0f};
    EXPECT_EQ(Digest({}, {0, 0}, {chunk}), empty);
    chunk->mark_voxel_data_dirty();
    EXPECT_NE(Digest({}, {0, 0}, {chunk}), empty);
    chunk->set_state(Luminumbra::ChunkState::Loading);
    EXPECT_EQ(Digest({}, {0, 0}, {chunk}), empty);
    const auto other = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(32, 0, 0));
    other->heightmap_data = {3.0f};
    other->mark_voxel_data_dirty();
    EXPECT_EQ(Digest({}, {0, 0}, {other}), empty);
}

TEST(GameSessionHeadlessWorldTest, RegionDigestIsIndependentOfResidencyAndSaveHistory) {
    const HeadlessRoot root;
    const auto x = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    x->heightmap_data = {4.0f};
    x->mark_voxel_data_dirty();
    const auto live_only = Digest({}, {0, 0}, {x});
    EXPECT_EQ(Digest({}, {0, 0}, {}, {x}), live_only);
    Luminumbra::WorldStreamingState durable;
    const auto stored = durable.get_or_create_chunk({0, 0, 0});
    stored->heightmap_data = {4.0f};
    stored->mark_voxel_data_dirty();
    const auto save = root.path() / "digest-history";
    ASSERT_TRUE(P::WorldSaveService{}.save_world(durable, save));
    EXPECT_EQ(Digest(save, {0, 0}, {}), live_only);
    EXPECT_EQ(Digest(save, {0, 0}, {x}), live_only);
    x->clear_voxel_data_dirty();
    EXPECT_EQ(Digest(save, {0, 0}, {x}), live_only);
    EXPECT_EQ(Digest({}, {0, 0}, {x}), live_only);
    x->heightmap_data[0] = 5.0f;
    EXPECT_NE(Digest({}, {0, 0}, {x}), live_only);
}

TEST(GameSessionHeadlessWorldTest, RegionDigestPriorityIsLiveThenParkedThenDurable) {
    const HeadlessRoot root;
    Luminumbra::WorldStreamingState durable;
    const auto stored = durable.get_or_create_chunk({0, 0, 0});
    stored->heightmap_data = {1.0f};
    stored->mark_voxel_data_dirty();
    const auto save = root.path() / "digest-priority";
    ASSERT_TRUE(P::WorldSaveService{}.save_world(durable, save));
    const auto parked = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    parked->heightmap_data = {2.0f};
    parked->mark_voxel_data_dirty();
    const auto live = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    live->heightmap_data = {3.0f};
    live->mark_voxel_data_dirty();
    EXPECT_EQ(Digest(save, {0, 0}, {live}, {parked}), Digest({}, {0, 0}, {live}));
    EXPECT_EQ(Digest(save, {0, 0}, {}, {parked}), Digest({}, {0, 0}, {}, {parked}));
    const auto durable_only = Digest(save, {0, 0}, {});
    EXPECT_NE(Digest(save, {0, 0}, {}, {parked}), durable_only);
    EXPECT_NE(Digest({}, {0, 0}, {live}), Digest({}, {0, 0}, {parked}));
    EXPECT_NE(Digest({}, {0, 0}, {live}), durable_only);
    EXPECT_NE(Digest({}, {0, 0}, {parked}), durable_only);
}

TEST(GameSessionHeadlessWorldTest, RegionDigestIgnoresUnflaggedDurableRecords) {
    const HeadlessRoot root;
    Luminumbra::WorldStreamingState durable;
    durable.get_or_create_chunk({0, 0, 0})->heightmap_data = {1.0f};
    const auto save = root.path() / "digest-unflagged";
    ASSERT_TRUE(P::WorldSaveService{}.save_world(durable, save));
    EXPECT_EQ(Digest(save, {0, 0}, {}), Digest({}, {0, 0}, {}));
}

TEST(GameSessionHeadlessWorldTest, RegionDigestIgnoresRenderAndResidencyState) {
    const auto live = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    live->heightmap_data = {4.0f};
    live->mark_voxel_data_dirty();
    const auto digest = Digest({}, {0, 0}, {live});
    live->mesh_vertices.resize(3);
    live->mesh_indices = {2, 1, 0};
    live->water_mesh_dirty_ticks = 29;
    EXPECT_EQ(Digest({}, {0, 0}, {live}), digest);
    for (const auto state : {Luminumbra::ChunkState::Meshing, Luminumbra::ChunkState::Ready}) {
        live->set_state(state);
        for (const bool collision : {true, false}) {
            live->has_collision.store(collision);
            EXPECT_EQ(Digest({}, {0, 0}, {live}), digest);
        }
    }
    live->heightmap_data[0] = 5.0f;
    EXPECT_NE(Digest({}, {0, 0}, {live}), digest);
    live->heightmap_data[0] = 4.0f;
    live->material_data = {1};
    EXPECT_NE(Digest({}, {0, 0}, {live}), digest);
    live->material_data.clear();
    live->water_depth_mm = {1};
    EXPECT_NE(Digest({}, {0, 0}, {live}), digest);
}

TEST(GameSessionHeadlessWorldTest, RegionDigestIgnoresLoadTimeCollisionNormalization) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    for (auto* s : {&session, &loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(session.CreateWorld("Collision normalization", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Meshing);
    chunk->has_collision.store(true);
    chunk->heightmap_data = {8.0f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    const auto digest = Digest({}, {0, 0}, {chunk});
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId));
    ASSERT_TRUE(loaded.GetWorldSystem()->restore_authoritative_chunk({0, 0, 0}));
    const auto chunks = loaded.GetWorldSystem()->snapshot_streamed_chunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_FALSE(chunks.front()->has_collision.load());
    EXPECT_EQ(chunks.front()->get_state(), Luminumbra::ChunkState::Idle);
    EXPECT_EQ(Digest({}, {0, 0}, chunks), digest);
    EXPECT_EQ(Digest(session.GetWorldSaveDir(), {0, 0}, {}), digest);
}

TEST(GameSessionHeadlessWorldTest, FirstEnabledChunkSaveIncludesCleanResidentRegions) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    for (auto* s : {&session, &loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(session.CreateWorld("Initial regions", "1337", "default"));
    const auto save = session.GetWorldSaveDir();
    EXPECT_TRUE(P::WorldSaveService::has_world_save(save));
    EXPECT_FALSE(P::WorldSaveService::has_chunk_snapshot(save));
    for (const int x : {0, 32}) {
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(x, 0, 0));
        chunk->set_state(Luminumbra::ChunkState::Idle);
        chunk->heightmap_data = {8.0f + x};
        if (x == 0)
            chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    }
    const auto before = SessionWorldHash(session);
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_TRUE(P::WorldSaveService::has_chunk_snapshot(save));
    EXPECT_TRUE(fs::exists(P::WorldSaveService::region_file_path(save, 0, 0)));
    EXPECT_TRUE(fs::exists(P::WorldSaveService::region_file_path(save, 1, 0)));
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetWorldSystem()->durable_chunk_inventory().size(), 1u);
    EXPECT_EQ(SessionWorldHash(loaded), before);
}

TEST(GameSessionHeadlessWorldTest, EmptyReplicatedAnchorsNeverUseLocalSpawnFallback) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession server;
    server.SetRootPath(root.root_string());
    server.SetJobSystem(&jobs);
    server.SetActiveRegionsEnabled(true);
    server.SetRegionSchedulerConfig({0, 1, 1});
    ASSERT_TRUE(server.CreateTransientWorld("Empty server", "1337", "default"));
    const auto spawn = server.GetMetadata().spawnPoint;
    ASSERT_EQ(server.GetActiveRegionLedger().local_anchor(),
              std::optional<Luminumbra::Vec3>(spawn));
    server.SetReplicatedSimulationAnchors({});
    const auto dt = server.GetSimulationClock().fixed_dt();
    ASSERT_EQ(server.TickSimulation(dt), 1u);
    EXPECT_TRUE(server.GetActiveRegionLedger().records().empty());
    EXPECT_TRUE(server.GetRegionSchedule().due.empty());
    server.SetReplicatedSimulationAnchors({spawn});
    ASSERT_EQ(server.TickSimulation(dt), 1u);
    const auto key = Luminumbra::world::ActiveRegionLedger::region_at(spawn);
    EXPECT_EQ(server.GetActiveRegionLedger().records().at(key).last_proximity, 2u);
    server.SetReplicatedSimulationAnchors({});
    server.RecordActiveRegionWork({key, 1, 0, 0, 0});
    ASSERT_EQ(server.TickSimulation(dt), 1u);
    for (const auto& [region, record] : server.GetActiveRegionLedger().records()) {
        (void)region;
        EXPECT_EQ(record.last_proximity, 2u);
    }
    ASSERT_EQ(server.GetRegionSchedule().transitions.size(), 1u);
    EXPECT_EQ(server.GetRegionSchedule().transitions.front().to,
              Luminumbra::world::RegionState::Reduced);
}

TEST(GameSessionHeadlessWorldTest, QueuedRegionWorkRefusesSaveUntilTickCompletes) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    for (auto* s : {&session, &loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(true);
        s->SetRegionSchedulerConfig({0, 2, 1});
    }
    ASSERT_TRUE(session.CreateWorld("Queued work", "1337", "default"));
    const Luminumbra::world::RegionKey far{40, -40};
    session.PinActiveRegion(far, true);
    const auto dt = session.GetSimulationClock().fixed_dt();
    ASSERT_EQ(session.TickSimulation(dt), 1u); // consume pin wake
    session.RecordActiveRegionWork({far, 1, 0, 0, 0});
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_EQ(session.GetActiveRegionLedger().records().at(far).over_hold, 1u);
    ASSERT_TRUE(session.SaveWorldState());
    const auto save = session.GetWorldSaveDir();
    const auto metadata = ReadClockTestFile(save / "world_info.json");
    const auto ledger = ReadClockTestFile(P::WorldSaveService::active_regions_path(save));
    session.RecordActiveRegionWork({far, 1, 0, 0, 0});
    EXPECT_FALSE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.TickSimulation(0.0), 0u);
    EXPECT_FALSE(session.IsSimulationTickBoundary());
    EXPECT_FALSE(session.SaveWorld());
    EXPECT_FALSE(session.SaveWorldState());
    const auto alternate = root.path() / "incomplete";
    EXPECT_FALSE(session.SaveWorldStateTo(alternate));
    EXPECT_FALSE(fs::exists(alternate));
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), metadata);
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::active_regions_path(save)), ledger);
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.GetActiveRegionLedger().records().at(far).state,
              Luminumbra::world::RegionState::Reduced);
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId));
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(session));
    for (auto* s : {&session, &loaded}) {
        s->RecordActiveRegionWork({far, 1, 0, 0, 0});
        ASSERT_EQ(s->TickSimulation(dt), 1u);
    }
    EXPECT_EQ(loaded.GetRegionSchedule(), session.GetRegionSchedule());
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(session));
}

namespace {
void AssertAndAdoptIdleRetentionResidents(GameSession& session,
                                          std::span<const Luminumbra::world::RegionKey> keys,
                                          Luminumbra::world::RegionKey resident,
                                          Luminumbra::world::RegionKey pristine,
                                          Luminumbra::world::RegionKey unowned,
                                          Luminumbra::world::RegionKey loading,
                                          Luminumbra::world::RegionKey pristine_loading,
                                          Luminumbra::world::RegionKey non_due_resident) {
    using Luminumbra::world::RegionState;
    for (const auto key : keys) {
        const auto& record = session.GetActiveRegionLedger().records().at(key);
        ASSERT_FALSE(record.pinned);
        ASSERT_FALSE(record.edited);
        ASSERT_FALSE(record.populated);
        ASSERT_EQ(record.water_cursor, 0u);
        ASSERT_EQ(record.last_proximity, 0u);
        ASSERT_FALSE(session.GetFieldPages().owns_region(key));
        ASSERT_FALSE(session.GetActiveRegionLedger().completed_work().contains(key));
        ASSERT_FALSE(session.GetActiveRegionLedger().energy_ages().contains(key));
    }
    for (const auto key : {pristine, unowned})
        ASSERT_EQ(session.GetActiveRegionLedger().records().at(key).state, RegionState::Active);

    for (const auto key : {resident, pristine, loading, pristine_loading, non_due_resident}) {
        auto chunk =
            std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(key.x * 32, 0, key.z * 32));
        chunk->set_state(key == loading || key == pristine_loading ? Luminumbra::ChunkState::Loading
                                                                   : Luminumbra::ChunkState::Idle);
        chunk->heightmap_data = {8.0f};
        // A complete, dry sleeping grid has no water work; only ownership is under test.
        chunk->current_water_resolution.store(8);
        chunk->water_depth_mm.assign(64, 0);
        chunk->water_bed_mm.assign(64, 8000);
        chunk->water_edge_flux.assign(128, 0);
        chunk->water_level_data.assign(64, 8.0f);
        chunk->water_flow_data.assign(64, Luminumbra::Vec2(0));
        chunk->water_sim_terrain_height.assign(64, 8.0f);
        chunk->has_water_sim.store(true);
        chunk->is_water_sleeping.store(true);
        if (key == resident || key == loading || key == non_due_resident)
            chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
        // Adoption settles populated Loading chunks; explicitly exercise an in-flight resident.
        if (key == loading || key == pristine_loading)
            chunk->set_state(Luminumbra::ChunkState::Loading);
        // Residency protects the record regardless of provenance or lifecycle state.
        ASSERT_FALSE(session.GetActiveRegionLedger().records().at(key).edited);
    }
}

void AssertIdleRetentionContinuation(GameSession& session,
                                     Luminumbra::world::ActiveRegionLedger& expected,
                                     std::span<const Luminumbra::world::RegionKey> owned_keys,
                                     Luminumbra::world::RegionKey resident,
                                     Luminumbra::world::RegionKey pristine,
                                     Luminumbra::world::RegionKey loading,
                                     Luminumbra::world::RegionKey pristine_loading,
                                     Luminumbra::world::RegionKey non_due_resident) {
    using Luminumbra::world::ActiveRegionLedger;
    using Luminumbra::world::RegionKey;
    using Luminumbra::world::RegionState;
    using Luminumbra::world::RegionWork;
    using Luminumbra::world::WorldClock;
    // Consumed host work expires; field pages own due keys and resident authority owns the rest.
    const auto next_schedule = expected.schedule(
        WorldClock(ActiveRegionLedger::kIdleEvictionTicks + 1),
        std::span<const Luminumbra::Vec3>{},
        {},
        {},
        [&session, resident, pristine, loading, pristine_loading, non_due_resident](RegionKey key) {
            return key == resident || key == pristine || key == loading ||
                   key == pristine_loading || key == non_due_resident ||
                   session.GetFieldPages().owns_region(key);
        });
    auto expected_pages = session.GetFieldPages();
    std::vector<RegionWork> completed;
    for (const auto& [key, updates] : expected_pages.step_scheduled(
             expected.records(), next_schedule.due, 1337, next_schedule.tick))
        completed.push_back({key, 0, 0, 0, updates});
    for (const auto key : next_schedule.due)
        completed.push_back({key, 0, 0, 0, 1}); // regional weather clock
    expected.publish_completed_work(completed);
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    const auto& next_fault = session.GetRegionSchedulerFault();
    ASSERT_FALSE(next_fault) << (next_fault ? next_fault->message : "");
    EXPECT_EQ(session.GetRegionSchedule(), next_schedule);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), expected.canonical_bytes());
    EXPECT_EQ(session.GetActiveRegionLedger().encode(), expected.encode());
    ASSERT_EQ(session.GetActiveRegionLedger().records().size(), owned_keys.size());
    for (const auto key : next_schedule.due) {
        EXPECT_TRUE(session.GetFieldPages().owns_region(key));
        EXPECT_TRUE(session.GetActiveRegionLedger().records().contains(key));
    }
    for (const auto key : {resident, pristine, loading, pristine_loading})
        EXPECT_TRUE(session.GetActiveRegionLedger().records().contains(key));
    EXPECT_TRUE(session.GetActiveRegionLedger().records().contains(non_due_resident));
    EXPECT_EQ(session.GetActiveRegionLedger().records().at(non_due_resident).state,
              RegionState::Frozen);
    EXPECT_EQ(session.GetActiveRegionLedger().records().at(non_due_resident).last_ticked, 0u);
    EXPECT_FALSE(session.GetFieldPages().owns_region(non_due_resident));
    EXPECT_FALSE(session.GetActiveRegionLedger().completed_work().contains(non_due_resident));
    EXPECT_FALSE(session.GetActiveRegionLedger().energy_ages().contains(non_due_resident));
}
} // namespace

TEST(GameSessionHeadlessWorldTest, IdleRetentionMatchesWorkAndAllResidentOwnership) {
    using Luminumbra::world::ActiveRegionLedger;
    using Luminumbra::world::ActiveRegionRecord;
    using Luminumbra::world::FieldPageState;
    using Luminumbra::world::RegionKey;
    using Luminumbra::world::RegionState;
    using Luminumbra::world::RegionWork;
    using Luminumbra::world::WorldClock;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetWaterHighResEnabled(false);
    session.SetWeatherRainEnabled(false);
    session.SetAetherStateEnabled(false);
    ASSERT_TRUE(session.CreateWorld("Idle retention", "1337", "default"));
    ASSERT_TRUE(session.SaveWorldState());

    const RegionKey pending{40, -40}, duplicate{41, -40}, zero_count{42, -40};
    const RegionKey resident{43, -40}, pristine{44, -40}, unowned{45, -40};
    const RegionKey loading{46, -40}, pristine_loading{47, -40};
    const RegionKey non_due_resident{48, -40};
    const std::array keys{pending,
                          duplicate,
                          zero_count,
                          resident,
                          pristine,
                          unowned,
                          loading,
                          pristine_loading,
                          non_due_resident};
    ActiveRegionLedger staged;
    ASSERT_TRUE(staged.adopt_content_identity(session.GetSimulationContent()->identity));
    staged.set_local_anchor(session.GetMetadata().spawnPoint);
    for (const auto key : keys) {
        staged.pin(key, true, WorldClock{});
        staged.pin(key, false, WorldClock{});
    }
    {
        // This authority must rely on the ownership probe without initializing field pages.
        auto& record = const_cast<std::map<RegionKey, ActiveRegionRecord>&>(staged.records())
                           .at(non_due_resident);
        record.wake_pending = false;
        record.state = RegionState::Frozen;
        record.frozen_at = 1;
        record.cadence_shift = staged.config().reduced_shift;
        record.cadence_phase = ActiveRegionLedger::phase(non_due_resident, record.cadence_shift);
    }
    // Stage just before retention without advancing thousands of simulation ticks.
    const WorldClock before_idle(ActiveRegionLedger::kIdleEvictionTicks - 1);
    staged.schedule(before_idle, std::span<const Luminumbra::Vec3>{});
    const auto save = session.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    before_idle.write_metadata(metadata);
    ASSERT_TRUE(
        P::WorldSaveService::save_metadata_and_active_regions(metadata.dump(), staged, save));
    ASSERT_TRUE(session.LoadWorldStateFrom(save)) << session.GetWorldOpenError();
    ASSERT_EQ(session.GetActiveRegionLedger().records().size(), keys.size());
    ASSERT_TRUE(session.GetRegistry().view<C::PlantTag>().empty());
    ASSERT_TRUE(session.GetFieldPages().soil().empty());
    ASSERT_TRUE(session.GetFieldPages().irrigation().empty());
    session.SetReplicatedSimulationAnchors({});
    ASSERT_NO_FATAL_FAILURE(AssertAndAdoptIdleRetentionResidents(
        session, keys, resident, pristine, unowned, loading, pristine_loading, non_due_resident));
    const std::array work{RegionWork{pending, 3, 0, 0, 0},
                          RegionWork{duplicate, 1, 0, 0, 0},
                          RegionWork{duplicate, 2, 0, 0, 0},
                          RegionWork{zero_count, 0, 0, 0, 0},
                          RegionWork{zero_count, 0, 0, 0, 0}};
    for (const auto& entry : work)
        session.RecordActiveRegionWork(entry);
    auto expected = session.GetActiveRegionLedger();
    const std::array owned_keys{pending,
                                duplicate,
                                zero_count,
                                resident,
                                pristine,
                                loading,
                                pristine_loading,
                                non_due_resident};
    const auto expected_schedule = expected.schedule(
        WorldClock(ActiveRegionLedger::kIdleEvictionTicks),
        std::span<const Luminumbra::Vec3>{},
        work,
        {},
        [&owned_keys](RegionKey key) {
            return std::find(owned_keys.begin(), owned_keys.end(), key) != owned_keys.end();
        });
    ASSERT_EQ(expected_schedule.due,
              (std::vector<RegionKey>{
                  pending, duplicate, zero_count, resident, pristine, loading, pristine_loading}));
    const auto dt = session.GetSimulationClock().fixed_dt();
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    const auto& fault = session.GetRegionSchedulerFault();
    ASSERT_FALSE(fault) << (fault ? fault->message : "");
    std::vector<RegionWork> completed;
    for (const auto key : expected_schedule.due) {
        // This fixture has no foliage authority. The first due tick seeds ambient cells
        // and advances the regional weather clock, with no additional S7 work.
        const auto cells = FieldPageState::cells_of_region(key).size();
        EXPECT_EQ(session.GetFieldPages().owned_initialized_cells(key), cells);
        EXPECT_EQ(session.GetFieldPages().weather_clocks().at(key), 1u);
        completed.push_back({key, 0, 0, 0, cells + 1});
    }
    expected.publish_completed_work(completed);
    EXPECT_EQ(session.GetRegionSchedule(), expected_schedule);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), expected.canonical_bytes());
    EXPECT_EQ(session.GetActiveRegionLedger().encode(), expected.encode());
    ASSERT_EQ(session.GetActiveRegionLedger().records().size(), owned_keys.size());
    for (const auto key : owned_keys)
        EXPECT_TRUE(session.GetActiveRegionLedger().records().contains(key));
    // Pristine residency retains the record under the repaired ownership contract.
    EXPECT_TRUE(session.GetActiveRegionLedger().records().contains(pristine));
    EXPECT_FALSE(session.GetActiveRegionLedger().records().contains(unowned));
    for (const auto key : {pristine, loading, pristine_loading, unowned}) {
        EXPECT_EQ(std::find(session.GetRegionSchedule().due.begin(),
                            session.GetRegionSchedule().due.end(),
                            key) != session.GetRegionSchedule().due.end(),
                  key != unowned);
        EXPECT_EQ(session.GetFieldPages().owns_region(key), key != unowned);
        EXPECT_EQ(session.GetActiveRegionLedger().completed_work().contains(key), key != unowned);
    }
    EXPECT_EQ(session.GetActiveRegionLedger().records().at(non_due_resident).state,
              RegionState::Frozen);
    EXPECT_EQ(session.GetActiveRegionLedger().records().at(non_due_resident).last_ticked, 0u);
    EXPECT_FALSE(session.GetFieldPages().owns_region(non_due_resident));
    EXPECT_FALSE(session.GetActiveRegionLedger().completed_work().contains(non_due_resident));
    EXPECT_FALSE(session.GetActiveRegionLedger().energy_ages().contains(non_due_resident));

    ASSERT_NO_FATAL_FAILURE(AssertIdleRetentionContinuation(session,
                                                            expected,
                                                            owned_keys,
                                                            resident,
                                                            pristine,
                                                            loading,
                                                            pristine_loading,
                                                            non_due_resident));
}

TEST(GameSessionHeadlessWorldTest, ConsumedWorkEvictsIdleRegionsThatNeverBecomeDue) {
    using Luminumbra::world::ActiveRegionLedger;
    using Luminumbra::world::ActiveRegionRecord;
    using Luminumbra::world::RegionKey;
    using Luminumbra::world::RegionState;
    using Luminumbra::world::RegionWork;
    using Luminumbra::world::WorldClock;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Consumed idle work", "1337", "default"));
    ASSERT_TRUE(session.SaveWorldState());

    const RegionKey pending{40, -40}, duplicate{41, -40}, zero_count{42, -40};
    const std::array keys{pending, duplicate, zero_count};
    ActiveRegionLedger staged;
    ASSERT_TRUE(staged.adopt_content_identity(session.GetSimulationContent()->identity));
    staged.set_local_anchor(session.GetMetadata().spawnPoint);
    for (const auto key : keys) {
        staged.pin(key, true, WorldClock{});
        staged.pin(key, false, WorldClock{});
        // Mutate only this fixture ledger: frozen records never initialize field authority.
        auto& record =
            const_cast<std::map<RegionKey, ActiveRegionRecord>&>(staged.records()).at(key);
        record.wake_pending = false;
        record.state = RegionState::Frozen;
        record.frozen_at = 1;
        record.cadence_shift = staged.config().reduced_shift;
        record.cadence_phase = ActiveRegionLedger::phase(key, record.cadence_shift);
    }
    const WorldClock before_idle(ActiveRegionLedger::kIdleEvictionTicks - 1);
    ASSERT_TRUE(staged.schedule(before_idle, std::span<const Luminumbra::Vec3>{}).due.empty());
    const auto save = session.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    before_idle.write_metadata(metadata);
    ASSERT_TRUE(
        P::WorldSaveService::save_metadata_and_active_regions(metadata.dump(), staged, save));
    ASSERT_TRUE(session.LoadWorldStateFrom(save)) << session.GetWorldOpenError();
    session.SetReplicatedSimulationAnchors({});
    const std::array work{RegionWork{pending, 3, 0, 0, 0},
                          RegionWork{duplicate, 1, 0, 0, 0},
                          RegionWork{duplicate, 2, 0, 0, 0},
                          RegionWork{zero_count, 0, 0, 0, 0},
                          RegionWork{zero_count, 0, 0, 0, 0}};
    for (const auto& entry : work)
        session.RecordActiveRegionWork(entry);
    auto expected = session.GetActiveRegionLedger();
    const auto retained_schedule =
        expected.schedule(WorldClock(ActiveRegionLedger::kIdleEvictionTicks),
                          std::span<const Luminumbra::Vec3>{},
                          work,
                          {},
                          [](RegionKey) { return true; });
    const auto dt = session.GetSimulationClock().fixed_dt();
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetRegionSchedule(), retained_schedule);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), expected.canonical_bytes());
    EXPECT_EQ(session.GetActiveRegionLedger().encode(), expected.encode());
    ASSERT_EQ(session.GetActiveRegionLedger().records().size(), keys.size());
    EXPECT_TRUE(session.GetRegionSchedule().due.empty());
    EXPECT_TRUE(session.GetFieldPages().empty());
    EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());

    const auto evicted_schedule =
        expected.schedule(WorldClock(ActiveRegionLedger::kIdleEvictionTicks + 1),
                          std::span<const Luminumbra::Vec3>{},
                          {},
                          {},
                          [](RegionKey) { return false; });
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetRegionSchedule(), evicted_schedule);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), expected.canonical_bytes());
    EXPECT_EQ(session.GetActiveRegionLedger().encode(), expected.encode());
    EXPECT_TRUE(session.GetActiveRegionLedger().records().empty());
    EXPECT_TRUE(session.GetRegionSchedule().due.empty());
    EXPECT_TRUE(session.GetFieldPages().empty());
}

TEST(GameSessionHeadlessWorldTest, WalkingAnchorSurvivesNoclipAndReloadWithoutCameraInputs) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession walking, noclip, loaded;
    for (auto* session : {&walking, &noclip, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(walking.CreateTransientWorld("Walking", "1337", "default"));
    ASSERT_TRUE(noclip.CreateWorld("Noclip", "1337", "default"));
    const Luminumbra::Vec3 feet(-768, 30, 768);
    walking.SetLocalPlayerSimulationPosition(feet, true);
    noclip.SetLocalPlayerSimulationPosition(feet, true);
    for (int tick = 0; tick < 5; ++tick) {
        noclip.SetLocalPlayerSimulationPosition(Luminumbra::Vec3(10000 + tick * 1000, 500, -20000),
                                                false);
        ASSERT_EQ(walking.TickSimulation(walking.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_EQ(noclip.TickSimulation(noclip.GetSimulationClock().fixed_dt()), 1u);
        EXPECT_EQ(walking.GetRegionSchedule(), noclip.GetRegionSchedule());
        EXPECT_EQ(SessionWorldHash(walking), SessionWorldHash(noclip));
    }
    ASSERT_TRUE(noclip.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(noclip.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetActiveRegionLedger().local_anchor(), std::optional<Luminumbra::Vec3>(feet));
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(noclip));
    for (auto* session : {&walking, &noclip, &loaded})
        ASSERT_EQ(session->TickSimulation(session->GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(walking));
    // Replicated avatars replace the single-player anchor for activation.
    loaded.SetReplicatedSimulationAnchors({Luminumbra::Vec3(10000, 0, 10000)});
    ASSERT_EQ(loaded.TickSimulation(loaded.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(loaded.GetActiveRegionLedger().records().at({-2, 1}).last_proximity, 6u);
    EXPECT_EQ(loaded.GetActiveRegionLedger().records().at({19, 19}).last_proximity, 7u);
}

TEST(GameSessionHeadlessWorldTest, LedgerFileAbsenceCorruptionFutureAndDisabledRefusals) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    owner.SetRootPath(root.root_string());
    owner.SetJobSystem(&jobs);
    owner.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(owner.CreateWorld("Ledger refusal", "1337", "default"));
    ASSERT_EQ(owner.TickSimulation(owner.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto path = P::WorldSaveService::active_regions_path(save);
    const auto good = ReadClockTestFile(path);
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    for (const bool future : {false, true}) {
        auto bytes = good;
        if (future)
            bytes[4] = 3;
        else
            bytes[8] ^= 1;
        std::ofstream(path, std::ios::binary) << bytes;
        std::vector<std::string> errors;
        EXPECT_FALSE(P::WorldSaveService::validate_save(save, &errors));
        ASSERT_FALSE(errors.empty());
        EXPECT_EQ(errors.front(),
                  future ? Luminumbra::world::ActiveRegionLedger::kFutureMessage
                         : Luminumbra::world::ActiveRegionLedger::kCorruptMessage);
        EXPECT_FALSE(owner.SaveWorldState());
        EXPECT_EQ(ReadClockTestFile(path), bytes);
        GameSession refused;
        refused.SetRootPath(root.root_string());
        refused.SetJobSystem(&jobs);
        refused.SetActiveRegionsEnabled(true);
        EXPECT_FALSE(refused.LoadWorld(owner.GetMetadata().worldId));
        EXPECT_EQ(refused.GetWorldOpenError(), errors.front());
    }
    std::ofstream(path, std::ios::binary) << good;
    GameSession disabled;
    disabled.SetActiveRegionsEnabled(false); // Exercise the incompatible-config refusal.
    disabled.SetRootPath(root.root_string());
    disabled.SetJobSystem(&jobs);
    EXPECT_FALSE(disabled.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_NE(disabled.GetWorldOpenError().find("Incompatible configuration"), std::string::npos);
    ASSERT_TRUE(P::WorldSaveService::remove_member(save, "chunks/region/field-pages.fpg"));
    ASSERT_TRUE(P::WorldSaveService::remove_member(save, "chunks/region/active-regions.arl"));
    Luminumbra::world::ActiveRegionLedger empty;
    EXPECT_TRUE(P::WorldSaveService::load_active_regions(empty, save));
    EXPECT_TRUE(empty.records().empty());
    EXPECT_TRUE(P::WorldSaveService::validate_save(save));
    GameSession legacy;
    legacy.SetRootPath(root.root_string());
    legacy.SetJobSystem(&jobs);
    legacy.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(legacy.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_TRUE(legacy.GetActiveRegionLedger().records().empty());
    EXPECT_EQ(legacy.GetSimulationTickCount(), 1u);
}

TEST(GameSessionHeadlessWorldTest, VoxelGroundObjectAndPinInputsActivateOnlyTheirRegions) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Edit activation", "1337", "default"));
    auto* world = session.GetWorldSystem();
    // Adopt an all-air lattice at the far edit, without render-driven activation.
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(320, 0, -320));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->sdf_data.assign(17 * 17 * 17, 1.0f);
    ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
    ASSERT_GT(world->EditTerrainVoxel(Luminumbra::Vec3(5128, 8, -5112), 1, true, nullptr), 0);
    EXPECT_TRUE(session.GetActiveRegionLedger().records().at({10, -10}).edited);
    session.NotifyGroundObjectEdit(Luminumbra::Vec3(-5121, 0, 5121));
    EXPECT_TRUE(session.GetActiveRegionLedger().records().at({-11, 10}).edited);
    session.PinActiveRegion({20, 20}, true);
    EXPECT_TRUE(session.GetActiveRegionLedger().records().at({20, 20}).pinned);
    EXPECT_EQ(session.GetActiveRegionLedger().records().size(), 3u);
}

TEST(GameSessionHeadlessWorldTest, ReplayScheduleCompanionRoundTripsAndRefusesInvalidTraces) {
    const HeadlessRoot root;
    const auto path = root.path() / "recording.lrec";
    using namespace Luminumbra::Replay;
    const RegionScheduleTrace trace{{1, "0123456789abcdef"}, {2, "fedcba9876543210"}};
    RegionScheduleTrace restored;
    std::string error;
    EXPECT_FALSE(ReadRegionScheduleTrace(path, 2, restored, error));
    EXPECT_EQ(error, "Missing region schedule trace.");
    ASSERT_TRUE(WriteRegionScheduleTrace(path, trace, error));
    ASSERT_TRUE(ReadRegionScheduleTrace(path, 2, restored, error));
    EXPECT_EQ(restored, trace);
    EXPECT_FALSE(ReadRegionScheduleTrace(path, 3, restored, error));
    EXPECT_EQ(error, "Corrupt region schedule trace.");
    const auto companion = path.string() + ".regions.json";
    const auto good = nlohmann::json::parse(ReadClockTestFile(companion));
    auto value = good;
    value["ticks"][0]["digest"] = "0000000000000000";
    std::ofstream(companion, std::ios::binary) << value.dump();
    EXPECT_FALSE(ReadRegionScheduleTrace(path, 2, restored, error));
    EXPECT_EQ(error, "Corrupt region schedule trace.");
    EXPECT_EQ(restored, trace);
    value = good;
    value["schema"] = "luminumbra.region_schedule_trace.v2";
    std::ofstream(companion, std::ios::binary) << value.dump();
    EXPECT_FALSE(ReadRegionScheduleTrace(path, 2, restored, error));
    EXPECT_EQ(error, "Unsupported future region schedule trace version.");
    EXPECT_FALSE(WriteRegionScheduleTrace(path, {{2, "0123456789abcdef"}}, error));
}

TEST(GameSessionHeadlessWorldTest, InvalidHostInputsNeverThrowAndAreCounted) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetRegionSchedulerConfig({0, 2, 1});
    ASSERT_TRUE(session.CreateTransientWorld("Invalid inputs", "1337", "default"));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_NO_THROW(session.SetLocalPlayerSimulationPosition(Luminumbra::Vec3(nan, 0, 0), true));
    EXPECT_NO_THROW(session.NotifyGroundObjectEdit(Luminumbra::Vec3(inf, 0, 0)));
    EXPECT_NO_THROW(session.PinActiveRegion({40000, 0}, true));
    EXPECT_EQ(session.GetRegionInputRejections().anchors, 1u);
    EXPECT_EQ(session.GetRegionInputRejections().edits, 1u);
    EXPECT_EQ(session.GetRegionInputRejections().pins, 1u);
    EXPECT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_TRUE(session.GetActiveRegionLedger().records().empty());
    session.SetReplicatedSimulationAnchors(
        {Luminumbra::Vec3(nan, 0, 0), Luminumbra::Vec3(256, 0, 256)});
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(session.GetRegionSchedule().rejected.anchors, 1u);
    EXPECT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(session.GetActiveRegionLedger().records().count({0, 0}), 1u);
    const Luminumbra::Vec3 walking(256, 0, 256);
    session.SetLocalPlayerSimulationPosition(walking, true);
    session.SetLocalPlayerSimulationPosition(Luminumbra::Vec3(nan, 0, 0), true);
    EXPECT_EQ(session.GetActiveRegionLedger().local_anchor(),
              std::optional<Luminumbra::Vec3>(walking));
    EXPECT_EQ(session.GetRegionInputRejections().anchors, 2u);
}

namespace {
constexpr Luminumbra::world::RegionKey kFaultRegion{40, -40};
constexpr Luminumbra::world::RegionKey kStormRegion{-1, 0};

// Arms a scheduler fault at tick k. Keep the pinned far region awake through k-2,
// then publish over-limit work in completed ticks k-2 and k-1: Active->Reduced
// at k-1, then Frozen at k, where the durable digest provider reads the corrupt
// region file and throws.
void ArmSchedulerFault(GameSession& session,
                       const HeadlessRoot& root,
                       JobSystem& jobs,
                       std::uint64_t k) {
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetRegionSchedulerConfig({1, 1, 1});
    ASSERT_TRUE(session.CreateWorld("Scheduler fault", "1337", "default"));
    session.PinActiveRegion(kFaultRegion, true);
    // Seed 1337 spawns a storm here at weather age 45. Keep this owner due even
    // when the fault region skips tick 45 after becoming Reduced.
    session.PinActiveRegion(kStormRegion, true);
    // Admit only the two pinned owners, without proximity neighbors.
    session.SetReplicatedSimulationAnchors({});
    const auto region = P::WorldSaveService::region_file_path(
        session.GetWorldSaveDir(), kFaultRegion.x, kFaultRegion.z);
    fs::create_directories(region.parent_path());
    std::ofstream(region, std::ios::binary) << "not a region file";
    session.GetSimulationEventBus().subscribe([&session, k](const auto& event) {
        // Wake-pending owners are exempt from budget transitions. Keep weather
        // due through the spawn epoch without taking either fault transition.
        session.PinActiveRegion(kStormRegion, true);
        // Field initialization/evolution also publishes pressure. Re-pinning keeps
        // this owner Active through k-2 without a proximity visit admitting neighbors
        // that could consume the intended two-decision fault window.
        if (event.tick < k - 2)
            session.PinActiveRegion(kFaultRegion, true);
        if (event.tick == k - 2) {
            session.SetReplicatedSimulationAnchors({});
            session.RecordActiveRegionWork({kFaultRegion, 5, 0, 0, 0});
        } else if (event.tick == k - 1)
            session.RecordActiveRegionWork({kFaultRegion, 5, 0, 0, 0});
    });
    for (std::uint64_t t = 1; t <= k + 1; ++t)
        session.GetSimulationEventBus().publish(t, "work", "");
}

// The corrupt durable file only exists to make the digest provider throw. A
// save validates every region file, so restore a clean miss before saving.
void RemoveFaultRegionFile(const GameSession& session) {
    std::error_code error;
    EXPECT_TRUE(fs::remove(P::WorldSaveService::region_file_path(
                               session.GetWorldSaveDir(), kFaultRegion.x, kFaultRegion.z),
                           error));
    EXPECT_FALSE(error) << error.message();
}

// The session clock produces at most two ticks per frame. Single-tick frames run
// ticks 1..start-1, then one two-tick frame starts at tick `start` (k or k-1), so
// the fault lands on the first or the second tick of that frame.
void RunSchedulerFaultCase(std::uint64_t k, std::uint64_t start) {
    SCOPED_TRACE("k=" + std::to_string(k) + " start=" + std::to_string(start));
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    ASSERT_NO_FATAL_FAILURE(ArmSchedulerFault(session, root, jobs, k));
    loaded.SetRootPath(root.root_string());
    loaded.SetJobSystem(&jobs);
    loaded.SetActiveRegionsEnabled(true);
    loaded.SetRegionSchedulerConfig({1, 1, 1});
    const auto dt = session.GetSimulationClock().fixed_dt();
    for (std::uint64_t t = 1; t < start; ++t)
        ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
    // Half a tick of slack keeps the clock from rounding the frame down. A fault
    // on the first tick of the frame stops the second tick from running.
    ASSERT_EQ(session.TickSimulation(2.5 * dt), k - start + 1);
    const auto fault = session.GetRegionSchedulerFault();
    ASSERT_TRUE(fault.has_value());
    if (!fault.has_value())
        return;
    if (fault.has_value()) {
        const auto snapshot = fault.value();
        EXPECT_EQ(snapshot.tick, k);
        EXPECT_FALSE(snapshot.message.empty());
    }
    EXPECT_EQ(session.GetSimulationTickCount(), k);
    EXPECT_LT(session.GetActiveRegionLedger().tick(), k);
    EXPECT_EQ(session.GetActiveRegionLedger().header_tick(), k);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.TickSimulation(dt), 0u);
    EXPECT_EQ(session.GetSimulationTickCount(), k);
    if (k == 46)
        ASSERT_FALSE(session.GetWeatherSystem()->StormCells().empty());
    const auto save = session.GetWorldSaveDir();
    RemoveFaultRegionFile(session);
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetSimulationTickCount(), k);
    EXPECT_FALSE(loaded.GetRegionSchedulerFault().has_value());
    if (k == 46)
        ASSERT_FALSE(loaded.GetWeatherSystem()->StormCells().empty());
    const auto saved_hashes = SessionWorldSubHashes(session);
    const auto loaded_hashes = SessionWorldSubHashes(loaded);
    for (const auto& [field, hash] : saved_hashes.items())
        EXPECT_EQ(loaded_hashes.at(field), hash) << field;
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(session));
}
} // namespace

TEST(GameSessionHeadlessWorldTest, SchedulerFaultStopsTheFrameAndLeavesASavableConsistentSession) {
    for (const auto& [k, start] : {std::pair<std::uint64_t, std::uint64_t>{3, 3},
                                   std::pair<std::uint64_t, std::uint64_t>{3, 2},
                                   std::pair<std::uint64_t, std::uint64_t>{4, 4},
                                   // Cross the first storm-spawn epoch so reconstruction
                                   // also covers storm history when the next tick faults.
                                   std::pair<std::uint64_t, std::uint64_t>{46, 46},
                                   std::pair<std::uint64_t, std::uint64_t>{46, 45}}) {
        ASSERT_NO_FATAL_FAILURE(RunSchedulerFaultCase(k, start));
    }
}

TEST(GameSessionHeadlessWorldTest, ThrowingDigestProviderWithQueuedWorkLeavesNoStaleWork) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(ArmSchedulerFault(session, root, jobs, 3));
    const auto dt = session.GetSimulationClock().fixed_dt();
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_EQ(session.TickSimulation(2.5 * dt), 1u);
    ASSERT_TRUE(session.GetRegionSchedulerFault().has_value());
    if (!session.GetRegionSchedulerFault().has_value())
        return;
    session.RecordActiveRegionWork({kFaultRegion, 5, 0, 0, 0});
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.TickSimulation(dt), 0u);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    RemoveFaultRegionFile(session);
    EXPECT_TRUE(session.SaveWorldState());
}

TEST(GameSessionHeadlessWorldTest, CorruptLedgerLoadPreservesTheLiveWorld) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, victim;
    for (auto* s : {&owner, &victim}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(owner.CreateWorld("Corrupt ledger", "1337", "default"));
    ASSERT_EQ(owner.TickSimulation(owner.GetSimulationClock().fixed_dt()), 1u);
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Meshing);
    chunk->heightmap_data = {8.0f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto path = P::WorldSaveService::active_regions_path(save);
    auto bytes = ReadClockTestFile(path);
    bytes[8] ^= 1;
    std::ofstream(path, std::ios::binary) << bytes;
    ASSERT_TRUE(victim.CreateWorld("Victim", "2", "default"));
    auto resident = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    resident->set_state(Luminumbra::ChunkState::Meshing);
    resident->heightmap_data = {9.0f};
    resident->mark_voxel_data_dirty();
    ASSERT_TRUE(victim.GetWorldSystem()->adopt_streamed_chunk(resident));
    ASSERT_FALSE(victim.GetWorldSystem()->snapshot_streamed_chunks().empty());
    EXPECT_FALSE(victim.LoadWorldStateFrom(save));
    EXPECT_EQ(victim.GetWorldSystem()->snapshot_streamed_chunks(),
              std::vector<std::shared_ptr<Luminumbra::Chunk>>{resident});
    EXPECT_EQ(victim.GetWorldOpenError(), Luminumbra::world::ActiveRegionLedger::kCorruptMessage);
}

namespace {

// Controlled persistence fixtures keep the independent historical rules explicit.
// Shipping defaults are exercised by the direct session tests above.
void EnableSnapshotSession(GameSession& session,
                           const HeadlessRoot& root,
                           JobSystem& jobs,
                           bool aether_state = false) {
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetAetherStateEnabled(aether_state);
    session.SetWaterHighResEnabled(false);
    session.SetWeatherRainEnabled(false);
    session.SetWeatherEventsEnabled(false);
    session.SetFiniteHydrologyEnabled(false);
}

} // namespace

TEST(GameSessionHeadlessWorldTest, SnapshotFlagOnSaveWritesIndexAndMarkerAndFlagOffDoesNot) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Snapshot on", "1337", "default"));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    EXPECT_TRUE(P::snapshot_index_exists(save));
    EXPECT_TRUE(fs::exists(save / "chunks/region/snapshot.json"));
    EXPECT_FALSE(P::journal_pending(save));
    const auto info = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    ASSERT_TRUE(info.contains("snapshotFormat"));
    EXPECT_EQ(info.at("snapshotFormat"), 1);
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(save, index, nullptr));
    EXPECT_GE(index.generation, 1u);
    EXPECT_TRUE(std::any_of(
        index.members.begin(), index.members.end(), [](const P::SnapshotMember& member) {
            return member.path == "world_info.json" && member.present;
        }));

    const HeadlessRoot plain_root;
    GameSession plain;
    plain.SetRootPath(plain_root.root_string());
    plain.SetJobSystem(&jobs);
    plain.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(plain.CreateWorld("Snapshot off", "1337", "default"));
    ASSERT_TRUE(plain.SaveWorldState());
    const auto plain_save = plain.GetWorldSaveDir();
    EXPECT_FALSE(P::snapshot_index_exists(plain_save));
    EXPECT_FALSE(fs::exists(P::journal_directory(plain_save)));
    const auto plain_info =
        nlohmann::json::parse(ReadClockTestFile(plain_save / "world_info.json"));
    EXPECT_FALSE(plain_info.contains("snapshotFormat"));
}

TEST(GameSessionHeadlessWorldTest, SnapshotReloadRoundTripsAndEachSaveBumpsGeneration) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    EnableSnapshotSession(owner, root, jobs);
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Snapshot reload", "1337", "default"));
    const auto save = owner.GetWorldSaveDir();
    ASSERT_TRUE(owner.SaveWorldState());
    P::SnapshotIndex first;
    ASSERT_TRUE(P::read_snapshot_index(save, first, nullptr));
    ASSERT_EQ(owner.TickSimulation(owner.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(owner.SaveWorldState());
    P::SnapshotIndex second;
    ASSERT_TRUE(P::read_snapshot_index(save, second, nullptr));
    EXPECT_GT(second.generation, first.generation);
    EXPECT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId));
}

TEST(GameSessionHeadlessWorldTest, SnapshotMissingIndexWithMarkerIsRefused) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Missing index", "1337", "default"));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto worldId = owner.GetMetadata().worldId;
    ASSERT_TRUE(fs::remove(P::snapshot_index_path(save)));
    GameSession reopened;
    EnableSnapshotSession(reopened, root, jobs);
    EXPECT_FALSE(reopened.LoadWorld(worldId));
    EXPECT_NE(reopened.GetWorldOpenError().find("inconsistent snapshot"), std::string::npos);
}

TEST(GameSessionHeadlessWorldTest, SnapshotTamperedMemberIsRefused) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Tampered member", "1337", "default"));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto worldId = owner.GetMetadata().worldId;
    // Rewrite valid metadata behind the index: the member keeps its format but not its fingerprint.
    auto info = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    info["name"] = "tampered behind the index";
    std::ofstream(save / "world_info.json", std::ios::binary | std::ios::trunc) << info.dump(2);
    GameSession refused;
    EnableSnapshotSession(refused, root, jobs);
    EXPECT_FALSE(refused.LoadWorld(worldId));
    EXPECT_NE(refused.GetWorldOpenError().find("inconsistent snapshot"), std::string::npos);
    ASSERT_TRUE(P::WorldSaveService::forget_snapshot_index_for_testing(save));
    GameSession legacy;
    EnableSnapshotSession(legacy, root, jobs);
    (void)legacy.LoadWorld(worldId);
    EXPECT_NE(legacy.GetWorldOpenError().find("inconsistent snapshot"), std::string::npos);
}

TEST(GameSessionHeadlessWorldTest,
     SnapshotLegacyWorldWithoutIndexOrMarkerLoadsAndUpgradesOnFirstFlagOnSave) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    owner.SetRootPath(root.root_string());
    owner.SetJobSystem(&jobs);
    owner.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(owner.CreateWorld("Legacy upgrade", "1337", "default"));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto worldId = owner.GetMetadata().worldId;
    EXPECT_FALSE(P::snapshot_index_exists(save));
    GameSession legacy;
    EnableSnapshotSession(legacy, root, jobs);
    ASSERT_TRUE(legacy.LoadWorld(worldId));
    ASSERT_TRUE(legacy.SaveWorldState());
    EXPECT_TRUE(P::snapshot_index_exists(save));
    const auto info = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    ASSERT_TRUE(info.contains("snapshotFormat"));
    EXPECT_EQ(info.at("snapshotFormat"), 1);
}

TEST(GameSessionHeadlessWorldTest, SnapshotPendingJournalIsRolledBackAtOpen) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Pending journal", "1337", "default"));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto worldId = owner.GetMetadata().worldId;
    const auto info_bytes = ReadClockTestFile(save / "world_info.json");
    // Stage a PUBLISHED transaction: protect the metadata (journal + undo copy), keep a copy of
    // the journal, roll the transaction back, then modify the metadata and restore the journal.
    // The directory now looks like a crash between "member replaced" and "index published".
    const auto side = root.path() / "journal-copy";
    {
        P::SaveTransaction txn(save, 0, {}, &P::WorldSaveService::member_digest);
        ASSERT_TRUE(txn.begin(nullptr));
        ASSERT_TRUE(txn.protect({"world_info.json"}, nullptr));
        fs::copy(P::journal_directory(save), side, fs::copy_options::recursive);
        txn.abort(nullptr);
    }
    ASSERT_FALSE(P::journal_pending(save));
    auto info = nlohmann::json::parse(info_bytes);
    info["name"] = "half written";
    std::ofstream(save / "world_info.json", std::ios::binary | std::ios::trunc) << info.dump(2);
    fs::copy(side, P::journal_directory(save), fs::copy_options::recursive);
    const auto entry = Luminumbra::Persistence::InspectSavedWorld(root.path(), worldId);
    EXPECT_TRUE(entry.pending_recovery);
    EXPECT_TRUE(entry.error.empty());
    EXPECT_TRUE(P::journal_pending(save));
    GameSession reopened;
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(worldId));
    EXPECT_FALSE(fs::exists(P::journal_directory(save)));
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), info_bytes);
}

TEST(GameSessionHeadlessWorldTest, SnapshotSaveDecodesTheWorldAtMostOnce) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Decode once", "1337", "default"));
    const auto dt = owner.GetSimulationClock().fixed_dt();
    for (int tick = 0; tick < 3; ++tick)
        ASSERT_EQ(owner.TickSimulation(dt), 1u);
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Meshing);
    chunk->heightmap_data = {8.0f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_TRUE(owner.SaveWorldState());
    chunk->mark_voxel_data_dirty(); // the warm-up save cleared the dirty flag
    const auto before = P::WorldSaveService::decode_count_for_testing();
    ASSERT_TRUE(owner.SaveWorldState());
    const auto after = P::WorldSaveService::decode_count_for_testing();
    EXPECT_LE(after - before, std::uint64_t{1});
}

namespace {

constexpr std::uint32_t kGroundType = 7;
const Luminumbra::world::RegionKey kGroundRegion{0, -1};
// Far from the spawn anchor's visitation disc, so a budget-limited run can freeze it.
const Luminumbra::world::RegionKey kFarGroundRegion{10, -3};

using GroundEvents = std::vector<std::pair<std::string, std::string>>;

void CreateGroundWorld(GameSession& session,
                       const HeadlessRoot& root,
                       JobSystem& jobs,
                       const char* name,
                       bool aether_state = false) {
    EnableSnapshotSession(session, root, jobs, aether_state);
    ASSERT_TRUE(session.CreateWorld(name, "1337", "default"));
}

// Spawns a ground object at (x_mm, 0, z_mm) with the fixed test type and payload.
std::uint64_t SpawnGround(GameSession& session,
                          std::int64_t x_mm,
                          std::int64_t z_mm,
                          std::uint64_t despawn_after = 0) {
    return session.SpawnGroundObject(kGroundType, x_mm, 0, z_mm, despawn_after, 0);
}

void TickOnce(GameSession& session) {
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
}

void CollectGroundEvents(GameSession& session, GroundEvents& seen) {
    session.GetSimulationEventBus().subscribe(
        [&seen](const auto& event) { seen.emplace_back(event.topic, event.payload); });
}

std::size_t CountTopic(const GroundEvents& seen, const std::string& topic) {
    return static_cast<std::size_t>(std::count_if(
        seen.begin(), seen.end(), [&topic](const auto& event) { return event.first == topic; }));
}

std::string FirstPayload(const GroundEvents& seen, const std::string& topic) {
    for (const auto& event : seen)
        if (event.first == topic)
            return event.second;
    return {};
}

std::size_t CountRegularFiles(const fs::path& dir) {
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir))
        if (entry.is_regular_file())
            ++count;
    return count;
}

void ExpectSameSaveTree(const fs::path& actual, const fs::path& expected) {
    EXPECT_EQ(CountRegularFiles(actual), CountRegularFiles(expected));
    for (const auto& entry : fs::recursive_directory_iterator(actual)) {
        if (!entry.is_regular_file())
            continue;
        const auto relative = fs::relative(entry.path(), actual);
        EXPECT_EQ(ReadClockTestFile(entry.path()), ReadClockTestFile(expected / relative))
            << relative.string();
    }
}

} // namespace

TEST(GameSessionHeadlessWorldTest, FlagOffSpawnIsInertAndLeavesSaveHashAndRejectionsUnchanged) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, expected;
    for (auto* s : {&session, &expected}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(false);
        ASSERT_TRUE(s->CreateWorld("Ground off", "1337", "default"));
        ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(*s, 0));
    }
    EXPECT_EQ(SpawnGround(session, 5200, -1500), 0u);
    EXPECT_EQ(SpawnGround(session, 5300, -1500), 0u);
    EXPECT_TRUE(session.GetGroundObjects().empty());
    for (int i = 0; i < 7; ++i) {
        ASSERT_NO_FATAL_FAILURE(TickOnce(session));
        ASSERT_NO_FATAL_FAILURE(TickOnce(expected));
    }
    const auto save = root.path() / "ground-off-save";
    const auto twin = root.path() / "ground-off-twin";
    ASSERT_TRUE(session.SaveWorldStateTo(save));
    ASSERT_TRUE(expected.SaveWorldStateTo(twin));
    ExpectSameSaveTree(save, twin);
    ExpectSameWorldHash(session, expected);
    const auto hashes = SessionWorldSubHashes(session);
    const auto expected_hashes = SessionWorldSubHashes(expected);
    EXPECT_EQ(hashes.at("ecology"), expected_hashes.at("ecology"));
    EXPECT_EQ(session.GetRegionInputRejections().ground_spawns, 0u);
    EXPECT_EQ(session.GetRegionInputRejections().edits, expected.GetRegionInputRejections().edits);
}

TEST(GameSessionHeadlessWorldTest, FlagOnSpawnReturnsSequentialIdsAndPublishesGroundSpawned) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground spawn");
    GroundEvents seen;
    CollectGroundEvents(session, seen);
    EXPECT_EQ(SpawnGround(session, 5200, -1500), 1u);
    EXPECT_EQ(SpawnGround(session, 5300, -1500), 2u);
    EXPECT_EQ(SpawnGround(session, 5400, -1500), 3u);
    // Events published between ticks are delivered by the next drain.
    EXPECT_TRUE(seen.empty());
    TickOnce(session);
    EXPECT_EQ(CountTopic(seen, "ground.spawned"), 3u);
    EXPECT_EQ(FirstPayload(seen, "ground.spawned"), "1,7,5200,0,-1500");
    EXPECT_EQ(session.GetGroundObjects().size(), 3u);
}

TEST(GameSessionHeadlessWorldTest, RefusedGroundSpawnsCountRejectionsAndConsumeNoId) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground refused");
    EXPECT_EQ(SpawnGround(session, Luminumbra::world::GroundObjectStore::kMaxCoordinateMm, -1500),
              0u);
    EXPECT_EQ(session.SpawnGroundObject(0, 5200, 0, -1500, 0, 0), 0u);
    EXPECT_EQ(session.GetRegionInputRejections().ground_spawns, 2u);
    EXPECT_TRUE(session.GetGroundObjects().empty());
    EXPECT_EQ(SpawnGround(session, 5200, -1500), 1u);
}

TEST(GameSessionHeadlessWorldTest, SpawnMarksItsRegionEdited) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground edit");
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    const auto& records = session.GetActiveRegionLedger().records();
    ASSERT_EQ(records.count(kGroundRegion), 1u);
    EXPECT_TRUE(records.at(kGroundRegion).edited);
    EXPECT_EQ(records.size(), 1u);
}

TEST(GameSessionHeadlessWorldTest, DespawnAfterRemovesObjectAndPublishesGroundDespawned) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground despawn");
    GroundEvents seen;
    CollectGroundEvents(session, seen);
    const auto id = SpawnGround(session, 5200, -1500, 3);
    ASSERT_EQ(id, 1u);
    TickOnce(session);
    TickOnce(session);
    ASSERT_EQ(session.GetGroundObjects().objects().count(id), 1u);
    EXPECT_EQ(session.GetGroundObjects().objects().at(id).active_age, 2u);
    TickOnce(session);
    EXPECT_TRUE(session.GetGroundObjects().objects().empty());
    // The due update publishes the despawn before this tick's ordered drain.
    EXPECT_EQ(CountTopic(seen, "ground.despawned"), 1u);
    EXPECT_EQ(FirstPayload(seen, "ground.despawned"), "1,7,5200,0,-1500");
}

TEST(GameSessionHeadlessWorldTest, FrozenRegionDoesNotAgeItsGroundObjects) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    session.SetRegionSchedulerConfig({0, 2, 1});
    ASSERT_TRUE(session.CreateWorld("Ground frozen", "1337", "default"));
    const auto id = SpawnGround(session, 5'200'000, -1'500'000);
    ASSERT_EQ(id, 1u);
    session.SetReplicatedSimulationAnchors({}); // No proximity protection for this pressure case.
    std::size_t frozen_ticks = 0;
    for (int i = 0; i < 10; ++i) {
        const auto before = session.GetGroundObjects().objects().at(id).active_age;
        session.RecordActiveRegionWork({kFarGroundRegion, 100, 0, 0, 0});
        TickOnce(session);
        ASSERT_GE(session.GetActiveRegionLedger().records().at(kFarGroundRegion).pressure, 100u);
        // Frozen after a tick means it was not due in that tick, so its objects did not age.
        if (session.GetActiveRegionLedger().records().at(kFarGroundRegion).state ==
            Luminumbra::world::RegionState::Frozen) {
            ++frozen_ticks;
            EXPECT_EQ(session.GetGroundObjects().objects().at(id).active_age, before)
                << "tick " << i;
        }
    }
    EXPECT_GT(frozen_ticks, 0u);
}

TEST(GameSessionHeadlessWorldTest, MetadataOnlySaveAfterSpawnWritesObjectFileAndLedger) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground metadata");
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    ASSERT_EQ(SpawnGround(session, 5300, -1500), 2u);
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::ground_objects_path(save)),
              session.GetGroundObjects().serialize());
    EXPECT_TRUE(fs::exists(P::WorldSaveService::active_regions_path(save)));
}

TEST(GameSessionHeadlessWorldTest, ReloadedSessionRestoresGroundObjectsAndContinuesIds) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Ground reload");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500), 1u);
    ASSERT_EQ(SpawnGround(owner, 5300, -1500), 2u);
    ASSERT_EQ(SpawnGround(owner, 5400, -1500), 3u);
    ASSERT_TRUE(owner.SaveWorld());
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetGroundObjects().serialize(), owner.GetGroundObjects().serialize());
    EXPECT_EQ(loaded.GetGroundObjects().size(), 3u);
    EXPECT_EQ(SpawnGround(loaded, 5500, -1500), 4u);
}

TEST(GameSessionHeadlessWorldTest, ResumedSessionMatchesUninterruptedGroundObjectsAndHash) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, uninterrupted, loaded;
    CreateGroundWorld(original, root, jobs, "Ground resume");
    CreateGroundWorld(uninterrupted, root, jobs, "Ground twin");
    for (auto* s : {&original, &uninterrupted}) {
        ASSERT_EQ(SpawnGround(*s, 5200, -1500), 1u);
        ASSERT_EQ(SpawnGround(*s, -600000, 900000), 2u);
    }
    for (int i = 0; i < 3; ++i) {
        TickOnce(original);
        TickOnce(uninterrupted);
    }
    ASSERT_TRUE(original.SaveWorldState());
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    for (int i = 0; i < 5; ++i) {
        TickOnce(uninterrupted);
        TickOnce(loaded);
        EXPECT_EQ(loaded.GetRegionSchedule(), uninterrupted.GetRegionSchedule());
        EXPECT_EQ(loaded.GetGroundObjects().objects(), uninterrupted.GetGroundObjects().objects());
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(uninterrupted));
    }
    EXPECT_EQ(loaded.GetGroundObjects().objects().at(1).active_age, 8u);
}

TEST(GameSessionHeadlessWorldTest, CorruptGroundObjectsFileRefusesToOpen) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, control, victim;
    CreateGroundWorld(owner, root, jobs, "Ground corrupt");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500), 1u);
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    EnableSnapshotSession(control, root, jobs);
    ASSERT_TRUE(control.LoadWorld(owner.GetMetadata().worldId)) << control.GetWorldOpenError();
    std::ofstream(P::WorldSaveService::ground_objects_path(save), std::ios::binary) << "not json\n";
    EnableSnapshotSession(victim, root, jobs);
    EXPECT_FALSE(victim.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_FALSE(victim.GetWorldOpenError().empty());
    EXPECT_TRUE(victim.GetGroundObjects().empty());
}

TEST(GameSessionHeadlessWorldTest, GroundObjectsChangeTheWorldHash) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession bare, with;
    CreateGroundWorld(bare, root, jobs, "Ground hash");
    CreateGroundWorld(with, root, jobs, "Ground hash");
    const auto before = SessionWorldHash(with);
    EXPECT_EQ(SessionWorldHash(bare), before);
    ASSERT_EQ(SpawnGround(with, 5200, -1500), 1u);
    EXPECT_NE(SessionWorldHash(with), before);
}

TEST(GameSessionHeadlessWorldTest, FlagOnIndexedSaveWithGroundObjectsReloadsThroughSnapshot) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Ground snapshot");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500), 1u);
    ASSERT_EQ(SpawnGround(owner, 5300, -1500), 2u);
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    ASSERT_TRUE(P::snapshot_index_exists(save));
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(save, index, nullptr));
    EXPECT_TRUE(std::any_of(
        index.members.begin(), index.members.end(), [](const P::SnapshotMember& member) {
            return member.path == "chunks/region/ground-objects.json" && member.present;
        }));
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetGroundObjects().size(), 2u);
}

namespace {

fs::path FieldPagePath(const fs::path& save) {
    return save / Luminumbra::world::FieldPageState::kRelativePath;
}

} // namespace

TEST(GameSessionHeadlessWorldTest, FieldPagesFlagOffLeavesNoFileAndNoPages) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.CreateWorld("Field off", "1337", "default"));
    ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(session, 0));
    ASSERT_NO_FATAL_FAILURE(TickOnce(session));
    ASSERT_TRUE(session.SaveWorld());
    EXPECT_TRUE(session.GetFieldPages().empty());
    EXPECT_FALSE(fs::exists(FieldPagePath(session.GetWorldSaveDir())));
}

TEST(GameSessionHeadlessWorldTest, FieldPagesInitializeOnFirstDueTick) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Field init");
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    ASSERT_NO_FATAL_FAILURE(TickOnce(session));
    const auto& ledger = session.GetActiveRegionLedger().records();
    ASSERT_EQ(ledger.count(kGroundRegion), 1u);
    const auto& due = session.GetRegionSchedule().due;
    ASSERT_NE(std::find(due.begin(), due.end(), kGroundRegion), due.end());
    EXPECT_GT(session.GetFieldPages().owned_initialized_cells(kGroundRegion), 0u);
    EXPECT_TRUE(session.GetFieldPages().owns_region(kGroundRegion));
}

TEST(GameSessionHeadlessWorldTest, FieldPagesStepOncePerDueTickTowardTarget) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, twin;
    CreateGroundWorld(session, root, jobs, "Field step");
    CreateGroundWorld(twin, root, jobs, "Field step twin");
    for (auto* s : {&session, &twin})
        ASSERT_EQ(SpawnGround(*s, 5200, -1500), 1u);
    ASSERT_NO_FATAL_FAILURE(TickOnce(session));
    ASSERT_NO_FATAL_FAILURE(TickOnce(twin));
    std::string previous = session.GetFieldPages().region_bytes(kGroundRegion);
    ASSERT_FALSE(previous.empty());
    std::size_t changes = 0;
    for (int i = 0; i < 24; ++i) {
        TickOnce(session);
        TickOnce(twin);
        const auto current = session.GetFieldPages().region_bytes(kGroundRegion);
        if (current != previous)
            ++changes;
        previous = current;
        EXPECT_EQ(current, twin.GetFieldPages().region_bytes(kGroundRegion)) << "tick " << i;
    }
    // The target changes every 8 ticks (new epoch), so the cells must move at some point.
    EXPECT_GT(changes, 0u);
}

TEST(GameSessionHeadlessWorldTest, FieldPagesFrozenRegionDoesNotChange) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    session.SetRegionSchedulerConfig({0, 2, 1});
    ASSERT_TRUE(session.CreateWorld("Field frozen", "1337", "default"));
    ASSERT_EQ(SpawnGround(session, 5'200'000, -1'500'000), 1u);
    bool was_frozen = false;
    std::string previous_bytes;
    std::size_t consecutive_frozen_ticks = 0;
    for (int i = 0; i < 10; ++i) {
        TickOnce(session);
        const bool frozen = session.GetActiveRegionLedger().records().at(kFarGroundRegion).state ==
                            Luminumbra::world::RegionState::Frozen;
        const auto current = session.GetFieldPages().region_bytes(kFarGroundRegion);
        // A region that stays Frozen across a tick is not due in it, so its cells do not move.
        if (frozen && was_frozen) {
            EXPECT_EQ(current, previous_bytes) << "tick " << i;
            ++consecutive_frozen_ticks;
        }
        was_frozen = frozen;
        previous_bytes = current;
    }
    EXPECT_GT(consecutive_frozen_ticks, 0u);
    EXPECT_FALSE(session.GetFieldPages().empty());
}

TEST(GameSessionHeadlessWorldTest, FieldPagesSaveLoadContinueMatchesUninterruptedTwin) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, uninterrupted, loaded;
    CreateGroundWorld(original, root, jobs, "Field resume");
    CreateGroundWorld(uninterrupted, root, jobs, "Field twin");
    for (auto* s : {&original, &uninterrupted})
        ASSERT_EQ(SpawnGround(*s, 5200, -1500), 1u);
    for (int i = 0; i < 3; ++i) {
        TickOnce(original);
        TickOnce(uninterrupted);
    }
    ASSERT_FALSE(original.GetFieldPages().empty());
    ASSERT_TRUE(original.SaveWorldState());
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetFieldPages().serialize(), original.GetFieldPages().serialize());
    for (int i = 0; i < 5; ++i) {
        TickOnce(uninterrupted);
        TickOnce(loaded);
        EXPECT_EQ(loaded.GetRegionSchedule(), uninterrupted.GetRegionSchedule());
        EXPECT_EQ(loaded.GetFieldPages().serialize(), uninterrupted.GetFieldPages().serialize());
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(uninterrupted));
    }
}

TEST(GameSessionHeadlessWorldTest, FieldPagesMetadataOnlySaveWritesFile) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Field metadata");
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    ASSERT_NO_FATAL_FAILURE(TickOnce(session));
    ASSERT_FALSE(session.GetFieldPages().empty());
    ASSERT_TRUE(session.SaveWorld());
    const auto path = FieldPagePath(session.GetWorldSaveDir());
    ASSERT_TRUE(fs::exists(path));
    EXPECT_EQ(ReadClockTestFile(path), session.GetFieldPages().serialize());
}

TEST(GameSessionHeadlessWorldTest, FieldPagesCorruptFileRefusesToOpen) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, control, victim;
    CreateGroundWorld(owner, root, jobs, "Field corrupt");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500), 1u);
    ASSERT_NO_FATAL_FAILURE(TickOnce(owner));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    EnableSnapshotSession(control, root, jobs);
    ASSERT_TRUE(control.LoadWorld(owner.GetMetadata().worldId)) << control.GetWorldOpenError();
    std::ofstream(FieldPagePath(save), std::ios::binary) << "FPG1";
    EnableSnapshotSession(victim, root, jobs);
    EXPECT_FALSE(victim.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_FALSE(victim.GetWorldOpenError().empty());
    EXPECT_TRUE(victim.GetFieldPages().empty());
}

TEST(GameSessionHeadlessWorldTest, FieldPagesChangeTheWorldHash) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Field hash");
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    const auto before = SessionWorldHash(session);
    ASSERT_NO_FATAL_FAILURE(TickOnce(session));
    ASSERT_FALSE(session.GetFieldPages().empty());
    EXPECT_NE(SessionWorldHash(session), before);
}

TEST(GameSessionHeadlessWorldTest, FieldPagesAreListedInTheSnapshotIndex) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Field snapshot");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500), 1u);
    ASSERT_NO_FATAL_FAILURE(TickOnce(owner));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    ASSERT_TRUE(P::snapshot_index_exists(save));
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(save, index, nullptr));
    EXPECT_TRUE(std::any_of(
        index.members.begin(), index.members.end(), [](const P::SnapshotMember& member) {
            return member.path == "chunks/region/field-pages.fpg" && member.present;
        }));
}

TEST(GameSessionHeadlessWorldTest, FieldPagesAndGroundObjectsBothSchedule) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Field ground");
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    ASSERT_NO_FATAL_FAILURE(TickOnce(session));
    ASSERT_EQ(SpawnGround(session, 5300, -1500), 2u);
    ASSERT_NO_FATAL_FAILURE(TickOnce(session));
    EXPECT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_FALSE(session.GetGroundObjects().empty());
    EXPECT_FALSE(session.GetFieldPages().empty());
    EXPECT_TRUE(session.GetFieldPages().owns_region(kGroundRegion));
}

TEST(GameSessionHeadlessWorldTest, GroundSpawnQuantizesAndProbesEditedTerrainBeforePublishing) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Edited ground probe");
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3{0, 0, 0});
    chunk->set_state(Luminumbra::ChunkState::Ready);
    constexpr int side = Luminumbra::CHUNK_SIZE_X + 1;
    chunk->sdf_data.resize(side * (Luminumbra::CHUNK_SIZE_Y + 1) * (Luminumbra::CHUNK_SIZE_Z + 1));
    for (int z = 0; z <= Luminumbra::CHUNK_SIZE_Z; ++z)
        for (int y = 0; y <= Luminumbra::CHUNK_SIZE_Y; ++y)
            for (int x = 0; x <= Luminumbra::CHUNK_SIZE_X; ++x)
                chunk->sdf_data[static_cast<std::size_t>(
                    x + side * (y + (Luminumbra::CHUNK_SIZE_Y + 1) * z))] =
                    static_cast<float>(y) - 4.0f;
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    const auto id = session.SpawnGroundObject(kGroundType, Luminumbra::Vec3{8, 12, 8}, 0, 0);
    ASSERT_NE(id, 0u);
    const auto* landed = session.GetGroundObjects().find(id);
    ASSERT_NE(landed, nullptr);
    EXPECT_TRUE(landed->settled());
    EXPECT_EQ(landed->y_mm, 4000);
    const auto contact = session.SpawnGroundObject(kGroundType, Luminumbra::Vec3{8, 4, 8}, 0, 0);
    ASSERT_NE(contact, 0u);
    const auto* touching = session.GetGroundObjects().find(contact);
    ASSERT_NE(touching, nullptr);
    EXPECT_TRUE(touching->settled());
    EXPECT_EQ(touching->y_mm, 4000);
    // The owner-thread query neither loads another column nor substitutes procedural terrain.
    const auto absent =
        session.SpawnGroundObject(kGroundType, Luminumbra::Vec3{320, 12, 320}, 0, 0);
    ASSERT_NE(absent, 0u);
    const auto* waiting = session.GetGroundObjects().find(absent);
    ASSERT_NE(waiting, nullptr);
    EXPECT_FALSE(waiting->settled());
    EXPECT_EQ(waiting->y_mm, 12000);
    const auto rounded =
        session.SpawnGroundObject(kGroundType, Luminumbra::Vec3{320.0005f, -0.0015f, 320}, 0, 0);
    ASSERT_NE(rounded, 0u);
    const auto* quantized = session.GetGroundObjects().find(rounded);
    ASSERT_NE(quantized, nullptr);
    // -0.0015f is slightly below the half-millimetre tie in binary32.
    EXPECT_EQ(quantized->y_mm, -2);
    EXPECT_EQ(session.SpawnGroundObject(kGroundType, Luminumbra::Vec3{16777216, 0, 0}, 0, 0), 0u);
    EXPECT_EQ(session.SpawnGroundObject(kGroundType, Luminumbra::Vec3{0, -1048576, 0}, 0, 0), 0u);
}

TEST(GameSessionHeadlessWorldTest, BoundedBootSaveAsTransfersDiskOnlyAuthorityAndFaultsOnLoss) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded, copied;
    for (auto* session : {&owner, &loaded, &copied})
        EnableSnapshotSession(*session, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Disk authority", "1337", "default"));
    const Luminumbra::IVec3 distant(1280, 0, -1280);
    auto chunk = std::make_shared<Luminumbra::Chunk>(distant);
    chunk->sdf_data.assign(17 * 17 * 17, -4.0f);
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    const auto before = owner.ComputeChunkStateHash();
    ASSERT_TRUE(owner.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_FALSE(loaded.GetWorldSystem()->find_streamed_chunk(distant));
    EXPECT_EQ(loaded.GetWorldSystem()->durable_chunk_inventory().size(), 1u);
    EXPECT_EQ(loaded.ComputeChunkStateHash(), before);
    const auto destination = root.path() / "disk-authority-copy";
    ASSERT_TRUE(loaded.SaveWorldStateTo(destination));
    // Metadata-only persistence to the named world must retain the disk-only backing source.
    ASSERT_TRUE(loaded.SaveWorld());
    EXPECT_EQ(loaded.ComputeChunkStateHash(), before);
    ASSERT_TRUE(copied.CreateTransientWorld("Copy", "1337", "default"));
    ASSERT_TRUE(copied.LoadWorldStateFrom(destination));
    EXPECT_FALSE(copied.GetWorldSystem()->find_streamed_chunk(distant));
    EXPECT_EQ(copied.ComputeChunkStateHash(), before);
    ASSERT_TRUE(copied.GetWorldSystem()->restore_authoritative_chunk(distant));
    const auto restored = copied.GetWorldSystem()->find_streamed_chunk(distant);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->sdf_data, chunk->sdf_data);
    const auto source_region = P::WorldSaveService::region_file_path(destination, 40, -40);
    ASSERT_TRUE(fs::remove(source_region));
    // A fresh bounded boot still refuses the lost distant member before adopting any chunk.
    GameSession refused;
    EnableSnapshotSession(refused, root, jobs);
    ASSERT_TRUE(refused.CreateTransientWorld("Refused", "1337", "default"));
    EXPECT_FALSE(refused.LoadWorldStateFrom(destination));
    EXPECT_TRUE(refused.GetWorldSystem()->snapshot_streamed_chunks().empty());
    // loaded remains disk-only at destination: rehydration latches a fault and cannot generate.
    EXPECT_TRUE(loaded.GetWorldSystem()->restore_authoritative_chunk(distant));
    EXPECT_TRUE(loaded.GetRegionSchedulerFault().has_value());
    EXPECT_FALSE(loaded.GetWorldSystem()->find_streamed_chunk(distant));
    EXPECT_EQ(loaded.GetWorldSystem()->EditTerrainVoxel({20488, 8, -20472}, 1, true, nullptr), 0);
    EXPECT_EQ(loaded.TickSimulation(loaded.GetSimulationClock().fixed_dt()), 0u);
}

namespace {
Luminumbra::Chunk*& ChunkRevisionDuringCommit() {
    static Luminumbra::Chunk* chunk = nullptr;
    return chunk;
}
void ChangeChunkAtIndexPublication(const char* step, const fs::path& path) {
    if (ChunkRevisionDuringCommit() && std::string(step) == "before-replace" &&
        path.filename() == "snapshot.json") {
        ChunkRevisionDuringCommit()->sdf_data.front() -= 1.0f;
        ChunkRevisionDuringCommit()->mark_voxel_data_dirty();
        ChunkRevisionDuringCommit()->mark_water_state_dirty();
    }
}
struct ChunkRevisionHook {
    explicit ChunkRevisionHook(Luminumbra::Chunk* chunk) {
        ChunkRevisionDuringCommit() = chunk;
        P::set_snapshot_step_hook_for_testing(&ChangeChunkAtIndexPublication);
    }
    ~ChunkRevisionHook() {
        P::set_snapshot_step_hook_for_testing(nullptr);
        ChunkRevisionDuringCommit() = nullptr;
    }
};
} // namespace

TEST(GameSessionHeadlessWorldTest, FirstAndIncrementalSavePreserveChangesAfterSerialization) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Revision acknowledgement", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->sdf_data.assign(17 * 17 * 17, -1.0f);
    chunk->mark_voxel_data_dirty();
    chunk->mark_water_state_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    for (const bool first : {true, false}) {
        SCOPED_TRACE(first);
        const auto serialized_sdf = chunk->sdf_data;
        {
            ChunkRevisionHook hook(chunk.get());
            ASSERT_TRUE(owner.SaveWorldState());
        }
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        EXPECT_TRUE(chunk->is_water_state_dirty());
        const auto durable =
            P::WorldSaveService::read_chunk(owner.GetWorldSaveDir(), {0, 0, 0}, true);
        ASSERT_EQ(durable.status, P::WorldSaveService::ChunkReadStatus::Found);
        ASSERT_NE(durable.chunk, nullptr);
        EXPECT_EQ(durable.chunk->sdf_data, serialized_sdf);
        EXPECT_NE(chunk->sdf_data, serialized_sdf);
    }
    ASSERT_TRUE(owner.SaveWorldState());
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    EXPECT_FALSE(chunk->is_water_state_dirty());
}

namespace {
using Luminumbra::Chunk;
using Luminumbra::ChunkID;
using Luminumbra::ChunkSdfProvenance;
using Luminumbra::world::ActiveRegionLedger;
using Luminumbra::world::ActiveRegionRecord;
using Luminumbra::world::RegionKey;
using Luminumbra::world::RegionState;
struct RegionalWaterScenario {
    // WaterSystem retains this map's address; destroy the session before the map.
    std::unordered_map<ChunkID, std::shared_ptr<Chunk>> water_chunks;
    GameSession session;
};

void SeedRegionalWaterFixture(RegionalWaterScenario& scenario,
                              const std::shared_ptr<Luminumbra::Chunk>& chunk,
                              RegionKey water_region,
                              RegionKey work_region,
                              const Luminumbra::Vec3& visit_position,
                              std::uint64_t empty_digest,
                              std::vector<std::int32_t>& generated_depth) {
    auto& session = scenario.session;
    scenario.water_chunks.emplace(chunk->get_id(), chunk);
    const auto before_initialization = P::WorldSaveService::region_simulation_digest(
        {},
        water_region,
        P::RegionDigestInputs{.live = {chunk},
                              .parked = {},
                              .durable = {},
                              .ground_object_bytes = {},
                              .field_page_bytes = {}});
    // A legacy generator supplies the pristine baseline for this provenance fixture.
    // The enabled streaming caller only binds query storage; fixed ticks own evolution.
    Luminumbra::Systems::WaterSystem baseline_seed(nullptr, session.GetWorldSystem());
    baseline_seed.SetSimResolution(static_cast<int>(Luminumbra::Systems::WaterDetailLevel::Medium));
    baseline_seed.update(session.GetRegistry(), scenario.water_chunks);
    session.GetWaterSystem()->update(session.GetRegistry(), scenario.water_chunks);
    ASSERT_TRUE(chunk->has_water_sim.load());
    ASSERT_EQ(chunk->water_depth_mm.size(), 64u);
    ASSERT_EQ(chunk->water_bed_mm.size(), 64u);
    ASSERT_EQ(chunk->water_edge_flux.size(), 128u);
    ASSERT_EQ(chunk->sdf_provenance(), ChunkSdfProvenance::GeneratedCurrentParams);
    ASSERT_FALSE(chunk->is_water_state_dirty());
    ASSERT_FALSE(chunk->has_water_authority());
    ASSERT_FALSE(chunk->is_voxel_data_dirty());
    generated_depth = chunk->water_depth_mm;
    EXPECT_EQ(before_initialization, empty_digest);
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(
                  {},
                  water_region,
                  P::RegionDigestInputs{.live = {chunk},
                                        .parked = {},
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes = {}}),
              before_initialization);

    session.PinActiveRegion(work_region, true);
    session.SetReplicatedSimulationAnchors({visit_position});
    const auto dt = session.GetSimulationClock().fixed_dt();
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    const auto initial_records = session.GetActiveRegionLedger().records();
    const auto initial_record = initial_records.at(water_region);
    ASSERT_TRUE(initial_record.visited);
    ASSERT_FALSE(initial_record.pinned);
    ASSERT_FALSE(initial_record.edited);
    ASSERT_FALSE(initial_record.populated);
    ASSERT_EQ(initial_record.water_cursor, 0u);
    ASSERT_EQ(initial_record.state, RegionState::Active);
    ASSERT_GT(session.GetFieldPages().owned_initialized_cells(water_region), 0u);
    ASSERT_FALSE(chunk->has_water_authority());
}

void FreezeRegionalWaterFixture(RegionalWaterScenario& scenario,
                                const std::shared_ptr<Luminumbra::Chunk>& chunk,
                                bool displace,
                                RegionKey water_region,
                                RegionKey work_region,
                                const Luminumbra::Vec3& displacement_position,
                                const std::vector<std::int32_t>& generated_depth,
                                const ActiveRegionRecord& initial_record) {
    auto& session = scenario.session;
    const auto dt = session.GetSimulationClock().fixed_dt();
    if (displace) {
        session.GetWaterSystem()->update(session.GetRegistry(), scenario.water_chunks);
        session.GetWaterSystem()->apply_displacement(displacement_position, 4.0f);
        ASSERT_NE(chunk->water_depth_mm, generated_depth);
        ASSERT_TRUE(chunk->is_water_state_dirty());
        // The writer itself must leave promotion to the scheduling boundary under test.
        ASSERT_EQ(chunk->sdf_provenance(), ChunkSdfProvenance::GeneratedCurrentParams);
        ASSERT_FALSE(chunk->has_water_authority());
    }
    session.SetReplicatedSimulationAnchors({});
    session.RecordActiveRegionWork({work_region, 1, 0, 0, 0});
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(chunk->has_water_authority(), displace);
    EXPECT_EQ(chunk->sdf_provenance(),
              displace ? ChunkSdfProvenance::LoadedOrEdited
                       : ChunkSdfProvenance::GeneratedCurrentParams);
    EXPECT_EQ(chunk->is_voxel_data_dirty(), displace);
    EXPECT_FALSE(chunk->is_water_state_dirty());
    // K is still unfrozen when the first post-writer boundary has promoted its water.
    ASSERT_NE(session.GetActiveRegionLedger().records().at(water_region).state,
              RegionState::Frozen);

    // At most two budget transitions per unpinned record suffice; only J supplies work.
    const auto transition_count = 2 * session.GetActiveRegionLedger().records().size();
    for (std::size_t step = 0; step < transition_count; ++step) {
        session.RecordActiveRegionWork({work_region, 1, 0, 0, 0});
        ASSERT_EQ(session.TickSimulation(dt), 1u);
        ASSERT_FALSE(session.GetRegionSchedulerFault());
    }
    const auto frozen_record = session.GetActiveRegionLedger().records().at(water_region);
    ASSERT_EQ(frozen_record.state, RegionState::Frozen);
    EXPECT_FALSE(frozen_record.pinned);
    EXPECT_FALSE(frozen_record.edited);
    EXPECT_FALSE(frozen_record.populated);
    EXPECT_EQ(frozen_record.water_cursor, 0u);
    EXPECT_EQ(frozen_record.pressure, 0u);
    EXPECT_EQ(frozen_record.last_proximity, initial_record.last_proximity);
    EXPECT_EQ(frozen_record.frozen_digest,
              P::WorldSaveService::region_simulation_digest(
                  {},
                  water_region,
                  P::RegionDigestInputs{.live = {chunk},
                                        .parked = {},
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes =
                                            session.GetFieldPages().region_bytes(water_region)}));
}

void ExpectGeneratedWaterNeutral(const GameSession& session,
                                 const std::shared_ptr<Luminumbra::Chunk>& chunk,
                                 RegionKey water_region,
                                 std::uint64_t empty_digest,
                                 std::uint64_t generated_digest,
                                 const std::vector<std::int32_t>& generated_depth) {
    // Generated water contributes nothing to the chunk projection; fields still do.
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(
                  {},
                  water_region,
                  P::RegionDigestInputs{.live = {chunk},
                                        .parked = {},
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes = {}}),
              empty_digest);
    EXPECT_EQ(generated_digest,
              P::WorldSaveService::region_simulation_digest(
                  {},
                  water_region,
                  P::RegionDigestInputs{.live = {},
                                        .parked = {},
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes =
                                            session.GetFieldPages().region_bytes(water_region)}));
    EXPECT_EQ(chunk->water_depth_mm, generated_depth);
    EXPECT_FALSE(chunk->has_water_authority());
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
}

void AssertWaterRetentionRecords(GameSession& session,
                                 RegionKey water_region,
                                 RegionKey disk_region,
                                 const Luminumbra::IVec3& disk_coords,
                                 const ActiveRegionRecord& frozen_record,
                                 const ActiveRegionRecord& frozen_disk_record,
                                 const std::map<RegionKey, ActiveRegionRecord>& initial_records,
                                 bool reopened) {
    const auto& records = session.GetActiveRegionLedger().records();
    ASSERT_TRUE(records.contains(water_region));
    const auto& retained = records.at(water_region);
    EXPECT_EQ(retained.state, RegionState::Frozen);
    EXPECT_EQ(retained.frozen_digest, frozen_record.frozen_digest);
    EXPECT_EQ(retained.frozen_at, frozen_record.frozen_at);
    EXPECT_EQ(retained.last_proximity, frozen_record.last_proximity);
    EXPECT_FALSE(retained.pinned);
    EXPECT_FALSE(retained.edited);
    EXPECT_FALSE(retained.populated);
    EXPECT_EQ(retained.water_cursor, 0u);
    EXPECT_EQ(retained.pressure, 0u);
    // Due neighbours retain field authority; the never-due region uses inventory.
    EXPECT_EQ(records.size(), initial_records.size() + 1);
    for (const auto& [key, previous] : initial_records) {
        ASSERT_TRUE(records.contains(key));
        if (previous.visited)
            EXPECT_TRUE(session.GetFieldPages().owns_region(key));
    }
    const auto& disk_retained = records.at(disk_region);
    EXPECT_EQ(disk_retained.state, RegionState::Frozen);
    EXPECT_EQ(disk_retained.last_ticked, 0u);
    EXPECT_EQ(disk_retained.frozen_at, frozen_disk_record.frozen_at);
    EXPECT_EQ(disk_retained.frozen_digest, frozen_disk_record.frozen_digest);
    EXPECT_FALSE(disk_retained.visited);
    EXPECT_FALSE(disk_retained.pinned);
    EXPECT_FALSE(disk_retained.edited);
    EXPECT_FALSE(disk_retained.populated);
    EXPECT_EQ(disk_retained.water_cursor, 0u);
    EXPECT_EQ(disk_retained.pressure, 0u);
    EXPECT_FALSE(session.GetFieldPages().owns_region(disk_region));
    EXPECT_TRUE(session.GetFieldPages().region_bytes(disk_region).empty());
    EXPECT_FALSE(session.GetActiveRegionLedger().energy_ages().contains(disk_region));
    EXPECT_FALSE(session.GetActiveRegionLedger().completed_work().contains(disk_region));
    if (reopened) {
        EXPECT_EQ(session.GetWorldSystem()->find_streamed_chunk(disk_coords), nullptr);
        for (const auto& resident : session.GetWorldSystem()->snapshot_authoritative_chunks())
            EXPECT_NE(resident->get_coords(), disk_coords);
    }
}
struct FrozenWaterSnapshot {
    std::vector<std::int32_t> depth, bed, flux;
};

void AssertDurableWaterContinuation(GameSession& session,
                                    GameSession& reopened,
                                    const std::shared_ptr<Chunk>& chunk,
                                    const Luminumbra::IVec3& disk_coords,
                                    RegionKey water_region,
                                    const ActiveRegionRecord& frozen_record,
                                    const FrozenWaterSnapshot& expected) {
    EXPECT_GT(session.GetWorldClock().tick(),
              frozen_record.last_proximity + ActiveRegionLedger::kIdleEvictionTicks);
    ASSERT_EQ(reopened.GetWorldSystem()->find_streamed_chunk(chunk->get_coords()), nullptr);
    ASSERT_EQ(reopened.GetWorldSystem()->find_streamed_chunk(disk_coords), nullptr);
    const auto durable_chunk =
        reopened.GetWorldSystem()->find_authoritative_chunk(chunk->get_coords());
    ASSERT_NE(durable_chunk, nullptr);
    EXPECT_EQ(durable_chunk->sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
    EXPECT_EQ(durable_chunk->water_depth_mm, expected.depth);
    EXPECT_EQ(durable_chunk->water_bed_mm, expected.bed);
    EXPECT_EQ(durable_chunk->water_edge_flux, expected.flux);
    EXPECT_EQ(durable_chunk->is_water_sleeping.load(), chunk->is_water_sleeping.load());
    EXPECT_EQ(reopened.ComputeChunkStateHash(), session.ComputeChunkStateHash());
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(
                  session.GetWorldSaveDir(),
                  water_region,
                  P::RegionDigestInputs{
                      .live = {},
                      .parked = {},
                      .durable = reopened.GetWorldSystem()->durable_chunk_inventory(),
                      .ground_object_bytes = {},
                      .field_page_bytes = reopened.GetFieldPages().region_bytes(water_region)}),
              frozen_record.frozen_digest);
}

void PersistRegionalWaterBeforeRetention(GameSession& session,
                                         const std::shared_ptr<Chunk>& chunk,
                                         RegionKey work_region,
                                         RegionKey disk_region,
                                         const Luminumbra::IVec3& coords,
                                         const Luminumbra::IVec3& disk_coords,
                                         const ActiveRegionRecord& frozen_record,
                                         ActiveRegionRecord& frozen_disk_record) {
    using Luminumbra::ChunkState;
    using Luminumbra::Vec3;
    using Luminumbra::world::RegionWork;
    using Luminumbra::world::WorldClock;
    // Persist a second authority without ever giving its region a consumer tick.
    auto disk_chunk = std::make_shared<Chunk>(disk_coords);
    disk_chunk->set_state(ChunkState::Idle);
    disk_chunk->sdf_data.assign(17 * 17 * 17, -1.0f);
    disk_chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(disk_chunk));
    ASSERT_FALSE(session.GetFieldPages().owns_region(disk_region));
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    EXPECT_FALSE(chunk->is_water_state_dirty());
    EXPECT_TRUE(chunk->has_water_authority());

    // Use the existing paired-clock/ledger fixture idiom to approach retention directly.
    auto staged = session.GetActiveRegionLedger();
    const auto save = session.GetWorldSaveDir();
    staged.pin(disk_region, true, session.GetWorldClock());
    staged.pin(disk_region, false, session.GetWorldClock());
    // Mutate only the staged fixture: reopening leaves no flags or fields to mask inventory.
    auto& disk_record =
        const_cast<std::map<RegionKey, ActiveRegionRecord>&>(staged.records()).at(disk_region);
    disk_record.wake_pending = false;
    disk_record.state = RegionState::Frozen;
    disk_record.frozen_at = session.GetWorldClock().tick();
    disk_record.cadence_shift = staged.config().reduced_shift;
    disk_record.cadence_phase = ActiveRegionLedger::phase(disk_region, disk_record.cadence_shift);
    disk_record.frozen_digest = P::WorldSaveService::region_simulation_digest(
        save,
        disk_region,
        P::RegionDigestInputs{.live = {},
                              .parked = {},
                              .durable = session.GetWorldSystem()->durable_chunk_inventory(),
                              .ground_object_bytes = {},
                              .field_page_bytes = {}});
    frozen_disk_record = disk_record;
    const WorldClock before_idle(frozen_record.last_proximity +
                                 ActiveRegionLedger::kIdleEvictionTicks - 1);
    const std::array pressure{RegionWork{work_region, 1, 0, 0, 0}};
    staged.schedule(before_idle, std::span<const Vec3>{}, pressure);
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    before_idle.write_metadata(metadata);
    ASSERT_TRUE(
        P::WorldSaveService::save_metadata_and_active_regions(metadata.dump(), staged, save));
    ASSERT_TRUE(session.LoadWorldStateFrom(save)) << session.GetWorldOpenError();
    ASSERT_EQ(session.GetWorldSystem()->find_streamed_chunk(coords), chunk);
}

void ReopenRegionalWaterBeforeRetention(GameSession& session,
                                        GameSession& reopened,
                                        const HeadlessRoot& root,
                                        JobSystem& jobs,
                                        const Luminumbra::IVec3& coords,
                                        const Luminumbra::IVec3& disk_coords) {
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    // Both far chunks are disk-only; disk_region has no field authority to mask the probe.
    ASSERT_EQ(reopened.GetWorldSystem()->find_streamed_chunk(coords), nullptr);
    ASSERT_EQ(reopened.GetWorldSystem()->find_streamed_chunk(disk_coords), nullptr);
    const auto inventory = reopened.GetWorldSystem()->durable_chunk_inventory();
    ASSERT_NE(std::find(inventory.begin(), inventory.end(), disk_coords), inventory.end());
    for (const auto& resident : reopened.GetWorldSystem()->snapshot_authoritative_chunks())
        ASSERT_NE(resident->get_coords(), disk_coords);
    EXPECT_EQ(reopened.GetActiveRegionLedger().encode(), session.GetActiveRegionLedger().encode());
}

void TickRegionalWaterRetention(GameSession& session,
                                GameSession& reopened,
                                RegionKey water_region,
                                RegionKey work_region,
                                RegionKey disk_region,
                                const Luminumbra::IVec3& disk_coords,
                                const ActiveRegionRecord& frozen_record,
                                const ActiveRegionRecord& frozen_disk_record,
                                const std::map<RegionKey, ActiveRegionRecord>& initial_records) {
    const auto dt = session.GetSimulationClock().fixed_dt();
    for (auto* active_session : {&session, &reopened})
        active_session->SetReplicatedSimulationAnchors({});
    for (int step = 0; step < 2; ++step) {
        for (auto* active_session : {&session, &reopened}) {
            active_session->RecordActiveRegionWork({work_region, 1, 0, 0, 0});
            ASSERT_EQ(active_session->TickSimulation(dt), 1u);
            ASSERT_FALSE(active_session->GetRegionSchedulerFault());
            ASSERT_NO_FATAL_FAILURE(AssertWaterRetentionRecords(*active_session,
                                                                water_region,
                                                                disk_region,
                                                                disk_coords,
                                                                frozen_record,
                                                                frozen_disk_record,
                                                                initial_records,
                                                                active_session == &reopened));
        }
        EXPECT_EQ(reopened.GetRegionSchedule(), session.GetRegionSchedule());
        EXPECT_EQ(reopened.GetActiveRegionLedger().canonical_bytes(),
                  session.GetActiveRegionLedger().canonical_bytes());
    }
}
} // namespace

TEST(GameSessionHeadlessWorldTest, DisplacementWaterPromotesBeforeFreezeAndSurvivesIdleReopen) {
    using Luminumbra::Chunk;
    using Luminumbra::ChunkState;
    using Luminumbra::IVec3;
    using Luminumbra::Vec3;
    using Luminumbra::world::ActiveRegionLedger;
    using Luminumbra::world::ActiveRegionRecord;
    using Luminumbra::world::RegionKey;
    using Luminumbra::world::RegionState;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    const RegionKey water_region{40, -40}, work_region{30, -40}, disk_region{60, -40};
    const IVec3 coords(water_region.x * 32, 0, water_region.z * 32);
    const IVec3 disk_coords(disk_region.x * 32, 0, disk_region.z * 32);
    const Vec3 visit_position(water_region.x * 512 + 256, 0, water_region.z * 512 + 256);
    const Vec3 displacement_position(coords.x * 16 + 8, 0, coords.z * 16 + 8);
    const auto empty_digest = P::WorldSaveService::region_simulation_digest(
        {},
        water_region,
        P::RegionDigestInputs{.live = {},
                              .parked = {},
                              .durable = {},
                              .ground_object_bytes = {},
                              .field_page_bytes = {}});
    std::uint64_t generated_digest = 0;

    // Run the same seeded resident fixture with and without a production water writer.
    for (const bool displace : {false, true}) {
        SCOPED_TRACE(displace);
        RegionalWaterScenario scenario;
        auto& session = scenario.session;
        EnableSnapshotSession(session, root, jobs);
        session.SetRegionSchedulerConfig({0, 1, 1});
        ASSERT_TRUE(session.CreateWorld(
            displace ? "Displaced water" : "Generated water", "1337", "default"));
        auto chunk = std::make_shared<Chunk>(coords);
        chunk->set_state(ChunkState::Idle);
        chunk->sdf_data.assign(17 * 17 * 17, 1.0f);
        chunk->mark_sdf_generated_current_params();
        // Seed through the real initializer, keeping it asleep so this call only generates water.
        chunk->is_water_sleeping.store(true);
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
        std::vector<std::int32_t> generated_depth;
        ASSERT_NO_FATAL_FAILURE(SeedRegionalWaterFixture(scenario,
                                                         chunk,
                                                         water_region,
                                                         work_region,
                                                         visit_position,
                                                         empty_digest,
                                                         generated_depth));
        const auto initial_records = session.GetActiveRegionLedger().records();
        const auto initial_record = initial_records.at(water_region);
        ASSERT_NO_FATAL_FAILURE(FreezeRegionalWaterFixture(scenario,
                                                           chunk,
                                                           displace,
                                                           water_region,
                                                           work_region,
                                                           displacement_position,
                                                           generated_depth,
                                                           initial_record));
        const auto frozen_record = session.GetActiveRegionLedger().records().at(water_region);
        if (!displace) {
            generated_digest = frozen_record.frozen_digest;
            ASSERT_NO_FATAL_FAILURE(ExpectGeneratedWaterNeutral(
                session, chunk, water_region, empty_digest, generated_digest, generated_depth));
            continue;
        }
        EXPECT_NE(frozen_record.frozen_digest, generated_digest);
        const FrozenWaterSnapshot evolved{.depth = chunk->water_depth_mm,
                                          .bed = chunk->water_bed_mm,
                                          .flux = chunk->water_edge_flux};
        ActiveRegionRecord frozen_disk_record;
        ASSERT_NO_FATAL_FAILURE(PersistRegionalWaterBeforeRetention(session,
                                                                    chunk,
                                                                    work_region,
                                                                    disk_region,
                                                                    coords,
                                                                    disk_coords,
                                                                    frozen_record,
                                                                    frozen_disk_record));
        GameSession reopened;
        ASSERT_NO_FATAL_FAILURE(
            ReopenRegionalWaterBeforeRetention(session, reopened, root, jobs, coords, disk_coords));
        ASSERT_NO_FATAL_FAILURE(TickRegionalWaterRetention(session,
                                                           reopened,
                                                           water_region,
                                                           work_region,
                                                           disk_region,
                                                           disk_coords,
                                                           frozen_record,
                                                           frozen_disk_record,
                                                           initial_records));
        ASSERT_NO_FATAL_FAILURE(AssertDurableWaterContinuation(
            session, reopened, chunk, disk_coords, water_region, frozen_record, evolved));
        ASSERT_TRUE(session.SaveWorldState());
        GameSession after_retention;
        EnableSnapshotSession(after_retention, root, jobs);
        ASSERT_TRUE(after_retention.LoadWorld(session.GetMetadata().worldId))
            << after_retention.GetWorldOpenError();
        EXPECT_EQ(after_retention.GetWorldClock().tick(), session.GetWorldClock().tick());
        EXPECT_EQ(after_retention.GetActiveRegionLedger().encode(),
                  session.GetActiveRegionLedger().encode());
        EXPECT_EQ(after_retention.ComputeChunkStateHash(), session.ComputeChunkStateHash());
    }
}

TEST(GameSessionHeadlessWorldTest, WaterOnlyAuthorityHashDoesNotChangeWhenSaved) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Water-only hash", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->sdf_data.assign(17 * 17 * 17, 1.0f);
    chunk->mark_sdf_generated_current_params();
    chunk->water_depth_mm.assign(64, 123);
    chunk->water_bed_mm.assign(64, 1000);
    chunk->water_edge_flux.assign(128, 0);
    chunk->water_level_data.assign(64, 1.123f);
    chunk->water_flow_data.assign(64, Luminumbra::Vec2(0));
    chunk->water_sim_terrain_height.assign(64, 1.0f);
    chunk->has_water_sim.store(true);
    chunk->mark_water_state_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    const auto before = owner.ComputeChunkStateHash();
    ASSERT_TRUE(owner.SaveWorldState());
    EXPECT_EQ(owner.ComputeChunkStateHash(), before);
    const auto restored = P::WorldSaveService::read_chunk(owner.GetWorldSaveDir(), {0, 0, 0}, true);
    ASSERT_EQ(restored.status, P::WorldSaveService::ChunkReadStatus::Found);
    ASSERT_NE(restored.chunk, nullptr);
    EXPECT_EQ(restored.chunk->water_depth_mm, chunk->water_depth_mm);
}

TEST(GameSessionHeadlessWorldTest, WaterOnlySavesAcknowledgeWithoutHashOrTickPromotion) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Water-only acknowledgement", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->sdf_data.assign(17 * 17 * 17, 1.0f);
    chunk->mark_sdf_generated_current_params();
    chunk->water_depth_mm.assign(64, 123);
    chunk->water_bed_mm.assign(64, 1000);
    chunk->water_edge_flux.assign(128, 0);
    chunk->water_level_data.assign(64, 1.123f);
    chunk->water_flow_data.assign(64, Luminumbra::Vec2(0));
    chunk->water_sim_terrain_height.assign(64, 1.0f);
    chunk->has_water_sim.store(true);
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    for (const bool first : {true, false}) {
        SCOPED_TRACE(first);
        ++chunk->water_depth_mm.front();
        chunk->water_level_data.front() =
            static_cast<float>(chunk->water_bed_mm.front() + chunk->water_depth_mm.front()) /
            1000.0f;
        chunk->mark_water_state_dirty();
        const auto revision = chunk->water_revision();
        ASSERT_TRUE(chunk->is_water_state_dirty());
        ASSERT_FALSE(chunk->is_voxel_data_dirty());
        // Save itself must promote and acknowledge the live object. Hashing or ticking
        // first would consume the pending flag and mask a missing save-side promotion.
        ASSERT_TRUE(owner.SaveWorldState());
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        EXPECT_FALSE(chunk->is_water_state_dirty());
        EXPECT_TRUE(chunk->has_water_authority());
        EXPECT_EQ(chunk->water_revision(), revision);
        const auto durable =
            P::WorldSaveService::read_chunk(owner.GetWorldSaveDir(), {0, 0, 0}, true);
        ASSERT_EQ(durable.status, P::WorldSaveService::ChunkReadStatus::Found);
        ASSERT_NE(durable.chunk, nullptr);
        EXPECT_EQ(durable.chunk->water_depth_mm, chunk->water_depth_mm);
        EXPECT_EQ(durable.chunk->water_edge_flux, chunk->water_edge_flux);
    }
}

TEST(GameSessionHeadlessWorldTest, JoinedLedgerWriterDoesNotRetainCallerLocalReference) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Ledger lifetime", "1337", "default"));
    const auto save = owner.GetWorldSaveDir();
    {
        P::SaveTransaction transaction(save, 0, {}, &P::WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        {
            auto local = owner.GetActiveRegionLedger();
            local.mark_edited({10, 10}, owner.GetWorldClock());
            ASSERT_TRUE(
                P::WorldSaveService::save_active_regions(local, owner.GetWorldClock(), save));
        }
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    Luminumbra::world::ActiveRegionLedger durable;
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(durable, save));
    EXPECT_TRUE(durable.records().contains({10, 10}));
}

TEST(GameSessionHeadlessWorldTest, FarOverlayFilesDoNotChangeTheWorldHash) {
    using Luminumbra::World::FarBrickState;
    using Luminumbra::World::FarOverlayBrick;
    using Luminumbra::World::FarOverlayTile;
    using Luminumbra::World::FarTileKey;

    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, loaded, reloaded;
    for (auto* session : {&original, &loaded, &reloaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
    }
    ASSERT_TRUE(original.CreateWorld("Far overlay", "1337", "default"));
    ASSERT_TRUE(original.SaveWorldState());
    const auto save = original.GetWorldSaveDir();
    const auto before = SessionWorldHash(original);

    FarOverlayTile near_tile;
    near_tile.key = FarTileKey{1, 0, 0};
    near_tile.generation = 1;
    near_tile.bricks.push_back(FarOverlayBrick{0, 0, 0, FarBrickState::HomogeneousAir});
    FarOverlayTile coarse_tile;
    coarse_tile.key = FarTileKey{3, -1, -1};
    coarse_tile.generation = 1;
    coarse_tile.bricks.push_back(FarOverlayBrick{-32, 1, -32, FarBrickState::HomogeneousSolid});
    ASSERT_TRUE(Luminumbra::World::WriteFarOverlay(save, near_tile));
    ASSERT_TRUE(Luminumbra::World::WriteFarOverlay(save, coarse_tile));
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    ASSERT_TRUE(Luminumbra::World::ValidateFarOverlayDirectory(save));

    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(SessionWorldHash(loaded), before);

    fs::remove_all(save / "far");
    ASSERT_TRUE(reloaded.LoadWorld(original.GetMetadata().worldId)) << reloaded.GetWorldOpenError();
    EXPECT_EQ(SessionWorldHash(reloaded), before);
}

TEST(GameSessionHeadlessWorldTest, DespawnedGroundWorkSurvivesBoundarySaveAndReopen) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded, baseline;
    CreateGroundWorld(owner, root, jobs, "Completed despawn work");
    CreateGroundWorld(baseline, root, jobs, "Completed despawn baseline");
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_EQ(SpawnGround(owner, 5200, -1500, 1), 1u);
    TickOnce(owner);
    EXPECT_TRUE(owner.GetGroundObjects().empty());
    // The baseline has identical ownership and allocator state, but no object to process.
    ASSERT_EQ(SpawnGround(baseline, 5200, -1500, 1), 1u);
    ASSERT_TRUE(
        const_cast<Luminumbra::world::GroundObjectStore&>(baseline.GetGroundObjects()).erase(1));
    TickOnce(baseline);
    ASSERT_TRUE(baseline.GetGroundObjects().empty());
    EXPECT_EQ(owner.GetFieldPages().serialize(), baseline.GetFieldPages().serialize());
    EXPECT_EQ(owner.GetActiveRegionLedger().completed_work().at(kGroundRegion),
              baseline.GetActiveRegionLedger().completed_work().at(kGroundRegion) + 1u);
    std::uint64_t pending = 0;
    for (const auto& [key, work] : owner.GetActiveRegionLedger().completed_work()) {
        (void)key;
        pending += work;
    }
    EXPECT_TRUE(owner.IsSimulationTickBoundary());
    ASSERT_TRUE(owner.SaveWorldState());
    ASSERT_TRUE(owner.SaveWorld());
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(owner));
    TickOnce(owner);
    TickOnce(loaded);
    TickOnce(baseline);
    EXPECT_EQ(owner.GetRegionSchedule().work, baseline.GetRegionSchedule().work + 1u);
    EXPECT_EQ(owner.GetActiveRegionLedger().completed_work(),
              baseline.GetActiveRegionLedger().completed_work());
    EXPECT_EQ(owner.GetRegionSchedule().work, pending);
    EXPECT_EQ(loaded.GetRegionSchedule(), owner.GetRegionSchedule());
    EXPECT_TRUE(owner.GetGroundObjects().empty());
    pending = 0;
    for (const auto& [key, work] : owner.GetActiveRegionLedger().completed_work()) {
        (void)key;
        pending += work;
    }
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(owner));
    TickOnce(owner);
    TickOnce(loaded);
    TickOnce(baseline);
    EXPECT_EQ(owner.GetRegionSchedule().work, baseline.GetRegionSchedule().work);
    // Ongoing ambient work replaces the despawn tick's register; ARL unit cases check empty
    // replacement independently. Reopening must neither lose nor duplicate either register.
    EXPECT_EQ(owner.GetRegionSchedule().work, pending);
    EXPECT_EQ(loaded.GetRegionSchedule(), owner.GetRegionSchedule());
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(owner));
}

TEST(GameSessionHeadlessWorldTest, EventConsumerFaultStopsDeliveryAndClearsCompletedWork) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    CreateGroundWorld(owner, root, jobs, "Consumer event fault");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500, 1), 1u);
    int later_calls = 0;
    owner.GetSimulationEventBus().subscribe(
        [](const auto&) { throw std::runtime_error("injected consumer callback fault"); });
    owner.GetSimulationEventBus().subscribe([&](const auto&) { ++later_calls; });
    TickOnce(owner);
    EXPECT_EQ(later_calls, 0);
    EXPECT_TRUE(owner.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(owner.GetActiveRegionLedger().tick(), 1u);
    EXPECT_TRUE(owner.GetActiveRegionLedger().completed_work().empty());
    EXPECT_TRUE(owner.IsSimulationTickBoundary());
    EXPECT_EQ(owner.TickSimulation(owner.GetSimulationClock().fixed_dt()), 0u);
}

TEST(GameSessionHeadlessWorldTest, RegionalQueriesReadStoredFieldsAndSurviveReopen) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Stored regional queries");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500), 1u);
    TickOnce(owner);
    const Luminumbra::Vec3 position{5.2f, 5.0f, -1.5f};
    std::array<std::uint16_t, 5> values{};
    for (int channel = 0; channel < 5; ++channel)
        ASSERT_TRUE(owner.GetFieldPages().sample(channel, 0, -1, values[channel]));
    const auto wind =
        owner.GetWindFieldSystem()->SampleWind(position, Luminumbra::Systems::WindLayer::Ground);
    EXPECT_FLOAT_EQ(wind.x, static_cast<float>(std::bit_cast<std::int16_t>(values[0])) / 1024.0f);
    EXPECT_FLOAT_EQ(wind.y, static_cast<float>(std::bit_cast<std::int16_t>(values[1])) / 1024.0f);
    EXPECT_FLOAT_EQ(owner.GetAetherFieldSystem()->SampleAether(position), values[4] / 65535.0f);
    const auto before = SessionWorldHash(owner);
    ASSERT_TRUE(owner.SaveWorld());
    EXPECT_EQ(SessionWorldHash(owner), before);
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(SessionWorldHash(loaded), before);
    const auto restored = loaded.GetWindFieldSystem()->SampleWind(position);
    EXPECT_EQ(restored, wind);
    EXPECT_EQ(loaded.GetWeatherSystem()->SampleAt(position).category,
              owner.GetWeatherSystem()->SampleAt(position).category);
    EXPECT_FLOAT_EQ(loaded.GetAetherFieldSystem()->SampleAether(position),
                    owner.GetAetherFieldSystem()->SampleAether(position));
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherOpportunityClockContinuesAcrossReopen) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Regional weather opportunity");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500), 1u);
    for (int tick = 0; tick < 44; ++tick)
        TickOnce(owner);
    ASSERT_EQ(owner.GetFieldPages().weather_clocks().at(kGroundRegion), 44u);
    ASSERT_TRUE(owner.SaveWorld());
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetFieldPages().weather_clocks(), owner.GetFieldPages().weather_clocks());
    for (int tick = 0; tick < 4; ++tick) {
        TickOnce(owner);
        TickOnce(loaded);
        if (tick == 0) {
            const auto& storms = owner.GetFieldPages().storms();
            const auto spawned = std::find_if(storms.begin(), storms.end(), [](const auto& storm) {
                return storm.owner == kGroundRegion && storm.spawn_tick == 45;
            });
            ASSERT_NE(spawned, storms.end());
            EXPECT_EQ(spawned->seed_salt, 1812730280u); // root 1337+40, spawn domain
            EXPECT_EQ(spawned->active_age, 0u);
            EXPECT_NEAR(std::bit_cast<float>(spawned->center_x), 325.6141f, 0.001f);
            EXPECT_NEAR(std::bit_cast<float>(spawned->center_z), -212.8846f, 0.001f);
        }
        EXPECT_EQ(loaded.GetFieldPages().serialize(), owner.GetFieldPages().serialize());
        EXPECT_EQ(loaded.GetRegionSchedule(), owner.GetRegionSchedule());
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(owner));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalEnergySaveIsObservationalAndContinuesOwnerCadence) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Regional energy cadence", true);
    ASSERT_EQ(SpawnGround(owner, 5200, -1500), 1u);
    ASSERT_TRUE(owner.GetEnergyFieldState()->QueueRegionalDeposit(1, 0, -1, 0, 60000));
    TickOnce(owner);
    ASSERT_GT(owner.GetEnergyFieldState()->total_raw(), 0u);
    const auto before = owner.GetEnergyFieldState()->SerializeRegionalRecord();
    const auto ages = owner.GetActiveRegionLedger().energy_ages();
    ASSERT_EQ(ages.at(kGroundRegion), 1u);
    ASSERT_TRUE(owner.SaveWorld());
    EXPECT_EQ(owner.GetEnergyFieldState()->SerializeRegionalRecord(), before);
    EXPECT_EQ(owner.GetActiveRegionLedger().energy_ages(), ages);
    EnableSnapshotSession(loaded, root, jobs, true);
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetEnergyFieldState()->SerializeRegionalRecord(), before);
    EXPECT_EQ(loaded.GetActiveRegionLedger().energy_ages(), ages);
    for (int tick = 0; tick < 5; ++tick) {
        TickOnce(owner);
        TickOnce(loaded);
        EXPECT_EQ(loaded.GetEnergyFieldState()->CanonicalRegionalBytes(),
                  owner.GetEnergyFieldState()->CanonicalRegionalBytes());
        EXPECT_EQ(loaded.GetActiveRegionLedger().energy_ages(),
                  owner.GetActiveRegionLedger().energy_ages());
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(owner));
    }
    // A legacy phase-bearing EFS is incompatible with this indexed regional snapshot.
    std::string incompatible = owner.GetEnergyFieldState()->SerializeRegionalRecord();
    ASSERT_FALSE(incompatible.empty());
    const auto phase = incompatible.find(" 0\n");
    ASSERT_NE(phase, std::string::npos);
    incompatible.replace(phase, 3, " 1\n");
    const auto save = owner.GetWorldSaveDir();
    std::ofstream(save / "aether_state.efs", std::ios::binary) << incompatible;
    P::MemberDigest digest;
    std::string error;
    EXPECT_FALSE(P::WorldSaveService::member_digest(save, "aether_state.efs", digest, error));
    EXPECT_FALSE(error.empty());
}

TEST(GameSessionHeadlessWorldTest, HistoricalRegionalScentSurvivesEmitterRemovalAndReopen) {
    using namespace Luminumbra::Components;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Historical regional scent");
    const auto emitter = owner.GetRegistry().create();
    owner.GetRegistry().emplace<TransformComponent>(emitter).position =
        owner.GetMetadata().spawnPoint;
    auto& sensable = owner.GetRegistry().emplace<SensableComponent>(emitter);
    sensable.scent_channel = 0;
    sensable.scent_deposit = 10.0f;
    TickOnce(owner);
    ASSERT_FALSE(owner.GetFieldPages().scent_cells().empty());
    owner.GetRegistry().destroy(emitter);
    const auto persisted = owner.GetFieldPages().scent_cells();
    ASSERT_TRUE(owner.SaveWorld());
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetFieldPages().scent_cells(), persisted);
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(owner));
    TickOnce(owner);
    TickOnce(loaded);
    EXPECT_EQ(owner.GetFieldPages().scent_cells(), persisted);
    EXPECT_EQ(loaded.GetFieldPages().scent_cells(), persisted);
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(owner));
}

namespace {
using namespace Luminumbra::world;

// Construct typed authority locally, seal it through the real codecs and commit a valid index.
// The const views are cast only on these mutable fixture values, never on a live session.
ActiveRegionLedger FixtureLedger(std::uint64_t tick,
                                 const std::map<RegionKey, RegionState>& owners,
                                 std::uint8_t shift = 2) {
    const WorldClock clock(tick);
    ActiveRegionLedger ledger({std::numeric_limits<std::uint64_t>::max(), 10000, shift}, clock);
    for (const auto& [key, state] : owners) {
        ledger.pin(key, true, clock);
        auto& record =
            const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records()).at(key);
        record.wake_pending = false;
        record.state = state;
        record.cadence_shift = state == RegionState::Active ? 0 : shift;
        record.cadence_phase = ActiveRegionLedger::phase(key, record.cadence_shift);
        if (state == RegionState::Frozen) {
            record.frozen_at = tick;
            record.frozen_digest = 123;
        }
    }
    return ledger;
}

void CommitRegionalFixture(GameSession& base,
                           ActiveRegionLedger ledger,
                           const FieldPageState& fields) {
    ASSERT_TRUE(ledger.adopt_content_identity(base.GetSimulationContent()->identity));
    const auto save = base.GetWorldSaveDir();
    const WorldClock clock(ledger.tick());
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    clock.write_metadata(metadata);
    metadata["ambientFieldAnchor"] = {{"x", 0}, {"y", 0}, {"z", 0}};
    std::vector<std::string> errors;
    P::WorldSaveService::SaveFactsScope facts(save);
    P::SaveTransaction transaction(
        save, clock.tick(), {{{0, 0, 0}}}, &P::WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(&errors));
    ASSERT_TRUE(P::WorldSaveService::save_metadata_and_active_regions(
        metadata.dump(), ledger, save, &errors));
    ASSERT_TRUE(P::WorldSaveService::save_field_pages(fields, save, &errors));
    ASSERT_TRUE(transaction.commit(&errors));
}

RegionalStorm FixtureStorm(RegionKey owner,
                           float x,
                           float z,
                           float vx,
                           float vz,
                           std::uint32_t age = 10,
                           std::uint32_t salt = 99,
                           std::uint32_t lifetime = 240) {
    return {owner,
            std::bit_cast<std::uint32_t>(x),
            std::bit_cast<std::uint32_t>(z),
            std::bit_cast<std::uint32_t>(vx),
            std::bit_cast<std::uint32_t>(vz),
            std::bit_cast<std::uint32_t>(1.0f),
            0,
            lifetime,
            salt,
            age};
}

void FixtureWind(FieldPageState& fields, RegionKey owner, float x, float z) {
    ASSERT_TRUE(fields.activate_region(owner, 1337, 1000));
    auto& pages = const_cast<std::map<FieldPageKey, FieldPage>&>(fields.pages());
    for (auto& [key, page] : pages) {
        (void)key;
        for (std::size_t i = 0; i < FieldPage::kCells; ++i)
            if ((page.mask[i / 64] >> (i % 64)) & 1) {
                page.cells[0][i] =
                    std::bit_cast<std::uint16_t>(static_cast<std::int16_t>(x * 1024));
                page.cells[1][i] =
                    std::bit_cast<std::uint16_t>(static_cast<std::int16_t>(z * 1024));
            }
    }
}

bool RegionDue(const GameSession& session, RegionKey owner) {
    const auto& due = session.GetRegionSchedule().due;
    return std::find(due.begin(), due.end(), owner) != due.end();
}
} // namespace

TEST(GameSessionHeadlessWorldTest, RegionalStormHandoverUsesCommonPhaseWithoutSourceInteriorDue) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession base, loaded, reopened;
    CreateGroundWorld(base, root, jobs, "Independent storm handover");
    const RegionKey source{0, -1}, destination{1, -1};
    auto ledger =
        FixtureLedger(1002, {{source, RegionState::Reduced}, {destination, RegionState::Reduced}});
    ASSERT_NE(ledger.records().at(source).cadence_phase,
              ledger.records().at(destination).cadence_phase);
    FieldPageState fields;
    const auto storm = FixtureStorm(source, 511.75f, -256.0f, 30.0f, 0.0f);
    ASSERT_TRUE(fields.replace_weather({storm}, {}));
    CommitRegionalFixture(base, ledger, fields);
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(base.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    loaded.SetReplicatedSimulationAnchors({});
    TickOnce(loaded); // 1003 matches neither the source cadence offset nor the global common phase.
    ASSERT_EQ(loaded.GetFieldPages().storms().size(), 1u);
    EXPECT_EQ(loaded.GetFieldPages().storms().front(), storm);
    TickOnce(loaded); // 1004 common phase, source interior still not due.
    EXPECT_FALSE(RegionDue(loaded, source));
    EXPECT_TRUE(RegionDue(loaded, destination));
    ASSERT_EQ(loaded.GetFieldPages().storms().size(), 1u);
    const auto& moved = loaded.GetFieldPages().storms().front();
    EXPECT_EQ(moved.owner, destination);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(moved.center_x), 512.75f);
    EXPECT_EQ(moved.active_age, storm.active_age);
    EXPECT_EQ(moved.seed_salt, storm.seed_salt);
    EXPECT_GT(loaded.GetActiveRegionLedger().completed_work().at(source), 0u);
    EXPECT_GT(loaded.GetActiveRegionLedger().completed_work().at(destination), 0u);
    const auto transferred = moved;
    ASSERT_TRUE(loaded.SaveWorld());
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(base.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    ASSERT_EQ(reopened.GetFieldPages().storms().size(), 1u);
    EXPECT_EQ(reopened.GetFieldPages().storms().front(), transferred);
    EXPECT_EQ(SessionWorldHash(reopened), SessionWorldHash(loaded));
}

TEST(GameSessionHeadlessWorldTest, RegionalStormCornerRequiresBothCardinalOwners) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    const RegionKey source{0, 0}, destination{1, 1}, east{1, 0}, north{0, 1};
    for (const int blocked : {0, 1, 2, 3}) {
        SCOPED_TRACE(blocked);
        GameSession base, loaded;
        CreateGroundWorld(base, root, jobs, "Storm cardinal admission");
        auto owners = std::map<RegionKey, RegionState>{{source, RegionState::Reduced},
                                                       {destination, RegionState::Reduced},
                                                       {east, RegionState::Reduced},
                                                       {north, RegionState::Reduced}};
        if (blocked == 1)
            owners.erase(east);
        if (blocked == 2)
            owners[north] = RegionState::Frozen;
        if (blocked == 3)
            owners[destination] = RegionState::Frozen;
        auto ledger = FixtureLedger(1003, owners);
        FieldPageState fields;
        const auto storm = FixtureStorm(source, 511.75f, 511.75f, 30.0f, 30.0f);
        ASSERT_TRUE(fields.replace_weather({storm}, {}));
        CommitRegionalFixture(base, ledger, fields);
        EnableSnapshotSession(loaded, root, jobs);
        ASSERT_TRUE(loaded.LoadWorld(base.GetMetadata().worldId)) << loaded.GetWorldOpenError();
        loaded.SetReplicatedSimulationAnchors({});
        TickOnce(loaded);
        EXPECT_FALSE(RegionDue(loaded, source));
        ASSERT_EQ(loaded.GetFieldPages().storms().size(), 1u);
        const auto& after = loaded.GetFieldPages().storms().front();
        EXPECT_EQ(after.owner, blocked == 0 ? destination : source);
        EXPECT_FLOAT_EQ(std::bit_cast<float>(after.center_x), blocked == 0 ? 512.75f : 511.75f);
        EXPECT_FLOAT_EQ(std::bit_cast<float>(after.center_z), blocked == 0 ? 512.75f : 511.75f);
        EXPECT_EQ(after.active_age, storm.active_age);
    }
}

TEST(GameSessionHeadlessWorldTest, BlockedDueStormDiscardsMotionWithinHalfOpenSourceOwner) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession base, loaded;
    CreateGroundWorld(base, root, jobs, "Blocked due storm");
    const RegionKey source{0, 0};
    FieldPageState fields;
    const std::map<RegionKey, RegionState> owners{{source, RegionState::Active},
                                                  {{1, 0}, RegionState::Reduced},
                                                  {{0, 1}, RegionState::Reduced},
                                                  {{1, 1}, RegionState::Reduced}};
    // The 24m cell sampled at this corner belongs to the diagonal owner. Initialize its
    // projection too; motion is blocked because 1001..1003 are not common-phase ticks.
    for (const auto& [owner, state] : owners) {
        (void)state;
        FixtureWind(fields, owner, 31.0f, 31.0f);
    }
    ASSERT_TRUE(fields.replace_weather({FixtureStorm(source, 511.99f, 511.99f, 31, 31)}, {}));
    CommitRegionalFixture(base, FixtureLedger(1000, owners), fields);
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(base.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    loaded.SetReplicatedSimulationAnchors({});
    for (int i = 0; i < 3; ++i) {
        TickOnce(loaded);
        ASSERT_EQ(loaded.GetFieldPages().storms().size(), 1u);
        const auto& storm = loaded.GetFieldPages().storms().front();
        EXPECT_EQ(storm.owner, source);
        EXPECT_FLOAT_EQ(std::bit_cast<float>(storm.center_x), std::nextafter(512.0f, 0.0f));
        EXPECT_FLOAT_EQ(std::bit_cast<float>(storm.center_z), std::nextafter(512.0f, 0.0f));
        EXPECT_EQ(storm.active_age, 11u + i);
    }
}

TEST(GameSessionHeadlessWorldTest,
     RegionalStrikeRoot40RejectsFrozenScatterTargetAndDoesNotReemitOnLoad) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    const RegionKey source{0, 0}, target{1, 0};
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen);
        GameSession base, loaded, reopened;
        CreateGroundWorld(base, root, jobs, "Regional strike target");
        FieldPageState fields;
        FixtureWind(fields, source, 0, 0);
        // Root 1337+40, strike domain, salt 4, opportunity 3 qualifies and scatters east.
        ASSERT_TRUE(fields.replace_weather({FixtureStorm(source, 500, 256, 0, 0, 53, 4)}, {}));
        CommitRegionalFixture(
            base,
            FixtureLedger(1003,
                          {{source, RegionState::Active},
                           {target, frozen ? RegionState::Frozen : RegionState::Reduced}}),
            fields);
        EnableSnapshotSession(loaded, root, jobs);
        ASSERT_TRUE(loaded.LoadWorld(base.GetMetadata().worldId)) << loaded.GetWorldOpenError();
        loaded.SetReplicatedSimulationAnchors({});
        TickOnce(loaded);
        EXPECT_FALSE(loaded.GetRegionSchedulerFault().has_value());
        const auto& strikes = loaded.GetFieldPages().strikes();
        EXPECT_EQ(strikes.size(), frozen ? 0u : 1u);
        EXPECT_EQ(loaded.GetWeatherSystem()->StrikesThisTick().size(), frozen ? 0u : 1u);
        if (frozen)
            continue;
        ASSERT_EQ(strikes.size(), 1u);
        EXPECT_EQ(strikes.front().owner, target);
        EXPECT_EQ(strikes.front().storm_salt, 4u);
        EXPECT_EQ(strikes.front().strike_tick, 1004u);
        EXPECT_NEAR(std::bit_cast<float>(strikes.front().world_x), 638.60f, 1.0f);
        EXPECT_NEAR(std::bit_cast<float>(strikes.front().world_z), 389.23f, 1.0f);
        EXPECT_FLOAT_EQ(std::bit_cast<float>(strikes.front().magnitude), 0.91478163f);
        ASSERT_TRUE(loaded.SaveWorld());
        EnableSnapshotSession(reopened, root, jobs);
        ASSERT_TRUE(reopened.LoadWorld(base.GetMetadata().worldId)) << reopened.GetWorldOpenError();
        EXPECT_EQ(reopened.GetFieldPages().strikes(), strikes);
        EXPECT_TRUE(reopened.GetWeatherSystem()->StrikesThisTick().empty());
        EXPECT_EQ(SessionWorldHash(reopened), SessionWorldHash(loaded));
        reopened.SetReplicatedSimulationAnchors({});
        TickOnce(loaded);
        TickOnce(reopened);
        EXPECT_TRUE(reopened.GetWeatherSystem()->StrikesThisTick().empty());
        EXPECT_EQ(reopened.GetFieldPages().serialize(), loaded.GetFieldPages().serialize());
        EXPECT_EQ(SessionWorldHash(reopened), SessionWorldHash(loaded));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalStormExpiresByDueActiveAgeAndFrozenStormRemainsExact) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    const RegionKey owner{0, 0};
    for (const bool frozen : {false, true}) {
        GameSession base, loaded;
        CreateGroundWorld(base, root, jobs, "Regional storm active expiry");
        FieldPageState fields;
        const auto storm = FixtureStorm(owner, 256, 256, 0, 0, 239);
        ASSERT_TRUE(fields.replace_weather({storm}, {}));
        CommitRegionalFixture(
            base,
            FixtureLedger(1003, {{owner, frozen ? RegionState::Frozen : RegionState::Reduced}}),
            fields);
        EnableSnapshotSession(loaded, root, jobs);
        ASSERT_TRUE(loaded.LoadWorld(base.GetMetadata().worldId)) << loaded.GetWorldOpenError();
        loaded.SetReplicatedSimulationAnchors({});
        TickOnce(loaded); // common tick 1004; the reduced cadence offset is not due
        ASSERT_EQ(loaded.GetFieldPages().storms().size(), 1u);
        EXPECT_EQ(loaded.GetFieldPages().storms().front(), storm);
        TickOnce(loaded); // 1005 interior due only for reduced
        if (frozen) {
            ASSERT_EQ(loaded.GetFieldPages().storms().size(), 1u);
            EXPECT_EQ(loaded.GetFieldPages().storms().front(), storm);
        } else {
            EXPECT_TRUE(loaded.GetFieldPages().storms().empty());
        }
    }
}

TEST(GameSessionHeadlessWorldTest,
     NearScentCommonBorderRunsWithUnequalNonDueInteriorsAndRestoresHistory) {
    using namespace Luminumbra::Components;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession base, near, outside, reopened;
    CreateGroundWorld(base, root, jobs, "Integrated common scent border");
    const RegionKey left{-1, -1}, right{0, -1}, historical_owner{0, 0};
    auto ledger = FixtureLedger(1006,
                                {{left, RegionState::Reduced},
                                 {right, RegionState::Reduced},
                                 {historical_owner, RegionState::Reduced}},
                                3);
    ASSERT_NE(ledger.records().at(left).cadence_phase, ledger.records().at(right).cadence_phase);
    ASSERT_NE(ledger.records().at(left).cadence_phase, 0u);
    ASSERT_NE(ledger.records().at(right).cadence_phase, 0u);
    FieldPageState fields;
    ASSERT_TRUE(fields.set_scent({-1, -10}, {std::bit_cast<std::uint64_t>(20.0), 0, 0, 0}));
    const ScentCell historical{0, std::bit_cast<std::uint64_t>(7.0), 0, 0};
    ASSERT_TRUE(fields.set_scent({200, 0}, historical));
    CommitRegionalFixture(base, ledger, fields);
    for (auto* session : {&near, &outside}) {
        EnableSnapshotSession(*session, root, jobs);
        ASSERT_TRUE(session->LoadWorld(base.GetMetadata().worldId)) << session->GetWorldOpenError();
        session->SetReplicatedSimulationAnchors({});
    }
    const auto add_sensor = [](GameSession& session, Luminumbra::Vec3 position) {
        const auto entity = session.GetRegistry().create();
        session.GetRegistry().emplace<TransformComponent>(entity).position = position;
        session.GetRegistry().emplace<ScentSenseComponent>(entity).channel = 0;
        return entity;
    };
    const auto near_sensor = add_sensor(near, {-0.5f, 0, -9.5f});
    const auto outside_sensor = add_sensor(outside, {200, 0, 0});
    const auto before_hash = near.ComputeScentSubHash();
    ASSERT_FALSE(before_hash.empty());
    for (auto* session : {&near, &outside}) {
        TickOnce(*session); // 1007: neither owner due, and no common border.
        EXPECT_FALSE(RegionDue(*session, left));
        EXPECT_FALSE(RegionDue(*session, right));
        EXPECT_EQ(session->GetFieldPages().scent_cells(), fields.scent_cells());
        EXPECT_EQ(session->ComputeScentSubHash(), before_hash);
        TickOnce(*session); // 1008: global common phase; both interiors still not due.
        EXPECT_FALSE(RegionDue(*session, left));
        EXPECT_FALSE(RegionDue(*session, right));
    }
    EXPECT_EQ(outside.GetFieldPages().scent_cells(), fields.scent_cells());
    ASSERT_TRUE(near.GetFieldPages().scent_cells().contains({0, -10}));
    EXPECT_GT(std::bit_cast<double>(near.GetFieldPages().scent_cells().at({0, -10})[0]), 0.0);
    EXPECT_LT(std::bit_cast<double>(near.GetFieldPages().scent_cells().at({-1, -10})[0]), 20.0);
    double sum = 0;
    for (const auto& [key, values] : near.GetFieldPages().scent_cells()) {
        (void)key;
        sum += std::bit_cast<double>(values[0]);
    }
    EXPECT_DOUBLE_EQ(sum, 20.0); // no advection, smoothing, evaporation or clamp without interiors
    EXPECT_EQ(near.GetFieldPages().scent_cells().at({200, 0}), historical);
    EXPECT_NE(near.ComputeScentSubHash(), before_hash);
    near.GetRegistry().destroy(near_sensor);
    outside.GetRegistry().destroy(outside_sensor);
    const auto history = near.GetFieldPages().scent_cells();
    const auto history_hash = near.ComputeScentSubHash();
    ASSERT_TRUE(near.SaveWorld());
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(base.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    reopened.SetReplicatedSimulationAnchors({});
    EXPECT_EQ(reopened.GetFieldPages().scent_cells(), history);
    EXPECT_EQ(reopened.ComputeScentSubHash(), history_hash);
    EXPECT_DOUBLE_EQ(reopened.GetScentField()->Sample(
                         0, reopened.ScentWorldToCellX(0), reopened.ScentWorldToCellZ(-10)),
                     std::bit_cast<double>(history.at({0, -10})[0]));
    EXPECT_EQ(SessionWorldHash(reopened), SessionWorldHash(near));
    for (int tick = 0; tick < 8; ++tick) {
        TickOnce(near);
        TickOnce(reopened);
        EXPECT_EQ(near.GetFieldPages().scent_cells(), history);
        EXPECT_EQ(reopened.GetFieldPages().scent_cells(), history);
        EXPECT_EQ(reopened.ComputeScentSubHash(), history_hash);
        EXPECT_EQ(SessionWorldHash(reopened), SessionWorldHash(near));
    }
}

TEST(GameSessionHeadlessWorldTest,
     QualifyingStormOpportunityRefusesOwnerCapacityWithoutAdoptingWeather) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession base, loaded;
    CreateGroundWorld(base, root, jobs, "Transactional weather capacity");
    FieldPageState fields;
    std::vector<RegionalStorm> storms;
    for (std::uint32_t salt = 0; salt < FieldPageState::kMaxOwnerStorms; ++salt)
        storms.push_back(FixtureStorm(kGroundRegion, 256, -256, 0, 0, 10, salt));
    ASSERT_TRUE(fields.replace_weather(storms, {}));
    ASSERT_TRUE(fields.set_weather_age(kGroundRegion, 44));
    CommitRegionalFixture(base, FixtureLedger(44, {{kGroundRegion, RegionState::Active}}), fields);
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(base.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    loaded.SetReplicatedSimulationAnchors({});
    TickOnce(loaded); // root+40 spawn qualifies; the 17th owner storm must refuse the staged copy
    ASSERT_TRUE(loaded.GetRegionSchedulerFault().has_value());
    if (!loaded.GetRegionSchedulerFault().has_value())
        return;
    EXPECT_EQ(loaded.GetFieldPages().storms(), storms);
    EXPECT_EQ(loaded.GetFieldPages().weather_clocks(), fields.weather_clocks());
    EXPECT_EQ(loaded.GetFieldPages().strikes(), fields.strikes());
    EXPECT_TRUE(loaded.GetActiveRegionLedger().completed_work().empty());
    EXPECT_EQ(loaded.TickSimulation(loaded.GetSimulationClock().fixed_dt()), 0u);
}

TEST(GameSessionHeadlessWorldTest, EmptyRegionalEnergyAgeWorkReachesNextSchedulePressure) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession energy, baseline;
    CreateGroundWorld(energy, root, jobs, "Empty energy work", true);
    CreateGroundWorld(baseline, root, jobs, "Empty energy work baseline");
    TickOnce(energy);
    TickOnce(baseline);
    ASSERT_EQ(energy.GetRegionSchedule().due, baseline.GetRegionSchedule().due);
    ASSERT_FALSE(energy.GetRegionSchedule().due.empty());
    EXPECT_TRUE(energy.GetEnergyFieldState()->SerializeRegionalRecord().empty());
    EXPECT_EQ(energy.GetFieldPages().serialize(), baseline.GetFieldPages().serialize());
    for (const auto owner : energy.GetRegionSchedule().due) {
        EXPECT_EQ(energy.GetActiveRegionLedger().energy_ages().at(owner), 1u);
        EXPECT_EQ(energy.GetActiveRegionLedger().completed_work().at(owner),
                  baseline.GetActiveRegionLedger().completed_work().at(owner) + 1u);
    }
    const auto age_updates = energy.GetRegionSchedule().due.size();
    TickOnce(energy);
    TickOnce(baseline);
    EXPECT_EQ(energy.GetRegionSchedule().work, baseline.GetRegionSchedule().work + age_updates);
}

namespace {
namespace SF = luminumbra::foliage;
namespace SC = Luminumbra::Components;

std::string S7PlantBytes(GameSession& session) {
    return Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
        SF::BuildPlantEntitySnapshot(session.GetRegistry(), true));
}
entt::entity S7FindPlant(GameSession& session, std::uint64_t id) {
    for (auto e : session.GetRegistry().view<SC::PlantRegionComponent>())
        if (session.GetRegistry().get<SC::PlantRegionComponent>(e).persistent_id == id)
            return e;
    return entt::null;
}
void S7KeepAllocator(ActiveRegionLedger& ledger, std::uint64_t next) {
    while (ledger.next_persistent_id() < next) {
        std::uint64_t id = 0;
        ASSERT_TRUE(ledger.allocate_persistent_id(id));
    }
    auto& records = const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records());
    for (auto& [key, record] : records) {
        record.first_active = 0;
        if (record.state == RegionState::Active)
            record.last_ticked = ledger.tick();
    }
}
void S7CommitFixture(GameSession& base,
                     ActiveRegionLedger ledger,
                     const FieldPageState& fields,
                     const Luminumbra::Ecs::EntityRegistrySnapshot& plants,
                     WorldCalendar calendar = {}) {
    ASSERT_TRUE(ledger.adopt_content_identity(base.GetSimulationContent()->identity));
    const auto save = base.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    const WorldClock clock(ledger.tick(), calendar);
    clock.write_metadata(metadata);
    metadata["ambientFieldAnchor"] = {{"x", 0}, {"y", 0}, {"z", 0}};
    std::vector<std::string> errors;
    P::WorldSaveService::SaveFactsScope facts(save);
    P::SaveTransaction transaction(save, clock.tick(), {}, &P::WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(&errors));
    ASSERT_TRUE(P::WorldSaveService::save_metadata_and_active_regions(
        metadata.dump(), ledger, save, &errors));
    ASSERT_TRUE(P::WorldSaveService::save_field_pages(fields, save, &errors));
    ASSERT_TRUE(P::WorldSaveService::save_plant_entities(plants, save, &errors));
    ASSERT_TRUE(transaction.commit(&errors));
}
void S7SetSnapshotStamp(Luminumbra::Ecs::EntityRegistrySnapshot& snap, std::uint64_t stamp) {
    for (auto& plant : snap.entities)
        for (auto& c : plant.components) {
            if (c.type == "PlantRegion")
                c.data["region_stamp"] = stamp;
            if (c.type == "PlantGrowth")
                c.data["last_tick"] = stamp;
        }
}
void ExpectFoliageBorderProbe(bool along_x, std::uint8_t shift, int mode) {
    const RegionKey owner{-2, -3};
    const RegionKey other{owner.x + (along_x ? 1 : 0), owner.z + (along_x ? 0 : 1)};
    const ScentCellKey a{owner.x * 512 + (along_x ? 511 : 8), owner.z * 512 + (along_x ? 8 : 511)};
    const ScentCellKey b{a.x + (along_x ? 1 : 0), a.z + (along_x ? 0 : 1)};
    const auto other_state =
        mode == 1 ? RegionState::Frozen : (shift == 0 ? RegionState::Active : RegionState::Reduced);
    const auto ledger = FixtureLedger(100,
                                      {{owner, RegionState::Active}, {other, other_state}},
                                      std::max<std::uint8_t>(1, shift));
    auto records = ledger.records();
    if (mode == 2)
        records.erase(other);
    FieldPageState pages;
    ASSERT_TRUE(pages.set_irrigation(a, mode == 0 ? 0 : 800));
    if (mode != 3)
        ASSERT_TRUE(pages.set_irrigation(b, 0));
    if (mode == 4)
        records.at(owner).state = RegionState::Frozen;
    for (std::uint64_t tick = 100; tick < 117; ++tick) {
        auto exchanged = pages.foliage_snapshot();
        const auto work = SF::ExchangeFoliageBorders(exchanged, pages, records, tick);
        EXPECT_EQ(SF::HasRegionalFoliageBorderWork(pages, records, tick), !work.empty())
            << "axis " << along_x << " shift " << static_cast<unsigned>(shift) << " mode " << mode
            << " tick " << tick;
    }
}
SF::PlantEnvSample
S7Environment(GameSession& session, const Luminumbra::Vec3& p, std::uint64_t tick) {
    // Independent public terrain/weather/calendar samples. No session foliage
    // sampler or coarse implementation is used by this reference.
    SF::PlantEnvSample env;
    const auto height = session.GetWorldSystem()->GetTerrainHeightAt(p.x, p.z);
    switch (session.GetWorldSystem()->SurfaceVertexMaterial(p.x, p.z, height)) {
        case Luminumbra::MaterialType::Grass:
            env.soil_quality = .95f;
            break;
        case Luminumbra::MaterialType::Soil:
            env.soil_quality = .85f;
            break;
        case Luminumbra::MaterialType::Sand:
            env.soil_quality = .45f;
            break;
        case Luminumbra::MaterialType::Stone:
            env.soil_quality = .30f;
            break;
        default:
            env.soil_quality = .25f;
            break;
    }
    const auto day = static_cast<float>(session.GetWorldClock().day_phase(tick));
    const auto year = static_cast<float>(session.GetWorldClock().year_phase(tick));
    const auto daylight = 1 - 2 * (day < .5f ? .5f - day : day - .5f);
    const auto warmth = 1 - 2 * (year < .5f ? .5f - year : year - .5f);
    env.light = SF::clamp01(.15f + .85f * daylight);
    env.temperature = SF::clamp01(
        SF::clamp01(.60f -
                    std::max(0.0f, height - static_cast<float>(Luminumbra::SEA_LEVEL)) * .00045f) +
        (warmth - .5f) * .30f);
    env.moisture = SF::clamp01(.30f + session.GetWeatherSystem()->PrecipitationAt(p) * .70f);
    return env;
}
void S7ExpectGrowth(const SC::PlantGrowthComponent& a, const SC::PlantGrowthComponent& b) {
    EXPECT_EQ(a.species_id, b.species_id);
    EXPECT_EQ(a.stage, b.stage);
    EXPECT_EQ(a.quality, b.quality);
    EXPECT_EQ(a.growth_points, b.growth_points);
    EXPECT_EQ(a.stress_points, b.stress_points);
    EXPECT_EQ(a.tended, b.tended);
    EXPECT_EQ(a.planted_tick, b.planted_tick);
    EXPECT_EQ(a.last_tick, b.last_tick);
}
} // namespace

TEST(GameSessionHeadlessWorldTest, S7StableSharedPlantIdentityAndEntireLivingSnapshotContinue) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, reopened;
    CreateGroundWorld(owner, root, jobs, "S7 plant identity");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500), 1u);
    const auto plant = ClockTestPlant(owner.GetRegistry(), {5.2f, 0, -1.5f});
    owner.GetRegistry().emplace<SC::SoilFeederComponent>(plant);
    owner.GetRegistry().emplace<SC::PlantHealthComponent>(plant);
    owner.GetRegistry().emplace<SC::MortalComponent>(plant).age_ticks = 19;
    owner.GetRegistry().emplace<SC::GrazeableComponent>(plant).biomass = .2f;
    owner.GetRegistry().emplace<SC::CombustibleComponent>(plant).burn_ticks_remaining = 100;
    owner.GetRegistry().emplace<SC::WaterSourceComponent>(plant).output = 65535;
    const auto legacy = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
        SF::BuildPlantEntitySnapshot(owner.GetRegistry()));
    ASSERT_TRUE(owner.AdmitPlant(plant));
    const auto identity = owner.GetRegistry().get<SC::PlantRegionComponent>(plant).persistent_id;
    EXPECT_EQ(identity, 2u);
    EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
                  SF::BuildPlantEntitySnapshot(owner.GetRegistry())),
              legacy);
    // Preserve opt-in markers literally in enabled snapshots. A component
    // without its tag must not start receiving pollen after reopen, and a bare
    // tag must not disappear merely because it has no payload component yet.
    owner.GetRegistry().emplace<SC::PollinationComponent>(plant);
    const auto donor = ClockTestPlant(owner.GetRegistry(), {6.2f, 0, -1.5f});
    owner.GetRegistry().get<SC::PlantGrowthComponent>(donor).stage =
        static_cast<std::uint8_t>(SC::PlantStage::Flowering);
    owner.GetRegistry().emplace<SC::PollinationTag>(donor);
    owner.GetRegistry().emplace<SC::PollinationComponent>(donor);
    ASSERT_TRUE(owner.AdmitPlant(donor));
    const auto marker = ClockTestPlant(owner.GetRegistry(), {7.2f, 0, -1.5f});
    owner.GetRegistry().emplace<SC::PollinationTag>(marker);
    ASSERT_TRUE(owner.AdmitPlant(marker));
    TickOnce(owner);
    const auto saved = S7PlantBytes(owner);
    ASSERT_TRUE(owner.SaveWorld());
    EXPECT_EQ(S7PlantBytes(owner), saved);
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(owner.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    EXPECT_EQ(S7PlantBytes(reopened), saved);
    ASSERT_NE(S7FindPlant(reopened, identity), static_cast<entt::entity>(entt::null));
    EXPECT_FALSE(reopened.GetRegistry().all_of<SC::PollinationTag>(S7FindPlant(reopened, 2)));
    EXPECT_TRUE(reopened.GetRegistry().all_of<SC::PollinationTag>(S7FindPlant(reopened, 4)));
    EXPECT_FALSE(reopened.GetRegistry().all_of<SC::PollinationComponent>(S7FindPlant(reopened, 4)));
    EXPECT_EQ(reopened.GetRegistry()
                  .get<SC::PlantRegionComponent>(S7FindPlant(reopened, identity))
                  .region_stamp,
              owner.GetWorldClock().tick());
    for (int i = 0; i < 5; ++i) {
        TickOnce(owner);
        TickOnce(reopened);
        EXPECT_EQ(S7PlantBytes(owner), S7PlantBytes(reopened));
        EXPECT_EQ(owner.GetFieldPages().serialize(), reopened.GetFieldPages().serialize());
    }
}

TEST(GameSessionHeadlessWorldTest,
     S7ReducedDueUsesExactConstantGrowthAndFrozenLivingStateIsUntouched) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession base, live;
    CreateGroundWorld(base, root, jobs, "S7 reduced living");
    const RegionKey reduced{10, -3}, frozen{20, -3};
    for (const auto pos : {Luminumbra::Vec3{5200, 0, -1500}, Luminumbra::Vec3{10300, 0, -1500}}) {
        const auto plant = ClockTestPlant(base.GetRegistry(), pos);
        base.GetRegistry().get<SC::PlantGrowthComponent>(plant).tended = 255;
        base.GetRegistry().emplace<SC::MortalComponent>(plant).age_ticks = 10;
        auto& fire = base.GetRegistry().emplace<SC::CombustibleComponent>(plant);
        fire.set_state(SC::BurnState::Burning);
        fire.burn_ticks_remaining = 1000;
        base.GetRegistry().emplace<SC::GrazeableComponent>(plant).biomass = .2f;
        base.GetRegistry().emplace<SC::PlantHealthComponent>(plant).infection = 400;
    }
    ASSERT_TRUE(base.SaveWorld());
    auto snap = SF::BuildPlantEntitySnapshot(base.GetRegistry(), true);
    S7SetSnapshotStamp(snap, 96);
    auto ledger =
        FixtureLedger(100, {{reduced, RegionState::Reduced}, {frozen, RegionState::Frozen}});
    S7KeepAllocator(ledger, base.GetActiveRegionLedger().next_persistent_id());
    for (const auto key : {reduced, frozen})
        const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records()).at(key).last_ticked =
            96;
    S7CommitFixture(base, ledger, {}, snap);
    EnableSnapshotSession(live, root, jobs);
    ASSERT_TRUE(live.LoadWorld(base.GetMetadata().worldId)) << live.GetWorldOpenError();
    live.SetReplicatedSimulationAnchors({});
    const auto frozen_before =
        SF::BuildPlantEntitySnapshot(live.GetRegistry(), true).entities.at(1);
    std::uint64_t previous = 96;
    int skipped = 0, stepped = 0;
    for (int n = 0; n < 8; ++n) {
        const auto e = S7FindPlant(live, 1);
        const auto before = live.GetRegistry().get<SC::PlantGrowthComponent>(e);
        const auto living_before = S7PlantBytes(live);
        const auto health_before = live.GetRegistry().get<SC::PlantHealthComponent>(e);
        const auto fire_before = live.GetRegistry().get<SC::CombustibleComponent>(e);
        const auto grazing_before = live.GetRegistry().get<SC::GrazeableComponent>(e);
        const auto mortal_before = live.GetRegistry().get<SC::MortalComponent>(e);
        const auto age = live.GetRegistry().get<SC::MortalComponent>(e).age_ticks;
        const auto infection = live.GetRegistry().get<SC::PlantHealthComponent>(e).infection;
        const auto stage_count = SF::FoliageRegistryStageCount();
        TickOnce(live);
        ASSERT_FALSE(live.GetRegionSchedulerFault());
        const auto t = live.GetWorldClock().tick();
        const bool due = std::find(live.GetRegionSchedule().due.begin(),
                                   live.GetRegionSchedule().due.end(),
                                   reduced) != live.GetRegionSchedule().due.end();
        if (!due) {
            EXPECT_EQ(SF::FoliageRegistryStageCount(), stage_count);
            S7ExpectGrowth(live.GetRegistry().get<SC::PlantGrowthComponent>(e), before);
            EXPECT_EQ(live.GetRegistry().get<SC::MortalComponent>(e).age_ticks, age);
            EXPECT_EQ(S7PlantBytes(live), living_before);
            ++skipped;
        } else {
            EXPECT_EQ(SF::FoliageRegistryStageCount(), stage_count + 1);
            entt::registry reference;
            const auto r = ClockTestPlant(reference, {5200, 0, -1500});
            reference.get<SC::PlantGrowthComponent>(r) = before;
            reference.emplace<SC::PlantHealthComponent>(r).infection = infection;
            const auto env = S7Environment(live, {5200, 0, -1500}, t);
            for (auto k = previous + 1; k <= t; ++k)
                SF::RunPlantGrowthSystemOnTick(reference, k, [&](const auto&) { return env; });
            S7ExpectGrowth(live.GetRegistry().get<SC::PlantGrowthComponent>(e),
                           reference.get<SC::PlantGrowthComponent>(r));
            EXPECT_EQ(live.GetRegistry().get<SC::MortalComponent>(e).age_ticks, age + t - previous);
            reference.get<SC::PlantHealthComponent>(r) = health_before;
            reference.emplace<SC::CombustibleComponent>(r, fire_before);
            reference.emplace<SC::GrazeableComponent>(r, grazing_before);
            reference.emplace<SC::MortalComponent>(r, mortal_before);
            for (auto k = previous + 1; k <= t; ++k) {
                SF::RunPlantDiseaseOnTick(reference, k);
                luminumbra::sim::RunFireSpreadOnTick(reference, k);
                luminumbra::ai::RunWildlifeFoliageOnTick(reference, k);
                luminumbra::ai::RunLifespanOnTick(reference, k);
            }
            reference.emplace<SC::PlantRegionComponent>(
                r,
                SC::PlantRegionComponent{.persistent_id = 1u,
                                         .owner_x = reduced.x,
                                         .owner_z = reduced.z,
                                         .region_stamp = t});
            auto expected = SF::BuildPlantEntitySnapshot(reference, true);
            auto actual = SF::BuildPlantEntitySnapshot(live.GetRegistry(), true);
            actual.entities.resize(1);
            EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(actual),
                      Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(expected));
            previous = t;
            ++stepped;
        }
        auto frozen_now = SF::BuildPlantEntitySnapshot(live.GetRegistry(), true);
        Luminumbra::Ecs::EntityRegistrySnapshot a, b;
        a.entities.push_back(frozen_before);
        b.entities.push_back(frozen_now.entities.at(1));
        EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(a),
                  Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(b));
    }
    EXPECT_GT(skipped, 0);
    EXPECT_GT(stepped, 0);
    ASSERT_TRUE(live.SaveWorld());
    GameSession reopened;
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(base.GetMetadata().worldId));
    EXPECT_EQ(S7PlantBytes(live), S7PlantBytes(reopened));
}

TEST(GameSessionHeadlessWorldTest, OffDueFoliageSkipsStagingAndMatchesStagedEmptyTick) {
    using Stage = luminumbra::simulation::SimBudgetStage;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession base, fast, staged;
    CreateGroundWorld(base, root, jobs, "Off-due foliage staging");
    const auto plant = ClockTestPlant(base.GetRegistry(), {5200, 0, -1500});
    base.GetRegistry().emplace<SC::MortalComponent>(plant).age_ticks = 19;
    ASSERT_TRUE(base.SaveWorld());
    // The next tick differs from the Reduced owner's phase, including when it
    // is a common border phase. These isolated page cells have no border edge.
    const auto start = 100u + (ActiveRegionLedger::phase(kFarGroundRegion, 2) + 1u) % 4u;
    auto ledger = FixtureLedger(start, {{kFarGroundRegion, RegionState::Reduced}});
    S7KeepAllocator(ledger, base.GetActiveRegionLedger().next_persistent_id());
    auto plants = SF::BuildPlantEntitySnapshot(base.GetRegistry(), true);
    S7SetSnapshotStamp(plants, start);
    FieldPageState pages;
    ASSERT_TRUE(pages.set_soil({5200, -1500}, 17));
    ASSERT_TRUE(pages.set_irrigation({5200, -1500}, 80));
    ASSERT_NO_FATAL_FAILURE(S7CommitFixture(base, ledger, pages, plants));
    for (auto* session : {&fast, &staged}) {
        EnableSnapshotSession(*session, root, jobs);
        ASSERT_TRUE(session->LoadWorld(base.GetMetadata().worldId)) << session->GetWorldOpenError();
        session->SetReplicatedSimulationAnchors({});
        session->SetSimBudgetTelemetryEnabled(true);
    }
    const auto plants_before = S7PlantBytes(fast);
    const auto allocator_before = fast.GetActiveRegionLedger().next_persistent_id();
    const auto stage_count = SF::FoliageRegistryStageCount();
    ASSERT_NO_FATAL_FAILURE(TickOnce(fast));
    ASSERT_FALSE(fast.GetRegionSchedulerFault());
    ASSERT_TRUE(fast.GetRegionSchedule().due.empty());
    EXPECT_EQ(SF::FoliageRegistryStageCount(), stage_count);
    ASSERT_NO_FATAL_FAILURE(TickOnce(staged));
    ASSERT_FALSE(staged.GetRegionSchedulerFault());
    ASSERT_TRUE(staged.GetRegionSchedule().due.empty());
    EXPECT_EQ(SF::FoliageRegistryStageCount(), stage_count);

    // The old empty-window transaction performed just this registry round trip:
    // no stage changed an entity, and there was no border work to publish.
    std::set<entt::entity> retained;
    const auto registry = SF::StageFoliageRegistry(staged.GetRegistry(), retained);
    SF::CommitFoliageRegistry(staged.GetRegistry(), registry, retained);
    EXPECT_EQ(SF::FoliageRegistryStageCount(), stage_count + 1);
    EXPECT_EQ(S7PlantBytes(fast), plants_before);
    EXPECT_EQ(S7PlantBytes(fast), S7PlantBytes(staged));
    EXPECT_EQ(fast.GetActiveRegionLedger().next_persistent_id(), allocator_before);
    EXPECT_EQ(fast.GetActiveRegionLedger().encode(), staged.GetActiveRegionLedger().encode());
    EXPECT_EQ(fast.GetFieldPages().serialize(), staged.GetFieldPages().serialize());
    EXPECT_EQ(fast.GetFieldPages().soil().records(), pages.soil().records());
    EXPECT_EQ(fast.GetFieldPages().irrigation().records(), pages.irrigation().records());
    ExpectSameWorldHash(fast, staged);
    EXPECT_TRUE(fast.GetSimBudgetTelemetry().WorkMatches(staged.GetSimBudgetTelemetry()));
    for (const auto stage : {Stage::Irrigation,
                             Stage::Soil,
                             Stage::Plants,
                             Stage::Pollination,
                             Stage::Disease,
                             Stage::Crops,
                             Stage::Fire,
                             Stage::Grazing,
                             Stage::Lifespan}) {
        const auto& samples =
            fast.GetSimBudgetTelemetry().SamplesByStage()[static_cast<std::size_t>(stage)];
        ASSERT_EQ(samples.size(), 1u);
        EXPECT_EQ(samples.front().tick, start + 1u);
        EXPECT_EQ(samples.front().work, 0u);
    }
}

TEST(GameSessionHeadlessWorldTest, FoliageBorderWorkProbeMatchesExchangeEligibility) {
    // Exercise both axes and negative coordinates, sparse masks, mixed
    // cadences, sealed endpoints, and zero flux (which still reports work).
    for (const bool along_x : {false, true})
        for (const auto shift : {0, 1, 2, 4})
            for (int mode = 0; mode < 6; ++mode)
                ASSERT_NO_FATAL_FAILURE(
                    ExpectFoliageBorderProbe(along_x, static_cast<std::uint8_t>(shift), mode));
}

TEST(GameSessionHeadlessWorldTest, EmptyFoliageWindowsPreserveStampAndResumeWork) {
    entt::registry registry;
    const auto plant = registry.create();
    registry.emplace<SC::PlantTag>(plant);
    auto& owner = registry.emplace<SC::PlantRegionComponent>(
        plant,
        SC::PlantRegionComponent{
            .persistent_id = 1, .owner_x = 0, .owner_z = 0, .region_stamp = 10});
    auto& growth = registry.emplace<SC::PlantGrowthComponent>(plant);
    growth.last_tick = 10;
    SF::RegionalFoliageWindows windows;
    EXPECT_FALSE(SF::HasRegionalFoliageWindowWork(registry, windows, 10));
    windows.elapsed[{0, 0}] = 0;
    EXPECT_FALSE(SF::HasRegionalFoliageWindowWork(registry, windows, 10));
    growth.last_tick = 9;
    EXPECT_TRUE(SF::HasRegionalFoliageWindowWork(registry, windows, 10));
    growth.last_tick = 10;
    owner.region_stamp = 9;
    EXPECT_TRUE(SF::HasRegionalFoliageWindowWork(registry, windows, 10));
    owner.region_stamp = 10;
    windows.resumes[{0, 0}] = 10;
    EXPECT_TRUE(SF::HasRegionalFoliageWindowWork(registry, windows, 10));
    windows.resumes.clear();
    windows.elapsed[{0, 0}] = 1;
    EXPECT_TRUE(SF::HasRegionalFoliageWindowWork(registry, windows, 10));
}

TEST(GameSessionHeadlessWorldTest,
     S7PageSoilAndIrrigationMatchLegacyIntegerOrderAndRetainEmptyHistory) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, reopened;
    CreateGroundWorld(owner, root, jobs, "S7 exact page units");
    const Luminumbra::Vec3 pos{5201.25f, 0, -1499.25f};
    const auto cell = SF::FoliageCell(pos);
    const auto origin_x = (cell.x / 16) * 16;
    const auto origin_z = ((cell.z - 15) / 16) * 16;
    const auto plant = ClockTestPlant(owner.GetRegistry(), pos);
    owner.GetRegistry().emplace<SC::SoilFeederComponent>(plant).uptake = 80;
    owner.GetRegistry().emplace<SC::WaterSourceComponent>(plant).output = 65535;
    ASSERT_TRUE(owner.AdmitPlant(plant));
    ASSERT_TRUE(owner.SetFoliageCell(pos, true, 3));
    ASSERT_TRUE(owner.SetFoliageCell(pos, false, 3999990));
    owner.SetReplicatedSimulationAnchors({});
    entt::registry legacy;
    const auto r = ClockTestPlant(legacy, pos);
    legacy.emplace<SC::SoilFeederComponent>(r).uptake = 80;
    legacy.emplace<SC::WaterSourceComponent>(r).output = 65535;
    SF::SoilGrid soil(16, 16);
    SF::IrrigationGrid irrigation(16, 16);
    soil.SetCell(cell.x - origin_x, cell.z - origin_z, 3);
    irrigation.SetCell(cell.x - origin_x, cell.z - origin_z, 3999990);
    for (int n = 0; n < 4; ++n) {
        SF::RunIrrigationOnTick(legacy, irrigation, origin_x, origin_z, 1);
        SF::RunSoilNutrientOnTick(legacy, soil, origin_x, origin_z, 1);
        TickOnce(owner);
        EXPECT_EQ(owner.SoilNutrientAt(pos), soil.AtCell(cell.x - origin_x, cell.z - origin_z));
        EXPECT_EQ(owner.IrrigationAt(pos), irrigation.AtCell(cell.x - origin_x, cell.z - origin_z));
        EXPECT_EQ(owner.GetRegistry().get<SC::SoilFeederComponent>(plant).absorbed,
                  legacy.get<SC::SoilFeederComponent>(r).absorbed);
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x) {
                EXPECT_EQ(
                    owner.GetFieldPages().soil().sample({origin_x + x, origin_z + z}, 1000000),
                    soil.AtCell(x, z));
                EXPECT_EQ(
                    owner.GetFieldPages().irrigation().sample({origin_x + x, origin_z + z}, 0),
                    irrigation.AtCell(x, z));
            }
    }
    owner.GetRegistry().destroy(plant);
    legacy.destroy(r);
    const auto history = owner.GetFieldPages().serialize();
    ASSERT_TRUE(owner.SaveWorld());
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(owner.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    reopened.SetReplicatedSimulationAnchors({});
    EXPECT_EQ(reopened.GetFieldPages().serialize(), history);
    EXPECT_TRUE(reopened.GetFieldPages().owns_region({10, -3}));
    for (int n = 0; n < 3; ++n) {
        SF::RunIrrigationOnTick(legacy, irrigation, origin_x, origin_z, 1);
        SF::RunSoilNutrientOnTick(legacy, soil, origin_x, origin_z, 1);
        TickOnce(owner);
        TickOnce(reopened);
        EXPECT_EQ(owner.GetFieldPages().serialize(), reopened.GetFieldPages().serialize());
        EXPECT_EQ(reopened.SoilNutrientAt(pos), soil.AtCell(cell.x - origin_x, cell.z - origin_z));
        EXPECT_EQ(reopened.IrrigationAt(pos),
                  irrigation.AtCell(cell.x - origin_x, cell.z - origin_z));
    }
}

TEST(GameSessionHeadlessWorldTest,
     S7ActualIrrigationBorderCommonPhaseIgnoresInteriorDueAndSealsFrozenOrAbsent) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    RegionKey left{10, -30}, right{11, -30};
    for (int z = -30; z < 30; ++z)
        if (ActiveRegionLedger::phase({10, z}, 2) != 0 &&
            ActiveRegionLedger::phase({11, z}, 2) != 0) {
            left.z = right.z = z;
            break;
        }
    ASSERT_NE(ActiveRegionLedger::phase(left, 2), 0u);
    ASSERT_NE(ActiveRegionLedger::phase(right, 2), 0u);
    for (int mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode);
        GameSession base, live;
        CreateGroundWorld(base, root, jobs, ("S7 border " + std::to_string(mode)).c_str());
        ASSERT_TRUE(base.SaveWorld());
        std::map<RegionKey, RegionState> owners{{left, RegionState::Reduced}};
        if (mode != 2)
            owners[right] = mode == 0 ? RegionState::Reduced : RegionState::Frozen;
        const auto ledger = FixtureLedger(103, owners);
        FieldPageState pages;
        const ScentCellKey a{left.x * 512 + 511, left.z * 512 + 50}, b{a.x + 1, a.z};
        ASSERT_TRUE(pages.set_irrigation(a, 800));
        ASSERT_TRUE(pages.set_soil(a, 999997));
        if (mode != 2)
            ASSERT_TRUE(pages.set_irrigation(b, 0));
        CommitRegionalFixture(base, ledger, pages);
        EnableSnapshotSession(live, root, jobs);
        ASSERT_TRUE(live.LoadWorld(base.GetMetadata().worldId)) << live.GetWorldOpenError();
        live.SetReplicatedSimulationAnchors({});
        const auto stage_count = SF::FoliageRegistryStageCount();
        TickOnce(live); // 104 common phase; neither Reduced interior is due
        EXPECT_EQ(SF::FoliageRegistryStageCount(), stage_count + (mode == 0 ? 1u : 0u));
        EXPECT_TRUE(live.GetRegionSchedule().due.empty());
        EXPECT_EQ(live.GetFieldPages().soil().records(), pages.soil().records());
        if (mode == 0) {
            EXPECT_EQ(live.GetFieldPages().irrigation().sample(a, 0), 700);
            EXPECT_EQ(live.GetFieldPages().irrigation().sample(b, 0), 100);
            EXPECT_EQ(live.GetActiveRegionLedger().completed_work().at(left), 1u);
            EXPECT_EQ(live.GetActiveRegionLedger().completed_work().at(right), 1u);
        } else
            EXPECT_EQ(live.GetFieldPages().irrigation().records(), pages.irrigation().records());
        ASSERT_TRUE(live.SaveWorld());
        GameSession reopened;
        EnableSnapshotSession(reopened, root, jobs);
        ASSERT_TRUE(reopened.LoadWorld(base.GetMetadata().worldId));
        EXPECT_EQ(reopened.GetFieldPages().serialize(), live.GetFieldPages().serialize());
    }
}

TEST(GameSessionHeadlessWorldTest,
     S7IrrigationInteriorsAndBordersConserveWithDepletedDonorAndSaturatedRecipient) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    const RegionKey left{0, 0}, right{1, 0};
    const ScentCellKey a{511, 8}, b{512, 8};
    constexpr std::int32_t source_output = 80;
    for (const bool saturated : {false, true})
        for (const bool reverse : {false, true}) {
            SCOPED_TRACE(saturated);
            SCOPED_TRACE(reverse);
            const auto donor = reverse ? b : a;
            const auto recipient = reverse ? a : b;
            GameSession base, live;
            CreateGroundWorld(base, root, jobs, "S7 conserving irrigation seam");
            ASSERT_TRUE(base.SaveWorld());
            FieldPageState pages;
            for (int z = 0; z < 16; ++z)
                for (int x = 496; x < 528; ++x)
                    ASSERT_TRUE(pages.set_irrigation({x, z}, saturated ? SF::kMoistureMax : 0));
            if (saturated)
                ASSERT_TRUE(pages.set_irrigation(recipient, SF::kMoistureMax - source_output));
            else
                ASSERT_TRUE(pages.set_irrigation(donor, 8));
            std::int64_t total_before = 0;
            for (const auto& entry : SF::FoliageCells(pages.irrigation()))
                total_before += entry.second;
            ASSERT_NO_FATAL_FAILURE(CommitRegionalFixture(
                base,
                FixtureLedger(103, {{left, RegionState::Active}, {right, RegionState::Active}}),
                pages));
            EnableSnapshotSession(live, root, jobs);
            ASSERT_TRUE(live.LoadWorld(base.GetMetadata().worldId)) << live.GetWorldOpenError();
            live.SetReplicatedSimulationAnchors({});
            if (saturated) {
                // Fill the recipient before diffusion: both pages then contain
                // exactly the maximum. Drain frees six units, while the prior
                // host-tick gradient still requests a ten-unit border transfer.
                const auto source = live.GetRegistry().create();
                live.GetRegistry().emplace<SC::TransformComponent>(source).position = {
                    static_cast<float>(recipient.x) + 0.25f,
                    0,
                    static_cast<float>(recipient.z) + 0.25f};
                live.GetRegistry().emplace<SC::WaterSourceComponent>(source).output = source_output;
            }
            ASSERT_NO_FATAL_FAILURE(TickOnce(live)); // Both interiors and the common border run.
            ASSERT_FALSE(live.GetRegionSchedulerFault());
            const auto& due = live.GetRegionSchedule().due;
            EXPECT_NE(std::find(due.begin(), due.end(), left), due.end());
            EXPECT_NE(std::find(due.begin(), due.end(), right), due.end());
            const auto cells = SF::FoliageCells(live.GetFieldPages().irrigation());
            ASSERT_EQ(cells.size(), 512u);
            std::int64_t total_after = 0;
            for (const auto& [cell, value] : cells) {
                std::int32_t expected = 0;
                if (saturated) {
                    expected = SF::kMoistureMax - SF::kMoistureDrainPerTick;
                    if (cell == donor)
                        expected -= SF::kMoistureDrainPerTick;
                    if (cell == recipient)
                        expected = SF::kMoistureMax;
                }
                EXPECT_EQ(value, expected) << "cell (" << cell.x << ',' << cell.z << ')';
                total_after += value;
            }
            EXPECT_LE(total_after, total_before);
            if (saturated)
                EXPECT_EQ(total_after,
                          total_before + source_output - 512 * SF::kMoistureDrainPerTick);
            else {
                // The eight-unit donor sheds one unit to each of its three
                // same-owner neighbors; drainage removes all eight. Historical
                // border flux must not recreate a unit at the dry recipient.
                EXPECT_EQ(total_before, 8);
                EXPECT_EQ(total_after, 0);
            }
        }
}

TEST(GameSessionHeadlessWorldTest, S7ResumeCapClearsPersistedTokenAndDoesNotReplayAfterReopen) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const auto calendar : {WorldCalendar{}, WorldCalendar{120, 4}}) {
        SCOPED_TRACE(calendar.dayLengthTicks);
        GameSession base, live, reopened;
        CreateGroundWorld(
            base, root, jobs, ("S7 resume " + std::to_string(calendar.dayLengthTicks)).c_str());
        const auto plant = ClockTestPlant(base.GetRegistry(), {5200, 0, -1500});
        base.GetRegistry().get<SC::PlantGrowthComponent>(plant).tended = 255;
        base.GetRegistry().emplace<SC::MortalComponent>(plant).age_ticks = 10;
        ASSERT_TRUE(base.SaveWorld());
        auto snap = SF::BuildPlantEntitySnapshot(base.GetRegistry(), true);
        S7SetSnapshotStamp(snap, 96);
        auto ledger = FixtureLedger(70000, {{kFarGroundRegion, RegionState::Frozen}});
        S7KeepAllocator(ledger, base.GetActiveRegionLedger().next_persistent_id());
        auto& record = const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records())
                           .at(kFarGroundRegion);
        record.frozen_at = 100;
        record.last_ticked = 96;
        S7CommitFixture(base, ledger, {}, snap, calendar);
        EnableSnapshotSession(live, root, jobs);
        ASSERT_TRUE(live.LoadWorld(base.GetMetadata().worldId)) << live.GetWorldOpenError();
        live.SetReplicatedSimulationAnchors({});
        // A save while Frozen preserves the token; opening it does no catch-up.
        const auto before = S7PlantBytes(live);
        ASSERT_TRUE(live.SaveWorld());
        EXPECT_EQ(S7PlantBytes(live), before);
        live.PinActiveRegion(kFarGroundRegion, true);
        const auto e = S7FindPlant(live, 1);
        const auto growth = live.GetRegistry().get<SC::PlantGrowthComponent>(e);
        TickOnce(live);
        ASSERT_FALSE(live.GetRegionSchedulerFault());
        const auto cap = std::min(36000u, calendar.dayLengthTicks);
        EXPECT_EQ(live.GetRegistry().get<SC::MortalComponent>(e).age_ticks, 10u + cap);
        EXPECT_EQ(live.GetActiveRegionLedger().records().at(kFarGroundRegion).frozen_at, 0u);
        EXPECT_EQ(live.GetRegistry().get<SC::PlantRegionComponent>(e).region_stamp, 70001u);
        entt::registry reference;
        const auto r = ClockTestPlant(reference, {5200, 0, -1500});
        reference.get<SC::PlantGrowthComponent>(r) = growth;
        const auto env = S7Environment(live, {5200, 0, -1500}, 70001);
        for (std::uint32_t k = 0; k < cap; ++k)
            SF::RunPlantGrowthSystemOnTick(
                reference, 70002 - cap + k, [&](const auto&) { return env; });
        S7ExpectGrowth(live.GetRegistry().get<SC::PlantGrowthComponent>(e),
                       reference.get<SC::PlantGrowthComponent>(r));
        const auto resumed = S7PlantBytes(live);
        ASSERT_TRUE(live.SaveWorld());
        EnableSnapshotSession(reopened, root, jobs);
        ASSERT_TRUE(reopened.LoadWorld(base.GetMetadata().worldId)) << reopened.GetWorldOpenError();
        reopened.SetReplicatedSimulationAnchors({});
        EXPECT_EQ(S7PlantBytes(reopened), resumed);
        EXPECT_EQ(reopened.GetActiveRegionLedger().records().at(kFarGroundRegion).frozen_at, 0u);
        TickOnce(live);
        TickOnce(reopened);
        EXPECT_EQ(S7PlantBytes(reopened), S7PlantBytes(live));
        EXPECT_EQ(
            reopened.GetRegistry().get<SC::MortalComponent>(S7FindPlant(reopened, 1)).age_ticks,
            11u + cap);
    }
}

TEST(GameSessionHeadlessWorldTest, S7FrozenSourcesAndRecipientsCannotPollinateInfectIgniteOrGraze) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    const RegionKey left{10, -3}, right{11, -3};
    for (const int mode : {0, 1, 2}) {
        SCOPED_TRACE(mode);
        const bool freeze_source = mode == 0;
        const bool freeze_recipient = mode == 1;
        GameSession base, live;
        CreateGroundWorld(
            base, root, jobs, freeze_source ? "S7 source gates" : "S7 recipient gates");
        const auto source = ClockTestPlant(base.GetRegistry(), {5631.5f, 0, -1500});
        const auto recipient = ClockTestPlant(base.GetRegistry(), {5632.5f, 0, -1500});
        for (auto e : {source, recipient}) {
            base.GetRegistry().emplace<SC::PollinationTag>(e);
            base.GetRegistry().emplace<SC::PollinationComponent>(e);
            base.GetRegistry().emplace<SC::PlantHealthComponent>(e);
            base.GetRegistry().emplace<SC::GrazeableComponent>(e).biomass = .2f;
            base.GetRegistry().emplace<SC::CombustibleComponent>(e);
            base.GetRegistry().emplace<SC::MortalComponent>(e).age_ticks = 10;
        }
        base.GetRegistry().get<SC::PlantGrowthComponent>(source).stage =
            static_cast<std::uint8_t>(SC::PlantStage::Flowering);
        auto& disease = base.GetRegistry().get<SC::PlantHealthComponent>(source);
        disease.state = static_cast<std::uint8_t>(SC::PlantDiseaseState::Infected);
        disease.infection = 1000;
        base.GetRegistry().emplace<SC::IgnitionSourceComponent>(source).radius = 4;
        ASSERT_TRUE(base.SaveWorld());
        auto ledger =
            FixtureLedger(100,
                          {{left, freeze_source ? RegionState::Frozen : RegionState::Active},
                           {right, freeze_recipient ? RegionState::Frozen : RegionState::Active}});
        S7KeepAllocator(ledger, base.GetActiveRegionLedger().next_persistent_id());
        auto snap = SF::BuildPlantEntitySnapshot(base.GetRegistry(), true);
        S7SetSnapshotStamp(snap, 99);
        for (const auto key : {left, right})
            const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records())
                .at(key)
                .last_ticked = 99;
        S7CommitFixture(base, ledger, {}, snap);
        EnableSnapshotSession(live, root, jobs);
        ASSERT_TRUE(live.LoadWorld(base.GetMetadata().worldId)) << live.GetWorldOpenError();
        live.SetReplicatedSimulationAnchors({});
        const auto frozen_id = freeze_source ? 1u : 2u;
        const auto frozen_before =
            SF::BuildPlantEntitySnapshot(live.GetRegistry(), true).entities.at(frozen_id - 1);
        // A bare herbivore is the grazing source on both sides of the gate
        // matrix; the all-Active control proves there is a real feeding opportunity.
        {
            const auto grazer = live.GetRegistry().create();
            live.GetRegistry().emplace<SC::TransformComponent>(grazer).position = {
                5631.5f, 0, -1500};
            live.GetRegistry().emplace<SC::CreatureComponent>(grazer).hunger = .5f;
        }
        TickOnce(live);
        ASSERT_FALSE(live.GetRegionSchedulerFault());
        const auto target = S7FindPlant(live, 2);
        if (mode == 2) {
            EXPECT_GT(live.GetRegistry().get<SC::PollinationComponent>(target).crosses, 0u);
            EXPECT_GT(live.GetRegistry().get<SC::PlantHealthComponent>(target).infection, 0u);
            EXPECT_GT(live.GetRegistry().get<SC::GrazeableComponent>(target).last_grazed_tick, 0u);
            EXPECT_EQ(
                live.GetRegistry().get<SC::CombustibleComponent>(S7FindPlant(live, 1)).state(),
                SC::BurnState::Burning);
            continue;
        }
        EXPECT_EQ(live.GetRegistry().get<SC::PollinationComponent>(target).crosses, 0u);
        EXPECT_EQ(live.GetRegistry().get<SC::PlantHealthComponent>(target).infection, 0u);
        EXPECT_EQ(live.GetRegistry().get<SC::CombustibleComponent>(target).state(),
                  SC::BurnState::Unburnt);
        EXPECT_EQ(live.GetRegistry().get<SC::GrazeableComponent>(target).last_grazed_tick, 0u);
        const auto frozen_now =
            SF::BuildPlantEntitySnapshot(live.GetRegistry(), true).entities.at(frozen_id - 1);
        Luminumbra::Ecs::EntityRegistrySnapshot a, b;
        a.entities.push_back(frozen_before);
        b.entities.push_back(frozen_now);
        EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(a),
                  Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(b));
    }
}

TEST(GameSessionHeadlessWorldTest, S7CropBirthRefusesFrozenDestinationBeforeRemovingParent) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession base, live;
    CreateGroundWorld(base, root, jobs, "S7 birth recipient gate");
    auto rng = luminumbra::core::DeterministicRng::seeded(SF::kGerminationSeedOffset, 1, 101);
    const auto angle = rng.next_unit() * 6.2831853f;
    const auto radius = 1 + rng.next_unit() * 2;
    const auto dx = radius * Luminumbra::DeterministicMath::Cos(angle);
    ASSERT_NE(dx, 0.0f);
    const Luminumbra::Vec3 pos{dx > 0 ? 5631.999f : 5632.001f, 0, -1500};
    const auto source_owner = ActiveRegionLedger::region_at(pos);
    const auto destination = ActiveRegionLedger::region_at(
        pos + Luminumbra::Vec3{dx, 0, radius * Luminumbra::DeterministicMath::Sin(angle)});
    ASSERT_NE(destination, source_owner);
    const auto plant = ClockTestPlant(base.GetRegistry(), pos);
    auto& growth = base.GetRegistry().get<SC::PlantGrowthComponent>(plant);
    growth.stage = static_cast<std::uint8_t>(SC::PlantStage::Fruiting);
    auto& lifecycle = base.GetRegistry().emplace<SC::CropLifecycleComponent>(plant);
    lifecycle.lifespan_ticks = 1;
    ASSERT_TRUE(base.SaveWorld());
    auto ledger = FixtureLedger(
        100, {{source_owner, RegionState::Active}, {destination, RegionState::Frozen}});
    S7KeepAllocator(ledger, base.GetActiveRegionLedger().next_persistent_id());
    auto snap = SF::BuildPlantEntitySnapshot(base.GetRegistry(), true);
    S7SetSnapshotStamp(snap, 99);
    const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records())
        .at(source_owner)
        .last_ticked = 100;
    S7CommitFixture(base, ledger, {}, snap);
    EnableSnapshotSession(live, root, jobs);
    ASSERT_TRUE(live.LoadWorld(base.GetMetadata().worldId));
    live.SetReplicatedSimulationAnchors({});
    TickOnce(live);
    ASSERT_NE(S7FindPlant(live, 1), static_cast<entt::entity>(entt::null));
    EXPECT_EQ(SF::BuildPlantEntitySnapshot(live.GetRegistry(), true).entities.size(), 1u);
    EXPECT_EQ(live.GetRegistry().get<SC::CropLifecycleComponent>(S7FindPlant(live, 1)).generations,
              0u);
    EXPECT_EQ(live.GetActiveRegionLedger().next_persistent_id(), 2u);
}

TEST(GameSessionHeadlessWorldTest,
     S7RemovedCropWorkAndBornIdentityReachActualCompletedWorkAndSave) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, baseline, reopened;
    CreateGroundWorld(owner, root, jobs, "S7 completed removed crop");
    CreateGroundWorld(baseline, root, jobs, "S7 completed empty region");
    auto rng = luminumbra::core::DeterministicRng::seeded(SF::kGerminationSeedOffset, 1, 1);
    const auto angle = rng.next_unit() * 6.2831853f;
    const auto radius = 1 + rng.next_unit() * 2;
    const auto dx = radius * Luminumbra::DeterministicMath::Cos(angle);
    ASSERT_NE(dx, 0.0f);
    const Luminumbra::Vec3 pos{dx > 0 ? 5631.999f : 5632.001f, 0, -1500};
    const auto source = ActiveRegionLedger::region_at(pos);
    const auto destination = ActiveRegionLedger::region_at(
        pos + Luminumbra::Vec3{dx, 0, radius * Luminumbra::DeterministicMath::Sin(angle)});
    ASSERT_NE(destination, source);
    const auto plant = ClockTestPlant(owner.GetRegistry(), pos);
    owner.GetRegistry().get<SC::PlantGrowthComponent>(plant).stage =
        static_cast<std::uint8_t>(SC::PlantStage::Fruiting);
    owner.GetRegistry().emplace<SC::CropLifecycleComponent>(plant).lifespan_ticks = 1;
    ASSERT_TRUE(owner.AdmitPlant(plant));
    baseline.NotifyGroundObjectEdit(pos);
    for (auto* session : {&owner, &baseline}) {
        session->SetReplicatedSimulationAnchors({});
        TickOnce(*session);
    }
    EXPECT_EQ(S7FindPlant(owner, 1), static_cast<entt::entity>(entt::null));
    ASSERT_NE(S7FindPlant(owner, 2), static_cast<entt::entity>(entt::null));
    // No surviving participant lives in the source owner. Its growth + crop
    // loop + completed region tick still contribute exactly three operations.
    EXPECT_EQ(owner.GetActiveRegionLedger().completed_work().at(source),
              baseline.GetActiveRegionLedger().completed_work().at(source) + 3);
    EXPECT_EQ(owner.GetActiveRegionLedger().completed_work().at(destination),
              258u); // page init + birth + region
    const auto completed = owner.GetActiveRegionLedger().completed_work();
    ASSERT_TRUE(owner.SaveWorld());
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(owner.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    EXPECT_EQ(S7PlantBytes(reopened), S7PlantBytes(owner));
    EXPECT_EQ(reopened.GetActiveRegionLedger().completed_work(), completed);
    EXPECT_EQ(
        reopened.GetRegistry().get<SC::PlantRegionComponent>(S7FindPlant(reopened, 2)).region_stamp,
        1u);
    reopened.SetReplicatedSimulationAnchors({});
    TickOnce(reopened);
    std::uint64_t total = 0;
    for (const auto& [key, work] : completed)
        total += work;
    EXPECT_EQ(reopened.GetRegionSchedule().work, total);
}

TEST(GameSessionHeadlessWorldTest,
     S7IndexedPlantAuthorityRefusesDuplicatesMissingIdentityOwnerAndMissingPage) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (int mutation = 0; mutation < 5; ++mutation) {
        SCOPED_TRACE(mutation);
        GameSession base, refused;
        CreateGroundWorld(
            base, root, jobs, ("S7 corrupt plant " + std::to_string(mutation)).c_str());
        ASSERT_EQ(SpawnGround(base, 5200000, -1500000), 1u);
        const auto plant = ClockTestPlant(base.GetRegistry(), {5200, 0, -1500});
        base.GetRegistry().emplace<SC::SoilFeederComponent>(plant);
        ASSERT_TRUE(base.SaveWorld());
        auto snap = SF::BuildPlantEntitySnapshot(base.GetRegistry(), true);
        auto pages = base.GetFieldPages();
        if (mutation == 0)
            snap.entities.front().entity_id = 1; // ground-store collision
        if (mutation == 1)
            snap.persistent_identity = false;
        if (mutation == 2)
            for (auto& c : snap.entities.front().components)
                if (c.type == "PlantRegion")
                    c.data["owner_x"] = 11;
        if (mutation == 3)
            snap.entities.push_back(snap.entities.front());
        if (mutation == 4)
            pages.clear(); // feeder is not fresh admission on load
        // Recompute the index through a real transaction: refusal must come from
        // decoded authority validation, not just an obsolete file digest.
        S7CommitFixture(base, base.GetActiveRegionLedger(), pages, snap);
        EnableSnapshotSession(refused, root, jobs);
        EXPECT_FALSE(refused.LoadWorld(base.GetMetadata().worldId));
        EXPECT_NE(refused.GetWorldOpenError().find("plant"), std::string::npos);
        EXPECT_TRUE(refused.GetRegistry().view<SC::PlantTag>().empty());
    }
}

TEST(GameSessionHeadlessWorldTest,
     S7PlantIndexedAbsenceGuardDoesNotTurnMissingAuthorityIntoFreshPlants) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession base, refused;
    CreateGroundWorld(base, root, jobs, "S7 plant Error versus Absent");
    ClockTestPlant(base.GetRegistry(), {5200, 0, -1500});
    ASSERT_TRUE(base.SaveWorld());
    fs::remove(P::WorldSaveService::plant_entities_path(base.GetWorldSaveDir()));
    Luminumbra::Ecs::EntityRegistrySnapshot snap;
    std::vector<std::string> errors;
    EXPECT_FALSE(P::WorldSaveService::load_plant_entities(snap, base.GetWorldSaveDir(), &errors));
    ASSERT_FALSE(errors.empty());
    EnableSnapshotSession(refused, root, jobs);
    EXPECT_FALSE(refused.LoadWorld(base.GetMetadata().worldId));
}

TEST(GameSessionHeadlessWorldTest, S7PageCapEditAndFailedResumeAreAtomicAndRetainPendingToken) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession base, live, reopened;
    CreateGroundWorld(base, root, jobs, "S7 resume refusal");
    const auto plant = ClockTestPlant(base.GetRegistry(), {5200, 0, -1500});
    base.GetRegistry().get<SC::PlantGrowthComponent>(plant).stage =
        static_cast<std::uint8_t>(SC::PlantStage::Fruiting);
    base.GetRegistry().emplace<SC::CropLifecycleComponent>(plant).lifespan_ticks = 1;
    ASSERT_TRUE(base.SaveWorld());
    FieldPageState pages;
    std::map<RegionKey, RegionState> owners{{kFarGroundRegion, RegionState::Frozen}};
    for (std::size_t p = 0; p < FoliageFieldPages::kMaxPages; ++p) {
        const ScentCellKey cell{static_cast<int>(p) * 16, 0};
        ASSERT_TRUE(pages.set_soil(cell, 1000000));
        ASSERT_TRUE(pages.set_irrigation(cell, 0));
        owners[FieldPageState::region_of_scent_cell(cell)] = RegionState::Active;
    }
    auto ledger = FixtureLedger(70000, owners);
    S7KeepAllocator(ledger, base.GetActiveRegionLedger().next_persistent_id());
    auto& record =
        const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records()).at(kFarGroundRegion);
    record.frozen_at = 100;
    record.last_ticked = 96;
    auto snap = SF::BuildPlantEntitySnapshot(base.GetRegistry(), true);
    S7SetSnapshotStamp(snap, 96);
    S7CommitFixture(base, ledger, pages, snap);
    EnableSnapshotSession(live, root, jobs);
    ASSERT_TRUE(live.LoadWorld(base.GetMetadata().worldId)) << live.GetWorldOpenError();
    live.SetReplicatedSimulationAnchors({});
    const auto ledger_before = live.GetActiveRegionLedger().encode();
    const auto pages_before = live.GetFieldPages().serialize();
    EXPECT_FALSE(live.SetFoliageCell({5200, 0, -1500}, true, 0));
    EXPECT_FALSE(live.SetFoliageCell({5200, 0, -1500}, false, 4));
    EXPECT_FALSE(live.SetFoliageCell({std::numeric_limits<float>::quiet_NaN(), 0, 0}, false, 1));
    EXPECT_EQ(live.GetActiveRegionLedger().encode(), ledger_before);
    EXPECT_EQ(live.GetFieldPages().serialize(), pages_before);
    const auto plants_before = S7PlantBytes(live);
    const auto retained_plant = S7FindPlant(live, 1);
    ASSERT_NE(retained_plant, static_cast<entt::entity>(entt::null));
    live.GetRegistry().emplace<std::uint32_t>(retained_plant, 42u);
    const auto retained_source = live.GetRegistry().create();
    live.GetRegistry().emplace<SC::TransformComponent>(retained_source).position = {
        -8000, 0, -8000};
    live.GetRegistry().emplace<SC::CombustibleComponent>(retained_source).burn_ticks_remaining =
        1234;
    const auto transforms_before = live.GetRegistry().view<SC::TransformComponent>().size();
    live.PinActiveRegion(kFarGroundRegion, true);
    // Independently schedule the pre-foliage ledger. Only the failed birth's
    // allocator burn may survive the transaction; no consumer work is published.
    auto expected_ledger = live.GetActiveRegionLedger();
    expected_ledger.schedule(WorldClock(live.GetWorldClock().tick() + 1),
                             std::span<const Luminumbra::Vec3>{});
    std::uint64_t burned_id = 0;
    ASSERT_TRUE(expected_ledger.allocate_persistent_id(burned_id));
    EXPECT_EQ(burned_id, 2u);
    const auto stage_count = SF::FoliageRegistryStageCount();
    TickOnce(live); // resume growth then a child needs page 4097: refuse all staged foliage
    const auto fault = live.GetRegionSchedulerFault();
    ASSERT_TRUE(fault.has_value());
    if (!fault.has_value())
        return;
    EXPECT_NE(fault->message.find("Crop identity admission refused"), std::string::npos);
    EXPECT_EQ(SF::FoliageRegistryStageCount(), stage_count + 1);
    EXPECT_EQ(live.GetActiveRegionLedger().encode(), expected_ledger.encode());
    EXPECT_EQ(live.GetRegistry().view<SC::TransformComponent>().size(), transforms_before);
    ASSERT_TRUE(live.GetRegistry().valid(retained_plant));
    EXPECT_EQ(live.GetRegistry().get<std::uint32_t>(retained_plant), 42u);
    ASSERT_TRUE(live.GetRegistry().valid(retained_source));
    EXPECT_EQ(
        live.GetRegistry().get<SC::CombustibleComponent>(retained_source).burn_ticks_remaining,
        1234u);
    EXPECT_EQ(S7PlantBytes(live), plants_before);
    EXPECT_EQ(live.GetFieldPages().soil().records(), pages.soil().records());
    EXPECT_EQ(live.GetFieldPages().irrigation().records(), pages.irrigation().records());
    EXPECT_EQ(live.GetActiveRegionLedger().records().at(kFarGroundRegion).frozen_at, 100u);
    EXPECT_EQ(live.GetActiveRegionLedger().next_persistent_id(), 3u); // failed spawn consumed ID 2
    EXPECT_TRUE(live.GetActiveRegionLedger().completed_work().empty());
    ASSERT_TRUE(live.SaveWorld());
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(base.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    EXPECT_EQ(S7PlantBytes(reopened), plants_before);
    EXPECT_EQ(reopened.GetActiveRegionLedger().records().at(kFarGroundRegion).frozen_at, 100u);
    EXPECT_EQ(reopened.GetActiveRegionLedger().next_persistent_id(), 3u);
}

TEST(GameSessionHeadlessWorldTest, S7LivePlantsRetainOtherwiseEvictableUneditedOwner) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession base, live;
    CreateGroundWorld(base, root, jobs, "S7 plant retention");
    ClockTestPlant(base.GetRegistry(), {5200, 0, -1500});
    ASSERT_TRUE(base.SaveWorld());
    const RegionKey soil_owner{20, -3}, irrigation_owner{30, -3}, debt_owner{40, -3};
    const RegionKey unowned{50, -3};
    auto ledger = FixtureLedger(108001,
                                {{kFarGroundRegion, RegionState::Active},
                                 {soil_owner, RegionState::Frozen},
                                 {irrigation_owner, RegionState::Frozen},
                                 {debt_owner, RegionState::Frozen},
                                 {unowned, RegionState::Frozen}});
    S7KeepAllocator(ledger, base.GetActiveRegionLedger().next_persistent_id());
    for (auto& [key, owner] :
         const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records())) {
        (void)key;
        owner.pinned = false;
        owner.edited = false;
        owner.last_proximity = 0;
    }
    auto& record =
        const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records()).at(kFarGroundRegion);
    record.first_active = 0;
    record.last_ticked = 108000;
    FieldPageState pages;
    // Independent one-metre histories: no ambient/weather cells or live feeders mask ownership.
    ASSERT_TRUE(pages.set_soil({soil_owner.x * 512, soil_owner.z * 512}, 1000000));
    ASSERT_TRUE(pages.set_irrigation({irrigation_owner.x * 512, irrigation_owner.z * 512}, 0));
    const ActiveRegionLedger::WaterDebts debts{{debt_owner, 1}};
    ASSERT_TRUE(ledger.set_water_debts(debts));
    auto snap = SF::BuildPlantEntitySnapshot(base.GetRegistry(), true);
    S7SetSnapshotStamp(snap, 108000);
    S7CommitFixture(base, ledger, pages, snap);
    EnableSnapshotSession(live, root, jobs);
    ASSERT_TRUE(live.LoadWorld(base.GetMetadata().worldId)) << live.GetWorldOpenError();
    live.SetReplicatedSimulationAnchors({});
    TickOnce(live);
    ASSERT_FALSE(live.GetRegionSchedulerFault());
    const auto& retained = live.GetActiveRegionLedger().records();
    ASSERT_EQ(retained.size(), 4u);
    EXPECT_FALSE(retained.contains(unowned));
    EXPECT_EQ(live.GetRegionSchedule().due, (std::vector<RegionKey>{kFarGroundRegion}));
    for (const auto key : {soil_owner, irrigation_owner, debt_owner}) {
        ASSERT_TRUE(retained.contains(key));
        EXPECT_EQ(retained.at(key).state, RegionState::Frozen);
        EXPECT_FALSE(retained.at(key).pinned);
        EXPECT_FALSE(retained.at(key).edited);
        EXPECT_FALSE(retained.at(key).populated);
        EXPECT_EQ(retained.at(key).water_cursor, 0u);
        EXPECT_EQ(live.GetFieldPages().owned_initialized_cells(key), 0u);
        EXPECT_FALSE(live.GetActiveRegionLedger().energy_ages().contains(key));
        EXPECT_FALSE(live.GetActiveRegionLedger().completed_work().contains(key));
    }
    EXPECT_EQ(live.GetFieldPages().soil().records(), pages.soil().records());
    EXPECT_EQ(live.GetFieldPages().irrigation().records(), pages.irrigation().records());
    EXPECT_EQ(live.GetActiveRegionLedger().water_debts(), debts);
    EXPECT_EQ(live.GetRegistry().get<SC::PlantRegionComponent>(S7FindPlant(live, 1)).region_stamp,
              108002u);
}

TEST(GameSessionHeadlessWorldTest,
     S7ReducedAndCalendarBoundedResumePagesMatchIndependentIntegerTicks) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const bool resume : {false, true}) {
        SCOPED_TRACE(resume);
        GameSession base, live, reopened;
        CreateGroundWorld(
            base, root, jobs, resume ? "S7 resumed integer pages" : "S7 reduced integer pages");
        const Luminumbra::Vec3 pos{5201.25f, 0, -1499.25f};
        const auto cell = SF::FoliageCell(pos);
        const int ox = 5200, oz = -1504;
        entt::registry reference;
        for (int i = 0; i < 2; ++i) {
            const auto p = ClockTestPlant(base.GetRegistry(), pos);
            base.GetRegistry().emplace<SC::SoilFeederComponent>(p).uptake = 80;
            base.GetRegistry().emplace<SC::WaterSourceComponent>(p).output = 65535;
            ASSERT_TRUE(base.AdmitPlant(p));
            const auto r = ClockTestPlant(reference, pos);
            reference.emplace<SC::SoilFeederComponent>(r).uptake = 80;
            reference.emplace<SC::WaterSourceComponent>(r).output = 65535;
        }
        ASSERT_TRUE(base.SetFoliageCell(pos, true, 3));
        ASSERT_TRUE(base.SetFoliageCell(pos, false, 3999990));
        ASSERT_TRUE(base.SaveWorld());
        const RegionKey frozen{20, -3};
        auto pages = base.GetFieldPages();
        const ScentCellKey sealed{10240, -1500};
        ASSERT_TRUE(pages.set_soil(sealed, 123456));
        ASSERT_TRUE(pages.set_irrigation(sealed, 3456789));
        auto ledger =
            FixtureLedger(100,
                          {{kFarGroundRegion, resume ? RegionState::Frozen : RegionState::Reduced},
                           {frozen, RegionState::Frozen}});
        S7KeepAllocator(ledger, base.GetActiveRegionLedger().next_persistent_id());
        auto& record = const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records())
                           .at(kFarGroundRegion);
        record.last_ticked = resume ? 16u : 96u;
        if (resume)
            record.frozen_at = 20;
        auto plants = SF::BuildPlantEntitySnapshot(base.GetRegistry(), true);
        S7SetSnapshotStamp(plants, resume ? 16u : 96u);
        WorldCalendar calendar;
        if (resume)
            calendar.dayLengthTicks = 16;
        S7CommitFixture(base, ledger, pages, plants, calendar);
        EnableSnapshotSession(live, root, jobs);
        ASSERT_TRUE(live.LoadWorld(base.GetMetadata().worldId)) << live.GetWorldOpenError();
        live.SetReplicatedSimulationAnchors({});
        if (resume)
            live.PinActiveRegion(kFarGroundRegion, true);
        SF::SoilGrid soil(16, 16);
        SF::IrrigationGrid moisture(16, 16);
        soil.SetCell(cell.x - ox, cell.z - oz, 3);
        moisture.SetCell(cell.x - ox, cell.z - oz, 3999990);
        std::uint64_t previous = 96;
        int actual_due = 0;
        for (int host = 0; host < (resume ? 2 : 8); ++host) {
            const auto before = live.GetFieldPages().serialize();
            TickOnce(live);
            ASSERT_FALSE(live.GetRegionSchedulerFault());
            const auto tick = live.GetWorldClock().tick();
            const bool due = std::find(live.GetRegionSchedule().due.begin(),
                                       live.GetRegionSchedule().due.end(),
                                       kFarGroundRegion) != live.GetRegionSchedule().due.end();
            if (resume || due) {
                const auto k = resume && host == 0 ? 16 : tick - previous;
                for (std::uint64_t n = 0; n < k; ++n) {
                    SF::RunSoilNutrientOnTick(reference, soil, ox, oz, 1);
                    SF::RunIrrigationOnTick(reference, moisture, ox, oz, 1);
                }
                previous = tick;
                ++actual_due;
                for (int z = 0; z < 16; ++z)
                    for (int x = 0; x < 16; ++x) {
                        EXPECT_EQ(live.GetFieldPages().soil().sample({ox + x, oz + z}, 0),
                                  soil.AtCell(x, z));
                        EXPECT_EQ(live.GetFieldPages().irrigation().sample({ox + x, oz + z}, 0),
                                  moisture.AtCell(x, z));
                    }
                std::uint64_t id = 1;
                std::vector<entt::entity> feeders;
                for (auto e : reference.view<SC::SoilFeederComponent>())
                    feeders.push_back(e);
                std::sort(feeders.begin(), feeders.end());
                for (auto e : feeders)
                    EXPECT_EQ(live.GetRegistry()
                                  .get<SC::SoilFeederComponent>(S7FindPlant(live, id++))
                                  .absorbed,
                              reference.get<SC::SoilFeederComponent>(e).absorbed);
            } else
                EXPECT_EQ(live.GetFieldPages().serialize(), before);
            EXPECT_EQ(live.GetFieldPages().soil().sample(sealed, 0), 123456);
            EXPECT_EQ(live.GetFieldPages().irrigation().sample(sealed, 0), 3456789);
            if (resume)
                EXPECT_EQ(live.GetActiveRegionLedger().records().at(kFarGroundRegion).frozen_at,
                          0u);
        }
        EXPECT_GT(actual_due, 0);
        ASSERT_TRUE(live.SaveWorld());
        EnableSnapshotSession(reopened, root, jobs);
        ASSERT_TRUE(reopened.LoadWorld(base.GetMetadata().worldId)) << reopened.GetWorldOpenError();
        EXPECT_EQ(S7PlantBytes(reopened), S7PlantBytes(live));
        EXPECT_EQ(reopened.GetFieldPages().serialize(), live.GetFieldPages().serialize());
        reopened.SetReplicatedSimulationAnchors({});
        TickOnce(reopened);
        TickOnce(live);
        EXPECT_EQ(S7PlantBytes(reopened), S7PlantBytes(live));
        EXPECT_EQ(reopened.GetFieldPages().serialize(), live.GetFieldPages().serialize());
    }
}

TEST(GameSessionHeadlessWorldTest, S7OffDueFrozenToReducedEmptyPageResumeDoesNotReplayAfterSave) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession base, live, reopened;
    CreateGroundWorld(base, root, jobs, "S7 off-due resume boundary");
    ASSERT_TRUE(base.SaveWorld());
    RegionKey owner{10, -3};
    while (ActiveRegionLedger::phase(owner, 2) == (101u & 3u))
        ++owner.z;
    auto ledger = FixtureLedger(100, {{owner, RegionState::Frozen}});
    auto& record = const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records()).at(owner);
    record.frozen_at = 20;
    record.last_ticked = 16;
    record.under_hold = 9999; // next schedule recovers exactly to Reduced
    FieldPageState pages;
    const ScentCellKey cell{owner.x * 512 + 20, owner.z * 512 + 20};
    ASSERT_TRUE(pages.set_soil(cell, 100));
    ASSERT_TRUE(pages.set_irrigation(cell, 123));
    Luminumbra::Ecs::EntityRegistrySnapshot empty;
    empty.persistent_identity = true;
    S7CommitFixture(
        base, ledger, pages, empty, WorldCalendar{.dayLengthTicks = 16, .daysPerYear = 8});
    EnableSnapshotSession(live, root, jobs);
    ASSERT_TRUE(live.LoadWorld(base.GetMetadata().worldId)) << live.GetWorldOpenError();
    live.SetReplicatedSimulationAnchors({});
    TickOnce(live);
    ASSERT_FALSE(live.GetRegionSchedulerFault());
    const auto& resumed = live.GetActiveRegionLedger().records().at(owner);
    EXPECT_EQ(resumed.state, RegionState::Reduced);
    EXPECT_EQ(resumed.frozen_at, 0u);
    EXPECT_EQ(resumed.last_ticked, 101u);
    EXPECT_TRUE(live.GetRegionSchedule().due.empty());
    EXPECT_EQ(live.GetFieldPages().soil().sample(cell, 0), 164);
    EXPECT_EQ(live.GetFieldPages().irrigation().sample(cell, 0), 27); // 16 exact drains
    ASSERT_TRUE(live.SaveWorld());
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(base.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    reopened.SetReplicatedSimulationAnchors({});
    EXPECT_EQ(reopened.GetActiveRegionLedger().records().at(owner).last_ticked, 101u);
    std::uint64_t previous = 101;
    std::int32_t nutrient = 164, moisture = 27;
    for (int n = 0; n < 8; ++n) {
        TickOnce(live);
        TickOnce(reopened);
        ASSERT_FALSE(reopened.GetRegionSchedulerFault());
        const auto tick = reopened.GetWorldClock().tick();
        if (std::find(reopened.GetRegionSchedule().due.begin(),
                      reopened.GetRegionSchedule().due.end(),
                      owner) != reopened.GetRegionSchedule().due.end()) {
            nutrient += 4 * static_cast<std::int32_t>(tick - previous);
            moisture = std::max(0, moisture - 6 * static_cast<std::int32_t>(tick - previous));
            previous = tick;
        }
        EXPECT_EQ(reopened.GetFieldPages().soil().sample(cell, 0), nutrient);
        EXPECT_EQ(reopened.GetFieldPages().irrigation().sample(cell, 0), moisture);
        EXPECT_EQ(live.GetFieldPages().serialize(), reopened.GetFieldPages().serialize());
    }
}

namespace {
std::shared_ptr<Luminumbra::Chunk> S7HostWaterGrid(const Luminumbra::IVec3& coords,
                                                   int resolution) {
    auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->sdf_data.assign(17 * 17 * 17, 1.0f);
    chunk->mark_sdf_generated_current_params();
    const auto cells = static_cast<std::size_t>(resolution * resolution);
    chunk->water_depth_mm.assign(cells, 1000);
    chunk->water_bed_mm.assign(cells, 1000);
    chunk->water_edge_flux.assign(2 * cells, 0);
    chunk->water_level_data.assign(cells, 2.0f);
    chunk->water_flow_data.assign(cells, Luminumbra::Vec2(0));
    chunk->water_sim_terrain_height.assign(cells, 1.0f);
    chunk->has_water_sim.store(true);
    chunk->current_water_resolution.store(resolution);
    chunk->ticks_below_threshold = 19;
    chunk->mark_water_state_dirty();
    return chunk;
}
void S7CompareWater(const Luminumbra::Chunk& a, const Luminumbra::Chunk& b) {
    EXPECT_EQ(a.water_depth_mm, b.water_depth_mm);
    EXPECT_EQ(a.water_bed_mm, b.water_bed_mm);
    EXPECT_EQ(a.water_edge_flux, b.water_edge_flux);
    EXPECT_EQ(a.water_level_data, b.water_level_data);
    EXPECT_EQ(a.water_flow_data, b.water_flow_data);
    EXPECT_EQ(a.water_sim_terrain_height, b.water_sim_terrain_height);
    EXPECT_EQ(a.current_water_resolution.load(), b.current_water_resolution.load());
    EXPECT_EQ(a.is_water_sleeping.load(), b.is_water_sleeping.load());
    EXPECT_EQ(a.ticks_below_threshold, b.ticks_below_threshold);
    EXPECT_EQ(a.max_water_delta_last_tick, b.max_water_delta_last_tick);
}
} // namespace

TEST(GameSessionHeadlessWorldTest, S7WaterPlantsPagesDebtAndCompletedWorkShareIndexedContinuation) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, reopened;
    CreateGroundWorld(owner, root, jobs, "S7 combined continuation");
    owner.SetLocalPlayerSimulationPosition({8, 0, 8}, true);
    const auto plant = ClockTestPlant(owner.GetRegistry(), {.25f, 0, .25f});
    owner.GetRegistry().emplace<SC::SoilFeederComponent>(plant);
    owner.GetRegistry().emplace<SC::WaterSourceComponent>(plant).output = 65535;
    owner.GetRegistry().emplace<SC::WaterFlowSourceComponent>(plant).flow_rate = .375f;
    ASSERT_TRUE(owner.AdmitPlant(plant));
    auto a = S7HostWaterGrid({0, 0, 0}, 8), b = S7HostWaterGrid({1, 0, 0}, 4);
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(a));
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(b));
    owner.GetWaterSystem()->SetHydrology(true, 1, 0);
    auto& ledger = const_cast<ActiveRegionLedger&>(owner.GetActiveRegionLedger());
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, 17}}));
    ledger.set_water_cursor({0, 0}, 1);
    // Streaming calls bind the live query map but do not advance source/rain/damping/sleep.
    const std::unordered_map<Luminumbra::ChunkID, std::shared_ptr<Luminumbra::Chunk>> resident{
        {a->get_id(), a}, {b->get_id(), b}};
    const auto depth = a->water_depth_mm;
    owner.GetWaterSystem()->update(owner.GetRegistry(), resident);
    owner.GetWaterSystem()->update(owner.GetRegistry(), resident);
    EXPECT_EQ(a->water_depth_mm, depth);
    EXPECT_EQ(a->ticks_below_threshold, 19);
    ASSERT_TRUE(owner.SaveWorldState());
    const auto saved_plants = S7PlantBytes(owner);
    const auto saved_pages = owner.GetFieldPages().serialize();
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(owner.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    ASSERT_TRUE(reopened.GetWorldSystem()->restore_authoritative_chunk({0, 0, 0}));
    ASSERT_TRUE(reopened.GetWorldSystem()->restore_authoritative_chunk({1, 0, 0}));
    auto loaded_a = reopened.GetWorldSystem()->find_streamed_chunk({0, 0, 0});
    auto loaded_b = reopened.GetWorldSystem()->find_streamed_chunk({1, 0, 0});
    ASSERT_NE(loaded_a, nullptr);
    ASSERT_NE(loaded_b, nullptr);
    S7CompareWater(*a, *loaded_a);
    S7CompareWater(*b, *loaded_b);
    EXPECT_EQ(loaded_b->current_water_resolution.load(), 4); // no boot migration/catch-up
    EXPECT_EQ(S7PlantBytes(reopened), saved_plants);
    EXPECT_EQ(reopened.GetFieldPages().serialize(), saved_pages);
    EXPECT_EQ(reopened.GetActiveRegionLedger().water_debts(), ledger.water_debts());
    EXPECT_EQ(reopened.GetActiveRegionLedger().records().at({0, 0}).water_cursor, 1u);
    reopened.GetWaterSystem()->SetHydrology(true, 1, 0);
    for (int tick = 0; tick < 3; ++tick) {
        TickOnce(owner);
        TickOnce(reopened);
        ASSERT_FALSE(owner.GetRegionSchedulerFault());
        ASSERT_FALSE(reopened.GetRegionSchedulerFault());
        S7CompareWater(*a, *loaded_a);
        S7CompareWater(*b, *loaded_b);
        EXPECT_EQ(S7PlantBytes(owner), S7PlantBytes(reopened));
        EXPECT_EQ(owner.GetFieldPages().serialize(), reopened.GetFieldPages().serialize());
        EXPECT_EQ(owner.GetActiveRegionLedger().water_debts(),
                  reopened.GetActiveRegionLedger().water_debts());
        EXPECT_EQ(owner.GetActiveRegionLedger().completed_work(),
                  reopened.GetActiveRegionLedger().completed_work());
        EXPECT_EQ(owner.GetWaterSystem()->regional_work().used_windows, 2u);
    }
    EXPECT_EQ(owner.GetWaterSystem()->regional_work().source_mm, 2 * 64 + 3);
    ASSERT_TRUE(owner.SaveWorldState());
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(owner.GetWorldSaveDir(), index, nullptr));
    for (const auto* path : {"chunks/region/active-regions.arl",
                             "chunks/region/field-pages.fpg",
                             "chunks/region/plant-entities.json"})
        EXPECT_TRUE(
            std::any_of(index.members.begin(), index.members.end(), [path](const auto& member) {
                return member.path == path && member.present;
            }));
    GameSession after_work;
    EnableSnapshotSession(after_work, root, jobs);
    ASSERT_TRUE(after_work.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_EQ(after_work.GetActiveRegionLedger().completed_work(),
              owner.GetActiveRegionLedger().completed_work());
    EXPECT_EQ(after_work.GetActiveRegionLedger().water_debts(),
              owner.GetActiveRegionLedger().water_debts());
    EXPECT_EQ(S7PlantBytes(after_work), S7PlantBytes(owner));
}

TEST(GameSessionHeadlessWorldTest,
     S7MalformedWaterFaultStopsHostWithoutPublishingDebtCursorOrWork) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "S7 water refusal");
    session.SetLocalPlayerSimulationPosition({8, 0, 8}, true);
    session.PinActiveRegion({0, 0}, true);
    auto good = S7HostWaterGrid({0, 0, 0}, 8), bad = S7HostWaterGrid({1, 0, 0}, 8);
    bad->water_bed_mm.pop_back();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(good));
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(bad));
    const auto prior_depth = good->water_depth_mm, prior_flux = good->water_edge_flux;
    const auto revision = good->water_revision();
    const auto telemetry = session.GetWaterSystem()->regional_work();
    TickOnce(session);
    ASSERT_TRUE(session.GetRegionSchedulerFault());
    EXPECT_EQ(good->water_depth_mm, prior_depth);
    EXPECT_EQ(good->water_edge_flux, prior_flux);
    EXPECT_EQ(good->water_revision(), revision);
    EXPECT_TRUE(session.GetActiveRegionLedger().water_debts().empty());
    EXPECT_EQ(session.GetActiveRegionLedger().records().at({0, 0}).water_cursor, 0u);
    EXPECT_EQ(session.GetWaterSystem()->regional_work(), telemetry);
    EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
    EXPECT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 0u);
    EXPECT_EQ(good->water_depth_mm, prior_depth);
}

TEST(GameSessionHeadlessWorldTest, S7IndexedMalformedWaterIsErrorAndCannotOpenOrRegenerate) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, reopened;
    CreateGroundWorld(owner, root, jobs, "S7 durable water refusal");
    auto chunk = S7HostWaterGrid({0, 0, 0}, 8);
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    // Inject malformed authority without a simulation visit. This saves an internally
    // consistent index/checksum over a semantically bad payload, rather than merely tearing bytes.
    chunk->water_edge_flux.pop_back();
    ASSERT_TRUE(owner.SaveWorldState());
    const auto read = P::WorldSaveService::read_chunk(owner.GetWorldSaveDir(), {0, 0, 0}, true);
    EXPECT_EQ(read.status, P::WorldSaveService::ChunkReadStatus::Error);
    EXPECT_EQ(read.chunk, nullptr);
    EnableSnapshotSession(reopened, root, jobs);
    EXPECT_FALSE(reopened.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_FALSE(reopened.GetWorldOpenError().empty());
}

TEST(GameSessionHeadlessWorldTest, S7WaterTickDoesNotLoadDurableOnlySourceTargets) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "S7 resident water admission");
    const Luminumbra::IVec3 coords{640, 0, 640};
    const RegionKey key{20, 20};
    session.PinActiveRegion(key, true);
    session.GetWorldSystem()->remember_durable_chunks({coords});
    int calls = 0;
    session.GetWorldSystem()->set_durable_chunk_loader(
        [&calls](const auto&) -> std::shared_ptr<Luminumbra::Chunk> {
            ++calls;
            throw std::runtime_error("Water attempted durable-only admission");
        },
        [](const auto&) {});
    const auto entity = session.GetRegistry().create();
    session.GetRegistry().emplace<SC::TransformComponent>(entity).position = {10241, 0, 10241};
    session.GetRegistry().emplace<SC::WaterFlowSourceComponent>(entity);
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(session.GetWorldSystem()->find_streamed_chunk(coords), nullptr);
    EXPECT_EQ(session.GetWaterSystem()->regional_work().used_windows, 0u);
    EXPECT_EQ(session.GetWaterSystem()->debug_source_injected_mm(), 0);
}

namespace {
void WriteSessionPolicy(const HeadlessRoot& root, std::uint32_t need_rate) {
    const auto path = root.path() / "data/common/sim/distant_policy.json";
    fs::create_directories(path.parent_path());
    nlohmann::json policy{{"schema", "luminumbra.distant_policy.v1"},
                          {"creature_species",
                           {{"fixture_creature",
                             {{"coarse_need_rate_ppm", need_rate},
                              {"coarse_travel_m_per_cadence", 0},
                              {"promote_distance_m", 512},
                              {"lifespan_cadences", 0},
                              {"reproduce_cadences", 0}}}}},
                          {"plant_species", nlohmann::json::object()}};
    std::ofstream(path, std::ios::binary) << policy.dump() << '\n';
}
} // namespace

TEST(GameSessionHeadlessWorldTest, EnabledContentPreflightRefusesBeforePublishingOrSavingWorld) {
    const HeadlessRoot root(false);
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    EXPECT_FALSE(session.CreateWorld("Missing content", "1337", "default"));
    EXPECT_NE(session.GetWorldOpenError().find("content admission failed"), std::string::npos);
    EXPECT_EQ(session.GetWorldSystem(), nullptr);
    EXPECT_EQ(session.GetSimulationContent(), nullptr);
    EXPECT_FALSE(session.SaveWorld());
    EXPECT_FALSE(fs::exists(root.path() / "worlds/saves"));
    Luminumbra::TestSupport::WriteSimulationContentFixture(root.path());
    ASSERT_TRUE(fs::remove(root.path() / "data/common/sim"));
    EXPECT_FALSE(session.CreateWorld("Missing policy parent", "1337", "default"));
    EXPECT_NE(session.GetWorldOpenError().find("content admission failed"), std::string::npos);
    EXPECT_EQ(session.GetWorldSystem(), nullptr);
    EXPECT_EQ(session.GetSimulationContent(), nullptr);
    EXPECT_FALSE(session.SaveWorld());
    EXPECT_FALSE(fs::exists(root.path() / "worlds/saves"));
    WriteSessionPolicy(root, 1);
    std::ofstream(root.path() / "data/common/sim/distant_policy.json", std::ios::binary) << "{}\n";
    EXPECT_FALSE(session.CreateWorld("Malformed policy", "1337", "default"));
    EXPECT_EQ(session.GetWorldSystem(), nullptr);
    EXPECT_EQ(session.GetSimulationContent(), nullptr);
    EXPECT_FALSE(session.SaveWorld());
    EXPECT_FALSE(fs::exists(root.path() / "worlds/saves"));
}

TEST(GameSessionHeadlessWorldTest, CustomPresetRawNulRefusesBeforePublishingOrSavingWorld) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    const auto preset = ReadClockTestFile(root.path() / "worlds/atlas/presets/default.json");
    for (const auto& invalid : {preset + '\0',
                                preset + '\0' + "garbage",
                                preset + '\x1a' + '\0' + "garbage",
                                '\0' + preset}) {
        EXPECT_FALSE(session.CreateWorld("Invalid custom preset", "1337", "default", &invalid));
        EXPECT_NE(session.GetWorldOpenError().find("raw NUL"), std::string::npos);
        EXPECT_EQ(session.GetWorldSystem(), nullptr);
        EXPECT_EQ(session.GetSimulationContent(), nullptr);
        EXPECT_FALSE(session.SaveWorld());
        EXPECT_FALSE(fs::exists(root.path() / "worlds/saves"));
    }
    ASSERT_TRUE(session.CreateWorld("Valid custom preset", "1337", "default", &preset));
    EXPECT_NE(session.GetWorldSystem(), nullptr);
    EXPECT_NE(session.GetSimulationContent(), nullptr);
}

TEST(GameSessionHeadlessWorldTest, AdoptedContentIsImmutableAndPolicyMismatchRefusesBothLoadPaths) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original;
    EnableSnapshotSession(original, root, jobs);
    ASSERT_TRUE(original.CreateWorld("Policy identity", "1337", "default"));
    ASSERT_TRUE(original.SaveWorldState());
    ASSERT_NE(original.GetSimulationContent(), nullptr);
    const auto identity = original.GetSimulationContent()->identity;
    const auto original_hash = SessionWorldHash(original);
    WriteSessionPolicy(root, 1000);
    EXPECT_EQ(original.GetSimulationContent()->identity, identity);
    EXPECT_EQ(SessionWorldHash(original), original_hash);
    EXPECT_THROW(original.SetActiveRegionsEnabled(false), std::logic_error);
    GameSession fresh;
    EnableSnapshotSession(fresh, root, jobs);
    ASSERT_TRUE(fresh.CreateTransientWorld("Changed content", "1337", "default"));
    EXPECT_NE(fresh.GetSimulationContent()->identity, identity);
    EXPECT_NE(fresh.FoldClockIntoEcologyHash("empty"), original.FoldClockIntoEcologyHash("empty"));
    const auto before =
        ReadClockTestFile(P::WorldSaveService::active_regions_path(original.GetWorldSaveDir()));
    EXPECT_FALSE(fresh.LoadWorldStateFrom(original.GetWorldSaveDir()));
    EXPECT_EQ(fresh.GetWorldOpenError(),
              Luminumbra::world::ActiveRegionLedger::kContentMismatchMessage);
    EXPECT_FALSE(fresh.SaveWorldStateTo(original.GetWorldSaveDir()));
    GameSession reopened;
    EnableSnapshotSession(reopened, root, jobs);
    EXPECT_FALSE(reopened.LoadWorld(original.GetMetadata().worldId));
    EXPECT_EQ(reopened.GetWorldOpenError(),
              Luminumbra::world::ActiveRegionLedger::kContentMismatchMessage);
    EXPECT_FALSE(reopened.SaveWorld());
    EXPECT_EQ(
        ReadClockTestFile(P::WorldSaveService::active_regions_path(original.GetWorldSaveDir())),
        before);
}

TEST(GameSessionHeadlessWorldTest, SpeciesSimulationMismatchRefusesAndPresentationChangesReload) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original;
    EnableSnapshotSession(original, root, jobs);
    ASSERT_TRUE(original.CreateWorld("Species identity", "1337", "default"));
    ASSERT_TRUE(original.SaveWorldState());
    const auto path = root.path() / "data/common/creatures/species/fixture_creature.json";
    std::ofstream(path, std::ios::binary)
        << "{\"id\":\"fixture_creature\",\"display_name\":\"Other name\",\"base_color\":[1,0,0]}\n";
    GameSession presentation;
    EnableSnapshotSession(presentation, root, jobs);
    ASSERT_TRUE(presentation.LoadWorld(original.GetMetadata().worldId))
        << presentation.GetWorldOpenError();
    EXPECT_EQ(presentation.GetSimulationContent()->identity,
              original.GetSimulationContent()->identity);
    std::ofstream(path, std::ios::binary)
        << "{\"id\":\"fixture_creature\",\"brain\":{\"wander_weight\":0.8}}\n";
    GameSession changed;
    EnableSnapshotSession(changed, root, jobs);
    EXPECT_FALSE(changed.LoadWorld(original.GetMetadata().worldId));
    EXPECT_EQ(changed.GetWorldOpenError(),
              Luminumbra::world::ActiveRegionLedger::kContentMismatchMessage);
    EXPECT_FALSE(changed.SaveWorld());
}

TEST(GameSessionHeadlessWorldTest, IndexedEnabledHistoricalLedgerAdoptsIdentityOnNextSave) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original;
    EnableSnapshotSession(original, root, jobs);
    ASSERT_TRUE(original.CreateWorld("Historical content", "1337", "default"));
    Luminumbra::world::ActiveRegionLedger historical;
    historical.pin({20, -20}, true, original.GetWorldClock());
    // Loading stamps the spawn anchor into anchorless ledgers; canonical bytes include it.
    historical.set_local_anchor(original.GetMetadata().spawnPoint);
    ASSERT_FALSE(historical.records().empty());
    ASSERT_FALSE(historical.content_identity());
    const auto save = original.GetWorldSaveDir();
    ASSERT_TRUE(
        P::WorldSaveService::save_active_regions(historical, original.GetWorldClock(), save));
    const auto before = ReadClockTestFile(P::WorldSaveService::active_regions_path(save));
    ASSERT_TRUE(P::snapshot_index_exists(save));
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    Luminumbra::world::ActiveRegionLedger unidentified;
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(unidentified, save));
    ASSERT_FALSE(unidentified.records().empty());
    ASSERT_FALSE(unidentified.content_identity());
    GameSession reopened;
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(original.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    ASSERT_NE(reopened.GetSimulationContent(), nullptr);
    const auto identity = reopened.GetSimulationContent()->identity;
    auto expected = historical;
    ASSERT_TRUE(expected.adopt_content_identity(identity));
    EXPECT_EQ(reopened.GetActiveRegionLedger().canonical_bytes(), expected.canonical_bytes());
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::active_regions_path(save)), before);
    ASSERT_TRUE(reopened.SaveWorldState());
    Luminumbra::world::ActiveRegionLedger persisted;
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(persisted, save));
    ASSERT_TRUE(persisted.content_identity());
    EXPECT_EQ(*persisted.content_identity(), identity);
    EXPECT_NE(ReadClockTestFile(P::WorldSaveService::active_regions_path(save)), before);
    GameSession matched;
    EnableSnapshotSession(matched, root, jobs);
    ASSERT_TRUE(matched.LoadWorld(original.GetMetadata().worldId)) << matched.GetWorldOpenError();
    EXPECT_EQ(matched.GetSimulationContent()->identity, identity);
    EXPECT_EQ(matched.GetActiveRegionLedger().canonical_bytes(), persisted.canonical_bytes());
    auto tampered = historical;
    auto changed_identity = identity;
    ++changed_identity.effective_policy_digest;
    ASSERT_TRUE(tampered.adopt_content_identity(changed_identity));
    ASSERT_TRUE(P::WorldSaveService::save_active_regions(tampered, matched.GetWorldClock(), save));
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    GameSession refused;
    EnableSnapshotSession(refused, root, jobs);
    EXPECT_FALSE(refused.LoadWorld(original.GetMetadata().worldId));
    EXPECT_EQ(refused.GetWorldOpenError(),
              Luminumbra::world::ActiveRegionLedger::kContentMismatchMessage);
    EXPECT_FALSE(refused.SaveWorld());
}

TEST(GameSessionHeadlessWorldTest, FlagOffPolicyHasNoEffectOnSaveHashOrArtifactBytes) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.CreateTransientWorld("Policy off", "1337", "default"));
    const auto before_hash = SessionWorldHash(session);
    const auto first = root.path() / "policy-off-first";
    const auto second = root.path() / "policy-off-second";
    ASSERT_TRUE(session.SaveWorldStateTo(first));
    nlohmann::json artifact{{"schema", "luminumbra.server_tick.v1"}, {"world_hash", before_hash}};
    Luminumbra::Server::AppendSimBudgetArtifact(
        artifact, session.GetSimBudgetTelemetry(), session.GetSimBudgetTelemetry());
    const auto before_artifact = artifact.dump(2) + "\n";
    WriteSessionPolicy(root, 9999);
    std::ofstream(root.path() / "data/common/sim/distant_policy.json", std::ios::binary)
        << "malformed";
    ASSERT_TRUE(session.SaveWorldStateTo(second));
    ExpectSameSaveTree(first, second);
    EXPECT_EQ(SessionWorldHash(session), before_hash);
    EXPECT_EQ(session.GetSimulationContent(), nullptr);
    EXPECT_FALSE(session.GetActiveRegionLedger().content_identity());
    EXPECT_FALSE(fs::exists(second / "chunks/region/active-regions.arl"));
    Luminumbra::Server::AppendSimBudgetArtifact(
        artifact, session.GetSimBudgetTelemetry(), session.GetSimBudgetTelemetry());
    EXPECT_EQ(artifact.dump(2) + "\n", before_artifact);
    GameSession legacy;
    legacy.SetRootPath(root.root_string());
    legacy.SetJobSystem(&jobs);
    legacy.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(legacy.CreateTransientWorld("Legacy malformed policy", "1337", "default"));
    EXPECT_EQ(SessionWorldHash(legacy), before_hash);
}

TEST(GameSessionHeadlessWorldTest, FlagOffLoadThenSavePreservesExistingWorldBytes) {
    const HeadlessRoot root(false);
    JobSystem jobs;
    jobs.startup(1);
    GameSession original;
    original.SetRootPath(root.root_string());
    original.SetJobSystem(&jobs);
    original.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(original.CreateWorld("Flag off round trip", "1337", "default"));
    ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(original, 0));
    ASSERT_TRUE(original.SaveWorldState());
    ASSERT_TRUE(original.SaveWorld());
    const auto save = original.GetWorldSaveDir();
    const auto before = root.path() / "flag-off-before-load";
    fs::copy(save, before, fs::copy_options::recursive);
    ASSERT_FALSE(P::snapshot_index_exists(save));
    ASSERT_FALSE(fs::exists(P::WorldSaveService::active_regions_path(save)));
    GameSession loaded;
    loaded.SetRootPath(root.root_string());
    loaded.SetJobSystem(&jobs);
    loaded.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_GT(loaded.GetLastLoadedChunkCount(), 0u);
    EXPECT_EQ(loaded.GetSimulationContent(), nullptr);
    EXPECT_FALSE(loaded.GetActiveRegionLedger().content_identity());
    // Loaded chunks are clean; force a rewrite so the comparison exercises serialization.
    const auto chunk = loaded.GetWorldSystem()->find_streamed_chunk(Luminumbra::IVec3(0, 0, 0));
    ASSERT_NE(chunk, nullptr);
    chunk->mark_dirty_for_save();
    Luminumbra::world::WorldStateSaveReport report;
    ASSERT_TRUE(loaded.SaveWorldState(&report));
    EXPECT_TRUE(report.saved);
    EXPECT_GT(report.chunks_dirty, 0u);
    ASSERT_TRUE(loaded.SaveWorld());
    ExpectSameSaveTree(save, before);
}

TEST(GameSessionHeadlessWorldTest, RegisteredPopulationIdentityRefusesChangedConfigurationOnLoad) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original;
    EnableSnapshotSession(original, root, jobs);
    original.SetSimulationPopulationIdentity("fixture-populator:v1", "configuration:v1");
    ASSERT_TRUE(original.CreateWorld("Population identity", "1337", "default"));
    EXPECT_THROW(original.SetSimulationPopulationIdentity("changed:v1"), std::logic_error);
    GameSession compatible;
    EnableSnapshotSession(compatible, root, jobs);
    compatible.SetSimulationPopulationIdentity("fixture-populator:v1", "configuration:v1");
    ASSERT_TRUE(compatible.LoadWorld(original.GetMetadata().worldId))
        << compatible.GetWorldOpenError();
    GameSession changed;
    EnableSnapshotSession(changed, root, jobs);
    changed.SetSimulationPopulationIdentity("fixture-populator:v1", "configuration:v2");
    EXPECT_FALSE(changed.LoadWorld(original.GetMetadata().worldId));
    EXPECT_EQ(changed.GetWorldOpenError(),
              Luminumbra::world::ActiveRegionLedger::kContentMismatchMessage);
    EXPECT_FALSE(changed.SaveWorld());
}

TEST(GameSessionHeadlessWorldTest, CreatureAuthoritySavesReloadsAndRemovesEmptyStore) {
    namespace CA = luminumbra::ai;
    namespace C = Luminumbra::Components;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original;
    EnableSnapshotSession(original, root, jobs);
    original.SetRegionSchedulerConfig({10, 3, 2});
    ASSERT_TRUE(original.CreateWorld("Creature authority", "1337", "default"));
    auto& ledger = const_cast<ActiveRegionLedger&>(original.GetActiveRegionLedger());
    std::uint64_t id = 0;
    ASSERT_TRUE(ledger.allocate_persistent_id(id));
    const auto pos = original.GetMetadata().spawnPoint;
    ASSERT_TRUE(original.NotifyGroundObjectEdit(pos));
    const auto region = ActiveRegionLedger::region_at(pos);
    auto& registry = original.GetRegistry();
    const auto e = registry.create();
    registry.emplace<C::TransformComponent>(e).position = pos;
    registry.emplace<C::CreatureComponent>(e).species_id =
        C::CreatureSpeciesId16("fixture_creature");
    auto& owner = registry.emplace<C::CreatureRegionComponent>(e);
    owner = {id, region.x, region.z, original.GetWorldClock().tick()};
    registry.emplace<C::CreatureCoarseComponent>(e);
    registry.emplace<C::CreatureGenomeComponent>(e).reproduce_cooldown = 37;
    registry.emplace<C::CreatureLocomotionStateComponent>(e).velocity_y = -1.25f;
    const auto expected = CA::SerializeCreatureSnapshot(CA::BuildCreatureSnapshot(registry));
    ASSERT_TRUE(original.SaveWorldState());
    EXPECT_TRUE(
        fs::exists(P::WorldSaveService::creature_entities_path(original.GetWorldSaveDir())));
    GameSession restored;
    EnableSnapshotSession(restored, root, jobs);
    std::size_t checked = 0;
    const GameSession::WorldOpenCheck check = [&](const P::SavedWorld&,
                                                  const Luminumbra::Systems::TerrainGenParams&,
                                                  std::uint32_t seed) {
        ++checked;
        EXPECT_EQ(seed, 1337u);
        return std::string{};
    };
    ASSERT_TRUE(restored.LoadWorld(original.GetMetadata().worldId, check))
        << restored.GetWorldOpenError();
    EXPECT_EQ(checked, 1u);
    EXPECT_EQ(restored.GetActiveRegionLedger().config(), original.GetActiveRegionLedger().config());
    EXPECT_EQ(restored.PresetContentHash(), original.PresetContentHash());
    EXPECT_TRUE(restored.InspectCurrentSavedWorld().error.empty());
    EXPECT_EQ(SessionWorldHash(restored), SessionWorldHash(original));
    EXPECT_EQ(CA::SerializeCreatureSnapshot(CA::BuildCreatureSnapshot(restored.GetRegistry())),
              expected);
    std::vector<entt::entity> entities;
    for (auto creature : restored.GetRegistry().view<C::CreatureComponent>())
        entities.push_back(creature);
    for (auto creature : entities)
        restored.GetRegistry().destroy(creature);
    ASSERT_TRUE(restored.SaveWorldState());
    EXPECT_FALSE(
        fs::exists(P::WorldSaveService::creature_entities_path(restored.GetWorldSaveDir())));
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(restored.GetWorldSaveDir(), index, nullptr));
    const auto absent =
        std::find_if(index.members.begin(), index.members.end(), [](const auto& member) {
            return member.path == "chunks/region/creature-entities.json";
        });
    ASSERT_NE(absent, index.members.end());
    EXPECT_FALSE(absent->present);
    GameSession incompatible;
    EnableSnapshotSession(incompatible, root, jobs);
    incompatible.SetRegionSchedulerConfig({11, 3, 2});
    ASSERT_TRUE(incompatible.CreateWorld("Creature override refusal", "1337", "default"));
    const auto* current_world = incompatible.GetWorldSystem();
    const auto current_ledger = incompatible.GetActiveRegionLedger().encode();
    EXPECT_FALSE(incompatible.LoadWorld(original.GetMetadata().worldId));
    EXPECT_NE(incompatible.GetWorldOpenError().find("explicit override"), std::string::npos);
    EXPECT_EQ(incompatible.GetWorldSystem(), current_world);
    EXPECT_EQ(incompatible.GetActiveRegionLedger().encode(), current_ledger);
    ASSERT_TRUE(incompatible.SaveWorld());
    ASSERT_TRUE(restored.CreateTransientWorld("Adopted scheduler", "1337", "default"));
    EXPECT_EQ(restored.GetActiveRegionLedger().config(), original.GetActiveRegionLedger().config());
}

TEST(GameSessionHeadlessWorldTest,
     NearLimitLegacyCreaturesValidateLoadAndSaveWithControllerStamps) {
    namespace CA = luminumbra::ai;
    namespace C = Luminumbra::Components;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original;
    EnableSnapshotSession(original, root, jobs);
    ASSERT_TRUE(original.CreateWorld("Near-limit legacy creatures", "1337", "default"));
    auto& ledger = const_cast<ActiveRegionLedger&>(original.GetActiveRegionLedger());
    std::uint64_t id = 0;
    for (std::size_t i = 0; i < CA::kMaxCreatures; ++i)
        ASSERT_TRUE(ledger.allocate_persistent_id(id));
    ASSERT_EQ(id, CA::kMaxCreatures);
    ASSERT_TRUE(original.NotifyGroundObjectEdit({0, 0, 0}));
    auto& registry = original.GetRegistry();
    const auto e = registry.create();
    registry.emplace<C::TransformComponent>(e);
    const auto species = C::CreatureSpeciesId16("fixture_creature");
    registry.emplace<C::CreatureComponent>(e).species_id = species;
    registry.emplace<C::CreatureRegionComponent>(e).persistent_id = 1;
    registry.emplace<C::CreatureCoarseComponent>(e);
    registry.emplace<C::NeedsComponent>(e).needs = {{std::string(1024, 'a'), 0.0f, 0.0f},
                                                    {std::string(1024, 'b'), 0.0f, 0.0f},
                                                    {std::string(940, 'c'), 0.0f, 0.0f}};
    ASSERT_TRUE(original.SaveWorldState());
    const auto save = original.GetWorldSaveDir();
    const auto path = P::WorldSaveService::creature_entities_path(save);
    std::size_t legacy_size = 0;
    {
        // Assemble compact legacy bytes from one record, without a second population-sized DOM
        // or a checked-in fixture. IDs differ in both the record and its Region component.
        auto prototype = CA::BuildCreatureSnapshot(registry).entities.front();
        prototype.components.erase("Controller");
        std::string bytes = "{\"entities\":[";
        bytes.reserve(CA::kMaxCreatureBytes);
        for (std::size_t i = 1; i <= CA::kMaxCreatures; ++i) {
            if (i != 1)
                bytes += ',';
            prototype.components["Region"]["data"]["persistent_id"] = i;
            bytes += nlohmann::json{{"id", i}, {"components", prototype.components}}.dump();
        }
        bytes += "],\"schema\":\"";
        bytes += CA::kCreatureSchema;
        bytes += "\"}\n";
        legacy_size = bytes.size();
        // Species 1 yields 66,873,718 bytes; the registered species changes only those digits.
        ASSERT_EQ(legacy_size,
                  66873718u + CA::kMaxCreatures * (std::to_string(species).size() - 1));
        ASSERT_LT(legacy_size, CA::kMaxCreatureBytes);
        ASSERT_GT(legacy_size + CA::kMaxCreatures * 63, CA::kMaxCreatureBytes);
        ASSERT_TRUE(P::WorldSaveService::durable_write_file(path, bytes));
        P::SnapshotIndex index;
        ASSERT_TRUE(P::read_snapshot_index(save, index, nullptr));
        const auto member =
            std::find_if(index.members.begin(), index.members.end(), [](const auto& m) {
                return m.path == "chunks/region/creature-entities.json";
            });
        ASSERT_NE(member, index.members.end());
        ASSERT_TRUE(member->present);
        member->bytes = bytes.size();
        member->fnv64 = P::fnv1a64_bytes(bytes.data(), bytes.size());
        ASSERT_TRUE(P::WorldSaveService::durable_write_file(P::snapshot_index_path(save),
                                                            P::encode_snapshot_index(index)));
    }
    // Catalogue inspection includes cross-store and canonical creature validation.
    const auto inspected = P::InspectSavedWorld(root.path(), original.GetMetadata().worldId);
    ASSERT_TRUE(inspected.error.empty()) << inspected.error;
    GameSession restored;
    EnableSnapshotSession(restored, root, jobs);
    ASSERT_TRUE(restored.LoadWorld(original.GetMetadata().worldId)) << restored.GetWorldOpenError();
    auto& loaded = restored.GetRegistry();
    std::size_t count = 0;
    for (const auto creature : loaded.view<C::CreatureComponent>()) {
        ++count;
        ASSERT_TRUE((loaded.all_of<C::CreatureControllerComponent, C::NeedsComponent>(creature)));
        EXPECT_FALSE(loaded.get<C::CreatureControllerComponent>(creature).physics_controlled);
        const auto& needs = loaded.get<C::NeedsComponent>(creature).needs;
        ASSERT_EQ(needs.size(), 3u);
        EXPECT_EQ(needs[0].name, std::string(1024, 'a'));
        EXPECT_EQ(needs[1].name, std::string(1024, 'b'));
        EXPECT_EQ(needs[2].name, std::string(940, 'c'));
    }
    EXPECT_EQ(count, CA::kMaxCreatures);
    ASSERT_TRUE(restored.SaveWorldState()) << restored.GetWorldOpenError();
    EXPECT_EQ(fs::file_size(path), legacy_size + CA::kMaxCreatures * 63);
    // The stamped file exceeds the legacy cap and must remain readable by the file loader.
    const auto saved = P::InspectSavedWorld(root.path(), original.GetMetadata().worldId);
    EXPECT_TRUE(saved.error.empty()) << saved.error;
    CA::CreatureSnapshot snapshot;
    ASSERT_TRUE(P::WorldSaveService::load_creature_entities(snapshot, save));
    // All strings and collections still fit their budgets, but the payload itself is too large.
    for (auto& record : snapshot.entities)
        record.components["Needs"]["data"]["needs"][2]["name"] = std::string(1024, 'c');
    EXPECT_THROW(CA::SerializeCreatureSnapshot(snapshot), std::invalid_argument);
}

TEST(GameSessionHeadlessWorldTest, CreatureRefusalsLeaveEveryPopulatedLiveMemberUnchanged) {
    namespace CA = luminumbra::ai;
    namespace C = Luminumbra::Components;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    CreateGroundWorld(owner, root, jobs, "Creature refusal source");
    auto& source_ledger = const_cast<ActiveRegionLedger&>(owner.GetActiveRegionLedger());
    std::uint64_t id = 0;
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(source_ledger.allocate_persistent_id(id));
    const auto source_position = owner.GetMetadata().spawnPoint;
    ASSERT_TRUE(owner.NotifyGroundObjectEdit(source_position));
    const auto source_region = ActiveRegionLedger::region_at(source_position);
    auto& source = owner.GetRegistry();
    const auto source_entity = source.create();
    source.emplace<C::TransformComponent>(source_entity).position = source_position;
    source.emplace<C::CreatureComponent>(source_entity).species_id =
        C::CreatureSpeciesId16("fixture_creature");
    source.emplace<C::CreatureRegionComponent>(source_entity) =
        C::CreatureRegionComponent{id, source_region.x, source_region.z, 0};
    source.emplace<C::CreatureCoarseComponent>(source_entity);
    ASSERT_TRUE(owner.SaveWorldState());
    const auto baseline = CA::BuildCreatureSnapshot(source);
    const auto save = owner.GetWorldSaveDir();
    for (const int refusal : {0, 1, 2, 3, 4}) {
        SCOPED_TRACE(refusal);
        const bool public_load = refusal == 4;
        auto incoming = baseline;
        auto& components = incoming.entities[0].components;
        if (refusal == 0 || public_load) {
            const auto unknown = C::CreatureSpeciesId16("unregistered_creature");
            ASSERT_NE(unknown, C::CreatureSpeciesId16("fixture_creature"));
            components["Creature"]["data"]["species_id"] = unknown;
        } else if (refusal == 1) {
            components["FieldEmitter"] = {{"version", 1},
                                          {"data",
                                           {{"channel", 0},
                                            {"rate_raw_per_tick", 1},
                                            {"radius_cells", std::numeric_limits<int>::max()}}}};
        } else if (refusal == 2) {
            components["Senses"] = {{"version", 1},
                                    {"data",
                                     {{"nearest_food_source", {{"kind", "null"}, {"id", 0}}},
                                      {"nearest_water_source", {{"kind", "null"}, {"id", 0}}},
                                      {"nearest_threat", {{"kind", "creature"}, {"id", 99}}},
                                      {"threat_distance_bits", 0}}}};
        }
        // Keep membership integrity valid, so semantic preflight is exercised.
        const auto bytes = CA::SerializeCreatureSnapshot(incoming);
        ASSERT_TRUE(P::WorldSaveService::durable_write_file(
            P::WorldSaveService::creature_entities_path(save), bytes));
        P::SnapshotIndex index;
        ASSERT_TRUE(P::read_snapshot_index(save, index, nullptr));
        for (auto& member : index.members)
            if (member.path == "chunks/region/creature-entities.json") {
                member.bytes = bytes.size();
                member.fnv64 = P::fnv1a64_bytes(bytes.data(), bytes.size());
            }
        ASSERT_TRUE(P::WorldSaveService::durable_write_file(P::snapshot_index_path(save),
                                                            P::encode_snapshot_index(index)));
        if (refusal == 0 || refusal == 3 || public_load)
            ASSERT_TRUE(P::WorldSaveService::validate_save(save));

        GameSession live;
        live.SetAetherStateEnabled(true);
        CreateGroundWorld(live, root, jobs, "Populated live creature refusal");
        TickOnce(live);
        ASSERT_FALSE(live.GetFieldPages().empty());
        ASSERT_NE(SpawnGround(live, 5200, -1500), 0u);
        const auto position = live.GetMetadata().spawnPoint;
        const auto plant = ClockTestPlant(live.GetRegistry(), position);
        ASSERT_TRUE(live.AdmitPlant(plant));
        auto& live_ledger = const_cast<ActiveRegionLedger&>(live.GetActiveRegionLedger());
        ASSERT_TRUE(live_ledger.allocate_persistent_id(id));
        ASSERT_EQ(id, incoming.entities[0].id);
        ASSERT_TRUE(live.NotifyGroundObjectEdit(position));
        const auto region = ActiveRegionLedger::region_at(position);
        auto& registry = live.GetRegistry();
        const auto creature = registry.create();
        registry.emplace<C::TransformComponent>(creature).position = position;
        registry.emplace<C::CreatureComponent>(creature).species_id =
            C::CreatureSpeciesId16("fixture_creature");
        registry.emplace<C::CreatureRegionComponent>(creature) =
            C::CreatureRegionComponent{id, region.x, region.z, live.GetWorldClock().tick()};
        registry.emplace<C::CreatureCoarseComponent>(creature);
        if (public_load)
            ASSERT_TRUE(live.SaveWorldState());
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
        chunk->set_state(Luminumbra::ChunkState::Idle);
        chunk->heightmap_data = {42.0f};
        chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(live.GetWorldSystem()->adopt_streamed_chunk(chunk));
        const auto runtime_entity = registry.create();
        registry.emplace<C::TransformComponent>(runtime_entity).position = {12, 34, 56};
        live.GetSimulationEventBus().publish(
            live.GetSimulationTickCount() + 1, "fixture.pending", "retained");
        const auto chunks = live.GetWorldSystem()->snapshot_streamed_chunks();
        const auto ledger = live.GetActiveRegionLedger().encode();
        const auto ground = live.GetGroundObjects().canonical_bytes();
        const auto fields = live.GetFieldPages().serialize();
        const auto energy = live.GetEnergyFieldState()->CanonicalRegionalBytes();
        const auto clock = live.GetWorldClock();
        const auto tick = live.GetSimulationTickCount();
        const auto schedule = live.GetRegionSchedule();
        const auto plants = S7PlantBytes(live);
        const auto creatures = CA::SerializeCreatureSnapshot(CA::BuildCreatureSnapshot(registry));
        const auto hash = SessionWorldHash(live);
        const auto metadata = live.GetMetadata();
        const auto directory = live.GetWorldSaveDir();
        const auto loaded_chunks = live.GetLastLoadedChunkCount();
        const auto pending_events = live.GetSimulationEventBus().pending_count();
        const auto simulation_clock = live.GetSimulationClock();
        const auto persistence = live.GetPersistenceStatus();
        const auto rejections = live.GetRegionInputRejections();
        const auto* world_system = live.GetWorldSystem();
        const auto* water_system = live.GetWaterSystem();
        const auto* physics_system = live.GetPhysicsSystem();
        const auto* wind_system = live.GetWindFieldSystem();
        const auto* weather_system = live.GetWeatherSystem();
        const auto* aether_system = live.GetAetherFieldSystem();
        const auto* energy_state = live.GetEnergyFieldState();
        const auto* scent_field = live.GetScentField();
        const auto* script_state = live.GetScriptState();
        const auto* species = live.GetSpeciesTable();
        const auto* content = live.GetSimulationContent();
        if (public_load)
            EXPECT_FALSE(live.LoadWorld(owner.GetMetadata().worldId));
        else
            EXPECT_FALSE(live.LoadWorldStateFrom(save));
        EXPECT_FALSE(live.GetWorldOpenError().empty());
        EXPECT_EQ(live.GetWorldSystem(), world_system);
        EXPECT_EQ(live.GetWaterSystem(), water_system);
        EXPECT_EQ(live.GetPhysicsSystem(), physics_system);
        EXPECT_EQ(live.GetWindFieldSystem(), wind_system);
        EXPECT_EQ(live.GetWeatherSystem(), weather_system);
        EXPECT_EQ(live.GetAetherFieldSystem(), aether_system);
        EXPECT_EQ(live.GetEnergyFieldState(), energy_state);
        EXPECT_EQ(live.GetScentField(), scent_field);
        EXPECT_EQ(live.GetScriptState(), script_state);
        EXPECT_EQ(live.GetSpeciesTable(), species);
        EXPECT_EQ(live.GetSimulationContent(), content);
        EXPECT_EQ(live.GetWorldSaveDir(), directory);
        EXPECT_EQ(live.GetMetadata().name, metadata.name);
        EXPECT_EQ(live.GetMetadata().seed, metadata.seed);
        EXPECT_EQ(live.GetMetadata().worldType, metadata.worldType);
        EXPECT_EQ(live.GetMetadata().worldId, metadata.worldId);
        EXPECT_EQ(live.GetMetadata().creationTime, metadata.creationTime);
        EXPECT_EQ(live.GetMetadata().spawnPoint, metadata.spawnPoint);
        EXPECT_EQ(live.GetLastLoadedChunkCount(), loaded_chunks);
        EXPECT_EQ(live.GetSimulationEventBus().pending_count(), pending_events);
        EXPECT_EQ(live.GetSimulationClock().accumulator(), simulation_clock.accumulator());
        EXPECT_EQ(live.GetSimulationClock().dropped_time_seconds(),
                  simulation_clock.dropped_time_seconds());
        EXPECT_EQ(live.GetSimulationClock().dropped_frame_count(),
                  simulation_clock.dropped_frame_count());
        EXPECT_EQ(live.GetPersistenceStatus().last_save_tick, persistence.last_save_tick);
        EXPECT_EQ(live.GetPersistenceStatus().last_save_ms, persistence.last_save_ms);
        EXPECT_EQ(live.GetPersistenceStatus().failures, persistence.failures);
        EXPECT_EQ(live.GetPersistenceStatus().parked_count, persistence.parked_count);
        EXPECT_EQ(live.GetPersistenceStatus().parked_bytes, persistence.parked_bytes);
        EXPECT_EQ(live.GetPersistenceStatus().oldest_parked_tick, persistence.oldest_parked_tick);
        EXPECT_EQ(live.GetPersistenceStatus().save_requested, persistence.save_requested);
        EXPECT_EQ(live.GetRegionInputRejections().anchors, rejections.anchors);
        EXPECT_EQ(live.GetRegionInputRejections().edits, rejections.edits);
        EXPECT_EQ(live.GetRegionInputRejections().pins, rejections.pins);
        EXPECT_EQ(live.GetRegionInputRejections().ground_spawns, rejections.ground_spawns);
        EXPECT_EQ(live.GetRegionInputRejections().field_pages, rejections.field_pages);
        EXPECT_FALSE(live.GetRegionSchedulerFault());
        EXPECT_EQ(live.GetActiveRegionLedger().encode(), ledger);
        EXPECT_EQ(live.GetGroundObjects().canonical_bytes(), ground);
        EXPECT_EQ(live.GetFieldPages().serialize(), fields);
        EXPECT_EQ(live.GetEnergyFieldState()->CanonicalRegionalBytes(), energy);
        EXPECT_EQ(live.GetWorldClock().tick(), clock.tick());
        EXPECT_EQ(live.GetWorldClock().calendar(), clock.calendar());
        EXPECT_EQ(live.GetSimulationTickCount(), tick);
        EXPECT_EQ(live.GetRegionSchedule(), schedule);
        EXPECT_EQ(live.GetWorldSystem()->snapshot_streamed_chunks(), chunks);
        EXPECT_EQ(chunk->heightmap_data, std::vector<float>{42.0f});
        EXPECT_EQ(S7PlantBytes(live), plants);
        EXPECT_EQ(CA::SerializeCreatureSnapshot(CA::BuildCreatureSnapshot(registry)), creatures);
        EXPECT_EQ(SessionWorldHash(live), hash);
        EXPECT_TRUE(registry.valid(plant));
        EXPECT_TRUE(registry.valid(creature));
        ASSERT_TRUE(registry.valid(runtime_entity));
        EXPECT_EQ(registry.get<C::TransformComponent>(runtime_entity).position,
                  Luminumbra::Vec3(12, 34, 56));
        if (refusal == 0 || public_load)
            EXPECT_NE(live.GetWorldOpenError().find("Unknown persistent creature species"),
                      std::string::npos);
        if (refusal == 3)
            EXPECT_NE(live.GetWorldOpenError().find("duplicate"), std::string::npos);
        if (public_load) {
            ASSERT_TRUE(live.SaveWorldState()) << live.GetWorldOpenError();
            ASSERT_TRUE(live.SaveWorld()) << live.GetWorldOpenError();
            EXPECT_TRUE(P::WorldSaveService::validate_save(directory));
            EXPECT_EQ(live.GetWorldSaveDir(), directory);
            EXPECT_EQ(S7PlantBytes(live), plants);
            EXPECT_EQ(CA::SerializeCreatureSnapshot(CA::BuildCreatureSnapshot(registry)),
                      creatures);
        }
    }
}

TEST(GameSessionHeadlessWorldTest, LegacyUnindexedWorldAdoptsEmptyCreatureStoreOnNextSave) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, restored;
    original.SetRootPath(root.root_string());
    original.SetJobSystem(&jobs);
    original.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(original.CreateWorld("Legacy creature absence", "1337", "default"));
    ASSERT_TRUE(original.SaveWorldState());
    ASSERT_FALSE(P::snapshot_index_exists(original.GetWorldSaveDir()));
    EnableSnapshotSession(restored, root, jobs);
    ASSERT_TRUE(restored.LoadWorld(original.GetMetadata().worldId)) << restored.GetWorldOpenError();
    EXPECT_TRUE(luminumbra::ai::BuildCreatureSnapshot(restored.GetRegistry()).entities.empty());
    ASSERT_TRUE(restored.SaveWorldState());
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(restored.GetWorldSaveDir(), index, nullptr));
    const auto member =
        std::find_if(index.members.begin(), index.members.end(), [](const auto& value) {
            return value.path == "chunks/region/creature-entities.json";
        });
    ASSERT_NE(member, index.members.end());
    EXPECT_FALSE(member->present);
    EXPECT_TRUE(restored.GetActiveRegionLedger().content_identity().has_value());
}

TEST(GameSessionHeadlessWorldTest, PreviousReleaseSnapshotWithoutCreatureMemberLoadsCleanly) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, restored;
    EnableSnapshotSession(original, root, jobs);
    ASSERT_TRUE(original.CreateWorld("Previous creature absence", "1337", "default"));
    ASSERT_TRUE(original.SaveWorldState());
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(original.GetWorldSaveDir(), index, nullptr));
    std::erase_if(index.members, [](const auto& member) {
        return member.path == "chunks/region/creature-entities.json";
    });
    ASSERT_TRUE(P::WorldSaveService::durable_write_file(
        P::snapshot_index_path(original.GetWorldSaveDir()), P::encode_snapshot_index(index)));
    EnableSnapshotSession(restored, root, jobs);
    ASSERT_TRUE(restored.LoadWorld(original.GetMetadata().worldId)) << restored.GetWorldOpenError();
    EXPECT_TRUE(luminumbra::ai::BuildCreatureSnapshot(restored.GetRegistry()).entities.empty());
    ASSERT_TRUE(restored.SaveWorldState());
    ASSERT_TRUE(P::read_snapshot_index(restored.GetWorldSaveDir(), index, nullptr));
    const auto member =
        std::find_if(index.members.begin(), index.members.end(), [](const auto& value) {
            return value.path == "chunks/region/creature-entities.json";
        });
    ASSERT_NE(member, index.members.end());
    EXPECT_FALSE(member->present);
}

TEST(GameSessionHeadlessWorldTest,
     FlagOffCreaturePersistenceMetadataChangesNoSaveHashOrArtifactBytes) {
    namespace C = Luminumbra::Components;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.CreateTransientWorld("Creature flag off", "1337", "default"));
    auto& registry = session.GetRegistry();
    const auto e = registry.create();
    registry.emplace<C::TransformComponent>(e);
    registry.emplace<C::CreatureComponent>(e);
    const auto hash = SessionWorldHash(session);
    const auto first = root.path() / "creature-off-first",
               second = root.path() / "creature-off-second";
    ASSERT_TRUE(session.SaveWorldStateTo(first));
    nlohmann::json artifact{{"schema", "luminumbra.server_tick.v1"}, {"world_hash", hash}};
    Luminumbra::Server::AppendSimBudgetArtifact(
        artifact, session.GetSimBudgetTelemetry(), session.GetSimBudgetTelemetry());
    const auto artifact_bytes = artifact.dump(2) + "\n";
    registry.emplace<C::CreatureRegionComponent>(e).persistent_id = 123;
    registry.emplace<C::CreatureCoarseComponent>(e).age_cadences = 456;
    registry.emplace<C::CreatureControllerComponent>(e).physics_controlled = true;
    registry.emplace<C::CreatureLocomotionStateComponent>(e).velocity_y = -7;
    ASSERT_TRUE(session.SaveWorldStateTo(second));
    ExpectSameSaveTree(first, second);
    EXPECT_EQ(SessionWorldHash(session), hash);
    EXPECT_FALSE(fs::exists(P::WorldSaveService::creature_entities_path(second)));
    EXPECT_FALSE(P::snapshot_index_exists(second));
    Luminumbra::Server::AppendSimBudgetArtifact(
        artifact, session.GetSimBudgetTelemetry(), session.GetSimBudgetTelemetry());
    EXPECT_EQ(artifact.dump(2) + "\n", artifact_bytes);
}

namespace {
// Preserve every file byte, including compressed records and index member order.
// Only transaction header fields vary on repeated successful commits; their
// generation/transaction progression is checked separately below.
std::map<std::string, std::string> RepeatedSaveBytes(const fs::path& save) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(save)) {
        if (!entry.is_regular_file())
            continue;
        const auto relative = fs::relative(entry.path(), save).generic_string();
        auto bytes = ReadClockTestFile(entry.path());
        if (relative == P::kSnapshotIndexRelativePath) {
            auto index = nlohmann::json::parse(bytes);
            index["generation"] = 0;
            index["txn_id"] = 0;
            bytes = index.dump();
        }
        files.emplace(relative, std::move(bytes));
    }
    return files;
}
} // namespace

TEST(GameSessionHeadlessWorldTest, RepeatedWorldAndStateSavesPreserveBytesWithValidationReuse) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Repeated saves", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_TRUE(session.SaveWorldState());
    const auto seeded = session.GetChunkValidationCacheStats();
    EXPECT_EQ(seeded.modes[0].writer_seeded, 1u);
    EXPECT_EQ(seeded.modes[1].writer_seeded, 1u);
    EXPECT_EQ(seeded.modes[0].decodes, 0u);
    EXPECT_EQ(seeded.modes[1].decodes, 0u);
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    const auto before = RepeatedSaveBytes(save);
    const auto hash = SessionWorldHash(session);
    P::SnapshotIndex previous;
    std::vector<std::string> errors;
    ASSERT_TRUE(P::read_snapshot_index(save, previous, &errors));
    for (int pass = 0; pass < 3; ++pass) {
        SCOPED_TRACE(pass);
        ASSERT_TRUE(session.SaveWorldState());
        ASSERT_TRUE(session.SaveWorld());
        EXPECT_EQ(RepeatedSaveBytes(save), before);
        EXPECT_EQ(SessionWorldHash(session), hash);
        P::SnapshotIndex current;
        ASSERT_TRUE(P::read_snapshot_index(save, current, &errors));
        EXPECT_EQ(current.generation, previous.generation + 2);
        EXPECT_EQ(current.txn_id, previous.txn_id + 2);
        EXPECT_EQ(current.tick, previous.tick);
        previous = std::move(current);
    }
    const auto reused = session.GetChunkValidationCacheStats();
    EXPECT_GT(reused.modes[0].hits, 0u);
    EXPECT_GT(reused.modes[1].hits, 0u);
    EXPECT_EQ(reused.modes[0].decodes, 0u);
    EXPECT_EQ(reused.modes[1].decodes, 0u);
    EXPECT_TRUE(errors.empty());
}

TEST(GameSessionHeadlessWorldTest, SuccessiveWorldsClearValidationCacheOnCreateAndLoad) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    const auto expect_empty_cache = [&session]() {
        const auto stats = session.GetChunkValidationCacheStats();
        EXPECT_EQ(stats.entries, 0u);
        EXPECT_EQ(stats.payload_bytes, 0u);
        EXPECT_EQ(stats.hits, 0u);
        EXPECT_EQ(stats.misses, 0u);
        EXPECT_EQ(stats.evictions, 0u);
        EXPECT_EQ(stats.spill_bytes, 0u);
        EXPECT_EQ(stats.spill_extent, 0u);
        for (const auto& mode : stats.modes) {
            EXPECT_EQ(mode.writer_seeded, 0u);
            EXPECT_EQ(mode.spill_hits, 0u);
            EXPECT_EQ(mode.spill_faults, 0u);
        }
    };
    const auto save_chunk = [&session](float density) {
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
        chunk->set_state(Luminumbra::ChunkState::Idle);
        constexpr std::size_t lattice = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                        (Luminumbra::CHUNK_SIZE_Y + 1) *
                                        (Luminumbra::CHUNK_SIZE_Z + 1);
        chunk->sdf_data.assign(lattice, density);
        chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
        ASSERT_TRUE(session.SaveWorldState());
        ASSERT_TRUE(session.SaveWorld());
        const auto saved_bytes = RepeatedSaveBytes(session.GetWorldSaveDir());
        // Writer facts seed independent permissive and strict successes.
        // Save again without edits to compare freshly read compressed bytes.
        ASSERT_TRUE(session.SaveWorldState());
        EXPECT_EQ(RepeatedSaveBytes(session.GetWorldSaveDir()), saved_bytes);
        ASSERT_GT(session.GetChunkValidationCacheStats().entries, 0u);
        EXPECT_GT(session.GetChunkValidationCacheStats().payload_bytes, 0u);
        EXPECT_GT(session.GetChunkValidationCacheStats().hits, 0u);
    };

    ASSERT_TRUE(session.CreateWorld("Cache first world", "1337", "default"));
    ASSERT_NO_FATAL_FAILURE(save_chunk(-1.0f));
    const auto first_id = session.GetMetadata().worldId;
    const auto first_hash = SessionWorldHash(session);
    const auto first_bytes = RepeatedSaveBytes(session.GetWorldSaveDir());

    ASSERT_TRUE(session.CreateWorld("Cache second world", "1337", "default"));
    const auto second_id = session.GetMetadata().worldId;
    ASSERT_NE(first_id, second_id);
    expect_empty_cache(); // Create cleared the first world's buffers and counters.
    ASSERT_TRUE(session.LoadWorld(first_id)) << session.GetWorldOpenError();
    EXPECT_EQ(SessionWorldHash(session), first_hash);
    ASSERT_GT(session.GetChunkValidationCacheStats().entries, 0u);
    ASSERT_TRUE(session.LoadWorld(second_id)) << session.GetWorldOpenError();
    expect_empty_cache(); // Loading an empty world also reclaims all prior facts.

    // Same record identity, different exact bytes in the second world.
    ASSERT_NO_FATAL_FAILURE(save_chunk(1.0f));
    const auto second_hash = SessionWorldHash(session);
    EXPECT_NE(first_hash, second_hash);
    const auto second_bytes = RepeatedSaveBytes(session.GetWorldSaveDir());
    EXPECT_NE(first_bytes.at("chunks/region/r.0.0.lmr"),
              second_bytes.at("chunks/region/r.0.0.lmr"));
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.SaveWorld());
    EXPECT_EQ(RepeatedSaveBytes(session.GetWorldSaveDir()), second_bytes);
    EXPECT_EQ(SessionWorldHash(session), second_hash);
    ASSERT_TRUE(session.LoadWorld(first_id)) << session.GetWorldOpenError();
    EXPECT_EQ(SessionWorldHash(session), first_hash);
    ASSERT_TRUE(session.LoadWorld(second_id)) << session.GetWorldOpenError();
    EXPECT_EQ(SessionWorldHash(session), second_hash);
}

TEST(GameSessionHeadlessWorldTest, SharedSystemConfigPreservesExplicitOff) {
    GameSession session;
    const auto off = luminumbra::core::SystemConfig::FromJsonString(R"({"sim":{
        "active_regions":{"enabled":false}, "aether_state":{"enabled":false},
        "water_high_res":{"enabled":false}, "hydrology_weather":{"enabled":false},
        "weather_events":{"enabled":false}}})");
    session.ApplySystemConfig(off);
    EXPECT_FALSE(session.ActiveRegionsEnabled());
    EXPECT_FALSE(session.AetherStateEnabled());
    EXPECT_FALSE(session.WeatherRainEnabled());
    EXPECT_FALSE(session.WeatherEventsEnabled());
    // Finite hydrology is an independent shipping gate, not a tuning selector.
    EXPECT_TRUE(session.FiniteHydrologyEnabled());
    session.SetFiniteHydrologyEnabled(false);
    EXPECT_FALSE(session.FiniteHydrologyEnabled());
}

TEST(GameSessionHeadlessWorldTest, WaterRuleIdentitiesObserveIndependentSetters) {
    GameSession first, second;
    EXPECT_EQ(first.SystemConfigIdentity(), second.SystemConfigIdentity());
    first.SetFiniteHydrologyEnabled(false);
    EXPECT_NE(first.SystemConfigIdentity(), second.SystemConfigIdentity());
    second.SetFiniteHydrologyEnabled(false);
    EXPECT_EQ(first.SystemConfigIdentity(), second.SystemConfigIdentity());
    first.SetFiniteHydrologyEnabled(true);
    second.SetFiniteHydrologyEnabled(true);
    first.SetWeatherRainEnabled(true, 26);
    EXPECT_NE(first.SystemConfigIdentity(), second.SystemConfigIdentity());
    second.SetWeatherRainEnabled(true, 26);
    EXPECT_EQ(first.SystemConfigIdentity(), second.SystemConfigIdentity());
}

TEST(GameSessionHeadlessWorldTest, EffectiveIdentityUsesDefaultsAndCurrentSetters) {
    using luminumbra::core::SystemConfig;
    GameSession direct, applied;
    applied.ApplySystemConfig(SystemConfig::Defaults());
    const auto baseline = direct.SystemConfigIdentity();
    EXPECT_FALSE(baseline.empty());
    EXPECT_EQ(baseline, applied.SystemConfigIdentity());
    EXPECT_EQ(baseline, "60ab3c242d54ce89");
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

TEST(GameSessionHeadlessWorldTest, FreshOpenDoesNotPlanTerrainOrPopulateRegionsBeforeFirstTick) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    // Exercise the defaults-on creation path, including the client's immediate state load.
    ASSERT_TRUE(session.CreateWorld("Fresh open work", "424242", "default"));
    const auto id = session.GetMetadata().worldId;
    const auto expect_unpopulated = [&session]() {
        ASSERT_NE(session.GetWorldSystem(), nullptr);
        EXPECT_EQ(session.GetWorldSystem()->debug_initial_load_plan_count(), 0u);
        EXPECT_TRUE(session.GetWorldSystem()->snapshot_streamed_chunks().empty());
        EXPECT_TRUE(session.GetWorldSystem()->durable_chunk_inventory().empty());
        EXPECT_TRUE(session.GetFieldPages().empty());
        EXPECT_TRUE(session.GetGroundObjects().empty());
        EXPECT_TRUE(session.GetActiveRegionLedger().records().empty());
        EXPECT_EQ(session.GetSimulationTickCount(), 0u);
        EXPECT_FALSE(session.GetRegionSchedulerFault());
    };
    ASSERT_NO_FATAL_FAILURE(expect_unpopulated());
    ASSERT_TRUE(session.LoadWorldState()) << session.GetWorldOpenError();
    ASSERT_NO_FATAL_FAILURE(expect_unpopulated());
    const auto fresh_decodes = P::WorldSaveService::chunk_decode_count_for_testing();
    const auto fresh_identity = session.InspectCurrentSavedWorld();
    ASSERT_TRUE(fresh_identity.error.empty()) << fresh_identity.error;
    EXPECT_EQ(fresh_identity.metadata.worldId, id);
    EXPECT_EQ(P::WorldSaveService::chunk_decode_count_for_testing(), fresh_decodes);
    ASSERT_NO_FATAL_FAILURE(expect_unpopulated());
    // A metadata-only reload also has no chunk admission decisions.
    ASSERT_TRUE(session.LoadWorld(id)) << session.GetWorldOpenError();
    ASSERT_NO_FATAL_FAILURE(expect_unpopulated());
}

namespace {
void AssertCatalogRefusesMalformedMetadata(const HeadlessRoot& root,
                                           GameSession& owner,
                                           GameSession& reopened) {
    for (const auto* filename : {"preset.json", "world_info.json"}) {
        const auto path = owner.GetWorldSaveDir() / filename;
        const auto original = ReadClockTestFile(path);
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "{";
            ASSERT_TRUE(output.good());
        }
        const auto changed_files = RepeatedSaveBytes(owner.GetWorldSaveDir());
        const auto cached = reopened.InspectCurrentSavedWorld();
        const auto uncached = P::InspectSavedWorld(root.root_string(), owner.GetMetadata().worldId);
        EXPECT_FALSE(cached.error.empty());
        EXPECT_EQ(cached.error, uncached.error);
        EXPECT_EQ(RepeatedSaveBytes(owner.GetWorldSaveDir()), changed_files);
        EXPECT_TRUE(reopened.GetWorldOpenError().empty());
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << original;
            ASSERT_TRUE(output.good());
        }
    }
}
} // namespace

TEST(GameSessionHeadlessWorldTest, IndexedOpenAdmitsEachRecordOnceThroughClientIdentity) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, reopened;
    for (auto* session : {&owner, &reopened}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
    }
    ASSERT_TRUE(owner.CreateWorld("Indexed admission work", "1337", "default"));
    const auto plan =
        owner.GetWorldSystem()->GetInitialChunkLoadList(owner.GetMetadata().spawnPoint);
    ASSERT_GE(plan.size(), 2u);
    const Luminumbra::IVec3 excluded{plan.front().x + 64, plan.front().y, plan.front().z};
    const auto near_a = S7HostWaterGrid(plan[0], 16);
    const auto near_b = S7HostWaterGrid(plan[1], 8);
    const auto far = S7HostWaterGrid(excluded, 4);
    for (const auto& chunk : {near_a, near_b, far})
        ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_NE(SpawnGround(owner, 100, 100), 0u);
    const auto plant = ClockTestPlant(owner.GetRegistry(), {.25f, 0, .25f});
    owner.GetRegistry().emplace<SC::SoilFeederComponent>(plant);
    owner.GetRegistry().emplace<SC::WaterSourceComponent>(plant);
    ASSERT_TRUE(owner.AdmitPlant(plant));
    ASSERT_TRUE(owner.SaveWorldState());
    ASSERT_TRUE(P::snapshot_index_exists(owner.GetWorldSaveDir()));
    ASSERT_FALSE(owner.GetFieldPages().empty());
    const auto saved_files = RepeatedSaveBytes(owner.GetWorldSaveDir());
    const auto plants = S7PlantBytes(owner);
    const auto ground = owner.GetGroundObjects().serialize();
    const auto pages = owner.GetFieldPages().serialize();
    using Service = Luminumbra::Persistence::WorldSaveService;
    const auto admissions = Service::decode_count_for_testing();
    const auto chunks = Service::chunk_decode_count_for_testing();
    const auto fields = Service::field_page_decode_count_for_testing();
    const auto objects = Service::ground_object_decode_count_for_testing();
    ASSERT_TRUE(reopened.LoadWorld(owner.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    EXPECT_EQ(Service::decode_count_for_testing() - admissions, 1u);
    EXPECT_EQ(Service::chunk_decode_count_for_testing() - chunks, 3u);
    EXPECT_EQ(Service::field_page_decode_count_for_testing() - fields, 1u);
    EXPECT_EQ(Service::ground_object_decode_count_for_testing() - objects, 1u);
    EXPECT_EQ(reopened.GetWorldSystem()->debug_initial_load_plan_count(), 1u);
    EXPECT_EQ(reopened.GetLastLoadedChunkCount(), 2u);
    EXPECT_EQ(reopened.GetWorldSystem()->snapshot_streamed_chunks().size(), 2u);
    EXPECT_EQ(reopened.GetWorldSystem()->find_streamed_chunk(excluded), nullptr);
    EXPECT_EQ(reopened.GetWorldSystem()->durable_chunk_inventory().size(), 3u);
    for (const auto& source : {near_a, near_b}) {
        const auto loaded = reopened.GetWorldSystem()->find_streamed_chunk(source->get_coords());
        ASSERT_NE(loaded, nullptr);
        S7CompareWater(*source, *loaded);
    }
    EXPECT_EQ(reopened.GetGroundObjects().serialize(), ground);
    EXPECT_EQ(reopened.GetFieldPages().serialize(), pages);
    EXPECT_EQ(S7PlantBytes(reopened), plants);
    EXPECT_EQ(reopened.GetWorldClock().canonical_bytes(), owner.GetWorldClock().canonical_bytes());
    EXPECT_EQ(RepeatedSaveBytes(owner.GetWorldSaveDir()), saved_files);
    // Use the same identity inspection as the render client after entering the world.
    // Both retained and excluded authority must hit admission facts without decoding again.
    const auto identity = reopened.InspectCurrentSavedWorld();
    ASSERT_TRUE(identity.error.empty()) << identity.error;
    EXPECT_EQ(identity.metadata.worldId, owner.GetMetadata().worldId);
    EXPECT_EQ(identity.metadata.seed, owner.GetMetadata().seed);
    EXPECT_EQ(identity.metadata.worldType, owner.GetMetadata().worldType);
    EXPECT_EQ(identity.preset_path, owner.GetWorldSaveDir() / "preset.json");
    EXPECT_EQ(Service::decode_count_for_testing() - admissions, 2u);
    EXPECT_EQ(Service::chunk_decode_count_for_testing() - chunks, 3u);
    EXPECT_EQ(RepeatedSaveBytes(owner.GetWorldSaveDir()), saved_files);
    // Exact-byte admission facts survive reset, so the next unchanged save only
    // verifies its baseline through the cache, including the nonresident record.
    const auto save_decodes = Service::chunk_decode_count_for_testing();
    ASSERT_TRUE(reopened.SaveWorldState());
    EXPECT_EQ(Service::chunk_decode_count_for_testing(), save_decodes);
    EXPECT_EQ(RepeatedSaveBytes(owner.GetWorldSaveDir()), saved_files);
    // Reinspection still reads current metadata and presets. Match the uncached catalog's
    // refusal exactly, preserve live state, and leave every on-disk byte untouched.
    ASSERT_NO_FATAL_FAILURE(AssertCatalogRefusesMalformedMetadata(root, owner, reopened));
    EXPECT_EQ(RepeatedSaveBytes(owner.GetWorldSaveDir()), saved_files);
    // Excluded authority stays available for later demand without eager residency.
    ASSERT_TRUE(reopened.GetWorldSystem()->restore_authoritative_chunk(excluded));
    const auto restored = reopened.GetWorldSystem()->find_streamed_chunk(excluded);
    ASSERT_NE(restored, nullptr);
    S7CompareWater(*far, *restored);

    // An excluded record still undergoes strict validation, even with a valid
    // index over its malformed bytes. A refusal cannot replace the live world.
    far->water_edge_flux.pop_back();
    far->mark_water_state_dirty();
    ASSERT_TRUE(owner.SaveWorldState());
    const auto corrupt_files = RepeatedSaveBytes(owner.GetWorldSaveDir());
    const auto* previous_world = reopened.GetWorldSystem();
    const auto refused_identity = reopened.InspectCurrentSavedWorld();
    const auto uncached_refusal =
        P::InspectSavedWorld(root.root_string(), owner.GetMetadata().worldId);
    EXPECT_FALSE(refused_identity.error.empty());
    EXPECT_EQ(refused_identity.error, uncached_refusal.error);
    EXPECT_EQ(reopened.GetWorldSystem(), previous_world);
    EXPECT_EQ(reopened.GetGroundObjects().serialize(), ground);
    EXPECT_EQ(reopened.GetFieldPages().serialize(), pages);
    EXPECT_EQ(S7PlantBytes(reopened), plants);
    EXPECT_EQ(RepeatedSaveBytes(owner.GetWorldSaveDir()), corrupt_files);
    EXPECT_FALSE(reopened.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_FALSE(reopened.GetWorldOpenError().empty());
    EXPECT_EQ(reopened.GetWorldSystem(), previous_world);
    EXPECT_EQ(reopened.GetGroundObjects().serialize(), ground);
    EXPECT_EQ(reopened.GetFieldPages().serialize(), pages);
    EXPECT_EQ(S7PlantBytes(reopened), plants);
    EXPECT_FALSE(reopened.SaveWorldState());
    EXPECT_EQ(RepeatedSaveBytes(owner.GetWorldSaveDir()), corrupt_files);
}

TEST(GameSessionHeadlessWorldTest,
     OpenKeepsPresetSnapshotAndAdvertisedIdentityAcrossDependencyMutation) {
    for (const bool persisted_chunks : {false, true}) {
        SCOPED_TRACE(persisted_chunks);
        const HeadlessRoot root;
        ASSERT_TRUE(fs::remove(root.path() / "data/common/biomes.json"));
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root.path());
        const auto table_path = root.path() / "data/common/biomes.json";
        const auto structures_path = root.path() / "data/common/structures";
        const auto write_bytes = [](const fs::path& path, const std::string& bytes) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << bytes;
            output.close();
            return output.good();
        };
        const std::string first_table = R"({"schema":"luminumbra.biome_table.v1","biomes":[{
            "id":0,"name":"grassland","surface":{
                "top":"Grass","filler":"Soil","depth":"Stone","underwater":"Sand"}}]})";
        auto changed_table = nlohmann::json::parse(first_table);
        changed_table["biomes"][0]["surface"]["top"] = "Stone";
        ASSERT_TRUE(write_bytes(table_path, first_table));
        JobSystem jobs;
        jobs.startup(1);
        GameSession owner, reopened;
        for (auto* session : {&owner, &reopened}) {
            session->SetRootPath(root.root_string());
            session->SetJobSystem(&jobs);
        }
        ASSERT_TRUE(owner.CreateWorld("Preset snapshot admission", "1337", "default"));
        if (persisted_chunks) {
            const auto plan =
                owner.GetWorldSystem()->GetInitialChunkLoadList(owner.GetMetadata().spawnPoint);
            ASSERT_FALSE(plan.empty());
            ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(S7HostWaterGrid(plan[0], 8)));
            ASSERT_TRUE(owner.SaveWorldState());
        }
        const auto preset_path = owner.GetWorldSaveDir() / "preset.json";
        const auto first = Luminumbra::world::LoadTerrainPreset(preset_path);
        ASSERT_TRUE(first.ok);
        ASSERT_TRUE(first.params.worldgen_dependencies);
        ASSERT_TRUE(first.params.worldgen_dependencies->biome_table.ok());
        ASSERT_FALSE(first.params.worldgen_dependencies->biome_table.empty());
        const auto saved_files = RepeatedSaveBytes(owner.GetWorldSaveDir());
        std::size_t biome_queries = 0;
        std::size_t structure_queries = 0;
        // The loader queries structures only after capturing the biome table. Change that
        // referenced file at this exact seam, before the residency planner or adoption runs.
        const Luminumbra::World::WorldgenFileStatusReader read_status = [&](const fs::path& path,
                                                                            std::error_code& ec) {
            if (path == table_path)
                ++biome_queries;
            if (path == structures_path) {
                ++structure_queries;
                EXPECT_TRUE(write_bytes(table_path, changed_table.dump()));
            }
            return Luminumbra::World::ReadWorldgenSymlinkStatus(path, ec);
        };
        ASSERT_TRUE(reopened.LoadWorld(owner.GetMetadata().worldId, read_status))
            << reopened.GetWorldOpenError();
        EXPECT_EQ(biome_queries, 1u);
        EXPECT_EQ(structure_queries, 1u);
        EXPECT_EQ(reopened.PresetContentHash(), first.preset_content_hash);
        const auto* world = reopened.GetWorldSystem();
        ASSERT_NE(world, nullptr);
        EXPECT_EQ(world->debug_initial_load_plan_count(), persisted_chunks ? 1u : 0u);
        EXPECT_EQ(reopened.GetLastLoadedChunkCount(), persisted_chunks ? 1u : 0u);
        EXPECT_EQ(world->biome_table().content_hash(),
                  first.params.worldgen_dependencies->biome_table.content_hash());
        ASSERT_TRUE(world->biomes_enabled());
        const auto biome = world->BiomeIdAt(8.0f, 8.0f);
        EXPECT_EQ(biome, 0u);
        EXPECT_EQ(world->SurfaceMaterialForColumn(39.5f, 40.0f, biome),
                  Luminumbra::MaterialType::Grass);
        const auto second = Luminumbra::world::LoadTerrainPreset(preset_path);
        ASSERT_TRUE(second.ok);
        ASSERT_TRUE(second.params.worldgen_dependencies);
        EXPECT_NE(second.preset_content_hash, first.preset_content_hash);
        EXPECT_NE(second.params.worldgen_dependencies->biome_table.content_hash(),
                  world->biome_table().content_hash());
        // The client must refuse a current-file identity that describes a different snapshot.
        const auto tick = reopened.GetSimulationTickCount();
        const auto refused = reopened.InspectCurrentSavedWorld();
        EXPECT_EQ(refused.error,
                  "Incompatible world preset: content changed since world admission.");
        EXPECT_EQ(reopened.GetWorldSystem(), world);
        EXPECT_EQ(reopened.GetSimulationTickCount(), tick);
        EXPECT_EQ(reopened.PresetContentHash(), first.preset_content_hash);
        EXPECT_TRUE(reopened.GetWorldOpenError().empty());
        EXPECT_EQ(RepeatedSaveBytes(owner.GetWorldSaveDir()), saved_files);
        EXPECT_EQ(ReadClockTestFile(table_path), changed_table.dump());
        ASSERT_TRUE(write_bytes(table_path, first_table));
        const auto identity = reopened.InspectCurrentSavedWorld();
        EXPECT_TRUE(identity.error.empty()) << identity.error;
        EXPECT_EQ(identity.preset_content_hash, reopened.PresetContentHash());
        EXPECT_EQ(identity.preset_source_json, first.source_json);
    }
}

TEST(GameSessionHeadlessWorldTest, CatalogPresetCompatibilityUsesCapturedSource) {
    for (const bool decode_chunks : {false, true}) {
        const HeadlessRoot root;
        JobSystem jobs;
        jobs.startup(1);
        GameSession owner;
        owner.SetRootPath(root.root_string());
        owner.SetJobSystem(&jobs);
        owner.SetActiveRegionsEnabled(false);
        ASSERT_TRUE(owner.CreateWorld("Captured preset compatibility", "1337", "default"));
        const auto save_dir = owner.GetWorldSaveDir();
        const auto preset_path = save_dir / "preset.json";
        const auto first_source = nlohmann::json::parse(ReadClockTestFile(preset_path));
        auto second_source = first_source;
        second_source["generation_params"]["terrain"]["height_offset"] = 42.0f;
        const auto metadata_path = save_dir / "world_info.json";
        auto metadata = nlohmann::json::parse(ReadClockTestFile(metadata_path));
        metadata["presetIdentity"] = P::StableChecksum(first_source.dump());
        {
            std::ofstream output(metadata_path, std::ios::binary | std::ios::trunc);
            output << metadata.dump();
            output.close();
            ASSERT_TRUE(output.good());
        }
        std::size_t mutations = 0;
        const Luminumbra::World::WorldgenFileStatusReader read_status = [&](const fs::path& path,
                                                                            std::error_code& ec) {
            if (path == root.path() / "data/common/structures") {
                ++mutations;
                std::ofstream output(preset_path, std::ios::binary | std::ios::trunc);
                output << second_source.dump();
                output.close();
                EXPECT_TRUE(output.good());
            }
            return Luminumbra::World::ReadWorldgenSymlinkStatus(path, ec);
        };
        P::WorldSaveAdmission admission;
        const auto saved = P::InspectSavedWorld(root.path(),
                                                owner.GetMetadata().worldId,
                                                {},
                                                decode_chunks ? &admission : nullptr,
                                                {},
                                                read_status);
        EXPECT_EQ(mutations, 1u);
        EXPECT_TRUE(saved.error.empty()) << saved.error;
        EXPECT_EQ(saved.preset_source_json, first_source.dump());
        if (decode_chunks) {
            ASSERT_TRUE(admission.preset.ok);
            EXPECT_EQ(admission.preset.source_json, saved.preset_source_json);
            EXPECT_EQ(admission.preset.preset_content_hash, saved.preset_content_hash);
        }
        EXPECT_EQ(ReadClockTestFile(preset_path), second_source.dump());
        // A separate inspection sees the later source and enforces the original compatibility
        // check.
        const auto changed = P::InspectSavedWorld(root.path(), owner.GetMetadata().worldId);
        EXPECT_EQ(changed.error,
                  "Incompatible world preset: saved preset identity does not match.");
    }
}

TEST(GameSessionHeadlessWorldTest, AutomaticSavesRetainWaterAndEditsWithoutWritingGeneratedCache) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    ASSERT_TRUE(session.CreateWorld("Automatic durable save", "1337", "default"));
    const auto water = S7HostWaterGrid({0, 0, 0}, 8);
    auto edited = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(1, 0, 0));
    edited->mark_voxel_data_dirty();
    auto generated = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(2, 0, 0));
    generated->mark_sdf_generated_current_params();
    auto distant_cache = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(64, 0, 0));
    distant_cache->mark_sdf_generated_current_params();
    for (const auto& chunk : {water, edited, generated, distant_cache}) {
        chunk->set_state(Luminumbra::ChunkState::Idle);
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    }
    const auto save = session.GetWorldSaveDir();
    const auto before_hash = session.ComputeChunkStateHash();
    const auto encodes = P::SnapshotStreamEncodeCountForTesting();
    bool reported = false;
    Luminumbra::Client::SaveSessionOnExit(&session, [] {}, [&] { reported = true; });
    ASSERT_TRUE(reported);
    EXPECT_EQ(P::SnapshotStreamEncodeCountForTesting() - encodes, 2u);
    EXPECT_FALSE(fs::exists(P::WorldSaveService::region_file_path(save, 2, 0)));
    EXPECT_EQ(session.ComputeChunkStateHash(), before_hash);
    EXPECT_FALSE(water->is_voxel_data_dirty());
    EXPECT_FALSE(edited->is_voxel_data_dirty());
    EXPECT_TRUE(water->has_water_authority());

    Luminumbra::WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(P::WorldSaveService{}.load_world(restored, save, errors));
    ASSERT_TRUE(errors.empty());
    ASSERT_EQ(restored.size(), 2u);
    ASSERT_NE(restored.find_chunk(water->get_id()), nullptr);
    S7CompareWater(*water, *restored.find_chunk(water->get_id()));
    EXPECT_NE(restored.find_chunk(edited->get_id()), nullptr);
    EXPECT_EQ(restored.find_chunk(generated->get_id()), nullptr);

    // A failed incremental automatic save must retain both the old snapshot and retry obligation.
    water->water_depth_mm.front() += 7;
    water->mark_water_state_dirty();
    const auto before_failure = RepeatedSaveBytes(save);
    P::WorldSaveService::set_before_active_regions_replace_for_testing([] { return false; });
    const bool failed =
        session.SaveWorldState(nullptr, Luminumbra::world::WorldStateSaveScope::DurableChanges);
    P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    EXPECT_FALSE(failed);
    EXPECT_TRUE(water->is_voxel_data_dirty());
    EXPECT_EQ(RepeatedSaveBytes(save), before_failure);
    const auto retry_encodes = P::SnapshotStreamEncodeCountForTesting();
    Luminumbra::world::WorldStateSaveReport report;
    ASSERT_TRUE(
        session.SaveWorldState(&report, Luminumbra::world::WorldStateSaveScope::DurableChanges));
    EXPECT_EQ(report.chunks_total, 4u);
    EXPECT_EQ(report.chunks_dirty, 1u);
    EXPECT_EQ(report.chunks_saved, 1u);
    EXPECT_EQ(P::SnapshotStreamEncodeCountForTesting() - retry_encodes, 1u);
    ASSERT_TRUE(P::WorldSaveService{}.load_world(restored, save, errors));
    ASSERT_EQ(restored.size(), 2u);
    ASSERT_NE(restored.find_chunk(water->get_id()), nullptr);
    S7CompareWater(*water, *restored.find_chunk(water->get_id()));
    EXPECT_FALSE(water->is_voxel_data_dirty());
}

namespace {
struct CopiedRegionObservation {
    fs::path path;
    std::size_t publications = 0;
};
CopiedRegionObservation& CopiedRegion() {
    static CopiedRegionObservation observation;
    return observation;
}
void ObserveCopiedRegion(const char* step, const fs::path& path) {
    if (std::string_view(step) == "before-replace" && path == CopiedRegion().path)
        ++CopiedRegion().publications;
}
struct ObserveRegionCopy {
    explicit ObserveRegionCopy(fs::path path) {
        CopiedRegion().path = std::move(path);
        CopiedRegion().publications = 0;
        P::set_snapshot_step_hook_for_testing(&ObserveCopiedRegion);
    }
    ~ObserveRegionCopy() {
        P::set_snapshot_step_hook_for_testing(nullptr);
        CopiedRegion().path.clear();
    }
};
} // namespace

TEST(GameSessionHeadlessWorldTest, SaveAsCopiesEachDistantRegionOnceWithExactCompressedBytes) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    for (auto* session : {&owner, &loaded})
        EnableSnapshotSession(*session, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Batched durable copy", "1337", "default"));
    for (int x : {1280, 1281, 1282}) {
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(x, 0, -1280));
        chunk->set_state(Luminumbra::ChunkState::Idle);
        chunk->sdf_data.assign(17 * 17 * 17, -4.0f);
        chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    }
    ASSERT_TRUE(owner.SaveWorldState());
    const auto source_region =
        P::WorldSaveService::region_file_path(owner.GetWorldSaveDir(), 40, -40);
    const auto source_bytes = ReadClockTestFile(source_region);
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId));
    ASSERT_TRUE(loaded.GetWorldSystem()->snapshot_streamed_chunks().empty());
    const auto hash = loaded.ComputeChunkStateHash();
    const auto destination = root.path() / "batched-copy";
    const auto target_region = P::WorldSaveService::region_file_path(destination, 40, -40);
    const ObserveRegionCopy observe(target_region);
    ASSERT_TRUE(loaded.SaveWorldStateTo(destination));
    EXPECT_EQ(CopiedRegion().publications, 1u);
    EXPECT_EQ(ReadClockTestFile(target_region), source_bytes);
    EXPECT_EQ(ReadClockTestFile(source_region), source_bytes);
    EXPECT_EQ(loaded.ComputeChunkStateHash(), hash);
    EXPECT_TRUE(loaded.GetWorldSystem()->snapshot_streamed_chunks().empty());

    // Failure after copying the region must roll back the destination and retain the source.
    const auto failed_destination = root.path() / "failed-batched-copy";
    P::WorldSaveService::set_before_active_regions_replace_for_testing([] { return false; });
    const bool copied = loaded.SaveWorldStateTo(failed_destination);
    P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    EXPECT_FALSE(copied);
    EXPECT_TRUE(RepeatedSaveBytes(failed_destination).empty());
    EXPECT_EQ(ReadClockTestFile(target_region), source_bytes);
    EXPECT_EQ(loaded.ComputeChunkStateHash(), hash);
    ASSERT_TRUE(loaded.SaveWorldStateTo(failed_destination));
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::region_file_path(failed_destination, 40, -40)),
              source_bytes);
}

TEST(GameSessionHeadlessWorldTest, ContentRefusalPrecedesChunkValidationAndTerrainPlanning) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, reopened;
    for (auto* session : {&owner, &reopened})
        EnableSnapshotSession(*session, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Content admission", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto before = RepeatedSaveBytes(owner.GetWorldSaveDir());
    const auto decodes = P::WorldSaveService::decode_count_for_testing();
    const auto chunk_decodes = P::WorldSaveService::chunk_decode_count_for_testing();
    std::size_t checked = 0;
    const GameSession::WorldOpenCheck refuse =
        [&](const P::SavedWorld& saved,
            const Luminumbra::Systems::TerrainGenParams& params,
            std::uint32_t seed) {
            ++checked;
            EXPECT_TRUE(saved.error.empty()) << saved.error;
            EXPECT_EQ(seed, 1337u);
            EXPECT_EQ(saved.metadata.worldId, owner.GetMetadata().worldId);
            EXPECT_EQ(params.height_offset, owner.GetWorldSystem()->get_params().height_offset);
            EXPECT_FALSE(saved.preset_source_json.empty());
            return std::string("content identity mismatch");
        };
    EXPECT_FALSE(reopened.LoadWorld(owner.GetMetadata().worldId, refuse));
    EXPECT_EQ(checked, 1u);
    EXPECT_EQ(reopened.GetWorldOpenError(), "content identity mismatch");
    EXPECT_EQ(reopened.GetWorldSystem(), nullptr);
    EXPECT_EQ(reopened.GetLastLoadedChunkCount(), 0u);
    EXPECT_EQ(P::WorldSaveService::decode_count_for_testing(), decodes);
    EXPECT_EQ(P::WorldSaveService::chunk_decode_count_for_testing(), chunk_decodes);
    EXPECT_EQ(RepeatedSaveBytes(owner.GetWorldSaveDir()), before);
    const GameSession::WorldOpenCheck accept =
        [](const P::SavedWorld&, const Luminumbra::Systems::TerrainGenParams&, std::uint32_t) {
            return std::string{};
        };
    ASSERT_TRUE(reopened.LoadWorld(owner.GetMetadata().worldId, accept))
        << reopened.GetWorldOpenError();
    EXPECT_EQ(reopened.GetLastLoadedChunkCount(), 1u);
    EXPECT_EQ(P::WorldSaveService::decode_count_for_testing() - decodes, 1u);
    EXPECT_EQ(RepeatedSaveBytes(owner.GetWorldSaveDir()), before);
}

TEST(GameSessionHeadlessWorldTest, TraversalAdmissionUsesFinalizedBiomeAndStructureContent) {
    const HeadlessRoot root;
    ASSERT_TRUE(fs::remove(root.path() / "data/common/biomes.json"));
    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root.path());
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, reopened;
    for (auto* session : {&owner, &reopened})
        EnableSnapshotSession(*session, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Traversal content admission", "1337", "default"));
    const auto* world = owner.GetWorldSystem();
    ASSERT_NE(world, nullptr);
    ASSERT_NE(world->get_params().biome_table_content_hash, 0u);
    ASSERT_NE(world->get_params().structures_content_hash, 0u);
    const auto world_identity =
        Luminumbra::World::ComputeTerrainParamsHash(world->get_params(), world->get_seed());
    const auto spawn_chunk = Luminumbra::Systems::SHIELD_WorldSystem::world_to_chunk_coords(
        owner.GetMetadata().spawnPoint);
    auto chunk = std::make_shared<Luminumbra::Chunk>(spawn_chunk);
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto saved = owner.InspectCurrentSavedWorld();
    ASSERT_TRUE(saved.error.empty()) << saved.error;
    const auto preset = Luminumbra::world::LoadTerrainPreset(saved.preset_path);
    ASSERT_TRUE(preset.ok);
    // Direct preset consumers (including benchmarks) must see the same identity too.
    EXPECT_EQ(Luminumbra::World::ComputeTerrainParamsHash(preset.params, world->get_seed()),
              world_identity);
    const auto source = nlohmann::json::parse(saved.preset_source_json);
    std::istringstream recording("luminumbra.traversal.v1\n"
                                 "duration_seconds 1\ntick_rate 30\nspeed_mps 1\nseed 1337\n"
                                 "preset default\npreset_revision 7\npreset_identity " +
                                 P::StableChecksum(source.dump()) + "\ncontent_identity " +
                                 std::to_string(world_identity) +
                                 "\nexpected_ticks 30\ncold_cache true\npoints 2\n"
                                 "point 0 0 0 0 0\npoint 1 0 0 0 0\n");
    auto traversal = Luminumbra::Client::Measurement::Traversal::read(recording);
    std::size_t checked = 0;
    const GameSession::WorldOpenCheck check =
        [&](const P::SavedWorld& candidate,
            const Luminumbra::Systems::TerrainGenParams& params,
            std::uint32_t seed) {
            ++checked;
            EXPECT_EQ(params.biome_table_content_hash,
                      world->get_params().biome_table_content_hash);
            EXPECT_EQ(params.structures_content_hash, world->get_params().structures_content_hash);
            const auto early_identity = Luminumbra::World::ComputeTerrainParamsHash(params, seed);
            EXPECT_EQ(early_identity, world_identity);
            const auto candidate_source = nlohmann::json::parse(candidate.preset_source_json);
            if (seed != traversal.seed || candidate.metadata.worldType != traversal.preset ||
                candidate_source.at("schema_rev") != traversal.presetRevision ||
                P::StableChecksum(candidate_source.dump()) != traversal.presetIdentity ||
                std::to_string(early_identity) != traversal.contentIdentity)
                return std::string("traversal world seed, preset or content identity mismatch");
            return std::string{};
        };
    const auto before = RepeatedSaveBytes(owner.GetWorldSaveDir());
    const auto decodes = P::WorldSaveService::decode_count_for_testing();
    const auto chunk_decodes = P::WorldSaveService::chunk_decode_count_for_testing();
    traversal.contentIdentity = std::to_string(world_identity ^ 1ull);
    EXPECT_FALSE(reopened.LoadWorld(owner.GetMetadata().worldId, check));
    EXPECT_EQ(checked, 1u);
    EXPECT_EQ(reopened.GetWorldOpenError(),
              "traversal world seed, preset or content identity mismatch");
    EXPECT_EQ(reopened.GetWorldSystem(), nullptr);
    EXPECT_EQ(reopened.GetLastLoadedChunkCount(), 0u);
    EXPECT_EQ(P::WorldSaveService::decode_count_for_testing(), decodes);
    EXPECT_EQ(P::WorldSaveService::chunk_decode_count_for_testing(), chunk_decodes);
    EXPECT_EQ(RepeatedSaveBytes(owner.GetWorldSaveDir()), before);

    traversal.contentIdentity = std::to_string(world_identity);
    ASSERT_TRUE(reopened.LoadWorld(owner.GetMetadata().worldId, check))
        << reopened.GetWorldOpenError();
    EXPECT_EQ(checked, 2u);
    const auto* loaded = reopened.GetWorldSystem();
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(Luminumbra::World::ComputeTerrainParamsHash(loaded->get_params(), loaded->get_seed()),
              world_identity);
    EXPECT_EQ(reopened.GetLastLoadedChunkCount(), 1u);
    EXPECT_EQ(P::WorldSaveService::decode_count_for_testing() - decodes, 1u);
    EXPECT_EQ(RepeatedSaveBytes(owner.GetWorldSaveDir()), before);
}
