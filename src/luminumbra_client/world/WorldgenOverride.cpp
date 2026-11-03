#include "world/WorldgenOverride.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <ios>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include "luminumbra_common/persistence/SavedJsonAdmission.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/world/KnobLayer.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"

namespace Luminumbra::Client {

namespace {

// "terrain.shaping.enabled" -> json_pointer "/generation_params/terrain/shaping/enabled".
nlohmann::json::json_pointer PointerFor(const std::string& dotted_path) {
    std::string ptr = "/generation_params/";
    for (const char ch : dotted_path)
        ptr += (ch == '.') ? '/' : ch;
    return nlohmann::json::json_pointer(ptr);
}

bool ParseFloat(const std::string& s, float& out) {
    const char* begin = s.c_str();
    const char* end = begin + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc() && ptr == end && std::isfinite(out);
}

bool ParseInt(const std::string& s, long long& out) {
    // Range inputs can report "6.000000". Parse the integer exactly, then
    // accept only a zero fractional suffix, without a float-to-integer cast.
    const char* begin = s.c_str();
    const char* end = begin + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc())
        return false;
    if (ptr == end)
        return true;
    if (*ptr != '.' || ++ptr == end)
        return false;
    // Work is bounded by this one form value's length.
    for (; ptr != end; ++ptr) {
        if (*ptr != '0')
            return false;
    }
    return true;
}

bool IntegerMatches(const nlohmann::json& base, long long value) {
    if (base.is_number_unsigned()) {
        return value >= 0 &&
               base.get<unsigned long long>() == static_cast<unsigned long long>(value);
    }
    if (base.is_number_integer())
        return base.get<long long>() == value;
    if (!base.is_number_float())
        return false;
    const double number = base.get<double>();
    // The upper exclusive bound is exact even where double rounds LLONG_MAX up.
    const double lower = static_cast<double>(std::numeric_limits<long long>::lowest());
    return number >= lower && number < -lower && std::trunc(number) == number &&
           static_cast<long long>(number) == value;
}

} // namespace

bool RenameUserPreset(const std::filesystem::path& source,
                      const std::filesystem::path& destination,
                      const std::string& display_name) {
    if (display_name.size() > Persistence::SavedInputLimits::kPresetBytes)
        return false;
    try {
        std::string original;
        {
            const Persistence::SavedInputFile input(source);
            if (!input.read(original, Persistence::SavedInputLimits::kPresetBytes) ||
                !Persistence::SavedInputLimits::JsonDepthAllowed(original))
                return false;
        }
        auto preset = nlohmann::json::parse(original);
        if (!preset.is_object())
            return false;
        preset["name"] = display_name.empty() ? std::string("Custom") : display_name;
        std::string serialized;
        if (!Persistence::BoundedJsonDump(
                preset, Persistence::SavedInputLimits::kPresetBytes, serialized, 2, false))
            return false;
        return Persistence::WorldSaveService::rename_file_durably(
            source, destination, original, serialized);
    } catch (const std::exception&) {
        return false;
    }
}

CustomPresetResult BuildCustomPreset(const nlohmann::json& base,
                                     const std::vector<WorldGenParam>& params) {
    CustomPresetResult result;
    const auto resolved_base = Luminumbra::world::ResolveEffectivePreset(
        base, Luminumbra::world::TerrainPresetUse::NewWorld);
    result.json = resolved_base;

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
            const std::string base_table =
                (resolved_base.contains(tjp) && resolved_base.at(tjp).is_string())
                    ? resolved_base.at(tjp).get<std::string>()
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
        const bool base_has = resolved_base.contains(jp);

        if (p.type == "bool") {
            const bool v = (p.value == "true" || p.value == "1");
            const bool base_v = base_has && resolved_base.at(jp).is_boolean()
                                    ? resolved_base.at(jp).get<bool>()
                                    : !v;
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
            if (!base_has || !IntegerMatches(resolved_base.at(jp), v)) {
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
            const bool matches =
                base_has && resolved_base.at(jp).is_number() &&
                std::fabs(resolved_base.at(jp).get<double>() - static_cast<double>(v)) <= 1e-6;
            if (!matches) {
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
    const auto resolved_base = ResolveEffectivePreset(base, TerrainPresetUse::NewWorld);

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
            if (!ParseFloat(p.value, v))
                continue;
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
    const nlohmann::json knob_applied = ApplyKnobLayer(resolved_base, knobs);

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
        for (const char ch : p.path)
            ptr += (ch == '.') ? '/' : ch;
        const nlohmann::json::json_pointer jp(
            p.path == "biomes.enabled" ? "/generation_params/biomes/table" : ptr);
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
    result.json = resolved_base;
    WriteKnobLayer(result.json, resolved_base, knobs, overrides);
    result.changed = knob_moved || !overrides.empty();
    return result;
}

} // namespace Luminumbra::Client
