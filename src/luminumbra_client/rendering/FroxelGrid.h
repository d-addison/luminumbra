#pragma once

#include <algorithm>
#include <cmath>

//  rendering (,  ): the froxel grid model — dimensions and
// the exponential depth-slice mapping, shared by the froxel_inject/froxel_integrate
// compute shaders and the FroxelModel gtests. ONE definition; the shaders mirror
// these exact expressions (the GLSL constants are pinned by the gtest anchors).
//
// Geometry: a view-frustum-aligned 160x90x64 grid. X/Y tile the screen
// uniformly; Z slices distribute EXPONENTIALLY between kNearDepth and kFarDepth so
// near media (where parallax and lighting detail matter) get fine slices and the
// far field coarsens — the standard froxel-volumetrics slicing (Frostbite/id).
//
// Render-only: never feeds the sim or world_hash.
namespace Luminumbra::Rendering::Froxel {

inline constexpr int kGridX = 160;
inline constexpr int kGridY = 90;
inline constexpr int kGridZ = 64;

// The participating-media march range in view-space metres. Media beyond
// kFarDepth is the analytic aerial's domain (the froxel volume COMPOSES with the
// aerial pass, it does not replace it — ).
inline constexpr float kNearDepth = 0.5f;
inline constexpr float kFarDepth = 160.0f;

// View depth of slice boundary i (0..kGridZ inclusive): exponential distribution
//   d(i) = near * (far/near)^(i / kGridZ)
// d(0) == kNearDepth exactly; d(kGridZ) == kFarDepth exactly.
inline float SliceBoundaryDepth(int boundary) {
    const float t = static_cast<float>(boundary) / static_cast<float>(kGridZ);
    return kNearDepth * std::pow(kFarDepth / kNearDepth, t);
}

// The inverse: which slice a view depth lands in, clamped to [0, kGridZ-1].
inline int DepthToSlice(float view_depth) {
    if (view_depth <= kNearDepth)
        return 0;
    const float t = std::log(view_depth / kNearDepth) / std::log(kFarDepth / kNearDepth);
    const int slice = static_cast<int>(t * static_cast<float>(kGridZ));
    return std::clamp(slice, 0, kGridZ - 1);
}

// The normalized W (3D-texture depth coordinate, slice centre) for a view depth —
// what the composite samples in volumetric_lighting.frag.
inline float DepthToTextureW(float view_depth) {
    const float clamped = std::clamp(view_depth, kNearDepth, kFarDepth);
    const float t = std::log(clamped / kNearDepth) / std::log(kFarDepth / kNearDepth);
    return std::clamp(t, 0.0f, 1.0f);
}

} // namespace Luminumbra::Rendering::Froxel
