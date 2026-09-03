#include "world/WorldgenOverride.h"

#include <charconv>
#include <cmath>

#include "luminumbra_common/world/KnobLayer.h"

namespace Luminumbra::Client {

namespace {

// "terrain.shaping.enabled" -> json_pointer "/generation_params/terrain/shaping/enabled".
nlohmann::json::json_pointer PointerFor(const std::string& dotted_path) {
    std::string ptr = "/generation_params/";
    for (char ch : dotted_path)
        ptr += (ch == '.') ? '/' : ch;
    return nlohmann::json::json_pointer(ptr);
}

bool ParseFloat(const std::string& s, float& out) {
    const char* begin = s.c_str();
    const char* end = begin + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc();
}

bool ParseInt(const std::string& s, long long& out) {
    // Range inputs can report "6.000000"; accept an integral prefix.
    const char* begin = s.c_str();
    const char* end = begin + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec == std::errc())
        return true;
    // Fall back through a float parse for "6.000000"-style strings.
    float f = 0.0f;
    if (ParseFloat(s, f)) {
        out = static_cast<long long>(f);
        return true;
    }
    return false;
}

} // namespace

CustomPresetResult BuildCustomPreset(const nlohmann::json& base,
                                     const std::vector<WorldGenParam>& params) {
    CustomPresetResult result;
    result.json = base;

    for (const WorldGenParam& p : params) {
        if (p.path.empty()) {
            ++result.skipped;
            continue;
        }

        // The whole biome system has no explicit flag — it is on iff biomes.table is set. Map the
        // "biomes" toggle to that: off clears the table, on restores it (default if the base had
        // none).
        if (p.path == "biomes.enabled") {
            const bool on = (p.value == "true" || p.value == "1");
            const auto tjp = nlohmann::json::json_pointer("/generation_params/biomes/table");
            const std::string base_table = (base.contains(tjp) && base.at(tjp).is_string())
                                               ? base.at(tjp).get<std::string>()
                                               : std::string();
            const std::string desired =
                on ? (base_table.empty() ? std::string("common/biomes.json") : base_table)
                   : std::string();
            if (desired != base_table) {
                result.json[tjp] = desired;
                result.changed = true;
                ++result.applied;
            }
            continue;
        }

        const nlohmann::json::json_pointer jp = PointerFor(p.path);
        const bool base_has = base.contains(jp);

        if (p.type == "bool") {
            const bool v = (p.value == "true" || p.value == "1");
            const bool base_v = base_has && base.at(jp).is_boolean() ? base.at(jp).get<bool>() : !v;
            if (!base_has || base_v != v) {
                result.json[jp] = v;
                result.changed = true;
                ++result.applied;
            }
        } else if (p.type == "int") {
            long long v = 0;
            if (!ParseInt(p.value, v)) {
                ++result.skipped;
                continue;
            }
            const long long base_v =
                base_has && base.at(jp).is_number() ? base.at(jp).get<long long>() : v + 1;
            if (!base_has || base_v != v) {
                result.json[jp] = v;
                result.changed = true;
                ++result.applied;
            }
        } else { // float (default)
            float v = 0.0f;
            if (!ParseFloat(p.value, v)) {
                ++result.skipped;
                continue;
            }
            const double base_v =
                base_has && base.at(jp).is_number() ? base.at(jp).get<double>() : v + 1.0;
            if (!base_has || std::fabs(base_v - static_cast<double>(v)) > 1e-6) {
                result.json[jp] = v;
                result.changed = true;
                ++result.applied;
            }
        }
    }

    return result;
}

KnobPresetResult BuildKnobResolvedPreset(const nlohmann::json& base,
                                         const std::vector<WorldGenParam>& params) {
    using namespace Luminumbra::world;
    KnobPresetResult result;

    // 1. Split knob entries from raw advanced-panel override entries.
    KnobVector knobs = NeutralKnobVector();
    std::vector<WorldGenParam> raw;
    raw.reserve(params.size());
    bool knob_moved = false;
    for (const WorldGenParam& p : params) {
        if (p.type == "knob" || p.path.rfind("knob.", 0) == 0) {
            const std::string id = (p.path.rfind("knob.", 0) == 0) ? p.path.substr(5) : p.path;
            Knob k;
            if (!KnobFromId(id, k))
                continue;
            float v = 0.5f;
            {
                const char* b = p.value.c_str();
                std::from_chars(b, b + p.value.size(), v);
            }
            if (v < 0.0f)
                v = 0.0f;
            if (v > 1.0f)
                v = 1.0f;
            knobs[static_cast<std::size_t>(k)] = v;
            ++result.knob_count;
            if (std::fabs(v - 0.5f) > 1e-4f)
                knob_moved = true;
        } else {
            raw.push_back(p);
        }
    }

    // 2. Apply the knob layer over the baseline (curated preset snapshot).
    const nlohmann::json knob_applied = ApplyKnobLayer(base, knobs);

    // 3. The raw overrides are the SPARSE diff vs the KNOB-APPLIED base (so an
    //    advanced edit that merely matches the knob result records nothing). Reuse
    //    BuildCustomPreset's typed diff to find the real deltas.
    const CustomPresetResult diff = BuildCustomPreset(knob_applied, raw);
    std::vector<KnobOverride> overrides;
    // BuildCustomPreset wrote the applied deltas into diff.json; recover them as a
    // typed override list by re-walking `raw` against knob_applied.
    for (const WorldGenParam& p : raw) {
        if (p.path.empty())
            continue;
        std::string ptr = "/generation_params/";
        for (char ch : p.path)
            ptr += (ch == '.') ? '/' : ch;
        const nlohmann::json::json_pointer jp(ptr);
        if (!diff.json.contains(jp))
            continue;
        // Keep only entries whose value actually differs from the knob-applied base.
        if (knob_applied.contains(jp) && diff.json.at(jp) == knob_applied.at(jp))
            continue;
        overrides.push_back(
            KnobOverride{p.path, p.value, p.type.empty() ? std::string("float") : p.type});
    }
    result.override_count = static_cast<int>(overrides.size());

    // 4. Persist BOTH layers: resolved generation_params (full) + the knob_layer
    //    (knobs + baseline snapshot + override diff) for exact reopen.
    result.json = base;
    WriteKnobLayer(result.json, base, knobs, overrides);
    result.changed = knob_moved || !overrides.empty();
    return result;
}

} // namespace Luminumbra::Client
