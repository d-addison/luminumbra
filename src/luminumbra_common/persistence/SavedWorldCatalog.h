#pragma once

#include "world/WorldMetadata.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Luminumbra::Persistence {

struct SavedWorld {
    world::WorldMetadata metadata;
    std::filesystem::path preset_path;
    bool has_spawn_point = false;
    std::size_t water_sim_cursor = 0;
    // Empty only when metadata, preset and all existing save artifacts are supported.
    std::string error;
};

struct SavedWorldCatalog {
    std::vector<SavedWorld> worlds;
    // An unreadable save directory is distinct from an empty collection.
    std::string error;
};

// Read-only. Never creates directories, repairs files or generates terrain.
SavedWorld InspectSavedWorld(const std::filesystem::path& root, const std::string& world_id);
SavedWorldCatalog EnumerateSavedWorlds(const std::filesystem::path& root);

} // namespace Luminumbra::Persistence
