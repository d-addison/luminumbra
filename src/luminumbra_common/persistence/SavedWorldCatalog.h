#pragma once

#include "world/WorldClock.h"
#include "world/WorldMetadata.h"

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace Luminumbra::Persistence {

// Presence of presetFormat opts into mandatory embedded content and identity.
inline constexpr int kSavedPresetFormat = 1;

struct SavedWorld {
    world::WorldMetadata metadata;
    std::filesystem::path preset_path;
    // Canonical JSON verified against presetIdentity; retained for subsequent saves.
    std::string preset_json;
    bool has_spawn_point = false;
    std::size_t water_sim_cursor = 0;
    world::WorldClock clock{0, world::WorldCalendar::Legacy()};
    std::optional<world::WorldCalendar> render_calendar;
    bool requires_active_regions = false;
    // An interrupted indexed save left a journal. The listing is metadata-only; opening the world
    // recovers it first (inspection itself never mutates).
    bool pending_recovery = false;
    // Empty only when metadata, preset and all existing save artifacts are supported.
    std::string error;
};

struct SavedWorldCatalog {
    std::vector<SavedWorld> worlds;
    // An unreadable save directory is distinct from an empty collection.
    std::string error;
};

// Read-only. Never creates directories, repairs files or generates terrain.
// Split roots keep runtime presets and dependencies independent of save storage.
SavedWorld InspectSavedWorld(const std::filesystem::path& save_root,
                             const std::filesystem::path& runtime_root,
                             const std::string& world_id,
                             const std::stop_token& stop = {});
SavedWorldCatalog EnumerateSavedWorlds(const std::filesystem::path& save_root,
                                       const std::filesystem::path& runtime_root,
                                       const std::stop_token& stop = {});
// Combined-root callers retain their original resolution semantics.
SavedWorld InspectSavedWorld(const std::filesystem::path& root,
                             const std::string& world_id,
                             const std::stop_token& stop = {});
SavedWorldCatalog EnumerateSavedWorlds(const std::filesystem::path& root,
                                       const std::stop_token& stop = {});

} // namespace Luminumbra::Persistence
