#pragma once

// SEMANTIC KNOBS (separate persisted layer).
//
// The create-world DEFAULT surface is ~6 OUTCOME knobs (mountainousness,
// ruggedness, wetness, erosion/age, climate, feature density). Each knob is a
// scalar in [0,1] that drives a TUNED RESPONSE CURVE (a per-param monotone
// spline, NOT a 2-point lerp) over the raw generation_params, plus RAMPED
// enable-flags (a feature ramps in across a window, no discontinuous pop). The
// raw ~30-param panel becomes the "advanced" fold.
//
// This file is the ONE CANONICAL param-descriptor + knob-map table — the single
// source of truth for every knob's response (so the RML min/max AND the knob map
// derive from the SAME numbers). It is engine-side (luminumbra_common/world),
// has NO UI/GL dependency, and is a PURE JSON transform over generation_params
// (it never reimplements worldgen sampling — it only writes the same param keys
// the loader already consumes).
//
// PERSISTENCE MODEL (owner decision: separate persisted layer; the lossy one-way
// lerp is rejected). A world save / user preset carries an ADDITIVE
// generation_params.knob_layer:
//   { "knobs":   { "mountainousness": 0.7,... },     // the knob vector
//     "baseline": {...generation_params snapshot... }, // the override baseline
//     "overrides":[ {"path":..., "value":..., "type":...},... ] }  // sparse raw diff
// apply = ApplyKnobLayer(baseline, knobs) THEN overlay the sparse overrides. The
// resolved generation_params are ALSO written in full (so loaders that ignore the
// knob layer still work). Reopen is EXACT: recomputing apply+overlay reproduces
// the resolved params byte-for-byte.
//
// CURATED PRESETS carry NO knob_layer -> the knobs render NEUTRAL ("preset /
// custom"), never inverse-lerped. Engaging a knob SNAPSHOTS the curated
// generation_params as the override baseline, so authored splines survive
// (ApplyKnobLayer at the neutral vector over a baseline == that baseline, by
// construction) — a curated preset is NEVER flattened by a knob touch.

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Luminumbra::world {

// The 6 outcome knobs, in canonical order. KnobCount is the vector length.
enum class Knob : std::size_t {
    Mountainousness = 0, // overall elevation / peak height
    Ruggedness,          // detail roughness (octaves/persistence/domain warp)
    Wetness,             // rivers + lakes presence + depth
    Erosion,             // hydraulic/thermal age (smoothing + erosion passes)
    Climate,             // biome temperature/humidity spread + per-biome relief
    FeatureDensity,      // caves + cliffs + structures prevalence
    Count
};
inline constexpr std::size_t kKnobCount = static_cast<std::size_t>(Knob::Count);

// A knob vector: one scalar in [0,1] per knob.
using KnobVector = std::array<float, kKnobCount>;

// The NEUTRAL knob vector (every knob centered). Applying it over a baseline is
// the identity on that baseline (so curated presets are preserved). 0.5 reads as
// "leave the authored value where it is".
KnobVector NeutralKnobVector();

// Stable lowercase id for a knob (the JSON key + the data-knob attribute).
const char* KnobId(Knob k);
// Human label for the knob (the RML control label).
const char* KnobLabel(Knob k);
// Parse a knob id back to the enum; returns false on an unknown id.
bool KnobFromId(const std::string& id, Knob& out);

// A canonical worldgen-param descriptor — the single source of truth for the
// param's UI range AND the knob map's spline range. `path` is the dotted key
// under generation_params (e.g. "terrain.base_amplitude"); `type` one of
// "float"|"int"|"bool".
struct ParamDescriptor {
    std::string path;
    std::string type;
    std::string label;
    double min_value = 0.0;
    double max_value = 1.0;
    double step = 0.0;
    double default_value = 0.0;
};

// Every descriptor (terrain/water/biomes/features), in a stable order. The RML
// min/max for each.worldgen-param control should match its descriptor here.
const std::vector<ParamDescriptor>& ParamDescriptors();
// Lookup a descriptor by dotted path; nullptr if unknown.
const ParamDescriptor* FindParamDescriptor(const std::string& path);

// Startup invariant: assert every knob spline's endpoint OUTPUTS lie
// within the mapped param's declared [min,max] range, and every spline is a
// monotone, in-order [0,1]-domain control set. Returns false + fills `errors` on
// any violation. Called once at startup (the host asserts it).
bool ValidateKnobEndpoints(std::vector<std::string>& errors);

// Apply the knob layer to a generation_params-bearing preset JSON. `base` is the
// override BASELINE (a curated preset snapshot, or a prior resolved preset);
// `knobs` the knob vector. Returns a COPY of `base` with every knob-mapped param
// under /generation_params overwritten by the knob response (numeric params set
// to the spline output; enable-flags ramped on/off at their threshold). Pure: no
// sampling, no IO. The neutral vector reproduces `base` exactly on the knob-mapped
// keys that have a neutral==identity response (see the table).
nlohmann::json ApplyKnobLayer(const nlohmann::json& base, const KnobVector& knobs);

// --- Persistence helpers (additive generation_params.knob_layer) ---

// A single raw override (mirrors the client WorldGenParam transport but lives
// here so persistence is engine-owned).
struct KnobOverride {
    std::string path;
    std::string value;
    std::string type;
};

struct KnobLayerData {
    bool present = false;                // a knob_layer block existed
    KnobVector knobs{};                  // the knob vector (neutral if absent)
    nlohmann::json baseline;             // the override baseline (a generation_params object)
    std::vector<KnobOverride> overrides; // sparse raw diff over ApplyKnobLayer(baseline,knobs)
};

// Read generation_params.knob_layer from a preset JSON. If absent, returns
// present=false with the neutral vector (so a curated preset reads as neutral).
KnobLayerData ReadKnobLayer(const nlohmann::json& preset);

// Write the knob layer INTO a preset JSON (under generation_params.knob_layer)
// AND fully resolve generation_params = overlay(ApplyKnobLayer(baseline,knobs),
// overrides). Mutates and returns `preset`. The resolved params are written in
// full so a loader that ignores the knob layer still loads the same world; the
// knob_layer block lets the create screen REOPEN the exact knob positions +
// override deltas. `baseline` is the override baseline snapshot.
nlohmann::json& WriteKnobLayer(nlohmann::json& preset,
                               const nlohmann::json& baseline,
                               const KnobVector& knobs,
                               const std::vector<KnobOverride>& overrides);

// Resolve generation_params from a knob layer: overlay(ApplyKnobLayer(baseline,
// knobs), overrides). The override application matches the client BuildCustomPreset
// semantics (typed, dotted-path writes under /generation_params). This is the
// single function the reopen-exactness test pins.
nlohmann::json ResolveKnobLayer(const nlohmann::json& baseline,
                                const KnobVector& knobs,
                                const std::vector<KnobOverride>& overrides);

} // namespace Luminumbra::world
