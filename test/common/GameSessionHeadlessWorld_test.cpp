// asset-manifest split. The engine-side GameSession validates
// SIMULATION requirements only (world preset readable/parseable); renderer/UI
// asset requirements are caller-supplied by the client. Headless CreateWorld
// must succeed in a root containing nothing but the world preset.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "luminumbra_client/rendering/TimeOfDayModel.h"
#include "luminumbra_common/ai/CircadianSystem.h"
#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/ai/MigrationSystem.h"
#include "luminumbra_common/ai/StimulusChannels.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/replay/RegionScheduleTrace.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/PlantGrowthSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"

#include <nlohmann/json.hpp>

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/scripting/LuaState.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"

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

// Temp root containing ONLY worlds/atlas/presets/default.json — no res/ or
// data/ client assets anywhere. The root is unique per fixture instance so
// concurrent common_tests processes never share (or clobber) a directory; each
// TEST builds ONE instance and reuses it, so within-run path stability still
// holds. The destructor removes the tree.
class HeadlessRoot {
public:
    HeadlessRoot() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ =
            fs::temp_directory_path() / ("luminumbra_headless_world_test_" + std::to_string(stamp));
        fs::create_directories(root_ / "worlds" / "atlas" / "presets");
        fs::copy_file(SourcePresetPath(), root_ / "worlds" / "atlas" / "presets" / "default.json");
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
    const HeadlessRoot root;
    ASSERT_FALSE(fs::exists(root.path() / "res"));
    ASSERT_FALSE(fs::exists(root.path() / "data"));

    const WorldConfigValidationResult result =
        GameSession::ValidateWorldConfig(root.root_string(), "default");
    EXPECT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_TRUE(result.errors.empty());
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
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        // No SetRequiredClientAssets call: a headless host registers none.
        ASSERT_TRUE(session.CreateWorld("HeadlessWorld", "12345", "default"));
        EXPECT_NE(session.GetWorldSystem(), nullptr);
        EXPECT_NE(session.GetPhysicsSystem(), nullptr);
        EXPECT_FALSE(session.GetMetadata().worldId.empty());
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
    return {{"chunk", P::ComputeWorldStreamingStateHash(state)},
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
        ASSERT_EQ(original.TickSimulation(dt), 1u);
        ASSERT_EQ(control.TickSimulation(dt), 1u);
    }
    ASSERT_FALSE(original.GetWeatherSystem()->StormCells().empty());
    // Exercise nonempty energy epoch restoration with a deposit on the last tick.
    for (auto* session : {&original, &control}) {
        auto* energy = session->GetEnergyFieldState();
        energy->QueueDeposit(1, 0, 0, 0, 60000);
        energy->Tick(n);
    }
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
    ASSERT_TRUE(fs::remove(missing_save / "aether_state.efs"));
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

void CheckPartialPairReloadAndSave(const HeadlessRoot& root,
                                   JobSystem& jobs,
                                   const std::string& world_id,
                                   const fs::path& save) {
    const auto metadata_bytes = ReadClockTestFile(save / "world_info.json");
    const auto ledger_path = P::WorldSaveService::active_regions_path(save);
    const auto ledger_bytes = ReadClockTestFile(ledger_path);
    Luminumbra::world::ActiveRegionLedger durable;
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(durable, save));
    const auto tick =
        nlohmann::json::parse(metadata_bytes).at("simulationTick").get<std::uint64_t>();
    ASSERT_LT(durable.tick(), tick);
    for (const bool edit : {false, true}) {
        SCOPED_TRACE(edit);
        // Each fresh session must load the partial pair, including the edit case.
        std::ofstream(save / "world_info.json", std::ios::binary) << metadata_bytes;
        std::ofstream(ledger_path, std::ios::binary) << ledger_bytes;
        GameSession loaded, reloaded;
        for (auto* session : {&loaded, &reloaded}) {
            session->SetRootPath(root.root_string());
            session->SetJobSystem(&jobs);
            session->SetActiveRegionsEnabled(true);
        }
        ASSERT_TRUE(loaded.LoadWorld(world_id)) << loaded.GetWorldOpenError();
        EXPECT_EQ(loaded.GetSimulationTickCount(), tick);
        EXPECT_EQ(loaded.GetActiveRegionLedger().tick(), tick);
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

    // A failure between the two rewind replacements still leaves a readable pair.
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(session.LoadWorldStateFrom(alternate));
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool interrupted_save = session.SaveWorld();
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(interrupted_save);
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    EXPECT_EQ(P::InspectSavedWorld(root.path(), session.GetMetadata().worldId).clock.tick(), 2u);
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(saved_ledger, save));
    EXPECT_EQ(saved_ledger.tick(), 1u);
    ASSERT_NO_FATAL_FAILURE(
        CheckPartialPairReloadAndSave(root, jobs, session.GetMetadata().worldId, save));
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
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    P::WorldSaveService::set_before_active_regions_replace_for_testing([] { return false; });
    const bool saved = session.SaveWorld();
    P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    ASSERT_FALSE(saved);
    ASSERT_NO_FATAL_FAILURE(CheckPartialPairReloadAndSave(
        root, jobs, session.GetMetadata().worldId, session.GetWorldSaveDir()));
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
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    JobSystem jobs;
    jobs.startup(1);
    ASSERT_NO_FATAL_FAILURE(
        CheckPartialPairReloadAndSave(root, jobs, save.filename().string(), save));
    EXPECT_TRUE(fs::exists(debris.front())); // recovery never relies on cleaning it up
}
#endif

TEST(GameSessionHeadlessWorldTest, ActiveClockKeepsAmbientAnchorWhenClientSavesMovedSpawn) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, control, loaded, explicit_load, wrong_anchor;
    for (auto* session : {&original, &control, &loaded, &explicit_load, &wrong_anchor}) {
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
    // Model the bug: using the new respawn position for historical reconstruction.
    metadata["ambientFieldAnchor"] = metadata.at("spawnPoint");
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), explicit_save));
    ASSERT_TRUE(wrong_anchor.CreateTransientWorld("Wrong anchor", "1337", "default"));
    ASSERT_TRUE(wrong_anchor.LoadWorldStateFrom(explicit_save));
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
        EXPECT_NE(wrong_anchor.GetWeatherSystem()->ComputeWeatherSubHash(),
                  control.GetWeatherSystem()->ComputeWeatherSubHash());
        if (i < k) {
            for (auto* session : {&original, &control, &loaded, &explicit_load, &wrong_anchor})
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
    previous->QueueDeposit(1, 0, 0, 0, 40000);
    ASSERT_EQ(explicit_load.TickSimulation(dt), 1u);
    previous->QueueDeposit(2, 0, 0, 0, 1000);
    ASSERT_TRUE(explicit_load.LoadWorldStateFrom(save));
    for (auto* session : {&control, &loaded, &explicit_load}) {
        auto* energy = session->GetEnergyFieldState();
        ASSERT_EQ(energy->page_count(), 0u);
        EXPECT_EQ(energy->next_fire_tick(), 320u);
        const auto fires = energy->fires_completed();
        energy->QueueDeposit(1, 0, 0, 0, 60000);
        ASSERT_EQ(session->TickSimulation(dt), 1u);
        EXPECT_EQ(energy->at_cell(0, 0), 60000u);
        EXPECT_EQ(energy->fires_completed(), fires);
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
    GameSession legacy, control, loaded, explicit_load;
    for (auto* session : {&legacy, &control, &loaded, &explicit_load}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetAetherStateEnabled(true);
        session->SetActiveRegionsEnabled(session != &legacy);
    }
    ASSERT_TRUE(legacy.CreateWorld("Legacy energy", "1337", "default"));
    const double dt = legacy.GetSimulationClock().fixed_dt();
    for (int i = 0; i < 5; ++i)
        ASSERT_EQ(legacy.TickSimulation(dt), 1u);
    legacy.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 1);
    legacy.GetEnergyFieldState()->Tick(5);
    ASSERT_TRUE(legacy.SaveWorldState());
    const auto save = legacy.GetWorldSaveDir();
    ASSERT_FALSE(nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"))
                     .contains("simulationTick"));
    ASSERT_TRUE(control.LoadWorld(legacy.GetMetadata().worldId));
    ASSERT_EQ(control.GetSimulationTickCount(), 0u);
    ASSERT_EQ(control.GetEnergyFieldState()->total_raw(), 1u);
    ASSERT_EQ(control.GetEnergyFieldState()->next_fire_tick(), 3u);
    for (int i = 0; i < 7; ++i)
        ASSERT_EQ(control.TickSimulation(dt), 1u);
    ASSERT_EQ(control.GetEnergyFieldState()->page_count(), 0u);
    ASSERT_EQ(control.GetEnergyFieldState()->next_fire_tick(), 11u);
    const auto explicit_save = root.path() / "empty-offset-snapshot";
    ASSERT_TRUE(control.SaveWorldStateTo(explicit_save));
    EXPECT_EQ(ReadClockTestTextRecord(explicit_save / "aether_state.efs"), "EFS1 2 4\n");
    ASSERT_TRUE(control.SaveWorldState());
    EXPECT_EQ(ReadClockTestTextRecord(save / "aether_state.efs"), "EFS1 2 4\n");
    ASSERT_TRUE(loaded.LoadWorld(control.GetMetadata().worldId));
    ASSERT_TRUE(explicit_load.CreateTransientWorld("Explicit", "1337", "default"));
    ASSERT_TRUE(explicit_load.LoadWorldStateFrom(explicit_save));
    for (auto* session : {&control, &loaded, &explicit_load}) {
        auto* energy = session->GetEnergyFieldState();
        ASSERT_EQ(energy->page_count(), 0u);
        EXPECT_EQ(energy->next_fire_tick(), 11u);
        energy->QueueDeposit(1, 0, 0, 0, 60000);
    }
    for (int tick = 8; tick <= 15; ++tick) {
        SCOPED_TRACE(tick);
        for (auto* session : {&control, &loaded, &explicit_load}) {
            ASSERT_EQ(session->TickSimulation(dt), 1u);
            if (tick < 11)
                EXPECT_EQ(session->GetEnergyFieldState()->at_cell(0, 0), 60000u);
            else
                EXPECT_LT(session->GetEnergyFieldState()->at_cell(0, 0), 60000u);
        }
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(control));
        EXPECT_EQ(SessionWorldHash(explicit_load), SessionWorldHash(control));
    }
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
    original.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 1);
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
    EXPECT_EQ(loaded.GetEnergyFieldState()->next_fire_tick(), 16u);
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
    original.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 60000);
    ASSERT_EQ(original.TickSimulation(original.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(original.SaveWorldState());
    const auto save = original.GetWorldSaveDir();
    ASSERT_TRUE(transient.CreateTransientWorld("Snapshot reader", "1337", "default"));
    ASSERT_TRUE(transient.LoadWorldStateFrom(save));
    // Make every member writer observable if it runs before the refusal.
    ClockTestPlant(transient.GetRegistry(), transient.GetMetadata().spawnPoint);
    transient.GetEnergyFieldState()->QueueDeposit(2, 0, 0, 0, 1000);
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
    original.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 60000);
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

TEST(GameSessionHeadlessWorldTest, DefaultOffCallbacksKeepLegacySavesLoadsAndNestedTicks) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
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

TEST(GameSessionHeadlessWorldTest, DefaultOffKeepsLegacyMetadataBytesAndTickZeroLoad) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    for (auto* s : {&session, &loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
    }
    ASSERT_TRUE(session.CreateWorld("Legacy clock", "1337", "default"));
    const auto path = session.GetWorldSaveDir() / "world_info.json";
    const auto before = ReadClockTestFile(path);
    const auto& m = session.GetMetadata();
    const nlohmann::json legacy = {
        {"container_version", 2},
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
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, expected;
    for (auto* s : {&session, &expected}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        ASSERT_TRUE(s->CreateWorld("Region golden", "1337", "default"));
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
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded, expected_live, expected_loaded;
    for (auto* s : {&session, &loaded, &expected_live, &expected_loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
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
        ASSERT_TRUE(
            fs::remove(P::WorldSaveService::active_regions_path(original.GetWorldSaveDir())));
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

TEST(GameSessionHeadlessWorldTest, RegionScheduleAndWorldHashResumeAcrossBothHoldCounters) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, uninterrupted, render_only, loaded;
    for (auto* session : {&original, &uninterrupted, &render_only, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetRegionSchedulerConfig({10, 3, 2});
    }
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
                const auto chunks = session->GetWorldSystem()->snapshot_streamed_chunks();
                ASSERT_EQ(chunks.size(), 1u);
                chunks.front()->heightmap_data[0] = 12.0f;
                chunks.front()->mark_voxel_data_dirty();
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
        }
        if (tick == 3 || tick == 11) {
            const auto before = SessionWorldHash(original);
            ASSERT_TRUE(original.SaveWorldState());
            EXPECT_EQ(SessionWorldHash(original), before);
            ASSERT_TRUE(render_only.SaveWorldState());
            ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId))
                << loaded.GetWorldOpenError();
            EXPECT_EQ(loaded.GetActiveRegionLedger().encode(),
                      original.GetActiveRegionLedger().encode());
            EXPECT_EQ(SessionWorldHash(loaded), before);
        }
    }
    EXPECT_EQ(loaded.GetSimulationTickCount(), 20u);
}

TEST(GameSessionHeadlessWorldTest, RegionDigestMergesLiveAndDurableSimulationOnly) {
    const HeadlessRoot root;
    Luminumbra::WorldStreamingState durable;
    const auto old = durable.get_or_create_chunk({0, 0, 0});
    old->heightmap_data = {1.0f};
    const auto unloaded = durable.get_or_create_chunk({1, 0, 0});
    unloaded->heightmap_data = {2.0f};
    durable.get_or_create_chunk({32, 0, 0})->heightmap_data = {3.0f};
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
    const auto digest = P::WorldSaveService::region_simulation_digest({}, {0, 0}, {chunk});
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId));
    const auto chunks = loaded.GetWorldSystem()->snapshot_streamed_chunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_FALSE(chunks.front()->has_collision.load());
    EXPECT_EQ(chunks.front()->get_state(), Luminumbra::ChunkState::Idle);
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest({}, {0, 0}, chunks), digest);
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(session.GetWorldSaveDir(), {0, 0}, {}),
              digest);
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
    EXPECT_EQ(loaded.GetWorldSystem()->snapshot_streamed_chunks().size(), 2u);
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
        bytes[future ? 4 : 8] ^= future ? 3 : 1;
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
    disabled.SetRootPath(root.root_string());
    disabled.SetJobSystem(&jobs);
    EXPECT_FALSE(disabled.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_NE(disabled.GetWorldOpenError().find("Incompatible configuration"), std::string::npos);
    ASSERT_TRUE(fs::remove(path));
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
