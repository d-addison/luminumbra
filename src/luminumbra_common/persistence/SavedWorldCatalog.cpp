#include "SavedWorldCatalog.h"

#include "SavedJsonAdmission.h"
#include "WorldPersistenceRoundtrip.h"
#include "WorldSaveService.h"
#include "core/FilesystemPath.h"
#include "world/TerrainPresetLoader.h"
#include "world/WorldClock.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
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
    nlohmann::json embedded_json;
    bool embedded_missing = false;
    if (!ReadSavedJson(
            embedded, SavedInputLimits::kPresetBytes, embedded_json, embedded_missing, false)) {
        result.error = "Saved-input admission refused: invalid embedded world preset.";
        return false;
    }
    const bool has_embedded = !embedded_missing;
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
    if (!has_embedded) {
        bool missing = false;
        SavedJsonFailure failure = SavedJsonFailure::None;
        if (!ReadSavedJson(result.preset_path,
                           SavedInputLimits::kPresetBytes,
                           embedded_json,
                           missing,
                           false,
                           &failure) ||
            missing) {
            const bool unavailable = missing || failure == SavedJsonFailure::Unreadable;
            result.error = (unavailable ? "failed to open world preset: "
                                        : "failed to parse world preset JSON: ") +
                           Filesystem::GenericUtf8PathString(result.preset_path);
            return false;
        }
    }
    // Validate, hash and later boot from the same admitted bytes.
    const auto preset =
        world::LoadTerrainPresetFromJson(embedded_json,
                                         runtime_root / "data",
                                         Filesystem::GenericUtf8PathString(result.preset_path));
    if (!preset.ok) {
        result.error =
            preset.errors.empty() ? "World preset is unavailable." : preset.errors.front();
        return false;
    }
    result.preset_json = embedded_json.dump();
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
        const auto save = root / "worlds/saves" / world_id;
        if (!fs::is_directory(save) || fs::is_symlink(save))
            return result;
        const auto metadata_path = save / "world_info.json";
        nlohmann::json json;
        bool missing = false;
        const bool readable =
            ReadSavedJson(metadata_path, SavedInputLimits::kMetadataBytes, json, missing);
        if (missing)
            return result;
        if (!readable || !AdmitMetadata(json)) {
            result.error = "Corrupt world metadata: unreadable or exceeds the 2 MiB limit or text "
                           "compatibility limits.";
            return result;
        }
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
        const auto directory = root / "worlds/saves";
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
