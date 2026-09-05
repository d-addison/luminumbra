#include "TerrainPresetLoader.h"

#include <fstream>
#include <initializer_list>

#include "nlohmann/json.hpp"

#include "../core/Log.h"

namespace Luminumbra::world {
namespace {

void WarnUnknownKeys(const nlohmann::json& object,
                     const char* scope,
                     std::initializer_list<const char*> known_keys,
                     const std::string& provenance,
                     std::vector<std::string>& warnings) {
    if (!object.is_object()) {
        return;
    }
    for (const auto& item : object.items()) {
        // Older authored revisions may still contain retired stage selectors.
        // Ignore their values; the modern pipeline always runs every stage.
        if (item.key() == "enabled" || item.key() == "relief_enabled" ||
            item.key() == "caves_enabled" || item.key() == "cave_style" ||
            item.key() == "surface_breaks_enabled" || item.key() == "island_mask_enabled" ||
            item.key() == "shaping_enabled" || item.key() == "biomes_enabled" ||
            item.key() == "biome_relief_enabled" || item.key() == "cliffs_enabled" ||
            item.key() == "rivers_enabled" || item.key() == "lakes_enabled" ||
            item.key() == "structures_enabled" || item.key() == "hydro_enabled") {
            continue;
        }
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
                    {"iterations",
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
    biomes.relief_strength = block.value("relief_strength", biomes.relief_strength);
    biomes.table = block.value("table", std::string{});
    biomes.enabled = !biomes.table.empty();
    if (biomes.enabled) {
        // The table path is relative to the data/ root, resolved against the
        // explicit data_root the caller supplied (the on-disk loader derives it
        // four parents up from the preset; the in-memory seam is handed it
        // directly so the table resolves correctly without a temp file).
        biomes.resolved_table_path = (data_root / biomes.table).lexically_normal().string();
    }
    WarnUnknownKeys(block,
                    "generation_params.biomes",
                    {"temperature_frequency", "humidity_frequency", "table", "relief_strength"},
                    provenance,
                    warnings);
}

} // namespace

TerrainPresetLoadResult LoadTerrainPreset(const std::filesystem::path& preset_path) {
    TerrainPresetLoadResult result;

    std::ifstream file(preset_path);
    if (!file.is_open()) {
        result.errors.push_back("failed to open world preset: " + preset_path.string());
        return result;
    }

    nlohmann::json data;
    try {
        data = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        result.errors.push_back("failed to parse world preset JSON '" + preset_path.string() +
                                "': " + e.what());
        return result;
    }

    // The table/structure paths are relative to the data/ root. Presets live at
    // <root>/worlds/atlas/presets/<name>.json, so the data root is four parents
    // up from the preset file. Derive it here and hand it to the in-memory parse
    // so on-disk loads are byte-identical to before.
    std::error_code ec;
    const std::filesystem::path preset_dir =
        std::filesystem::absolute(preset_path, ec).parent_path();
    const std::filesystem::path data_root =
        preset_dir.parent_path().parent_path().parent_path() / "data";
    return LoadTerrainPresetFromJson(data, data_root, preset_path.string());
}

TerrainPresetLoadResult LoadTerrainPresetFromJson(const nlohmann::json& data,
                                                  const std::filesystem::path& data_root,
                                                  const std::string& provenance) {
    TerrainPresetLoadResult result;

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
                                             : kTerrainPresetSchemaRevision;
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
    if (!features.contains("cave_frequency") || !features["cave_frequency"].is_number()) {
        result.errors.push_back("world preset feature cave_frequency must be numeric");
    }

    if (!result.errors.empty()) {
        return result;
    }

    // Consumed generation parameters. Defaults match Systems::TerrainGenParams
    // so an absent optional key never drifts behavior.
    Systems::TerrainGenParams& params = result.params;
    params.base_frequency = terrain.value("base_frequency", params.base_frequency);
    params.base_amplitude = terrain.value("base_amplitude", params.base_amplitude);
    params.octaves = terrain.value("octaves", params.octaves);
    params.persistence = terrain.value("persistence", params.persistence);
    params.lacunarity = terrain.value("lacunarity", params.lacunarity);
    params.height_offset = terrain.value("height_offset", params.height_offset);

    params.island_mask_frequency = terrain.value("island_mask_frequency", 0.004f);

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

    // Hydraulic relief parameters.
    ParseHydroBlock(terrain, result.extras.hydro, provenance, result.warnings);
    if (result.extras.hydro.present) {
        const TerrainHydroPreset& hydro = result.extras.hydro;

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

    // Resolve the optional biome table and consume its parameters.
    ParseBiomesBlock(gen_params, result.extras.biomes, data_root, provenance, result.warnings);
    if (result.extras.biomes.present && result.extras.biomes.enabled) {
        const TerrainBiomesPreset& biomes = result.extras.biomes;

        params.biome_table_path = biomes.resolved_table_path;
        params.temperature_frequency = biomes.temperature_frequency;
        params.humidity_frequency = biomes.humidity_frequency;

        params.biome_relief_strength = biomes.relief_strength;
    }
    // All feature stages run; resolve structure data and consume tuning.
    result.extras.features.present = true;

    {
        // Resolve <data_root>/common/structures to an absolute path (the same
        // data root the biome table resolves against, supplied by the caller).
        params.structures_data_dir =
            (data_root / "common" / "structures").lexically_normal().string();
    }
    {
        params.river_frequency = features.value("river_frequency", params.river_frequency);
        params.river_depth = features.value("river_depth", params.river_depth);
        params.river_pv_min = features.value("river_pv_min", params.river_pv_min);
        params.river_pv_max = features.value("river_pv_max", params.river_pv_max);
        params.river_max_carve = features.value("river_max_carve", params.river_max_carve);
    }
    {
        params.lake_frequency = features.value("lake_frequency", params.lake_frequency);
        params.lake_threshold = features.value("lake_threshold", params.lake_threshold);
        params.lake_depth = features.value("lake_depth", params.lake_depth);
        params.lake_max_carve = features.value("lake_max_carve", params.lake_max_carve);
        params.lake_bank_offset = features.value("lake_bank_offset", params.lake_bank_offset);
    }
    // Surface-breaking caves, sinkholes and cave mouths.
    {
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
    {
        params.cliff_frequency = features.value("cliff_frequency", params.cliff_frequency);
        params.cliff_threshold = features.value("cliff_threshold", params.cliff_threshold);
        params.cliff_step = features.value("cliff_step", params.cliff_step);
    }
    // Unknown-key audit over every consumed scope.
    WarnUnknownKeys(data,
                    "$",
                    {"name", "description", "schema_rev", "generation_params"},
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
                     "island_mask_frequency",
                     "shaping",
                     "hydro"},
                    provenance,
                    result.warnings);
    WarnUnknownKeys(
        features,
        "generation_params.features",
        {"cave_frequency",        "cave_threshold",      "cave_carve_value",    "river_frequency",
         "river_depth",           "river_pv_min",        "river_pv_max",        "river_max_carve",
         "lake_frequency",        "lake_threshold",      "lake_depth",          "lake_max_carve",
         "lake_bank_offset",      "cliff_frequency",     "cliff_threshold",     "cliff_step",
         "surface_break_density", "feature_cell_size",   "max_feature_radius",  "carve_smoothness",
         "entrance_min_cap",      "spaghetti_frequency", "spaghetti_thickness", "worley_frequency",
         "worley_threshold"},
        provenance,
        result.warnings);

    result.ok = true;
    return result;
}

} // namespace Luminumbra::world
