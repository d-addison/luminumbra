#include "SavedWorldCatalog.h"

#include "SaveTransaction.h"
#include "WorldPersistenceRoundtrip.h"
#include "WorldSaveService.h"
#include "world/TerrainPresetLoader.h"
#include "world/WorldClock.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <stop_token>
#include <string>
#include <vector>

namespace Luminumbra::Persistence {
namespace {
namespace fs = std::filesystem;

bool SafeName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string::npos;
}

bool HasFrozenPreset(const std::string& name) {
    return name == "amplified" || name == "archipelago" || name == "cave_dense" ||
           name == "caverns" || name == "default" || name == "flat_lands" || name == "mountains" ||
           name == "temperate_forest";
}

bool ResolveSavedPreset(const fs::path& runtime_root,
                        const fs::path& save,
                        const nlohmann::json& metadata,
                        SavedWorld& result) {
    const bool requires_embedded = metadata.contains("presetFormat");
    if (requires_embedded && (!metadata.at("presetFormat").is_number_integer() ||
                              metadata.at("presetFormat") != kSavedPresetFormat)) {
        result.error = "Unsupported or corrupt saved preset format.";
        return false;
    }
    const auto embedded = save / "preset.json";
    const bool has_embedded = fs::exists(embedded);
    if (requires_embedded && !has_embedded) {
        result.error = "World preset is unavailable: required embedded preset.json is missing.";
        return false;
    }
    if (requires_embedded && !metadata.contains("presetIdentity")) {
        result.error = "Corrupt world metadata: required preset identity is missing.";
        return false;
    }
    // Only unmarked saves without embedded content may resolve packaged presets.
    const auto presets = HasFrozenPreset(result.metadata.worldType) ? "worlds/atlas/presets-rev6"
                                                                    : "worlds/atlas/presets";
    result.preset_path =
        has_embedded ? embedded : runtime_root / presets / (result.metadata.worldType + ".json");
    const auto preset = world::LoadTerrainPreset(result.preset_path, runtime_root / "data");
    if (!preset.ok) {
        result.error =
            preset.errors.empty() ? "World preset is unavailable." : preset.errors.front();
        return false;
    }
    std::ifstream input(result.preset_path);
    result.preset_json = nlohmann::json::parse(input).dump();
    if (metadata.contains("presetIdentity") &&
        (!metadata.at("presetIdentity").is_string() ||
         metadata.at("presetIdentity").get<std::string>() != StableChecksum(result.preset_json))) {
        result.error = "Incompatible world preset: saved preset identity does not match.";
        return false;
    }
    return true;
}
} // namespace

SavedWorld
InspectSavedWorld(const fs::path& root, const std::string& world_id, const std::stop_token& stop) {
    return InspectSavedWorld(root, root, world_id, stop);
}

SavedWorld InspectSavedWorld(const fs::path& root,
                             const fs::path& runtime_root,
                             const std::string& world_id,
                             const std::stop_token& stop) {
    SavedWorld result;
    result.metadata.worldId = world_id;
    result.metadata.name = world_id;
    if (stop.stop_requested()) {
        result.error = "Saved-world validation cancelled.";
        return result;
    }
    result.error = "World unavailable: its save directory or metadata is missing or unreadable.";
    if (!SafeName(world_id)) {
        result.error = "World unavailable: invalid save identifier.";
        return result;
    }
    try {
        const auto save = root / "worlds" / "saves" / world_id;
        if (!fs::is_directory(save) || fs::is_symlink(save))
            return result;
        const auto metadata_path = save / "world_info.json";
        // A pending journal means the directory may be mid-transaction: its metadata can be
        // missing, half-written or inconsistent until LoadWorld recovers it, so the entry stays
        // selectable and is described best-effort without validation.
        if (journal_pending(save)) {
            result.pending_recovery = true;
            result.error.clear();
            try {
                std::error_code size_ec;
                if (fs::is_regular_file(metadata_path) &&
                    fs::file_size(metadata_path, size_ec) <= 1024 * 1024 && !size_ec) {
                    std::ifstream pending_input(metadata_path);
                    const auto pending_json = nlohmann::json::parse(pending_input);
                    if (pending_json.is_object()) {
                        if (pending_json.contains("name") && pending_json.at("name").is_string())
                            result.metadata.name = pending_json.at("name").get<std::string>();
                        if (pending_json.contains("seed") && pending_json.at("seed").is_string())
                            result.metadata.seed = pending_json.at("seed").get<std::string>();
                    }
                }
            } catch (const std::exception&) {
                // Best effort only.
            }
            return result;
        }
        if (!fs::is_regular_file(metadata_path))
            return result;
        // Bound metadata to 1 MiB before parsing.
        constexpr std::uintmax_t kMaxMetadataBytes = 1'048'576;
        if (fs::file_size(metadata_path) > kMaxMetadataBytes) {
            result.error = "Corrupt world metadata: exceeds the 1 MiB limit.";
            return result;
        }
        std::ifstream input(metadata_path);
        if (!input)
            return result;
        const auto json = nlohmann::json::parse(input);
        if (json.is_object() && json.contains("name") && json.at("name").is_string())
            result.metadata.name = json.at("name").get<std::string>();
        // Preserve the persistence service's exact obsolete/future/corrupt diagnostics.
        std::vector<std::string> errors;
        if (!WorldSaveService::validate_save(save, &errors, stop)) {
            result.error = errors.empty() ? "Corrupt world save." : errors.front();
            return result;
        }
        result.metadata.name = json.value("name", "Unnamed World");
        result.metadata.seed = json.value("seed", "0");
        result.metadata.worldType = json.value("worldType", "default");
        result.metadata.creationTime = json.value("creationTime", std::time_t{0});
        result.has_spawn_point = json.contains("spawnPoint");
        if (result.has_spawn_point) {
            const auto& spawn = json.at("spawnPoint");
            result.metadata.spawnPoint = Vec3(
                spawn.at("x").get<float>(), spawn.at("y").get<float>(), spawn.at("z").get<float>());
            const auto& p = result.metadata.spawnPoint;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
                result.error = "Corrupt world metadata: spawn point must be finite.";
                return result;
            }
        }
        if (!WorldSaveService::read_clock_metadata(save,
                                                   result.clock,
                                                   result.requires_active_regions,
                                                   &errors,
                                                   nullptr,
                                                   &result.render_calendar,
                                                   &result.water_sim_cursor)) {
            result.error = errors.empty() ? "Corrupt world metadata." : errors.front();
            return result;
        }
        if (!SafeName(result.metadata.worldType)) {
            result.error = "Corrupt world metadata: invalid world type.";
            return result;
        }
        if (!ResolveSavedPreset(runtime_root, save, json, result))
            return result;
        result.error.clear();
    } catch (const nlohmann::json::exception& e) {
        result.error = std::string("Corrupt world metadata: ") + e.what();
    } catch (const fs::filesystem_error& e) {
        result.error = std::string("World unavailable: ") + e.what();
    }
    return result;
}

SavedWorldCatalog EnumerateSavedWorlds(const fs::path& root, const std::stop_token& stop) {
    return EnumerateSavedWorlds(root, root, stop);
}

SavedWorldCatalog EnumerateSavedWorlds(const fs::path& root,
                                       const fs::path& runtime_root,
                                       const std::stop_token& stop) {
    SavedWorldCatalog result;
    if (stop.stop_requested()) {
        result.error = "Saved-world validation cancelled.";
        return result;
    }
    try {
        const auto directory = root / "worlds" / "saves";
        if (!fs::exists(directory))
            return result;
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (stop.stop_requested()) {
                result.worlds.clear();
                result.error = "Saved-world validation cancelled.";
                return result;
            }
            if (entry.is_directory() || entry.is_symlink())
                result.worlds.push_back(
                    InspectSavedWorld(root, runtime_root, entry.path().filename().string(), stop));
        }
        std::sort(result.worlds.begin(), result.worlds.end(), [](const auto& a, const auto& b) {
            if (a.metadata.creationTime != b.metadata.creationTime)
                return a.metadata.creationTime > b.metadata.creationTime;
            return a.metadata.worldId < b.metadata.worldId;
        });
    } catch (const fs::filesystem_error& e) {
        result.error = std::string("Saved worlds unavailable: ") + e.what();
    }
    return result;
}

} // namespace Luminumbra::Persistence
