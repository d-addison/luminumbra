#pragma once

//   ( , ): the DECLARATIVE FRAME GRAPH.
//
// RenderPipeline::render_frame dispatches ~23 GPU stages in a hand-scripted sequence whose
// ordering is IMPLICIT -- each stage's GL writes happen to be read (or blended onto) by a later
// stage, and the correctness of the whole frame rests on a human keeping those ~600 lines in the
// right order. This header promotes that sequence to DATA: an ordered list of nodes, each
// declaring the named render resources it READS and WRITES, plus the one ordering fact the
// resource-flow can't express on its own -- the god-rays "latest opaque snapshot" (it samples
// whichever of the two post-sky snapshots executed most recently). A scheduler topologically
// orders the nodes from those declared dependencies; a validator proves the declaration is
// internally consistent (no read-before-write; every latest-writer edge resolves to a real prior
// writer).
//
// The graph drives render_frame through the executor table. Runtime stage tracing
// asserts that the scheduled declaration and executed order stay identical;
// validation catches missing resource dependencies, and the god-rays contract
// verifies its latest opaque snapshot under both weather branches.
//
// DETERMINISM. Pure CPU ordering logic over resource NAME strings. No GL, no sim, no readback.
// Render-only observability -- it can never feed world_hash; the server never
// even constructs it.

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {

// One frame-graph stage: its stable id, the named resources it consumes/produces, and whether its
// GL work is runtime-gated. `latest_writer_reads` is a read whose ORDERING pins to the LAST prior
// writer of the resource (the god-rays post-sky opaque snapshot: snapshot #2 if the weather
// overlay ran, else #1) -- distinct from a plain `reads`, which only requires *some* prior writer.
struct RenderGraphNode {
    std::string name;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    std::vector<std::string> latest_writer_reads;
    bool conditional = false; // GL work is runtime-gated; still holds a fixed slot in the order
};

// The ordered declaration + the schedule/validation derived from it.
class RenderGraph {
public:
    void add(RenderGraphNode node) {
        m_nodes.push_back(std::move(node));
    }
    const std::vector<RenderGraphNode>& nodes() const {
        return m_nodes;
    }
    std::size_t size() const {
        return m_nodes.size();
    }

    // Stable topological order. Edges are derived from the declared resource flow:
    //   RAW: a read of R depends on the most-recent prior WRITER of R;
    //   WAW: writer_i of R depends on writer_{i-1} of R (writers of a target stay in authored
    //   order); latest_writer_reads: same RAW edge, but the validator also PINS the identity of
    //   that writer.
    // Ties (nodes with all dependencies already satisfied and no edge between them -- e.g. shadow
    // vs g-buffer, which share no resource) break on AUTHORED INDEX, matching render_frame's chosen
    // linearization of the partial order. A well-formed authored graph therefore schedules to
    // exactly its authored order; a MIS-declared dependency (a read whose only producer is authored
    // later) forces a reorder that diverges from the golden trace, or a cycle that empties the
    // schedule -- either way the gate catches it.
    std::vector<std::string> schedule() const {
        const std::size_t n = m_nodes.size();
        std::vector<std::vector<std::size_t>> succ(n);
        std::vector<int> indeg(n, 0);
        auto add_edge = [&](std::size_t from, std::size_t to) {
            if (from == to)
                return;
            succ[from].push_back(to);
            ++indeg[to];
        };
        // Build RAW + WAW + latest-writer edges by walking authored order and tracking, for each
        // resource, the sequence of writer indices seen so far.
        for (std::size_t i = 0; i < n; ++i) {
            auto latest_writer = [&](const std::string& res) -> long {
                long w = -1;
                for (std::size_t j = 0; j < i; ++j) {
                    const RenderGraphNode& p = m_nodes[j];
                    if (std::find(p.writes.begin(), p.writes.end(), res) != p.writes.end())
                        w = static_cast<long>(j);
                }
                return w;
            };
            for (const std::string& r : m_nodes[i].reads) {
                const long w = latest_writer(r);
                if (w >= 0)
                    add_edge(static_cast<std::size_t>(w), i);
            }
            for (const std::string& r : m_nodes[i].latest_writer_reads) {
                const long w = latest_writer(r);
                if (w >= 0)
                    add_edge(static_cast<std::size_t>(w), i);
            }
            for (const std::string& wres : m_nodes[i].writes) {
                const long prev = latest_writer(wres); // previous writer of this same target
                if (prev >= 0)
                    add_edge(static_cast<std::size_t>(prev), i);
            }
        }
        // Kahn's algorithm, always draining the READY node of LOWEST authored index.
        std::vector<std::string> order;
        order.reserve(n);
        std::vector<char> done(n, 0);
        for (std::size_t emitted = 0; emitted < n; ++emitted) {
            long pick = -1;
            for (std::size_t i = 0; i < n; ++i) {
                if (!done[i] && indeg[i] == 0) {
                    pick = static_cast<long>(i);
                    break;
                }
            }
            if (pick < 0)
                break; // cycle: leave the schedule short so the gate fails loudly
            const std::size_t p = static_cast<std::size_t>(pick);
            done[p] = 1;
            indeg[p] = -1;
            order.push_back(m_nodes[p].name);
            for (std::size_t s : succ[p])
                --indeg[s];
        }
        return order;
    }

    // Internal-consistency violations (empty == valid):
    //   * a plain read of a resource that NO authored-earlier node writes (read-before-write);
    //   * a latest_writer_read with no prior writer at all (the dynamic edge can't resolve).
    // Reads of EXTERNAL inputs (never written by any node) are declared as such by simply not
    // listing them -- only resources some node writes are graph-internal and checkable.
    std::vector<std::string> validate() const {
        std::vector<std::string> violations;
        auto writes_before = [&](const std::string& res, std::size_t before) {
            for (std::size_t j = 0; j < before; ++j) {
                const RenderGraphNode& p = m_nodes[j];
                if (std::find(p.writes.begin(), p.writes.end(), res) != p.writes.end())
                    return true;
            }
            return false;
        };
        auto written_anywhere = [&](const std::string& res) {
            for (const RenderGraphNode& p : m_nodes) {
                if (std::find(p.writes.begin(), p.writes.end(), res) != p.writes.end())
                    return true;
            }
            return false;
        };
        for (std::size_t i = 0; i < m_nodes.size(); ++i) {
            const RenderGraphNode& node = m_nodes[i];
            for (const std::string& r : node.reads) {
                // A graph-internal resource (written by some node) must be written BEFORE this
                // read.
                if (written_anywhere(r) && !writes_before(r, i)) {
                    violations.push_back(node.name + " reads '" + r +
                                         "' before any node writes it");
                }
            }
            for (const std::string& r : node.latest_writer_reads) {
                if (!writes_before(r, i)) {
                    violations.push_back(node.name + " latest-writer-reads '" + r +
                                         "' but no prior node writes it");
                }
            }
        }
        return violations;
    }

    // The name of the last node BEFORE `consumer` (authored order) that writes `resource`, or ""
    // if none -- the concrete resolution of a latest_writer_reads edge. Pruning a conditional
    // writer (e.g. the weather snapshot when no weather ran) shifts this to the earlier snapshot,
    // which is exactly the god-rays runtime behavior this models.
    std::string latest_writer_of(const std::string& resource, const std::string& consumer) const {
        std::string writer;
        for (const RenderGraphNode& node : m_nodes) {
            if (node.name == consumer)
                break;
            if (std::find(node.writes.begin(), node.writes.end(), resource) != node.writes.end()) {
                writer = node.name;
            }
        }
        return writer;
    }

    // Remove a node by name (used to model a conditional stage that did not run this frame).
    void prune(const std::string& name) {
        m_nodes.erase(
            std::remove_if(m_nodes.begin(),
                           m_nodes.end(),
                           [&](const RenderGraphNode& node) { return node.name == name; }),
            m_nodes.end());
    }

private:
    std::vector<RenderGraphNode> m_nodes;
};

// The canonical Luminumbra frame graph: render_frame's authored dispatch sequence AS DATA.
// The node names + order MUST match the RenderPipeline::record_frame_stage calls in render_frame
// (the gate asserts schedule == the emitted trace). Resource names are the load-bearing render
// targets; external inputs (meshes, LUTs, camera) are intentionally omitted from `reads` so
// validate only checks graph-internal producer/consumer ordering.
inline RenderGraph BuildLuminumbraFrameGraph() {
    RenderGraph g;
    // The four G-buffer color attachments, written by the geometry stage and any stage that draws
    // into the same G-buffer afterward (plant / far-field), read by SSAO + lighting.
    const std::vector<std::string> gbuf_color = {
        "gbuffer.position", "gbuffer.normal_material", "gbuffer.albedo", "gbuffer.metallic_ao"};
    auto gbuf_all = [&]() {
        std::vector<std::string> v = gbuf_color;
        v.push_back("gbuffer.depth");
        return v;
    };

    // 1. Shadow cascades. Reads terrain (external); writes the cascade depth array
    // + ( , ) the tinted-transmission cascade the glass
    // occluder sub-pass fills (white = identity when no glass exists).
    g.add({"shadow", {}, {"shadow.depth_array", "shadow.tint_array"}, {}, false});
    // 2. G-buffer: the deferred geometry pass writes all four attachments + depth.
    g.add({"gbuffer", {}, gbuf_all(), {}, false});
    // 2a/2b: procedural plants + experimental far-field raymarch draw into the SAME G-buffer,
    // depth-tested against gbuffer.depth (they read depth, over-write color + depth).
    g.add({"plant_procgen", {"gbuffer.depth"}, gbuf_all(), {}, true});
    // 2c: pheromone ground decals additively tint the ALBEDO attachment only.
    g.add({"ground_decals", {"gbuffer.albedo"}, {"gbuffer.albedo"}, {}, true});
    // 3: SSAO reads position+normal, writes raw; blur reads raw, writes blur.
    g.add({"ssao", {"gbuffer.position", "gbuffer.normal_material"}, {"ssao.raw"}, {}, false});
    g.add({"ssao_blur", {"ssao.raw"}, {"ssao.blur"}, {}, false});
    // 4: Lighting consumes the G-buffer + shadows + AO, writes the lit color + its own depth.
    {
        std::vector<std::string> reads = gbuf_color;
        reads.push_back("shadow.depth_array");
        reads.push_back("shadow.tint_array"); //  the glass transmission multiply
        reads.push_back("ssao.blur");
        g.add({"lighting", reads, {"lighting.color", "lighting.depth"}, {}, false});
    }
    // 5: blit the G-buffer depth into the lighting FBO so sky + water share occlusion.
    g.add({"depth_blit_to_lighting", {"gbuffer.depth"}, {"lighting.depth"}, {}, false});
    // 6: skybox draws the sky where depth == far (reads lighting.depth + gbuffer.depth for the
    // half-res cloud composite), blending into lighting.color.
    g.add({"skybox", {"lighting.depth", "gbuffer.depth"}, {"lighting.color"}, {}, false});
    // 7: snapshot #1 of the lit opaque+sky color, then water blends over it.
    g.add({"opaque_snapshot", {"lighting.color"}, {"lighting.opaque_color"}, {}, false});
    g.add({"water", {"lighting.opaque_color", "lighting.depth"}, {"lighting.color"}, {}, true});
    // 7-W: waterfall veils blend over the lit scene, depth-tested.
    g.add({"waterfall", {"lighting.depth"}, {"lighting.color"}, {}, true});
    // WBOIT glass — panes accumulate into oit.accum/
    // oit.reveal (depth-tested vs the shared lighting depth, refraction from the
    // pre-water opaque snapshot), then the resolve composites over lighting.color
    // BEFORE the weather snapshot so god-rays/weather see resolved glass.
    g.add({"glass_oit_accum",
           {"lighting.depth", "lighting.opaque_color"},
           {"oit.accum", "oit.reveal"},
           {},
           true});
    g.add({"glass_oit_resolve", {"oit.accum", "oit.reveal"}, {"lighting.color"}, {}, true});
    // 7a: snapshot #2 (only when the weather overlay runs), then the weather overlay reads it.
    g.add({"weather_opaque_snapshot", {"lighting.color"}, {"lighting.opaque_color"}, {}, true});
    g.add({"weather_overlay", {"lighting.opaque_color"}, {"lighting.color"}, {}, false});
    //  rendering (,  ): the froxel media volume — inject
    // (density + in-scatter per froxel, sampling the shadow depth AND the  tint
    // cascade for colored shafts) then front-to-back integrate. The aerial stage
    // composes the integrated volume (: extends the analytic, never
    // replaces it). Quality 0 (default) leaves both stages as zero-GL no-ops.
    g.add({"froxel_inject",
           {"shadow.depth_array", "shadow.tint_array"},
           {"froxel.scatter"},
           {},
           true});
    g.add({"froxel_integrate", {"froxel.scatter"}, {"froxel.integrated"}, {}, true});
    // 7b: aerial-perspective in-scatter over the lit scene (+ the froxel compose).
    g.add({"aerial",
           {"lighting.color", "lighting.depth", "froxel.integrated"},
           {"lighting.color"},
           {},
           true});
    // 7b2: god rays sample the LATEST opaque snapshot (#2 if weather ran, else #1) + composite.
    g.add({"god_rays", {"lighting.color"}, {"lighting.color"}, {"lighting.opaque_color"}, false});
    // 7c: foliage cards blend into the lit target, depth-tested.
    g.add({"foliage", {"lighting.depth"}, {"lighting.color"}, {}, true});
    // TAAU resolve of the opaque lit color before the transparent particle/lightning composite.
    g.add({"taau_resolve", {"lighting.color"}, {"lighting.color"}, {}, true});
    // mean-log-luminance meter of the resolved lit scene ->
    // the AsyncReadbackRing (consumed next prepare_frame). The first BORN-graph-
    // native stage: it exists as a node + executor entry, never in a hand script.
    g.add({"luminance_meter", {"lighting.color"}, {"exposure.meter"}, {}, true});
    // 8 / 8b: particles then lightning composite over the lit target.
    g.add({"particles", {"lighting.depth"}, {"lighting.color"}, {}, true});
    g.add({"lightning_overlay", {"lighting.color"}, {"lighting.color"}, {}, true});
    // 9: final blit of the lit color to the swapchain (or offscreen preview target).
    g.add({"final_blit", {"lighting.color"}, {"swapchain.color"}, {}, false});
    // Debug-view override (default-OFF) re-writes the swapchain with a single G-buffer channel.
    g.add({"debug_view", gbuf_all(), {"swapchain.color"}, {}, true});
    return g;
}

} // namespace Luminumbra::Rendering
