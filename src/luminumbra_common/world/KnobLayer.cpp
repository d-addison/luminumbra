#include "world/KnobLayer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace Luminumbra::world {

namespace {

// ------------------------------------------------------------------------
// Response curves. A KnobMap entry maps ONE knob to ONE param via a monotone
// piecewise-linear spline over the knob domain [0,1] -> param value. The
// neutral knob position (0.5) MUST map to `neutral_value`, which the table sets
// equal to the param's authored default so ApplyKnobLayer at the neutral vector
// reproduces the baseline's intent (curated presets unflattened).
//
// Splines are tuned (more than 2 points where the response should accelerate /
// hold), and are MONOTONE in the param so the relief metric moves monotonically
// under a sweep (the monotonicity gate). Enable-flags ramp on at a knob
// threshold (FlagMap): no discontinuous feature pop because the numeric params
// the flag governs ramp in alongside it.
// ------------------------------------------------------------------------

struct SplinePoint { float knob; float value; };

struct KnobNumericMap {
    Knob knob;
    const char* path;     // dotted generation_params key
    const char* type;     // "float" | "int"
    std::vector<SplinePoint> spline; // knob in [0,1] ascending; value monotone
};

// Enable-flag ramp with a NEUTRAL DEADBAND so a curated preset is never
// flattened by a neutral knob: below `off_below` the flag is forced OFF, above
// `on_above` it is forced ON, and BETWEEN (including the neutral 0.5) it
// INHERITS the baseline's authored flag. Engaging a knob (pushing it past a
// threshold) is what turns a feature on/off; leaving it centered preserves the
// preset.
struct KnobFlagMap {
    Knob knob;
    const char* path;
    float off_below; // knob < off_below -> force OFF
    float on_above;  // knob > on_above  -> force ON
};

// Monotone piecewise-linear evaluation with endpoint clamping. `spline` is
// ascending in .knob.
float EvalSpline(const std::vector<SplinePoint>& s, float t) {
    if (s.empty()) return 0.0f;
    if (t <= s.front().knob) return s.front().value;
    if (t >= s.back().knob) return s.back().value;
    for (std::size_t i = 1; i < s.size(); ++i) {
        if (t <= s[i].knob) {
            const float t0 = s[i - 1].knob, t1 = s[i].knob;
            const float v0 = s[i - 1].value, v1 = s[i].value;
            const float a = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
            return v0 + a * (v1 - v0);
        }
    }
    return s.back().value;
}

// The knob map. Built once. Splines are tuned so 0.5 == the authored default of
// the matching ParamDescriptor (the neutral identity), and the endpoints sit
// inside the descriptor range (ValidateKnobEndpoints enforces it).
const std::vector<KnobNumericMap>& NumericMaps() {
    static const std::vector<KnobNumericMap> maps = {
        // --- Mountainousness: overall elevation + peak height. ---
        {Knob::Mountainousness, "terrain.base_amplitude", "float",
            {{0.0f, 18.0f}, {0.5f, 60.0f}, {0.8f, 130.0f}, {1.0f, 180.0f}}},
        {Knob::Mountainousness, "terrain.shaping.peaks_amplitude", "float",
            {{0.0f, 10.0f}, {0.5f, 52.0f}, {1.0f, 140.0f}}},
        {Knob::Mountainousness, "terrain.height_offset", "float",
            {{0.0f, -10.0f}, {0.5f, 10.0f}, {1.0f, 55.0f}}},

        // --- Ruggedness: detail roughness. ---
        {Knob::Ruggedness, "terrain.octaves", "int",
            {{0.0f, 3.0f}, {0.5f, 5.0f}, {1.0f, 8.0f}}},
        {Knob::Ruggedness, "terrain.persistence", "float",
            {{0.0f, 0.35f}, {0.5f, 0.5f}, {1.0f, 0.7f}}},
        {Knob::Ruggedness, "terrain.shaping.domain_warp_amplitude", "float",
            {{0.0f, 6.0f}, {0.5f, 20.0f}, {1.0f, 55.0f}}},

        // --- Wetness: river + lake depth. ---
        {Knob::Wetness, "features.river_depth", "float",
            {{0.0f, 1.0f}, {0.5f, 4.0f}, {1.0f, 16.0f}}},
        {Knob::Wetness, "features.lake_depth", "float",
            {{0.0f, 1.0f}, {0.5f, 6.0f}, {1.0f, 18.0f}}},

        // --- Erosion / age: hydraulic+thermal smoothing. ---
        {Knob::Erosion, "terrain.hydro.iterations", "int",
            {{0.0f, 0.0f}, {0.5f, 9.0f}, {1.0f, 20.0f}}},
        {Knob::Erosion, "terrain.hydro.thermal_rate", "float",
            {{0.0f, 0.05f}, {0.5f, 0.3f}, {1.0f, 0.9f}}},

        // --- Climate: biome spread + per-biome relief. ---
        {Knob::Climate, "biomes.temperature_frequency", "float",
            {{0.0f, 0.0015f}, {0.5f, 0.003f}, {1.0f, 0.009f}}},
        {Knob::Climate, "biomes.humidity_frequency", "float",
            {{0.0f, 0.0015f}, {0.5f, 0.004f}, {1.0f, 0.009f}}},
        {Knob::Climate, "biomes.relief_strength", "float",
            {{0.0f, 0.1f}, {0.5f, 0.45f}, {1.0f, 0.95f}}},

        // --- Feature density: cliff prominence + cave density. ---
        {Knob::FeatureDensity, "features.cliff_step", "float",
            {{0.0f, 3.0f}, {0.5f, 11.0f}, {1.0f, 28.0f}}},
        {Knob::FeatureDensity, "features.cave_frequency", "float",
            {{0.0f, 0.008f}, {0.5f, 0.03f}, {1.0f, 0.048f}}},
    };
    return maps;
}

const std::vector<KnobFlagMap>& FlagMaps() {
    static const std::vector<KnobFlagMap> maps = {
        // Wetness ramps rivers in first, then lakes. Centered -> inherit preset.
        {Knob::Wetness, "features.rivers_enabled", 0.30f, 0.60f},
        {Knob::Wetness, "features.lakes_enabled", 0.40f, 0.75f},
        // Erosion gates the hydro relief. Centered -> inherit (NEVER force on at
        // neutral, or a curated preset with erosion off would get flattened).
        {Knob::Erosion, "terrain.hydro.enabled", 0.30f, 0.60f},
        // Climate gates per-biome relief shaping (biomes themselves keep the
        // preset's authored table; this only ramps the relief flag).
        {Knob::Climate, "biomes.relief_enabled", 0.35f, 0.65f},
        // Feature density ramps cliffs in past the upper band.
        {Knob::FeatureDensity, "features.cliffs_enabled", 0.35f, 0.65f},
    };
    return maps;
}

nlohmann::json::json_pointer GenPtr(const std::string& dotted) {
    std::string ptr = "/generation_params/";
    for (char ch : dotted) ptr += (ch == '.') ? '/' : ch;
    return nlohmann::json::json_pointer(ptr);
}

bool ParseFloat(const std::string& s, float& out) {
    const char* b = s.c_str();
    auto [p, ec] = std::from_chars(b, b + s.size(), out);
    return ec == std::errc();
}
bool ParseInt(const std::string& s, long long& out) {
    const char* b = s.c_str();
    auto [p, ec] = std::from_chars(b, b + s.size(), out);
    if (ec == std::errc()) return true;
    float f = 0.0f;
    if (ParseFloat(s, f)) { out = static_cast<long long>(f); return true; }
    return false;
}

}  // namespace

KnobVector NeutralKnobVector() {
    KnobVector v{};
    v.fill(0.5f);
    return v;
}

const char* KnobId(Knob k) {
    switch (k) {
        case Knob::Mountainousness: return "mountainousness";
        case Knob::Ruggedness:      return "ruggedness";
        case Knob::Wetness:         return "wetness";
        case Knob::Erosion:         return "erosion";
        case Knob::Climate:         return "climate";
        case Knob::FeatureDensity:  return "feature_density";
        default:                    return "";
    }
}

const char* KnobLabel(Knob k) {
    switch (k) {
        case Knob::Mountainousness: return "mountainousness";
        case Knob::Ruggedness:      return "ruggedness";
        case Knob::Wetness:         return "wetness";
        case Knob::Erosion:         return "erosion / age";
        case Knob::Climate:         return "climate";
        case Knob::FeatureDensity:  return "feature density";
        default:                    return "";
    }
}

bool KnobFromId(const std::string& id, Knob& out) {
    for (std::size_t i = 0; i < kKnobCount; ++i) {
        const Knob k = static_cast<Knob>(i);
        if (id == KnobId(k)) { out = k; return true; }
    }
    return false;
}

const std::vector<ParamDescriptor>& ParamDescriptors() {
    // The single source of truth for the worldgen-param ranges. Must match the
    // .worldgen-param controls in data/ui/world_creation.rml min/max/step/value.
    static const std::vector<ParamDescriptor> d = {
        {"terrain.base_frequency", "float", "base scale", 0.001, 0.05, 0.001, 0.01},
        {"terrain.base_amplitude", "float", "amplitude", 0.0, 200.0, 1.0, 60.0},
        {"terrain.octaves", "int", "octaves", 1.0, 8.0, 1.0, 5.0},
        {"terrain.persistence", "float", "persistence", 0.0, 1.0, 0.05, 0.5},
        {"terrain.lacunarity", "float", "lacunarity", 1.5, 3.0, 0.1, 2.0},
        {"terrain.height_offset", "float", "height offset", -50.0, 100.0, 1.0, 10.0},
        {"terrain.shaping.peaks_amplitude", "float", "peak height", 0.0, 150.0, 1.0, 52.0},
        {"terrain.shaping.peaks_frequency", "float", "peak scale", 0.001, 0.01, 0.0005, 0.0025},
        {"terrain.shaping.domain_warp_amplitude", "float", "domain warp", 0.0, 60.0, 1.0, 20.0},
        {"terrain.hydro.iterations", "int", "erosion passes", 0.0, 20.0, 1.0, 9.0},
        {"terrain.hydro.thermal_rate", "float", "thermal rate", 0.0, 1.0, 0.05, 0.3},
        {"features.river_depth", "float", "river depth", 0.0, 20.0, 0.5, 4.0},
        {"features.river_frequency", "float", "river density", 0.0005, 0.005, 0.0001, 0.0016},
        {"features.lake_depth", "float", "lake depth", 0.0, 20.0, 0.5, 6.0},
        {"biomes.temperature_frequency", "float", "temperature scale", 0.001, 0.01, 0.0005, 0.003},
        {"biomes.humidity_frequency", "float", "humidity scale", 0.001, 0.01, 0.0005, 0.004},
        {"biomes.relief_strength", "float", "relief strength", 0.0, 1.0, 0.05, 0.45},
        {"features.cave_frequency", "float", "cave density", 0.005, 0.05, 0.001, 0.03},
        {"features.cliff_step", "float", "cliff height", 2.0, 30.0, 1.0, 11.0},
        {"features.cliff_frequency", "float", "cliff density", 0.0005, 0.005, 0.0001, 0.0011},
    };
    return d;
}

const ParamDescriptor* FindParamDescriptor(const std::string& path) {
    for (const ParamDescriptor& d : ParamDescriptors())
        if (d.path == path) return &d;
    return nullptr;
}

bool ValidateKnobEndpoints(std::vector<std::string>& errors) {
    const std::size_t before = errors.size();
    for (const KnobNumericMap& m : NumericMaps()) {
        const ParamDescriptor* d = FindParamDescriptor(m.path);
        if (!d) {
            errors.push_back(std::string("knob map references unknown param '") + m.path + "'");
            continue;
        }
        if (m.spline.size() < 2) {
            errors.push_back(std::string("knob map '") + m.path + "' needs >= 2 spline points");
            continue;
        }
        // Domain must be an ascending [0,1] cover; values must be monotone and in
        // range; the midpoint (neutral) must equal the param default.
        bool ascending = true, monotone_up = true, monotone_down = true;
        for (std::size_t i = 1; i < m.spline.size(); ++i) {
            if (m.spline[i].knob < m.spline[i - 1].knob) ascending = false;
            if (m.spline[i].value < m.spline[i - 1].value) monotone_up = false;
            if (m.spline[i].value > m.spline[i - 1].value) monotone_down = false;
        }
        if (!ascending)
            errors.push_back(std::string("knob spline '") + m.path + "' domain not ascending");
        if (!monotone_up && !monotone_down)
            errors.push_back(std::string("knob spline '") + m.path + "' is not monotone");
        if (m.spline.front().knob > 0.0f || m.spline.back().knob < 1.0f)
            errors.push_back(std::string("knob spline '") + m.path + "' must cover [0,1]");
        for (const SplinePoint& p : m.spline) {
            if (p.value < d->min_value - 1e-4 || p.value > d->max_value + 1e-4) {
                errors.push_back(std::string("knob spline '") + m.path + "' value " +
                                 std::to_string(p.value) + " outside [" +
                                 std::to_string(d->min_value) + "," +
                                 std::to_string(d->max_value) + "]");
            }
        }
        const float neutral = EvalSpline(m.spline, 0.5f);
        if (std::fabs(neutral - static_cast<float>(d->default_value)) > 1e-3 * std::max(1.0f, std::fabs(static_cast<float>(d->default_value)))) {
            errors.push_back(std::string("knob spline '") + m.path +
                             "' neutral(0.5)=" + std::to_string(neutral) +
                             " != default " + std::to_string(d->default_value));
        }
    }
    return errors.size() == before;
}

nlohmann::json ApplyKnobLayer(const nlohmann::json& base, const KnobVector& knobs) {
    nlohmann::json out = base;
    if (!out.contains("generation_params") || !out["generation_params"].is_object())
        out["generation_params"] = nlohmann::json::object();

    for (const KnobNumericMap& m : NumericMaps()) {
        const float t = std::clamp(knobs[static_cast<std::size_t>(m.knob)], 0.0f, 1.0f);
        const float v = EvalSpline(m.spline, t);
        const auto jp = GenPtr(m.path);
        if (std::string(m.type) == "int") {
            out[jp] = static_cast<long long>(std::lround(v));
        } else {
            out[jp] = v;
        }
    }
    for (const KnobFlagMap& f : FlagMaps()) {
        const float t = std::clamp(knobs[static_cast<std::size_t>(f.knob)], 0.0f, 1.0f);
        const auto jp = GenPtr(f.path);
        if (t < f.off_below) {
            out[jp] = false;
        } else if (t > f.on_above) {
            out[jp] = true;
        } else {
            // Deadband: inherit the baseline's authored flag (default false if the
            // baseline didn't set it) so a neutral knob never flips a feature.
            const bool inherited = out.contains(jp) && out.at(jp).is_boolean()
                                       ? out.at(jp).get<bool>()
                                       : false;
            out[jp] = inherited;
        }
    }
    return out;
}

// Overlay a sparse raw-override diff (typed dotted writes), matching the client
// BuildCustomPreset semantics so an advanced-panel edit lands identically.
static void OverlayOverrides(nlohmann::json& preset, const std::vector<KnobOverride>& overrides) {
    if (!preset.contains("generation_params") || !preset["generation_params"].is_object())
        preset["generation_params"] = nlohmann::json::object();
    for (const KnobOverride& o : overrides) {
        if (o.path.empty()) continue;
        const auto jp = GenPtr(o.path);
        if (o.type == "bool") {
            preset[jp] = (o.value == "true" || o.value == "1");
        } else if (o.type == "int") {
            long long v = 0;
            if (ParseInt(o.value, v)) preset[jp] = v;
        } else {
            float v = 0.0f;
            if (ParseFloat(o.value, v)) preset[jp] = v;
        }
    }
}

nlohmann::json ResolveKnobLayer(const nlohmann::json& baseline,
                                const KnobVector& knobs,
                                const std::vector<KnobOverride>& overrides) {
    nlohmann::json resolved = ApplyKnobLayer(baseline, knobs);
    OverlayOverrides(resolved, overrides);
    return resolved;
}

KnobLayerData ReadKnobLayer(const nlohmann::json& preset) {
    KnobLayerData data;
    data.knobs = NeutralKnobVector();
    const auto klp = nlohmann::json::json_pointer("/generation_params/knob_layer");
    if (!preset.contains(klp) || !preset.at(klp).is_object()) return data;
    const nlohmann::json& kl = preset.at(klp);
    data.present = true;

    if (kl.contains("knobs") && kl["knobs"].is_object()) {
        for (std::size_t i = 0; i < kKnobCount; ++i) {
            const char* id = KnobId(static_cast<Knob>(i));
            if (kl["knobs"].contains(id) && kl["knobs"][id].is_number())
                data.knobs[i] = std::clamp(kl["knobs"][id].get<float>(), 0.0f, 1.0f);
        }
    }
    if (kl.contains("baseline") && kl["baseline"].is_object()) {
        data.baseline = kl["baseline"];
    } else {
        data.baseline = nlohmann::json::object();
    }
    if (kl.contains("overrides") && kl["overrides"].is_array()) {
        for (const auto& o : kl["overrides"]) {
            if (!o.is_object() || !o.contains("path")) continue;
            KnobOverride ov;
            ov.path = o.value("path", std::string());
            ov.value = o.value("value", std::string());
            ov.type = o.value("type", std::string("float"));
            data.overrides.push_back(std::move(ov));
        }
    }
    return data;
}

nlohmann::json& WriteKnobLayer(nlohmann::json& preset,
                               const nlohmann::json& baseline,
                               const KnobVector& knobs,
                               const std::vector<KnobOverride>& overrides) {
    // Fully resolve the params first (so non-knob loaders see the real world).
    nlohmann::json resolved = ResolveKnobLayer(baseline, knobs, overrides);
    if (resolved.contains("generation_params") && resolved["generation_params"].is_object())
        preset["generation_params"] = resolved["generation_params"];
    else if (!preset.contains("generation_params"))
        preset["generation_params"] = nlohmann::json::object();

    // Then stamp the knob_layer provenance so reopen is exact.
    nlohmann::json kl = nlohmann::json::object();
    nlohmann::json kvec = nlohmann::json::object();
    for (std::size_t i = 0; i < kKnobCount; ++i)
        kvec[KnobId(static_cast<Knob>(i))] = knobs[i];
    kl["knobs"] = std::move(kvec);
    // The baseline snapshot is the generation_params object of `baseline` (strip
    // any nested knob_layer to avoid recursion).
    nlohmann::json baseline_params = nlohmann::json::object();
    if (baseline.contains("generation_params") && baseline["generation_params"].is_object()) {
        baseline_params = baseline["generation_params"];
        baseline_params.erase("knob_layer");
    }
    kl["baseline"] = nlohmann::json::object();
    kl["baseline"]["generation_params"] = std::move(baseline_params);
    nlohmann::json ovs = nlohmann::json::array();
    for (const KnobOverride& o : overrides) {
        ovs.push_back({{"path", o.path}, {"value", o.value}, {"type", o.type}});
    }
    kl["overrides"] = std::move(ovs);

    preset["generation_params"]["knob_layer"] = std::move(kl);
    return preset;
}

}  // namespace Luminumbra::world
