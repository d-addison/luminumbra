#include "world/LayerGraph.h"

#include <array>

namespace Luminumbra::world {

namespace {

// Canonical stage metadata, indexed by LayerStage. Order here IS the edge
// topology (base FBM first, materials last).
struct StageMeta {
    LayerStage stage;
    const char* id;
    const char* label;
};

constexpr std::array<StageMeta, kLayerStageCount> kStages = {{
    {LayerStage::BaseTerrain, "base_terrain", "Base Terrain (FBM)"},
    {LayerStage::Shaping, "shaping", "Continental Shaping"},
    {LayerStage::Hydro, "hydro", "Hydraulic / Thermal Erosion"},
    {LayerStage::Biomes, "biomes", "Biomes & Relief"},
    {LayerStage::Features, "features", "Features (caves/rivers/lakes/cliffs)"},
    {LayerStage::Materials, "materials", "Materials (strata/veins)"},
}};

// A node carrying an absent stage (params null, present=false).
LayerNode AbsentNode(LayerStage stage) {
    LayerNode n;
    n.stage = stage;
    n.present = false;
    n.params = nlohmann::json(nullptr);
    return n;
}

} // namespace

const char* LayerStageId(LayerStage stage) {
    return kStages[static_cast<std::size_t>(stage)].id;
}

const char* LayerStageLabel(LayerStage stage) {
    return kStages[static_cast<std::size_t>(stage)].label;
}

bool LayerStageFromId(const std::string& id, LayerStage& out) {
    for (const StageMeta& m : kStages) {
        if (id == m.id) {
            out = m.stage;
            return true;
        }
    }
    return false;
}

const char* LayerGraphSchema() {
    return "luminumbra.worldgen_graph.v1";
}

const LayerNode& LayerGraph::node(LayerStage stage) const {
    return nodes[static_cast<std::size_t>(stage)];
}

LayerNode& LayerGraph::node(LayerStage stage) {
    return nodes[static_cast<std::size_t>(stage)];
}

LayerGraph LayerGraphFromPreset(const nlohmann::json& preset) {
    LayerGraph graph;
    graph.nodes.reserve(kLayerStageCount);
    for (const StageMeta& m : kStages) {
        graph.nodes.push_back(AbsentNode(m.stage));
    }

    if (!preset.is_object() || !preset.contains("generation_params") ||
        !preset["generation_params"].is_object()) {
        return graph;
    }
    const nlohmann::json& gp = preset["generation_params"];

    // --- terrain: split into BaseTerrain (terrain minus shaping/hydro), Shaping,
    //     Hydro. Preserve the verbatim slices so Compile re-assembles bit-exactly.
    if (gp.contains("terrain") && gp["terrain"].is_object()) {
        const nlohmann::json& terrain = gp["terrain"];
        nlohmann::json base = nlohmann::json::object();
        for (auto it = terrain.begin(); it != terrain.end(); ++it) {
            const std::string& key = it.key();
            if (key == "shaping") {
                LayerNode& n = graph.node(LayerStage::Shaping);
                n.present = true;
                n.params = it.value();
            } else if (key == "hydro") {
                LayerNode& n = graph.node(LayerStage::Hydro);
                n.present = true;
                n.params = it.value();
            } else {
                base[key] = it.value();
            }
        }
        LayerNode& bn = graph.node(LayerStage::BaseTerrain);
        bn.present = true;
        bn.params = std::move(base);
    }

    // --- top-level blocks ---
    if (gp.contains("biomes") && gp["biomes"].is_object()) {
        LayerNode& n = graph.node(LayerStage::Biomes);
        n.present = true;
        n.params = gp["biomes"];
    }
    if (gp.contains("features") && gp["features"].is_object()) {
        LayerNode& n = graph.node(LayerStage::Features);
        n.present = true;
        n.params = gp["features"];
    }
    if (gp.contains("materials") && gp["materials"].is_object()) {
        LayerNode& n = graph.node(LayerStage::Materials);
        n.present = true;
        n.params = gp["materials"];
    }

    // --- passthrough: any top-level generation_params key the graph does not
    //     model (excluding the graph block itself, which is editing metadata, and
    //     terrain/biomes/features/materials which become nodes). This keeps
    //     Compile exhaustive: an unknown future key round-trips bit-exactly.
    for (auto it = gp.begin(); it != gp.end(); ++it) {
        const std::string& key = it.key();
        if (key == "terrain" || key == "biomes" || key == "features" || key == "materials" ||
            key == "graph") {
            continue;
        }
        graph.passthrough[key] = it.value();
    }

    return graph;
}

nlohmann::json CompileLayerGraph(const LayerGraph& graph) {
    nlohmann::json gp = nlohmann::json::object();

    // Re-assemble terrain = base + shaping + hydro (only the present blocks).
    const LayerNode& base = graph.node(LayerStage::BaseTerrain);
    const LayerNode& shaping = graph.node(LayerStage::Shaping);
    const LayerNode& hydro = graph.node(LayerStage::Hydro);
    if (base.present || shaping.present || hydro.present) {
        nlohmann::json terrain = nlohmann::json::object();
        if (base.present && base.params.is_object()) {
            for (auto it = base.params.begin(); it != base.params.end(); ++it) {
                terrain[it.key()] = it.value();
            }
        }
        if (shaping.present) {
            terrain["shaping"] = shaping.params;
        }
        if (hydro.present) {
            terrain["hydro"] = hydro.params;
        }
        gp["terrain"] = std::move(terrain);
    }

    if (graph.node(LayerStage::Biomes).present) {
        gp["biomes"] = graph.node(LayerStage::Biomes).params;
    }
    if (graph.node(LayerStage::Features).present) {
        gp["features"] = graph.node(LayerStage::Features).params;
    }
    if (graph.node(LayerStage::Materials).present) {
        gp["materials"] = graph.node(LayerStage::Materials).params;
    }

    // Re-attach unrecognized keys verbatim.
    if (graph.passthrough.is_object()) {
        for (auto it = graph.passthrough.begin(); it != graph.passthrough.end(); ++it) {
            gp[it.key()] = it.value();
        }
    }

    return gp;
}

nlohmann::json SerializeLayerGraph(const LayerGraph& graph) {
    nlohmann::json out = nlohmann::json::object();
    out["schema"] = LayerGraphSchema();
    nlohmann::json nodes = nlohmann::json::array();
    for (const LayerNode& n : graph.nodes) {
        nlohmann::json jn = nlohmann::json::object();
        jn["stage"] = LayerStageId(n.stage);
        jn["present"] = n.present;
        jn["params"] = n.params;
        nodes.push_back(std::move(jn));
    }
    out["nodes"] = std::move(nodes);
    out["passthrough"] = graph.passthrough;
    return out;
}

LayerGraph DeserializeLayerGraph(const nlohmann::json& graph_json) {
    LayerGraph graph;
    graph.nodes.reserve(kLayerStageCount);
    for (const StageMeta& m : kStages) {
        graph.nodes.push_back(AbsentNode(m.stage));
    }

    if (!graph_json.is_object()) {
        return graph;
    }
    if (graph_json.contains("nodes") && graph_json["nodes"].is_array()) {
        for (const auto& jn : graph_json["nodes"]) {
            if (!jn.is_object() || !jn.contains("stage") || !jn["stage"].is_string()) {
                continue;
            }
            LayerStage stage;
            if (!LayerStageFromId(jn["stage"].get<std::string>(), stage)) {
                continue;
            }
            LayerNode& n = graph.node(stage);
            n.present = jn.value("present", false);
            if (jn.contains("params")) {
                n.params = jn["params"];
            }
        }
    }
    if (graph_json.contains("passthrough") && graph_json["passthrough"].is_object()) {
        graph.passthrough = graph_json["passthrough"];
    }
    return graph;
}

nlohmann::json& WriteLayerGraph(nlohmann::json& preset, const LayerGraph& graph) {
    if (!preset.is_object()) {
        preset = nlohmann::json::object();
    }
    nlohmann::json gp = CompileLayerGraph(graph);
    // Attach the editing/provenance graph (the resolved params remain
    // authoritative on load; this block lets the authoring tool reopen the graph).
    gp["graph"] = SerializeLayerGraph(graph);
    preset["generation_params"] = std::move(gp);
    return preset;
}

} // namespace Luminumbra::world
