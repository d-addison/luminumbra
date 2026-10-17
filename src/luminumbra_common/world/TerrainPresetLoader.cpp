#include "TerrainPresetLoader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "nlohmann/json_fwd.hpp"

#include "../ai/CreatureSpeciesRegistry.h"
#include "../core/FilesystemPath.h"
#include "../core/Log.h"
#include "BiomeTable.h"
#include "StructurePlacement.h"
#include "WorldgenDependencySnapshot.h"
#include "WorldgenFileReader.h"

namespace Luminumbra::world {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void HashBytes(std::uint64_t& hash, std::string_view bytes) {
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
}

std::string HashHex(std::uint64_t hash) {
    std::string hex(16, '0');
    for (std::size_t i = 0; i < hex.size(); ++i) {
        hex[hex.size() - 1 - i] = "0123456789abcdef"[hash & 0xfu];
        hash >>= 4u;
    }
    return hex;
}

std::string HashPresetIdentity(const nlohmann::json& preset, const nlohmann::json& manifest) {
    // Canonical envelope: sorted logical paths in dependencies, sorted preset keys,
    // compact ASCII JSON. Delimiters/escaping bind both paths and file boundaries.
    std::uint64_t hash = kFnvOffsetBasis;
    HashBytes(hash, "{\"dependencies\":");
    HashBytes(hash, manifest.dump(-1, ' ', true));
    HashBytes(hash, ",\"preset\":");
    HashBytes(hash, CanonicalResolvedTerrainPresetJson(preset));
    HashBytes(hash, "}");
    return HashHex(hash);
}

bool ResolveDependencyManifest(TerrainPresetLoadResult& preset,
                               const std::filesystem::path& data_root,
                               nlohmann::json& manifest,
                               std::vector<std::string>& errors,
                               const World::WorldgenFileStatusReader& read_status,
                               const World::WorldgenFileReader& dependency_reader) {
    // Use the generator's loaders to record their decisions and only the bytes
    // they consume. In particular, a rejected placement stops that pool before
    // any pieces are read; malformed pieces reject a pool after all piece reads.
    std::map<std::string, nlohmann::json> entries;
    auto snapshot = std::make_shared<World::WorldgenDependencySnapshot>();
    const auto logical_path = [&](const std::filesystem::path& relative) {
        const auto logical = Filesystem::LexicallyNormalPath(relative);
        if (logical.empty() || logical == "." || Filesystem::IsAbsolutePath(logical) ||
            logical.has_root_name() || logical.has_root_directory()) {
            errors.push_back("worldgen dependency must be data_root-relative: " +
                             Filesystem::GenericUtf8PathString(relative));
            return std::string();
        }
        for (const auto& component : logical) {
            if (component == "..") {
                errors.push_back("worldgen dependency escapes data_root: " +
                                 Filesystem::GenericUtf8PathString(relative));
                return std::string();
            }
        }
        auto text = Filesystem::GenericUtf8PathString(logical);
        if (!World::IsValidDependencyPathUtf8(text)) {
            errors.push_back("worldgen dependency logical path is not valid UTF-8");
            return std::string();
        }
        return text;
    };
    const auto state = [&](const std::string& logical, const char* outcome) {
        auto& entry = entries[logical];
        if (entry.is_null())
            entry = nlohmann::json::array({logical, outcome});
        else
            entry.push_back(outcome);
    };
    const World::WorldgenFileReader read_file = [&](const std::filesystem::path& path,
                                                    std::string& bytes) {
        const auto logical = logical_path(path.lexically_relative(data_root));
        if (logical.empty())
            return false;
        if (!dependency_reader(path, bytes)) {
            errors.push_back("failed to read worldgen dependency: " + logical);
            return false;
        }
        std::uint64_t hash = kFnvOffsetBasis;
        HashBytes(hash, bytes);
        auto& entry = entries[logical];
        if (entry.is_null()) {
            entry = nlohmann::json::array({logical, HashHex(hash)});
        } else if (entry[1] != HashHex(hash)) {
            errors.push_back("worldgen dependency changed between reads: " + logical);
            return false;
        }
        return true;
    };
    const auto dependency_exists = [&](const std::filesystem::path& path) {
        std::error_code ec;
        const auto status = read_status(path, ec);
        // A not-found status can carry ENOENT/ENOTDIR; other status failures
        // must refuse identity construction, just like throwing symlink_status.
        if (ec && status.type() != std::filesystem::file_type::not_found)
            throw std::filesystem::filesystem_error("worldgen dependency status", path, ec);
        return std::filesystem::exists(status);
    };
    try {
        if (preset.params.biomes_enabled) {
            const auto relative = Filesystem::PathFromUtf8(preset.extras.biomes.table);
            const auto logical = logical_path(relative);
            if (logical.empty())
                return false;
            const auto path = data_root / Filesystem::LexicallyNormalPath(relative);
            if (!dependency_exists(path)) {
                state(logical, "biome-table-absent:legacy-classification");
            } else {
                snapshot->biome_table = World::BiomeTable::Load(path, read_file);
                const auto& table = snapshot->biome_table;
                if (!errors.empty())
                    return false;
                state(logical,
                      table.ok() && !table.empty()
                          ? "biome-table-accepted"
                          : "biome-table-rejected/empty:legacy-classification");
            }
        }
        if (preset.params.structures_enabled) {
            const auto structures = data_root / "common/structures";
            if (!dependency_exists(structures)) {
                state("common/structures", "structures-directory-absent");
            } else {
                if (!std::filesystem::is_directory(structures)) {
                    errors.push_back("unreadable worldgen dependency directory: common/structures");
                    return false;
                }
                state("common/structures", "structures-directory-present");
                std::vector<std::filesystem::path> type_dirs;
                for (const auto& type : std::filesystem::directory_iterator(structures)) {
                    if (type.is_directory())
                        type_dirs.push_back(type.path());
                }
                std::sort(type_dirs.begin(), type_dirs.end(), [](const auto& a, const auto& b) {
                    return Filesystem::GenericUtf8PathString(a.filename()) <
                           Filesystem::GenericUtf8PathString(b.filename());
                });
                for (const auto& type : type_dirs) {
                    const auto relative_type =
                        std::filesystem::path("common/structures") / type.filename();
                    const auto logical = logical_path(relative_type);
                    if (logical.empty())
                        return false;
                    // Follow placement symlinks like the direct loader: a dangling link
                    // is absent, so identity and snapshot both use compiled defaults.
                    // Other status errors throw and refuse the preset below.
                    if (!std::filesystem::exists(type / "placement.json"))
                        state(logical + "/placement.json", "placement-absent:compiled-defaults");
                    auto pool = World::LoadStructureTemplatePool(
                        type, Filesystem::GenericUtf8PathString(type.filename()), read_file);
                    if (pool.io_failed) {
                        errors.push_back("failed to read worldgen structure pool: " + logical);
                        return false;
                    }
                    if (!errors.empty())
                        return false;
                    state(logical, pool.ok() ? "pool-accepted" : "pool-rejected/empty");
                    snapshot->structure_pools.push_back(std::move(pool));
                }
            }
        }
        // Species are session inputs even when biome/structure generation is off.
        // Parse through the manifest reader, then retain that exact registry so
        // GameSession cannot reopen different bytes after identity construction.
        auto species = std::make_shared<luminumbra::ai::CreatureSpeciesRegistry>();
        const auto species_dir = data_root / "common/creatures/species";
        if (dependency_exists(species_dir)) {
            state("common/creatures/species", "species-directory-present");
            std::vector<std::string> species_errors;
            bool species_io_failed = false;
            species->LoadFromDirectory(
                species_dir, species_errors, read_file, &species_io_failed, read_status);
            if (species_io_failed) {
                errors.push_back("failed to read session species dependencies");
                for (const auto& message : species_errors)
                    errors.push_back("Species definition load: " + message);
                return false;
            }
            if (!errors.empty())
                return false;
            for (const auto& message : species_errors) {
                preset.warnings.push_back("Species definition load: " + message);
                LUMINUMBRA_CORE_WARN("Species definition load: {}", message);
            }
        }
        snapshot->species_table = std::move(species);
        manifest = nlohmann::json::array();
        for (const auto& [logical, entry] : entries) {
            manifest.push_back(entry);
            snapshot->manifest.push_back(entry.get<std::vector<std::string>>());
        }
    } catch (const std::filesystem::filesystem_error& error) {
        errors.push_back("failed to resolve worldgen dependencies: " + std::string(error.what()));
        return false;
    }
    preset.params.worldgen_dependencies = std::move(snapshot);
    return true;
}

void WarnUnknownKeys(const nlohmann::json& object,
                     const char* scope,
                     std::initializer_list<const char*> known_keys,
                     const std::string& provenance,
                     std::vector<std::string>& warnings) {
    if (!object.is_object()) {
        return;
    }
    for (const auto& item : object.items()) {
        bool known = false;
        for (const char* key : known_keys) {
            if (item.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            std::string warning =
                "world preset '" + provenance + "' has unknown key " + scope + "." + item.key();
            LUMINUMBRA_CORE_WARN("{}", warning);
            warnings.push_back(std::move(warning));
        }
    }
}

bool JsonHasObject(const nlohmann::json& data, const char* key) {
    return data.contains(key) && data[key].is_object();
}

std::vector<std::array<float, 2>> ParseSplinePoints(const nlohmann::json& block, const char* key) {
    std::vector<std::array<float, 2>> points;
    if (!block.contains(key) || !block[key].is_array()) {
        return points;
    }
    for (const auto& entry : block[key]) {
        if (entry.is_array() && entry.size() == 2 && entry[0].is_number() && entry[1].is_number()) {
            points.push_back({entry[0].get<float>(), entry[1].get<float>()});
        }
    }
    return points;
}

void ParseShapingBlock(const nlohmann::json& terrain,
                       TerrainShapingPreset& shaping,
                       const std::string& provenance,
                       std::vector<std::string>& warnings) {
    if (!JsonHasObject(terrain, "shaping")) {
        return;
    }
    const nlohmann::json& block = terrain["shaping"];
    shaping.present = true;
    shaping.continentalness_frequency =
        block.value("continentalness_frequency", shaping.continentalness_frequency);
    shaping.erosion_frequency = block.value("erosion_frequency", shaping.erosion_frequency);
    shaping.peaks_frequency = block.value("peaks_frequency", shaping.peaks_frequency);
    shaping.peaks_amplitude = block.value("peaks_amplitude", shaping.peaks_amplitude);
    shaping.domain_warp_amplitude =
        block.value("domain_warp_amplitude", shaping.domain_warp_amplitude);
    shaping.domain_warp_frequency =
        block.value("domain_warp_frequency", shaping.domain_warp_frequency);
    shaping.continental_spline = ParseSplinePoints(block, "continental_spline");
    shaping.erosion_spline = ParseSplinePoints(block, "erosion_spline");
    shaping.peaks_spline = ParseSplinePoints(block, "peaks_spline");
    WarnUnknownKeys(block,
                    "generation_params.terrain.shaping",
                    {"continentalness_frequency",
                     "erosion_frequency",
                     "peaks_frequency",
                     "peaks_amplitude",
                     "domain_warp_amplitude",
                     "domain_warp_frequency",
                     "continental_spline",
                     "erosion_spline",
                     "peaks_spline"},
                    provenance,
                    warnings);
}

void ParseHydroBlock(const nlohmann::json& terrain,
                     TerrainHydroPreset& hydro,
                     const std::string& provenance,
                     std::vector<std::string>& warnings) {
    if (!JsonHasObject(terrain, "hydro")) {
        return;
    }
    const nlohmann::json& block = terrain["hydro"];
    hydro.present = true;
    hydro.enabled = block.value("enabled", hydro.enabled);
    hydro.iterations = block.value("iterations", hydro.iterations);
    hydro.cell_size_m = block.value("cell_size_m", hydro.cell_size_m);
    hydro.talus_height = block.value("talus_height", hydro.talus_height);
    hydro.thermal_rate = block.value("thermal_rate", hydro.thermal_rate);
    hydro.rain_per_sweep = block.value("rain_per_sweep", hydro.rain_per_sweep);
    hydro.solubility = block.value("solubility", hydro.solubility);
    hydro.deposition = block.value("deposition", hydro.deposition);
    hydro.evaporation = block.value("evaporation", hydro.evaporation);
    hydro.sediment_capacity = block.value("sediment_capacity", hydro.sediment_capacity);
    hydro.max_offset = block.value("max_offset", hydro.max_offset);
    WarnUnknownKeys(block,
                    "generation_params.terrain.hydro",
                    {"enabled",
                     "iterations",
                     "cell_size_m",
                     "talus_height",
                     "thermal_rate",
                     "rain_per_sweep",
                     "solubility",
                     "deposition",
                     "evaporation",
                     "sediment_capacity",
                     "max_offset"},
                    provenance,
                    warnings);
}

void ParseBiomesBlock(const nlohmann::json& gen_params,
                      TerrainBiomesPreset& biomes,
                      const std::filesystem::path& data_root,
                      const std::string& provenance,
                      std::vector<std::string>& warnings) {
    if (!JsonHasObject(gen_params, "biomes")) {
        return;
    }
    const nlohmann::json& block = gen_params["biomes"];
    biomes.present = true;
    biomes.temperature_frequency =
        block.value("temperature_frequency", biomes.temperature_frequency);
    biomes.humidity_frequency = block.value("humidity_frequency", biomes.humidity_frequency);
    biomes.relief_enabled = block.value("relief_enabled", biomes.relief_enabled);
    biomes.relief_strength = block.value("relief_strength", biomes.relief_strength);
    biomes.table = block.value("table", biomes.table);
    biomes.enabled = !biomes.table.empty();
    if (biomes.enabled) {
        // The table path is relative to the data/ root, resolved against the
        // explicit data_root the caller supplied (the on-disk loader derives it
        // four parents up from the preset; the in-memory seam is handed it
        // directly so the table resolves correctly without a temp file).
        biomes.resolved_table_path = Filesystem::GenericUtf8PathString(
            Filesystem::LexicallyNormalPath(data_root / Filesystem::PathFromUtf8(biomes.table)));
    }
    WarnUnknownKeys(block,
                    "generation_params.biomes",
                    {"temperature_frequency",
                     "humidity_frequency",
                     "table",
                     "relief_enabled",
                     "relief_strength"},
                    provenance,
                    warnings);
}

} // namespace

nlohmann::json ResolveTerrainPresetDefaults(const nlohmann::json& preset) {
    nlohmann::json out = preset;
    if (!out.is_object())
        return out;
    const bool on = out.value("schema_rev", kMinTerrainPresetSchemaRevision) >= 7;
    if (!JsonHasObject(out, "generation_params"))
        return out;
    auto& gen = out["generation_params"];
    auto fill = [](nlohmann::json& block, const char* key, const nlohmann::json& value) {
        if (!block.contains(key))
            block[key] = value;
    };
    if (JsonHasObject(gen, "terrain")) {
        auto& terrain = gen["terrain"];
        fill(terrain, "island_mask_enabled", on);
        fill(terrain, "hydro", nlohmann::json::object());
        if (terrain["hydro"].is_object())
            fill(terrain["hydro"], "enabled", on);
    }
    fill(gen, "biomes", nlohmann::json::object());
    if (gen["biomes"].is_object()) {
        fill(gen["biomes"], "table", on ? "common/biomes.json" : "");
        fill(gen["biomes"], "relief_enabled", on);
    }
    if (JsonHasObject(gen, "features")) {
        for (const char* key : {"rivers_enabled",
                                "lakes_enabled",
                                "cliffs_enabled",
                                "structures_enabled",
                                "surface_breaks_enabled"})
            fill(gen["features"], key, on);
    }
    return out;
}

std::string CanonicalResolvedTerrainPresetJson(const nlohmann::json& preset) {
    auto resolved = ResolveTerrainPresetDefaults(preset);
    resolved["schema_rev"] = kTerrainPresetSchemaRevision;
    return resolved.dump(-1, ' ', true);
}

std::string TerrainPresetContentHash(const nlohmann::json& preset) {
    return HashPresetIdentity(preset, nlohmann::json::array());
}

TerrainPresetLoadResult LoadTerrainPreset(const std::filesystem::path& preset_path) {
    std::error_code ec;
    const auto preset_dir = Filesystem::AbsolutePath(preset_path, ec).parent_path();
    return LoadTerrainPreset(preset_path,
                             preset_dir.parent_path().parent_path().parent_path() / "data");
}

TerrainPresetLoadResult LoadTerrainPreset(const std::filesystem::path& preset_path,
                                          const std::filesystem::path& data_root) {
    TerrainPresetLoadResult result;

    std::ifstream file(preset_path);
    if (!file.is_open()) {
        result.errors.push_back("failed to open world preset: " +
                                Filesystem::GenericUtf8PathString(preset_path));
        return result;
    }

    nlohmann::json data;
    try {
        data = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        result.errors.push_back("failed to parse world preset JSON '" +
                                Filesystem::GenericUtf8PathString(preset_path) + "': " + e.what());
        return result;
    }

    return LoadTerrainPresetFromJson(
        data, data_root, Filesystem::GenericUtf8PathString(preset_path));
}

TerrainPresetLoadResult
LoadTerrainPresetFromJson(const nlohmann::json& source,
                          const std::filesystem::path& data_root,
                          const std::string& provenance,
                          const World::WorldgenFileStatusReader& read_status,
                          const World::WorldgenFileReader& dependency_reader) try {
    TerrainPresetLoadResult result;
    // Validate revision type before resolving defaults, retaining the no-throw error path.
    if (source.contains("schema_rev") && !source["schema_rev"].is_number_integer()) {
        result.errors.push_back("world preset schema_rev must be an integer: " + provenance);
        return result;
    }
    const nlohmann::json data = ResolveTerrainPresetDefaults(source);

    // schema_rev is validated when declared, not required. Every preset under
    // worlds/atlas/presets/ carries it, but in-memory fixtures construct minimal
    // param blocks for unrelated assertions and have never had to declare it.
    // Requiring it would be a new contract, which is a bigger change than this
    // guard is for.
    if (data.contains("schema_rev") && !data["schema_rev"].is_number_integer()) {
        result.errors.push_back("world preset schema_rev must be an integer: found " +
                                data["schema_rev"].dump() + ": " + provenance);
        return result;
    }

    const std::int64_t schema_revision = data.contains("schema_rev")
                                             ? data["schema_rev"].get<std::int64_t>()
                                             : kMinTerrainPresetSchemaRevision;
    if (schema_revision < kMinTerrainPresetSchemaRevision ||
        schema_revision > kTerrainPresetSchemaRevision) {
        result.errors.push_back("world preset schema revision out of range: supported " +
                                std::to_string(kMinTerrainPresetSchemaRevision) + ".." +
                                std::to_string(kTerrainPresetSchemaRevision) + ", found " +
                                std::to_string(schema_revision) + ": " + provenance);
        return result;
    }

    if (!JsonHasObject(data, "generation_params")) {
        result.errors.push_back("world preset is missing object generation_params: " + provenance);
        return result;
    }

    const nlohmann::json& gen_params = data["generation_params"];
    if (!JsonHasObject(gen_params, "terrain")) {
        result.errors.push_back("world preset is missing object generation_params.terrain: " +
                                provenance);
        return result;
    }
    if (!JsonHasObject(gen_params, "features")) {
        result.errors.push_back("world preset is missing object generation_params.features: " +
                                provenance);
        return result;
    }

    const nlohmann::json& terrain = gen_params["terrain"];
    const nlohmann::json& features = gen_params["features"];
    // Defaults only fill absent fields; malformed explicit choices must return
    // contract errors before the typed reads below.
    auto require_type = [&](bool valid, const char* type, const std::string& field) {
        if (!valid) {
            result.errors.push_back("world preset field must be " + std::string(type) + ": " +
                                    field + ": " + provenance);
        }
    };
    require_type(terrain["island_mask_enabled"].is_boolean(),
                 "boolean",
                 "generation_params.terrain.island_mask_enabled");
    require_type(terrain["hydro"].is_object(), "object", "generation_params.terrain.hydro");
    if (terrain["hydro"].is_object()) {
        require_type(terrain["hydro"]["enabled"].is_boolean(),
                     "boolean",
                     "generation_params.terrain.hydro.enabled");
    }
    require_type(gen_params["biomes"].is_object(), "object", "generation_params.biomes");
    if (gen_params["biomes"].is_object()) {
        require_type(
            gen_params["biomes"]["table"].is_string(), "string", "generation_params.biomes.table");
        require_type(gen_params["biomes"]["relief_enabled"].is_boolean(),
                     "boolean",
                     "generation_params.biomes.relief_enabled");
    }
    for (const char* key : {"rivers_enabled",
                            "lakes_enabled",
                            "cliffs_enabled",
                            "structures_enabled",
                            "surface_breaks_enabled"}) {
        require_type(features[key].is_boolean(),
                     "boolean",
                     std::string("generation_params.features.") + key);
    }
    if (features.contains("cave_style")) {
        result.errors.push_back(
            "retired world preset selector generation_params.features.cave_style: " + provenance);
    }
    if (JsonHasObject(terrain, "shaping") && terrain["shaping"].contains("enabled")) {
        result.errors.push_back(
            "retired world preset selector generation_params.terrain.shaping.enabled: " +
            provenance);
    }
    for (const char* key : {"base_frequency",
                            "base_amplitude",
                            "octaves",
                            "persistence",
                            "lacunarity",
                            "height_offset"}) {
        if (!terrain.contains(key) || !terrain[key].is_number()) {
            result.errors.push_back(std::string("world preset terrain field must be numeric: ") +
                                    key);
        }
    }
    if (!features.contains("caves_enabled") || !features["caves_enabled"].is_boolean()) {
        result.errors.push_back("world preset feature caves_enabled must be boolean");
    }
    if (!features.contains("cave_frequency") || !features["cave_frequency"].is_number()) {
        result.errors.push_back("world preset feature cave_frequency must be numeric");
    }

    if (!result.errors.empty()) {
        return result;
    }

    // Consume the versioned, materialized generation parameters.
    Systems::TerrainGenParams& params = result.params;
    // Missing optional blocks retain historical off semantics in revision 6.
    const bool shipping_defaults = schema_revision >= 7;
    result.extras.hydro.enabled = shipping_defaults;
    params.hydro_enabled = shipping_defaults;
    result.extras.biomes.enabled = false;
    result.extras.biomes.table = shipping_defaults ? "common/biomes.json" : "";
    result.extras.biomes.relief_enabled = shipping_defaults;
    params.base_frequency = terrain.value("base_frequency", params.base_frequency);
    params.base_amplitude = terrain.value("base_amplitude", params.base_amplitude);
    params.octaves = terrain.value("octaves", params.octaves);
    params.persistence = terrain.value("persistence", params.persistence);
    params.lacunarity = terrain.value("lacunarity", params.lacunarity);
    params.height_offset = terrain.value("height_offset", params.height_offset);
    params.island_mask_enabled = terrain.value("island_mask_enabled", false);
    params.island_mask_frequency = terrain.value("island_mask_frequency", 0.004f);
    params.caves_enabled = features.value("caves_enabled", true);
    params.cave_frequency = features.value("cave_frequency", 0.02f);
    params.cave_threshold = features.value("cave_threshold", params.cave_threshold);
    params.cave_carve_value = features.value("cave_carve_value", params.cave_carve_value);
    params.spaghetti_frequency = features.value("spaghetti_frequency", params.spaghetti_frequency);
    params.spaghetti_thickness = features.value("spaghetti_thickness", params.spaghetti_thickness);
    params.worley_frequency = features.value("worley_frequency", params.worley_frequency);
    params.worley_threshold = features.value("worley_threshold", params.worley_threshold);

    // Shaping block: parsed into extras AND consumed - the loader is
    // the one place preset shaping data lands in TerrainGenParams, so every
    // host (GameSession, tests, headless server) gets identical params.
    ParseShapingBlock(terrain, result.extras.shaping, provenance, result.warnings);
    if (result.extras.shaping.present) {
        const TerrainShapingPreset& shaping = result.extras.shaping;
        params.continentalness_frequency = shaping.continentalness_frequency;
        params.erosion_frequency = shaping.erosion_frequency;
        params.peaks_frequency = shaping.peaks_frequency;
        params.peaks_amplitude = shaping.peaks_amplitude;
        params.domain_warp_amplitude = shaping.domain_warp_amplitude;
        params.domain_warp_frequency = shaping.domain_warp_frequency;
        params.continental_spline = shaping.continental_spline;
        params.erosion_spline = shaping.erosion_spline;
        params.peaks_spline = shaping.peaks_spline;
    }

    // Hydro block: versioned hydraulic/thermal relief, with explicit off preserved.
    // Mapped into TerrainGenParams.hydro_*
    // (deliberate world_hash-affecting feature when enabled).
    ParseHydroBlock(terrain, result.extras.hydro, provenance, result.warnings);
    // Resolution materializes historical off choices for saved presets before
    // revision upgrades. Legacy extras still report whether a block was authored.
    if (!shipping_defaults)
        result.extras.hydro.present =
            JsonHasObject(source["generation_params"]["terrain"], "hydro");
    if (result.extras.hydro.present) {
        const TerrainHydroPreset& hydro = result.extras.hydro;
        params.hydro_enabled = hydro.enabled;
        params.hydro_iterations = hydro.iterations;
        params.hydro_cell_size_m = hydro.cell_size_m;
        params.hydro_talus_height = hydro.talus_height;
        params.hydro_thermal_rate = hydro.thermal_rate;
        params.hydro_rain_per_sweep = hydro.rain_per_sweep;
        params.hydro_solubility = hydro.solubility;
        params.hydro_deposition = hydro.deposition;
        params.hydro_evaporation = hydro.evaporation;
        params.hydro_sediment_capacity = hydro.sediment_capacity;
        params.hydro_max_offset = hydro.max_offset;
    }

    // Biomes block: parsed into extras AND consumed when it opts in via a
    // table. With no table the consumed params keep biomes_enabled=false ->
    // byte-zero drift from the pre-biome implementation.
    ParseBiomesBlock(gen_params, result.extras.biomes, data_root, provenance, result.warnings);
    if (!shipping_defaults)
        result.extras.biomes.present = JsonHasObject(source["generation_params"], "biomes");
    params.biomes_enabled = result.extras.biomes.enabled;
    params.biome_relief_enabled =
        result.extras.biomes.enabled && result.extras.biomes.relief_enabled;
    params.biome_table_path.clear();
    if (result.extras.biomes.enabled) {
        const TerrainBiomesPreset& biomes = result.extras.biomes;
        params.biomes_enabled = true;
        params.biome_table_path = biomes.resolved_table_path;
        params.temperature_frequency = biomes.temperature_frequency;
        params.humidity_frequency = biomes.humidity_frequency;
        params.biome_relief_enabled = biomes.relief_enabled;
        params.biome_relief_strength = biomes.relief_strength;
    }
    // Features river/structure flags: rivers and structures
    // use the versioned resolved choices. The structure template content hash is stamped later by
    // the world system when it loads the pools (mirrors the biome-table hash).
    result.extras.features.present = true;
    result.extras.features.rivers_enabled = features.value("rivers_enabled", false);
    result.extras.features.structures_enabled = features.value("structures_enabled", false);
    params.structures_enabled = result.extras.features.structures_enabled;
    params.structures_data_dir.clear();
    if (params.structures_enabled) {
        // Resolve <data_root>/common/structures to an absolute path (the same
        // data root the biome table resolves against, supplied by the caller).
        params.structures_data_dir = Filesystem::GenericUtf8PathString(
            Filesystem::LexicallyNormalPath(data_root / "common" / "structures"));
    }
    params.rivers_enabled = result.extras.features.rivers_enabled;
    if (params.rivers_enabled) {
        params.river_frequency = features.value("river_frequency", params.river_frequency);
        params.river_depth = features.value("river_depth", params.river_depth);
        params.river_pv_min = features.value("river_pv_min", params.river_pv_min);
        params.river_pv_max = features.value("river_pv_max", params.river_pv_max);
        params.river_max_carve = features.value("river_max_carve", params.river_max_carve);
    }
    // Lakes: the resolver supplies the versioned missing-field choice.
    params.lakes_enabled = features.value("lakes_enabled", false);
    if (params.lakes_enabled) {
        params.lake_frequency = features.value("lake_frequency", params.lake_frequency);
        params.lake_threshold = features.value("lake_threshold", params.lake_threshold);
        params.lake_depth = features.value("lake_depth", params.lake_depth);
        params.lake_max_carve = features.value("lake_max_carve", params.lake_max_carve);
        params.lake_bank_offset = features.value("lake_bank_offset", params.lake_bank_offset);
    }
    // surface-breaking caves / sinkholes / cave-mouths. Opt-in via
    // features.surface_breaks_enabled, resolved by revision. When on, the
    // 18 m cave cap becomes a per-column field inside hashed doline footprints.
    params.surface_breaks_enabled = features.value("surface_breaks_enabled", false);
    if (params.surface_breaks_enabled) {
        params.surface_break_density =
            features.value("surface_break_density", params.surface_break_density);
        params.feature_cell_size = features.value("feature_cell_size", params.feature_cell_size);
        params.max_feature_radius = features.value("max_feature_radius", params.max_feature_radius);
        params.carve_smoothness = features.value("carve_smoothness", params.carve_smoothness);
        params.entrance_min_cap = features.value("entrance_min_cap", params.entrance_min_cap);
        // The 3x3 doline-cell scan only suffices if a feature's finite support
        // stays inside the immediate neighborhood. Clamp defensively rather than
        // crash a release world load on a bad preset.
        if (params.max_feature_radius >= params.feature_cell_size) {
            result.warnings.push_back(
                "surface_breaks: max_feature_radius >= feature_cell_size; clamping radius");
            params.max_feature_radius = params.feature_cell_size * 0.49f;
        }
    }
    // Cliffs: the resolver supplies the versioned missing-field choice.
    params.cliffs_enabled = features.value("cliffs_enabled", false);
    if (params.cliffs_enabled) {
        params.cliff_frequency = features.value("cliff_frequency", params.cliff_frequency);
        params.cliff_threshold = features.value("cliff_threshold", params.cliff_threshold);
        params.cliff_step = features.value("cliff_step", params.cliff_step);
    }
    // Unknown-key audit over every consumed scope.
    WarnUnknownKeys(data,
                    "$",
                    {"name", "description", "schema_rev", "realism_exempt", "generation_params"},
                    provenance,
                    result.warnings);
    // "knob_layer" is the semantic-knob layer the create-world UI writes alongside the resolved
    // params (KnobLayer.h/WriteKnobLayer); it is consumed by the KnobLayer loader, not here, so
    // whitelist it to avoid a spurious unknown-key warning on every saved/knob-adjusted preset.
    WarnUnknownKeys(gen_params,
                    "generation_params",
                    {"terrain", "biomes", "features", "knob_layer"},
                    provenance,
                    result.warnings);
    WarnUnknownKeys(terrain,
                    "generation_params.terrain",
                    {"base_frequency",
                     "base_amplitude",
                     "octaves",
                     "persistence",
                     "lacunarity",
                     "height_offset",
                     "island_mask_enabled",
                     "island_mask_frequency",
                     "shaping",
                     "hydro"},
                    provenance,
                    result.warnings);
    WarnUnknownKeys(features,
                    "generation_params.features",
                    {"caves_enabled",          "cave_frequency",        "cave_threshold",
                     "cave_carve_value",       "rivers_enabled",        "structures_enabled",
                     "river_frequency",        "river_depth",           "river_pv_min",
                     "river_pv_max",           "river_max_carve",       "lakes_enabled",
                     "lake_frequency",         "lake_threshold",        "lake_depth",
                     "lake_max_carve",         "lake_bank_offset",      "cliffs_enabled",
                     "cliff_frequency",        "cliff_threshold",       "cliff_step",
                     "surface_breaks_enabled", "surface_break_density", "feature_cell_size",
                     "max_feature_radius",     "carve_smoothness",      "entrance_min_cap",
                     "spaghetti_frequency",    "spaghetti_thickness",   "worley_frequency",
                     "worley_threshold"},
                    provenance,
                    result.warnings);

    nlohmann::json manifest;
    if (!ResolveDependencyManifest(
            result, data_root, manifest, result.errors, read_status, dependency_reader))
        return result;
    result.preset_content_hash = HashPresetIdentity(source, manifest);
    result.ok = true;
    return result;
} catch (const std::exception& error) {
    // Include serialization/hashing in the resolver boundary. Discard any captured
    // snapshot as well as the identity so callers cannot adopt a partial result.
    TerrainPresetLoadResult refused;
    refused.errors.push_back("failed to resolve world preset: " + std::string(error.what()));
    return refused;
} catch (...) {
    TerrainPresetLoadResult refused;
    refused.errors.push_back("failed to resolve world preset: unknown exception");
    return refused;
}

bool LoadTerrainPresetOverride(const std::filesystem::path& preset_path,
                               const std::filesystem::path& data_root,
                               std::string& json_text,
                               std::string& error) {
    std::ifstream file(preset_path);
    if (!file) {
        error = "cannot read preset override: " + Filesystem::GenericUtf8PathString(preset_path);
        return false;
    }
    try {
        const auto document = nlohmann::json::parse(file);
        if (file.bad()) {
            error =
                "cannot read preset override: " + Filesystem::GenericUtf8PathString(preset_path);
            return false;
        }
        const auto validated = LoadTerrainPresetFromJson(
            document, data_root, Filesystem::GenericUtf8PathString(preset_path));
        if (!validated.ok) {
            error = validated.errors.front();
            return false;
        }
        json_text = document.dump();
    } catch (const nlohmann::json::exception& exception) {
        error = "invalid preset override: " + Filesystem::GenericUtf8PathString(preset_path) +
                ": " + exception.what();
        return false;
    }
    error.clear();
    return true;
}

} // namespace Luminumbra::world
