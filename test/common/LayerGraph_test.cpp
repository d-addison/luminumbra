// CONSTRAINED layer graph.
//
// These tests pin the engine-side fixed-topology graph (world/LayerGraph):
//   - decompose a curated preset's generation_params into the fixed stages and
//     recompile BIT-EXACT field-for-field (the determinism guarantee — same keys,
//     same values, same.dump string -> world_hash / run==replay unchanged);
//   - serialize -> deserialize -> compile round-trips bit-exactly (the graph block
//     is lossless editing metadata);
//   - WriteLayerGraph embeds the resolved params + a graph block, and the resolved
//     params (minus the graph block) still equal the source;
//   - unrecognized future keys survive (passthrough);
//   - the fixed topology / stage decomposition is correct.
//
// Pinned to a FIXED self-contained literal, NOT default.json (that file is owned
// by the concurrent worldgen agent). No GL, no streaming — a pure JSON transform.

#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "luminumbra_common/world/LayerGraph.h"

using namespace Luminumbra::world;

namespace {

// A FIXED, rich preset exercising EVERY stage (base terrain + shaping + hydro +
// biomes + features + materials) plus nested arrays/splines, so the bit-exact
// parity has real surface area. Mirrors the shipped schema (worlds/atlas/presets)
// without depending on default.json.
nlohmann::json FixedRichPreset() {
    return nlohmann::json::parse(R"({
        "name": "graph_test_base",
        "description": "fixed literal for the layer-graph parity test",
        "schema_rev": 5,
        "generation_params": {
            "terrain": {
                "base_frequency": 0.011,
                "base_amplitude": 42.0,
                "octaves": 6,
                "persistence": 0.52,
                "lacunarity": 2.0,
                "height_offset": 7.0,
                "island_mask_enabled": false,
                "shaping": {
                    "continentalness_frequency": 0.0012,
                    "erosion_frequency": 0.0018,
                    "peaks_frequency": 0.004,
                    "peaks_amplitude": 22.0,
                    "domain_warp_amplitude": 14.0,
                    "domain_warp_frequency": 0.004,
                    "continental_spline": [[-1.0, -9.0], [-0.3, -2.0], [0.1, 3.0], [0.5, 9.0], [1.0, 18.0]],
                    "erosion_spline": [[-1.0, 0.7], [0.0, 0.3], [1.0, 0.12]],
                    "peaks_spline": [[-1.0, 0.0], [0.3, 0.05], [1.0, 1.0]]
                },
                "hydro": {
                    "enabled": true,
                    "iterations": 4,
                    "cell_size_m": 8.0,
                    "talus_height": 2.6,
                    "thermal_rate": 0.3,
                    "max_offset": 8.0
                }
            },
            "biomes": {
                "table": "common/biomes.json",
                "temperature_frequency": 0.005,
                "humidity_frequency": 0.005,
                "relief_enabled": true,
                "relief_strength": 0.45
            },
            "features": {
                "caves_enabled": true,
                "cave_frequency": 0.02,
                "rivers_enabled": true,
                "river_frequency": 0.0016,
                "river_depth": 4.0,
                "lakes_enabled": true,
                "lake_frequency": 0.0009,
                "cliffs_enabled": true,
                "cliff_frequency": 0.0011,
                "structures_enabled": true
            },
            "materials": {
                "strata": [
                    {"material": "soil", "max_depth": 4, "thickness": 2},
                    {"material": "stone", "max_depth": 64, "thickness": 60}
                ],
                "veins": [
                    {"material": "ore", "host_materials": ["stone"], "noise_frequency": 0.05, "noise_threshold": 0.7}
                ]
            }
        }
    })");
}

// A MINIMAL preset (only the required terrain+features) — proves absent blocks
// (shaping/hydro/biomes/materials) stay absent through the round-trip.
nlohmann::json FixedMinimalPreset() {
    return nlohmann::json::parse(R"({
        "name": "graph_test_minimal",
        "generation_params": {
            "terrain": {
                "base_frequency": 0.01,
                "base_amplitude": 30.0,
                "octaves": 4,
                "persistence": 0.5,
                "lacunarity": 2.0,
                "height_offset": 0.0
            },
            "features": {
                "caves_enabled": false,
                "cave_frequency": 0.02
            }
        }
    })");
}

} // namespace

// --- The headline determinism test: compile is BIT-EXACT to the source params,
//     field-for-field (== AND identical.dump). ----------------------------
TEST(LayerGraphTest, CompileIsBitExactToCuratedPresetParams) {
    const nlohmann::json preset = FixedRichPreset();
    const nlohmann::json& source_gp = preset.at("generation_params");

    const LayerGraph graph = LayerGraphFromPreset(preset);
    const nlohmann::json compiled = CompileLayerGraph(graph);

    // Field-for-field (nlohmann::json == is a deep, key/value compare).
    EXPECT_EQ(compiled, source_gp);
    // And the canonical serialization is byte-identical (sorted-key dump is
    // deterministic), so world_hash over the params text is unchanged.
    EXPECT_EQ(compiled.dump(), source_gp.dump());

    // Spot-check a few load-bearing nested values survive the decompose/recompose.
    EXPECT_EQ(compiled["terrain"]["base_amplitude"], 42.0);
    EXPECT_EQ(compiled["terrain"]["shaping"]["peaks_amplitude"], 22.0);
    EXPECT_EQ(compiled["terrain"]["shaping"]["continental_spline"].size(), 5u);
    EXPECT_EQ(compiled["terrain"]["hydro"]["iterations"], 4);
    EXPECT_EQ(compiled["biomes"]["table"], "common/biomes.json");
    EXPECT_EQ(compiled["features"]["structures_enabled"], true);
    EXPECT_EQ(compiled["materials"]["strata"].size(), 2u);
}

// --- The fixed topology decomposed the right way. --------------------------
TEST(LayerGraphTest, FixedTopologyDecomposesStagesCorrectly) {
    const LayerGraph graph = LayerGraphFromPreset(FixedRichPreset());

    ASSERT_EQ(graph.nodes.size(), kLayerStageCount);
    // Stage order is the canonical pipeline order.
    EXPECT_EQ(graph.nodes[0].stage, LayerStage::BaseTerrain);
    EXPECT_EQ(graph.nodes[1].stage, LayerStage::Shaping);
    EXPECT_EQ(graph.nodes[2].stage, LayerStage::Hydro);
    EXPECT_EQ(graph.nodes[3].stage, LayerStage::Biomes);
    EXPECT_EQ(graph.nodes[4].stage, LayerStage::Features);
    EXPECT_EQ(graph.nodes[5].stage, LayerStage::Materials);

    // Every stage present in the rich preset.
    for (const LayerNode& n : graph.nodes) {
        EXPECT_TRUE(n.present) << "stage " << LayerStageId(n.stage) << " should be present";
    }

    // BaseTerrain owns the terrain scalars but NOT the nested shaping/hydro blocks.
    const LayerNode& base = graph.node(LayerStage::BaseTerrain);
    EXPECT_TRUE(base.params.contains("base_amplitude"));
    EXPECT_FALSE(base.params.contains("shaping"));
    EXPECT_FALSE(base.params.contains("hydro"));

    // Shaping / Hydro own their nested blocks verbatim.
    EXPECT_TRUE(graph.node(LayerStage::Shaping).params.contains("continental_spline"));
    EXPECT_TRUE(graph.node(LayerStage::Hydro).params.contains("iterations"));
    EXPECT_TRUE(graph.node(LayerStage::Features).params.contains("rivers_enabled"));
}

// --- Absent blocks stay absent (a minimal preset round-trips bit-exact too). -
TEST(LayerGraphTest, MinimalPresetAbsentBlocksStayAbsent) {
    const nlohmann::json preset = FixedMinimalPreset();
    const nlohmann::json& source_gp = preset.at("generation_params");

    const LayerGraph graph = LayerGraphFromPreset(preset);
    EXPECT_TRUE(graph.node(LayerStage::BaseTerrain).present);
    EXPECT_FALSE(graph.node(LayerStage::Shaping).present);
    EXPECT_FALSE(graph.node(LayerStage::Hydro).present);
    EXPECT_FALSE(graph.node(LayerStage::Biomes).present);
    EXPECT_TRUE(graph.node(LayerStage::Features).present);
    EXPECT_FALSE(graph.node(LayerStage::Materials).present);

    const nlohmann::json compiled = CompileLayerGraph(graph);
    EXPECT_EQ(compiled, source_gp);
    EXPECT_EQ(compiled.dump(), source_gp.dump());
    // No phantom blocks emitted.
    EXPECT_FALSE(compiled["terrain"].contains("shaping"));
    EXPECT_FALSE(compiled.contains("biomes"));
    EXPECT_FALSE(compiled.contains("materials"));
}

// --- Serialize -> deserialize -> compile round-trips bit-exactly. ----------
TEST(LayerGraphTest, SerializeDeserializeRoundTrip) {
    const nlohmann::json preset = FixedRichPreset();
    const nlohmann::json& source_gp = preset.at("generation_params");

    const LayerGraph graph = LayerGraphFromPreset(preset);
    const nlohmann::json serialized = SerializeLayerGraph(graph);

    // The serialized form is stable (schema + node array) and itself round-trips.
    EXPECT_EQ(serialized["schema"], LayerGraphSchema());
    ASSERT_TRUE(serialized["nodes"].is_array());
    EXPECT_EQ(serialized["nodes"].size(), kLayerStageCount);
    EXPECT_EQ(SerializeLayerGraph(DeserializeLayerGraph(serialized)).dump(), serialized.dump());

    // And compiling the deserialized graph reproduces the source params bit-exact.
    const LayerGraph reloaded = DeserializeLayerGraph(serialized);
    const nlohmann::json compiled = CompileLayerGraph(reloaded);
    EXPECT_EQ(compiled, source_gp);
    EXPECT_EQ(compiled.dump(), source_gp.dump());
}

// --- WriteLayerGraph embeds resolved params + a graph block; stripping the
//     graph block recovers the bit-exact source params. -----------------------
TEST(LayerGraphTest, WriteLayerGraphEmbedsResolvedParamsPlusGraph) {
    const nlohmann::json preset = FixedRichPreset();
    const nlohmann::json source_gp = preset.at("generation_params");

    const LayerGraph graph = LayerGraphFromPreset(preset);
    nlohmann::json out = preset; // copy
    WriteLayerGraph(out, graph);

    ASSERT_TRUE(out["generation_params"].contains("graph"));
    EXPECT_EQ(out["generation_params"]["graph"]["schema"], LayerGraphSchema());

    // Strip the additive graph block -> the resolved params are bit-exact to the
    // source (so a loader that ignores the graph loads the identical world).
    nlohmann::json resolved = out["generation_params"];
    resolved.erase("graph");
    EXPECT_EQ(resolved, source_gp);
    EXPECT_EQ(resolved.dump(), source_gp.dump());

    // The embedded graph itself reopens to the same graph (lossless reopen).
    const LayerGraph reopened = DeserializeLayerGraph(out["generation_params"]["graph"]);
    EXPECT_EQ(CompileLayerGraph(reopened).dump(), source_gp.dump());
}

// --- Unrecognized future top-level keys survive (passthrough exhaustiveness).
TEST(LayerGraphTest, UnknownKeysSurviveViaPassthrough) {
    nlohmann::json preset = FixedMinimalPreset();
    // A future loader key the graph does not model.
    preset["generation_params"]["future_block"] = {{"alpha", 1}, {"beta", 2.5}};
    const nlohmann::json source_gp = preset.at("generation_params");

    const LayerGraph graph = LayerGraphFromPreset(preset);
    EXPECT_TRUE(graph.passthrough.contains("future_block"));

    const nlohmann::json compiled = CompileLayerGraph(graph);
    EXPECT_EQ(compiled, source_gp);
    EXPECT_EQ(compiled.dump(), source_gp.dump());
    EXPECT_EQ(compiled["future_block"]["beta"], 2.5);
}

// --- A graph derived from a preset does NOT include any pre-existing graph
//     block in its own decomposition (graph is derived from resolved params). --
TEST(LayerGraphTest, PreExistingGraphBlockIsNotRecursed) {
    nlohmann::json preset = FixedMinimalPreset();
    // Inject a stale graph block; it must be ignored on decompose (NOT passthrough).
    preset["generation_params"]["graph"] = {{"schema", "stale"},
                                            {"nodes", nlohmann::json::array()}};

    const LayerGraph graph = LayerGraphFromPreset(preset);
    EXPECT_FALSE(graph.passthrough.contains("graph"));

    const nlohmann::json compiled = CompileLayerGraph(graph);
    // Compile emits the params WITHOUT a graph key (editing metadata is added only
    // by WriteLayerGraph).
    EXPECT_FALSE(compiled.contains("graph"));
}
