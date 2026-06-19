#include "core/SystemConfig.h"

#include <fstream>
#include <iomanip>
#include <sstream>

#include "nlohmann/json.hpp"
#include "persistence/WorldPersistenceRoundtrip.h"  // Luminumbra::Persistence::StableChecksum

namespace luminumbra::core {
namespace {

enum class Section : std::uint8_t { Sim, Render };

struct KeyMeta {
    SysKey key;
    Section section;
    const char* json_section;  // "sim" | "render"
    const char* json_name;     // e.g. "plant_growth"
};

struct ParamMeta {
    SysParam id;
    SysKey owner;
    const char* json_name;  // e.g. "mutation_rate"
    bool is_vec3;
    float default_scalar;
    glm::vec3 default_vec3;
};

// Canonical registry. Order is the hash's canonical order; keep sim.* keys grouped.
constexpr KeyMeta kKeys[] = {
    {SysKey::SimPlantGrowth, Section::Sim, "sim", "plant_growth"},
    {SysKey::SimErosion, Section::Sim, "sim", "erosion"},
    {SysKey::RenderMoonlight, Section::Render, "render", "moonlight"},
    {SysKey::RenderTreeWind, Section::Render, "render", "tree_wind"},
};

constexpr ParamMeta kParams[] = {
    {SysParam::PlantMutationRate, SysKey::SimPlantGrowth, "mutation_rate", false, 0.05f, glm::vec3(0.0f)},
    {SysParam::MoonlightStrength, SysKey::RenderMoonlight, "strength", false, 0.0f, glm::vec3(0.0f)},
    {SysParam::MoonlightColor, SysKey::RenderMoonlight, "color", true, 0.0f, glm::vec3(0.6f, 0.7f, 1.0f)},
};

}  // namespace

SystemConfig SystemConfig::Defaults() { return SystemConfig{}; }

SystemConfig SystemConfig::FromJsonString(const std::string& json_text) {
    SystemConfig cfg;  // start from all-defaults; overlay only what the JSON names

    nlohmann::json data;
    try {
        data = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::parse_error&) {
        return cfg;  // malformed/empty/blank -> defaults (graceful)
    }
    if (!data.is_object()) return cfg;

    for (const auto& key_meta : kKeys) {
        if (!data.contains(key_meta.json_section)) continue;
        const nlohmann::json& section = data[key_meta.json_section];
        if (!section.is_object() || !section.contains(key_meta.json_name)) continue;
        const nlohmann::json& entry = section[key_meta.json_name];
        if (!entry.is_object()) continue;

        if (entry.contains("enabled") && entry["enabled"].is_boolean() &&
            entry["enabled"].get<bool>()) {
            cfg.m_enabled |= (1u << static_cast<unsigned>(key_meta.key));
        }

        if (!entry.contains("params") || !entry["params"].is_object()) continue;
        const nlohmann::json& params = entry["params"];
        for (const auto& pm : kParams) {
            if (pm.owner != key_meta.key) continue;
            if (!params.contains(pm.json_name)) continue;
            const nlohmann::json& value = params[pm.json_name];
            const std::size_t pi = static_cast<std::size_t>(pm.id);
            if (pm.is_vec3) {
                if (value.is_array() && value.size() == 3) {
                    cfg.m_params[pi] = glm::vec3(value[0].get<float>(), value[1].get<float>(),
                                                 value[2].get<float>());
                    cfg.m_param_set |= (1u << static_cast<unsigned>(pm.id));
                }
            } else if (value.is_number()) {
                cfg.m_params[pi].x = value.get<float>();
                cfg.m_param_set |= (1u << static_cast<unsigned>(pm.id));
            }
        }
    }
    return cfg;
}

SystemConfig SystemConfig::LoadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) return SystemConfig{};  // missing/unreadable -> defaults
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return FromJsonString(buffer.str());
}

float SystemConfig::param(SysParam id, float fallback) const {
    if ((m_param_set >> static_cast<unsigned>(id)) & 1u) {
        return m_params[static_cast<std::size_t>(id)].x;
    }
    return fallback;
}

glm::vec3 SystemConfig::param3(SysParam id, glm::vec3 fallback) const {
    if ((m_param_set >> static_cast<unsigned>(id)) & 1u) {
        return m_params[static_cast<std::size_t>(id)];
    }
    return fallback;
}

std::string SystemConfig::ComputeConfigSubHash() const {
    std::ostringstream bytes;
    bytes << "config:v1:" << std::setprecision(17);
    bool any = false;

    for (const auto& key_meta : kKeys) {
        if (key_meta.section != Section::Sim) continue;  // render.* never hashed
        if (!enabled(key_meta.key)) continue;            // only enabled sim systems affect state
        any = true;
        bytes << key_meta.json_name << ":en=1;";
        // Emit every owned param's RESOLVED value (set value or compiled default), in
        // SysParam enum order, so identical configs hash identically regardless of JSON order.
        for (const auto& pm : kParams) {
            if (pm.owner != key_meta.key) continue;
            const std::size_t pi = static_cast<std::size_t>(pm.id);
            const bool set = (m_param_set >> static_cast<unsigned>(pm.id)) & 1u;
            if (pm.is_vec3) {
                const glm::vec3 v = set ? m_params[pi] : pm.default_vec3;
                bytes << pm.json_name << '=' << v.x << ',' << v.y << ',' << v.z << ';';
            } else {
                const float v = set ? m_params[pi].x : pm.default_scalar;
                bytes << pm.json_name << '=' << v << ';';
            }
        }
    }

    if (!any) return {};  // all sim defaults -> byte-identical baseline
    return Luminumbra::Persistence::StableChecksum(bytes.str());
}

}  // namespace luminumbra::core
