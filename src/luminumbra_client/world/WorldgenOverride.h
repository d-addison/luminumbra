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
    nlohmann::json json;  // the base preset with applied overrides
    bool changed = false; // false when no override actually differed from the base
    int applied = 0;      // number of overrides that differed and were applied
    int skipped = 0;      // overrides dropped (unparseable / unknown base key mismatch)
};

// Merge param overrides onto a base preset JSON under /generation_params. Only keys whose typed
// value DIFFERS from the base are applied (so an untouched form does not "customize" anything,
// and a customized world only records real deltas). Numeric parsing is locale-independent
// (std::from_chars); a malformed numeric value is skipped rather than silently zeroed.
CustomPresetResult BuildCustomPreset(const nlohmann::json& base,
                                     const std::vector<WorldGenParam>& params);

// SEMANTIC KNOBS (separate persisted layer). The create-world
// form sends one WorldGenParam vector that mixes knob entries (path "knob.<id>",
// type "knob", value in [0,1]) with the raw advanced-panel override entries. This
// resolves a world preset from BOTH layers: the knobs drive the engine-side
// KnobLayer response curves over `base` (the override BASELINE — a curated preset
// or a prior resolved one), then the raw overrides overlay as a sparse diff. The
// returned json carries the FULL resolved generation_params AND a persisted
// generation_params.knob_layer (knob vector + baseline + override diff) so the
// create screen REOPENS the exact knob positions + deltas. `changed` is true when
// any knob left neutral OR any override differed (so an untouched form stays on
// the curated base, knobs neutral, no knob_layer written).
struct KnobPresetResult {
    nlohmann::json json;    // base with knob layer applied + overrides + persisted knob_layer
    bool changed = false;   // a knob moved off neutral or an override differed
    int knob_count = 0;     // knob entries parsed
    int override_count = 0; // raw override entries kept (real deltas vs the knob-applied base)
};
KnobPresetResult BuildKnobResolvedPreset(const nlohmann::json& base,
                                         const std::vector<WorldGenParam>& params);

} // namespace Luminumbra::Client
