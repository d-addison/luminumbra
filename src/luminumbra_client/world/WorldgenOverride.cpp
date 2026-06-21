#include "world/WorldgenOverride.h"

#include <charconv>
#include <cmath>

namespace Luminumbra::Client {

namespace {

// "terrain.shaping.enabled" -> json_pointer "/generation_params/terrain/shaping/enabled".
nlohmann::json::json_pointer PointerFor(const std::string& dotted_path) {
    std::string ptr = "/generation_params/";
    for (char ch : dotted_path) ptr += (ch == '.') ? '/' : ch;
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
    if (ec == std::errc()) return true;
    // Fall back through a float parse for "6.000000"-style strings.
    float f = 0.0f;
    if (ParseFloat(s, f)) { out = static_cast<long long>(f); return true; }
    return false;
}

}  // namespace

CustomPresetResult BuildCustomPreset(const nlohmann::json& base, const std::vector<WorldGenParam>& params) {
    CustomPresetResult result;
    result.json = base;

    for (const WorldGenParam& p : params) {
        if (p.path.empty()) { ++result.skipped; continue; }
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
            if (!ParseInt(p.value, v)) { ++result.skipped; continue; }
            const long long base_v = base_has && base.at(jp).is_number() ? base.at(jp).get<long long>() : v + 1;
            if (!base_has || base_v != v) {
                result.json[jp] = v;
                result.changed = true;
                ++result.applied;
            }
        } else {  // float (default)
            float v = 0.0f;
            if (!ParseFloat(p.value, v)) { ++result.skipped; continue; }
            const double base_v = base_has && base.at(jp).is_number() ? base.at(jp).get<double>() : v + 1.0;
            if (!base_has || std::fabs(base_v - static_cast<double>(v)) > 1e-6) {
                result.json[jp] = v;
                result.changed = true;
                ++result.applied;
            }
        }
    }

    return result;
}

}  // namespace Luminumbra::Client
