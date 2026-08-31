#include "BiomeTable.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <optional>

#include "nlohmann/json.hpp"

#include "../core/Log.h"

namespace Luminumbra::World {
namespace {

constexpr u64 kFnvOffsetBasis = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

void FnvMix(u64& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(bytes[i]);
        hash *= kFnvPrime;
    }
}

template<typename T>
void FnvMixValue(u64& hash, const T& value) {
    FnvMix(hash, &value, sizeof(T));
}

// Climate range edges are quantized onto a fixed 1/1024 grid before hashing so
// the content hash is a stable function of the AUTHORED values (and never of
// float formatting). i32 range is ample for the normalized [-1, 1] domain.
i32 QuantizeClimateEdge(float value) {
    return static_cast<i32>(std::lround(static_cast<double>(value) * 1024.0));
}

// Resolves a material name to its MaterialType id. The mapping mirrors the
// MaterialType enum (Types.h); biomes.json references existing material ids by
// name only (palettes reference material ids; they
// never define new materials).
std::optional<u8> MaterialIdFromName(const std::string& name) {
    if (name == "Air")
        return static_cast<u8>(MaterialType::Air);
    if (name == "Stone")
        return static_cast<u8>(MaterialType::Stone);
    if (name == "Soil")
        return static_cast<u8>(MaterialType::Soil);
    if (name == "Grass")
        return static_cast<u8>(MaterialType::Grass);
    if (name == "Sand")
        return static_cast<u8>(MaterialType::Sand);
    if (name == "Deepslate")
        return static_cast<u8>(MaterialType::Deepslate);
    if (name == "Water")
        return static_cast<u8>(MaterialType::Water);
    // Game-content material names (emissive crystals etc.) are not resolvable
    // here by design: biome palettes name only the generic terrain materials.
    // Unknown names warn and fall back, keeping engine source noun-free.
    return std::nullopt;
}

void WarnUnknownKeys(const nlohmann::json& object,
                     const std::string& scope,
                     std::initializer_list<const char*> known_keys,
                     const std::filesystem::path& table_path,
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
            std::string warning = "biome table '" + table_path.string() + "' has unknown key " +
                                  scope + "." + item.key();
            LUMINUMBRA_CORE_WARN("{}", warning);
            warnings.push_back(std::move(warning));
        }
    }
}

// Parses a [min, max] climate-range array. An absent / malformed range keeps
// the full-span default so an unspecified dimension matches everything.
BiomeClimateRange ParseRange(const nlohmann::json& climate, const char* key) {
    BiomeClimateRange range;
    if (!climate.is_object() || !climate.contains(key)) {
        return range;
    }
    const nlohmann::json& entry = climate[key];
    if (entry.is_array() && entry.size() == 2 && entry[0].is_number() && entry[1].is_number()) {
        range.min = entry[0].get<float>();
        range.max = entry[1].get<float>();
    }
    return range;
}

} // namespace

BiomeTable BiomeTable::Load(const std::filesystem::path& table_path) {
    BiomeTable table;

    std::ifstream file(table_path);
    if (!file.is_open()) {
        table.m_errors.push_back("failed to open biome table: " + table_path.string());
        return table;
    }

    nlohmann::json data;
    try {
        data = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        table.m_errors.push_back("failed to parse biome table JSON '" + table_path.string() +
                                 "': " + e.what());
        return table;
    }

    if (!data.is_object() || !data.contains("biomes") || !data["biomes"].is_array()) {
        table.m_errors.push_back("biome table is missing array 'biomes': " + table_path.string());
        return table;
    }

    WarnUnknownKeys(data, "$", {"schema", "description", "biomes"}, table_path, table.m_warnings);

    std::array<bool, 256> id_seen{};
    for (const auto& entry : data["biomes"]) {
        if (!entry.is_object()) {
            table.m_errors.push_back("biome table entry is not an object");
            continue;
        }
        if (!entry.contains("id") || !entry["id"].is_number_integer()) {
            table.m_errors.push_back("biome table entry is missing an integer id");
            continue;
        }
        const int64_t raw_id = entry["id"].get<int64_t>();
        if (raw_id < 0 || raw_id >= static_cast<int64_t>(kNoBiome)) {
            table.m_errors.push_back("biome id out of range [0, 254]: " + std::to_string(raw_id));
            continue;
        }
        const u8 id = static_cast<u8>(raw_id);
        if (id_seen[id]) {
            table.m_errors.push_back("duplicate biome id: " + std::to_string(raw_id));
            continue;
        }
        id_seen[id] = true;

        BiomeDefinition biome;
        biome.id = id;
        biome.name = entry.value("name", std::string{});

        const nlohmann::json climate =
            entry.contains("climate") ? entry["climate"] : nlohmann::json::object();
        biome.continentalness = ParseRange(climate, "continentalness");
        biome.erosion = ParseRange(climate, "erosion");
        biome.peaks_valleys = ParseRange(climate, "peaks_valleys");
        biome.temperature = ParseRange(climate, "temperature");
        biome.humidity = ParseRange(climate, "humidity");
        WarnUnknownKeys(climate,
                        "biomes[" + std::to_string(raw_id) + "].climate",
                        {"continentalness", "erosion", "peaks_valleys", "temperature", "humidity"},
                        table_path,
                        table.m_warnings);

        if (entry.contains("surface") && entry["surface"].is_object()) {
            const nlohmann::json& surface = entry["surface"];
            const auto assign = [&](const char* key, u8& slot) {
                if (surface.contains(key) && surface[key].is_string()) {
                    const std::string material_name = surface[key].get<std::string>();
                    const std::optional<u8> material_id = MaterialIdFromName(material_name);
                    if (material_id.has_value()) {
                        slot = *material_id;
                    } else {
                        table.m_errors.push_back("biome '" + biome.name + "' surface." + key +
                                                 " references unknown material '" + material_name +
                                                 "'");
                    }
                }
            };
            assign("top", biome.palette.top);
            assign("filler", biome.palette.filler);
            assign("depth", biome.palette.depth);
            assign("underwater", biome.palette.underwater);
            WarnUnknownKeys(surface,
                            "biomes[" + std::to_string(raw_id) + "].surface",
                            {"top", "filler", "depth", "underwater"},
                            table_path,
                            table.m_warnings);
        } else {
            table.m_errors.push_back("biome '" + biome.name +
                                     "' is missing a surface palette object");
        }

        //  vegetation is now CONSUMED render-side (foliage scatter
        // density). Render-only: NOT mixed into compute_content_hash (see the
        // note there) so it never invalidates far-LOD tiles or perturbs
        // world_hash. The scatter labels stay opaque game content.
        if (entry.contains("vegetation") && entry["vegetation"].is_object()) {
            const nlohmann::json& veg = entry["vegetation"];
            biome.vegetation.density =
                std::clamp(veg.value("density", biome.vegetation.density), 0.0f, 1.0f);
            if (veg.contains("scatter") && veg["scatter"].is_array()) {
                for (const auto& s : veg["scatter"]) {
                    if (s.is_string()) {
                        biome.vegetation.scatter.push_back(s.get<std::string>());
                    }
                }
            }
            WarnUnknownKeys(veg,
                            "biomes[" + std::to_string(raw_id) + "].vegetation",
                            {"density", "scatter"},
                            table_path,
                            table.m_warnings);
        }
        // reverb is now CONSUMED (per-biome environmental audio).
        if (entry.contains("reverb") && entry["reverb"].is_object()) {
            const nlohmann::json& reverb = entry["reverb"];
            biome.reverb.preset = reverb.value("preset", biome.reverb.preset);
            biome.reverb.wet = std::clamp(reverb.value("wet", biome.reverb.wet), 0.0f, 1.0f);
            biome.reverb.dry = std::clamp(reverb.value("dry", biome.reverb.dry), 0.0f, 1.0f);
            biome.reverb.decay = std::max(0.0f, reverb.value("decay", biome.reverb.decay));
            WarnUnknownKeys(reverb,
                            "biomes[" + std::to_string(raw_id) + "].reverb",
                            {"preset", "wet", "dry", "decay"},
                            table_path,
                            table.m_warnings);
        }

        WarnUnknownKeys(entry,
                        "biomes[" + std::to_string(raw_id) + "]",
                        {"id", "name", "description", "climate", "surface", "vegetation", "reverb"},
                        table_path,
                        table.m_warnings);

        table.m_biomes.push_back(std::move(biome));
    }

    if (!table.m_errors.empty()) {
        // Structural errors leave the table empty so the caller falls back to
        // legacy single-material behavior rather than mis-classifying terrain.
        table.m_biomes.clear();
        return table;
    }

    table.rebuild_index();
    table.m_content_hash = table.compute_content_hash();
    table.m_ok = true;
    return table;
}

void BiomeTable::rebuild_index() {
    m_id_to_index.fill(kNoBiome);
    for (std::size_t i = 0; i < m_biomes.size(); ++i) {
        m_id_to_index[m_biomes[i].id] = static_cast<u8>(i);
    }
}

u8 BiomeTable::lookup(float continentalness,
                      float erosion,
                      float peaks_valleys,
                      float temperature,
                      float humidity) const {
    for (const BiomeDefinition& biome : m_biomes) {
        if (biome.continentalness.contains(continentalness) && biome.erosion.contains(erosion) &&
            biome.peaks_valleys.contains(peaks_valleys) &&
            biome.temperature.contains(temperature) && biome.humidity.contains(humidity)) {
            return biome.id;
        }
    }
    return kNoBiome;
}

const BiomeSurfacePalette& BiomeTable::palette_for(u8 biome_id) const {
    if (biome_id < m_id_to_index.size()) {
        const u8 index = m_id_to_index[biome_id];
        if (index != kNoBiome && index < m_biomes.size()) {
            return m_biomes[index].palette;
        }
    }
    return m_default_palette;
}

const BiomeReverb& BiomeTable::reverb_for(u8 biome_id) const {
    if (biome_id < m_id_to_index.size()) {
        const u8 index = m_id_to_index[biome_id];
        if (index != kNoBiome && index < m_biomes.size()) {
            return m_biomes[index].reverb;
        }
    }
    return m_default_reverb;
}

const BiomeVegetation& BiomeTable::vegetation_for(u8 biome_id) const {
    if (biome_id < m_id_to_index.size()) {
        const u8 index = m_id_to_index[biome_id];
        if (index != kNoBiome && index < m_biomes.size()) {
            return m_biomes[index].vegetation;
        }
    }
    return m_default_vegetation;
}

const std::string& BiomeTable::name_for(u8 biome_id) const {
    static const std::string kNoneName = "none";
    if (biome_id < m_id_to_index.size()) {
        const u8 index = m_id_to_index[biome_id];
        if (index != kNoBiome && index < m_biomes.size()) {
            return m_biomes[index].name;
        }
    }
    return kNoneName;
}

// NOTE: reverb AND vegetation are deliberately NOT mixed into
// compute_content_hash. The
// content hash gates the terrain far-LOD cache (ComputeTerrainParamsHash), and
// reverb is audio-only - a reverb retune must not invalidate terrain tiles.
u64 BiomeTable::compute_content_hash() const {
    u64 hash = kFnvOffsetBasis;
    const auto mix_range = [&hash](const BiomeClimateRange& range) {
        const i32 lo = QuantizeClimateEdge(range.min);
        const i32 hi = QuantizeClimateEdge(range.max);
        FnvMixValue(hash, lo);
        FnvMixValue(hash, hi);
    };
    for (const BiomeDefinition& biome : m_biomes) {
        FnvMixValue(hash, biome.id);
        mix_range(biome.continentalness);
        mix_range(biome.erosion);
        mix_range(biome.peaks_valleys);
        mix_range(biome.temperature);
        mix_range(biome.humidity);
        FnvMixValue(hash, biome.palette.top);
        FnvMixValue(hash, biome.palette.filler);
        FnvMixValue(hash, biome.palette.depth);
        FnvMixValue(hash, biome.palette.underwater);
    }
    return hash;
}

} // namespace Luminumbra::World
