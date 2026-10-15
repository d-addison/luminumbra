#pragma once

#include "world/WorldClock.h"
#include "world/WorldMetadata.h"
#include "world/WorldgenFileReader.h"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace Luminumbra::Systems {
struct TerrainGenParams;
}

namespace Luminumbra::Persistence {

struct WorldSaveAdmission;

struct SavedWorld {
    world::WorldMetadata metadata;
    std::filesystem::path preset_path;
    // Same captured preset source and dependency identity used during this inspection.
    std::string preset_source_json;
    std::string preset_content_hash;
    bool has_spawn_point = false;
    std::size_t water_sim_cursor = 0;
    world::WorldClock clock;
    bool requires_active_regions = false;
    // An interrupted indexed save left a journal. The listing is metadata-only; opening the world
    // recovers it first (inspection itself never mutates).
    bool pending_recovery = false;
    // Empty only when metadata, preset and all existing save artifacts are supported.
    std::string error;
};

using SavedWorldChunkSelector =
    std::function<bool(const SavedWorld&, const Systems::TerrainGenParams&, const IVec3&)>;

struct SavedWorldCatalog {
    std::vector<SavedWorld> worlds;
    // An unreadable save directory is distinct from an empty collection.
    std::string error;
};

// Read-only. Never creates directories or repairs files. An optional admission output
// carries validated records into the same synchronous open. Its residency callback runs
// after metadata/preset checks and must not mutate the save directory.
SavedWorld InspectSavedWorld(
    const std::filesystem::path& root,
    const std::string& world_id,
    const std::stop_token& stop = {},
    WorldSaveAdmission* admission = nullptr,
    const SavedWorldChunkSelector& retain = {},
    const World::WorldgenFileStatusReader& read_status = World::ReadWorldgenSymlinkStatus);
SavedWorldCatalog EnumerateSavedWorlds(const std::filesystem::path& root,
                                       const std::stop_token& stop = {});

} // namespace Luminumbra::Persistence
