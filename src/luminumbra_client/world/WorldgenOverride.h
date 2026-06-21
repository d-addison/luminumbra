#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Luminumbra::Client {

// A single overridden world-generation parameter from the create-world "customize" form.
// `path` is the dotted JSON key under generation_params (e.g. "terrain.base_amplitude"),
// `value` the string value, `type` one of "float" | "int" | "bool". Lives here (not in the UI
// header) so the engine/world layer owns the worldgen-param transport, not RmlUi.
struct WorldGenParam {
    std::string path;
    std::string value;
    std::string type;
};

struct CustomPresetResult {
    nlohmann::json json;     // the base preset with applied overrides
    bool changed = false;    // false when no override actually differed from the base
    int applied = 0;         // number of overrides that differed and were applied
    int skipped = 0;         // overrides dropped (unparseable / unknown base key mismatch)
};

// Merge param overrides onto a base preset JSON under /generation_params. Only keys whose typed
// value DIFFERS from the base are applied (so an untouched form does not "customize" anything,
// and a customized world only records real deltas). Numeric parsing is locale-independent
// (std::from_chars); a malformed numeric value is skipped rather than silently zeroed.
CustomPresetResult BuildCustomPreset(const nlohmann::json& base, const std::vector<WorldGenParam>& params);

}  // namespace Luminumbra::Client
