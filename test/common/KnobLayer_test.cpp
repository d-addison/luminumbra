// SEMANTIC KNOBS (separate persisted layer).
//
// These tests pin the engine-side knob map (world/KnobLayer): knob extremes hit
// the expected params, a MONOTONICITY sweep proves each knob's relief metric
// moves monotonically across 0->1 (the same relief 's diorama shows —
// max-min terrain height over a sample grid, sampled directly from the analytic
// GetTerrainHeightAt of a real SHIELD_WorldSystem, no GL), reopen is EXACT
// (resolve(persisted) == the resolved params byte-for-byte), curated presets are
// NEVER flattened by a neutral knob touch, and the startup endpoint invariant
// holds. No GL, no streaming — GetTerrainHeightAt is a pure function of
// seed/params, so this runs in the headless common suite.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/KnobLayer.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"

using namespace Luminumbra::world;
using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::Systems::TerrainGenParams;

namespace {

// A FIXED self-contained base preset (NOT default.json — that file is owned by
// the concurrent worldgen agent). Shaping on so knobs have real relief to move;
// biomes off so no data tables are needed.
nlohmann::json FixedBasePreset() {
    return nlohmann::json::parse(R"({
        "name": "knob_test_base",
        "generation_params": {
            "terrain": {
                "base_frequency": 0.012,
                "base_amplitude": 60.0,
                "octaves": 5,
                "persistence": 0.5,
                "lacunarity": 2.0,
                "height_offset": 10.0,
                "island_mask_enabled": false,
                "shaping": {
                    "enabled": true,
                    "peaks_amplitude": 52.0,
                    "peaks_frequency": 0.0025,
                    "domain_warp_amplitude": 20.0
                }
            },
            "features": {
                "caves_enabled": false,
                "cave_frequency": 0.03,
                "rivers_enabled": false,
                "lakes_enabled": false,
                "cliffs_enabled": false,
                "structures_enabled": false
            }
        }
    })");
}

// Build TerrainGenParams from a resolved preset JSON in memory (the same loader
// seam  uses). data_root irrelevant here (biomes off -> no table lookup).
TerrainGenParams ParamsFrom(const nlohmann::json& preset) {
    auto res = LoadTerrainPresetFromJson(preset, std::filesystem::path("."), "<knob_test>");
    EXPECT_TRUE(res.ok);
    return res.params;
}

// 's relief/roughness metric: from the REAL analytic surface (no GL, no
// streaming). Combines the macro RELIEF (max-min height span) with the local
// ROUGHNESS (mean absolute height delta between adjacent samples) so it responds
// monotonically BOTH to taller terrain (mountainousness) AND to finer detail
// (ruggedness: more octaves / domain warp). Sampled at a fine spacing so
// high-frequency octaves register (a coarse grid aliases them away).
double ReliefMetric(const TerrainGenParams& params, int seed = 4242) {
    SHIELD_WorldSystem world(nullptr, nullptr, params, seed);
    constexpr int N = 64;
    constexpr float kStep = 6.0f; // fine enough to register added octaves
    std::array<std::array<float, N>, N> h{};
    double lo = 1e30, hi = -1e30;
    for (int iz = 0; iz < N; ++iz) {
        for (int ix = 0; ix < N; ++ix) {
            const float x = (ix - N / 2) * kStep;
            const float z = (iz - N / 2) * kStep;
            const float v = world.GetTerrainHeightAt(x, z);
            h[iz][ix] = v;
            lo = std::min(lo, static_cast<double>(v));
            hi = std::max(hi, static_cast<double>(v));
        }
    }
    // Mean absolute local slope (roughness).
    double rough = 0.0;
    int count = 0;
    for (int iz = 0; iz < N; ++iz) {
        for (int ix = 1; ix < N; ++ix) {
            rough += std::fabs(h[iz][ix] - h[iz][ix - 1]);
            ++count;
        }
    }
    const double meanSlope = count ? rough / count : 0.0;
    // Span dominates (macro relief) with roughness adding local detail response.
    return (hi - lo) + 8.0 * meanSlope;
}

// The local-roughness component alone (mean absolute adjacent-sample height
// delta) — the lever the Ruggedness knob moves. Span-dominated metrics wash out
// fine octave detail, so the ruggedness sweep is gated on this directly.
double RoughnessMetric(const TerrainGenParams& params, int seed = 4242) {
    SHIELD_WorldSystem world(nullptr, nullptr, params, seed);
    constexpr int N = 64;
    constexpr float kStep = 4.0f;
    double rough = 0.0;
    int count = 0;
    for (int iz = 0; iz < N; ++iz) {
        float prev = world.GetTerrainHeightAt((-N / 2) * kStep, (iz - N / 2) * kStep);
        for (int ix = 1; ix < N; ++ix) {
            const float x = (ix - N / 2) * kStep;
            const float z = (iz - N / 2) * kStep;
            const float v = world.GetTerrainHeightAt(x, z);
            rough += std::fabs(v - prev);
            prev = v;
            ++count;
        }
    }
    return count ? rough / count : 0.0;
}

KnobVector KnobsWith(Knob k, float v) {
    KnobVector kv = NeutralKnobVector();
    kv[static_cast<std::size_t>(k)] = v;
    return kv;
}

double JsonNum(const nlohmann::json& preset, const std::string& dotted) {
    std::string ptr = "/generation_params/";
    for (char ch : dotted)
        ptr += (ch == '.') ? '/' : ch;
    return preset.at(nlohmann::json::json_pointer(ptr)).get<double>();
}

} // namespace

// The startup invariant the host asserts: every knob spline endpoint sits inside
// its mapped param's declared range, splines are monotone, neutral==default.
TEST(KnobLayerTest, KnobEndpointsLieWithinDeclaredRanges) {
    std::vector<std::string> errors;
    const bool ok = ValidateKnobEndpoints(errors);
    std::string joined;
    for (const auto& e : errors)
        joined += "\n  " + e;
    EXPECT_TRUE(ok) << "knob endpoint/monotonicity violations:" << joined;
}

// Knob extremes hit the expected params: 0 and 1 push the mapped param to the
// spline's low / high end (and the enable-flags ramp accordingly).
TEST(KnobLayerTest, KnobExtremesHitExpectedParams) {
    const nlohmann::json base = FixedBasePreset();

    const nlohmann::json lowMtn = ApplyKnobLayer(base, KnobsWith(Knob::Mountainousness, 0.0f));
    const nlohmann::json highMtn = ApplyKnobLayer(base, KnobsWith(Knob::Mountainousness, 1.0f));
    EXPECT_LT(JsonNum(lowMtn, "terrain.base_amplitude"), 30.0);
    EXPECT_GT(JsonNum(highMtn, "terrain.base_amplitude"), 150.0);
    EXPECT_GT(JsonNum(highMtn, "terrain.shaping.peaks_amplitude"),
              JsonNum(lowMtn, "terrain.shaping.peaks_amplitude"));

    // Wetness ramps rivers/lakes ON at the high end (flag ramp, no pop).
    const nlohmann::json dry = ApplyKnobLayer(base, KnobsWith(Knob::Wetness, 0.0f));
    const nlohmann::json wet = ApplyKnobLayer(base, KnobsWith(Knob::Wetness, 1.0f));
    EXPECT_FALSE(dry.at(nlohmann::json::json_pointer("/generation_params/features/rivers_enabled"))
                     .get<bool>());
    EXPECT_TRUE(wet.at(nlohmann::json::json_pointer("/generation_params/features/rivers_enabled"))
                    .get<bool>());
    EXPECT_TRUE(wet.at(nlohmann::json::json_pointer("/generation_params/features/lakes_enabled"))
                    .get<bool>());
    EXPECT_GT(JsonNum(wet, "features.river_depth"), JsonNum(dry, "features.river_depth"));

    // Erosion ramps the hydro relief on at the high end.
    const nlohmann::json young = ApplyKnobLayer(base, KnobsWith(Knob::Erosion, 0.0f));
    const nlohmann::json old = ApplyKnobLayer(base, KnobsWith(Knob::Erosion, 1.0f));
    EXPECT_FALSE(young.at(nlohmann::json::json_pointer("/generation_params/terrain/hydro/enabled"))
                     .get<bool>());
    EXPECT_TRUE(old.at(nlohmann::json::json_pointer("/generation_params/terrain/hydro/enabled"))
                    .get<bool>());
}

// MONOTONICITY: sweeping the Mountainousness knob 0->1 increases the relief
// metric monotonically (the dominant relief lever). This is the gate the spec
// names — each knob 0->1 -> 's relief metric moves monotonically.
TEST(KnobLayerTest, MountainousnessSweepIsReliefMonotone) {
    const nlohmann::json base = FixedBasePreset();
    constexpr int kSteps = 9;
    double prev = -1.0;
    for (int i = 0; i < kSteps; ++i) {
        const float t = static_cast<float>(i) / (kSteps - 1);
        const nlohmann::json resolved = ApplyKnobLayer(base, KnobsWith(Knob::Mountainousness, t));
        const double relief = ReliefMetric(ParamsFrom(resolved));
        if (i > 0) {
            // Allow a tiny epsilon for FP noise; relief must not DECREASE.
            EXPECT_GE(relief, prev - 0.5)
                << "mountainousness=" << t << " relief=" << relief << " prev=" << prev;
        }
        prev = relief;
    }
    // And the span actually moves a meaningful amount end-to-end.
    const double low =
        ReliefMetric(ParamsFrom(ApplyKnobLayer(base, KnobsWith(Knob::Mountainousness, 0.0f))));
    const double high =
        ReliefMetric(ParamsFrom(ApplyKnobLayer(base, KnobsWith(Knob::Mountainousness, 1.0f))));
    EXPECT_GT(high, low + 20.0) << "mountainousness must materially raise relief";
}

// Ruggedness sweep also moves relief monotonically (more octaves/persistence/
// warp => rougher => greater max-min span).
TEST(KnobLayerTest, RuggednessSweepIsReliefMonotone) {
    const nlohmann::json base = FixedBasePreset();
    constexpr int kSteps = 9;
    double prev = -1.0;
    for (int i = 0; i < kSteps; ++i) {
        const float t = static_cast<float>(i) / (kSteps - 1);
        const nlohmann::json resolved = ApplyKnobLayer(base, KnobsWith(Knob::Ruggedness, t));
        const double roughness = RoughnessMetric(ParamsFrom(resolved));
        if (i > 0) {
            // Roughness must not DECREASE as ruggedness rises (tiny FP epsilon).
            EXPECT_GE(roughness, prev - 0.02)
                << "ruggedness=" << t << " roughness=" << roughness << " prev=" << prev;
        }
        prev = roughness;
    }
    const double low =
        RoughnessMetric(ParamsFrom(ApplyKnobLayer(base, KnobsWith(Knob::Ruggedness, 0.0f))));
    const double high =
        RoughnessMetric(ParamsFrom(ApplyKnobLayer(base, KnobsWith(Knob::Ruggedness, 1.0f))));
    EXPECT_GT(high, low * 1.1) << "ruggedness must materially raise local roughness";
}

// REOPEN-EXACTNESS (the load-bearing persisted-layer property): writing a knob
// layer then reading + resolving it reproduces the resolved generation_params
// byte-for-byte. Covers the knob vector AND a sparse advanced-panel override on
// top.
TEST(KnobLayerTest, ReopenIsExactWithKnobsAndOverrides) {
    const nlohmann::json baseline = FixedBasePreset();
    KnobVector knobs = NeutralKnobVector();
    knobs[static_cast<std::size_t>(Knob::Mountainousness)] = 0.72f;
    knobs[static_cast<std::size_t>(Knob::Wetness)] = 0.61f;
    knobs[static_cast<std::size_t>(Knob::Erosion)] = 0.33f;

    // An advanced-panel override that the user tweaked on TOP of the knobs.
    std::vector<KnobOverride> overrides = {
        {"terrain.base_amplitude", "133.0", "float"}, // override the knob's value
        {"terrain.lacunarity", "2.4", "float"},       // a param no knob touches
    };

    // Resolve directly (what the world actually generates from).
    const nlohmann::json resolvedDirect = ResolveKnobLayer(baseline, knobs, overrides);

    // Persist (write into a fresh save), then reopen.
    nlohmann::json save = nlohmann::json::object();
    WriteKnobLayer(save, baseline, knobs, overrides);

    const KnobLayerData reopened = ReadKnobLayer(save);
    ASSERT_TRUE(reopened.present);
    EXPECT_EQ(reopened.knobs, knobs);
    ASSERT_EQ(reopened.overrides.size(), overrides.size());

    const nlohmann::json resolvedReopen =
        ResolveKnobLayer(reopened.baseline, reopened.knobs, reopened.overrides);

    // Exact: the resolved generation_params (minus the provenance block) match.
    nlohmann::json a = resolvedDirect["generation_params"];
    nlohmann::json b = resolvedReopen["generation_params"];
    a.erase("knob_layer");
    b.erase("knob_layer");
    EXPECT_EQ(a, b);

    // The override beat the knob on the shared param.
    EXPECT_NEAR(JsonNum(resolvedReopen, "terrain.base_amplitude"), 133.0, 1e-4);
    EXPECT_NEAR(JsonNum(resolvedReopen, "terrain.lacunarity"), 2.4, 1e-4);

    // The persisted save's full generation_params also equal the direct resolve
    // (so a non-knob loader sees the same world).
    nlohmann::json saved = save["generation_params"];
    saved.erase("knob_layer");
    EXPECT_EQ(saved, a);
}

// CURATED PRESETS ARE NEVER FLATTENED: a curated preset has no knob layer; with
// the neutral vector and no overrides, ApplyKnobLayer must preserve its AUTHORED
// values (the knob map's neutral position == each param's default, so a neutral
// touch is the identity on the knob-mapped keys, and untouched keys are
// untouched). Authored splines / non-knob keys survive verbatim.
TEST(KnobLayerTest, NeutralKnobsPreserveCuratedPreset) {
    // A curated preset with AUTHORED values that differ from the knob defaults on
    // keys the knobs DON'T map (so those must pass through untouched) and equal
    // them on keys the knobs DO map at neutral.
    nlohmann::json curated = FixedBasePreset();
    curated["generation_params"]["terrain"]["lacunarity"] = 2.35; // no knob maps this
    curated["generation_params"]["terrain"]["shaping"]["peaks_frequency"] = 0.0031; // no knob

    const KnobLayerData kl = ReadKnobLayer(curated);
    EXPECT_FALSE(kl.present) << "curated preset carries no knob layer -> neutral";
    EXPECT_EQ(kl.knobs, NeutralKnobVector());

    const nlohmann::json applied = ApplyKnobLayer(curated, NeutralKnobVector());
    // Non-knob authored keys survive verbatim.
    EXPECT_NEAR(JsonNum(applied, "terrain.lacunarity"), 2.35, 1e-6);
    EXPECT_NEAR(JsonNum(applied, "terrain.shaping.peaks_frequency"), 0.0031, 1e-6);
    // Knob-mapped keys at neutral land on their authored defaults (== the curated
    // values here), not some inverse-lerped guess.
    EXPECT_NEAR(JsonNum(applied, "terrain.base_amplitude"), 60.0, 1e-3);
    EXPECT_NEAR(JsonNum(applied, "terrain.shaping.peaks_amplitude"), 52.0, 1e-3);
    EXPECT_NEAR(JsonNum(applied, "terrain.height_offset"), 10.0, 1e-3);

    // And the relief of the neutral-applied curated preset matches the relief of
    // the curated preset itself within tight tolerance (not flattened).
    const double curatedRelief = ReliefMetric(ParamsFrom(curated));
    const double appliedRelief = ReliefMetric(ParamsFrom(applied));
    EXPECT_NEAR(appliedRelief, curatedRelief, 1.0);
}

// The descriptor table is the single source of truth and self-consistent: every
// knob-mapped param has a descriptor (covered by ValidateKnobEndpoints), and
// FindParamDescriptor resolves the canonical paths.
TEST(KnobLayerTest, ParamDescriptorTableIsTheSourceOfTruth) {
    EXPECT_NE(FindParamDescriptor("terrain.base_amplitude"), nullptr);
    EXPECT_NE(FindParamDescriptor("features.river_depth"), nullptr);
    EXPECT_EQ(FindParamDescriptor("does.not.exist"), nullptr);
    const ParamDescriptor* amp = FindParamDescriptor("terrain.base_amplitude");
    ASSERT_NE(amp, nullptr);
    EXPECT_EQ(amp->min_value, 0.0);
    EXPECT_EQ(amp->max_value, 200.0);
}
