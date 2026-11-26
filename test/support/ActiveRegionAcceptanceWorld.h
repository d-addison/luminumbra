#pragma once

// Bounded session acceptance support only; never include from src/.
#include "GameSessionTestWorld.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/JobSystem.h"

#include <filesystem>
#include <memory>
#include <stdexcept>

namespace Luminumbra::TestSupport {

class ActiveRegionAcceptanceWorld {
public:
    explicit ActiveRegionAcceptanceWorld(unsigned workers) {
        jobs_.startup(workers);
    }
    ~ActiveRegionAcceptanceWorld() {
        jobs_.shutdown();
    }
    ActiveRegionAcceptanceWorld(const ActiveRegionAcceptanceWorld&) = delete;
    ActiveRegionAcceptanceWorld& operator=(const ActiveRegionAcceptanceWorld&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const {
        return root_.path();
    }

    void Configure(world::GameSession& session, bool enabled = true) {
        session.SetRootPath(Filesystem::GenericPathString(root_.path()) + '/');
        session.SetJobSystem(&jobs_);
        session.SetActiveRegionsEnabled(enabled);
        // Acceptance-only pressure/hold settings; shipping limits stay unlimited.
        session.SetRegionSchedulerConfig({10, 2, 1});
        session.SetAetherStateEnabled(false);
        session.SetWaterHighResEnabled(false);
        session.SetWeatherRainEnabled(false);
        session.SetWeatherEventsEnabled(false);
        session.SetFiniteHydrologyEnabled(false);
        // Active-region saves require a persistent world identity, even after
        // loading a snapshot. Keep the empty load to reset the ambient fields.
        if (!session.CreateWorld("Region acceptance", "1337", "default") ||
            !session.LoadWorldStateFrom(root_.path() / "unmarked-snapshot")) {
            throw std::runtime_error("cannot initialize region acceptance world: " +
                                     session.GetWorldOpenError());
        }
        session.GetWorldSystem()->SetWaterHydrology(false, 0, 0);
        session.SetLocalPlayerSimulationPosition(Vec3(256, 0, 256), true);
    }

    static void AdoptAuthority(world::GameSession& session) {
        // Exactly two tiny authoritative chunks; no update/streaming requests.
        for (const IVec3 coords : {IVec3(0, 0, 0), IVec3(1280, 0, -1280)}) {
            auto chunk = std::make_shared<Chunk>(coords);
            chunk->set_state(ChunkState::Idle);
            chunk->heightmap_data = {8.0f, 8.5f, 9.0f};
            chunk->mark_voxel_data_dirty();
            if (!session.GetWorldSystem()->adopt_streamed_chunk(chunk)) {
                throw std::runtime_error("cannot adopt acceptance authority");
            }
        }
    }

private:
    HeadlessRoot root_;
    JobSystem jobs_;
};

} // namespace Luminumbra::TestSupport
