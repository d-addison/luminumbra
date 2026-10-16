#pragma once

#include "world/WorldClock.h"
#include "world/WorldMetadata.h"

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace Luminumbra::Persistence {

struct SavedWorld {
    world::WorldMetadata metadata;
    std::filesystem::path preset_path;
    bool has_spawn_point = false;
    std::size_t water_sim_cursor = 0;
    world::WorldClock clock;
    bool requires_active_regions = false;
    // Empty only when metadata, preset and all existing save artifacts are supported.
    std::string error;
};

struct SavedWorldCatalog {
    std::vector<SavedWorld> worlds;
    // An unreadable save directory is distinct from an empty collection.
    std::string error;
};

// Read-only. Never creates directories, repairs files or generates terrain.
// The runtime data root also locates packaged presets, independently of save storage.
SavedWorld InspectSavedWorld(const std::filesystem::path& root,
                             const std::filesystem::path& data_root,
                             const std::string& world_id,
                             const std::stop_token& stop = {});
SavedWorldCatalog EnumerateSavedWorlds(const std::filesystem::path& root,
                                       const std::filesystem::path& data_root,
                                       const std::stop_token& stop = {});
// Convenience for runtimes whose saves and assets share a root.
SavedWorld InspectSavedWorld(const std::filesystem::path& root,
                             const std::string& world_id,
                             const std::stop_token& stop = {});
SavedWorldCatalog EnumerateSavedWorlds(const std::filesystem::path& root,
                                       const std::stop_token& stop = {});

} // namespace Luminumbra::Persistence
