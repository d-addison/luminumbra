#include "SavedWorldCatalog.h"

#include "WorldSaveService.h"
#include "world/TerrainPresetLoader.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Luminumbra::Persistence {
namespace {
namespace fs = std::filesystem;

bool SafeName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string::npos;
}
} // namespace

SavedWorld
InspectSavedWorld(const fs::path& root, const std::string& world_id, const std::stop_token& stop) {
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
        if (!fs::is_regular_file(metadata_path))
            return result;
        if (fs::file_size(metadata_path) > 1024 * 1024) {
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
        if (json.contains("waterSimCursor") && (!json.at("waterSimCursor").is_number_unsigned())) {
            result.error = "Corrupt world metadata: invalid water simulation cursor.";
            return result;
        }
        result.water_sim_cursor = json.value("waterSimCursor", std::size_t{0});
        if (!SafeName(result.metadata.worldType)) {
            result.error = "Corrupt world metadata: invalid world type.";
            return result;
        }
        const auto embedded = save / "preset.json";
        result.preset_path = fs::exists(embedded) ? embedded
                                                  : root / "worlds/atlas/presets" /
                                                        (result.metadata.worldType + ".json");
        const auto preset = world::LoadTerrainPreset(result.preset_path);
        if (!preset.ok) {
            result.error =
                preset.errors.empty() ? "World preset is unavailable." : preset.errors.front();
            return result;
        }
        result.error.clear();
    } catch (const nlohmann::json::exception& e) {
        result.error = std::string("Corrupt world metadata: ") + e.what();
    } catch (const fs::filesystem_error& e) {
        result.error = std::string("World unavailable: ") + e.what();
    }
    return result;
}

SavedWorldCatalog EnumerateSavedWorlds(const fs::path& root, const std::stop_token& stop) {
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
                    InspectSavedWorld(root, entry.path().filename().string(), stop));
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
