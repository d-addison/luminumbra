#pragma once

// Test support for the seed-1337 region golden scenarios. Never include from src/.

#include "UniqueTempPath.h"
#include "WorldgenTestData.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <system_error>

namespace Luminumbra::TestSupport {

class RegionFixtureWorld {
public:
    RegionFixtureWorld()
        : root_(CreateUniqueTempDirectory("region_fixture_")) {
        try {
            const std::filesystem::path source(LUMINUMBRA_SOURCE_ROOT);
            const auto preset = root_ / "worlds/atlas/presets/default.json";
            std::filesystem::create_directories(preset.parent_path());
            std::ifstream input(source / "worlds/atlas/presets/default.json");
            auto document = nlohmann::json::parse(input);
            document["generation_params"]["terrain"]["island_mask_enabled"] = false;
            std::ofstream output(preset);
            output.exceptions(std::ios::badbit | std::ios::failbit);
            output << document.dump(2);
            output.close();
            CopyWorldgenTestData(source, root_);
            jobs_.startup(1);
        } catch (...) {
            RemoveRoot();
            throw;
        }
    }
    ~RegionFixtureWorld() {
        jobs_.shutdown();
        RemoveRoot();
    }
    RegionFixtureWorld(const RegionFixtureWorld&) = delete;
    RegionFixtureWorld& operator=(const RegionFixtureWorld&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const {
        return root_;
    }

    void Configure(world::GameSession& session, bool plant) {
        session.SetRootPath(Filesystem::GenericPathString(root_) + '/');
        session.SetJobSystem(&jobs_);
        session.SetActiveRegionsEnabled(false);
        session.SetAetherStateEnabled(false);
        session.SetWaterHighResEnabled(false);
        session.SetWeatherRainEnabled(false);
        session.SetWeatherEventsEnabled(false);
        session.SetFiniteHydrologyEnabled(false);
        const bool created =
            plant ? session.CreateTransientWorld("Plant region golden", "1337", "default")
                  : session.CreateWorld("Region golden", "1337", "default");
        if (!created || !session.LoadWorldStateFrom(root_ / "unmarked-snapshot"))
            throw std::runtime_error("cannot initialize region fixture world");
        if (!plant)
            session.GetWorldSystem()->SetWaterHydrology(false, 0, 0);
    }

    static void AddPlant(world::GameSession& session) {
        auto& registry = session.GetRegistry();
        const auto plant = registry.create();
        registry.emplace<Components::PlantTag>(plant);
        registry.emplace<Components::PlantGenomeComponent>(plant).genes.fill(0.5f);
        registry.emplace<Components::PlantGrowthComponent>(plant);
        registry.emplace<Components::TransformComponent>(plant).position = Vec3(8, 20, 8);
    }

    static void AddChunk(world::GameSession& session, int x) {
        auto chunk = std::make_shared<Chunk>(IVec3(x, 0, 0));
        chunk->set_state(ChunkState::Idle);
        chunk->heightmap_data = {8.0f + static_cast<float>(x),
                                 8.5f + static_cast<float>(x),
                                 9.0f + static_cast<float>(x)};
        if (x == 0)
            chunk->mark_voxel_data_dirty();
        if (!session.GetWorldSystem()->adopt_streamed_chunk(chunk))
            throw std::runtime_error("cannot adopt region fixture chunk");
    }

private:
    void RemoveRoot() const {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path root_;
    JobSystem jobs_;
};

} // namespace Luminumbra::TestSupport
