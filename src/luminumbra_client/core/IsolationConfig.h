#pragma once

// Isolation/layer render mode config. Blender-like
// "isolate a subsystem/layer against a neutral backdrop" for review/critique/debug.
//
// Isolation is driven by not spawning non-selected content in the scenario
// world, plus a SkyboxPass flat-colour backdrop override. This defines the
// supported capture-mode contract without affecting normal rendering.
//
// This header is DEPENDENCY-FREE (only <cstdint>/<string>) so the parse + the
// default-is-noop guarantee are unit-testable without a GL context (mirrors
// core/CaptureScale.h). The default IsolationConfig is a provable no-op so the
// existing render path + visual gates stay byte-stable when isolation is off.

#include <cstdint>
#include <string>
#include <string_view>

namespace Luminumbra::Client::ScenarioHarness {

// Backdrop that fills the no-geometry / background pixels (replaces the skybox).
enum class BackdropMode : std::uint8_t {
    Scene = 0,   // default skybox (no-op; byte-stable)
    Void,        // flat dark grey
    Greenscreen, // (0,1,0) chroma key
    Checker,     // magenta/grey checker (scale/alignment reference)
    Transparent, // alpha 0 where no geometry (v2: needs RGBA capture chain)
};

// Content/layer bits. v1 isolates SPAWNED content (terrain/foliage/particles/water/
// skinned) by spawn-suppression; the render-pass bits (skybox/aerial/lightning/
// far-field) are reserved for the v2 render-side masking. All = no isolation.
namespace IsolationLayer {
constexpr std::uint32_t Terrain = 1u << 0;
constexpr std::uint32_t FarField = 1u << 1;
constexpr std::uint32_t Water = 1u << 2;
constexpr std::uint32_t Foliage = 1u << 3;
constexpr std::uint32_t Particles = 1u << 4;
constexpr std::uint32_t Skinned = 1u << 5;
constexpr std::uint32_t Skybox = 1u << 6;
constexpr std::uint32_t Aerial = 1u << 7;
constexpr std::uint32_t Lightning = 1u << 8;
constexpr std::uint32_t All = 0xFFFFFFFFu;
} // namespace IsolationLayer

struct IsolationConfig {
    std::uint32_t layer_mask = IsolationLayer::All;
    BackdropMode backdrop = BackdropMode::Scene;

    // True only when isolation is actually requested. The default {All, Scene} is a
    // no-op so the normal render path is byte-identical (the byte-stability contract).
    bool active() const {
        return layer_mask != IsolationLayer::All || backdrop != BackdropMode::Scene;
    }
    bool renders(std::uint32_t layer_bit) const {
        return (layer_mask & layer_bit) != 0u;
    }
};

namespace detail {
inline char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
inline std::string lower_trim(std::string_view tok) {
    std::size_t b = 0, e = tok.size();
    while (b < e && (tok[b] == ' ' || tok[b] == '\t'))
        ++b;
    while (e > b && (tok[e - 1] == ' ' || tok[e - 1] == '\t'))
        --e;
    std::string out;
    out.reserve(e - b);
    for (std::size_t i = b; i < e; ++i)
        out.push_back(to_lower_ascii(tok[i]));
    return out;
}
inline std::uint32_t layer_bit_for(const std::string& name) {
    if (name == "terrain")
        return IsolationLayer::Terrain;
    if (name == "farfield" || name == "far_field" || name == "far")
        return IsolationLayer::FarField;
    if (name == "water")
        return IsolationLayer::Water;
    if (name == "foliage" || name == "grass")
        return IsolationLayer::Foliage;
    if (name == "particles" || name == "particle")
        return IsolationLayer::Particles;
    if (name == "skinned" || name == "creatures" || name == "creature")
        return IsolationLayer::Skinned;
    if (name == "skybox" || name == "sky")
        return IsolationLayer::Skybox;
    if (name == "aerial" || name == "fog")
        return IsolationLayer::Aerial;
    if (name == "lightning")
        return IsolationLayer::Lightning;
    return 0u; // unknown token -> contributes nothing
}
} // namespace detail

// Parse a CSV of layer names into a layer mask. Empty/whitespace -> All (no
// isolation). Unknown tokens are ignored. If a CSV contains only unknown tokens the
// result is All (treated as "no valid selection" -> no isolation) rather than 0
// (which would render nothing).
inline std::uint32_t ParseIsolationLayers(std::string_view csv) {
    std::uint32_t mask = 0u;
    bool any_valid = false;
    // Split on ',' (explicit, allocation-light).
    std::size_t start = 0;
    for (std::size_t pos = 0; pos <= csv.size(); ++pos) {
        if (pos == csv.size() || csv[pos] == ',') {
            const std::string tok = detail::lower_trim(csv.substr(start, pos - start));
            if (!tok.empty()) {
                const std::uint32_t bit = detail::layer_bit_for(tok);
                if (bit != 0u) {
                    mask |= bit;
                    any_valid = true;
                }
            }
            start = pos + 1;
        }
    }
    return any_valid ? mask : IsolationLayer::All;
}

// Parse a backdrop name. Empty/"scene"/unknown -> Scene (no-op default).
inline BackdropMode ParseBackdropMode(std::string_view s) {
    const std::string v = detail::lower_trim(s);
    if (v == "void" || v == "black")
        return BackdropMode::Void;
    if (v == "greenscreen" || v == "green" || v == "chroma")
        return BackdropMode::Greenscreen;
    if (v == "checker" || v == "checkerboard")
        return BackdropMode::Checker;
    if (v == "transparent" || v == "alpha")
        return BackdropMode::Transparent;
    return BackdropMode::Scene;
}

inline const char* BackdropModeName(BackdropMode m) {
    switch (m) {
        case BackdropMode::Scene:
            return "scene";
        case BackdropMode::Void:
            return "void";
        case BackdropMode::Greenscreen:
            return "greenscreen";
        case BackdropMode::Checker:
            return "checker";
        case BackdropMode::Transparent:
            return "transparent";
    }
    return "scene";
}

inline IsolationConfig ParseIsolationConfig(std::string_view layers_csv,
                                            std::string_view backdrop) {
    return IsolationConfig{ParseIsolationLayers(layers_csv), ParseBackdropMode(backdrop)};
}

} // namespace Luminumbra::Client::ScenarioHarness
