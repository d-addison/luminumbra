// asset-manifest split. The engine-side GameSession validates
// SIMULATION requirements only (world preset readable/parseable); renderer/UI
// asset requirements are caller-supplied by the client. Headless CreateWorld
// must succeed in a root containing nothing but the world preset.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
// data/ client assets anywhere.
class HeadlessRoot {
public:
    HeadlessRoot() {
        root_ = fs::temp_directory_path() / "luminumbra_headless_world_test";
        fs::remove_all(root_);
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
TEST(GameSessionHeadlessWorldTest, WrongSizedSdfLatticeIsQuarantinedOnLoad) {
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
        ASSERT_TRUE(session.LoadWorldStateFrom(save_dir));

        auto* world = session.GetWorldSystem();
        ASSERT_NE(world, nullptr);
        const Chunk* corrupt = nullptr;
        const Chunk* control = nullptr;
        for (const Chunk* c : world->get_renderable_chunks()) {
            if (c->get_coords() == corrupt_coords)
                corrupt = c;
            if (c->get_coords() == control_coords)
                control = c;
        }
        ASSERT_NE(corrupt, nullptr) << "the quarantined chunk must still load (mesh intact)";
        ASSERT_NE(control, nullptr);
        EXPECT_TRUE(corrupt->sdf_data.empty())
            << "a wrong-sized SDF lattice must be quarantined (cleared for regeneration), "
               "not adopted verbatim — got size "
            << corrupt->sdf_data.size();
        EXPECT_EQ(control->sdf_data.size(), kFullLattice)
            << "a valid full lattice in the same save must survive adoption verbatim";
        EXPECT_FALSE(corrupt->mesh_vertices.empty())
            << "quarantine must keep the saved mesh renderable while regeneration is pending";
    }
    jobs.shutdown();
}

} // namespace
