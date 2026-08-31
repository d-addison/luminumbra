#pragma once

// tree rendering (tree rendering, ): far-field tree IMPOSTOR support — the
// hemi-octahedral view-direction <-> atlas-tile mapping math.
//
// An octahedral impostor captures a mesh's appearance from a grid of view
// directions into a single texture atlas (one tile per direction). At runtime a
// distant instance draws ONE camera-facing quad that samples the atlas tile(s)
// nearest the current view direction, so a vast forest's far field collapses to a
// few quads + one shared atlas draw instead of per-instance branch geometry. The
// near field is untouched (LOD0..LOD2, see TreeLod.h).
//
// WHY this header is GL-free: the SAME direction<->tile mapping is needed by (a)
// the offline/runtime ATLAS BAKE (which direction does tile (i,j) capture?) and
// (b) the runtime IMPOSTOR SHADER (which tile(s) to sample for this view?). Keeping
// it a pure, dependency-free header lets both share one source of truth and lets
// the mapping be unit-tested in isolation (test/rendering/octa_impostor_test.cpp) —
// exactly the discipline TreeLod.h follows.
//
// DETERMINISM: render-only. Nothing here is hashed, serialized, or fed to the sim.
//
// Convention: tree "up" is +Y. Impostors capture the UPPER hemisphere of view
// directions (horizon up to top-down) — you never view a tree from underground —
// so we use a HEMI-octahedral parameterization that maps the whole y>=0 hemisphere
// onto the [0,1]^2 atlas with no wasted area (full octahedral would waste half the
// atlas on never-sampled bottom-hemisphere tiles).

#include <algorithm>
#include <array>
#include <cmath>

namespace Luminumbra::Rendering {

struct Vec3f {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
struct Vec2f {
    float x = 0.0f, y = 0.0f;
};

// --- Hemi-octahedral encode/decode (y = up hemisphere, y >= 0) --------------------
//
// Encode maps a unit direction with y >= 0 to atlas UV in [0,1]^2; decode is its
// inverse. The 45-degree fold (the (x+z, x-z) rotation) packs the diamond-shaped
// hemi-octahedron projection into the full square so every texel maps to a valid
// direction (no gutter). Round-trip is exact to float precision on the hemisphere.

inline Vec2f HemiOctaEncode(Vec3f dir) {
    const float l1 = std::fabs(dir.x) + std::fabs(dir.y) + std::fabs(dir.z);
    const float inv = (l1 > 0.0f) ? 1.0f / l1 : 0.0f;
    const float px = dir.x * inv;
    const float pz = dir.z * inv;
    // 45-degree rotation folds the upper-hemisphere diamond into the unit square.
    Vec2f uv{px + pz, px - pz};
    uv.x = uv.x * 0.5f + 0.5f;
    uv.y = uv.y * 0.5f + 0.5f;
    return uv;
}

inline Vec3f HemiOctaDecode(Vec2f uv) {
    // Undo the bias/scale, then the 45-degree rotation.
    const float ux = uv.x * 2.0f - 1.0f;
    const float uy = uv.y * 2.0f - 1.0f;
    const float x = (ux + uy) * 0.5f;
    const float z = (ux - uy) * 0.5f;
    Vec3f dir{x, 1.0f - std::fabs(x) - std::fabs(z), z};
    // Normalize (the octahedron point projects back to the unit hemisphere).
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    const float inv = (len > 0.0f) ? 1.0f / len : 0.0f;
    return Vec3f{dir.x * inv, dir.y * inv, dir.z * inv};
}

// --- Atlas grid -------------------------------------------------------------------
//
// gridResolution x gridResolution tiles. Tile (i,j) captures the direction at its
// CELL CENTER so the bake and the shader agree. A larger grid = smoother view
// transitions at the cost of atlas memory (gridResolution^2 * tileResolution^2).

struct OctaImpostorGrid {
    int gridResolution = 8;   // tiles per axis (8x8 = 64 captured views)
    int tileResolution = 128; // pixels per tile side in the atlas
};

// Direction captured by tile (i,j) — the hemi-octa decode of the cell CENTER.
inline Vec3f OctaTileDirection(int i, int j, const OctaImpostorGrid& grid) {
    const int n = std::max(1, grid.gridResolution);
    const Vec2f uv{(static_cast<float>(i) + 0.5f) / static_cast<float>(n),
                   (static_cast<float>(j) + 0.5f) / static_cast<float>(n)};
    return HemiOctaDecode(uv);
}

// Continuous tile coordinate for a view direction (in [0, gridResolution]); the
// integer part is the tile, the fractional part is the blend toward the next tile.
inline Vec2f OctaTileCoord(Vec3f viewDir, const OctaImpostorGrid& grid) {
    const int n = std::max(1, grid.gridResolution);
    const Vec2f uv = HemiOctaEncode(viewDir);
    return Vec2f{uv.x * static_cast<float>(n), uv.y * static_cast<float>(n)};
}

// Nearest tile (i,j) for a view direction, clamped to the grid.
inline void OctaNearestTile(Vec3f viewDir, const OctaImpostorGrid& grid, int& outI, int& outJ) {
    const int n = std::max(1, grid.gridResolution);
    const Vec2f tc = OctaTileCoord(viewDir, grid);
    outI = std::clamp(static_cast<int>(std::floor(tc.x)), 0, n - 1);
    outJ = std::clamp(static_cast<int>(std::floor(tc.y)), 0, n - 1);
}

} // namespace Luminumbra::Rendering
