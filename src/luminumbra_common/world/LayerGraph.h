#pragma once

// CONSTRAINED layer graph (flagged authoring tool).
//
// Authors worldgen as a SMALL, FIXED-TOPOLOGY graph of the EXISTING pipeline
// stages (base FBM -> domain warp/shaping -> biome relief -> rivers -> lakes ->
// cliffs -> hydro -> materials), so the layered structure is visible/editable
// WITHOUT a free-topology evaluator (that would require rewriting the
// determinism-pinned worldgen core — an explicit NON-GOAL).
//
// SCOPE (de-risked per the 4-way critique):
//   - The graph is a STRUCTURED VIEW over the SAME generation_params JSON the
//     loader already consumes. Each node owns the JSON sub-object for one stage;
//     nodes expose the same scalars as TerrainGenParams. Edges are the FIXED
//     stage order (implicit in v1).
//   - Compile(graph) -> a generation_params JSON that is BIT-EXACT to the source
//     preset's generation_params (so world_hash / run==replay / far-LOD are
//     UNCHANGED). Bit-exactness is structural: the compiler re-assembles the
//     verbatim per-stage slices it decomposed, in the canonical key order, and
//     re-attaches anything it did not recognize unchanged.
//   - The graph SERIALIZES to generation_params.graph (editing/provenance
//     metadata). A loader PREFERS the embedded resolved params; the graph block
//     is additive and ignorable.
//
// This is engine-side (luminumbra_common/world), has NO UI/GL dependency, and is
// a PURE JSON transform — it never reimplements worldgen sampling. The ImGui
// debug/WorldGenViewer (a flagged --worldgen-graph internal authoring tool)
// renders it.

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Luminumbra::world {

// The fixed pipeline stages, in canonical evaluation order. Each stage maps to a
// slice of generation_params (a top-level key, or a nested terrain.* block). The
// order is the edge topology of the graph (read-only in v1).
enum class LayerStage : std::size_t {
    BaseTerrain = 0, // generation_params.terrain scalars (FBM: freq/amp/octaves/...)
    Shaping,   // generation_params.terrain.shaping (continentalness/erosion/peaks + domain warp)
    Hydro,     // generation_params.terrain.hydro (hydraulic/thermal erosion)
    Biomes,    // generation_params.biomes (temperature/humidity/relief + table)
    Features,  // generation_params.features (caves/rivers/lakes/cliffs/structures)
    Materials, // generation_params.materials (strata/veins)
    Count
};
inline constexpr std::size_t kLayerStageCount = static_cast<std::size_t>(LayerStage::Count);

// Stable lowercase id for a stage (the JSON key under graph.nodes[].stage).
const char* LayerStageId(LayerStage stage);
// Human label for a stage (the ImGui card title).
const char* LayerStageLabel(LayerStage stage);
// Parse a stage id back to the enum; returns false on an unknown id.
bool LayerStageFromId(const std::string& id, LayerStage& out);

// One node in the constrained graph: a fixed stage plus the verbatim JSON slice
// of generation_params it governs. `present` records whether the source preset
// actually carried this stage's block (so Compile re-emits ONLY the blocks the
// source had — an absent biomes block must stay absent for bit-exactness).
//   - BaseTerrain.params = the generation_params.terrain object MINUS its nested
//     "shaping"/"hydro" sub-objects (those are their own nodes).
//   - Shaping.params      = generation_params.terrain.shaping (if present).
//   - Hydro.params        = generation_params.terrain.hydro (if present).
//   - Biomes.params       = generation_params.biomes (if present).
//   - Features.params     = generation_params.features.
//   - Materials.params    = generation_params.materials (if present).
struct LayerNode {
    LayerStage stage = LayerStage::BaseTerrain;
    bool present = false;  // the block existed in the source generation_params
    nlohmann::json params; // the verbatim JSON slice for this stage
};

// The constrained, fixed-topology graph. Holds exactly one node per stage (in
// stage order) plus a passthrough for any UNRECOGNIZED top-level generation_params
// keys (so a future loader key round-trips bit-exactly without a code change).
struct LayerGraph {
    std::vector<LayerNode> nodes; // one per LayerStage, in canonical order
    // Unrecognized top-level keys under generation_params (NOT terrain/biomes/
    // features/materials/graph), preserved verbatim so Compile is exhaustive.
    nlohmann::json passthrough = nlohmann::json::object();

    // Accessor: the node for a given stage (nodes is always stage-ordered after
    // FromPreset / Default).
    const LayerNode& node(LayerStage stage) const;
    LayerNode& node(LayerStage stage);
};

// Decompose a preset's generation_params into the fixed-topology graph. `preset`
// is the WHOLE preset JSON (with a "generation_params" object). Any existing
// generation_params.graph block is IGNORED here (the graph is derived from the
// resolved params, not from itself). Unrecognized top-level keys land in
// `passthrough`. The terrain node's params are terrain MINUS shaping/hydro.
LayerGraph LayerGraphFromPreset(const nlohmann::json& preset);

// Compile the graph back to a generation_params JSON object. The result is
// BIT-EXACT to the source preset's generation_params (same keys, same order,
// same values) — proven field-for-field by the parity test. Re-assembles
// terrain = terrain-base + shaping + hydro (in the source's key order), then
// biomes/features/materials (only the blocks that were present), then the
// passthrough keys. Does NOT emit a "graph" key (that is editing metadata, added
// separately by WriteLayerGraph).
nlohmann::json CompileLayerGraph(const LayerGraph& graph);

// Serialize the graph to its provenance JSON (the value stored at
// generation_params.graph): { "schema": "...", "nodes": [ {"stage","present",
// "params"},... ], "passthrough": {...} }. Deserialize is the inverse and
// round-trips byte-for-byte.
nlohmann::json SerializeLayerGraph(const LayerGraph& graph);
// Parse a serialized graph back. On a malformed/absent block returns a graph
// with all nodes absent (Compile then yields an empty generation_params); callers
// that need the real params should prefer the embedded resolved params.
LayerGraph DeserializeLayerGraph(const nlohmann::json& graph_json);

// The schema id stamped into the serialized graph (lets a future format bump be
// detected). Mirrors the WorldGenViewer artifact-schema convention.
const char* LayerGraphSchema();

// Write the graph INTO a preset: sets preset["generation_params"] =
// Compile(graph) (bit-exact resolved params) and attaches
// preset["generation_params"]["graph"] = Serialize(graph) (editing metadata).
// Mutates and returns `preset`. The resolved params are authoritative on load;
// the graph block is additive provenance the create/authoring tool reopens.
nlohmann::json& WriteLayerGraph(nlohmann::json& preset, const LayerGraph& graph);

} // namespace Luminumbra::world
