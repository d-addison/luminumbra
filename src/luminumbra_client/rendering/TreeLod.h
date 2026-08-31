#pragma once

// tree rendering (tree rendering): per-instance tree LOD mesh selection by camera
// distance. This is a PURELY, mechanical optimization: distant tree
// static-mesh instances draw a coarser LOD variant of the same asset so the far
// field spends fewer triangles, while the near field is byte-identical to today.
//
// IMPORTANT determinism note: nothing in this header touches the sim / world_hash
// path. LOD selection is a function of the *render* camera distance only and is
// never serialized, hashed, or fed back into the simulation. It lives entirely
// inside the G-Buffer draw pass.
//
// The header is intentionally free of any OpenGL / engine dependencies so the
// selection logic can be unit-tested in isolation (see
// test/rendering/tree_lod_test.cpp).

#include <cstddef>
#include <string>
#include <string_view>

namespace Luminumbra::Rendering {

// Number of LOD buckets we support (LOD0 = full asset, LOD1..LOD2 = coarser).
inline constexpr int kTreeLodCount = 4;

// Data-driven LOD distance thresholds (world units, render-only -- NEVER hashed).
// An instance whose camera distance is < lod1Distance draws LOD0 (the full,
// unchanged mesh); [lod1Distance, lod2Distance) draws LOD1; >= lod2Distance draws
// LOD2. Defaults are tuned so the NEAR field is unaffected; only genuinely
// distant trees swap down. These can be overridden from a render config table
// (render.tree_lod.*) without recompiling.
struct TreeLodConfig {
    // Master switch. When false, SelectTreeLod always returns 0 (LOD0) so the
    // scene is byte-identical to the pre-LOD renderer. Enabled by default but
    // behaves as a no-op until LOD variant meshes exist on disk (the draw path
    // falls back to LOD0 for any LOD whose file is missing).
    bool enabled = true;
    // Near radius around the camera that always renders the full mesh.
    // note: these thresholds were investigated as a lever but kept at the
    // blessed values. The instanced-foliage G-buffer cost is fill/OVERDRAW-bound, not triangle-
    // bound (measured: cutting forest triangles ~58% via LOD distance changes did NOT move
    // gbuffer ms), and pulling LOD3 inward made it WORSE — the cross-billboards are large flat
    // surfaces that add overdraw at oblique/aerial poses. So the win came from cheaper foliage
    // FRAGMENTS (see u_macroRockOverlay / u_forceFlat in g_buffer.frag), not LOD distance.
    float lod1Distance = 140.0f;
    // Beyond this, render the coarse procedural mesh.
    float lod2Distance = 320.0f;
    // Beyond this, render the cheap FAR-FIELD cross-billboard (LOD3) — a few quads
    // instead of branch geometry, so a vast forest stays in budget out to the horizon.
    float lod3Distance = 620.0f;
};

// Returns the LOD bucket index [0.. kTreeLodCount-1] for a tree instance at the
// given camera distance. Monotonic non-decreasing in distance. Negative or NaN
// distances clamp to LOD0 (treated as "very near"); the comparisons below make
// NaN fall through to LOD0 because all `>=` comparisons against NaN are false.
inline int SelectTreeLod(float cameraDistance, const TreeLodConfig& cfg) {
    if (!cfg.enabled) {
        return 0;
    }
    if (cameraDistance >= cfg.lod3Distance) {
        return 3;
    }
    if (cameraDistance >= cfg.lod2Distance) {
        return 2;
    }
    if (cameraDistance >= cfg.lod1Distance) {
        return 1;
    }
    return 0;
}

// Derives the LOD-variant mesh path from a base ".lmesh" asset path.
//   lod == 0 -> the base path unchanged (full mesh).
//   lod >= 1 -> "<stem>.lodN.lmesh" (e.g. "tree_leaves.lmesh" -> "tree_leaves.lod1.lmesh").
// The asset processor emits these coarser variants via its existing --max-tris
// decimation path (see tools/asset_processor.cpp). If the derived file is absent
// the draw path falls back to the base path, so a world with no LOD variants
// renders exactly as before.
inline std::string LodMeshPath(std::string_view basePath, int lod) {
    if (lod <= 0) {
        return std::string(basePath);
    }
    constexpr std::string_view kExt = ".lmesh";
    std::string out(basePath);
    // Insert ".lodN" before the ".lmesh" extension when present; otherwise append.
    const bool hasExt =
        out.size() >= kExt.size() && std::string_view(out).substr(out.size() - kExt.size()) == kExt;
    const std::string suffix = ".lod" + std::to_string(lod);
    if (hasExt) {
        out.insert(out.size() - kExt.size(), suffix);
    } else {
        out += suffix;
    }
    return out;
}

} // namespace Luminumbra::Rendering
